/*
 * ArmOS
 * Copyright (c) 2026 Mohamed Ennassiri
 *
 * Licensed under the Apache License, Version 2.0.
 * See LICENSE for details.
 *
 * File: userland/programs/armos-wlcomp/renderer.c
 * Layer: Userland / graphical services
 *
 * Responsibilities:
 * - Composite committed Wayland SHM surfaces into an ARGB8888 canvas.
 * - Present the canvas through the architecture-neutral framebuffer ABI.
 * - Retain committed pixels independently from client buffer lifetimes.
 *
 * Notes:
 * - Headless mode uses the same compositor path without touching /dev/fb0.
 * - Surface placement is temporary policy for the first server milestone.
 */

#include "armos_wlcomp.h"

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#define WL_HEADLESS_WIDTH  800u
#define WL_HEADLESS_HEIGHT 600u
#define WL_BACKGROUND      0xffd8dde5u
#define WL_WINDOW_TITLE_HEIGHT 28u
#define WL_WINDOW_RADIUS       10u

static const char *const wl_pointer_shape[] = {
    "X...........",
    "XX..........",
    "XOX.........",
    "XOOX........",
    "XOOOX.......",
    "XOOOOX......",
    "XOOOOOX.....",
    "XOOOOOOX....",
    "XOOOOOOOX...",
    "XOOOOXXXXX..",
    "XOOXOOX.....",
    "XOX.XOOX....",
    "XX..XOOX....",
    "X....XOOX...",
    ".....XOOX...",
    "......XX...."
};

#define WL_POINTER_WIDTH  12u
#define WL_POINTER_HEIGHT 16u

static int wl_write_full(int fd, const void *buffer, size_t size)
{
    const uint8_t *cursor = (const uint8_t *)buffer;
    size_t done = 0;

    while (done < size) {
        ssize_t count = write(fd, cursor + done, size - done);

        if (count < 0 && errno == EINTR)
            continue;
        if (count <= 0)
            return -1;
        done += (size_t)count;
    }
    return 0;
}

int wl_renderer_init(struct wl_server_renderer *renderer, bool headless)
{
    if (!renderer)
        return -1;
    memset(renderer, 0, sizeof(*renderer));
    renderer->framebuffer_fd = -1;
    renderer->headless = headless;

    if (headless) {
        renderer->framebuffer.width = WL_HEADLESS_WIDTH;
        renderer->framebuffer.height = WL_HEADLESS_HEIGHT;
        renderer->framebuffer.pitch = WL_HEADLESS_WIDTH * 4u;
        renderer->framebuffer.bpp = 32u;
        renderer->framebuffer.size =
            renderer->framebuffer.pitch * WL_HEADLESS_HEIGHT;
        renderer->framebuffer.format = ARMOS_FB_FORMAT_ARGB8888;
    } else {
        renderer->framebuffer_fd = open("/dev/fb0", O_RDWR, 0);
        if (renderer->framebuffer_fd < 0)
            return -1;
        if (ioctl(renderer->framebuffer_fd, ARMOS_FBIOACQUIRE, NULL) < 0)
            goto fail;
        if (ioctl(renderer->framebuffer_fd, ARMOS_FBIOGET_INFO,
                  &renderer->framebuffer) < 0)
            goto fail;
        if (renderer->framebuffer.bpp != 32u ||
            renderer->framebuffer.format != ARMOS_FB_FORMAT_ARGB8888 ||
            renderer->framebuffer.pitch < renderer->framebuffer.width * 4u ||
            renderer->framebuffer.size <
                renderer->framebuffer.pitch * renderer->framebuffer.height) {
            errno = ENOTSUP;
            goto fail;
        }
    }

    renderer->canvas_size = renderer->framebuffer.size;
    renderer->canvas = malloc(renderer->canvas_size);
    if (!renderer->canvas)
        goto fail;
    return 0;

fail:
    wl_renderer_destroy(renderer);
    return -1;
}

void wl_renderer_destroy(struct wl_server_renderer *renderer)
{
    if (!renderer)
        return;
    free(renderer->canvas);
    renderer->canvas = NULL;
    if (renderer->framebuffer_fd >= 0)
        close(renderer->framebuffer_fd);
    renderer->framebuffer_fd = -1;
}

