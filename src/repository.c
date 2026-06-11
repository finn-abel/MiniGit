#define _POSIX_C_SOURCE 200809L

#include "repository.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "fs.h"

#define MG_HEAD_REF_PREFIX "ref: "
#define MG_MAIN_REF "refs/heads/main"

/*
 * repo_set_paths anchors repository paths at the current working directory.
 */
static MGResult repo_set_paths(Repository *repo) {
    if (repo == NULL) {
        return MG_INVALID_ARG;
    }
    if (getcwd(repo->worktree_path, sizeof(repo->worktree_path)) == NULL) {
        return MG_IO_ERROR;
    }
    return fs_join_path(repo->worktree_path, ".minigit", repo->gitdir_path, sizeof(repo->gitdir_path));
}

/*
 * trim_trailing_newline normalizes text file contents read from refs.
 */
static void trim_trailing_newline(char *value) {
    size_t len = strlen(value);
    while (len > 0 && (value[len - 1] == '\n' || value[len - 1] == '\r')) {
        value[len - 1] = '\0';
        len--;
    }
}

/*
 * copy_trimmed_text copies bytes to text, trims newlines, and checks capacity.
 */
static MGResult copy_trimmed_text(const unsigned char *data, size_t size, char *out, size_t out_size) {
    char *buffer;
    size_t trimmed_size;

    if (data == NULL || out == NULL || out_size == 0) {
        return MG_INVALID_ARG;
    }

    buffer = malloc(size + 1);
    if (buffer == NULL) {
        return MG_ERROR;
    }
    memcpy(buffer, data, size);
    buffer[size] = '\0';
    trim_trailing_newline(buffer);

    trimmed_size = strlen(buffer);
    if (trimmed_size >= out_size) {
        free(buffer);
        return MG_INVALID_ARG;
    }

    memcpy(out, buffer, trimmed_size + 1);
    free(buffer);
    return MG_OK;
}

/*
 * repo_path joins a path inside .minigit into out.
 */
static MGResult repo_path(const Repository *repo, const char *relative, char *out, size_t out_size) {
    if (repo == NULL) {
        return MG_INVALID_ARG;
    }
    return fs_join_path(repo->gitdir_path, relative, out, out_size);
}

/*
 * repo_init creates the initial .minigit directory and ref files.
 */
MGResult repo_init(Repository *repo) {
    char objects_path[MG_MAX_PATH];
    char refs_heads_path[MG_MAX_PATH];
    char main_ref_path[MG_MAX_PATH];
    char head_path[MG_MAX_PATH];
    char index_path[MG_MAX_PATH];
    const unsigned char head_contents[] = "ref: refs/heads/main\n";

    if (repo_set_paths(repo) != MG_OK) {
        return MG_IO_ERROR;
    }
    if (fs_exists(repo->gitdir_path)) {
        return MG_REPO_ERROR;
    }

    if (repo_path(repo, "objects", objects_path, sizeof(objects_path)) != MG_OK ||
        repo_path(repo, "refs/heads", refs_heads_path, sizeof(refs_heads_path)) != MG_OK ||
        repo_path(repo, "refs/heads/main", main_ref_path, sizeof(main_ref_path)) != MG_OK ||
        repo_path(repo, "HEAD", head_path, sizeof(head_path)) != MG_OK ||
        repo_path(repo, "index", index_path, sizeof(index_path)) != MG_OK) {
        return MG_INVALID_ARG;
    }

    if (fs_mkdir_p(objects_path) != MG_OK || fs_mkdir_p(refs_heads_path) != MG_OK) {
        return MG_IO_ERROR;
    }
    if (fs_write_file(head_path, head_contents, sizeof(head_contents) - 1) != MG_OK ||
        fs_write_file(main_ref_path, (const unsigned char *)"", 0) != MG_OK ||
        fs_write_file(index_path, (const unsigned char *)"", 0) != MG_OK) {
        return MG_IO_ERROR;
    }

    return MG_OK;
}

/*
 * repo_open verifies that the current directory contains a MiniGit repo.
 */
MGResult repo_open(Repository *repo) {
    char head_path[MG_MAX_PATH];

    if (repo_set_paths(repo) != MG_OK) {
        return MG_IO_ERROR;
    }
    if (!fs_is_dir(repo->gitdir_path)) {
        return MG_REPO_ERROR;
    }
    if (repo_path(repo, "HEAD", head_path, sizeof(head_path)) != MG_OK || !fs_is_file(head_path)) {
        return MG_REPO_ERROR;
    }

    return MG_OK;
}

/*
 * repo_read_head reads and trims the HEAD file.
 */
MGResult repo_read_head(const Repository *repo, char *out, size_t out_size) {
    char head_path[MG_MAX_PATH];
    unsigned char *data;
    size_t size;
    MGResult result;

    if (out == NULL || out_size == 0) {
        return MG_INVALID_ARG;
    }
    if (repo_path(repo, "HEAD", head_path, sizeof(head_path)) != MG_OK) {
        return MG_INVALID_ARG;
    }
    if (fs_read_file(head_path, &data, &size) != MG_OK) {
        return MG_IO_ERROR;
    }
    result = copy_trimmed_text(data, size, out, out_size);
    free(data);
    return result;
}

