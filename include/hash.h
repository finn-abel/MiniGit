#ifndef MINIGIT_HASH_H
#define MINIGIT_HASH_H

#include <stddef.h>

#include "common.h"

typedef enum {
    HASH_MINIGIT_SHA256,
    HASH_GIT_SHA1
} HashMode;

/*
 * hash_bytes computes the SHA-256 digest for a byte buffer as lowercase hex.
 */
MGResult hash_bytes(const unsigned char *data, size_t size, char out_hex[MG_HASH_HEX_SIZE]);

/*
 * hash_bytes_with_mode computes either MiniGit's SHA-256 or Git's SHA-1 digest.
 */
MGResult hash_bytes_with_mode(
    const unsigned char *data,
    size_t size,
    HashMode mode,
    char out_hex[MG_HASH_HEX_SIZE]
);

/*
 * hash_object_bytes hashes canonical Git object bytes for type/payload.
 */
MGResult hash_object_bytes(
    const char *type,
    const unsigned char *payload,
    size_t size,
    HashMode mode,
    char out_hex[MG_HASH_HEX_SIZE]
);

/*
 * hash_default_object_mode returns the object-id mode selected by the environment.
 */
HashMode hash_default_object_mode(void);

/*
 * hash_file computes the SHA-256 digest for a file's raw bytes.
 */
MGResult hash_file(const char *path, char out_hex[MG_HASH_HEX_SIZE]);

/*
 * hash_to_hex converts a 32-byte SHA-256 digest to lowercase hex.
 */
void hash_to_hex(const unsigned char digest[32], char out_hex[MG_HASH_HEX_SIZE]);

/*
 * hash_is_valid_hex reports whether hash is a 64-character lowercase/uppercase hex digest.
 */
int hash_is_valid_hex(const char *hash);

#endif