static uint32_t wl_blend_pixel(uint32_t destination, uint32_t source)
{
    uint32_t alpha = source >> 24;
    uint32_t inverse;
    uint32_t red;
    uint32_t green;
    uint32_t blue;

    if (alpha == 255u)
        return source;
    if (alpha == 0u)
        return destination;

    inverse = 255u - alpha;
    red = ((((source >> 16) & 0xffu) * alpha) +
           (((destination >> 16) & 0xffu) * inverse)) / 255u;
    green = ((((source >> 8) & 0xffu) * alpha) +
             (((destination >> 8) & 0xffu) * inverse)) / 255u;
    blue = (((source & 0xffu) * alpha) +
            ((destination & 0xffu) * inverse)) / 255u;
    return 0xff000000u | (red << 16) | (green << 8) | blue;
}

static void wl_renderer_put_pixel(struct wl_server_renderer *renderer,
                                  int32_t x, int32_t y, uint32_t color)
{
    uint32_t canvas_width = renderer->framebuffer.pitch / 4u;

    if (x < 0 || y < 0 ||
        (uint32_t)x >= renderer->framebuffer.width ||
        (uint32_t)y >= renderer->framebuffer.height)
        return;
    if (renderer->clip_enabled &&
        (x < renderer->clip_x0 || y < renderer->clip_y0 ||
         x >= renderer->clip_x1 || y >= renderer->clip_y1))
        return;
    renderer->canvas[(uint32_t)y * canvas_width + (uint32_t)x] =
        wl_blend_pixel(
            renderer->canvas[(uint32_t)y * canvas_width + (uint32_t)x],
            color);
}

static int wl_point_in_rounded_rect(uint32_t x, uint32_t y,
                                    uint32_t width, uint32_t height,
                                    uint32_t radius)
{
    int32_t dx;
    int32_t dy;

    if (x >= width || y >= height)
        return 0;
    if ((x >= radius && x < width - radius) ||
        (y >= radius && y < height - radius))
        return 1;
    dx = x < radius ? (int32_t)radius - 1 - (int32_t)x :
         (int32_t)x - (int32_t)(width - radius);
    dy = y < radius ? (int32_t)radius - 1 - (int32_t)y :
         (int32_t)y - (int32_t)(height - radius);
    return dx * dx + dy * dy < (int32_t)(radius * radius);
}

static void wl_renderer_rounded_rect(struct wl_server_renderer *renderer,
                                     int32_t x, int32_t y,
                                     uint32_t width, uint32_t height,
                                     uint32_t radius, uint32_t color)
{
    if (width <= radius * 2u || height <= radius * 2u)
        radius = 0u;
    for (uint32_t row = 0; row < height; row++) {
        for (uint32_t column = 0; column < width; column++) {
            if (radius == 0u ||
                wl_point_in_rounded_rect(column, row, width, height, radius)) {
                wl_renderer_put_pixel(renderer, x + (int32_t)column,
                                      y + (int32_t)row, color);
            }
        }
    }
}

static void wl_renderer_circle(struct wl_server_renderer *renderer,
                               int32_t center_x, int32_t center_y,
                               uint32_t radius, uint32_t color)
{
    int32_t limit = (int32_t)radius;

    for (int32_t y = -limit; y <= limit; y++) {
        for (int32_t x = -limit; x <= limit; x++) {
            if (x * x + y * y <= (int32_t)(radius * radius))
                wl_renderer_put_pixel(renderer, center_x + x, center_y + y,
                                      color);
        }
    }
}

