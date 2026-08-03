/* Generic libc compatibility used by the imported FreeBSD top frontend. */
#include "armos_top_compat.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

const char *const sys_signame[] = {
    "Signal 0", "HUP", "INT", "QUIT", "ILL", "TRAP", "ABRT", "BUS",
    "FPE", "KILL", "USR1", "SEGV", "USR2", "PIPE", "ALRM", "TERM",
    "STKFLT", "CHLD", "CONT", "STOP", "TSTP", "TTIN", "TTOU", "URG",
    "XCPU", "XFSZ", "VTALRM", "PROF", "WINCH", "IO", "PWR", "SYS"
};
const int sys_nsig = (int)nitems(sys_signame);

static void
vreport(const char *prefix, const char *format, va_list ap, int with_errno)
{
    int saved = errno;

    if (prefix != NULL)
        fputs(prefix, stderr);
    if (format != NULL)
        vfprintf(stderr, format, ap);
    if (with_errno) {
        if (format != NULL)
            fputs(": ", stderr);
        fputs(strerror(saved), stderr);
    }
    fputc('\n', stderr);
}

void warn(const char *format, ...)
{
    va_list ap;
    va_start(ap, format);
    vreport("top: ", format, ap, 1);
    va_end(ap);
}

void warnx(const char *format, ...)
{
    va_list ap;
    va_start(ap, format);
    vreport("top: ", format, ap, 0);
    va_end(ap);
}

void err(int status, const char *format, ...)
{
    va_list ap;
    va_start(ap, format);
    vreport("top: ", format, ap, 1);
    va_end(ap);
    exit(status);
}

void errx(int status, const char *format, ...)
{
    va_list ap;
    va_start(ap, format);
    vreport("top: ", format, ap, 0);
    va_end(ap);
    exit(status);
}

long long
strtonum(const char *text, long long minimum, long long maximum,
    const char **error)
{
    char *end;
    long long value;

    errno = 0;
    value = strtoll(text, &end, 10);
    if (text == end || *end != '\0' || errno == ERANGE) {
        if (error != NULL)
            *error = "invalid";
        errno = EINVAL;
        return 0;
    }
    if (value < minimum) {
        if (error != NULL)
            *error = "too small";
        errno = ERANGE;
        return 0;
    }
    if (value > maximum) {
        if (error != NULL)
            *error = "too large";
        errno = ERANGE;
        return 0;
    }
    if (error != NULL)
        *error = NULL;
    return value;
}

int jail_getid(const char *name __unused)
{
    errno = ENOSYS;
    return -1;
}

static void
legacy_mask_to_set(int mask, sigset_t *set)
{
    int signal_number;
    (void)sigemptyset(set);
    for (signal_number = 1; signal_number < sys_nsig; signal_number++)
        if ((mask & (1U << (signal_number - 1))) != 0)
            (void)sigaddset(set, signal_number);
}

static int
set_to_legacy_mask(const sigset_t *set)
{
    int signal_number;
    int mask = 0;
    for (signal_number = 1; signal_number < sys_nsig; signal_number++)
        if (sigismember(set, signal_number) == 1)
            mask |= (int)(1U << (signal_number - 1));
    return mask;
}

int
sigblock(int mask)
{
    sigset_t set;
    sigset_t old;
    legacy_mask_to_set(mask, &set);
    if (sigprocmask(SIG_BLOCK, &set, &old) != 0)
        return -1;
    return set_to_legacy_mask(&old);
}

int
sigsetmask(int mask)
{
    sigset_t set;
    sigset_t old;
    legacy_mask_to_set(mask, &set);
    if (sigprocmask(SIG_SETMASK, &set, &old) != 0)
        return -1;
    return set_to_legacy_mask(&old);
}
