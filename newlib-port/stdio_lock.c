/*
 * ArmOS
 * Copyright (c) 2026 Mohamed Ennassiri
 *
 * Licensed under the Apache License, Version 2.0.
 * See LICENSE for details.
 *
 * File: newlib-port/stdio_lock.c
 * Layer: Userspace / Newlib synchronization
 *
 * Responsibilities:
 * - Retarget Newlib's internal locks to ArmOS futex primitives.
 * - Provide the POSIX flockfile family advertised by the ArmOS sysroot.
 * - Keep the implementation shared by every architecture and platform.
 *
 * Notes:
 * - Locks are recursive when requested by Newlib's stdio implementation.
 * - Dynamic lock storage is bounded consistently with the process file limit.
 */

#include <stdint.h>
#include <stdio.h>
#include <sys/lock.h>
#include <sys/stdio.h>
#include <uapi/armos/futex.h>
#include <uapi/armos/time.h>

#define ARMOS_NEWLIB_DYNAMIC_LOCKS 272u

struct __lock {
    volatile uint32_t allocated;
    volatile uint32_t initialized;
    volatile uint32_t word;
    volatile uint32_t owner;
    uint32_t recursion;
    uint32_t recursive;
};

extern long sys_gettid(void);
extern long sys_futex(volatile uint32_t *address, int operation,
                      uint32_t value, const armos_timespec_t *timeout);

struct __lock __lock___sfp_recursive_mutex;
struct __lock __lock___atexit_recursive_mutex;
struct __lock __lock___at_quick_exit_mutex;
struct __lock __lock___malloc_recursive_mutex;
struct __lock __lock___env_recursive_mutex;
struct __lock __lock___tz_mutex;
struct __lock __lock___dd_hash_mutex;
struct __lock __lock___arc4random_mutex;

static struct __lock dynamic_locks[ARMOS_NEWLIB_DYNAMIC_LOCKS];
static struct __lock fallback_lock;
static volatile uint32_t allocation_lock;

static void wait_word(volatile uint32_t *word, uint32_t expected)
{
    (void)sys_futex(word, ARMOS_FUTEX_WAIT, expected, NULL);
}

static void wake_word(volatile uint32_t *word, uint32_t count)
{
    (void)sys_futex(word, ARMOS_FUTEX_WAKE, count, NULL);
}

static void allocation_lock_acquire(void)
{
    for (;;) {
        uint32_t expected = 0;

        if (__atomic_compare_exchange_n(&allocation_lock, &expected, 1u, 0,
                                        __ATOMIC_ACQUIRE,
                                        __ATOMIC_RELAXED))
            return;
        wait_word(&allocation_lock, 1u);
    }
}

static void allocation_lock_release(void)
{
    __atomic_store_n(&allocation_lock, 0u, __ATOMIC_RELEASE);
    wake_word(&allocation_lock, 1u);
}

static void lock_prepare(struct __lock *lock, int recursive)
{
    uint32_t expected;

    if (!lock)
        return;
    if (__atomic_load_n(&lock->initialized, __ATOMIC_ACQUIRE) == 2u)
        return;

    expected = 0;
    if (__atomic_compare_exchange_n(&lock->initialized, &expected, 1u, 0,
                                    __ATOMIC_ACQ_REL,
                                    __ATOMIC_RELAXED)) {
        lock->word = 0;
        lock->owner = 0;
        lock->recursion = 0;
        lock->recursive = recursive ? 1u : 0u;
        __atomic_store_n(&lock->initialized, 2u, __ATOMIC_RELEASE);
        wake_word(&lock->initialized, 0x7fffffffu);
        return;
    }

    while (__atomic_load_n(&lock->initialized, __ATOMIC_ACQUIRE) != 2u)
        wait_word(&lock->initialized, 1u);
}

static struct __lock *lock_allocate(int recursive)
{
    struct __lock *result = NULL;

    allocation_lock_acquire();
    for (unsigned int index = 0;
         index < ARMOS_NEWLIB_DYNAMIC_LOCKS; index++) {
        struct __lock *lock = &dynamic_locks[index];

        if (!lock->allocated) {
            lock->allocated = 1u;
            lock->initialized = 0;
            result = lock;
            break;
        }
    }
    allocation_lock_release();

