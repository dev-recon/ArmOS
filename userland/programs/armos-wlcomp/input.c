/*
 * ArmOS
 * Copyright (c) 2026 Mohamed Ennassiri
 *
 * Licensed under the Apache License, Version 2.0.
 * See LICENSE for details.
 *
 * File: userland/programs/armos-wlcomp/input.c
 * Layer: Userland / graphical services
 *
 * Responsibilities:
 * - Consume architecture-neutral events from /dev/input0.
 * - Maintain pointer focus and title-bar dragging policy.
 * - Deliver wl_pointer and wl_keyboard events to focused clients.
 *
 * Notes:
 * - Device-specific decoding remains in kernel platform drivers.
 * - Window movement is compositor policy and never enters the kernel.
 */

#include "armos_wlcomp.h"

#include <errno.h>
#include <unistd.h>

#define WL_WINDOW_TITLE_HEIGHT 28
#define WL_WINDOW_CLOSE_X      14
#define WL_WINDOW_CLOSE_Y      14
#define WL_WINDOW_BUTTON_HIT   9

#define WL_XKB_MOD_SHIFT   (1u << 0)
#define WL_XKB_MOD_LOCK    (1u << 1)
#define WL_XKB_MOD_CONTROL (1u << 2)
#define WL_XKB_MOD_ALT     (1u << 3)

static struct wl_server_object *wl_find_input_object(
    struct wl_server_client *client, enum wl_server_object_type type)
{
    if (!client || !client->used)
        return NULL;
    for (size_t index = 0; index < WL_SERVER_MAX_OBJECTS; index++) {
        if (client->objects[index].type == type)
            return &client->objects[index];
    }
    return NULL;
}

static struct wl_server_object *wl_find_surface_object(
    struct wl_server_client *client, enum wl_server_object_type type,
    struct wl_server_surface *surface)
{
    if (!client || !client->used || !surface)
        return NULL;
    for (size_t index = 0u; index < WL_SERVER_MAX_OBJECTS; index++) {
        struct wl_server_object *object = &client->objects[index];

        if (object->type == type && object->resource == surface)
            return object;
    }
    return NULL;
}

static int wl_surface_close_button_at(const struct wl_server_surface *surface,
                                      int32_t x, int32_t y)
{
    int32_t local_x;
    int32_t local_y;

    if (!surface)
        return 0;
    local_x = x - surface->x;
    local_y = y - surface->y;
    return local_x >= WL_WINDOW_CLOSE_X - WL_WINDOW_BUTTON_HIT &&
           local_x <= WL_WINDOW_CLOSE_X + WL_WINDOW_BUTTON_HIT &&
           local_y >= WL_WINDOW_CLOSE_Y - WL_WINDOW_BUTTON_HIT &&
           local_y <= WL_WINDOW_CLOSE_Y + WL_WINDOW_BUTTON_HIT;
}

static int wl_send_toplevel_close(struct wl_server_client *client,
                                  struct wl_server_surface *surface)
{
    struct wl_server_object *toplevel = wl_find_surface_object(
        client, WL_SERVER_OBJECT_XDG_TOPLEVEL, surface);

    if (!toplevel)
        return 0;
    return wl_client_send_words(client, toplevel->id, 1u, NULL, 0u);
}

static struct wl_server_surface *wl_surface_at(
    struct wl_server *server, int32_t x, int32_t y,
    struct wl_server_client **owner)
{
    struct wl_server_surface *top = NULL;
    struct wl_server_client *top_owner = NULL;

    for (size_t ci = 0u; ci < WL_SERVER_MAX_CLIENTS; ci++) {
        struct wl_server_client *client = &server->clients[ci];

        if (!client->used)
            continue;
        for (size_t si = 0u; si < WL_SERVER_MAX_SURFACES; si++) {
            struct wl_server_surface *surface = &client->surfaces[si];
            int32_t bottom;

            if (!surface->used || !surface->mapped)
                continue;
            bottom = surface->y + WL_WINDOW_TITLE_HEIGHT +
                     (int32_t)surface->height;
            if (x >= surface->x &&
                x < surface->x + (int32_t)surface->width &&
                y >= surface->y && y < bottom) {
                if (!top || surface->z_order > top->z_order) {
                    top = surface;
                    top_owner = client;
                }
            }
        }
    }
    *owner = top_owner;
    return top;
}

static void wl_send_pointer_motion(struct wl_server *server,
                                   uint32_t timestamp)
{
    struct wl_server_client *client;
    struct wl_server_surface *surface;
    struct wl_server_object *pointer;
    uint32_t words[3];

    surface = wl_surface_at(server, server->pointer_x, server->pointer_y,
                            &client);
    pointer = wl_find_input_object(client, WL_SERVER_OBJECT_POINTER);
    if (!surface || !pointer)
        return;
    words[0] = timestamp;
    words[1] = (uint32_t)((server->pointer_x - surface->x) * 256);
    words[2] = (uint32_t)((server->pointer_y - surface->y -
                           WL_WINDOW_TITLE_HEIGHT) * 256);
    (void)wl_client_send_words(client, pointer->id, 2u, words, 3u);
}

