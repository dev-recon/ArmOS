/*
 * ArmOS
 * Copyright (c) 2026 Mohamed Ennassiri
 *
 * Licensed under the Apache License, Version 2.0.
 * See LICENSE for details.
 *
 * File: newlib-port/pthread_sync.c
 * Layer: Userspace / POSIX synchronization
 */

#include "pthread_internal.h"

#include <errno.h>
#include <limits.h>
#include <semaphore.h>
#include <stdlib.h>
#include <string.h>

#define HANDLE_INDEX_BITS 8u
#define HANDLE_INDEX_MASK ((1u << HANDLE_INDEX_BITS) - 1u)
#define MUTEX_MAX 192u
#define COND_MAX 128u
#define RWLOCK_MAX 128u
#define BARRIER_MAX 64u
#define SEMAPHORE_MAX 128u

typedef struct {
    uint32_t generation;
    int used;
    volatile uint32_t word;
    volatile uint32_t owner;
    uint32_t recursion;
    int type;
    int robust;
    int owner_dead;
    int inconsistent;
    int not_recoverable;
} mutex_slot_t;

typedef struct {
    uint32_t generation;
    int used;
    volatile uint32_t sequence;
    volatile uint32_t waiters;
    clockid_t clock_id;
} cond_slot_t;

typedef struct {
    uint32_t generation;
    int used;
    volatile int32_t state;
    volatile uint32_t sequence;
    volatile uint32_t waiting_writers;
    volatile uint32_t writer;
} rwlock_slot_t;

typedef struct {
    uint32_t generation;
    int used;
    uint32_t count;
    volatile uint32_t waiting;
    volatile uint32_t generation_word;
} barrier_slot_t;

typedef struct {
    uint32_t generation;
    int used;
    volatile uint32_t value;
    volatile uint32_t sequence;
    volatile uint32_t waiters;
} semaphore_slot_t;

static mutex_slot_t mutex_table[MUTEX_MAX];
static cond_slot_t cond_table[COND_MAX];
static rwlock_slot_t rwlock_table[RWLOCK_MAX];
static barrier_slot_t barrier_table[BARRIER_MAX];
static semaphore_slot_t semaphore_table[SEMAPHORE_MAX];

static uint32_t make_handle(unsigned int index, uint32_t generation)
{
    return (generation << HANDLE_INDEX_BITS) | (index + 1u);
}

static int decode_handle(uint32_t handle, unsigned int limit,
                         unsigned int *index, uint32_t *generation)
{
    uint32_t raw_index = handle & HANDLE_INDEX_MASK;

    if (raw_index == 0 || raw_index > limit)
        return EINVAL;
    *index = raw_index - 1u;
    *generation = handle >> HANDLE_INDEX_BITS;
    return *generation ? 0 : EINVAL;
}

static uint32_t next_generation(uint32_t generation)
{
    generation++;
    return generation ? generation : 1u;
}

static int wake_word(volatile uint32_t *word, uint32_t count)
{
    int saved_errno = errno;
    int result = armos_futex_wake(word, count);

    if (result < 0)
        result = errno;
    else
        result = 0;
    errno = saved_errno;
    return result;
}

static int mutex_allocate(pthread_mutex_t *mutex,
                          const pthread_mutexattr_t *attr)
{
    int result = EAGAIN;

    __armos_pthread_registry_lock();
    if (__atomic_load_n(mutex, __ATOMIC_ACQUIRE) !=
        (uint32_t)_PTHREAD_MUTEX_INITIALIZER &&
        __atomic_load_n(mutex, __ATOMIC_ACQUIRE) != 0u) {
        __armos_pthread_registry_unlock();
        return EBUSY;
    }
    for (unsigned int index = 0; index < MUTEX_MAX; index++) {
        mutex_slot_t *slot = &mutex_table[index];

        if (!slot->used) {
            uint32_t generation = next_generation(slot->generation);

            memset(slot, 0, sizeof(*slot));
            slot->generation = generation;
            slot->used = 1;
            slot->type = attr ? attr->type : PTHREAD_MUTEX_DEFAULT;
            slot->robust = attr ? attr->recursive : PTHREAD_MUTEX_STALLED;
            __atomic_store_n(mutex, make_handle(index, generation),
                             __ATOMIC_RELEASE);
            result = 0;
            break;
        }
    }
    __armos_pthread_registry_unlock();
    return result;
}

static int mutex_resolve(pthread_mutex_t *mutex, mutex_slot_t **slot)
{
    uint32_t handle;
    uint32_t generation;
    unsigned int index;
    int result;

    if (!mutex || !slot)
        return EINVAL;
    handle = __atomic_load_n(mutex, __ATOMIC_ACQUIRE);
    if (handle == (uint32_t)_PTHREAD_MUTEX_INITIALIZER) {
        result = mutex_allocate(mutex, NULL);
        if (result != 0)
            return result;
        handle = __atomic_load_n(mutex, __ATOMIC_ACQUIRE);
    }
    if (decode_handle(handle, MUTEX_MAX, &index, &generation) != 0)
        return EINVAL;
    *slot = &mutex_table[index];
    if (!(*slot)->used || (*slot)->generation != generation)
        return EINVAL;
    return 0;
}

