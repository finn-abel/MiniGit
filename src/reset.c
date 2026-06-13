#define _POSIX_C_SOURCE 200809L

#include "reset.h"

#include <string.h>
#include <sys/stat.h>

#include "commit.h"
#include "fs.h"
#include "index.h"
#include "tree.h"

/*
 * load_head_tree returns HEAD's tree, or an empty tree before the first commit.
 */
static MGResult load_head_tree(const Repository *repo, Tree *tree) {
    char commit_hash[MG_HASH_HEX_SIZE];
    Commit commit;
    MGResult result;

    if (repo == NULL || tree == NULL) {
        return MG_INVALID_ARG;
    }

    tree->entries = NULL;
    tree->count = 0;
    tree->capacity = 0;

    result = repo_current_commit(repo, commit_hash, sizeof(commit_hash));
    if (result != MG_OK) {
        return result;
    }
    if (commit_hash[0] == '\0') {
        return MG_OK;
    }

    result = commit_read(repo, commit_hash, &commit);
    if (result != MG_OK) {
        return result;
    }

    result = tree_read(repo, commit.tree_hash, tree);
    commit_free(&commit);
    return result;
}

/*
 * current_mtime reads the worktree mtime when there is a regular file.
 * The index's hash and size come from HEAD; mtime is just cache metadata.
 */
static time_t current_mtime(const Repository *repo, const char *relative_path) {
    char full_path[MG_MAX_PATH];
    struct stat st;

    if (fs_join_path(repo->worktree_path, relative_path, full_path, sizeof(full_path)) != MG_OK) {
        return 0;
    }
    if (fs_validate_worktree_path(repo->worktree_path, relative_path) != MG_OK) {
        return 0;
    }
    if (lstat(full_path, &st) != 0 || !S_ISREG(st.st_mode)) {
        return 0;
    }
    return st.st_mtime;
}

MGResult reset_path(const Repository *repo, const char *relative_path) {
    Tree head_tree;
    Index index;
    const TreeEntry *head_entry;
    const IndexEntry *index_entry;
    MGResult result;

    if (repo == NULL || relative_path == NULL || relative_path[0] == '\0') {
        return MG_INVALID_ARG;
    }

    result = load_head_tree(repo, &head_tree);
    if (result != MG_OK) {
        return result;
    }

    result = index_load(repo, &index);
    if (result != MG_OK) {
        tree_free(&head_tree);
        return result;
    }

    head_entry = tree_find_entry(&head_tree, relative_path);
    index_entry = index_find_const(&index, relative_path);
    if (head_entry == NULL && index_entry == NULL) {
        index_free(&index);
        tree_free(&head_tree);
        return MG_NOT_FOUND;
    }

    if (head_entry == NULL) {
        result = index_remove(&index, relative_path);
        if (result == MG_NOT_FOUND) {
            result = MG_OK;
        }
    } else {
        result = index_add_or_update(
            &index,
            head_entry->path,
            head_entry->hash,
            head_entry->mode,
            head_entry->size,
            current_mtime(repo, relative_path)
        );
    }

    if (result == MG_OK) {
        result = index_save(repo, &index);
    }

    index_free(&index);
    tree_free(&head_tree);
    return result;
}
