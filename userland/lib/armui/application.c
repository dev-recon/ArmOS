/*
 * ArmOS
 * Copyright (c) 2026 Mohamed Ennassiri
 *
 * Licensed under the Apache License, Version 2.0.
 * See LICENSE for details.
 *
 * File: userland/lib/armui/application.c
 * Layer: Userland / graphical libraries
 *
 * Responsibilities:
 * - Own the common Wayland and xdg-shell application lifecycle.
 * - Route pointer, keyboard and UTF-8 input into an ArmUI context.
 * - Manage reusable SHM buffer generations and frame callbacks.
 *
 * Notes:
 * - Applications provide state and drawing callbacks, never Wayland policy.
 * - Platform display and input details remain behind the compositor.
 */

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <armos-shell-client-protocol.h>
#include <wayland-client.h>
#include <xdg-shell-client-protocol.h>
#include <xkbcommon/xkbcommon.h>

#include <armui/armui.h>

#define ARMUI_BUFFER_COUNT 2u
#define ARMUI_RETIRED_GENERATIONS 4u
#define ARMUI_REPEAT_DELAY_MS 350u
#define ARMUI_REPEAT_RATE_MS 83u

#define ARMUI_BTN_LEFT  0x110u
#define ARMUI_BTN_RIGHT 0x111u

struct armui_buffer_generation {
    struct wl_shm_pool *pool;
    struct wl_buffer *buffers[ARMUI_BUFFER_COUNT];
    uint32_t *mapping;
    size_t mapping_bytes;
    int busy[ARMUI_BUFFER_COUNT];
    int fd;
    int used;
};

struct armui_application {
    struct wl_display *display;
    struct wl_registry *registry;
    struct wl_compositor *compositor;
    struct wl_shm *shm;
    struct wl_seat *seat;
    struct wl_pointer *pointer;
    struct wl_keyboard *keyboard;
    struct xdg_wm_base *wm_base;
    struct wl_surface *surface;
    struct xdg_surface *xdg_surface;
    struct xdg_toplevel *toplevel;
    struct armos_shell_v1 *shell;
    struct armos_shell_panel_v1 *panel;
    struct wl_callback *frame_callback;
    struct armui_buffer_generation active;
    struct armui_buffer_generation retired[ARMUI_RETIRED_GENERATIONS];
    unsigned int generation;
    int width;
    int height;
    int pending_width;
    int pending_height;
    int minimum_width;
    int minimum_height;
    int pointer_x;
    int pointer_y;
    int pointer_down;
    int pointer_event_pending;
    int configured;
    int dirty;
    int continuous;
    int closed;
    int input_open;
    int waiting_for_buffer;
    int waiting_for_generation;
    int clear_target;
    int repeat_pulse;
    int repeat_started;
    int repeat_release;
    int settle_after_release;
    uint32_t background;
    uint32_t frame_time_ms;
    uint32_t next_repeat_ms;
    struct armui_context *ui;
    struct xkb_context *xkb_context;
    struct xkb_keymap *xkb_keymap;
    struct xkb_state *xkb_state;
    armui_application_frame_fn frame;
    armui_application_key_fn key;
    void *data;
    enum armui_application_role role;
};

static int armui_minimum(int left, int right)
{
    return left < right ? left : right;
}

static void armui_generation_init(struct armui_buffer_generation *generation)
{
    if (!generation)
        return;
    memset(generation, 0, sizeof(*generation));
    generation->fd = -1;
    generation->mapping = MAP_FAILED;
}

static void armui_generation_destroy(
    struct armui_buffer_generation *generation)
{
    if (!generation)
        return;
    for (size_t index = 0u; index < ARMUI_BUFFER_COUNT; index++) {
        if (generation->buffers[index])
            wl_buffer_destroy(generation->buffers[index]);
    }
    if (generation->pool)
        wl_shm_pool_destroy(generation->pool);
    if (generation->mapping && generation->mapping != MAP_FAILED)
        munmap(generation->mapping, generation->mapping_bytes);
    if (generation->fd >= 0)
        close(generation->fd);
    armui_generation_init(generation);
}

