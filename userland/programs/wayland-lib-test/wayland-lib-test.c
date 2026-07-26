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
#include <poll.h>
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
    unsigned int data_device_manager;
    uint32_t compositor_name;
    uint32_t compositor_version;
    uint32_t shm_name;
    uint32_t seat_name;
    uint32_t seat_version;
    uint32_t shell_name;
    uint32_t data_device_manager_name;
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
    unsigned int clipboard_offers;
    unsigned int clipboard_selections;
    unsigned int clipboard_sends;
    struct wl_data_offer *selection_offer;
};

static const char clipboard_mime[] = "text/plain;charset=utf-8";
static const char clipboard_text[] = "ArmOS clipboard";

static const struct wl_message generated_compositor_methods[] = {
    {"create_surface", "n", NULL},
    {"create_region", "n", NULL}
};

static const struct wl_interface generated_compositor_interface = {
    "wl_compositor", 4, 2, generated_compositor_methods, 0, NULL
};

static const struct wl_message generated_shm_methods[] = {
    {"create_pool", "nhi", NULL}
};

static const struct wl_interface generated_shm_interface = {
    "wl_shm", 1, 1, generated_shm_methods, 1, NULL
};

static const struct wl_message server_test_events[] = {
    {"words", "us", NULL},
    {"descriptor", "uhu", NULL},
    {"new_object", "n", NULL}
};

static const struct wl_message server_test_methods[] = {
    {"request", "ush", NULL}
};

