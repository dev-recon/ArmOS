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
#include <sys/mman.h>
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

struct wl_renderer_scene {
    struct wl_server_surface *ordered[
        WL_SERVER_MAX_CLIENTS * WL_SERVER_MAX_SURFACES];
    size_t count;
};

static void wl_renderer_select_draw_buffer(
    struct wl_server_renderer *renderer)
{
    if (!renderer || !renderer->direct_present ||
        renderer->present_buffer_count == 0u)
        return;
    renderer->present_draw_buffer =
        renderer->present_buffer_count > 1u ?
        (renderer->present_front_buffer + 1u) %
            renderer->present_buffer_count :
        renderer->present_front_buffer;
    renderer->canvas = (uint32_t *)(void *)(
        (uint8_t *)(void *)renderer->mapped_buffers +
        (size_t)renderer->present_draw_buffer * renderer->canvas_size);
    renderer->dirty_tiles = renderer->dirty_tile_storage +
        (size_t)renderer->present_draw_buffer *
        renderer->dirty_tile_word_count;
}

static void wl_renderer_mark_all_buffers_dirty(
    struct wl_server_renderer *renderer)
{
    if (!renderer || !renderer->dirty_tile_storage)
        return;
    memset(renderer->dirty_tile_storage, 0xff,
           renderer->dirty_tile_word_count *
           renderer->present_buffer_count *
           sizeof(*renderer->dirty_tile_storage));
}

int wl_renderer_init(struct wl_server_renderer *renderer, bool headless)
{
    struct armos_fb_map map = {0};
    uint64_t tile_count;
    uint64_t tile_word_count;
    uint64_t dirty_word_count;

    if (!renderer)
        return -1;
    memset(renderer, 0, sizeof(*renderer));
    renderer->framebuffer_fd = -1;
    renderer->headless = headless;
    renderer->present_buffer_count = 1u;

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
        if (ioctl(renderer->framebuffer_fd, ARMOS_FBIOGET_MAP, &map) == 0 &&
            map.buffer_count != 0u &&
            map.front_buffer < map.buffer_count &&
            map.buffer_size == renderer->framebuffer.size &&
            (uint64_t)map.mapping_size ==
                (uint64_t)map.buffer_size * map.buffer_count) {
            renderer->mapped_buffers = mmap(
                NULL, map.mapping_size, PROT_READ | PROT_WRITE,
                MAP_SHARED, renderer->framebuffer_fd, 0);
            if (renderer->mapped_buffers != MAP_FAILED) {
                renderer->mapped_buffers_size = map.mapping_size;
                renderer->present_buffer_count = map.buffer_count;
                renderer->present_front_buffer = map.front_buffer;
                renderer->present_draw_buffer = map.front_buffer;
                renderer->direct_present = true;
                renderer->canvas = (uint32_t *)(void *)(
                    (uint8_t *)(void *)renderer->mapped_buffers +
                    (size_t)map.front_buffer * map.buffer_size);
            } else {
                renderer->mapped_buffers = NULL;
            }
        }
    }

    renderer->canvas_size = renderer->framebuffer.size;
    if (!renderer->direct_present) {
        renderer->canvas = malloc(renderer->canvas_size);
        if (!renderer->canvas)
            goto fail;
    }
    renderer->tile_columns =
        (renderer->framebuffer.width + WL_RENDER_TILE_SIZE - 1u) /
        WL_RENDER_TILE_SIZE;
    renderer->tile_rows =
        (renderer->framebuffer.height + WL_RENDER_TILE_SIZE - 1u) /
        WL_RENDER_TILE_SIZE;
    tile_count = (uint64_t)renderer->tile_columns * renderer->tile_rows;
    tile_word_count = (tile_count + 63u) / 64u;
    if (tile_count == 0u ||
        tile_word_count > SIZE_MAX / sizeof(uint64_t)) {
        errno = EOVERFLOW;
        goto fail;
    }
    renderer->dirty_tile_word_count = (size_t)tile_word_count;
    dirty_word_count = tile_word_count * renderer->present_buffer_count;
    if (renderer->present_buffer_count != 0u &&
        dirty_word_count / renderer->present_buffer_count !=
            tile_word_count) {
        errno = EOVERFLOW;
        goto fail;
    }
    if (dirty_word_count > SIZE_MAX / sizeof(uint64_t)) {
        errno = EOVERFLOW;
        goto fail;
    }
    renderer->dirty_tile_storage = calloc(
        (size_t)dirty_word_count, sizeof(*renderer->dirty_tile_storage));
    if (!renderer->dirty_tile_storage)
        goto fail;
    renderer->dirty_tiles = renderer->dirty_tile_storage +
        (size_t)renderer->present_draw_buffer *
        renderer->dirty_tile_word_count;
    return 0;

