/*
 * ArmOS
 * Copyright (c) 2026 Mohamed Ennassiri
 *
 * Licensed under the Apache License, Version 2.0.
 * See LICENSE for details.
 *
 * File: userland/coreutils/src/keymap.c
 * Layer: Userland / system utilities
 *
 * Responsibilities:
 * - Report the active keyboard layout for the common input seat.
 * - Change the layout at runtime through the stable TTY ioctl contract.
 *
 * Notes:
 * - Valid layouts are us, us-mac, fr, fr-mac, and fr-legacy.
 * - The compositor receives the change through the common input event stream.
 */

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/input.h>
#include <sys/ioctl.h>
#include <unistd.h>

struct layout_name {
    const char *name;
    uint32_t value;
};

static const struct layout_name layouts[] = {
    {"us", ARMOS_KEYBOARD_LAYOUT_US},
    {"us-mac", ARMOS_KEYBOARD_LAYOUT_US_MAC},
    {"fr", ARMOS_KEYBOARD_LAYOUT_FR},
    {"fr-mac", ARMOS_KEYBOARD_LAYOUT_FR_MAC},
    {"fr-legacy", ARMOS_KEYBOARD_LAYOUT_FR_LEGACY},
};

static const char *layout_name(uint32_t value)
{
    for (size_t index = 0u; index < sizeof(layouts) / sizeof(layouts[0]);
         index++) {
        if (layouts[index].value == value)
            return layouts[index].name;
    }
    return "unknown";
}

static int terminal_ioctl(unsigned long request, uint32_t *layout)
{
    const int descriptors[] = {STDIN_FILENO, STDOUT_FILENO, STDERR_FILENO};

    for (size_t index = 0u;
         index < sizeof(descriptors) / sizeof(descriptors[0]); index++) {
        if (ioctl(descriptors[index], request, layout) == 0)
            return 0;
        if (errno != ENOTTY && errno != EBADF)
            return -1;
    }
    errno = ENOTTY;
    return -1;
}

int main(int argc, char **argv)
{
    uint32_t layout;

    if (argc > 2) {
        fprintf(stderr,
                "usage: keymap [us|us-mac|fr|fr-mac|fr-legacy]\n");
        return 2;
    }
    if (argc == 1) {
        if (terminal_ioctl(ARMOS_TIOCGKEYMAP, &layout) < 0) {
            perror("keymap");
            return 1;
        }
        puts(layout_name(layout));
        return 0;
    }
    for (size_t index = 0u; index < sizeof(layouts) / sizeof(layouts[0]);
         index++) {
        if (strcmp(argv[1], layouts[index].name) == 0) {
            layout = layouts[index].value;
            if (terminal_ioctl(ARMOS_TIOCSKEYMAP, &layout) < 0) {
                perror("keymap");
                return 1;
            }
            printf("keyboard layout: %s\n", layouts[index].name);
            return 0;
        }
    }
    fprintf(stderr, "keymap: unknown layout '%s'\n", argv[1]);
    return 2;
}
