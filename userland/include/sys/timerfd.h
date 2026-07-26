/*
 * ArmOS
 * Copyright (c) 2026 Mohamed Ennassiri
 *
 * Licensed under the Apache License, Version 2.0.
 * See LICENSE for details.
 *
 * File: userland/include/sys/timerfd.h
 * Layer: Userland / descriptor compatibility
 *
 * Responsibilities:
 * - Declare monotonic timer descriptors used by event loops.
 * - Publish flags compatible with common POSIX application sources.
 */

#ifndef ARMOS_SYS_TIMERFD_H
#define ARMOS_SYS_TIMERFD_H

#include <fcntl.h>
#include <time.h>

#define TFD_NONBLOCK      O_NONBLOCK
#define TFD_CLOEXEC       O_CLOEXEC
#define TFD_TIMER_ABSTIME 1

int timerfd_create(clockid_t clock_id, int flags);
int timerfd_settime(int fd, int flags, const struct itimerspec *new_value,
                    struct itimerspec *old_value);
int timerfd_gettime(int fd, struct itimerspec *current_value);

#endif /* ARMOS_SYS_TIMERFD_H */
