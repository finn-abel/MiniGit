#define _POSIX_C_SOURCE 200809L

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "commit.h"
#include "fs.h"
#include "hash.h"
#include "index.h"
#include "object.h"
#include "repository.h"
#include "tree.h"

static void assert_ok(MGResult result, const char *message) {
    if (result != MG_OK) {
        fprintf(stderr, "%s\n", message);
        exit(1);
    }
}

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
            assert_ok(fs_join_path(path, entry->d_name, child, sizeof(child)), "cleanup path too long");
            remove_recursive(child);
        }
        (void)closedir(dir);
        (void)rmdir(path);
    } else {
        (void)unlink(path);
    }
}

static void make_temp_repo(Repository *repo, char original_dir[MG_MAX_PATH], char temp_dir[MG_MAX_PATH]) {
    int written;

    if (getcwd(original_dir, MG_MAX_PATH) == NULL) {
        fprintf(stderr, "getcwd failed\n");
        exit(1);
    }
    written = snprintf(temp_dir, MG_MAX_PATH, "/tmp/minigit_commit_%ld", (long)getpid());
    if (written < 0 || (size_t)written >= MG_MAX_PATH) {
        fprintf(stderr, "temp path too long\n");
        exit(1);
    }
    if (mkdir(temp_dir, 0700) != 0 || chdir(temp_dir) != 0) {
        fprintf(stderr, "temp repo setup failed\n");
        exit(1);
    }
    assert_ok(repo_init(repo), "repo_init failed");
}

static void cleanup_temp_repo(const char *original_dir, const char *temp_dir) {
    remove_recursive(temp_dir);
    if (chdir(original_dir) != 0) {
        fprintf(stderr, "chdir back failed\n");
        exit(1);
    }
}

static void write_sample_tree(const Repository *repo, char tree_hash[MG_HASH_HEX_SIZE]) {
    Index index;
    Tree tree;
    char blob_hash[MG_HASH_HEX_SIZE];
    const unsigned char payload[] = "hello";

    assert_ok(object_write(repo, "blob", payload, strlen((const char *)payload), blob_hash), "blob write failed");
    index_init(&index);
    assert_ok(index_add_or_update(&index, "README.md", blob_hash, 0100644, strlen((const char *)payload), 1), "index add failed");
    assert_ok(tree_from_index(&index, &tree), "tree_from_index failed");
    assert_ok(tree_write(repo, &tree, tree_hash), "tree_write failed");
    tree_free(&tree);
    index_free(&index);
}

static void test_commit_create_read_first_commit(void) {
    Repository repo;
    Commit commit;
    char original_dir[MG_MAX_PATH];
    char temp_dir[MG_MAX_PATH];
    char tree_hash[MG_HASH_HEX_SIZE];
    char commit_hash[MG_HASH_HEX_SIZE];

    make_temp_repo(&repo, original_dir, temp_dir);
    write_sample_tree(&repo, tree_hash);

    assert_ok(commit_create(&repo, tree_hash, "", "initial commit", commit_hash), "commit_create failed");
    assert_true(hash_is_valid_hex(commit_hash), "commit hash should be valid");
    assert_ok(commit_read(&repo, commit_hash, &commit), "commit_read failed");

    assert_true(strcmp(commit.tree_hash, tree_hash) == 0, "commit tree hash mismatch");
    assert_true(commit.parent_hash[0] == '\0', "first commit should not have parent");
    assert_true(strcmp(commit.author_name, "MiniGit User") == 0, "default author name mismatch");
    assert_true(strcmp(commit.author_email, "minigit@example.com") == 0, "default author email mismatch");
    assert_true(commit.timestamp > 0, "commit timestamp should be set");
    assert_true(strcmp(commit.message, "initial commit") == 0, "commit message mismatch");

    commit_free(&commit);
    cleanup_temp_repo(original_dir, temp_dir);
}

