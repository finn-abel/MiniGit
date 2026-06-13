#define _POSIX_C_SOURCE 200809L

#include "commit.h"

#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "hash.h"
#include "object.h"

#define MG_DEFAULT_AUTHOR_NAME "MiniGit User"
#define MG_DEFAULT_AUTHOR_EMAIL "minigit@example.com"

/*
 * copy_text owns a parsed commit string field.
 */
static MGResult copy_text(char **out, const char *value) {
    size_t len;

    if (out == NULL || value == NULL || value[0] == '\0') {
        return MG_PARSE_ERROR;
    }

    len = strlen(value) + 1;
    *out = malloc(len);
    if (*out == NULL) {
        return MG_ERROR;
    }
    memcpy(*out, value, len);
    return MG_OK;
}

static MGResult copy_message(Commit *commit, const char *message) {
    if (commit == NULL) {
        return MG_PARSE_ERROR;
    }
    return copy_text(&commit->message, message);
}

static MGResult copy_author(Commit *commit, const char *name, const char *email) {
    MGResult result;

    if (commit == NULL) {
        return MG_PARSE_ERROR;
    }

    result = copy_text(&commit->author_name, name);
    if (result != MG_OK) {
        return result;
    }
    result = copy_text(&commit->author_email, email);
    if (result != MG_OK) {
        free(commit->author_name);
        commit->author_name = NULL;
        return result;
    }
    return MG_OK;
}

static const char *author_env_or_default(const char *name, const char *fallback) {
    const char *value = getenv(name);
    if (value == NULL || value[0] == '\0') {
        return fallback;
    }
    return value;
}

static int author_value_is_valid(const char *value) {
    return value != NULL && value[0] != '\0' && strchr(value, '\n') == NULL && strchr(value, '\r') == NULL;
}

static MGResult parse_author_line(Commit *commit, const char *line) {
    const char *name_start;
    const char *email_start;
    const char *email_end;
    size_t name_len;
    size_t email_len;
    char *name;
    char *email;
    MGResult result;

    if (commit == NULL || line == NULL || strncmp(line, "author ", 7) != 0) {
        return MG_PARSE_ERROR;
    }

    name_start = line + 7;
    email_start = strchr(name_start, '<');
    email_end = email_start == NULL ? NULL : strchr(email_start + 1, '>');
    if (email_start == NULL || email_end == NULL || email_end[1] != '\0' || email_start == name_start) {
        return MG_PARSE_ERROR;
    }
    if (email_start[-1] != ' ') {
        return MG_PARSE_ERROR;
    }

    name_len = (size_t)(email_start - name_start - 1);
    email_len = (size_t)(email_end - email_start - 1);
    if (name_len == 0 || email_len == 0) {
        return MG_PARSE_ERROR;
    }

    name = malloc(name_len + 1);
    email = malloc(email_len + 1);
    if (name == NULL || email == NULL) {
        free(name);
        free(email);
        return MG_ERROR;
    }
    memcpy(name, name_start, name_len);
    name[name_len] = '\0';
    memcpy(email, email_start + 1, email_len);
    email[email_len] = '\0';

    result = copy_author(commit, name, email);
    free(name);
    free(email);
    return result;
}

/*
 * parse_timestamp validates a complete non-negative timestamp.
 * The commit format stores time(NULL) as decimal text.
 */
