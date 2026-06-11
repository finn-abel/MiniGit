#define _POSIX_C_SOURCE 200809L

#include "diff.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "commit.h"
#include "fs.h"
#include "index.h"
#include "object.h"
#include "tree.h"

/*
 * FileContent represents one side of a diff. Missing files keep exists false
 * so additions and deletions can share the same comparison path.
 */
typedef struct {
    unsigned char *data;
    size_t size;
    int exists;
} FileContent;

/*
 * DiffLine borrows a slice from FileContent. Lines are not copied; the owning
 * file buffer must outlive the LineList built from it.
 */
typedef struct {
    const unsigned char *data;
    size_t size;
} DiffLine;

/*
 * LineList stores all line slices for the LCS diff pass.
 */
typedef struct {
    DiffLine *items;
    size_t count;
} LineList;

/*
 * PathList owns the sorted set of tracked paths that might need diff output.
 */
typedef struct {
    char (*items)[MG_MAX_PATH];
    size_t count;
    size_t capacity;
} PathList;

/*
 * file_content_init prepares an absent file side.
 */
static void file_content_init(FileContent *content) {
    content->data = NULL;
    content->size = 0;
    content->exists = 0;
}

/*
 * file_content_free releases the byte buffer for one diff side.
 */
static void file_content_free(FileContent *content) {
    if (content == NULL) {
        return;
    }
    free(content->data);
    file_content_init(content);
}

/*
 * copy_bytes copies object payload bytes into an owned, NUL-padded buffer.
 */
static MGResult copy_bytes(const unsigned char *data, size_t size, FileContent *out) {
    if ((data == NULL && size > 0) || out == NULL) {
        return MG_INVALID_ARG;
    }

    out->data = malloc(size + 1);
    if (out->data == NULL) {
        return MG_ERROR;
    }
    if (size > 0) {
        memcpy(out->data, data, size);
    }
    out->data[size] = '\0';
    out->size = size;
    out->exists = 1;
    return MG_OK;
}

/*
 * read_blob_content loads a stored blob object as one side of a diff.
 */
static MGResult read_blob_content(const Repository *repo, const char *hash, FileContent *out) {
    Object object;
    MGResult result;

    if (repo == NULL || hash == NULL || out == NULL) {
        return MG_INVALID_ARG;
    }

    file_content_init(out);
    result = object_read(repo, hash, &object);
    if (result != MG_OK) {
        return result;
    }
    if (strcmp(object.type, "blob") != 0) {
        object_free(&object);
        return MG_PARSE_ERROR;
    }

    result = copy_bytes(object.payload, object.size, out);
    object_free(&object);
    return result;
}

/*
 * read_worktree_content loads a tracked path from the working tree.
 * A missing path remains absent; a non-file path is treated as changed.
 */
static MGResult read_worktree_content(const Repository *repo, const char *path, FileContent *out) {
    char full_path[MG_MAX_PATH];
    MGResult result;

    if (repo == NULL || path == NULL || out == NULL) {
        return MG_INVALID_ARG;
    }

    file_content_init(out);
    if (fs_join_path(repo->worktree_path, path, full_path, sizeof(full_path)) != MG_OK) {
        return MG_INVALID_ARG;
    }
    if (!fs_exists(full_path)) {
        return MG_OK;
    }
    if (!fs_is_file(full_path)) {
        out->exists = 1;
        return MG_OK;
    }

    result = fs_read_file(full_path, &out->data, &out->size);
    if (result == MG_OK) {
        out->exists = 1;
    }
    return result;
}

/*
 * load_head_tree returns the HEAD tree, or an empty tree before the first commit.
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
 * compare_paths gives deterministic multi-file diff output.
 */
static int compare_paths(const void *left, const void *right) {
    const char *const a = left;
    const char *const b = right;
    return strcmp(a, b);
}

/*
 * path_list_init prepares an empty path set.
 */
static void path_list_init(PathList *list) {
    list->items = NULL;
    list->count = 0;
    list->capacity = 0;
}

/*
 * path_list_contains keeps collect_paths from adding duplicates when a path is
 * present in both HEAD and the index.
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
 * path_list_add appends a path if it is not already present.
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
 * path_list_sort gives stable output independent of index/tree merge order.
 */
static void path_list_sort(PathList *list) {
    if (list != NULL && list->count > 1) {
        qsort(list->items, list->count, sizeof(list->items[0]), compare_paths);
    }
}

/*
 * path_list_free releases the owned path array.
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
 * collect_paths builds the tracked path universe for the requested diff mode.
 * Plain worktree diff only needs index paths; staged and HEAD diffs also need
 * HEAD-only paths so deletions are visible.
 */
