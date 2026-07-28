/*
 * ArmOS
 * Copyright (c) 2026 Mohamed Ennassiri
 *
 * Licensed under the Apache License, Version 2.0.
 * See LICENSE for details.
 *
 * File: userland/programs/pthreadtest/pthreadtest.c
 * Layer: Userland / pthread validation
 *
 * Responsibilities:
 * - Validate the POSIX thread lifecycle and synchronization surface.
 * - Exercise compiler TLS, cancellation cleanup and robust owner recovery.
 *
 * Notes:
 * - The same source is executed on ARM32 and ARM64 SMP configurations.
 * - A failure is reported through both diagnostics and process status.
 */

#include <errno.h>
#include <pthread.h>
#include <semaphore.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#define WORKER_COUNT 4
#define ITERATIONS 1000
#define EXPECTED_DEFAULT_STACK_SIZE (1024u * 1024u)

static pthread_mutex_t gate_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t gate_cond = PTHREAD_COND_INITIALIZER;
static pthread_once_t once_control = PTHREAD_ONCE_INIT;
static pthread_rwlock_t rwlock = PTHREAD_RWLOCK_INITIALIZER;
static pthread_barrier_t barrier;
static pthread_key_t key;
static sem_t completion_sem;
static sem_t cancel_sem;
static sem_t cancel_ready_sem;
static sem_t cleanup_sem;
static pthread_mutex_t robust_mutex;

static int gate_open;
static int gate_waiters;
static int counter;
static int rw_value;
static int once_count;
static int destructor_count;
static __thread int compiler_tls = 37;

static void once_routine(void)
{
    __atomic_add_fetch(&once_count, 1, __ATOMIC_RELAXED);
}

static void key_destructor(void *value)
{
    if (value)
        __atomic_add_fetch(&destructor_count, 1, __ATOMIC_RELAXED);
}

static void *worker(void *opaque)
{
    intptr_t index = (intptr_t)opaque;
    int barrier_result;

    if (compiler_tls != 37)
        return (void *)(intptr_t)-100;
    compiler_tls = 100 + (int)index;
    if (pthread_setspecific(key, (void *)(index + 1)) != 0)
        return (void *)(intptr_t)-101;
    if (pthread_once(&once_control, once_routine) != 0)
        return (void *)(intptr_t)-102;

    if (pthread_mutex_lock(&gate_mutex) != 0)
        return (void *)(intptr_t)-103;
    gate_waiters++;
    pthread_cond_broadcast(&gate_cond);
    while (!gate_open) {
        if (pthread_cond_wait(&gate_cond, &gate_mutex) != 0) {
            pthread_mutex_unlock(&gate_mutex);
            return (void *)(intptr_t)-104;
        }
    }
    pthread_mutex_unlock(&gate_mutex);

    for (int iteration = 0; iteration < ITERATIONS; iteration++) {
        pthread_mutex_lock(&gate_mutex);
        counter++;
        pthread_mutex_unlock(&gate_mutex);
    }

    pthread_rwlock_wrlock(&rwlock);
    rw_value++;
    pthread_rwlock_unlock(&rwlock);

    barrier_result = pthread_barrier_wait(&barrier);
    if (barrier_result != 0 &&
        barrier_result != PTHREAD_BARRIER_SERIAL_THREAD)
        return (void *)(intptr_t)-105;

    if (compiler_tls != 100 + (int)index)
        return (void *)(intptr_t)-106;
    sem_post(&completion_sem);
    return (void *)(index + 1);
}

static void detached_cleanup(void *unused)
{
    (void)unused;
    sem_post(&cleanup_sem);
}

static void *cancel_worker(void *unused)
{
    (void)unused;
    pthread_cleanup_push(detached_cleanup, NULL);
    sem_post(&cancel_ready_sem);
    (void)sem_wait(&cancel_sem);
    pthread_cleanup_pop(0);
    return NULL;
}

static void *robust_worker(void *unused)
{
    (void)unused;
    if (pthread_mutex_lock(&robust_mutex) != 0)
        return (void *)(intptr_t)-1;
    return NULL;
}

static void *detached_worker(void *unused)
{
    (void)unused;
    sem_post(&completion_sem);
    return NULL;
}

static void *noop_worker(void *unused)
{
    return unused;
}

static void deadline_after_ms(struct timespec *deadline, long milliseconds)
{
    clock_gettime(CLOCK_REALTIME, deadline);
    deadline->tv_nsec += milliseconds * 1000000L;
    if (deadline->tv_nsec >= 1000000000L) {
        deadline->tv_sec++;
        deadline->tv_nsec -= 1000000000L;
    }
}

