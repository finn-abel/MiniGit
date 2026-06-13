#define _POSIX_C_SOURCE 200809L

#include "merge.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "checkout.h"
#include "commit.h"
#include "fs.h"
#include "hash.h"
#include "index.h"
#include "object.h"
#include "tree.h"

/*
 * HashList stores first-parent ancestors while finding a merge base.
 */
typedef struct {
    char (*items)[MG_HASH_HEX_SIZE];
    size_t count;
    size_t capacity;
} HashList;

/*
 * PathList owns the sorted union of paths from base, ours, and theirs.
 */
typedef struct {
    char (*items)[MG_MAX_PATH];
    size_t count;
    size_t capacity;
} PathList;

/*
 * hash_list_init prepares an empty ancestor list.
 */
static void hash_list_init(HashList *list) {
    list->items = NULL;
    list->count = 0;
    list->capacity = 0;
}

/*
 * hash_list_add appends one validated commit hash.
 */
static MGResult hash_list_add(HashList *list, const char *hash) {
    char (*grown)[MG_HASH_HEX_SIZE];
    size_t capacity;

    if (list == NULL || !hash_is_valid_hex(hash)) {
        return MG_INVALID_ARG;
    }
    for (size_t i = 0; i < list->count; i++) {
        if (strcmp(list->items[i], hash) == 0) {
            return MG_OK;
        }
    }
    if (list->count == list->capacity) {
        capacity = list->capacity == 0 ? 8 : list->capacity * 2;
        grown = realloc(list->items, capacity * sizeof(list->items[0]));
        if (grown == NULL) {
            return MG_ERROR;
        }
        list->items = grown;
        list->capacity = capacity;
    }
    strcpy(list->items[list->count], hash);
    list->count++;
    return MG_OK;
}

/*
 * hash_list_contains does a linear lookup; v1 histories are intentionally small.
 */
static int hash_list_contains(const HashList *list, const char *hash) {
    if (list == NULL || hash == NULL) {
        return 0;
    }
    for (size_t i = 0; i < list->count; i++) {
        if (strcmp(list->items[i], hash) == 0) {
            return 1;
        }
    }
    return 0;
}

/*
 * hash_list_free releases ancestor storage.
 */
static void hash_list_free(HashList *list) {
    if (list == NULL) {
        return;
    }
    free(list->items);
    list->items = NULL;
    list->count = 0;
    list->capacity = 0;
}

/*
 * compare_paths gives deterministic merge processing order.
 */
static int compare_paths(const void *left, const void *right) {
    const char *const a = left;
    const char *const b = right;
    return strcmp(a, b);
}

/*
 * path_list_init prepares an empty path union.
 */
static void path_list_init(PathList *list) {
    list->items = NULL;
    list->count = 0;
    list->capacity = 0;
}

/*
 * path_list_contains prevents duplicate paths in the three-tree union.
 */
static int path_list_contains(const PathList *list, const char *path) {
    for (size_t i = 0; i < list->count; i++) {
        if (strcmp(list->items[i], path) == 0) {
            return 1;
        }
    }
    return 0;
}

/*
 * path_list_add appends a repo-relative path if it is not already present.
 */
static MGResult path_list_add(PathList *list, const char *path) {
    char (*grown)[MG_MAX_PATH];
    size_t capacity;

    if (list == NULL || path == NULL || path[0] == '\0' || strlen(path) >= MG_MAX_PATH) {
        return MG_INVALID_ARG;
    }
    if (path_list_contains(list, path)) {
        return MG_OK;
    }
    if (list->count == list->capacity) {
        capacity = list->capacity == 0 ? 8 : list->capacity * 2;
        grown = realloc(list->items, capacity * sizeof(list->items[0]));
        if (grown == NULL) {
            return MG_ERROR;
        }
        list->items = grown;
        list->capacity = capacity;
    }
    strcpy(list->items[list->count], path);
    list->count++;
    return MG_OK;
}

/*
 * path_list_sort keeps merge behavior stable across runs.
 */
static void path_list_sort(PathList *list) {
    if (list != NULL && list->count > 1) {
        qsort(list->items, list->count, sizeof(list->items[0]), compare_paths);
    }
}