static void wl_send_keyboard_modifiers(struct wl_server *server,
                                       struct wl_server_client *client,
                                       struct wl_server_object *keyboard)
{
    uint32_t words[5];

    if (!server || !client || !keyboard)
        return;
    words[0] = ++server->serial;
    words[1] = server->modifiers_depressed;
    words[2] = 0u;
    words[3] = server->modifiers_locked;
    words[4] = 0u;
    (void)wl_client_send_words(client, keyboard->id, 4u, words, 5u);
}

static void wl_focus_surface(struct wl_server *server,
                             struct wl_server_client *client,
                             struct wl_server_surface *surface)
{
    struct wl_server_object *pointer;
    struct wl_server_object *keyboard;
    struct wl_server_object *old_pointer;
    struct wl_server_object *old_keyboard;
    uint32_t enter[4];
    uint32_t keyboard_enter[3];
    uint32_t leave[2];

    if (server->focus_client == client && server->focus_surface == surface)
        return;
    if (server->focus_client && server->focus_client->used &&
        server->focus_surface && server->focus_surface->used) {
        old_pointer = wl_find_input_object(server->focus_client,
                                           WL_SERVER_OBJECT_POINTER);
        old_keyboard = wl_find_input_object(server->focus_client,
                                            WL_SERVER_OBJECT_KEYBOARD);
        leave[0] = ++server->serial;
        leave[1] = server->focus_surface->object_id;
        if (old_pointer) {
            (void)wl_client_send_words(server->focus_client,
                                       old_pointer->id, 1u, leave, 2u);
        }
        if (old_keyboard) {
            leave[0] = ++server->serial;
            (void)wl_client_send_words(server->focus_client,
                                       old_keyboard->id, 2u, leave, 2u);
        }
    }
    server->focus_client = client;
    server->focus_surface = surface;
    pointer = wl_find_input_object(client, WL_SERVER_OBJECT_POINTER);
    if (pointer) {
        enter[0] = ++server->serial;
    enter[1] = surface->object_id;
    enter[2] = 0u;
    enter[3] = 0u;
    (void)wl_client_send_words(client, pointer->id, 0u, enter, 4u);
    }
    keyboard = wl_find_input_object(client, WL_SERVER_OBJECT_KEYBOARD);
    if (keyboard) {
        keyboard_enter[0] = ++server->serial;
        keyboard_enter[1] = surface->object_id;
        keyboard_enter[2] = 0u;
        (void)wl_client_send_words(client, keyboard->id, 1u,
                                   keyboard_enter, 3u);
        wl_send_keyboard_modifiers(server, client, keyboard);
    }
}

static void wl_handle_button(struct wl_server *server,
                             const struct armos_input_event *event)
{
    struct wl_server_client *client = NULL;
    struct wl_server_surface *surface = NULL;
    struct wl_server_object *pointer;
    uint32_t words[4];
    bool pressed = event->value != 0;

    if (event->code != ARMOS_INPUT_BUTTON_LEFT)
        return;
    server->pointer_left = pressed;
    if (pressed) {
        surface = wl_surface_at(server, server->pointer_x, server->pointer_y,
                                &client);
        if (surface) {
            bool close_button;

            if (surface->z_order != server->next_surface_z) {
                surface->z_order = ++server->next_surface_z;
                server->scene_damage_pending = true;
            }
            wl_focus_surface(server, client, surface);
            close_button = wl_surface_close_button_at(
                surface, server->pointer_x, server->pointer_y);
            if (close_button) {
                (void)wl_send_toplevel_close(client, surface);
            } else if (server->pointer_y <
                surface->y + WL_WINDOW_TITLE_HEIGHT) {
                server->drag_client = client;
                server->drag_surface = surface;
                server->drag_offset_x = server->pointer_x - surface->x;
                server->drag_offset_y = server->pointer_y - surface->y;
            }
        }
    } else {
        client = server->focus_client;
        surface = server->focus_surface;
        server->drag_client = NULL;
        server->drag_surface = NULL;
    }
    pointer = wl_find_input_object(client, WL_SERVER_OBJECT_POINTER);
    if (!surface || !pointer)
        return;
    words[0] = ++server->serial;
    words[1] = event->timestamp_ms;
    words[2] = event->code;
    words[3] = pressed ? 1u : 0u;
    if (pressed)
        server->pointer_grab_serial = words[0];
    else
        server->pointer_grab_serial = 0u;
    (void)wl_client_send_words(client, pointer->id, 3u, words, 4u);
}

