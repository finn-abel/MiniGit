#define _POSIX_C_SOURCE 200809L

#include "index.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fs.h"
#include "hash.h"

/*
 * index_path builds the absolute path to .minigit/index.
 */
static MGResult index_path(const Repository *repo, char *out, size_t out_size) {
    if (repo == NULL) {
        return MG_INVALID_ARG;
    }
    return fs_join_path(repo->gitdir_path, "index", out, out_size);
}

/*
 * compare_entries orders index entries by repo-relative path.
 */
static int compare_entries(const void *left, const void *right) {
    const IndexEntry *a = left;
    const IndexEntry *b = right;
    return strcmp(a->path, b->path);
}

/*
 * index_sort keeps the on-disk index deterministic.
 */
static void index_sort(Index *index) {
    if (index != NULL && index->count > 1) {
        qsort(index->entries, index->count, sizeof(index->entries[0]), compare_entries);
    }
}

/*
 * ensure_capacity grows the entry array when needed.
 * The index keeps entries in one contiguous allocation so sorting and saving
 * can work directly on the stored array.
 */
static MGResult ensure_capacity(Index *index, size_t needed) {
    IndexEntry *grown;
    size_t capacity;

    if (index == NULL) {
        return MG_INVALID_ARG;
    }
    if (needed <= index->capacity) {
        return MG_OK;
    }

    capacity = index->capacity == 0 ? 8 : index->capacity;
    while (capacity < needed) {
        capacity *= 2;
    }

    grown = realloc(index->entries, capacity * sizeof(index->entries[0]));
    if (grown == NULL) {
        return MG_ERROR;
    }

    index->entries = grown;
    index->capacity = capacity;
    return MG_OK;
}

/*
 * parse_size_field parses a non-empty decimal size.
 * Sizes come from the index file, so partial parses such as "12abc" are
 * rejected instead of silently truncating.
 */
static MGResult parse_size_field(const char *value, size_t *out) {
    char *end = NULL;
    unsigned long parsed;

    if (value == NULL || value[0] == '\0' || out == NULL) {
        return MG_PARSE_ERROR;
    }

    errno = 0;
    parsed = strtoul(value, &end, 10);
    if (errno != 0 || *end != '\0') {
        return MG_PARSE_ERROR;
    }

    *out = (size_t)parsed;
    return MG_OK;
}

/*
 * parse_mtime_field parses a non-empty decimal modification time.
 * Negative mtimes are rejected because the on-disk format stores plain
 * non-negative Unix timestamps.
 */
static MGResult parse_mtime_field(const char *value, time_t *out) {
    char *end = NULL;
    long parsed;

    if (value == NULL || value[0] == '\0' || out == NULL) {
        return MG_PARSE_ERROR;
    }

    errno = 0;
    parsed = strtol(value, &end, 10);
    if (errno != 0 || *end != '\0' || parsed < 0) {
        return MG_PARSE_ERROR;
    }

    *out = (time_t)parsed;
    return MG_OK;
}

/*
 * parse_index_line validates and loads one tab-separated index line.
 * The path field is last so paths may contain spaces, but not tabs/newlines.
 */
static MGResult parse_index_line(Index *index, char *line) {
    char *mode;
    char *hash;
    char *size_text;
    char *mtime_text;
    char *path;
    char *extra;
    size_t size;
    time_t mtime;

    /* strtok is safe here because index_load gives us one mutable line at a time. */
    mode = strtok(line, "\t");
    hash = strtok(NULL, "\t");
    size_text = strtok(NULL, "\t");
    mtime_text = strtok(NULL, "\t");
    path = strtok(NULL, "\t");
    extra = strtok(NULL, "\t");

    if (mode == NULL || hash == NULL || size_text == NULL || mtime_text == NULL ||
        path == NULL || extra != NULL) {
        return MG_PARSE_ERROR;
    }
    if (strcmp(mode, "100644") != 0 || !hash_is_valid_hex(hash)) {
        return MG_PARSE_ERROR;
    }
    if (path[0] == '\0' || strchr(path, '\n') != NULL) {
        return MG_PARSE_ERROR;
    }
    if (parse_size_field(size_text, &size) != MG_OK || parse_mtime_field(mtime_text, &mtime) != MG_OK) {
        return MG_PARSE_ERROR;
    }

    return index_add_or_update(index, path, hash, size, mtime);
}

/*
 * append_entry appends an already validated entry.
 * Sorting is intentionally handled by the public update path after mutation.
 */
static MGResult append_entry(Index *index, const IndexEntry *entry) {
    MGResult result = ensure_capacity(index, index->count + 1);
    if (result != MG_OK) {
        return result;
    }
    index->entries[index->count] = *entry;
    index->count++;
    return MG_OK;
}

void index_init(Index *index) {
    if (index == NULL) {
        return;
    }
    index->entries = NULL;
    index->count = 0;
    index->capacity = 0;
}

/*
 * index_load accepts an absent index as empty, which keeps early repository
 * recovery and tests simple while repo_init still creates the file normally.
 */
