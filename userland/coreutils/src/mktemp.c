/*
 * ArmOS
 * Copyright (c) 2026 Mohamed Ennassiri
 *
 * Licensed under the Apache License, Version 2.0.
 * See LICENSE for details.
 *
 * File: userland/coreutils/src/mktemp.c
 * Layer: Userland / POSIX core utility
 * Description: Safely create a unique temporary file or directory.
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static char *create_directory(char *template)
{
    static const char alphabet[] =
        "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";
    struct timespec now;
    size_t length = strlen(template);
    size_t first_x = length;
    unsigned long state;

    while (first_x > 0 && template[first_x - 1] == 'X')
        first_x--;
    if (length - first_x < 6) {
        errno = EINVAL;
        return NULL;
    }
    clock_gettime(CLOCK_MONOTONIC, &now);
    state = (unsigned long)now.tv_nsec ^
            ((unsigned long)getpid() << 16) ^ (unsigned long)now.tv_sec;
    for (unsigned attempt = 0; attempt < 256; attempt++) {
        unsigned long value = state + attempt * 1103515245u;

        for (size_t index = first_x; index < length; index++) {
            value = value * 1103515245u + 12345u;
            template[index] = alphabet[value % (sizeof(alphabet) - 1u)];
        }
        if (mkdir(template, 0700) == 0)
            return template;
        if (errno != EEXIST)
            return NULL;
    }
    errno = EEXIST;
    return NULL;
}

int main(int argc, char **argv)
{
    int directory = 0;
    const char *argument = NULL;
    char default_template[] = "/tmp/tmp.XXXXXX";
    char *template;
    int fd;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-d") == 0)
            directory = 1;
        else if (argv[i][0] == '-') {
            fprintf(stderr, "usage: mktemp [-d] [template]\n");
            return 1;
        } else if (argument) {
            fprintf(stderr, "mktemp: too many templates\n");
            return 1;
        } else {
            argument = argv[i];
        }
    }
    template = strdup(argument ? argument : default_template);
    if (!template) {
        fprintf(stderr, "mktemp: out of memory\n");
        return 1;
    }
    if (directory) {
        if (!create_directory(template)) {
            fprintf(stderr, "mktemp: %s: %s\n", template, strerror(errno));
            free(template);
            return 1;
        }
    } else {
        fd = mkstemp(template);
        if (fd < 0) {
            fprintf(stderr, "mktemp: %s: %s\n", template, strerror(errno));
            free(template);
            return 1;
        }
        close(fd);
    }
    puts(template);
    free(template);
    return 0;
}
