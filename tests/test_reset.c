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
#include "tree.h"

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

static void make_temp_dir(const char *name, char original_dir[MG_MAX_PATH], char temp_dir[MG_MAX_PATH]) {
    int written;

    if (getcwd(original_dir, MG_MAX_PATH) == NULL) {
        fprintf(stderr, "getcwd failed\n");
        exit(1);
    }

    written = snprintf(temp_dir, MG_MAX_PATH, "/tmp/minigit_%s_%ld", name, (long)getpid());
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

static void commit_file(const char *contents, const char *message, char out_hash[MG_HASH_HEX_SIZE]) {
    Repository repo;
    char *add_args[] = {"file.txt"};
    char *commit_args[] = {"-m", (char *)message};

    assert_result(fs_write_file("file.txt", (const unsigned char *)contents, strlen(contents)), MG_OK, "write file failed");
    assert_result(mg_command_add(1, add_args), MG_OK, "add file failed");
    assert_result(mg_command_commit(2, commit_args), MG_OK, "commit file failed");
    assert_result(repo_open(&repo), MG_OK, "repo_open failed");
    assert_result(repo_current_commit(&repo, out_hash, MG_HASH_HEX_SIZE), MG_OK, "repo_current_commit failed");
}

static void test_reset_staged_modification(void) {
    Repository repo;
    Commit commit;
    Tree tree;
    Index index;
    const TreeEntry *tree_entry;
    const IndexEntry *index_entry;
    char original_dir[MG_MAX_PATH];
    char temp_dir[MG_MAX_PATH];
    char commit_hash[MG_HASH_HEX_SIZE];
    char *add_args[] = {"file.txt"};
    char *reset_args[] = {"file.txt"};
    char *contents;

    make_temp_dir("reset_modify", original_dir, temp_dir);
    assert_result(mg_command_init(0, NULL), MG_OK, "init failed");
    commit_file("one\n", "one", commit_hash);

    assert_result(fs_write_file("file.txt", (const unsigned char *)"two\n", strlen("two\n")), MG_OK, "modify file failed");
    assert_result(mg_command_add(1, add_args), MG_OK, "stage modification failed");
    assert_result(mg_command_reset(1, reset_args), MG_OK, "reset modification failed");

    read_text_file("file.txt", &contents);
    assert_true(strcmp(contents, "two\n") == 0, "reset should not change worktree contents");
    free(contents);

    assert_result(repo_open(&repo), MG_OK, "repo_open failed");
    assert_result(commit_read(&repo, commit_hash, &commit), MG_OK, "commit_read failed");
    assert_result(tree_read(&repo, commit.tree_hash, &tree), MG_OK, "tree_read failed");
    assert_result(index_load(&repo, &index), MG_OK, "index_load failed");

    tree_entry = tree_find_entry(&tree, "file.txt");
    index_entry = index_find_const(&index, "file.txt");
    assert_true(tree_entry != NULL && index_entry != NULL, "file should exist in HEAD and index");
    assert_true(strcmp(tree_entry->hash, index_entry->hash) == 0, "index should be restored to HEAD hash");

    index_free(&index);
    tree_free(&tree);
    commit_free(&commit);
    cleanup_temp_dir(original_dir, temp_dir);
}

static void test_reset_new_file_before_first_commit(void) {
    Repository repo;
    Index index;
    char original_dir[MG_MAX_PATH];
    char temp_dir[MG_MAX_PATH];
    char *add_args[] = {"new.txt"};
    char *reset_args[] = {"new.txt"};
    char *contents;

    make_temp_dir("reset_new", original_dir, temp_dir);
    assert_result(mg_command_init(0, NULL), MG_OK, "init failed");
    assert_result(fs_write_file("new.txt", (const unsigned char *)"new\n", strlen("new\n")), MG_OK, "write new file failed");
    assert_result(mg_command_add(1, add_args), MG_OK, "stage new file failed");
    assert_result(mg_command_reset(1, reset_args), MG_OK, "reset new file failed");

    read_text_file("new.txt", &contents);
    assert_true(strcmp(contents, "new\n") == 0, "reset should keep new worktree file");
    free(contents);

    assert_result(repo_open(&repo), MG_OK, "repo_open failed");
    assert_result(index_load(&repo, &index), MG_OK, "index_load failed");
    assert_true(index_find_const(&index, "new.txt") == NULL, "new file should be removed from index");
    index_free(&index);

    cleanup_temp_dir(original_dir, temp_dir);
}

static void test_reset_unknown_path(void) {
    char original_dir[MG_MAX_PATH];
    char temp_dir[MG_MAX_PATH];
    char *reset_args[] = {"missing.txt"};

    make_temp_dir("reset_missing", original_dir, temp_dir);
    assert_result(mg_command_init(0, NULL), MG_OK, "init failed");
    assert_result(mg_command_reset(1, reset_args), MG_NOT_FOUND, "reset should reject unknown path");

    cleanup_temp_dir(original_dir, temp_dir);
}

int main(void) {
    test_reset_staged_modification();
    test_reset_new_file_before_first_commit();
    test_reset_unknown_path();
    puts("test_reset passed");
    return 0;
}