MGResult index_load(const Repository *repo, Index *index) {
    char path[MG_MAX_PATH];
    unsigned char *data = NULL;
    size_t size = 0;
    char *cursor;
    MGResult result;

    if (index == NULL) {
        return MG_INVALID_ARG;
    }
    index_init(index);

    if (index_path(repo, path, sizeof(path)) != MG_OK) {
        return MG_INVALID_ARG;
    }
    if (!fs_exists(path)) {
        return MG_OK;
    }

    result = fs_read_file(path, &data, &size);
    if (result != MG_OK) {
        return result;
    }
    if (size == 0) {
        free(data);
        return MG_OK;
    }
    /*
     * Require a trailing newline so every non-empty index is a sequence of
     * complete records and malformed partial writes are not accepted.
     */
    if (data[size - 1] != '\n') {
        free(data);
        return MG_PARSE_ERROR;
    }

    /*
     * fs_read_file adds a spare NUL byte, letting this loop treat the binary
     * buffer as line-oriented text after the newline check.
     */
    cursor = (char *)data;
    while (*cursor != '\0') {
        char *line_end = strchr(cursor, '\n');
        if (line_end == NULL) {
            result = MG_PARSE_ERROR;
            break;
        }
        *line_end = '\0';
        result = parse_index_line(index, cursor);
        if (result != MG_OK) {
            break;
        }
        cursor = line_end + 1;
    }

    free(data);
    if (result != MG_OK) {
        index_free(index);
        return result;
    }
    index_sort(index);
    return MG_OK;
}

/*
 * index_save serializes the full index into memory before writing so the
 * on-disk file is generated from one canonical formatter.
 */
MGResult index_save(const Repository *repo, const Index *index) {
    char path[MG_MAX_PATH];
    size_t total_size = 0;
    unsigned char *data;
    size_t offset = 0;

    if (index == NULL) {
        return MG_INVALID_ARG;
    }
    if (index_path(repo, path, sizeof(path)) != MG_OK) {
        return MG_INVALID_ARG;
    }

    /* First pass calculates the exact byte count needed for all lines. */
    for (size_t i = 0; i < index->count; i++) {
        int line_size = snprintf(
            NULL,
            0,
            "100644\t%s\t%zu\t%ld\t%s\n",
            index->entries[i].hash,
            index->entries[i].size,
            (long)index->entries[i].mtime,
            index->entries[i].path
        );
        if (line_size < 0) {
            return MG_ERROR;
        }
        total_size += (size_t)line_size;
    }

    data = malloc(total_size + 1);
    if (data == NULL) {
        return MG_ERROR;
    }

    /* Second pass writes the same canonical lines into the allocated buffer. */
    for (size_t i = 0; i < index->count; i++) {
        int written = snprintf(
            (char *)data + offset,
            total_size + 1 - offset,
            "100644\t%s\t%zu\t%ld\t%s\n",
            index->entries[i].hash,
            index->entries[i].size,
            (long)index->entries[i].mtime,
            index->entries[i].path
        );
        if (written < 0) {
            free(data);
            return MG_ERROR;
        }
        offset += (size_t)written;
    }

    data[total_size] = '\0';
    MGResult result = fs_write_file(path, data, total_size);
    free(data);
    return result;
}

/*
 * index_add_or_update is the only mutation path for staged file metadata.
 * Re-adding an existing path replaces the whole entry instead of duplicating it.
 */
MGResult index_add_or_update(Index *index, const char *path, const char *hash, size_t size, time_t mtime) {
    IndexEntry *existing;
    IndexEntry entry;

    if (index == NULL || path == NULL || hash == NULL || path[0] == '\0' ||
        strlen(path) >= sizeof(entry.path) || !hash_is_valid_hex(hash)) {
        return MG_INVALID_ARG;
    }

    /* Build a complete entry first so update and insert share validation. */
    existing = index_find(index, path);
    memset(&entry, 0, sizeof(entry));
    strcpy(entry.path, path);
    strcpy(entry.hash, hash);
    entry.size = size;
    entry.mtime = mtime;

    if (existing != NULL) {
        *existing = entry;
    } else {
        MGResult result = append_entry(index, &entry);
        if (result != MG_OK) {
            return result;
        }
    }

    index_sort(index);
    return MG_OK;
}

/*
 * index_remove compacts the array in place after deleting a path.
 */
MGResult index_remove(Index *index, const char *path) {
    if (index == NULL || path == NULL) {
        return MG_INVALID_ARG;
    }

    for (size_t i = 0; i < index->count; i++) {
        if (strcmp(index->entries[i].path, path) == 0) {
            if (i + 1 < index->count) {
                memmove(
                    &index->entries[i],
                    &index->entries[i + 1],
                    (index->count - i - 1) * sizeof(index->entries[0])
                );
            }
            index->count--;
            return MG_OK;
        }
    }

    return MG_NOT_FOUND;
}

/*
 * index_find performs a linear lookup; the index is small in v1 and this keeps
 * ownership simple until larger status/checkout behavior exists.
 */
IndexEntry *index_find(Index *index, const char *path) {
    if (index == NULL || path == NULL) {
        return NULL;
    }

    for (size_t i = 0; i < index->count; i++) {
        if (strcmp(index->entries[i].path, path) == 0) {
            return &index->entries[i];
        }
    }

    return NULL;
}

/*
 * index_free leaves the struct reusable after releasing its allocation.
 */
void index_free(Index *index) {
    if (index == NULL) {
        return;
    }

    free(index->entries);
    index->entries = NULL;
    index->count = 0;
    index->capacity = 0;
}
