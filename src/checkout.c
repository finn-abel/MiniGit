#define _POSIX_C_SOURCE 200809L

#include "checkout.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "commit.h"
#include "fs.h"
#include "hash.h"
#include "index.h"
#include "object.h"
#include "tree.h"

/*
 * DirectoryCollisionContext carries the target path being checked against an
 * existing worktree directory at that same path.
 */
typedef struct {
    const Index *index;
    const Tree *target_tree;
    const char *target_path;
} DirectoryCollisionContext;

typedef struct {
    Object object;
} PreparedBlob;

typedef struct {
    char path[MG_MAX_PATH];
    unsigned char *data;
    size_t size;
    unsigned int mode;
} WorktreeBackup;

static void prepared_blobs_free(PreparedBlob *prepared, size_t count) {
    if (prepared == NULL) {
        return;
    }
    for (size_t i = 0; i < count; i++) {
        object_free(&prepared[i].object);
    }
    free(prepared);
}

static MGResult prepare_target_blobs(const Repository *repo, const Tree *tree, PreparedBlob **out_prepared) {
    PreparedBlob *prepared;

    if (repo == NULL || tree == NULL || out_prepared == NULL) {
        return MG_INVALID_ARG;
    }
    *out_prepared = NULL;
    if (tree->count == 0) {
        return MG_OK;
    }
    prepared = calloc(tree->count, sizeof(prepared[0]));
    if (prepared == NULL) {
        return MG_ERROR;
    }
    for (size_t i = 0; i < tree->count; i++) {
        MGResult result = object_read(repo, tree->entries[i].hash, &prepared[i].object);
        if (result != MG_OK || strcmp(prepared[i].object.type, "blob") != 0 ||
            prepared[i].object.size != tree->entries[i].size) {
            prepared_blobs_free(prepared, tree->count);
            return result == MG_OK ? MG_PARSE_ERROR : result;
        }
    }
    *out_prepared = prepared;
    return MG_OK;
}

static void worktree_backups_free(WorktreeBackup *backups, size_t count) {
    if (backups == NULL) {
        return;
    }
    for (size_t i = 0; i < count; i++) {
        free(backups[i].data);
    }
    free(backups);
}

static MGResult backup_current_worktree(
    const Repository *repo,
    const Index *index,
    WorktreeBackup **out_backups
) {
    WorktreeBackup *backups;

    *out_backups = NULL;
    if (index->count == 0) {
        return MG_OK;
    }
    backups = calloc(index->count, sizeof(backups[0]));
    if (backups == NULL) {
        return MG_ERROR;
    }
    for (size_t i = 0; i < index->count; i++) {
        char full_path[MG_MAX_PATH];
        struct stat st;
        MGResult result;

        strcpy(backups[i].path, index->entries[i].path);
        result = fs_read_worktree_file(
            repo->worktree_path,
            backups[i].path,
            &backups[i].data,
            &backups[i].size
        );
        if (result != MG_OK ||
            fs_join_path(repo->worktree_path, backups[i].path, full_path, sizeof(full_path)) != MG_OK ||
            lstat(full_path, &st) != 0) {
            worktree_backups_free(backups, index->count);
            return result == MG_OK ? MG_IO_ERROR : result;
        }
        backups[i].mode = (st.st_mode & S_IXUSR) ? 0100755 : 0100644;
    }
    *out_backups = backups;
    return MG_OK;
}

static void prune_empty_parent_dirs(const Repository *repo, const char *relative_path) {
    char parent[MG_MAX_PATH];

    if (fs_parent_dir(relative_path, parent, sizeof(parent)) != MG_OK) {
        return;
    }
    while (strcmp(parent, ".") != 0) {
        char full_path[MG_MAX_PATH];
        char next[MG_MAX_PATH];

        if (fs_join_path(repo->worktree_path, parent, full_path, sizeof(full_path)) != MG_OK ||
            rmdir(full_path) != 0 || fs_parent_dir(parent, next, sizeof(next)) != MG_OK) {
            break;
        }
        strcpy(parent, next);
    }
}

