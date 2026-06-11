#ifndef MINIGIT_DIFF_H
#define MINIGIT_DIFF_H

#include "common.h"
#include "repository.h"

/*
 * DiffMode selects which repository snapshots are compared.
 */
typedef enum {
    DIFF_WORKTREE,
    DIFF_STAGED,
    DIFF_HEAD
} DiffMode;

/*
 * diff_print computes and prints line-based diffs for tracked paths.
 */
MGResult diff_print(const Repository *repo, DiffMode mode);

#endif
