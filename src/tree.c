#include "tree.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hash.h"
#include "object.h"

/*
 * tree_init prepares an empty tree for population.
 * A tree can be initialized from the index or from an object payload.
 */
static void tree_init(Tree *tree) {
    if (tree == NULL) {
        return;
    }
    tree->entries = NULL;
    tree->count = 0;
    tree->capacity = 0;
}

/*
 * compare_tree_entries keeps payload and parsed trees sorted by path.
 */
static int compare_tree_entries(const void *left, const void *right) {
    const TreeEntry *a = left;
    const TreeEntry *b = right;
    return strcmp(a->path, b->path);
}

/*
 * tree_sort gives tree objects deterministic payload ordering.
 */
static void tree_sort(Tree *tree) {
    if (tree != NULL && tree->count > 1) {
        qsort(tree->entries, tree->count, sizeof(tree->entries[0]), compare_tree_entries);
    }
}

/*
 * ensure_capacity grows the tree entry array as entries are appended.
 * The flat tree representation keeps every repo-relative path in one sorted
 * array; v1 does not build nested tree objects.
 */
static MGResult ensure_capacity(Tree *tree, size_t needed) {
    TreeEntry *grown;
    size_t capacity;

    if (tree == NULL) {
        return MG_INVALID_ARG;
    }
    if (needed <= tree->capacity) {
        return MG_OK;
    }

    capacity = tree->capacity == 0 ? 8 : tree->capacity;
    while (capacity < needed) {
        capacity *= 2;
    }

    grown = realloc(tree->entries, capacity * sizeof(tree->entries[0]));
    if (grown == NULL) {
        return MG_ERROR;
    }

    tree->entries = grown;
    tree->capacity = capacity;
    return MG_OK;
}

/*
 * append_entry copies a validated tree entry into the owned array.
 * Hash validation happens here so both index conversion and object parsing
 * share the same entry-level guard.
 */
static MGResult append_entry(Tree *tree, const char *path, const char *hash, size_t size) {
    TreeEntry entry;
    MGResult result;

    if (tree == NULL || path == NULL || hash == NULL || path[0] == '\0' ||
        strlen(path) >= sizeof(entry.path) || !hash_is_valid_hex(hash)) {
        return MG_INVALID_ARG;
    }

    memset(&entry, 0, sizeof(entry));
    strcpy(entry.path, path);
    strcpy(entry.hash, hash);
    entry.size = size;

    result = ensure_capacity(tree, tree->count + 1);
    if (result != MG_OK) {
        return result;
    }
    tree->entries[tree->count] = entry;
    tree->count++;
    return MG_OK;
}

/*
 * parse_size parses a complete decimal size field.
 * Partial parses are rejected so malformed payloads cannot quietly become
 * different sizes in memory.
 */
static MGResult parse_size(const char *text, size_t *out_size) {
    char *end = NULL;
    unsigned long parsed;

    if (text == NULL || text[0] == '\0' || out_size == NULL) {
        return MG_PARSE_ERROR;
    }

    parsed = strtoul(text, &end, 10);
    if (*end != '\0') {
        return MG_PARSE_ERROR;
    }

    *out_size = (size_t)parsed;
    return MG_OK;
}

/*
 * parse_tree_line validates one "100644 blob <hash> <size>\t<path>" line.
 * The tab separates metadata from path so paths can contain spaces.
 */
static MGResult parse_tree_line(Tree *tree, char *line) {
    char *tab;
    char *mode;
    char *type;
    char *hash;
    char *size_text;
    char *extra;
    char *path;
    size_t size;

    /* Split once on the required tab before tokenizing metadata by spaces. */
    tab = strchr(line, '\t');
    if (tab == NULL) {
        return MG_PARSE_ERROR;
    }
    *tab = '\0';
    path = tab + 1;
    if (path[0] == '\0' || strchr(path, '\n') != NULL) {
        return MG_PARSE_ERROR;
    }

    /* strtok is local to this mutable line from tree_read's payload buffer. */
    mode = strtok(line, " ");
    type = strtok(NULL, " ");
    hash = strtok(NULL, " ");
    size_text = strtok(NULL, " ");
    extra = strtok(NULL, " ");

    if (mode == NULL || type == NULL || hash == NULL || size_text == NULL || extra != NULL) {
        return MG_PARSE_ERROR;
    }
    if (strcmp(mode, "100644") != 0 || strcmp(type, "blob") != 0 || !hash_is_valid_hex(hash)) {
        return MG_PARSE_ERROR;
    }
    if (parse_size(size_text, &size) != MG_OK) {
        return MG_PARSE_ERROR;
    }

    return append_entry(tree, path, hash, size);
}

