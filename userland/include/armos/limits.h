/*
 * ArmOS
 * Copyright (c) 2026 Mohamed Ennassiri
 *
 * Licensed under the Apache License, Version 2.0.
 * See LICENSE for details.
 *
 * File: userland/include/armos/limits.h
 * Layer: Userland / POSIX limits compatibility
 *
 * Responsibilities:
 * - Publish POSIX compile-time limits omitted by bare-metal newlib.
 * - Keep portable applications independent from target sysroot internals.
 */

#ifndef ARMOS_LIMITS_COMPAT_H
#define ARMOS_LIMITS_COMPAT_H

#include <limits.h>
#include <uapi/armos/limits.h>

#ifndef ARG_MAX
#define ARG_MAX ARMOS_ARG_MAX
#endif

#ifndef PATH_MAX
#define PATH_MAX ARMOS_PATH_MAX
#endif

#ifndef NAME_MAX
#define NAME_MAX ARMOS_NAME_MAX
#endif

#ifndef _POSIX_HOST_NAME_MAX
#define _POSIX_HOST_NAME_MAX 255
#endif

#endif /* ARMOS_LIMITS_COMPAT_H */
