#ifndef MINIGIT_TREE_H
#define MINIGIT_TREE_H

#include <stddef.h>

#include "common.h"
#include "index.h"
#include "repository.h"

/*
 * TreeEntry stores one tracked file entry inside a flat tree object.
 */
typedef struct {
    char path[MG_MAX_PATH];
    char hash[MG_HASH_HEX_SIZE];
    unsigned int mode;
    size_t size;
} TreeEntry;

/*
 * Tree owns the list of entries parsed from or built for a tree object.
 */
typedef struct {
    TreeEntry *entries;
    size_t count;
    size_t capacity;
} Tree;

/*
 * tree_from_index builds a sorted tree snapshot from the staging index.
 */
MGResult tree_from_index(const Index *index, Tree *tree);

/*
 * tree_write writes a tree object and returns its object hash.
 */
MGResult tree_write(const Repository *repo, const Tree *tree, char out_hash[MG_HASH_HEX_SIZE]);

/*
 * tree_read reads and parses a tree object by hash.
 */
MGResult tree_read(const Repository *repo, const char *hash, Tree *tree);

/*
 * tree_find_entry returns the entry for path or NULL.
 */
const TreeEntry *tree_find_entry(const Tree *tree, const char *path);

/*
 * tree_free releases memory owned by tree.
 */
void tree_free(Tree *tree);

#endif