static void wl_handle_key(struct wl_server *server,
                          const struct armos_input_event *event)
{
    struct wl_server_object *keyboard;
    uint32_t words[4];
    uint32_t previous_depressed;
    uint32_t previous_locked;
    uint32_t mask = 0u;
    bool modifiers_changed;

    if (event->code >= ARMOS_INPUT_BUTTON_LEFT)
        return;
    previous_depressed = server->modifiers_depressed;
    previous_locked = server->modifiers_locked;
    if (event->code == ARMOS_INPUT_KEY_LEFTSHIFT ||
        event->code == ARMOS_INPUT_KEY_RIGHTSHIFT)
        mask = WL_XKB_MOD_SHIFT;
    else if (event->code == ARMOS_INPUT_KEY_LEFTCTRL ||
             event->code == ARMOS_INPUT_KEY_RIGHTCTRL)
        mask = WL_XKB_MOD_CONTROL;
    else if (event->code == ARMOS_INPUT_KEY_LEFTALT ||
             event->code == ARMOS_INPUT_KEY_RIGHTALT)
        mask = WL_XKB_MOD_ALT;
    if (mask != 0u) {
        if (event->value != 0)
            server->modifiers_depressed |= mask;
        else
            server->modifiers_depressed &= ~mask;
    } else if (event->code == ARMOS_INPUT_KEY_CAPSLOCK &&
               event->value == 1) {
        server->modifiers_locked ^= WL_XKB_MOD_LOCK;
    }
    modifiers_changed =
        server->modifiers_depressed != previous_depressed ||
        server->modifiers_locked != previous_locked;
    if (!server->focus_client || !server->focus_client->used ||
        !server->focus_surface || !server->focus_surface->used)
        return;
    keyboard = wl_find_input_object(server->focus_client,
                                    WL_SERVER_OBJECT_KEYBOARD);
    if (!keyboard)
        return;
    words[0] = ++server->serial;
    words[1] = event->timestamp_ms;
    words[2] = event->code;
    words[3] = event->value != 0 ? 1u : 0u;
    (void)wl_client_send_words(server->focus_client, keyboard->id, 3u,
                               words, 4u);
    if (modifiers_changed)
        wl_send_keyboard_modifiers(server, server->focus_client, keyboard);
}

static void wl_handle_input_event(struct wl_server *server,
                                  const struct armos_input_event *event)
{
    if (event->type == ARMOS_INPUT_EVENT_RELATIVE ||
        event->type == ARMOS_INPUT_EVENT_ABSOLUTE) {
        int32_t previous_pointer_x = server->pointer_x;
        int32_t previous_pointer_y = server->pointer_y;

        if (event->code == ARMOS_INPUT_AXIS_X) {
            if (event->type == ARMOS_INPUT_EVENT_ABSOLUTE) {
                server->pointer_x = (int32_t)(
                    ((uint64_t)(uint32_t)event->value *
                     (server->renderer.framebuffer.width - 1u)) /
                    ARMOS_INPUT_ABSOLUTE_MAX);
            } else {
                server->pointer_x += event->value;
            }
        } else if (event->code == ARMOS_INPUT_AXIS_Y) {
            if (event->type == ARMOS_INPUT_EVENT_ABSOLUTE) {
                server->pointer_y = (int32_t)(
                    ((uint64_t)(uint32_t)event->value *
                     (server->renderer.framebuffer.height - 1u)) /
                    ARMOS_INPUT_ABSOLUTE_MAX);
            } else {
                server->pointer_y += event->value;
            }
        }
        if (server->pointer_x < 0)
            server->pointer_x = 0;
        if (server->pointer_y < 0)
            server->pointer_y = 0;
        if ((uint32_t)server->pointer_x >= server->renderer.framebuffer.width)
            server->pointer_x =
                (int32_t)server->renderer.framebuffer.width - 1;
        if ((uint32_t)server->pointer_y >= server->renderer.framebuffer.height)
            server->pointer_y =
                (int32_t)server->renderer.framebuffer.height - 1;
        if ((server->pointer_x != previous_pointer_x ||
             server->pointer_y != previous_pointer_y) &&
            server->drag_surface && server->drag_client &&
            server->drag_surface->used && server->drag_client->used) {
            if (!server->move_damage_pending) {
                server->move_old_x = server->drag_surface->x;
                server->move_old_y = server->drag_surface->y;
                server->move_client = server->drag_client;
                server->move_surface = server->drag_surface;
                server->move_damage_pending = true;
            }
            server->drag_surface->x =
                server->pointer_x - server->drag_offset_x;
            server->drag_surface->y =
                server->pointer_y - server->drag_offset_y;
        }
        wl_send_pointer_motion(server, event->timestamp_ms);
    } else if (event->type == ARMOS_INPUT_EVENT_KEY) {
        if (event->code >= ARMOS_INPUT_BUTTON_LEFT)
            wl_handle_button(server, event);
        else
            wl_handle_key(server, event);
    }
}

int wl_server_handle_input(struct wl_server *server)
{
    struct armos_input_event events[32];
    bool handled = false;

    for (;;) {
        ssize_t size = read(server->input_fd, events, sizeof(events));

        if (size < 0) {
            if (errno == EAGAIN)
                return handled ? 1 : 0;
            return errno == EINTR ? (handled ? 1 : 0) : -1;
        }
        if (size == 0)
            return handled ? 1 : 0;
        if ((size_t)size % sizeof(events[0]) != 0u)
            return -1;
        for (size_t index = 0;
             index < (size_t)size / sizeof(events[0]); index++)
            wl_handle_input_event(server, &events[index]);
        handled = true;
        if ((size_t)size < sizeof(events))
            return 1;
    }
}
