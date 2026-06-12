#include "hash.h"

#include <ctype.h>
#include <openssl/evp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fs.h"

#define SHA256_DIGEST_SIZE 32
#define SHA1_DIGEST_SIZE 20

static void hash_digest_to_hex(const unsigned char *digest, size_t digest_size, char out_hex[MG_HASH_HEX_SIZE]);

/*
 * hash_to_hex converts digest bytes to the storage format used by MiniGit.
 */
void hash_to_hex(const unsigned char digest[SHA256_DIGEST_SIZE], char out_hex[MG_HASH_HEX_SIZE]) {
    hash_digest_to_hex(digest, SHA256_DIGEST_SIZE, out_hex);
}

static void hash_digest_to_hex(const unsigned char *digest, size_t digest_size, char out_hex[MG_HASH_HEX_SIZE]) {
    static const char hex[] = "0123456789abcdef";

    for (size_t i = 0; i < digest_size; i++) {
        out_hex[i * 2] = hex[digest[i] >> 4];
        out_hex[i * 2 + 1] = hex[digest[i] & 0x0f];
    }
    out_hex[digest_size * 2] = '\0';
}

/*
 * hash_bytes computes SHA-256 using OpenSSL EVP APIs.
 */
MGResult hash_bytes(const unsigned char *data, size_t size, char out_hex[MG_HASH_HEX_SIZE]) {
    return hash_bytes_with_mode(data, size, HASH_MINIGIT_SHA256, out_hex);
}

MGResult hash_bytes_with_mode(
    const unsigned char *data,
    size_t size,
    HashMode mode,
    char out_hex[MG_HASH_HEX_SIZE]
) {
    EVP_MD_CTX *ctx;
    unsigned char digest[SHA256_DIGEST_SIZE];
    unsigned int digest_size = 0;
    const EVP_MD *digest_type;
    unsigned int expected_size;

    if ((data == NULL && size > 0) || out_hex == NULL) {
        return MG_INVALID_ARG;
    }
    if (mode == HASH_MINIGIT_SHA256) {
        digest_type = EVP_sha256();
        expected_size = SHA256_DIGEST_SIZE;
    } else if (mode == HASH_GIT_SHA1) {
        digest_type = EVP_sha1();
        expected_size = SHA1_DIGEST_SIZE;
    } else {
        return MG_INVALID_ARG;
    }

    ctx = EVP_MD_CTX_new();
    if (ctx == NULL) {
        return MG_ERROR;
    }

    if (EVP_DigestInit_ex(ctx, digest_type, NULL) != 1 ||
        EVP_DigestUpdate(ctx, data, size) != 1 ||
        EVP_DigestFinal_ex(ctx, digest, &digest_size) != 1) {
        EVP_MD_CTX_free(ctx);
        return MG_ERROR;
    }
    EVP_MD_CTX_free(ctx);

    if (digest_size != expected_size) {
        return MG_ERROR;
    }

    hash_digest_to_hex(digest, digest_size, out_hex);
    return MG_OK;
}

MGResult hash_object_bytes(
    const char *type,
    const unsigned char *payload,
    size_t size,
    HashMode mode,
    char out_hex[MG_HASH_HEX_SIZE]
) {
    int header_size;
    size_t object_size;
    unsigned char *object_data;
    MGResult result;

    if (type == NULL || type[0] == '\0' || (payload == NULL && size > 0) || out_hex == NULL) {
        return MG_INVALID_ARG;
    }

    header_size = snprintf(NULL, 0, "%s %zu", type, size);
    if (header_size < 0) {
        return MG_ERROR;
    }
    object_size = (size_t)header_size + 1 + size;
    object_data = malloc(object_size);
    if (object_data == NULL) {
        return MG_ERROR;
    }
    snprintf((char *)object_data, (size_t)header_size + 1, "%s %zu", type, size);
    object_data[header_size] = '\0';
    if (size > 0) {
        memcpy(object_data + header_size + 1, payload, size);
    }

    result = hash_bytes_with_mode(object_data, object_size, mode, out_hex);
    free(object_data);
    return result;
}

HashMode hash_default_object_mode(void) {
    const char *value = getenv("MINIGIT_OBJECT_HASH_MODE");
    if (value != NULL && (strcmp(value, "git") == 0 || strcmp(value, "sha1") == 0)) {
        return HASH_GIT_SHA1;
    }
    return HASH_MINIGIT_SHA256;
}

/*
 * hash_file reads a file as bytes and hashes the exact contents.
 */
MGResult hash_file(const char *path, char out_hex[MG_HASH_HEX_SIZE]) {
    unsigned char *data = NULL;
    size_t size = 0;
    MGResult result;

    if (path == NULL || out_hex == NULL) {
        return MG_INVALID_ARG;
    }

    result = fs_read_file(path, &data, &size);
    if (result != MG_OK) {
        return result;
    }

    result = hash_bytes(data, size, out_hex);
    free(data);
    return result;
}

/*
 * hash_is_valid_hex validates the textual digest shape accepted by object lookup.
 */
int hash_is_valid_hex(const char *hash) {
    if (hash == NULL || (strlen(hash) != MG_HASH_HEX_SIZE - 1 && strlen(hash) != MG_GIT_HASH_HEX_SIZE - 1)) {
        return 0;
    }

    for (size_t i = 0; hash[i] != '\0'; i++) {
        if (!isxdigit((unsigned char)hash[i])) {
            return 0;
        }
    }

    return 1;
}
