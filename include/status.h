#ifndef MINIGIT_STATUS_H
#define MINIGIT_STATUS_H

#include "common.h"
#include "repository.h"

/*
 * status_print computes and prints the current repository status.
 */
MGResult status_print(const Repository *repo);

#endif
