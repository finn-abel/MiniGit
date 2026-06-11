#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "fs.h"
#include "hash.h"
#include "object.h"
#include "repository.h"

static void assert_ok(MGResult result, const char *message) {
    if (result != MG_OK) {
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

    written = snprintf(temp_dir, MG_MAX_PATH, "/tmp/minigit_object_%ld", (long)getpid());
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

static void test_write_read_blob_object(void) {
    Repository repo;
    Object object;
    char original_dir[MG_MAX_PATH];
    char temp_dir[MG_MAX_PATH];
    char hash[MG_HASH_HEX_SIZE];
    char fanout_dir[3];
    char object_dir_path[MG_MAX_PATH];
    char object_path[MG_MAX_PATH];
    const unsigned char payload[] = "hello world";

    make_temp_repo(&repo, original_dir, temp_dir);

    assert_ok(object_write(&repo, "blob", payload, strlen((const char *)payload), hash), "object_write failed");
    assert_true(hash_is_valid_hex(hash), "object_write returned invalid hash");

    fanout_dir[0] = hash[0];
    fanout_dir[1] = hash[1];
    fanout_dir[2] = '\0';
    assert_ok(fs_join_path(".minigit/objects", fanout_dir, object_dir_path, sizeof(object_dir_path)), "object dir path failed");
    assert_ok(fs_join_path(object_dir_path, hash + 2, object_path, sizeof(object_path)), "object path failed");
    assert_true(fs_is_file(object_path), "object file was not written under .minigit/objects");

    assert_ok(object_read(&repo, hash, &object), "object_read failed");
    assert_true(strcmp(object.type, "blob") == 0, "object type mismatch");
    assert_true(object.size == strlen((const char *)payload), "object size mismatch");
    assert_true(memcmp(object.payload, payload, object.size) == 0, "object payload mismatch");
    object_free(&object);

    assert_ok(object_write(&repo, "blob", payload, strlen((const char *)payload), hash), "duplicate object_write failed");

    assert_ok(fs_remove_file(object_path), "object cleanup failed");
    assert_true(rmdir(object_dir_path) == 0, "object dir cleanup failed");
    cleanup_temp_repo(original_dir, temp_dir);
}

int main(void) {
    test_write_read_blob_object();
    puts("test_object passed");
    return 0;
}
