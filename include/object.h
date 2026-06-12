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
 * object_resolve_prefix expands a full hash or unique hex prefix to an object id.
 */
MGResult object_resolve_prefix(const Repository *repo, const char *prefix, char out_hash[MG_HASH_HEX_SIZE]);

/*
 * object_pack_all writes a simple MiniGit packfile containing loose objects.
 */
MGResult object_pack_all(const Repository *repo, size_t *out_count);

/*
 * object_free releases payload memory held by an Object.
 */
void object_free(Object *object);

#endif