static MGResult collect_paths(const Tree *head_tree, const Index *index, DiffMode mode, PathList *paths) {
    MGResult result;

    path_list_init(paths);

    if (mode != DIFF_WORKTREE) {
        for (size_t i = 0; i < head_tree->count; i++) {
            result = path_list_add(paths, head_tree->entries[i].path);
            if (result != MG_OK) {
                return result;
            }
        }
    }

    for (size_t i = 0; i < index->count; i++) {
        result = path_list_add(paths, index->entries[i].path);
        if (result != MG_OK) {
            return result;
        }
    }

    path_list_sort(paths);
    return MG_OK;
}

/*
 * content_equal compares file existence and raw bytes before doing line work.
 */
static int content_equal(const FileContent *left, const FileContent *right) {
    if (left->exists != right->exists || left->size != right->size) {
        return 0;
    }
    if (!left->exists) {
        return 1;
    }
    return left->size == 0 || memcmp(left->data, right->data, left->size) == 0;
}

/*
 * content_is_binary uses a NUL byte as the v1 binary-file heuristic.
 */
static int content_is_binary(const FileContent *content) {
    if (content == NULL || !content->exists) {
        return 0;
    }
    return content->size > 0 && memchr(content->data, '\0', content->size) != NULL;
}

/*
 * split_lines turns a file buffer into borrowed line slices, preserving trailing
 * newlines when present and keeping a final unterminated line as its own slice.
 */
static MGResult split_lines(const FileContent *content, LineList *lines) {
    size_t count = 0;
    size_t start = 0;

    if (content == NULL || lines == NULL) {
        return MG_INVALID_ARG;
    }

    lines->items = NULL;
    lines->count = 0;
    if (!content->exists || content->size == 0) {
        return MG_OK;
    }

    for (size_t i = 0; i < content->size; i++) {
        if (content->data[i] == '\n') {
            count++;
        }
    }
    if (content->data[content->size - 1] != '\n') {
        count++;
    }

    lines->items = malloc(count * sizeof(lines->items[0]));
    if (lines->items == NULL) {
        return MG_ERROR;
    }

    for (size_t i = 0; i < content->size; i++) {
        if (content->data[i] == '\n') {
            lines->items[lines->count].data = content->data + start;
            lines->items[lines->count].size = i - start + 1;
            lines->count++;
            start = i + 1;
        }
    }
    if (start < content->size) {
        lines->items[lines->count].data = content->data + start;
        lines->items[lines->count].size = content->size - start;
        lines->count++;
    }

    return MG_OK;
}

/*
 * line_list_free releases line slice metadata, not the underlying file bytes.
 */
static void line_list_free(LineList *lines) {
    if (lines == NULL) {
        return;
    }
    free(lines->items);
    lines->items = NULL;
    lines->count = 0;
}

/*
 * lines_equal compares complete line slices, including newline bytes.
 */
static int lines_equal(const DiffLine *left, const DiffLine *right) {
    return left->size == right->size &&
           (left->size == 0 || memcmp(left->data, right->data, left->size) == 0);
}

/*
 * print_diff_line writes one context/add/delete line and normalizes missing
 * trailing newlines for readable terminal output.
 */
static void print_diff_line(char prefix, const DiffLine *line) {
    putchar(prefix);
    if (line->size > 0) {
        (void)fwrite(line->data, 1, line->size, stdout);
    }
    if (line->size == 0 || line->data[line->size - 1] != '\n') {
        putchar('\n');
    }
}

/*
 * print_line_diff builds an LCS table for the two files and walks it to emit a
 * compact whole-file hunk. v1 intentionally omits Git-style line numbers.
 */