int main(void)
{
    pthread_t threads[WORKER_COUNT];
    pthread_t thread;
    pthread_attr_t attr;
    pthread_mutexattr_t mutex_attr;
    struct timespec deadline;
    size_t default_stack_size = 0;
    int failures = 0;

    setvbuf(stdout, NULL, _IONBF, 0);
    printf("pthreadtest: starting\n");

#define CHECK(expression, message) \
    do { \
        if (!(expression)) { \
            printf("pthreadtest: %s\n", message); \
            failures++; \
        } \
    } while (0)

    if (compiler_tls != 37) {
        printf("pthreadtest: main TLS template failed\n");
        return 1;
    }
    CHECK(pthread_key_create(&key, key_destructor) == 0,
          "pthread_key_create failed");
    CHECK(pthread_barrier_init(&barrier, NULL, WORKER_COUNT) == 0,
          "pthread_barrier_init failed");
    CHECK(sem_init(&completion_sem, 0, 0) == 0,
          "completion sem_init failed");

    for (intptr_t index = 0; index < WORKER_COUNT; index++)
        CHECK(pthread_create(&threads[index], NULL, worker,
                             (void *)index) == 0,
              "pthread_create failed");

    pthread_mutex_lock(&gate_mutex);
    while (gate_waiters != WORKER_COUNT)
        pthread_cond_wait(&gate_cond, &gate_mutex);
    gate_open = 1;
    pthread_cond_broadcast(&gate_cond);
    pthread_mutex_unlock(&gate_mutex);

    for (int index = 0; index < WORKER_COUNT; index++)
        CHECK(sem_wait(&completion_sem) == 0, "sem_wait failed");
    for (intptr_t index = 0; index < WORKER_COUNT; index++) {
        void *result = NULL;

        CHECK(pthread_join(threads[index], &result) == 0,
              "pthread_join failed");
        CHECK(result == (void *)(index + 1), "thread return value failed");
    }
    CHECK(counter == WORKER_COUNT * ITERATIONS, "mutex counter mismatch");
    pthread_rwlock_rdlock(&rwlock);
    CHECK(rw_value == WORKER_COUNT, "rwlock value mismatch");
    pthread_rwlock_unlock(&rwlock);
    CHECK(once_count == 1, "pthread_once ran more than once");
    CHECK(destructor_count == WORKER_COUNT, "TSD destructors mismatch");
    CHECK(compiler_tls == 37, "worker TLS leaked into main thread");
    printf("pthreadtest: lifecycle/sync/TLS ok\n");

    CHECK(sem_init(&cancel_sem, 0, 0) == 0, "cancel sem_init failed");
    CHECK(sem_init(&cancel_ready_sem, 0, 0) == 0,
          "cancel ready sem_init failed");
    CHECK(sem_init(&cleanup_sem, 0, 0) == 0, "cleanup sem_init failed");
    CHECK(pthread_create(&thread, NULL, cancel_worker, NULL) == 0,
          "cancel pthread_create failed");
    CHECK(sem_wait(&cancel_ready_sem) == 0, "cancel worker not ready");
    CHECK(pthread_cancel(thread) == 0, "pthread_cancel failed");
    {
        void *result = NULL;

        CHECK(pthread_join(thread, &result) == 0,
              "canceled pthread_join failed");
        CHECK(result == PTHREAD_CANCELED, "canceled result mismatch");
    }
    CHECK(sem_wait(&cleanup_sem) == 0, "cancel cleanup handler failed");
    printf("pthreadtest: cancellation ok\n");

    CHECK(pthread_mutexattr_init(&mutex_attr) == 0,
          "robust mutexattr init failed");
    CHECK(pthread_mutexattr_setrobust(
              &mutex_attr, PTHREAD_MUTEX_ROBUST) == 0,
          "robust mutexattr setup failed");
    CHECK(pthread_mutex_init(&robust_mutex, &mutex_attr) == 0,
          "robust mutex init failed");
    CHECK(pthread_create(&thread, NULL, robust_worker, NULL) == 0,
          "robust pthread_create failed");
    CHECK(pthread_join(thread, NULL) == 0, "robust pthread_join failed");
    CHECK(pthread_mutex_lock(&robust_mutex) == EOWNERDEAD,
          "robust owner death not reported");
    CHECK(pthread_mutex_consistent(&robust_mutex) == 0,
          "pthread_mutex_consistent failed");
    CHECK(pthread_mutex_unlock(&robust_mutex) == 0,
          "robust mutex unlock failed");
    printf("pthreadtest: robust mutex ok\n");

    CHECK(pthread_attr_init(&attr) == 0, "pthread_attr_init failed");
    CHECK(pthread_attr_getstacksize(&attr, &default_stack_size) == 0 &&
          default_stack_size == EXPECTED_DEFAULT_STACK_SIZE,
          "pthread default stack is not 1 MiB");
    CHECK(pthread_attr_setdetachstate(
              &attr, PTHREAD_CREATE_DETACHED) == 0,
          "pthread detach attribute failed");
    CHECK(pthread_create(&thread, &attr, detached_worker, NULL) == 0,
          "detached pthread_create failed");
    CHECK(sem_wait(&completion_sem) == 0, "detached thread failed");

    deadline_after_ms(&deadline, 10);
    errno = 0;
    CHECK(sem_timedwait(&cancel_sem, &deadline) == -1 &&
          errno == ETIMEDOUT, "semaphore timeout failed");
    CHECK(pthread_create(&thread, NULL, noop_worker, NULL) == 0,
          "detached reaper create failed");
    CHECK(pthread_join(thread, NULL) == 0, "detached reaper join failed");

    pthread_attr_destroy(&attr);
    pthread_mutexattr_destroy(&mutex_attr);
    pthread_mutex_destroy(&robust_mutex);
    pthread_barrier_destroy(&barrier);
    pthread_rwlock_destroy(&rwlock);
    pthread_cond_destroy(&gate_cond);
    pthread_mutex_destroy(&gate_mutex);
    pthread_key_delete(key);
    sem_destroy(&completion_sem);
    sem_destroy(&cancel_sem);
    sem_destroy(&cancel_ready_sem);
    sem_destroy(&cleanup_sem);

    if (failures) {
        printf("pthreadtest: failed (%d)\n", failures);
        return 1;
    }
    printf("pthreadtest: passed\n");
    return 0;
}
