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

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>
#include <wayland-client.h>
#include <wayland-server.h>
#include <xdg-shell-client-protocol.h>

struct registry_state {
    unsigned int globals;
    unsigned int compositor;
    unsigned int shm;
    unsigned int seat;
    unsigned int shell;
    uint32_t compositor_name;
    uint32_t shm_name;
    uint32_t seat_name;
    uint32_t shell_name;
    uint32_t output_name;
    unsigned int formats;
    unsigned int releases;
    uint32_t seat_capabilities;
    unsigned int xdg_configures;
    unsigned int toplevel_configures;
    unsigned int keymaps;
    unsigned int repeat_info;
    unsigned int output_geometry;
    unsigned int output_modes;
    unsigned int output_done;
    unsigned int output_scale;
    unsigned int surface_enters;
};

static const struct wl_message generated_compositor_methods[] = {
    {"create_surface", "n", NULL},
    {"create_region", "n", NULL}
};

static const struct wl_interface generated_compositor_interface = {
    "wl_compositor", 1, 2, generated_compositor_methods, 0, NULL
};

static const struct wl_message generated_shm_methods[] = {
    {"create_pool", "nhi", NULL}
};

static const struct wl_interface generated_shm_interface = {
    "wl_shm", 1, 1, generated_shm_methods, 1, NULL
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
    if (strcmp(interface, "wl_seat") == 0) {
        state->seat++;
        state->seat_name = name;
    }
    if (strcmp(interface, "xdg_wm_base") == 0) {
        state->shell++;
        state->shell_name = name;
    }
    if (strcmp(interface, "wl_output") == 0)
        state->output_name = name;
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

static void seat_capabilities(void *data, struct wl_seat *seat,
                              uint32_t capabilities)
{
    struct registry_state *state = data;

    (void)seat;
    state->seat_capabilities = capabilities;
}

static void seat_name(void *data, struct wl_seat *seat, const char *name)
{
    (void)data;
    (void)seat;
    (void)name;
}

static void keyboard_keymap(void *data, struct wl_keyboard *keyboard,
                            uint32_t format, int32_t fd, uint32_t size)
{
    struct registry_state *state = data;
    const char *mapping;

    (void)keyboard;
    mapping = mmap(NULL, size, PROT_READ, MAP_SHARED, fd, 0);
    if (format == WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1 &&
        size > 11u && mapping != MAP_FAILED &&
        strncmp(mapping, "xkb_keymap", 10u) == 0)
        state->keymaps++;
    if (mapping != MAP_FAILED)
        munmap((void *)mapping, size);
    close(fd);
}

static void keyboard_enter(void *data, struct wl_keyboard *keyboard,
                           uint32_t serial, struct wl_surface *surface,
                           struct wl_array *keys)
{
    (void)data;
    (void)keyboard;
    (void)serial;
    (void)surface;
    (void)keys;
}

static void keyboard_leave(void *data, struct wl_keyboard *keyboard,
                           uint32_t serial, struct wl_surface *surface)
{
    (void)data;
    (void)keyboard;
    (void)serial;
    (void)surface;
}

static void keyboard_key(void *data, struct wl_keyboard *keyboard,
                         uint32_t serial, uint32_t time, uint32_t key,
                         uint32_t state)
{
    (void)data;
    (void)keyboard;
    (void)serial;
    (void)time;
    (void)key;
    (void)state;
}

static void keyboard_modifiers(void *data, struct wl_keyboard *keyboard,
                               uint32_t serial, uint32_t depressed,
                               uint32_t latched, uint32_t locked,
                               uint32_t group)
{
    (void)data;
    (void)keyboard;
    (void)serial;
    (void)depressed;
    (void)latched;
    (void)locked;
    (void)group;
}

static void keyboard_repeat_info(void *data, struct wl_keyboard *keyboard,
                                 int32_t rate, int32_t delay)
{
    struct registry_state *state = data;

    (void)keyboard;
    if (rate == 25 && delay == 600)
        state->repeat_info++;
}

static void output_geometry(void *data, struct wl_output *output,
                            int32_t x, int32_t y, int32_t physical_width,
                            int32_t physical_height, int32_t subpixel,
                            const char *make, const char *model,
                            int32_t transform)
{
    struct registry_state *state = data;

    (void)output;
    (void)x;
    (void)y;
    (void)physical_width;
    (void)physical_height;
    if (subpixel == WL_OUTPUT_SUBPIXEL_UNKNOWN &&
        transform == WL_OUTPUT_TRANSFORM_NORMAL &&
        strcmp(make, "ArmOS") == 0 && model[0] != '\0')
        state->output_geometry++;
}

static void output_mode(void *data, struct wl_output *output,
                        uint32_t flags, int32_t width, int32_t height,
                        int32_t refresh)
{
    struct registry_state *state = data;

    (void)output;
    if ((flags & (WL_OUTPUT_MODE_CURRENT | WL_OUTPUT_MODE_PREFERRED)) ==
            (WL_OUTPUT_MODE_CURRENT | WL_OUTPUT_MODE_PREFERRED) &&
        width > 0 && height > 0 && refresh == 60000)
        state->output_modes++;
}

static void output_done(void *data, struct wl_output *output)
{
    struct registry_state *state = data;

    (void)output;
    state->output_done++;
}

static void output_scale(void *data, struct wl_output *output, int32_t factor)
{
    struct registry_state *state = data;

    (void)output;
    if (factor == 1)
        state->output_scale++;
}

static void surface_enter(void *data, struct wl_surface *surface,
                          struct wl_output *output)
{
    struct registry_state *state = data;

    (void)surface;
    (void)output;
    state->surface_enters++;
}

static void surface_leave(void *data, struct wl_surface *surface,
                          struct wl_output *output)
{
    (void)data;
    (void)surface;
    (void)output;
}

static void xdg_ping(void *data, struct xdg_wm_base *xdg_wm_base,
                     uint32_t serial)
{
    (void)data;
    xdg_wm_base_pong(xdg_wm_base, serial);
}

static void xdg_configure(void *data, struct xdg_surface *xdg_surface,
                          uint32_t serial)
{
    struct registry_state *state = data;

    state->xdg_configures++;
    xdg_surface_ack_configure(xdg_surface, serial);
}

static void toplevel_configure(void *data,
                               struct xdg_toplevel *xdg_toplevel,
                               int32_t width, int32_t height,
                               struct wl_array *states)
{
    struct registry_state *state = data;

    (void)xdg_toplevel;
    (void)width;
    (void)height;
    (void)states;
    state->toplevel_configures++;
}

static void toplevel_close(void *data, struct xdg_toplevel *xdg_toplevel)
{
    (void)data;
    (void)xdg_toplevel;
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
    static const struct wl_seat_listener seat_listener = {
        seat_capabilities,
        seat_name
    };
    static const struct wl_keyboard_listener keyboard_listener = {
        keyboard_keymap,
        keyboard_enter,
        keyboard_leave,
        keyboard_key,
        keyboard_modifiers,
        keyboard_repeat_info
    };
    static const struct wl_output_listener output_listener = {
        output_geometry,
        output_mode,
        output_done,
        output_scale
    };
    static const struct wl_surface_listener surface_listener = {
        surface_enter,
        surface_leave
    };
    static const struct xdg_wm_base_listener wm_base_listener = {
        xdg_ping
    };
    static const struct xdg_surface_listener xdg_surface_listener = {
        xdg_configure
    };
    static const struct xdg_toplevel_listener toplevel_listener = {
        toplevel_configure,
        toplevel_close
    };
    struct registry_state state = { 0 };
    struct wl_display *display;
    struct wl_registry *registry;
    struct wl_compositor *compositor = NULL;
    struct wl_surface *surface = NULL;
    struct wl_shm *shm = NULL;
    struct wl_shm_pool *pool = NULL;
    struct wl_buffer *buffer = NULL;
    struct wl_seat *seat = NULL;
    struct wl_pointer *pointer = NULL;
    struct wl_keyboard *keyboard = NULL;
    struct wl_output *output = NULL;
    struct xdg_wm_base *wm_base = NULL;
    struct xdg_surface *xdg_surface = NULL;
    struct xdg_toplevel *xdg_toplevel = NULL;
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
    valid = state.globals == 5u && state.compositor == 1u &&
        state.shm == 1u && state.seat == 1u && state.shell == 1u;
    if (!valid)
        goto protocol_failed;
    compositor = wl_registry_bind(registry, state.compositor_name,
                                  &generated_compositor_interface, 1u);
    shm = wl_registry_bind(registry, state.shm_name,
                           &generated_shm_interface, 1u);
    seat = wl_registry_bind(registry, state.seat_name, &wl_seat_interface,
                            4u);
    wm_base = wl_registry_bind(registry, state.shell_name,
                               &xdg_wm_base_interface, 1u);
    output = wl_registry_bind(registry, state.output_name,
                              &wl_output_interface, 2u);
    if (!compositor || !shm || !seat || !wm_base || !output ||
        wl_seat_add_listener(seat, &seat_listener, &state) < 0 ||
        wl_shm_add_listener(shm, &shm_listener, &state) < 0 ||
        wl_output_add_listener(output, &output_listener, &state) < 0 ||
        xdg_wm_base_add_listener(wm_base, &wm_base_listener, &state) < 0)
        goto protocol_failed;
    pointer = wl_seat_get_pointer(seat);
    keyboard = wl_seat_get_keyboard(seat);
    if (!pointer || !keyboard ||
        wl_keyboard_add_listener(keyboard, &keyboard_listener, &state) < 0)
        goto protocol_failed;
    surface = (struct wl_surface *)wl_proxy_marshal_flags(
        (struct wl_proxy *)compositor, 0u, &wl_surface_interface, 1u, 0u,
        NULL);
    if (!surface ||
        wl_surface_add_listener(surface, &surface_listener, &state) < 0)
        goto protocol_failed;
    xdg_surface = surface ?
        xdg_wm_base_get_xdg_surface(wm_base, surface) : NULL;
    if (!xdg_surface ||
        xdg_surface_add_listener(xdg_surface, &xdg_surface_listener,
                                 &state) < 0)
        goto protocol_failed;
    xdg_toplevel = xdg_surface_get_toplevel(xdg_surface);
    if (!xdg_toplevel ||
        xdg_toplevel_add_listener(xdg_toplevel, &toplevel_listener,
                                  &state) < 0)
        goto protocol_failed;
    xdg_toplevel_set_title(xdg_toplevel, "ArmOS Wayland library test");
    xdg_toplevel_set_app_id(xdg_toplevel, "org.armos.wayland-lib-test");
    xdg_surface_set_window_geometry(xdg_surface, 0, 0, 16, 16);
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
    pool = (struct wl_shm_pool *)wl_proxy_marshal_flags(
        (struct wl_proxy *)shm, 0u, &wl_shm_pool_interface, 1u, 0u,
        NULL, shm_fd, 4096);
    buffer = pool ? wl_shm_pool_create_buffer(
        pool, 0, 16, 16, 64, WL_SHM_FORMAT_XRGB8888) : NULL;
    if (!surface || !pool || !buffer ||
        wl_buffer_add_listener(buffer, &buffer_listener, &state) < 0)
        goto protocol_failed;
    wl_surface_attach(surface, buffer, 0, 0);
    wl_surface_damage(surface, 0, 0, 16, 16);
    wl_surface_commit(surface);
    if (wl_display_roundtrip(display) < 0 ||
        state.formats != 2u || state.releases != 1u ||
        state.seat_capabilities !=
            (WL_SEAT_CAPABILITY_POINTER | WL_SEAT_CAPABILITY_KEYBOARD) ||
        state.keymaps != 1u || state.repeat_info != 1u ||
        state.output_geometry != 1u || state.output_modes != 1u ||
        state.output_done != 1u || state.output_scale != 1u ||
        state.surface_enters != 1u ||
        state.xdg_configures != 1u || state.toplevel_configures != 1u)
        goto protocol_failed;
    for (unsigned int iteration = 0; iteration < 600u; iteration++) {
        if (wl_display_roundtrip(display) < 0)
            goto protocol_failed;
    }

    wl_keyboard_destroy(keyboard);
    wl_pointer_destroy(pointer);
    wl_seat_destroy(seat);
    xdg_toplevel_destroy(xdg_toplevel);
    xdg_surface_destroy(xdg_surface);
    xdg_wm_base_destroy(wm_base);
    wl_surface_destroy(surface);
    wl_buffer_destroy(buffer);
    wl_shm_pool_destroy(pool);
    munmap(pixels, 4096);
    close(shm_fd);
    wl_shm_destroy(shm);
    wl_output_destroy(output);
    wl_compositor_destroy(compositor);
    wl_registry_destroy(registry);
    wl_display_disconnect(display);
    printf("wayland-lib-test: registry, SHM, input and xdg-shell passed\n");
    return 0;

protocol_failed:
    fprintf(stderr,
            "wayland-lib-test: protocol failed (error=%d errno=%d globals=%u formats=%u release=%u seat=%u keymap=%u repeat=%u xdg=%u/%u)\n",
            wl_display_get_error(display), errno,
            state.globals, state.formats, state.releases,
            (unsigned)state.seat_capabilities, state.keymaps,
            state.repeat_info,
            state.xdg_configures, state.toplevel_configures);
    if (keyboard)
        wl_keyboard_destroy(keyboard);
    if (pointer)
        wl_pointer_destroy(pointer);
    if (seat)
        wl_seat_destroy(seat);
    if (xdg_toplevel)
        xdg_toplevel_destroy(xdg_toplevel);
    if (xdg_surface)
        xdg_surface_destroy(xdg_surface);
    if (wm_base)
        xdg_wm_base_destroy(wm_base);
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
    if (output)
        wl_output_destroy(output);
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
