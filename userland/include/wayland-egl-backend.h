/*
 * ArmOS
 * Copyright (c) 2026 Mohamed Ennassiri
 *
 * Licensed under the Apache License, Version 2.0.
 * See LICENSE for details.
 *
 * File: userland/include/wayland-egl-backend.h
 * Layer: Userland ABI / EGL platform backend
 *
 * Responsibilities:
 * - Validate native windows passed to EGL platform implementations.
 * - Expose only the display, surface and current configure dimensions.
 * - Keep the application-facing native-window structure opaque.
 */

#ifndef ARMOS_WAYLAND_EGL_BACKEND_H
#define ARMOS_WAYLAND_EGL_BACKEND_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct wl_display;
struct wl_surface;
struct wl_egl_window;

#define ARMOS_WL_EGL_WINDOW_ABI_VERSION 1u

uint32_t armos_wl_egl_window_get_abi(
    const struct wl_egl_window *window);
struct wl_display *armos_wl_egl_window_get_display(
    const struct wl_egl_window *window);
struct wl_surface *armos_wl_egl_window_get_surface(
    const struct wl_egl_window *window);
int armos_wl_egl_window_get_size(
    const struct wl_egl_window *window, uint32_t *width, uint32_t *height);

#ifdef __cplusplus
}
#endif

#endif /* ARMOS_WAYLAND_EGL_BACKEND_H */
