#include "hash.h"

#include <ctype.h>
#include <openssl/evp.h>
#include <stdlib.h>
#include <string.h>

#include "fs.h"

#define SHA256_DIGEST_SIZE 32

/*
 * hash_to_hex converts digest bytes to the storage format used by MiniGit.
 */
void hash_to_hex(const unsigned char digest[SHA256_DIGEST_SIZE], char out_hex[MG_HASH_HEX_SIZE]) {
    static const char hex[] = "0123456789abcdef";

    for (size_t i = 0; i < SHA256_DIGEST_SIZE; i++) {
        out_hex[i * 2] = hex[digest[i] >> 4];
        out_hex[i * 2 + 1] = hex[digest[i] & 0x0f];
    }
    out_hex[MG_HASH_HEX_SIZE - 1] = '\0';
}

/*
 * hash_bytes computes SHA-256 using OpenSSL EVP APIs.
 */
MGResult hash_bytes(const unsigned char *data, size_t size, char out_hex[MG_HASH_HEX_SIZE]) {
    EVP_MD_CTX *ctx;
    unsigned char digest[SHA256_DIGEST_SIZE];
    unsigned int digest_size = 0;

    if ((data == NULL && size > 0) || out_hex == NULL) {
        return MG_INVALID_ARG;
    }

    ctx = EVP_MD_CTX_new();
    if (ctx == NULL) {
        return MG_ERROR;
    }

    if (EVP_DigestInit_ex(ctx, EVP_sha256(), NULL) != 1 ||
        EVP_DigestUpdate(ctx, data, size) != 1 ||
        EVP_DigestFinal_ex(ctx, digest, &digest_size) != 1) {
        EVP_MD_CTX_free(ctx);
        return MG_ERROR;
    }
    EVP_MD_CTX_free(ctx);

    if (digest_size != SHA256_DIGEST_SIZE) {
        return MG_ERROR;
    }

    hash_to_hex(digest, out_hex);
    return MG_OK;
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
    if (hash == NULL || strlen(hash) != MG_HASH_HEX_SIZE - 1) {
        return 0;
    }

    for (size_t i = 0; i < MG_HASH_HEX_SIZE - 1; i++) {
        if (!isxdigit((unsigned char)hash[i])) {
            return 0;
        }
    }

    return 1;
}
