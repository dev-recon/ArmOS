/*
 * ArmOS
 * Copyright (c) 2026 Mohamed Ennassiri
 *
 * Licensed under the Apache License, Version 2.0.
 * See LICENSE for details.
 *
 * File: include/kernel/signal.h
 * Layer: Kernel / public internal interface
 *
 * Responsibilities:
 * - Declare kernel types, constants, and subsystem contracts.
 * - Keep cross-module ABI and structure expectations explicit.
 *
 * Notes:
 * - Header changes can ripple across kernel and user ABI glue.
 */

#ifndef _KERNEL_SIGNAL_H
#define _KERNEL_SIGNAL_H

#include <kernel/types.h>
#include <kernel/task.h>
#include <kernel/user_layout.h>
#include <uapi/armos/signal.h>


/* Signal numbers */
#define SIGHUP      1
#define SIGINT      2
#define SIGQUIT     3
#define SIGILL      4
#define SIGTRAP     5
#define SIGABRT     6
#define SIGBUS      7
#define SIGFPE      8
#define SIGKILL     9
#define SIGUSR1    10
#define SIGSEGV    11
#define SIGUSR2    12
#define SIGPIPE    13
#define SIGALRM    14
#define SIGTERM    15
#define SIGCHLD    17
#define SIGCONT    18
#define SIGSTOP    19
#define SIGTSTP    20
#define SIGTTIN    21
#define SIGTTOU    22
#define SIGWINCH   28

/*
 * Per-process signal stack defaults.
 *
 * The signal stack lives in the architecture-provided user signal region,
 * between normal user mappings and the kernel split.
 */
#define DEFAULT_SIGNAL_STACK_SIZE   (16 * 1024u)
#define MAX_SIGNAL_STACK_SIZE       (1024 * 1024u)
#define SIGNAL_STACK_BASE_DEFAULT   (USER_SIGNAL_REGION_START + DEFAULT_SIGNAL_STACK_SIZE)

/* Signal flags */
#define SA_RESTART    ARMOS_SA_RESTART
#define SA_NODEFER    ARMOS_SA_NODEFER
#define SA_RESETHAND  ARMOS_SA_RESETHAND
#define SA_NOCLDWAIT  ARMOS_SA_NOCLDWAIT

typedef enum {
    SIGNAL_CHECK_NONE = 0,
    SIGNAL_CHECK_IGNORED,
    SIGNAL_CHECK_USER_FRAME,
    SIGNAL_CHECK_STOPPED,
    SIGNAL_CHECK_EXITED,
    SIGNAL_CHECK_DEFERRED
} signal_check_result_t;

/* Signal functions */
void init_process_signals(task_t* proc);
int send_signal(task_t* target, int sig);
signal_check_result_t check_pending_signals(void);
int signal_consume_user_return_override(void);
signal_check_result_t deliver_signal(task_t* proc, int sig);
bool has_pending_signals(task_t* proc);

void print_signal_stack_stats(void);
void cleanup_process_signals(task_t* proc);
void init_signal_stack_allocator(void);
   
#endif