static MGResult rollback_worktree(
    const Repository *repo,
    const Tree *target_tree,
    const WorktreeBackup *backups,
    size_t backup_count
) {
    MGResult rollback_result = MG_OK;

    for (size_t i = 0; i < target_tree->count; i++) {
        MGResult result = fs_remove_worktree_file(repo->worktree_path, target_tree->entries[i].path);
        if (result != MG_OK && result != MG_NOT_FOUND && result != MG_CONFLICT) {
            rollback_result = result;
        }
    }
    for (size_t i = target_tree->count; i > 0; i--) {
        prune_empty_parent_dirs(repo, target_tree->entries[i - 1].path);
    }
    for (size_t i = 0; i < backup_count; i++) {
        char full_path[MG_MAX_PATH];
        MGResult result = fs_write_worktree_file(
            repo->worktree_path,
            backups[i].path,
            backups[i].data,
            backups[i].size
        );
        if (result == MG_OK &&
            fs_join_path(repo->worktree_path, backups[i].path, full_path, sizeof(full_path)) == MG_OK &&
            chmod(full_path, backups[i].mode == 0100755 ? 0755 : 0644) != 0) {
            result = MG_IO_ERROR;
        }
        if (result != MG_OK) {
            rollback_result = result;
        }
    }
    return rollback_result;
}

/*
 * hash_working_file computes a blob object hash without storing an object.
 */
static MGResult hash_working_file(const Repository *repo, const char *relative_path, char out_hash[MG_HASH_HEX_SIZE]) {
    char full_path[MG_MAX_PATH];
    unsigned char *file_data = NULL;
    size_t file_size = 0;
    MGResult result;

    if (repo == NULL || relative_path == NULL || out_hash == NULL) {
        return MG_INVALID_ARG;
    }
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
        result = fs_validate_worktree_path(repo->worktree_path, entry->path);
        if (result != MG_OK || !fs_is_file(full_path)) {
            return MG_CONFLICT;
        }
        struct stat st;
        if (lstat(full_path, &st) != 0) {
            return MG_IO_ERROR;
        }

        result = hash_working_file(repo, entry->path, working_hash);
        if (result != MG_OK) {
            return result;
        }
        if (strcmp(working_hash, entry->hash) != 0 ||
            (((st.st_mode & S_IXUSR) ? 0100755 : 0100644) != entry->mode)) {
            return MG_CONFLICT;
        }
    }

    return MG_OK;
}

static int tracked_file_will_be_removed(const Index *index, const Tree *target_tree, const char *relative_path) {
    return index_find_const(index, relative_path) != NULL && tree_find_entry(target_tree, relative_path) == NULL;
}

/*
 * ensure_target_parent_dirs_clear refuses paths that cannot be created because
 * an existing file occupies one of the target's parent directory positions.
 */
static MGResult ensure_target_parent_dirs_clear(
    const Repository *repo,
    const Index *index,
    const Tree *target_tree,
    const char *relative_path
) {
    char prefix[MG_MAX_PATH];
    char full_path[MG_MAX_PATH];
    size_t prefix_len = 0;

    for (size_t i = 0; relative_path[i] != '\0'; i++) {
        if (relative_path[i] != '/') {
            if (prefix_len + 1 >= sizeof(prefix)) {
                return MG_INVALID_ARG;
            }
            prefix[prefix_len++] = relative_path[i];
            continue;
        }

        if (prefix_len == 0) {
            return MG_INVALID_ARG;
        }
        prefix[prefix_len] = '\0';
        if (fs_join_path(repo->worktree_path, prefix, full_path, sizeof(full_path)) != MG_OK) {
            return MG_INVALID_ARG;
        }
        if (fs_exists(full_path) && !fs_is_dir(full_path)) {
            if (!tracked_file_will_be_removed(index, target_tree, prefix)) {
                return MG_CONFLICT;
            }
        }

        if (prefix_len + 1 >= sizeof(prefix)) {
            return MG_INVALID_ARG;
        }
        prefix[prefix_len++] = '/';
    }

    return MG_OK;
}

/*
 * ensure_directory_collision_file_is_safe rejects untracked files inside an
 * existing directory that the target tree wants to replace with a file.
 */
static MGResult ensure_directory_collision_file_is_safe(const char *walk_relative_path, void *ctx) {
    DirectoryCollisionContext *context = ctx;
    char relative_path[MG_MAX_PATH];

    if (fs_join_path(context->target_path, walk_relative_path, relative_path, sizeof(relative_path)) != MG_OK) {
        return MG_INVALID_ARG;
    }
    return tracked_file_will_be_removed(context->index, context->target_tree, relative_path) ? MG_OK : MG_CONFLICT;
}

/*
 * ensure_target_path_clear refuses exact-path collisions, except when an
 * existing directory contains only tracked files that this checkout will remove.
 */
