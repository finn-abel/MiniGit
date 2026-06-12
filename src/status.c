#define _POSIX_C_SOURCE 200809L

#include "status.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "commit.h"
#include "fs.h"
#include "hash.h"
#include "ignore.h"
#include "index.h"
#include "tree.h"

/*
 * ChangeKind identifies the user-facing status label.
 */
typedef enum {
    CHANGE_ADDED,
    CHANGE_MODIFIED,
    CHANGE_DELETED,
    CHANGE_RENAMED,
    CHANGE_UNTRACKED
} ChangeKind;

/*
 * Change stores one status line before stable sorting and printing.
 */
typedef struct {
    ChangeKind kind;
    char path[MG_MAX_PATH];
    char old_path[MG_MAX_PATH];
} Change;

/*
 * ChangeList owns a growable array of status changes.
 */
typedef struct {
    Change *items;
    size_t count;
    size_t capacity;
} ChangeList;

/*
 * UntrackedContext carries state while walking the working tree.
 */
typedef struct {
    const Index *index;
    const IgnoreRules *ignore_rules;
    ChangeList *untracked;
} UntrackedContext;

/*
 * change_label maps internal change kinds to status text.
 */
static const char *change_label(ChangeKind kind) {
    switch (kind) {
        case CHANGE_ADDED:
            return "added";
        case CHANGE_MODIFIED:
            return "modified";
        case CHANGE_DELETED:
            return "deleted";
        case CHANGE_RENAMED:
            return "renamed";
        case CHANGE_UNTRACKED:
            return "";
    }
    return "";
}

/*
 * compare_changes keeps output sorted by visible destination path.
 */
static int compare_changes(const void *left, const void *right) {
    const Change *a = left;
    const Change *b = right;
    return strcmp(a->path, b->path);
}

/*
 * change_list_init prepares an empty list.
 */
static void change_list_init(ChangeList *list) {
    list->items = NULL;
    list->count = 0;
    list->capacity = 0;
}

/*
 * change_list_add appends one path/kind pair.
 */
