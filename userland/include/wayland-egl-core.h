/*
 * ArmOS
 * Copyright (c) 2026 Mohamed Ennassiri
 *
 * Licensed under the Apache License, Version 2.0.
 * See LICENSE for details.
 *
 * File: userland/include/wayland-egl-core.h
 * Layer: Userland API / Wayland EGL native windows
 *
 * Responsibilities:
 * - Bind a wl_surface to the dimensions expected by EGL window surfaces.
 * - Publish configure-driven resizes without exposing Mesa internals.
 * - Preserve the conventional libwayland-egl application contract.
 */

#ifndef ARMOS_WAYLAND_EGL_CORE_H
#define ARMOS_WAYLAND_EGL_CORE_H

#ifdef __cplusplus
extern "C" {
#endif

struct wl_surface;
struct wl_egl_window;

struct wl_egl_window *wl_egl_window_create(
    struct wl_surface *surface, int width, int height);
void wl_egl_window_destroy(struct wl_egl_window *window);
void wl_egl_window_resize(struct wl_egl_window *window,
                          int width, int height, int dx, int dy);
void wl_egl_window_get_attached_size(struct wl_egl_window *window,
                                     int *width, int *height);

#ifdef __cplusplus
}
#endif

#endif /* ARMOS_WAYLAND_EGL_CORE_H */
