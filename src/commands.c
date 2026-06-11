#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "commands.h"
#include "checkout.h"
#include "commit.h"
#include "diff.h"
#include "fs.h"
#include "hash.h"
#include "index.h"
#include "object.h"
#include "repository.h"
#include "reset.h"
#include "status.h"
#include "tree.h"

/*
 * AddContext carries state needed while recursively staging a directory.
 */
typedef struct {
    const Repository *repo;
    Index *index;
    const char *base_relative;
} AddContext;

/*
 * ignore_args keeps temporary stubs warning-free until real validation exists.
 */
static void ignore_args(int argc, char **argv) {
    (void)argc;
    (void)argv;
}

/*
 * require_repo opens the current repository for commands that need one.
 */
static MGResult require_repo(Repository *repo) {
    MGResult result = repo_open(repo);
    if (result != MG_OK) {
        puts("not a MiniGit repository");
    }
    return result;
}

/*
 * stage_file writes a blob object and records it in the index.
 */
static MGResult stage_file(const Repository *repo, Index *index, const char *relative_path) {
    char full_path[MG_MAX_PATH];
    unsigned char *data = NULL;
    size_t size = 0;
    char hash[MG_HASH_HEX_SIZE];
    struct stat st;
    MGResult result;

    if (fs_join_path(repo->worktree_path, relative_path, full_path, sizeof(full_path)) != MG_OK) {
        return MG_INVALID_ARG;
    }
    if (lstat(full_path, &st) != 0 || !S_ISREG(st.st_mode)) {
        return MG_INVALID_ARG;
    }

    result = fs_read_file(full_path, &data, &size);
    if (result != MG_OK) {
        return result;
    }

    result = object_write(repo, "blob", data, size, hash);
    free(data);
    if (result != MG_OK) {
        return result;
    }

    result = index_add_or_update(index, relative_path, hash, (size_t)st.st_size, st.st_mtime);
    if (result == MG_OK) {
        printf("added %s\n", relative_path);
    }
    return result;
}

/*
 * stage_walked_file stages one file found by fs_walk.
 */
static MGResult stage_walked_file(const char *walk_relative_path, void *ctx) {
    AddContext *add_ctx = ctx;
    char relative_path[MG_MAX_PATH];

    if (add_ctx->base_relative[0] == '\0') {
        if (snprintf(relative_path, sizeof(relative_path), "%s", walk_relative_path) >= (int)sizeof(relative_path)) {
            return MG_INVALID_ARG;
        }
    } else if (fs_join_path(add_ctx->base_relative, walk_relative_path, relative_path, sizeof(relative_path)) != MG_OK) {
        return MG_INVALID_ARG;
    }

    return stage_file(add_ctx->repo, add_ctx->index, relative_path);
}

/*
 * stage_path stages either one regular file or every regular file under a directory.
 */
static MGResult stage_path(const Repository *repo, Index *index, const char *input_path) {
    char relative_path[MG_MAX_PATH];
    char full_path[MG_MAX_PATH];
    struct stat st;

    if (fs_repo_relative_path(repo->worktree_path, input_path, relative_path, sizeof(relative_path)) != MG_OK) {
        return MG_INVALID_ARG;
    }
    if (relative_path[0] == '\0') {
        if (snprintf(full_path, sizeof(full_path), "%s", repo->worktree_path) >= (int)sizeof(full_path)) {
            return MG_INVALID_ARG;
        }
    } else if (fs_join_path(repo->worktree_path, relative_path, full_path, sizeof(full_path)) != MG_OK) {
        return MG_INVALID_ARG;
    }

    if (lstat(full_path, &st) != 0) {
        return MG_NOT_FOUND;
    }
    if (S_ISREG(st.st_mode)) {
        if (relative_path[0] == '\0') {
            return MG_INVALID_ARG;
        }
        return stage_file(repo, index, relative_path);
    }
    if (S_ISDIR(st.st_mode)) {
        AddContext ctx = {repo, index, relative_path};
        return fs_walk(full_path, stage_walked_file, &ctx);
    }

    return MG_INVALID_ARG;
}

