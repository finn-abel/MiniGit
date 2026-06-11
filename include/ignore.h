#ifndef MINIGIT_IGNORE_H
#define MINIGIT_IGNORE_H

#include <stddef.h>

#include "common.h"
#include "repository.h"

/*
 * IgnoreRules owns patterns loaded from .minigitignore.
 */
typedef struct {
    char **patterns;
    size_t count;
    size_t capacity;
} IgnoreRules;

/*
 * ignore_rules_load reads .minigitignore if present.
 */
MGResult ignore_rules_load(const Repository *repo, IgnoreRules *rules);

/*
 * ignore_rules_match reports whether a repo-relative path is ignored.
 */
int ignore_rules_match(const IgnoreRules *rules, const char *relative_path);

/*
 * ignore_rules_free releases loaded patterns.
 */
void ignore_rules_free(IgnoreRules *rules);

#endif
