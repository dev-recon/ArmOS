/*
 * ArmOS
 * Copyright (c) 2026 Mohamed Ennassiri
 *
 * Licensed under the Apache License, Version 2.0.
 * See LICENSE for details.
 *
 * File: userland/programs/armos-wlcomp/protocol.c
 * Layer: Userland / graphical services
 *
 * Responsibilities:
 * - Dispatch the Wayland core and stable xdg-shell protocols used by ArmOS.
 * - Manage registries, SHM pools, buffers, surfaces and frame callbacks.
 * - Reject malformed requests before they can affect compositor state.
 *
 * Notes:
 * - wl_seat exposes the common ArmOS pointer and keyboard event stream.
 * - Damage and region requests are accepted but full-surface redraw is used.
 */

#include "armos_wlcomp.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#define WL_PROTOCOL_ERROR_INVALID_OBJECT 0u
#define WL_PROTOCOL_ERROR_INVALID_METHOD 1u
#define WL_PROTOCOL_ERROR_IMPLEMENTATION 3u

struct wl_request {
    const uint8_t *data;
    size_t size;
    size_t cursor;
};

static int wl_request_u32(struct wl_request *request, uint32_t *value)
{
    if (!request || !value || request->cursor + 4u > request->size)
        return -1;
    *value = wl_wire_u32(request->data + request->cursor);
    request->cursor += 4u;
    return 0;
}

static int wl_request_string(struct wl_request *request, const char **text,
                             uint32_t *length)
{
    uint32_t size;
    uint32_t padded;

    if (wl_request_u32(request, &size) < 0 || size == 0)
        return -1;
    padded = wl_wire_align(size);
    if (request->cursor + padded > request->size ||
        request->data[request->cursor + size - 1u] != '\0')
        return -1;
    *text = (const char *)(request->data + request->cursor);
    if (length)
        *length = size;
    request->cursor += padded;
    return 0;
}

static int wl_request_nullable_string(struct wl_request *request,
                                      const char **text)
{
    uint32_t size;

    if (!request || !text || request->cursor + 4u > request->size)
        return -1;
    size = wl_wire_u32(request->data + request->cursor);
    if (size == 0u) {
        request->cursor += 4u;
        *text = NULL;
        return 0;
    }
    return wl_request_string(request, text, NULL);
}

static int wl_protocol_fail(struct wl_server_client *client,
                            uint32_t object_id, uint32_t code,
                            const char *message)
{
    fprintf(stderr,
            "armos-wlcomp: protocol error object=%u code=%u: %s\n",
            object_id, code, message ? message : "unknown error");
    (void)wl_client_send_error(client, object_id, code, message);
    return -1;
}

static int wl_request_complete(const struct wl_request *request)
{
    return request && request->cursor == request->size;
}

static bool wl_surface_has_role_object(
    const struct wl_server_client *client,
    const struct wl_server_surface *surface,
    enum wl_server_object_type type)
{
    if (!client || !surface)
        return false;
    for (size_t index = 0u; index < WL_SERVER_MAX_OBJECTS; index++) {
        const struct wl_server_object *object = &client->objects[index];

        if (object->type == type && object->resource == surface)
            return true;
    }
    return false;
}

static struct wl_server_pool *wl_allocate_pool(
    struct wl_server_client *client)
{
    for (size_t index = 0; index < WL_SERVER_MAX_POOLS; index++) {
        if (!client->pools[index].used) {
            memset(&client->pools[index], 0, sizeof(client->pools[index]));
            client->pools[index].used = true;
            client->pools[index].fd = -1;
            return &client->pools[index];
        }
    }
    return NULL;
}

static struct wl_server_buffer *wl_allocate_buffer(
    struct wl_server_client *client)
{
    for (size_t index = 0; index < WL_SERVER_MAX_BUFFERS; index++) {
        if (!client->buffers[index].used) {
            memset(&client->buffers[index], 0,
                   sizeof(client->buffers[index]));
            client->buffers[index].used = true;
            return &client->buffers[index];
        }
    }
    return NULL;
}

static struct wl_server_surface *wl_allocate_surface(
    struct wl_server *server, struct wl_server_client *client)
{
    uint32_t width = server->renderer.framebuffer.width;
    uint32_t height = server->renderer.framebuffer.height;

    for (size_t index = 0; index < WL_SERVER_MAX_SURFACES; index++) {
        if (!client->surfaces[index].used) {
            struct wl_server_surface *surface = &client->surfaces[index];
            uint32_t position = server->next_surface_position++;

            memset(surface, 0, sizeof(*surface));
            surface->used = true;
            surface->server_decorated = true;
            surface->z_order = ++server->next_surface_z;
            surface->x = (int32_t)(24u + (position * 36u) %
                                   (width > 300u ? width - 300u : 1u));
            surface->y = (int32_t)(24u + (position * 28u) %
                                   (height > 220u ? height - 220u : 1u));
            return surface;
        }
    }
    return NULL;
}

static int wl_dispatch_display(struct wl_server *server,
                               struct wl_server_client *client,
                               uint16_t opcode, struct wl_request *request)
{
    uint32_t new_id;

    if (wl_request_u32(request, &new_id) < 0 ||
        !wl_request_complete(request))
        return wl_protocol_fail(client, WL_DISPLAY_ID,
                                WL_PROTOCOL_ERROR_INVALID_METHOD,
                                "malformed wl_display request");
    if (opcode == 0u) {
        uint32_t done;

        if (wl_client_add_object(client, new_id, WL_SERVER_OBJECT_CALLBACK,
                                 1u, NULL) < 0)
            return wl_protocol_fail(client, WL_DISPLAY_ID,
                                    WL_PROTOCOL_ERROR_INVALID_OBJECT,
                                    "invalid callback object id");
        done = ++server->serial;
        if (wl_client_send_words(client, new_id, 0, &done, 1) < 0)
            return -1;
        wl_client_remove_object(client, new_id, true);
        return 0;
    }
    if (opcode == 1u) {
        if (wl_client_add_object(client, new_id, WL_SERVER_OBJECT_REGISTRY,
                                 1u, NULL) < 0)
            return wl_protocol_fail(client, WL_DISPLAY_ID,
                                    WL_PROTOCOL_ERROR_INVALID_OBJECT,
                                    "invalid registry object id");
        if (wl_client_send_global(client, new_id, WL_GLOBAL_COMPOSITOR,
                                  "wl_compositor", 4u) < 0 ||
            wl_client_send_global(client, new_id, WL_GLOBAL_SHM,
                                  "wl_shm", 1u) < 0 ||
            wl_client_send_global(client, new_id, WL_GLOBAL_SEAT,
                                  "wl_seat", 5u) < 0 ||
            wl_client_send_global(client, new_id, WL_GLOBAL_XDG_SHELL,
                                  "xdg_wm_base", 1u) < 0)
            return -1;
        if (wl_client_send_global(client, new_id, WL_GLOBAL_OUTPUT,
                                  "wl_output", 4u) < 0)
            return -1;
        if (wl_client_send_global(client, new_id, WL_GLOBAL_XDG_OUTPUT,
                                  "zxdg_output_manager_v1", 3u) < 0)
            return -1;
        if (wl_client_send_global(
                client, new_id, WL_GLOBAL_XDG_DECORATION,
                "zxdg_decoration_manager_v1", 1u) < 0)
            return -1;
        if (wl_client_send_global(client, new_id, WL_GLOBAL_DATA_DEVICE,
                                  "wl_data_device_manager", 3u) < 0)
            return -1;
        if (wl_client_send_global(client, new_id, WL_GLOBAL_SUBCOMPOSITOR,
                                  "wl_subcompositor", 1u) < 0)
            return -1;
        return 0;
    }
    return wl_protocol_fail(client, WL_DISPLAY_ID,
                            WL_PROTOCOL_ERROR_INVALID_METHOD,
                            "unsupported wl_display request");
}

