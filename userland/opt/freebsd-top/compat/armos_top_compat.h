/*
 * ArmOS compatibility contract for the FreeBSD top frontend.
 *
 * This header deliberately contains no architecture or platform knowledge.
 */
#ifndef ARMOS_TOP_COMPAT_H
#define ARMOS_TOP_COMPAT_H

#include <limits.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <sys/types.h>

#ifndef __unused
#define __unused __attribute__((unused))
#endif
#ifndef __pure2
#define __pure2 __attribute__((pure))
#endif
#ifndef __DECONST
#define __DECONST(type, var) ((type)(uintptr_t)(const void *)(var))
#endif
#ifndef nitems
#define nitems(array) (sizeof(array) / sizeof((array)[0]))
#endif
#ifndef MAXLOGNAME
#define MAXLOGNAME 64
#endif
#ifndef CPUSTATES
#define CPUSTATES 5
#endif
#ifndef PRIO_MIN
#define PRIO_MIN (-20)
#endif
#ifndef PRIO_MAX
#define PRIO_MAX 20
#endif
#ifndef TAB3
#define TAB3 0
#endif

/* Avoid newlib's non-POSIX itoa() prototype without modifying upstream. */
#define itoa armos_top_itoa

extern const char *const sys_signame[];
extern const int sys_nsig;

long long strtonum(const char *, long long, long long, const char **);
int jail_getid(const char *);
int sigblock(int);
int sigsetmask(int);

#endif