static int armui_generation_busy(
    const struct armui_buffer_generation *generation)
{
    if (!generation || !generation->used)
        return 0;
    for (size_t index = 0u; index < ARMUI_BUFFER_COUNT; index++) {
        if (generation->busy[index])
            return 1;
    }
    return 0;
}

static const struct wl_buffer_listener armui_buffer_listener;

static int armui_generation_create(
    struct armui_application *application,
    struct armui_buffer_generation *generation,
    int width, int height)
{
    char name[64];
    size_t buffer_bytes;

    if (!application || !generation ||
        width < application->minimum_width ||
        height < application->minimum_height)
        return -1;
    buffer_bytes = (size_t)width * (size_t)height * sizeof(uint32_t);
    if (buffer_bytes > (size_t)INT32_MAX / ARMUI_BUFFER_COUNT)
        return -1;
    armui_generation_init(generation);
    generation->mapping_bytes = buffer_bytes * ARMUI_BUFFER_COUNT;
    snprintf(name, sizeof(name), "/armui-%d-%u",
             getpid(), application->generation++);
    generation->fd = shm_open(name, O_CREAT | O_EXCL | O_RDWR, 0600);
    if (generation->fd < 0 || shm_unlink(name) < 0 ||
        ftruncate(generation->fd,
                  (off_t)generation->mapping_bytes) < 0)
        goto failed;
    generation->mapping = mmap(
        NULL, generation->mapping_bytes,
        PROT_READ | PROT_WRITE, MAP_SHARED, generation->fd, 0);
    if (generation->mapping == MAP_FAILED)
        goto failed;
    generation->pool = wl_shm_create_pool(
        application->shm, generation->fd,
        (int32_t)generation->mapping_bytes);
    if (!generation->pool)
        goto failed;
    close(generation->fd);
    generation->fd = -1;
    for (size_t index = 0u; index < ARMUI_BUFFER_COUNT; index++) {
        generation->buffers[index] = wl_shm_pool_create_buffer(
            generation->pool, (int32_t)(index * buffer_bytes),
            width, height, width * (int)sizeof(uint32_t),
            WL_SHM_FORMAT_XRGB8888);
        if (!generation->buffers[index] ||
            wl_buffer_add_listener(generation->buffers[index],
                                   &armui_buffer_listener,
                                   application) < 0)
            goto failed;
    }
    generation->used = 1;
    return 0;

failed:
    armui_generation_destroy(generation);
    return -1;
}

static struct armui_buffer_generation *armui_retire_active(
    struct armui_application *application)
{
    struct armui_buffer_generation *slot = NULL;

    for (size_t index = 0u;
         index < ARMUI_RETIRED_GENERATIONS; index++) {
        if (!application->retired[index].used) {
            slot = &application->retired[index];
            break;
        }
    }
    if (!slot)
        return NULL;
    *slot = application->active;
    armui_generation_init(&application->active);
    return slot;
}

static void armui_buffer_release(void *data, struct wl_buffer *buffer)
{
    struct armui_application *application = data;

    for (size_t index = 0u; index < ARMUI_BUFFER_COUNT; index++) {
        if (application->active.buffers[index] == buffer) {
            application->active.busy[index] = 0;
            if (application->waiting_for_buffer) {
                application->waiting_for_buffer = 0;
                application->dirty = 1;
            }
            return;
        }
    }
    for (size_t retired_index = 0u;
         retired_index < ARMUI_RETIRED_GENERATIONS; retired_index++) {
        struct armui_buffer_generation *generation =
            &application->retired[retired_index];
        int matched = 0;

        if (!generation->used)
            continue;
        for (size_t index = 0u; index < ARMUI_BUFFER_COUNT; index++) {
            if (generation->buffers[index] == buffer) {
                generation->busy[index] = 0;
                matched = 1;
            }
        }
        if (!matched)
            continue;
        if (!armui_generation_busy(generation)) {
            armui_generation_destroy(generation);
            if (application->waiting_for_generation) {
                application->waiting_for_generation = 0;
                application->dirty = 1;
            }
        }
        return;
    }
}

static const struct wl_buffer_listener armui_buffer_listener = {
    armui_buffer_release
};