static int wl_dispatch_registry(struct wl_server *server,
                                struct wl_server_client *client,
                                struct wl_server_object *object,
                                uint16_t opcode, struct wl_request *request)
{
    uint32_t name;
    uint32_t requested_version;
    uint32_t new_id;
    const char *interface_name;

    if (opcode != 0u ||
        wl_request_u32(request, &name) < 0 ||
        wl_request_string(request, &interface_name, NULL) < 0 ||
        wl_request_u32(request, &requested_version) < 0 ||
        wl_request_u32(request, &new_id) < 0 ||
        !wl_request_complete(request) || requested_version == 0u)
        return wl_protocol_fail(client, object->id,
                                WL_PROTOCOL_ERROR_INVALID_METHOD,
                                "malformed wl_registry.bind");

    if (name == WL_GLOBAL_COMPOSITOR &&
        strcmp(interface_name, "wl_compositor") == 0) {
        uint32_t version = requested_version < 4u ?
            requested_version : 4u;

        return wl_client_add_object(client, new_id,
                                    WL_SERVER_OBJECT_COMPOSITOR,
                                    version, NULL);
    }
    if (name == WL_GLOBAL_SHM && strcmp(interface_name, "wl_shm") == 0) {
        uint32_t format;

        if (wl_client_add_object(client, new_id, WL_SERVER_OBJECT_SHM,
                                 1u, NULL) < 0)
            return -1;
        format = WL_SHM_FORMAT_ARGB8888;
        if (wl_client_send_words(client, new_id, 0, &format, 1) < 0)
            return -1;
        format = WL_SHM_FORMAT_XRGB8888;
        return wl_client_send_words(client, new_id, 0, &format, 1);
    }
    if (name == WL_GLOBAL_SEAT && strcmp(interface_name, "wl_seat") == 0) {
        uint32_t capabilities = 3u;
        uint32_t version = requested_version < 5u ?
            requested_version : 5u;

        if (wl_client_add_object(client, new_id, WL_SERVER_OBJECT_SEAT,
                                 version, NULL) < 0)
            return -1;
        return wl_client_send_words(client, new_id, 0, &capabilities, 1);
    }
    if (name == WL_GLOBAL_XDG_SHELL &&
        strcmp(interface_name, "xdg_wm_base") == 0) {
        return wl_client_add_object(client, new_id,
                                    WL_SERVER_OBJECT_XDG_WM_BASE, 1u, NULL);
    }
    if (name == WL_GLOBAL_OUTPUT && strcmp(interface_name, "wl_output") == 0)
        return wl_server_bind_output(server, client, new_id,
                                     requested_version);
    if (name == WL_GLOBAL_XDG_OUTPUT &&
        strcmp(interface_name, "zxdg_output_manager_v1") == 0) {
        uint32_t version = requested_version < 3u ?
            requested_version : 3u;

        return wl_client_add_object(
            client, new_id, WL_SERVER_OBJECT_XDG_OUTPUT_MANAGER,
            version, NULL);
    }
    if (name == WL_GLOBAL_XDG_DECORATION &&
        strcmp(interface_name, "zxdg_decoration_manager_v1") == 0) {
        return wl_client_add_object(
            client, new_id, WL_SERVER_OBJECT_XDG_DECORATION_MANAGER,
            1u, NULL);
    }
    if (name == WL_GLOBAL_DATA_DEVICE &&
        strcmp(interface_name, "wl_data_device_manager") == 0) {
        uint32_t version = requested_version < 3u ?
            requested_version : 3u;

        return wl_client_add_object(
            client, new_id, WL_SERVER_OBJECT_DATA_DEVICE_MANAGER,
            version, NULL);
    }
    if (name == WL_GLOBAL_SUBCOMPOSITOR &&
        strcmp(interface_name, "wl_subcompositor") == 0) {
        return wl_client_add_object(client, new_id,
                                    WL_SERVER_OBJECT_SUBCOMPOSITOR,
                                    1u, NULL);
    }
    return wl_protocol_fail(client, object->id,
                            WL_PROTOCOL_ERROR_INVALID_OBJECT,
                            "unknown registry global");
}

static int wl_dispatch_xdg_wm_base(struct wl_server *server,
                                   struct wl_server_client *client,
                                   struct wl_server_object *object,
                                   uint16_t opcode,
                                   struct wl_request *request)
{
    uint32_t new_id;
    uint32_t surface_id;
    struct wl_server_object *surface_object;

    (void)server;
    if (opcode == 0u && wl_request_complete(request)) {
        wl_client_remove_object(client, object->id, true);
        return 0;
    }
    if (opcode == 2u &&
        wl_request_u32(request, &new_id) == 0 &&
        wl_request_u32(request, &surface_id) == 0 &&
        wl_request_complete(request)) {
        surface_object = wl_client_find_object(client, surface_id);
        if (!surface_object ||
            surface_object->type != WL_SERVER_OBJECT_SURFACE)
            return wl_protocol_fail(client, object->id,
                                    WL_PROTOCOL_ERROR_INVALID_OBJECT,
                                    "xdg_wm_base needs a wl_surface");
        return wl_client_add_object(client, new_id,
                                    WL_SERVER_OBJECT_XDG_SURFACE, 1u,
                                    surface_object->resource);
    }
    if (opcode == 3u && request->size == 4u) {
        request->cursor = request->size;
        return 0;
    }
    return wl_protocol_fail(client, object->id,
                            WL_PROTOCOL_ERROR_INVALID_METHOD,
                            "unsupported xdg_wm_base request");
}

static int wl_dispatch_xdg_surface(struct wl_server *server,
                                   struct wl_server_client *client,
                                   struct wl_server_object *object,
                                   uint16_t opcode,
                                   struct wl_request *request)
{
    struct wl_server_surface *surface = object->resource;

    if (!surface || !surface->used)
        return wl_protocol_fail(client, object->id,
                                WL_PROTOCOL_ERROR_INVALID_OBJECT,
                                "xdg_surface lost its wl_surface");
    if (opcode == 0u && wl_request_complete(request)) {
        wl_client_remove_object(client, object->id, true);
        return 0;
    }
    if (opcode == 1u) {
        uint32_t new_id;
        uint32_t configure[3] = {0u, 0u, 0u};
        uint32_t serial;
        bool assigned_role;

        if (wl_request_u32(request, &new_id) < 0 ||
            !wl_request_complete(request))
            return wl_protocol_fail(client, object->id,
                                    WL_PROTOCOL_ERROR_INVALID_OBJECT,
                                    "invalid xdg_toplevel object");
        if ((surface->role != WL_SERVER_SURFACE_ROLE_NONE &&
             surface->role != WL_SERVER_SURFACE_ROLE_TOPLEVEL) ||
            wl_surface_has_role_object(
                client, surface, WL_SERVER_OBJECT_XDG_TOPLEVEL))
            return wl_protocol_fail(client, object->id,
                                    WL_PROTOCOL_ERROR_INVALID_OBJECT,
                                    "wl_surface already has a role");
        assigned_role =
            surface->role == WL_SERVER_SURFACE_ROLE_NONE;
        surface->role = WL_SERVER_SURFACE_ROLE_TOPLEVEL;
        surface->server_decorated = true;
        if (wl_client_add_object(client, new_id,
                                 WL_SERVER_OBJECT_XDG_TOPLEVEL, 1u,
                                 surface) < 0) {
            if (assigned_role)
                surface->role = WL_SERVER_SURFACE_ROLE_NONE;
            surface->server_decorated = false;
            return wl_protocol_fail(client, object->id,
                                    WL_PROTOCOL_ERROR_INVALID_OBJECT,
                                    "invalid xdg_toplevel object");
        }
        if (wl_client_send_words(client, new_id, 0u, configure, 3u) < 0)
            return -1;
        serial = ++server->serial;
        return wl_client_send_words(client, object->id, 0u, &serial, 1u);
    }
    if (opcode == 3u && request->size == 16u) {
        request->cursor = request->size;
        return 0;
    }
    if (opcode == 4u && request->size == 4u) {
        request->cursor = request->size;
        return 0;
    }
    return wl_protocol_fail(client, object->id,
                            WL_PROTOCOL_ERROR_INVALID_METHOD,
                            "unsupported xdg_surface request");
}

static int wl_dispatch_xdg_toplevel(struct wl_server *server,
                                    struct wl_server_client *client,
                                    struct wl_server_object *object,
                                    uint16_t opcode,
                                    struct wl_request *request)
{
    struct wl_server_surface *surface = object->resource;

