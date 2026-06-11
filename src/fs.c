#define _POSIX_C_SOURCE 200809L

#include "fs.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/*
 * stat_path wraps lstat so callers do not follow symlinks in v1.
 */
static int stat_path(const char *path, struct stat *st) {
    return lstat(path, st) == 0;
}

/*
 * is_dot_entry filters directory self and parent entries during walks.
 */
static int is_dot_entry(const char *name) {
    return strcmp(name, ".") == 0 || strcmp(name, "..") == 0;
}

/*
 * copy_string stores directory entry names before sorting them.
 */
static char *copy_string(const char *value) {
    size_t len = strlen(value) + 1;
    char *copy = malloc(len);
    if (copy == NULL) {
        return NULL;
    }
    memcpy(copy, value, len);
    return copy;
}

/*
 * compare_strings provides stable lexicographic order for directory walks.
 */
static int compare_strings(const void *left, const void *right) {
    const char *const *a = left;
    const char *const *b = right;
    return strcmp(*a, *b);
}

/*
 * fs_exists reports whether a path exists without following symlinks.
 */
int fs_exists(const char *path) {
    struct stat st;
    return stat_path(path, &st);
}

/*
 * fs_is_file reports whether a path points to a regular file.
 */
int fs_is_file(const char *path) {
    struct stat st;
    return stat_path(path, &st) && S_ISREG(st.st_mode);
}

/*
 * fs_is_dir reports whether a path points to a directory.
 */
int fs_is_dir(const char *path) {
    struct stat st;
    return stat_path(path, &st) && S_ISDIR(st.st_mode);
}

/*
 * fs_mkdir creates one directory and treats an existing directory as success.
 */
MGResult fs_mkdir(const char *path) {
    if (mkdir(path, 0777) == 0) {
        return MG_OK;
    }
    if (errno == EEXIST && fs_is_dir(path)) {
        return MG_OK;
    }
    return MG_IO_ERROR;
}

/*
 * fs_mkdir_p creates every missing component in a directory path.
 */
MGResult fs_mkdir_p(const char *path) {
    char current[MG_MAX_PATH];
    size_t len;

    if (path == NULL || path[0] == '\0') {
        return MG_INVALID_ARG;
    }
    if (strlen(path) >= sizeof(current)) {
        return MG_INVALID_ARG;
    }

    strcpy(current, path);
    len = strlen(current);
    if (len > 1 && current[len - 1] == '/') {
        current[len - 1] = '\0';
    }

    for (char *p = current + 1; *p != '\0'; p++) {
        if (*p == '/') {
            *p = '\0';
            if (fs_mkdir(current) != MG_OK) {
                return MG_IO_ERROR;
            }
            *p = '/';
        }
    }

    return fs_mkdir(current);
}

/*
 * fs_read_file reads a whole file in binary mode and returns a byte buffer.
 */
MGResult fs_read_file(const char *path, unsigned char **out_data, size_t *out_size) {
    FILE *file;
    long end;
    unsigned char *data;
    size_t size;

    if (path == NULL || out_data == NULL || out_size == NULL) {
        return MG_INVALID_ARG;
    }

    *out_data = NULL;
    *out_size = 0;
    file = fopen(path, "rb");
    if (file == NULL) {
        return MG_IO_ERROR;
    }
    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        return MG_IO_ERROR;
    }
    end = ftell(file);
    if (end < 0) {
        fclose(file);
        return MG_IO_ERROR;
    }
    if (fseek(file, 0, SEEK_SET) != 0) {
        fclose(file);
        return MG_IO_ERROR;
    }

    size = (size_t)end;
    data = malloc(size + 1);
    if (data == NULL) {
        fclose(file);
        return MG_ERROR;
    }
    if (size > 0 && fread(data, 1, size, file) != size) {
        free(data);
        fclose(file);
        return MG_IO_ERROR;
    }
    data[size] = '\0';

    if (fclose(file) != 0) {
        free(data);
        return MG_IO_ERROR;
    }

    *out_data = data;
    *out_size = size;
    return MG_OK;
}

/*
 * fs_write_file writes exactly size bytes in binary mode.
 */
MGResult fs_write_file(const char *path, const unsigned char *data, size_t size) {
    FILE *file;

    if (path == NULL || (data == NULL && size > 0)) {
        return MG_INVALID_ARG;
    }

    file = fopen(path, "wb");
    if (file == NULL) {
        return MG_IO_ERROR;
    }
    if (size > 0 && fwrite(data, 1, size, file) != size) {
        fclose(file);
        return MG_IO_ERROR;
    }
    if (fclose(file) != 0) {
        return MG_IO_ERROR;
    }

    return MG_OK;
}

/*
 * fs_join_path joins two path components without normalizing the result.
 */
MGResult fs_join_path(const char *left, const char *right, char *out, size_t out_size) {
    int written;
    int needs_slash;

    if (left == NULL || right == NULL || out == NULL || out_size == 0) {
        return MG_INVALID_ARG;
    }

    needs_slash = left[0] != '\0' && left[strlen(left) - 1] != '/';
    written = snprintf(out, out_size, needs_slash ? "%s/%s" : "%s%s", left, right);
    if (written < 0 || (size_t)written >= out_size) {
        return MG_INVALID_ARG;
    }

    return MG_OK;
}

/*
 * fs_parent_dir extracts the directory part of a path.
 */