static MGResult change_list_add(ChangeList *list, ChangeKind kind, const char *path) {
    Change *grown;
    size_t capacity;

    if (list == NULL || path == NULL || path[0] == '\0' || strlen(path) >= MG_MAX_PATH) {
        return MG_INVALID_ARG;
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

    list->items[list->count].kind = kind;
    strcpy(list->items[list->count].path, path);
    list->items[list->count].old_path[0] = '\0';
    list->count++;
    return MG_OK;
}

static MGResult change_list_add_rename(ChangeList *list, const char *old_path, const char *new_path) {
    MGResult result;

    if (old_path == NULL || old_path[0] == '\0' || strlen(old_path) >= MG_MAX_PATH) {
        return MG_INVALID_ARG;
    }
    result = change_list_add(list, CHANGE_RENAMED, new_path);
    if (result != MG_OK) {
        return result;
    }
    strcpy(list->items[list->count - 1].old_path, old_path);
    return MG_OK;
}

static int change_list_has_rename_from(const ChangeList *list, const char *old_path) {
    for (size_t i = 0; i < list->count; i++) {
        if (list->items[i].kind == CHANGE_RENAMED && strcmp(list->items[i].old_path, old_path) == 0) {
            return 1;
        }
    }
    return 0;
}

/*
 * change_list_sort gives deterministic section ordering.
 */
static void change_list_sort(ChangeList *list) {
    if (list != NULL && list->count > 1) {
        qsort(list->items, list->count, sizeof(list->items[0]), compare_changes);
    }
}

/*
 * change_list_free releases list storage.
 */
static void change_list_free(ChangeList *list) {
    if (list == NULL) {
        return;
    }
    free(list->items);
    list->items = NULL;
    list->count = 0;
    list->capacity = 0;
}

/*
 * tree_find returns the entry for path or NULL.
 */
static const TreeEntry *tree_find(const Tree *tree, const char *path) {
    if (tree == NULL || path == NULL) {
        return NULL;
    }

    for (size_t i = 0; i < tree->count; i++) {
        if (strcmp(tree->entries[i].path, path) == 0) {
            return &tree->entries[i];
        }
    }
    return NULL;
}

static unsigned int mode_from_stat(const struct stat *st) {
    return (st->st_mode & S_IXUSR) ? 0100755 : 0100644;
}

/*
 * load_head_tree returns the current committed tree, or an empty tree before the first commit.
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
 * hash_working_file computes the blob object hash for a working-tree file without storing it.
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
 * compare_head_to_index finds staged additions, modifications, and deletions.
 */
static MGResult compare_head_to_index(const Tree *head_tree, const Index *index, ChangeList *staged) {
    const TreeEntry *head_entry;
    const IndexEntry *index_entry;
    MGResult result;

    for (size_t i = 0; i < index->count; i++) {
        index_entry = &index->entries[i];
        head_entry = tree_find(head_tree, index_entry->path);
        if (head_entry == NULL) {
            int renamed = 0;
            result = MG_OK;
            for (size_t j = 0; j < head_tree->count; j++) {
                const TreeEntry *candidate = &head_tree->entries[j];
                if (index_find_const(index, candidate->path) == NULL &&
                    !change_list_has_rename_from(staged, candidate->path) &&
                    strcmp(candidate->hash, index_entry->hash) == 0 &&
                    candidate->mode == index_entry->mode) {
                    result = change_list_add_rename(staged, candidate->path, index_entry->path);
                    renamed = 1;
                    break;
                }
            }
            if (result == MG_OK && !renamed) {
                result = change_list_add(staged, CHANGE_ADDED, index_entry->path);
            }
        } else if (strcmp(head_entry->hash, index_entry->hash) != 0 || head_entry->mode != index_entry->mode) {
            result = change_list_add(staged, CHANGE_MODIFIED, index_entry->path);
        } else {
            result = MG_OK;
        }
        if (result != MG_OK) {
            return result;
        }
    }

    for (size_t i = 0; i < head_tree->count; i++) {
        head_entry = &head_tree->entries[i];
        if (index_find_const(index, head_entry->path) == NULL) {
            if (change_list_has_rename_from(staged, head_entry->path)) {
                continue;
            }
            result = change_list_add(staged, CHANGE_DELETED, head_entry->path);
            if (result != MG_OK) {
                return result;
            }
        }
    }

    return MG_OK;
}

/*
 * compare_index_to_worktree finds tracked files changed outside the index.
 */
static MGResult compare_index_to_worktree(const Repository *repo, const Index *index, ChangeList *unstaged) {
    char full_path[MG_MAX_PATH];
    char working_hash[MG_HASH_HEX_SIZE];
    MGResult result;

    for (size_t i = 0; i < index->count; i++) {
        const IndexEntry *entry = &index->entries[i];
        if (fs_join_path(repo->worktree_path, entry->path, full_path, sizeof(full_path)) != MG_OK) {
            return MG_INVALID_ARG;
        }
        if (!fs_exists(full_path)) {
            result = change_list_add(unstaged, CHANGE_DELETED, entry->path);
        } else if (!fs_is_file(full_path)) {
            result = change_list_add(unstaged, CHANGE_MODIFIED, entry->path);
        } else {
            struct stat st;
            if (lstat(full_path, &st) != 0) {
                return MG_IO_ERROR;
            }
            result = hash_working_file(repo, entry->path, working_hash);
            if (result != MG_OK) {
                return result;
            }
            result = strcmp(working_hash, entry->hash) == 0 && mode_from_stat(&st) == entry->mode
                ? MG_OK
                : change_list_add(unstaged, CHANGE_MODIFIED, entry->path);
        }
        if (result != MG_OK) {
            return result;
        }
    }

    return MG_OK;
}

/*
 * collect_untracked adds regular files absent from the index.
 */
static MGResult collect_untracked(const char *relative_path, void *ctx) {
    UntrackedContext *context = ctx;
    if (index_find_const(context->index, relative_path) != NULL) {
        return MG_OK;
    }
    if (ignore_rules_match(context->ignore_rules, relative_path)) {
        return MG_OK;
    }
    return change_list_add(context->untracked, CHANGE_UNTRACKED, relative_path);
}

/*
 * print_labeled_section prints staged/unstaged changes.
 */
static void print_labeled_section(const char *title, const ChangeList *list) {
    if (list->count == 0) {
        return;
    }

    printf("%s:\n", title);
    for (size_t i = 0; i < list->count; i++) {
        if (list->items[i].kind == CHANGE_RENAMED) {
            printf("  %s: %s -> %s\n", change_label(list->items[i].kind), list->items[i].old_path, list->items[i].path);
        } else {
            printf("  %s: %s\n", change_label(list->items[i].kind), list->items[i].path);
        }
    }
}

/*
 * print_untracked_section prints untracked files.
 */
static void print_untracked_section(const ChangeList *list) {
    if (list->count == 0) {
        return;
    }

    puts("Untracked files:");
    for (size_t i = 0; i < list->count; i++) {
        printf("  %s\n", list->items[i].path);
    }
}

MGResult status_print(const Repository *repo) {
    Index index;
    Tree head_tree;
    ChangeList staged;
    ChangeList unstaged;
    ChangeList untracked;
    IgnoreRules ignore_rules;
    UntrackedContext untracked_context;
    MGResult result;

    if (repo == NULL) {
        return MG_INVALID_ARG;
    }

    change_list_init(&staged);
    change_list_init(&unstaged);
    change_list_init(&untracked);

    result = index_load(repo, &index);
    if (result != MG_OK) {
        goto done_without_index;
    }

    result = load_head_tree(repo, &head_tree);
    if (result != MG_OK) {
        goto done_with_index;
    }

    result = ignore_rules_load(repo, &ignore_rules);
    if (result != MG_OK) {
        goto done_with_tree;
    }

    result = compare_head_to_index(&head_tree, &index, &staged);
    if (result != MG_OK) {
        goto done_with_ignore_rules;
    }

    result = compare_index_to_worktree(repo, &index, &unstaged);
    if (result != MG_OK) {
        goto done_with_ignore_rules;
    }

    untracked_context.index = &index;
    untracked_context.ignore_rules = &ignore_rules;
    untracked_context.untracked = &untracked;
    result = fs_walk(repo->worktree_path, collect_untracked, &untracked_context);
    if (result != MG_OK) {
        goto done_with_ignore_rules;
    }

    change_list_sort(&staged);
    change_list_sort(&unstaged);
    change_list_sort(&untracked);
    print_labeled_section("Changes to be committed", &staged);
    print_labeled_section("Changes not staged for commit", &unstaged);
    print_untracked_section(&untracked);

done_with_ignore_rules:
    ignore_rules_free(&ignore_rules);
done_with_tree:
    tree_free(&head_tree);
done_with_index:
    index_free(&index);
done_without_index:
    change_list_free(&staged);
    change_list_free(&unstaged);
    change_list_free(&untracked);
    return result;
}
