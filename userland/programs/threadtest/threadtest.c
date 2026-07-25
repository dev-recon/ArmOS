/*
 * ArmOS
 * Copyright (c) 2026 Mohamed Ennassiri
 *
 * Licensed under the Apache License, Version 2.0.
 * See LICENSE for details.
 *
 * File: userland/programs/threadtest/threadtest.c
 * Layer: Userland / thread validation
 *
 * Responsibilities:
 * - Validate native thread creation before libpthread is introduced.
 * - Verify shared process memory, distinct TIDs and independent user stacks.
 *
 * Notes:
 * - Workers verify per-thread newlib reentrancy and clear-child-TID futex wake.
 */

#include <armos/thread.h>
#include <errno.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define THREAD_COUNT 4u
#define THREAD_STACK_SIZE (64u * 1024u)
#define THREAD_STACK_ALIGNMENT 16u
#define FUTEX_WAKE_ATTEMPTS 10000u

extern int sched_yield(void);

typedef struct {
    unsigned int index;
    volatile uint32_t alive_tid;
    volatile uint32_t futex_gate;
    volatile unsigned int futex_ready;
    volatile unsigned int done;
    volatile uint32_t observed_tid;
    uint32_t created_tid;
    volatile int observed_pid;
    volatile uintptr_t observed_stack;
    volatile int observed_errno;
    volatile int futex_wait_result;
    volatile int futex_wait_errno;
    volatile unsigned int malloc_ok;
    void *allocation;
    void *reent;
    uintptr_t stack_base;
    uintptr_t stack_top;
} worker_state_t;

static worker_state_t workers[THREAD_COUNT];

static void thread_worker(void *opaque)
{
    worker_state_t *state = (worker_state_t *)opaque;
    unsigned int stack_local = state->index;
    armos_timespec_t timeout = {
        .sec = 5,
        .nsec = 0,
    };
    size_t scratch_size = 128u + state->index;
    void *scratch;

    state->observed_tid = (unsigned int)armos_gettid();
    state->observed_pid = getpid();
    state->observed_stack = (uintptr_t)&stack_local;
    errno = 100 + (int)state->index;

    scratch = malloc(scratch_size);
    if (scratch) {
        memset(scratch, (int)state->index, scratch_size);
        free(scratch);
        state->malloc_ok = 1u;
    }

    __atomic_store_n(&state->futex_ready, 1u, __ATOMIC_RELEASE);
    state->futex_wait_result =
        armos_futex_wait(&state->futex_gate, 0u, &timeout);
    state->futex_wait_errno =
        state->futex_wait_result < 0 ? errno : 0;
    sched_yield();
    state->observed_errno = errno;
    __atomic_store_n(&state->done, 1u, __ATOMIC_RELEASE);
    armos_thread_exit(0);
}

static int wait_for_worker(worker_state_t *state)
{
    armos_timespec_t timeout = {
        .sec = 5,
        .nsec = 0,
    };

    for (;;) {
        unsigned int done = __atomic_load_n(&state->done, __ATOMIC_ACQUIRE);
        uint32_t alive =
            __atomic_load_n(&state->alive_tid, __ATOMIC_ACQUIRE);

        if (done && alive == 0u)
            return 0;
        if (alive != 0u &&
            armos_futex_wait(&state->alive_tid, alive, &timeout) < 0 &&
            errno != EAGAIN && errno != EINTR)
            return -1;
    }
}

static int wake_worker(worker_state_t *state)
{
    for (unsigned int attempt = 0;
         attempt < FUTEX_WAKE_ATTEMPTS;
         attempt++) {
        int woken;

        if (!__atomic_load_n(&state->futex_ready, __ATOMIC_ACQUIRE)) {
            sched_yield();
            continue;
        }

        woken = armos_futex_wake(&state->futex_gate, 1u);
        if (woken == 1)
            return 0;
        if (woken < 0)
            return -1;
        sched_yield();
    }

    errno = ETIMEDOUT;
    return -1;
}

static int validate_futex_errors(void)
{
    volatile uint32_t word = 1u;
    armos_timespec_t timeout = {
        .sec = 0,
        .nsec = 5 * 1000 * 1000,
    };

    errno = 0;
    if (armos_futex_wait(&word, 0u, NULL) != -1 || errno != EAGAIN) {
        printf("threadtest: futex mismatch result invalid errno=%d\n", errno);
        return -1;
    }

    __atomic_store_n(&word, 0u, __ATOMIC_RELEASE);
    errno = 0;
    if (armos_futex_wait(&word, 0u, &timeout) != -1 ||
        errno != ETIMEDOUT) {
        printf("threadtest: futex timeout result invalid errno=%d\n", errno);
        return -1;
    }

    return 0;
}

