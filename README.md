# MiniGit-C

## Overview

MiniGit-C is a small educational version control system written in C11.
It is being built as a local-only subset of Git.

## Build

```sh
make
```

## Usage

```sh
minigit <command> [args]
```

Initial command list:

```sh
minigit init
minigit add <path>...
minigit rm <path>...
minigit status
minigit commit -m "message"
minigit log
minigit branch
minigit branch <name>
minigit switch <branch>
minigit checkout <commit_hash>
```

These commands are currently scaffolded and print "not implemented yet".
