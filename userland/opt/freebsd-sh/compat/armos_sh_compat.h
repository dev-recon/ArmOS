/*
 * ArmOS
 * Copyright (c) 2026 Mohamed Ennassiri
 *
 * Licensed under the Apache License, Version 2.0.
 * See LICENSE for details.
 *
 * File: userland/opt/freebsd-sh/compat/armos_sh_compat.h
 * Layer: Userland / FreeBSD sh portability
 *
 * Responsibilities:
 * - Describe compiler and libc compatibility missing from bare-metal newlib.
 * - Keep BSD-only source assumptions outside the ArmOS kernel.
 */

#ifndef ARMOS_SH_COMPAT_H
#define ARMOS_SH_COMPAT_H

#include <limits.h>
#include <setjmp.h>
#include <signal.h>
#include <stddef.h>
#include <stdio.h>
#include <stdint.h>
#include <sys/resource.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <wchar.h>

#ifndef __dead2
#define __dead2 __attribute__((__noreturn__))
#endif
#ifndef __dead
#define __dead __attribute__((__noreturn__))
#endif
#ifndef __unused
#define __unused __attribute__((__unused__))
#endif
#ifndef __nonstring
#define __nonstring
#endif
#ifndef __printflike
#define __printflike(a, b) __attribute__((__format__(__printf__, a, b)))
#endif
#ifndef __printf0like
#define __printf0like(a, b) __attribute__((__format__(__printf__, a, b)))
#endif
#ifndef __DECONST
#define __DECONST(type, value) ((type)(uintptr_t)(const void *)(value))
#endif
#ifndef ALIGN
#define ALIGN(value) (((value) + sizeof(void *) - 1u) & ~(sizeof(void *) - 1u))
#endif
#ifndef CLOCK_UPTIME
#define CLOCK_UPTIME CLOCK_MONOTONIC
#endif
/* Older ArmOS newlib sysroots exposed a permanently-false placeholder. */
#undef WCONTINUED
#define WCONTINUED 8
#undef WIFCONTINUED
#define WIFCONTINUED(status) ((status) == 0xffff)
#ifndef WCOREDUMP
#define WCOREDUMP(status) 0
#endif
#ifndef SA_ONSTACK
#define SA_ONSTACK 0
#endif
#ifndef _PATH_TTY
#define _PATH_TTY "/dev/tty"
#endif
#ifndef _PATH_CONSOLE
#define _PATH_CONSOLE "/dev/tty0"
#endif
#ifndef _PATH_DEFPATH
#define _PATH_DEFPATH "/sbin:/bin:/usr/sbin:/usr/bin:/opt/freebsd-sh/bin"
#endif
#ifndef _PATH_STDPATH
#define _PATH_STDPATH _PATH_DEFPATH
#endif
#ifndef MAXLOGNAME
#define MAXLOGNAME 64
#endif
#ifndef O_VERIFY
#define O_VERIFY 0
#endif
#ifndef SIG2STR_MAX
#define SIG2STR_MAX 16
#endif

#define vfork fork

int eaccess(const char *, int);
char *strchrnul(const char *, int);
pid_t wait3(int *, int, struct rusage *);
pid_t wait4(pid_t, int *, int, struct rusage *);
int tcsetsid(int, pid_t);
void *setmode(const char *);
mode_t getmode(const void *, mode_t);
int sig2str(int, char *);
int str2sig(const char *, int *);
char *getlogin(void);
extern const char *const sys_signame[];
extern const int sys_nsig;
int wcwidth(wchar_t);
int wcswidth(const wchar_t *, size_t);
ssize_t getline(char **, size_t *, FILE *);
int asprintf(char **, const char *, ...);

#define qsort_s(base, count, size, compare, context) \
    qsort_r((base), (count), (size), (compare), (context))

#endif /* ARMOS_SH_COMPAT_H */
