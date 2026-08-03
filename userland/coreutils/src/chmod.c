/*
 * ArmOS
 * Copyright (c) 2026 Mohamed Ennassiri
 *
 * Licensed under the Apache License, Version 2.0.
 * See LICENSE for details.
 *
 * File: userland/coreutils/src/chmod.c
 * Layer: Userland / core utility
 * Description: POSIX-like command-line utility for ArmOS.
 */

#include <errno.h>
#include <dirent.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define PATH_BUF 512

static int parse_octal_mode(const char *s, mode_t *out)
{
    unsigned value = 0;
    int digits = 0;

    if (!s || !*s || !out)
        return -1;

    while (*s) {
        if (*s < '0' || *s > '7')
            return -1;
        value = (value << 3) + (unsigned)(*s - '0');
        if (value > 07777)
            return -1;
        digits++;
        s++;
    }

    if (digits < 1 || digits > 4)
        return -1;

    *out = (mode_t)value;
    return 0;
}

static int parse_symbolic_mode(const char *s, mode_t old_mode, mode_t *out)
{
    unsigned who = 0;
    unsigned perms = 0;
    char op;

    if (!s || !*s || !out)
        return -1;

    while (*s == 'u' || *s == 'g' || *s == 'o' || *s == 'a') {
        if (*s == 'u' || *s == 'a') who |= 0700;
        if (*s == 'g' || *s == 'a') who |= 0070;
        if (*s == 'o' || *s == 'a') who |= 0007;
        s++;
    }

    if (who == 0)
        who = 0777;

    op = *s++;
    if (op != '+' && op != '-' && op != '=')
        return -1;

    while (*s) {
        if (*s == 'r') perms |= 0444;
        else if (*s == 'w') perms |= 0222;
        else if (*s == 'x') perms |= 0111;
        else return -1;
        s++;
    }

    perms &= who;

    if (op == '+')
        *out = old_mode | perms;
    else if (op == '-')
        *out = old_mode & ~perms;
    else
        *out = (old_mode & ~who) | perms;

    *out &= 07777;
    return 0;
}

static int parse_mode_for_path(const char *spec, const char *path, mode_t *out)
{
    struct stat st;

    if (parse_octal_mode(spec, out) == 0)
        return 0;

    if (stat(path, &st) < 0)
        return -1;

    return parse_symbolic_mode(spec, st.st_mode & 07777, out);
}

static void join_path(char* out, size_t out_size, const char* dir, const char* name)
{
    size_t len = strlen(dir);
    snprintf(out, out_size, "%s%s%s", dir, (len > 0 && dir[len - 1] == '/') ? "" : "/", name);
}

static int chmod_path(const char* path, const char* spec, int recursive)
{
    struct stat st;
    mode_t mode;
    int status = 0;

    if (parse_mode_for_path(spec, path, &mode) < 0) {
        printf("chmod: invalid mode '%s' or cannot stat '%s'\n", spec, path);
        return 1;
    }

    errno = 0;
    if (chmod(path, mode) < 0) {
        printf("chmod: cannot change mode of '%s' (errno=%d)\n", path, errno);
        status = 1;
    }

    if (!recursive || lstat(path, &st) < 0 || !S_ISDIR(st.st_mode) || S_ISLNK(st.st_mode))
        return status;

    int fd = open(path, O_RDONLY | O_DIRECTORY, 0);
    if (fd < 0)
        return 1;

    char buf[1024];
    int n;
    while ((n = getdents(fd, buf, sizeof(buf))) > 0) {
        int pos = 0;
        while (pos < n) {
            struct linux_dirent* entry = (struct linux_dirent*)(buf + pos);
            char child[PATH_BUF];

            if (entry->d_reclen == 0)
                break;
            if (strcmp(entry->d_name, ".") != 0 && strcmp(entry->d_name, "..") != 0) {
                join_path(child, sizeof(child), path, entry->d_name);
                if (chmod_path(child, spec, recursive) != 0)
                    status = 1;
            }
            pos += entry->d_reclen;
        }
    }

    if (n < 0)
        status = 1;
    close(fd);
    return status;
}

int main(int argc, char **argv)
{
    int status = 0;
    int recursive = 0;
    int mode_index = 1;

    if (argc > 1 && strcmp(argv[1], "-R") == 0) {
        recursive = 1;
        mode_index = 2;
    }

    if (argc < mode_index + 2) {
        printf("Usage: chmod [-R] MODE FILE...\n");
        return 1;
    }

    for (int i = mode_index + 1; i < argc; i++) {
        if (chmod_path(argv[i], argv[mode_index], recursive) != 0)
            status = 1;
    }

    return status;
}
