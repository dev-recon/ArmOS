/*
 * ArmOS
 * Copyright (c) 2026 Mohamed Ennassiri
 *
 * Licensed under the Apache License, Version 2.0.
 * See LICENSE for details.
 *
 * File: include/uapi/armos/syscall.h
 * Layer: UAPI / syscall ABI
 *
 * Responsibilities:
 * - Define syscall numbers shared by kernel and architecture entry paths.
 * - Preserve the architecture-neutral ArmOS number space used by userland.
 *
 * Notes:
 * - Each architecture defines its own register calling convention.
 * - sched_yield uses the established Linux-compatible syscall slot 158.
 */

#ifndef _UAPI_ARMOS_SYSCALL_H
#define _UAPI_ARMOS_SYSCALL_H

#define ARMOS_NR_EXIT       1
#define ARMOS_NR_FORK       2
#define ARMOS_NR_READ       3
#define ARMOS_NR_WRITE      4
#define ARMOS_NR_OPEN       5
#define ARMOS_NR_CLOSE      6
#define ARMOS_NR_WAITPID    7
#define ARMOS_NR_EXECVE     11
#define ARMOS_NR_CHDIR      12
#define ARMOS_NR_GETPID     20
#define ARMOS_NR_KILL       37
#define ARMOS_NR_PIPE       42
#define ARMOS_NR_BRK        45
#define ARMOS_NR_SETPGID    57
#define ARMOS_NR_DUP2       63
#define ARMOS_NR_GETPGRP    65
#define ARMOS_NR_SIGACTION  67
#define ARMOS_NR_SETRLIMIT  75
#define ARMOS_NR_GETRLIMIT  76
#define ARMOS_NR_FCHMOD     94
#define ARMOS_NR_FCHOWN     95
#define ARMOS_NR_GETPPID    119
#define ARMOS_NR_CLONE      120
#define ARMOS_NR_FDATASYNC  148
#define ARMOS_NR_SCHED_YIELD 158
#define ARMOS_NR_NANOSLEEP  162
#define ARMOS_NR_PREAD      180
#define ARMOS_NR_PWRITE     181
#define ARMOS_NR_GETCWD     183
#define ARMOS_NR_SHUTDOWN   194
#define ARMOS_NR_MMAP       195
#define ARMOS_NR_MUNMAP     196
#define ARMOS_NR_SYSCONF    197
#define ARMOS_NR_GETTID     224
#define ARMOS_NR_CLOCK_GETTIME 263
#define ARMOS_NR_CLOCK_GETRES  264
#define ARMOS_NR_CLOCK_NANOSLEEP 265
#define ARMOS_NR_STATVFS         266
#define ARMOS_NR_FSTATVFS        267
#define ARMOS_NR_UTIMENSAT       268
#define ARMOS_NR_FUTIMENS        269
#define ARMOS_NR_SENDTO          286
#define ARMOS_NR_RECVFROM        287
#define ARMOS_NR_SOCKET_SHUTDOWN 288
#define ARMOS_NR_RESOLVE         289
#define ARMOS_NR_OPENAT        322
#define ARMOS_NR_MKDIRAT       323
#define ARMOS_NR_FSTATAT       327
#define ARMOS_NR_UNLINKAT      328
#define ARMOS_NR_RENAMEAT      329
#define ARMOS_NR_THREAD_EXIT   330
#define ARMOS_NR_FUTEX         331
#define ARMOS_NR_SET_TLS       332
#define ARMOS_NR_GET_TLS_INFO  333
#define ARMOS_SYSCALL_MAX 512

/* Values shared with the newlib _SC_* namespace used by ArmOS. */
#define ARMOS_SC_ARG_MAX          0
#define ARMOS_SC_CHILD_MAX        1
#define ARMOS_SC_CLK_TCK          2
#define ARMOS_SC_OPEN_MAX         4
#define ARMOS_SC_JOB_CONTROL      5
#define ARMOS_SC_SAVED_IDS        6
#define ARMOS_SC_VERSION          7
#define ARMOS_SC_PAGESIZE         8
#define ARMOS_SC_NPROCESSORS_CONF 9
#define ARMOS_SC_NPROCESSORS_ONLN 10
#define ARMOS_SC_PHYS_PAGES       11
#define ARMOS_SC_AVPHYS_PAGES     12
#define ARMOS_SC_FSYNC            22
#define ARMOS_SC_MAPPED_FILES     23
#define ARMOS_SC_MEMORY_PROTECTION 26
#define ARMOS_SC_SHARED_MEMORY_OBJECTS 31
#define ARMOS_SC_TIMERS           33
#define ARMOS_SC_IOV_MAX          66
#define ARMOS_SC_MONOTONIC_CLOCK  69

#define ARMOS_POSIX_VERSION 200809

/* Raw kernel result used for a known but unsupported sysconf selector. */
#define ARMOS_SYSCONF_UNSUPPORTED (-38)

#endif
