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
#define WL_WINDOW_TITLE_ACTIVE   0xff5ac8fau
#define WL_WINDOW_TITLE_INACTIVE 0xffe8e8eau

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

static bool wl_renderer_local_clip(
    const struct wl_server_renderer *renderer,
    int32_t x, int32_t y, uint32_t width, uint32_t height,
    uint32_t *column0, uint32_t *row0,
    uint32_t *column1, uint32_t *row1)
{
    int32_t x0 = x < 0 ? -x : 0;
    int32_t y0 = y < 0 ? -y : 0;
    int32_t x1 = (int32_t)width;
    int32_t y1 = (int32_t)height;

    if (x + x1 > (int32_t)renderer->framebuffer.width)
        x1 = (int32_t)renderer->framebuffer.width - x;
    if (y + y1 > (int32_t)renderer->framebuffer.height)
        y1 = (int32_t)renderer->framebuffer.height - y;
    if (renderer->clip_enabled) {
        if (x + x0 < renderer->clip_x0)
            x0 = renderer->clip_x0 - x;
        if (y + y0 < renderer->clip_y0)
            y0 = renderer->clip_y0 - y;
        if (x + x1 > renderer->clip_x1)
            x1 = renderer->clip_x1 - x;
        if (y + y1 > renderer->clip_y1)
            y1 = renderer->clip_y1 - y;
    }
    if (x0 >= x1 || y0 >= y1)
        return false;
    *column0 = (uint32_t)x0;
    *row0 = (uint32_t)y0;
    *column1 = (uint32_t)x1;
    *row1 = (uint32_t)y1;
    return true;
}

static void wl_renderer_rounded_rect(struct wl_server_renderer *renderer,
                                     int32_t x, int32_t y,
                                     uint32_t width, uint32_t height,
                                     uint32_t radius, uint32_t color)
{
    uint32_t column0;
    uint32_t row0;
    uint32_t column1;
    uint32_t row1;

    if (width <= radius * 2u || height <= radius * 2u)
        radius = 0u;
    if (!wl_renderer_local_clip(renderer, x, y, width, height,
                                &column0, &row0, &column1, &row1))
        return;
    for (uint32_t row = row0; row < row1; row++) {
        for (uint32_t column = column0; column < column1; column++) {
            if (radius == 0u ||
                wl_point_in_rounded_rect(column, row, width, height, radius)) {
                wl_renderer_put_pixel(renderer, x + (int32_t)column,
                                      y + (int32_t)row, color);
            }
        }
    }
}