/*
 * remove_tracked_path removes one tracked file from the index and working tree.
 */
static MGResult remove_tracked_path(const Repository *repo, Index *index, const char *input_path) {
    char relative_path[MG_MAX_PATH];
    char full_path[MG_MAX_PATH];
    const IndexEntry *entry;

    if (fs_repo_relative_path(repo->worktree_path, input_path, relative_path, sizeof(relative_path)) != MG_OK ||
        relative_path[0] == '\0') {
        return MG_INVALID_ARG;
    }

    entry = index_find_const(index, relative_path);
    if (entry == NULL) {
        printf("path not tracked: %s\n", relative_path);
        return MG_NOT_FOUND;
    }

    if (fs_join_path(repo->worktree_path, relative_path, full_path, sizeof(full_path)) != MG_OK) {
        return MG_INVALID_ARG;
    }
    if (fs_exists(full_path)) {
        if (!fs_is_file(full_path)) {
            return MG_INVALID_ARG;
        }
        if (fs_remove_file(full_path) != MG_OK) {
            return MG_IO_ERROR;
        }
    }

    if (index_remove(index, relative_path) != MG_OK) {
        return MG_ERROR;
    }

    printf("removed %s\n", relative_path);
    return MG_OK;
}

/*
 * parse_commit_message validates `minigit commit -m <message>`.
 */
static MGResult parse_commit_message(int argc, char **argv, const char **out_message) {
    if (out_message == NULL) {
        return MG_INVALID_ARG;
    }
    if (argc != 2 || argv == NULL || strcmp(argv[0], "-m") != 0 || argv[1] == NULL || argv[1][0] == '\0') {
        puts("usage: minigit commit -m <message>");
        return MG_INVALID_ARG;
    }

    *out_message = argv[1];
    return MG_OK;
}

/*
 * print_invalid_branch_name gives users the v1 branch-name rules directly.
 */
static void print_invalid_branch_name(const char *name) {
    if (name == NULL) {
        puts("invalid branch name");
        return;
    }
    printf("invalid branch name: %s\n", name);
    puts("branch names cannot be empty or contain '/', '..', spaces, or control characters");
}

/*
 * checkout_commit_arg_is_valid accepts full SHA-256 ids or v1 short prefixes.
 */
