/*
 * ArmOS
 * Copyright (c) 2026 Mohamed Ennassiri
 *
 * Licensed under the Apache License, Version 2.0.
 * See LICENSE for details.
 *
 * File: newlib-port/pthread_internal.h
 * Layer: Userspace / POSIX threads internals
 *
 * Responsibilities:
 * - Share private pthread control structures across lifecycle and
 *   synchronization translation units.
 * - Centralize cancellation, cleanup and futex helper contracts.
 *
 * Notes:
 * - This header is private to the ArmOS newlib port.
 * - Its structures are not part of the public pthread ABI.
 */

#ifndef _ARMOS_PTHREAD_INTERNAL_H
#define _ARMOS_PTHREAD_INTERNAL_H

#include <armos/thread.h>
#include <pthread.h>
#include <stdint.h>
#include <time.h>

#define ARMOS_PTHREAD_MAX_THREADS 128u
#define ARMOS_PTHREAD_MAX_KEYS 64u
#define ARMOS_PTHREAD_DESTRUCTOR_ITERATIONS 4u

typedef struct armos_pthread_control {
    pthread_t tid;
    volatile uint32_t alive_tid;
    volatile uint32_t finished;
    volatile uint32_t joined;
    volatile uint32_t detached;
    volatile uint32_t cancel_pending;
    int cancel_state;
    int cancel_type;
    volatile uint32_t *cancel_address;
    volatile uint32_t cancel_address_changes;
    void *result;
    void *stack_allocation;
    void *stack_base;
    size_t stack_size;
    int owns_stack;
    void *tls;
    void *(*start_routine)(void *);
    void *argument;
    struct _pthread_cleanup_context *cleanup;
} armos_pthread_control_t;

extern __thread armos_pthread_control_t *__armos_pthread_current;
extern __thread void *__armos_pthread_tsd[ARMOS_PTHREAD_MAX_KEYS];

void __armos_pthread_registry_lock(void);
void __armos_pthread_registry_unlock(void);
int __armos_pthread_wait(volatile uint32_t *address, uint32_t expected,
                         const armos_timespec_t *timeout,
                         int cancellation_point);
int __armos_pthread_relative_timeout(clockid_t clock_id,
                                     const struct timespec *absolute,
                                     armos_timespec_t *relative);
void __armos_pthread_release_robust(uint32_t tid);

#endif /* _ARMOS_PTHREAD_INTERNAL_H */