    if (opcode == 0u && wl_request_complete(request)) {
        wl_client_remove_object(client, object->id, true);
        return 0;
    }
    if (opcode == 2u || opcode == 3u) {
        const char *text;
        uint32_t length;

        if (wl_request_string(request, &text, &length) == 0 &&
            wl_request_complete(request)) {
            if (opcode == 2u && surface && surface->used) {
                size_t copy_length = length - 1u;

                if (copy_length >= sizeof(surface->title))
                    copy_length = sizeof(surface->title) - 1u;
                memcpy(surface->title, text, copy_length);
                surface->title[copy_length] = '\0';
                if (surface->mapped) {
                    wl_renderer_damage_surface_at(
                        server, surface, surface->x, surface->y);
                    if (wl_server_schedule_render(server, false) < 0)
                        return -1;
                }
            }
            return 0;
        }
    }
    if (opcode == 5u) {
        uint32_t seat_id;
        uint32_t serial;
        struct wl_server_object *seat;

        if (wl_request_u32(request, &seat_id) < 0 ||
            wl_request_u32(request, &serial) < 0 ||
            !wl_request_complete(request))
            return wl_protocol_fail(client, object->id,
                                    WL_PROTOCOL_ERROR_INVALID_METHOD,
                                    "malformed xdg_toplevel.move");
        seat = wl_client_find_object(client, seat_id);
        if (!seat || seat->type != WL_SERVER_OBJECT_SEAT || !surface ||
            !surface->used || !server->pointer_left ||
            server->pointer_grab_serial != serial ||
            server->focus_client != client ||
            server->focus_surface != surface)
            return wl_protocol_fail(client, object->id,
                                    WL_PROTOCOL_ERROR_INVALID_METHOD,
                                    "invalid xdg_toplevel.move grab");
        server->drag_client = client;
        server->drag_surface = surface;
        server->drag_offset_x = server->pointer_x - surface->x;
        server->drag_offset_y = server->pointer_y - surface->y;
        return 0;
    }
    if ((opcode == 7u || opcode == 8u) && request->size == 8u) {
        request->cursor = request->size;
        return 0;
    }
    if ((opcode == 9u || opcode == 11u) && request->size == 4u) {
        request->cursor = request->size;
        return 0;
    }
    if ((opcode == 10u || opcode == 12u || opcode == 13u) &&
        wl_request_complete(request))
        return 0;
    return wl_protocol_fail(client, object->id,
                            WL_PROTOCOL_ERROR_INVALID_METHOD,
                            "unsupported xdg_toplevel request");
}

static int wl_dispatch_xdg_decoration_manager(
    struct wl_server_client *client, struct wl_server_object *object,
    uint16_t opcode, struct wl_request *request)
{
    uint32_t new_id;
    uint32_t toplevel_id;
    struct wl_server_object *toplevel;

    if (opcode == 0u && wl_request_complete(request)) {
        wl_client_remove_object(client, object->id, true);
        return 0;
    }
    if (opcode != 1u ||
        wl_request_u32(request, &new_id) < 0 ||
        wl_request_u32(request, &toplevel_id) < 0 ||
        !wl_request_complete(request))
        return wl_protocol_fail(client, object->id,
                                WL_PROTOCOL_ERROR_INVALID_METHOD,
                                "malformed xdg-decoration request");
    toplevel = wl_client_find_object(client, toplevel_id);
    if (!toplevel || toplevel->type != WL_SERVER_OBJECT_XDG_TOPLEVEL ||
        !toplevel->resource)
        return wl_protocol_fail(client, object->id,
                                WL_PROTOCOL_ERROR_INVALID_OBJECT,
                                "decoration requires an xdg_toplevel");
    for (size_t index = 0u; index < WL_SERVER_MAX_OBJECTS; index++) {
        if (client->objects[index].type ==
                WL_SERVER_OBJECT_XDG_TOPLEVEL_DECORATION &&
            client->objects[index].resource == toplevel->resource)
            return wl_protocol_fail(client, object->id,
                                    WL_PROTOCOL_ERROR_INVALID_OBJECT,
                                    "toplevel already has a decoration");
    }
    return wl_client_add_object(
        client, new_id, WL_SERVER_OBJECT_XDG_TOPLEVEL_DECORATION,
        1u, toplevel->resource);
}

static int wl_dispatch_xdg_toplevel_decoration(
    struct wl_server *server, struct wl_server_client *client,
    struct wl_server_object *object, uint16_t opcode,
    struct wl_request *request)
{
    struct wl_server_surface *surface = object->resource;
    uint32_t mode = 2u;

    if (opcode == 0u && wl_request_complete(request)) {
        if (surface && surface->used) {
            if (surface->mapped)
                wl_renderer_damage_surface_at(
                    server, surface, surface->x, surface->y);
            surface->server_decorated = true;
            if (surface->mapped) {
                wl_renderer_damage_surface_at(
                    server, surface, surface->x, surface->y);
                if (wl_server_schedule_render(server, false) < 0)
                    return -1;
            }
        }
        wl_client_remove_object(client, object->id, true);
        return 0;
    }
    if (!surface || !surface->used)
        return wl_protocol_fail(client, object->id,
                                WL_PROTOCOL_ERROR_INVALID_OBJECT,
                                "decoration lost its surface");
    if (opcode == 1u) {
        if (wl_request_u32(request, &mode) < 0 ||
            !wl_request_complete(request) ||
            (mode != 1u && mode != 2u))
            return wl_protocol_fail(client, object->id,
                                    WL_PROTOCOL_ERROR_INVALID_METHOD,
                                    "invalid decoration mode");
    } else if (opcode == 2u && wl_request_complete(request)) {
        mode = 2u;
    } else {
        return wl_protocol_fail(client, object->id,
                                WL_PROTOCOL_ERROR_INVALID_METHOD,
                                "unsupported decoration request");
    }
    if (surface->mapped)
        wl_renderer_damage_surface_at(
            server, surface, surface->x, surface->y);
    surface->server_decorated = mode == 2u;
    if (surface->mapped)
        wl_renderer_damage_surface_at(
            server, surface, surface->x, surface->y);
    if (wl_client_send_words(client, object->id, 0u, &mode, 1u) < 0)
        return -1;
    return surface->mapped ? wl_server_schedule_render(server, false) : 0;
}

static int wl_dispatch_compositor(struct wl_server *server,
                                  struct wl_server_client *client,
                                  struct wl_server_object *object,
                                  uint16_t opcode,
                                  struct wl_request *request)
{
    uint32_t new_id;

    if (wl_request_u32(request, &new_id) < 0 ||
        !wl_request_complete(request))
        return wl_protocol_fail(client, object->id,
                                WL_PROTOCOL_ERROR_INVALID_METHOD,
                                "malformed wl_compositor request");
    if (opcode == 0u) {
        struct wl_server_surface *surface =
            wl_allocate_surface(server, client);

        if (!surface)
            return wl_protocol_fail(client, object->id,
                                    WL_PROTOCOL_ERROR_IMPLEMENTATION,
                                    "surface limit reached");
        surface->object_id = new_id;
        if (wl_client_add_object(client, new_id, WL_SERVER_OBJECT_SURFACE,
                                 object->version, surface) < 0) {
            memset(surface, 0, sizeof(*surface));
            return wl_protocol_fail(client, object->id,
                                    WL_PROTOCOL_ERROR_INVALID_OBJECT,
                                    "invalid surface object id");
        }
        return 0;
    }
    if (opcode == 1u) {
        if (wl_client_add_object(client, new_id, WL_SERVER_OBJECT_REGION,
                                 1u, NULL) < 0)
            return wl_protocol_fail(client, object->id,
                                    WL_PROTOCOL_ERROR_INVALID_OBJECT,
                                    "invalid region object id");
        return 0;
    }
    return wl_protocol_fail(client, object->id,
                            WL_PROTOCOL_ERROR_INVALID_METHOD,
                            "unsupported wl_compositor request");
}

