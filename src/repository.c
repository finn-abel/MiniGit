#define _POSIX_C_SOURCE 200809L

#include "repository.h"

#include <ctype.h>
#include <dirent.h>
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

/*
 * compare_names sorts branch names for stable listing.
 * Branch output should not depend on filesystem directory order.
 */
static int compare_names(const void *left, const void *right) {
    const char *const *a = left;
    const char *const *b = right;
    return strcmp(*a, *b);
}

/*
 * copy_name owns a branch name while listing refs.
 * dirent names are reused by readdir, so the list stores private copies.
 */
static char *copy_name(const char *name) {
    size_t len = strlen(name) + 1;
    char *copy = malloc(len);
    if (copy == NULL) {
        return NULL;
    }
    memcpy(copy, name, len);
    return copy;
}

/*
 * repo_branch_name_is_valid enforces the v1 branch-name subset.
 * Keeping names flat avoids path traversal and nested ref directories.
 */
int repo_branch_name_is_valid(const char *name) {
    if (name == NULL || name[0] == '\0' || strlen(name) >= MG_MAX_BRANCH) {
        return 0;
    }
    if (strstr(name, "..") != NULL) {
        return 0;
    }

    /*
     * Spaces and control bytes make command output and ref files awkward; v1
     * rejects them instead of adding quoting rules.
     */
    for (size_t i = 0; name[i] != '\0'; i++) {
        unsigned char ch = (unsigned char)name[i];
        if (ch == '/' || isspace(ch) || iscntrl(ch)) {
            return 0;
        }
    }

    return 1;
}

/*
 * repo_create_branch writes refs/heads/<name> at the current commit.
 */
MGResult repo_create_branch(const Repository *repo, const char *name) {
    char current_commit[MG_HASH_HEX_SIZE];
    char refs_heads_path[MG_MAX_PATH];
    char branch_path[MG_MAX_PATH];
    char contents[MG_HASH_HEX_SIZE + 1];
    int written;
    MGResult result;

    if (repo == NULL || !repo_branch_name_is_valid(name)) {
        return MG_INVALID_ARG;
    }

    /*
     * A branch is just a named commit pointer, so creating one before the first
     * commit would produce an unusable empty ref.
     */
    result = repo_current_commit(repo, current_commit, sizeof(current_commit));
    if (result != MG_OK) {
        return result;
    }
    if (current_commit[0] == '\0') {
        return MG_REPO_ERROR;
    }

    if (repo_path(repo, "refs/heads", refs_heads_path, sizeof(refs_heads_path)) != MG_OK ||
        fs_join_path(refs_heads_path, name, branch_path, sizeof(branch_path)) != MG_OK) {
        return MG_INVALID_ARG;
    }
    /* Ref creation is intentionally non-overwriting. */
    if (fs_exists(branch_path)) {
        return MG_CONFLICT;
    }

    written = snprintf(contents, sizeof(contents), "%s\n", current_commit);
    if (written < 0 || (size_t)written >= sizeof(contents)) {
        return MG_INVALID_ARG;
    }

    return fs_write_file(branch_path, (const unsigned char *)contents, strlen(contents));
}

/*
 * repo_delete_branch removes a flat local branch ref.
 */
MGResult repo_delete_branch(const Repository *repo, const char *name) {
    char current_branch[MG_MAX_BRANCH];
    char refs_heads_path[MG_MAX_PATH];
    char branch_path[MG_MAX_PATH];
    int exists;
    MGResult result;

    if (repo == NULL || !repo_branch_name_is_valid(name)) {
        return MG_INVALID_ARG;
    }

    result = repo_branch_exists(repo, name, &exists);
    if (result != MG_OK) {
        return result;
    }
    if (!exists) {
        return MG_NOT_FOUND;
    }

    result = repo_current_branch(repo, current_branch, sizeof(current_branch));
    if (result == MG_OK && strcmp(current_branch, name) == 0) {
        return MG_CONFLICT;
    }
    if (result != MG_OK && result != MG_NOT_FOUND) {
        return result;
    }

    if (repo_path(repo, "refs/heads", refs_heads_path, sizeof(refs_heads_path)) != MG_OK ||
        fs_join_path(refs_heads_path, name, branch_path, sizeof(branch_path)) != MG_OK) {
        return MG_INVALID_ARG;
    }
    return fs_remove_file(branch_path);
}

/*
 * repo_list_branches visits flat local branches in sorted order.
 */
