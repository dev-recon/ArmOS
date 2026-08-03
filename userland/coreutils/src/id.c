/*
 * ArmOS
 * Copyright (c) 2026 Mohamed Ennassiri
 *
 * Licensed under the Apache License, Version 2.0.
 * See LICENSE for details.
 *
 * File: userland/coreutils/src/id.c
 * Layer: Userland / POSIX core utility
 * Description: Report the process credentials exposed by ArmOS.
 */

#include <pwd.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

int main(int argc, char **argv)
{
    uid_t uid = getuid();
    gid_t gid = getgid();
    int names = 0;
    int user_only = 0;
    int group_only = 0;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-u") == 0)
            user_only = 1;
        else if (strcmp(argv[i], "-g") == 0)
            group_only = 1;
        else if (strcmp(argv[i], "-n") == 0)
            names = 1;
        else {
            fprintf(stderr, "usage: id [-u|-g] [-n]\n");
            return 1;
        }
    }
    if (user_only && group_only) {
        fprintf(stderr, "id: -u and -g are mutually exclusive\n");
        return 1;
    }
    if (user_only) {
        struct passwd *entry = getpwuid(uid);
        if (names && entry)
            puts(entry->pw_name);
        else
            printf("%u\n", (unsigned)uid);
    } else if (group_only) {
        if (names)
            puts(gid == 0 ? "root" : "user");
        else
            printf("%u\n", (unsigned)gid);
    } else {
        struct passwd *user = getpwuid(uid);
        printf("uid=%u(%s) gid=%u(%s)\n", (unsigned)uid,
               user ? user->pw_name : "?", (unsigned)gid,
               gid == 0 ? "root" : "user");
    }
    return 0;
}
