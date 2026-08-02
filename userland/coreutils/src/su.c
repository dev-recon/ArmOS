/*
 * ArmOS
 * Copyright (c) 2026 Mohamed Ennassiri
 *
 * Licensed under the Apache License, Version 2.0.
 * See LICENSE for details.
 *
 * File: userland/coreutils/src/su.c
 * Layer: Userland / core utility
 * Description: POSIX-like command-line utility for ArmOS.
 */

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

int main(int argc, char **argv)
{
    char *const shell_argv[] = { "mash", NULL };
    char *const shell_envp[] = {
        "PATH=/sbin:/bin:/usr/bin:/opt/kilo/bin",
        "HOME=/root",
        "USER=root",
        "PWD=/root",
        "SHELL=/bin/sh",
        "ENV=/root/.shrc",
        "PS1=root# ",
        "MASH_PROTECT=1",
        NULL
    };

    if (argc > 2 || (argc == 2 && strcmp(argv[1], "root") != 0 && strcmp(argv[1], "-") != 0)) {
        printf("Usage: su [root|-]\n");
        return 1;
    }

    if (setgid(0) < 0) {
        printf("su: setgid failed (errno=%d)\n", errno);
        return 1;
    }

    if (setuid(0) < 0) {
        printf("su: setuid failed (errno=%d)\n", errno);
        return 1;
    }

    if (chdir("/root") < 0)
        chdir("/");

    execve("/sbin/mash", shell_argv, shell_envp);
    printf("su: cannot exec /sbin/mash (errno=%d)\n", errno);
    return 1;
}