MGResult repo_list_branches(const Repository *repo, RepoBranchCallback callback, void *ctx) {
    char refs_heads_path[MG_MAX_PATH];
    char current_branch[MG_MAX_BRANCH];
    DIR *dir;
    struct dirent *entry;
    char **names = NULL;
    size_t count = 0;
    size_t capacity = 0;
    MGResult result = MG_OK;

    if (repo == NULL || callback == NULL) {
        return MG_INVALID_ARG;
    }
    if (repo_path(repo, "refs/heads", refs_heads_path, sizeof(refs_heads_path)) != MG_OK) {
        return MG_INVALID_ARG;
    }

    /*
     * Detached HEAD has no current branch, but branch listing still works; no
     * branch will be marked current in that case.
     */
    result = repo_current_branch(repo, current_branch, sizeof(current_branch));
    if (result == MG_NOT_FOUND) {
        current_branch[0] = '\0';
        result = MG_OK;
    }
    if (result != MG_OK) {
        return result;
    }

    dir = opendir(refs_heads_path);
    if (dir == NULL) {
        return MG_IO_ERROR;
    }

    while ((entry = readdir(dir)) != NULL) {
        char **grown;
        char branch_path[MG_MAX_PATH];

        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }
        /* Ignore any ref file outside the v1 branch-name subset. */
        if (!repo_branch_name_is_valid(entry->d_name)) {
            continue;
        }
        if (fs_join_path(refs_heads_path, entry->d_name, branch_path, sizeof(branch_path)) != MG_OK) {
            result = MG_INVALID_ARG;
            break;
        }
        if (!fs_is_file(branch_path)) {
            continue;
        }

        if (count == capacity) {
            capacity = capacity == 0 ? 8 : capacity * 2;
            grown = realloc(names, capacity * sizeof(names[0]));
            if (grown == NULL) {
                result = MG_ERROR;
                break;
            }
            names = grown;
        }

        names[count] = copy_name(entry->d_name);
        if (names[count] == NULL) {
            result = MG_ERROR;
            break;
        }
        count++;
    }

    if (closedir(dir) != 0 && result == MG_OK) {
        result = MG_IO_ERROR;
    }

    if (result == MG_OK) {
        /* Sort after reading all names so callback order is deterministic. */
        qsort(names, count, sizeof(names[0]), compare_names);
        for (size_t i = 0; i < count; i++) {
            result = callback(names[i], strcmp(names[i], current_branch) == 0, ctx);
            if (result != MG_OK) {
                break;
            }
        }
    }

    for (size_t i = 0; i < count; i++) {
        free(names[i]);
    }
    free(names);
    return result;
}

/*
 * repo_branch_exists checks for a flat refs/heads/<name> file.
 */
MGResult repo_branch_exists(const Repository *repo, const char *name, int *out_exists) {
    char refs_heads_path[MG_MAX_PATH];
    char branch_path[MG_MAX_PATH];

    if (repo == NULL || out_exists == NULL || !repo_branch_name_is_valid(name)) {
        return MG_INVALID_ARG;
    }
    if (repo_path(repo, "refs/heads", refs_heads_path, sizeof(refs_heads_path)) != MG_OK ||
        fs_join_path(refs_heads_path, name, branch_path, sizeof(branch_path)) != MG_OK) {
        return MG_INVALID_ARG;
    }

    *out_exists = fs_is_file(branch_path);
    return MG_OK;
}

/*
 * repo_read_branch_commit reads the commit hash stored in one branch ref.
 * Empty branch files are preserved as empty strings for unborn branches.
 */
MGResult repo_read_branch_commit(const Repository *repo, const char *name, char *out, size_t out_size) {
    char refs_heads_path[MG_MAX_PATH];
    char branch_path[MG_MAX_PATH];
    unsigned char *data = NULL;
    size_t size = 0;
    int exists;
    MGResult result;

    if (out == NULL || out_size == 0) {
        return MG_INVALID_ARG;
    }

    result = repo_branch_exists(repo, name, &exists);
    if (result != MG_OK) {
        return result;
    }
    if (!exists) {
        return MG_NOT_FOUND;
    }

    if (repo_path(repo, "refs/heads", refs_heads_path, sizeof(refs_heads_path)) != MG_OK ||
        fs_join_path(refs_heads_path, name, branch_path, sizeof(branch_path)) != MG_OK) {
        return MG_INVALID_ARG;
    }
    if (fs_read_file(branch_path, &data, &size) != MG_OK) {
        return MG_IO_ERROR;
    }

    result = copy_trimmed_text(data, size, out, out_size);
    free(data);
    return result;
}

/*
 * repo_write_head_to_branch reattaches HEAD to an existing branch.
 */
MGResult repo_write_head_to_branch(const Repository *repo, const char *name) {
    char contents[MG_MAX_PATH];
    int written;
    int exists;
    MGResult result;

    if (repo == NULL || !repo_branch_name_is_valid(name)) {
        return MG_INVALID_ARG;
    }

    result = repo_branch_exists(repo, name, &exists);
    if (result != MG_OK) {
        return result;
    }
    if (!exists) {
        return MG_NOT_FOUND;
    }

    written = snprintf(contents, sizeof(contents), "ref: refs/heads/%s\n", name);
    if (written < 0 || (size_t)written >= sizeof(contents)) {
        return MG_INVALID_ARG;
    }

    return repo_write_head(repo, contents);
}