static int armui_apply_resize(struct armui_application *application)
{
    struct armui_buffer_generation *retired;

    if (application->pending_width <= 0 ||
        application->pending_height <= 0)
        return 0;
    if (application->pending_width == application->width &&
        application->pending_height == application->height) {
        application->pending_width = 0;
        application->pending_height = 0;
        return 0;
    }
    retired = armui_retire_active(application);
    if (!retired) {
        application->waiting_for_generation = 1;
        return 0;
    }
    if (armui_generation_create(
            application, &application->active,
            application->pending_width,
            application->pending_height) < 0) {
        application->active = *retired;
        armui_generation_init(retired);
        return -1;
    }
    if (!armui_generation_busy(retired))
        armui_generation_destroy(retired);
    application->width = application->pending_width;
    application->height = application->pending_height;
    application->pending_width = 0;
    application->pending_height = 0;
    application->waiting_for_generation = 0;
    if (application->xdg_surface)
        xdg_surface_set_window_geometry(
            application->xdg_surface, 0, 0,
            application->width, application->height);
    application->dirty = 1;
    return 1;
}

static void armui_frame_done(void *data, struct wl_callback *callback,
                             uint32_t time)
{
    struct armui_application *application = data;

    if (callback == application->frame_callback) {
        wl_callback_destroy(callback);
        application->frame_callback = NULL;
    }
    application->frame_time_ms = time;
    if (application->pointer_down &&
        (int32_t)(time - application->next_repeat_ms) >= 0) {
        application->repeat_pulse = 1;
        application->repeat_started = 1;
        application->next_repeat_ms = time + ARMUI_REPEAT_RATE_MS;
    }
    if (application->continuous || application->pointer_down)
        application->dirty = 1;
}

static const struct wl_callback_listener armui_frame_listener = {
    armui_frame_done
};

static int armui_present(struct armui_application *application)
{
    struct armui_target target;
    unsigned int result;
    size_t index;
    size_t pixels_per_buffer;
    int settle_after_release;

    if (!application->configured || !application->dirty)
        return 0;
    /*
     * A surface owns at most one frame transaction in flight. Input remains
     * accumulated until the compositor releases that presentation slot.
     */
    if (application->frame_callback)
        return 0;
    if (armui_apply_resize(application) < 0)
        return -1;
    if (application->pending_width > 0)
        return 0;
    for (index = 0u; index < ARMUI_BUFFER_COUNT; index++) {
        if (!application->active.busy[index])
            break;
    }
    if (index == ARMUI_BUFFER_COUNT) {
        application->waiting_for_buffer = 1;
        return 0;
    }
    if (application->input_open) {
        armui_input_end(application->ui);
        application->input_open = 0;
    }
    pixels_per_buffer =
        (size_t)application->width * (size_t)application->height;
    target.pixels = application->active.mapping +
        index * pixels_per_buffer;
    target.width = application->width;
    target.height = application->height;
    target.stride = application->width;
    result = application->frame(
        application, application->ui, &target,
        application->frame_time_ms, application->data);
    if ((result & ARMUI_FRAME_ERROR) != 0u) {
        if (errno == 0)
            errno = EIO;
        return -1;
    }
    if ((result & ARMUI_FRAME_CLOSE) != 0u) {
        application->closed = 1;
        return 0;
    }
    armui_render(application->ui, &target,
                 application->clear_target,
                 application->background);
    application->continuous =
        (result & ARMUI_FRAME_CONTINUE) != 0u;
    /*
     * Nuklear is immediate-mode: a widget can change application state while
     * the current command list is being built.  Theme and enabled-state
     * changes therefore need one settled frame after button release.  Keep
     * this in the common application transaction instead of requiring every
     * application to request a second redraw explicitly.
     */
    settle_after_release = application->settle_after_release;
    application->settle_after_release = 0;
    application->dirty = 0;
    application->repeat_pulse = 0;
    application->repeat_release = 0;
    if (settle_after_release)
        application->dirty = 1;
    if ((application->continuous || application->pointer_down ||
         settle_after_release) &&
        !application->frame_callback) {
        application->frame_callback =
            wl_surface_frame(application->surface);
        if (!application->frame_callback ||
            wl_callback_add_listener(
                application->frame_callback,
                &armui_frame_listener, application) < 0)
            return -1;
    }
    application->active.busy[index] = 1;
    wl_surface_attach(application->surface,
                      application->active.buffers[index], 0, 0);
    wl_surface_damage_buffer(application->surface, 0, 0,
                             application->width,
                             application->height);
    wl_surface_commit(application->surface);
    return wl_display_flush(application->display) < 0 ? -1 : 1;
}

