/*
 * ArmOS
 * Copyright (c) 2026 Mohamed Ennassiri
 *
 * Licensed under the Apache License, Version 2.0.
 * See LICENSE for details.
 *
 * File: userland/coreutils/src/mkfifo.c
 * Layer: Userland / POSIX core utility
 * Description: Create named pipes through the common ArmOS VFS contract.
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

int main(int argc, char **argv)
{
    mode_t mode = 0666;
    int first = 1;
    int status = 0;

    if (argc > 2 && strcmp(argv[1], "-m") == 0) {
        char *end;
        unsigned long parsed = strtoul(argv[2], &end, 8);

        if (!*argv[2] || *end || parsed > 07777) {
            fprintf(stderr, "mkfifo: invalid mode: %s\n", argv[2]);
            return 1;
        }
        mode = (mode_t)parsed;
        first = 3;
    }
    if (first >= argc) {
        fprintf(stderr, "usage: mkfifo [-m mode] file ...\n");
        return 1;
    }
    for (int i = first; i < argc; i++) {
        if (mkfifo(argv[i], mode) < 0) {
            fprintf(stderr, "mkfifo: %s: %s\n", argv[i], strerror(errno));
            status = 1;
        }
    }
    return status;
}
