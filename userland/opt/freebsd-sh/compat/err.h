#ifndef ARMOS_FREEBSD_SH_ERR_H
#define ARMOS_FREEBSD_SH_ERR_H

#include <stdarg.h>

void warn(const char *, ...);
void warnx(const char *, ...);
void err(int, const char *, ...) __attribute__((noreturn));
void errx(int, const char *, ...) __attribute__((noreturn));
void vwarn(const char *, va_list);
void vwarnx(const char *, va_list);
void verr(int, const char *, va_list) __attribute__((noreturn));
void verrx(int, const char *, va_list) __attribute__((noreturn));

#endif
