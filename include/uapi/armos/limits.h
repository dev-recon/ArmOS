/*
 * ArmOS
 * Copyright (c) 2026 Mohamed Ennassiri
 *
 * Licensed under the Apache License, Version 2.0.
 * See LICENSE for details.
 *
 * File: include/uapi/armos/limits.h
 * Layer: UAPI / POSIX limits
 *
 * Responsibilities:
 * - Publish limits contractual across the kernel and userland.
 * - Keep sysconf values consistent with syscall enforcement.
 */

#ifndef _UAPI_ARMOS_LIMITS_H
#define _UAPI_ARMOS_LIMITS_H

#define ARMOS_ARG_MAX 65536u
#define ARMOS_PATH_MAX 1024u
#define ARMOS_NAME_MAX 255u

/*
 * A process may enter a directory whose absolute physical name is longer
 * than PATH_MAX by resolving short relative components.  Keep one complete
 * component of headroom for the kernel's canonical path representation.
 */
#define ARMOS_RESOLVED_PATH_MAX \
    (ARMOS_PATH_MAX + ARMOS_NAME_MAX + 1u)

#endif /* _UAPI_ARMOS_LIMITS_H */
