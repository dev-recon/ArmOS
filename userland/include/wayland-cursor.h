/*
 * ArmOS
 * Copyright (c) 2026 Mohamed Ennassiri
 *
 * Licensed under the Apache License, Version 2.0.
 * See LICENSE for details.
 *
 * File: userland/include/wayland-cursor.h
 * Layer: Userland / Wayland cursor compatibility
 *
 * Responsibilities:
 * - Expose the conventional libwayland-cursor client API.
 * - Describe cursor images backed by Wayland shared-memory buffers.
 *
 * Notes:
 * - ArmOS currently supplies a built-in software cursor theme.
 */

#ifndef ARMOS_WAYLAND_CURSOR_H
#define ARMOS_WAYLAND_CURSOR_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct wl_buffer;
struct wl_shm;
struct wl_cursor_theme;

struct wl_cursor_image {
    uint32_t width;
    uint32_t height;
    uint32_t hotspot_x;
    uint32_t hotspot_y;
    uint32_t delay;
};

struct wl_cursor {
    unsigned int image_count;
    struct wl_cursor_image **images;
    char *name;
};

struct wl_cursor_theme *wl_cursor_theme_load(const char *name, int size,
                                              struct wl_shm *shm);
void wl_cursor_theme_destroy(struct wl_cursor_theme *theme);
struct wl_cursor *wl_cursor_theme_get_cursor(struct wl_cursor_theme *theme,
                                              const char *name);
struct wl_buffer *wl_cursor_image_get_buffer(
    struct wl_cursor_image *image);
int wl_cursor_frame(struct wl_cursor *cursor, uint32_t time);
int wl_cursor_frame_and_duration(struct wl_cursor *cursor, uint32_t time,
                                 uint32_t *duration);

#ifdef __cplusplus
}
#endif

#endif /* ARMOS_WAYLAND_CURSOR_H */
