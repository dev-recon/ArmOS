/*
 * ArmOS
 * Copyright (c) 2026 Mohamed Ennassiri
 *
 * Licensed under the Apache License, Version 2.0.
 * See LICENSE for details.
 *
 * File: userland/lib/threads/threads.c
 * Layer: Userland / C library compatibility
 *
 * Responsibilities:
 * - Implement ISO C11 threads using the ArmOS pthread runtime.
 * - Translate POSIX error values into the C11 thread result domain.
 */

#include <threads.h>

#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <sched.h>

struct thrd_start {
    thrd_start_t function;
    void *argument;
};

static int translate_error(int error)
{
    switch (error) {
    case 0:
        return thrd_success;
    case EBUSY:
        return thrd_busy;
    case ETIMEDOUT:
        return thrd_timedout;
    case ENOMEM:
    case EAGAIN:
        return thrd_nomem;
    default:
        return thrd_error;
    }
}

static void *thrd_trampoline(void *opaque)
{
    struct thrd_start *start = opaque;
    thrd_start_t function = start->function;
    void *argument = start->argument;
    int result;

    free(start);
    result = function(argument);
    return (void *)(intptr_t)result;
}

void call_once(once_flag *flag, void (*function)(void))
{
    (void)pthread_once(flag, function);
}

int cnd_broadcast(cnd_t *condition)
{
    return translate_error(pthread_cond_broadcast(condition));
}

void cnd_destroy(cnd_t *condition)
{
    (void)pthread_cond_destroy(condition);
}

int cnd_init(cnd_t *condition)
{
    return translate_error(pthread_cond_init(condition, NULL));
}

int cnd_signal(cnd_t *condition)
{
    return translate_error(pthread_cond_signal(condition));
}

int cnd_timedwait(cnd_t *condition, mtx_t *mutex,
                  const struct timespec *deadline)
{
    return translate_error(pthread_cond_timedwait(condition, mutex, deadline));
}

int cnd_wait(cnd_t *condition, mtx_t *mutex)
{
    return translate_error(pthread_cond_wait(condition, mutex));
}

void mtx_destroy(mtx_t *mutex)
{
    (void)pthread_mutex_destroy(mutex);
}

int mtx_init(mtx_t *mutex, int type)
{
    pthread_mutexattr_t attributes;
    int error;

    if ((type & ~(mtx_plain | mtx_recursive | mtx_timed)) != 0 || type == 0)
        return thrd_error;

    error = pthread_mutexattr_init(&attributes);
    if (error != 0)
        return translate_error(error);
    if ((type & mtx_recursive) != 0)
        error = pthread_mutexattr_settype(&attributes, PTHREAD_MUTEX_RECURSIVE);
    if (error == 0)
        error = pthread_mutex_init(mutex, &attributes);
    (void)pthread_mutexattr_destroy(&attributes);
    return translate_error(error);
}

int mtx_lock(mtx_t *mutex)
{
    return translate_error(pthread_mutex_lock(mutex));
}

int mtx_timedlock(mtx_t *mutex, const struct timespec *deadline)
{
    return translate_error(pthread_mutex_timedlock(mutex, deadline));
}

int mtx_trylock(mtx_t *mutex)
{
    return translate_error(pthread_mutex_trylock(mutex));
}

int mtx_unlock(mtx_t *mutex)
{
    return translate_error(pthread_mutex_unlock(mutex));
}

int thrd_create(thrd_t *thread, thrd_start_t function, void *argument)
{
    struct thrd_start *start;
    int error;

    if (thread == NULL || function == NULL)
        return thrd_error;

    start = malloc(sizeof(*start));
    if (start == NULL)
        return thrd_nomem;
    start->function = function;
    start->argument = argument;
    error = pthread_create(thread, NULL, thrd_trampoline, start);
    if (error != 0)
        free(start);
    return translate_error(error);
}

thrd_t thrd_current(void)
{
    return pthread_self();
}

int thrd_detach(thrd_t thread)
{
    return translate_error(pthread_detach(thread));
}

int thrd_equal(thrd_t left, thrd_t right)
{
    return pthread_equal(left, right);
}

_Noreturn void thrd_exit(int result)
{
    pthread_exit((void *)(intptr_t)result);
}

int thrd_join(thrd_t thread, int *result)
{
    void *value = NULL;
    int error = pthread_join(thread, &value);

    if (error == 0 && result != NULL)
        *result = (int)(intptr_t)value;
    return translate_error(error);
}

int thrd_sleep(const struct timespec *duration, struct timespec *remaining)
{
    if (nanosleep(duration, remaining) == 0)
        return 0;
    return errno == EINTR ? -1 : -2;
}

void thrd_yield(void)
{
    (void)sched_yield();
}

int tss_create(tss_t *key, tss_dtor_t destructor)
{
    return translate_error(pthread_key_create(key, destructor));
}

void tss_delete(tss_t key)
{
    (void)pthread_key_delete(key);
}

void *tss_get(tss_t key)
{
    return pthread_getspecific(key);
}

int tss_set(tss_t key, void *value)
{
    return translate_error(pthread_setspecific(key, value));
}
