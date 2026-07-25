/*
 * ArmOS
 * Copyright (c) 2026 Mohamed Ennassiri
 *
 * Licensed under the Apache License, Version 2.0.
 * See LICENSE for details.
 *
 * File: userland/include/pthread.h
 * Layer: Userland / C library compatibility
 *
 * Responsibilities:
 * - Enable newlib's portable pthread declarations for ArmOS.
 * - Publish ArmOS-supported feature levels and robust mutex extensions.
 *
 * Notes:
 * - The implementation lives in the architecture-neutral newlib port.
 * - Process-shared synchronization is deliberately reported unsupported.
 */

#ifndef _ARMOS_PTHREAD_WRAPPER_H
#define _ARMOS_PTHREAD_WRAPPER_H

#ifndef _POSIX_THREADS
#define _POSIX_THREADS 200809L
#endif
#ifndef _POSIX_TIMEOUTS
#define _POSIX_TIMEOUTS 200809L
#endif
#ifndef _POSIX_THREAD_PROCESS_SHARED
#define _POSIX_THREAD_PROCESS_SHARED -1
#endif
#ifndef _POSIX_BARRIERS
#define _POSIX_BARRIERS 200809L
#endif
#ifndef _POSIX_READER_WRITER_LOCKS
#define _POSIX_READER_WRITER_LOCKS 200809L
#endif
#ifndef _POSIX_SPIN_LOCKS
#define _POSIX_SPIN_LOCKS 200809L
#endif
#ifndef _UNIX98_THREAD_MUTEX_ATTRIBUTES
#define _UNIX98_THREAD_MUTEX_ATTRIBUTES 1
#endif

#include_next <pthread.h>

#ifndef PTHREAD_STACK_MIN
#define PTHREAD_STACK_MIN 16384
#endif

#define PTHREAD_MUTEX_STALLED 0
#define PTHREAD_MUTEX_ROBUST  1

int pthread_mutexattr_getrobust(const pthread_mutexattr_t *attr,
                                int *robustness);
int pthread_mutexattr_setrobust(pthread_mutexattr_t *attr, int robustness);
int pthread_mutex_consistent(pthread_mutex_t *mutex);

#endif /* _ARMOS_PTHREAD_WRAPPER_H */
