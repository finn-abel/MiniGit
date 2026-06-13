#define _POSIX_C_SOURCE 200809L

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "fs.h"
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

    written = snprintf(temp_dir, MG_MAX_PATH, "/tmp/minigit_repository_%ld", (long)getpid());
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

static void test_init_and_open(void) {
    Repository repo;
    Repository opened;
    char original_dir[MG_MAX_PATH];
    char temp_dir[MG_MAX_PATH];
    char head[MG_MAX_PATH];
    char branch[MG_MAX_BRANCH];
    char display[MG_MAX_BRANCH];
    char commit_hash[MG_HASH_HEX_SIZE];

    make_temp_dir(original_dir, temp_dir);

    assert_result(repo_open(&opened), MG_REPO_ERROR, "repo_open should fail outside repo");
    assert_result(repo_init(&repo), MG_OK, "repo_init failed");
    assert_true(fs_is_dir(".minigit"), ".minigit should exist");
    assert_true(fs_is_dir(".minigit/objects"), "objects dir should exist");
    assert_true(fs_is_dir(".minigit/refs/heads"), "refs/heads dir should exist");
    assert_true(fs_is_file(".minigit/refs/heads/main"), "main ref should exist");
    assert_true(fs_is_file(".minigit/index"), "index should exist");
    assert_result(repo_init(&repo), MG_REPO_ERROR, "repo_init should refuse existing repo");
    assert_result(repo_open(&opened), MG_OK, "repo_open should succeed inside repo");

    assert_result(repo_read_head(&opened, head, sizeof(head)), MG_OK, "repo_read_head failed");
    assert_true(strcmp(head, "ref: refs/heads/main") == 0, "HEAD should point at main");
    assert_true(!repo_head_is_detached(head), "initial HEAD should be symbolic");
    assert_result(repo_current_branch(&opened, branch, sizeof(branch)), MG_OK, "repo_current_branch failed");
    assert_true(strcmp(branch, "main") == 0, "current branch should be main");
    assert_result(repo_head_display_name(&opened, display, sizeof(display)), MG_OK, "repo_head_display_name failed");
    assert_true(strcmp(display, "main") == 0, "display should be main");
    assert_result(repo_current_commit(&opened, commit_hash, sizeof(commit_hash)), MG_OK, "repo_current_commit failed");
    assert_true(commit_hash[0] == '\0', "new branch should not have a commit");

    cleanup_temp_dir(original_dir, temp_dir);
}

static void test_ref_updates_and_detached_head(void) {
    Repository repo;
    char original_dir[MG_MAX_PATH];
    char temp_dir[MG_MAX_PATH];
    char current_hash[MG_HASH_HEX_SIZE];
    char display[MG_MAX_BRANCH];
    char branch[MG_MAX_BRANCH];
    const char *first_hash = "1111111111111111111111111111111111111111111111111111111111111111";
    const char *detached_hash = "2222222222222222222222222222222222222222222222222222222222222222";

    make_temp_dir(original_dir, temp_dir);
    assert_result(repo_init(&repo), MG_OK, "repo_init failed");

    assert_result(repo_update_current_ref(&repo, first_hash), MG_OK, "repo_update_current_ref failed");
    assert_result(repo_current_commit(&repo, current_hash, sizeof(current_hash)), MG_OK, "repo_current_commit after update failed");
    assert_true(strcmp(current_hash, first_hash) == 0, "current commit should match branch ref");
    assert_result(repo_head_display_name(&repo, display, sizeof(display)), MG_OK, "branch display failed");
    assert_true(strcmp(display, "main") == 0, "branch display should be main");

    assert_result(repo_write_head(&repo, detached_hash), MG_OK, "repo_write_head detached failed");
    assert_result(repo_read_head(&repo, current_hash, sizeof(current_hash)), MG_OK, "repo_read_head detached failed");
    assert_true(repo_head_is_detached(current_hash), "raw hash HEAD should be detached");
    assert_result(repo_current_branch(&repo, branch, sizeof(branch)), MG_NOT_FOUND, "detached HEAD should have no branch");
    assert_result(repo_current_commit(&repo, current_hash, sizeof(current_hash)), MG_OK, "detached current commit failed");
    assert_true(strcmp(current_hash, detached_hash) == 0, "detached current commit mismatch");
    assert_result(repo_head_display_name(&repo, display, sizeof(display)), MG_OK, "detached display failed");
    assert_true(strcmp(display, "detached") == 0, "display should be detached");

    cleanup_temp_dir(original_dir, temp_dir);
}