static void wl_renderer_shadow_ring(
    struct wl_server_renderer *renderer,
    int32_t x, int32_t y, uint32_t width, uint32_t height,
    uint32_t radius, int32_t inner_x, int32_t inner_y,
    uint32_t inner_width, uint32_t inner_height,
    uint32_t inner_radius, uint32_t color)
{
    uint32_t column0;
    uint32_t row0;
    uint32_t column1;
    uint32_t row1;

    if (!wl_renderer_local_clip(renderer, x, y, width, height,
                                &column0, &row0, &column1, &row1))
        return;
    for (uint32_t row = row0; row < row1; row++) {
        for (uint32_t column = column0; column < column1; column++) {
            int32_t screen_x = x + (int32_t)column;
            int32_t screen_y = y + (int32_t)row;
            bool shadow_side =
                screen_x >= inner_x + (int32_t)inner_width ||
                screen_y >= inner_y + (int32_t)inner_height;
            bool hidden_by_window =
                screen_x >= inner_x && screen_y >= inner_y &&
                screen_x < inner_x + (int32_t)inner_width &&
                screen_y < inner_y + (int32_t)inner_height &&
                wl_point_in_rounded_rect(
                    (uint32_t)(screen_x - inner_x),
                    (uint32_t)(screen_y - inner_y),
                    inner_width, inner_height, inner_radius);

            if (shadow_side && !hidden_by_window &&
                wl_point_in_rounded_rect(column, row, width, height, radius))
                wl_renderer_put_pixel(renderer, screen_x, screen_y, color);
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

static uint8_t wl_title_glyph_row(char character, uint32_t row)
{
    static const uint8_t letters[26][7] = {
        {14,17,17,31,17,17,17}, {30,17,17,30,17,17,30},
        {14,17,16,16,16,17,14}, {30,17,17,17,17,17,30},
        {31,16,16,30,16,16,31}, {31,16,16,30,16,16,16},
        {14,17,16,23,17,17,15}, {17,17,17,31,17,17,17},
        {14,4,4,4,4,4,14}, {7,2,2,2,18,18,12},
        {17,18,20,24,20,18,17}, {16,16,16,16,16,16,31},
        {17,27,21,21,17,17,17}, {17,25,21,19,17,17,17},
        {14,17,17,17,17,17,14}, {30,17,17,30,16,16,16},
        {14,17,17,17,21,18,13}, {30,17,17,30,20,18,17},
        {15,16,16,14,1,1,30}, {31,4,4,4,4,4,4},
        {17,17,17,17,17,17,14}, {17,17,17,17,17,10,4},
        {17,17,17,21,21,21,10}, {17,17,10,4,10,17,17},
        {17,17,10,4,4,4,4}, {31,1,2,4,8,16,31}
    };
    static const uint8_t digits[10][7] = {
        {14,17,19,21,25,17,14}, {4,12,4,4,4,4,14},
        {14,17,1,2,4,8,31}, {30,1,1,14,1,1,30},
        {2,6,10,18,31,2,2}, {31,16,16,30,1,1,30},
        {14,16,16,30,17,17,14}, {31,1,2,4,8,8,8},
        {14,17,17,14,17,17,14}, {14,17,17,15,1,1,14}
    };

    if (row >= 7u)
        return 0u;
    if (character >= 'a' && character <= 'z')
        character = (char)(character - 'a' + 'A');
    if (character >= 'A' && character <= 'Z')
        return letters[(uint32_t)(character - 'A')][row];
    if (character >= '0' && character <= '9')
        return digits[(uint32_t)(character - '0')][row];
    if (character == '-')
        return row == 3u ? 31u : 0u;
    if (character == '.')
        return row == 6u ? 4u : 0u;
    return 0u;
}

static void wl_renderer_title(struct wl_server_renderer *renderer,
                              const struct wl_server_surface *surface)
{
    const uint32_t glyph_width = 6u;
    uint32_t maximum = surface->width > 70u ?
        (surface->width - 70u) / glyph_width : 0u;
    size_t length = strnlen(surface->title, sizeof(surface->title));
    int32_t x;

    if (length > maximum)
        length = maximum;
    x = surface->x + (int32_t)((surface->width -
                                (uint32_t)length * glyph_width) / 2u);
    if (x < surface->x + 62)
        x = surface->x + 62;
    for (size_t character = 0u; character < length; character++) {
        for (uint32_t row = 0u; row < 7u; row++) {
            uint8_t bits = wl_title_glyph_row(
                surface->title[character], row);

            for (uint32_t column = 0u; column < 5u; column++) {
                if (bits & (1u << (4u - column))) {
                    wl_renderer_put_pixel(
                        renderer, x + (int32_t)column,
                        surface->y + 10 + (int32_t)row,
                        0xff343438u);
                }
            }
        }
        x += (int32_t)glyph_width;
    }
}

static void wl_renderer_draw_surface(struct wl_server_renderer *renderer,
                                     const struct wl_server_surface *surface,
                                     bool active)
{
    uint32_t frame_width = surface->width;
    uint32_t frame_height = surface->height + WL_WINDOW_TITLE_HEIGHT;
    int32_t content_y = surface->y + (int32_t)WL_WINDOW_TITLE_HEIGHT;
    int32_t content_x = surface->x;
    bool content_only_damage = false;

    if (surface->is_subsurface) {
        const struct wl_server_surface *ancestor = surface;
        size_t depth = 0u;

        content_x = surface->subsurface_x;
        content_y = surface->subsurface_y;
        while (ancestor->is_subsurface && ancestor->parent &&
               depth++ < WL_SERVER_MAX_SURFACES) {
            ancestor = ancestor->parent;
            if (ancestor->is_subsurface) {
                content_x += ancestor->subsurface_x;
                content_y += ancestor->subsurface_y;
            } else {
                content_x += ancestor->x;
                content_y += ancestor->y +
                    (int32_t)WL_WINDOW_TITLE_HEIGHT;
            }
        }
        if (depth > WL_SERVER_MAX_SURFACES)
            return;
        goto draw_content;
    }

    if (renderer->clip_enabled && surface->opaque &&
        renderer->clip_x0 >= content_x &&
        renderer->clip_y0 >= content_y &&
        renderer->clip_x1 <= content_x + (int32_t)surface->width &&
        renderer->clip_y1 <= content_y + (int32_t)surface->height)
        content_only_damage = true;

    if (!content_only_damage) {
        /* Soft multi-pass shadow, generated rather than sourced from an asset. */
        for (uint32_t spread = 8u; spread > 0u; spread -= 2u) {
            uint32_t alpha = 8u + (8u - spread) * 3u;

            wl_renderer_shadow_ring(
                renderer, surface->x - (int32_t)spread,
                surface->y - (int32_t)spread + 4,
                frame_width + spread * 2u, frame_height + spread * 2u,
                WL_WINDOW_RADIUS + spread,
                surface->x, surface->y, frame_width, frame_height,
                WL_WINDOW_RADIUS,
                (alpha << 24));
        }
        wl_renderer_rounded_rect(renderer, surface->x, surface->y,
                                 frame_width, frame_height, WL_WINDOW_RADIUS,
                                 0xfff4f4f5u);
        wl_renderer_rounded_rect(
            renderer, surface->x + 1, surface->y + 1,
            frame_width > 2u ? frame_width - 2u : frame_width,
            WL_WINDOW_TITLE_HEIGHT,
            WL_WINDOW_RADIUS > 1u ? WL_WINDOW_RADIUS - 1u : 0u,
            active ? WL_WINDOW_TITLE_ACTIVE : WL_WINDOW_TITLE_INACTIVE);
        wl_renderer_circle(renderer, surface->x + 14, surface->y + 14,
                           5u, 0xffff5f57u);
        wl_renderer_circle(renderer, surface->x + 30, surface->y + 14,
                           5u, 0xffffbd2eu);
        wl_renderer_circle(renderer, surface->x + 46, surface->y + 14,
                           5u, 0xff28c840u);
        wl_renderer_title(renderer, surface);
    }

draw_content:
    {
        int32_t source_x0 = content_x < 0 ? -content_x : 0;
        int32_t source_y0 = content_y < 0 ? -content_y : 0;
        int32_t source_x1 = (int32_t)surface->width;
        int32_t source_y1 = (int32_t)surface->height;
        uint32_t canvas_width = renderer->framebuffer.pitch /
            sizeof(uint32_t);

        if (content_x + source_x1 > (int32_t)renderer->framebuffer.width)
            source_x1 = (int32_t)renderer->framebuffer.width - content_x;
        if (content_y + source_y1 > (int32_t)renderer->framebuffer.height)
            source_y1 = (int32_t)renderer->framebuffer.height - content_y;
        if (renderer->clip_enabled) {
            if (content_x + source_x0 < renderer->clip_x0)
                source_x0 = renderer->clip_x0 - content_x;
            if (content_y + source_y0 < renderer->clip_y0)
                source_y0 = renderer->clip_y0 - content_y;
            if (content_x + source_x1 > renderer->clip_x1)
                source_x1 = renderer->clip_x1 - content_x;
            if (content_y + source_y1 > renderer->clip_y1)
                source_y1 = renderer->clip_y1 - content_y;
        }
        if (source_x0 >= source_x1 || source_y0 >= source_y1)
            return;
        for (int32_t source_y = source_y0;
             source_y < source_y1; source_y++) {
            uint32_t *destination = renderer->canvas +
                (uint32_t)(content_y + source_y) * canvas_width +
                (uint32_t)(content_x + source_x0);
            const uint32_t *source = surface->pixels +
                (size_t)(uint32_t)source_y * surface->width +
                (uint32_t)source_x0;
            size_t count = (size_t)(source_x1 - source_x0);

            if (surface->opaque) {
                memcpy(destination, source, count * sizeof(*source));
            } else {
                for (size_t index = 0u; index < count; index++) {
                    destination[index] = wl_blend_pixel(
                        destination[index], source[index]);
                }
            }
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

static bool wl_renderer_clip_has_opaque_cover(
    const struct wl_server *server)
{
    const struct wl_server_renderer *renderer = &server->renderer;

    if (!renderer->clip_enabled)
        return false;
    for (size_t client_index = 0u;
         client_index < WL_SERVER_MAX_CLIENTS; client_index++) {
        const struct wl_server_client *client =
            &server->clients[client_index];

        if (!client->used)
            continue;
        for (size_t index = 0u; index < WL_SERVER_MAX_SURFACES; index++) {
            const struct wl_server_surface *surface =
                &client->surfaces[index];
            int32_t content_y;

            if (!surface->used || !surface->mapped || !surface->pixels ||
                surface->is_subsurface || !surface->opaque)
                continue;
            content_y = surface->y + (int32_t)WL_WINDOW_TITLE_HEIGHT;
            if (renderer->clip_x0 >= surface->x &&
                renderer->clip_y0 >= content_y &&
                renderer->clip_x1 <=
                    surface->x + (int32_t)surface->width &&
                renderer->clip_y1 <=
                    content_y + (int32_t)surface->height)
                return true;
        }
    }
    return false;
}

static void wl_renderer_draw_surfaces(struct wl_server *server)
{
    struct wl_server_surface *ordered[
        WL_SERVER_MAX_CLIENTS * WL_SERVER_MAX_SURFACES];
    size_t count = 0u;
    size_t first = 0u;

    for (size_t client_index = 0;
         client_index < WL_SERVER_MAX_CLIENTS; client_index++) {
        struct wl_server_client *client = &server->clients[client_index];

        if (!client->used)
            continue;
        for (size_t index = 0; index < WL_SERVER_MAX_SURFACES; index++) {
            struct wl_server_surface *surface = &client->surfaces[index];

            if (!surface->used || !surface->mapped || !surface->pixels)
                continue;
            size_t position = count;

            while (position > 0u &&
                   ordered[position - 1u]->z_order > surface->z_order) {
                ordered[position] = ordered[position - 1u];
                position--;
            }
            ordered[position] = surface;
            count++;
        }
    }
    /*
     * If an opaque top-level content area completely covers the clip, every
     * surface below it is invisible. Starting there avoids rebuilding an
     * entire hidden scene for animated opaque clients such as teapot-demo.
     */
    if (server->renderer.clip_enabled) {
        for (size_t index = count; index > 0u; index--) {
            struct wl_server_surface *surface = ordered[index - 1u];
            int32_t content_y =
                surface->y + (int32_t)WL_WINDOW_TITLE_HEIGHT;

            if (!surface->is_subsurface && surface->opaque &&
                server->renderer.clip_x0 >= surface->x &&
                server->renderer.clip_y0 >= content_y &&
                server->renderer.clip_x1 <=
                    surface->x + (int32_t)surface->width &&
                server->renderer.clip_y1 <=
                    content_y + (int32_t)surface->height) {
                first = index - 1u;
                break;
            }
        }
    }
    for (size_t index = first; index < count; index++) {
        wl_renderer_draw_surface(
            &server->renderer, ordered[index],
            ordered[index] == server->focus_surface);
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
    struct armos_fb_blit blit;
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
    height = (uint32_t)(y1 - y);
    blit.x = (uint32_t)x;
    blit.y = (uint32_t)y;
    blit.width = width;
    blit.height = height;
    blit.source_pitch = renderer->framebuffer.pitch;
    blit.source = (uint64_t)(uintptr_t)(
        (const uint8_t *)renderer->canvas +
        (uint32_t)y * renderer->framebuffer.pitch +
        (uint32_t)x * sizeof(uint32_t));
    if (ioctl(renderer->framebuffer_fd, ARMOS_FBIOBLIT, &blit) == 0)
        return 0;
    if (errno != ENOTTY && errno != ENOSYS)
        return -1;

    /*
     * Compatibility path for kernels predating ARMOS_FBIOBLIT. Keep precise
     * rows: writing complete scanline bands can multiply the transferred
     * bytes for narrow damage rectangles on physical framebuffers.
     */
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
    int result;

    if (wl_renderer_build_canvas(server) < 0)
        return -1;
    renderer = &server->renderer;
    server->pointer_presented = true;
    server->presented_pointer_x = server->pointer_x;
    server->presented_pointer_y = server->pointer_y;
    if (renderer->headless) {
        result = 0;
    } else {
        if (lseek(renderer->framebuffer_fd, 0, SEEK_SET) < 0)
            return -1;
        result = wl_write_full(renderer->framebuffer_fd, renderer->canvas,
                               renderer->canvas_size);
    }
    if (result == 0) {
        /*
         * A complete composition subsumes every queued partial update.
         * Keeping an older damage rectangle would repaint stale geometry on
         * the next timer tick.
         */
        server->damage_pending = false;
        server->damage_count = 0u;
    }
    return result;
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

static bool wl_renderer_rects_touch(const struct wl_renderer_rect *left,
                                    const struct wl_renderer_rect *right)
{
    return left->x0 <= right->x1 && left->x1 >= right->x0 &&
           left->y0 <= right->y1 && left->y1 >= right->y0;
}

static void wl_renderer_region_add(struct wl_renderer_rect *regions,
                                   size_t *count, size_t capacity,
                                   struct wl_renderer_rect damage)
{
    size_t index = 0u;

    while (index < *count) {
        struct wl_renderer_rect *region = &regions[index];

        if (!wl_renderer_rects_touch(region, &damage)) {
            index++;
            continue;
        }
        if (region->x0 < damage.x0)
            damage.x0 = region->x0;
        if (region->y0 < damage.y0)
            damage.y0 = region->y0;
        if (region->x1 > damage.x1)
            damage.x1 = region->x1;
        if (region->y1 > damage.y1)
            damage.y1 = region->y1;
        regions[index] = regions[--(*count)];
        index = 0u;
    }
    if (*count < capacity) {
        regions[(*count)++] = damage;
        return;
    }

    /*
     * Region overflow remains bounded: merge with the rectangle whose union
     * grows the least instead of collapsing the complete scene immediately.
     */
    {
        size_t best = 0u;
        uint64_t best_growth = ~(uint64_t)0;

        for (index = 0u; index < *count; index++) {
            struct wl_renderer_rect *region = &regions[index];
            int32_t x0 = region->x0 < damage.x0 ?
                region->x0 : damage.x0;
            int32_t y0 = region->y0 < damage.y0 ?
                region->y0 : damage.y0;
            int32_t x1 = region->x1 > damage.x1 ?
                region->x1 : damage.x1;
            int32_t y1 = region->y1 > damage.y1 ?
                region->y1 : damage.y1;
            uint64_t old_area =
                (uint64_t)(uint32_t)(region->x1 - region->x0) *
                (uint32_t)(region->y1 - region->y0);
            uint64_t new_area =
                (uint64_t)(uint32_t)(x1 - x0) *
                (uint32_t)(y1 - y0);
            uint64_t growth = new_area - old_area;

            if (growth < best_growth) {
                best = index;
                best_growth = growth;
            }
        }
        wl_renderer_rect_add(
            &regions[best], damage.x0, damage.y0,
            (uint32_t)(damage.x1 - damage.x0),
            (uint32_t)(damage.y1 - damage.y0));
    }
}

void wl_renderer_damage_rect(struct wl_server *server, int32_t x, int32_t y,
                             uint32_t width, uint32_t height)
{
    struct wl_renderer_rect damage;

    if (!server || width == 0u || height == 0u)
        return;
    damage.x0 = x;
    damage.y0 = y;
    damage.x1 = x + (int32_t)width;
    damage.y1 = y + (int32_t)height;
    wl_renderer_region_add(server->damage, &server->damage_count,
                           WL_SERVER_MAX_DAMAGE_RECTS, damage);
    server->damage_pending = server->damage_count != 0u;
}

void wl_renderer_damage_surface_at(
    struct wl_server *server, const struct wl_server_surface *surface,
    int32_t x, int32_t y)
{
    if (!surface)
        return;
    wl_renderer_damage_rect(
        server, x - 8, y - 4, surface->width + 16u,
        surface->height + WL_WINDOW_TITLE_HEIGHT + 16u);
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

static bool wl_renderer_rect_intersects(
    const struct wl_renderer_rect *damage, int32_t x, int32_t y,
    uint32_t width, uint32_t height)
{
    int32_t x1 = x + (int32_t)width;
    int32_t y1 = y + (int32_t)height;

    return x < damage->x1 && x1 > damage->x0 &&
           y < damage->y1 && y1 > damage->y0;
}

int wl_renderer_compose_damage(struct wl_server *server)
{
    struct wl_server_renderer *renderer;
    struct wl_renderer_rect damage[
        WL_SERVER_MAX_DAMAGE_RECTS + 2u];
    size_t damage_count;
    bool pointer_damage;

    if (!server || !server->renderer.canvas)
        return -1;
    renderer = &server->renderer;
    if (!server->damage_pending)
        return wl_renderer_compose(server);

    damage_count = server->damage_count;
    memcpy(damage, server->damage,
           damage_count * sizeof(damage[0]));
    pointer_damage = !server->pointer_presented ||
        server->presented_pointer_x != server->pointer_x ||
        server->presented_pointer_y != server->pointer_y;
    if (!pointer_damage) {
        for (size_t index = 0u; index < damage_count; index++) {
            if (wl_renderer_rect_intersects(
                    &damage[index], server->presented_pointer_x,
                    server->presented_pointer_y,
                    WL_POINTER_WIDTH, WL_POINTER_HEIGHT)) {
                pointer_damage = true;
                break;
            }
        }
    }
    if (pointer_damage) {
        struct wl_renderer_rect pointer;

        pointer.x0 = server->presented_pointer_x;
        pointer.y0 = server->presented_pointer_y;
        pointer.x1 = pointer.x0 + (int32_t)WL_POINTER_WIDTH;
        pointer.y1 = pointer.y0 + (int32_t)WL_POINTER_HEIGHT;
        wl_renderer_region_add(
            damage, &damage_count,
            WL_SERVER_MAX_DAMAGE_RECTS + 2u, pointer);
        pointer.x0 = server->pointer_x;
        pointer.y0 = server->pointer_y;
        pointer.x1 = pointer.x0 + (int32_t)WL_POINTER_WIDTH;
        pointer.y1 = pointer.y0 + (int32_t)WL_POINTER_HEIGHT;
        wl_renderer_region_add(
            damage, &damage_count,
            WL_SERVER_MAX_DAMAGE_RECTS + 2u, pointer);
    }

    if (pointer_damage)
        wl_renderer_restore_pointer(server);
    for (size_t index = 0u; index < damage_count; index++) {
        if (!wl_renderer_clip_rect(renderer, &damage[index]))
            continue;
        renderer->clip_enabled = true;
        renderer->clip_x0 = damage[index].x0;
        renderer->clip_y0 = damage[index].y0;
        renderer->clip_x1 = damage[index].x1;
        renderer->clip_y1 = damage[index].y1;
        if (!wl_renderer_clip_has_opaque_cover(server))
            wl_renderer_clear_rect(renderer, &damage[index]);
        wl_renderer_draw_surfaces(server);
    }
    renderer->clip_enabled = false;
    if (pointer_damage)
        wl_renderer_draw_pointer(server);

    for (size_t index = 0u; index < damage_count; index++) {
        uint32_t width;
        uint32_t height;

        if (damage[index].x0 >= damage[index].x1 ||
            damage[index].y0 >= damage[index].y1)
            continue;
        width = (uint32_t)(damage[index].x1 - damage[index].x0);
        height = (uint32_t)(damage[index].y1 - damage[index].y0);
        if (wl_renderer_present_rect(
                renderer, damage[index].x0, damage[index].y0,
                width, height) < 0)
            return -1;
    }
    if (pointer_damage) {
        server->pointer_presented = true;
        server->presented_pointer_x = server->pointer_x;
        server->presented_pointer_y = server->pointer_y;
    }
    server->damage_pending = false;
    server->damage_count = 0u;
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
    uint32_t previous_width;
    uint32_t previous_height;
    bool previous_mapped;
    bool content_changed;
    bool callback_pending = false;

    if (!server || !client || !surface)
        return -1;
    previous_width = surface->width;
    previous_height = surface->height;
    previous_mapped = surface->mapped;
    content_changed = surface->pending_attach;
    if (surface->pending_attach) {
        buffer = surface->pending_buffer;
        surface->pending_attach = false;
        surface->pending_buffer = NULL;
        if (!buffer) {
            free(surface->pixels);
            surface->pixels = NULL;
            surface->pixels_size = 0;
            surface->mapped = false;
            surface->opaque = false;
            surface->width = 0;
            surface->height = 0;
        } else {
            uint64_t pixel_bytes =
                (uint64_t)buffer->width * buffer->height * 4u;
            uint32_t *copy;

            if (!wl_surface_buffer_valid(buffer) ||
                pixel_bytes > (uint64_t)SIZE_MAX)
                return -1;
            if (surface->pixels_size != (size_t)pixel_bytes) {
                copy = realloc(surface->pixels, (size_t)pixel_bytes);
                if (!copy)
                    return -1;
                surface->pixels = copy;
                surface->pixels_size = (size_t)pixel_bytes;
            } else {
                copy = surface->pixels;
            }
            if (buffer->stride == buffer->width * sizeof(uint32_t)) {
                memcpy(copy, buffer->pool->mapping + buffer->offset,
                       (size_t)pixel_bytes);
            } else {
                for (uint32_t y = 0; y < buffer->height; y++) {
                    const uint8_t *source = buffer->pool->mapping +
                        buffer->offset + (size_t)y * buffer->stride;
                    uint32_t *destination =
                        copy + (size_t)y * buffer->width;

                    memcpy(destination, source,
                           (size_t)buffer->width * sizeof(uint32_t));
                }
            }
            surface->width = buffer->width;
            surface->height = buffer->height;
            surface->mapped = true;
            surface->opaque = buffer->format == WL_SHM_FORMAT_XRGB8888;
            if (buffer->object_alive) {
                (void)wl_client_send_words(client, buffer->object_id, 0,
                                           NULL, 0);
            }
        }
    }

    if (content_changed) {
        bool same_extent = previous_mapped &&
            (previous_width == surface->width &&
             previous_height == surface->height);

        if (surface->mapped && same_extent) {
            wl_renderer_damage_rect(
                server, surface->x,
                surface->y + (int32_t)WL_WINDOW_TITLE_HEIGHT,
                surface->width, surface->height);
            if (wl_server_schedule_render(server, false) < 0)
                return -1;
        } else if (wl_server_schedule_render(server, true) < 0) {
            return -1;
        }
    }
    for (size_t index = 0; index < WL_SERVER_MAX_CALLBACKS; index++) {
        if (surface->callbacks[index].used) {
            callback_pending = true;
            break;
        }
    }
    if (callback_pending && !content_changed &&
        wl_server_schedule_render(server, false) < 0)
        return -1;
    return 0;
}

int wl_server_complete_frame_callbacks(struct wl_server *server)
{
    if (!server)
        return -1;
    for (size_t client_index = 0u;
         client_index < WL_SERVER_MAX_CLIENTS; client_index++) {
        struct wl_server_client *client = &server->clients[client_index];

        if (!client->used)
            continue;
        for (size_t surface_index = 0u;
             surface_index < WL_SERVER_MAX_SURFACES; surface_index++) {
            struct wl_server_surface *surface =
                &client->surfaces[surface_index];

            if (!surface->used)
                continue;
            for (size_t callback_index = 0u;
                 callback_index < WL_SERVER_MAX_CALLBACKS;
                 callback_index++) {
                struct wl_server_callback *callback =
                    &surface->callbacks[callback_index];
                uint32_t done;

                if (!callback->used)
                    continue;
                done = ++server->serial;
                if (wl_client_send_words(client, callback->object_id,
                                         0u, &done, 1u) < 0)
                    return -1;
                wl_client_remove_object(client, callback->object_id, true);
                memset(callback, 0, sizeof(*callback));
            }
        }
    }
    return 0;
}
