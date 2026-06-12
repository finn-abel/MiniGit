# MiniGit-C

MiniGit-C is a small educational version control system written in C11. It
implements a local-only subset of Git so the core pieces of a VCS are visible in
plain C: repository metadata, an index, content-addressed objects, commits,
status, diffs, checkout, branches, and simple merges.

This project is not a replacement for Git. It is intentionally compact and is
best treated as a learning implementation.

## Requirements

- A C11 compiler such as `gcc` or `clang`
- `make`
- OpenSSL development headers and libraries
- zlib development headers and libraries

On macOS with Homebrew, the `Makefile` automatically looks for OpenSSL under
common Homebrew prefixes such as `/opt/homebrew/opt/openssl@3`.

## Build

```sh
make
```

The build creates the `minigit` executable in the project root.

## Test

```sh
make test
```

The test target builds and runs the unit tests, then removes generated binaries
and object files with `make clean`.

## Quick Start

```sh
make
mkdir demo
cd demo
../minigit init
printf "hello\n" > hello.txt
../minigit add hello.txt
../minigit commit -m "initial commit"
../minigit status
```

## Usage

```sh
minigit <command> [args]
```

Supported commands:

```sh
minigit init
minigit add <path>...
minigit rm <path>...
minigit status
minigit diff [--staged|--cached|HEAD]
minigit commit -m "message"
minigit log
minigit show <commit>
minigit pack
minigit branch
minigit branch <name>
minigit branch -d <name>
minigit merge <branch>
minigit switch <branch>
minigit checkout <commit>
minigit checkout <commit> -- <path>
minigit restore <path>
minigit reset <path>
```

Commit arguments can be full object ids or unique hexadecimal prefixes of at
least 7 characters.

## Command Notes

- `init` creates `.minigit`, `.minigit/objects`, `.minigit/refs/heads`, an empty
  index, and an unborn `main` branch.
- `add` stages regular files or recursively stages files under a directory.
- `rm` removes tracked files from the working tree and index.
- `status` reports staged changes, unstaged tracked changes, and untracked files.
- `diff` prints line-based diffs for tracked files. With no argument it compares
  the index to the working tree, `--staged` and `--cached` compare `HEAD` to the
  index, and `HEAD` compares `HEAD` to the working tree.
- `commit -m` writes a tree and commit from the current index, then advances the
  current branch or detached `HEAD`.
- `log` walks first parents from the current commit.
- `show` prints commit metadata and the commit's tree entries.
- `pack` writes a simple MiniGit packfile at
  `.minigit/objects/pack/minigit.pack`.
- `branch` lists local branches, creates a branch at the current commit, or
  deletes a non-current branch with `-d`.
- `switch` moves to a named branch and restores that branch's commit.
- `checkout <commit>` restores a commit and enters detached `HEAD` state.
- `checkout <commit> -- <path>` restores one path from a commit without moving
  `HEAD`.
- `restore <path>` restores a tracked path from `HEAD` into both the index and
  working tree.
- `reset <path>` restores a path's index entry from `HEAD` without changing the
  working tree.
- `merge <branch>` performs a simple three-way merge. Conflicting file contents
  are written with conflict markers and the command exits with a conflict result.

## Configuration

Commits use these environment variables when present:

```sh
MINIGIT_AUTHOR_NAME="Ada Lovelace"
MINIGIT_AUTHOR_EMAIL="ada@example.com"
```

Objects are SHA-256 by default. For tests or experiments that need Git-style
SHA-1 object ids, set:

```sh
MINIGIT_OBJECT_HASH_MODE=git
```

## Ignore Rules

MiniGit reads ignore rules from `.minigitignore` in the repository root. The
supported pattern language is intentionally small:

- Exact repository-relative file paths, such as `build/output.o`
- Directory prefixes ending in `/`, such as `build/`
- Blank lines are ignored

## Repository Layout

MiniGit stores all repository metadata under `.minigit`:

```text
.minigit/
  HEAD
  index
  objects/
  refs/
    heads/
```

Loose objects are zlib-compressed and stored under `.minigit/objects` using a
fanout directory based on the object id. Branches are flat files under
`.minigit/refs/heads`.

## Current Limitations

- Local repositories only; there are no remotes, fetch, pull, or push commands.
- Branch names are a small flat subset: no empty names, slashes, `..`, spaces, or
  control characters.
- The merge implementation is intentionally simple and does not provide the full
  strategy set or conflict-resolution workflow of Git.
- The packfile format is MiniGit-specific.
