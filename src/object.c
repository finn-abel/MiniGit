#include "object.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fs.h"
#include "hash.h"

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

    total_size = (size_t)header_size + 1 + size;
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
 * parse_size parses the decimal payload size in an object header.
 */
static MGResult parse_size(const char *text, size_t *out_size) {
    char *end = NULL;
    unsigned long parsed;

    if (text == NULL || text[0] == '\0' || out_size == NULL) {
        return MG_PARSE_ERROR;
    }

    parsed = strtoul(text, &end, 10);
    if (*end != '\0') {
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
    if (header_size + 1 + payload_size != data_size) {
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
    size_t object_size = 0;
    char dir_path[MG_MAX_PATH];
    char path[MG_MAX_PATH];
    MGResult result;

    if (repo == NULL || out_hash == NULL) {
        return MG_INVALID_ARG;
    }

    result = build_object_bytes(type, payload, size, &object_data, &object_size);
    if (result != MG_OK) {
        return result;
    }

    result = hash_bytes(object_data, object_size, out_hash);
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
        free(object_data);
        return MG_OK;
    }

    if (fs_mkdir_p(dir_path) != MG_OK) {
        free(object_data);
        return MG_IO_ERROR;
    }

    result = fs_write_file(path, object_data, object_size);
    free(object_data);
    return result;
}

/*
 * object_read loads canonical bytes and validates the embedded header.
 */
MGResult object_read(const Repository *repo, const char *hash, Object *out_object) {
    char path[MG_MAX_PATH];
    unsigned char *data = NULL;
    size_t data_size = 0;
    MGResult result;

    if (repo == NULL || out_object == NULL || !hash_is_valid_hex(hash)) {
        return MG_INVALID_ARG;
    }

    out_object->type[0] = '\0';
    out_object->size = 0;
    out_object->payload = NULL;

    if (object_path(repo, hash, path, sizeof(path)) != MG_OK) {
        return MG_INVALID_ARG;
    }
    if (!fs_is_file(path)) {
        return MG_NOT_FOUND;
    }

    result = fs_read_file(path, &data, &data_size);
    if (result != MG_OK) {
        return result;
    }

    result = parse_object(data, data_size, out_object);
    free(data);
    return result;
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