static void armui_pointer_enter(
    void *data, struct wl_pointer *pointer, uint32_t serial,
    struct wl_surface *surface, wl_fixed_t x, wl_fixed_t y)
{
    struct armui_application *application = data;

    (void)pointer;
    (void)serial;
    (void)surface;
    application->pointer_x = wl_fixed_to_int(x);
    application->pointer_y = wl_fixed_to_int(y);
    armui_input_motion(application->ui,
                       application->pointer_x,
                       application->pointer_y);
    application->pointer_event_pending = 1;
}

static void armui_pointer_leave(
    void *data, struct wl_pointer *pointer, uint32_t serial,
    struct wl_surface *surface)
{
    struct armui_application *application = data;

    (void)pointer;
    (void)serial;
    (void)surface;
    application->pointer_event_pending = 1;
}

static void armui_pointer_motion(
    void *data, struct wl_pointer *pointer, uint32_t time,
    wl_fixed_t x, wl_fixed_t y)
{
    struct armui_application *application = data;

    (void)pointer;
    (void)time;
    application->pointer_x = wl_fixed_to_int(x);
    application->pointer_y = wl_fixed_to_int(y);
    armui_input_motion(application->ui,
                       application->pointer_x,
                       application->pointer_y);
    application->pointer_event_pending = 1;
}

static void armui_pointer_button(
    void *data, struct wl_pointer *pointer, uint32_t serial,
    uint32_t time, uint32_t button, uint32_t state)
{
    struct armui_application *application = data;
    enum armui_button mapped;
    int pressed = state == WL_POINTER_BUTTON_STATE_PRESSED;

    (void)pointer;
    (void)serial;
    if (button == ARMUI_BTN_LEFT)
        mapped = ARMUI_BUTTON_LEFT;
    else if (button == ARMUI_BTN_RIGHT)
        mapped = ARMUI_BUTTON_RIGHT;
    else
        return;
    armui_input_button(application->ui, mapped,
                       application->pointer_x,
                       application->pointer_y, pressed);
    if (button == ARMUI_BTN_LEFT) {
        application->pointer_down = pressed;
        if (pressed) {
            application->repeat_started = 0;
            application->repeat_release = 0;
            application->next_repeat_ms =
                time + ARMUI_REPEAT_DELAY_MS;
        } else {
            application->repeat_release =
                application->repeat_started;
            application->repeat_started = 0;
            application->settle_after_release = 1;
        }
    }
    application->pointer_event_pending = 1;
}

static void armui_pointer_axis(
    void *data, struct wl_pointer *pointer, uint32_t time,
    uint32_t axis, wl_fixed_t value)
{
    struct armui_application *application = data;
    float amount = (float)wl_fixed_to_int(value) / 10.0f;

    (void)pointer;
    (void)time;
    if (axis == WL_POINTER_AXIS_VERTICAL_SCROLL)
        armui_input_scroll(application->ui, 0.0f, -amount);
    else
        armui_input_scroll(application->ui, -amount, 0.0f);
    application->pointer_event_pending = 1;
}

static void armui_pointer_frame(void *data, struct wl_pointer *pointer)
{
    struct armui_application *application = data;

    (void)pointer;
    if (application->pointer_event_pending) {
        application->pointer_event_pending = 0;
        application->dirty = 1;
    }
}

static void armui_pointer_axis_source(
    void *data, struct wl_pointer *pointer, uint32_t source)
{
    (void)data;
    (void)pointer;
    (void)source;
}

static void armui_pointer_axis_stop(
    void *data, struct wl_pointer *pointer, uint32_t time, uint32_t axis)
{
    (void)data;
    (void)pointer;
    (void)time;
    (void)axis;
}