static MGResult print_line_diff(const char *path, const FileContent *old_content, const FileContent *new_content) {
    LineList old_lines;
    LineList new_lines;
    size_t *lcs = NULL;
    size_t old_count;
    size_t new_count;
    size_t i;
    size_t j;
    MGResult result;

    result = split_lines(old_content, &old_lines);
    if (result != MG_OK) {
        return result;
    }
    result = split_lines(new_content, &new_lines);
    if (result != MG_OK) {
        line_list_free(&old_lines);
        return result;
    }

    old_count = old_lines.count;
    new_count = new_lines.count;
    lcs = calloc((old_count + 1) * (new_count + 1), sizeof(lcs[0]));
    if (lcs == NULL) {
        line_list_free(&old_lines);
        line_list_free(&new_lines);
        return MG_ERROR;
    }

    /*
     * Fill the LCS matrix from the bottom-right so each cell can look one step
     * ahead for either keeping a common line or choosing insertion/deletion.
     */
    for (i = old_count; i > 0; i--) {
        for (j = new_count; j > 0; j--) {
            size_t index = (i - 1) * (new_count + 1) + (j - 1);
            if (lines_equal(&old_lines.items[i - 1], &new_lines.items[j - 1])) {
                lcs[index] = lcs[i * (new_count + 1) + j] + 1;
            } else {
                size_t delete_score = lcs[i * (new_count + 1) + (j - 1)];
                size_t insert_score = lcs[(i - 1) * (new_count + 1) + j];
                lcs[index] = delete_score > insert_score ? delete_score : insert_score;
            }
        }
    }

    printf("diff --minigit %s\n", path);
    printf("--- %s\n", old_content->exists ? path : "/dev/null");
    printf("+++ %s\n", new_content->exists ? path : "/dev/null");
    puts("@@");

    /*
     * Walk the LCS matrix from the start of both files. Equal lines are context;
     * otherwise the larger remaining LCS score decides whether to print an
     * insertion or deletion next.
     */
    i = 0;
    j = 0;
    while (i < old_count || j < new_count) {
        if (i < old_count && j < new_count && lines_equal(&old_lines.items[i], &new_lines.items[j])) {
            print_diff_line(' ', &old_lines.items[i]);
            i++;
            j++;
        } else if (j < new_count &&
                   (i == old_count || lcs[i * (new_count + 1) + (j + 1)] >= lcs[(i + 1) * (new_count + 1) + j])) {
            print_diff_line('+', &new_lines.items[j]);
            j++;
        } else if (i < old_count) {
            print_diff_line('-', &old_lines.items[i]);
            i++;
        }
    }

    free(lcs);
    line_list_free(&old_lines);
    line_list_free(&new_lines);
    return MG_OK;
}

/*
 * load_side_content maps each public mode onto its old/new snapshots:
 * index->worktree, HEAD->index, or HEAD->worktree.
 */
static MGResult load_side_content(
    const Repository *repo,
    const Tree *head_tree,
    const Index *index,
    DiffMode mode,
    const char *path,
    FileContent *old_content,
    FileContent *new_content
) {
    const TreeEntry *tree_entry;
    const IndexEntry *index_entry;
    MGResult result = MG_OK;

    file_content_init(old_content);
    file_content_init(new_content);

    tree_entry = tree_find_entry(head_tree, path);
    index_entry = index_find_const(index, path);

    if (mode == DIFF_WORKTREE) {
        if (index_entry == NULL) {
            return MG_OK;
        }
        result = read_blob_content(repo, index_entry->hash, old_content);
        if (result == MG_OK) {
            result = read_worktree_content(repo, path, new_content);
        }
    } else if (mode == DIFF_STAGED) {
        if (tree_entry != NULL) {
            result = read_blob_content(repo, tree_entry->hash, old_content);
        }
        if (result == MG_OK && index_entry != NULL) {
            result = read_blob_content(repo, index_entry->hash, new_content);
        }
    } else {
        if (tree_entry != NULL) {
            result = read_blob_content(repo, tree_entry->hash, old_content);
        }
        if (result == MG_OK) {
            result = read_worktree_content(repo, path, new_content);
        }
    }

    if (result != MG_OK) {
        file_content_free(old_content);
        file_content_free(new_content);
    }
    return result;
}

/*
 * diff_print is the module entry point used by the command layer.
 */
MGResult diff_print(const Repository *repo, DiffMode mode) {
    Index index;
    Tree head_tree;
    PathList paths;
    MGResult result;

    if (repo == NULL) {
        return MG_INVALID_ARG;
    }

    result = index_load(repo, &index);
    if (result != MG_OK) {
        return result;
    }

    result = load_head_tree(repo, &head_tree);
    if (result != MG_OK) {
        index_free(&index);
        return result;
    }

    result = collect_paths(&head_tree, &index, mode, &paths);
    if (result != MG_OK) {
        tree_free(&head_tree);
        index_free(&index);
        return result;
    }

    for (size_t i = 0; i < paths.count; i++) {
        FileContent old_content;
        FileContent new_content;

        result = load_side_content(repo, &head_tree, &index, mode, paths.items[i], &old_content, &new_content);
        if (result != MG_OK) {
            path_list_free(&paths);
            tree_free(&head_tree);
            index_free(&index);
            return result;
        }

        if (!content_equal(&old_content, &new_content)) {
            if (content_is_binary(&old_content) || content_is_binary(&new_content)) {
                printf("diff --minigit %s\n", paths.items[i]);
                puts("Binary files differ");
            } else {
                result = print_line_diff(paths.items[i], &old_content, &new_content);
                if (result != MG_OK) {
                    file_content_free(&old_content);
                    file_content_free(&new_content);
                    path_list_free(&paths);
                    tree_free(&head_tree);
                    index_free(&index);
                    return result;
                }
            }
        }

        file_content_free(&old_content);
        file_content_free(&new_content);
    }

    path_list_free(&paths);
    tree_free(&head_tree);
    index_free(&index);
    return MG_OK;
}
