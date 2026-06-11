#ifndef MINIGIT_COMMANDS_H
#define MINIGIT_COMMANDS_H

#include "common.h"

/*
 * mg_command_init handles `minigit init`.
 */
MGResult mg_command_init(int argc, char **argv);

/*
 * mg_command_add handles `minigit add <path>...`.
 */
MGResult mg_command_add(int argc, char **argv);

/*
 * mg_command_rm handles `minigit rm <path>...`.
 */
MGResult mg_command_rm(int argc, char **argv);

/*
 * mg_command_status handles `minigit status`.
 */
MGResult mg_command_status(int argc, char **argv);

/*
 * mg_command_diff handles `minigit diff [--staged|--cached|HEAD]`.
 */
MGResult mg_command_diff(int argc, char **argv);

/*
 * mg_command_commit handles `minigit commit -m <message>`.
 */
MGResult mg_command_commit(int argc, char **argv);

/*
 * mg_command_log handles `minigit log`.
 */
MGResult mg_command_log(int argc, char **argv);

/*
 * mg_command_branch handles `minigit branch [name]`.
 */
MGResult mg_command_branch(int argc, char **argv);

/*
 * mg_command_switch handles `minigit switch <branch>`.
 */
MGResult mg_command_switch(int argc, char **argv);

/*
 * mg_command_checkout handles `minigit checkout <commit_hash>`.
 */
MGResult mg_command_checkout(int argc, char **argv);

/*
 * mg_command_restore handles `minigit restore <path>`.
 */
MGResult mg_command_restore(int argc, char **argv);

/*
 * mg_command_reset handles `minigit reset <path>`.
 */
MGResult mg_command_reset(int argc, char **argv);

#endif
