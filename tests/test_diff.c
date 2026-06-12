#define _POSIX_C_SOURCE 200809L

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "commands.h"
#include "common.h"
#include "fs.h"

static void assert_result(MGResult actual, MGResult expected, const char *message) {
    if (actual != expected) {
        fprintf(stderr, "%s\n", message);
        exit(1);
    }
}

static void assert_contains(const char *haystack, const char *needle, const char *message) {
    if (strstr(haystack, needle) == NULL) {
        fprintf(stderr, "%s\n", message);
        fprintf(stderr, "missing: %s\noutput:\n%s\n", needle, haystack);
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

static void capture_stdout_start(const char *path, int *saved_stdout) {
    fflush(stdout);
    *saved_stdout = dup(STDOUT_FILENO);
    if (*saved_stdout < 0) {
        fprintf(stderr, "dup stdout failed\n");
        exit(1);
    }
    if (freopen(path, "wb", stdout) == NULL) {
        fprintf(stderr, "freopen stdout failed\n");
        exit(1);
    }
}

static char *capture_stdout_end(int saved_stdout, const char *path) {
    unsigned char *data = NULL;
    size_t size = 0;

    fflush(stdout);
    if (dup2(saved_stdout, STDOUT_FILENO) < 0) {
        fprintf(stderr, "restore stdout failed\n");
        exit(1);
    }
    close(saved_stdout);
    clearerr(stdout);

    assert_result(fs_read_file(path, &data, &size), MG_OK, "read captured stdout failed");
    (void)size;
    return (char *)data;
}

static char *capture_diff(int argc, char **argv) {
    int saved_stdout;

    capture_stdout_start("diff.out", &saved_stdout);
    assert_result(mg_command_diff(argc, argv), MG_OK, "diff command failed");
    return capture_stdout_end(saved_stdout, "diff.out");
}

static void test_diff_modes(void) {
    char original_dir[MG_MAX_PATH];
    char temp_dir[MG_MAX_PATH];
    char *add_args[] = {"file.txt"};
    char *commit_args[] = {"-m", "initial"};
    char *staged_args[] = {"--staged"};
    char *head_args[] = {"HEAD"};
    char *output;

    make_temp_dir("diff", original_dir, temp_dir);

    assert_result(mg_command_diff(0, NULL), MG_REPO_ERROR, "diff should require a repo");
    assert_result(mg_command_init(0, NULL), MG_OK, "init failed");
    assert_result(fs_write_file("file.txt", (const unsigned char *)"one\ntwo\n", 8), MG_OK, "write file failed");
    assert_result(mg_command_add(1, add_args), MG_OK, "add failed");
    assert_result(mg_command_commit(2, commit_args), MG_OK, "commit failed");

    assert_result(fs_write_file("file.txt", (const unsigned char *)"one\nTWO\nthree\n", 14), MG_OK, "modify file failed");
    output = capture_diff(0, NULL);
    assert_contains(output, "diff --minigit file.txt\n", "unstaged diff should name path");
    assert_contains(output, "-two\n", "unstaged diff should include removed line");
    assert_contains(output, "+TWO\n", "unstaged diff should include changed line");
    assert_contains(output, "+three\n", "unstaged diff should include inserted line");
    free(output);

    assert_result(mg_command_add(1, add_args), MG_OK, "second add failed");
    output = capture_diff(1, staged_args);
    assert_contains(output, "diff --minigit file.txt\n", "staged diff should name path");
    assert_contains(output, "-two\n", "staged diff should include removed line");
    assert_contains(output, "+three\n", "staged diff should include inserted line");
    free(output);

    assert_result(fs_write_file("file.txt", (const unsigned char *)"one\nTWO\nthree\nfour\n", 19), MG_OK, "second modify failed");
    output = capture_diff(1, head_args);
    assert_contains(output, "-two\n", "HEAD diff should include committed old line");
    assert_contains(output, "+four\n", "HEAD diff should include working-tree line");
    free(output);

    cleanup_temp_dir(original_dir, temp_dir);
}

static void test_diff_mode_change(void) {
    char original_dir[MG_MAX_PATH];
    char temp_dir[MG_MAX_PATH];
    char *add_args[] = {"script.sh"};
    char *commit_args[] = {"-m", "script"};
    char *staged_args[] = {"--staged"};
    char *output;

    make_temp_dir("diff_mode", original_dir, temp_dir);

    assert_result(mg_command_init(0, NULL), MG_OK, "init failed");
    assert_result(fs_write_file("script.sh", (const unsigned char *)"#!/bin/sh\n", 10), MG_OK, "write script failed");
    assert_result(mg_command_add(1, add_args), MG_OK, "add failed");
    assert_result(mg_command_commit(2, commit_args), MG_OK, "commit failed");
    assert_result(chmod("script.sh", 0755) == 0 ? MG_OK : MG_IO_ERROR, MG_OK, "chmod failed");

    output = capture_diff(0, NULL);
    assert_contains(output, "diff --minigit script.sh\n", "mode diff should name path");
    assert_contains(output, "old mode 100644\n", "mode diff should show old mode");
    assert_contains(output, "new mode 100755\n", "mode diff should show new mode");
    free(output);

    assert_result(mg_command_add(1, add_args), MG_OK, "stage mode failed");
    output = capture_diff(1, staged_args);
    assert_contains(output, "old mode 100644\n", "staged mode diff should show old mode");
    assert_contains(output, "new mode 100755\n", "staged mode diff should show new mode");
    free(output);

    cleanup_temp_dir(original_dir, temp_dir);
}

int main(void) {
    test_diff_modes();
    test_diff_mode_change();
    puts("All diff tests passed.");
    return 0;
}
