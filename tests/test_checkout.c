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

    written = snprintf(temp_dir, MG_MAX_PATH, "/tmp/minigit_checkout_%ld", (long)getpid());
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

static void read_text_file(const char *path, char **out) {
    unsigned char *data = NULL;
    size_t size = 0;

    assert_result(fs_read_file(path, &data, &size), MG_OK, "read file failed");
    *out = (char *)data;
}

static void commit_file(const char *contents, const char *message, char out_hash[MG_HASH_HEX_SIZE]) {
    Repository repo;
    char *add_args[] = {"file.txt"};
    char *commit_args[] = {"-m", (char *)message};

    assert_result(fs_write_file("file.txt", (const unsigned char *)contents, strlen(contents)), MG_OK, "write file failed");
    assert_result(mg_command_add(1, add_args), MG_OK, "add file failed");
    assert_result(mg_command_commit(2, commit_args), MG_OK, "commit file failed");
    assert_result(repo_open(&repo), MG_OK, "repo_open failed");
    assert_result(repo_current_commit(&repo, out_hash, MG_HASH_HEX_SIZE), MG_OK, "repo_current_commit failed");
}

static void test_detached_checkout_restores_commit(void) {
    char original_dir[MG_MAX_PATH];
    char temp_dir[MG_MAX_PATH];
    char first_hash[MG_HASH_HEX_SIZE];
    char second_hash[MG_HASH_HEX_SIZE];
    char head[MG_MAX_PATH];
    char *checkout_args[] = {first_hash};
    char *file_contents;

    make_temp_dir(original_dir, temp_dir);
    assert_result(mg_command_init(0, NULL), MG_OK, "init failed");
    commit_file("one\n", "one", first_hash);
    commit_file("two\n", "two", second_hash);

    assert_result(mg_command_checkout(1, checkout_args), MG_OK, "checkout first commit failed");
    read_text_file("file.txt", &file_contents);
    assert_true(strcmp(file_contents, "one\n") == 0, "checkout should restore first file contents");
    free(file_contents);

    {
        Repository repo;
        assert_result(repo_open(&repo), MG_OK, "repo_open failed");
        assert_result(repo_read_head(&repo, head, sizeof(head)), MG_OK, "repo_read_head failed");
    }
    assert_true(strcmp(head, first_hash) == 0, "detached HEAD should contain checked out hash");

    (void)second_hash;
    cleanup_temp_dir(original_dir, temp_dir);
}

static void test_checkout_refuses_dirty_tracked_file(void) {
    char original_dir[MG_MAX_PATH];
    char temp_dir[MG_MAX_PATH];
    char first_hash[MG_HASH_HEX_SIZE];
    char second_hash[MG_HASH_HEX_SIZE];
    char *checkout_args[] = {first_hash};
    char *file_contents;

    make_temp_dir(original_dir, temp_dir);
    assert_result(mg_command_init(0, NULL), MG_OK, "init failed");
    commit_file("one\n", "one", first_hash);
    commit_file("two\n", "two", second_hash);
    assert_result(fs_write_file("file.txt", (const unsigned char *)"dirty\n", strlen("dirty\n")), MG_OK, "dirty write failed");

    assert_result(mg_command_checkout(1, checkout_args), MG_CONFLICT, "checkout should refuse dirty tracked file");
    read_text_file("file.txt", &file_contents);
    assert_true(strcmp(file_contents, "dirty\n") == 0, "dirty file should be preserved");
    free(file_contents);

    (void)second_hash;
    cleanup_temp_dir(original_dir, temp_dir);
}

static void test_switch_restores_branch_and_updates_head(void) {
    char original_dir[MG_MAX_PATH];
    char temp_dir[MG_MAX_PATH];
    char first_hash[MG_HASH_HEX_SIZE];
    char second_hash[MG_HASH_HEX_SIZE];
    char head[MG_MAX_PATH];
    char *branch_args[] = {"feature"};
    char *switch_args[] = {"feature"};
    char *file_contents;
    Repository repo;

    make_temp_dir(original_dir, temp_dir);
    assert_result(mg_command_init(0, NULL), MG_OK, "init failed");
    commit_file("one\n", "one", first_hash);
    assert_result(mg_command_branch(1, branch_args), MG_OK, "branch feature failed");
    commit_file("two\n", "two", second_hash);

    assert_result(mg_command_switch(1, switch_args), MG_OK, "switch feature failed");
    read_text_file("file.txt", &file_contents);
    assert_true(strcmp(file_contents, "one\n") == 0, "switch should restore feature branch contents");
    free(file_contents);

    assert_result(repo_open(&repo), MG_OK, "repo_open failed");
    assert_result(repo_read_head(&repo, head, sizeof(head)), MG_OK, "repo_read_head failed");
    assert_true(strcmp(head, "ref: refs/heads/feature") == 0, "HEAD should point to feature branch");

    (void)first_hash;
    (void)second_hash;
    cleanup_temp_dir(original_dir, temp_dir);
}

int main(void) {
    test_detached_checkout_restores_commit();
    test_checkout_refuses_dirty_tracked_file();
    test_switch_restores_branch_and_updates_head();
    puts("test_checkout passed");
    return 0;
}