MGResult fs_parent_dir(const char *path, char *out, size_t out_size) {
    const char *slash;
    size_t len;

    if (path == NULL || out == NULL || out_size == 0) {
        return MG_INVALID_ARG;
    }

    slash = strrchr(path, '/');
    if (slash == NULL) {
        if (out_size < 2) {
            return MG_INVALID_ARG;
        }
        strcpy(out, ".");
        return MG_OK;
    }

    len = (size_t)(slash - path);
    if (len == 0) {
        len = 1;
    }
    if (len >= out_size) {
        return MG_INVALID_ARG;
    }

    memcpy(out, path, len);
    out[len] = '\0';
    return MG_OK;
}

/*
 * fs_remove_file removes a file path from the working tree.
 */
MGResult fs_remove_file(const char *path) {
    if (path == NULL) {
        return MG_INVALID_ARG;
    }
    if (unlink(path) != 0) {
        return MG_IO_ERROR;
    }
    return MG_OK;
}

/*
 * path_has_forbidden_segment rejects traversal and .minigit storage paths.
 */
static int path_has_forbidden_segment(const char *path) {
    const char *segment = path;

    while (*segment != '\0') {
        const char *end = strchr(segment, '/');
        size_t len = end == NULL ? strlen(segment) : (size_t)(end - segment);

        if ((len == 2 && strncmp(segment, "..", len) == 0) ||
            (len == 8 && strncmp(segment, ".minigit", len) == 0)) {
            return 1;
        }

        if (end == NULL) {
            break;
        }
        segment = end + 1;
    }

    return 0;
}

/*
 * fs_repo_relative_path converts user input to a stable repo-relative path.
 */
MGResult fs_repo_relative_path(const char *repo_root, const char *path, char *out, size_t out_size) {
    const char *relative = path;
    size_t root_len;
    size_t len;

    if (repo_root == NULL || path == NULL || out == NULL || out_size == 0 || path[0] == '\0') {
        return MG_INVALID_ARG;
    }

    root_len = strlen(repo_root);
    if (path[0] == '/') {
        if (strncmp(path, repo_root, root_len) != 0 || (path[root_len] != '\0' && path[root_len] != '/')) {
            return MG_INVALID_ARG;
        }
        relative = path + root_len;
        if (*relative == '/') {
            relative++;
        }
    }

    while (relative[0] == '.' && relative[1] == '/') {
        relative += 2;
    }
    if (strcmp(relative, ".") == 0) {
        relative = "";
    }
    if (relative[0] == '/' || path_has_forbidden_segment(relative)) {
        return MG_INVALID_ARG;
    }

    len = strlen(relative);
    if (len >= out_size) {
        return MG_INVALID_ARG;
    }

    memcpy(out, relative, len + 1);
    return MG_OK;
}

/*
 * walk_dir recursively visits regular files in sorted order.
 */
static MGResult walk_dir(const char *root_path, const char *relative_path, FSWalkCallback callback, void *ctx) {
    char dir_path[MG_MAX_PATH];
    DIR *dir;
    struct dirent *entry;
    char **names = NULL;
    size_t count = 0;
    size_t capacity = 0;
    MGResult result = MG_OK;

    if (relative_path[0] == '\0') {
        if (snprintf(dir_path, sizeof(dir_path), "%s", root_path) >= (int)sizeof(dir_path)) {
            return MG_INVALID_ARG;
        }
    } else if (fs_join_path(root_path, relative_path, dir_path, sizeof(dir_path)) != MG_OK) {
        return MG_INVALID_ARG;
    }

    dir = opendir(dir_path);
    if (dir == NULL) {
        return MG_IO_ERROR;
    }

    while ((entry = readdir(dir)) != NULL) {
        char **grown;

        if (is_dot_entry(entry->d_name) || strcmp(entry->d_name, ".minigit") == 0) {
            continue;
        }

        if (count == capacity) {
            capacity = capacity == 0 ? 16 : capacity * 2;
            grown = realloc(names, capacity * sizeof(*names));
            if (grown == NULL) {
                result = MG_ERROR;
                break;
            }
            names = grown;
        }

        names[count] = copy_string(entry->d_name);
        if (names[count] == NULL) {
            result = MG_ERROR;
            break;
        }
        count++;
    }

    if (closedir(dir) != 0 && result == MG_OK) {
        result = MG_IO_ERROR;
    }
    if (result == MG_OK) {
        qsort(names, count, sizeof(*names), compare_strings);
    }

    for (size_t i = 0; i < count && result == MG_OK; i++) {
        char child_relative[MG_MAX_PATH];
        char child_path[MG_MAX_PATH];
        struct stat st;

        if (relative_path[0] == '\0') {
            if (snprintf(child_relative, sizeof(child_relative), "%s", names[i]) >= (int)sizeof(child_relative)) {
                result = MG_INVALID_ARG;
                break;
            }
        } else if (fs_join_path(relative_path, names[i], child_relative, sizeof(child_relative)) != MG_OK) {
            result = MG_INVALID_ARG;
            break;
        }

        if (fs_join_path(root_path, child_relative, child_path, sizeof(child_path)) != MG_OK) {
            result = MG_INVALID_ARG;
            break;
        }
        if (lstat(child_path, &st) != 0) {
            result = MG_IO_ERROR;
            break;
        }
        if (S_ISDIR(st.st_mode)) {
            result = walk_dir(root_path, child_relative, callback, ctx);
        } else if (S_ISREG(st.st_mode)) {
            result = callback(child_relative, ctx);
        }
    }

    for (size_t i = 0; i < count; i++) {
        free(names[i]);
    }
    free(names);
    return result;
}

/*
 * fs_walk starts a repository-relative walk that skips .minigit.
 */
MGResult fs_walk(const char *root_path, FSWalkCallback callback, void *ctx) {
    if (root_path == NULL || callback == NULL) {
        return MG_INVALID_ARG;
    }
    return walk_dir(root_path, "", callback, ctx);
}
