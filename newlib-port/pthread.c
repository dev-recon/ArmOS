/*
 * ArmOS
 * Copyright (c) 2026 Mohamed Ennassiri
 *
 * Licensed under the Apache License, Version 2.0.
 * See LICENSE for details.
 *
 * File: newlib-port/pthread.c
 * Layer: Userspace / POSIX threads
 *
 * Responsibilities:
 * - Build the pthread lifecycle API on the native clone/futex/TLS ABI.
 * - Keep joins, cancellation state, cleanup handlers and TSD in userspace.
 *
 * Notes:
 * - Kernel code only creates scheduler-visible tasks and provides wait/wake.
 * - Process-shared objects and asynchronous cancellation are not advertised.
 */

#include "pthread_internal.h"

#include <errno.h>
#include <sched.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define ARMOS_PTHREAD_DEFAULT_STACK (64u * 1024u)
#define ARMOS_PTHREAD_STACK_ALIGNMENT 16u
#define ARMOS_KEY_INDEX_BITS 8u
#define ARMOS_KEY_INDEX_MASK ((1u << ARMOS_KEY_INDEX_BITS) - 1u)

typedef struct {
    uint32_t generation;
    int used;
    void (*destructor)(void *);
} armos_key_slot_t;

static volatile uint32_t registry_lock_word;
static armos_pthread_control_t *thread_table[ARMOS_PTHREAD_MAX_THREADS];
static armos_pthread_control_t main_thread;
static armos_key_slot_t key_table[ARMOS_PTHREAD_MAX_KEYS];

__thread armos_pthread_control_t *__armos_pthread_current;
__thread void *__armos_pthread_tsd[ARMOS_PTHREAD_MAX_KEYS];
__thread uint32_t armos_pthread_tsd_generation[ARMOS_PTHREAD_MAX_KEYS];

static void registry_lock_acquire(volatile uint32_t *word)
{
    for (;;) {
        uint32_t expected = 0;

        if (__atomic_compare_exchange_n(word, &expected, 1u, 0,
                                        __ATOMIC_ACQUIRE,
                                        __ATOMIC_RELAXED))
            return;
        (void)armos_futex_wait(word, 1u, NULL);
    }
}

static void registry_lock_release(volatile uint32_t *word)
{
    __atomic_store_n(word, 0u, __ATOMIC_RELEASE);
    (void)armos_futex_wake(word, 1u);
}

void __armos_pthread_registry_lock(void)
{
    registry_lock_acquire(&registry_lock_word);
}

void __armos_pthread_registry_unlock(void)
{
    registry_lock_release(&registry_lock_word);
}

static armos_pthread_control_t *thread_lookup_locked(pthread_t tid)
{
    for (unsigned int index = 0;
         index < ARMOS_PTHREAD_MAX_THREADS;
         index++) {
        armos_pthread_control_t *control = thread_table[index];

        if (control && control->tid == tid)
            return control;
    }
    return NULL;
}

static int thread_insert_locked(armos_pthread_control_t *control)
{
    for (unsigned int index = 0;
         index < ARMOS_PTHREAD_MAX_THREADS;
         index++) {
        if (!thread_table[index]) {
            thread_table[index] = control;
            return 0;
        }
    }
    return EAGAIN;
}

static void thread_remove_locked(armos_pthread_control_t *control)
{
    for (unsigned int index = 0;
         index < ARMOS_PTHREAD_MAX_THREADS;
         index++) {
        if (thread_table[index] == control) {
            thread_table[index] = NULL;
            return;
        }
    }
}

static void thread_control_destroy(armos_pthread_control_t *control)
{
    if (!control || control == &main_thread)
        return;
    armos_thread_reent_destroy(control->tls);
    if (control->owns_stack)
        free(control->stack_allocation);
    free(control);
}

