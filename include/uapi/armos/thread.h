/*
 * ArmOS
 * Copyright (c) 2026 Mohamed Ennassiri
 *
 * Licensed under the Apache License, Version 2.0.
 * See LICENSE for details.
 *
 * File: include/uapi/armos/thread.h
 * Layer: UAPI / thread creation
 *
 * Responsibilities:
 * - Define the architecture-neutral argument block used to create a thread.
 * - Keep the first ArmOS thread ABI extensible without consuming syscall
 *   argument registers for every future clone option.
 *
 * Notes:
 * - Pointer-sized fields follow the native ARM32 or ARM64 user ABI.
 * - The initial kernel accepts the mandatory shared-process flags plus the
 *   parent/child TID publication flags defined below.
 */

#ifndef _UAPI_ARMOS_THREAD_H
#define _UAPI_ARMOS_THREAD_H

#define ARMOS_CLONE_VM              0x00000100UL
#define ARMOS_CLONE_FS              0x00000200UL
#define ARMOS_CLONE_FILES           0x00000400UL
#define ARMOS_CLONE_SIGHAND         0x00000800UL
#define ARMOS_CLONE_PARENT_SETTID   0x00100000UL
#define ARMOS_CLONE_CHILD_CLEARTID  0x00200000UL
#define ARMOS_CLONE_CHILD_SETTID    0x01000000UL
#define ARMOS_CLONE_THREAD          0x00010000UL

#define ARMOS_CLONE_THREAD_REQUIRED \
    (ARMOS_CLONE_VM | ARMOS_CLONE_FS | ARMOS_CLONE_FILES | \
     ARMOS_CLONE_SIGHAND | ARMOS_CLONE_THREAD)

#define ARMOS_CLONE_THREAD_SUPPORTED \
    (ARMOS_CLONE_THREAD_REQUIRED | ARMOS_CLONE_PARENT_SETTID | \
     ARMOS_CLONE_CHILD_SETTID | ARMOS_CLONE_CHILD_CLEARTID)

typedef struct {
    unsigned long flags;
    unsigned long stack;
    unsigned long stack_size;
    unsigned long entry;
    unsigned long argument;
    unsigned long tls;
    unsigned long parent_tid;
    unsigned long child_tid;
} armos_clone_args_t;

#endif /* _UAPI_ARMOS_THREAD_H */