static void test_commit_with_parent(void) {
    Repository repo;
    Commit commit;
    char original_dir[MG_MAX_PATH];
    char temp_dir[MG_MAX_PATH];
    char tree_hash[MG_HASH_HEX_SIZE];
    char first_hash[MG_HASH_HEX_SIZE];
    char second_hash[MG_HASH_HEX_SIZE];

    make_temp_repo(&repo, original_dir, temp_dir);
    write_sample_tree(&repo, tree_hash);

    assert_ok(commit_create(&repo, tree_hash, "", "initial commit", first_hash), "first commit_create failed");
    assert_ok(commit_create(&repo, tree_hash, first_hash, "second commit", second_hash), "second commit_create failed");
    assert_ok(commit_read(&repo, second_hash, &commit), "second commit_read failed");

    assert_true(strcmp(commit.parent_hash, first_hash) == 0, "parent hash mismatch");
    assert_true(strcmp(commit.message, "second commit") == 0, "second message mismatch");

    commit_free(&commit);
    cleanup_temp_repo(original_dir, temp_dir);
}

static void test_commit_author_from_environment(void) {
    Repository repo;
    Commit commit;
    char original_dir[MG_MAX_PATH];
    char temp_dir[MG_MAX_PATH];
    char tree_hash[MG_HASH_HEX_SIZE];
    char commit_hash[MG_HASH_HEX_SIZE];

    make_temp_repo(&repo, original_dir, temp_dir);
    write_sample_tree(&repo, tree_hash);

    assert_true(setenv("MINIGIT_AUTHOR_NAME", "Ada Lovelace", 1) == 0, "setenv author name failed");
    assert_true(setenv("MINIGIT_AUTHOR_EMAIL", "ada@example.com", 1) == 0, "setenv author email failed");
    assert_ok(commit_create(&repo, tree_hash, "", "authored commit", commit_hash), "commit_create with author failed");
    assert_ok(commit_read(&repo, commit_hash, &commit), "commit_read with author failed");

    assert_true(strcmp(commit.author_name, "Ada Lovelace") == 0, "env author name mismatch");
    assert_true(strcmp(commit.author_email, "ada@example.com") == 0, "env author email mismatch");

    commit_free(&commit);
    unsetenv("MINIGIT_AUTHOR_NAME");
    unsetenv("MINIGIT_AUTHOR_EMAIL");
    cleanup_temp_repo(original_dir, temp_dir);
}

static void test_invalid_commit_inputs(void) {
    Repository repo;
    char original_dir[MG_MAX_PATH];
    char temp_dir[MG_MAX_PATH];
    char tree_hash[MG_HASH_HEX_SIZE];
    char commit_hash[MG_HASH_HEX_SIZE];
    const char *missing_tree = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";

    make_temp_repo(&repo, original_dir, temp_dir);
    write_sample_tree(&repo, tree_hash);

    assert_result(commit_create(&repo, tree_hash, "", "", commit_hash), MG_INVALID_ARG, "empty message should fail");
    assert_result(commit_create(&repo, tree_hash, "", "bad\nmessage", commit_hash), MG_INVALID_ARG, "newline message should fail");
    assert_true(setenv("MINIGIT_AUTHOR_EMAIL", "bad<email@example.com", 1) == 0, "setenv invalid email failed");
    assert_result(commit_create(&repo, tree_hash, "", "message", commit_hash), MG_INVALID_ARG, "invalid author email should fail");
    unsetenv("MINIGIT_AUTHOR_EMAIL");
    assert_result(commit_create(&repo, missing_tree, "", "message", commit_hash), MG_NOT_FOUND, "missing tree should fail");

    cleanup_temp_repo(original_dir, temp_dir);
}

int main(void) {
    test_commit_create_read_first_commit();
    test_commit_with_parent();
    test_commit_author_from_environment();
    test_invalid_commit_inputs();
    puts("test_commit passed");
    return 0;
}