static void reap_detached_threads(void)
{
    for (;;) {
        armos_pthread_control_t *reap = NULL;

        __armos_pthread_registry_lock();
        for (unsigned int index = 0;
             index < ARMOS_PTHREAD_MAX_THREADS;
             index++) {
            armos_pthread_control_t *control = thread_table[index];

            if (control && control != &main_thread &&
                __atomic_load_n(&control->detached, __ATOMIC_ACQUIRE) &&
                __atomic_load_n(&control->finished, __ATOMIC_ACQUIRE) &&
                __atomic_load_n(&control->alive_tid, __ATOMIC_ACQUIRE) == 0) {
                thread_table[index] = NULL;
                reap = control;
                break;
            }
        }
        __armos_pthread_registry_unlock();

        if (!reap)
            return;
        thread_control_destroy(reap);
    }
}

static void pthread_cancel_if_needed(void)
{
    armos_pthread_control_t *control = __armos_pthread_current;

    if (control &&
        control->cancel_state == PTHREAD_CANCEL_ENABLE &&
        __atomic_load_n(&control->cancel_pending, __ATOMIC_ACQUIRE))
        pthread_exit(PTHREAD_CANCELED);
}

int __armos_pthread_wait(volatile uint32_t *address, uint32_t expected,
                         const armos_timespec_t *timeout,
                         int cancellation_point)
{
    armos_pthread_control_t *control = __armos_pthread_current;
    int saved_errno = errno;
    int result;

    if (cancellation_point)
        pthread_cancel_if_needed();
    if (control)
        __atomic_store_n(&control->cancel_address, address,
                         __ATOMIC_RELEASE);
    if (control)
        __atomic_store_n(&control->cancel_address_changes, 0u,
                         __ATOMIC_RELEASE);
    if (cancellation_point)
        pthread_cancel_if_needed();

    result = armos_futex_wait(address, expected, timeout);
    if (result < 0)
        result = errno;
    else
        result = 0;

    if (control)
        __atomic_store_n(&control->cancel_address, NULL,
                         __ATOMIC_RELEASE);
    if (control)
        __atomic_store_n(&control->cancel_address_changes, 0u,
                         __ATOMIC_RELEASE);
    errno = saved_errno;
    if (cancellation_point)
        pthread_cancel_if_needed();
    return result;
}

int __armos_pthread_relative_timeout(clockid_t clock_id,
                                     const struct timespec *absolute,
                                     armos_timespec_t *relative)
{
    struct timespec now;
    time_t seconds;
    long nanoseconds;

    if (!absolute || !relative ||
        absolute->tv_nsec < 0 || absolute->tv_nsec >= 1000000000L)
        return EINVAL;
    if (clock_gettime(clock_id, &now) < 0)
        return errno;

    seconds = absolute->tv_sec - now.tv_sec;
    nanoseconds = absolute->tv_nsec - now.tv_nsec;
    if (nanoseconds < 0) {
        seconds--;
        nanoseconds += 1000000000L;
    }
    if (seconds < 0 || (seconds == 0 && nanoseconds == 0))
        return ETIMEDOUT;
    relative->sec = seconds;
    relative->nsec = nanoseconds;
    return 0;
}

void __armos_pthread_runtime_init(void *tcb)
{
    memset(&main_thread, 0, sizeof(main_thread));
    main_thread.tid = (pthread_t)armos_gettid();
    main_thread.alive_tid = main_thread.tid;
    main_thread.cancel_state = PTHREAD_CANCEL_ENABLE;
    main_thread.cancel_type = PTHREAD_CANCEL_DEFERRED;
    main_thread.tls = tcb;
    __armos_pthread_current = &main_thread;

    __armos_pthread_registry_lock();
    if (!thread_lookup_locked(main_thread.tid))
        (void)thread_insert_locked(&main_thread);
    __armos_pthread_registry_unlock();
}

static void run_tsd_destructors(void)
{
    for (unsigned int iteration = 0;
         iteration < ARMOS_PTHREAD_DESTRUCTOR_ITERATIONS;
         iteration++) {
        int called = 0;

        for (unsigned int index = 0;
             index < ARMOS_PTHREAD_MAX_KEYS;
             index++) {
            void *value = __armos_pthread_tsd[index];
            void (*destructor)(void *) = NULL;

            if (!value)
                continue;
            __armos_pthread_registry_lock();
            if (key_table[index].used &&
                key_table[index].generation ==
                    armos_pthread_tsd_generation[index])
                destructor = key_table[index].destructor;
            __armos_pthread_registry_unlock();

            __armos_pthread_tsd[index] = NULL;
            if (destructor) {
                destructor(value);
                called = 1;
            }
        }
        if (!called)
            break;
    }
}

