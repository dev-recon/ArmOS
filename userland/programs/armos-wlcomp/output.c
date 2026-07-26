/*
 * ArmOS
 * Copyright (c) 2026 Mohamed Ennassiri
 *
 * Licensed under the Apache License, Version 2.0.
 * See LICENSE for details.
 *
 * File: userland/programs/armos-wlcomp/output.c
 * Layer: Userland / graphical services
 *
 * Responsibilities:
 * - Expose compositor output geometry through wl_output version 2.
 * - Derive the current mode from the architecture-neutral framebuffer ABI.
 * - Notify surfaces when they become visible on the compositor output.
 *
 * Notes:
 * - Hardware transport names never enter the Wayland protocol layer.
 * - The initial compositor exposes one logical output at scale factor one.
 */

#include "armos_wlcomp.h"

#include <errno.h>
#include <string.h>
#include <unistd.h>

#define WL_OUTPUT_MODE_CURRENT   1u
#define WL_OUTPUT_MODE_PREFERRED 2u
#define WL_OUTPUT_SCALE          1u
#define WL_OUTPUT_REFRESH_MHZ    60000u
#define WL_WIRE_HEADER_SIZE      8u
#define WL_OUTPUT_EVENT_MAX      256u

static int wl_output_write_full(int fd, const void *buffer, size_t size)
{
    const uint8_t *cursor = buffer;
    size_t written = 0u;

    while (written < size) {
        ssize_t count = write(fd, cursor + written, size - written);

        if (count < 0 && errno == EINTR)
            continue;
        if (count <= 0)
            return -1;
        written += (size_t)count;
    }
    return 0;
}

static int wl_output_send_geometry(struct wl_server_client *client,
                                   uint32_t output_id)
{
    static const char make[] = "ArmOS";
    static const char model[] = "Logical framebuffer";
    uint8_t message[WL_OUTPUT_EVENT_MAX];
    uint32_t make_size = sizeof(make);
    uint32_t model_size = sizeof(model);
    uint32_t make_padded = wl_wire_align(make_size);
    uint32_t model_padded = wl_wire_align(model_size);
    uint32_t size = WL_WIRE_HEADER_SIZE + 20u + 4u + make_padded +
        4u + model_padded + 4u;
    uint32_t cursor = WL_WIRE_HEADER_SIZE;

    if (!client || size > sizeof(message))
        return -1;
    memset(message, 0, size);
    wl_wire_store_u32(message, output_id);
    wl_wire_store_u32(message + 4u, (size << 16) | 0u);
    wl_wire_store_u32(message + cursor, 0u);
    cursor += 4u;
    wl_wire_store_u32(message + cursor, 0u);
    cursor += 4u;
    wl_wire_store_u32(message + cursor, 0u);
    cursor += 4u;
    wl_wire_store_u32(message + cursor, 0u);
    cursor += 4u;
    wl_wire_store_u32(message + cursor, 0u);
    cursor += 4u;
    wl_wire_store_u32(message + cursor, make_size);
    cursor += 4u;
    memcpy(message + cursor, make, make_size);
    cursor += make_padded;
    wl_wire_store_u32(message + cursor, model_size);
    cursor += 4u;
    memcpy(message + cursor, model, model_size);
    cursor += model_padded;
    wl_wire_store_u32(message + cursor, 0u);
    return wl_output_write_full(client->fd, message, size);
}

int wl_server_bind_output(struct wl_server *server,
                          struct wl_server_client *client,
                          uint32_t output_id, uint32_t version)
{
    uint32_t mode[4];
    uint32_t scale = WL_OUTPUT_SCALE;

    if (!server || !client || version == 0u)
        return -1;
    if (version > 2u)
        version = 2u;
    if (wl_client_add_object(client, output_id, WL_SERVER_OBJECT_OUTPUT,
                             version, NULL) < 0 ||
        wl_output_send_geometry(client, output_id) < 0)
        return -1;
    mode[0] = WL_OUTPUT_MODE_CURRENT | WL_OUTPUT_MODE_PREFERRED;
    mode[1] = server->renderer.framebuffer.width;
    mode[2] = server->renderer.framebuffer.height;
    mode[3] = WL_OUTPUT_REFRESH_MHZ;
    if (wl_client_send_words(client, output_id, 1u, mode, 4u) < 0)
        return -1;
    if (version >= 2u &&
        (wl_client_send_words(client, output_id, 3u, &scale, 1u) < 0 ||
         wl_client_send_words(client, output_id, 2u, NULL, 0u) < 0))
        return -1;
    return 0;
}

int wl_server_surface_enter_output(struct wl_server_client *client,
                                   uint32_t surface_id)
{
    for (size_t index = 0u; index < WL_SERVER_MAX_OBJECTS; index++) {
        struct wl_server_object *object = &client->objects[index];

        if (object->type == WL_SERVER_OBJECT_OUTPUT)
            return wl_client_send_words(client, surface_id, 0u,
                                        &object->id, 1u);
    }
    return 0;
}
