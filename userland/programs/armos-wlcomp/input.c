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

#define WL_WINDOW_CLOSE_X      14
#define WL_WINDOW_MINIMIZE_X   30
#define WL_WINDOW_MAXIMIZE_X   46
#define WL_WINDOW_CLOSE_Y      14
#define WL_WINDOW_BUTTON_HIT   7
#define WL_WINDOW_MINIMIZED_WIDTH 220u

#define WL_XKB_MOD_SHIFT   (1u << 0)
#define WL_XKB_MOD_LOCK    (1u << 1)
#define WL_XKB_MOD_CONTROL (1u << 2)
#define WL_XKB_MOD_ALT     (1u << 3)
#define WL_XKB_MOD_LOGO    (1u << 5)
#define WL_XKB_MOD_LEVEL3  (1u << 6)

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

static void wl_damage_surface_group(struct wl_server *server,
                                    struct wl_server_client *client,
                                    struct wl_server_surface *root);

static void wl_damage_resize_outline(struct wl_server *server,
                                     int32_t x, int32_t y,
                                     uint32_t width, uint32_t height)
{
    const uint32_t thickness = 3u;
    uint32_t frame_height = height + WL_WINDOW_TITLE_HEIGHT;

    wl_renderer_damage_rect(
        server, x - (int32_t)thickness, y - (int32_t)thickness,
        width + thickness * 2u, thickness * 2u);
    wl_renderer_damage_rect(
        server, x - (int32_t)thickness,
        y + (int32_t)frame_height - (int32_t)thickness,
        width + thickness * 2u, thickness * 2u);
    wl_renderer_damage_rect(
        server, x - (int32_t)thickness, y,
        thickness * 2u, frame_height);
    wl_renderer_damage_rect(
        server, x + (int32_t)width - (int32_t)thickness, y,
        thickness * 2u, frame_height);
}

static uint32_t wl_surface_frame_width(
    const struct wl_server_surface *surface)
{
    if (surface && surface->minimized &&
        surface->width > WL_WINDOW_MINIMIZED_WIDTH)
        return WL_WINDOW_MINIMIZED_WIDTH;
    return surface ? surface->width : 0u;
}

static int wl_surface_button_at(const struct wl_server_surface *surface,
                                int32_t x, int32_t y, int32_t center_x)
{
    int32_t local_x;
    int32_t local_y;

    if (!surface)
        return 0;
    local_x = x - surface->x;
    local_y = y - surface->y;
    return local_x >= center_x - WL_WINDOW_BUTTON_HIT &&
           local_x <= center_x + WL_WINDOW_BUTTON_HIT &&
           local_y >= WL_WINDOW_CLOSE_Y - WL_WINDOW_BUTTON_HIT &&
           local_y <= WL_WINDOW_CLOSE_Y + WL_WINDOW_BUTTON_HIT;
}

static uint32_t wl_surface_resize_edges_at(
    const struct wl_server_surface *surface, int32_t x, int32_t y)
{
    const int32_t border = 6;
    int32_t local_x;
    int32_t local_y;
    int32_t frame_height;
    uint32_t edges = 0u;

    if (!wl_surface_has_server_decoration(surface) || surface->maximized ||
        surface->minimized)
        return 0u;
    local_x = x - surface->x;
    local_y = y - surface->y;
    frame_height = (int32_t)surface->height + WL_WINDOW_TITLE_HEIGHT;
    if (local_y < border)
        edges |= 1u;
    else if (local_y >= frame_height - border)
        edges |= 2u;
    if (local_x < border)
        edges |= 4u;
    else if (local_x >= (int32_t)wl_surface_frame_width(surface) - border)
        edges |= 8u;
    return edges;
}