static void thread_entry(void *opaque)
{
    armos_pthread_control_t *control =
        (armos_pthread_control_t *)opaque;
    void *result;

    __armos_pthread_current = control;
    control->tid = (pthread_t)armos_gettid();
    result = control->start_routine(control->argument);
    pthread_exit(result);
}

int pthread_attr_init(pthread_attr_t *attr)
{
    if (!attr)
        return EINVAL;
    memset(attr, 0, sizeof(*attr));
    attr->is_initialized = 1;
    attr->stacksize = ARMOS_PTHREAD_DEFAULT_STACK;
    attr->contentionscope = PTHREAD_SCOPE_SYSTEM;
    attr->inheritsched = PTHREAD_INHERIT_SCHED;
    attr->detachstate = PTHREAD_CREATE_JOINABLE;
    return 0;
}

int pthread_attr_destroy(pthread_attr_t *attr)
{
    if (!attr || !attr->is_initialized)
        return EINVAL;
    memset(attr, 0, sizeof(*attr));
    return 0;
}

int pthread_attr_setstack(pthread_attr_t *attr, void *stackaddr,
                          size_t stacksize)
{
    if (!attr || !attr->is_initialized || !stackaddr ||
        stacksize < PTHREAD_STACK_MIN || stacksize > 0x7fffffffu)
        return EINVAL;
    attr->stackaddr = stackaddr;
    attr->stacksize = (int)stacksize;
    return 0;
}

int pthread_attr_getstack(const pthread_attr_t *attr, void **stackaddr,
                          size_t *stacksize)
{
    if (!attr || !attr->is_initialized || !stackaddr || !stacksize)
        return EINVAL;
    *stackaddr = attr->stackaddr;
    *stacksize = (size_t)attr->stacksize;
    return 0;
}

int pthread_attr_setstacksize(pthread_attr_t *attr, size_t stacksize)
{
    if (!attr || !attr->is_initialized ||
        stacksize < PTHREAD_STACK_MIN || stacksize > 0x7fffffffu)
        return EINVAL;
    attr->stacksize = (int)stacksize;
    return 0;
}

int pthread_attr_getstacksize(const pthread_attr_t *attr, size_t *stacksize)
{
    if (!attr || !attr->is_initialized || !stacksize)
        return EINVAL;
    *stacksize = (size_t)attr->stacksize;
    return 0;
}

int pthread_attr_setstackaddr(pthread_attr_t *attr, void *stackaddr)
{
    if (!attr || !attr->is_initialized || !stackaddr)
        return EINVAL;
    attr->stackaddr = stackaddr;
    return 0;
}

int pthread_attr_getstackaddr(const pthread_attr_t *attr, void **stackaddr)
{
    if (!attr || !attr->is_initialized || !stackaddr)
        return EINVAL;
    *stackaddr = attr->stackaddr;
    return 0;
}

int pthread_attr_setdetachstate(pthread_attr_t *attr, int detachstate)
{
    if (!attr || !attr->is_initialized ||
        (detachstate != PTHREAD_CREATE_JOINABLE &&
         detachstate != PTHREAD_CREATE_DETACHED))
        return EINVAL;
    attr->detachstate = detachstate;
    return 0;
}

int pthread_attr_getdetachstate(const pthread_attr_t *attr, int *detachstate)
{
    if (!attr || !attr->is_initialized || !detachstate)
        return EINVAL;
    *detachstate = attr->detachstate;
    return 0;
}

int pthread_attr_setguardsize(pthread_attr_t *attr, size_t guardsize)
{
    if (!attr || !attr->is_initialized)
        return EINVAL;
    return guardsize == 0 ? 0 : ENOTSUP;
}

int pthread_attr_getguardsize(const pthread_attr_t *attr, size_t *guardsize)
{
    if (!attr || !attr->is_initialized || !guardsize)
        return EINVAL;
    *guardsize = 0;
    return 0;
}