static void wl_renderer_draw_surface(struct wl_server_renderer *renderer,
                                     const struct wl_server_surface *surface)
{
    uint32_t frame_width = surface->width;
    uint32_t frame_height = surface->height + WL_WINDOW_TITLE_HEIGHT;
    int32_t content_y = surface->y + (int32_t)WL_WINDOW_TITLE_HEIGHT;

    /* Soft multi-pass shadow, generated rather than sourced from an asset. */
    for (uint32_t spread = 8u; spread > 0u; spread -= 2u) {
        uint32_t alpha = 8u + (8u - spread) * 3u;

        wl_renderer_rounded_rect(
            renderer, surface->x - (int32_t)spread,
            surface->y - (int32_t)spread + 4,
            frame_width + spread * 2u, frame_height + spread * 2u,
            WL_WINDOW_RADIUS + spread,
            (alpha << 24));
    }
    wl_renderer_rounded_rect(renderer, surface->x, surface->y,
                             frame_width, frame_height, WL_WINDOW_RADIUS,
                             0xfff4f4f5u);
    wl_renderer_rounded_rect(renderer, surface->x + 1, surface->y + 1,
                             frame_width > 2u ? frame_width - 2u : frame_width,
                             WL_WINDOW_TITLE_HEIGHT,
                             WL_WINDOW_RADIUS > 1u ?
                                 WL_WINDOW_RADIUS - 1u : 0u,
                             0xffe8e8eau);
    wl_renderer_circle(renderer, surface->x + 14, surface->y + 14,
                       5u, 0xffff5f57u);
    wl_renderer_circle(renderer, surface->x + 30, surface->y + 14,
                       5u, 0xffffbd2eu);
    wl_renderer_circle(renderer, surface->x + 46, surface->y + 14,
                       5u, 0xff28c840u);

    for (uint32_t source_y = 0; source_y < surface->height; source_y++) {
        int32_t target_y = content_y + (int32_t)source_y;

        if (target_y < 0 ||
            (uint32_t)target_y >= renderer->framebuffer.height)
            continue;
        for (uint32_t source_x = 0; source_x < surface->width; source_x++) {
            int32_t target_x = surface->x + (int32_t)source_x;
            uint32_t source;

            if (target_x < 0 ||
                (uint32_t)target_x >= renderer->framebuffer.width)
                continue;
            source = surface->pixels[source_y * surface->width + source_x];
            wl_renderer_put_pixel(renderer, target_x, target_y, source);
        }
    }
}

static void wl_renderer_capture_pointer(struct wl_server *server)
{
    struct wl_server_renderer *renderer = &server->renderer;
    uint32_t canvas_width = renderer->framebuffer.pitch / sizeof(uint32_t);

    for (uint32_t row = 0u; row < WL_POINTER_HEIGHT; row++) {
        for (uint32_t column = 0u; column < WL_POINTER_WIDTH; column++) {
            int32_t x = server->pointer_x + (int32_t)column;
            int32_t y = server->pointer_y + (int32_t)row;
            uint32_t pixel = WL_BACKGROUND;

            if (x >= 0 && y >= 0 &&
                (uint32_t)x < renderer->framebuffer.width &&
                (uint32_t)y < renderer->framebuffer.height)
                pixel = renderer->canvas[(uint32_t)y * canvas_width +
                                         (uint32_t)x];
            renderer->pointer_backing[row * WL_POINTER_WIDTH + column] =
                pixel;
        }
    }
    renderer->pointer_backing_valid = true;
}

static void wl_renderer_restore_pointer(struct wl_server *server)
{
    struct wl_server_renderer *renderer = &server->renderer;
    uint32_t canvas_width = renderer->framebuffer.pitch / sizeof(uint32_t);

    if (!renderer->pointer_backing_valid)
        return;
    for (uint32_t row = 0u; row < WL_POINTER_HEIGHT; row++) {
        for (uint32_t column = 0u; column < WL_POINTER_WIDTH; column++) {
            int32_t x = server->presented_pointer_x + (int32_t)column;
            int32_t y = server->presented_pointer_y + (int32_t)row;

            if (x >= 0 && y >= 0 &&
                (uint32_t)x < renderer->framebuffer.width &&
                (uint32_t)y < renderer->framebuffer.height) {
                renderer->canvas[(uint32_t)y * canvas_width + (uint32_t)x] =
                    renderer->pointer_backing[
                        row * WL_POINTER_WIDTH + column];
            }
        }
    }
}