int wl_server_set_surface_minimized(struct wl_server *server,
                                    struct wl_server_client *client,
                                    struct wl_server_surface *surface,
                                    bool minimized)
{
    uint32_t columns;
    uint32_t slot = 0u;

    if (!server || !client || !surface ||
        surface->minimized == minimized)
        return 0;
    wl_damage_surface_group(server, client, surface);
    if (minimized) {
        surface->minimize_restore_x = surface->x;
        surface->minimize_restore_y = surface->y;
        surface->minimized = true;
        surface->shaded = true;
        for (size_t client_index = 0u;
             client_index < WL_SERVER_MAX_CLIENTS; client_index++) {
            struct wl_server_client *candidate =
                &server->clients[client_index];

            if (!candidate->used)
                continue;
            for (size_t surface_index = 0u;
                 surface_index < WL_SERVER_MAX_SURFACES; surface_index++) {
                struct wl_server_surface *other =
                    &candidate->surfaces[surface_index];

                if (other != surface && other->used && other->minimized)
                    slot++;
            }
        }
        columns = server->renderer.framebuffer.width /
            (WL_WINDOW_MINIMIZED_WIDTH + 12u);
        if (columns == 0u)
            columns = 1u;
        surface->x = 12 + (int32_t)((slot % columns) *
            (WL_WINDOW_MINIMIZED_WIDTH + 12u));
        surface->y = (int32_t)server->renderer.framebuffer.height -
            WL_WINDOW_TITLE_HEIGHT - 12 -
            (int32_t)((slot / columns) *
                      (WL_WINDOW_TITLE_HEIGHT + 8u));
    } else {
        surface->minimized = false;
        surface->shaded = false;
        surface->x = surface->minimize_restore_x;
        surface->y = surface->minimize_restore_y;
    }
    wl_damage_surface_group(server, client, surface);
    return wl_server_schedule_render(server, false);
}

static int wl_toggle_maximize(struct wl_server *server,
                              struct wl_server_client *client,
                              struct wl_server_surface *surface)
{
    if (surface->minimized &&
        wl_server_set_surface_minimized(server, client, surface, false) < 0)
        return -1;
    wl_damage_surface_group(server, client, surface);
    surface->shaded = false;
    if (!surface->maximized) {
        surface->restore_x = surface->x;
        surface->restore_y = surface->y;
        surface->restore_width = surface->width;
        surface->restore_height = surface->height;
        surface->maximized = true;
        surface->x = 0;
        surface->y = (int32_t)server->panel_height;
        return wl_server_configure_toplevel(
            server, client, surface,
            server->renderer.framebuffer.width,
            server->renderer.framebuffer.height -
                server->panel_height - WL_WINDOW_TITLE_HEIGHT,
            1u);
    }
    surface->maximized = false;
    surface->x = surface->restore_x;
    surface->y = surface->restore_y;
    return wl_server_configure_toplevel(
        server, client, surface,
        surface->restore_width, surface->restore_height, 0u);
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

static struct wl_server_surface *wl_surface_root(
    struct wl_server_surface *surface)
{
    size_t depth = 0u;

