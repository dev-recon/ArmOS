/*
 * ArmOS
 * Copyright (c) 2026 Mohamed Ennassiri
 *
 * Licensed under the Apache License, Version 2.0.
 * See LICENSE for details.
 *
 * File: userland/coreutils/src/env.c
 * Layer: Userland / core utility
 * Description: POSIX-like command-line utility for ArmOS.
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

extern char **environ;
static char *empty_environment[] = { NULL };

static void usage(void)
{
    fprintf(stderr, "usage: env [-i] [-u name] [name=value ...] [command [argument ...]]\n");
}

int main(int argc, char **argv)
{
    int index = 1;

    while (index < argc) {
        if (strcmp(argv[index], "-") == 0 || strcmp(argv[index], "-i") == 0) {
            environ = empty_environment;
            index++;
            continue;
        }
        if (strcmp(argv[index], "-u") == 0) {
            if (++index >= argc) {
                usage();
                return 1;
            }
            if (unsetenv(argv[index++]) < 0) {
                perror("env: unsetenv");
                return 1;
            }
            continue;
        }
        if (strcmp(argv[index], "--") == 0) {
            index++;
            break;
        }
        if (argv[index][0] == '-') {
            usage();
            return 1;
        }
        break;
    }

    while (index < argc) {
        char *equals = strchr(argv[index], '=');

        if (!equals || equals == argv[index])
            break;
        *equals = '\0';
        if (setenv(argv[index], equals + 1, 1) < 0) {
            *equals = '=';
            perror("env: setenv");
            return 1;
        }
        *equals = '=';
        index++;
    }

    if (index < argc) {
        execvp(argv[index], &argv[index]);
        fprintf(stderr, "env: %s: %s\n", argv[index], strerror(errno));
        return errno == ENOENT ? 127 : 126;
    }

    for (char **p = environ; p && *p; p++)
        printf("%s\n", *p);

    return 0;
}