MGResult tree_from_index(const Index *index, Tree *tree) {
    MGResult result;

    if (index == NULL || tree == NULL) {
        return MG_INVALID_ARG;
    }

    /*
     * Removed files are absent from the index, so copying current index
     * entries directly produces the staged snapshot.
     */
    tree_init(tree);
    for (size_t i = 0; i < index->count; i++) {
        result = append_entry(tree, index->entries[i].path, index->entries[i].hash, index->entries[i].size);
        if (result != MG_OK) {
            tree_free(tree);
            return result;
        }
    }

    tree_sort(tree);
    return MG_OK;
}

MGResult tree_write(const Repository *repo, const Tree *tree, char out_hash[MG_HASH_HEX_SIZE]) {
    unsigned char *payload;
    size_t total_size = 0;
    size_t offset = 0;
    MGResult result;

    if (repo == NULL || tree == NULL || out_hash == NULL) {
        return MG_INVALID_ARG;
    }

    /* First pass computes the exact canonical payload size. */
    for (size_t i = 0; i < tree->count; i++) {
        int line_size = snprintf(
            NULL,
            0,
            "100644 blob %s %zu\t%s\n",
            tree->entries[i].hash,
            tree->entries[i].size,
            tree->entries[i].path
        );
        if (line_size < 0) {
            return MG_ERROR;
        }
        total_size += (size_t)line_size;
    }

    payload = malloc(total_size + 1);
    if (payload == NULL) {
        return MG_ERROR;
    }

    /* Second pass writes the canonical lines into one contiguous payload. */
    for (size_t i = 0; i < tree->count; i++) {
        int written = snprintf(
            (char *)payload + offset,
            total_size + 1 - offset,
            "100644 blob %s %zu\t%s\n",
            tree->entries[i].hash,
            tree->entries[i].size,
            tree->entries[i].path
        );
        if (written < 0) {
            free(payload);
            return MG_ERROR;
        }
        offset += (size_t)written;
    }

    payload[total_size] = '\0';
    result = object_write(repo, "tree", payload, total_size, out_hash);
    free(payload);
    return result;
}

MGResult tree_read(const Repository *repo, const char *hash, Tree *tree) {
    Object object;
    char *cursor;
    MGResult result;

    if (repo == NULL || tree == NULL || !hash_is_valid_hex(hash)) {
        return MG_INVALID_ARG;
    }

    /* object_read verifies the outer "<type> <size>\0" object wrapper. */
    tree_init(tree);
    result = object_read(repo, hash, &object);
    if (result != MG_OK) {
        return result;
    }
    if (strcmp(object.type, "tree") != 0) {
        object_free(&object);
        return MG_PARSE_ERROR;
    }
    if (object.size == 0) {
        object_free(&object);
        return MG_OK;
    }
    /*
     * Non-empty tree payloads must end in newline so every entry is complete.
     */
    if (object.payload[object.size - 1] != '\n') {
        object_free(&object);
        return MG_PARSE_ERROR;
    }

    /*
     * Object payloads are allocated with a spare NUL byte, so the parser can
     * temporarily replace newlines while walking lines.
     */
    cursor = (char *)object.payload;
    while (*cursor != '\0') {
        char *line_end = strchr(cursor, '\n');
        if (line_end == NULL) {
            result = MG_PARSE_ERROR;
            break;
        }
        *line_end = '\0';
        result = parse_tree_line(tree, cursor);
        if (result != MG_OK) {
            break;
        }
        cursor = line_end + 1;
    }

    object_free(&object);
    if (result != MG_OK) {
        tree_free(tree);
        return result;
    }
    tree_sort(tree);
    return MG_OK;
}

void tree_free(Tree *tree) {
    if (tree == NULL) {
        return;
    }

    free(tree->entries);
    tree->entries = NULL;
    tree->count = 0;
    tree->capacity = 0;
}
