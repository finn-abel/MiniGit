#define _POSIX_C_SOURCE 200809L

#include "commit.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "hash.h"
#include "object.h"

/*
 * copy_message owns the commit message string after parsing.
 * Commit messages are one line in v1, so empty parsed messages are rejected.
 */
static MGResult copy_message(Commit *commit, const char *message) {
    size_t len;

    if (commit == NULL || message == NULL || message[0] == '\0') {
        return MG_PARSE_ERROR;
    }

    len = strlen(message) + 1;
    commit->message = malloc(len);
    if (commit->message == NULL) {
        return MG_ERROR;
    }
    memcpy(commit->message, message, len);
    return MG_OK;
}

/*
 * parse_timestamp validates a complete non-negative timestamp.
 * The commit format stores time(NULL) as decimal text.
 */
static MGResult parse_timestamp(const char *text, time_t *out_timestamp) {
    char *end = NULL;
    long parsed;

    if (text == NULL || text[0] == '\0' || out_timestamp == NULL) {
        return MG_PARSE_ERROR;
    }

    parsed = strtol(text, &end, 10);
    if (*end != '\0' || parsed < 0) {
        return MG_PARSE_ERROR;
    }

    *out_timestamp = (time_t)parsed;
    return MG_OK;
}

/*
 * commit_tree_exists ensures commits only point at real tree objects.
 * This catches both missing objects and hashes that name a blob/commit.
 */
static MGResult commit_tree_exists(const Repository *repo, const char *tree_hash) {
    Object object;
    MGResult result;

    if (!hash_is_valid_hex(tree_hash)) {
        return MG_INVALID_ARG;
    }

    result = object_read(repo, tree_hash, &object);
    if (result != MG_OK) {
        return result;
    }
    if (strcmp(object.type, "tree") != 0) {
        object_free(&object);
        return MG_PARSE_ERROR;
    }

    object_free(&object);
    return MG_OK;
}

/*
 * next_line returns one mutable line from a newline-delimited buffer.
 * It avoids non-standard strsep while still letting the parser consume lines
 * in order.
 */
static char *next_line(char **cursor) {
    char *line;
    char *newline;

    if (cursor == NULL || *cursor == NULL) {
        return NULL;
    }

    line = *cursor;
    newline = strchr(line, '\n');
    if (newline == NULL) {
        *cursor = NULL;
        return line;
    }

    *newline = '\0';
    *cursor = newline + 1;
    return line;
}

/*
 * parse_commit_payload validates the commit wire format line by line.
 * Valid order is tree, optional parent, timestamp, message, then EOF.
 */
static MGResult parse_commit_payload(const unsigned char *payload, size_t size, Commit *commit) {
    char *buffer;
    char *cursor;
    char *line;
    MGResult result = MG_PARSE_ERROR;

    if (payload == NULL || commit == NULL || size == 0 || payload[size - 1] != '\n') {
        return MG_PARSE_ERROR;
    }

    /*
     * Work on a mutable copy because next_line writes NUL terminators into the
     * payload while splitting it into records.
     */
    buffer = malloc(size + 1);
    if (buffer == NULL) {
        return MG_ERROR;
    }
    memcpy(buffer, payload, size);
    buffer[size] = '\0';

    /* First line is always the tree reference. */
    cursor = buffer;
    /* Parent is present for normal commits and omitted for the first commit. */
    line = next_line(&cursor);
    if (line == NULL || strncmp(line, "tree ", 5) != 0 || !hash_is_valid_hex(line + 5)) {
        goto done;
    }
    strcpy(commit->tree_hash, line + 5);

    line = next_line(&cursor);
    if (line != NULL && strncmp(line, "parent ", 7) == 0) {
        if (!hash_is_valid_hex(line + 7)) {
            goto done;
        }
        strcpy(commit->parent_hash, line + 7);
        line = next_line(&cursor);
    } else {
        commit->parent_hash[0] = '\0';
    }

    /* Timestamp and message are required for every commit. */
    if (line == NULL || strncmp(line, "timestamp ", 10) != 0 ||
        parse_timestamp(line + 10, &commit->timestamp) != MG_OK) {
        goto done;
    }

    /*
     * The trailing newline creates one final empty line after message; any
     * additional content means the object is malformed.
     */
    line = next_line(&cursor);
    if (line == NULL || strncmp(line, "message ", 8) != 0 ||
        copy_message(commit, line + 8) != MG_OK) {
        goto done;
    }

    line = next_line(&cursor);
    if (line == NULL || line[0] != '\0' || (cursor != NULL && cursor[0] != '\0')) {
        goto done;
    }

    result = MG_OK;

done:
    free(buffer);
    return result;
}

