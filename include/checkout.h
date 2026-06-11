#ifndef MINIGIT_CHECKOUT_H
#define MINIGIT_CHECKOUT_H

#include "common.h"
#include "repository.h"

/*
 * checkout_commit restores a commit tree and optionally detaches HEAD.
 */
MGResult checkout_commit(const Repository *repo, const char commit_hash[MG_HASH_HEX_SIZE], int detached);

/*
 * checkout_restore_path restores one tracked path from HEAD into the index and working tree.
 */
MGResult checkout_restore_path(const Repository *repo, const char *relative_path);

/*
 * checkout_restore_path_from_commit restores one path from a specific commit.
 */
MGResult checkout_restore_path_from_commit(
    const Repository *repo,
    const char commit_hash[MG_HASH_HEX_SIZE],
    const char *relative_path
);

#endif