/*
 * repo_write_head replaces HEAD with caller-provided contents.
 */
MGResult repo_write_head(const Repository *repo, const char *contents) {
    char head_path[MG_MAX_PATH];

    if (contents == NULL) {
        return MG_INVALID_ARG;
    }
    if (repo_path(repo, "HEAD", head_path, sizeof(head_path)) != MG_OK) {
        return MG_INVALID_ARG;
    }
    return fs_write_file(head_path, (const unsigned char *)contents, strlen(contents));
}

/*
 * repo_head_is_detached distinguishes raw commit HEAD from branch refs.
 */
int repo_head_is_detached(const char *head_contents) {
    if (head_contents == NULL) {
        return 0;
    }
    return strncmp(head_contents, MG_HEAD_REF_PREFIX, strlen(MG_HEAD_REF_PREFIX)) != 0;
}

/*
 * repo_current_branch extracts the current branch name from symbolic HEAD.
 */
MGResult repo_current_branch(const Repository *repo, char *out, size_t out_size) {
    char head[MG_MAX_PATH];
    const char *ref_path;
    const char *branch_name;

    if (out == NULL || out_size == 0) {
        return MG_INVALID_ARG;
    }
    if (repo_read_head(repo, head, sizeof(head)) != MG_OK) {
        return MG_REPO_ERROR;
    }
    if (repo_head_is_detached(head)) {
        return MG_NOT_FOUND;
    }

    ref_path = head + strlen(MG_HEAD_REF_PREFIX);
    branch_name = strrchr(ref_path, '/');
    branch_name = branch_name == NULL ? ref_path : branch_name + 1;
    if (strlen(branch_name) >= out_size) {
        return MG_INVALID_ARG;
    }

    strcpy(out, branch_name);
    return MG_OK;
}

/*
 * repo_current_commit resolves HEAD to the current commit hash if one exists.
 */
MGResult repo_current_commit(const Repository *repo, char *out, size_t out_size) {
    char head[MG_MAX_PATH];
    char ref_file_path[MG_MAX_PATH];
    unsigned char *data;
    size_t size;
    MGResult result;

    if (out == NULL || out_size == 0) {
        return MG_INVALID_ARG;
    }
    if (repo_read_head(repo, head, sizeof(head)) != MG_OK) {
        return MG_REPO_ERROR;
    }

    if (repo_head_is_detached(head)) {
        if (strlen(head) >= out_size) {
            return MG_INVALID_ARG;
        }
        strcpy(out, head);
        return MG_OK;
    }

    if (repo_path(repo, head + strlen(MG_HEAD_REF_PREFIX), ref_file_path, sizeof(ref_file_path)) != MG_OK) {
        return MG_INVALID_ARG;
    }
    if (fs_read_file(ref_file_path, &data, &size) != MG_OK) {
        return MG_IO_ERROR;
    }
    result = copy_trimmed_text(data, size, out, out_size);
    free(data);
    return result;
}

/*
 * repo_update_current_ref writes a new commit hash to HEAD or the active ref.
 */
MGResult repo_update_current_ref(const Repository *repo, const char *commit_hash) {
    char head[MG_MAX_PATH];
    char target_path[MG_MAX_PATH];
    char contents[MG_HASH_HEX_SIZE + 1];
    int written;

    if (commit_hash == NULL) {
        return MG_INVALID_ARG;
    }
    written = snprintf(contents, sizeof(contents), "%s\n", commit_hash);
    if (written < 0 || (size_t)written >= sizeof(contents)) {
        return MG_INVALID_ARG;
    }

    if (repo_read_head(repo, head, sizeof(head)) != MG_OK) {
        return MG_REPO_ERROR;
    }

    if (repo_head_is_detached(head)) {
        return repo_write_head(repo, contents);
    }

    if (repo_path(repo, head + strlen(MG_HEAD_REF_PREFIX), target_path, sizeof(target_path)) != MG_OK) {
        return MG_INVALID_ARG;
    }
    return fs_write_file(target_path, (const unsigned char *)contents, strlen(contents));
}

/*
 * repo_head_display_name chooses the label used in commit output.
 */
MGResult repo_head_display_name(const Repository *repo, char *out, size_t out_size) {
    MGResult result;

    if (out == NULL || out_size == 0) {
        return MG_INVALID_ARG;
    }

    result = repo_current_branch(repo, out, out_size);
    if (result == MG_OK) {
        return MG_OK;
    }
    if (result == MG_NOT_FOUND) {
        if (strlen("detached") >= out_size) {
            return MG_INVALID_ARG;
        }
        strcpy(out, "detached");
        return MG_OK;
    }

    return result;
}
