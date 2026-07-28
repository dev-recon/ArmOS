/*
 * ArmOS
 * Copyright (c) 2026 Mohamed Ennassiri
 *
 * Licensed under the Apache License, Version 2.0.
 * See LICENSE for details.
 *
 * File: userland/include/syslog.h
 * Layer: Userland / POSIX compatibility API
 *
 * Responsibilities:
 * - Expose the BSD/POSIX syslog priority and facility constants.
 * - Declare the logging API required by fcft and Foot.
 */

#ifndef ARMOS_SYSLOG_H
#define ARMOS_SYSLOG_H

#include <stdarg.h>

enum {
    LOG_EMERG = 0,
    LOG_ALERT = 1,
    LOG_CRIT = 2,
    LOG_ERR = 3,
    LOG_WARNING = 4,
    LOG_NOTICE = 5,
    LOG_INFO = 6,
    LOG_DEBUG = 7
};

#define LOG_PRIMASK     0x07
#define LOG_PRI(value)  ((value) & LOG_PRIMASK)
#define LOG_MASK(value) (1 << (value))
#define LOG_UPTO(value) ((1 << ((value) + 1)) - 1)

#define LOG_KERN        (0 << 3)
#define LOG_USER        (1 << 3)
#define LOG_MAIL        (2 << 3)
#define LOG_DAEMON      (3 << 3)
#define LOG_AUTH        (4 << 3)
#define LOG_SYSLOG      (5 << 3)
#define LOG_LPR         (6 << 3)
#define LOG_NEWS        (7 << 3)
#define LOG_UUCP        (8 << 3)
#define LOG_CRON        (9 << 3)
#define LOG_AUTHPRIV    (10 << 3)
#define LOG_FTP         (11 << 3)
#define LOG_LOCAL0      (16 << 3)
#define LOG_LOCAL1      (17 << 3)
#define LOG_LOCAL2      (18 << 3)
#define LOG_LOCAL3      (19 << 3)
#define LOG_LOCAL4      (20 << 3)
#define LOG_LOCAL5      (21 << 3)
#define LOG_LOCAL6      (22 << 3)
#define LOG_LOCAL7      (23 << 3)

#define LOG_FACMASK     0x03f8
#define LOG_FAC(value)  (((value) & LOG_FACMASK) >> 3)
#define LOG_MAKEPRI(facility, priority) ((facility) | (priority))

#define LOG_PID         0x01
#define LOG_CONS        0x02
#define LOG_ODELAY      0x04
#define LOG_NDELAY      0x08
#define LOG_NOWAIT      0x10
#define LOG_PERROR      0x20

#ifdef __cplusplus
extern "C" {
#endif

void openlog(const char *ident, int option, int facility);
void closelog(void);
int setlogmask(int mask);
void syslog(int priority, const char *format, ...);
void vsyslog(int priority, const char *format, va_list arguments);

#ifdef __cplusplus
}
#endif

#endif /* ARMOS_SYSLOG_H */
