/*
 * ArmOS
 * Copyright (c) 2026 Mohamed Ennassiri
 *
 * Licensed under the Apache License, Version 2.0.
 * See LICENSE for details.
 *
 * File: userland/include/semaphore.h
 * Layer: Userland / C library compatibility
 *
 * Responsibilities:
 * - Declare the unnamed POSIX semaphore surface implemented by ArmOS.
 * - Keep semaphore storage compatible across the common userspace runtime.
 *
 * Notes:
 * - Named and process-shared semaphores are not currently implemented.
 * - Waiting is delegated to the common futex-backed pthread runtime.
 */

#ifndef _ARMOS_SEMAPHORE_H
#define _ARMOS_SEMAPHORE_H

#include <stdint.h>
#include <time.h>

typedef uint32_t sem_t;

#define SEM_FAILED ((sem_t *)0)
#define SEM_VALUE_MAX 0x7fffffff

int sem_init(sem_t *sem, int pshared, unsigned int value);
int sem_destroy(sem_t *sem);
int sem_wait(sem_t *sem);
int sem_trywait(sem_t *sem);
int sem_timedwait(sem_t *sem, const struct timespec *abstime);
int sem_post(sem_t *sem);
int sem_getvalue(sem_t *sem, int *value);

#endif /* _ARMOS_SEMAPHORE_H */
