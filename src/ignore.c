#define _POSIX_C_SOURCE 200809L

#include "ignore.h"

#include <stdlib.h>
#include <string.h>

#include "fs.h"

/*
 * ignore_rules_init prepares an empty rule set. The loader treats a missing
 * .minigitignore file the same way: no patterns, no error.
 */
static void ignore_rules_init(IgnoreRules *rules) {
    rules->patterns = NULL;
    rules->count = 0;
    rules->capacity = 0;
}

/*
 * copy_pattern owns one parsed pattern line after the file buffer is freed.
 */
static char *copy_pattern(const char *pattern) {
    size_t len = strlen(pattern) + 1;
    char *copy = malloc(len);
    if (copy == NULL) {
        return NULL;
    }
    memcpy(copy, pattern, len);
    return copy;
}

/*
 * ignore_rules_add appends one validated pattern and grows the owned array.
 */
static MGResult ignore_rules_add(IgnoreRules *rules, const char *pattern) {
    char **grown;
    size_t capacity;

    if (rules == NULL || pattern == NULL || pattern[0] == '\0') {
        return MG_INVALID_ARG;
    }

    if (rules->count == rules->capacity) {
        capacity = rules->capacity == 0 ? 8 : rules->capacity * 2;
        grown = realloc(rules->patterns, capacity * sizeof(rules->patterns[0]));
        if (grown == NULL) {
            return MG_ERROR;
        }
        rules->patterns = grown;
        rules->capacity = capacity;
    }

    rules->patterns[rules->count] = copy_pattern(pattern);
    if (rules->patterns[rules->count] == NULL) {
        return MG_ERROR;
    }
    rules->count++;
    return MG_OK;
}

/*
 * trim_line_end removes platform newline endings before matching patterns.
 */
static void trim_line_end(char *line) {
    size_t len = strlen(line);
    while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) {
        line[len - 1] = '\0';
        len--;
    }
}

MGResult ignore_rules_load(const Repository *repo, IgnoreRules *rules) {
    char path[MG_MAX_PATH];
    unsigned char *data = NULL;
    size_t size = 0;
    char *cursor;
    MGResult result = MG_OK;

    if (repo == NULL || rules == NULL) {
        return MG_INVALID_ARG;
    }

    ignore_rules_init(rules);
    if (fs_join_path(repo->worktree_path, ".minigitignore", path, sizeof(path)) != MG_OK) {
        return MG_INVALID_ARG;
    }
    if (!fs_is_file(path)) {
        return MG_OK;
    }

    result = fs_read_file(path, &data, &size);
    if (result != MG_OK) {
        return result;
    }

    /*
     * The v1 ignore format is deliberately tiny: blank lines and comments are
     * skipped, leading slashes are ignored, and all other text is stored as an
     * exact path or directory-prefix pattern.
     */
    cursor = (char *)data;
    while (cursor != NULL && *cursor != '\0') {
        char *line = cursor;
        char *newline = strchr(cursor, '\n');
        if (newline == NULL) {
            cursor = NULL;
        } else {
            *newline = '\0';
            cursor = newline + 1;
        }

        trim_line_end(line);
        if (line[0] == '\0' || line[0] == '#') {
            continue;
        }
        while (line[0] == '/') {
            line++;
        }
        if (line[0] == '\0') {
            continue;
        }

        result = ignore_rules_add(rules, line);
        if (result != MG_OK) {
            break;
        }
    }

    free(data);
    if (result != MG_OK) {
        ignore_rules_free(rules);
    }
    return result;
}

int ignore_rules_match(const IgnoreRules *rules, const char *relative_path) {
    if (rules == NULL || relative_path == NULL || relative_path[0] == '\0') {
        return 0;
    }

    for (size_t i = 0; i < rules->count; i++) {
        const char *pattern = rules->patterns[i];
        size_t len = strlen(pattern);

        /*
         * A pattern ending in "/" matches everything below that repo-relative
         * directory. Other patterns must match the complete relative path.
         */
        if (len > 0 && pattern[len - 1] == '/') {
            if (strncmp(relative_path, pattern, len) == 0) {
                return 1;
            }
        } else if (strcmp(relative_path, pattern) == 0) {
            return 1;
        }
    }

    return 0;
}

void ignore_rules_free(IgnoreRules *rules) {
    if (rules == NULL) {
        return;
    }
    for (size_t i = 0; i < rules->count; i++) {
        free(rules->patterns[i]);
    }
    free(rules->patterns);
    rules->patterns = NULL;
    rules->count = 0;
    rules->capacity = 0;
}