static void armui_pointer_axis_discrete(
    void *data, struct wl_pointer *pointer, uint32_t axis, int32_t discrete)
{
    (void)data;
    (void)pointer;
    (void)axis;
    (void)discrete;
}

static const struct wl_pointer_listener armui_pointer_listener = {
    armui_pointer_enter,
    armui_pointer_leave,
    armui_pointer_motion,
    armui_pointer_button,
    armui_pointer_axis,
    armui_pointer_frame,
    armui_pointer_axis_source,
    armui_pointer_axis_stop,
    armui_pointer_axis_discrete
};

static void armui_keyboard_keymap(
    void *data, struct wl_keyboard *keyboard,
    uint32_t format, int32_t fd, uint32_t size)
{
    struct armui_application *application = data;
    struct xkb_keymap *keymap = NULL;
    struct xkb_state *state = NULL;
    char *mapping = MAP_FAILED;

    (void)keyboard;
    if (format == WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1 && size > 0u) {
        mapping = mmap(NULL, size, PROT_READ, MAP_PRIVATE, fd, 0);
        if (mapping != MAP_FAILED && application->xkb_context) {
            keymap = xkb_keymap_new_from_buffer(
                application->xkb_context, mapping, size,
                XKB_KEYMAP_FORMAT_TEXT_V1,
                XKB_KEYMAP_COMPILE_NO_FLAGS);
            if (keymap)
                state = xkb_state_new(keymap);
        }
    }
    if (mapping != MAP_FAILED)
        munmap(mapping, size);
    close(fd);
    if (!keymap || !state) {
        if (state)
            xkb_state_unref(state);
        if (keymap)
            xkb_keymap_unref(keymap);
        return;
    }
    if (application->xkb_state)
        xkb_state_unref(application->xkb_state);
    if (application->xkb_keymap)
        xkb_keymap_unref(application->xkb_keymap);
    application->xkb_keymap = keymap;
    application->xkb_state = state;
}

static void armui_keyboard_enter(
    void *data, struct wl_keyboard *keyboard, uint32_t serial,
    struct wl_surface *surface, struct wl_array *keys)
{
    (void)data;
    (void)keyboard;
    (void)serial;
    (void)surface;
    (void)keys;
}

static void armui_keyboard_leave(
    void *data, struct wl_keyboard *keyboard, uint32_t serial,
    struct wl_surface *surface)
{
    (void)data;
    (void)keyboard;
    (void)serial;
    (void)surface;
}

static void armui_keyboard_key(
    void *data, struct wl_keyboard *keyboard, uint32_t serial,
    uint32_t time, uint32_t key, uint32_t state)
{
    struct armui_application *application = data;
    enum armui_key mapped_key = ARMUI_KEY_ENTER;
    int has_mapped_key = 1;
    int pressed = state == WL_KEYBOARD_KEY_STATE_PRESSED;
    uint32_t codepoint;

    (void)keyboard;
    (void)serial;
    (void)time;
    if (key == 1u && pressed)
        application->closed = 1;
    if (key == 28u)
        mapped_key = ARMUI_KEY_ENTER;
    else if (key == 14u)
        mapped_key = ARMUI_KEY_BACKSPACE;
    else if (key == 103u)
        mapped_key = ARMUI_KEY_UP;
    else if (key == 108u)
        mapped_key = ARMUI_KEY_DOWN;
    else if (key == 105u)
        mapped_key = ARMUI_KEY_LEFT;
    else if (key == 106u)
        mapped_key = ARMUI_KEY_RIGHT;
    else
        has_mapped_key = 0;
    if (has_mapped_key) {
        armui_input_key(application->ui, mapped_key, pressed);
        if (application->key)
            application->key(
                application, mapped_key, pressed, application->data);
    }
    if (pressed && application->xkb_state) {
        codepoint = xkb_state_key_get_utf32(
            application->xkb_state, key + 8u);
        if (codepoint >= 0x20u && codepoint != 0x7fu)
            armui_input_unicode(application->ui, codepoint);
    }
    application->dirty = 1;
}