static void wl_renderer_draw_pointer(struct wl_server *server)
{
    struct wl_server_renderer *renderer = &server->renderer;
    int32_t x = server->pointer_x;
    int32_t y = server->pointer_y;

    wl_renderer_capture_pointer(server);
    for (size_t row = 0;
         row < sizeof(wl_pointer_shape) / sizeof(wl_pointer_shape[0]);
         row++) {
        for (size_t column = 0;
             wl_pointer_shape[row][column] != '\0'; column++) {
            char pixel = wl_pointer_shape[row][column];

            if (pixel != '.')
                wl_renderer_put_pixel(
                    renderer, x + (int32_t)column, y + (int32_t)row,
                    pixel == 'X' ? 0xff101010u : 0xfffafafau);
        }
    }
}

static void wl_renderer_draw_surfaces(struct wl_server *server)
{
    for (size_t client_index = 0;
         client_index < WL_SERVER_MAX_CLIENTS; client_index++) {
        struct wl_server_client *client = &server->clients[client_index];

        if (!client->used)
            continue;
        for (size_t index = 0; index < WL_SERVER_MAX_SURFACES; index++) {
            struct wl_server_surface *surface = &client->surfaces[index];

            if (surface->used && surface->mapped && surface->pixels)
                wl_renderer_draw_surface(&server->renderer, surface);
        }
    }
}

static int wl_renderer_build_canvas(struct wl_server *server)
{
    struct wl_server_renderer *renderer;
    uint32_t canvas_width;

    if (!server || !server->renderer.canvas)
        return -1;
    renderer = &server->renderer;
    renderer->clip_enabled = false;
    canvas_width = renderer->framebuffer.pitch / 4u;
    for (uint32_t y = 0; y < renderer->framebuffer.height; y++) {
        for (uint32_t x = 0; x < canvas_width; x++)
            renderer->canvas[y * canvas_width + x] = WL_BACKGROUND;
    }

    wl_renderer_draw_surfaces(server);

    if (!renderer->headless)
        wl_renderer_draw_pointer(server);

    return 0;
}

static int wl_renderer_present_rect(struct wl_server_renderer *renderer,
                                    int32_t x, int32_t y,
                                    uint32_t width, uint32_t height)
{
    int32_t x1;
    int32_t y1;

    if (!renderer || renderer->headless || width == 0u || height == 0u)
        return 0;
    x1 = x + (int32_t)width;
    y1 = y + (int32_t)height;
    if (x < 0)
        x = 0;
    if (y < 0)
        y = 0;
    if (x1 > (int32_t)renderer->framebuffer.width)
        x1 = (int32_t)renderer->framebuffer.width;
    if (y1 > (int32_t)renderer->framebuffer.height)
        y1 = (int32_t)renderer->framebuffer.height;
    if (x >= x1 || y >= y1)
        return 0;
    width = (uint32_t)(x1 - x);
    for (int32_t row = y; row < y1; row++) {
        off_t offset = (off_t)(uint32_t)row * renderer->framebuffer.pitch +
            (off_t)(uint32_t)x * sizeof(uint32_t);
        const uint8_t *source = (const uint8_t *)renderer->canvas + offset;

        if (lseek(renderer->framebuffer_fd, offset, SEEK_SET) < 0 ||
            wl_write_full(renderer->framebuffer_fd, source,
                          (size_t)width * sizeof(uint32_t)) < 0)
            return -1;
    }
    return 0;
}

int wl_renderer_compose(struct wl_server *server)
{
    struct wl_server_renderer *renderer;

    if (wl_renderer_build_canvas(server) < 0)
        return -1;
    renderer = &server->renderer;
    server->pointer_presented = true;
    server->presented_pointer_x = server->pointer_x;
    server->presented_pointer_y = server->pointer_y;
    if (renderer->headless)
        return 0;
    if (lseek(renderer->framebuffer_fd, 0, SEEK_SET) < 0)
        return -1;
    return wl_write_full(renderer->framebuffer_fd, renderer->canvas,
                         renderer->canvas_size);
}