static int wl_dispatch_subcompositor(
    struct wl_server_client *client, struct wl_server_object *object,
    uint16_t opcode, struct wl_request *request)
{
    uint32_t new_id;
    uint32_t surface_id;
    uint32_t parent_id;
    struct wl_server_object *surface_object;
    struct wl_server_object *parent_object;
    struct wl_server_surface *surface;
    struct wl_server_surface *parent;
    struct wl_server_surface *ancestor;
    size_t depth;
    bool assigned_role;

    if (opcode == 0u && wl_request_complete(request)) {
        wl_client_remove_object(client, object->id, true);
        return 0;
    }
    if (opcode != 1u ||
        wl_request_u32(request, &new_id) < 0 ||
        wl_request_u32(request, &surface_id) < 0 ||
        wl_request_u32(request, &parent_id) < 0 ||
        !wl_request_complete(request) || surface_id == parent_id)
        return wl_protocol_fail(client, object->id,
                                WL_PROTOCOL_ERROR_INVALID_METHOD,
                                "malformed wl_subcompositor request");
    surface_object = wl_client_find_object(client, surface_id);
    parent_object = wl_client_find_object(client, parent_id);
    if (!surface_object || !parent_object ||
        surface_object->type != WL_SERVER_OBJECT_SURFACE ||
        parent_object->type != WL_SERVER_OBJECT_SURFACE)
        return wl_protocol_fail(client, object->id,
                                WL_PROTOCOL_ERROR_INVALID_OBJECT,
                                "subsurface needs two wl_surfaces");
    surface = surface_object->resource;
    parent = parent_object->resource;
    if (!surface || !parent ||
        (surface->role != WL_SERVER_SURFACE_ROLE_NONE &&
         surface->role != WL_SERVER_SURFACE_ROLE_SUBSURFACE) ||
        wl_surface_has_role_object(
            client, surface, WL_SERVER_OBJECT_SUBSURFACE) ||
        parent->role == WL_SERVER_SURFACE_ROLE_CURSOR)
        return wl_protocol_fail(client, object->id,
                                WL_PROTOCOL_ERROR_INVALID_OBJECT,
                                "invalid wl_subsurface roles");
    ancestor = parent;
    for (depth = 0u; ancestor && depth < WL_SERVER_MAX_SURFACES; depth++) {
        if (ancestor == surface)
            return wl_protocol_fail(client, object->id,
                                    WL_PROTOCOL_ERROR_INVALID_OBJECT,
                                    "cyclic subsurface hierarchy");
        ancestor = ancestor->parent;
    }
    if (ancestor)
        return wl_protocol_fail(client, object->id,
                                WL_PROTOCOL_ERROR_IMPLEMENTATION,
                                "subsurface hierarchy is too deep");
    assigned_role =
        surface->role == WL_SERVER_SURFACE_ROLE_NONE;
    surface->role = WL_SERVER_SURFACE_ROLE_SUBSURFACE;
    surface->subsurface_synchronized = true;
    surface->parent = parent;
    surface->subsurface_x = 0;
    surface->subsurface_y = 0;
    if (wl_client_add_object(client, new_id, WL_SERVER_OBJECT_SUBSURFACE,
                             1u, surface) < 0) {
        if (assigned_role)
            surface->role = WL_SERVER_SURFACE_ROLE_NONE;
        surface->subsurface_synchronized = false;
        surface->parent = NULL;
        return wl_protocol_fail(client, object->id,
                                WL_PROTOCOL_ERROR_INVALID_OBJECT,
                                "invalid wl_subsurface object id");
    }
    return 0;
}

static int wl_dispatch_subsurface(
    struct wl_server *server, struct wl_server_client *client,
    struct wl_server_object *object, uint16_t opcode,
    struct wl_request *request)
{
    struct wl_server_surface *surface = object->resource;

    /*
     * The wl_subsurface object outlives either wl_surface.  In particular,
     * clients are allowed to destroy the parent first and release the role
     * object afterwards.  Accept destroy without dereferencing the former
     * surface hierarchy.
     */
    if (opcode == 0u && wl_request_complete(request)) {
        if (surface && surface->used) {
            surface->subsurface_synchronized = false;
            surface->parent = NULL;
            surface->mapped = false;
        }
        wl_client_remove_object(client, object->id, true);
        return wl_renderer_compose(server);
    }
    if (!surface || !surface->used ||
        surface->role != WL_SERVER_SURFACE_ROLE_SUBSURFACE ||
        !surface->parent || !surface->parent->used)
        return wl_protocol_fail(client, object->id,
                                WL_PROTOCOL_ERROR_INVALID_OBJECT,
                                "wl_subsurface lost its surface");
    if (opcode == 1u && request->size == 8u) {
        uint32_t x;
        uint32_t y;

        if (wl_request_u32(request, &x) < 0 ||
            wl_request_u32(request, &y) < 0)
            return -1;
        surface->subsurface_x = (int32_t)x;
        surface->subsurface_y = (int32_t)y;
        return 0;
    }
    if ((opcode == 2u || opcode == 3u) && request->size == 4u) {
        uint32_t sibling_id;
        struct wl_server_object *sibling;

        if (wl_request_u32(request, &sibling_id) < 0)
            return -1;
        sibling = wl_client_find_object(client, sibling_id);
        if (!sibling || sibling->type != WL_SERVER_OBJECT_SURFACE)
            return wl_protocol_fail(client, object->id,
                                    WL_PROTOCOL_ERROR_INVALID_OBJECT,
                                    "unknown subsurface sibling");
        return 0;
    }
    if ((opcode == 4u || opcode == 5u) &&
        wl_request_complete(request)) {
        surface->subsurface_synchronized = opcode == 4u;
        return 0;
    }
    return wl_protocol_fail(client, object->id,
                            WL_PROTOCOL_ERROR_INVALID_METHOD,
                            "unsupported wl_subsurface request");
}

static int wl_dispatch_region(struct wl_server_client *client,
                              struct wl_server_object *object,
                              uint16_t opcode, struct wl_request *request)
{
    if (opcode == 0u && wl_request_complete(request)) {
        wl_client_remove_object(client, object->id, true);
        return 0;
    }
    if ((opcode == 1u || opcode == 2u) && request->size == 16u) {
        request->cursor = request->size;
        return 0;
    }
    return wl_protocol_fail(client, object->id,
                            WL_PROTOCOL_ERROR_INVALID_METHOD,
                            "malformed wl_region request");
}

static int wl_dispatch_shm(struct wl_server_client *client,
                           struct wl_server_object *object,
                           uint16_t opcode, struct wl_request *request)
{
    struct wl_server_pool *pool;
    uint32_t new_id;
    uint32_t size;
    int fd;

    if (opcode != 0u || wl_request_u32(request, &new_id) < 0 ||
        wl_request_u32(request, &size) < 0 ||
        !wl_request_complete(request) || size == 0u)
        return wl_protocol_fail(client, object->id,
                                WL_PROTOCOL_ERROR_INVALID_METHOD,
                                "malformed wl_shm.create_pool");
    fd = wl_client_take_fd(client);
    if (fd < 0)
        return wl_protocol_fail(client, object->id,
                                WL_PROTOCOL_ERROR_INVALID_METHOD,
                                "wl_shm.create_pool needs one fd");
    pool = wl_allocate_pool(client);
    if (!pool) {
        close(fd);
        return wl_protocol_fail(client, object->id,
                                WL_PROTOCOL_ERROR_IMPLEMENTATION,
                                "SHM pool limit reached");
    }
    pool->mapping = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED,
                         fd, 0);
    if (pool->mapping == MAP_FAILED) {
        close(fd);
        memset(pool, 0, sizeof(*pool));
        return wl_protocol_fail(client, object->id,
                                WL_PROTOCOL_ERROR_IMPLEMENTATION,
                                "cannot map SHM pool");
    }
    pool->object_id = new_id;
    pool->object_alive = true;
    pool->fd = fd;
    pool->size = size;
    if (wl_client_add_object(client, new_id, WL_SERVER_OBJECT_SHM_POOL,
                             1u, pool) < 0) {
        munmap(pool->mapping, pool->size);
        close(pool->fd);
        memset(pool, 0, sizeof(*pool));
        return wl_protocol_fail(client, object->id,
                                WL_PROTOCOL_ERROR_INVALID_OBJECT,
                                "invalid pool object id");
    }
    return 0;
}

static int wl_pool_create_buffer(struct wl_server_client *client,
                                 struct wl_server_object *object,
                                 struct wl_server_pool *pool,
                                 struct wl_request *request)
{
    struct wl_server_buffer *buffer;
    uint32_t new_id;
    uint32_t offset;
    uint32_t width;
    uint32_t height;
    uint32_t stride;
    uint32_t format;
    uint64_t end;

    if (wl_request_u32(request, &new_id) < 0 ||
        wl_request_u32(request, &offset) < 0 ||
        wl_request_u32(request, &width) < 0 ||
        wl_request_u32(request, &height) < 0 ||
        wl_request_u32(request, &stride) < 0 ||
        wl_request_u32(request, &format) < 0 ||
        !wl_request_complete(request) || width == 0u || height == 0u ||
        stride < width * 4u ||
        (format != WL_SHM_FORMAT_ARGB8888 &&
         format != WL_SHM_FORMAT_XRGB8888))
        return wl_protocol_fail(client, object->id,
                                WL_PROTOCOL_ERROR_INVALID_METHOD,
                                "invalid wl_shm buffer");
    end = (uint64_t)offset + (uint64_t)(height - 1u) * stride +
          (uint64_t)width * 4u;
    if (end > pool->size)
        return wl_protocol_fail(client, object->id,
                                WL_PROTOCOL_ERROR_INVALID_METHOD,
                                "wl_shm buffer exceeds pool");
    buffer = wl_allocate_buffer(client);
    if (!buffer)
        return wl_protocol_fail(client, object->id,
                                WL_PROTOCOL_ERROR_IMPLEMENTATION,
                                "buffer limit reached");
    buffer->object_id = new_id;
    buffer->object_alive = true;
    buffer->pool = pool;
    buffer->offset = offset;
    buffer->width = width;
    buffer->height = height;
    buffer->stride = stride;
    buffer->format = format;
    if (wl_client_add_object(client, new_id, WL_SERVER_OBJECT_BUFFER,
                             1u, buffer) < 0) {
        memset(buffer, 0, sizeof(*buffer));
        return wl_protocol_fail(client, object->id,
                                WL_PROTOCOL_ERROR_INVALID_OBJECT,
                                "invalid buffer object id");
    }
    return 0;
}

