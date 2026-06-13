#define _POSIX_C_SOURCE 200809L

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "commands.h"
#include "commit.h"
#include "common.h"
#include "fs.h"
#include "index.h"
#include "repository.h"

static void assert_result(MGResult actual, MGResult expected, const char *message) {
    if (actual != expected) {
        fprintf(stderr, "%s\n", message);
        exit(1);
    }
}

static void assert_true(int condition, const char *message) {
    if (!condition) {
        fprintf(stderr, "%s\n", message);
        exit(1);
    }
}

static void remove_recursive(const char *path) {
    struct stat st;

    if (lstat(path, &st) != 0) {
        return;
    }
    if (S_ISDIR(st.st_mode)) {
        DIR *dir = opendir(path);
        struct dirent *entry;

        if (dir == NULL) {
            return;
        }
        while ((entry = readdir(dir)) != NULL) {
            char child[MG_MAX_PATH];

            if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
                continue;
            }
            assert_result(fs_join_path(path, entry->d_name, child, sizeof(child)), MG_OK, "cleanup path too long");
            remove_recursive(child);
        }
        (void)closedir(dir);
        (void)rmdir(path);
    } else {
        (void)unlink(path);
    }
}

static void make_temp_dir(char original_dir[MG_MAX_PATH], char temp_dir[MG_MAX_PATH]) {
    int written;

    if (getcwd(original_dir, MG_MAX_PATH) == NULL) {
        fprintf(stderr, "getcwd failed\n");
        exit(1);
    }
    written = snprintf(temp_dir, MG_MAX_PATH, "/tmp/minigit_merge_%ld", (long)getpid());
    if (written < 0 || (size_t)written >= MG_MAX_PATH) {
        fprintf(stderr, "temp path too long\n");
        exit(1);
    }
    if (mkdir(temp_dir, 0700) != 0 || chdir(temp_dir) != 0) {
        fprintf(stderr, "temp dir setup failed\n");
        exit(1);
    }
}

static void cleanup_temp_dir(const char *original_dir, const char *temp_dir) {
    if (chdir(original_dir) != 0) {
        fprintf(stderr, "chdir back failed\n");
        exit(1);
    }
    remove_recursive(temp_dir);
}

static void read_text_file(const char *path, char **out) {
    unsigned char *data = NULL;
    size_t size = 0;

    assert_result(fs_read_file(path, &data, &size), MG_OK, "read file failed");
    (void)size;
    *out = (char *)data;
}

static void commit_all(const char *message) {
    char *add_args[] = {"."};
    char *commit_args[] = {"-m", (char *)message};

    assert_result(mg_command_add(1, add_args), MG_OK, "add all failed");
    assert_result(mg_command_commit(2, commit_args), MG_OK, "commit failed");
}

