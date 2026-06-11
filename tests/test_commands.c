#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

#include "commands.h"
#include "common.h"

static void assert_result(MGResult actual, MGResult expected, const char *message) {
    if (actual != expected) {
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

static void test_repo_required_commands(void) {
    char original_dir[MG_MAX_PATH];
    char temp_dir[MG_MAX_PATH];
    int written;

    if (getcwd(original_dir, sizeof(original_dir)) == NULL) {
        fprintf(stderr, "getcwd failed\n");
        exit(1);
    }

    written = snprintf(temp_dir, sizeof(temp_dir), "/tmp/minigit_commands_%ld", (long)getpid());
    if (written < 0 || (size_t)written >= sizeof(temp_dir)) {
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

    assert_result(mg_command_status(0, NULL), MG_REPO_ERROR, "status should require a repo");
    assert_result(mg_command_init(0, NULL), MG_OK, "init should create a repo");
    assert_result(mg_command_status(0, NULL), MG_OK, "status should open an initialized repo");
    assert_result(mg_command_init(0, NULL), MG_REPO_ERROR, "init should refuse existing repo");

    remove_initialized_repo();
    if (chdir(original_dir) != 0) {
        fprintf(stderr, "chdir back failed\n");
        exit(1);
    }
    (void)rmdir(temp_dir);
}

int main(void) {
    test_repo_required_commands();
    puts("All commands tests passed.");
    return 0;
}
