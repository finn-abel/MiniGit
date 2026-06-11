#define _POSIX_C_SOURCE 200809L

#include "checkout.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "commit.h"
#include "fs.h"
#include "hash.h"
#include "index.h"
#include "object.h"
#include "tree.h"

/*
 * hash_working_file computes a blob object hash without storing an object.
 */
static MGResult hash_working_file(const Repository *repo, const char *relative_path, char out_hash[MG_HASH_HEX_SIZE]) {
    char full_path[MG_MAX_PATH];
    unsigned char *file_data = NULL;
    unsigned char *object_data;
    size_t file_size = 0;
    int header_size;
    size_t object_size;
    MGResult result;

    if (repo == NULL || relative_path == NULL || out_hash == NULL) {
        return MG_INVALID_ARG;
    }
    if (fs_join_path(repo->worktree_path, relative_path, full_path, sizeof(full_path)) != MG_OK) {
        return MG_INVALID_ARG;
    }

    result = fs_read_file(full_path, &file_data, &file_size);
    if (result != MG_OK) {
        return result;
    }

    header_size = snprintf(NULL, 0, "blob %zu", file_size);
    if (header_size < 0) {
        free(file_data);
        return MG_ERROR;
    }

    object_size = (size_t)header_size + 1 + file_size;
    object_data = malloc(object_size);
    if (object_data == NULL) {
        free(file_data);
        return MG_ERROR;
    }

    snprintf((char *)object_data, (size_t)header_size + 1, "blob %zu", file_size);
    object_data[header_size] = '\0';
    if (file_size > 0) {
        memcpy(object_data + header_size + 1, file_data, file_size);
    }

    result = hash_bytes(object_data, object_size, out_hash);
    free(object_data);
    free(file_data);
    return result;
}

/*
 * ensure_no_unstaged_changes refuses to overwrite tracked local edits.
 */
static MGResult ensure_no_unstaged_changes(const Repository *repo, const Index *index) {
    char full_path[MG_MAX_PATH];
    char working_hash[MG_HASH_HEX_SIZE];
    MGResult result;

    for (size_t i = 0; i < index->count; i++) {
        const IndexEntry *entry = &index->entries[i];
        if (fs_join_path(repo->worktree_path, entry->path, full_path, sizeof(full_path)) != MG_OK) {
            return MG_INVALID_ARG;
        }
        if (!fs_is_file(full_path)) {
            return MG_CONFLICT;
        }

        result = hash_working_file(repo, entry->path, working_hash);
        if (result != MG_OK) {
            return result;
        }
        if (strcmp(working_hash, entry->hash) != 0) {
            return MG_CONFLICT;
        }
    }

    return MG_OK;
}

/*
 * remove_files_absent_from_target deletes tracked files missing from target tree.
 */
static MGResult remove_files_absent_from_target(const Repository *repo, const Index *index, const Tree *target_tree) {
    char full_path[MG_MAX_PATH];

    for (size_t i = 0; i < index->count; i++) {
        const IndexEntry *entry = &index->entries[i];
        if (tree_find_entry(target_tree, entry->path) != NULL) {
            continue;
        }
        if (fs_join_path(repo->worktree_path, entry->path, full_path, sizeof(full_path)) != MG_OK) {
            return MG_INVALID_ARG;
        }
        if (fs_exists(full_path) && fs_is_file(full_path) && fs_remove_file(full_path) != MG_OK) {
            return MG_IO_ERROR;
        }
    }

    return MG_OK;
}

/*
 * restore_blob writes one target tree entry into the working tree.
 */
static MGResult restore_blob(const Repository *repo, const TreeEntry *entry, Index *new_index) {
    Object object;
    char full_path[MG_MAX_PATH];
    char parent_path[MG_MAX_PATH];
    struct stat st;
    MGResult result;

    result = object_read(repo, entry->hash, &object);
    if (result != MG_OK) {
        return result;
    }
    if (strcmp(object.type, "blob") != 0) {
        object_free(&object);
        return MG_PARSE_ERROR;
    }

    if (fs_join_path(repo->worktree_path, entry->path, full_path, sizeof(full_path)) != MG_OK ||
        fs_parent_dir(full_path, parent_path, sizeof(parent_path)) != MG_OK) {
        object_free(&object);
        return MG_INVALID_ARG;
    }
    if (strcmp(parent_path, ".") != 0 && fs_mkdir_p(parent_path) != MG_OK) {
        object_free(&object);
        return MG_IO_ERROR;
    }

    result = fs_write_file(full_path, object.payload, object.size);
    object_free(&object);
    if (result != MG_OK) {
        return result;
    }
    if (lstat(full_path, &st) != 0) {
        return MG_IO_ERROR;
    }

    return index_add_or_update(new_index, entry->path, entry->hash, entry->size, st.st_mtime);
}

/*
 * restore_tree writes all target tree entries and builds the replacement index.
 */
