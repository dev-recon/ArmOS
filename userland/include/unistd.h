/*
 * ArmOS
 * Copyright (c) 2026 Mohamed Ennassiri
 *
 * Licensed under the Apache License, Version 2.0.
 * See LICENSE for details.
 *
 * File: userland/include/unistd.h
 * Layer: Userland / POSIX compatibility
 *
 * Responsibilities:
 * - Extend newlib's unistd declarations with ArmOS-supported interfaces.
 * - Keep common application sources independent from kernel ABI details.
 */

#ifndef ARMOS_UNISTD_H
#define ARMOS_UNISTD_H

#include_next <unistd.h>

int pipe2(int pipefd[2], int flags);
int execlp(const char *file, const char *arg0, ...);

#endif /* ARMOS_UNISTD_H */
