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

static struct wl_server_surface *wl_surface_at(
    struct wl_server *server, int32_t x, int32_t y,
    struct wl_server_client **owner)
{
    for (size_t ci = WL_SERVER_MAX_CLIENTS; ci > 0u; ci--) {
        struct wl_server_client *client = &server->clients[ci - 1u];

        if (!client->used)
            continue;
        for (size_t si = WL_SERVER_MAX_SURFACES; si > 0u; si--) {
            struct wl_server_surface *surface = &client->surfaces[si - 1u];
            int32_t bottom;

            if (!surface->used || !surface->mapped)
                continue;
            bottom = surface->y + WL_WINDOW_TITLE_HEIGHT +
                     (int32_t)surface->height;
            if (x >= surface->x &&
                x < surface->x + (int32_t)surface->width &&
                y >= surface->y && y < bottom) {
                *owner = client;
                return surface;
            }
        }
    }
    *owner = NULL;
    return NULL;
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
    uint32_t enter[4];
    uint32_t keyboard_enter[3];

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
            wl_focus_surface(server, client, surface);
            if (server->pointer_y <
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
        if (server->drag_surface && server->drag_client &&
            server->drag_surface->used && server->drag_client->used) {
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
