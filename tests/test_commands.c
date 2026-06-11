#define _POSIX_C_SOURCE 200809L

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "commit.h"
#include "commands.h"
#include "common.h"
#include "fs.h"
#include "hash.h"
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
            if (fs_join_path(path, entry->d_name, child, sizeof(child)) != MG_OK) {
                fprintf(stderr, "cleanup path too long\n");
                exit(1);
            }
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
    if (mkdir(temp_dir, 0700) != 0) {
        fprintf(stderr, "mkdir temp dir failed\n");
        exit(1);
    }
    if (chdir(temp_dir) != 0) {
        fprintf(stderr, "chdir to temp dir failed\n");
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

static void capture_stdout_start(const char *path, int *saved_stdout) {
    fflush(stdout);
    *saved_stdout = dup(STDOUT_FILENO);
    if (*saved_stdout < 0) {
        fprintf(stderr, "dup stdout failed\n");
        exit(1);
    }
    if (freopen(path, "wb", stdout) == NULL) {
        fprintf(stderr, "freopen stdout failed\n");
        exit(1);
    }
}

static char *capture_stdout_end(int saved_stdout, const char *path) {
    unsigned char *data = NULL;
    size_t size = 0;

    fflush(stdout);
    if (dup2(saved_stdout, STDOUT_FILENO) < 0) {
        fprintf(stderr, "restore stdout failed\n");
        exit(1);
    }
    close(saved_stdout);
    clearerr(stdout);

    assert_result(fs_read_file(path, &data, &size), MG_OK, "read captured stdout failed");
    (void)size;
    return (char *)data;
}

static void assert_contains(const char *haystack, const char *needle, const char *message) {
    if (strstr(haystack, needle) == NULL) {
        fprintf(stderr, "%s\nmissing: %s\noutput:\n%s\n", message, needle, haystack);
        exit(1);
    }
}

static void test_repo_required_commands(void) {
    char original_dir[MG_MAX_PATH];
    char temp_dir[MG_MAX_PATH];

    make_temp_dir("commands_required", original_dir, temp_dir);

    assert_result(mg_command_status(0, NULL), MG_REPO_ERROR, "status should require a repo");
    assert_result(mg_command_init(0, NULL), MG_OK, "init should create a repo");
    assert_result(mg_command_status(0, NULL), MG_OK, "status should open an initialized repo");
    assert_result(mg_command_init(0, NULL), MG_REPO_ERROR, "init should refuse existing repo");

    cleanup_temp_dir(original_dir, temp_dir);
}

static void test_command_validation_before_repo_open(void) {
    char original_dir[MG_MAX_PATH];
    char temp_dir[MG_MAX_PATH];
    char *bad_branch_args[] = {"bad/name"};
    char *bad_checkout_args[] = {"nope"};
    char *bad_checkout_path_args[] = {"1111111", "file.txt"};
    char *bad_checkout_missing_path_args[] = {"1111111", "--"};
    char *bad_checkout_hash_path_args[] = {"nope", "--", "file.txt"};

    make_temp_dir("commands_validation", original_dir, temp_dir);

    assert_result(mg_command_add(0, NULL), MG_INVALID_ARG, "add without paths should fail before repo open");
    assert_result(mg_command_rm(0, NULL), MG_INVALID_ARG, "rm without paths should fail before repo open");
    assert_result(mg_command_commit(0, NULL), MG_INVALID_ARG, "commit without message should fail before repo open");
    assert_result(mg_command_restore(0, NULL), MG_INVALID_ARG, "restore without path should fail before repo open");
    assert_result(mg_command_reset(0, NULL), MG_INVALID_ARG, "reset without path should fail before repo open");
    assert_result(mg_command_diff(1, bad_checkout_args), MG_INVALID_ARG, "bad diff mode should fail before repo open");
    assert_result(mg_command_branch(1, bad_branch_args), MG_INVALID_ARG, "bad branch should fail before repo open");
    assert_result(mg_command_switch(1, bad_branch_args), MG_INVALID_ARG, "bad switch branch should fail before repo open");
    assert_result(mg_command_checkout(1, bad_checkout_args), MG_INVALID_ARG, "bad checkout hash should fail before repo open");
    assert_result(mg_command_checkout(2, bad_checkout_path_args), MG_INVALID_ARG, "checkout path without separator should fail before repo open");
    assert_result(mg_command_checkout(2, bad_checkout_missing_path_args), MG_INVALID_ARG, "checkout path without path should fail before repo open");
    assert_result(mg_command_checkout(3, bad_checkout_hash_path_args), MG_INVALID_ARG, "bad checkout path hash should fail before repo open");

    cleanup_temp_dir(original_dir, temp_dir);
}

static void test_commit_and_log_commands(void) {
    Repository repo;
    Commit commit;
    char original_dir[MG_MAX_PATH];
    char temp_dir[MG_MAX_PATH];
    char commit_hash[MG_HASH_HEX_SIZE];
    char *add_args[] = {"README.md"};
    char *commit_args[] = {"-m", "initial commit"};
    char *again_args[] = {"-m", "again"};
    const unsigned char contents[] = "hello\n";

    make_temp_dir("commands_commit", original_dir, temp_dir);

    assert_result(mg_command_log(0, NULL), MG_REPO_ERROR, "log should require a repo");
    assert_result(mg_command_init(0, NULL), MG_OK, "init should create repo for commit test");
    assert_result(mg_command_log(0, NULL), MG_OK, "log should handle empty repo");
    assert_result(fs_write_file("README.md", contents, sizeof(contents) - 1), MG_OK, "write README failed");
    assert_result(mg_command_add(1, add_args), MG_OK, "add command failed");
    assert_result(mg_command_commit(2, commit_args), MG_OK, "commit command failed");

    assert_result(repo_open(&repo), MG_OK, "repo_open failed");
    assert_result(repo_current_commit(&repo, commit_hash, sizeof(commit_hash)), MG_OK, "repo_current_commit failed");
    assert_true(hash_is_valid_hex(commit_hash), "branch ref should contain a full commit hash");
    assert_result(commit_read(&repo, commit_hash, &commit), MG_OK, "created commit should parse");
    assert_true(strcmp(commit.message, "initial commit") == 0, "created commit message mismatch");
    assert_true(commit.parent_hash[0] == '\0', "first command commit should not have a parent");
    commit_free(&commit);

    assert_result(mg_command_log(0, NULL), MG_OK, "log command failed after commit");
    assert_result(mg_command_commit(2, again_args), MG_OK, "unchanged commit should return success");

    cleanup_temp_dir(original_dir, temp_dir);
}

static void test_show_command(void) {
    Repository repo;
    char original_dir[MG_MAX_PATH];
    char temp_dir[MG_MAX_PATH];
    char commit_hash[MG_HASH_HEX_SIZE];
    char commit_prefix[8];
    char *add_args[] = {"README.md"};
    char *commit_args[] = {"-m", "initial commit"};
    char *show_args[] = {commit_prefix};
    char *bad_show_args[] = {"nope"};
    char *output;
    int saved_stdout;
    const unsigned char contents[] = "hello\n";

    make_temp_dir("commands_show", original_dir, temp_dir);

    assert_result(mg_command_show(0, NULL), MG_INVALID_ARG, "show without commit should fail before repo open");
    assert_result(mg_command_show(1, bad_show_args), MG_INVALID_ARG, "bad show hash should fail before repo open");
    assert_result(mg_command_init(0, NULL), MG_OK, "init should create repo for show test");
    assert_result(fs_write_file("README.md", contents, sizeof(contents) - 1), MG_OK, "write README failed");
    assert_result(mg_command_add(1, add_args), MG_OK, "add command failed");
    assert_result(mg_command_commit(2, commit_args), MG_OK, "commit command failed");

    assert_result(repo_open(&repo), MG_OK, "repo_open failed");
    assert_result(repo_current_commit(&repo, commit_hash, sizeof(commit_hash)), MG_OK, "repo_current_commit failed");
    memcpy(commit_prefix, commit_hash, 7);
    commit_prefix[7] = '\0';

    capture_stdout_start("show.out", &saved_stdout);
    assert_result(mg_command_show(1, show_args), MG_OK, "show command failed");
    output = capture_stdout_end(saved_stdout, "show.out");

    assert_contains(output, "commit ", "show should include commit header");
    assert_contains(output, "Author: MiniGit User <minigit@example.com>\n", "show should include author");
    assert_contains(output, "    initial commit\n", "show should include message");
    assert_contains(output, "Files:\n", "show should include files heading");
    assert_contains(output, "\tREADME.md\n", "show should include tree entry path");
    free(output);

    cleanup_temp_dir(original_dir, temp_dir);
}

static void test_branch_command_create(void) {
    Repository repo;
    char original_dir[MG_MAX_PATH];
    char temp_dir[MG_MAX_PATH];
    char commit_hash[MG_HASH_HEX_SIZE];
    unsigned char *feature_ref = NULL;
    size_t feature_ref_size = 0;
    char *add_args[] = {"README.md"};
    char *commit_args[] = {"-m", "initial commit"};
    char *branch_args[] = {"feature"};
    char *bad_branch_args[] = {"bad/name"};
    const unsigned char contents[] = "hello\n";

    make_temp_dir("commands_branch", original_dir, temp_dir);

    assert_result(mg_command_init(0, NULL), MG_OK, "init should create repo for branch test");
    assert_result(mg_command_branch(0, NULL), MG_OK, "branch list should work before commits");
    assert_result(mg_command_branch(1, branch_args), MG_REPO_ERROR, "branch before commit should fail");
    assert_result(fs_write_file("README.md", contents, sizeof(contents) - 1), MG_OK, "write README failed");
    assert_result(mg_command_add(1, add_args), MG_OK, "add command failed");
    assert_result(mg_command_commit(2, commit_args), MG_OK, "commit command failed");
    assert_result(repo_open(&repo), MG_OK, "repo_open failed");
    assert_result(repo_current_commit(&repo, commit_hash, sizeof(commit_hash)), MG_OK, "repo_current_commit failed");

    assert_result(mg_command_branch(1, branch_args), MG_OK, "branch create failed");
    assert_result(mg_command_branch(1, branch_args), MG_CONFLICT, "duplicate branch should fail");
    assert_result(mg_command_branch(1, bad_branch_args), MG_INVALID_ARG, "invalid branch should fail");
    assert_result(mg_command_branch(0, NULL), MG_OK, "branch list should work after create");

    assert_result(fs_read_file(".minigit/refs/heads/feature", &feature_ref, &feature_ref_size), MG_OK, "read feature ref failed");
    assert_true(feature_ref_size == MG_HASH_HEX_SIZE, "feature ref should include hash plus newline");
    assert_true(strncmp((const char *)feature_ref, commit_hash, MG_HASH_HEX_SIZE - 1) == 0, "feature ref hash mismatch");
    free(feature_ref);

    cleanup_temp_dir(original_dir, temp_dir);
}

static void test_branch_command_delete(void) {
    Repository repo;
    char original_dir[MG_MAX_PATH];
    char temp_dir[MG_MAX_PATH];
    char refs_heads_path[MG_MAX_PATH];
    char feature_ref_path[MG_MAX_PATH];
    char *add_args[] = {"README.md"};
    char *commit_args[] = {"-m", "initial commit"};
    char *branch_args[] = {"feature"};
    char *delete_args[] = {"-d", "feature"};
    char *delete_main_args[] = {"-d", "main"};
    char *delete_missing_args[] = {"-d", "missing"};
    const unsigned char contents[] = "hello\n";

    make_temp_dir("commands_branch_delete", original_dir, temp_dir);

    assert_result(mg_command_init(0, NULL), MG_OK, "init should create repo for branch delete test");
    assert_result(fs_write_file("README.md", contents, sizeof(contents) - 1), MG_OK, "write README failed");
    assert_result(mg_command_add(1, add_args), MG_OK, "add command failed");
    assert_result(mg_command_commit(2, commit_args), MG_OK, "commit command failed");
    assert_result(mg_command_branch(1, branch_args), MG_OK, "branch create failed");

    assert_result(repo_open(&repo), MG_OK, "repo_open failed");
    assert_result(fs_join_path(repo.gitdir_path, "refs/heads", refs_heads_path, sizeof(refs_heads_path)), MG_OK, "refs path failed");
    assert_result(fs_join_path(refs_heads_path, "feature", feature_ref_path, sizeof(feature_ref_path)), MG_OK, "feature path failed");
    assert_true(fs_is_file(feature_ref_path), "feature ref should exist before delete");

    assert_result(mg_command_branch(2, delete_main_args), MG_CONFLICT, "deleting current branch should fail");
    assert_result(mg_command_branch(2, delete_args), MG_OK, "branch delete failed");
    assert_true(!fs_exists(feature_ref_path), "feature ref should be removed");
    assert_result(mg_command_branch(2, delete_args), MG_NOT_FOUND, "deleting removed branch should fail");
    assert_result(mg_command_branch(2, delete_missing_args), MG_NOT_FOUND, "deleting unknown branch should fail");

    cleanup_temp_dir(original_dir, temp_dir);
}

static void test_add_respects_minigitignore(void) {
    Repository repo;
    Index index;
    char original_dir[MG_MAX_PATH];
    char temp_dir[MG_MAX_PATH];
    char *add_args[] = {"."};
    const unsigned char ignore[] = "ignored.txt\nlogs/\n";
    const unsigned char contents[] = "x\n";

    make_temp_dir("commands_ignore", original_dir, temp_dir);

    assert_result(mg_command_init(0, NULL), MG_OK, "init should create repo for ignore test");
    assert_result(fs_write_file(".minigitignore", ignore, sizeof(ignore) - 1), MG_OK, "write ignore failed");
    assert_result(fs_write_file("ignored.txt", contents, sizeof(contents) - 1), MG_OK, "write ignored failed");
    assert_result(mkdir("logs", 0700) == 0 ? MG_OK : MG_IO_ERROR, MG_OK, "mkdir logs failed");
    assert_result(fs_write_file("logs/run.log", contents, sizeof(contents) - 1), MG_OK, "write log failed");
    assert_result(fs_write_file("keep.txt", contents, sizeof(contents) - 1), MG_OK, "write keep failed");

    assert_result(mg_command_add(1, add_args), MG_OK, "add dot should skip ignored files");
    assert_result(repo_open(&repo), MG_OK, "repo_open failed");
    assert_result(index_load(&repo, &index), MG_OK, "index_load failed");
    assert_true(index_find_const(&index, "ignored.txt") == NULL, "ignored file should not be staged");
    assert_true(index_find_const(&index, "logs/run.log") == NULL, "ignored dir file should not be staged");
    assert_true(index_find_const(&index, "keep.txt") != NULL, "non-ignored file should be staged");
    index_free(&index);

    cleanup_temp_dir(original_dir, temp_dir);
}

int main(void) {
    test_repo_required_commands();
    test_command_validation_before_repo_open();
    test_commit_and_log_commands();
    test_show_command();
    test_branch_command_create();
    test_branch_command_delete();
    test_add_respects_minigitignore();
    puts("All commands tests passed.");
    return 0;
}