static void armui_keyboard_modifiers(
    void *data, struct wl_keyboard *keyboard, uint32_t serial,
    uint32_t depressed, uint32_t latched, uint32_t locked,
    uint32_t group)
{
    struct armui_application *application = data;

    (void)keyboard;
    (void)serial;
    if (application->xkb_state)
        (void)xkb_state_update_mask(
            application->xkb_state, depressed, latched, locked,
            0u, 0u, group);
}

static void armui_keyboard_repeat_info(
    void *data, struct wl_keyboard *keyboard, int32_t rate, int32_t delay)
{
    (void)data;
    (void)keyboard;
    (void)rate;
    (void)delay;
}

static const struct wl_keyboard_listener armui_keyboard_listener = {
    armui_keyboard_keymap,
    armui_keyboard_enter,
    armui_keyboard_leave,
    armui_keyboard_key,
    armui_keyboard_modifiers,
    armui_keyboard_repeat_info
};

static void armui_seat_capabilities(
    void *data, struct wl_seat *seat, uint32_t capabilities)
{
    struct armui_application *application = data;

    if ((capabilities & WL_SEAT_CAPABILITY_POINTER) != 0u &&
        !application->pointer) {
        application->pointer = wl_seat_get_pointer(seat);
        if (application->pointer)
            (void)wl_pointer_add_listener(
                application->pointer,
                &armui_pointer_listener, application);
    }
    if ((capabilities & WL_SEAT_CAPABILITY_KEYBOARD) != 0u &&
        !application->keyboard) {
        application->keyboard = wl_seat_get_keyboard(seat);
        if (application->keyboard)
            (void)wl_keyboard_add_listener(
                application->keyboard,
                &armui_keyboard_listener, application);
    }
}

static void armui_seat_name(
    void *data, struct wl_seat *seat, const char *name)
{
    (void)data;
    (void)seat;
    (void)name;
}

static const struct wl_seat_listener armui_seat_listener = {
    armui_seat_capabilities,
    armui_seat_name
};

static void armui_wm_base_ping(
    void *data, struct xdg_wm_base *wm_base, uint32_t serial)
{
    (void)data;
    xdg_wm_base_pong(wm_base, serial);
}

static const struct xdg_wm_base_listener armui_wm_base_listener = {
    armui_wm_base_ping
};

static void armui_xdg_surface_configure(
    void *data, struct xdg_surface *surface, uint32_t serial)
{
    struct armui_application *application = data;

    xdg_surface_ack_configure(surface, serial);
    application->configured = 1;
    application->dirty = 1;
}

static const struct xdg_surface_listener armui_xdg_surface_listener = {
    armui_xdg_surface_configure
};

static void armui_toplevel_configure(
    void *data, struct xdg_toplevel *toplevel,
    int32_t width, int32_t height, struct wl_array *states)
{
    struct armui_application *application = data;

    (void)toplevel;
    (void)states;
    if (width >= application->minimum_width &&
        height >= application->minimum_height &&
        (width != application->width ||
         height != application->height)) {
        application->pending_width = width;
        application->pending_height = height;
        application->dirty = 1;
    }
}

static void armui_toplevel_close(
    void *data, struct xdg_toplevel *toplevel)
{
    struct armui_application *application = data;

    (void)toplevel;
    application->closed = 1;
}

static const struct xdg_toplevel_listener armui_toplevel_listener = {
    armui_toplevel_configure,
    armui_toplevel_close
};

static void armui_registry_global(
    void *data, struct wl_registry *registry,
    uint32_t name, const char *interface, uint32_t version)
{
    struct armui_application *application = data;

    if (strcmp(interface, "wl_compositor") == 0) {
        application->compositor = wl_registry_bind(
            registry, name, &wl_compositor_interface,
            (uint32_t)armui_minimum((int)version, 4));
    } else if (strcmp(interface, "wl_shm") == 0) {
        application->shm = wl_registry_bind(
            registry, name, &wl_shm_interface, 1u);
    } else if (strcmp(interface, "wl_seat") == 0) {
        application->seat = wl_registry_bind(
            registry, name, &wl_seat_interface,
            (uint32_t)armui_minimum((int)version, 5));
    } else if (strcmp(interface, "xdg_wm_base") == 0) {
        application->wm_base = wl_registry_bind(
            registry, name, &xdg_wm_base_interface, 1u);
    } else if (strcmp(interface, "armos_shell_v1") == 0 &&
               application->role == ARMUI_APPLICATION_SYSTEM_BAR) {
        application->shell = wl_registry_bind(
            registry, name, &armos_shell_v1_interface, 1u);
    }
}