    if (!result)
        result = &fallback_lock;
    lock_prepare(result, recursive);
    return result;
}

static int lock_is_dynamic(const struct __lock *lock)
{
    uintptr_t address = (uintptr_t)lock;

    return address >= (uintptr_t)&dynamic_locks[0] &&
           address < (uintptr_t)&dynamic_locks[ARMOS_NEWLIB_DYNAMIC_LOCKS];
}

static void lock_acquire(struct __lock *lock, int recursive)
{
    uint32_t tid;

    if (!lock)
        return;
    lock_prepare(lock, recursive);
    tid = (uint32_t)sys_gettid();

    if (lock->recursive &&
        __atomic_load_n(&lock->owner, __ATOMIC_ACQUIRE) == tid) {
        lock->recursion++;
        return;
    }

    for (;;) {
        uint32_t expected = 0;

        if (__atomic_compare_exchange_n(&lock->word, &expected, 1u, 0,
                                        __ATOMIC_ACQUIRE,
                                        __ATOMIC_RELAXED)) {
            __atomic_store_n(&lock->owner, tid, __ATOMIC_RELEASE);
            lock->recursion = 1u;
            return;
        }
        wait_word(&lock->word, 1u);
    }
}

static int lock_try_acquire(struct __lock *lock, int recursive)
{
    uint32_t expected = 0;
    uint32_t tid;

    if (!lock)
        return 1;
    lock_prepare(lock, recursive);
    tid = (uint32_t)sys_gettid();

    if (lock->recursive &&
        __atomic_load_n(&lock->owner, __ATOMIC_ACQUIRE) == tid) {
        lock->recursion++;
        return 1;
    }
    if (!__atomic_compare_exchange_n(&lock->word, &expected, 1u, 0,
                                     __ATOMIC_ACQUIRE,
                                     __ATOMIC_RELAXED))
        return 0;
    __atomic_store_n(&lock->owner, tid, __ATOMIC_RELEASE);
    lock->recursion = 1u;
    return 1;
}

static void lock_release(struct __lock *lock)
{
    uint32_t tid;

    if (!lock)
        return;
    tid = (uint32_t)sys_gettid();
    if (__atomic_load_n(&lock->owner, __ATOMIC_ACQUIRE) != tid)
        return;
    if (lock->recursive && lock->recursion > 1u) {
        lock->recursion--;
        return;
    }

    lock->recursion = 0;
    __atomic_store_n(&lock->owner, 0u, __ATOMIC_RELEASE);
    __atomic_store_n(&lock->word, 0u, __ATOMIC_RELEASE);
    wake_word(&lock->word, 1u);
}

void __retarget_lock_init(_LOCK_T *lock)
{
    if (lock)
        *lock = lock_allocate(0);
}

void __retarget_lock_init_recursive(_LOCK_T *lock)
{
    if (lock)
        *lock = lock_allocate(1);
}

void __retarget_lock_close(_LOCK_T lock)
{
    if (!lock || !lock_is_dynamic(lock))
        return;
    allocation_lock_acquire();
    lock->initialized = 0;
    lock->allocated = 0;
    allocation_lock_release();
}

void __retarget_lock_close_recursive(_LOCK_T lock)
{
    __retarget_lock_close(lock);
}

void __retarget_lock_acquire(_LOCK_T lock)
{
    lock_acquire(lock, 0);
}

void __retarget_lock_acquire_recursive(_LOCK_T lock)
{
    lock_acquire(lock, 1);
}

int __retarget_lock_try_acquire(_LOCK_T lock)
{
    return lock_try_acquire(lock, 0);
}

int __retarget_lock_try_acquire_recursive(_LOCK_T lock)
{
    return lock_try_acquire(lock, 1);
}

void __retarget_lock_release(_LOCK_T lock)
{
    lock_release(lock);
}

void __retarget_lock_release_recursive(_LOCK_T lock)
{
    lock_release(lock);
}

void flockfile(FILE *stream)
{
    if (stream)
        (void)_flockfile(stream);
}

int ftrylockfile(FILE *stream)
{
    if (!stream)
        return -1;
    if (stream->_flags & __SSTR)
        return 0;
    return __lock_try_acquire_recursive(stream->_lock) ? 0 : -1;
}

void funlockfile(FILE *stream)
{
    if (stream)
        (void)_funlockfile(stream);
}