static MGResult restore_tree(const Repository *repo, const Tree *target_tree, Index *new_index) {
    MGResult result;

    index_init(new_index);
    for (size_t i = 0; i < target_tree->count; i++) {
        result = restore_blob(repo, &target_tree->entries[i], new_index);
        if (result != MG_OK) {
            index_free(new_index);
            return result;
        }
    }

    return MG_OK;
}

MGResult checkout_commit(const Repository *repo, const char commit_hash[MG_HASH_HEX_SIZE], int detached) {
    Commit commit;
    Tree target_tree;
    Index current_index;
    Index new_index;
    char head_contents[MG_HASH_HEX_SIZE + 1];
    int written;
    MGResult result;

    if (repo == NULL || !hash_is_valid_hex(commit_hash)) {
        return MG_INVALID_ARG;
    }

    result = commit_read(repo, commit_hash, &commit);
    if (result != MG_OK) {
        return result;
    }

    result = tree_read(repo, commit.tree_hash, &target_tree);
    commit_free(&commit);
    if (result != MG_OK) {
        return result;
    }

    result = index_load(repo, &current_index);
    if (result != MG_OK) {
        tree_free(&target_tree);
        return result;
    }

    result = ensure_no_unstaged_changes(repo, &current_index);
    if (result != MG_OK) {
        index_free(&current_index);
        tree_free(&target_tree);
        return result;
    }

    result = remove_files_absent_from_target(repo, &current_index, &target_tree);
    if (result == MG_OK) {
        result = restore_tree(repo, &target_tree, &new_index);
    }
    index_free(&current_index);
    tree_free(&target_tree);
    if (result != MG_OK) {
        return result;
    }

    result = index_save(repo, &new_index);
    index_free(&new_index);
    if (result != MG_OK) {
        return result;
    }

    if (detached) {
        written = snprintf(head_contents, sizeof(head_contents), "%s\n", commit_hash);
        if (written < 0 || (size_t)written >= sizeof(head_contents)) {
            return MG_INVALID_ARG;
        }
        return repo_write_head(repo, head_contents);
    }

    return MG_OK;
}

/*
 * checkout_restore_path restores one HEAD tree entry without changing HEAD.
 */
MGResult checkout_restore_path(const Repository *repo, const char *relative_path) {
    char commit_hash[MG_HASH_HEX_SIZE];
    Commit commit;
    Tree head_tree;
    Index index;
    const TreeEntry *entry;
    MGResult result;

    if (repo == NULL || relative_path == NULL || relative_path[0] == '\0') {
        return MG_INVALID_ARG;
    }

    result = repo_current_commit(repo, commit_hash, sizeof(commit_hash));
    if (result != MG_OK) {
        return result;
    }
    if (commit_hash[0] == '\0') {
        return MG_REPO_ERROR;
    }

    result = commit_read(repo, commit_hash, &commit);
    if (result != MG_OK) {
        return result;
    }

    result = tree_read(repo, commit.tree_hash, &head_tree);
    commit_free(&commit);
    if (result != MG_OK) {
        return result;
    }

    entry = tree_find_entry(&head_tree, relative_path);
    if (entry == NULL) {
        tree_free(&head_tree);
        return MG_NOT_FOUND;
    }

    result = index_load(repo, &index);
    if (result != MG_OK) {
        tree_free(&head_tree);
        return result;
    }

    result = restore_blob(repo, entry, &index);
    if (result == MG_OK) {
        result = index_save(repo, &index);
    }

    index_free(&index);
    tree_free(&head_tree);
    return result;
}

/*
 * checkout_restore_path_from_commit restores one tree entry from a commit without changing HEAD.
 */
MGResult checkout_restore_path_from_commit(
    const Repository *repo,
    const char commit_hash[MG_HASH_HEX_SIZE],
    const char *relative_path
) {
    Commit commit;
    Tree target_tree;
    Index index;
    const TreeEntry *entry;
    MGResult result;

    if (repo == NULL || !hash_is_valid_hex(commit_hash) || relative_path == NULL || relative_path[0] == '\0') {
        return MG_INVALID_ARG;
    }

    result = commit_read(repo, commit_hash, &commit);
    if (result != MG_OK) {
        return result;
    }

    result = tree_read(repo, commit.tree_hash, &target_tree);
    commit_free(&commit);
    if (result != MG_OK) {
        return result;
    }

    entry = tree_find_entry(&target_tree, relative_path);
    if (entry == NULL) {
        tree_free(&target_tree);
        return MG_NOT_FOUND;
    }

    result = index_load(repo, &index);
    if (result != MG_OK) {
        tree_free(&target_tree);
        return result;
    }

    result = restore_blob(repo, entry, &index);
    if (result == MG_OK) {
        result = index_save(repo, &index);
    }

    index_free(&index);
    tree_free(&target_tree);
    return result;
}