static void armui_registry_remove(
    void *data, struct wl_registry *registry, uint32_t name)
{
    (void)data;
    (void)registry;
    (void)name;
}

static const struct wl_registry_listener armui_registry_listener = {
    armui_registry_global,
    armui_registry_remove
};

static void armui_panel_configure(
    void *data, struct armos_shell_panel_v1 *panel,
    int32_t width, int32_t height)
{
    struct armui_application *application = data;

    (void)panel;
    if (width < application->minimum_width ||
        height < application->minimum_height) {
        application->closed = 1;
        errno = EPROTO;
        return;
    }
    application->width = width;
    application->height = height;
    application->configured = 1;
    application->dirty = 1;
}

static void armui_panel_closed(
    void *data, struct armos_shell_panel_v1 *panel)
{
    struct armui_application *application = data;

    (void)panel;
    application->closed = 1;
}

static const struct armos_shell_panel_v1_listener armui_panel_listener = {
    armui_panel_configure,
    armui_panel_closed
};

static int armui_application_setup(
    struct armui_application *application,
    const struct armui_application_config *config)
{
    application->ui = armui_create();
    application->xkb_context =
        xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    application->display = wl_display_connect(NULL);
    if (!application->ui || !application->xkb_context ||
        !application->display)
        return -1;
    application->registry =
        wl_display_get_registry(application->display);
    if (!application->registry ||
        wl_registry_add_listener(
            application->registry,
            &armui_registry_listener, application) < 0 ||
        wl_display_roundtrip(application->display) < 0 ||
        !application->compositor || !application->shm ||
        !application->seat ||
        (application->role == ARMUI_APPLICATION_WINDOW &&
         !application->wm_base) ||
        (application->role == ARMUI_APPLICATION_SYSTEM_BAR &&
         !application->shell) ||
        wl_seat_add_listener(
            application->seat,
            &armui_seat_listener, application) < 0 ||
        (application->wm_base && xdg_wm_base_add_listener(
            application->wm_base,
            &armui_wm_base_listener, application) < 0))
        return -1;
    application->surface =
        wl_compositor_create_surface(application->compositor);
    if (!application->surface)
        return -1;
    if (application->role == ARMUI_APPLICATION_SYSTEM_BAR) {
        const char *token_text = getenv("ARMOS_SHELL_TOKEN");
        char *end = NULL;
        unsigned long token;

        if (!token_text || *token_text == '\0') {
            errno = EACCES;
            return -1;
        }
        token = strtoul(token_text, &end, 10);
        if (!end || *end != '\0' || token == 0u ||
            token > UINT32_MAX) {
            errno = EACCES;
            return -1;
        }
        armos_shell_v1_authenticate(
            application->shell, (uint32_t)token);
        application->panel = armos_shell_v1_get_panel(
            application->shell, application->surface,
            application->height);
        if (!application->panel ||
            armos_shell_panel_v1_add_listener(
                application->panel, &armui_panel_listener,
                application) < 0 ||
            wl_display_roundtrip(application->display) < 0 ||
            !application->configured)
            return -1;
    } else {
        application->xdg_surface =
            xdg_wm_base_get_xdg_surface(
                application->wm_base, application->surface);
        if (!application->xdg_surface ||
            xdg_surface_add_listener(
                application->xdg_surface,
                &armui_xdg_surface_listener, application) < 0)
            return -1;
        application->toplevel =
            xdg_surface_get_toplevel(application->xdg_surface);
        if (!application->toplevel ||
            xdg_toplevel_add_listener(
                application->toplevel,
                &armui_toplevel_listener, application) < 0)
            return -1;
        xdg_toplevel_set_title(application->toplevel, config->title);
        xdg_toplevel_set_app_id(application->toplevel, config->app_id);
        xdg_toplevel_set_min_size(
            application->toplevel,
            application->minimum_width,
            application->minimum_height);
        xdg_toplevel_set_max_size(application->toplevel, 0, 0);
    }
    if (armui_generation_create(
            application, &application->active,
            application->width, application->height) < 0)
        return -1;
    if (application->xdg_surface)
        xdg_surface_set_window_geometry(
            application->xdg_surface, 0, 0,
            application->width, application->height);
    wl_surface_commit(application->surface);
    return wl_display_flush(application->display);
}