static int wl_dispatch_pool(struct wl_server_client *client,
                            struct wl_server_object *object,
                            uint16_t opcode, struct wl_request *request)
{
    struct wl_server_pool *pool = object->resource;

    if (!pool || !pool->used)
        return -1;
    if (opcode == 0u)
        return wl_pool_create_buffer(client, object, pool, request);
    if (opcode == 1u && wl_request_complete(request)) {
        pool->object_alive = false;
        wl_client_remove_object(client, object->id, true);
        return 0;
    }
    if (opcode == 2u) {
        uint32_t size;
        uint8_t *mapping;

        if (wl_request_u32(request, &size) < 0 ||
            !wl_request_complete(request) || size <= pool->size)
            return wl_protocol_fail(client, object->id,
                                    WL_PROTOCOL_ERROR_INVALID_METHOD,
                                    "invalid wl_shm_pool.resize");
        mapping = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED,
                       pool->fd, 0);
        if (mapping == MAP_FAILED)
            return wl_protocol_fail(client, object->id,
                                    WL_PROTOCOL_ERROR_IMPLEMENTATION,
                                    "cannot resize SHM pool");
        munmap(pool->mapping, pool->size);
        pool->mapping = mapping;
        pool->size = size;
        return 0;
    }
    return wl_protocol_fail(client, object->id,
                            WL_PROTOCOL_ERROR_INVALID_METHOD,
                            "unsupported wl_shm_pool request");
}

static int wl_dispatch_buffer(struct wl_server_client *client,
                              struct wl_server_object *object,
                              uint16_t opcode, struct wl_request *request)
{
    struct wl_server_buffer *buffer = object->resource;

    if (opcode != 0u || !wl_request_complete(request) || !buffer)
        return wl_protocol_fail(client, object->id,
                                WL_PROTOCOL_ERROR_INVALID_METHOD,
                                "malformed wl_buffer.destroy");
    buffer->object_alive = false;
    wl_client_remove_object(client, object->id, true);
    return 0;
}

static int wl_surface_add_callback(struct wl_server_client *client,
                                   struct wl_server_object *object,
                                   struct wl_server_surface *surface,
                                   struct wl_request *request)
{
    uint32_t new_id;

    if (wl_request_u32(request, &new_id) < 0 ||
        !wl_request_complete(request))
        return wl_protocol_fail(client, object->id,
                                WL_PROTOCOL_ERROR_INVALID_METHOD,
                                "malformed wl_surface.frame");
    for (size_t index = 0; index < WL_SERVER_MAX_CALLBACKS; index++) {
        struct wl_server_callback *callback = &surface->callbacks[index];

        if (callback->used)
            continue;
        callback->used = true;
        callback->object_id = new_id;
        if (wl_client_add_object(client, new_id, WL_SERVER_OBJECT_CALLBACK,
                                 1u, callback) < 0) {
            memset(callback, 0, sizeof(*callback));
            return wl_protocol_fail(client, object->id,
                                    WL_PROTOCOL_ERROR_INVALID_OBJECT,
                                    "invalid frame callback id");
        }
        return 0;
    }
    return wl_protocol_fail(client, object->id,
                            WL_PROTOCOL_ERROR_IMPLEMENTATION,
                            "frame callback limit reached");
}

static int wl_surface_add_damage(struct wl_server_client *client,
                                 struct wl_server_object *object,
                                 struct wl_server_surface *surface,
                                 struct wl_request *request)
{
    uint32_t wire_x;
    uint32_t wire_y;
    uint32_t wire_width;
    uint32_t wire_height;
    int32_t x;
    int32_t y;
    int32_t width;
    int32_t height;
    int64_t x1;
    int64_t y1;
    struct wl_renderer_rect incoming;

    if (wl_request_u32(request, &wire_x) < 0 ||
        wl_request_u32(request, &wire_y) < 0 ||
        wl_request_u32(request, &wire_width) < 0 ||
        wl_request_u32(request, &wire_height) < 0 ||
        !wl_request_complete(request))
        return wl_protocol_fail(client, object->id,
                                WL_PROTOCOL_ERROR_INVALID_METHOD,
                                "malformed wl_surface damage");
    x = (int32_t)wire_x;
    y = (int32_t)wire_y;
    width = (int32_t)wire_width;
    height = (int32_t)wire_height;
    if (width < 0 || height < 0)
        return wl_protocol_fail(client, object->id,
                                WL_PROTOCOL_ERROR_INVALID_METHOD,
                                "negative wl_surface damage");
    if (width == 0 || height == 0)
        return 0;
    x1 = (int64_t)x + width;
    y1 = (int64_t)y + height;
    if (x1 > INT32_MAX)
        x1 = INT32_MAX;
    if (y1 > INT32_MAX)
        y1 = INT32_MAX;
    incoming.x0 = x;
    incoming.y0 = y;
    incoming.x1 = (int32_t)x1;
    incoming.y1 = (int32_t)y1;
    for (size_t index = 0u;
         index < surface->pending_damage_count; index++) {
        struct wl_renderer_rect *damage = &surface->pending_damage[index];

        if (incoming.x0 > damage->x1 || incoming.x1 < damage->x0 ||
            incoming.y0 > damage->y1 || incoming.y1 < damage->y0)
            continue;
        if (incoming.x0 < damage->x0)
            damage->x0 = incoming.x0;
        if (incoming.y0 < damage->y0)
            damage->y0 = incoming.y0;
        if (incoming.x1 > damage->x1)
            damage->x1 = incoming.x1;
        if (incoming.y1 > damage->y1)
            damage->y1 = incoming.y1;
        return 0;
    }
    if (surface->pending_damage_count < WL_SERVER_MAX_DAMAGE_RECTS) {
        surface->pending_damage[surface->pending_damage_count++] = incoming;
        return 0;
    }
    {
        size_t best = 0u;
        uint64_t best_growth = UINT64_MAX;

        for (size_t index = 0u;
             index < surface->pending_damage_count; index++) {
            const struct wl_renderer_rect *damage =
                &surface->pending_damage[index];
            int32_t union_x0 = damage->x0 < incoming.x0 ?
                damage->x0 : incoming.x0;
            int32_t union_y0 = damage->y0 < incoming.y0 ?
                damage->y0 : incoming.y0;
            int32_t union_x1 = damage->x1 > incoming.x1 ?
                damage->x1 : incoming.x1;
            int32_t union_y1 = damage->y1 > incoming.y1 ?
                damage->y1 : incoming.y1;
            uint64_t old_area =
                (uint64_t)(uint32_t)(damage->x1 - damage->x0) *
                (uint32_t)(damage->y1 - damage->y0);
            uint64_t union_area =
                (uint64_t)(uint32_t)(union_x1 - union_x0) *
                (uint32_t)(union_y1 - union_y0);
            uint64_t growth = union_area - old_area;

            if (growth < best_growth) {
                best = index;
                best_growth = growth;
            }
        }
        if (incoming.x0 < surface->pending_damage[best].x0)
            surface->pending_damage[best].x0 = incoming.x0;
        if (incoming.y0 < surface->pending_damage[best].y0)
            surface->pending_damage[best].y0 = incoming.y0;
        if (incoming.x1 > surface->pending_damage[best].x1)
            surface->pending_damage[best].x1 = incoming.x1;
        if (incoming.y1 > surface->pending_damage[best].y1)
            surface->pending_damage[best].y1 = incoming.y1;
    }
    return 0;
}

