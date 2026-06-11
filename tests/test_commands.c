#include <stdio.h>
#include <stdlib.h>

#include "commands.h"
#include "common.h"

static void assert_ok(MGResult result, const char *name) {
    if (result != MG_OK) {
        fprintf(stderr, "%s failed\n", name);
        exit(1);
    }
}

static void test_command_stubs_return_ok(void) {
    assert_ok(mg_command_init(0, NULL), "init");
    assert_ok(mg_command_add(0, NULL), "add");
    assert_ok(mg_command_rm(0, NULL), "rm");
    assert_ok(mg_command_status(0, NULL), "status");
    assert_ok(mg_command_commit(0, NULL), "commit");
    assert_ok(mg_command_log(0, NULL), "log");
    assert_ok(mg_command_branch(0, NULL), "branch");
    assert_ok(mg_command_switch(0, NULL), "switch");
    assert_ok(mg_command_checkout(0, NULL), "checkout");
}

int main(void) {
    test_command_stubs_return_ok();
    puts("All commands tests passed.");
    return 0;
}
