/*
 * ArmOS
 * Copyright (c) 2026 Mohamed Ennassiri
 *
 * Licensed under the Apache License, Version 2.0.
 * See LICENSE for details.
 *
 * File: userland/lib/wayland/egl.c
 * Layer: Userland library / Wayland EGL native windows
 *
 * Responsibilities:
 * - Implement the stable wl_egl_window lifecycle used by EGL clients.
 * - Record compositor configure dimensions atomically for the render thread.
 * - Validate the private backend ABI without depending on a GPU driver.
 */

#include <wayland-egl-backend.h>
#include <wayland-egl-core.h>

#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <wayland-client-core.h>

#define ARMOS_WL_EGL_WINDOW_MAGIC 0x5745474cu

struct wl_egl_window {
    uint32_t magic;
    uint32_t abi_version;
    struct wl_display *display;
    struct wl_surface *surface;
    int width;
    int height;
    int dx;
    int dy;
    uint32_t resize_sequence;
};

static int wl_egl_window_valid(const struct wl_egl_window *window)
{
    return window && window->magic == ARMOS_WL_EGL_WINDOW_MAGIC &&
        window->abi_version == ARMOS_WL_EGL_WINDOW_ABI_VERSION &&
        window->display && window->surface;
}

struct wl_egl_window *wl_egl_window_create(
    struct wl_surface *surface, int width, int height)
{
    struct wl_egl_window *window;

    if (!surface || width <= 0 || height <= 0) {
        errno = EINVAL;
        return NULL;
    }
    window = calloc(1, sizeof(*window));
    if (!window)
        return NULL;
    window->display = wl_proxy_get_display((struct wl_proxy *)surface);
    if (!window->display) {
        free(window);
        errno = EINVAL;
        return NULL;
    }
    window->magic = ARMOS_WL_EGL_WINDOW_MAGIC;
    window->abi_version = ARMOS_WL_EGL_WINDOW_ABI_VERSION;
    window->surface = surface;
    window->width = width;
    window->height = height;
    return window;
}

void wl_egl_window_destroy(struct wl_egl_window *window)
{
    if (!window)
        return;
    window->magic = 0u;
    free(window);
}

void wl_egl_window_resize(struct wl_egl_window *window,
                          int width, int height, int dx, int dy)
{
    if (!wl_egl_window_valid(window) || width <= 0 || height <= 0)
        return;
    (void)__atomic_add_fetch(&window->resize_sequence, 1u,
                             __ATOMIC_ACQ_REL);
    __atomic_store_n(&window->width, width, __ATOMIC_RELAXED);
    __atomic_store_n(&window->height, height, __ATOMIC_RELAXED);
    __atomic_store_n(&window->dx, dx, __ATOMIC_RELAXED);
    __atomic_store_n(&window->dy, dy, __ATOMIC_RELAXED);
    (void)__atomic_add_fetch(&window->resize_sequence, 1u,
                             __ATOMIC_RELEASE);
}

void wl_egl_window_get_attached_size(struct wl_egl_window *window,
                                     int *width, int *height)
{
    uint32_t current_width;
    uint32_t current_height;

    if (armos_wl_egl_window_get_size(window, &current_width,
                                     &current_height) < 0)
        return;
    if (width)
        *width = (int)current_width;
    if (height)
        *height = (int)current_height;
}

uint32_t armos_wl_egl_window_get_abi(
    const struct wl_egl_window *window)
{
    return wl_egl_window_valid(window) ? window->abi_version : 0u;
}

struct wl_display *armos_wl_egl_window_get_display(
    const struct wl_egl_window *window)
{
    return wl_egl_window_valid(window) ? window->display : NULL;
}

struct wl_surface *armos_wl_egl_window_get_surface(
    const struct wl_egl_window *window)
{
    return wl_egl_window_valid(window) ? window->surface : NULL;
}

int armos_wl_egl_window_get_size(
    const struct wl_egl_window *window, uint32_t *width, uint32_t *height)
{
    uint32_t before;
    uint32_t after;
    int current_width;
    int current_height;

    if (!wl_egl_window_valid(window) || !width || !height)
        return -1;
    for (;;) {
        before = __atomic_load_n(&window->resize_sequence,
                                 __ATOMIC_ACQUIRE);
        if ((before & 1u) != 0u)
            continue;
        current_width = __atomic_load_n(&window->width, __ATOMIC_RELAXED);
        current_height = __atomic_load_n(&window->height, __ATOMIC_RELAXED);
        after = __atomic_load_n(&window->resize_sequence, __ATOMIC_ACQUIRE);
        if (before == after && (after & 1u) == 0u)
            break;
    }
    if (current_width <= 0 || current_height <= 0)
        return -1;
    *width = (uint32_t)current_width;
    *height = (uint32_t)current_height;
    return 0;
}