int pthread_attr_setscope(pthread_attr_t *attr, int scope)
{
    if (!attr || !attr->is_initialized || scope != PTHREAD_SCOPE_SYSTEM)
        return scope == PTHREAD_SCOPE_PROCESS ? ENOTSUP : EINVAL;
    attr->contentionscope = scope;
    return 0;
}

int pthread_attr_getscope(const pthread_attr_t *attr, int *scope)
{
    if (!attr || !attr->is_initialized || !scope)
        return EINVAL;
    *scope = attr->contentionscope;
    return 0;
}

int pthread_attr_setinheritsched(pthread_attr_t *attr, int inherit)
{
    if (!attr || !attr->is_initialized ||
        (inherit != PTHREAD_INHERIT_SCHED &&
         inherit != PTHREAD_EXPLICIT_SCHED))
        return EINVAL;
    attr->inheritsched = inherit;
    return 0;
}

int pthread_attr_getinheritsched(const pthread_attr_t *attr, int *inherit)
{
    if (!attr || !attr->is_initialized || !inherit)
        return EINVAL;
    *inherit = attr->inheritsched;
    return 0;
}

int pthread_attr_setschedpolicy(pthread_attr_t *attr, int policy)
{
    if (!attr || !attr->is_initialized || policy != SCHED_OTHER)
        return policy == SCHED_OTHER ? EINVAL : ENOTSUP;
    attr->schedpolicy = policy;
    return 0;
}

int pthread_attr_getschedpolicy(const pthread_attr_t *attr, int *policy)
{
    if (!attr || !attr->is_initialized || !policy)
        return EINVAL;
    *policy = attr->schedpolicy;
    return 0;
}

int pthread_attr_setschedparam(pthread_attr_t *attr,
                               const struct sched_param *param)
{
    if (!attr || !attr->is_initialized || !param)
        return EINVAL;
    if (param->sched_priority != 0)
        return ENOTSUP;
    attr->schedparam = *param;
    return 0;
}

int pthread_attr_getschedparam(const pthread_attr_t *attr,
                               struct sched_param *param)
{
    if (!attr || !attr->is_initialized || !param)
        return EINVAL;
    *param = attr->schedparam;
    return 0;
}

int pthread_create(pthread_t *thread, const pthread_attr_t *attr,
                   void *(*start_routine)(void *), void *argument)
{
    armos_pthread_control_t *control;
    armos_clone_args_t args;
    uintptr_t raw;
    uintptr_t stack_base;
    size_t stack_size = ARMOS_PTHREAD_DEFAULT_STACK;
    int detached = 0;
    int result;

    if (!thread || !start_routine)
        return EINVAL;
    if (attr) {
        if (!attr->is_initialized)
            return EINVAL;
        stack_size = (size_t)attr->stacksize;
        detached = attr->detachstate == PTHREAD_CREATE_DETACHED;
    }
    if (stack_size < PTHREAD_STACK_MIN)
        return EINVAL;

    reap_detached_threads();
    control = calloc(1, sizeof(*control));
    if (!control)
        return EAGAIN;
    control->cancel_state = PTHREAD_CANCEL_ENABLE;
    control->cancel_type = PTHREAD_CANCEL_DEFERRED;
    control->start_routine = start_routine;
    control->argument = argument;
    control->stack_size = stack_size;
    control->detached = detached ? 1u : 0u;

    if (attr && attr->stackaddr) {
        control->stack_base = attr->stackaddr;
    } else {
        control->stack_allocation =
            malloc(stack_size + ARMOS_PTHREAD_STACK_ALIGNMENT);
        if (!control->stack_allocation) {
            free(control);
            return EAGAIN;
        }
        raw = (uintptr_t)control->stack_allocation;
        stack_base =
            (raw + ARMOS_PTHREAD_STACK_ALIGNMENT - 1u) &
            ~(uintptr_t)(ARMOS_PTHREAD_STACK_ALIGNMENT - 1u);
        control->stack_base = (void *)stack_base;
        control->owns_stack = 1;
    }

    control->tls = armos_thread_reent_create();
    if (!control->tls) {
        thread_control_destroy(control);
        return EAGAIN;
    }

    __armos_pthread_registry_lock();
    result = thread_insert_locked(control);
    __armos_pthread_registry_unlock();
    if (result != 0) {
        thread_control_destroy(control);
        return result;
    }

    memset(&args, 0, sizeof(args));
    args.flags = ARMOS_CLONE_THREAD_REQUIRED |
                 ARMOS_CLONE_CHILD_SETTID |
                 ARMOS_CLONE_CHILD_CLEARTID;
    args.stack =
        (unsigned long)((uintptr_t)control->stack_base + stack_size);
    args.stack_size = (unsigned long)stack_size;
    args.entry = (unsigned long)(uintptr_t)thread_entry;
    args.argument = (unsigned long)(uintptr_t)control;
    args.tls = (unsigned long)(uintptr_t)control->tls;
    args.child_tid = (unsigned long)(uintptr_t)&control->alive_tid;

    result = armos_clone(&args);
    if (result < 0) {
        result = errno;
        __armos_pthread_registry_lock();
        thread_remove_locked(control);
        __armos_pthread_registry_unlock();
        thread_control_destroy(control);
        return result;
    }

    control->tid = (pthread_t)result;
    *thread = control->tid;
    return 0;
}

