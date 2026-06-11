#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fs.h"

static void assert_ok(MGResult result, const char *message) {
    if (result != MG_OK) {
        fprintf(stderr, "%s\n", message);
        exit(1);
    }
}

static void test_binary_read_write(void) {
    const char *path = "tests/tmp_fs_binary.dat";
    const unsigned char expected[] = {0x00, 0x01, 'm', 'g', 0xff};
    unsigned char *actual = NULL;
    size_t actual_size = 0;

    assert_ok(fs_write_file(path, expected, sizeof(expected)), "fs_write_file failed");
    assert_ok(fs_read_file(path, &actual, &actual_size), "fs_read_file failed");

    if (actual_size != sizeof(expected) || memcmp(actual, expected, sizeof(expected)) != 0) {
        fprintf(stderr, "binary file contents did not round-trip\n");
        free(actual);
        exit(1);
    }

    free(actual);
    assert_ok(fs_remove_file(path), "fs_remove_file failed");
}

int main(void) {
    test_binary_read_write();
    puts("All fs tests passed.");
    return 0;
}