static int mutex_take_word(mutex_slot_t *slot,
                           const armos_timespec_t *timeout)
{
    uint32_t expected = 0;

    if (__atomic_compare_exchange_n(&slot->word, &expected, 1u, 0,
                                    __ATOMIC_ACQUIRE,
                                    __ATOMIC_RELAXED))
        return 0;

    for (;;) {
        uint32_t previous =
            __atomic_exchange_n(&slot->word, 2u, __ATOMIC_ACQUIRE);
        int result;

        if (previous == 0)
            return 0;
        result = __armos_pthread_wait(&slot->word, 2u, timeout, 0);
        if (result == ETIMEDOUT)
            return ETIMEDOUT;
        if (result != 0 && result != EAGAIN && result != EINTR)
            return result;
    }
}

static int mutex_lock_common(pthread_mutex_t *mutex,
                             const struct timespec *absolute,
                             int try_only)
{
    mutex_slot_t *slot;
    armos_timespec_t relative;
    const armos_timespec_t *timeout = NULL;
    uint32_t tid = (uint32_t)pthread_self();
    int result;

    result = mutex_resolve(mutex, &slot);
    if (result != 0)
        return result;
    if (__atomic_load_n(&slot->owner, __ATOMIC_ACQUIRE) == tid) {
        if (slot->type == PTHREAD_MUTEX_RECURSIVE) {
            if (slot->recursion == UINT_MAX)
                return EAGAIN;
            slot->recursion++;
            return 0;
        }
        if (slot->type == PTHREAD_MUTEX_ERRORCHECK)
            return EDEADLK;
    }
    if (slot->not_recoverable)
        return ENOTRECOVERABLE;
    if (try_only) {
        uint32_t expected = 0;

        if (!__atomic_compare_exchange_n(&slot->word, &expected, 1u, 0,
                                         __ATOMIC_ACQUIRE,
                                         __ATOMIC_RELAXED))
            return EBUSY;
    } else {
        if (absolute) {
            result = __armos_pthread_relative_timeout(
                CLOCK_REALTIME, absolute, &relative);
            if (result != 0)
                return result;
            timeout = &relative;
        }
        result = mutex_take_word(slot, timeout);
        if (result != 0)
            return result;
    }

    if (slot->not_recoverable) {
        uint32_t previous =
            __atomic_exchange_n(&slot->word, 0u, __ATOMIC_RELEASE);
        if (previous == 2u)
            (void)wake_word(&slot->word, 1u);
        return ENOTRECOVERABLE;
    }
    __atomic_store_n(&slot->owner, tid, __ATOMIC_RELEASE);
    slot->recursion = 1u;
    if (slot->robust == PTHREAD_MUTEX_ROBUST && slot->owner_dead) {
        slot->owner_dead = 0;
        slot->inconsistent = 1;
        return EOWNERDEAD;
    }
    return 0;
}

int pthread_mutexattr_init(pthread_mutexattr_t *attr)
{
    if (!attr)
        return EINVAL;
    memset(attr, 0, sizeof(*attr));
    attr->is_initialized = 1;
    attr->process_shared = PTHREAD_PROCESS_PRIVATE;
    attr->type = PTHREAD_MUTEX_DEFAULT;
    attr->recursive = PTHREAD_MUTEX_STALLED;
    return 0;
}

int pthread_mutexattr_destroy(pthread_mutexattr_t *attr)
{
    if (!attr || !attr->is_initialized)
        return EINVAL;
    memset(attr, 0, sizeof(*attr));
    return 0;
}

int pthread_mutexattr_getpshared(const pthread_mutexattr_t *attr,
                                 int *pshared)
{
    if (!attr || !attr->is_initialized || !pshared)
        return EINVAL;
    *pshared = attr->process_shared;
    return 0;
}

int pthread_mutexattr_setpshared(pthread_mutexattr_t *attr, int pshared)
{
    if (!attr || !attr->is_initialized)
        return EINVAL;
    if (pshared == PTHREAD_PROCESS_SHARED)
        return ENOTSUP;
    if (pshared != PTHREAD_PROCESS_PRIVATE)
        return EINVAL;
    attr->process_shared = pshared;
    return 0;
}

int pthread_mutexattr_gettype(const pthread_mutexattr_t *attr, int *type)
{
    if (!attr || !attr->is_initialized || !type)
        return EINVAL;
    *type = attr->type;
    return 0;
}

int pthread_mutexattr_settype(pthread_mutexattr_t *attr, int type)
{
    if (!attr || !attr->is_initialized ||
        type < PTHREAD_MUTEX_NORMAL || type > PTHREAD_MUTEX_DEFAULT)
        return EINVAL;
    attr->type = type;
    return 0;
}

int pthread_mutexattr_getrobust(const pthread_mutexattr_t *attr,
                                int *robustness)
{
    if (!attr || !attr->is_initialized || !robustness)
        return EINVAL;
    *robustness = attr->recursive;
    return 0;
}

int pthread_mutexattr_setrobust(pthread_mutexattr_t *attr, int robustness)
{
    if (!attr || !attr->is_initialized ||
        (robustness != PTHREAD_MUTEX_STALLED &&
         robustness != PTHREAD_MUTEX_ROBUST))
        return EINVAL;
    attr->recursive = robustness;
    return 0;
}

int pthread_mutex_init(pthread_mutex_t *mutex,
                       const pthread_mutexattr_t *attr)
{
    if (!mutex || (attr && !attr->is_initialized))
        return EINVAL;
    __atomic_store_n(mutex, 0u, __ATOMIC_RELEASE);
    return mutex_allocate(mutex, attr);
}

