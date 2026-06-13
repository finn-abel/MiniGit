#define _POSIX_C_SOURCE 200809L

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "commands.h"
#include "commit.h"
#include "common.h"
#include "fs.h"
#include "index.h"
#include "repository.h"
#include "tree.h"

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

static void commit_named_file(const char *path, const char *contents, const char *message, char out_hash[MG_HASH_HEX_SIZE]) {
    Repository repo;
    char parent_path[MG_MAX_PATH];
    char *add_args[] = {(char *)path};
    char *commit_args[] = {"-m", (char *)message};

    assert_result(fs_parent_dir(path, parent_path, sizeof(parent_path)), MG_OK, "parent path failed");
    if (strcmp(parent_path, ".") != 0) {
        assert_result(fs_mkdir_p(parent_path), MG_OK, "mkdir parent failed");
    }
    assert_result(fs_write_file(path, (const unsigned char *)contents, strlen(contents)), MG_OK, "write file failed");
    assert_result(mg_command_add(1, add_args), MG_OK, "add file failed");
    assert_result(mg_command_commit(2, commit_args), MG_OK, "commit file failed");
    assert_result(repo_open(&repo), MG_OK, "repo_open failed");
    assert_result(repo_current_commit(&repo, out_hash, MG_HASH_HEX_SIZE), MG_OK, "repo_current_commit failed");
}

static void commit_file(const char *contents, const char *message, char out_hash[MG_HASH_HEX_SIZE]) {
    commit_named_file("file.txt", contents, message, out_hash);
}