/*
 * path_list_free releases the path union.
 */
static void path_list_free(PathList *list) {
    if (list == NULL) {
        return;
    }
    free(list->items);
    list->items = NULL;
    list->count = 0;
    list->capacity = 0;
}

/*
 * tree_entry_same treats two absent entries as equal and otherwise compares blob hashes and modes.
 */
static int tree_entry_same(const TreeEntry *left, const TreeEntry *right) {
    if (left == NULL || right == NULL) {
        return left == right;
    }
    return strcmp(left->hash, right->hash) == 0 && left->mode == right->mode;
}

/*
 * collect_tree_paths builds the set of paths that can differ in a three-way merge.
 */
static MGResult collect_tree_paths(const Tree *base_tree, const Tree *ours_tree, const Tree *theirs_tree, PathList *paths) {
    MGResult result;

    path_list_init(paths);
    for (size_t i = 0; i < base_tree->count; i++) {
        result = path_list_add(paths, base_tree->entries[i].path);
        if (result != MG_OK) {
            return result;
        }
    }
    for (size_t i = 0; i < ours_tree->count; i++) {
        result = path_list_add(paths, ours_tree->entries[i].path);
        if (result != MG_OK) {
            return result;
        }
    }
    for (size_t i = 0; i < theirs_tree->count; i++) {
        result = path_list_add(paths, theirs_tree->entries[i].path);
        if (result != MG_OK) {
            return result;
        }
    }
    path_list_sort(paths);
    return MG_OK;
}

/*
 * read_commit_tree resolves a commit to its flat tree object.
 */
