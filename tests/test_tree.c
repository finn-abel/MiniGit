#define _POSIX_C_SOURCE 200809L

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

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
    written = snprintf(temp_dir, MG_MAX_PATH, "/tmp/minigit_tree_%ld", (long)getpid());
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

static void test_tree_write_read_roundtrip(void) {
    Repository repo;
    Index index;
    Tree tree;
    Tree loaded;
    char original_dir[MG_MAX_PATH];
    char temp_dir[MG_MAX_PATH];
    char hash_a[MG_HASH_HEX_SIZE];
    char hash_b[MG_HASH_HEX_SIZE];
    char tree_hash[MG_HASH_HEX_SIZE];
    const unsigned char payload_a[] = "alpha";
    const unsigned char payload_b[] = "beta";

    make_temp_repo(&repo, original_dir, temp_dir);

    assert_ok(object_write(&repo, "blob", payload_b, strlen((const char *)payload_b), hash_b), "blob b write failed");
    assert_ok(object_write(&repo, "blob", payload_a, strlen((const char *)payload_a), hash_a), "blob a write failed");

    index_init(&index);
    assert_ok(index_add_or_update(&index, "z.txt", hash_b, strlen((const char *)payload_b), 20), "index add z failed");
    assert_ok(index_add_or_update(&index, "a.txt", hash_a, strlen((const char *)payload_a), 10), "index add a failed");

    assert_ok(tree_from_index(&index, &tree), "tree_from_index failed");
    assert_true(tree.count == 2, "tree should contain two entries");
    assert_true(strcmp(tree.entries[0].path, "a.txt") == 0, "tree entries should be sorted");
    assert_true(strcmp(tree.entries[1].path, "z.txt") == 0, "tree entries should be sorted");

    assert_ok(tree_write(&repo, &tree, tree_hash), "tree_write failed");
    assert_true(hash_is_valid_hex(tree_hash), "tree hash should be valid");
    assert_ok(tree_read(&repo, tree_hash, &loaded), "tree_read failed");

    assert_true(loaded.count == 2, "loaded tree should contain two entries");
    assert_true(strcmp(loaded.entries[0].path, "a.txt") == 0, "loaded tree path mismatch");
    assert_true(strcmp(loaded.entries[0].hash, hash_a) == 0, "loaded tree hash mismatch");
    assert_true(loaded.entries[0].size == strlen((const char *)payload_a), "loaded tree size mismatch");
    assert_true(strcmp(loaded.entries[1].path, "z.txt") == 0, "loaded tree path mismatch");

    tree_free(&loaded);
    tree_free(&tree);
    index_free(&index);
    cleanup_temp_repo(original_dir, temp_dir);
}

static void test_malformed_tree_rejected(void) {
    Repository repo;
    Tree tree;
    char original_dir[MG_MAX_PATH];
    char temp_dir[MG_MAX_PATH];
    char tree_hash[MG_HASH_HEX_SIZE];
    const unsigned char bad_payload[] = "100644 blob badhash 1\tfile.txt\n";

    make_temp_repo(&repo, original_dir, temp_dir);
    assert_ok(object_write(&repo, "tree", bad_payload, sizeof(bad_payload) - 1, tree_hash), "bad tree write failed");
    assert_result(tree_read(&repo, tree_hash, &tree), MG_PARSE_ERROR, "malformed tree should be rejected");
    cleanup_temp_repo(original_dir, temp_dir);
}

int main(void) {
    test_tree_write_read_roundtrip();
    test_malformed_tree_rejected();
    puts("test_tree passed");
    return 0;
}