static int wl_dispatch_surface(struct wl_server *server,
                               struct wl_server_client *client,
                               struct wl_server_object *object,
                               uint16_t opcode, struct wl_request *request)
{
    struct wl_server_surface *surface = object->resource;

    if (!surface)
        return -1;
    if (opcode == 0u && wl_request_complete(request)) {
        if (server->focus_client == client &&
            server->focus_surface == surface) {
            server->focus_client = NULL;
            server->focus_surface = NULL;
        }
        if (server->pointer_client == client &&
            server->pointer_surface == surface) {
            server->pointer_client = NULL;
            server->pointer_surface = NULL;
        }
        if (server->drag_client == client &&
            server->drag_surface == surface) {
            server->drag_client = NULL;
            server->drag_surface = NULL;
        }
        for (size_t index = 0u; index < WL_SERVER_MAX_SURFACES; index++) {
            struct wl_server_surface *child = &client->surfaces[index];

            if (child->used && child->parent == surface) {
                child->parent = NULL;
                child->subsurface_synchronized = false;
                child->mapped = false;
            }
        }
        for (size_t index = 0u; index < WL_SERVER_MAX_OBJECTS; index++) {
            struct wl_server_object *role = &client->objects[index];

            if ((role->type == WL_SERVER_OBJECT_SUBSURFACE ||
                 role->type ==
                    WL_SERVER_OBJECT_XDG_TOPLEVEL_DECORATION) &&
                role->resource == surface)
                role->resource = NULL;
        }
        free(surface->pixels);
        memset(surface, 0, sizeof(*surface));
        wl_client_remove_object(client, object->id, true);
        return wl_renderer_compose(server);
    }
    if (opcode == 1u) {
        uint32_t buffer_id;
        uint32_t x;
        uint32_t y;
        struct wl_server_object *buffer_object = NULL;

        if (wl_request_u32(request, &buffer_id) < 0 ||
            wl_request_u32(request, &x) < 0 ||
            wl_request_u32(request, &y) < 0 ||
            !wl_request_complete(request))
            return wl_protocol_fail(client, object->id,
                                    WL_PROTOCOL_ERROR_INVALID_METHOD,
                                    "malformed wl_surface.attach");
        (void)x;
        (void)y;
        if (buffer_id != 0u) {
            buffer_object = wl_client_find_object(client, buffer_id);
            if (!buffer_object ||
                buffer_object->type != WL_SERVER_OBJECT_BUFFER)
                return wl_protocol_fail(client, object->id,
                                        WL_PROTOCOL_ERROR_INVALID_OBJECT,
                                        "unknown wl_buffer");
        }
        surface->pending_buffer =
            buffer_object ? buffer_object->resource : NULL;
        surface->pending_attach = true;
        return 0;
    }
    if (opcode == 2u)
        return wl_surface_add_damage(client, object, surface, request);
    if (opcode == 3u)
        return wl_surface_add_callback(client, object, surface, request);
    if ((opcode == 4u || opcode == 5u) && request->size == 4u) {
        uint32_t region_id;

        if (wl_request_u32(request, &region_id) < 0)
            return -1;
        if (region_id != 0u) {
            struct wl_server_object *region =
                wl_client_find_object(client, region_id);

            if (!region || region->type != WL_SERVER_OBJECT_REGION)
                return wl_protocol_fail(client, object->id,
                                        WL_PROTOCOL_ERROR_INVALID_OBJECT,
                                        "unknown wl_region");
        }
        return 0;
    }
    if (opcode == 6u && wl_request_complete(request)) {
        bool was_mapped = surface->mapped;

        if (wl_surface_commit(server, client, surface) < 0)
            return wl_protocol_fail(client, object->id,
                                    WL_PROTOCOL_ERROR_IMPLEMENTATION,
                                    "surface commit failed");
        if (!was_mapped && surface->mapped &&
            wl_server_surface_enter_output(client, object->id) < 0)
            return -1;
        return 0;
    }
    if (opcode == 7u && object->version >= 2u &&
        request->size == 4u) {
        uint32_t transform;

        if (wl_request_u32(request, &transform) < 0 ||
            transform != 0u)
            return wl_protocol_fail(client, object->id,
                                    WL_PROTOCOL_ERROR_INVALID_METHOD,
                                    "unsupported buffer transform");
        return 0;
    }
    if (opcode == 8u && object->version >= 3u &&
        request->size == 4u) {
        uint32_t scale;

        if (wl_request_u32(request, &scale) < 0 || scale == 0u)
            return wl_protocol_fail(client, object->id,
                                    WL_PROTOCOL_ERROR_INVALID_METHOD,
                                    "invalid buffer scale");
        return 0;
    }
    if (opcode == 9u && object->version >= 4u)
        return wl_surface_add_damage(client, object, surface, request);
    return wl_protocol_fail(client, object->id,
                            WL_PROTOCOL_ERROR_INVALID_METHOD,
                            "unsupported wl_surface request");
}

static int wl_dispatch_seat(struct wl_server *server,
                            struct wl_server_client *client,
                            struct wl_server_object *object,
                            uint16_t opcode, struct wl_request *request)
{
    uint32_t new_id;

    if (opcode == 3u && object->version >= 5u &&
        wl_request_complete(request)) {
        wl_client_remove_object(client, object->id, true);
        return 0;
    }
    if (opcode > 2u || wl_request_u32(request, &new_id) < 0 ||
        !wl_request_complete(request))
        return wl_protocol_fail(client, object->id,
                                WL_PROTOCOL_ERROR_INVALID_METHOD,
                                "malformed wl_seat request");
    if (opcode == 0u)
        return wl_client_add_object(client, new_id,
                                    WL_SERVER_OBJECT_POINTER,
                                    object->version, NULL);
    if (opcode == 1u) {
        if (wl_client_add_object(client, new_id,
                                 WL_SERVER_OBJECT_KEYBOARD,
                                 object->version, NULL) < 0)
            return -1;
        if (wl_server_send_keymap(server, client, new_id) < 0) {
            perror("armos-wlcomp: keyboard keymap");
            wl_client_remove_object(client, new_id, false);
            return -1;
        }
        if (object->version >= 4u) {
            uint32_t repeat[2] = { 25u, 600u };

            if (wl_client_send_words(client, new_id, 5u, repeat, 2u) < 0) {
                wl_client_remove_object(client, new_id, false);
                return -1;
            }
        }
        return 0;
    }
    if (opcode == 2u)
        return wl_client_add_object(client, new_id,
                                    WL_SERVER_OBJECT_TOUCH,
                                    object->version, NULL);
    return wl_protocol_fail(client, object->id,
                            WL_PROTOCOL_ERROR_INVALID_METHOD,
                            "requested seat capability is unavailable");
}

static int wl_dispatch_input_object(struct wl_server_client *client,
                                    struct wl_server_object *object,
                                    uint16_t opcode,
                                    struct wl_request *request)
{
    if (object->type == WL_SERVER_OBJECT_POINTER && opcode == 0u &&
        request->size == 16u) {
        uint32_t serial;
        uint32_t surface_id;
        uint32_t hotspot_x;
        uint32_t hotspot_y;

        if (wl_request_u32(request, &serial) < 0 ||
            wl_request_u32(request, &surface_id) < 0 ||
            wl_request_u32(request, &hotspot_x) < 0 ||
            wl_request_u32(request, &hotspot_y) < 0 ||
            !wl_request_complete(request))
            return wl_protocol_fail(client, object->id,
                                    WL_PROTOCOL_ERROR_INVALID_METHOD,
                                    "malformed pointer cursor request");
        if (surface_id != 0u) {
            struct wl_server_object *surface_object =
                wl_client_find_object(client, surface_id);
            struct wl_server_surface *surface;

            if (!surface_object ||
                surface_object->type != WL_SERVER_OBJECT_SURFACE ||
                !(surface = surface_object->resource) ||
                (surface->role != WL_SERVER_SURFACE_ROLE_NONE &&
                 surface->role != WL_SERVER_SURFACE_ROLE_CURSOR))
                return wl_protocol_fail(client, object->id,
                                        WL_PROTOCOL_ERROR_INVALID_OBJECT,
                                        "invalid pointer cursor surface");
            surface->role = WL_SERVER_SURFACE_ROLE_CURSOR;
            surface->server_decorated = false;
        }
        (void)serial;
        (void)hotspot_x;
        (void)hotspot_y;
        return 0;
    }
    if (object->type == WL_SERVER_OBJECT_POINTER && opcode == 1u &&
        object->version >= 3u && wl_request_complete(request)) {
        wl_client_remove_object(client, object->id, true);
        return 0;
    }
    if (object->type == WL_SERVER_OBJECT_KEYBOARD && opcode == 0u &&
        object->version >= 3u && wl_request_complete(request)) {
        wl_client_remove_object(client, object->id, true);
        return 0;
    }
    if (object->type == WL_SERVER_OBJECT_TOUCH && opcode == 0u &&
        object->version >= 3u && wl_request_complete(request)) {
        wl_client_remove_object(client, object->id, true);
        return 0;
    }
    return wl_protocol_fail(client, object->id,
                            WL_PROTOCOL_ERROR_INVALID_METHOD,
                            "malformed input object request");
}

static struct wl_server_data_source *wl_allocate_data_source(
    struct wl_server_client *client)
{
    for (size_t index = 0u; index < WL_SERVER_MAX_DATA_SOURCES; index++) {
        if (!client->data_sources[index].used) {
            memset(&client->data_sources[index], 0,
                   sizeof(client->data_sources[index]));
            client->data_sources[index].used = true;
            return &client->data_sources[index];
        }
    }
    return NULL;
}

