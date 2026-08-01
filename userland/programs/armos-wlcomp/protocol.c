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
 * - Damage and opaque-region state are applied transactionally on commit.
 */

#include "armos_wlcomp.h"
#include "gpu_backend.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
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

static bool wl_surface_effectively_synchronized(
    const struct wl_server_surface *surface);
int wl_surface_apply_commit_tree(
    struct wl_server *server, struct wl_server_client *client,
    struct wl_server_surface *surface, bool commit_surface);

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

static struct wl_server_region *wl_allocate_region(
    struct wl_server_client *client)
{
    for (size_t index = 0u; index < WL_SERVER_MAX_REGIONS; index++) {
        if (!client->regions[index].used) {
            memset(&client->regions[index], 0,
                   sizeof(client->regions[index]));
            client->regions[index].used = true;
            return &client->regions[index];
        }
    }
    return NULL;
}

static struct wl_server_positioner *wl_allocate_positioner(
    struct wl_server_client *client)
{
    for (size_t index = 0u; index < WL_SERVER_MAX_POSITIONERS; index++) {
        if (!client->positioners[index].used) {
            memset(&client->positioners[index], 0,
                   sizeof(client->positioners[index]));
            client->positioners[index].used = true;
            return &client->positioners[index];
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

static void wl_destroy_gpu_buffer(struct wl_server_client *client,
                                  struct wl_server_buffer *buffer)
{
    struct wl_server_renderer *renderer;

    if (!client || !buffer || !buffer->gpu_backed)
        return;
    renderer = client->server ? &client->server->renderer : NULL;
    if (renderer && buffer->gpu_image)
        wl_gpu_backend_destroy_image(
            renderer->gpu_backend, buffer->gpu_image);
    if (buffer->drm_mapping && buffer->drm_mapping != MAP_FAILED)
        (void)munmap(buffer->drm_mapping, buffer->drm_size);
    if (renderer && renderer->drm_fd >= 0 && buffer->drm_handle != 0u) {
        if (renderer->drm_context_id != 0u) {
            armos_drm_resource_attachment_t detach = {
                .context_id = renderer->drm_context_id,
                .handle = buffer->drm_handle,
            };

            (void)ioctl(renderer->drm_fd,
                        ARMOS_DRM_IOCTL_RESOURCE_DETACH, &detach);
        }
        {
            armos_drm_bo_destroy_t destroy = {
                .handle = buffer->drm_handle,
            };

            (void)ioctl(renderer->drm_fd,
                        ARMOS_DRM_IOCTL_BO_DESTROY, &destroy);
        }
    }
}

void wl_client_destroy_buffers(struct wl_server_client *client)
{
    if (!client)
        return;
    for (size_t index = 0u; index < WL_SERVER_MAX_SURFACES; index++) {
        struct wl_server_surface *surface = &client->surfaces[index];

        if (!surface->used)
            continue;
        if (surface->acquire_fence_source) {
            (void)wl_event_source_remove(surface->acquire_fence_source);
            surface->acquire_fence_source = NULL;
        }
        if (surface->pending_acquire_fence_fd >= 0) {
            close(surface->pending_acquire_fence_fd);
            surface->pending_acquire_fence_fd = -1;
        }
    }
    for (size_t index = 0u; index < WL_SERVER_MAX_BUFFERS; index++) {
        if (client->buffers[index].used)
            wl_destroy_gpu_buffer(client, &client->buffers[index]);
    }
}

void wl_client_reclaim_buffers(struct wl_server_client *client)
{
    if (!client)
        return;

    /*
     * Destroying a wl_buffer only destroys its protocol object.  Keep its
     * storage while a surface still references it, then reclaim dead buffers
     * before reclaiming their already-destroyed wl_shm_pool.
     */
    for (size_t buffer_index = 0u;
         buffer_index < WL_SERVER_MAX_BUFFERS; buffer_index++) {
        struct wl_server_buffer *buffer = &client->buffers[buffer_index];
        bool referenced = false;

        if (!buffer->used || buffer->object_alive)
            continue;
        for (size_t surface_index = 0u;
             surface_index < WL_SERVER_MAX_SURFACES; surface_index++) {
            const struct wl_server_surface *surface =
                &client->surfaces[surface_index];

            if (surface->used &&
                (surface->current_buffer == buffer ||
                 surface->pending_buffer == buffer)) {
                referenced = true;
                break;
            }
        }
        if (!referenced) {
            wl_destroy_gpu_buffer(client, buffer);
            memset(buffer, 0, sizeof(*buffer));
        }
    }

    for (size_t pool_index = 0u;
         pool_index < WL_SERVER_MAX_POOLS; pool_index++) {
        struct wl_server_pool *pool = &client->pools[pool_index];
        bool referenced = false;

        if (!pool->used || pool->object_alive)
            continue;
        for (size_t buffer_index = 0u;
             buffer_index < WL_SERVER_MAX_BUFFERS; buffer_index++) {
            const struct wl_server_buffer *buffer =
                &client->buffers[buffer_index];

            if (buffer->used && buffer->pool == pool) {
                referenced = true;
                break;
            }
        }
        if (referenced)
            continue;
        if (pool->mapping && pool->mapping != MAP_FAILED)
            munmap(pool->mapping, pool->size);
        if (pool->fd >= 0)
            close(pool->fd);
        memset(pool, 0, sizeof(*pool));
    }
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
            surface->pending_acquire_fence_fd = -1;
            surface->server_decorated = true;
            surface->z_order = ++server->next_surface_z;
            surface->x = (int32_t)(24u + (position * 36u) %
                                   (width > 300u ? width - 300u : 1u));
            surface->y = (int32_t)(server->panel_height + 24u +
                (position * 28u) %
                (height > server->panel_height + 220u ?
                 height - server->panel_height - 220u : 1u));
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
        if (wl_client_send_global(client, new_id, WL_GLOBAL_ARMOS_SHELL,
                                  "armos_shell_v1", 1u) < 0)
            return -1;
        if (server->renderer.gpu_buffer_import &&
            wl_client_send_global(
                client, new_id, WL_GLOBAL_ARMOS_GPU_BUFFER,
                "armos_gpu_buffer_manager_v1", 1u) < 0)
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
    if (name == WL_GLOBAL_ARMOS_SHELL &&
        strcmp(interface_name, "armos_shell_v1") == 0) {
        return wl_client_add_object(client, new_id,
                                    WL_SERVER_OBJECT_ARMOS_SHELL,
                                    1u, NULL);
    }
    if (name == WL_GLOBAL_ARMOS_GPU_BUFFER &&
        strcmp(interface_name, "armos_gpu_buffer_manager_v1") == 0 &&
        server->renderer.gpu_buffer_import) {
        return wl_client_add_object(
            client, new_id, WL_SERVER_OBJECT_ARMOS_GPU_BUFFER_MANAGER,
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
    if (opcode == 1u &&
        wl_request_u32(request, &new_id) == 0 &&
        wl_request_complete(request)) {
        struct wl_server_positioner *positioner =
            wl_allocate_positioner(client);

        if (!positioner)
            return wl_protocol_fail(client, object->id,
                                    WL_PROTOCOL_ERROR_IMPLEMENTATION,
                                    "xdg_positioner limit reached");
        positioner->object_id = new_id;
        if (wl_client_add_object(
                client, new_id, WL_SERVER_OBJECT_XDG_POSITIONER,
                1u, positioner) < 0) {
            memset(positioner, 0, sizeof(*positioner));
            return wl_protocol_fail(client, object->id,
                                    WL_PROTOCOL_ERROR_INVALID_OBJECT,
                                    "invalid xdg_positioner object");
        }
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

static bool wl_positioner_axis_valid(uint32_t value)
{
    return (value & ~15u) == 0u &&
        (value & (1u | 2u)) != (1u | 2u) &&
        (value & (4u | 8u)) != (4u | 8u);
}

static int wl_dispatch_xdg_positioner(
    struct wl_server_client *client, struct wl_server_object *object,
    uint16_t opcode, struct wl_request *request)
{
    struct wl_server_positioner *positioner = object->resource;
    uint32_t first;
    uint32_t second;

    if (!positioner || !positioner->used)
        return wl_protocol_fail(client, object->id,
                                WL_PROTOCOL_ERROR_INVALID_OBJECT,
                                "invalid xdg_positioner");
    if (opcode == 0u && wl_request_complete(request)) {
        memset(positioner, 0, sizeof(*positioner));
        wl_client_remove_object(client, object->id, true);
        return 0;
    }
    if (opcode == 1u &&
        wl_request_u32(request, &first) == 0 &&
        wl_request_u32(request, &second) == 0 &&
        wl_request_complete(request) &&
        (int32_t)first > 0 && (int32_t)second > 0) {
        positioner->width = (int32_t)first;
        positioner->height = (int32_t)second;
        return 0;
    }
    if (opcode == 2u && request->size == 16u) {
        uint32_t x;
        uint32_t y;
        uint32_t width;
        uint32_t height;

        if (wl_request_u32(request, &x) == 0 &&
            wl_request_u32(request, &y) == 0 &&
            wl_request_u32(request, &width) == 0 &&
            wl_request_u32(request, &height) == 0 &&
            wl_request_complete(request) &&
            (int32_t)width > 0 && (int32_t)height > 0) {
            positioner->anchor_x = (int32_t)x;
            positioner->anchor_y = (int32_t)y;
            positioner->anchor_width = (int32_t)width;
            positioner->anchor_height = (int32_t)height;
            return 0;
        }
    }
    if ((opcode == 3u || opcode == 4u || opcode == 5u) &&
        wl_request_u32(request, &first) == 0 &&
        wl_request_complete(request)) {
        if (opcode == 3u && wl_positioner_axis_valid(first)) {
            positioner->anchor = first;
            return 0;
        }
        if (opcode == 4u && wl_positioner_axis_valid(first)) {
            positioner->gravity = first;
            return 0;
        }
        if (opcode == 5u && (first & ~63u) == 0u) {
            positioner->constraint_adjustment = first;
            return 0;
        }
    }
    if (opcode == 6u &&
        wl_request_u32(request, &first) == 0 &&
        wl_request_u32(request, &second) == 0 &&
        wl_request_complete(request)) {
        positioner->offset_x = (int32_t)first;
        positioner->offset_y = (int32_t)second;
        return 0;
    }
    return wl_protocol_fail(client, object->id,
                            WL_PROTOCOL_ERROR_INVALID_METHOD,
                            "malformed xdg_positioner request");
}

static int32_t wl_positioner_anchor_coordinate(
    int32_t origin, int32_t extent, uint32_t flags,
    uint32_t negative, uint32_t positive)
{
    if ((flags & negative) != 0u)
        return origin;
    if ((flags & positive) != 0u)
        return origin + extent;
    return origin + extent / 2;
}

static int32_t wl_positioner_apply_gravity(
    int32_t anchor, int32_t extent, uint32_t flags,
    uint32_t negative, uint32_t positive)
{
    if ((flags & negative) != 0u)
        return anchor - extent;
    if ((flags & positive) != 0u)
        return anchor;
    return anchor - extent / 2;
}

static int wl_surface_parent_origin(
    const struct wl_server_surface *surface, int32_t *x, int32_t *y)
{
    const struct wl_server_surface *ancestor = surface;
    size_t depth = 0u;

    if (!surface || !x || !y)
        return -1;
    if (!wl_surface_is_child_role(surface)) {
        *x = surface->x;
        *y = surface->y +
            (wl_surface_has_server_decoration(surface) ?
             (int32_t)WL_WINDOW_TITLE_HEIGHT : 0);
        return 0;
    }
    *x = surface->subsurface_x;
    *y = surface->subsurface_y;
    while (wl_surface_is_child_role(ancestor) && ancestor->parent &&
           depth++ < WL_SERVER_MAX_SURFACES) {
        ancestor = ancestor->parent;
        if (wl_surface_is_child_role(ancestor)) {
            *x += ancestor->subsurface_x;
            *y += ancestor->subsurface_y;
        } else {
            *x += ancestor->x;
            *y += ancestor->y +
                (wl_surface_has_server_decoration(ancestor) ?
                 (int32_t)WL_WINDOW_TITLE_HEIGHT : 0);
        }
    }
    return depth <= WL_SERVER_MAX_SURFACES ? 0 : -1;
}

static void wl_position_popup(
    const struct wl_server *server, struct wl_server_surface *surface,
    const struct wl_server_positioner *positioner)
{
    int32_t anchor_x = wl_positioner_anchor_coordinate(
        positioner->anchor_x, positioner->anchor_width,
        positioner->anchor, 4u, 8u);
    int32_t anchor_y = wl_positioner_anchor_coordinate(
        positioner->anchor_y, positioner->anchor_height,
        positioner->anchor, 1u, 2u);
    int32_t x = wl_positioner_apply_gravity(
        anchor_x, positioner->width, positioner->gravity, 4u, 8u) +
        positioner->offset_x;
    int32_t y = wl_positioner_apply_gravity(
        anchor_y, positioner->height, positioner->gravity, 1u, 2u) +
        positioner->offset_y;
    int32_t parent_x = 0;
    int32_t parent_y = 0;
    int32_t screen_width = (int32_t)server->renderer.framebuffer.width;
    int32_t screen_height = (int32_t)server->renderer.framebuffer.height;

    if (surface->parent)
        (void)wl_surface_parent_origin(
            surface->parent, &parent_x, &parent_y);
    if ((positioner->constraint_adjustment & 1u) != 0u) {
        if (parent_x + x < 0)
            x = -parent_x;
        if (parent_x + x + positioner->width > screen_width)
            x = screen_width - parent_x - positioner->width;
    }
    if ((positioner->constraint_adjustment & 2u) != 0u) {
        if (parent_y + y < 0)
            y = -parent_y;
        if (parent_y + y + positioner->height > screen_height)
            y = screen_height - parent_y - positioner->height;
    }
    surface->subsurface_x = x;
    surface->subsurface_y = y;
    surface->width = (uint32_t)positioner->width;
    surface->height = (uint32_t)positioner->height;
}

static int wl_dispatch_xdg_surface(struct wl_server *server,
                                   struct wl_server_client *client,
                                   struct wl_server_object *object,
                                   uint16_t opcode,
                                   struct wl_request *request)
{
    struct wl_server_surface *surface = object->resource;

    (void)server;
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
        /*
         * xdg-shell mapping starts with an empty wl_surface.commit.  The
         * initial configure is emitted from that commit, never while the
         * role object is merely being constructed.
         */
        return 0;
    }
    if (opcode == 2u) {
        uint32_t new_id;
        uint32_t parent_id;
        uint32_t positioner_id;
        struct wl_server_object *parent_object = NULL;
        struct wl_server_object *positioner_object;
        struct wl_server_positioner *positioner;
        bool assigned_role;

        if (wl_request_u32(request, &new_id) < 0 ||
            wl_request_u32(request, &parent_id) < 0 ||
            wl_request_u32(request, &positioner_id) < 0 ||
            !wl_request_complete(request))
            return wl_protocol_fail(client, object->id,
                                    WL_PROTOCOL_ERROR_INVALID_METHOD,
                                    "malformed xdg_surface.get_popup");
        if (parent_id != 0u) {
            parent_object = wl_client_find_object(client, parent_id);
            if (!parent_object ||
                parent_object->type != WL_SERVER_OBJECT_XDG_SURFACE ||
                !parent_object->resource)
                return wl_protocol_fail(client, object->id,
                                        WL_PROTOCOL_ERROR_INVALID_OBJECT,
                                        "invalid xdg_popup parent");
        }
        positioner_object = wl_client_find_object(client, positioner_id);
        if (!positioner_object ||
            positioner_object->type != WL_SERVER_OBJECT_XDG_POSITIONER)
            return wl_protocol_fail(client, object->id,
                                    WL_PROTOCOL_ERROR_INVALID_OBJECT,
                                    "invalid xdg_popup positioner");
        positioner = positioner_object->resource;
        if (!positioner || !positioner->used ||
            positioner->width <= 0 || positioner->height <= 0 ||
            positioner->anchor_width <= 0 ||
            positioner->anchor_height <= 0)
            return wl_protocol_fail(client, object->id,
                                    WL_PROTOCOL_ERROR_INVALID_METHOD,
                                    "incomplete xdg_popup positioner");
        if ((surface->role != WL_SERVER_SURFACE_ROLE_NONE &&
             surface->role != WL_SERVER_SURFACE_ROLE_POPUP) ||
            wl_surface_has_role_object(
                client, surface, WL_SERVER_OBJECT_XDG_POPUP))
            return wl_protocol_fail(client, object->id,
                                    WL_PROTOCOL_ERROR_INVALID_OBJECT,
                                    "wl_surface already has a role");
        assigned_role = surface->role == WL_SERVER_SURFACE_ROLE_NONE;
        surface->role = WL_SERVER_SURFACE_ROLE_POPUP;
        surface->server_decorated = false;
        surface->parent = parent_object ?
            parent_object->resource : NULL;
        wl_position_popup(server, surface, positioner);
        if (wl_client_add_object(client, new_id,
                                 WL_SERVER_OBJECT_XDG_POPUP, 1u,
                                 surface) < 0) {
            if (assigned_role)
                surface->role = WL_SERVER_SURFACE_ROLE_NONE;
            surface->parent = NULL;
            return wl_protocol_fail(client, object->id,
                                    WL_PROTOCOL_ERROR_INVALID_OBJECT,
                                    "invalid xdg_popup object");
        }
        return 0;
    }
    if (opcode == 3u && request->size == 16u) {
        request->cursor = request->size;
        return 0;
    }
    if (opcode == 4u && request->size == 4u) {
        uint32_t serial;
        size_t configure_index;

        if (wl_request_u32(request, &serial) < 0 ||
            !wl_request_complete(request))
            return wl_protocol_fail(client, object->id,
                                    WL_PROTOCOL_ERROR_INVALID_METHOD,
                                    "malformed xdg_surface.ack_configure");
        for (configure_index = 0u;
             configure_index < surface->xdg_configure_count;
             configure_index++) {
            if (surface->xdg_configure_serials[configure_index] == serial)
                break;
        }
        if (!surface->xdg_initial_configure_sent ||
            configure_index == surface->xdg_configure_count)
            return wl_protocol_fail(client, object->id,
                                    WL_PROTOCOL_ERROR_INVALID_METHOD,
                                    "unknown xdg_surface configure serial");
        configure_index++;
        surface->xdg_configure_count -= configure_index;
        if (surface->xdg_configure_count != 0u) {
            memmove(surface->xdg_configure_serials,
                    surface->xdg_configure_serials + configure_index,
                    surface->xdg_configure_count *
                        sizeof(surface->xdg_configure_serials[0]));
            surface->xdg_pending_configure_serial =
                surface->xdg_configure_serials[
                    surface->xdg_configure_count - 1u];
        } else {
            surface->xdg_pending_configure_serial = 0u;
        }
        surface->xdg_configure_acked = true;
        surface->xdg_acked_configure_serial = serial;
        return 0;
    }
    return wl_protocol_fail(client, object->id,
                            WL_PROTOCOL_ERROR_INVALID_METHOD,
                            "unsupported xdg_surface request");
}

int wl_server_configure_toplevel(struct wl_server *server,
    struct wl_server_client *client, struct wl_server_surface *surface,
    uint32_t width, uint32_t height, uint32_t state)
{
    struct wl_server_object *xdg_surface = NULL;
    struct wl_server_object *toplevel = NULL;
    uint32_t configure[4] = { width, height, 0u, 0u };
    uint32_t serial;
    size_t word_count = 3u;

    if (!server || !client || !surface)
        return -1;
    if (surface->xdg_configure_count >= WL_SERVER_MAX_CONFIGURES) {
        errno = ENOBUFS;
        return -1;
    }
    for (size_t index = 0u; index < WL_SERVER_MAX_OBJECTS; index++) {
        struct wl_server_object *candidate = &client->objects[index];

        if (candidate->resource != surface)
            continue;
        if (candidate->type == WL_SERVER_OBJECT_XDG_SURFACE)
            xdg_surface = candidate;
        else if (candidate->type == WL_SERVER_OBJECT_XDG_TOPLEVEL)
            toplevel = candidate;
    }
    if (!xdg_surface || !toplevel)
        return -1;
    if (state != 0u) {
        configure[2] = sizeof(uint32_t);
        configure[3] = state;
        word_count = 4u;
    }
    if (wl_client_send_words(client, toplevel->id, 0u,
                             configure, word_count) < 0)
        return -1;
    serial = ++server->serial;
    if (wl_client_send_words(client, xdg_surface->id, 0u,
                             &serial, 1u) < 0)
        return -1;
    surface->xdg_initial_configure_sent = true;
    surface->xdg_configure_serials[
        surface->xdg_configure_count++] = serial;
    surface->xdg_pending_configure_serial = serial;
    return 0;
}

static int wl_server_configure_popup(
    struct wl_server *server, struct wl_server_client *client,
    struct wl_server_surface *surface)
{
    struct wl_server_object *xdg_surface = NULL;
    struct wl_server_object *popup = NULL;
    uint32_t configure[4] = {
        (uint32_t)surface->subsurface_x,
        (uint32_t)surface->subsurface_y,
        surface->width,
        surface->height
    };
    uint32_t serial;

    if (!server || !client || !surface ||
        surface->xdg_configure_count >= WL_SERVER_MAX_CONFIGURES) {
        errno = ENOBUFS;
        return -1;
    }
    for (size_t index = 0u; index < WL_SERVER_MAX_OBJECTS; index++) {
        struct wl_server_object *candidate = &client->objects[index];

        if (candidate->resource != surface)
            continue;
        if (candidate->type == WL_SERVER_OBJECT_XDG_SURFACE)
            xdg_surface = candidate;
        else if (candidate->type == WL_SERVER_OBJECT_XDG_POPUP)
            popup = candidate;
    }
    if (!xdg_surface || !popup)
        return -1;
    if (wl_client_send_words(client, popup->id, 0u,
                             configure, 4u) < 0)
        return -1;
    serial = ++server->serial;
    if (wl_client_send_words(client, xdg_surface->id, 0u,
                             &serial, 1u) < 0)
        return -1;
    surface->xdg_initial_configure_sent = true;
    surface->xdg_configure_serials[
        surface->xdg_configure_count++] = serial;
    surface->xdg_pending_configure_serial = serial;
    return 0;
}

static int wl_dispatch_xdg_toplevel(struct wl_server *server,
                                    struct wl_server_client *client,
                                    struct wl_server_object *object,
                                    uint16_t opcode,
                                    struct wl_request *request)
{
    struct wl_server_surface *surface = object->resource;
    const uint32_t state_maximized = 1u;
    const uint32_t state_fullscreen = 2u;

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
    if (opcode == 7u || opcode == 8u) {
        uint32_t width;
        uint32_t height;

        if (wl_request_u32(request, &width) < 0 ||
            wl_request_u32(request, &height) < 0 ||
            !wl_request_complete(request) || !surface)
            return wl_protocol_fail(client, object->id,
                                    WL_PROTOCOL_ERROR_INVALID_METHOD,
                                    "malformed xdg_toplevel size");
        if (opcode == 7u) {
            surface->maximum_width = width;
            surface->maximum_height = height;
        } else {
            surface->minimum_width = width;
            surface->minimum_height = height;
        }
        return 0;
    }
    if (opcode == 9u || opcode == 11u) {
        uint32_t output_id = 0u;

        if ((opcode == 11u && wl_request_u32(request, &output_id) < 0) ||
            !wl_request_complete(request))
            return wl_protocol_fail(client, object->id,
                                    WL_PROTOCOL_ERROR_INVALID_METHOD,
                                    "malformed xdg_toplevel state");
        (void)output_id;
        if (!surface)
            return -1;
        if (surface->minimized &&
            wl_server_set_surface_minimized(
                server, client, surface, false) < 0)
            return -1;
        wl_renderer_damage_surface_at(server, surface,
                                      surface->x, surface->y);
        if (!surface->maximized && !surface->fullscreen) {
            surface->restore_x = surface->x;
            surface->restore_y = surface->y;
            surface->restore_width = surface->width;
            surface->restore_height = surface->height;
        }
        surface->maximized = opcode == 9u;
        surface->fullscreen = opcode == 11u;
        surface->shaded = false;
        surface->x = 0;
        surface->y = opcode == 11u ? 0 :
            (int32_t)server->panel_height;
        return wl_server_configure_toplevel(
            server, client, surface,
            server->renderer.framebuffer.width,
            opcode == 11u ? server->renderer.framebuffer.height :
                            server->renderer.framebuffer.height -
                                server->panel_height -
                                WL_WINDOW_TITLE_HEIGHT,
            opcode == 11u ? state_fullscreen : state_maximized);
    }
    if ((opcode == 10u || opcode == 12u) && wl_request_complete(request)) {
        if (!surface)
            return -1;
        if ((opcode == 10u && !surface->maximized) ||
            (opcode == 12u && !surface->fullscreen))
            return 0;
        wl_renderer_damage_surface_at(server, surface,
                                      surface->x, surface->y);
        surface->maximized = false;
        surface->fullscreen = false;
        surface->x = surface->restore_x;
        surface->y = surface->restore_y;
        return wl_server_configure_toplevel(
            server, client, surface,
            surface->restore_width, surface->restore_height, 0u);
    }
    if (opcode == 13u && wl_request_complete(request)) {
        if (!surface)
            return -1;
        return wl_server_set_surface_minimized(
            server, client, surface, true);
    }
    if (opcode == 6u) {
        uint32_t seat_id;
        uint32_t serial;
        uint32_t edges;

        if (wl_request_u32(request, &seat_id) < 0 ||
            wl_request_u32(request, &serial) < 0 ||
            wl_request_u32(request, &edges) < 0 ||
            !wl_request_complete(request) || !surface)
            return wl_protocol_fail(client, object->id,
                                    WL_PROTOCOL_ERROR_INVALID_METHOD,
                                    "malformed xdg_toplevel.resize");
        (void)seat_id;
        if (!server->pointer_left ||
            server->pointer_grab_serial != serial)
            return wl_protocol_fail(client, object->id,
                                    WL_PROTOCOL_ERROR_INVALID_METHOD,
                                    "invalid xdg_toplevel.resize grab");
        server->resize_client = client;
        server->resize_surface = surface;
        server->resize_edges = edges;
        server->resize_pointer_x = server->pointer_x;
        server->resize_pointer_y = server->pointer_y;
        server->resize_width = surface->width;
        server->resize_height = surface->height;
        server->resize_initial_width = surface->width;
        server->resize_initial_height = surface->height;
        return 0;
    }
    return wl_protocol_fail(client, object->id,
                            WL_PROTOCOL_ERROR_INVALID_METHOD,
                            "unsupported xdg_toplevel request");
}

static int wl_dispatch_xdg_popup(struct wl_server *server,
                                 struct wl_server_client *client,
                                 struct wl_server_object *object,
                                 uint16_t opcode,
                                 struct wl_request *request)
{
    struct wl_server_surface *surface = object->resource;

    if (!surface || !surface->used)
        return wl_protocol_fail(client, object->id,
                                WL_PROTOCOL_ERROR_INVALID_OBJECT,
                                "xdg_popup lost its surface");
    if (opcode == 0u && wl_request_complete(request)) {
        int32_t origin_x = surface->subsurface_x;
        int32_t origin_y = surface->subsurface_y;

        (void)wl_surface_parent_origin(surface, &origin_x, &origin_y);
        if (surface->mapped)
            wl_renderer_damage_surface_at(
                server, surface, origin_x, origin_y);
        surface->mapped = false;
        wl_client_remove_object(client, object->id, true);
        return wl_server_schedule_render(server, true);
    }
    if (opcode == 1u) {
        uint32_t seat_id;
        uint32_t serial;
        struct wl_server_object *seat;

        if (wl_request_u32(request, &seat_id) < 0 ||
            wl_request_u32(request, &serial) < 0 ||
            !wl_request_complete(request))
            return wl_protocol_fail(client, object->id,
                                    WL_PROTOCOL_ERROR_INVALID_METHOD,
                                    "malformed xdg_popup.grab");
        seat = wl_client_find_object(client, seat_id);
        if (!seat || seat->type != WL_SERVER_OBJECT_SEAT ||
            serial == 0u)
            return wl_protocol_fail(client, object->id,
                                    WL_PROTOCOL_ERROR_INVALID_OBJECT,
                                    "invalid xdg_popup grab");
        /*
         * The seat already routes input to the top-most mapped surface.
         * Tracking the popup role here keeps the grab request valid while
         * leaving dismissal policy centralized in the compositor input path.
         */
        return 0;
    }
    return wl_protocol_fail(client, object->id,
                            WL_PROTOCOL_ERROR_INVALID_METHOD,
                            "unsupported xdg_popup request");
}

static int wl_dispatch_armos_shell_panel(
    struct wl_server *server, struct wl_server_client *client,
    struct wl_server_object *object, uint16_t opcode,
    struct wl_request *request)
{
    struct wl_server_surface *surface = object->resource;

    if (opcode != 0u || !wl_request_complete(request))
        return wl_protocol_fail(client, object->id,
                                WL_PROTOCOL_ERROR_INVALID_METHOD,
                                "unsupported armos_shell_panel request");
    if (surface && surface->used) {
        if (surface->mapped)
            wl_renderer_damage_surface_at(server, surface, 0, 0);
        surface->mapped = false;
        surface->role = WL_SERVER_SURFACE_ROLE_NONE;
        surface->server_decorated = true;
    }
    if (server->shell_client == client) {
        server->shell_client = NULL;
        server->panel_height = 0u;
    }
    wl_client_remove_object(client, object->id, true);
    return wl_server_schedule_render(server, true);
}

static int wl_dispatch_armos_shell(
    struct wl_server *server, struct wl_server_client *client,
    struct wl_server_object *object, uint16_t opcode,
    struct wl_request *request)
{
    if (opcode == 0u && wl_request_complete(request)) {
        wl_client_remove_object(client, object->id, true);
        return 0;
    }
    if (opcode == 1u) {
        uint32_t token;

        if (wl_request_u32(request, &token) < 0 ||
            !wl_request_complete(request))
            return wl_protocol_fail(client, object->id,
                                    WL_PROTOCOL_ERROR_INVALID_METHOD,
                                    "malformed shell authentication");
        if (token == 0u || token != server->shell_token)
            return wl_protocol_fail(client, object->id,
                                    WL_PROTOCOL_ERROR_INVALID_OBJECT,
                                    "shell authentication failed");
        client->shell_authenticated = true;
        return 0;
    }
    if (opcode == 2u) {
        uint32_t new_id;
        uint32_t surface_id;
        uint32_t requested_height;
        struct wl_server_object *surface_object;
        struct wl_server_surface *surface;
        uint32_t configure[2];

        if (wl_request_u32(request, &new_id) < 0 ||
            wl_request_u32(request, &surface_id) < 0 ||
            wl_request_u32(request, &requested_height) < 0 ||
            !wl_request_complete(request))
            return wl_protocol_fail(client, object->id,
                                    WL_PROTOCOL_ERROR_INVALID_METHOD,
                                    "malformed shell panel request");
        if (!client->shell_authenticated)
            return wl_protocol_fail(client, object->id,
                                    WL_PROTOCOL_ERROR_INVALID_OBJECT,
                                    "unauthenticated shell client");
        if (server->shell_client && server->shell_client != client)
            return wl_protocol_fail(client, object->id,
                                    WL_PROTOCOL_ERROR_INVALID_OBJECT,
                                    "system panel already owned");
        if (requested_height < 24u || requested_height > 128u ||
            requested_height >
                server->renderer.framebuffer.height / 3u)
            return wl_protocol_fail(client, object->id,
                                    WL_PROTOCOL_ERROR_INVALID_METHOD,
                                    "invalid panel height");
        surface_object = wl_client_find_object(client, surface_id);
        if (!surface_object ||
            surface_object->type != WL_SERVER_OBJECT_SURFACE ||
            !(surface = surface_object->resource) ||
            !surface->used ||
            surface->role != WL_SERVER_SURFACE_ROLE_NONE)
            return wl_protocol_fail(client, object->id,
                                    WL_PROTOCOL_ERROR_INVALID_OBJECT,
                                    "invalid panel surface");
        surface->role = WL_SERVER_SURFACE_ROLE_PANEL;
        surface->server_decorated = false;
        surface->z_order = UINT64_MAX;
        surface->x = 0;
        surface->y = 0;
        surface->width = server->renderer.framebuffer.width;
        surface->height = requested_height;
        if (wl_client_add_object(
                client, new_id, WL_SERVER_OBJECT_ARMOS_SHELL_PANEL,
                1u, surface) < 0) {
            surface->role = WL_SERVER_SURFACE_ROLE_NONE;
            surface->server_decorated = true;
            return wl_protocol_fail(client, object->id,
                                    WL_PROTOCOL_ERROR_INVALID_OBJECT,
                                    "invalid panel object");
        }
        server->shell_client = client;
        server->panel_height = requested_height;
        configure[0] = surface->width;
        configure[1] = surface->height;
        return wl_client_send_words(client, new_id, 0u, configure, 2u);
    }
    return wl_protocol_fail(client, object->id,
                            WL_PROTOCOL_ERROR_INVALID_METHOD,
                            "unsupported armos_shell request");
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
        struct wl_server_region *region = wl_allocate_region(client);

        if (!region)
            return wl_protocol_fail(client, object->id,
                                    WL_PROTOCOL_ERROR_IMPLEMENTATION,
                                    "region limit reached");
        region->object_id = new_id;
        if (wl_client_add_object(client, new_id, WL_SERVER_OBJECT_REGION,
                                 1u, region) < 0) {
            memset(region, 0, sizeof(*region));
            return wl_protocol_fail(client, object->id,
                                    WL_PROTOCOL_ERROR_INVALID_OBJECT,
                                    "invalid region object id");
        }
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
        surface->pending_subsurface_x = (int32_t)x;
        surface->pending_subsurface_y = (int32_t)y;
        surface->subsurface_position_pending = true;
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
        if (opcode == 5u && surface->subsurface_commit_pending &&
            !wl_surface_effectively_synchronized(surface) &&
            wl_surface_apply_commit_tree(
                server, client, surface, true) < 0)
            return wl_protocol_fail(
                client, object->id,
                WL_PROTOCOL_ERROR_IMPLEMENTATION,
                "failed to apply desynchronized subsurface");
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
    struct wl_server_region *region = object->resource;

    if (!region || !region->used)
        return wl_protocol_fail(client, object->id,
                                WL_PROTOCOL_ERROR_INVALID_OBJECT,
                                "invalid wl_region");
    if (opcode == 0u && wl_request_complete(request)) {
        memset(region, 0, sizeof(*region));
        wl_client_remove_object(client, object->id, true);
        return 0;
    }
    if ((opcode == 1u || opcode == 2u) && request->size == 16u) {
        uint32_t raw_x;
        uint32_t raw_y;
        uint32_t raw_width;
        uint32_t raw_height;
        int32_t x;
        int32_t y;
        int32_t width;
        int32_t height;
        int64_t x1;
        int64_t y1;

        if (wl_request_u32(request, &raw_x) < 0 ||
            wl_request_u32(request, &raw_y) < 0 ||
            wl_request_u32(request, &raw_width) < 0 ||
            wl_request_u32(request, &raw_height) < 0 ||
            !wl_request_complete(request))
            return -1;
        x = (int32_t)raw_x;
        y = (int32_t)raw_y;
        width = (int32_t)raw_width;
        height = (int32_t)raw_height;
        x1 = (int64_t)x + width;
        y1 = (int64_t)y + height;
        if (width <= 0 || height <= 0 ||
            x1 < INT32_MIN || x1 > INT32_MAX ||
            y1 < INT32_MIN || y1 > INT32_MAX)
            return wl_protocol_fail(client, object->id,
                                    WL_PROTOCOL_ERROR_INVALID_METHOD,
                                    "invalid wl_region rectangle");
        if (opcode == 1u) {
            struct wl_renderer_rect incoming = {
                .x0 = x,
                .y0 = y,
                .x1 = (int32_t)x1,
                .y1 = (int32_t)y1
            };

            /*
             * Keep rectangles independent. Replacing two overlapping
             * rectangles by their bounding box could incorrectly classify
             * uncovered corner pixels as opaque.
             */
            if (region->state.rect_count < WL_SERVER_MAX_REGION_RECTS)
                region->state.rects[region->state.rect_count++] = incoming;
        } else {
            /*
             * Subtraction is conservative for opacity: discard intersecting
             * rectangles. This may miss an optimization, but can never mark
             * translucent pixels opaque.
             */
            size_t output = 0u;

            for (size_t index = 0u;
                 index < region->state.rect_count; index++) {
                struct wl_renderer_rect rect =
                    region->state.rects[index];

                if (x >= rect.x1 || x1 <= rect.x0 ||
                    y >= rect.y1 || y1 <= rect.y0)
                    region->state.rects[output++] = rect;
            }
            region->state.rect_count = output;
        }
        return 0;
    }
    return wl_protocol_fail(client, object->id,
                            WL_PROTOCOL_ERROR_INVALID_METHOD,
                            "malformed wl_region request");
}

static int wl_dispatch_gpu_buffer_manager(
    struct wl_server *server, struct wl_server_client *client,
    struct wl_server_object *object, uint16_t opcode,
    struct wl_request *request)
{
    struct wl_server_renderer *renderer = &server->renderer;

    if (!renderer->gpu_buffer_import || renderer->drm_fd < 0 ||
        renderer->drm_context_id == 0u)
        return wl_protocol_fail(client, object->id,
                                WL_PROTOCOL_ERROR_IMPLEMENTATION,
                                "GPU buffer import is unavailable");
    if (opcode == 0u) {
        armos_drm_bo_import_t import_request;
        armos_drm_resource_attachment_t attach;
        struct wl_server_buffer *buffer;
        uint32_t new_id;
        int fd;

        if (wl_request_u32(request, &new_id) < 0 ||
            !wl_request_complete(request))
            return wl_protocol_fail(client, object->id,
                                    WL_PROTOCOL_ERROR_INVALID_METHOD,
                                    "malformed GPU buffer creation");
        fd = wl_client_take_fd(client);
        if (fd < 0)
            return wl_protocol_fail(client, object->id,
                                    WL_PROTOCOL_ERROR_INVALID_METHOD,
                                    "GPU buffer creation needs one fd");
        memset(&import_request, 0, sizeof(import_request));
        import_request.abi_version = ARMOS_DRM_ABI_VERSION;
        import_request.fd = fd;
        if (ioctl(renderer->drm_fd, ARMOS_DRM_IOCTL_BO_IMPORT,
                  &import_request) < 0) {
            close(fd);
            return wl_protocol_fail(client, object->id,
                                    WL_PROTOCOL_ERROR_INVALID_METHOD,
                                    "cannot import GPU buffer");
        }
        close(fd);
        if (import_request.width == 0u || import_request.height == 0u ||
            import_request.stride < import_request.width * 4u ||
            import_request.format != ARMOS_DRM_FORMAT_BGRA8888 ||
            import_request.size == 0u || import_request.size > SIZE_MAX ||
            (uint64_t)import_request.stride * import_request.height >
                import_request.size) {
            armos_drm_bo_destroy_t destroy = {
                .handle = import_request.handle,
            };

            (void)ioctl(renderer->drm_fd,
                        ARMOS_DRM_IOCTL_BO_DESTROY, &destroy);
            return wl_protocol_fail(client, object->id,
                                    WL_PROTOCOL_ERROR_INVALID_METHOD,
                                    "unsupported GPU buffer metadata");
        }
        memset(&attach, 0, sizeof(attach));
        attach.context_id = renderer->drm_context_id;
        attach.handle = import_request.handle;
        if (ioctl(renderer->drm_fd, ARMOS_DRM_IOCTL_RESOURCE_ATTACH,
                  &attach) < 0) {
            armos_drm_bo_destroy_t destroy = {
                .handle = import_request.handle,
            };

            (void)ioctl(renderer->drm_fd,
                        ARMOS_DRM_IOCTL_BO_DESTROY, &destroy);
            return wl_protocol_fail(client, object->id,
                                    WL_PROTOCOL_ERROR_IMPLEMENTATION,
                                    "cannot attach GPU buffer");
        }
        buffer = wl_allocate_buffer(client);
        if (!buffer) {
            (void)ioctl(renderer->drm_fd,
                        ARMOS_DRM_IOCTL_RESOURCE_DETACH, &attach);
            {
                armos_drm_bo_destroy_t destroy = {
                    .handle = import_request.handle,
                };

                (void)ioctl(renderer->drm_fd,
                            ARMOS_DRM_IOCTL_BO_DESTROY, &destroy);
            }
            return wl_protocol_fail(client, object->id,
                                    WL_PROTOCOL_ERROR_IMPLEMENTATION,
                                    "buffer limit reached");
        }
        buffer->object_id = new_id;
        buffer->object_alive = true;
        buffer->gpu_backed = true;
        buffer->manager_object_id = object->id;
        buffer->drm_handle = import_request.handle;
        buffer->drm_command_handle = import_request.command_handle;
        buffer->drm_size = (size_t)import_request.size;
        buffer->width = import_request.width;
        buffer->height = import_request.height;
        buffer->stride = import_request.stride;
        buffer->format = WL_SHM_FORMAT_ARGB8888;
        if (renderer->gpu_backend) {
            armos_drm_bo_export_t export_request;
            struct wl_gpu_image_config image_config;

            memset(&export_request, 0, sizeof(export_request));
            export_request.handle = buffer->drm_handle;
            export_request.flags = ARMOS_DRM_SHARE_CLOEXEC;
            if (ioctl(renderer->drm_fd, ARMOS_DRM_IOCTL_BO_EXPORT,
                      &export_request) == 0) {
                memset(&image_config, 0, sizeof(image_config));
                image_config.width = buffer->width;
                image_config.height = buffer->height;
                image_config.stride = buffer->stride;
                image_config.alpha =
                    buffer->format == WL_SHM_FORMAT_ARGB8888;
                buffer->gpu_image = wl_gpu_backend_import_image(
                    renderer->gpu_backend, &image_config,
                    export_request.fd);
                close(export_request.fd);
            }
            if (buffer->gpu_image)
                renderer->profile_gpu_imports++;
        }
        if (!buffer->gpu_image) {
            if ((import_request.bo_flags & ARMOS_DRM_BO_CPU_READ) == 0u) {
                wl_destroy_gpu_buffer(client, buffer);
                memset(buffer, 0, sizeof(*buffer));
                return wl_protocol_fail(
                    client, object->id,
                    WL_PROTOCOL_ERROR_IMPLEMENTATION,
                    "GPU buffer is neither importable nor CPU-readable");
            }
            buffer->drm_mapping = mmap(
                NULL, buffer->drm_size, PROT_READ, MAP_SHARED,
                renderer->drm_fd, (off_t)import_request.map_offset);
            if (buffer->drm_mapping == MAP_FAILED) {
                buffer->drm_mapping = NULL;
                wl_destroy_gpu_buffer(client, buffer);
                memset(buffer, 0, sizeof(*buffer));
                return wl_protocol_fail(
                    client, object->id,
                    WL_PROTOCOL_ERROR_IMPLEMENTATION,
                    "cannot map GPU buffer fallback");
            }
        }
        if (wl_client_add_object(client, new_id, WL_SERVER_OBJECT_BUFFER,
                                 1u, buffer) < 0) {
            wl_destroy_gpu_buffer(client, buffer);
            memset(buffer, 0, sizeof(*buffer));
            return wl_protocol_fail(client, object->id,
                                    WL_PROTOCOL_ERROR_INVALID_OBJECT,
                                    "invalid GPU buffer object id");
        }
        return 0;
    }
    if (opcode == 1u) {
        struct wl_server_object *surface_object;
        struct wl_server_surface *surface;
        uint32_t surface_id;
        int fd;

        if (wl_request_u32(request, &surface_id) < 0 ||
            !wl_request_complete(request))
            return wl_protocol_fail(client, object->id,
                                    WL_PROTOCOL_ERROR_INVALID_METHOD,
                                    "malformed acquire fence");
        fd = wl_client_take_fd(client);
        surface_object = wl_client_find_object(client, surface_id);
        if (fd < 0 || !surface_object ||
            surface_object->type != WL_SERVER_OBJECT_SURFACE ||
            !(surface = surface_object->resource) || !surface->used ||
            surface->pending_acquire_fence_fd >= 0) {
            if (fd >= 0)
                close(fd);
            return wl_protocol_fail(client, object->id,
                                    WL_PROTOCOL_ERROR_INVALID_METHOD,
                                    "invalid acquire fence target");
        }
        surface->pending_acquire_fence_fd = fd;
        return 0;
    }
    if (opcode == 2u && wl_request_complete(request)) {
        wl_client_remove_object(client, object->id, true);
        return 0;
    }
    return wl_protocol_fail(client, object->id,
                            WL_PROTOCOL_ERROR_INVALID_METHOD,
                            "unsupported GPU buffer manager request");
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
        stride < width * 4u || (stride & 3u) != 0u ||
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
        wl_client_reclaim_buffers(client);
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
        for (size_t index = 0u;
             index < WL_SERVER_MAX_SURFACES; index++) {
            struct wl_server_surface *surface = &client->surfaces[index];
            struct wl_server_buffer *buffer = surface->current_buffer;

            if (!surface->used || !buffer || buffer->pool != pool)
                continue;
            surface->pixels = (uint32_t *)(void *)(
                pool->mapping + buffer->offset);
        }
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
    wl_client_reclaim_buffers(client);
    return 0;
}

static int wl_surface_add_callback(struct wl_server_client *client,
                                   struct wl_server_object *object,
                                   struct wl_server_surface *surface,
                                   struct wl_request *request)
{
    uint32_t new_id;
    size_t callback_count = 0u;

    if (wl_request_u32(request, &new_id) < 0 ||
        !wl_request_complete(request))
        return wl_protocol_fail(client, object->id,
                                WL_PROTOCOL_ERROR_INVALID_METHOD,
                                "malformed wl_surface.frame");
    for (size_t index = 0u; index < WL_SERVER_MAX_CALLBACKS; index++) {
        if (surface->callbacks[index].used)
            callback_count++;
        if (surface->pending_callbacks[index].used)
            callback_count++;
    }
    if (callback_count >= WL_SERVER_MAX_CALLBACKS)
        return wl_protocol_fail(client, object->id,
                                WL_PROTOCOL_ERROR_IMPLEMENTATION,
                                "frame callback limit reached");
    for (size_t index = 0; index < WL_SERVER_MAX_CALLBACKS; index++) {
        struct wl_server_callback *callback =
            &surface->pending_callbacks[index];

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

static bool wl_surface_effectively_synchronized(
    const struct wl_server_surface *surface)
{
    const struct wl_server_surface *ancestor = surface;
    size_t depth = 0u;

    while (ancestor && depth++ < WL_SERVER_MAX_SURFACES) {
        if (ancestor->role == WL_SERVER_SURFACE_ROLE_SUBSURFACE &&
            ancestor->subsurface_synchronized)
            return true;
        ancestor = ancestor->parent;
    }
    return false;
}

static int wl_surface_apply_commit_subtree(
    struct wl_server *server, struct wl_server_client *client,
    struct wl_server_surface *surface, bool commit_surface)
{
    bool was_mapped;
    bool scene_position_changed = false;

    if (!surface || !surface->used)
        return 0;
    was_mapped = surface->mapped;
    if (commit_surface) {
        int result = wl_surface_commit(server, client, surface);

        if (result < 0)
            return -1;
        if (result > 0)
            return result;
        surface->subsurface_commit_pending = false;
        if (!was_mapped && surface->mapped &&
            wl_server_surface_enter_output(
                client, surface->object_id) < 0)
            return -1;
    }
    for (size_t index = 0u; index < WL_SERVER_MAX_SURFACES; index++) {
        struct wl_server_surface *child = &client->surfaces[index];
        bool commit_child;

        if (!child->used || child->parent != surface)
            continue;
        if (child->subsurface_position_pending) {
            if (child->mapped &&
                (child->subsurface_x != child->pending_subsurface_x ||
                 child->subsurface_y != child->pending_subsurface_y))
                scene_position_changed = true;
            child->subsurface_x = child->pending_subsurface_x;
            child->subsurface_y = child->pending_subsurface_y;
            child->subsurface_position_pending = false;
        }
        commit_child = child->subsurface_commit_pending;
        int result = wl_surface_apply_commit_subtree(
            server, client, child, commit_child);

        if (result < 0)
            return -1;
        if (result > 0)
            return result;
    }
    if (scene_position_changed &&
        wl_server_schedule_render(server, true) < 0)
        return -1;
    return 0;
}

int wl_surface_apply_commit_tree(
    struct wl_server *server, struct wl_server_client *client,
    struct wl_server_surface *surface, bool commit_surface)
{
    int result = wl_surface_apply_commit_subtree(
        server, client, surface, commit_surface);

    if (client) {
        if (result > 0)
            client->blocked_commit_root = surface;
        else if (client->blocked_commit_root == surface)
            client->blocked_commit_root = NULL;
    }
    return result;
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
                wl_renderer_release_surface_gpu(
                    &server->renderer, child);
                child->parent = NULL;
                child->subsurface_synchronized = false;
                child->mapped = false;
            }
        }
        for (size_t index = 0u; index < WL_SERVER_MAX_OBJECTS; index++) {
            struct wl_server_object *role = &client->objects[index];

            if ((role->type == WL_SERVER_OBJECT_SUBSURFACE ||
                 role->type == WL_SERVER_OBJECT_XDG_POPUP ||
                 role->type == WL_SERVER_OBJECT_ARMOS_SHELL_PANEL ||
                 role->type ==
                    WL_SERVER_OBJECT_XDG_TOPLEVEL_DECORATION) &&
                role->resource == surface)
                role->resource = NULL;
        }
        if (surface->role == WL_SERVER_SURFACE_ROLE_PANEL &&
            server->shell_client == client) {
            server->shell_client = NULL;
            server->panel_height = 0u;
        }
        for (size_t index = 0u; index < WL_SERVER_MAX_CALLBACKS; index++) {
            if (surface->pending_callbacks[index].used)
                wl_client_remove_object(
                    client,
                    surface->pending_callbacks[index].object_id, true);
            if (surface->callbacks[index].used)
                wl_client_remove_object(
                    client, surface->callbacks[index].object_id, true);
        }
        if (wl_surface_release_buffer(client, surface) < 0)
            return -1;
        if (surface->acquire_fence_source)
            (void)wl_event_source_remove(surface->acquire_fence_source);
        if (surface->pending_acquire_fence_fd >= 0)
            close(surface->pending_acquire_fence_fd);
        wl_renderer_release_surface_gpu(&server->renderer, surface);
        memset(surface, 0, sizeof(*surface));
        wl_client_remove_object(client, object->id, true);
        wl_client_reclaim_buffers(client);
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
        struct wl_server_region *region_state = NULL;

        if (wl_request_u32(request, &region_id) < 0)
            return -1;
        if (region_id != 0u) {
            struct wl_server_object *region =
                wl_client_find_object(client, region_id);

            if (!region || region->type != WL_SERVER_OBJECT_REGION)
                return wl_protocol_fail(client, object->id,
                                        WL_PROTOCOL_ERROR_INVALID_OBJECT,
                                        "unknown wl_region");
            region_state = region->resource;
            if (!region_state || !region_state->used)
                return wl_protocol_fail(client, object->id,
                                        WL_PROTOCOL_ERROR_INVALID_OBJECT,
                                        "invalid wl_region");
        }
        if (opcode == 4u) {
            surface->pending_opaque_region_set = true;
            if (region_state)
                surface->pending_opaque_region = region_state->state;
            else
                memset(&surface->pending_opaque_region, 0,
                       sizeof(surface->pending_opaque_region));
        }
        return 0;
    }
    if (opcode == 6u && wl_request_complete(request)) {
        bool was_mapped = surface->mapped;

        if (surface->role == WL_SERVER_SURFACE_ROLE_TOPLEVEL ||
            surface->role == WL_SERVER_SURFACE_ROLE_POPUP) {
            if (!surface->xdg_initial_commit_received) {
                if (surface->pending_attach &&
                    surface->pending_buffer != NULL)
                    return wl_protocol_fail(
                        client, object->id,
                        WL_PROTOCOL_ERROR_INVALID_METHOD,
                        "buffer attached before initial xdg configure");
                surface->xdg_initial_commit_received = true;
                if (wl_surface_commit(server, client, surface) < 0)
                    return wl_protocol_fail(
                        client, object->id,
                        WL_PROTOCOL_ERROR_IMPLEMENTATION,
                        "initial surface commit failed");
                if (surface->role == WL_SERVER_SURFACE_ROLE_POPUP)
                    return wl_server_configure_popup(
                        server, client, surface);
                return wl_server_configure_toplevel(
                    server, client, surface, 0u, 0u, 0u);
            }
            if (surface->pending_attach &&
                surface->pending_buffer != NULL &&
                !surface->xdg_configure_acked)
                return wl_protocol_fail(
                    client, object->id,
                    WL_PROTOCOL_ERROR_INVALID_METHOD,
                    "buffer committed before xdg configure ack");
        }
        if (surface->role == WL_SERVER_SURFACE_ROLE_PANEL &&
            surface->pending_attach &&
            surface->pending_buffer != NULL) {
            if (surface->pending_buffer->width != surface->width ||
                surface->pending_buffer->height != surface->height)
                return wl_protocol_fail(
                    client, object->id,
                    WL_PROTOCOL_ERROR_INVALID_METHOD,
                    "panel buffer does not match configured size");
        }
        if (surface->role == WL_SERVER_SURFACE_ROLE_SUBSURFACE &&
            wl_surface_effectively_synchronized(surface)) {
            surface->subsurface_commit_pending = true;
            return 0;
        }
        if (wl_surface_apply_commit_tree(
                server, client, surface, true) < 0)
            return wl_protocol_fail(client, object->id,
                                    WL_PROTOCOL_ERROR_IMPLEMENTATION,
                                    "surface commit failed");
        (void)was_mapped;
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
    case WL_SERVER_OBJECT_ARMOS_GPU_BUFFER_MANAGER:
        return wl_dispatch_gpu_buffer_manager(
            server, client, object, opcode, &request);
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
    case WL_SERVER_OBJECT_XDG_POSITIONER:
        return wl_dispatch_xdg_positioner(client, object, opcode,
                                          &request);
    case WL_SERVER_OBJECT_XDG_SURFACE:
        return wl_dispatch_xdg_surface(server, client, object, opcode,
                                       &request);
    case WL_SERVER_OBJECT_XDG_TOPLEVEL:
        return wl_dispatch_xdg_toplevel(server, client, object, opcode,
                                        &request);
    case WL_SERVER_OBJECT_XDG_POPUP:
        return wl_dispatch_xdg_popup(server, client, object, opcode,
                                     &request);
    case WL_SERVER_OBJECT_XDG_DECORATION_MANAGER:
        return wl_dispatch_xdg_decoration_manager(client, object, opcode,
                                                  &request);
    case WL_SERVER_OBJECT_XDG_TOPLEVEL_DECORATION:
        return wl_dispatch_xdg_toplevel_decoration(
            server, client, object, opcode, &request);
    case WL_SERVER_OBJECT_ARMOS_SHELL:
        return wl_dispatch_armos_shell(
            server, client, object, opcode, &request);
    case WL_SERVER_OBJECT_ARMOS_SHELL_PANEL:
        return wl_dispatch_armos_shell_panel(
            server, client, object, opcode, &request);
    default:
        return wl_protocol_fail(client, object_id,
                                WL_PROTOCOL_ERROR_INVALID_METHOD,
                                "object does not accept requests");
    }
}
