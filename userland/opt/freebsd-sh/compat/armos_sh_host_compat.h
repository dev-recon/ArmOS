/*
 * ArmOS
 * Copyright (c) 2026 Mohamed Ennassiri
 *
 * Licensed under the Apache License, Version 2.0.
 * See LICENSE for details.
 *
 * File: userland/opt/freebsd-sh/compat/armos_sh_host_compat.h
 * Layer: Host tooling / FreeBSD sh generators
 *
 * Responsibilities:
 * - Supply FreeBSD compiler annotations to native sh build generators.
 * - Keep host portability independent from the ArmOS target compatibility ABI.
 */

#ifndef ARMOS_SH_HOST_COMPAT_H
#define ARMOS_SH_HOST_COMPAT_H

#if defined(__GNUC__) || defined(__clang__)
#define ARMOS_HOST_ATTRIBUTE(value) __attribute__((value))
#else
#define ARMOS_HOST_ATTRIBUTE(value)
#endif

#ifndef __dead2
#define __dead2 ARMOS_HOST_ATTRIBUTE(__noreturn__)
#endif
#ifndef __unused
#define __unused ARMOS_HOST_ATTRIBUTE(__unused__)
#endif
#ifndef __printf0like
#define __printf0like(format_index, first_argument) \
    ARMOS_HOST_ATTRIBUTE(__format__(__printf__, format_index, first_argument))
#endif

#endif /* ARMOS_SH_HOST_COMPAT_H */
