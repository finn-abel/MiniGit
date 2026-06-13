#include "object.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <zlib.h>

#include "fs.h"
#include "hash.h"

#define MG_LOOSE_ZLIB_PREFIX "MGZ1 "
#define MG_PACK_HEADER "MGPACK1\n"
#define MG_MAX_OBJECT_SIZE ((size_t)64 * 1024 * 1024)
#define MG_MAX_STORED_OBJECT_SIZE ((size_t)128 * 1024 * 1024)
#define MG_MAX_PACK_SIZE ((size_t)512 * 1024 * 1024)

static MGResult parse_size(const char *text, size_t *out_size);

typedef struct {
    unsigned char *data;
    size_t size;
    size_t capacity;
} PackBuffer;

static int checked_add_size(size_t left, size_t right, size_t *out) {
    if (left > SIZE_MAX - right) {
        return 0;
    }
    *out = left + right;
    return 1;
}

static MGResult read_file_limited(const char *path, size_t limit, unsigned char **out_data, size_t *out_size) {
    struct stat st;

    if (path == NULL || out_data == NULL || out_size == NULL) {
        return MG_INVALID_ARG;
    }
    if (lstat(path, &st) != 0 || !S_ISREG(st.st_mode)) {
        return MG_IO_ERROR;
    }
    if (st.st_size < 0 || (uintmax_t)st.st_size > limit) {
        return MG_PARSE_ERROR;
    }
    return fs_read_file(path, out_data, out_size);
}

/*
 * object_path calculates .minigit/objects/xx/yyyy... for a full object hash.
 */
static MGResult object_path(const Repository *repo, const char *hash, char *out, size_t out_size) {
    char dir_name[3];
    const char *file_name;
    char objects_path[MG_MAX_PATH];
    char object_dir[MG_MAX_PATH];

    if (repo == NULL || !hash_is_valid_hex(hash)) {
        return MG_INVALID_ARG;
    }

    dir_name[0] = hash[0];
    dir_name[1] = hash[1];
    dir_name[2] = '\0';
    file_name = hash + 2;

    if (fs_join_path(repo->gitdir_path, "objects", objects_path, sizeof(objects_path)) != MG_OK ||
        fs_join_path(objects_path, dir_name, object_dir, sizeof(object_dir)) != MG_OK ||
        fs_join_path(object_dir, file_name, out, out_size) != MG_OK) {
        return MG_INVALID_ARG;
    }

    return MG_OK;
}

/*
 * object_dir_path calculates the fanout directory for a full object hash.
 */
static MGResult object_dir_path(const Repository *repo, const char *hash, char *out, size_t out_size) {
    char dir_name[3];
    char objects_path[MG_MAX_PATH];

    if (repo == NULL || !hash_is_valid_hex(hash)) {
        return MG_INVALID_ARG;
    }

    dir_name[0] = hash[0];
    dir_name[1] = hash[1];
    dir_name[2] = '\0';

    if (fs_join_path(repo->gitdir_path, "objects", objects_path, sizeof(objects_path)) != MG_OK ||
        fs_join_path(objects_path, dir_name, out, out_size) != MG_OK) {
        return MG_INVALID_ARG;
    }

    return MG_OK;
}

/*
 * object_pack_path calculates the v1 single-pack path.
 */
static MGResult object_pack_path(const Repository *repo, char *out, size_t out_size) {
    char objects_path[MG_MAX_PATH];
    char pack_dir[MG_MAX_PATH];

    if (repo == NULL || out == NULL) {
        return MG_INVALID_ARG;
    }
    if (fs_join_path(repo->gitdir_path, "objects", objects_path, sizeof(objects_path)) != MG_OK ||
        fs_join_path(objects_path, "pack", pack_dir, sizeof(pack_dir)) != MG_OK ||
        fs_join_path(pack_dir, "minigit.pack", out, out_size) != MG_OK) {
        return MG_INVALID_ARG;
    }
    return MG_OK;
}

/*
 * object_pack_dir_path calculates .minigit/objects/pack.
 */
static MGResult object_pack_dir_path(const Repository *repo, char *out, size_t out_size) {
    char objects_path[MG_MAX_PATH];

    if (repo == NULL || out == NULL) {
        return MG_INVALID_ARG;
    }
    if (fs_join_path(repo->gitdir_path, "objects", objects_path, sizeof(objects_path)) != MG_OK ||
        fs_join_path(objects_path, "pack", out, out_size) != MG_OK) {
        return MG_INVALID_ARG;
    }
    return MG_OK;
}