int pthread_mutex_destroy(pthread_mutex_t *mutex)
{
    mutex_slot_t *slot;
    uint32_t handle;
    int result = mutex_resolve(mutex, &slot);

    if (result != 0)
        return result;
    if (__atomic_load_n(&slot->word, __ATOMIC_ACQUIRE) != 0)
        return EBUSY;
    handle = __atomic_load_n(mutex, __ATOMIC_ACQUIRE);
    __armos_pthread_registry_lock();
    if (slot->used && make_handle(
            (unsigned int)(slot - mutex_table), slot->generation) == handle)
        slot->used = 0;
    __atomic_store_n(mutex, 0u, __ATOMIC_RELEASE);
    __armos_pthread_registry_unlock();
    return 0;
}

int pthread_mutex_lock(pthread_mutex_t *mutex)
{
    return mutex_lock_common(mutex, NULL, 0);
}

int pthread_mutex_trylock(pthread_mutex_t *mutex)
{
    return mutex_lock_common(mutex, NULL, 1);
}

int pthread_mutex_timedlock(pthread_mutex_t *mutex,
                            const struct timespec *absolute)
{
    return mutex_lock_common(mutex, absolute, 0);
}

int pthread_mutex_unlock(pthread_mutex_t *mutex)
{
    mutex_slot_t *slot;
    uint32_t tid = (uint32_t)pthread_self();
    uint32_t previous;
    int result = mutex_resolve(mutex, &slot);

    if (result != 0)
        return result;
    if (__atomic_load_n(&slot->owner, __ATOMIC_ACQUIRE) != tid)
        return EPERM;
    if (slot->recursion > 1u) {
        slot->recursion--;
        return 0;
    }
    if (slot->robust == PTHREAD_MUTEX_ROBUST && slot->inconsistent) {
        slot->inconsistent = 0;
        slot->not_recoverable = 1;
    }
    slot->recursion = 0;
    __atomic_store_n(&slot->owner, 0u, __ATOMIC_RELEASE);
    previous = __atomic_exchange_n(&slot->word, 0u, __ATOMIC_RELEASE);
    if (previous == 2u)
        (void)wake_word(&slot->word, 1u);
    return 0;
}

int pthread_mutex_consistent(pthread_mutex_t *mutex)
{
    mutex_slot_t *slot;
    int result = mutex_resolve(mutex, &slot);

    if (result != 0)
        return result;
    if (slot->robust != PTHREAD_MUTEX_ROBUST ||
        __atomic_load_n(&slot->owner, __ATOMIC_ACQUIRE) !=
            (uint32_t)pthread_self() ||
        !slot->inconsistent)
        return EINVAL;
    slot->inconsistent = 0;
    return 0;
}

void __armos_pthread_release_robust(uint32_t tid)
{
    __armos_pthread_registry_lock();
    for (unsigned int index = 0; index < MUTEX_MAX; index++) {
        mutex_slot_t *slot = &mutex_table[index];

        if (slot->used && slot->robust == PTHREAD_MUTEX_ROBUST &&
            __atomic_load_n(&slot->owner, __ATOMIC_ACQUIRE) == tid) {
            slot->owner = 0;
            slot->recursion = 0;
            slot->inconsistent = 0;
            slot->owner_dead = 1;
            __atomic_store_n(&slot->word, 0u, __ATOMIC_RELEASE);
            (void)wake_word(&slot->word, 0x7fffffffu);
        }
    }
    __armos_pthread_registry_unlock();
}

static int cond_allocate(pthread_cond_t *cond,
                         const pthread_condattr_t *attr)
{
    int result = EAGAIN;

    __armos_pthread_registry_lock();
    if (__atomic_load_n(cond, __ATOMIC_ACQUIRE) !=
        (uint32_t)_PTHREAD_COND_INITIALIZER &&
        __atomic_load_n(cond, __ATOMIC_ACQUIRE) != 0u) {
        __armos_pthread_registry_unlock();
        return EBUSY;
    }
    for (unsigned int index = 0; index < COND_MAX; index++) {
        cond_slot_t *slot = &cond_table[index];

        if (!slot->used) {
            uint32_t generation = next_generation(slot->generation);

            memset(slot, 0, sizeof(*slot));
            slot->generation = generation;
            slot->used = 1;
            slot->clock_id = attr ? (clockid_t)attr->clock :
                CLOCK_REALTIME;
            __atomic_store_n(cond, make_handle(index, generation),
                             __ATOMIC_RELEASE);
            result = 0;
            break;
        }
    }
    __armos_pthread_registry_unlock();
    return result;
}

static int cond_resolve(pthread_cond_t *cond, cond_slot_t **slot)
{
    uint32_t handle;
    uint32_t generation;
    unsigned int index;
    int result;

    if (!cond || !slot)
        return EINVAL;
    handle = __atomic_load_n(cond, __ATOMIC_ACQUIRE);
    if (handle == (uint32_t)_PTHREAD_COND_INITIALIZER) {
        result = cond_allocate(cond, NULL);
        if (result != 0)
            return result;
        handle = __atomic_load_n(cond, __ATOMIC_ACQUIRE);
    }
    if (decode_handle(handle, COND_MAX, &index, &generation) != 0)
        return EINVAL;
    *slot = &cond_table[index];
    return (*slot)->used && (*slot)->generation == generation ?
        0 : EINVAL;
}