MGResult commit_create(
    const Repository *repo,
    const char *tree_hash,
    const char *parent_hash,
    const char *message,
    char out_hash[MG_HASH_HEX_SIZE]
) {
    char timestamp_text[32];
    time_t now;
    int timestamp_len;
    int payload_size;
    unsigned char *payload;
    MGResult result;

    /*
     * Messages are intentionally single-line because the v1 commit format is
     * line-based and has no escaping rules yet.
     */
    if (repo == NULL || tree_hash == NULL || message == NULL || out_hash == NULL ||
        message[0] == '\0' || strchr(message, '\n') != NULL || !hash_is_valid_hex(tree_hash)) {
        return MG_INVALID_ARG;
    }
    if (parent_hash != NULL && parent_hash[0] != '\0' && !hash_is_valid_hex(parent_hash)) {
        return MG_INVALID_ARG;
    }

    result = commit_tree_exists(repo, tree_hash);
    if (result != MG_OK) {
        return result;
    }

    now = time(NULL);
    if (now == (time_t)-1) {
        return MG_ERROR;
    }
    timestamp_len = snprintf(timestamp_text, sizeof(timestamp_text), "%ld", (long)now);
    if (timestamp_len < 0 || (size_t)timestamp_len >= sizeof(timestamp_text)) {
        return MG_ERROR;
    }

    /* First pass calculates the exact payload size for either commit shape. */
    if (parent_hash != NULL && parent_hash[0] != '\0') {
        payload_size = snprintf(
            NULL,
            0,
            "tree %s\nparent %s\ntimestamp %s\nmessage %s\n",
            tree_hash,
            parent_hash,
            timestamp_text,
            message
        );
    } else {
        payload_size = snprintf(
            NULL,
            0,
            "tree %s\ntimestamp %s\nmessage %s\n",
            tree_hash,
            timestamp_text,
            message
        );
    }
    if (payload_size < 0) {
        return MG_ERROR;
    }

    payload = malloc((size_t)payload_size + 1);
    if (payload == NULL) {
        return MG_ERROR;
    }

    /* Second pass writes the canonical commit payload. */
    if (parent_hash != NULL && parent_hash[0] != '\0') {
        snprintf(
            (char *)payload,
            (size_t)payload_size + 1,
            "tree %s\nparent %s\ntimestamp %s\nmessage %s\n",
            tree_hash,
            parent_hash,
            timestamp_text,
            message
        );
    } else {
        snprintf(
            (char *)payload,
            (size_t)payload_size + 1,
            "tree %s\ntimestamp %s\nmessage %s\n",
            tree_hash,
            timestamp_text,
            message
        );
    }

    result = object_write(repo, "commit", payload, (size_t)payload_size, out_hash);
    free(payload);
    return result;
}

MGResult commit_read(const Repository *repo, const char *hash, Commit *commit) {
    Object object;
    MGResult result;

    if (repo == NULL || commit == NULL || !hash_is_valid_hex(hash)) {
        return MG_INVALID_ARG;
    }

    /* Start from a zeroed struct so commit_free is safe on parse failure. */
    memset(commit, 0, sizeof(*commit));
    result = object_read(repo, hash, &object);
    if (result != MG_OK) {
        return result;
    }
    if (strcmp(object.type, "commit") != 0) {
        object_free(&object);
        return MG_PARSE_ERROR;
    }

    result = parse_commit_payload(object.payload, object.size, commit);
    object_free(&object);
    if (result != MG_OK) {
        commit_free(commit);
    }
    return result;
}

void commit_free(Commit *commit) {
    if (commit == NULL) {
        return;
    }

    free(commit->message);
    memset(commit, 0, sizeof(*commit));
}
