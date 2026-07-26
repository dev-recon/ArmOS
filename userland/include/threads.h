/*
 * ArmOS
 * Copyright (c) 2026 Mohamed Ennassiri
 *
 * Licensed under the Apache License, Version 2.0.
 * See LICENSE for details.
 *
 * File: userland/include/threads.h
 * Layer: Userland / C library compatibility
 *
 * Responsibilities:
 * - Provide the ISO C11 threads API on top of ArmOS pthreads.
 * - Keep thread, mutex, condition and thread-local types architecture-neutral.
 *
 * Notes:
 * - Absolute deadlines use the same clock contract as pthread timed waits.
 * - The implementation is packaged in libthreads.a.
 */

#ifndef _ARMOS_THREADS_H
#define _ARMOS_THREADS_H

#include <pthread.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef pthread_t thrd_t;
typedef pthread_mutex_t mtx_t;
typedef pthread_cond_t cnd_t;
typedef pthread_key_t tss_t;
typedef pthread_once_t once_flag;

typedef int (*thrd_start_t)(void *);
typedef void (*tss_dtor_t)(void *);

#define ONCE_FLAG_INIT PTHREAD_ONCE_INIT
#ifdef PTHREAD_DESTRUCTOR_ITERATIONS
#define TSS_DTOR_ITERATIONS PTHREAD_DESTRUCTOR_ITERATIONS
#else
#define TSS_DTOR_ITERATIONS 4
#endif

enum {
    mtx_plain = 0x1,
    mtx_recursive = 0x2,
    mtx_timed = 0x4
};

enum {
    thrd_busy = 1,
    thrd_error = 2,
    thrd_nomem = 3,
    thrd_success = 4,
    thrd_timedout = 5
};

#if !defined(__cplusplus) || __cplusplus < 201103L
#define thread_local _Thread_local
#endif

void call_once(once_flag *flag, void (*function)(void));

int cnd_broadcast(cnd_t *condition);
void cnd_destroy(cnd_t *condition);
int cnd_init(cnd_t *condition);
int cnd_signal(cnd_t *condition);
int cnd_timedwait(cnd_t *condition, mtx_t *mutex,
                  const struct timespec *deadline);
int cnd_wait(cnd_t *condition, mtx_t *mutex);

void mtx_destroy(mtx_t *mutex);
int mtx_init(mtx_t *mutex, int type);
int mtx_lock(mtx_t *mutex);
int mtx_timedlock(mtx_t *mutex, const struct timespec *deadline);
int mtx_trylock(mtx_t *mutex);
int mtx_unlock(mtx_t *mutex);

int thrd_create(thrd_t *thread, thrd_start_t function, void *argument);
thrd_t thrd_current(void);
int thrd_detach(thrd_t thread);
int thrd_equal(thrd_t left, thrd_t right);
_Noreturn void thrd_exit(int result);
int thrd_join(thrd_t thread, int *result);
int thrd_sleep(const struct timespec *duration, struct timespec *remaining);
void thrd_yield(void);

int tss_create(tss_t *key, tss_dtor_t destructor);
void tss_delete(tss_t key);
void *tss_get(tss_t key);
int tss_set(tss_t key, void *value);

#ifdef __cplusplus
}
#endif

#endif /* _ARMOS_THREADS_H */
