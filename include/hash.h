#ifndef MINIGIT_HASH_H
#define MINIGIT_HASH_H

#include <stddef.h>

#include "common.h"

/*
 * hash_bytes computes the SHA-256 digest for a byte buffer as lowercase hex.
 */
MGResult hash_bytes(const unsigned char *data, size_t size, char out_hex[MG_HASH_HEX_SIZE]);

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
