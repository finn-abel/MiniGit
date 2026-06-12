#include <stdio.h>
#include <string.h>

#include "commands.h"
#include "common.h"

/*
 * print_usage shows the top-level command shape for invalid CLI input.
 */
static void print_usage(void) {
    puts("Usage: minigit <command> [args]");
}

/*
 * result_to_exit_code keeps process exit codes at the command boundary.
 */
static int result_to_exit_code(MGResult result) {
    return result == MG_OK ? 0 : 1;
}

/*
 * main parses the command name and dispatches to the command layer.
 */
int main(int argc, char **argv) {
    if (argc < 2) {
        print_usage();
        return 1;
    }

    const char *command = argv[1];
    MGResult result = MG_INVALID_ARG;

    if (strcmp(command, "init") == 0) {
        result = mg_command_init(argc - 2, argv + 2);
    } else if (strcmp(command, "add") == 0) {
        result = mg_command_add(argc - 2, argv + 2);
    } else if (strcmp(command, "rm") == 0) {
        result = mg_command_rm(argc - 2, argv + 2);
    } else if (strcmp(command, "status") == 0) {
        result = mg_command_status(argc - 2, argv + 2);
    } else if (strcmp(command, "diff") == 0) {
        result = mg_command_diff(argc - 2, argv + 2);
    } else if (strcmp(command, "commit") == 0) {
        result = mg_command_commit(argc - 2, argv + 2);
    } else if (strcmp(command, "log") == 0) {
        result = mg_command_log(argc - 2, argv + 2);
    } else if (strcmp(command, "show") == 0) {
        result = mg_command_show(argc - 2, argv + 2);
    } else if (strcmp(command, "branch") == 0) {
        result = mg_command_branch(argc - 2, argv + 2);
    } else if (strcmp(command, "merge") == 0) {
        result = mg_command_merge(argc - 2, argv + 2);
    } else if (strcmp(command, "switch") == 0) {
        result = mg_command_switch(argc - 2, argv + 2);
    } else if (strcmp(command, "checkout") == 0) {
        result = mg_command_checkout(argc - 2, argv + 2);
    } else if (strcmp(command, "restore") == 0) {
        result = mg_command_restore(argc - 2, argv + 2);
    } else if (strcmp(command, "reset") == 0) {
        result = mg_command_reset(argc - 2, argv + 2);
    } else {
        fprintf(stderr, "unknown command: %s\n", command);
        print_usage();
    }

    return result_to_exit_code(result);
}