int pthread_condattr_init(pthread_condattr_t *attr)
{
    if (!attr)
        return EINVAL;
    memset(attr, 0, sizeof(*attr));
    attr->is_initialized = 1;
    attr->clock = CLOCK_REALTIME;
    attr->process_shared = PTHREAD_PROCESS_PRIVATE;
    return 0;
}

int pthread_condattr_destroy(pthread_condattr_t *attr)
{
    if (!attr || !attr->is_initialized)
        return EINVAL;
    memset(attr, 0, sizeof(*attr));
    return 0;
}

int pthread_condattr_getclock(const pthread_condattr_t *attr,
                              clockid_t *clock_id)
{
    if (!attr || !attr->is_initialized || !clock_id)
        return EINVAL;
    *clock_id = (clockid_t)attr->clock;
    return 0;
}

int pthread_condattr_setclock(pthread_condattr_t *attr, clockid_t clock_id)
{
    if (!attr || !attr->is_initialized ||
        (clock_id != CLOCK_REALTIME && clock_id != CLOCK_MONOTONIC))
        return EINVAL;
    attr->clock = (clock_t)clock_id;
    return 0;
}

int pthread_condattr_getpshared(const pthread_condattr_t *attr, int *pshared)
{
    if (!attr || !attr->is_initialized || !pshared)
        return EINVAL;
    *pshared = attr->process_shared;
    return 0;
}

int pthread_condattr_setpshared(pthread_condattr_t *attr, int pshared)
{
    if (!attr || !attr->is_initialized)
        return EINVAL;
    if (pshared == PTHREAD_PROCESS_SHARED)
        return ENOTSUP;
    if (pshared != PTHREAD_PROCESS_PRIVATE)
        return EINVAL;
    attr->process_shared = pshared;
    return 0;
}

int pthread_cond_init(pthread_cond_t *cond, const pthread_condattr_t *attr)
{
    if (!cond || (attr && !attr->is_initialized))
        return EINVAL;
    __atomic_store_n(cond, 0u, __ATOMIC_RELEASE);
    return cond_allocate(cond, attr);
}

int pthread_cond_destroy(pthread_cond_t *cond)
{
    cond_slot_t *slot;
    int result = cond_resolve(cond, &slot);

    if (result != 0)
        return result;
    if (__atomic_load_n(&slot->waiters, __ATOMIC_ACQUIRE) != 0)
        return EBUSY;
    __armos_pthread_registry_lock();
    slot->used = 0;
    __atomic_store_n(cond, 0u, __ATOMIC_RELEASE);
    __armos_pthread_registry_unlock();
    return 0;
}

int pthread_cond_signal(pthread_cond_t *cond)
{
    cond_slot_t *slot;
    int result = cond_resolve(cond, &slot);

    if (result != 0)
        return result;
    __atomic_add_fetch(&slot->sequence, 1u, __ATOMIC_RELEASE);
    return wake_word(&slot->sequence, 1u);
}

int pthread_cond_broadcast(pthread_cond_t *cond)
{
    cond_slot_t *slot;
    int result = cond_resolve(cond, &slot);

    if (result != 0)
        return result;
    __atomic_add_fetch(&slot->sequence, 1u, __ATOMIC_RELEASE);
    return wake_word(&slot->sequence, 0x7fffffffu);
}

static int cond_wait_common(pthread_cond_t *cond, pthread_mutex_t *mutex,
                            const struct timespec *absolute)
{
    cond_slot_t *slot;
    armos_timespec_t relative;
    const armos_timespec_t *timeout = NULL;
    armos_pthread_control_t *control = __armos_pthread_current;
    uint32_t sequence;
    int wait_result;
    int lock_result;
    int result = cond_resolve(cond, &slot);

    if (result != 0)
        return result;
    if (absolute) {
        result = __armos_pthread_relative_timeout(
            slot->clock_id, absolute, &relative);
        if (result != 0)
            return result;
        timeout = &relative;
    }

    sequence = __atomic_load_n(&slot->sequence, __ATOMIC_ACQUIRE);
    __atomic_add_fetch(&slot->waiters, 1u, __ATOMIC_ACQ_REL);
    if (control)
        __atomic_store_n(&control->cancel_address, &slot->sequence,
                         __ATOMIC_RELEASE);
    if (control)
        __atomic_store_n(&control->cancel_address_changes, 1u,
                         __ATOMIC_RELEASE);
    pthread_testcancel();
    result = pthread_mutex_unlock(mutex);
    if (result != 0) {
        if (control)
            control->cancel_address = NULL;
        if (control)
            control->cancel_address_changes = 0u;
        __atomic_sub_fetch(&slot->waiters, 1u, __ATOMIC_ACQ_REL);
        return result;
    }

    wait_result = __armos_pthread_wait(
        &slot->sequence, sequence, timeout, 0);
    if (control)
        __atomic_store_n(&control->cancel_address, NULL, __ATOMIC_RELEASE);
    if (control)
        __atomic_store_n(&control->cancel_address_changes, 0u,
                         __ATOMIC_RELEASE);
    __atomic_sub_fetch(&slot->waiters, 1u, __ATOMIC_ACQ_REL);
    lock_result = pthread_mutex_lock(mutex);
    pthread_testcancel();
    if (lock_result != 0)
        return lock_result;
    if (wait_result == ETIMEDOUT)
        return ETIMEDOUT;
    if (wait_result != 0 && wait_result != EAGAIN && wait_result != EINTR)
        return wait_result;
    return 0;
}

