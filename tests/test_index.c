#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "fs.h"
#include "index.h"
#include "repository.h"

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

static void remove_initialized_repo(void) {
    (void)unlink(".minigit/HEAD");
    (void)unlink(".minigit/index");
    (void)unlink(".minigit/refs/heads/main");
    (void)rmdir(".minigit/refs/heads");
    (void)rmdir(".minigit/refs");
    (void)rmdir(".minigit/objects");
    (void)rmdir(".minigit");
}

static void make_temp_repo(Repository *repo, char original_dir[MG_MAX_PATH], char temp_dir[MG_MAX_PATH]) {
    int written;

    if (getcwd(original_dir, MG_MAX_PATH) == NULL) {
        fprintf(stderr, "getcwd failed\n");
        exit(1);
    }

    written = snprintf(temp_dir, MG_MAX_PATH, "/tmp/minigit_index_%ld", (long)getpid());
    if (written < 0 || (size_t)written >= MG_MAX_PATH) {
        fprintf(stderr, "temp path too long\n");
        exit(1);
    }
    if (mkdir(temp_dir, 0700) != 0) {
        fprintf(stderr, "mkdir temp dir failed\n");
        exit(1);
    }
    if (chdir(temp_dir) != 0) {
        fprintf(stderr, "chdir temp dir failed\n");
        exit(1);
    }

    assert_ok(repo_init(repo), "repo_init failed");
}

static void cleanup_temp_repo(const char *original_dir, const char *temp_dir) {
    remove_initialized_repo();
    if (chdir(original_dir) != 0) {
        fprintf(stderr, "chdir back failed\n");
        exit(1);
    }
    (void)rmdir(temp_dir);
}

static void test_empty_load(void) {
    Repository repo;
    Index index;
    char original_dir[MG_MAX_PATH];
    char temp_dir[MG_MAX_PATH];

    make_temp_repo(&repo, original_dir, temp_dir);
    assert_ok(index_load(&repo, &index), "empty index_load failed");
    assert_true(index.count == 0, "empty index should have no entries");
    index_free(&index);
    cleanup_temp_repo(original_dir, temp_dir);
}

static void test_save_load_update_remove(void) {
    Repository repo;
    Index index;
    Index loaded;
    IndexEntry *entry;
    char original_dir[MG_MAX_PATH];
    char temp_dir[MG_MAX_PATH];
    const char *hash_a = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
    const char *hash_b = "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb";
    const char *hash_c = "cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc";

    make_temp_repo(&repo, original_dir, temp_dir);

    index_init(&index);
    assert_ok(index_add_or_update(&index, "z.txt", hash_b, 2, 20), "add z.txt failed");
    assert_ok(index_add_or_update(&index, "a.txt", hash_a, 1, 10), "add a.txt failed");
    assert_ok(index_add_or_update(&index, "z.txt", hash_c, 3, 30), "update z.txt failed");
    assert_ok(index_save(&repo, &index), "index_save failed");
    index_free(&index);

    assert_ok(index_load(&repo, &loaded), "index_load failed");
    assert_true(loaded.count == 2, "loaded index should have two entries");
    assert_true(strcmp(loaded.entries[0].path, "a.txt") == 0, "entries should be sorted");
    assert_true(strcmp(loaded.entries[1].path, "z.txt") == 0, "entries should be sorted");

    entry = index_find(&loaded, "z.txt");
    assert_true(entry != NULL, "updated entry should exist");
    assert_true(strcmp(entry->hash, hash_c) == 0, "updated hash mismatch");
    assert_true(entry->size == 3, "updated size mismatch");
    assert_true((long)entry->mtime == 30, "updated mtime mismatch");

    assert_ok(index_remove(&loaded, "a.txt"), "index_remove failed");
    assert_result(index_remove(&loaded, "missing.txt"), MG_NOT_FOUND, "missing remove should return MG_NOT_FOUND");
    assert_true(index_find(&loaded, "a.txt") == NULL, "removed entry should be gone");
    index_free(&loaded);

    cleanup_temp_repo(original_dir, temp_dir);
}

static void test_malformed_line_rejected(void) {
    Repository repo;
    Index index;
    char original_dir[MG_MAX_PATH];
    char temp_dir[MG_MAX_PATH];
    const unsigned char bad_index[] = "100644\tbad-hash\t1\t2\tfile.txt\n";

    make_temp_repo(&repo, original_dir, temp_dir);
    assert_ok(fs_write_file(".minigit/index", bad_index, sizeof(bad_index) - 1), "write bad index failed");
    assert_result(index_load(&repo, &index), MG_PARSE_ERROR, "malformed index should be rejected");
    cleanup_temp_repo(original_dir, temp_dir);
}

int main(void) {
    test_empty_load();
    test_save_load_update_remove();
    test_malformed_line_rejected();
    puts("test_index passed");
    return 0;
}
