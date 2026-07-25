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
 * - Workers deliberately avoid libc calls because newlib reentrancy and TLS
 *   are part of the next pthread milestone.
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
#define THREAD_WAIT_LIMIT 1000000u

extern int sched_yield(void);

typedef struct {
    unsigned int index;
    volatile unsigned int alive_tid;
    volatile unsigned int done;
    volatile unsigned int observed_tid;
    unsigned int created_tid;
    volatile int observed_pid;
    volatile uintptr_t observed_stack;
    void *allocation;
    uintptr_t stack_base;
    uintptr_t stack_top;
} worker_state_t;

static worker_state_t workers[THREAD_COUNT];

static void thread_worker(void *opaque)
{
    worker_state_t *state = (worker_state_t *)opaque;
    unsigned int stack_local = state->index;

    state->observed_tid = (unsigned int)armos_gettid();
    state->observed_pid = getpid();
    state->observed_stack = (uintptr_t)&stack_local;
    __atomic_store_n(&state->done, 1u, __ATOMIC_RELEASE);
    armos_thread_exit(0);
}

static int wait_for_worker(worker_state_t *state)
{
    for (unsigned int spin = 0; spin < THREAD_WAIT_LIMIT; spin++) {
        unsigned int done = __atomic_load_n(&state->done, __ATOMIC_ACQUIRE);
        unsigned int alive =
            __atomic_load_n(&state->alive_tid, __ATOMIC_ACQUIRE);

        if (done && alive == 0u)
            return 0;
        sched_yield();
    }
    return -1;
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
        if (!state->allocation) {
            printf("threadtest: stack allocation %u failed\n", i);
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

    for (unsigned int i = 0; i < THREAD_COUNT; i++)
        free(workers[i].allocation);

    if (failures) {
        printf("threadtest: failed (%d failure%s)\n",
               failures, failures == 1 ? "" : "s");
        return 1;
    }

    printf("threadtest: passed\n");
    return 0;
}
