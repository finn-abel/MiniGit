#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fs.h"
#include "hash.h"

static void assert_ok(MGResult result, const char *message) {
    if (result != MG_OK) {
        fprintf(stderr, "%s\n", message);
        exit(1);
    }
}

static void test_hash_bytes_known_value(void) {
    char hash[MG_HASH_HEX_SIZE];
    const unsigned char data[] = "hello world";
    const char *expected = "b94d27b9934d3e08a52e52d7da7dabfac484efe37a5380ee9088f7ace2efcde9";

    assert_ok(hash_bytes(data, strlen((const char *)data), hash), "hash_bytes failed");
    if (strcmp(hash, expected) != 0) {
        fprintf(stderr, "hash_bytes returned unexpected digest\n");
        exit(1);
    }
}

static void test_hash_file_matches_bytes(void) {
    const char *path = "tests/tmp_hash_file.dat";
    const unsigned char data[] = {0x00, 'h', 'a', 's', 'h', 0xff};
    char file_hash[MG_HASH_HEX_SIZE];
    char bytes_hash[MG_HASH_HEX_SIZE];

    assert_ok(fs_write_file(path, data, sizeof(data)), "fs_write_file failed");
    assert_ok(hash_file(path, file_hash), "hash_file failed");
    assert_ok(hash_bytes(data, sizeof(data), bytes_hash), "hash_bytes failed");

    if (strcmp(file_hash, bytes_hash) != 0) {
        fprintf(stderr, "hash_file did not match hash_bytes\n");
        exit(1);
    }

    assert_ok(fs_remove_file(path), "fs_remove_file failed");
}

static void test_hash_validation(void) {
    const char *valid = "0123456789abcdef0123456789ABCDEF0123456789abcdef0123456789ABCDEF";

    if (!hash_is_valid_hex(valid)) {
        fprintf(stderr, "valid hash was rejected\n");
        exit(1);
    }
    if (hash_is_valid_hex("abc") || hash_is_valid_hex("zzzz456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef")) {
        fprintf(stderr, "invalid hash was accepted\n");
        exit(1);
    }
}

int main(void) {
    test_hash_bytes_known_value();
    test_hash_file_matches_bytes();
    test_hash_validation();
    puts("test_hash passed");
    return 0;
}
