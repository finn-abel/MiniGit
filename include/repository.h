#ifndef MINIGIT_REPOSITORY_H
#define MINIGIT_REPOSITORY_H

#include <stddef.h>

#include "common.h"

/*
 * RepoBranchCallback is called once for each branch name.
 */
typedef MGResult (*RepoBranchCallback)(const char *branch_name, int is_current, void *ctx);

/*
 * Repository stores paths to the working tree and internal .minigit directory.
 */
typedef struct {
    char worktree_path[MG_MAX_PATH];
    char gitdir_path[MG_MAX_PATH];
} Repository;

/*
 * repo_init creates the .minigit directory layout in the current directory.
 */
MGResult repo_init(Repository *repo);

/*
 * repo_open loads a MiniGit repository from the current directory.
 */
MGResult repo_open(Repository *repo);

/*
 * repo_read_head reads the current HEAD file contents.
 */
MGResult repo_read_head(const Repository *repo, char *out, size_t out_size);

/*
 * repo_write_head replaces the current HEAD file contents.
 */
MGResult repo_write_head(const Repository *repo, const char *contents);

/*
 * repo_head_is_detached reports whether HEAD stores a raw commit hash.
 */
int repo_head_is_detached(const char *head_contents);

/*
 * repo_current_branch resolves the current branch name from HEAD.
 */
MGResult repo_current_branch(const Repository *repo, char *out, size_t out_size);

/*
 * repo_current_commit resolves the current commit hash or an empty string.
 */
MGResult repo_current_commit(const Repository *repo, char *out, size_t out_size);

/*
 * repo_update_current_ref updates the current branch ref or detached HEAD.
 */
MGResult repo_update_current_ref(const Repository *repo, const char *commit_hash);

/*
 * repo_head_display_name writes the current branch name or "detached".
 */
MGResult repo_head_display_name(const Repository *repo, char *out, size_t out_size);

/*
 * repo_branch_name_is_valid reports whether name is accepted in v1.
 */
int repo_branch_name_is_valid(const char *name);

/*
 * repo_create_branch creates a branch at the current commit.
 */
MGResult repo_create_branch(const Repository *repo, const char *name);

/*
 * repo_list_branches visits every local branch in sorted order.
 */
MGResult repo_list_branches(const Repository *repo, RepoBranchCallback callback, void *ctx);

#endif