static struct wl_server_data_offer *wl_allocate_data_offer(
    struct wl_server_client *client)
{
    for (size_t index = 0u; index < WL_SERVER_MAX_DATA_OFFERS; index++) {
        if (!client->data_offers[index].used) {
            memset(&client->data_offers[index], 0,
                   sizeof(client->data_offers[index]));
            client->data_offers[index].used = true;
            return &client->data_offers[index];
        }
    }
    return NULL;
}

static struct wl_server_object *wl_find_client_object_type(
    struct wl_server_client *client, enum wl_server_object_type type)
{
    for (size_t index = 0u; index < WL_SERVER_MAX_OBJECTS; index++) {
        if (client->objects[index].type == type)
            return &client->objects[index];
    }
    return NULL;
}

static int wl_publish_selection_to_client(
    struct wl_server *server, struct wl_server_client *target)
{
    struct wl_server_object *device = wl_find_client_object_type(
        target, WL_SERVER_OBJECT_DATA_DEVICE);
    struct wl_server_data_offer *offer;
    uint32_t offer_id;

    if (!device)
        return 0;
    if (!server->selection_client || !server->selection_source ||
        !server->selection_source->used) {
        offer_id = 0u;
        return wl_client_send_words(target, device->id, 5u, &offer_id, 1u);
    }
    offer = wl_allocate_data_offer(target);
    if (!offer)
        return -1;
    offer_id = target->next_server_id++;
    if (offer_id < 0xff000000u ||
        wl_client_find_object(target, offer_id)) {
        memset(offer, 0, sizeof(*offer));
        return -1;
    }
    offer->object_id = offer_id;
    offer->source_client = server->selection_client;
    offer->source = server->selection_source;
    if (wl_client_add_object(target, offer_id, WL_SERVER_OBJECT_DATA_OFFER,
                             device->version, offer) < 0) {
        memset(offer, 0, sizeof(*offer));
        return -1;
    }
    if (wl_client_send_words(target, device->id, 0u, &offer_id, 1u) < 0)
        return -1;
    for (size_t index = 0u; index < offer->source->mime_count; index++) {
        if (wl_client_send_string(target, offer_id, 0u,
                                  offer->source->mime_types[index]) < 0)
            return -1;
    }
    return wl_client_send_words(target, device->id, 5u, &offer_id, 1u);
}

static int wl_publish_selection(struct wl_server *server)
{
    for (size_t index = 0u; index < WL_SERVER_MAX_CLIENTS; index++) {
        if (server->clients[index].used &&
            wl_publish_selection_to_client(server,
                                           &server->clients[index]) < 0)
            return -1;
    }
    return 0;
}

void wl_server_drop_client_selection(struct wl_server *server,
                                     struct wl_server_client *client)
{
    if (!server || !client || server->selection_client != client)
        return;
    server->selection_client = NULL;
    server->selection_source = NULL;
    for (size_t index = 0u; index < WL_SERVER_MAX_CLIENTS; index++) {
        struct wl_server_client *other = &server->clients[index];

        if (other->used && other != client)
            (void)wl_publish_selection_to_client(server, other);
    }
}

static int wl_dispatch_data_device_manager(
    struct wl_server *server, struct wl_server_client *client,
    struct wl_server_object *object, uint16_t opcode,
    struct wl_request *request)
{
    uint32_t new_id;

    if (opcode == 0u && wl_request_u32(request, &new_id) == 0 &&
        wl_request_complete(request)) {
        struct wl_server_data_source *source =
            wl_allocate_data_source(client);

        if (!source)
            return -1;
        source->object_id = new_id;
        if (wl_client_add_object(client, new_id,
                                 WL_SERVER_OBJECT_DATA_SOURCE,
                                 object->version, source) < 0) {
            memset(source, 0, sizeof(*source));
            return -1;
        }
        return 0;
    }
    if (opcode == 1u) {
        uint32_t seat_id;
        struct wl_server_object *seat;

        if (wl_request_u32(request, &new_id) < 0 ||
            wl_request_u32(request, &seat_id) < 0 ||
            !wl_request_complete(request))
            return -1;
        seat = wl_client_find_object(client, seat_id);
        if (!seat || seat->type != WL_SERVER_OBJECT_SEAT)
            return -1;
        if (wl_client_add_object(client, new_id,
                                 WL_SERVER_OBJECT_DATA_DEVICE,
                                 object->version, NULL) < 0)
            return -1;
        return wl_publish_selection_to_client(server, client);
    }
    return -1;
}

static int wl_dispatch_data_source(
    struct wl_server *server, struct wl_server_client *client,
    struct wl_server_object *object, uint16_t opcode,
    struct wl_request *request)
{
    struct wl_server_data_source *source = object->resource;

    if (!source || !source->used)
        return -1;
    if (opcode == 0u) {
        const char *mime_type;
        uint32_t length;

        if (wl_request_string(request, &mime_type, &length) < 0 ||
            !wl_request_complete(request) ||
            source->mime_count >= WL_SERVER_MAX_MIME_TYPES ||
            length > WL_SERVER_MAX_MIME_LENGTH)
            return -1;
        memcpy(source->mime_types[source->mime_count++], mime_type,
               length);
        return 0;
    }
    if (opcode == 1u && wl_request_complete(request)) {
        if (server->selection_source == source) {
            server->selection_source = NULL;
            server->selection_client = NULL;
            (void)wl_publish_selection(server);
        }
        memset(source, 0, sizeof(*source));
        wl_client_remove_object(client, object->id, true);
        return 0;
    }
    if (opcode == 2u && object->version >= 3u &&
        request->size == 4u) {
        uint32_t actions;

        if (wl_request_u32(request, &actions) < 0 ||
            (actions & ~7u) != 0u)
            return -1;
        return 0;
    }
    return -1;
}

static int wl_data_offer_has_mime(const struct wl_server_data_offer *offer,
                                  const char *mime_type)
{
    if (!offer || !offer->source || !mime_type)
        return 0;
    for (size_t index = 0u; index < offer->source->mime_count; index++) {
        if (strcmp(offer->source->mime_types[index], mime_type) == 0)
            return 1;
    }
    return 0;
}

static int wl_dispatch_data_offer(
    struct wl_server_client *client, struct wl_server_object *object,
    uint16_t opcode, struct wl_request *request)
{
    struct wl_server_data_offer *offer = object->resource;

    if (!offer || !offer->used || !offer->source ||
        !offer->source_client || !offer->source_client->used)
        return -1;
    if (opcode == 0u) {
        uint32_t serial;
        const char *mime_type;

        if (wl_request_u32(request, &serial) < 0 ||
            wl_request_nullable_string(request, &mime_type) < 0 ||
            !wl_request_complete(request) ||
            (mime_type && !wl_data_offer_has_mime(offer, mime_type)))
            return -1;
        (void)serial;
        return 0;
    }
    if (opcode == 1u) {
        const char *mime_type;
        int fd;
        int result;

        if (wl_request_string(request, &mime_type, NULL) < 0 ||
            !wl_request_complete(request) ||
            !wl_data_offer_has_mime(offer, mime_type))
            return -1;
        fd = wl_client_take_fd(client);
        if (fd < 0)
            return -1;
        result = wl_client_send_fd_string(
            offer->source_client, offer->source->object_id, 1u,
            mime_type, fd);
        close(fd);
        return result;
    }
    if (opcode == 2u && wl_request_complete(request)) {
        memset(offer, 0, sizeof(*offer));
        wl_client_remove_object(client, object->id, false);
        return 0;
    }
    if (opcode == 3u && object->version >= 3u &&
        wl_request_complete(request)) {
        memset(offer, 0, sizeof(*offer));
        wl_client_remove_object(client, object->id, false);
        return 0;
    }
    if (opcode == 4u && object->version >= 3u &&
        request->size == 8u) {
        uint32_t actions;
        uint32_t preferred;

        if (wl_request_u32(request, &actions) < 0 ||
            wl_request_u32(request, &preferred) < 0 ||
            (actions & ~7u) != 0u ||
            (preferred != 0u && preferred != 1u &&
             preferred != 2u && preferred != 4u) ||
            (preferred & actions) != preferred)
            return -1;
        return 0;
    }
    return -1;
}

