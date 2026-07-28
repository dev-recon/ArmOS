/*
 * ArmOS
 * Copyright (c) 2026 Mohamed Ennassiri
 *
 * Licensed under the Apache License, Version 2.0.
 * See LICENSE for details.
 *
 * File: userland/include/armos/unistd.h
 * Layer: Userland / POSIX process compatibility
 *
 * Responsibilities:
 * - Declare POSIX process helpers omitted by the bare-metal newlib profile.
 * - Provide a stable include for third-party application builds.
 */

#ifndef ARMOS_UNISTD_COMPAT_H
#define ARMOS_UNISTD_COMPAT_H

#include <stddef.h>
#include <unistd.h>

int gethostname(char *name, size_t length);

#endif /* ARMOS_UNISTD_COMPAT_H */
