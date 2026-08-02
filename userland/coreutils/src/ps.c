/*
 * ArmOS
 * Copyright (c) 2026 Mohamed Ennassiri
 *
 * Licensed under the Apache License, Version 2.0.
 * See LICENSE for details.
 *
 * File: userland/coreutils/src/ps.c
 * Layer: Userland / core utility
 * Description: POSIX-like command-line utility for ArmOS.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>

#define PROC_BUF_SIZE 4096
#define PROC_HZ 1000

static int is_digit(char c)
{
    return c >= '0' && c <= '9';
}

static int read_file(const char *path, char *buf, int size)
{
    int fd;
    int n;

    if (!buf || size <= 1)
        return -1;

    fd = open(path, O_RDONLY, 0);
    if (fd < 0)
        return -1;

    n = read(fd, buf, size - 1);
    close(fd);

    if (n < 0)
        return -1;

    buf[n] = '\0';
    return n;
}

static const char *parse_int(const char *p, int *out)
{
    int sign = 1;
    int value = 0;

    while (*p == ' ' || *p == '\t')
        p++;

    if (*p == '-') {
        sign = -1;
        p++;
    }

    if (!is_digit(*p))
        return NULL;

    while (is_digit(*p)) {
        value = value * 10 + (*p - '0');
        p++;
    }

    *out = value * sign;
    return p;
}

static int parse_proc_stat(const char *buf, int *pid, int *tty,
                           int *runtime_ticks, char *name, int name_size)
{
    const char *p;
    const char *start;
    const char *end;
    int dummy;
    int len;

    p = parse_int(buf, pid);
    if (!p)
        return -1;

    while (*p == ' ')
        p++;
    if (*p != '(')
        return -1;

    start = ++p;
    end = start;
    while (*end && *end != ')')
        end++;
    if (*end != ')')
        return -1;

    len = (int)(end - start);
    if (len >= name_size)
        len = name_size - 1;
    memcpy(name, start, (size_t)len);
    name[len] = '\0';

    p = end + 1;
    while (*p == ' ')
        p++;
    if (*p)
        p++; /* state */

    p = parse_int(p, &dummy); /* ppid */
    if (!p) return -1;
    p = parse_int(p, &dummy); /* pgid */
    if (!p) return -1;
    p = parse_int(p, &dummy); /* sid */
    if (!p) return -1;
    p = parse_int(p, tty);
    if (!p) return -1;

    *runtime_ticks = 0;
    p = parse_int(p, &dummy); /* page_faults */
    if (!p) return 0;
    p = parse_int(p, &dummy); /* cow_faults */
    if (!p) return 0;
    p = parse_int(p, &dummy); /* stack_faults */
    if (!p) return 0;
    p = parse_int(p, &dummy); /* ctx switches */
    if (!p) return 0;
    p = parse_int(p, runtime_ticks); /* runtime ticks */

    return 0;
}

static void format_time(int runtime_ticks, char *buf, int size)
{
    unsigned int ticks;
    unsigned int centis;
    unsigned int minutes;
    unsigned int seconds;
    unsigned int hundredths;

    if (!buf || size <= 0)
        return;

    if (runtime_ticks < 0)
        runtime_ticks = 0;

    ticks = (unsigned int)runtime_ticks;
    centis = (ticks * 100u) / PROC_HZ;
    minutes = centis / 6000u;
    seconds = (centis / 100u) % 60u;
    hundredths = centis % 100u;

    snprintf(buf, (size_t)size, "%u:%02u.%02u", minutes, seconds, hundredths);
}

static void print_tty(int tty)
{
    if (tty >= 0)
        printf("tty%d", tty);
    else
        printf("?");
}

int main(int argc, char **argv)
{
    char *dirbuf;
    char statbuf[512];
    int fd;
    int n;
    int selected_pid = -1;
    int comm_only = 0;
    int comm_header = 0;

    for (int arg = 1; arg < argc; arg++) {
        if (strcmp(argv[arg], "-p") == 0 && arg + 1 < argc) {
            char *end = NULL;
            long value = strtol(argv[++arg], &end, 10);

            if (!end || *end || value <= 0 || value > 0x7fffffffL) {
                fprintf(stderr, "ps: invalid process id\n");
                return 1;
            }
            selected_pid = (int)value;
        } else if (strcmp(argv[arg], "-o") == 0 && arg + 1 < argc) {
            const char *format = argv[++arg];

            if (strcmp(format, "comm=") == 0) {
                comm_only = 1;
                comm_header = 0;
            } else if (strcmp(format, "comm") == 0) {
                comm_only = 1;
                comm_header = 1;
            } else {
                fprintf(stderr, "ps: unsupported format: %s\n", format);
                return 1;
            }
        } else {
            fprintf(stderr, "usage: ps [-p pid] [-o comm[=]]\n");
            return 1;
        }
    }

    dirbuf = malloc(PROC_BUF_SIZE);
    if (!dirbuf) {
        printf("ps: out of memory\n");
        return 1;
    }

    fd = open("/proc", O_RDONLY | O_DIRECTORY, 0);
    if (fd < 0) {
        printf("ps: cannot open /proc\n");
        free(dirbuf);
        return 1;
    }

    if (comm_only) {
        if (comm_header)
            printf("COMMAND\n");
    } else {
        printf("%5s %-8s %8s %s\n", "PID", "TTY", "TIME", "CMD");
    }

    while ((n = getdents(fd, dirbuf, PROC_BUF_SIZE)) > 0) {
        char *ptr = dirbuf;

        while (ptr < dirbuf + n) {
            struct linux_dirent *e = (struct linux_dirent *)ptr;
            char path[64];
            char name[64];
            char timebuf[16];
            int pid;
            int tty;
            int runtime_ticks;

            if (e->d_reclen == 0)
                break;

            if (e->d_ino != 0 && is_digit(e->d_name[0])) {
                sprintf(path, "/proc/%s/stat", e->d_name);
                if (read_file(path, statbuf, sizeof(statbuf)) >= 0 &&
                    parse_proc_stat(statbuf, &pid, &tty, &runtime_ticks, name, sizeof(name)) == 0) {
                    if (selected_pid > 0 && pid != selected_pid) {
                        ptr += e->d_reclen;
                        continue;
                    }
                    if (comm_only) {
                        printf("%s\n", name);
                        ptr += e->d_reclen;
                        continue;
                    }
                    format_time(runtime_ticks, timebuf, sizeof(timebuf));
                    printf("%5d ", pid);
                    print_tty(tty);
                    printf("%*s %8s %s\n", tty >= 0 ? 4 : 7, "", timebuf, name);
                }
            }

            ptr += e->d_reclen;
        }
    }

    close(fd);
    free(dirbuf);
    return n < 0 ? 1 : 0;
}
