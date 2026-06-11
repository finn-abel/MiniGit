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

    written = snprintf(temp_dir, MG_MAX_PATH, "/tmp/minigit_status_%ld", (long)getpid());
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

static char *capture_status_output(void) {
    const char *path = ".minigit/status-output.txt";
    FILE *file;
    int saved_stdout;
    unsigned char *data = NULL;
    size_t size = 0;

    fflush(stdout);
    saved_stdout = dup(STDOUT_FILENO);
    if (saved_stdout < 0) {
        fprintf(stderr, "dup stdout failed\n");
        exit(1);
    }

    file = fopen(path, "wb");
    if (file == NULL) {
        fprintf(stderr, "open capture file failed\n");
        exit(1);
    }
    if (dup2(fileno(file), STDOUT_FILENO) < 0) {
        fprintf(stderr, "redirect stdout failed\n");
        exit(1);
    }

    assert_result(mg_command_status(0, NULL), MG_OK, "status command failed");
    fflush(stdout);

    if (dup2(saved_stdout, STDOUT_FILENO) < 0) {
        fprintf(stderr, "restore stdout failed\n");
        exit(1);
    }
    close(saved_stdout);
    fclose(file);

    assert_result(fs_read_file(path, &data, &size), MG_OK, "read captured status failed");
    assert_result(fs_remove_file(path), MG_OK, "remove captured status failed");
    return (char *)data;
}

static void assert_status_equals(const char *expected) {
    char *actual = capture_status_output();
    if (strcmp(actual, expected) != 0) {
        fprintf(stderr, "status output mismatch\nexpected:\n%sactual:\n%s", expected, actual);
        free(actual);
        exit(1);
    }
    free(actual);
}

static void test_status_lifecycle(void) {
    char original_dir[MG_MAX_PATH];
    char temp_dir[MG_MAX_PATH];
    char *add_readme_args[] = {"README.md"};
    char *commit_args[] = {"-m", "initial commit"};
    const unsigned char hello[] = "hello\n";
    const unsigned char changed[] = "changed\n";

    make_temp_dir(original_dir, temp_dir);

    assert_result(mg_command_init(0, NULL), MG_OK, "init failed");
    assert_result(fs_write_file("README.md", hello, sizeof(hello) - 1), MG_OK, "write README failed");
    assert_status_equals("Untracked files:\n  README.md\n");

    assert_result(mg_command_add(1, add_readme_args), MG_OK, "add README failed");
    assert_status_equals("Changes to be committed:\n  added: README.md\n");

    assert_result(mg_command_commit(2, commit_args), MG_OK, "commit failed");
    assert_result(fs_write_file("README.md", changed, sizeof(changed) - 1), MG_OK, "modify README failed");
    assert_status_equals("Changes not staged for commit:\n  modified: README.md\n");

    cleanup_temp_dir(original_dir, temp_dir);
}

static void test_rm_stages_deletion(void) {
    char original_dir[MG_MAX_PATH];
    char temp_dir[MG_MAX_PATH];
    char *add_temp_args[] = {"temp.txt"};
    char *commit_args[] = {"-m", "add temp"};
    char *rm_args[] = {"temp.txt"};
    const unsigned char temp[] = "temp\n";

    make_temp_dir(original_dir, temp_dir);

    assert_result(mg_command_init(0, NULL), MG_OK, "init failed");
    assert_result(fs_write_file("temp.txt", temp, sizeof(temp) - 1), MG_OK, "write temp failed");
    assert_result(mg_command_add(1, add_temp_args), MG_OK, "add temp failed");
    assert_result(mg_command_commit(2, commit_args), MG_OK, "commit temp failed");
    assert_result(mg_command_rm(1, rm_args), MG_OK, "rm temp failed");
    assert_true(!fs_exists("temp.txt"), "rm should delete working tree file");
    assert_status_equals("Changes to be committed:\n  deleted: temp.txt\n");

    cleanup_temp_dir(original_dir, temp_dir);
}

static void test_rm_rejects_untracked_path(void) {
    char original_dir[MG_MAX_PATH];
    char temp_dir[MG_MAX_PATH];
    char *rm_args[] = {"missing.txt"};

    make_temp_dir(original_dir, temp_dir);

    assert_result(mg_command_init(0, NULL), MG_OK, "init failed");
    assert_result(mg_command_rm(1, rm_args), MG_NOT_FOUND, "rm should reject untracked path");

    cleanup_temp_dir(original_dir, temp_dir);
}