static void test_merge_non_conflicting_branch(void) {
    Repository repo;
    Index index;
    Commit merge_commit;
    char original_dir[MG_MAX_PATH];
    char temp_dir[MG_MAX_PATH];
    char *branch_args[] = {"feature"};
    char *switch_feature_args[] = {"feature"};
    char *switch_main_args[] = {"main"};
    char *merge_args[] = {"feature"};
    char *contents;
    char main_hash[MG_HASH_HEX_SIZE];
    char feature_hash[MG_HASH_HEX_SIZE];
    char merge_hash[MG_HASH_HEX_SIZE];

    make_temp_dir(original_dir, temp_dir);
    assert_result(mg_command_init(0, NULL), MG_OK, "init failed");
    assert_result(fs_write_file("main.txt", (const unsigned char *)"base main\n", strlen("base main\n")), MG_OK, "write main failed");
    assert_result(fs_write_file("feature.txt", (const unsigned char *)"base feature\n", strlen("base feature\n")), MG_OK, "write feature failed");
    commit_all("base");
    assert_result(mg_command_branch(1, branch_args), MG_OK, "branch failed");

    assert_result(fs_write_file("main.txt", (const unsigned char *)"main change\n", strlen("main change\n")), MG_OK, "write main change failed");
    commit_all("main change");
    assert_result(mg_command_switch(1, switch_feature_args), MG_OK, "switch feature failed");
    assert_result(fs_write_file("feature.txt", (const unsigned char *)"feature change\n", strlen("feature change\n")), MG_OK, "write feature change failed");
    commit_all("feature change");
    assert_result(repo_open(&repo), MG_OK, "repo_open failed");
    assert_result(repo_current_commit(&repo, feature_hash, sizeof(feature_hash)), MG_OK, "feature hash failed");
    assert_result(mg_command_switch(1, switch_main_args), MG_OK, "switch main failed");
    assert_result(repo_current_commit(&repo, main_hash, sizeof(main_hash)), MG_OK, "main hash failed");

    assert_result(mg_command_merge(1, merge_args), MG_OK, "merge should succeed");
    read_text_file("main.txt", &contents);
    assert_true(strcmp(contents, "main change\n") == 0, "main file should keep ours");
    free(contents);
    read_text_file("feature.txt", &contents);
    assert_true(strcmp(contents, "feature change\n") == 0, "feature file should take theirs");
    free(contents);

    assert_result(repo_current_commit(&repo, merge_hash, sizeof(merge_hash)), MG_OK, "merge hash failed");
    assert_true(strcmp(merge_hash, main_hash) != 0, "non-fast-forward merge should create a commit");
    assert_result(commit_read(&repo, merge_hash, &merge_commit), MG_OK, "read merge commit failed");
    assert_true(strcmp(merge_commit.parent_hash, main_hash) == 0, "merge first parent mismatch");
    assert_true(strcmp(merge_commit.second_parent_hash, feature_hash) == 0, "merge second parent mismatch");
    commit_free(&merge_commit);
    assert_result(mg_command_merge(1, merge_args), MG_OK, "repeat merge should be a no-op");
    assert_result(repo_current_commit(&repo, main_hash, sizeof(main_hash)), MG_OK, "repeat merge hash failed");
    assert_true(strcmp(main_hash, merge_hash) == 0, "repeat merge should not create another commit");

    assert_result(index_load(&repo, &index), MG_OK, "index_load failed");
    assert_true(index_find_const(&index, "feature.txt") != NULL, "merged file should be staged");
    index_free(&index);

    cleanup_temp_dir(original_dir, temp_dir);
}

static void test_merge_refuses_untracked_target(void) {
    char original_dir[MG_MAX_PATH];
    char temp_dir[MG_MAX_PATH];
    char *branch_args[] = {"feature"};
    char *switch_feature_args[] = {"feature"};
    char *switch_main_args[] = {"main"};
    char *merge_args[] = {"feature"};
    char *contents;

    make_temp_dir(original_dir, temp_dir);
    assert_result(mg_command_init(0, NULL), MG_OK, "init failed");
    assert_result(fs_write_file("base.txt", (const unsigned char *)"base\n", 5), MG_OK, "write base failed");
    commit_all("base");
    assert_result(mg_command_branch(1, branch_args), MG_OK, "branch failed");
    assert_result(fs_write_file("main.txt", (const unsigned char *)"main\n", 5), MG_OK, "write main failed");
    commit_all("main");
    assert_result(mg_command_switch(1, switch_feature_args), MG_OK, "switch feature failed");
    assert_result(fs_write_file("collision.txt", (const unsigned char *)"feature\n", 8), MG_OK, "write feature file failed");
    commit_all("feature");
    assert_result(mg_command_switch(1, switch_main_args), MG_OK, "switch main failed");
    assert_result(fs_write_file("collision.txt", (const unsigned char *)"local\n", 6), MG_OK, "write local file failed");

    assert_result(mg_command_merge(1, merge_args), MG_CONFLICT, "merge should preserve an untracked target");
    read_text_file("collision.txt", &contents);
    assert_true(strcmp(contents, "local\n") == 0, "merge overwrote untracked data");
    free(contents);
    cleanup_temp_dir(original_dir, temp_dir);
}