pthread_t pthread_self(void)
{
    return (pthread_t)armos_gettid();
}

int pthread_equal(pthread_t left, pthread_t right)
{
    return left == right;
}

static void pthread_join_cancel_cleanup(void *opaque)
{
    armos_pthread_control_t *control =
        (armos_pthread_control_t *)opaque;

    __atomic_store_n(&control->joined, 0u, __ATOMIC_RELEASE);
}

int pthread_join(pthread_t thread, void **result_pointer)
{
    struct _pthread_cleanup_context cleanup;
    armos_pthread_control_t *control;
    uint32_t expected;
    int result;

    if (thread == pthread_self())
        return EDEADLK;

    __armos_pthread_registry_lock();
    control = thread_lookup_locked(thread);
    if (!control || control == &main_thread) {
        __armos_pthread_registry_unlock();
        return ESRCH;
    }
    if (__atomic_load_n(&control->detached, __ATOMIC_ACQUIRE)) {
        __armos_pthread_registry_unlock();
        return EINVAL;
    }
    expected = 0;
    if (!__atomic_compare_exchange_n(&control->joined, &expected, 1u, 0,
                                     __ATOMIC_ACQ_REL,
                                     __ATOMIC_RELAXED)) {
        __armos_pthread_registry_unlock();
        return EINVAL;
    }
    __armos_pthread_registry_unlock();

    _pthread_cleanup_push(&cleanup, pthread_join_cancel_cleanup, control);
    while (!__atomic_load_n(&control->finished, __ATOMIC_ACQUIRE)) {
        result = __armos_pthread_wait(&control->finished, 0u, NULL, 1);
        if (result != 0 && result != EAGAIN && result != EINTR) {
            _pthread_cleanup_pop(&cleanup, 1);
            return result;
        }
    }
    _pthread_cleanup_pop(&cleanup, 0);
    while ((expected =
            __atomic_load_n(&control->alive_tid, __ATOMIC_ACQUIRE)) != 0u) {
        result = __armos_pthread_wait(&control->alive_tid, expected,
                                      NULL, 1);
        if (result != 0 && result != EAGAIN && result != EINTR) {
            _pthread_cleanup_pop(&cleanup, 1);
            return result;
        }
    }

    if (result_pointer)
        *result_pointer = control->result;
    __armos_pthread_registry_lock();
    thread_remove_locked(control);
    __armos_pthread_registry_unlock();
    thread_control_destroy(control);
    return 0;
}

int pthread_detach(pthread_t thread)
{
    armos_pthread_control_t *control;

    __armos_pthread_registry_lock();
    control = thread_lookup_locked(thread);
    if (!control || control == &main_thread) {
        __armos_pthread_registry_unlock();
        return ESRCH;
    }
    if (__atomic_load_n(&control->detached, __ATOMIC_ACQUIRE) ||
        __atomic_load_n(&control->joined, __ATOMIC_ACQUIRE)) {
        __armos_pthread_registry_unlock();
        return EINVAL;
    }
    __atomic_store_n(&control->detached, 1u, __ATOMIC_RELEASE);
    __armos_pthread_registry_unlock();
    reap_detached_threads();
    return 0;
}

