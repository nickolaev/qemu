/*
 * Linux Multikernel host control interface
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef MULTIKERNEL_HOST_H
#define MULTIKERNEL_HOST_H

#include "qapi/error.h"

#define MULTIKERNEL_MIN_ID 1
#define MULTIKERNEL_MAX_ID 511

char *multikernel_host_find_instance(unsigned int id, Error **errp);
char *multikernel_host_read_status(const char *instance_name, Error **errp);
bool multikernel_host_wait_status(const char *instance_name,
                                  const char *expected,
                                  unsigned int timeout_ms,
                                  Error **errp);
int multikernel_host_load(unsigned int id, const char *kernel,
                          const char *initrd, const char *cmdline,
                          Error **errp);
int multikernel_host_exec(unsigned int id, Error **errp);
int multikernel_host_halt(unsigned int id, bool force, Error **errp);
int multikernel_host_unload(unsigned int id, Error **errp);
int multikernel_host_open_console(unsigned int id, Error **errp);

#endif