static void test_merge_conflict_markers(void) {
    char original_dir[MG_MAX_PATH];
    char temp_dir[MG_MAX_PATH];
    char *branch_args[] = {"feature"};
    char *switch_feature_args[] = {"feature"};
    char *switch_main_args[] = {"main"};
    char *merge_args[] = {"feature"};
    char *contents;

    make_temp_dir(original_dir, temp_dir);
    assert_result(mg_command_init(0, NULL), MG_OK, "init failed");
    assert_result(fs_write_file("file.txt", (const unsigned char *)"base\n", strlen("base\n")), MG_OK, "write base failed");
    commit_all("base");
    assert_result(mg_command_branch(1, branch_args), MG_OK, "branch failed");

    assert_result(fs_write_file("file.txt", (const unsigned char *)"main\n", strlen("main\n")), MG_OK, "write main failed");
    commit_all("main");
    assert_result(mg_command_switch(1, switch_feature_args), MG_OK, "switch feature failed");
    assert_result(fs_write_file("file.txt", (const unsigned char *)"feature\n", strlen("feature\n")), MG_OK, "write feature failed");
    commit_all("feature");
    assert_result(mg_command_switch(1, switch_main_args), MG_OK, "switch main failed");

    assert_result(mg_command_merge(1, merge_args), MG_CONFLICT, "merge should conflict");
    read_text_file("file.txt", &contents);
    assert_true(strstr(contents, "<<<<<<< HEAD\nmain\n=======\nfeature\n>>>>>>> feature\n") != NULL,
        "conflict markers should include both sides");
    free(contents);

    cleanup_temp_dir(original_dir, temp_dir);
}

static void test_merge_applies_clean_deletion(void) {
    Repository repo;
    Index index;
    char original_dir[MG_MAX_PATH];
    char temp_dir[MG_MAX_PATH];
    char *branch_args[] = {"feature"};
    char *switch_feature_args[] = {"feature"};
    char *switch_main_args[] = {"main"};
    char *rm_args[] = {"delete.txt"};
    char *feature_commit_args[] = {"-m", "delete file"};
    char *merge_args[] = {"feature"};

    make_temp_dir(original_dir, temp_dir);
    assert_result(mg_command_init(0, NULL), MG_OK, "init failed");
    assert_result(fs_write_file("base.txt", (const unsigned char *)"base\n", 5), MG_OK, "write base failed");
    assert_result(fs_write_file("delete.txt", (const unsigned char *)"delete me\n", 10), MG_OK,
                  "write delete target failed");
    commit_all("base");
    assert_result(mg_command_branch(1, branch_args), MG_OK, "branch failed");

    assert_result(fs_write_file("main.txt", (const unsigned char *)"main\n", 5), MG_OK, "write main failed");
    commit_all("main");
    assert_result(mg_command_switch(1, switch_feature_args), MG_OK, "switch feature failed");
    assert_result(mg_command_rm(1, rm_args), MG_OK, "feature delete failed");
    assert_result(mg_command_commit(2, feature_commit_args), MG_OK, "feature delete commit failed");
    assert_result(mg_command_switch(1, switch_main_args), MG_OK, "switch main failed");

    assert_result(mg_command_merge(1, merge_args), MG_OK, "clean deletion merge failed");
    assert_true(!fs_exists("delete.txt"), "merge did not delete the path");
    assert_result(repo_open(&repo), MG_OK, "repo_open failed");
    assert_result(index_load(&repo, &index), MG_OK, "index_load failed");
    assert_true(index_find_const(&index, "delete.txt") == NULL, "deleted path remained in index");
    assert_true(index_find_const(&index, "main.txt") != NULL, "merge lost main-side file");
    index_free(&index);

    cleanup_temp_dir(original_dir, temp_dir);
}

int main(void) {
    test_merge_non_conflicting_branch();
    test_merge_conflict_markers();
    test_merge_refuses_untracked_target();
    test_merge_applies_clean_deletion();
    puts("test_merge passed");
    return 0;
}