static void test_detached_checkout_restores_commit(void) {
    char original_dir[MG_MAX_PATH];
    char temp_dir[MG_MAX_PATH];
    char first_hash[MG_HASH_HEX_SIZE];
    char second_hash[MG_HASH_HEX_SIZE];
    char first_prefix[8];
    char head[MG_MAX_PATH];
    char *checkout_args[] = {first_prefix};
    char *file_contents;

    make_temp_dir(original_dir, temp_dir);
    assert_result(mg_command_init(0, NULL), MG_OK, "init failed");
    commit_file("one\n", "one", first_hash);
    commit_file("two\n", "two", second_hash);
    memcpy(first_prefix, first_hash, 7);
    first_prefix[7] = '\0';

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

static void test_checkout_refuses_untracked_target_file(void) {
    char original_dir[MG_MAX_PATH];
    char temp_dir[MG_MAX_PATH];
    char first_hash[MG_HASH_HEX_SIZE];
    char second_hash[MG_HASH_HEX_SIZE];
    char *checkout_first_args[] = {first_hash};
    char *checkout_second_args[] = {second_hash};
    char *file_contents;

    make_temp_dir(original_dir, temp_dir);
    assert_result(mg_command_init(0, NULL), MG_OK, "init failed");
    commit_file("one\n", "one", first_hash);
    commit_named_file("extra.txt", "committed\n", "add extra", second_hash);
    assert_result(mg_command_checkout(1, checkout_first_args), MG_OK, "checkout first commit failed");
    assert_result(fs_write_file("extra.txt", (const unsigned char *)"local\n", strlen("local\n")), MG_OK, "write untracked file failed");

    assert_result(mg_command_checkout(1, checkout_second_args), MG_CONFLICT, "checkout should refuse untracked target file");
    read_text_file("extra.txt", &file_contents);
    assert_true(strcmp(file_contents, "local\n") == 0, "untracked file should be preserved");
    free(file_contents);

    cleanup_temp_dir(original_dir, temp_dir);
}

static void test_checkout_allows_clean_file_to_directory_transition(void) {
    char original_dir[MG_MAX_PATH];
    char temp_dir[MG_MAX_PATH];
    char first_hash[MG_HASH_HEX_SIZE];
    char second_hash[MG_HASH_HEX_SIZE];
    char *checkout_first_args[] = {first_hash};
    char *checkout_second_args[] = {second_hash};
    char *rm_args[] = {"node"};
    char *file_contents;

    make_temp_dir(original_dir, temp_dir);
    assert_result(mg_command_init(0, NULL), MG_OK, "init failed");
    commit_named_file("node", "file\n", "file", first_hash);
    assert_result(mg_command_rm(1, rm_args), MG_OK, "rm node failed");
    commit_named_file("node/file.txt", "nested\n", "directory", second_hash);

    assert_result(mg_command_checkout(1, checkout_first_args), MG_OK, "checkout file commit failed");
    assert_true(fs_is_file("node"), "node should be restored as a file");
    assert_result(mg_command_checkout(1, checkout_second_args), MG_OK, "checkout directory commit failed");
    assert_true(fs_is_dir("node"), "node should become a directory");
    read_text_file("node/file.txt", &file_contents);
    assert_true(strcmp(file_contents, "nested\n") == 0, "nested file should be restored");
    free(file_contents);

    cleanup_temp_dir(original_dir, temp_dir);
}

static void test_checkout_allows_clean_directory_to_file_transition(void) {
    char original_dir[MG_MAX_PATH];
    char temp_dir[MG_MAX_PATH];
    char first_hash[MG_HASH_HEX_SIZE];
    char second_hash[MG_HASH_HEX_SIZE];
    char *checkout_first_args[] = {first_hash};
    char *checkout_second_args[] = {second_hash};
    char *rm_args[] = {"node/file.txt"};
    char *file_contents;

    make_temp_dir(original_dir, temp_dir);
    assert_result(mg_command_init(0, NULL), MG_OK, "init failed");
    commit_named_file("node/file.txt", "nested\n", "directory", first_hash);
    assert_result(mg_command_rm(1, rm_args), MG_OK, "rm nested file failed");
    assert_true(rmdir("node") == 0, "remove empty node directory failed");
    commit_named_file("node", "file\n", "file", second_hash);

    assert_result(mg_command_checkout(1, checkout_first_args), MG_OK, "checkout directory commit failed");
    assert_true(fs_is_dir("node"), "node should be restored as a directory");
    assert_result(mg_command_checkout(1, checkout_second_args), MG_OK, "checkout file commit failed");
    assert_true(fs_is_file("node"), "node should become a file");
    read_text_file("node", &file_contents);
    assert_true(strcmp(file_contents, "file\n") == 0, "file should be restored");
    free(file_contents);

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

static void test_switch_refuses_untracked_target_file(void) {
    char original_dir[MG_MAX_PATH];
    char temp_dir[MG_MAX_PATH];
    char first_hash[MG_HASH_HEX_SIZE];
    char second_hash[MG_HASH_HEX_SIZE];
    char head[MG_MAX_PATH];
    char *branch_args[] = {"feature"};
    char *switch_feature_args[] = {"feature"};
    char *switch_main_args[] = {"main"};
    char *file_contents;
    Repository repo;

    make_temp_dir(original_dir, temp_dir);
    assert_result(mg_command_init(0, NULL), MG_OK, "init failed");
    commit_file("one\n", "one", first_hash);
    assert_result(mg_command_branch(1, branch_args), MG_OK, "branch feature failed");
    commit_named_file("extra.txt", "committed\n", "add extra", second_hash);
    assert_result(mg_command_switch(1, switch_feature_args), MG_OK, "switch feature failed");
    assert_result(fs_write_file("extra.txt", (const unsigned char *)"local\n", strlen("local\n")), MG_OK, "write untracked file failed");

    assert_result(mg_command_switch(1, switch_main_args), MG_CONFLICT, "switch should refuse untracked target file");
    read_text_file("extra.txt", &file_contents);
    assert_true(strcmp(file_contents, "local\n") == 0, "untracked file should be preserved");
    free(file_contents);

    assert_result(repo_open(&repo), MG_OK, "repo_open failed");
    assert_result(repo_read_head(&repo, head, sizeof(head)), MG_OK, "repo_read_head failed");
    assert_true(strcmp(head, "ref: refs/heads/feature") == 0, "failed switch should leave HEAD on feature");

    (void)first_hash;
    (void)second_hash;
    cleanup_temp_dir(original_dir, temp_dir);
}

static void test_restore_path_from_head(void) {
    Repository repo;
    Commit commit;
    Tree tree;
    Index index;
    const TreeEntry *tree_entry;
    const IndexEntry *index_entry;
    char original_dir[MG_MAX_PATH];
    char temp_dir[MG_MAX_PATH];
    char commit_hash[MG_HASH_HEX_SIZE];
    char *add_args[] = {"file.txt"};
    char *restore_args[] = {"file.txt"};
    char *file_contents;

    make_temp_dir(original_dir, temp_dir);
    assert_result(mg_command_init(0, NULL), MG_OK, "init failed");
    commit_file("one\n", "one", commit_hash);

    assert_result(fs_write_file("file.txt", (const unsigned char *)"staged\n", strlen("staged\n")), MG_OK, "write staged failed");
    assert_result(mg_command_add(1, add_args), MG_OK, "stage modified file failed");
    assert_result(fs_write_file("file.txt", (const unsigned char *)"dirty\n", strlen("dirty\n")), MG_OK, "write dirty failed");

    assert_result(mg_command_restore(1, restore_args), MG_OK, "restore path failed");
    read_text_file("file.txt", &file_contents);
    assert_true(strcmp(file_contents, "one\n") == 0, "restore should write HEAD contents");
    free(file_contents);

    assert_result(repo_open(&repo), MG_OK, "repo_open failed");
    assert_result(commit_read(&repo, commit_hash, &commit), MG_OK, "commit_read failed");
    assert_result(tree_read(&repo, commit.tree_hash, &tree), MG_OK, "tree_read failed");
    assert_result(index_load(&repo, &index), MG_OK, "index_load failed");

    tree_entry = tree_find_entry(&tree, "file.txt");
    index_entry = index_find_const(&index, "file.txt");
    assert_true(tree_entry != NULL, "HEAD tree should contain file");
    assert_true(index_entry != NULL, "index should contain restored file");
    assert_true(strcmp(index_entry->hash, tree_entry->hash) == 0, "restore should reset index to HEAD");

    index_free(&index);
    tree_free(&tree);
    commit_free(&commit);
    cleanup_temp_dir(original_dir, temp_dir);
}

static void test_restore_rejects_path_missing_from_head(void) {
    char original_dir[MG_MAX_PATH];
    char temp_dir[MG_MAX_PATH];
    char commit_hash[MG_HASH_HEX_SIZE];
    char *restore_args[] = {"missing.txt"};

    make_temp_dir(original_dir, temp_dir);
    assert_result(mg_command_init(0, NULL), MG_OK, "init failed");
    commit_file("one\n", "one", commit_hash);

    assert_result(mg_command_restore(1, restore_args), MG_NOT_FOUND, "restore should reject paths absent from HEAD");

    (void)commit_hash;
    cleanup_temp_dir(original_dir, temp_dir);
}

static void test_checkout_path_from_commit(void) {
    Repository repo;
    Commit commit;
    Tree tree;
    Index index;
    const TreeEntry *tree_entry;
    const IndexEntry *index_entry;
    char original_dir[MG_MAX_PATH];
    char temp_dir[MG_MAX_PATH];
    char first_hash[MG_HASH_HEX_SIZE];
    char second_hash[MG_HASH_HEX_SIZE];
    char head_hash[MG_HASH_HEX_SIZE];
    char *checkout_args[] = {first_hash, "--", "file.txt"};
    char *file_contents;

    make_temp_dir(original_dir, temp_dir);
    assert_result(mg_command_init(0, NULL), MG_OK, "init failed");
    commit_file("one\n", "one", first_hash);
    commit_file("two\n", "two", second_hash);

    assert_result(mg_command_checkout(3, checkout_args), MG_OK, "checkout path from commit failed");
    read_text_file("file.txt", &file_contents);
    assert_true(strcmp(file_contents, "one\n") == 0, "checkout path should restore selected commit contents");
    free(file_contents);

    assert_result(repo_open(&repo), MG_OK, "repo_open failed");
    assert_result(repo_current_commit(&repo, head_hash, sizeof(head_hash)), MG_OK, "repo_current_commit failed");
    assert_true(strcmp(head_hash, second_hash) == 0, "path checkout should not move HEAD");

    assert_result(commit_read(&repo, first_hash, &commit), MG_OK, "commit_read failed");
    assert_result(tree_read(&repo, commit.tree_hash, &tree), MG_OK, "tree_read failed");
    assert_result(index_load(&repo, &index), MG_OK, "index_load failed");
    tree_entry = tree_find_entry(&tree, "file.txt");
    index_entry = index_find_const(&index, "file.txt");
    assert_true(tree_entry != NULL && index_entry != NULL, "file should exist in commit tree and index");
    assert_true(strcmp(index_entry->hash, tree_entry->hash) == 0, "index should match checked-out commit path");

    index_free(&index);
    tree_free(&tree);
    commit_free(&commit);
    cleanup_temp_dir(original_dir, temp_dir);
}

static void test_checkout_path_missing_from_commit(void) {
    char original_dir[MG_MAX_PATH];
    char temp_dir[MG_MAX_PATH];
    char commit_hash[MG_HASH_HEX_SIZE];
    char *checkout_args[] = {commit_hash, "--", "missing.txt"};

    make_temp_dir(original_dir, temp_dir);
    assert_result(mg_command_init(0, NULL), MG_OK, "init failed");
    commit_file("one\n", "one", commit_hash);

    assert_result(mg_command_checkout(3, checkout_args), MG_NOT_FOUND, "checkout path should reject missing path");

    cleanup_temp_dir(original_dir, temp_dir);
}

static void test_restore_rejects_external_symlink_target(void) {
    char original_dir[MG_MAX_PATH];
    char temp_dir[MG_MAX_PATH];
    char victim_path[MG_MAX_PATH];
    char commit_hash[MG_HASH_HEX_SIZE];
    char *restore_args[] = {"file.txt"};
    unsigned char *victim_data = NULL;
    size_t victim_size = 0;

    make_temp_dir(original_dir, temp_dir);
    assert_result(mg_command_init(0, NULL), MG_OK, "init failed");
    commit_file("committed\n", "base", commit_hash);
    assert_true(snprintf(victim_path, sizeof(victim_path), "%s_victim", temp_dir) > 0, "victim path failed");
    assert_result(fs_write_file(victim_path, (const unsigned char *)"outside\n", 8), MG_OK, "write victim failed");
    assert_true(unlink("file.txt") == 0, "unlink tracked file failed");
    assert_true(symlink(victim_path, "file.txt") == 0, "create tracked symlink failed");

    assert_result(mg_command_restore(1, restore_args), MG_CONFLICT, "restore should reject symlink target");
    assert_result(fs_read_file(victim_path, &victim_data, &victim_size), MG_OK, "read victim failed");
    assert_true(victim_size == 8 && memcmp(victim_data, "outside\n", 8) == 0, "restore overwrote external target");
    free(victim_data);

    cleanup_temp_dir(original_dir, temp_dir);
    (void)unlink(victim_path);
    (void)commit_hash;
}

static void test_checkout_prevalidates_all_target_blobs(void) {
    Repository repo;
    Commit commit;
    Tree tree;
    char original_dir[MG_MAX_PATH];
    char temp_dir[MG_MAX_PATH];
    char target_hash[MG_HASH_HEX_SIZE];
    char current_hash[MG_HASH_HEX_SIZE];
    char object_dir[MG_MAX_PATH];
    char object_path[MG_MAX_PATH];
    char fanout[3];
    char *add_args[] = {"old.txt", "keep.txt"};
    char *commit_args[] = {"-m", "target"};
    char *rm_args[] = {"old.txt"};
    char *add_keep_args[] = {"keep.txt"};
    char *current_commit_args[] = {"-m", "current"};
    char *checkout_args[] = {target_hash};
    char *contents;
    const TreeEntry *old_entry;

    make_temp_dir(original_dir, temp_dir);
    assert_result(mg_command_init(0, NULL), MG_OK, "init failed");
    assert_result(fs_write_file("old.txt", (const unsigned char *)"old\n", 4), MG_OK, "write old failed");
    assert_result(fs_write_file("keep.txt", (const unsigned char *)"one\n", 4), MG_OK, "write keep failed");
    assert_result(mg_command_add(2, add_args), MG_OK, "add target files failed");
    assert_result(mg_command_commit(2, commit_args), MG_OK, "target commit failed");
    assert_result(repo_open(&repo), MG_OK, "repo_open failed");
    assert_result(repo_current_commit(&repo, target_hash, sizeof(target_hash)), MG_OK, "read target hash failed");

    assert_result(mg_command_rm(1, rm_args), MG_OK, "remove old failed");
    assert_result(fs_write_file("keep.txt", (const unsigned char *)"two\n", 4), MG_OK, "write current keep failed");
    assert_result(mg_command_add(1, add_keep_args), MG_OK, "add current keep failed");
    assert_result(mg_command_commit(2, current_commit_args), MG_OK, "current commit failed");
    assert_result(repo_current_commit(&repo, current_hash, sizeof(current_hash)), MG_OK, "read current hash failed");

    assert_result(commit_read(&repo, target_hash, &commit), MG_OK, "read target commit failed");
    assert_result(tree_read(&repo, commit.tree_hash, &tree), MG_OK, "read target tree failed");
    old_entry = tree_find_entry(&tree, "old.txt");
    assert_true(old_entry != NULL, "target tree should contain old.txt");
    fanout[0] = old_entry->hash[0];
    fanout[1] = old_entry->hash[1];
    fanout[2] = '\0';
    assert_result(fs_join_path(".minigit/objects", fanout, object_dir, sizeof(object_dir)), MG_OK, "object dir failed");
    assert_result(fs_join_path(object_dir, old_entry->hash + 2, object_path, sizeof(object_path)), MG_OK, "object path failed");
    assert_true(unlink(object_path) == 0, "remove target blob failed");
    tree_free(&tree);
    commit_free(&commit);

    assert_result(mg_command_checkout(1, checkout_args), MG_NOT_FOUND, "checkout should fail before mutation");
    read_text_file("keep.txt", &contents);
    assert_true(strcmp(contents, "two\n") == 0, "failed checkout changed an existing file");
    free(contents);
    assert_true(!fs_exists("old.txt"), "failed checkout created a target-only file");
    assert_result(repo_current_commit(&repo, target_hash, sizeof(target_hash)), MG_OK, "read HEAD after failure failed");
    assert_true(strcmp(target_hash, current_hash) == 0, "failed checkout moved HEAD");

    cleanup_temp_dir(original_dir, temp_dir);
}

static void test_checkout_rolls_back_when_index_save_fails(void) {
    Repository repo;
    Index index;
    char original_dir[MG_MAX_PATH];
    char temp_dir[MG_MAX_PATH];
    char target_hash[MG_HASH_HEX_SIZE];
    char current_hash[MG_HASH_HEX_SIZE];
    char *target_add_args[] = {"keep.txt", "nested/target.txt"};
    char *target_commit_args[] = {"-m", "target"};
    char *rm_args[] = {"nested/target.txt"};
    char *current_add_args[] = {"keep.txt"};
    char *current_commit_args[] = {"-m", "current"};
    char *checkout_args[] = {target_hash};
    char *contents;

    make_temp_dir(original_dir, temp_dir);
    assert_result(mg_command_init(0, NULL), MG_OK, "init failed");
    assert_true(mkdir("nested", 0700) == 0, "mkdir nested failed");
    assert_result(fs_write_file("keep.txt", (const unsigned char *)"target\n", 7), MG_OK,
                  "write target keep failed");
    assert_result(fs_write_file("nested/target.txt", (const unsigned char *)"target only\n", 12), MG_OK,
                  "write target-only file failed");
    assert_result(mg_command_add(2, target_add_args), MG_OK, "add target files failed");
    assert_result(mg_command_commit(2, target_commit_args), MG_OK, "target commit failed");
    assert_result(repo_open(&repo), MG_OK, "repo_open failed");
    assert_result(repo_current_commit(&repo, target_hash, sizeof(target_hash)), MG_OK, "target hash failed");

    assert_result(mg_command_rm(1, rm_args), MG_OK, "remove target-only file failed");
    assert_result(fs_write_file("keep.txt", (const unsigned char *)"current\n", 8), MG_OK,
                  "write current keep failed");
    assert_result(mg_command_add(1, current_add_args), MG_OK, "add current file failed");
    assert_result(mg_command_commit(2, current_commit_args), MG_OK, "current commit failed");
    assert_result(repo_current_commit(&repo, current_hash, sizeof(current_hash)), MG_OK, "current hash failed");

    assert_true(chmod(".minigit", 0500) == 0, "make metadata directory read-only failed");
    assert_result(mg_command_checkout(1, checkout_args), MG_IO_ERROR,
                  "checkout should fail when the index cannot be replaced");
    assert_true(chmod(".minigit", 0700) == 0, "restore metadata permissions failed");

    read_text_file("keep.txt", &contents);
    assert_true(strcmp(contents, "current\n") == 0, "failed checkout did not restore current contents");
    free(contents);
    assert_true(!fs_exists("nested/target.txt"), "failed checkout left a target-only file");
    assert_true(!fs_exists("nested"), "failed checkout left an empty target-only directory");
    assert_result(repo_current_commit(&repo, target_hash, sizeof(target_hash)), MG_OK, "HEAD read failed");
    assert_true(strcmp(target_hash, current_hash) == 0, "failed checkout moved HEAD");
    assert_result(index_load(&repo, &index), MG_OK, "index_load failed");
    assert_true(index_find_const(&index, "keep.txt") != NULL, "rollback lost current index entry");
    assert_true(index_find_const(&index, "nested/target.txt") == NULL, "rollback changed the index");
    index_free(&index);

    cleanup_temp_dir(original_dir, temp_dir);
}

static void test_restore_rolls_back_when_index_save_fails(void) {
    Repository repo;
    Index index;
    char original_dir[MG_MAX_PATH];
    char temp_dir[MG_MAX_PATH];
    char commit_hash[MG_HASH_HEX_SIZE];
    char before_hash[MG_HASH_HEX_SIZE];
    char after_hash[MG_HASH_HEX_SIZE];
    char *restore_args[] = {"file.txt"};
    char *contents;

    make_temp_dir(original_dir, temp_dir);
    assert_result(mg_command_init(0, NULL), MG_OK, "init failed");
    commit_file("committed\n", "base", commit_hash);
    assert_result(fs_write_file("file.txt", (const unsigned char *)"staged change\n", 14), MG_OK,
                  "write staged change failed");
    assert_result(mg_command_add(1, restore_args), MG_OK, "stage change failed");
    assert_result(repo_open(&repo), MG_OK, "repo_open failed");
    assert_result(index_load(&repo, &index), MG_OK, "index_load before restore failed");
    assert_true(index_find_const(&index, "file.txt") != NULL, "file should be indexed");
    strcpy(before_hash, index_find_const(&index, "file.txt")->hash);
    index_free(&index);

    assert_true(chmod(".minigit", 0500) == 0, "make metadata directory read-only failed");
    assert_result(mg_command_restore(1, restore_args), MG_IO_ERROR,
                  "restore should fail when the index cannot be replaced");
    assert_true(chmod(".minigit", 0700) == 0, "restore metadata permissions failed");

    read_text_file("file.txt", &contents);
    assert_true(strcmp(contents, "staged change\n") == 0, "failed restore did not restore prior contents");
    free(contents);
    assert_result(index_load(&repo, &index), MG_OK, "index_load after restore failed");
    assert_true(index_find_const(&index, "file.txt") != NULL, "failed restore lost index entry");
    strcpy(after_hash, index_find_const(&index, "file.txt")->hash);
    assert_true(strcmp(before_hash, after_hash) == 0, "failed restore changed the index");
    index_free(&index);

    cleanup_temp_dir(original_dir, temp_dir);
    (void)commit_hash;
}

int main(void) {
    test_detached_checkout_restores_commit();
    test_checkout_refuses_dirty_tracked_file();
    test_checkout_refuses_untracked_target_file();
    test_checkout_allows_clean_file_to_directory_transition();
    test_checkout_allows_clean_directory_to_file_transition();
    test_switch_restores_branch_and_updates_head();
    test_switch_refuses_untracked_target_file();
    test_restore_path_from_head();
    test_restore_rejects_path_missing_from_head();
    test_checkout_path_from_commit();
    test_checkout_path_missing_from_commit();
    test_restore_rejects_external_symlink_target();
    test_checkout_prevalidates_all_target_blobs();
    test_checkout_rolls_back_when_index_save_fails();
    test_restore_rolls_back_when_index_save_fails();
    puts("test_checkout passed");
    return 0;
}