void pthread_exit(void *result)
{
    armos_pthread_control_t *control = __armos_pthread_current;

    if (!control || control == &main_thread)
        exit(0);

    while (control->cleanup) {
        struct _pthread_cleanup_context *cleanup = control->cleanup;

        control->cleanup = cleanup->_previous;
        cleanup->_routine(cleanup->_arg);
    }
    run_tsd_destructors();
    __armos_pthread_release_robust((uint32_t)pthread_self());
    control->result = result;
    __atomic_store_n(&control->finished, 1u, __ATOMIC_RELEASE);
    (void)armos_futex_wake(&control->finished, 0x7fffffffu);
    armos_thread_exit(0);
}

static void pthread_once_cancel_cleanup(void *opaque)
{
    pthread_once_t *once = (pthread_once_t *)opaque;

    __atomic_store_n(&once->init_executed, 0, __ATOMIC_RELEASE);
    (void)armos_futex_wake(
        (volatile uint32_t *)&once->init_executed, 0x7fffffffu);
}

int pthread_once(pthread_once_t *once, void (*routine)(void))
{
    struct _pthread_cleanup_context cleanup;
    int expected;

    if (!once || !routine)
        return EINVAL;
    if (!once->is_initialized)
        once->is_initialized = 1;

    for (;;) {
        expected = __atomic_load_n(&once->init_executed,
                                   __ATOMIC_ACQUIRE);
        if (expected == 2)
            return 0;
        if (expected == 0 &&
            __atomic_compare_exchange_n(&once->init_executed, &expected, 1,
                                         0, __ATOMIC_ACQ_REL,
                                         __ATOMIC_RELAXED)) {
            _pthread_cleanup_push(&cleanup, pthread_once_cancel_cleanup, once);
            routine();
            _pthread_cleanup_pop(&cleanup, 0);
            __atomic_store_n(&once->init_executed, 2, __ATOMIC_RELEASE);
            (void)armos_futex_wake(
                (volatile uint32_t *)&once->init_executed, 0x7fffffffu);
            return 0;
        }
        (void)__armos_pthread_wait(
            (volatile uint32_t *)&once->init_executed, 1u, NULL, 1);
    }
}

int pthread_key_create(pthread_key_t *key, void (*destructor)(void *))
{
    if (!key)
        return EINVAL;

    __armos_pthread_registry_lock();
    for (unsigned int index = 0;
         index < ARMOS_PTHREAD_MAX_KEYS;
         index++) {
        if (!key_table[index].used) {
            uint32_t generation = key_table[index].generation + 1u;

            if (generation == 0)
                generation = 1;
            key_table[index].generation = generation;
            key_table[index].destructor = destructor;
            key_table[index].used = 1;
            *key = (pthread_key_t)(
                (generation << ARMOS_KEY_INDEX_BITS) | (index + 1u));
            __armos_pthread_registry_unlock();
            return 0;
        }
    }
    __armos_pthread_registry_unlock();
    return EAGAIN;
}

static int key_decode(pthread_key_t key, unsigned int *index,
                      uint32_t *generation)
{
    uint32_t raw_index = (uint32_t)key & ARMOS_KEY_INDEX_MASK;

    if (raw_index == 0 || raw_index > ARMOS_PTHREAD_MAX_KEYS)
        return EINVAL;
    *index = raw_index - 1u;
    *generation = (uint32_t)key >> ARMOS_KEY_INDEX_BITS;
    if (*generation == 0)
        return EINVAL;
    return 0;
}

int pthread_key_delete(pthread_key_t key)
{
    unsigned int index;
    uint32_t generation;

    if (key_decode(key, &index, &generation) != 0)
        return EINVAL;
    __armos_pthread_registry_lock();
    if (!key_table[index].used ||
        key_table[index].generation != generation) {
        __armos_pthread_registry_unlock();
        return EINVAL;
    }
    key_table[index].used = 0;
    key_table[index].destructor = NULL;
    __armos_pthread_registry_unlock();
    return 0;
}