int wl_renderer_compose_pointer(struct wl_server *server)
{
    int32_t old_x;
    int32_t old_y;

    if (!server)
        return -1;
    if (!server->pointer_presented)
        return wl_renderer_compose(server);
    old_x = server->presented_pointer_x;
    old_y = server->presented_pointer_y;
    wl_renderer_restore_pointer(server);
    wl_renderer_draw_pointer(server);
    if (wl_renderer_present_rect(&server->renderer, old_x, old_y,
                                 WL_POINTER_WIDTH,
                                 WL_POINTER_HEIGHT) < 0)
        return -1;
    if ((old_x != server->pointer_x || old_y != server->pointer_y) &&
        wl_renderer_present_rect(&server->renderer,
                                 server->pointer_x, server->pointer_y,
                                 WL_POINTER_WIDTH,
                                 WL_POINTER_HEIGHT) < 0)
        return -1;
    server->presented_pointer_x = server->pointer_x;
    server->presented_pointer_y = server->pointer_y;
    return 0;
}

struct wl_renderer_rect {
    int32_t x0;
    int32_t y0;
    int32_t x1;
    int32_t y1;
};

static void wl_renderer_rect_add(struct wl_renderer_rect *damage,
                                 int32_t x, int32_t y,
                                 uint32_t width, uint32_t height)
{
    int32_t x1 = x + (int32_t)width;
    int32_t y1 = y + (int32_t)height;

    if (x < damage->x0)
        damage->x0 = x;
    if (y < damage->y0)
        damage->y0 = y;
    if (x1 > damage->x1)
        damage->x1 = x1;
    if (y1 > damage->y1)
        damage->y1 = y1;
}

static int wl_renderer_clip_rect(struct wl_server_renderer *renderer,
                                 struct wl_renderer_rect *damage)
{
    if (damage->x0 < 0)
        damage->x0 = 0;
    if (damage->y0 < 0)
        damage->y0 = 0;
    if (damage->x1 > (int32_t)renderer->framebuffer.width)
        damage->x1 = (int32_t)renderer->framebuffer.width;
    if (damage->y1 > (int32_t)renderer->framebuffer.height)
        damage->y1 = (int32_t)renderer->framebuffer.height;
    return damage->x0 < damage->x1 && damage->y0 < damage->y1;
}

static void wl_renderer_clear_rect(struct wl_server_renderer *renderer,
                                   const struct wl_renderer_rect *damage)
{
    uint32_t canvas_width = renderer->framebuffer.pitch / sizeof(uint32_t);

    for (int32_t y = damage->y0; y < damage->y1; y++) {
        uint32_t *row = renderer->canvas +
            (uint32_t)y * canvas_width + (uint32_t)damage->x0;

        for (int32_t x = damage->x0; x < damage->x1; x++)
            *row++ = WL_BACKGROUND;
    }
}

int wl_renderer_compose_move(struct wl_server *server)
{
    struct wl_server_renderer *renderer;
    struct wl_server_surface *surface;
    struct wl_renderer_rect damage;
    uint32_t damage_width;
    uint32_t damage_height;
    int result;

    if (!server || !server->renderer.canvas)
        return -1;
    renderer = &server->renderer;
    surface = server->move_surface;
    if (!server->move_damage_pending || !surface || !surface->used ||
        !surface->mapped || !surface->pixels) {
        server->move_damage_pending = false;
        server->move_client = NULL;
        server->move_surface = NULL;
        return wl_renderer_compose(server);
    }

    damage.x0 = server->move_old_x - 8;
    damage.y0 = server->move_old_y - 4;
    damage.x1 = server->move_old_x + (int32_t)surface->width + 8;
    damage.y1 = server->move_old_y + (int32_t)surface->height +
        (int32_t)WL_WINDOW_TITLE_HEIGHT + 12;
    wl_renderer_rect_add(&damage, surface->x - 8, surface->y - 4,
                         surface->width + 16u,
                         surface->height + WL_WINDOW_TITLE_HEIGHT + 16u);
    wl_renderer_rect_add(&damage, server->presented_pointer_x,
                         server->presented_pointer_y,
                         WL_POINTER_WIDTH, WL_POINTER_HEIGHT);
    wl_renderer_rect_add(&damage, server->pointer_x, server->pointer_y,
                         WL_POINTER_WIDTH, WL_POINTER_HEIGHT);
    if (!wl_renderer_clip_rect(renderer, &damage)) {
        server->move_damage_pending = false;
        server->move_client = NULL;
        server->move_surface = NULL;
        return 0;
    }

    wl_renderer_restore_pointer(server);
    wl_renderer_clear_rect(renderer, &damage);
    renderer->clip_enabled = true;
    renderer->clip_x0 = damage.x0;
    renderer->clip_y0 = damage.y0;
    renderer->clip_x1 = damage.x1;
    renderer->clip_y1 = damage.y1;
    wl_renderer_draw_surfaces(server);
    renderer->clip_enabled = false;
    wl_renderer_draw_pointer(server);

    damage_width = (uint32_t)(damage.x1 - damage.x0);
    damage_height = (uint32_t)(damage.y1 - damage.y0);
    result = wl_renderer_present_rect(renderer, damage.x0, damage.y0,
                                      damage_width, damage_height);
    if (result < 0)
        return result;
    server->pointer_presented = true;
    server->presented_pointer_x = server->pointer_x;
    server->presented_pointer_y = server->pointer_y;
    server->move_damage_pending = false;
    server->move_client = NULL;
    server->move_surface = NULL;
    return 0;
}

