/*
 * ArmOS
 * Copyright (c) 2026 Mohamed Ennassiri
 *
 * Licensed under the Apache License, Version 2.0.
 * See LICENSE for details.
 *
 * File: userland/coreutils/src/clear.c
 * Layer: Userland / core utility
 * Description: Clear an ANSI-compatible terminal and return the cursor home.
 */

#include <errno.h>
#include <stdio.h>
#include <unistd.h>

int main(int argc, char **argv)
{
    static const char sequence[] = "\033[H\033[2J\033[3J";
    size_t offset = 0;

    (void)argv;
    if (argc != 1) {
        fprintf(stderr, "usage: clear\n");
        return 1;
    }

    while (offset < sizeof(sequence) - 1) {
        ssize_t written = write(STDOUT_FILENO, sequence + offset,
                                sizeof(sequence) - 1 - offset);

        if (written < 0) {
            if (errno == EINTR)
                continue;
            perror("clear");
            return 1;
        }
        if (written == 0)
            return 1;
        offset += (size_t)written;
    }

    return 0;
}
