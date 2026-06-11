#ifndef MINIGIT_RESET_H
#define MINIGIT_RESET_H

#include "common.h"
#include "repository.h"

/*
 * reset_path restores one index entry from HEAD without changing the worktree.
 */
MGResult reset_path(const Repository *repo, const char *relative_path);

#endif
