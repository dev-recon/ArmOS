/*
 * ArmOS
 * Copyright (c) 2026 Mohamed Ennassiri
 *
 * Licensed under the Apache License, Version 2.0.
 * See LICENSE for details.
 *
 * File: userland/coreutils/src/date.c
 * Layer: Userland / core utility
 * Description: POSIX-like command-line utility for ArmOS.
 */

#include <stdio.h>
#include <string.h>
#include <time.h>

static int print_format(const char *format, const struct tm *tm,
                        const struct timespec *now)
{
    char chunk[256];
    const char *cursor = format;

    while (*cursor) {
        const char *special = strchr(cursor, '%');
        size_t length;

        if (!special) {
            fputs(cursor, stdout);
            break;
        }
        if (special != cursor) {
            length = (size_t)(special - cursor);
            if (length >= sizeof(chunk))
                return -1;
            memcpy(chunk, cursor, length);
            chunk[length] = '\0';
            fputs(chunk, stdout);
        }
        if (special[1] == 's') {
            printf("%lld", (long long)now->tv_sec);
            cursor = special + 2;
            continue;
        }
        if (special[1] == 'N' ||
            (special[1] >= '1' && special[1] <= '9' && special[2] == 'N')) {
            int digits = special[1] == 'N' ? 9 : special[1] - '0';
            char nanoseconds[10];

            snprintf(nanoseconds, sizeof(nanoseconds), "%09ld", now->tv_nsec);
            fwrite(nanoseconds, 1, (size_t)digits, stdout);
            cursor = special + (special[1] == 'N' ? 2 : 3);
            continue;
        }

        length = special[1] ? 2u : 1u;
        if (length >= sizeof(chunk))
            return -1;
        memcpy(chunk, special, length);
        chunk[length] = '\0';
        if (strftime(chunk + 16, sizeof(chunk) - 16, chunk, tm) == 0)
            return -1;
        fputs(chunk + 16, stdout);
        cursor = special + length;
    }
    putchar('\n');
    return ferror(stdout) ? -1 : 0;
}

int main(int argc, char **argv)
{
    struct timespec now;
    struct tm *tm;
    char buf[64];
    const char *format = NULL;
    int utc = 0;

    if (argc == 2 && strcmp(argv[1], "-u") == 0) {
        utc = 1;
    } else if (argc == 2 && argv[1][0] == '+') {
        format = argv[1] + 1;
    } else if (argc == 3 && strcmp(argv[1], "-u") == 0 &&
               argv[2][0] == '+') {
        utc = 1;
        format = argv[2] + 1;
    } else if (argc > 1) {
        printf("date: setting date is not supported\n");
        return 1;
    }

    if (clock_gettime(CLOCK_REALTIME, &now) < 0) {
        printf("date: cannot read time\n");
        return 1;
    }
    tm = utc ? gmtime(&now.tv_sec) : localtime(&now.tv_sec);
    if (!tm) {
        printf("date: cannot read time\n");
        return 1;
    }

    if (format)
        return print_format(format, tm, &now) < 0;

    if (strftime(buf, sizeof(buf), "%a %b %d %H:%M:%S %Z %Y", tm) == 0)
        return 1;
    printf("%s\n", buf);
    return 0;
}