static int checkout_commit_arg_is_valid(const char *value) {
    size_t len;

    if (value == NULL) {
        return 0;
    }
    len = strlen(value);
    if (len < 7 || len >= MG_HASH_HEX_SIZE) {
        return 0;
    }
    for (size_t i = 0; i < len; i++) {
        char ch = value[i];
        if (!((ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f') || (ch >= 'A' && ch <= 'F'))) {
            return 0;
        }
    }
    return 1;
}

/*
 * tree_matches_parent reports whether the staged tree equals the parent tree.
 */
static MGResult tree_matches_parent(const Repository *repo, const char *parent_hash, const char *tree_hash, int *out_matches) {
    Commit parent_commit;
    MGResult result;

    if (repo == NULL || parent_hash == NULL || tree_hash == NULL || out_matches == NULL) {
        return MG_INVALID_ARG;
    }

    *out_matches = 0;
    if (parent_hash[0] == '\0') {
        return MG_OK;
    }

    result = commit_read(repo, parent_hash, &parent_commit);
    if (result != MG_OK) {
        return result;
    }

    *out_matches = strcmp(parent_commit.tree_hash, tree_hash) == 0;
    commit_free(&parent_commit);
    return MG_OK;
}

/*
 * print_commit_summary prints the short hash line after creating a commit.
 */
static MGResult print_commit_summary(const Repository *repo, const char *commit_hash, const char *message) {
    char head_name[MG_MAX_BRANCH];
    MGResult result = repo_head_display_name(repo, head_name, sizeof(head_name));

    if (result != MG_OK) {
        return result;
    }
    printf("[%s %.7s] %s\n", head_name, commit_hash, message);
    return MG_OK;
}

/*
 * print_commit_log_entry prints one commit in log format.
 */
static MGResult print_commit_log_entry(const char *hash, const Commit *commit) {
    char date[128];
    MGResult result;

    if (hash == NULL || commit == NULL) {
        return MG_INVALID_ARG;
    }

    result = commit_format_timestamp(commit->timestamp, date, sizeof(date));
    if (result != MG_OK) {
        return result;
    }

    printf("commit %s\n", hash);
    printf("Date: %s\n\n", date);
    printf("    %s\n\n", commit->message);
    return MG_OK;
}

/*
 * print_branch_name prints one branch list entry.
 */
static MGResult print_branch_name(const char *branch_name, int is_current, void *ctx) {
    (void)ctx;
    printf("%c %s\n", is_current ? '*' : ' ', branch_name);
    return MG_OK;
}

/*
 * mg_command_init handles `minigit init`.
 */
MGResult mg_command_init(int argc, char **argv) {
    Repository repo;
    MGResult result;

    if (argc != 0) {
        ignore_args(argc, argv);
        puts("usage: minigit init");
        return MG_INVALID_ARG;
    }

    result = repo_init(&repo);
    if (result == MG_REPO_ERROR) {
        puts("MiniGit repository already exists");
        return result;
    }
    if (result != MG_OK) {
        puts("failed to initialize MiniGit repository");
        return result;
    }

    puts("Initialized empty MiniGit repository in .minigit");
    return MG_OK;
}

/*
 * mg_command_add handles `minigit add <path>...`.
 */
MGResult mg_command_add(int argc, char **argv) {
    Repository repo;
    Index index;
    MGResult result;

    if (argc < 1) {
        puts("usage: minigit add <path>...");
        return MG_INVALID_ARG;
    }

    result = require_repo(&repo);
    if (result != MG_OK) {
        return result;
    }

    result = index_load(&repo, &index);
    if (result != MG_OK) {
        puts("failed to load index");
        return result;
    }

    for (int i = 0; i < argc; i++) {
        result = stage_path(&repo, &index, argv[i]);
        if (result != MG_OK) {
            index_free(&index);
            puts("failed to add path");
            return result;
        }
    }

    result = index_save(&repo, &index);
    index_free(&index);
    if (result != MG_OK) {
        puts("failed to save index");
        return result;
    }

    return MG_OK;
}

/*
 * mg_command_rm handles `minigit rm <path>...`.
 */
MGResult mg_command_rm(int argc, char **argv) {
    Repository repo;
    Index index;
    MGResult result;

    if (argc < 1) {
        puts("usage: minigit rm <path>...");
        return MG_INVALID_ARG;
    }

    result = require_repo(&repo);
    if (result != MG_OK) {
        return result;
    }

    result = index_load(&repo, &index);
    if (result != MG_OK) {
        puts("failed to load index");
        return result;
    }

    for (int i = 0; i < argc; i++) {
        result = remove_tracked_path(&repo, &index, argv[i]);
        if (result != MG_OK) {
            index_free(&index);
            return result;
        }
    }

    result = index_save(&repo, &index);
    index_free(&index);
    if (result != MG_OK) {
        puts("failed to save index");
        return result;
    }

    return MG_OK;
}

/*
 * mg_command_status handles `minigit status`.
 */
MGResult mg_command_status(int argc, char **argv) {
    Repository repo;
    MGResult result;

    if (argc != 0) {
        ignore_args(argc, argv);
        puts("usage: minigit status");
        return MG_INVALID_ARG;
    }

    result = require_repo(&repo);
    if (result != MG_OK) {
        return result;
    }

    result = status_print(&repo);
    if (result != MG_OK) {
        puts("failed to get status");
    }
    return result;
}

/*
 * mg_command_diff handles `minigit diff [--staged|--cached|HEAD]`.
 */
MGResult mg_command_diff(int argc, char **argv) {
    Repository repo;
    DiffMode mode = DIFF_WORKTREE;
    MGResult result;

    if (argc == 1 && (strcmp(argv[0], "--staged") == 0 || strcmp(argv[0], "--cached") == 0)) {
        mode = DIFF_STAGED;
    } else if (argc == 1 && strcmp(argv[0], "HEAD") == 0) {
        mode = DIFF_HEAD;
    } else if (argc != 0) {
        ignore_args(argc, argv);
        puts("usage: minigit diff [--staged|--cached|HEAD]");
        return MG_INVALID_ARG;
    }

    result = require_repo(&repo);
    if (result != MG_OK) {
        return result;
    }

    result = diff_print(&repo, mode);
    if (result != MG_OK) {
        puts("failed to diff");
    }
    return result;
}

/*
 * mg_command_commit handles `minigit commit -m <message>`.
 */
MGResult mg_command_commit(int argc, char **argv) {
    Repository repo;
    Index index;
    Tree tree;
    const char *message;
    char tree_hash[MG_HASH_HEX_SIZE];
    char parent_hash[MG_HASH_HEX_SIZE];
    char commit_hash[MG_HASH_HEX_SIZE];
    int unchanged;
    MGResult result;

    result = parse_commit_message(argc, argv, &message);
    if (result != MG_OK) {
        return result;
    }

    result = require_repo(&repo);
    if (result != MG_OK) {
        return result;
    }

    result = index_load(&repo, &index);
    if (result != MG_OK) {
        puts("failed to load index");
        return result;
    }

    result = tree_from_index(&index, &tree);
    index_free(&index);
    if (result != MG_OK) {
        puts("failed to build tree");
        return result;
    }

    result = tree_write(&repo, &tree, tree_hash);
    tree_free(&tree);
    if (result != MG_OK) {
        puts("failed to write tree");
        return result;
    }

    result = repo_current_commit(&repo, parent_hash, sizeof(parent_hash));
    if (result != MG_OK) {
        puts("failed to resolve current commit");
        return result;
    }

    result = tree_matches_parent(&repo, parent_hash, tree_hash, &unchanged);
    if (result != MG_OK) {
        puts("failed to read parent commit");
        return result;
    }
    if (unchanged) {
        puts("nothing to commit");
        return MG_OK;
    }

    result = commit_create(&repo, tree_hash, parent_hash, message, commit_hash);
    if (result != MG_OK) {
        puts("failed to create commit");
        return result;
    }

    result = repo_update_current_ref(&repo, commit_hash);
    if (result != MG_OK) {
        puts("failed to update HEAD");
        return result;
    }

    return print_commit_summary(&repo, commit_hash, message);
}

/*
 * mg_command_log handles `minigit log`.
 */
MGResult mg_command_log(int argc, char **argv) {
    Repository repo;
    char current_hash[MG_HASH_HEX_SIZE];
    MGResult result;

    if (argc != 0) {
        ignore_args(argc, argv);
        puts("usage: minigit log");
        return MG_INVALID_ARG;
    }

    result = require_repo(&repo);
    if (result != MG_OK) {
        return result;
    }

    result = repo_current_commit(&repo, current_hash, sizeof(current_hash));
    if (result != MG_OK) {
        puts("failed to resolve current commit");
        return result;
    }
    if (current_hash[0] == '\0') {
        puts("no commits yet");
        return MG_OK;
    }

    while (current_hash[0] != '\0') {
        Commit commit;
        char parent_hash[MG_HASH_HEX_SIZE];

        result = commit_read(&repo, current_hash, &commit);
        if (result != MG_OK) {
            puts("failed to read commit");
            return result;
        }

        result = print_commit_log_entry(current_hash, &commit);
        if (result != MG_OK) {
            commit_free(&commit);
            return result;
        }

        strcpy(parent_hash, commit.parent_hash);
        commit_free(&commit);
        strcpy(current_hash, parent_hash);
    }

    return MG_OK;
}

/*
 * mg_command_branch handles `minigit branch [name]` and `minigit branch -d <name>`.
 */
MGResult mg_command_branch(int argc, char **argv) {
    Repository repo;
    MGResult result;

    if (argc == 0) {
        result = require_repo(&repo);
        if (result != MG_OK) {
            return result;
        }
        return repo_list_branches(&repo, print_branch_name, NULL);
    }
    if (argc == 2 && strcmp(argv[0], "-d") == 0) {
        if (!repo_branch_name_is_valid(argv[1])) {
            print_invalid_branch_name(argv[1]);
            return MG_INVALID_ARG;
        }

        result = require_repo(&repo);
        if (result != MG_OK) {
            return result;
        }

        result = repo_delete_branch(&repo, argv[1]);
        if (result == MG_INVALID_ARG) {
            print_invalid_branch_name(argv[1]);
        } else if (result == MG_NOT_FOUND) {
            puts("unknown branch");
        } else if (result == MG_CONFLICT) {
            puts("cannot delete current branch");
        } else if (result != MG_OK) {
            puts("failed to delete branch");
        } else {
            printf("deleted branch %s\n", argv[1]);
        }
        return result;
    }
    if (argc != 1) {
        puts("usage: minigit branch [-d <name>|<name>]");
        return MG_INVALID_ARG;
    }
    if (!repo_branch_name_is_valid(argv[0])) {
        print_invalid_branch_name(argv[0]);
        return MG_INVALID_ARG;
    }

    result = require_repo(&repo);
    if (result != MG_OK) {
        return result;
    }

    result = repo_create_branch(&repo, argv[0]);
    if (result == MG_INVALID_ARG) {
        print_invalid_branch_name(argv[0]);
    } else if (result == MG_REPO_ERROR) {
        puts("cannot create branch before first commit");
    } else if (result == MG_CONFLICT) {
        puts("branch already exists");
    } else if (result != MG_OK) {
        puts("failed to create branch");
    }
    return result;
}

/*
 * mg_command_switch handles `minigit switch <branch>`.
 */
MGResult mg_command_switch(int argc, char **argv) {
    Repository repo;
    char branch_commit[MG_HASH_HEX_SIZE];
    MGResult result;

    if (argc != 1) {
        ignore_args(argc, argv);
        puts("usage: minigit switch <branch>");
        return MG_INVALID_ARG;
    }
    if (!repo_branch_name_is_valid(argv[0])) {
        print_invalid_branch_name(argv[0]);
        return MG_INVALID_ARG;
    }

    result = require_repo(&repo);
    if (result != MG_OK) {
        return result;
    }

    result = repo_read_branch_commit(&repo, argv[0], branch_commit, sizeof(branch_commit));
    if (result == MG_INVALID_ARG || result == MG_NOT_FOUND) {
        puts("unknown branch");
        return result;
    }
    if (result != MG_OK) {
        puts("failed to read branch");
        return result;
    }
    if (branch_commit[0] == '\0') {
        puts("branch has no commits");
        return MG_REPO_ERROR;
    }

    result = checkout_commit(&repo, branch_commit, 0);
    if (result == MG_CONFLICT) {
        puts("checkout would overwrite local changes");
        return result;
    }
    if (result != MG_OK) {
        puts("failed to switch branch");
        return result;
    }

    result = repo_write_head_to_branch(&repo, argv[0]);
    if (result != MG_OK) {
        puts("failed to update HEAD");
        return result;
    }

    printf("switched to branch %s\n", argv[0]);
    return MG_OK;
}

/*
 * mg_command_checkout handles `minigit checkout <commit_hash>`.
 */
MGResult mg_command_checkout(int argc, char **argv) {
    Repository repo;
    Object object;
    char commit_hash[MG_HASH_HEX_SIZE];
    MGResult result;

    if (argc != 1) {
        ignore_args(argc, argv);
        puts("usage: minigit checkout <commit>");
        return MG_INVALID_ARG;
    }
    if (!checkout_commit_arg_is_valid(argv[0])) {
        puts("unknown commit");
        return MG_INVALID_ARG;
    }

    result = require_repo(&repo);
    if (result != MG_OK) {
        return result;
    }

    result = object_resolve_prefix(&repo, argv[0], commit_hash);
    if (result == MG_CONFLICT) {
        puts("ambiguous object prefix");
        return result;
    }
    if (result == MG_INVALID_ARG || result == MG_NOT_FOUND) {
        puts("unknown commit");
        return result;
    }
    if (result != MG_OK) {
        puts("failed to resolve commit");
        return result;
    }

    result = object_read(&repo, commit_hash, &object);
    if (result != MG_OK) {
        puts("unknown commit");
        return result;
    }
    if (strcmp(object.type, "commit") != 0) {
        object_free(&object);
        puts("unknown commit");
        return MG_NOT_FOUND;
    }
    object_free(&object);

    result = checkout_commit(&repo, commit_hash, 1);
    if (result == MG_CONFLICT) {
        puts("checkout would overwrite local changes");
    } else if (result == MG_NOT_FOUND || result == MG_PARSE_ERROR) {
        puts("unknown commit");
    } else if (result != MG_OK) {
        puts("failed to checkout commit");
    } else {
        printf("checked out %.7s in detached HEAD state\n", commit_hash);
    }

    return result;
}

/*
 * mg_command_restore handles `minigit restore <path>`.
 */
MGResult mg_command_restore(int argc, char **argv) {
    Repository repo;
    char relative_path[MG_MAX_PATH];
    MGResult result;

    if (argc != 1) {
        ignore_args(argc, argv);
        puts("usage: minigit restore <path>");
        return MG_INVALID_ARG;
    }

    result = require_repo(&repo);
    if (result != MG_OK) {
        return result;
    }

    if (fs_repo_relative_path(repo.worktree_path, argv[0], relative_path, sizeof(relative_path)) != MG_OK ||
        relative_path[0] == '\0') {
        puts("invalid path");
        return MG_INVALID_ARG;
    }

    result = checkout_restore_path(&repo, relative_path);
    if (result == MG_REPO_ERROR) {
        puts("no commits yet");
    } else if (result == MG_NOT_FOUND) {
        printf("path not tracked in HEAD: %s\n", relative_path);
    } else if (result != MG_OK) {
        puts("failed to restore path");
    } else {
        printf("restored %s\n", relative_path);
    }

    return result;
}

/*
 * mg_command_reset handles `minigit reset <path>`.
 */
MGResult mg_command_reset(int argc, char **argv) {
    Repository repo;
    char relative_path[MG_MAX_PATH];
    MGResult result;

    if (argc != 1) {
        ignore_args(argc, argv);
        puts("usage: minigit reset <path>");
        return MG_INVALID_ARG;
    }

    result = require_repo(&repo);
    if (result != MG_OK) {
        return result;
    }

    if (fs_repo_relative_path(repo.worktree_path, argv[0], relative_path, sizeof(relative_path)) != MG_OK ||
        relative_path[0] == '\0') {
        puts("invalid path");
        return MG_INVALID_ARG;
    }

    result = reset_path(&repo, relative_path);
    if (result == MG_NOT_FOUND) {
        printf("path not staged or tracked: %s\n", relative_path);
    } else if (result != MG_OK) {
        puts("failed to reset path");
    } else {
        printf("unstaged %s\n", relative_path);
    }

    return result;
}