int pthread_cond_wait(pthread_cond_t *cond, pthread_mutex_t *mutex)
{
    return cond_wait_common(cond, mutex, NULL);
}

int pthread_cond_timedwait(pthread_cond_t *cond, pthread_mutex_t *mutex,
                           const struct timespec *absolute)
{
    return cond_wait_common(cond, mutex, absolute);
}

static int rwlock_allocate(pthread_rwlock_t *rwlock)
{
    int result = EAGAIN;

    __armos_pthread_registry_lock();
    if (__atomic_load_n(rwlock, __ATOMIC_ACQUIRE) !=
        (uint32_t)_PTHREAD_RWLOCK_INITIALIZER &&
        __atomic_load_n(rwlock, __ATOMIC_ACQUIRE) != 0u) {
        __armos_pthread_registry_unlock();
        return EBUSY;
    }
    for (unsigned int index = 0; index < RWLOCK_MAX; index++) {
        rwlock_slot_t *slot = &rwlock_table[index];

        if (!slot->used) {
            uint32_t generation = next_generation(slot->generation);

            memset(slot, 0, sizeof(*slot));
            slot->generation = generation;
            slot->used = 1;
            __atomic_store_n(rwlock, make_handle(index, generation),
                             __ATOMIC_RELEASE);
            result = 0;
            break;
        }
    }
    __armos_pthread_registry_unlock();
    return result;
}

static int rwlock_resolve(pthread_rwlock_t *rwlock, rwlock_slot_t **slot)
{
    uint32_t handle;
    uint32_t generation;
    unsigned int index;
    int result;

    if (!rwlock || !slot)
        return EINVAL;
    handle = __atomic_load_n(rwlock, __ATOMIC_ACQUIRE);
    if (handle == (uint32_t)_PTHREAD_RWLOCK_INITIALIZER) {
        result = rwlock_allocate(rwlock);
        if (result != 0)
            return result;
        handle = __atomic_load_n(rwlock, __ATOMIC_ACQUIRE);
    }
    if (decode_handle(handle, RWLOCK_MAX, &index, &generation) != 0)
        return EINVAL;
    *slot = &rwlock_table[index];
    return (*slot)->used && (*slot)->generation == generation ?
        0 : EINVAL;
}

int pthread_rwlockattr_init(pthread_rwlockattr_t *attr)
{
    if (!attr)
        return EINVAL;
    memset(attr, 0, sizeof(*attr));
    attr->is_initialized = 1;
    attr->process_shared = PTHREAD_PROCESS_PRIVATE;
    return 0;
}

int pthread_rwlockattr_destroy(pthread_rwlockattr_t *attr)
{
    if (!attr || !attr->is_initialized)
        return EINVAL;
    memset(attr, 0, sizeof(*attr));
    return 0;
}

int pthread_rwlockattr_getpshared(const pthread_rwlockattr_t *attr,
                                  int *pshared)
{
    if (!attr || !attr->is_initialized || !pshared)
        return EINVAL;
    *pshared = attr->process_shared;
    return 0;
}

int pthread_rwlockattr_setpshared(pthread_rwlockattr_t *attr, int pshared)
{
    if (!attr || !attr->is_initialized)
        return EINVAL;
    if (pshared == PTHREAD_PROCESS_SHARED)
        return ENOTSUP;
    if (pshared != PTHREAD_PROCESS_PRIVATE)
        return EINVAL;
    attr->process_shared = pshared;
    return 0;
}

int pthread_rwlock_init(pthread_rwlock_t *rwlock,
                        const pthread_rwlockattr_t *attr)
{
    if (!rwlock || (attr && !attr->is_initialized))
        return EINVAL;
    if (attr && attr->process_shared == PTHREAD_PROCESS_SHARED)
        return ENOTSUP;
    __atomic_store_n(rwlock, 0u, __ATOMIC_RELEASE);
    return rwlock_allocate(rwlock);
}

int pthread_rwlock_destroy(pthread_rwlock_t *rwlock)
{
    rwlock_slot_t *slot;
    int result = rwlock_resolve(rwlock, &slot);

    if (result != 0)
        return result;
    if (__atomic_load_n(&slot->state, __ATOMIC_ACQUIRE) != 0)
        return EBUSY;
    __armos_pthread_registry_lock();
    slot->used = 0;
    __atomic_store_n(rwlock, 0u, __ATOMIC_RELEASE);
    __armos_pthread_registry_unlock();
    return 0;
}

