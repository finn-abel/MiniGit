#ifndef MINIGIT_FS_H
#define MINIGIT_FS_H

#include <stddef.h>

#include "common.h"

/*
 * FSWalkCallback is called once for each regular file found by fs_walk.
 */
typedef MGResult (*FSWalkCallback)(const char *relative_path, void *ctx);

/*
 * fs_exists reports whether a filesystem path exists.
 */
int fs_exists(const char *path);

/*
 * fs_is_file reports whether a path is a regular file.
 */
int fs_is_file(const char *path);

/*
 * fs_is_dir reports whether a path is a directory.
 */
int fs_is_dir(const char *path);

/*
 * fs_mkdir creates one directory.
 */
MGResult fs_mkdir(const char *path);

/*
 * fs_mkdir_p creates a directory and any missing parent directories.
 */
MGResult fs_mkdir_p(const char *path);

/*
 * fs_read_file reads an entire file as bytes.
 */
MGResult fs_read_file(const char *path, unsigned char **out_data, size_t *out_size);

/*
 * fs_write_file writes bytes to a file, replacing existing contents.
 */
MGResult fs_write_file(const char *path, const unsigned char *data, size_t size);

/*
 * fs_join_path joins two path components into out.
 */
MGResult fs_join_path(const char *left, const char *right, char *out, size_t out_size);

/*
 * fs_parent_dir writes the parent directory for path into out.
 */
MGResult fs_parent_dir(const char *path, char *out, size_t out_size);

/*
 * fs_remove_file removes a regular file.
 */
MGResult fs_remove_file(const char *path);

/*
 * fs_repo_relative_path normalizes path to a repo-relative path.
 */
MGResult fs_repo_relative_path(const char *repo_root, const char *path, char *out, size_t out_size);

/*
 * fs_walk visits regular files below root_path using repo-relative paths.
 */
MGResult fs_walk(const char *root_path, FSWalkCallback callback, void *ctx);

#endif
