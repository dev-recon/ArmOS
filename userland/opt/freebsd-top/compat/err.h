#ifndef ARMOS_TOP_ERR_H
#define ARMOS_TOP_ERR_H

void err(int, const char *, ...) __attribute__((noreturn, format(printf, 2, 3)));
void errx(int, const char *, ...) __attribute__((noreturn, format(printf, 2, 3)));
void warn(const char *, ...) __attribute__((format(printf, 1, 2)));
void warnx(const char *, ...) __attribute__((format(printf, 1, 2)));

#endif