fail:
    wl_renderer_destroy(renderer);
    return -1;
}

void wl_renderer_destroy(struct wl_server_renderer *renderer)
{
    if (!renderer)
        return;
    free(renderer->dirty_tile_storage);
    renderer->dirty_tile_storage = NULL;
    renderer->dirty_tiles = NULL;
    renderer->dirty_tile_word_count = 0u;
    renderer->tile_columns = 0u;
    renderer->tile_rows = 0u;
    if (renderer->mapped_buffers) {
        (void)munmap(renderer->mapped_buffers,
                     renderer->mapped_buffers_size);
    } else {
        free(renderer->canvas);
    }
    renderer->mapped_buffers = NULL;
    renderer->mapped_buffers_size = 0u;
    renderer->canvas = NULL;
    if (renderer->framebuffer_fd >= 0)
        close(renderer->framebuffer_fd);
    renderer->framebuffer_fd = -1;
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
        wl_render_blend_pixel(
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

static bool wl_renderer_surface_clip_opaque(
    const struct wl_server_renderer *renderer,
    const struct wl_server_surface *surface)
{
    int32_t content_x;
    int32_t content_y;
    uint32_t source_x0;
    uint32_t source_y0;
    uint32_t width;
    uint32_t height;

    if (!renderer->clip_enabled || !surface->pixels ||
        surface->role == WL_SERVER_SURFACE_ROLE_SUBSURFACE)
        return false;
    content_x = surface->x;
    content_y = surface->y +
        (surface->server_decorated ?
         (int32_t)WL_WINDOW_TITLE_HEIGHT : 0);
    if (renderer->clip_x0 < content_x ||
        renderer->clip_y0 < content_y ||
        renderer->clip_x1 > content_x + (int32_t)surface->width ||
        renderer->clip_y1 > content_y + (int32_t)surface->height)
        return false;
    if (surface->opaque)
        return true;

    source_x0 = (uint32_t)(renderer->clip_x0 - content_x);
    source_y0 = (uint32_t)(renderer->clip_y0 - content_y);
    width = (uint32_t)(renderer->clip_x1 - renderer->clip_x0);
    height = (uint32_t)(renderer->clip_y1 - renderer->clip_y0);
    for (uint32_t row = 0u; row < height; row++) {
        const uint32_t *source = surface->pixels +
            (size_t)(source_y0 + row) * surface->width + source_x0;

        for (uint32_t column = 0u; column < width; column++) {
            if ((source[column] >> 24) != 255u)
                return false;
        }
    }
    return true;
}

static void wl_renderer_draw_surface(struct wl_server_renderer *renderer,
                                     const struct wl_server_surface *surface,
                                     bool active, bool opaque_clip)
{
    uint32_t title_height =
        surface->role == WL_SERVER_SURFACE_ROLE_TOPLEVEL &&
        surface->server_decorated ?
        WL_WINDOW_TITLE_HEIGHT : 0u;
    uint32_t frame_width = surface->width;
    uint32_t frame_height = surface->height + title_height;
    int32_t content_y = surface->y + (int32_t)title_height;
    int32_t content_x = surface->x;
    bool content_only_damage = false;

    if (surface->role == WL_SERVER_SURFACE_ROLE_SUBSURFACE) {
        const struct wl_server_surface *ancestor = surface;
        size_t depth = 0u;

        content_x = surface->subsurface_x;
        content_y = surface->subsurface_y;
        while (ancestor->role == WL_SERVER_SURFACE_ROLE_SUBSURFACE &&
               ancestor->parent &&
               depth++ < WL_SERVER_MAX_SURFACES) {
            ancestor = ancestor->parent;
            if (ancestor->role == WL_SERVER_SURFACE_ROLE_SUBSURFACE) {
                content_x += ancestor->subsurface_x;
                content_y += ancestor->subsurface_y;
            } else {
                content_x += ancestor->x;
                content_y += ancestor->y +
                    (ancestor->server_decorated ?
                     (int32_t)WL_WINDOW_TITLE_HEIGHT : 0);
            }
        }
        if (depth > WL_SERVER_MAX_SURFACES)
            return;
        goto draw_content;
    }

    if (opaque_clip ||
        (renderer->clip_enabled && surface->opaque &&
         renderer->clip_x0 >= content_x &&
         renderer->clip_y0 >= content_y &&
         renderer->clip_x1 <= content_x + (int32_t)surface->width &&
         renderer->clip_y1 <= content_y + (int32_t)surface->height))
        content_only_damage = true;

    if (!content_only_damage && title_height != 0u) {
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
            title_height,
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
        {
            uint32_t *destination = renderer->canvas +
                (uint32_t)(content_y + source_y0) * canvas_width +
                (uint32_t)(content_x + source_x0);
            const uint32_t *source = surface->pixels +
                (size_t)(uint32_t)source_y0 * surface->width +
                (uint32_t)source_x0;
            uint32_t width = (uint32_t)(source_x1 - source_x0);
            uint32_t height = (uint32_t)(source_y1 - source_y0);

            if (surface->opaque || opaque_clip)
                wl_render_copy_rect(destination, canvas_width,
                                    source, surface->width, width, height);
            else
                wl_render_blend_rect(destination, canvas_width,
                                     source, surface->width, width, height);
        }
    }
}

static void wl_renderer_draw_pointer(struct wl_server *server)
{
    struct wl_server_renderer *renderer = &server->renderer;
    int32_t x = server->pointer_x;
    int32_t y = server->pointer_y;

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

static void wl_renderer_build_scene(struct wl_server *server,
                                    struct wl_renderer_scene *scene)
{
    for (size_t client_index = 0u;
         client_index < WL_SERVER_MAX_CLIENTS; client_index++) {
        struct wl_server_client *client = &server->clients[client_index];

        if (!client->used)
            continue;
        for (size_t index = 0u; index < WL_SERVER_MAX_SURFACES; index++) {
            struct wl_server_surface *surface = &client->surfaces[index];
            size_t position;

            if (!surface->used || !surface->mapped ||
                surface->role == WL_SERVER_SURFACE_ROLE_CURSOR ||
                !surface->pixels)
                continue;
            position = scene->count;
            while (position > 0u &&
                   scene->ordered[position - 1u]->z_order >
                       surface->z_order) {
                scene->ordered[position] =
                    scene->ordered[position - 1u];
                position--;
            }
            scene->ordered[position] = surface;
            scene->count++;
        }
    }
}

static size_t wl_renderer_scene_first_visible(
    const struct wl_server *server,
    const struct wl_renderer_scene *scene,
    bool *opaque_cover)
{
    const struct wl_server_renderer *renderer = &server->renderer;

    *opaque_cover = false;
    if (!renderer->clip_enabled)
        return 0u;
    /*
     * If an opaque top-level content area completely covers the clip, every
     * surface below it is invisible. Starting there avoids rebuilding an
     * entire hidden scene for animated opaque clients such as teapot-demo.
     */
    for (size_t index = scene->count; index > 0u; index--) {
        const struct wl_server_surface *surface =
            scene->ordered[index - 1u];

        if (wl_renderer_surface_clip_opaque(renderer, surface)) {
            *opaque_cover = true;
            return index - 1u;
        }
    }
    return 0u;
}

static void wl_renderer_draw_scene(
    struct wl_server *server, const struct wl_renderer_scene *scene,
    size_t first, bool first_opaque)
{
    for (size_t index = first; index < scene->count; index++) {
        wl_renderer_draw_surface(
            &server->renderer, scene->ordered[index],
            scene->ordered[index] == server->focus_surface,
            first_opaque && index == first);
    }
}

static int wl_renderer_build_canvas(struct wl_server *server)
{
    struct wl_server_renderer *renderer;
    struct wl_renderer_scene scene = {0};
    uint32_t canvas_width;

    if (!server || !server->renderer.canvas)
        return -1;
    renderer = &server->renderer;
    renderer->clip_enabled = false;
    canvas_width = renderer->framebuffer.pitch / 4u;
    wl_render_fill_rect(renderer->canvas, canvas_width, canvas_width,
                        renderer->framebuffer.height, WL_BACKGROUND);

    wl_renderer_build_scene(server, &scene);
    wl_renderer_draw_scene(server, &scene, 0u, false);

    if (!renderer->headless)
        wl_renderer_draw_pointer(server);

    return 0;
}

int wl_renderer_compose(struct wl_server *server)
{
    struct wl_server_renderer *renderer;
    int result;

    if (!server)
        return -1;
    wl_renderer_mark_all_buffers_dirty(&server->renderer);
    wl_renderer_select_draw_buffer(&server->renderer);
    if (wl_renderer_build_canvas(server) < 0)
        return -1;
    renderer = &server->renderer;
    server->pointer_presented = true;
    server->presented_pointer_x = server->pointer_x;
    server->presented_pointer_y = server->pointer_y;
    result = wl_renderer_backend_present_rect(
        renderer, 0, 0, renderer->framebuffer.width,
        renderer->framebuffer.height);
    if (result == 0) {
        renderer->present_front_buffer =
            renderer->present_draw_buffer;
        /*
         * A complete composition subsumes every queued partial update.
         * Keeping an older damage rectangle would repaint stale geometry on
         * the next timer tick.
         */
        server->damage_pending = false;
        memset(renderer->dirty_tiles, 0,
               renderer->dirty_tile_word_count *
               sizeof(*renderer->dirty_tiles));
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
    wl_renderer_damage_rect(server, old_x, old_y,
                            WL_POINTER_WIDTH, WL_POINTER_HEIGHT);
    wl_renderer_damage_rect(server, server->pointer_x, server->pointer_y,
                            WL_POINTER_WIDTH, WL_POINTER_HEIGHT);
    return wl_renderer_compose_damage(server);
}

static size_t wl_renderer_tile_index(
    const struct wl_server_renderer *renderer,
    uint32_t column, uint32_t row)
{
    return (size_t)row * renderer->tile_columns + column;
}

static bool wl_renderer_tile_dirty(
    const struct wl_server_renderer *renderer,
    uint32_t column, uint32_t row)
{
    size_t index = wl_renderer_tile_index(renderer, column, row);

    return (renderer->dirty_tiles[index / 64u] &
            (1ull << (index % 64u))) != 0u;
}

static void wl_renderer_mark_tile(
    struct wl_server_renderer *renderer,
    uint32_t column, uint32_t row)
{
    size_t index = wl_renderer_tile_index(renderer, column, row);

    for (uint32_t buffer = 0u;
         buffer < renderer->present_buffer_count; buffer++) {
        uint64_t *tiles = renderer->dirty_tile_storage +
            (size_t)buffer * renderer->dirty_tile_word_count;

        tiles[index / 64u] |= 1ull << (index % 64u);
    }
}

void wl_renderer_damage_rect(struct wl_server *server, int32_t x, int32_t y,
                             uint32_t width, uint32_t height)
{
    struct wl_server_renderer *renderer;
    int64_t x1;
    int64_t y1;
    uint32_t column0;
    uint32_t column1;
    uint32_t row0;
    uint32_t row1;

    if (!server || width == 0u || height == 0u)
        return;
    renderer = &server->renderer;
    x1 = (int64_t)x + width;
    y1 = (int64_t)y + height;
    if (x1 <= 0 || y1 <= 0 ||
        x >= (int32_t)renderer->framebuffer.width ||
        y >= (int32_t)renderer->framebuffer.height)
        return;
    if (x < 0)
        x = 0;
    if (y < 0)
        y = 0;
    if (x1 > renderer->framebuffer.width)
        x1 = renderer->framebuffer.width;
    if (y1 > renderer->framebuffer.height)
        y1 = renderer->framebuffer.height;

    column0 = (uint32_t)x / WL_RENDER_TILE_SIZE;
    column1 = ((uint32_t)x1 - 1u) / WL_RENDER_TILE_SIZE;
    row0 = (uint32_t)y / WL_RENDER_TILE_SIZE;
    row1 = ((uint32_t)y1 - 1u) / WL_RENDER_TILE_SIZE;
    for (uint32_t row = row0; row <= row1; row++) {
        for (uint32_t column = column0; column <= column1; column++)
            wl_renderer_mark_tile(renderer, column, row);
    }
    server->damage_pending = true;
}

static void wl_renderer_damage_surface_extent_at(
    struct wl_server *server, int32_t x, int32_t y,
    uint32_t width, uint32_t height, bool server_decorated)
{
    wl_renderer_damage_rect(
        server, x - 8, y - 4, width + 16u,
        height + (server_decorated ? WL_WINDOW_TITLE_HEIGHT : 0u) + 16u);
}

void wl_renderer_damage_surface_at(
    struct wl_server *server, const struct wl_server_surface *surface,
    int32_t x, int32_t y)
{
    if (!surface)
        return;
    wl_renderer_damage_surface_extent_at(
        server, x, y, surface->width, surface->height,
        surface->server_decorated);
}

static struct wl_renderer_rect wl_renderer_tile_rect(
    const struct wl_server_renderer *renderer,
    uint32_t column, uint32_t row)
{
    struct wl_renderer_rect tile;

    tile.x0 = (int32_t)(column * WL_RENDER_TILE_SIZE);
    tile.y0 = (int32_t)(row * WL_RENDER_TILE_SIZE);
    tile.x1 = tile.x0 + (int32_t)WL_RENDER_TILE_SIZE;
    tile.y1 = tile.y0 + (int32_t)WL_RENDER_TILE_SIZE;
    if (tile.x1 > (int32_t)renderer->framebuffer.width)
        tile.x1 = (int32_t)renderer->framebuffer.width;
    if (tile.y1 > (int32_t)renderer->framebuffer.height)
        tile.y1 = (int32_t)renderer->framebuffer.height;
    return tile;
}

static void wl_renderer_clear_rect(struct wl_server_renderer *renderer,
                                   const struct wl_renderer_rect *damage)
{
    uint32_t canvas_width = renderer->framebuffer.pitch / sizeof(uint32_t);
    uint32_t *destination = renderer->canvas +
        (uint32_t)damage->y0 * canvas_width + (uint32_t)damage->x0;

    wl_render_fill_rect(
        destination, canvas_width,
        (uint32_t)(damage->x1 - damage->x0),
        (uint32_t)(damage->y1 - damage->y0), WL_BACKGROUND);
}

static bool wl_renderer_dirty_intersects_rect(
    const struct wl_server_renderer *renderer,
    int32_t x, int32_t y, uint32_t width, uint32_t height)
{
    int64_t x1 = (int64_t)x + width;
    int64_t y1 = (int64_t)y + height;
    uint32_t column0;
    uint32_t column1;
    uint32_t row0;
    uint32_t row1;

    if (width == 0u || height == 0u || x1 <= 0 || y1 <= 0 ||
        x >= (int32_t)renderer->framebuffer.width ||
        y >= (int32_t)renderer->framebuffer.height)
        return false;
    if (x < 0)
        x = 0;
    if (y < 0)
        y = 0;
    if (x1 > renderer->framebuffer.width)
        x1 = renderer->framebuffer.width;
    if (y1 > renderer->framebuffer.height)
        y1 = renderer->framebuffer.height;
    column0 = (uint32_t)x / WL_RENDER_TILE_SIZE;
    column1 = ((uint32_t)x1 - 1u) / WL_RENDER_TILE_SIZE;
    row0 = (uint32_t)y / WL_RENDER_TILE_SIZE;
    row1 = ((uint32_t)y1 - 1u) / WL_RENDER_TILE_SIZE;
    for (uint32_t row = row0; row <= row1; row++) {
        for (uint32_t column = column0; column <= column1; column++) {
            if (wl_renderer_tile_dirty(renderer, column, row))
                return true;
        }
    }
    return false;
}

int wl_renderer_compose_damage(struct wl_server *server)
{
    struct wl_server_renderer *renderer;
    struct wl_renderer_scene scene = {0};
    bool pointer_damage;

    if (!server || !server->renderer.canvas)
        return -1;
    renderer = &server->renderer;
    if (!server->damage_pending)
        return 0;
    wl_renderer_select_draw_buffer(renderer);

    pointer_damage = !server->pointer_presented ||
        server->presented_pointer_x != server->pointer_x ||
        server->presented_pointer_y != server->pointer_y ||
        wl_renderer_dirty_intersects_rect(
            renderer, server->presented_pointer_x,
            server->presented_pointer_y,
            WL_POINTER_WIDTH, WL_POINTER_HEIGHT);
    if (pointer_damage) {
        wl_renderer_damage_rect(
            server, server->presented_pointer_x,
            server->presented_pointer_y,
            WL_POINTER_WIDTH, WL_POINTER_HEIGHT);
        wl_renderer_damage_rect(
            server, server->pointer_x, server->pointer_y,
            WL_POINTER_WIDTH, WL_POINTER_HEIGHT);
    }

    wl_renderer_build_scene(server, &scene);
    /*
     * Determine occlusion per tile, then merge adjacent tiles that share the
     * same first visible surface. This preserves conservative tile coverage
     * while avoiding hundreds of renderer invocations for a large update.
     */
    for (uint32_t row = 0u; row < renderer->tile_rows; row++) {
        uint32_t column = 0u;

        while (column < renderer->tile_columns) {
            struct wl_renderer_rect run;
            bool opaque_cover;
            size_t first;
            uint32_t run_end;

            if (!wl_renderer_tile_dirty(renderer, column, row)) {
                column++;
                continue;
            }
            run = wl_renderer_tile_rect(renderer, column, row);
            renderer->clip_enabled = true;
            renderer->clip_x0 = run.x0;
            renderer->clip_y0 = run.y0;
            renderer->clip_x1 = run.x1;
            renderer->clip_y1 = run.y1;
            first = wl_renderer_scene_first_visible(
                server, &scene, &opaque_cover);
            run_end = column;
            while (run_end + 1u < renderer->tile_columns &&
                   wl_renderer_tile_dirty(renderer, run_end + 1u, row)) {
                struct wl_renderer_rect candidate =
                    wl_renderer_tile_rect(renderer, run_end + 1u, row);
                bool candidate_opaque;
                size_t candidate_first;

                renderer->clip_x0 = candidate.x0;
                renderer->clip_y0 = candidate.y0;
                renderer->clip_x1 = candidate.x1;
                renderer->clip_y1 = candidate.y1;
                candidate_first = wl_renderer_scene_first_visible(
                    server, &scene, &candidate_opaque);
                if (candidate_first != first ||
                    candidate_opaque != opaque_cover)
                    break;
                run.x1 = candidate.x1;
                run_end++;
            }
            renderer->clip_x0 = run.x0;
            renderer->clip_y0 = run.y0;
            renderer->clip_x1 = run.x1;
            renderer->clip_y1 = run.y1;
            if (!opaque_cover)
                wl_renderer_clear_rect(renderer, &run);
            wl_renderer_draw_scene(
                server, &scene, first, opaque_cover);
            column = run_end + 1u;
        }
    }
    renderer->clip_enabled = false;
    if (pointer_damage)
        wl_renderer_draw_pointer(server);

    {
        struct wl_renderer_rect presentation = {0};
        bool have_damage = false;

        /*
         * Composition remains tile-local, but publish one final rectangle.
         * Multiple FBIOBLIT calls allowed displayd to expose intermediate
         * cache-clean states on a scanout framebuffer, producing visible
         * stripes and trails on Raspberry Pi.
         */
        for (uint32_t row = 0u; row < renderer->tile_rows; row++) {
            for (uint32_t column = 0u;
                 column < renderer->tile_columns; column++) {
                struct wl_renderer_rect tile;

                if (!wl_renderer_tile_dirty(renderer, column, row))
                    continue;
                tile = wl_renderer_tile_rect(renderer, column, row);
                if (!have_damage) {
                    presentation = tile;
                    have_damage = true;
                } else {
                    if (tile.x0 < presentation.x0)
                        presentation.x0 = tile.x0;
                    if (tile.y0 < presentation.y0)
                        presentation.y0 = tile.y0;
                    if (tile.x1 > presentation.x1)
                        presentation.x1 = tile.x1;
                    if (tile.y1 > presentation.y1)
                        presentation.y1 = tile.y1;
                }
            }
        }
        if (have_damage &&
            wl_renderer_backend_present_rect(
                renderer, presentation.x0, presentation.y0,
                (uint32_t)(presentation.x1 - presentation.x0),
                (uint32_t)(presentation.y1 - presentation.y0)) < 0)
            return -1;
        if (have_damage)
            renderer->present_front_buffer =
                renderer->present_draw_buffer;
        memset(renderer->dirty_tiles, 0,
               renderer->dirty_tile_word_count *
               sizeof(*renderer->dirty_tiles));
    }
    if (pointer_damage) {
        server->pointer_presented = true;
        server->presented_pointer_x = server->pointer_x;
        server->presented_pointer_y = server->pointer_y;
    }
    server->damage_pending = false;
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

static int wl_surface_content_origin(
    const struct wl_server_surface *surface, int32_t *x, int32_t *y)
{
    const struct wl_server_surface *ancestor = surface;
    size_t depth = 0u;

    if (!surface || !x || !y)
        return -1;
    if (surface->role != WL_SERVER_SURFACE_ROLE_SUBSURFACE) {
        *x = surface->x;
        *y = surface->y +
            (surface->server_decorated ?
             (int32_t)WL_WINDOW_TITLE_HEIGHT : 0);
        return 0;
    }
    *x = surface->subsurface_x;
    *y = surface->subsurface_y;
    while (ancestor->role == WL_SERVER_SURFACE_ROLE_SUBSURFACE &&
           ancestor->parent &&
           depth++ < WL_SERVER_MAX_SURFACES) {
        ancestor = ancestor->parent;
        if (ancestor->role == WL_SERVER_SURFACE_ROLE_SUBSURFACE) {
            *x += ancestor->subsurface_x;
            *y += ancestor->subsurface_y;
        } else {
            *x += ancestor->x;
            *y += ancestor->y +
                (ancestor->server_decorated ?
                 (int32_t)WL_WINDOW_TITLE_HEIGHT : 0);
        }
    }
    return depth <= WL_SERVER_MAX_SURFACES ? 0 : -1;
}

static bool wl_surface_clip_local_damage(
    const struct wl_server_surface *surface,
    struct wl_renderer_rect *damage)
{
    if (damage->x0 < 0)
        damage->x0 = 0;
    if (damage->y0 < 0)
        damage->y0 = 0;
    if (damage->x1 > (int32_t)surface->width)
        damage->x1 = (int32_t)surface->width;
    if (damage->y1 > (int32_t)surface->height)
        damage->y1 = (int32_t)surface->height;
    return damage->x0 < damage->x1 && damage->y0 < damage->y1;
}

static void wl_surface_copy_damage(
    struct wl_server_surface *surface,
    const struct wl_server_buffer *buffer,
    const struct wl_renderer_rect *damage)
{
    size_t row_bytes =
        (size_t)(damage->x1 - damage->x0) * sizeof(uint32_t);

    for (int32_t y = damage->y0; y < damage->y1; y++) {
        const uint8_t *source = buffer->pool->mapping + buffer->offset +
            (size_t)y * buffer->stride +
            (size_t)damage->x0 * sizeof(uint32_t);
        uint32_t *destination = surface->pixels +
            (size_t)y * surface->width + (size_t)damage->x0;

        memcpy(destination, source, row_bytes);
    }
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
    bool full_copy;
    bool newly_attached;
    bool callback_pending = false;
    bool previous_origin_valid = false;
    int32_t previous_origin_x = 0;
    int32_t previous_origin_y = 0;
    int32_t content_x = 0;
    int32_t content_y = 0;

    if (!server || !client || !surface)
        return -1;
    previous_width = surface->width;
    previous_height = surface->height;
    previous_mapped = surface->mapped;
    if (previous_mapped) {
        if (surface->role == WL_SERVER_SURFACE_ROLE_SUBSURFACE) {
            previous_origin_valid =
                wl_surface_content_origin(
                    surface, &previous_origin_x,
                    &previous_origin_y) == 0;
        } else {
            previous_origin_x = surface->x;
            previous_origin_y = surface->y;
            previous_origin_valid = true;
        }
    }
    newly_attached = surface->pending_attach;
    content_changed = surface->pending_attach ||
        surface->pending_damage_count != 0u;
    buffer = surface->pending_attach ?
        surface->pending_buffer : surface->current_buffer;
    full_copy = surface->pending_damage_count == 0u;
    if (surface->pending_attach) {
        surface->pending_attach = false;
        surface->pending_buffer = NULL;
        surface->current_buffer = buffer;
        if (!buffer) {
            free(surface->pixels);
            surface->pixels = NULL;
            surface->pixels_size = 0;
            surface->mapped = false;
            surface->opaque = false;
            surface->width = 0;
            surface->height = 0;
        }
    }
    if (buffer && content_changed) {
        uint64_t pixel_bytes =
            (uint64_t)buffer->width * buffer->height * 4u;
        uint32_t *copy;
        bool same_extent;

        if (!wl_surface_buffer_valid(buffer) ||
            pixel_bytes > (uint64_t)SIZE_MAX)
            return -1;
        same_extent = previous_mapped &&
            previous_width == buffer->width &&
            previous_height == buffer->height;
        if (surface->pixels_size != (size_t)pixel_bytes) {
            copy = realloc(surface->pixels, (size_t)pixel_bytes);
            if (!copy)
                return -1;
            surface->pixels = copy;
            surface->pixels_size = (size_t)pixel_bytes;
        } else {
            copy = surface->pixels;
        }
        surface->width = buffer->width;
        surface->height = buffer->height;
        if (!same_extent)
            full_copy = true;
        if (full_copy &&
            buffer->stride == buffer->width * sizeof(uint32_t)) {
            memcpy(copy, buffer->pool->mapping + buffer->offset,
                   (size_t)pixel_bytes);
        } else if (full_copy) {
            for (uint32_t y = 0; y < buffer->height; y++) {
                const uint8_t *source = buffer->pool->mapping +
                    buffer->offset + (size_t)y * buffer->stride;
                uint32_t *destination =
                    copy + (size_t)y * buffer->width;

                memcpy(destination, source,
                       (size_t)buffer->width * sizeof(uint32_t));
            }
        } else {
            for (size_t index = 0u;
                 index < surface->pending_damage_count; index++) {
                struct wl_renderer_rect damage =
                    surface->pending_damage[index];

                if (wl_surface_clip_local_damage(surface, &damage))
                    wl_surface_copy_damage(surface, buffer, &damage);
            }
        }
        surface->mapped = true;
        surface->opaque = buffer->format == WL_SHM_FORMAT_XRGB8888;
        if (newly_attached && buffer->object_alive) {
            (void)wl_client_send_words(client, buffer->object_id, 0,
                                       NULL, 0);
        }
    }

    if (content_changed) {
        bool same_extent = previous_mapped &&
            (previous_width == surface->width &&
             previous_height == surface->height);

        if (surface->mapped && same_extent &&
            wl_surface_content_origin(surface, &content_x, &content_y) == 0) {
            if (full_copy) {
                wl_renderer_damage_rect(
                    server, content_x, content_y,
                    surface->width, surface->height);
            } else {
                for (size_t index = 0u;
                     index < surface->pending_damage_count; index++) {
                    struct wl_renderer_rect damage =
                        surface->pending_damage[index];

                    if (!wl_surface_clip_local_damage(surface, &damage))
                        continue;
                    wl_renderer_damage_rect(
                        server, content_x + damage.x0,
                        content_y + damage.y0,
                        (uint32_t)(damage.x1 - damage.x0),
                        (uint32_t)(damage.y1 - damage.y0));
                }
            }
            if (wl_server_schedule_render(server, false) < 0)
                return -1;
        } else {
            if (previous_mapped && previous_origin_valid) {
                wl_renderer_damage_surface_extent_at(
                    server, previous_origin_x, previous_origin_y,
                    previous_width, previous_height,
                    surface->server_decorated);
            }
            if (surface->mapped) {
                int32_t current_x = surface->x;
                int32_t current_y = surface->y;

                if (surface->role != WL_SERVER_SURFACE_ROLE_SUBSURFACE ||
                    wl_surface_content_origin(
                        surface, &current_x, &current_y) == 0) {
                    wl_renderer_damage_surface_at(
                        server, surface, current_x, current_y);
                }
            }
            if (wl_server_schedule_render(server, false) < 0)
                return -1;
        }
    }
    surface->pending_damage_count = 0u;
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
        bool disconnected = false;

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
                                         0u, &done, 1u) < 0) {
                    /*
                     * An abruptly terminated client may leave a frame
                     * callback pending until its socket reports the hangup.
                     * A broken client connection must not stop the compositor
                     * or disconnect unrelated clients.
                     */
                    wl_server_disconnect_client(server, client);
                    disconnected = true;
                    break;
                }
                wl_client_remove_object(client, callback->object_id, true);
                memset(callback, 0, sizeof(*callback));
            }
            if (disconnected)
                break;
        }
    }
    return 0;
}
