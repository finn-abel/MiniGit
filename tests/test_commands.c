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

    make_temp_dir("commands_validation", original_dir, temp_dir);

    assert_result(mg_command_add(0, NULL), MG_INVALID_ARG, "add without paths should fail before repo open");
    assert_result(mg_command_rm(0, NULL), MG_INVALID_ARG, "rm without paths should fail before repo open");
    assert_result(mg_command_commit(0, NULL), MG_INVALID_ARG, "commit without message should fail before repo open");
    assert_result(mg_command_diff(1, bad_checkout_args), MG_INVALID_ARG, "bad diff mode should fail before repo open");
    assert_result(mg_command_branch(1, bad_branch_args), MG_INVALID_ARG, "bad branch should fail before repo open");
    assert_result(mg_command_switch(1, bad_branch_args), MG_INVALID_ARG, "bad switch branch should fail before repo open");
    assert_result(mg_command_checkout(1, bad_checkout_args), MG_INVALID_ARG, "bad checkout hash should fail before repo open");

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

int main(void) {
    test_repo_required_commands();
    test_command_validation_before_repo_open();
    test_commit_and_log_commands();
    test_branch_command_create();
    puts("All commands tests passed.");
    return 0;
}
