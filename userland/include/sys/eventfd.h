/*
 * ArmOS
 * Copyright (c) 2026 Mohamed Ennassiri
 *
 * Licensed under the Apache License, Version 2.0.
 * See LICENSE for details.
 *
 * File: userland/include/sys/eventfd.h
 * Layer: Userland / descriptor compatibility
 *
 * Responsibilities:
 * - Declare the event counter descriptor interface.
 * - Publish flags shared with ArmOS descriptor semantics.
 */

#ifndef ARMOS_SYS_EVENTFD_H
#define ARMOS_SYS_EVENTFD_H

#include <fcntl.h>
#include <stdint.h>

typedef uint64_t eventfd_t;

#define EFD_SEMAPHORE 1
#define EFD_NONBLOCK  O_NONBLOCK
#define EFD_CLOEXEC   O_CLOEXEC

int eventfd(unsigned int initial_value, int flags);
int eventfd_read(int fd, eventfd_t *value);
int eventfd_write(int fd, eventfd_t value);

#endif /* ARMOS_SYS_EVENTFD_H */
