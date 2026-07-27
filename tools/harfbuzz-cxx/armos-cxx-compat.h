/*
 * ArmOS
 * Copyright (c) 2026 Mohamed Ennassiri
 *
 * Licensed under the Apache License, Version 2.0.
 * See LICENSE for details.
 *
 * File: tools/harfbuzz-cxx/armos-cxx-compat.h
 * Layer: Host tooling / HarfBuzz cross-build compatibility
 *
 * Responsibilities:
 * - Complete the newlib declarations expected by build-only libc++ headers.
 * - Keep C++ compatibility details outside the ArmOS target headers.
 *
 * Notes:
 * - HarfBuzz does not call timespec_get in this configuration.
 * - This file is force-included only while compiling harfbuzz.cc.
 */

#ifndef ARMOS_HARFBUZZ_CXX_COMPAT_H
#define ARMOS_HARFBUZZ_CXX_COMPAT_H

#include <time.h>

#ifndef TIME_UTC
#define TIME_UTC 1
#endif

#ifdef __cplusplus
extern "C" {
#endif

int timespec_get(struct timespec *ts, int base);

#ifdef __cplusplus
}
#endif

#endif
