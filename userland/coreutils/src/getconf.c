/*
 * ArmOS
 * Copyright (c) 2026 Mohamed Ennassiri
 *
 * Licensed under the Apache License, Version 2.0.
 * See LICENSE for details.
 *
 * File: userland/coreutils/src/getconf.c
 * Layer: Userland / POSIX core utility
 * Description: Query the limits implemented by the ArmOS POSIX layer.
 */

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

int main(int argc, char **argv)
{
    long value;

    if (argc == 2 && strcmp(argv[1], "ARG_MAX") == 0)
        value = sysconf(_SC_ARG_MAX);
    else if (argc == 3 && strcmp(argv[1], "PATH_MAX") == 0)
        value = pathconf(argv[2], _PC_PATH_MAX);
    else if (argc == 3 && strcmp(argv[1], "NAME_MAX") == 0)
        value = pathconf(argv[2], _PC_NAME_MAX);
    else {
        fprintf(stderr, "usage: getconf ARG_MAX | getconf {PATH_MAX|NAME_MAX} path\n");
        return 1;
    }
    if (value < 0) {
        fprintf(stderr, "getconf: %s\n", strerror(errno));
        return 1;
    }
    printf("%ld\n", value);
    return 0;
}