int main(void)
{
    int process_pid = getpid();
    int failures = 0;
    unsigned int created = 0;

    memset(workers, 0, sizeof(workers));
    printf("threadtest: pid=%d creating %u native threads\n",
           process_pid, THREAD_COUNT);

    for (unsigned int i = 0; i < THREAD_COUNT; i++) {
        worker_state_t *state = &workers[i];
        uintptr_t raw;
        armos_clone_args_t args;
        int tid;

        state->index = i;
        state->allocation =
            malloc(THREAD_STACK_SIZE + THREAD_STACK_ALIGNMENT);
        state->reent = armos_thread_reent_create();
        if (!state->allocation || !state->reent) {
            printf("threadtest: runtime allocation %u failed\n", i);
            failures++;
            break;
        }

        raw = (uintptr_t)state->allocation;
        state->stack_base =
            (raw + THREAD_STACK_ALIGNMENT - 1u) &
            ~(uintptr_t)(THREAD_STACK_ALIGNMENT - 1u);
        state->stack_top = state->stack_base + THREAD_STACK_SIZE;

        memset(&args, 0, sizeof(args));
        args.flags = ARMOS_CLONE_THREAD_REQUIRED |
                     ARMOS_CLONE_CHILD_SETTID |
                     ARMOS_CLONE_CHILD_CLEARTID;
        args.stack = (unsigned long)state->stack_top;
        args.stack_size = THREAD_STACK_SIZE;
        args.entry = (unsigned long)(uintptr_t)thread_worker;
        args.argument = (unsigned long)(uintptr_t)state;
        args.tls = (unsigned long)(uintptr_t)state->reent;
        args.child_tid = (unsigned long)(uintptr_t)&state->alive_tid;

        tid = armos_clone(&args);
        if (tid < 0) {
            printf("threadtest: clone %u failed errno=%d\n", i, errno);
            failures++;
            break;
        }
        state->created_tid = (unsigned int)tid;
        created++;
        printf("threadtest: created worker=%u tid=%d stack=%p-%p\n",
               i, tid, (void *)state->stack_base, (void *)state->stack_top);
    }

    for (unsigned int i = 0; i < created; i++) {
        if (wake_worker(&workers[i]) < 0) {
            printf("threadtest: worker %u futex wake failed errno=%d\n",
                   i, errno);
            failures++;
        }
    }

    for (unsigned int i = 0; i < created; i++) {
        worker_state_t *state = &workers[i];

        if (wait_for_worker(state) < 0) {
            printf("threadtest: worker %u timed out alive=%u done=%u\n",
                   i, state->alive_tid, state->done);
            failures++;
            continue;
        }
        if (state->observed_pid != process_pid) {
            printf("threadtest: worker %u pid mismatch got=%d expected=%d\n",
                   i, state->observed_pid, process_pid);
            failures++;
        }
        if (state->observed_tid == 0u ||
            state->observed_tid != state->created_tid) {
            printf("threadtest: worker %u tid mismatch got=%u expected=%u\n",
                   i, state->observed_tid, state->created_tid);
            failures++;
        }
        if (state->observed_stack < state->stack_base ||
            state->observed_stack >= state->stack_top) {
            printf("threadtest: worker %u stack pointer %p out of range\n",
                   i, (void *)state->observed_stack);
            failures++;
        }
        if (state->observed_errno != 100 + (int)i) {
            printf("threadtest: worker %u errno mismatch got=%d expected=%d\n",
                   i, state->observed_errno, 100 + (int)i);
            failures++;
        }
        if (state->futex_wait_result != 0) {
            printf("threadtest: worker %u futex wait failed result=%d errno=%d\n",
                   i, state->futex_wait_result, state->futex_wait_errno);
            failures++;
        }
        if (!state->malloc_ok) {
            printf("threadtest: worker %u malloc failed\n", i);
            failures++;
        }
    }

    for (unsigned int i = 0; i < created; i++) {
        for (unsigned int j = i + 1u; j < created; j++) {
            if (workers[i].observed_tid == workers[j].observed_tid) {
                printf("threadtest: duplicate tid %u\n",
                       workers[i].observed_tid);
                failures++;
            }
            if (workers[i].observed_stack == workers[j].observed_stack) {
                printf("threadtest: duplicate stack address %p\n",
                       (void *)workers[i].observed_stack);
                failures++;
            }
        }
    }

    if (validate_futex_errors() < 0)
        failures++;

    for (unsigned int i = 0; i < THREAD_COUNT; i++) {
        armos_thread_reent_destroy(workers[i].reent);
        free(workers[i].allocation);
    }

    if (failures) {
        printf("threadtest: failed (%d failure%s)\n",
               failures, failures == 1 ? "" : "s");
        return 1;
    }

    printf("threadtest: passed\n");
    return 0;
}