static int wl_dispatch_data_device(
    struct wl_server *server, struct wl_server_client *client,
    struct wl_server_object *object, uint16_t opcode,
    struct wl_request *request)
{
    if (opcode == 1u) {
        uint32_t source_id;
        uint32_t serial;
        struct wl_server_object *source_object = NULL;

        if (wl_request_u32(request, &source_id) < 0 ||
            wl_request_u32(request, &serial) < 0 ||
            !wl_request_complete(request))
            return -1;
        (void)serial;
        if (source_id != 0u) {
            source_object = wl_client_find_object(client, source_id);
            if (!source_object ||
                source_object->type != WL_SERVER_OBJECT_DATA_SOURCE)
                return -1;
        }
        if (server->selection_source &&
            server->selection_source !=
                (source_object ? source_object->resource : NULL)) {
            (void)wl_client_send_words(
                server->selection_client,
                server->selection_source->object_id, 2u, NULL, 0u);
        }
        server->selection_client = source_object ? client : NULL;
        server->selection_source =
            source_object ? source_object->resource : NULL;
        return wl_publish_selection(server);
    }
    if (opcode == 2u && object->version >= 2u &&
        wl_request_complete(request)) {
        wl_client_remove_object(client, object->id, true);
        return 0;
    }
    return wl_protocol_fail(client, object->id,
                            WL_PROTOCOL_ERROR_INVALID_METHOD,
                            "unsupported wl_data_device request");
}

static int wl_dispatch_xdg_output_manager(
    struct wl_server *server, struct wl_server_client *client,
    struct wl_server_object *object, uint16_t opcode,
    struct wl_request *request)
{
    uint32_t new_id;
    uint32_t output_id;
    uint32_t logical_position[2] = {0u, 0u};
    uint32_t logical_size[2];
    struct wl_server_object *output;

    if (opcode == 0u && wl_request_complete(request)) {
        wl_client_remove_object(client, object->id, true);
        return 0;
    }
    if (opcode != 1u ||
        wl_request_u32(request, &new_id) < 0 ||
        wl_request_u32(request, &output_id) < 0 ||
        !wl_request_complete(request))
        return wl_protocol_fail(client, object->id,
                                WL_PROTOCOL_ERROR_INVALID_METHOD,
                                "malformed xdg-output manager request");

    output = wl_client_find_object(client, output_id);
    if (!output || output->type != WL_SERVER_OBJECT_OUTPUT)
        return wl_protocol_fail(client, object->id,
                                WL_PROTOCOL_ERROR_INVALID_OBJECT,
                                "xdg-output requires a wl_output");
    if (wl_client_add_object(client, new_id, WL_SERVER_OBJECT_XDG_OUTPUT,
                             object->version, output) < 0)
        return -1;

    logical_size[0] = server->renderer.framebuffer.width;
    logical_size[1] = server->renderer.framebuffer.height;
    if (wl_client_send_words(client, new_id, 0u,
                             logical_position, 2u) < 0 ||
        wl_client_send_words(client, new_id, 1u,
                             logical_size, 2u) < 0)
        return -1;
    if (object->version >= 2u &&
        (wl_client_send_string(client, new_id, 3u, "ARMOS-1") < 0 ||
         wl_client_send_string(client, new_id, 4u,
                               "ArmOS logical framebuffer") < 0))
        return -1;
    if (object->version < 3u)
        return wl_client_send_words(client, new_id, 2u, NULL, 0u);
    return wl_client_send_words(client, output_id, 2u, NULL, 0u);
}

static int wl_dispatch_xdg_output(struct wl_server_client *client,
                                  struct wl_server_object *object,
                                  uint16_t opcode,
                                  struct wl_request *request)
{
    if (opcode == 0u && wl_request_complete(request)) {
        wl_client_remove_object(client, object->id, true);
        return 0;
    }
    return wl_protocol_fail(client, object->id,
                            WL_PROTOCOL_ERROR_INVALID_METHOD,
                            "unsupported xdg-output request");
}

static int wl_dispatch_output(struct wl_server_client *client,
                              struct wl_server_object *object,
                              uint16_t opcode,
                              struct wl_request *request)
{
    if (opcode == 0u && object->version >= 3u &&
        wl_request_complete(request)) {
        wl_client_remove_object(client, object->id, true);
        return 0;
    }
    return wl_protocol_fail(client, object->id,
                            WL_PROTOCOL_ERROR_INVALID_METHOD,
                            "unsupported wl_output request");
}

int wl_server_dispatch_message(struct wl_server *server,
                               struct wl_server_client *client,
                               const uint8_t *message, size_t size)
{
    struct wl_server_object *object;
    struct wl_request request;
    uint32_t object_id;
    uint32_t header;
    uint16_t opcode;

    if (!server || !client || !message || size < 8u)
        return -1;
    object_id = wl_wire_u32(message);
    header = wl_wire_u32(message + 4u);
    opcode = (uint16_t)(header & 0xffffu);
    object = wl_client_find_object(client, object_id);
    if (!object)
        return wl_protocol_fail(client, object_id,
                                WL_PROTOCOL_ERROR_INVALID_OBJECT,
                                "unknown Wayland object");
    request.data = message + 8u;
    request.size = size - 8u;
    request.cursor = 0;

    switch (object->type) {
    case WL_SERVER_OBJECT_DISPLAY:
        return wl_dispatch_display(server, client, opcode, &request);
    case WL_SERVER_OBJECT_REGISTRY:
        return wl_dispatch_registry(server, client, object, opcode,
                                    &request);
    case WL_SERVER_OBJECT_COMPOSITOR:
        return wl_dispatch_compositor(server, client, object, opcode,
                                      &request);
    case WL_SERVER_OBJECT_SUBCOMPOSITOR:
        return wl_dispatch_subcompositor(client, object, opcode, &request);
    case WL_SERVER_OBJECT_SUBSURFACE:
        return wl_dispatch_subsurface(server, client, object, opcode,
                                      &request);
    case WL_SERVER_OBJECT_REGION:
        return wl_dispatch_region(client, object, opcode, &request);
    case WL_SERVER_OBJECT_SHM:
        return wl_dispatch_shm(client, object, opcode, &request);
    case WL_SERVER_OBJECT_SHM_POOL:
        return wl_dispatch_pool(client, object, opcode, &request);
    case WL_SERVER_OBJECT_BUFFER:
        return wl_dispatch_buffer(client, object, opcode, &request);
    case WL_SERVER_OBJECT_SURFACE:
        return wl_dispatch_surface(server, client, object, opcode, &request);
    case WL_SERVER_OBJECT_SEAT:
        return wl_dispatch_seat(server, client, object, opcode, &request);
    case WL_SERVER_OBJECT_POINTER:
    case WL_SERVER_OBJECT_KEYBOARD:
    case WL_SERVER_OBJECT_TOUCH:
        return wl_dispatch_input_object(client, object, opcode, &request);
    case WL_SERVER_OBJECT_OUTPUT:
        return wl_dispatch_output(client, object, opcode, &request);
    case WL_SERVER_OBJECT_XDG_OUTPUT_MANAGER:
        return wl_dispatch_xdg_output_manager(server, client, object,
                                              opcode, &request);
    case WL_SERVER_OBJECT_XDG_OUTPUT:
        return wl_dispatch_xdg_output(client, object, opcode, &request);
    case WL_SERVER_OBJECT_DATA_DEVICE_MANAGER:
        return wl_dispatch_data_device_manager(server, client, object,
                                               opcode, &request);
    case WL_SERVER_OBJECT_DATA_SOURCE:
        return wl_dispatch_data_source(server, client, object, opcode,
                                       &request);
    case WL_SERVER_OBJECT_DATA_DEVICE:
        return wl_dispatch_data_device(server, client, object, opcode,
                                       &request);
    case WL_SERVER_OBJECT_DATA_OFFER:
        return wl_dispatch_data_offer(client, object, opcode, &request);
    case WL_SERVER_OBJECT_XDG_WM_BASE:
        return wl_dispatch_xdg_wm_base(server, client, object, opcode,
                                       &request);
    case WL_SERVER_OBJECT_XDG_SURFACE:
        return wl_dispatch_xdg_surface(server, client, object, opcode,
                                       &request);
    case WL_SERVER_OBJECT_XDG_TOPLEVEL:
        return wl_dispatch_xdg_toplevel(server, client, object, opcode,
                                        &request);
    case WL_SERVER_OBJECT_XDG_DECORATION_MANAGER:
        return wl_dispatch_xdg_decoration_manager(client, object, opcode,
                                                  &request);
    case WL_SERVER_OBJECT_XDG_TOPLEVEL_DECORATION:
        return wl_dispatch_xdg_toplevel_decoration(
            server, client, object, opcode, &request);
    default:
        return wl_protocol_fail(client, object_id,
                                WL_PROTOCOL_ERROR_INVALID_METHOD,
                                "object does not accept requests");
    }
}