static MGResult ensure_target_path_clear(
    const Repository *repo,
    const Index *index,
    const Tree *target_tree,
    const char *relative_path
) {
    char full_path[MG_MAX_PATH];
    DirectoryCollisionContext context;

    if (index_find_const(index, relative_path) != NULL) {
        return MG_OK;
    }
    if (fs_join_path(repo->worktree_path, relative_path, full_path, sizeof(full_path)) != MG_OK) {
        return MG_INVALID_ARG;
    }
    if (!fs_exists(full_path)) {
        return MG_OK;
    }
    if (!fs_is_dir(full_path)) {
        return MG_CONFLICT;
    }

    context.index = index;
    context.target_tree = target_tree;
    context.target_path = relative_path;
    return fs_walk(full_path, ensure_directory_collision_file_is_safe, &context);
}

/*
 * ensure_no_untracked_overwrites refuses to clobber files that are absent from
 * the current index but would be written by the target commit.
 */
static MGResult ensure_no_untracked_overwrites(const Repository *repo, const Index *index, const Tree *target_tree) {
    MGResult result;

    for (size_t i = 0; i < target_tree->count; i++) {
        const TreeEntry *entry = &target_tree->entries[i];

        result = ensure_target_parent_dirs_clear(repo, index, target_tree, entry->path);
        if (result != MG_OK) {
            return result;
        }
        result = ensure_target_path_clear(repo, index, target_tree, entry->path);
        if (result != MG_OK) {
            return result;
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
        if (fs_exists(full_path)) {
            MGResult remove_result = fs_remove_worktree_file(repo->worktree_path, entry->path);
            if (remove_result != MG_OK) {
                return remove_result;
            }
        }
    }

    return MG_OK;
}

/*
 * remove_empty_dir_tree removes a directory that has already been proven not
 * to contain untracked files. It leaves non-empty or unexpected entries as a
 * conflict instead of forcing deletion.
 */
static MGResult remove_empty_dir_tree(const char *path) {
    DIR *dir;
    struct dirent *entry;
    MGResult result = MG_OK;

    dir = opendir(path);
    if (dir == NULL) {
        return MG_IO_ERROR;
    }

    while ((entry = readdir(dir)) != NULL && result == MG_OK) {
        char child[MG_MAX_PATH];

        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }
        if (fs_join_path(path, entry->d_name, child, sizeof(child)) != MG_OK) {
            result = MG_INVALID_ARG;
        } else if (fs_is_dir(child)) {
            result = remove_empty_dir_tree(child);
        } else if (fs_exists(child)) {
            result = MG_CONFLICT;
        }
    }

    if (closedir(dir) != 0 && result == MG_OK) {
        result = MG_IO_ERROR;
    }
    if (result == MG_OK && rmdir(path) != 0) {
        result = MG_IO_ERROR;
    }
    return result;
}

/*
 * remove_directories_replaced_by_files clears directory shells after their
 * tracked contents were removed, allowing clean dir-to-file checkouts.
 */
static MGResult remove_directories_replaced_by_files(const Repository *repo, const Tree *target_tree) {
    char full_path[MG_MAX_PATH];
    MGResult result;

    for (size_t i = 0; i < target_tree->count; i++) {
        if (fs_join_path(repo->worktree_path, target_tree->entries[i].path, full_path, sizeof(full_path)) != MG_OK) {
            return MG_INVALID_ARG;
        }
        if (!fs_is_dir(full_path)) {
            continue;
        }
        result = remove_empty_dir_tree(full_path);
        if (result != MG_OK) {
            return result;
        }
    }

    return MG_OK;
}

/*
 * restore_blob writes one target tree entry into the working tree.
 */
static MGResult restore_prepared_blob(
    const Repository *repo,
    const TreeEntry *entry,
    const Object *object,
    Index *new_index
) {
    char full_path[MG_MAX_PATH];
    char parent_path[MG_MAX_PATH];
    struct stat st;
    MGResult result;

    if (object == NULL || strcmp(object->type, "blob") != 0 || object->size != entry->size) {
        return MG_PARSE_ERROR;
    }

    if (fs_join_path(repo->worktree_path, entry->path, full_path, sizeof(full_path)) != MG_OK ||
        fs_parent_dir(full_path, parent_path, sizeof(parent_path)) != MG_OK) {
        return MG_INVALID_ARG;
    }
    if (strcmp(parent_path, ".") != 0 && fs_mkdir_p(parent_path) != MG_OK) {
        return MG_IO_ERROR;
    }

    result = fs_write_worktree_file(repo->worktree_path, entry->path, object->payload, object->size);
    if (result != MG_OK) {
        return result;
    }
    if (chmod(full_path, entry->mode == 0100755 ? 0755 : 0644) != 0) {
        return MG_IO_ERROR;
    }
    if (lstat(full_path, &st) != 0) {
        return MG_IO_ERROR;
    }

    return index_add_or_update(new_index, entry->path, entry->hash, entry->mode, entry->size, st.st_mtime);
}

