/*
 * ArmOS
 * Copyright (c) 2026 Mohamed Ennassiri
 *
 * Licensed under the Apache License, Version 2.0.
 * See LICENSE for details.
 *
 * File: userland/include/armos/stdio.h
 * Layer: Userland / POSIX stdio compatibility
 *
 * Responsibilities:
 * - Declare POSIX line-reading functions not declared by this newlib profile.
 * - Keep third-party source builds independent from generated sysroot edits.
 *
 * Notes:
 * - Include this header after the standard <stdio.h> and <sys/types.h>.
 */

#ifndef ARMOS_STDIO_COMPAT_H
#define ARMOS_STDIO_COMPAT_H

#include <stdio.h>
#include <sys/types.h>

ssize_t getdelim(char **line, size_t *capacity, int delimiter, FILE *stream);
ssize_t getline(char **line, size_t *capacity, FILE *stream);

#endif /* ARMOS_STDIO_COMPAT_H */