static void test_status_deleted_working_file(void) {
    char original_dir[MG_MAX_PATH];
    char temp_dir[MG_MAX_PATH];
    char *add_args[] = {"tracked.txt"};
    char *commit_args[] = {"-m", "track file"};
    const unsigned char contents[] = "tracked\n";

    make_temp_dir(original_dir, temp_dir);

    assert_result(mg_command_init(0, NULL), MG_OK, "init failed");
    assert_result(fs_write_file("tracked.txt", contents, sizeof(contents) - 1), MG_OK, "write tracked failed");
    assert_result(mg_command_add(1, add_args), MG_OK, "add tracked failed");
    assert_result(mg_command_commit(2, commit_args), MG_OK, "commit tracked failed");
    assert_result(fs_remove_file("tracked.txt"), MG_OK, "remove tracked working file failed");
    assert_status_equals("Changes not staged for commit:\n  deleted: tracked.txt\n");

    cleanup_temp_dir(original_dir, temp_dir);
}

static void test_status_staged_modification(void) {
    char original_dir[MG_MAX_PATH];
    char temp_dir[MG_MAX_PATH];
    char *add_args[] = {"tracked.txt"};
    char *commit_args[] = {"-m", "track file"};
    const unsigned char original[] = "tracked\n";
    const unsigned char changed[] = "changed\n";

    make_temp_dir(original_dir, temp_dir);

    assert_result(mg_command_init(0, NULL), MG_OK, "init failed");
    assert_result(fs_write_file("tracked.txt", original, sizeof(original) - 1), MG_OK, "write tracked failed");
    assert_result(mg_command_add(1, add_args), MG_OK, "add tracked failed");
    assert_result(mg_command_commit(2, commit_args), MG_OK, "commit tracked failed");
    assert_result(fs_write_file("tracked.txt", changed, sizeof(changed) - 1), MG_OK, "modify tracked failed");
    assert_result(mg_command_add(1, add_args), MG_OK, "add modified tracked failed");
    assert_status_equals("Changes to be committed:\n  modified: tracked.txt\n");

    cleanup_temp_dir(original_dir, temp_dir);
}

static void test_status_sorted_untracked_files(void) {
    char original_dir[MG_MAX_PATH];
    char temp_dir[MG_MAX_PATH];
    const unsigned char contents[] = "x\n";

    make_temp_dir(original_dir, temp_dir);

    assert_result(mg_command_init(0, NULL), MG_OK, "init failed");
    assert_result(fs_write_file("z.txt", contents, sizeof(contents) - 1), MG_OK, "write z failed");
    assert_result(fs_write_file("a.txt", contents, sizeof(contents) - 1), MG_OK, "write a failed");
    assert_status_equals("Untracked files:\n  a.txt\n  z.txt\n");

    cleanup_temp_dir(original_dir, temp_dir);
}

static void test_status_respects_minigitignore(void) {
    char original_dir[MG_MAX_PATH];
    char temp_dir[MG_MAX_PATH];
    const unsigned char ignore[] = "ignored.txt\nlogs/\n";
    const unsigned char contents[] = "x\n";

    make_temp_dir(original_dir, temp_dir);

    assert_result(mg_command_init(0, NULL), MG_OK, "init failed");
    assert_result(fs_write_file(".minigitignore", ignore, sizeof(ignore) - 1), MG_OK, "write ignore failed");
    assert_result(fs_write_file("ignored.txt", contents, sizeof(contents) - 1), MG_OK, "write ignored failed");
    assert_result(mkdir("logs", 0700) == 0 ? MG_OK : MG_IO_ERROR, MG_OK, "mkdir logs failed");
    assert_result(fs_write_file("logs/run.log", contents, sizeof(contents) - 1), MG_OK, "write log failed");
    assert_result(fs_write_file("keep.txt", contents, sizeof(contents) - 1), MG_OK, "write keep failed");

    assert_status_equals("Untracked files:\n  .minigitignore\n  keep.txt\n");

    cleanup_temp_dir(original_dir, temp_dir);
}

int main(void) {
    test_status_lifecycle();
    test_rm_stages_deletion();
    test_rm_rejects_untracked_path();
    test_status_deleted_working_file();
    test_status_staged_modification();
    test_status_sorted_untracked_files();
    test_status_respects_minigitignore();
    puts("test_status passed");
    return 0;
}