static MGResult read_commit_tree(const Repository *repo, const char *commit_hash, Tree *tree) {
    Commit commit;
    MGResult result;

    if (repo == NULL || !hash_is_valid_hex(commit_hash) || tree == NULL) {
        return MG_INVALID_ARG;
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
 * collect_ancestors walks first-parent history from one commit back to the root.
 */
static MGResult collect_ancestors(const Repository *repo, const char *start_hash, HashList *ancestors) {
    MGResult result;

    hash_list_init(ancestors);
    result = hash_list_add(ancestors, start_hash);
    if (result != MG_OK) {
        return result;
    }
    for (size_t i = 0; i < ancestors->count; i++) {
        Commit commit;

        result = commit_read(repo, ancestors->items[i], &commit);
        if (result != MG_OK) {
            return result;
        }
        if (commit.parent_hash[0] != '\0') {
            result = hash_list_add(ancestors, commit.parent_hash);
        }
        if (result == MG_OK && commit.second_parent_hash[0] != '\0') {
            result = hash_list_add(ancestors, commit.second_parent_hash);
        }
        commit_free(&commit);
        if (result != MG_OK) {
            return result;
        }
    }
    return MG_OK;
}

/*
 * find_merge_base returns the first commit on theirs' first-parent walk that is
 * also reachable from ours. This is simple but enough for MiniGit's linear-parent v1.
 */
static MGResult find_merge_base(
    const Repository *repo,
    const char *ours_hash,
    const char *theirs_hash,
    char out_hash[MG_HASH_HEX_SIZE]
) {
    HashList ours_ancestors;
    HashList theirs_ancestors;
    MGResult result;

    out_hash[0] = '\0';
    result = collect_ancestors(repo, ours_hash, &ours_ancestors);
    if (result != MG_OK) {
        hash_list_free(&ours_ancestors);
        return result;
    }

    result = collect_ancestors(repo, theirs_hash, &theirs_ancestors);
    if (result != MG_OK) {
        hash_list_free(&ours_ancestors);
        hash_list_free(&theirs_ancestors);
        return result;
    }
    for (size_t i = 0; i < theirs_ancestors.count; i++) {
        if (hash_list_contains(&ours_ancestors, theirs_ancestors.items[i])) {
            strcpy(out_hash, theirs_ancestors.items[i]);
            hash_list_free(&ours_ancestors);
            hash_list_free(&theirs_ancestors);
            return MG_OK;
        }
    }

    hash_list_free(&ours_ancestors);
    hash_list_free(&theirs_ancestors);
    return MG_NOT_FOUND;
}

/*
 * blob_hash_from_file computes the MiniGit blob object hash for a worktree file
 * without writing a new object.
 */
static MGResult blob_hash_from_file(const Repository *repo, const char *relative_path, char out_hash[MG_HASH_HEX_SIZE]) {
    char full_path[MG_MAX_PATH];
    unsigned char *file_data = NULL;
    size_t file_size = 0;
    MGResult result;

    if (fs_join_path(repo->worktree_path, relative_path, full_path, sizeof(full_path)) != MG_OK) {
        return MG_INVALID_ARG;
    }

    result = fs_read_worktree_file(repo->worktree_path, relative_path, &file_data, &file_size);
    if (result != MG_OK) {
        return result;
    }

    result = hash_object_bytes("blob", file_data, file_size, hash_default_object_mode(), out_hash);
    free(file_data);
    return result;
}

/*
 * ensure_clean_worktree refuses merges when the index or tracked worktree files
 * differ from HEAD. This avoids overwriting local edits.
 */
static MGResult ensure_clean_worktree(const Repository *repo, const Tree *head_tree, const Index *index) {
    char full_path[MG_MAX_PATH];
    char working_hash[MG_HASH_HEX_SIZE];
    const TreeEntry *head_entry;
    MGResult result;

    for (size_t i = 0; i < index->count; i++) {
        const IndexEntry *entry = &index->entries[i];
        head_entry = tree_find_entry(head_tree, entry->path);
        if (head_entry == NULL || strcmp(head_entry->hash, entry->hash) != 0 || head_entry->mode != entry->mode) {
            return MG_CONFLICT;
        }
        if (fs_join_path(repo->worktree_path, entry->path, full_path, sizeof(full_path)) != MG_OK) {
            return MG_INVALID_ARG;
        }
        result = fs_validate_worktree_path(repo->worktree_path, entry->path);
        if (result != MG_OK || !fs_is_file(full_path)) {
            return MG_CONFLICT;
        }
        struct stat st;
        if (lstat(full_path, &st) != 0) {
            return MG_IO_ERROR;
        }
        result = blob_hash_from_file(repo, entry->path, working_hash);
        if (result != MG_OK) {
            return result;
        }
        if (strcmp(working_hash, entry->hash) != 0 ||
            (((st.st_mode & S_IXUSR) ? 0100755 : 0100644) != entry->mode)) {
            return MG_CONFLICT;
        }
    }

    for (size_t i = 0; i < head_tree->count; i++) {
        if (index_find_const(index, head_tree->entries[i].path) == NULL) {
            return MG_CONFLICT;
        }
    }
    return MG_OK;
}

/*
 * read_blob_payload copies the payload for a tree entry. NULL entries represent
 * deleted files and return an empty absent side to callers like conflict writing.
 */
static MGResult read_blob_payload(const Repository *repo, const TreeEntry *entry, unsigned char **out_data, size_t *out_size) {
    Object object;
    MGResult result;

    *out_data = NULL;
    *out_size = 0;
    if (entry == NULL) {
        return MG_OK;
    }

    result = object_read(repo, entry->hash, &object);
    if (result != MG_OK) {
        return result;
    }
    if (strcmp(object.type, "blob") != 0) {
        object_free(&object);
        return MG_PARSE_ERROR;
    }

    *out_data = malloc(object.size + 1);
    if (*out_data == NULL) {
        object_free(&object);
        return MG_ERROR;
    }
    if (object.size > 0) {
        memcpy(*out_data, object.payload, object.size);
    }
    (*out_data)[object.size] = '\0';
    *out_size = object.size;
    object_free(&object);
    return MG_OK;
}

/*
 * write_file_content writes bytes to a repo-relative path, creating parents first.
 */
static MGResult write_file_content(const Repository *repo, const char *path, const unsigned char *data, size_t size) {
    return fs_write_worktree_file(repo->worktree_path, path, data, size);
}

/*
 * apply_tree_entry writes one chosen tree entry and stages it in the index.
 */
static MGResult apply_tree_entry(const Repository *repo, Index *index, const TreeEntry *entry) {
    unsigned char *data = NULL;
    size_t size = 0;
    struct stat st;
    char full_path[MG_MAX_PATH];
    MGResult result;

    result = read_blob_payload(repo, entry, &data, &size);
    if (result != MG_OK) {
        return result;
    }
    result = write_file_content(repo, entry->path, data, size);
    free(data);
    if (result != MG_OK) {
        return result;
    }
    if (fs_join_path(repo->worktree_path, entry->path, full_path, sizeof(full_path)) != MG_OK) {
        return MG_INVALID_ARG;
    }
    if (lstat(full_path, &st) != 0) {
        return MG_IO_ERROR;
    }
    if (chmod(full_path, entry->mode == 0100755 ? 0755 : 0644) != 0) {
        return MG_IO_ERROR;
    }
    return index_add_or_update(index, entry->path, entry->hash, entry->mode, entry->size, st.st_mtime);
}

/*
 * apply_delete removes a path from the worktree and index when the merge result is deletion.
 */
static MGResult apply_delete(const Repository *repo, Index *index, const char *path) {
    char full_path[MG_MAX_PATH];
    MGResult result;

    if (fs_join_path(repo->worktree_path, path, full_path, sizeof(full_path)) != MG_OK) {
        return MG_INVALID_ARG;
    }
    if (fs_exists(full_path)) {
        result = fs_remove_worktree_file(repo->worktree_path, path);
        if (result != MG_OK) {
            return result;
        }
    }
    result = index_remove(index, path);
    return result == MG_NOT_FOUND ? MG_OK : result;
}

/*
 * copy_payload_with_newline keeps conflict marker sections readable even when
 * one side lacks a trailing newline or is an absent/deleted side.
 */
static void copy_payload_with_newline(unsigned char *out, size_t *offset, const unsigned char *data, size_t size) {
    if (size > 0) {
        memcpy(out + *offset, data, size);
        *offset += size;
        if (data[size - 1] == '\n') {
            return;
        }
    }
    out[*offset] = '\n';
    *offset += 1;
}

/*
 * write_conflict_file writes standard conflict markers into the worktree. The
 * conflict is intentionally left unstaged for the user to resolve and add.
 */
static MGResult write_conflict_file(
    const Repository *repo,
    const char *path,
    const char *branch_name,
    const TreeEntry *ours_entry,
    const TreeEntry *theirs_entry
) {
    unsigned char *ours = NULL;
    unsigned char *theirs = NULL;
    unsigned char *merged;
    size_t ours_size = 0;
    size_t theirs_size = 0;
    size_t total_size;
    size_t offset = 0;
    int marker_size;
    MGResult result;

    result = read_blob_payload(repo, ours_entry, &ours, &ours_size);
    if (result != MG_OK) {
        return result;
    }
    result = read_blob_payload(repo, theirs_entry, &theirs, &theirs_size);
    if (result != MG_OK) {
        free(ours);
        return result;
    }

    marker_size = snprintf(NULL, 0, "<<<<<<< HEAD\n=======\n>>>>>>> %s\n", branch_name);
    if (marker_size < 0) {
        free(ours);
        free(theirs);
        return MG_ERROR;
    }
    total_size = (size_t)marker_size + ours_size + theirs_size + 2;
    merged = malloc(total_size + 1);
    if (merged == NULL) {
        free(ours);
        free(theirs);
        return MG_ERROR;
    }

    offset += (size_t)sprintf((char *)merged + offset, "<<<<<<< HEAD\n");
    copy_payload_with_newline(merged, &offset, ours, ours_size);
    offset += (size_t)sprintf((char *)merged + offset, "=======\n");
    copy_payload_with_newline(merged, &offset, theirs, theirs_size);
    offset += (size_t)sprintf((char *)merged + offset, ">>>>>>> %s\n", branch_name);
    merged[offset] = '\0';

    result = write_file_content(repo, path, merged, offset);
    free(merged);
    free(ours);
    free(theirs);
    return result;
}

static MGResult ensure_merge_write_is_safe(const Repository *repo, const Index *index, const char *path) {
    char prefix[MG_MAX_PATH];
    char full_path[MG_MAX_PATH];
    size_t prefix_len = 0;
    struct stat st;

    if (index_find_const(index, path) != NULL) {
        return MG_OK;
    }
    if (!fs_repo_relative_path_is_valid(path)) {
        return MG_PARSE_ERROR;
    }

    for (size_t i = 0; path[i] != '\0'; i++) {
        if (path[i] != '/') {
            prefix[prefix_len++] = path[i];
            continue;
        }
        prefix[prefix_len] = '\0';
        if (fs_join_path(repo->worktree_path, prefix, full_path, sizeof(full_path)) != MG_OK) {
            return MG_INVALID_ARG;
        }
        if (lstat(full_path, &st) == 0 && !S_ISDIR(st.st_mode)) {
            return MG_CONFLICT;
        }
        prefix[prefix_len++] = '/';
    }

    if (fs_join_path(repo->worktree_path, path, full_path, sizeof(full_path)) != MG_OK) {
        return MG_INVALID_ARG;
    }
    if (lstat(full_path, &st) == 0) {
        return MG_CONFLICT;
    }
    return MG_OK;
}

/*
 * apply_merge_trees handles the core three-way file decisions:
 * same as theirs => keep ours; same as ours => take theirs; otherwise conflict.
 */
static MGResult apply_merge_trees(
    const Repository *repo,
    const char *branch_name,
    const Tree *base_tree,
    const Tree *ours_tree,
    const Tree *theirs_tree,
    Index *index,
    int *out_conflicts
) {
    PathList paths;
    MGResult result;

    *out_conflicts = 0;
    result = collect_tree_paths(base_tree, ours_tree, theirs_tree, &paths);
    if (result != MG_OK) {
        path_list_free(&paths);
        return result;
    }

    for (size_t i = 0; i < paths.count; i++) {
        const char *path = paths.items[i];
        const TreeEntry *base_entry = tree_find_entry(base_tree, path);
        const TreeEntry *ours_entry = tree_find_entry(ours_tree, path);
        const TreeEntry *theirs_entry = tree_find_entry(theirs_tree, path);
        int writes_path = 0;

        if (!tree_entry_same(ours_entry, theirs_entry) && !tree_entry_same(base_entry, theirs_entry)) {
            writes_path = tree_entry_same(base_entry, ours_entry) ? theirs_entry != NULL : 1;
        }
        if (writes_path) {
            result = ensure_merge_write_is_safe(repo, index, path);
            if (result != MG_OK) {
                path_list_free(&paths);
                return result;
            }
        }
    }

    for (size_t i = 0; i < paths.count; i++) {
        const char *path = paths.items[i];
        const TreeEntry *base_entry = tree_find_entry(base_tree, path);
        const TreeEntry *ours_entry = tree_find_entry(ours_tree, path);
        const TreeEntry *theirs_entry = tree_find_entry(theirs_tree, path);

        if (tree_entry_same(ours_entry, theirs_entry) || tree_entry_same(base_entry, theirs_entry)) {
            continue;
        }
        if (tree_entry_same(base_entry, ours_entry)) {
            result = theirs_entry == NULL ? apply_delete(repo, index, path) : apply_tree_entry(repo, index, theirs_entry);
            if (result != MG_OK) {
                path_list_free(&paths);
                return result;
            }
            continue;
        }

        result = write_conflict_file(repo, path, branch_name, ours_entry, theirs_entry);
        if (result != MG_OK) {
            path_list_free(&paths);
            return result;
        }
        *out_conflicts = 1;
    }

    path_list_free(&paths);
    return MG_OK;
}

/*
 * merge_branch is the public entry point. It handles branch resolution,
 * fast-forward cases, clean-worktree checks, and then applies the three-way merge.
 */
MGResult merge_branch(const Repository *repo, const char *branch_name) {
    char ours_hash[MG_HASH_HEX_SIZE];
    char theirs_hash[MG_HASH_HEX_SIZE];
    char base_hash[MG_HASH_HEX_SIZE];
    Tree base_tree;
    Tree ours_tree;
    Tree theirs_tree;
    Index index;
    int conflicts = 0;
    char merge_tree_hash[MG_HASH_HEX_SIZE];
    char merge_commit_hash[MG_HASH_HEX_SIZE];
    char merge_message[MG_MAX_BRANCH + 32];
    MGResult result;

    if (repo == NULL || !repo_branch_name_is_valid(branch_name)) {
        return MG_INVALID_ARG;
    }

    result = repo_current_commit(repo, ours_hash, sizeof(ours_hash));
    if (result != MG_OK) {
        return result;
    }
    if (ours_hash[0] == '\0') {
        return MG_REPO_ERROR;
    }

    result = repo_read_branch_commit(repo, branch_name, theirs_hash, sizeof(theirs_hash));
    if (result != MG_OK) {
        return result;
    }
    if (theirs_hash[0] == '\0') {
        return MG_REPO_ERROR;
    }
    if (strcmp(ours_hash, theirs_hash) == 0) {
        return MG_OK;
    }

    result = read_commit_tree(repo, ours_hash, &ours_tree);
    if (result != MG_OK) {
        return result;
    }
    result = index_load(repo, &index);
    if (result != MG_OK) {
        tree_free(&ours_tree);
        return result;
    }
    result = ensure_clean_worktree(repo, &ours_tree, &index);
    if (result != MG_OK) {
        index_free(&index);
        tree_free(&ours_tree);
        return result;
    }

    result = find_merge_base(repo, ours_hash, theirs_hash, base_hash);
    if (result != MG_OK) {
        index_free(&index);
        tree_free(&ours_tree);
        return result;
    }
    if (strcmp(base_hash, theirs_hash) == 0) {
        index_free(&index);
        tree_free(&ours_tree);
        return MG_OK;
    }
    if (strcmp(base_hash, ours_hash) == 0) {
        index_free(&index);
        tree_free(&ours_tree);
        result = checkout_commit(repo, theirs_hash, 0);
        if (result == MG_OK) {
            result = repo_update_current_ref(repo, theirs_hash);
            if (result != MG_OK) {
                (void)checkout_commit(repo, ours_hash, 0);
            }
        }
        return result;
    }

    result = read_commit_tree(repo, base_hash, &base_tree);
    if (result != MG_OK) {
        index_free(&index);
        tree_free(&ours_tree);
        return result;
    }
    result = read_commit_tree(repo, theirs_hash, &theirs_tree);
    if (result != MG_OK) {
        tree_free(&base_tree);
        index_free(&index);
        tree_free(&ours_tree);
        return result;
    }

    result = apply_merge_trees(repo, branch_name, &base_tree, &ours_tree, &theirs_tree, &index, &conflicts);
    if (result == MG_OK && !conflicts) {
        result = index_save(repo, &index);
        if (result == MG_OK) {
            Tree merge_tree;
            result = tree_from_index(&index, &merge_tree);
            if (result == MG_OK) {
                result = tree_write(repo, &merge_tree, merge_tree_hash);
                tree_free(&merge_tree);
            }
        }
        if (result == MG_OK) {
            int written = snprintf(merge_message, sizeof(merge_message), "Merge branch %s", branch_name);
            if (written < 0 || (size_t)written >= sizeof(merge_message)) {
                result = MG_INVALID_ARG;
            }
        }
        if (result == MG_OK) {
            result = commit_create_merge(
                repo,
                merge_tree_hash,
                ours_hash,
                theirs_hash,
                merge_message,
                merge_commit_hash
            );
        }
        if (result == MG_OK) {
            result = repo_update_current_ref(repo, merge_commit_hash);
            if (result != MG_OK) {
                (void)checkout_commit(repo, ours_hash, 0);
            }
        }
    } else if (result == MG_OK) {
        (void)index_save(repo, &index);
        result = MG_CONFLICT;
    }

    tree_free(&theirs_tree);
    tree_free(&base_tree);
    index_free(&index);
    tree_free(&ours_tree);
    return result;
}
