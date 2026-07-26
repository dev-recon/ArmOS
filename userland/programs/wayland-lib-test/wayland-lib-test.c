/*
 * ArmOS
 * Copyright (c) 2026 Mohamed Ennassiri
 *
 * Licensed under the Apache License, Version 2.0.
 * See LICENSE for details.
 *
 * File: userland/programs/wayland-lib-test/wayland-lib-test.c
 * Layer: Userland / Wayland library validation
 *
 * Responsibilities:
 * - Validate the native libwayland-server listening transport.
 * - Validate libwayland-client name resolution and connection ownership.
 * - Exercise both libraries across a fork boundary.
 *
 * Notes:
 * - Protocol marshaling is covered by later registry and roundtrip tests.
 */

#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>
#include <wayland-client.h>
#include <wayland-server.h>

int main(void)
{
    struct wl_display *server;
    char path[64];
    int accepted;
    int status;
    pid_t child;

    snprintf(path, sizeof(path), "/tmp/wayland-lib-%d", getpid());
    server = wl_display_create();
    if (!server || wl_display_add_socket(server, path) < 0) {
        perror("wayland-lib-test: server");
        return 1;
    }

    child = fork();
    if (child == 0) {
        struct wl_display *client = wl_display_connect(path);
        int valid = client && wl_display_get_fd(client) >= 0 &&
            wl_display_get_error(client) == 0 &&
            wl_display_flush(client) == 0;

        wl_display_disconnect(client);
        _exit(valid ? 0 : 2);
    }
    if (child < 0) {
        wl_display_destroy(server);
        return 1;
    }

    accepted = accept(wl_display_get_server_fd(server), NULL, NULL);
    if (accepted >= 0)
        close(accepted);
    waitpid(child, &status, 0);
    wl_display_destroy(server);

    if (accepted < 0 || !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        fprintf(stderr, "wayland-lib-test: transport failed\n");
        return 1;
    }
    printf("wayland-lib-test: client/server transport passed\n");
    return 0;
}
