#ifndef MINIGIT_COMMON_H
#define MINIGIT_COMMON_H

/*
 * MGResult represents recoverable outcomes returned by MiniGit modules.
 */
typedef enum {
    MG_OK = 0,
    MG_ERROR,
    MG_IO_ERROR,
    MG_NOT_FOUND,
    MG_INVALID_ARG,
    MG_PARSE_ERROR,
    MG_REPO_ERROR,
    MG_CONFLICT
} MGResult;

#define MG_HASH_HEX_SIZE 65
#define MG_MAX_PATH 1024
#define MG_MAX_BRANCH 128

#endif