static int rwlock_rdlock_common(pthread_rwlock_t *rwlock,
                                const struct timespec *absolute,
                                int try_only)
{
    rwlock_slot_t *slot;
    armos_timespec_t relative;
    const armos_timespec_t *timeout = NULL;
    int result = rwlock_resolve(rwlock, &slot);

    if (result != 0)
        return result;
    if (absolute) {
        result = __armos_pthread_relative_timeout(
            CLOCK_REALTIME, absolute, &relative);
        if (result != 0)
            return result;
        timeout = &relative;
    }
    for (;;) {
        int32_t state = __atomic_load_n(&slot->state, __ATOMIC_ACQUIRE);

        if (state >= 0 &&
            __atomic_load_n(&slot->waiting_writers,
                            __ATOMIC_ACQUIRE) == 0) {
            if (state == INT32_MAX)
                return EAGAIN;
            if (__atomic_compare_exchange_n(&slot->state, &state, state + 1,
                                             0, __ATOMIC_ACQUIRE,
                                             __ATOMIC_RELAXED))
                return 0;
            continue;
        }
        if (try_only)
            return EBUSY;
        {
            uint32_t sequence =
                __atomic_load_n(&slot->sequence, __ATOMIC_ACQUIRE);
            result = __armos_pthread_wait(
                &slot->sequence, sequence, timeout, 0);
        }
        if (result == ETIMEDOUT)
            return result;
        if (result != 0 && result != EAGAIN && result != EINTR)
            return result;
    }
}

static int rwlock_wrlock_common(pthread_rwlock_t *rwlock,
                                const struct timespec *absolute,
                                int try_only)
{
    rwlock_slot_t *slot;
    armos_timespec_t relative;
    const armos_timespec_t *timeout = NULL;
    int result = rwlock_resolve(rwlock, &slot);

    if (result != 0)
        return result;
    if (__atomic_load_n(&slot->writer, __ATOMIC_ACQUIRE) ==
        (uint32_t)pthread_self())
        return EDEADLK;
    if (absolute) {
        result = __armos_pthread_relative_timeout(
            CLOCK_REALTIME, absolute, &relative);
        if (result != 0)
            return result;
        timeout = &relative;
    }
    __atomic_add_fetch(&slot->waiting_writers, 1u, __ATOMIC_ACQ_REL);
    for (;;) {
        int32_t expected = 0;

        if (__atomic_compare_exchange_n(&slot->state, &expected, -1, 0,
                                         __ATOMIC_ACQUIRE,
                                         __ATOMIC_RELAXED)) {
            __atomic_sub_fetch(&slot->waiting_writers, 1u,
                               __ATOMIC_ACQ_REL);
            slot->writer = (uint32_t)pthread_self();
            return 0;
        }
        if (try_only) {
            __atomic_sub_fetch(&slot->waiting_writers, 1u,
                               __ATOMIC_ACQ_REL);
            return EBUSY;
        }
        {
            uint32_t sequence =
                __atomic_load_n(&slot->sequence, __ATOMIC_ACQUIRE);
            result = __armos_pthread_wait(
                &slot->sequence, sequence, timeout, 0);
        }
        if (result == ETIMEDOUT ||
            (result != 0 && result != EAGAIN && result != EINTR)) {
            __atomic_sub_fetch(&slot->waiting_writers, 1u,
                               __ATOMIC_ACQ_REL);
            return result;
        }
    }
}

int pthread_rwlock_rdlock(pthread_rwlock_t *rwlock)
{
    return rwlock_rdlock_common(rwlock, NULL, 0);
}

int pthread_rwlock_tryrdlock(pthread_rwlock_t *rwlock)
{
    return rwlock_rdlock_common(rwlock, NULL, 1);
}

int pthread_rwlock_timedrdlock(pthread_rwlock_t *rwlock,
                               const struct timespec *absolute)
{
    return rwlock_rdlock_common(rwlock, absolute, 0);
}

int pthread_rwlock_wrlock(pthread_rwlock_t *rwlock)
{
    return rwlock_wrlock_common(rwlock, NULL, 0);
}

int pthread_rwlock_trywrlock(pthread_rwlock_t *rwlock)
{
    return rwlock_wrlock_common(rwlock, NULL, 1);
}

int pthread_rwlock_timedwrlock(pthread_rwlock_t *rwlock,
                               const struct timespec *absolute)
{
    return rwlock_wrlock_common(rwlock, absolute, 0);
}

int pthread_rwlock_unlock(pthread_rwlock_t *rwlock)
{
    rwlock_slot_t *slot;
    int32_t state;
    int result = rwlock_resolve(rwlock, &slot);

    if (result != 0)
        return result;
    state = __atomic_load_n(&slot->state, __ATOMIC_ACQUIRE);
    if (state == -1) {
        if (slot->writer != (uint32_t)pthread_self())
            return EPERM;
        slot->writer = 0;
        __atomic_store_n(&slot->state, 0, __ATOMIC_RELEASE);
    } else if (state > 0) {
        __atomic_sub_fetch(&slot->state, 1, __ATOMIC_RELEASE);
    } else {
        return EPERM;
    }
    __atomic_add_fetch(&slot->sequence, 1u, __ATOMIC_RELEASE);
    (void)wake_word(&slot->sequence, 0x7fffffffu);
    return 0;
}

static int barrier_allocate(pthread_barrier_t *barrier, unsigned int count)
{
    int result = EAGAIN;

    __armos_pthread_registry_lock();
    for (unsigned int index = 0; index < BARRIER_MAX; index++) {
        barrier_slot_t *slot = &barrier_table[index];

        if (!slot->used) {
            uint32_t generation = next_generation(slot->generation);

            memset(slot, 0, sizeof(*slot));
            slot->generation = generation;
            slot->used = 1;
            slot->count = count;
            __atomic_store_n(barrier, make_handle(index, generation),
                             __ATOMIC_RELEASE);
            result = 0;
            break;
        }
    }
    __armos_pthread_registry_unlock();
    return result;
}

