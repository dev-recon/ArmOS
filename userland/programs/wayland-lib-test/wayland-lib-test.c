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
#include <fcntl.h>
#include <string.h>
#include <sys/mman.h>
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
    uint32_t compositor_name;
    uint32_t shm_name;
    unsigned int formats;
    unsigned int releases;
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
    if (strcmp(interface, "wl_compositor") == 0) {
        state->compositor++;
        state->compositor_name = name;
    }
    if (strcmp(interface, "wl_shm") == 0) {
        state->shm++;
        state->shm_name = name;
    }
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

static void shm_format(void *data, struct wl_shm *shm, uint32_t format)
{
    struct registry_state *state = data;

    (void)shm;
    if (format == WL_SHM_FORMAT_ARGB8888 ||
        format == WL_SHM_FORMAT_XRGB8888)
        state->formats++;
}

static void buffer_release(void *data, struct wl_buffer *buffer)
{
    struct registry_state *state = data;

    (void)buffer;
    state->releases++;
}

static int test_registry(void)
{
    static const struct wl_registry_listener listener = {
        registry_global,
        registry_global_remove
    };
    static const struct wl_shm_listener shm_listener = {
        shm_format
    };
    static const struct wl_buffer_listener buffer_listener = {
        buffer_release
    };
    struct registry_state state = { 0 };
    struct wl_display *display;
    struct wl_registry *registry;
    struct wl_compositor *compositor = NULL;
    struct wl_surface *surface = NULL;
    struct wl_shm *shm = NULL;
    struct wl_shm_pool *pool = NULL;
    struct wl_buffer *buffer = NULL;
    uint32_t *pixels = MAP_FAILED;
    char shm_name[48];
    int shm_fd = -1;
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
    if (!valid)
        goto protocol_failed;
    compositor = wl_registry_bind(registry, state.compositor_name,
                                  &wl_compositor_interface, 1u);
    shm = wl_registry_bind(registry, state.shm_name, &wl_shm_interface, 1u);
    if (!compositor || !shm ||
        wl_shm_add_listener(shm, &shm_listener, &state) < 0)
        goto protocol_failed;
    surface = wl_compositor_create_surface(compositor);
    snprintf(shm_name, sizeof(shm_name), "/wayland-lib-%d", getpid());
    shm_fd = shm_open(shm_name, O_CREAT | O_EXCL | O_RDWR, 0600);
    if (shm_fd < 0 || shm_unlink(shm_name) < 0 ||
        ftruncate(shm_fd, 4096) < 0)
        goto protocol_failed;
    pixels = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);
    if (pixels == MAP_FAILED)
        goto protocol_failed;
    for (size_t index = 0; index < 16u * 16u; index++)
        pixels[index] = 0xff336699u;
    pool = wl_shm_create_pool(shm, shm_fd, 4096);
    buffer = pool ? wl_shm_pool_create_buffer(
        pool, 0, 16, 16, 64, WL_SHM_FORMAT_XRGB8888) : NULL;
    if (!surface || !pool || !buffer ||
        wl_buffer_add_listener(buffer, &buffer_listener, &state) < 0)
        goto protocol_failed;
    wl_surface_attach(surface, buffer, 0, 0);
    wl_surface_damage(surface, 0, 0, 16, 16);
    wl_surface_commit(surface);
    if (wl_display_roundtrip(display) < 0 ||
        state.formats != 2u || state.releases != 1u)
        goto protocol_failed;

    wl_surface_destroy(surface);
    wl_buffer_destroy(buffer);
    wl_shm_pool_destroy(pool);
    munmap(pixels, 4096);
    close(shm_fd);
    wl_shm_destroy(shm);
    wl_compositor_destroy(compositor);
    wl_registry_destroy(registry);
    wl_display_disconnect(display);
    printf("wayland-lib-test: registry, SHM and surface passed\n");
    return 0;

protocol_failed:
    fprintf(stderr,
            "wayland-lib-test: protocol failed (globals=%u formats=%u release=%u)\n",
            state.globals, state.formats, state.releases);
    if (surface)
        wl_surface_destroy(surface);
    if (buffer)
        wl_buffer_destroy(buffer);
    if (pool)
        wl_shm_pool_destroy(pool);
    if (pixels != MAP_FAILED)
        munmap(pixels, 4096);
    if (shm_fd >= 0)
        close(shm_fd);
    if (shm)
        wl_shm_destroy(shm);
    if (compositor)
        wl_compositor_destroy(compositor);
    wl_registry_destroy(registry);
    wl_display_disconnect(display);
    return 1;
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
