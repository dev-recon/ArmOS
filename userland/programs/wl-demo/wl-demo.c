/*
 * ArmOS
 * Copyright (c) 2026 Mohamed Ennassiri
 *
 * Licensed under the Apache License, Version 2.0.
 * See LICENSE for details.
 *
 * File: userland/programs/wl-demo/wl-demo.c
 * Layer: Userland / graphical applications
 *
 * Responsibilities:
 * - Start the interactive Wayland protocol demonstration.
 * - Provide a stable user-facing command separate from validation defaults.
 * - Forward an optional compositor socket path.
 *
 * Notes:
 * - Text lives only in the client's SHM buffer and is never persisted.
 * - The test client remains the temporary wire implementation until libwayland.
 */

#include <stdio.h>
#include <unistd.h>

int main(int argc, char **argv)
{
    char *arguments[4];

    arguments[0] = "wayland-test";
    arguments[1] = "--demo";
    arguments[2] = argc > 1 ? argv[1] : NULL;
    arguments[3] = NULL;
    execv("/usr/bin/wayland-test", arguments);
    perror("wl-demo");
    return 1;
}
