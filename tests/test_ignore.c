#define _POSIX_C_SOURCE 200809L

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "fs.h"
#include "ignore.h"
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

static void make_temp_repo(Repository *repo, char original_dir[MG_MAX_PATH], char temp_dir[MG_MAX_PATH]) {
    int written;

    if (getcwd(original_dir, MG_MAX_PATH) == NULL) {
        fprintf(stderr, "getcwd failed\n");
        exit(1);
    }

    written = snprintf(temp_dir, MG_MAX_PATH, "/tmp/minigit_ignore_%ld", (long)getpid());
    if (written < 0 || (size_t)written >= MG_MAX_PATH) {
        fprintf(stderr, "temp path too long\n");
        exit(1);
    }
    if (mkdir(temp_dir, 0700) != 0 || chdir(temp_dir) != 0) {
        fprintf(stderr, "temp dir setup failed\n");
        exit(1);
    }

    assert_result(repo_init(repo), MG_OK, "repo_init failed");
}

static void cleanup_temp_repo(const char *original_dir, const char *temp_dir) {
    if (chdir(original_dir) != 0) {
        fprintf(stderr, "chdir back failed\n");
        exit(1);
    }
    remove_recursive(temp_dir);
}

static void test_missing_ignore_file_loads_empty_rules(void) {
    Repository repo;
    IgnoreRules rules;
    char original_dir[MG_MAX_PATH];
    char temp_dir[MG_MAX_PATH];

    make_temp_repo(&repo, original_dir, temp_dir);
    assert_result(ignore_rules_load(&repo, &rules), MG_OK, "missing ignore load failed");
    assert_true(rules.count == 0, "missing ignore file should produce no rules");
    assert_true(!ignore_rules_match(&rules, "file.txt"), "empty rules should not match");
    ignore_rules_free(&rules);
    cleanup_temp_repo(original_dir, temp_dir);
}

static void test_ignore_exact_and_directory_patterns(void) {
    Repository repo;
    IgnoreRules rules;
    char original_dir[MG_MAX_PATH];
    char temp_dir[MG_MAX_PATH];
    const unsigned char contents[] = "# comment\n\nignored.txt\n/logs/\n";

    make_temp_repo(&repo, original_dir, temp_dir);
    assert_result(fs_write_file(".minigitignore", contents, sizeof(contents) - 1), MG_OK, "write ignore failed");
    assert_result(ignore_rules_load(&repo, &rules), MG_OK, "ignore load failed");

    assert_true(rules.count == 2, "comments and blanks should be skipped");
    assert_true(ignore_rules_match(&rules, "ignored.txt"), "exact pattern should match");
    assert_true(!ignore_rules_match(&rules, "nested/ignored.txt"), "exact pattern should not match nested path");
    assert_true(ignore_rules_match(&rules, "logs/run.log"), "directory pattern should match child");
    assert_true(ignore_rules_match(&rules, "logs/nested/run.log"), "directory pattern should match nested child");
    assert_true(!ignore_rules_match(&rules, "logs"), "directory pattern should not match the directory name alone");
    assert_true(!ignore_rules_match(&rules, "keep.txt"), "unlisted file should not match");

    ignore_rules_free(&rules);
    cleanup_temp_repo(original_dir, temp_dir);
}

int main(void) {
    test_missing_ignore_file_loads_empty_rules();
    test_ignore_exact_and_directory_patterns();
    puts("test_ignore passed");
    return 0;
}