static MGResult restore_path_transaction(const Repository *repo, const TreeEntry *entry, Index *index) {
    char full_path[MG_MAX_PATH];
    unsigned char *backup_data = NULL;
    size_t backup_size = 0;
    unsigned int backup_mode = 0100644;
    int existed = 0;
    struct stat st;
    Object object;
    MGResult result;

    if (fs_join_path(repo->worktree_path, entry->path, full_path, sizeof(full_path)) != MG_OK) {
        return MG_INVALID_ARG;
    }
    result = object_read(repo, entry->hash, &object);
    if (result != MG_OK) {
        return result;
    }
    if (lstat(full_path, &st) == 0) {
        result = fs_read_worktree_file(repo->worktree_path, entry->path, &backup_data, &backup_size);
        if (result != MG_OK) {
            object_free(&object);
            return result;
        }
        existed = 1;
        backup_mode = (st.st_mode & S_IXUSR) ? 0100755 : 0100644;
    }

    result = restore_prepared_blob(repo, entry, &object, index);
    object_free(&object);
    if (result == MG_OK) {
        result = index_save(repo, index);
    }
    if (result != MG_OK) {
        if (existed) {
            if (fs_write_worktree_file(repo->worktree_path, entry->path, backup_data, backup_size) == MG_OK) {
                (void)chmod(full_path, backup_mode == 0100755 ? 0755 : 0644);
            }
        } else {
            (void)fs_remove_worktree_file(repo->worktree_path, entry->path);
            prune_empty_parent_dirs(repo, entry->path);
        }
    }
    free(backup_data);
    return result;
}

/*
 * restore_tree writes all target tree entries and builds the replacement index.
 */
static MGResult restore_tree(
    const Repository *repo,
    const Tree *target_tree,
    const PreparedBlob *prepared,
    Index *new_index
) {
    MGResult result;

    index_init(new_index);
    for (size_t i = 0; i < target_tree->count; i++) {
        result = restore_prepared_blob(repo, &target_tree->entries[i], &prepared[i].object, new_index);
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
    PreparedBlob *prepared = NULL;
    WorktreeBackup *backups = NULL;
    char head_contents[MG_HASH_HEX_SIZE + 1];
    char old_head[MG_MAX_PATH];
    int written;
    int new_index_ready = 0;
    int index_updated = 0;
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
        goto done;
    }

    result = ensure_no_untracked_overwrites(repo, &current_index, &target_tree);
    if (result != MG_OK) {
        goto done;
    }
    result = prepare_target_blobs(repo, &target_tree, &prepared);
    if (result != MG_OK) {
        goto done;
    }
    result = backup_current_worktree(repo, &current_index, &backups);
    if (result != MG_OK) {
        goto done;
    }
    if (detached) {
        result = repo_read_head(repo, old_head, sizeof(old_head));
        if (result != MG_OK) {
            goto done;
        }
    }

    result = remove_files_absent_from_target(repo, &current_index, &target_tree);
    if (result == MG_OK) {
        result = remove_directories_replaced_by_files(repo, &target_tree);
    }
    if (result == MG_OK) {
        result = restore_tree(repo, &target_tree, prepared, &new_index);
        new_index_ready = result == MG_OK;
    }
    if (result != MG_OK) {
        (void)rollback_worktree(repo, &target_tree, backups, current_index.count);
        goto done;
    }

    result = index_save(repo, &new_index);
    if (result != MG_OK) {
        (void)rollback_worktree(repo, &target_tree, backups, current_index.count);
        goto done;
    }
    index_updated = 1;

    if (detached) {
        written = snprintf(head_contents, sizeof(head_contents), "%s\n", commit_hash);
        if (written < 0 || (size_t)written >= sizeof(head_contents)) {
            result = MG_INVALID_ARG;
        } else {
            result = repo_write_head(repo, head_contents);
        }
        if (result != MG_OK) {
            if (index_updated) {
                (void)index_save(repo, &current_index);
            }
            (void)repo_write_head(repo, old_head);
            (void)rollback_worktree(repo, &target_tree, backups, current_index.count);
            goto done;
        }
    }

done:
    if (new_index_ready) {
        index_free(&new_index);
    }
    worktree_backups_free(backups, current_index.count);
    prepared_blobs_free(prepared, target_tree.count);
    index_free(&current_index);
    tree_free(&target_tree);
    return result;
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

    result = restore_path_transaction(repo, entry, &index);

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

    result = restore_path_transaction(repo, entry, &index);

    index_free(&index);
    tree_free(&target_tree);
    return result;
}