static void armui_application_destroy(
    struct armui_application *application)
{
    if (!application)
        return;
    if (application->frame_callback)
        wl_callback_destroy(application->frame_callback);
    if (application->pointer)
        wl_pointer_release(application->pointer);
    if (application->keyboard)
        wl_keyboard_release(application->keyboard);
    if (application->seat)
        wl_seat_release(application->seat);
    if (application->toplevel)
        xdg_toplevel_destroy(application->toplevel);
    if (application->xdg_surface)
        xdg_surface_destroy(application->xdg_surface);
    if (application->panel)
        armos_shell_panel_v1_destroy(application->panel);
    if (application->surface)
        wl_surface_destroy(application->surface);
    if (application->shell)
        armos_shell_v1_destroy(application->shell);
    armui_generation_destroy(&application->active);
    for (size_t index = 0u;
         index < ARMUI_RETIRED_GENERATIONS; index++)
        armui_generation_destroy(&application->retired[index]);
    if (application->wm_base)
        xdg_wm_base_destroy(application->wm_base);
    if (application->shm)
        wl_shm_destroy(application->shm);
    if (application->compositor)
        wl_compositor_destroy(application->compositor);
    if (application->registry)
        wl_registry_destroy(application->registry);
    if (application->display)
        wl_display_disconnect(application->display);
    if (application->xkb_state)
        xkb_state_unref(application->xkb_state);
    if (application->xkb_keymap)
        xkb_keymap_unref(application->xkb_keymap);
    if (application->xkb_context)
        xkb_context_unref(application->xkb_context);
    armui_destroy(application->ui);
}

int armui_application_run(
    const struct armui_application_config *config,
    armui_application_frame_fn frame, void *data)
{
    struct armui_application application;
    int status = 0;

    if (!config || !frame || !config->title || !config->app_id ||
        config->minimum_width <= 0 || config->minimum_height <= 0 ||
        config->width < config->minimum_width ||
        config->height < config->minimum_height) {
        errno = EINVAL;
        return -1;
    }
    memset(&application, 0, sizeof(application));
    armui_generation_init(&application.active);
    for (size_t index = 0u;
         index < ARMUI_RETIRED_GENERATIONS; index++)
        armui_generation_init(&application.retired[index]);
    application.width = config->width;
    application.height = config->height;
    application.minimum_width = config->minimum_width;
    application.minimum_height = config->minimum_height;
    application.clear_target = config->clear_target;
    application.background = config->background;
    application.frame = frame;
    application.key = config->key;
    application.data = data;
    application.role = config->role;
    application.dirty = 1;
    if (armui_application_setup(&application, config) < 0) {
        armui_application_destroy(&application);
        return -1;
    }
    while (!application.closed) {
        if (!application.input_open) {
            armui_input_begin(application.ui);
            application.input_open = 1;
        }
        if (wl_display_dispatch(application.display) < 0) {
            status = -1;
            break;
        }
        if (armui_present(&application) < 0) {
            status = -1;
            break;
        }
    }
    if (application.input_open)
        armui_input_end(application.ui);
    armui_application_destroy(&application);
    return status;
}

void armui_application_request_redraw(
    struct armui_application *application)
{
    if (application)
        application->dirty = 1;
}

void armui_application_close(struct armui_application *application)
{
    if (application)
        application->closed = 1;
}

void armui_application_set_background(
    struct armui_application *application,
    int clear_target, uint32_t background)
{
    if (!application)
        return;
    application->clear_target = clear_target;
    application->background = background;
}

int armui_application_pointer_down(
    const struct armui_application *application)
{
    return application ? application->pointer_down : 0;
}

int armui_application_repeat_pulse(
    const struct armui_application *application)
{
    return application ?
        application->repeat_pulse || application->repeat_release : 0;
}
