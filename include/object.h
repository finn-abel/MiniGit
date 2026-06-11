#ifndef MINIGIT_OBJECT_H
#define MINIGIT_OBJECT_H

#include <stddef.h>

#include "common.h"
#include "repository.h"

/*
 * Object stores a parsed MiniGit object payload and metadata.
 */
typedef struct {
    char type[16];
    size_t size;
    unsigned char *payload;
} Object;

/*
 * object_write writes canonical object bytes to the object database.
 */
MGResult object_write(
    const Repository *repo,
    const char *type,
    const unsigned char *payload,
    size_t size,
    char out_hash[MG_HASH_HEX_SIZE]
);

/*
 * object_read reads and parses an object by full hash.
 */
MGResult object_read(const Repository *repo, const char *hash, Object *out_object);

/*
 * object_free releases payload memory held by an Object.
 */
void object_free(Object *object);

#endif
