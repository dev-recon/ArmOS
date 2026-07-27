/*
 * ArmOS
 * Copyright (c) 2026 Mohamed Ennassiri
 *
 * Licensed under the Apache License, Version 2.0.
 * See LICENSE for details.
 *
 * File: userland/include/armos/pthread.h
 * Layer: Userland / pthread compatibility
 *
 * Responsibilities:
 * - Declare optional pthread extensions used by portable applications.
 * - Keep nonessential thread naming outside the kernel ABI.
 */

#ifndef ARMOS_PTHREAD_COMPAT_H
#define ARMOS_PTHREAD_COMPAT_H

#include <pthread.h>

int pthread_setname_np(pthread_t thread, const char *name);

#endif /* ARMOS_PTHREAD_COMPAT_H */
