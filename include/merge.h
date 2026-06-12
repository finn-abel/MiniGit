#ifndef MINIGIT_MERGE_H
#define MINIGIT_MERGE_H

#include "common.h"
#include "repository.h"

/*
 * merge_branch merges another branch into the current worktree and index.
 */
MGResult merge_branch(const Repository *repo, const char *branch_name);

#endif
