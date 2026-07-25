/*
 * ArmOS
 * Copyright (c) 2026 Mohamed Ennassiri
 *
 * Licensed under the Apache License, Version 2.0.
 * See LICENSE for details.
 *
 * File: userland/include/armos/thread.h
 * Layer: Userland / native thread foundation
 *
 * Responsibilities:
 * - Expose the native thread primitives used to bootstrap libpthread.
 * - Keep applications away from raw syscall entry symbols.
 */

#ifndef _ARMOS_THREAD_H
#define _ARMOS_THREAD_H

#include <uapi/armos/thread.h>

int armos_clone(const armos_clone_args_t *args);
int armos_gettid(void);
void armos_thread_exit(int status) __attribute__((noreturn));

#endif /* _ARMOS_THREAD_H */