int pthread_setspecific(pthread_key_t key, const void *value)
{
    unsigned int index;
    uint32_t generation;

    if (key_decode(key, &index, &generation) != 0)
        return EINVAL;
    __armos_pthread_registry_lock();
    if (!key_table[index].used ||
        key_table[index].generation != generation) {
        __armos_pthread_registry_unlock();
        return EINVAL;
    }
    __armos_pthread_registry_unlock();
    __armos_pthread_tsd[index] = (void *)value;
    armos_pthread_tsd_generation[index] = generation;
    return 0;
}

void *pthread_getspecific(pthread_key_t key)
{
    unsigned int index;
    uint32_t generation;

    if (key_decode(key, &index, &generation) != 0 ||
        armos_pthread_tsd_generation[index] != generation)
        return NULL;
    return __armos_pthread_tsd[index];
}

int pthread_cancel(pthread_t thread)
{
    armos_pthread_control_t *control;
    volatile uint32_t *address;

    __armos_pthread_registry_lock();
    control = thread_lookup_locked(thread);
    if (!control) {
        __armos_pthread_registry_unlock();
        return ESRCH;
    }
    __atomic_store_n(&control->cancel_pending, 1u, __ATOMIC_RELEASE);
    address = __atomic_load_n(&control->cancel_address, __ATOMIC_ACQUIRE);
    if (address &&
        __atomic_load_n(&control->cancel_address_changes,
                        __ATOMIC_ACQUIRE))
        __atomic_add_fetch(address, 1u, __ATOMIC_RELEASE);
    __armos_pthread_registry_unlock();
    if (address)
        (void)armos_futex_wake(address, 0x7fffffffu);
    if (thread == pthread_self())
        pthread_cancel_if_needed();
    return 0;
}

int pthread_setcancelstate(int state, int *oldstate)
{
    armos_pthread_control_t *control = __armos_pthread_current;

    if (!control ||
        (state != PTHREAD_CANCEL_ENABLE &&
         state != PTHREAD_CANCEL_DISABLE))
        return EINVAL;
    if (oldstate)
        *oldstate = control->cancel_state;
    control->cancel_state = state;
    if (state == PTHREAD_CANCEL_ENABLE)
        pthread_cancel_if_needed();
    return 0;
}

int pthread_setcanceltype(int type, int *oldtype)
{
    armos_pthread_control_t *control = __armos_pthread_current;

    if (!control ||
        (type != PTHREAD_CANCEL_DEFERRED &&
         type != PTHREAD_CANCEL_ASYNCHRONOUS))
        return EINVAL;
    if (type == PTHREAD_CANCEL_ASYNCHRONOUS)
        return ENOTSUP;
    if (oldtype)
        *oldtype = control->cancel_type;
    control->cancel_type = type;
    return 0;
}

void pthread_testcancel(void)
{
    pthread_cancel_if_needed();
}

void _pthread_cleanup_push(struct _pthread_cleanup_context *context,
                           void (*routine)(void *), void *argument)
{
    armos_pthread_control_t *control = __armos_pthread_current;

    if (!control || !context)
        return;
    context->_routine = routine;
    context->_arg = argument;
    context->_canceltype = control->cancel_type;
    context->_previous = control->cleanup;
    control->cleanup = context;
}

void _pthread_cleanup_pop(struct _pthread_cleanup_context *context,
                          int execute)
{
    armos_pthread_control_t *control = __armos_pthread_current;

    if (!control || !context || control->cleanup != context)
        return;
    control->cleanup = context->_previous;
    if (execute)
        context->_routine(context->_arg);
}

int pthread_setconcurrency(int level)
{
    return level < 0 ? EINVAL : 0;
}

int pthread_getconcurrency(void)
{
    return 0;
}

void pthread_yield(void)
{
    (void)sched_yield();
}

int pthread_atfork(void (*prepare)(void), void (*parent)(void),
                   void (*child)(void))
{
    (void)prepare;
    (void)parent;
    (void)child;
    return ENOTSUP;
}
