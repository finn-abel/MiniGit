#ifndef MINIGIT_COMMIT_H
#define MINIGIT_COMMIT_H

#include <time.h>

#include "common.h"
#include "repository.h"

/*
 * Commit stores parsed commit object metadata.
 */
typedef struct {
    char tree_hash[MG_HASH_HEX_SIZE];
    char parent_hash[MG_HASH_HEX_SIZE];
    char *author_name;
    char *author_email;
    time_t timestamp;
    char *message;
} Commit;

/*
 * commit_create validates inputs, writes a commit object, and returns its hash.
 */
MGResult commit_create(
    const Repository *repo,
    const char *tree_hash,
    const char *parent_hash,
    const char *message,
    char out_hash[MG_HASH_HEX_SIZE]
);

/*
 * commit_read reads and parses a commit object by hash.
 */
MGResult commit_read(const Repository *repo, const char *hash, Commit *commit);

/*
 * commit_format_timestamp writes a human-readable timestamp.
 */
MGResult commit_format_timestamp(time_t timestamp, char *out, size_t out_size);

/*
 * commit_free releases memory owned by commit.
 */
void commit_free(Commit *commit);

#endif
