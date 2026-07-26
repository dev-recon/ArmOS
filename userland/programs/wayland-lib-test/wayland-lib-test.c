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
 * - Validate registry discovery and display roundtrips against armos-wlcomp.
 *
 * Notes:
 * - The registry mode expects a compositor on the conventional display.
 */

#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>
#include <wayland-client.h>
#include <wayland-server.h>

struct registry_state {
    unsigned int globals;
    unsigned int compositor;
    unsigned int shm;
    unsigned int seat;
    unsigned int shell;
};

static void registry_global(void *data, struct wl_registry *registry,
                            uint32_t name, const char *interface,
                            uint32_t version)
{
    struct registry_state *state = data;

    (void)registry;
    (void)name;
    (void)version;
    state->globals++;
    state->compositor += strcmp(interface, "wl_compositor") == 0;
    state->shm += strcmp(interface, "wl_shm") == 0;
    state->seat += strcmp(interface, "wl_seat") == 0;
    state->shell += strcmp(interface, "xdg_wm_base") == 0;
}

static void registry_global_remove(void *data, struct wl_registry *registry,
                                   uint32_t name)
{
    (void)data;
    (void)registry;
    (void)name;
}

static int test_registry(void)
{
    static const struct wl_registry_listener listener = {
        registry_global,
        registry_global_remove
    };
    struct registry_state state = { 0 };
    struct wl_display *display;
    struct wl_registry *registry;
    int valid;

    display = wl_display_connect(NULL);
    if (!display) {
        perror("wayland-lib-test: connect");
        return 1;
    }
    registry = wl_display_get_registry(display);
    if (!registry ||
        wl_registry_add_listener(registry, &listener, &state) < 0 ||
        wl_display_roundtrip(display) < 0) {
        perror("wayland-lib-test: registry");
        wl_registry_destroy(registry);
        wl_display_disconnect(display);
        return 1;
    }
    valid = state.globals == 4u && state.compositor == 1u &&
        state.shm == 1u && state.seat == 1u && state.shell == 1u;
    wl_registry_destroy(registry);
    wl_display_disconnect(display);
    if (!valid) {
        fprintf(stderr, "wayland-lib-test: incomplete registry (%u)\n",
                state.globals);
        return 1;
    }
    printf("wayland-lib-test: registry roundtrip passed\n");
    return 0;
}

static int test_transport(void)
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

int main(int argc, char **argv)
{
    if (argc == 2 && strcmp(argv[1], "--registry") == 0)
        return test_registry();
    if (argc != 1) {
        fprintf(stderr, "usage: wayland-lib-test [--registry]\n");
        return 2;
    }
    return test_transport();
}