static MGResult parse_timestamp(const char *text, time_t *out_timestamp) {
    char *end = NULL;
    uintmax_t parsed;
    time_t converted;

    if (text == NULL || text[0] == '\0' || out_timestamp == NULL) {
        return MG_PARSE_ERROR;
    }

    errno = 0;
    parsed = strtoumax(text, &end, 10);
    converted = (time_t)parsed;
    if (errno != 0 || *end != '\0' || converted < 0 || (uintmax_t)converted != parsed) {
        return MG_PARSE_ERROR;
    }

    *out_timestamp = converted;
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
 * Valid order is tree, up to two parents, optional author, timestamp, message, then EOF.
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
    /* Parents are present for normal/merge commits and omitted for the first commit. */
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
        if (line != NULL && strncmp(line, "parent ", 7) == 0) {
            if (!hash_is_valid_hex(line + 7)) {
                goto done;
            }
            strcpy(commit->second_parent_hash, line + 7);
            line = next_line(&cursor);
        }
    } else {
        commit->parent_hash[0] = '\0';
        commit->second_parent_hash[0] = '\0';
    }

    if (line != NULL && strncmp(line, "author ", 7) == 0) {
        if (parse_author_line(commit, line) != MG_OK) {
            goto done;
        }
        line = next_line(&cursor);
    } else if (copy_author(commit, MG_DEFAULT_AUTHOR_NAME, MG_DEFAULT_AUTHOR_EMAIL) != MG_OK) {
        goto done;
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

static MGResult commit_create_with_parents(
    const Repository *repo,
    const char *tree_hash,
    const char *parent_hash,
    const char *second_parent_hash,
    const char *message,
    char out_hash[MG_HASH_HEX_SIZE]
) {
    char timestamp_text[32];
    const char *author_name;
    const char *author_email;
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
    if (second_parent_hash != NULL && second_parent_hash[0] != '\0' &&
        ((parent_hash == NULL || parent_hash[0] == '\0') || !hash_is_valid_hex(second_parent_hash))) {
        return MG_INVALID_ARG;
    }
    author_name = author_env_or_default("MINIGIT_AUTHOR_NAME", MG_DEFAULT_AUTHOR_NAME);
    author_email = author_env_or_default("MINIGIT_AUTHOR_EMAIL", MG_DEFAULT_AUTHOR_EMAIL);
    if (!author_value_is_valid(author_name) || !author_value_is_valid(author_email) ||
        strchr(author_email, '<') != NULL || strchr(author_email, '>') != NULL) {
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
    if (second_parent_hash != NULL && second_parent_hash[0] != '\0') {
        payload_size = snprintf(
            NULL,
            0,
            "tree %s\nparent %s\nparent %s\nauthor %s <%s>\ntimestamp %s\nmessage %s\n",
            tree_hash,
            parent_hash,
            second_parent_hash,
            author_name,
            author_email,
            timestamp_text,
            message
        );
    } else if (parent_hash != NULL && parent_hash[0] != '\0') {
        payload_size = snprintf(
            NULL,
            0,
            "tree %s\nparent %s\nauthor %s <%s>\ntimestamp %s\nmessage %s\n",
            tree_hash,
            parent_hash,
            author_name,
            author_email,
            timestamp_text,
            message
        );
    } else {
        payload_size = snprintf(
            NULL,
            0,
            "tree %s\nauthor %s <%s>\ntimestamp %s\nmessage %s\n",
            tree_hash,
            author_name,
            author_email,
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
    if (second_parent_hash != NULL && second_parent_hash[0] != '\0') {
        snprintf(
            (char *)payload,
            (size_t)payload_size + 1,
            "tree %s\nparent %s\nparent %s\nauthor %s <%s>\ntimestamp %s\nmessage %s\n",
            tree_hash,
            parent_hash,
            second_parent_hash,
            author_name,
            author_email,
            timestamp_text,
            message
        );
    } else if (parent_hash != NULL && parent_hash[0] != '\0') {
        snprintf(
            (char *)payload,
            (size_t)payload_size + 1,
            "tree %s\nparent %s\nauthor %s <%s>\ntimestamp %s\nmessage %s\n",
            tree_hash,
            parent_hash,
            author_name,
            author_email,
            timestamp_text,
            message
        );
    } else {
        snprintf(
            (char *)payload,
            (size_t)payload_size + 1,
            "tree %s\nauthor %s <%s>\ntimestamp %s\nmessage %s\n",
            tree_hash,
            author_name,
            author_email,
            timestamp_text,
            message
        );
    }

    result = object_write(repo, "commit", payload, (size_t)payload_size, out_hash);
    free(payload);
    return result;
}

MGResult commit_create(
    const Repository *repo,
    const char *tree_hash,
    const char *parent_hash,
    const char *message,
    char out_hash[MG_HASH_HEX_SIZE]
) {
    return commit_create_with_parents(repo, tree_hash, parent_hash, "", message, out_hash);
}

MGResult commit_create_merge(
    const Repository *repo,
    const char *tree_hash,
    const char *first_parent_hash,
    const char *second_parent_hash,
    const char *message,
    char out_hash[MG_HASH_HEX_SIZE]
) {
    return commit_create_with_parents(
        repo,
        tree_hash,
        first_parent_hash,
        second_parent_hash,
        message,
        out_hash
    );
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

MGResult commit_format_timestamp(time_t timestamp, char *out, size_t out_size) {
    struct tm time_info;

    if (out == NULL || out_size == 0 || timestamp < 0) {
        return MG_INVALID_ARG;
    }
    if (localtime_r(&timestamp, &time_info) == NULL) {
        return MG_ERROR;
    }
    if (strftime(out, out_size, "%c", &time_info) == 0) {
        return MG_INVALID_ARG;
    }

    return MG_OK;
}

void commit_free(Commit *commit) {
    if (commit == NULL) {
        return;
    }

    free(commit->message);
    free(commit->author_name);
    free(commit->author_email);
    memset(commit, 0, sizeof(*commit));
}