static void test_branch_create_and_validation(void) {
    Repository repo;
    char original_dir[MG_MAX_PATH];
    char temp_dir[MG_MAX_PATH];
    char current_hash[MG_HASH_HEX_SIZE];
    unsigned char *feature_ref = NULL;
    size_t feature_ref_size = 0;
    const char *commit_hash = "3333333333333333333333333333333333333333333333333333333333333333";

    make_temp_dir(original_dir, temp_dir);
    assert_result(repo_init(&repo), MG_OK, "repo_init failed");

    assert_true(repo_branch_name_is_valid("feature"), "feature should be valid");
    assert_true(!repo_branch_name_is_valid(""), "empty branch should be invalid");
    assert_true(!repo_branch_name_is_valid("bad/name"), "slash branch should be invalid");
    assert_true(!repo_branch_name_is_valid("bad name"), "space branch should be invalid");
    assert_true(!repo_branch_name_is_valid("bad..name"), "dot-dot branch should be invalid");

    assert_result(repo_create_branch(&repo, "feature"), MG_REPO_ERROR, "branch before commit should fail");
    assert_result(repo_update_current_ref(&repo, commit_hash), MG_OK, "repo_update_current_ref failed");
    assert_result(repo_current_commit(&repo, current_hash, sizeof(current_hash)), MG_OK, "repo_current_commit failed");
    assert_true(strcmp(current_hash, commit_hash) == 0, "current commit mismatch");

    assert_result(repo_create_branch(&repo, "feature"), MG_OK, "create feature branch failed");
    assert_result(repo_create_branch(&repo, "feature"), MG_CONFLICT, "duplicate branch should fail");
    assert_result(repo_create_branch(&repo, "bad/name"), MG_INVALID_ARG, "invalid branch should fail");
    assert_result(fs_read_file(".minigit/refs/heads/feature", &feature_ref, &feature_ref_size), MG_OK, "read feature ref failed");
    assert_true(feature_ref_size == MG_HASH_HEX_SIZE, "feature ref should include hash plus newline");
    assert_true(strncmp((const char *)feature_ref, commit_hash, MG_HASH_HEX_SIZE - 1) == 0, "feature ref hash mismatch");
    free(feature_ref);

    cleanup_temp_dir(original_dir, temp_dir);
}

static void test_branch_delete(void) {
    Repository repo;
    char original_dir[MG_MAX_PATH];
    char temp_dir[MG_MAX_PATH];
    int exists;
    const char *commit_hash = "4444444444444444444444444444444444444444444444444444444444444444";
    const char *detached_hash = "5555555555555555555555555555555555555555555555555555555555555555";

    make_temp_dir(original_dir, temp_dir);
    assert_result(repo_init(&repo), MG_OK, "repo_init failed");
    assert_result(repo_update_current_ref(&repo, commit_hash), MG_OK, "repo_update_current_ref failed");
    assert_result(repo_create_branch(&repo, "feature"), MG_OK, "create feature branch failed");

    assert_result(repo_delete_branch(&repo, "bad/name"), MG_INVALID_ARG, "delete invalid branch should fail");
    assert_result(repo_delete_branch(&repo, "missing"), MG_NOT_FOUND, "delete missing branch should fail");
    assert_result(repo_delete_branch(&repo, "main"), MG_CONFLICT, "delete current branch should fail");
    assert_result(repo_branch_exists(&repo, "main", &exists), MG_OK, "main existence check failed");
    assert_true(exists, "main should still exist after refused delete");

    assert_result(repo_delete_branch(&repo, "feature"), MG_OK, "delete feature branch failed");
    assert_result(repo_branch_exists(&repo, "feature", &exists), MG_OK, "feature existence check failed");
    assert_true(!exists, "feature should be removed");
    assert_result(repo_delete_branch(&repo, "feature"), MG_NOT_FOUND, "delete removed branch should fail");

    assert_result(repo_create_branch(&repo, "detached-delete"), MG_OK, "create detached-delete branch failed");
    assert_result(repo_write_head(&repo, detached_hash), MG_OK, "detach HEAD failed");
    assert_result(repo_delete_branch(&repo, "detached-delete"), MG_OK, "delete branch while detached failed");
    assert_result(repo_branch_exists(&repo, "detached-delete", &exists), MG_OK, "detached-delete existence check failed");
    assert_true(!exists, "detached-delete should be removed");

    cleanup_temp_dir(original_dir, temp_dir);
}

static void test_malformed_head_ref_rejected(void) {
    Repository repo;
    char original_dir[MG_MAX_PATH];
    char temp_dir[MG_MAX_PATH];
    char target_path[MG_MAX_PATH];
    char head_contents[MG_MAX_PATH];
    char current_hash[MG_HASH_HEX_SIZE];
    unsigned char *target_data = NULL;
    size_t target_size = 0;
    const char *hash = "6666666666666666666666666666666666666666666666666666666666666666";

    make_temp_dir(original_dir, temp_dir);
    assert_result(repo_init(&repo), MG_OK, "repo_init failed");
    assert_true(snprintf(target_path, sizeof(target_path), "/tmp/minigit_head_target_%ld", (long)getpid()) > 0,
        "target path failed");
    assert_true(snprintf(head_contents, sizeof(head_contents), "ref: ../../minigit_head_target_%ld\n", (long)getpid()) > 0,
        "HEAD contents failed");
    assert_result(fs_write_file(target_path, (const unsigned char *)"unchanged\n", 10), MG_OK, "write target failed");
    assert_result(fs_write_file(".minigit/HEAD", (const unsigned char *)head_contents, strlen(head_contents)), MG_OK,
        "write malformed HEAD failed");

    assert_result(repo_current_commit(&repo, current_hash, sizeof(current_hash)), MG_REPO_ERROR,
        "malformed HEAD should not resolve");
    assert_result(repo_update_current_ref(&repo, hash), MG_REPO_ERROR, "malformed HEAD should not redirect writes");
    assert_result(fs_read_file(target_path, &target_data, &target_size), MG_OK, "read target failed");
    assert_true(target_size == 10 && memcmp(target_data, "unchanged\n", 10) == 0, "external target was modified");
    free(target_data);
    (void)unlink(target_path);
    cleanup_temp_dir(original_dir, temp_dir);
}

int main(void) {
    test_init_and_open();
    test_ref_updates_and_detached_head();
    test_branch_create_and_validation();
    test_branch_delete();
    test_malformed_head_ref_rejected();
    puts("All repository tests passed.");
    return 0;
}