static int barrier_resolve(pthread_barrier_t *barrier,
                           barrier_slot_t **slot)
{
    uint32_t generation;
    unsigned int index;
    uint32_t handle;

    if (!barrier || !slot)
        return EINVAL;
    handle = __atomic_load_n(barrier, __ATOMIC_ACQUIRE);
    if (decode_handle(handle, BARRIER_MAX, &index, &generation) != 0)
        return EINVAL;
    *slot = &barrier_table[index];
    return (*slot)->used && (*slot)->generation == generation ?
        0 : EINVAL;
}

int pthread_barrierattr_init(pthread_barrierattr_t *attr)
{
    if (!attr)
        return EINVAL;
    memset(attr, 0, sizeof(*attr));
    attr->is_initialized = 1;
    attr->process_shared = PTHREAD_PROCESS_PRIVATE;
    return 0;
}

int pthread_barrierattr_destroy(pthread_barrierattr_t *attr)
{
    if (!attr || !attr->is_initialized)
        return EINVAL;
    memset(attr, 0, sizeof(*attr));
    return 0;
}

int pthread_barrierattr_getpshared(const pthread_barrierattr_t *attr,
                                   int *pshared)
{
    if (!attr || !attr->is_initialized || !pshared)
        return EINVAL;
    *pshared = attr->process_shared;
    return 0;
}

int pthread_barrierattr_setpshared(pthread_barrierattr_t *attr, int pshared)
{
    if (!attr || !attr->is_initialized)
        return EINVAL;
    if (pshared == PTHREAD_PROCESS_SHARED)
        return ENOTSUP;
    if (pshared != PTHREAD_PROCESS_PRIVATE)
        return EINVAL;
    attr->process_shared = pshared;
    return 0;
}

int pthread_barrier_init(pthread_barrier_t *barrier,
                         const pthread_barrierattr_t *attr,
                         unsigned int count)
{
    if (!barrier || count == 0 || (attr && !attr->is_initialized))
        return EINVAL;
    if (attr && attr->process_shared == PTHREAD_PROCESS_SHARED)
        return ENOTSUP;
    __atomic_store_n(barrier, 0u, __ATOMIC_RELEASE);
    return barrier_allocate(barrier, count);
}

int pthread_barrier_destroy(pthread_barrier_t *barrier)
{
    barrier_slot_t *slot;
    int result = barrier_resolve(barrier, &slot);

    if (result != 0)
        return result;
    if (__atomic_load_n(&slot->waiting, __ATOMIC_ACQUIRE) != 0)
        return EBUSY;
    __armos_pthread_registry_lock();
    slot->used = 0;
    *barrier = 0;
    __armos_pthread_registry_unlock();
    return 0;
}

int pthread_barrier_wait(pthread_barrier_t *barrier)
{
    barrier_slot_t *slot;
    uint32_t generation;
    uint32_t waiting;
    int result = barrier_resolve(barrier, &slot);

    if (result != 0)
        return result;
    generation =
        __atomic_load_n(&slot->generation_word, __ATOMIC_ACQUIRE);
    waiting = __atomic_add_fetch(&slot->waiting, 1u, __ATOMIC_ACQ_REL);
    if (waiting == slot->count) {
        __atomic_store_n(&slot->waiting, 0u, __ATOMIC_RELEASE);
        __atomic_add_fetch(&slot->generation_word, 1u, __ATOMIC_RELEASE);
        (void)wake_word(&slot->generation_word, 0x7fffffffu);
        return PTHREAD_BARRIER_SERIAL_THREAD;
    }
    while (__atomic_load_n(&slot->generation_word, __ATOMIC_ACQUIRE) ==
           generation) {
        result = __armos_pthread_wait(
            &slot->generation_word, generation, NULL, 0);
        if (result != 0 && result != EAGAIN && result != EINTR)
            return result;
    }
    return 0;
}

int pthread_spin_init(pthread_spinlock_t *lock, int pshared)
{
    if (!lock)
        return EINVAL;
    if (pshared == PTHREAD_PROCESS_SHARED)
        return ENOTSUP;
    if (pshared != PTHREAD_PROCESS_PRIVATE)
        return EINVAL;
    *lock = 0;
    return 0;
}

int pthread_spin_destroy(pthread_spinlock_t *lock)
{
    if (!lock || __atomic_load_n(lock, __ATOMIC_ACQUIRE) != 0)
        return EBUSY;
    return 0;
}

int pthread_spin_trylock(pthread_spinlock_t *lock)
{
    uint32_t expected = 0;

    if (!lock)
        return EINVAL;
    return __atomic_compare_exchange_n(lock, &expected, 1u, 0,
                                       __ATOMIC_ACQUIRE,
                                       __ATOMIC_RELAXED) ? 0 : EBUSY;
}

int pthread_spin_lock(pthread_spinlock_t *lock)
{
    if (!lock)
        return EINVAL;
    while (pthread_spin_trylock(lock) == EBUSY)
        (void)sched_yield();
    return 0;
}

int pthread_spin_unlock(pthread_spinlock_t *lock)
{
    if (!lock || __atomic_exchange_n(lock, 0u, __ATOMIC_RELEASE) == 0)
        return EPERM;
    return 0;
}

