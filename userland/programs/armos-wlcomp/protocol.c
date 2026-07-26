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

static int wl_protocol_fail(struct wl_server_client *client,
                            uint32_t object_id, uint32_t code,
                            const char *message)
{
    (void)wl_client_send_error(client, object_id, code, message);
    return -1;
}

static int wl_request_complete(const struct wl_request *request)
{
    return request && request->cursor == request->size;
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
                                  "wl_compositor", 1u) < 0 ||
            wl_client_send_global(client, new_id, WL_GLOBAL_SHM,
                                  "wl_shm", 1u) < 0 ||
            wl_client_send_global(client, new_id, WL_GLOBAL_SEAT,
                                  "wl_seat", 4u) < 0 ||
            wl_client_send_global(client, new_id, WL_GLOBAL_XDG_SHELL,
                                  "xdg_wm_base", 1u) < 0)
            return -1;
        if (wl_client_send_global(client, new_id, WL_GLOBAL_OUTPUT,
                                  "wl_output", 2u) < 0)
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
        return wl_client_add_object(client, new_id,
                                    WL_SERVER_OBJECT_COMPOSITOR, 1u, NULL);
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
        uint32_t version = requested_version < 4u ?
            requested_version : 4u;

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

        if (wl_request_u32(request, &new_id) < 0 ||
            !wl_request_complete(request) ||
            wl_client_add_object(client, new_id,
                                 WL_SERVER_OBJECT_XDG_TOPLEVEL, 1u,
                                 surface) < 0)
            return wl_protocol_fail(client, object->id,
                                    WL_PROTOCOL_ERROR_INVALID_OBJECT,
                                    "invalid xdg_toplevel object");
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

static int wl_dispatch_xdg_toplevel(struct wl_server_client *client,
                                    struct wl_server_object *object,
                                    uint16_t opcode,
                                    struct wl_request *request)
{
    if (opcode == 0u && wl_request_complete(request)) {
        wl_client_remove_object(client, object->id, true);
        return 0;
    }
    if (opcode == 2u || opcode == 3u) {
        const char *text;

        if (wl_request_string(request, &text, NULL) == 0 &&
            wl_request_complete(request)) {
            (void)text;
            return 0;
        }
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
                                 1u, surface) < 0) {
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

static int wl_dispatch_surface(struct wl_server *server,
                               struct wl_server_client *client,
                               struct wl_server_object *object,
                               uint16_t opcode, struct wl_request *request)
{
    struct wl_server_surface *surface = object->resource;

    if (!surface)
        return -1;
    if (opcode == 0u && wl_request_complete(request)) {
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
    if (opcode == 2u && request->size == 16u) {
        request->cursor = request->size;
        return 0;
    }
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
    return wl_protocol_fail(client, object->id,
                            WL_PROTOCOL_ERROR_INVALID_METHOD,
                            "unsupported wl_surface request");
}

static int wl_dispatch_seat(struct wl_server_client *client,
                            struct wl_server_object *object,
                            uint16_t opcode, struct wl_request *request)
{
    uint32_t new_id;

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
        if (wl_server_send_keymap(client, new_id) < 0) {
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
        request->cursor = request->size;
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
    return wl_protocol_fail(client, object->id,
                            WL_PROTOCOL_ERROR_INVALID_METHOD,
                            "malformed input object request");
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
        return wl_dispatch_seat(client, object, opcode, &request);
    case WL_SERVER_OBJECT_POINTER:
    case WL_SERVER_OBJECT_KEYBOARD:
        return wl_dispatch_input_object(client, object, opcode, &request);
    case WL_SERVER_OBJECT_XDG_WM_BASE:
        return wl_dispatch_xdg_wm_base(server, client, object, opcode,
                                       &request);
    case WL_SERVER_OBJECT_XDG_SURFACE:
        return wl_dispatch_xdg_surface(server, client, object, opcode,
                                       &request);
    case WL_SERVER_OBJECT_XDG_TOPLEVEL:
        return wl_dispatch_xdg_toplevel(client, object, opcode, &request);
    default:
        return wl_protocol_fail(client, object_id,
                                WL_PROTOCOL_ERROR_INVALID_METHOD,
                                "object does not accept requests");
    }
}
