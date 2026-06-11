#ifndef MINIGIT_INDEX_H
#define MINIGIT_INDEX_H

#include <stddef.h>
#include <time.h>

#include "common.h"
#include "repository.h"

/*
 * IndexEntry stores one staged path and its blob metadata.
 */
typedef struct {
    char path[MG_MAX_PATH];
    char hash[MG_HASH_HEX_SIZE];
    size_t size;
    time_t mtime;
} IndexEntry;

/*
 * Index owns the in-memory list of staged entries.
 */
typedef struct {
    IndexEntry *entries;
    size_t count;
    size_t capacity;
} Index;

/*
 * index_init prepares an empty index.
 */
void index_init(Index *index);

/*
 * index_load reads and parses .minigit/index.
 */
MGResult index_load(const Repository *repo, Index *index);

/*
 * index_save writes .minigit/index using the canonical tab-separated format.
 */
MGResult index_save(const Repository *repo, const Index *index);

/*
 * index_add_or_update inserts or replaces an entry for path.
 */
MGResult index_add_or_update(Index *index, const char *path, const char *hash, size_t size, time_t mtime);

/*
 * index_remove deletes the entry for path if it exists.
 */
MGResult index_remove(Index *index, const char *path);

/*
 * index_find returns the entry for path or NULL.
 */
IndexEntry *index_find(Index *index, const char *path);

/*
 * index_free releases memory owned by index.
 */
void index_free(Index *index);

#endif