static const struct wl_interface server_test_interface = {
    "armos_server_test", 1, 1, server_test_methods, 3, server_test_events
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
        state->compositor_version = version;
    }
    if (strcmp(interface, "wl_shm") == 0) {
        state->shm++;
        state->shm_name = name;
    }
    if (strcmp(interface, "wl_seat") == 0) {
        state->seat++;
        state->seat_name = name;
        state->seat_version = version;
    }
    if (strcmp(interface, "xdg_wm_base") == 0) {
        state->shell++;
        state->shell_name = name;
    }
    if (strcmp(interface, "wl_data_device_manager") == 0) {
        state->data_device_manager++;
        state->data_device_manager_name = name;
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

static void data_source_target(void *data, struct wl_data_source *source,
                               const char *mime_type)
{
    (void)data;
    (void)source;
    (void)mime_type;
}

static void data_source_send(void *data, struct wl_data_source *source,
                             const char *mime_type, int32_t fd)
{
    struct registry_state *state = data;
    size_t length = sizeof(clipboard_text);

    (void)source;
    if (strcmp(mime_type, clipboard_mime) == 0 &&
        write(fd, clipboard_text, length) == (ssize_t)length)
        state->clipboard_sends++;
    close(fd);
}

static void data_source_cancelled(void *data, struct wl_data_source *source)
{
    (void)data;
    (void)source;
}

static void data_offer_offer(void *data, struct wl_data_offer *offer,
                             const char *mime_type)
{
    struct registry_state *state = data;

    (void)offer;
    if (strcmp(mime_type, clipboard_mime) == 0)
        state->clipboard_offers++;
}

static void data_device_data_offer(void *data,
                                   struct wl_data_device *device,
                                   struct wl_data_offer *offer)
{
    static const struct wl_data_offer_listener listener = {
        data_offer_offer
    };

    (void)device;
    (void)wl_data_offer_add_listener(offer, &listener, data);
}

static void data_device_enter(void *data, struct wl_data_device *device,
                              uint32_t serial, struct wl_surface *surface,
                              wl_fixed_t x, wl_fixed_t y,
                              struct wl_data_offer *offer)
{
    (void)data;
    (void)device;
    (void)serial;
    (void)surface;
    (void)x;
    (void)y;
    (void)offer;
}

static void data_device_leave(void *data, struct wl_data_device *device)
{
    (void)data;
    (void)device;
}

static void data_device_motion(void *data, struct wl_data_device *device,
                               uint32_t time, wl_fixed_t x, wl_fixed_t y)
{
    (void)data;
    (void)device;
    (void)time;
    (void)x;
    (void)y;
}

static void data_device_drop(void *data, struct wl_data_device *device)
{
    (void)data;
    (void)device;
}

static void data_device_selection(void *data,
                                  struct wl_data_device *device,
                                  struct wl_data_offer *offer)
{
    struct registry_state *state = data;

    (void)device;
    state->selection_offer = offer;
    state->clipboard_selections++;
}

static const struct wl_data_source_listener clipboard_source_listener = {
    data_source_target,
    data_source_send,
    data_source_cancelled
};

static const struct wl_data_device_listener clipboard_device_listener = {
    data_device_data_offer,
    data_device_enter,
    data_device_leave,
    data_device_motion,
    data_device_drop,
    data_device_selection
};

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
    struct wl_event_queue *event_queue = NULL;
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
    struct wl_data_device_manager *data_device_manager = NULL;
    struct wl_data_source *data_source = NULL;
    struct wl_data_device *data_device = NULL;
    struct xdg_wm_base *wm_base = NULL;
    struct xdg_surface *xdg_surface = NULL;
    struct xdg_toplevel *xdg_toplevel = NULL;
    uint32_t *pixels = MAP_FAILED;
    char shm_name[48];
    int shm_fd = -1;
    int clipboard_pipe[2] = { -1, -1 };
    char clipboard_buffer[sizeof(clipboard_text)] = { 0 };
    struct pollfd wayland_poll;
    int valid;

    for (unsigned int attempt = 0u; attempt < 100u; attempt++) {
        display = wl_display_connect(NULL);
        if (display || errno != ENOENT)
            break;
        usleep(10000u);
    }
    if (!display) {
        perror("wayland-lib-test: connect");
        return 1;
    }
    event_queue = wl_display_create_queue(display);
    registry = wl_display_get_registry(display);
    if (!event_queue || !registry ||
        wl_registry_add_listener(registry, &listener, &state) < 0) {
        perror("wayland-lib-test: registry");
        wl_registry_destroy(registry);
        wl_display_disconnect(display);
        return 1;
    }
    wl_proxy_set_queue((struct wl_proxy *)registry, event_queue);
    if (wl_display_prepare_read_queue(display, event_queue) < 0)
        goto protocol_failed;
    wl_display_cancel_read(display);
    if (wl_display_prepare_read_queue(display, event_queue) < 0)
        goto protocol_failed;
    wayland_poll.fd = wl_display_get_fd(display);
    wayland_poll.events = POLLIN;
    wayland_poll.revents = 0;
    if (poll(&wayland_poll, 1u, 1000) <= 0 ||
        !(wayland_poll.revents & POLLIN) ||
        wl_display_read_events(display) < 0) {
        wl_display_cancel_read(display);
        goto protocol_failed;
    }
    errno = 0;
    if (wl_display_prepare_read_queue(display, event_queue) == 0 ||
        errno != EAGAIN)
        goto protocol_failed;
    if (wl_display_dispatch_pending(display) != 0 ||
        wl_display_dispatch_queue_pending(display, event_queue) <= 0 ||
        wl_display_roundtrip_queue(display, event_queue) < 0)
        goto protocol_failed;
    valid = state.globals == 6u && state.compositor == 1u &&
        state.shm == 1u && state.seat == 1u && state.shell == 1u &&
        state.data_device_manager == 1u &&
        state.compositor_version >= 4u && state.seat_version >= 5u;
    if (!valid)
        goto protocol_failed;
    compositor = wl_registry_bind(registry, state.compositor_name,
                                  &generated_compositor_interface, 4u);
    shm = wl_registry_bind(registry, state.shm_name,
                           &generated_shm_interface, 1u);
    seat = wl_registry_bind(registry, state.seat_name, &wl_seat_interface,
                            5u);
    wm_base = wl_registry_bind(registry, state.shell_name,
                               &xdg_wm_base_interface, 1u);
    output = wl_registry_bind(registry, state.output_name,
                              &wl_output_interface, 2u);
    data_device_manager = wl_registry_bind(
        registry, state.data_device_manager_name,
        &wl_data_device_manager_interface, 1u);
    if (!compositor || !shm || !seat || !wm_base || !output ||
        !data_device_manager ||
        wl_seat_add_listener(seat, &seat_listener, &state) < 0 ||
        wl_shm_add_listener(shm, &shm_listener, &state) < 0 ||
        wl_output_add_listener(output, &output_listener, &state) < 0 ||
        xdg_wm_base_add_listener(wm_base, &wm_base_listener, &state) < 0)
        goto protocol_failed;
    pointer = wl_seat_get_pointer(seat);
    keyboard = wl_seat_get_keyboard(seat);
    data_source =
        wl_data_device_manager_create_data_source(data_device_manager);
    data_device =
        wl_data_device_manager_get_data_device(data_device_manager, seat);
    if (!pointer || !keyboard || !data_source || !data_device ||
        wl_data_source_add_listener(data_source, &clipboard_source_listener,
                                    &state) < 0 ||
        wl_data_device_add_listener(data_device, &clipboard_device_listener,
                                    &state) < 0 ||
        wl_keyboard_add_listener(keyboard, &keyboard_listener, &state) < 0)
        goto protocol_failed;
    wl_data_source_offer(data_source, clipboard_mime);
    wl_data_device_set_selection(data_device, data_source, 1u);
    surface = (struct wl_surface *)wl_proxy_marshal_flags(
        (struct wl_proxy *)compositor, 0u, &wl_surface_interface, 4u, 0u,
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
    wl_surface_set_buffer_scale(surface, 1);
    wl_surface_damage_buffer(surface, 0, 0, 16, 16);
    wl_surface_set_user_data(surface, &state);
    if (wl_surface_get_user_data(surface) != &state)
        goto protocol_failed;
    wl_surface_commit(surface);
    if (wl_display_roundtrip_queue(display, event_queue) < 0 ||
        state.formats != 2u || state.releases != 1u ||
        state.seat_capabilities !=
            (WL_SEAT_CAPABILITY_POINTER | WL_SEAT_CAPABILITY_KEYBOARD) ||
        state.keymaps != 1u || state.repeat_info != 1u ||
        state.output_geometry != 1u || state.output_modes != 1u ||
        state.output_done != 1u || state.output_scale != 1u ||
        state.surface_enters != 1u ||
        state.xdg_configures != 1u || state.toplevel_configures != 1u ||
        state.clipboard_offers != 1u ||
        state.clipboard_selections == 0u || !state.selection_offer)
        goto protocol_failed;
    if (pipe(clipboard_pipe) < 0)
        goto protocol_failed;
    wl_data_offer_receive(state.selection_offer, clipboard_mime,
                          clipboard_pipe[1]);
    close(clipboard_pipe[1]);
    clipboard_pipe[1] = -1;
    if (wl_display_roundtrip_queue(display, event_queue) < 0 ||
        read(clipboard_pipe[0], clipboard_buffer,
             sizeof(clipboard_buffer)) !=
            (ssize_t)sizeof(clipboard_buffer) ||
        memcmp(clipboard_buffer, clipboard_text,
               sizeof(clipboard_text)) != 0 ||
        state.clipboard_sends != 1u)
        goto protocol_failed;
    close(clipboard_pipe[0]);
    clipboard_pipe[0] = -1;
    for (unsigned int iteration = 0; iteration < 600u; iteration++) {
        if (wl_display_roundtrip_queue(display, event_queue) < 0)
            goto protocol_failed;
    }

    wl_data_offer_destroy(state.selection_offer);
    wl_data_device_destroy(data_device);
    wl_data_source_destroy(data_source);
    wl_data_device_manager_destroy(data_device_manager);
    wl_keyboard_release(keyboard);
    wl_pointer_release(pointer);
    wl_seat_release(seat);
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
    wl_event_queue_destroy(event_queue);
    wl_display_disconnect(display);
    printf("wayland-lib-test: registry, SHM, input, clipboard and xdg-shell passed\n");
    return 0;

protocol_failed:
    fprintf(stderr,
            "wayland-lib-test: protocol failed (error=%d errno=%d globals=%u formats=%u release=%u seat=%u keymap=%u repeat=%u clipboard=%u/%u/%u xdg=%u/%u)\n",
            wl_display_get_error(display), errno,
            state.globals, state.formats, state.releases,
            (unsigned)state.seat_capabilities, state.keymaps,
            state.repeat_info, state.clipboard_offers,
            state.clipboard_selections, state.clipboard_sends,
            state.xdg_configures, state.toplevel_configures);
    if (clipboard_pipe[0] >= 0)
        close(clipboard_pipe[0]);
    if (clipboard_pipe[1] >= 0)
        close(clipboard_pipe[1]);
    if (state.selection_offer)
        wl_data_offer_destroy(state.selection_offer);
    if (data_device)
        wl_data_device_destroy(data_device);
    if (data_source)
        wl_data_source_destroy(data_source);
    if (data_device_manager)
        wl_data_device_manager_destroy(data_device_manager);
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
    wl_event_queue_destroy(event_queue);
    wl_display_disconnect(display);
    return 1;
}

static int test_clipboard(void)
{
    static const struct wl_registry_listener registry_listener = {
        registry_global,
        registry_global_remove
    };
    struct registry_state source_state = { 0 };
    struct registry_state target_state = { 0 };
    struct wl_display *source_display = NULL;
    struct wl_display *target_display = NULL;
    struct wl_registry *source_registry = NULL;
    struct wl_registry *target_registry = NULL;
    struct wl_data_device_manager *source_manager = NULL;
    struct wl_data_device_manager *target_manager = NULL;
    struct wl_data_source *source = NULL;
    struct wl_data_device *source_device = NULL;
    struct wl_data_device *target_device = NULL;
    struct wl_seat *source_seat = NULL;
    struct wl_seat *target_seat = NULL;
    int transfer[2] = { -1, -1 };
    char received[sizeof(clipboard_text)] = { 0 };
    int source_error;
    int target_error;
    int saved_errno;
    int result = 1;

    for (unsigned int attempt = 0u; attempt < 100u; attempt++) {
        source_display = wl_display_connect(NULL);
        if (source_display || errno != ENOENT)
            break;
        usleep(10000u);
    }
    target_display = wl_display_connect(NULL);
    if (!source_display || !target_display)
        goto done;
    source_registry = wl_display_get_registry(source_display);
    target_registry = wl_display_get_registry(target_display);
    if (!source_registry || !target_registry ||
        wl_registry_add_listener(source_registry, &registry_listener,
                                 &source_state) < 0 ||
        wl_registry_add_listener(target_registry, &registry_listener,
                                 &target_state) < 0 ||
        wl_display_roundtrip(source_display) < 0 ||
        wl_display_roundtrip(target_display) < 0 ||
        source_state.data_device_manager != 1u ||
        source_state.seat != 1u ||
        target_state.data_device_manager != 1u ||
        target_state.seat != 1u)
        goto done;
    source_manager = wl_registry_bind(
        source_registry, source_state.data_device_manager_name,
        &wl_data_device_manager_interface, 1u);
    target_manager = wl_registry_bind(
        target_registry, target_state.data_device_manager_name,
        &wl_data_device_manager_interface, 1u);
    source_seat = wl_registry_bind(source_registry, source_state.seat_name,
                                   &wl_seat_interface, 1u);
    target_seat = wl_registry_bind(target_registry, target_state.seat_name,
                                   &wl_seat_interface, 1u);
    source = source_manager ?
        wl_data_device_manager_create_data_source(source_manager) : NULL;
    source_device = source_manager && source_seat ?
        wl_data_device_manager_get_data_device(source_manager,
                                               source_seat) : NULL;
    target_device = target_manager && target_seat ?
        wl_data_device_manager_get_data_device(target_manager,
                                               target_seat) : NULL;
    if (!source_manager || !target_manager ||
        !source_seat || !target_seat || !source ||
        !source_device || !target_device ||
        wl_data_source_add_listener(source, &clipboard_source_listener,
                                    &source_state) < 0 ||
        wl_data_device_add_listener(target_device,
                                    &clipboard_device_listener,
                                    &target_state) < 0)
        goto done;
    wl_data_source_offer(source, clipboard_mime);
    wl_data_device_set_selection(source_device, source, 1u);
    if (wl_display_roundtrip(source_display) < 0 ||
        wl_display_roundtrip(target_display) < 0 ||
        target_state.clipboard_offers != 1u ||
        target_state.clipboard_selections == 0u ||
        !target_state.selection_offer ||
        pipe(transfer) < 0)
        goto done;
    wl_data_offer_receive(target_state.selection_offer, clipboard_mime,
                          transfer[1]);
    close(transfer[1]);
    transfer[1] = -1;
    if (wl_display_roundtrip(target_display) < 0 ||
        wl_display_roundtrip(source_display) < 0 ||
        read(transfer[0], received, sizeof(received)) !=
            (ssize_t)sizeof(received) ||
        memcmp(received, clipboard_text, sizeof(received)) != 0 ||
        source_state.clipboard_sends != 1u)
        goto done;
    printf("wayland-lib-test: cross-client clipboard transfer passed\n");
    result = 0;

done:
    source_error = source_display ?
        wl_display_get_error(source_display) : 0;
    target_error = target_display ?
        wl_display_get_error(target_display) : 0;
    saved_errno = errno;
    if (transfer[0] >= 0)
        close(transfer[0]);
    if (transfer[1] >= 0)
        close(transfer[1]);
    if (target_state.selection_offer)
        wl_data_offer_destroy(target_state.selection_offer);
    if (target_device)
        wl_data_device_destroy(target_device);
    if (source_device)
        wl_data_device_destroy(source_device);
    if (source)
        wl_data_source_destroy(source);
    if (target_manager)
        wl_data_device_manager_destroy(target_manager);
    if (source_manager)
        wl_data_device_manager_destroy(source_manager);
    if (target_seat)
        wl_seat_destroy(target_seat);
    if (source_seat)
        wl_seat_destroy(source_seat);
    if (target_registry)
        wl_registry_destroy(target_registry);
    if (source_registry)
        wl_registry_destroy(source_registry);
    if (target_display)
        wl_display_disconnect(target_display);
    if (source_display)
        wl_display_disconnect(source_display);
    if (result != 0) {
        fprintf(stderr,
                "wayland-lib-test: clipboard transfer failed "
                "(source_error=%d target_error=%d errno=%d "
                "globals=%u/%u manager=%u/%u seat=%u/%u "
                "offer=%u selection=%u send=%u)\n",
                source_error, target_error, saved_errno,
                source_state.globals, target_state.globals,
                source_state.data_device_manager,
                target_state.data_device_manager,
                source_state.seat, target_state.seat,
                target_state.clipboard_offers,
                target_state.clipboard_selections,
                source_state.clipboard_sends);
    }
    return result;
}

struct server_loop_test_state {
    int pipe_events;
    int timer_events;
    int idle_events;
    int resource_destroys;
    int request_dispatches;
};

static const int server_resource_implementation;
static const int server_dispatch_implementation;

struct server_test_implementation {
    void (*request)(struct wl_client *client, struct wl_resource *resource,
                    uint32_t value, const char *text, int32_t fd);
};

static int server_pipe_event(int fd, uint32_t mask, void *data)
{
    struct server_loop_test_state *state = data;
    char byte;

    if ((mask & WL_EVENT_READABLE) == 0u || read(fd, &byte, 1u) != 1)
        return -1;
    state->pipe_events++;
    return -1;
}

static int server_timer_event(void *data)
{
    struct server_loop_test_state *state = data;

    state->timer_events++;
    return -1;
}

static void server_idle_event(void *data)
{
    struct server_loop_test_state *state = data;

    state->idle_events++;
}

static void server_global_bind(struct wl_client *client, void *data,
                               uint32_t version, uint32_t id)
{
    (void)client;
    (void)data;
    (void)version;
    (void)id;
}

static void server_resource_destroy(struct wl_resource *resource)
{
    struct server_loop_test_state *state =
        wl_resource_get_user_data(resource);

    if (state)
        state->resource_destroys++;
}

static uint32_t server_test_u32(const uint8_t *data)
{
    uint32_t value;

    memcpy(&value, data, sizeof(value));
    return value;
}

static void server_test_store_u32(uint8_t *data, uint32_t value)
{
    memcpy(data, &value, sizeof(value));
}

static int server_request_dispatch(
    const void *implementation, void *target, uint32_t opcode,
    const struct wl_message *message, union wl_argument *arguments)
{
    struct wl_resource *resource = target;
    struct server_loop_test_state *state =
        wl_resource_get_user_data(resource);
    char byte;

    if (implementation != &server_dispatch_implementation ||
        opcode != 0u || !message ||
        strcmp(message->name, "request") != 0 ||
        arguments[0].u != 77u ||
        !arguments[1].s || strcmp(arguments[1].s, "request") != 0 ||
        read(arguments[2].h, &byte, 1u) != 1 || byte != 'r') {
        close(arguments[2].h);
        return -1;
    }
    close(arguments[2].h);
    state->request_dispatches++;
    return 0;
}

static void server_generated_request(
    struct wl_client *client, struct wl_resource *resource,
    uint32_t value, const char *text, int32_t fd)
{
    struct server_loop_test_state *state =
        wl_resource_get_user_data(resource);
    char byte;

    if (client == wl_resource_get_client(resource) && value == 77u &&
        text && strcmp(text, "request") == 0 &&
        read(fd, &byte, 1u) == 1 && byte == 'r')
        state->request_dispatches++;
    close(fd);
}

static const struct server_test_implementation server_generated_implementation = {
    .request = server_generated_request
};

static int server_test_send_request(int peer_fd, uint32_t object_id)
{
    uint8_t message[24] = { 0 };
    unsigned char control[CMSG_SPACE(sizeof(int))];
    struct iovec vector;
    struct msghdr header;
    struct cmsghdr *control_header;
    int descriptor_pipe[2];
    ssize_t count;

    if (pipe(descriptor_pipe) < 0)
        return -1;
    if (write(descriptor_pipe[1], "r", 1u) != 1)
        goto failed;
    server_test_store_u32(message, object_id);
    server_test_store_u32(message + 4u, (24u << 16) | 0u);
    server_test_store_u32(message + 8u, 77u);
    server_test_store_u32(message + 12u, 8u);
    memcpy(message + 16u, "request", 8u);
    memset(control, 0, sizeof(control));
    memset(&header, 0, sizeof(header));
    vector.iov_base = message;
    vector.iov_len = sizeof(message);
    header.msg_iov = &vector;
    header.msg_iovlen = 1u;
    header.msg_control = control;
    header.msg_controllen = sizeof(control);
    control_header = CMSG_FIRSTHDR(&header);
    control_header->cmsg_level = SOL_SOCKET;
    control_header->cmsg_type = SCM_RIGHTS;
    control_header->cmsg_len = CMSG_LEN(sizeof(int));
    memcpy(CMSG_DATA(control_header), &descriptor_pipe[0], sizeof(int));
    count = sendmsg(peer_fd, &header, 0);
    close(descriptor_pipe[0]);
    close(descriptor_pipe[1]);
    return count == (ssize_t)sizeof(message) ? 0 : -1;

failed:
    close(descriptor_pipe[0]);
    close(descriptor_pipe[1]);
    return -1;
}

static int server_test_resource_events(struct wl_resource *resource,
                                       struct wl_resource *new_resource,
                                       int peer_fd)
{
    uint8_t message[32];
    union wl_argument arguments[2];
    int descriptor_pipe[2] = { -1, -1 };
    int received_fd = -1;
    unsigned char control[CMSG_SPACE(sizeof(int))];
    struct iovec vector;
    struct msghdr header;
    struct cmsghdr *control_header;
    ssize_t count;

    arguments[0].u = 42u;
    arguments[1].s = "ok";
    wl_resource_post_event_array(resource, 0u, arguments);
    if (read(peer_fd, message, 20u) != 20 ||
        server_test_u32(message) != wl_resource_get_id(resource) ||
        server_test_u32(message + 4u) != ((20u << 16) | 0u) ||
        server_test_u32(message + 8u) != 42u ||
        server_test_u32(message + 12u) != 3u ||
        strcmp((char *)message + 16u, "ok") != 0)
        return -1;

    if (pipe(descriptor_pipe) < 0)
        return -1;
    wl_resource_post_event(resource, 1u, 1u, descriptor_pipe[0], 64u);
    memset(message, 0, sizeof(message));
    memset(control, 0, sizeof(control));
    memset(&header, 0, sizeof(header));
    vector.iov_base = message;
    vector.iov_len = 16u;
    header.msg_iov = &vector;
    header.msg_iovlen = 1u;
    header.msg_control = control;
    header.msg_controllen = sizeof(control);
    count = recvmsg(peer_fd, &header, MSG_CMSG_CLOEXEC);
    control_header = CMSG_FIRSTHDR(&header);
    if (count != 16 || server_test_u32(message + 8u) != 1u ||
        server_test_u32(message + 12u) != 64u || !control_header ||
        control_header->cmsg_level != SOL_SOCKET ||
        control_header->cmsg_type != SCM_RIGHTS ||
        control_header->cmsg_len != CMSG_LEN(sizeof(int)))
        goto failed;
    memcpy(&received_fd, CMSG_DATA(control_header), sizeof(received_fd));
    if (received_fd < 0)
        goto failed;
    close(received_fd);
    close(descriptor_pipe[0]);
    close(descriptor_pipe[1]);
    wl_resource_post_event(resource, 2u, new_resource);
    if (read(peer_fd, message, 12u) != 12 ||
        server_test_u32(message + 8u) !=
            wl_resource_get_id(new_resource))
        return -1;
    return 0;

failed:
    if (received_fd >= 0)
        close(received_fd);
    close(descriptor_pipe[0]);
    close(descriptor_pipe[1]);
    return -1;
}

static int test_transport(void)
{
    static const struct wl_registry_listener transport_registry_listener = {
        .global = registry_global,
        .global_remove = registry_global_remove
    };
    struct wl_display *server;
    struct wl_event_loop *event_loop;
    struct wl_event_source *source;
    struct wl_global *transport_global;
    struct server_loop_test_state loop_state = { 0 };
    char path[64];
    int pipe_fds[2] = { -1, -1 };
    int object_fds[2] = { -1, -1 };
    int status;
    pid_t child;

    snprintf(path, sizeof(path), "/tmp/wayland-lib-%d", getpid());
    server = wl_display_create();
    if (!server || wl_display_add_socket(server, path) < 0) {
        perror("wayland-lib-test: server");
        return 1;
    }
    if (socketpair(AF_LOCAL, SOCK_STREAM, 0, object_fds) < 0)
        goto transport_failed;
    {
        struct wl_client *server_client =
            wl_client_create(server, object_fds[0]);
        struct wl_resource *resource;
        struct wl_resource *event_resource;
        struct wl_resource *new_resource;
        struct wl_global *global;

        if (!server_client)
            goto transport_failed;
        object_fds[0] = -1;
        if (wl_client_get_fd(server_client) < 0 ||
            wl_client_get_display(server_client) != server)
            goto transport_failed;
        resource = wl_resource_create(server_client,
                                      &wl_compositor_interface, 1, 7u);
        if (!resource ||
            wl_resource_create(server_client, &wl_compositor_interface,
                               1, 7u) != NULL ||
            wl_client_get_object(server_client, 7u) != resource)
            goto transport_failed;
        wl_resource_set_implementation(
            resource, &server_resource_implementation, &loop_state,
            server_resource_destroy);
        if (wl_resource_get_id(resource) != 7u ||
            wl_resource_get_version(resource) != 1 ||
            strcmp(wl_resource_get_class(resource), "wl_compositor") != 0 ||
            wl_resource_get_client(resource) != server_client ||
            wl_resource_get_user_data(resource) != &loop_state ||
            !wl_resource_instance_of(
                resource, &wl_compositor_interface,
                &server_resource_implementation))
            goto transport_failed;
        global = wl_global_create(server, &wl_compositor_interface, 1,
                                  &loop_state, server_global_bind);
        if (!global || wl_global_get_name(global) == 0u ||
            wl_global_get_version(global) != 1u ||
            wl_global_get_interface(global) != &wl_compositor_interface ||
            wl_global_get_user_data(global) != &loop_state)
            goto transport_failed;
        event_resource = wl_resource_create(server_client,
                                            &server_test_interface, 1, 8u);
        new_resource = wl_resource_create(server_client,
                                          &server_test_interface, 1, 9u);
        if (!event_resource || !new_resource ||
            server_test_resource_events(event_resource, new_resource,
                                        object_fds[1]) < 0)
            goto transport_failed;
        wl_resource_set_dispatcher(
            event_resource, server_request_dispatch,
            &server_dispatch_implementation, &loop_state, NULL);
        event_loop = wl_display_get_event_loop(server);
        if (!event_loop ||
            server_test_send_request(object_fds[1],
                                     wl_resource_get_id(event_resource)) < 0 ||
            wl_event_loop_dispatch(event_loop, 1000) != 1 ||
            loop_state.request_dispatches != 1)
            goto transport_failed;
        wl_resource_set_implementation(
            new_resource, &server_generated_implementation,
            &loop_state, NULL);
        if (server_test_send_request(
                object_fds[1], wl_resource_get_id(new_resource)) < 0 ||
            wl_event_loop_dispatch(event_loop, 1000) != 1 ||
            loop_state.request_dispatches != 2)
            goto transport_failed;
        wl_resource_destroy(resource);
        if (loop_state.resource_destroys != 1 ||
            wl_client_get_object(server_client, 7u) != NULL)
            goto transport_failed;
        wl_global_destroy(global);
        wl_client_destroy(server_client);
        close(object_fds[1]);
        object_fds[1] = -1;
    }
    event_loop = wl_display_get_event_loop(server);
    if (!event_loop || pipe(pipe_fds) < 0)
        goto transport_failed;
    source = wl_event_loop_add_fd(event_loop, pipe_fds[0],
                                  WL_EVENT_READABLE,
                                  server_pipe_event, &loop_state);
    if (!source ||
        wl_event_source_fd_update(source, WL_EVENT_READABLE) < 0 ||
        write(pipe_fds[1], "x", 1u) != 1 ||
        wl_event_loop_dispatch(event_loop, 1000) != 1 ||
        loop_state.pipe_events != 1 ||
        wl_event_loop_dispatch(event_loop, 0) != 0)
        goto transport_failed;
    source = wl_event_loop_add_timer(event_loop, server_timer_event,
                                     &loop_state);
    if (!source || wl_event_source_timer_update(source, 0) < 0 ||
        !wl_event_loop_add_idle(event_loop, server_idle_event, &loop_state) ||
        wl_event_loop_dispatch(event_loop, 1000) != 2 ||
        loop_state.timer_events != 1 || loop_state.idle_events != 1 ||
        wl_event_loop_dispatch(event_loop, 0) != 0)
        goto transport_failed;
    close(pipe_fds[0]);
    close(pipe_fds[1]);
    pipe_fds[0] = -1;
    pipe_fds[1] = -1;
    transport_global = wl_global_create(
        server, &wl_compositor_interface, 1, NULL, server_global_bind);
    if (!transport_global)
        goto transport_failed;
    child = fork();
    if (child == 0) {
        struct wl_display *client = wl_display_connect(path);
        struct wl_registry *registry =
            client ? wl_display_get_registry(client) : NULL;
        struct registry_state state = { 0 };
        int valid = client && registry &&
            wl_registry_add_listener(
                registry, &transport_registry_listener, &state) == 0 &&
            wl_display_roundtrip(client) >= 0 &&
            state.compositor == 1u &&
            wl_display_get_fd(client) >= 0 &&
            wl_display_get_error(client) == 0 &&
            wl_display_flush(client) == 0;

        wl_registry_destroy(registry);
        wl_display_disconnect(client);
        _exit(valid ? 0 : 2);
    }
    if (child < 0) {
        goto transport_failed;
    }

    if (wl_event_loop_dispatch(event_loop, 1000) != 1 ||
        wl_event_loop_dispatch(event_loop, 1000) != 1)
        goto child_failed;
    waitpid(child, &status, 0);
    wl_display_destroy(server);

    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        fprintf(stderr, "wayland-lib-test: transport failed\n");
        return 1;
    }
    printf("wayland-lib-test: client/server transport passed\n");
    return 0;

child_failed:
    waitpid(child, &status, 0);
transport_failed:
    if (pipe_fds[0] >= 0)
        close(pipe_fds[0]);
    if (pipe_fds[1] >= 0)
        close(pipe_fds[1]);
    if (object_fds[0] >= 0)
        close(object_fds[0]);
    if (object_fds[1] >= 0)
        close(object_fds[1]);
    wl_display_destroy(server);
    fprintf(stderr, "wayland-lib-test: server event loop failed\n");
    return 1;
}

int main(int argc, char **argv)
{
    if (argc == 2 && strcmp(argv[1], "--registry") == 0)
        return test_registry();
    if (argc == 2 && strcmp(argv[1], "--clipboard") == 0)
        return test_clipboard();
    if (argc != 1) {
        fprintf(stderr,
                "usage: wayland-lib-test [--registry|--clipboard]\n");
        return 2;
    }
    return test_transport();
}