static int wl_surface_buffer_valid(const struct wl_server_buffer *buffer)
{
    uint64_t last_row;
    uint64_t end;

    if (!buffer || !buffer->used || !buffer->pool ||
        !buffer->pool->used || !buffer->pool->mapping ||
        buffer->width == 0 || buffer->height == 0 ||
        buffer->stride < buffer->width * 4u)
        return 0;
    last_row = (uint64_t)buffer->offset +
               (uint64_t)(buffer->height - 1u) * buffer->stride;
    end = last_row + (uint64_t)buffer->width * 4u;
    return end <= buffer->pool->size;
}

int wl_surface_commit(struct wl_server *server,
                      struct wl_server_client *client,
                      struct wl_server_surface *surface)
{
    struct wl_server_buffer *buffer;

    if (!server || !client || !surface)
        return -1;
    if (surface->pending_attach) {
        buffer = surface->pending_buffer;
        surface->pending_attach = false;
        surface->pending_buffer = NULL;
        if (!buffer) {
            free(surface->pixels);
            surface->pixels = NULL;
            surface->pixels_size = 0;
            surface->mapped = false;
            surface->width = 0;
            surface->height = 0;
        } else {
            uint64_t pixel_bytes =
                (uint64_t)buffer->width * buffer->height * 4u;
            uint32_t *copy;

            if (!wl_surface_buffer_valid(buffer) ||
                pixel_bytes > (uint64_t)SIZE_MAX)
                return -1;
            copy = malloc((size_t)pixel_bytes);
            if (!copy)
                return -1;
            for (uint32_t y = 0; y < buffer->height; y++) {
                const uint8_t *source = buffer->pool->mapping +
                    buffer->offset + (size_t)y * buffer->stride;
                uint32_t *destination = copy + (size_t)y * buffer->width;

                memcpy(destination, source, (size_t)buffer->width * 4u);
                if (buffer->format == WL_SHM_FORMAT_XRGB8888) {
                    for (uint32_t x = 0; x < buffer->width; x++)
                        destination[x] |= 0xff000000u;
                }
            }
            free(surface->pixels);
            surface->pixels = copy;
            surface->pixels_size = (size_t)pixel_bytes;
            surface->width = buffer->width;
            surface->height = buffer->height;
            surface->mapped = true;
            if (buffer->object_alive) {
                (void)wl_client_send_words(client, buffer->object_id, 0,
                                           NULL, 0);
            }
        }
    }

    if (wl_renderer_compose(server) < 0)
        return -1;
    for (size_t index = 0; index < WL_SERVER_MAX_CALLBACKS; index++) {
        struct wl_server_callback *callback = &surface->callbacks[index];
        uint32_t done;

        if (!callback->used)
            continue;
        done = ++server->serial;
        if (wl_client_send_words(client, callback->object_id, 0, &done, 1) < 0)
            return -1;
        wl_client_remove_object(client, callback->object_id, true);
        memset(callback, 0, sizeof(*callback));
    }
    return 0;
}
