/*
 * ArmOS
 * Copyright (c) 2026 Mohamed Ennassiri
 *
 * Licensed under the Apache License, Version 2.0.
 * See LICENSE for details.
 *
 * File: userland/opt/freebsd-sh/compat/armos_sh_compat.c
 * Layer: Userland / FreeBSD sh portability
 *
 * Responsibilities:
 * - Supply small BSD libc interfaces required by FreeBSD sh.
 * - Express these interfaces through existing POSIX ArmOS services.
 */

#include "armos_sh_compat.h"

#undef vfork

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

const char *const sys_signame[] = {
    "ZERO", "HUP", "INT", "QUIT", "ILL", "TRAP", "ABRT", "EMT",
    "FPE", "KILL", "BUS", "SEGV", "SYS", "PIPE", "ALRM", "TERM",
    "URG", "STOP", "TSTP", "CONT", "CHLD", "TTIN", "TTOU", "IO",
    "WINCH", "USR1", "USR2", "RTMIN", "RT28", "RT29", "RT30", "RTMAX"
};
const int sys_nsig = (int)(sizeof(sys_signame) / sizeof(sys_signame[0]));

static void vmessage(int error_number, const char *format, va_list arguments)
{
    if (format && *format) {
        vfprintf(stderr, format, arguments);
        if (error_number)
            fputs(": ", stderr);
    }
    if (error_number)
        fputs(strerror(error_number), stderr);
    fputc('\n', stderr);
}

void vwarn(const char *format, va_list arguments)
{
    vmessage(errno, format, arguments);
}

void vwarnx(const char *format, va_list arguments)
{
    vmessage(0, format, arguments);
}

void warn(const char *format, ...)
{
    va_list arguments;
    va_start(arguments, format);
    vwarn(format, arguments);
    va_end(arguments);
}

void warnx(const char *format, ...)
{
    va_list arguments;
    va_start(arguments, format);
    vwarnx(format, arguments);
    va_end(arguments);
}

void verr(int status, const char *format, va_list arguments)
{
    vwarn(format, arguments);
    exit(status);
}

void verrx(int status, const char *format, va_list arguments)
{
    vwarnx(format, arguments);
    exit(status);
}

void err(int status, const char *format, ...)
{
    va_list arguments;
    va_start(arguments, format);
    verr(status, format, arguments);
}

void errx(int status, const char *format, ...)
{
    va_list arguments;
    va_start(arguments, format);
    verrx(status, format, arguments);
}

int eaccess(const char *path, int mode)
{
    return access(path, mode);
}

char *strchrnul(const char *string, int character)
{
    while (*string && *string != (char)character)
        string++;
    return (char *)(uintptr_t)string;
}

pid_t wait3(int *status, int options, struct rusage *usage)
{
    return wait4(-1, status, options, usage);
}

int tcsetsid(int fd, pid_t session)
{
    if (session != getsid(0)) {
        errno = EINVAL;
        return -1;
    }
    return ioctl(fd, TIOCSCTTY, 0);
}

void *setmode(const char *mode)
{
    if (!mode) {
        errno = EINVAL;
        return NULL;
    }
    return strdup(mode);
}

mode_t getmode(const void *cookie, mode_t mode)
{
    const char *spec = cookie;
    char *end;
    unsigned long parsed;

    if (!spec)
        return mode;
    errno = 0;
    parsed = strtoul(spec, &end, 8);
    if (errno == 0 && *spec && *end == '\0')
        return (mode_t)(parsed & 07777u);
    return mode;
}

int sig2str(int signal_number, char *name)
{
    if (!name || signal_number <= 0 || signal_number >= sys_nsig)
        return -1;
    strcpy(name, sys_signame[signal_number]);
    return 0;
}

int str2sig(const char *name, int *signal_number)
{
    int index;
    char *end;
    long numeric;

    if (!name || !signal_number)
        return -1;
    if (strncasecmp(name, "SIG", 3) == 0)
        name += 3;
    for (index = 1; index < sys_nsig; index++) {
        if (strcasecmp(name, sys_signame[index]) == 0) {
            *signal_number = index;
            return 0;
        }
    }
    numeric = strtol(name, &end, 10);
    if (*name && *end == '\0' && numeric > 0 && numeric < sys_nsig) {
        *signal_number = (int)numeric;
        return 0;
    }
    return -1;
}

char *getlogin(void)
{
    static char root[] = "root";
    static char user[] = "user";

    return getuid() == 0 ? root : user;
}

ssize_t getline(char **line, size_t *capacity, FILE *stream)
{
    size_t length = 0;
    int character;

    if (!line || !capacity || !stream) {
        errno = EINVAL;
        return -1;
    }
    if (!*line || *capacity < 2) {
        char *allocated = realloc(*line, 128);
        if (!allocated)
            return -1;
        *line = allocated;
        *capacity = 128;
    }
    while ((character = fgetc(stream)) != EOF) {
        if (length + 2 > *capacity) {
            size_t grown_capacity = *capacity * 2;
            char *grown = realloc(*line, grown_capacity);
            if (!grown)
                return -1;
            *line = grown;
            *capacity = grown_capacity;
        }
        (*line)[length++] = (char)character;
        if (character == '\n')
            break;
    }
    if (length == 0 && character == EOF)
        return -1;
    (*line)[length] = '\0';
    return (ssize_t)length;
}

int strvis(char *destination, const char *source, int flags)
{
    char *output = destination;

    (void)flags;
    while (*source) {
        unsigned char value = (unsigned char)*source++;

        if (value == '\\') {
            *output++ = '\\';
            *output++ = '\\';
        } else if (value == '\n') {
            *output++ = '\\';
            *output++ = 'n';
        } else if (value == '\r') {
            *output++ = '\\';
            *output++ = 'r';
        } else if (value == '\t') {
            *output++ = '\\';
            *output++ = 't';
        } else if (isprint(value)) {
            *output++ = (char)value;
        } else {
            *output++ = '\\';
            *output++ = (char)('0' + ((value >> 6) & 7));
            *output++ = (char)('0' + ((value >> 3) & 7));
            *output++ = (char)('0' + (value & 7));
        }
    }
    *output = '\0';
    return (int)(output - destination);
}

int strunvis(char *destination, const char *source)
{
    char *output = destination;

    while (*source) {
        if (*source++ != '\\') {
            *output++ = source[-1];
            continue;
        }
        if (*source == '\\') {
            *output++ = '\\';
            source++;
        } else if (*source == 'n') {
            *output++ = '\n';
            source++;
        } else if (*source == 'r') {
            *output++ = '\r';
            source++;
        } else if (*source == 't') {
            *output++ = '\t';
            source++;
        } else if (source[0] >= '0' && source[0] <= '7' &&
                   source[1] >= '0' && source[1] <= '7' &&
                   source[2] >= '0' && source[2] <= '7') {
            *output++ = (char)(((source[0] - '0') << 6) |
                               ((source[1] - '0') << 3) |
                               (source[2] - '0'));
            source += 3;
        } else if (*source) {
            *output++ = *source++;
        }
    }
    *output = '\0';
    return (int)(output - destination);
}
