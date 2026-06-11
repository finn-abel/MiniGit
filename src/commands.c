#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "commands.h"
#include "fs.h"
#include "index.h"
#include "object.h"
#include "repository.h"

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
    MGResult result = require_repo(&repo);
    if (result != MG_OK) {
        return result;
    }
    if (argc < 1) {
        puts("usage: minigit add <path>...");
        return MG_INVALID_ARG;
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
    MGResult result = require_repo(&repo);
    if (result != MG_OK) {
        return result;
    }
    ignore_args(argc, argv);
    puts("rm not implemented yet");
    return MG_OK;
}

/*
 * mg_command_status handles `minigit status`.
 */
MGResult mg_command_status(int argc, char **argv) {
    Repository repo;
    MGResult result = require_repo(&repo);
    if (result != MG_OK) {
        return result;
    }
    ignore_args(argc, argv);
    puts("status not implemented yet");
    return MG_OK;
}

/*
 * mg_command_commit handles `minigit commit -m <message>`.
 */
MGResult mg_command_commit(int argc, char **argv) {
    Repository repo;
    MGResult result = require_repo(&repo);
    if (result != MG_OK) {
        return result;
    }
    ignore_args(argc, argv);
    puts("commit not implemented yet");
    return MG_OK;
}

/*
 * mg_command_log handles `minigit log`.
 */
MGResult mg_command_log(int argc, char **argv) {
    Repository repo;
    MGResult result = require_repo(&repo);
    if (result != MG_OK) {
        return result;
    }
    ignore_args(argc, argv);
    puts("log not implemented yet");
    return MG_OK;
}

/*
 * mg_command_branch handles `minigit branch [name]`.
 */
MGResult mg_command_branch(int argc, char **argv) {
    Repository repo;
    MGResult result = require_repo(&repo);
    if (result != MG_OK) {
        return result;
    }
    ignore_args(argc, argv);
    puts("branch not implemented yet");
    return MG_OK;
}

/*
 * mg_command_switch handles `minigit switch <branch>`.
 */
MGResult mg_command_switch(int argc, char **argv) {
    Repository repo;
    MGResult result = require_repo(&repo);
    if (result != MG_OK) {
        return result;
    }
    ignore_args(argc, argv);
    puts("switch not implemented yet");
    return MG_OK;
}

/*
 * mg_command_checkout handles `minigit checkout <commit_hash>`.
 */
MGResult mg_command_checkout(int argc, char **argv) {
    Repository repo;
    MGResult result = require_repo(&repo);
    if (result != MG_OK) {
        return result;
    }
    ignore_args(argc, argv);
    puts("checkout not implemented yet");
    return MG_OK;
}
