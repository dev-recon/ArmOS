/*
 * ArmOS
 * Copyright (c) 2026 Mohamed Ennassiri
 *
 * Licensed under the Apache License, Version 2.0.
 * See LICENSE for details.
 *
 * File: userland/lib/wayland/cursor.c
 * Layer: Userland / Wayland cursor compatibility
 *
 * Responsibilities:
 * - Provide a native libwayland-cursor implementation for Wayland clients.
 * - Allocate cursor pixels through ArmOS named shared memory.
 * - Supply a deterministic built-in pointer without external theme files.
 *
 * Notes:
 * - Cursor aliases currently resolve to the same software pointer.
 * - The implementation is original ArmOS code and has no Xcursor dependency.
 */

#include <errno.h>
#include <fcntl.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <wayland-client.h>
#include <wayland-cursor.h>

struct armos_cursor_image {
    struct wl_cursor_image public;
    struct wl_buffer *buffer;
};

struct wl_cursor_theme {
    struct wl_cursor cursor;
    struct wl_cursor_image *images[1];
    struct armos_cursor_image image;
    uint32_t *pixels;
    size_t mapping_size;
    int shm_fd;
};

static unsigned int cursor_sequence;

static int pointer_contains(unsigned int x, unsigned int y)
{
    int arrow = y < 15u && x <= y / 2u;
    int stem = y >= 9u && y < 22u && x >= 4u && x <= 7u;

    return arrow || stem;
}

static void draw_pointer(uint32_t *pixels, unsigned int size)
{
    for (unsigned int y = 0u; y < size; y++) {
        for (unsigned int x = 0u; x < size; x++) {
            unsigned int px = x * 24u / size;
            unsigned int py = y * 24u / size;
            int inside = pointer_contains(px, py);
            int edge = 0;

            if (inside) {
                edge = px == 0u || py == 0u ||
                    !pointer_contains(px - (px != 0u), py) ||
                    !pointer_contains(px + 1u, py) ||
                    !pointer_contains(px, py - (py != 0u)) ||
                    !pointer_contains(px, py + 1u);
            }
            pixels[y * size + x] = !inside ? 0x00000000u :
                (edge ? 0xff101010u : 0xfff4f4f4u);
        }
    }
}

static void destroy_partial_theme(struct wl_cursor_theme *theme)
{
    if (!theme)
        return;
    if (theme->image.buffer)
        wl_buffer_destroy(theme->image.buffer);
    if (theme->pixels && theme->pixels != MAP_FAILED)
        munmap(theme->pixels, theme->mapping_size);
    if (theme->shm_fd >= 0)
        close(theme->shm_fd);
    free(theme->cursor.name);
    free(theme);
}

struct wl_cursor_theme *wl_cursor_theme_load(const char *name, int size,
                                              struct wl_shm *shm)
{
    struct wl_cursor_theme *theme;
    struct wl_shm_pool *pool = NULL;
    char object_name[64];
    size_t mapping_size;
    unsigned int sequence;
    int saved_errno;

    if (!shm || size <= 0) {
        errno = EINVAL;
        return NULL;
    }
    if (size < 8)
        size = 8;
    if (size > 128)
        size = 128;
    mapping_size = (size_t)size * (size_t)size * sizeof(uint32_t);
    theme = calloc(1u, sizeof(*theme));
    if (!theme)
        return NULL;
    theme->shm_fd = -1;
    theme->mapping_size = mapping_size;
    theme->cursor.name = strdup(name && name[0] ? name : "ArmOS");
    if (!theme->cursor.name)
        goto failed;
    sequence = __sync_fetch_and_add(&cursor_sequence, 1u);
    snprintf(object_name, sizeof(object_name), "/wl-cursor-%d-%u",
             (int)getpid(), sequence);
    theme->shm_fd = shm_open(object_name, O_CREAT | O_EXCL | O_RDWR, 0600);
    if (theme->shm_fd < 0)
        goto failed;
    if (shm_unlink(object_name) < 0 ||
        ftruncate(theme->shm_fd, (off_t)mapping_size) < 0)
        goto failed;
    theme->pixels = mmap(NULL, mapping_size, PROT_READ | PROT_WRITE,
                         MAP_SHARED, theme->shm_fd, 0);
    if (theme->pixels == MAP_FAILED)
        goto failed;
    draw_pointer(theme->pixels, (unsigned int)size);
    pool = wl_shm_create_pool(shm, theme->shm_fd, (int32_t)mapping_size);
    if (!pool)
        goto failed;
    theme->image.buffer = wl_shm_pool_create_buffer(
        pool, 0, size, size, size * (int)sizeof(uint32_t),
        WL_SHM_FORMAT_ARGB8888);
    wl_shm_pool_destroy(pool);
    pool = NULL;
    if (!theme->image.buffer)
        goto failed;
    theme->image.public.width = (uint32_t)size;
    theme->image.public.height = (uint32_t)size;
    theme->image.public.hotspot_x = (uint32_t)size / 24u;
    theme->image.public.hotspot_y = (uint32_t)size / 24u;
    theme->image.public.delay = 1000u;
    theme->images[0] = &theme->image.public;
    theme->cursor.image_count = 1u;
    theme->cursor.images = theme->images;
    return theme;

failed:
    saved_errno = errno ? errno : ENOMEM;
    if (pool)
        wl_shm_pool_destroy(pool);
    destroy_partial_theme(theme);
    errno = saved_errno;
    return NULL;
}

void wl_cursor_theme_destroy(struct wl_cursor_theme *theme)
{
    destroy_partial_theme(theme);
}

struct wl_cursor *wl_cursor_theme_get_cursor(struct wl_cursor_theme *theme,
                                              const char *name)
{
    if (!theme || !name || !name[0]) {
        errno = EINVAL;
        return NULL;
    }
    return &theme->cursor;
}

struct wl_buffer *wl_cursor_image_get_buffer(struct wl_cursor_image *image)
{
    struct armos_cursor_image *armos_image;

    if (!image) {
        errno = EINVAL;
        return NULL;
    }
    armos_image = (struct armos_cursor_image *)((char *)image -
        offsetof(struct armos_cursor_image, public));
    return armos_image->buffer;
}

int wl_cursor_frame_and_duration(struct wl_cursor *cursor, uint32_t time,
                                 uint32_t *duration)
{
    uint32_t total = 0u;

    if (!cursor || cursor->image_count == 0u || !cursor->images) {
        errno = EINVAL;
        return -1;
    }
    for (unsigned int index = 0u; index < cursor->image_count; index++)
        total += cursor->images[index]->delay ?
            cursor->images[index]->delay : 1u;
    time %= total;
    for (unsigned int index = 0u; index < cursor->image_count; index++) {
        uint32_t delay = cursor->images[index]->delay;

        if (delay == 0u)
            delay = 1u;
        if (time < delay) {
            if (duration)
                *duration = delay - time;
            return (int)index;
        }
        time -= delay;
    }
    errno = EINVAL;
    return -1;
}

int wl_cursor_frame(struct wl_cursor *cursor, uint32_t time)
{
    return wl_cursor_frame_and_duration(cursor, time, NULL);
}
