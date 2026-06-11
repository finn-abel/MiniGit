#include <stdio.h>

#include "commands.h"

/*
 * ignore_args keeps temporary stubs warning-free until real validation exists.
 */
static void ignore_args(int argc, char **argv) {
    (void)argc;
    (void)argv;
}

/*
 * mg_command_init handles `minigit init`.
 */
MGResult mg_command_init(int argc, char **argv) {
    ignore_args(argc, argv);
    puts("init not implemented yet");
    return MG_OK;
}

/*
 * mg_command_add handles `minigit add <path>...`.
 */
MGResult mg_command_add(int argc, char **argv) {
    ignore_args(argc, argv);
    puts("add not implemented yet");
    return MG_OK;
}

/*
 * mg_command_rm handles `minigit rm <path>...`.
 */
MGResult mg_command_rm(int argc, char **argv) {
    ignore_args(argc, argv);
    puts("rm not implemented yet");
    return MG_OK;
}

/*
 * mg_command_status handles `minigit status`.
 */
MGResult mg_command_status(int argc, char **argv) {
    ignore_args(argc, argv);
    puts("status not implemented yet");
    return MG_OK;
}

/*
 * mg_command_commit handles `minigit commit -m <message>`.
 */
MGResult mg_command_commit(int argc, char **argv) {
    ignore_args(argc, argv);
    puts("commit not implemented yet");
    return MG_OK;
}

/*
 * mg_command_log handles `minigit log`.
 */
MGResult mg_command_log(int argc, char **argv) {
    ignore_args(argc, argv);
    puts("log not implemented yet");
    return MG_OK;
}

/*
 * mg_command_branch handles `minigit branch [name]`.
 */
MGResult mg_command_branch(int argc, char **argv) {
    ignore_args(argc, argv);
    puts("branch not implemented yet");
    return MG_OK;
}

/*
 * mg_command_switch handles `minigit switch <branch>`.
 */
MGResult mg_command_switch(int argc, char **argv) {
    ignore_args(argc, argv);
    puts("switch not implemented yet");
    return MG_OK;
}

/*
 * mg_command_checkout handles `minigit checkout <commit_hash>`.
 */
MGResult mg_command_checkout(int argc, char **argv) {
    ignore_args(argc, argv);
    puts("checkout not implemented yet");
    return MG_OK;
}