static int semaphore_allocate(sem_t *sem, unsigned int value)
{
    int result = EAGAIN;

    __armos_pthread_registry_lock();
    for (unsigned int index = 0; index < SEMAPHORE_MAX; index++) {
        semaphore_slot_t *slot = &semaphore_table[index];

        if (!slot->used) {
            uint32_t generation = next_generation(slot->generation);

            memset(slot, 0, sizeof(*slot));
            slot->generation = generation;
            slot->used = 1;
            slot->value = value;
            *sem = make_handle(index, generation);
            result = 0;
            break;
        }
    }
    __armos_pthread_registry_unlock();
    return result;
}

static int semaphore_resolve(sem_t *sem, semaphore_slot_t **slot)
{
    uint32_t generation;
    unsigned int index;

    if (!sem || !slot ||
        decode_handle(*sem, SEMAPHORE_MAX, &index, &generation) != 0)
        return EINVAL;
    *slot = &semaphore_table[index];
    return (*slot)->used && (*slot)->generation == generation ?
        0 : EINVAL;
}

static int sem_fail(int error)
{
    errno = error;
    return -1;
}

int sem_init(sem_t *sem, int pshared, unsigned int value)
{
    int result;

    if (!sem || value > SEM_VALUE_MAX)
        return sem_fail(EINVAL);
    if (pshared)
        return sem_fail(ENOTSUP);
    *sem = 0;
    result = semaphore_allocate(sem, value);
    return result == 0 ? 0 : sem_fail(result);
}

int sem_destroy(sem_t *sem)
{
    semaphore_slot_t *slot;
    int result = semaphore_resolve(sem, &slot);

    if (result != 0)
        return sem_fail(result);
    if (__atomic_load_n(&slot->waiters, __ATOMIC_ACQUIRE) != 0)
        return sem_fail(EBUSY);
    __armos_pthread_registry_lock();
    slot->used = 0;
    *sem = 0;
    __armos_pthread_registry_unlock();
    return 0;
}

static int sem_wait_common(sem_t *sem, const struct timespec *absolute,
                           int try_only)
{
    semaphore_slot_t *slot;
    armos_pthread_control_t *control = __armos_pthread_current;
    armos_timespec_t relative;
    const armos_timespec_t *timeout = NULL;
    int result = semaphore_resolve(sem, &slot);

    if (result != 0)
        return sem_fail(result);
    if (absolute) {
        result = __armos_pthread_relative_timeout(
            CLOCK_REALTIME, absolute, &relative);
        if (result != 0)
            return sem_fail(result);
        timeout = &relative;
    }
    for (;;) {
        uint32_t value = __atomic_load_n(&slot->value, __ATOMIC_ACQUIRE);

        while (value > 0) {
            if (__atomic_compare_exchange_n(
                    &slot->value, &value, value - 1u, 0,
                    __ATOMIC_ACQUIRE, __ATOMIC_RELAXED))
                return 0;
        }
        if (try_only)
            return sem_fail(EAGAIN);
        {
            uint32_t sequence =
                __atomic_load_n(&slot->sequence, __ATOMIC_ACQUIRE);

            pthread_testcancel();
            __atomic_add_fetch(&slot->waiters, 1u, __ATOMIC_ACQ_REL);
            if (control) {
                __atomic_store_n(&control->cancel_address, &slot->sequence,
                                 __ATOMIC_RELEASE);
                __atomic_store_n(&control->cancel_address_changes, 1u,
                                 __ATOMIC_RELEASE);
            }
            result = __armos_pthread_wait(
                &slot->sequence, sequence, timeout, 0);
            if (control) {
                __atomic_store_n(&control->cancel_address, NULL,
                                 __ATOMIC_RELEASE);
                __atomic_store_n(&control->cancel_address_changes, 0u,
                                 __ATOMIC_RELEASE);
            }
            __atomic_sub_fetch(&slot->waiters, 1u, __ATOMIC_ACQ_REL);
            pthread_testcancel();
        }
        if (result == ETIMEDOUT)
            return sem_fail(ETIMEDOUT);
        if (result != 0 && result != EAGAIN && result != EINTR)
            return sem_fail(result);
    }
}

int sem_wait(sem_t *sem)
{
    return sem_wait_common(sem, NULL, 0);
}

int sem_trywait(sem_t *sem)
{
    return sem_wait_common(sem, NULL, 1);
}

int sem_timedwait(sem_t *sem, const struct timespec *absolute)
{
    return sem_wait_common(sem, absolute, 0);
}

int sem_post(sem_t *sem)
{
    semaphore_slot_t *slot;
    uint32_t value;
    int result = semaphore_resolve(sem, &slot);

    if (result != 0)
        return sem_fail(result);
    value = __atomic_load_n(&slot->value, __ATOMIC_ACQUIRE);
    for (;;) {
        if (value == SEM_VALUE_MAX)
            return sem_fail(EOVERFLOW);
        if (__atomic_compare_exchange_n(
                &slot->value, &value, value + 1u, 0,
                __ATOMIC_RELEASE, __ATOMIC_RELAXED))
            break;
    }
    __atomic_add_fetch(&slot->sequence, 1u, __ATOMIC_RELEASE);
    (void)wake_word(&slot->sequence, 1u);
    return 0;
}

int sem_getvalue(sem_t *sem, int *value)
{
    semaphore_slot_t *slot;
    int result = semaphore_resolve(sem, &slot);

    if (result != 0 || !value)
        return sem_fail(result ? result : EINVAL);
    *value = (int)__atomic_load_n(&slot->value, __ATOMIC_ACQUIRE);
    return 0;
}