    while (surface && wl_surface_is_child_role(surface) &&
           surface->parent &&
           depth++ < WL_SERVER_MAX_SURFACES)
        surface = surface->parent;
    return depth <= WL_SERVER_MAX_SURFACES ? surface : NULL;
}

static void wl_raise_surface_group(struct wl_server *server,
                                   struct wl_server_client *client,
                                   struct wl_server_surface *root)
{
    if (!server || !client || !root)
        return;
    wl_damage_surface_group(server, client, root);
    root->z_order =
        root->role == WL_SERVER_SURFACE_ROLE_PANEL ?
        UINT64_MAX : ++server->next_surface_z;
    for (size_t index = 0u; index < WL_SERVER_MAX_SURFACES; index++) {
        struct wl_server_surface *surface = &client->surfaces[index];

        if (surface->used && surface != root &&
            wl_surface_root(surface) == root)
            surface->z_order = ++server->next_surface_z;
    }
}

static int wl_surface_origin(const struct wl_server_surface *surface,
                             int32_t *x, int32_t *y)
{
    const struct wl_server_surface *ancestor = surface;
    size_t depth = 0u;

    if (!surface || !x || !y)
        return -1;
    if (!wl_surface_is_child_role(surface)) {
        *x = surface->x;
        *y = surface->y +
            (wl_surface_has_server_decoration(surface) ?
             WL_WINDOW_TITLE_HEIGHT : 0);
        return 0;
    }
    *x = surface->subsurface_x;
    *y = surface->subsurface_y;
    while (wl_surface_is_child_role(ancestor) &&
           ancestor->parent &&
           depth++ < WL_SERVER_MAX_SURFACES) {
        ancestor = ancestor->parent;
        if (wl_surface_is_child_role(ancestor)) {
            *x += ancestor->subsurface_x;
            *y += ancestor->subsurface_y;
        } else {
            *x += ancestor->x;
            *y += ancestor->y +
                (wl_surface_has_server_decoration(ancestor) ?
                 WL_WINDOW_TITLE_HEIGHT : 0);
        }
    }
    return depth <= WL_SERVER_MAX_SURFACES ? 0 : -1;
}

static void wl_damage_surface_group(struct wl_server *server,
                                    struct wl_server_client *client,
                                    struct wl_server_surface *root)
{
    for (size_t index = 0u; index < WL_SERVER_MAX_SURFACES; index++) {
        struct wl_server_surface *surface = &client->surfaces[index];
        int32_t x;
        int32_t y;

        if (!surface->used || !surface->mapped ||
            wl_surface_root(surface) != root ||
            wl_surface_origin(surface, &x, &y) < 0)
            continue;
        if (!wl_surface_is_child_role(surface)) {
            x = surface->x;
            y = surface->y;
        }
        wl_renderer_damage_surface_at(server, surface, x, y);
    }
}

static struct wl_server_surface *wl_surface_at(
    struct wl_server *server, int32_t x, int32_t y,
    size_t *owner_index)
{
    struct wl_server_surface *top = NULL;
    size_t top_owner_index = WL_SERVER_MAX_CLIENTS;

    for (size_t ci = 0u; ci < WL_SERVER_MAX_CLIENTS; ci++) {
        struct wl_server_client *client = &server->clients[ci];

        if (!client->used)
            continue;
        for (size_t si = 0u; si < WL_SERVER_MAX_SURFACES; si++) {
            struct wl_server_surface *surface = &client->surfaces[si];
            int32_t surface_x;
            int32_t surface_y;

            if (!surface->used || !surface->mapped ||
                surface->role == WL_SERVER_SURFACE_ROLE_CURSOR ||
                wl_surface_origin(surface, &surface_x, &surface_y) < 0)
                continue;
            if (!wl_surface_is_child_role(surface) &&
                wl_surface_has_server_decoration(surface))
                surface_y -= WL_WINDOW_TITLE_HEIGHT;
            if (x >= surface_x &&
                x < surface_x +
                    (int32_t)wl_surface_frame_width(surface) &&
                y >= surface_y &&
                y < surface_y + (int32_t)surface->height +
                    ((!wl_surface_is_child_role(surface) &&
                      wl_surface_has_server_decoration(surface)) ?
                     WL_WINDOW_TITLE_HEIGHT : 0) &&
                (!surface->shaded ||
                 y < surface_y + WL_WINDOW_TITLE_HEIGHT)) {
                if (!top || surface->z_order > top->z_order) {
                    top = surface;
                    top_owner_index = ci;
                }
            }
        }
    }
    *owner_index = top_owner_index;
    return top;
}

static enum wl_server_pointer_cursor wl_resize_cursor(uint32_t edges)
{
    if ((edges & (1u | 2u)) != 0u &&
        (edges & (4u | 8u)) != 0u) {
        if ((edges & (1u | 4u)) == (1u | 4u) ||
            (edges & (2u | 8u)) == (2u | 8u))
            return WL_SERVER_POINTER_CURSOR_RESIZE_NWSE;
        return WL_SERVER_POINTER_CURSOR_RESIZE_NESW;
    }
    if ((edges & (4u | 8u)) != 0u)
        return WL_SERVER_POINTER_CURSOR_RESIZE_EW;
    if ((edges & (1u | 2u)) != 0u)
        return WL_SERVER_POINTER_CURSOR_RESIZE_NS;
    return WL_SERVER_POINTER_CURSOR_ARROW;
}

static void wl_update_pointer_cursor(struct wl_server *server)
{
    struct wl_server_surface *surface;
    struct wl_server_surface *root;
    size_t owner_index;
    uint32_t edges = 0u;

    if (server->resize_surface && server->resize_surface->used) {
        edges = server->resize_edges;
    } else if (server->drag_surface && server->drag_surface->used) {
        edges = 0u;
    } else {
        surface = wl_surface_at(
            server, server->pointer_x, server->pointer_y, &owner_index);
        (void)owner_index;
        root = wl_surface_root(surface);
        if (root)
            edges = wl_surface_resize_edges_at(
                root, server->pointer_x, server->pointer_y);
    }
    server->pointer_cursor = wl_resize_cursor(edges);
}

static void wl_send_pointer_frame(struct wl_server_client *client,
                                  struct wl_server_object *pointer)
{
    if (client && pointer && pointer->version >= 5u)
        (void)wl_client_send_words(client, pointer->id, 5u, NULL, 0u);
}

static void wl_send_pointer_motion(struct wl_server *server,
                                   uint32_t timestamp)
{
    struct wl_server_client *client = NULL;
    struct wl_server_surface *surface;
    struct wl_server_object *pointer;
    struct wl_server_object *old_pointer;
    size_t owner_index;
    int32_t surface_x;
    int32_t surface_y;
    uint32_t words[4];

    surface = wl_surface_at(server, server->pointer_x, server->pointer_y,
                            &owner_index);
    if (!surface || owner_index >= WL_SERVER_MAX_CLIENTS) {
        if (server->pointer_client && server->pointer_surface) {
            old_pointer = wl_find_input_object(
                server->pointer_client, WL_SERVER_OBJECT_POINTER);
            if (old_pointer) {
                words[0] = ++server->serial;
                words[1] = server->pointer_surface->object_id;
                (void)wl_client_send_words(server->pointer_client,
                                           old_pointer->id, 1u, words, 2u);
                wl_send_pointer_frame(server->pointer_client, old_pointer);
            }
        }
        server->pointer_client = NULL;
        server->pointer_surface = NULL;
        return;
    }
    client = &server->clients[owner_index];
    if (!client->used)
        return;
    pointer = wl_find_input_object(client, WL_SERVER_OBJECT_POINTER);
    if (!pointer ||
        wl_surface_origin(surface, &surface_x, &surface_y) < 0)
        return;
    if (server->pointer_client != client ||
        server->pointer_surface != surface) {
        if (server->pointer_client && server->pointer_surface) {
            old_pointer = wl_find_input_object(
                server->pointer_client, WL_SERVER_OBJECT_POINTER);
            if (old_pointer) {
                words[0] = ++server->serial;
                words[1] = server->pointer_surface->object_id;
                (void)wl_client_send_words(server->pointer_client,
                                           old_pointer->id, 1u, words, 2u);
                wl_send_pointer_frame(server->pointer_client, old_pointer);
            }
        }
        server->pointer_client = client;
        server->pointer_surface = surface;
        words[0] = ++server->serial;
        words[1] = surface->object_id;
        words[2] = (uint32_t)((server->pointer_x - surface_x) * 256);
        words[3] = (uint32_t)((server->pointer_y - surface_y) * 256);
        (void)wl_client_send_words(client, pointer->id, 0u, words, 4u);
        wl_send_pointer_frame(client, pointer);
    }
    words[0] = timestamp;
    words[1] = (uint32_t)((server->pointer_x - surface_x) * 256);
    words[2] = (uint32_t)((server->pointer_y - surface_y) * 256);
    (void)wl_client_send_words(client, pointer->id, 2u, words, 3u);
    wl_send_pointer_frame(client, pointer);
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
    struct wl_server_object *keyboard;
    struct wl_server_object *old_keyboard;
    uint32_t keyboard_enter[3];
    uint32_t leave[2];

    if (server->focus_client == client && server->focus_surface == surface)
        return;
    if (server->focus_client && server->focus_client->used &&
        server->focus_surface && server->focus_surface->used) {
        struct wl_server_surface *old_root =
            wl_surface_root(server->focus_surface);

        if (old_root)
            wl_damage_surface_group(
                server, server->focus_client, old_root);
        old_keyboard = wl_find_input_object(server->focus_client,
                                            WL_SERVER_OBJECT_KEYBOARD);
        leave[1] = server->focus_surface->object_id;
        if (old_keyboard) {
            leave[0] = ++server->serial;
            (void)wl_client_send_words(server->focus_client,
                                       old_keyboard->id, 2u, leave, 2u);
        }
    }
    server->focus_client = client;
    server->focus_surface = surface;
    if (!client || !surface)
        return;
    {
        struct wl_server_surface *new_root = wl_surface_root(surface);

        if (new_root)
            wl_damage_surface_group(server, client, new_root);
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
    struct wl_server_surface *root = NULL;
    struct wl_server_object *pointer;
    size_t owner_index;
    uint32_t words[4];
    bool pressed = event->value != 0;

    if (event->code != ARMOS_INPUT_BUTTON_LEFT &&
        event->code != ARMOS_INPUT_BUTTON_RIGHT &&
        event->code != ARMOS_INPUT_BUTTON_MIDDLE)
        return;
    if (event->code != ARMOS_INPUT_BUTTON_LEFT) {
        /*
         * Window-management gestures are bound to the primary button.
         * Other buttons are still ordinary Wayland pointer events and must
         * reach every client through the same seat contract.
         */
        wl_send_pointer_motion(server, event->timestamp_ms);
        client = server->pointer_client;
        surface = server->pointer_surface;
        if (!client || !surface)
            return;
        pointer = wl_find_input_object(client, WL_SERVER_OBJECT_POINTER);
        if (!pointer)
            return;
        words[0] = ++server->serial;
        words[1] = event->timestamp_ms;
        words[2] = event->code;
        words[3] = pressed ? 1u : 0u;
        (void)wl_client_send_words(client, pointer->id, 3u, words, 4u);
        wl_send_pointer_frame(client, pointer);
        return;
    }
    server->pointer_left = pressed;
    if (pressed) {
        /*
         * Synchronize the protocol pointer focus before delivering a button.
         * A host click used only to capture the QEMU pointer may otherwise
         * have no Wayland target at all.
         */
        wl_send_pointer_motion(server, event->timestamp_ms);
        surface = wl_surface_at(server, server->pointer_x, server->pointer_y,
                                &owner_index);
        if (!surface || owner_index >= WL_SERVER_MAX_CLIENTS) {
            wl_focus_surface(server, NULL, NULL);
        } else {
            bool close_button;
            bool minimize_button;
            bool maximize_button;
            uint32_t resize_edges;

            client = &server->clients[owner_index];
            if (!client->used)
                return;
            root = wl_surface_root(surface);
            if (!root)
                return;
            wl_raise_surface_group(server, client, root);
            wl_focus_surface(server, client, root);
            close_button = wl_surface_button_at(
                root, server->pointer_x, server->pointer_y,
                WL_WINDOW_CLOSE_X);
            minimize_button = wl_surface_button_at(
                root, server->pointer_x, server->pointer_y,
                WL_WINDOW_MINIMIZE_X);
            maximize_button = wl_surface_button_at(
                root, server->pointer_x, server->pointer_y,
                WL_WINDOW_MAXIMIZE_X);
            resize_edges = wl_surface_resize_edges_at(
                root, server->pointer_x, server->pointer_y);
            if (surface == root &&
                wl_surface_has_server_decoration(root) &&
                close_button) {
                (void)wl_send_toplevel_close(client, root);
                return;
            } else if (surface == root &&
                       wl_surface_has_server_decoration(root) &&
                       minimize_button) {
                (void)wl_server_set_surface_minimized(
                    server, client, root, !root->minimized);
                return;
            } else if (surface == root &&
                       wl_surface_has_server_decoration(root) &&
                       maximize_button) {
                (void)wl_toggle_maximize(server, client, root);
                return;
            } else if (surface == root && resize_edges != 0u) {
                server->resize_client = client;
                server->resize_surface = root;
                server->resize_edges = resize_edges;
                server->resize_pointer_x = server->pointer_x;
                server->resize_pointer_y = server->pointer_y;
                server->resize_x = root->x;
                server->resize_y = root->y;
                server->resize_width = root->width;
                server->resize_height = root->height;
                server->resize_initial_width = root->width;
                server->resize_initial_height = root->height;
                root->resize_from_left = (resize_edges & 4u) != 0u;
                root->resize_from_top = (resize_edges & 1u) != 0u;
                root->resize_anchor_right =
                    root->x + (int32_t)root->width;
                root->resize_anchor_bottom =
                    root->y + WL_WINDOW_TITLE_HEIGHT +
                    (int32_t)root->height;
                wl_damage_resize_outline(
                    server, server->resize_x, server->resize_y,
                    server->resize_width, server->resize_height);
                wl_update_pointer_cursor(server);
                return;
            } else if (surface == root &&
                       wl_surface_has_server_decoration(root) &&
                       server->pointer_y <
                       root->y + WL_WINDOW_TITLE_HEIGHT) {
                server->drag_client = client;
                server->drag_surface = root;
                server->drag_offset_x = server->pointer_x - root->x;
                server->drag_offset_y = server->pointer_y - root->y;
            }
        }
    } else {
        server->drag_client = NULL;
        server->drag_surface = NULL;
        if (server->resize_client && server->resize_surface) {
            wl_damage_resize_outline(
                server, server->resize_x, server->resize_y,
                server->resize_width, server->resize_height);
            (void)wl_server_configure_toplevel(
                server, server->resize_client, server->resize_surface,
                server->resize_width, server->resize_height, 0u);
        }
        server->resize_client = NULL;
        server->resize_surface = NULL;
        server->resize_edges = 0u;
        wl_update_pointer_cursor(server);
        if (server->pointer_grab_serial == 0u)
            return;
        client = server->pointer_client;
        surface = server->pointer_surface;
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
    wl_send_pointer_frame(client, pointer);
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

    if (event->code >= ARMOS_INPUT_BUTTON_LEFT || event->value == 2)
        return;
    previous_depressed = server->modifiers_depressed;
    previous_locked = server->modifiers_locked;
    if (event->code == ARMOS_INPUT_KEY_LEFTSHIFT ||
        event->code == ARMOS_INPUT_KEY_RIGHTSHIFT)
        mask = WL_XKB_MOD_SHIFT;
    else if (event->code == ARMOS_INPUT_KEY_LEFTCTRL ||
             event->code == ARMOS_INPUT_KEY_RIGHTCTRL)
        mask = WL_XKB_MOD_CONTROL;
    else if (event->code == ARMOS_INPUT_KEY_LEFTALT)
        mask = server->keyboard_layout == ARMOS_KEYBOARD_LAYOUT_FR_MAC ?
            WL_XKB_MOD_LEVEL3 : WL_XKB_MOD_ALT;
    else if (event->code == ARMOS_INPUT_KEY_RIGHTALT)
        mask = (server->keyboard_layout == ARMOS_KEYBOARD_LAYOUT_FR ||
                server->keyboard_layout ==
                    ARMOS_KEYBOARD_LAYOUT_FR_LEGACY ||
                server->keyboard_layout == ARMOS_KEYBOARD_LAYOUT_FR_MAC) ?
            WL_XKB_MOD_LEVEL3 : WL_XKB_MOD_ALT;
    else if (event->code == ARMOS_INPUT_KEY_LEFTMETA ||
             event->code == ARMOS_INPUT_KEY_RIGHTMETA)
        mask = WL_XKB_MOD_LOGO;
    if (mask != 0u) {
        if (event->value != 0)
            server->modifiers_depressed |= mask;
        else
            server->modifiers_depressed &= ~mask;
    } else if (event->code == ARMOS_INPUT_KEY_CAPSLOCK &&
               event->value == 1) {
        if (server->keyboard_layout ==
            ARMOS_KEYBOARD_LAYOUT_FR_LEGACY)
            server->modifiers_locked ^= WL_XKB_MOD_SHIFT;
        else
            server->modifiers_locked ^= WL_XKB_MOD_LOCK;
    }
    modifiers_changed =
        server->modifiers_depressed != previous_depressed ||
        server->modifiers_locked != previous_locked;
    if (event->code == ARMOS_INPUT_KEY_C && event->value == 1 &&
        (server->modifiers_depressed & WL_XKB_MOD_CONTROL) != 0u &&
        (!server->focus_client || !server->focus_surface)) {
        server->exit_requested = true;
        return;
    }
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
    if (event->type == ARMOS_INPUT_EVENT_CONFIG) {
        if (event->code == ARMOS_INPUT_CONFIG_KEYBOARD_LAYOUT &&
            event->value >= 0 &&
            (uint32_t)event->value < ARMOS_KEYBOARD_LAYOUT_COUNT &&
            server->keyboard_layout != (uint32_t)event->value) {
            server->keyboard_layout = (uint32_t)event->value;
            (void)wl_server_broadcast_keymap(server);
        }
    } else if (event->type == ARMOS_INPUT_EVENT_RELATIVE ||
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
        wl_update_pointer_cursor(server);
        if ((server->pointer_x != previous_pointer_x ||
             server->pointer_y != previous_pointer_y) &&
            server->drag_surface && server->drag_client &&
            server->drag_surface->used && server->drag_client->used) {
            wl_renderer_damage_surface_at(
                server, server->drag_surface,
                server->drag_surface->x, server->drag_surface->y);
            server->drag_surface->x =
                server->pointer_x - server->drag_offset_x;
            server->drag_surface->y =
                server->pointer_y - server->drag_offset_y;
            wl_renderer_damage_surface_at(
                server, server->drag_surface,
                server->drag_surface->x, server->drag_surface->y);
        } else if ((server->pointer_x != previous_pointer_x ||
                    server->pointer_y != previous_pointer_y) &&
                   server->resize_surface && server->resize_client &&
                   server->resize_surface->used &&
                   server->resize_client->used) {
            struct wl_server_surface *surface = server->resize_surface;
            int32_t delta_x =
                server->pointer_x - server->resize_pointer_x;
            int32_t delta_y =
                server->pointer_y - server->resize_pointer_y;
            int64_t width = server->resize_initial_width;
            int64_t height = server->resize_initial_height;
            uint32_t minimum_width =
                surface->minimum_width ? surface->minimum_width : 160u;
            uint32_t minimum_height =
                surface->minimum_height ? surface->minimum_height : 100u;
            uint32_t maximum_width =
                surface->maximum_width ? surface->maximum_width :
                server->renderer.framebuffer.width;
            uint32_t maximum_height =
                surface->maximum_height ? surface->maximum_height :
                server->renderer.framebuffer.height -
                    WL_WINDOW_TITLE_HEIGHT;

            if ((server->resize_edges & 4u) != 0u)
                width -= delta_x;
            else if ((server->resize_edges & 8u) != 0u)
                width += delta_x;
            if ((server->resize_edges & 1u) != 0u)
                height -= delta_y;
            else if ((server->resize_edges & 2u) != 0u)
                height += delta_y;
            if (width < (int64_t)minimum_width)
                width = minimum_width;
            if (height < (int64_t)minimum_height)
                height = minimum_height;
            if (width > (int64_t)maximum_width)
                width = maximum_width;
            if (height > (int64_t)maximum_height)
                height = maximum_height;
            wl_damage_resize_outline(
                server, server->resize_x, server->resize_y,
                server->resize_width, server->resize_height);
            server->resize_width = (uint32_t)width;
            server->resize_height = (uint32_t)height;
            server->resize_x =
                (server->resize_edges & 4u) != 0u ?
                surface->resize_anchor_right - (int32_t)width :
                surface->x;
            server->resize_y =
                (server->resize_edges & 1u) != 0u ?
                surface->resize_anchor_bottom -
                    WL_WINDOW_TITLE_HEIGHT - (int32_t)height :
                surface->y;
            surface->resize_from_left =
                (server->resize_edges & 4u) != 0u;
            surface->resize_from_top =
                (server->resize_edges & 1u) != 0u;
            wl_damage_resize_outline(
                server, server->resize_x, server->resize_y,
                server->resize_width, server->resize_height);
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