/*
 * is_hex_prefix accepts the abbreviated object id syntax used by checkout.
 */
static int is_hex_prefix(const char *prefix) {
    size_t len;

    if (prefix == NULL) {
        return 0;
    }

    len = strlen(prefix);
    if (len < 7 || len >= MG_HASH_HEX_SIZE) {
        return 0;
    }

    for (size_t i = 0; i < len; i++) {
        char ch = prefix[i];
        if (!((ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f') || (ch >= 'A' && ch <= 'F'))) {
            return 0;
        }
    }

    return 1;
}

/*
 * is_object_fanout_dir checks the two-character directory part of an object id.
 */
static int is_object_fanout_dir(const char *name) {
    if (name == NULL || strlen(name) != 2) {
        return 0;
    }
    return ((name[0] >= '0' && name[0] <= '9') || (name[0] >= 'a' && name[0] <= 'f')) &&
           ((name[1] >= '0' && name[1] <= '9') || (name[1] >= 'a' && name[1] <= 'f'));
}

/*
 * is_object_file_name checks the 62-character file part of an object id.
 */
static int is_object_file_name(const char *name) {
    if (name == NULL || (strlen(name) != MG_HASH_HEX_SIZE - 3 && strlen(name) != MG_GIT_HASH_HEX_SIZE - 3)) {
        return 0;
    }
    for (size_t i = 0; name[i] != '\0'; i++) {
        if (!((name[i] >= '0' && name[i] <= '9') || (name[i] >= 'a' && name[i] <= 'f'))) {
            return 0;
        }
    }
    return 1;
}

/*
 * build_object_bytes creates the canonical <type> <size>\0<payload> byte buffer.
 */
static MGResult build_object_bytes(
    const char *type,
    const unsigned char *payload,
    size_t size,
    unsigned char **out_data,
    size_t *out_size
) {
    int header_size;
    size_t total_size;
    unsigned char *data;

    if (type == NULL || type[0] == '\0' || (payload == NULL && size > 0) ||
        out_data == NULL || out_size == NULL) {
        return MG_INVALID_ARG;
    }

    header_size = snprintf(NULL, 0, "%s %zu", type, size);
    if (header_size < 0) {
        return MG_ERROR;
    }

    if (size > MG_MAX_OBJECT_SIZE ||
        !checked_add_size((size_t)header_size, 1, &total_size) ||
        !checked_add_size(total_size, size, &total_size)) {
        return MG_INVALID_ARG;
    }
    data = malloc(total_size);
    if (data == NULL) {
        return MG_ERROR;
    }

    snprintf((char *)data, (size_t)header_size + 1, "%s %zu", type, size);
    data[header_size] = '\0';
    if (size > 0) {
        memcpy(data + header_size + 1, payload, size);
    }

    *out_data = data;
    *out_size = total_size;
    return MG_OK;
}

/*
 * copy_bytes returns an owned copy of a byte buffer with a spare NUL byte.
 */
static MGResult copy_bytes(const unsigned char *data, size_t size, unsigned char **out_data, size_t *out_size) {
    unsigned char *copy;

    if ((data == NULL && size > 0) || out_data == NULL || out_size == NULL) {
        return MG_INVALID_ARG;
    }

    if (size > MG_MAX_OBJECT_SIZE || size == SIZE_MAX) {
        return MG_PARSE_ERROR;
    }
    copy = malloc(size + 1);
    if (copy == NULL) {
        return MG_ERROR;
    }
    if (size > 0) {
        memcpy(copy, data, size);
    }
    copy[size] = '\0';
    *out_data = copy;
    *out_size = size;
    return MG_OK;
}

/*
 * compress_bytes wraps zlib compression for canonical object bytes.
 */
static MGResult compress_bytes(const unsigned char *data, size_t size, unsigned char **out_data, size_t *out_size) {
    uLongf compressed_size;
    unsigned char *compressed;

    if ((data == NULL && size > 0) || out_data == NULL || out_size == NULL) {
        return MG_INVALID_ARG;
    }

    if (size > MG_MAX_OBJECT_SIZE || size > ULONG_MAX) {
        return MG_INVALID_ARG;
    }
    compressed_size = compressBound((uLong)size);
    compressed = malloc((size_t)compressed_size);
    if (compressed == NULL) {
        return MG_ERROR;
    }
    if (compress2(compressed, &compressed_size, data, (uLong)size, Z_BEST_SPEED) != Z_OK) {
        free(compressed);
        return MG_ERROR;
    }

    *out_data = compressed;
    *out_size = (size_t)compressed_size;
    return MG_OK;
}

/*
 * decompress_bytes inflates a zlib buffer to a known canonical object size.
 */
static MGResult decompress_bytes(
    const unsigned char *data,
    size_t size,
    size_t uncompressed_size,
    unsigned char **out_data,
    size_t *out_size
) {
    unsigned char *uncompressed;
    uLongf actual_size = (uLongf)uncompressed_size;

    if ((data == NULL && size > 0) || out_data == NULL || out_size == NULL) {
        return MG_INVALID_ARG;
    }

    if (uncompressed_size > MG_MAX_OBJECT_SIZE || uncompressed_size == SIZE_MAX || size > ULONG_MAX) {
        return MG_PARSE_ERROR;
    }
    uncompressed = malloc(uncompressed_size + 1);
    if (uncompressed == NULL) {
        return MG_ERROR;
    }
    if (uncompress(uncompressed, &actual_size, data, (uLong)size) != Z_OK ||
        (size_t)actual_size != uncompressed_size) {
        free(uncompressed);
        return MG_PARSE_ERROR;
    }

    uncompressed[uncompressed_size] = '\0';
    *out_data = uncompressed;
    *out_size = uncompressed_size;
    return MG_OK;
}

/*
 * parse_decimal_field parses a bounded decimal field from binary storage.
 */
static MGResult parse_decimal_field(const unsigned char *start, const unsigned char *end, size_t *out_value) {
    char buffer[32];
    size_t len;

    if (start == NULL || end == NULL || start >= end || out_value == NULL) {
        return MG_PARSE_ERROR;
    }
    len = (size_t)(end - start);
    if (len >= sizeof(buffer)) {
        return MG_PARSE_ERROR;
    }
    memcpy(buffer, start, len);
    buffer[len] = '\0';
    return parse_size(buffer, out_value);
}

/*
 * decode_loose_object_bytes accepts both old raw loose objects and new zlib
 * loose objects with an "MGZ1 <size>\n" wrapper.
 */
static MGResult decode_loose_object_bytes(
    const unsigned char *data,
    size_t size,
    unsigned char **out_data,
    size_t *out_size
) {
    const unsigned char *newline;
    size_t uncompressed_size;

    if (data == NULL || out_data == NULL || out_size == NULL) {
        return MG_INVALID_ARG;
    }
    if (size < strlen(MG_LOOSE_ZLIB_PREFIX) ||
        memcmp(data, MG_LOOSE_ZLIB_PREFIX, strlen(MG_LOOSE_ZLIB_PREFIX)) != 0) {
        return copy_bytes(data, size, out_data, out_size);
    }

    newline = memchr(data, '\n', size);
    if (newline == NULL) {
        return MG_PARSE_ERROR;
    }
    if (parse_decimal_field(data + strlen(MG_LOOSE_ZLIB_PREFIX), newline, &uncompressed_size) != MG_OK) {
        return MG_PARSE_ERROR;
    }
    return decompress_bytes(
        newline + 1,
        size - (size_t)(newline + 1 - data),
        uncompressed_size,
        out_data,
        out_size
    );
}

/*
 * read_loose_object_bytes loads one loose object as canonical object bytes.
 */
static MGResult read_loose_object_bytes(
    const Repository *repo,
    const char *hash,
    unsigned char **out_data,
    size_t *out_size
) {
    char path[MG_MAX_PATH];
    unsigned char *stored_data = NULL;
    size_t stored_size = 0;
    MGResult result;

    if (object_path(repo, hash, path, sizeof(path)) != MG_OK) {
        return MG_INVALID_ARG;
    }
    if (!fs_is_file(path)) {
        return MG_NOT_FOUND;
    }

    result = read_file_limited(path, MG_MAX_STORED_OBJECT_SIZE, &stored_data, &stored_size);
    if (result != MG_OK) {
        return result;
    }

    result = decode_loose_object_bytes(stored_data, stored_size, out_data, out_size);
    free(stored_data);
    return result;
}

/*
 * build_loose_storage wraps compressed canonical object bytes for loose storage.
 */
static MGResult build_loose_storage(
    const unsigned char *object_data,
    size_t object_size,
    unsigned char **out_data,
    size_t *out_size
) {
    unsigned char *compressed = NULL;
    unsigned char *stored;
    size_t compressed_size = 0;
    int header_size;
    MGResult result;

    result = compress_bytes(object_data, object_size, &compressed, &compressed_size);
    if (result != MG_OK) {
        return result;
    }

    header_size = snprintf(NULL, 0, "%s%zu\n", MG_LOOSE_ZLIB_PREFIX, object_size);
    if (header_size < 0) {
        free(compressed);
        return MG_ERROR;
    }

    size_t stored_size;
    if (!checked_add_size((size_t)header_size, compressed_size, &stored_size)) {
        free(compressed);
        return MG_ERROR;
    }
    stored = malloc(stored_size);
    if (stored == NULL) {
        free(compressed);
        return MG_ERROR;
    }
    snprintf((char *)stored, (size_t)header_size + 1, "%s%zu\n", MG_LOOSE_ZLIB_PREFIX, object_size);
    memcpy(stored + header_size, compressed, compressed_size);
    free(compressed);

    *out_data = stored;
    *out_size = stored_size;
    return MG_OK;
}

/*
 * parse_size parses the decimal payload size in an object header.
 */
static MGResult parse_size(const char *text, size_t *out_size) {
    char *end = NULL;
    uintmax_t parsed;

    if (text == NULL || text[0] == '\0' || out_size == NULL) {
        return MG_PARSE_ERROR;
    }

    errno = 0;
    parsed = strtoumax(text, &end, 10);
    if (errno != 0 || *end != '\0' || parsed > SIZE_MAX) {
        return MG_PARSE_ERROR;
    }

    *out_size = (size_t)parsed;
    return MG_OK;
}

/*
 * parse_object converts canonical object bytes into an Object value.
 */
static MGResult parse_object(const unsigned char *data, size_t data_size, Object *out_object) {
    const unsigned char *nul;
    const char *space;
    size_t header_size;
    size_t type_size;
    size_t payload_size;

    if (data == NULL || out_object == NULL) {
        return MG_INVALID_ARG;
    }

    nul = memchr(data, '\0', data_size);
    if (nul == NULL) {
        return MG_PARSE_ERROR;
    }

    header_size = (size_t)(nul - data);
    space = memchr(data, ' ', header_size);
    if (space == NULL) {
        return MG_PARSE_ERROR;
    }

    type_size = (size_t)(space - (const char *)data);
    if (type_size == 0 || type_size >= sizeof(out_object->type)) {
        return MG_PARSE_ERROR;
    }
    memcpy(out_object->type, data, type_size);
    out_object->type[type_size] = '\0';

    if (parse_size(space + 1, &payload_size) != MG_OK) {
        return MG_PARSE_ERROR;
    }
    size_t expected_size;
    if (!checked_add_size(header_size, 1, &expected_size) ||
        !checked_add_size(expected_size, payload_size, &expected_size) ||
        expected_size != data_size || payload_size > MG_MAX_OBJECT_SIZE) {
        return MG_PARSE_ERROR;
    }

    out_object->payload = malloc(payload_size + 1);
    if (out_object->payload == NULL) {
        return MG_ERROR;
    }
    if (payload_size > 0) {
        memcpy(out_object->payload, nul + 1, payload_size);
    }
    out_object->payload[payload_size] = '\0';
    out_object->size = payload_size;

    return MG_OK;
}

/*
 * pack_buffer_append grows and appends bytes to an in-memory packfile.
 */
static MGResult pack_buffer_append(PackBuffer *buffer, const unsigned char *data, size_t size) {
    unsigned char *grown;
    size_t capacity;

    if (buffer == NULL || (data == NULL && size > 0)) {
        return MG_INVALID_ARG;
    }
    if (buffer->size + size < buffer->size || buffer->size + size > MG_MAX_PACK_SIZE) {
        return MG_ERROR;
    }
    if (buffer->size + size > buffer->capacity) {
        capacity = buffer->capacity == 0 ? 1024 : buffer->capacity;
        while (capacity < buffer->size + size) {
            if (capacity > SIZE_MAX / 2) {
                return MG_ERROR;
            }
            capacity *= 2;
        }
        grown = realloc(buffer->data, capacity);
        if (grown == NULL) {
            return MG_ERROR;
        }
        buffer->data = grown;
        buffer->capacity = capacity;
    }
    if (size > 0) {
        memcpy(buffer->data + buffer->size, data, size);
    }
    buffer->size += size;
    return MG_OK;
}

/*
 * parse_pack_line validates "<hash> <compressed-size> <uncompressed-size>".
 */
static MGResult parse_pack_line(char *line, char hash[MG_HASH_HEX_SIZE], size_t *compressed_size, size_t *object_size) {
    char *hash_text;
    char *compressed_text;
    char *object_text;
    char *extra;

    hash_text = strtok(line, " ");
    compressed_text = strtok(NULL, " ");
    object_text = strtok(NULL, " ");
    extra = strtok(NULL, " ");
    if (hash_text == NULL || compressed_text == NULL || object_text == NULL || extra != NULL ||
        !hash_is_valid_hex(hash_text)) {
        return MG_PARSE_ERROR;
    }
    strcpy(hash, hash_text);
    if (parse_size(compressed_text, compressed_size) != MG_OK || parse_size(object_text, object_size) != MG_OK) {
        return MG_PARSE_ERROR;
    }
    return MG_OK;
}

/*
 * read_packed_object_bytes finds a hash in the simple MiniGit packfile.
 */
static MGResult read_packed_object_bytes(
    const Repository *repo,
    const char *hash,
    unsigned char **out_data,
    size_t *out_size
) {
    char pack_path[MG_MAX_PATH];
    unsigned char *pack_data = NULL;
    size_t pack_size = 0;
    size_t offset;
    MGResult result;

    if (repo == NULL || !hash_is_valid_hex(hash) || out_data == NULL || out_size == NULL) {
        return MG_INVALID_ARG;
    }
    if (object_pack_path(repo, pack_path, sizeof(pack_path)) != MG_OK) {
        return MG_INVALID_ARG;
    }
    if (!fs_is_file(pack_path)) {
        return MG_NOT_FOUND;
    }

    result = read_file_limited(pack_path, MG_MAX_PACK_SIZE, &pack_data, &pack_size);
    if (result != MG_OK) {
        return result;
    }
    if (pack_size < strlen(MG_PACK_HEADER) ||
        memcmp(pack_data, MG_PACK_HEADER, strlen(MG_PACK_HEADER)) != 0) {
        free(pack_data);
        return MG_PARSE_ERROR;
    }

    offset = strlen(MG_PACK_HEADER);
    while (offset < pack_size) {
        unsigned char *line_start = pack_data + offset;
        unsigned char *line_end = memchr(line_start, '\n', pack_size - offset);
        char line[160];
        char entry_hash[MG_HASH_HEX_SIZE];
        size_t line_size;
        size_t compressed_size;
        size_t object_size;

        if (line_end == NULL) {
            free(pack_data);
            return MG_PARSE_ERROR;
        }
        line_size = (size_t)(line_end - line_start);
        if (line_size >= sizeof(line)) {
            free(pack_data);
            return MG_PARSE_ERROR;
        }
        memcpy(line, line_start, line_size);
        line[line_size] = '\0';
        if (parse_pack_line(line, entry_hash, &compressed_size, &object_size) != MG_OK) {
            free(pack_data);
            return MG_PARSE_ERROR;
        }

        offset = (size_t)(line_end + 1 - pack_data);
        if (compressed_size > pack_size - offset) {
            free(pack_data);
            return MG_PARSE_ERROR;
        }
        if (strcmp(entry_hash, hash) == 0) {
            result = decompress_bytes(pack_data + offset, compressed_size, object_size, out_data, out_size);
            free(pack_data);
            return result;
        }
        offset += compressed_size;
    }

    free(pack_data);
    return MG_NOT_FOUND;
}

/*
 * consider_prefix_candidate updates prefix resolution while ignoring duplicate
 * sightings of the same hash in loose and packed storage.
 */
static MGResult consider_prefix_candidate(
    const char *candidate,
    const char *prefix,
    size_t prefix_len,
    char out_hash[MG_HASH_HEX_SIZE],
    int *matches
) {
    if (strncmp(candidate, prefix, prefix_len) != 0) {
        return MG_OK;
    }
    if (*matches > 0 && strcmp(out_hash, candidate) == 0) {
        return MG_OK;
    }
    (*matches)++;
    if (*matches > 1) {
        return MG_CONFLICT;
    }
    strcpy(out_hash, candidate);
    return MG_OK;
}

/*
 * scan_pack_prefix checks hashes stored inside the packfile.
 */
static MGResult scan_pack_prefix(
    const Repository *repo,
    const char *prefix,
    size_t prefix_len,
    char out_hash[MG_HASH_HEX_SIZE],
    int *matches
) {
    char pack_path[MG_MAX_PATH];
    unsigned char *pack_data = NULL;
    size_t pack_size = 0;
    size_t offset;
    MGResult result;

    if (object_pack_path(repo, pack_path, sizeof(pack_path)) != MG_OK) {
        return MG_INVALID_ARG;
    }
    if (!fs_is_file(pack_path)) {
        return MG_OK;
    }
    result = read_file_limited(pack_path, MG_MAX_PACK_SIZE, &pack_data, &pack_size);
    if (result != MG_OK) {
        return result;
    }
    if (pack_size < strlen(MG_PACK_HEADER) ||
        memcmp(pack_data, MG_PACK_HEADER, strlen(MG_PACK_HEADER)) != 0) {
        free(pack_data);
        return MG_PARSE_ERROR;
    }

    offset = strlen(MG_PACK_HEADER);
    while (offset < pack_size) {
        unsigned char *line_start = pack_data + offset;
        unsigned char *line_end = memchr(line_start, '\n', pack_size - offset);
        char line[160];
        char entry_hash[MG_HASH_HEX_SIZE];
        size_t line_size;
        size_t compressed_size;
        size_t object_size;

        if (line_end == NULL) {
            free(pack_data);
            return MG_PARSE_ERROR;
        }
        line_size = (size_t)(line_end - line_start);
        if (line_size >= sizeof(line)) {
            free(pack_data);
            return MG_PARSE_ERROR;
        }
        memcpy(line, line_start, line_size);
        line[line_size] = '\0';
        if (parse_pack_line(line, entry_hash, &compressed_size, &object_size) != MG_OK) {
            free(pack_data);
            return MG_PARSE_ERROR;
        }

        offset = (size_t)(line_end + 1 - pack_data);
        if (compressed_size > pack_size - offset) {
            free(pack_data);
            return MG_PARSE_ERROR;
        }
        (void)object_size;
        result = consider_prefix_candidate(entry_hash, prefix, prefix_len, out_hash, matches);
        if (result != MG_OK) {
            free(pack_data);
            return result;
        }
        offset += compressed_size;
    }

    free(pack_data);
    return MG_OK;
}

/*
 * pack_buffer_append_object appends one compressed object record.
 */
static MGResult pack_buffer_append_object(
    PackBuffer *buffer,
    const char *hash,
    const unsigned char *object_data,
    size_t object_size
) {
    unsigned char *compressed = NULL;
    size_t compressed_size = 0;
    char line[192];
    int line_size;
    MGResult result;

    result = compress_bytes(object_data, object_size, &compressed, &compressed_size);
    if (result != MG_OK) {
        return result;
    }

    line_size = snprintf(line, sizeof(line), "%s %zu %zu\n", hash, compressed_size, object_size);
    if (line_size < 0 || (size_t)line_size >= sizeof(line)) {
        free(compressed);
        return MG_ERROR;
    }

    result = pack_buffer_append(buffer, (const unsigned char *)line, (size_t)line_size);
    if (result == MG_OK) {
        result = pack_buffer_append(buffer, compressed, compressed_size);
    }
    free(compressed);
    return result;
}

/*
 * object_write stores canonical object bytes unless the object already exists.
 */
MGResult object_write(
    const Repository *repo,
    const char *type,
    const unsigned char *payload,
    size_t size,
    char out_hash[MG_HASH_HEX_SIZE]
) {
    unsigned char *object_data = NULL;
    unsigned char *stored_data = NULL;
    size_t object_size = 0;
    size_t stored_size = 0;
    char dir_path[MG_MAX_PATH];
    char path[MG_MAX_PATH];
    MGResult result;

    if (repo == NULL || out_hash == NULL || size > MG_MAX_OBJECT_SIZE) {
        return MG_INVALID_ARG;
    }

    result = build_object_bytes(type, payload, size, &object_data, &object_size);
    if (result != MG_OK) {
        return result;
    }

    result = hash_bytes_with_mode(object_data, object_size, hash_default_object_mode(), out_hash);
    if (result != MG_OK) {
        free(object_data);
        return result;
    }

    if (object_dir_path(repo, out_hash, dir_path, sizeof(dir_path)) != MG_OK ||
        object_path(repo, out_hash, path, sizeof(path)) != MG_OK) {
        free(object_data);
        return MG_INVALID_ARG;
    }

    if (fs_exists(path)) {
        Object existing;
        result = object_read(repo, out_hash, &existing);
        free(object_data);
        if (result == MG_OK) {
            object_free(&existing);
        }
        return result;
    }

    if (fs_mkdir_p(dir_path) != MG_OK) {
        free(object_data);
        return MG_IO_ERROR;
    }

    result = build_loose_storage(object_data, object_size, &stored_data, &stored_size);
    free(object_data);
    if (result != MG_OK) {
        return result;
    }
    result = fs_write_file_atomic(path, stored_data, stored_size);
    free(stored_data);
    return result;
}

/*
 * object_read loads canonical bytes and validates the embedded header.
 */
MGResult object_read(const Repository *repo, const char *hash, Object *out_object) {
    unsigned char *data = NULL;
    size_t data_size = 0;
    MGResult result;

    if (repo == NULL || out_object == NULL || !hash_is_valid_hex(hash)) {
        return MG_INVALID_ARG;
    }

    out_object->type[0] = '\0';
    out_object->size = 0;
    out_object->payload = NULL;

    result = read_loose_object_bytes(repo, hash, &data, &data_size);
    if (result == MG_NOT_FOUND) {
        result = read_packed_object_bytes(repo, hash, &data, &data_size);
    }
    if (result != MG_OK) {
        return result;
    }

    {
        char actual_hash[MG_HASH_HEX_SIZE];
        HashMode mode = strlen(hash) == MG_GIT_HASH_HEX_SIZE - 1 ? HASH_GIT_SHA1 : HASH_MINIGIT_SHA256;

        result = hash_bytes_with_mode(data, data_size, mode, actual_hash);
        if (result == MG_OK) {
            for (size_t i = 0; hash[i] != '\0'; i++) {
                if ((char)tolower((unsigned char)hash[i]) != actual_hash[i]) {
                    result = MG_PARSE_ERROR;
                    break;
                }
            }
        }
    }
    if (result == MG_OK) {
        result = parse_object(data, data_size, out_object);
    }
    free(data);
    return result;
}

/*
 * object_resolve_prefix scans the object database for a unique matching id.
 */
MGResult object_resolve_prefix(const Repository *repo, const char *prefix, char out_hash[MG_HASH_HEX_SIZE]) {
    char objects_path[MG_MAX_PATH];
    char normalized_prefix[MG_HASH_HEX_SIZE];
    DIR *objects_dir;
    struct dirent *fanout_entry;
    size_t prefix_len;
    int matches = 0;
    MGResult result = MG_OK;

    if (repo == NULL || out_hash == NULL || !is_hex_prefix(prefix)) {
        return MG_INVALID_ARG;
    }

    prefix_len = strlen(prefix);
    for (size_t i = 0; i < prefix_len; i++) {
        normalized_prefix[i] = (char)tolower((unsigned char)prefix[i]);
    }
    normalized_prefix[prefix_len] = '\0';

    if (fs_join_path(repo->gitdir_path, "objects", objects_path, sizeof(objects_path)) != MG_OK) {
        return MG_INVALID_ARG;
    }

    objects_dir = opendir(objects_path);
    if (objects_dir == NULL) {
        return MG_IO_ERROR;
    }

    while ((fanout_entry = readdir(objects_dir)) != NULL && result == MG_OK) {
        char fanout_path[MG_MAX_PATH];
        DIR *fanout_dir;
        struct dirent *object_entry;

        if (!is_object_fanout_dir(fanout_entry->d_name)) {
            continue;
        }
        if (fs_join_path(objects_path, fanout_entry->d_name, fanout_path, sizeof(fanout_path)) != MG_OK) {
            result = MG_INVALID_ARG;
            break;
        }

        fanout_dir = opendir(fanout_path);
        if (fanout_dir == NULL) {
            result = MG_IO_ERROR;
            break;
        }

        while ((object_entry = readdir(fanout_dir)) != NULL) {
            char candidate[MG_HASH_HEX_SIZE];

            if (!is_object_file_name(object_entry->d_name)) {
                continue;
            }
            snprintf(candidate, sizeof(candidate), "%s%s", fanout_entry->d_name, object_entry->d_name);
            if (strncmp(candidate, normalized_prefix, prefix_len) != 0) {
                continue;
            }

            result = consider_prefix_candidate(candidate, normalized_prefix, prefix_len, out_hash, &matches);
            if (result != MG_OK) {
                break;
            }
        }

        if (closedir(fanout_dir) != 0 && result == MG_OK) {
            result = MG_IO_ERROR;
        }
    }

    if (closedir(objects_dir) != 0 && result == MG_OK) {
        result = MG_IO_ERROR;
    }
    if (result != MG_OK) {
        return result;
    }
    result = scan_pack_prefix(repo, normalized_prefix, prefix_len, out_hash, &matches);
    if (result != MG_OK) {
        return result;
    }
    if (matches == 0) {
        return MG_NOT_FOUND;
    }

    return MG_OK;
}

/*
 * object_pack_all writes all current loose objects into the simple packfile.
 * Loose objects are kept in place so packing is non-destructive.
 */
MGResult object_pack_all(const Repository *repo, size_t *out_count) {
    char objects_path[MG_MAX_PATH];
    char pack_dir[MG_MAX_PATH];
    char pack_path[MG_MAX_PATH];
    DIR *objects_dir;
    struct dirent *fanout_entry;
    PackBuffer buffer = {NULL, 0, 0};
    size_t count = 0;
    MGResult result = MG_OK;

    if (repo == NULL || out_count == NULL) {
        return MG_INVALID_ARG;
    }
    *out_count = 0;
    if (fs_join_path(repo->gitdir_path, "objects", objects_path, sizeof(objects_path)) != MG_OK ||
        object_pack_dir_path(repo, pack_dir, sizeof(pack_dir)) != MG_OK ||
        object_pack_path(repo, pack_path, sizeof(pack_path)) != MG_OK) {
        return MG_INVALID_ARG;
    }

    result = pack_buffer_append(&buffer, (const unsigned char *)MG_PACK_HEADER, strlen(MG_PACK_HEADER));
    if (result != MG_OK) {
        free(buffer.data);
        return result;
    }

    objects_dir = opendir(objects_path);
    if (objects_dir == NULL) {
        free(buffer.data);
        return MG_IO_ERROR;
    }

    while ((fanout_entry = readdir(objects_dir)) != NULL && result == MG_OK) {
        char fanout_path[MG_MAX_PATH];
        DIR *fanout_dir;
        struct dirent *object_entry;

        if (!is_object_fanout_dir(fanout_entry->d_name)) {
            continue;
        }
        if (fs_join_path(objects_path, fanout_entry->d_name, fanout_path, sizeof(fanout_path)) != MG_OK) {
            result = MG_INVALID_ARG;
            break;
        }

        fanout_dir = opendir(fanout_path);
        if (fanout_dir == NULL) {
            result = MG_IO_ERROR;
            break;
        }

        while ((object_entry = readdir(fanout_dir)) != NULL) {
            char hash[MG_HASH_HEX_SIZE];
            unsigned char *object_data = NULL;
            size_t object_size = 0;

            if (!is_object_file_name(object_entry->d_name)) {
                continue;
            }
            snprintf(hash, sizeof(hash), "%s%s", fanout_entry->d_name, object_entry->d_name);
            result = read_loose_object_bytes(repo, hash, &object_data, &object_size);
            if (result != MG_OK) {
                break;
            }
            {
                char actual_hash[MG_HASH_HEX_SIZE];
                HashMode mode = strlen(hash) == MG_GIT_HASH_HEX_SIZE - 1 ? HASH_GIT_SHA1 : HASH_MINIGIT_SHA256;
                result = hash_bytes_with_mode(object_data, object_size, mode, actual_hash);
                if (result == MG_OK && strcmp(actual_hash, hash) != 0) {
                    result = MG_PARSE_ERROR;
                }
            }
            if (result == MG_OK) {
                result = pack_buffer_append_object(&buffer, hash, object_data, object_size);
            }
            free(object_data);
            if (result != MG_OK) {
                break;
            }
            count++;
        }

        if (closedir(fanout_dir) != 0 && result == MG_OK) {
            result = MG_IO_ERROR;
        }
    }

    if (closedir(objects_dir) != 0 && result == MG_OK) {
        result = MG_IO_ERROR;
    }
    if (result == MG_OK && fs_mkdir_p(pack_dir) != MG_OK) {
        result = MG_IO_ERROR;
    }
    if (result == MG_OK) {
        result = fs_write_file_atomic(pack_path, buffer.data, buffer.size);
    }
    free(buffer.data);
    if (result != MG_OK) {
        return result;
    }

    *out_count = count;
    return MG_OK;
}

/*
 * object_free releases owned payload memory and clears the object.
 */
void object_free(Object *object) {
    if (object == NULL) {
        return;
    }

    free(object->payload);
    object->payload = NULL;
    object->size = 0;
    object->type[0] = '\0';
}
