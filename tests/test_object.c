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

static void object_file_path_for_hash(const char *hash, char object_dir_path[MG_MAX_PATH], char object_path[MG_MAX_PATH]) {
    char fanout_dir[3];

    fanout_dir[0] = hash[0];
    fanout_dir[1] = hash[1];
    fanout_dir[2] = '\0';
    assert_ok(fs_join_path(".minigit/objects", fanout_dir, object_dir_path, MG_MAX_PATH), "object dir path failed");
    assert_ok(fs_join_path(object_dir_path, hash + 2, object_path, MG_MAX_PATH), "object path failed");
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
    char object_dir_path[MG_MAX_PATH];
    char object_path[MG_MAX_PATH];
    unsigned char *stored = NULL;
    size_t stored_size = 0;
    const unsigned char payload[] = "hello world";

    make_temp_repo(&repo, original_dir, temp_dir);

    assert_ok(object_write(&repo, "blob", payload, strlen((const char *)payload), hash), "object_write failed");
    assert_true(hash_is_valid_hex(hash), "object_write returned invalid hash");

    object_file_path_for_hash(hash, object_dir_path, object_path);
    assert_true(fs_is_file(object_path), "object file was not written under .minigit/objects");
    assert_ok(fs_read_file(object_path, &stored, &stored_size), "read stored object failed");
    assert_true(stored_size > 4 && memcmp(stored, "MGZ1", 4) == 0, "loose object should be zlib wrapped");
    free(stored);

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

static void test_write_read_git_hash_mode_object(void) {
    Repository repo;
    Object object;
    char original_dir[MG_MAX_PATH];
    char temp_dir[MG_MAX_PATH];
    char hash[MG_HASH_HEX_SIZE];
    char object_dir_path[MG_MAX_PATH];
    char object_path[MG_MAX_PATH];
    const unsigned char payload[] = "hello world";
    const char *expected = "95d09f2b10159347eece71399a7e2e907ea3df4f";

    make_temp_repo(&repo, original_dir, temp_dir);
    setenv("MINIGIT_OBJECT_HASH_MODE", "git", 1);

    assert_ok(object_write(&repo, "blob", payload, strlen((const char *)payload), hash), "git mode object_write failed");
    assert_true(strcmp(hash, expected) == 0, "git mode object hash mismatch");

    object_file_path_for_hash(hash, object_dir_path, object_path);
    assert_true(fs_is_file(object_path), "git mode object file was not written");
    assert_ok(object_read(&repo, hash, &object), "git mode object_read failed");
    assert_true(strcmp(object.type, "blob") == 0, "git mode object type mismatch");
    assert_true(object.size == strlen((const char *)payload), "git mode object size mismatch");
    assert_true(memcmp(object.payload, payload, object.size) == 0, "git mode object payload mismatch");
    object_free(&object);

    unsetenv("MINIGIT_OBJECT_HASH_MODE");
    assert_ok(fs_remove_file(object_path), "git mode object cleanup failed");
    assert_true(rmdir(object_dir_path) == 0, "git mode object dir cleanup failed");
    cleanup_temp_repo(original_dir, temp_dir);
}

static void test_packfile_reads_after_loose_removal(void) {
    Repository repo;
    Object object;
    char original_dir[MG_MAX_PATH];
    char temp_dir[MG_MAX_PATH];
    char first_hash[MG_HASH_HEX_SIZE];
    char second_hash[MG_HASH_HEX_SIZE];
    char resolved[MG_HASH_HEX_SIZE];
    char object_dir_path[MG_MAX_PATH];
    char object_path[MG_MAX_PATH];
    size_t packed_count = 0;
    const unsigned char first[] = "first packed";
    const unsigned char second[] = "second packed";

    make_temp_repo(&repo, original_dir, temp_dir);
    assert_ok(object_write(&repo, "blob", first, strlen((const char *)first), first_hash), "first object_write failed");
    assert_ok(object_write(&repo, "blob", second, strlen((const char *)second), second_hash), "second object_write failed");
    assert_ok(object_pack_all(&repo, &packed_count), "object_pack_all failed");
    assert_true(packed_count == 2, "pack should include both loose objects");
    assert_true(fs_is_file(".minigit/objects/pack/minigit.pack"), "packfile should exist");

    object_file_path_for_hash(first_hash, object_dir_path, object_path);
    assert_ok(fs_remove_file(object_path), "remove first loose object failed");
    assert_ok(object_read(&repo, first_hash, &object), "object_read should fall back to pack");
    assert_true(strcmp(object.type, "blob") == 0, "packed object type mismatch");
    assert_true(object.size == strlen((const char *)first), "packed object size mismatch");
    assert_true(memcmp(object.payload, first, object.size) == 0, "packed object payload mismatch");
    object_free(&object);

    assert_ok(object_resolve_prefix(&repo, first_hash, resolved), "packed full hash should resolve");
    assert_true(strcmp(resolved, first_hash) == 0, "packed hash resolution mismatch");

    object_file_path_for_hash(second_hash, object_dir_path, object_path);
    assert_ok(fs_remove_file(object_path), "remove second loose object failed");
    assert_true(rmdir(object_dir_path) == 0, "second object dir cleanup failed");
    object_file_path_for_hash(first_hash, object_dir_path, object_path);
    assert_true(rmdir(object_dir_path) == 0, "first object dir cleanup failed");
    assert_ok(fs_remove_file(".minigit/objects/pack/minigit.pack"), "remove packfile failed");
    assert_true(rmdir(".minigit/objects/pack") == 0, "pack dir cleanup failed");
    cleanup_temp_repo(original_dir, temp_dir);
}

static void test_resolve_unique_object_prefix(void) {
    Repository repo;
    char original_dir[MG_MAX_PATH];
    char temp_dir[MG_MAX_PATH];
    char hash[MG_HASH_HEX_SIZE];
    char prefix[8];
    char resolved[MG_HASH_HEX_SIZE];
    const unsigned char payload[] = "prefix target";

    make_temp_repo(&repo, original_dir, temp_dir);

    assert_ok(object_write(&repo, "blob", payload, strlen((const char *)payload), hash), "object_write failed");
    memcpy(prefix, hash, 7);
    prefix[7] = '\0';

    assert_ok(object_resolve_prefix(&repo, prefix, resolved), "object_resolve_prefix failed");
    assert_true(strcmp(resolved, hash) == 0, "short prefix should resolve to full hash");

    assert_ok(object_resolve_prefix(&repo, hash, resolved), "full hash should resolve");
    assert_true(strcmp(resolved, hash) == 0, "full hash resolution mismatch");

    {
        char fanout_dir[3];
        char object_dir_path[MG_MAX_PATH];
        char object_path[MG_MAX_PATH];

        fanout_dir[0] = hash[0];
        fanout_dir[1] = hash[1];
        fanout_dir[2] = '\0';
        assert_ok(fs_join_path(".minigit/objects", fanout_dir, object_dir_path, sizeof(object_dir_path)), "object dir path failed");
        assert_ok(fs_join_path(object_dir_path, hash + 2, object_path, sizeof(object_path)), "object path failed");
        assert_ok(fs_remove_file(object_path), "object cleanup failed");
        assert_true(rmdir(object_dir_path) == 0, "object dir cleanup failed");
    }
    cleanup_temp_repo(original_dir, temp_dir);
}

static void write_fake_object_path(const char *hash) {
    char fanout_dir[3];
    char object_dir_path[MG_MAX_PATH];
    char object_path[MG_MAX_PATH];
    const unsigned char payload[] = "blob 0";

    fanout_dir[0] = hash[0];
    fanout_dir[1] = hash[1];
    fanout_dir[2] = '\0';
    assert_ok(fs_join_path(".minigit/objects", fanout_dir, object_dir_path, sizeof(object_dir_path)), "fake object dir path failed");
    assert_ok(fs_mkdir_p(object_dir_path), "fake object dir mkdir failed");
    assert_ok(fs_join_path(object_dir_path, hash + 2, object_path, sizeof(object_path)), "fake object path failed");
    assert_ok(fs_write_file(object_path, payload, sizeof(payload)), "fake object write failed");
}

static void test_resolve_ambiguous_object_prefix(void) {
    Repository repo;
    char original_dir[MG_MAX_PATH];
    char temp_dir[MG_MAX_PATH];
    char resolved[MG_HASH_HEX_SIZE];
    const char *first = "abcdef0111111111111111111111111111111111111111111111111111111111";
    const char *second = "abcdef0222222222222222222222222222222222222222222222222222222222";

    make_temp_repo(&repo, original_dir, temp_dir);
    write_fake_object_path(first);
    write_fake_object_path(second);

    assert_result(object_resolve_prefix(&repo, "abcdef0", resolved), MG_CONFLICT, "ambiguous prefix should conflict");
    assert_result(object_resolve_prefix(&repo, "abc1234", resolved), MG_NOT_FOUND, "unknown prefix should not resolve");
    assert_result(object_resolve_prefix(&repo, "abcdef", resolved), MG_INVALID_ARG, "short prefix should be invalid");

    {
        char object_dir_path[MG_MAX_PATH];
        char object_path[MG_MAX_PATH];

        assert_ok(fs_join_path(".minigit/objects", "ab", object_dir_path, sizeof(object_dir_path)), "fake cleanup dir failed");
        assert_ok(fs_join_path(object_dir_path, first + 2, object_path, sizeof(object_path)), "fake cleanup first failed");
        assert_ok(fs_remove_file(object_path), "fake cleanup first remove failed");
        assert_ok(fs_join_path(object_dir_path, second + 2, object_path, sizeof(object_path)), "fake cleanup second failed");
        assert_ok(fs_remove_file(object_path), "fake cleanup second remove failed");
        assert_true(rmdir(object_dir_path) == 0, "fake object dir cleanup failed");
    }
    cleanup_temp_repo(original_dir, temp_dir);
}

static void test_tampered_and_oversized_objects_rejected(void) {
    Repository repo;
    Object object;
    char original_dir[MG_MAX_PATH];
    char temp_dir[MG_MAX_PATH];
    char hash[MG_HASH_HEX_SIZE];
    char object_dir_path[MG_MAX_PATH];
    char object_path[MG_MAX_PATH];
    const unsigned char payload[] = "good\n";
    const unsigned char tampered[] = "blob 5\0evil\n";
    const unsigned char oversized[] = "MGZ1 67108865\nx";

    make_temp_repo(&repo, original_dir, temp_dir);
    assert_ok(object_write(&repo, "blob", payload, sizeof(payload) - 1, hash), "write object for tamper test failed");
    object_file_path_for_hash(hash, object_dir_path, object_path);

    assert_ok(fs_write_file(object_path, tampered, sizeof(tampered) - 1), "write tampered object failed");
    assert_result(object_read(&repo, hash, &object), MG_PARSE_ERROR, "tampered object hash should be rejected");

    assert_ok(fs_write_file(object_path, oversized, sizeof(oversized) - 1), "write oversized object failed");
    assert_result(object_read(&repo, hash, &object), MG_PARSE_ERROR, "oversized object should be rejected");

    assert_ok(fs_remove_file(object_path), "tampered object cleanup failed");
    assert_true(rmdir(object_dir_path) == 0, "tampered object directory cleanup failed");
    cleanup_temp_repo(original_dir, temp_dir);
}

int main(void) {
    test_write_read_blob_object();
    test_write_read_git_hash_mode_object();
    test_packfile_reads_after_loose_removal();
    test_resolve_unique_object_prefix();
    test_resolve_ambiguous_object_prefix();
    test_tampered_and_oversized_objects_rejected();
    puts("test_object passed");
    return 0;
}
