/*
 * ArmOS
 * Copyright (c) 2026 Mohamed Ennassiri
 *
 * Licensed under the Apache License, Version 2.0.
 * See LICENSE for details.
 *
 * File: newlib-port/arm64/syscall_raw.S
 * Layer: Userland / newlib port / AArch64
 *
 * Responsibilities:
 * - Marshal newlib calls into the ArmOS AArch64 syscall ABI.
 * - Preserve the AAPCS64 return convention around SVC entry.
 * - Provide the userspace signal-return trampoline.
 *
 * Notes:
 * - Arguments already occupy x0..x5, while x8 carries the syscall number.
 */

.text
.align 4

.macro RAW_SYSCALL name, number
.global \name
.type \name, %function
\name:
    mov x8, #\number
    svc #0
    ret
.endm

RAW_SYSCALL sys_exit, 1
RAW_SYSCALL sys_fork, 2
RAW_SYSCALL sys_read, 3
RAW_SYSCALL sys_write, 4
RAW_SYSCALL sys_open, 5
RAW_SYSCALL sys_close, 6
RAW_SYSCALL sys_waitpid, 7
RAW_SYSCALL sys_creat, 8
RAW_SYSCALL sys_link, 9
RAW_SYSCALL sys_unlink, 10
RAW_SYSCALL sys_execve, 11
RAW_SYSCALL sys_chdir, 12
RAW_SYSCALL sys_time, 13
RAW_SYSCALL sys_mknod, 14
RAW_SYSCALL sys_chmod, 15
RAW_SYSCALL sys_chown, 16
RAW_SYSCALL sys_lseek, 19
RAW_SYSCALL sys_mount, 21
RAW_SYSCALL sys_umount, 22
RAW_SYSCALL sys_setuid, 23
RAW_SYSCALL sys_getpid, 20
RAW_SYSCALL sys_getuid, 24
RAW_SYSCALL sys_alarm, 27
RAW_SYSCALL sys_pause, 29
RAW_SYSCALL sys_utime, 30
RAW_SYSCALL sys_stty, 31
RAW_SYSCALL sys_gtty, 32
RAW_SYSCALL sys_access, 33
RAW_SYSCALL sys_nice, 34
RAW_SYSCALL sys_sync, 36
RAW_SYSCALL sys_kill, 37
RAW_SYSCALL sys_rename, 38
RAW_SYSCALL sys_mkdir, 39
RAW_SYSCALL sys_rmdir, 40
RAW_SYSCALL sys_dup, 41
RAW_SYSCALL sys_pipe, 42
RAW_SYSCALL sys_times, 43
RAW_SYSCALL sys_brk, 45
RAW_SYSCALL sys_setgid, 46
RAW_SYSCALL sys_getgid, 47
RAW_SYSCALL sys_geteuid, 49
RAW_SYSCALL sys_getegid, 50
RAW_SYSCALL sys_ioctl, 54
RAW_SYSCALL sys_fcntl, 55
RAW_SYSCALL sys_setpgid, 57
RAW_SYSCALL sys_umask, 60
RAW_SYSCALL sys_dup2, 63
RAW_SYSCALL sys_getpgrp, 65
RAW_SYSCALL sys_setsid, 66
RAW_SYSCALL sys_sigaction, 67
RAW_SYSCALL sys_sigsuspend, 72
RAW_SYSCALL sys_sigpending, 73
RAW_SYSCALL sys_setrlimit, 75
RAW_SYSCALL sys_getrlimit, 76
RAW_SYSCALL sys_getrusage, 77
RAW_SYSCALL sys_gettimeofday, 78
RAW_SYSCALL sys_symlink, 83
RAW_SYSCALL sys_readlink, 85
RAW_SYSCALL sys_truncate, 92
RAW_SYSCALL sys_ftruncate, 93
RAW_SYSCALL sys_fchmod, 94
RAW_SYSCALL sys_fchown, 95
RAW_SYSCALL sys_getpriority, 96
RAW_SYSCALL sys_setpriority, 97
RAW_SYSCALL sys_statfs, 99
RAW_SYSCALL sys_statvfs, 266
RAW_SYSCALL sys_fstatvfs, 267
RAW_SYSCALL sys_utimensat, 268
RAW_SYSCALL sys_futimens, 269
RAW_SYSCALL sys_stat, 106
RAW_SYSCALL sys_lstat, 107
RAW_SYSCALL sys_fstat, 108
RAW_SYSCALL sys_wait4, 114
RAW_SYSCALL sys_sysinfo, 116
RAW_SYSCALL sys_fsync, 118
RAW_SYSCALL sys_getppid, 119
RAW_SYSCALL sys_clone, 120
RAW_SYSCALL sys_uname, 122
RAW_SYSCALL sys_mprotect, 125
RAW_SYSCALL sys_sigprocmask, 126
RAW_SYSCALL sys_getdents, 141
RAW_SYSCALL sys_select, 142
RAW_SYSCALL sys_readv, 145
RAW_SYSCALL sys_writev, 146
RAW_SYSCALL sys_getsid, 147
RAW_SYSCALL sys_fdatasync, 148
RAW_SYSCALL sys_sched_yield, 158
RAW_SYSCALL sys_nanosleep, 162
RAW_SYSCALL sys_poll, 168
RAW_SYSCALL sys_pread, 180
RAW_SYSCALL sys_pwrite, 181
RAW_SYSCALL sys_getcwd, 183
RAW_SYSCALL sys_shm_open, 190
RAW_SYSCALL sys_shm_unlink, 191
RAW_SYSCALL sys_shm_map, 192
RAW_SYSCALL sys_shm_unmap, 193
RAW_SYSCALL sys_shutdown, 194
RAW_SYSCALL sys_mmap, 195
RAW_SYSCALL sys_munmap, 196
RAW_SYSCALL sys_sysconf, 197
RAW_SYSCALL sys_gettid, 224
RAW_SYSCALL sys_clock_gettime, 263
RAW_SYSCALL sys_clock_getres, 264
RAW_SYSCALL sys_clock_nanosleep, 265
RAW_SYSCALL sys_socket, 281
RAW_SYSCALL sys_bind, 282
RAW_SYSCALL sys_connect, 283
RAW_SYSCALL sys_listen, 284
RAW_SYSCALL sys_accept, 285
RAW_SYSCALL sys_sendto, 286
RAW_SYSCALL sys_recvfrom, 287
RAW_SYSCALL sys_socket_shutdown, 288
RAW_SYSCALL sys_resolve, 289
RAW_SYSCALL sys_openat, 322
RAW_SYSCALL sys_mkdirat, 323
RAW_SYSCALL sys_fstatat, 327
RAW_SYSCALL sys_unlinkat, 328
RAW_SYSCALL sys_renameat, 329
RAW_SYSCALL sys_thread_exit, 330

.global __signal_return_trampoline
.type __signal_return_trampoline, %function
__signal_return_trampoline:
    mov x8, #173
    svc #0
    mov x8, #1
    mov x0, #-1
    svc #0

.size __signal_return_trampoline, . - __signal_return_trampoline
