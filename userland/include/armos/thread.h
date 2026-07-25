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
 *
 * Notes:
 * - This interface is native ArmOS API, not Linux clone compatibility.
 * - POSIX applications should normally include pthread.h instead.
 */

#ifndef _ARMOS_THREAD_H
#define _ARMOS_THREAD_H

#include <stdint.h>
#include <uapi/armos/futex.h>
#include <uapi/armos/thread.h>
#include <uapi/armos/time.h>
#include <uapi/armos/tls.h>

int armos_clone(const armos_clone_args_t *args);
int armos_gettid(void);
void armos_thread_exit(int status) __attribute__((noreturn));
int armos_futex_wait(volatile uint32_t *address, uint32_t expected,
                     const armos_timespec_t *timeout);
int armos_futex_wake(volatile uint32_t *address, uint32_t count);
int armos_set_tls(void *tls_base);
int armos_get_tls_info(armos_tls_info_t *info);
void *armos_thread_reent_create(void);
void armos_thread_reent_destroy(void *reent);
void __armos_runtime_init(void);

#endif /* _ARMOS_THREAD_H */
