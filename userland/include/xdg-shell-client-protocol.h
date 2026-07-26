/*
 * ArmOS
 * Copyright (c) 2026 Mohamed Ennassiri
 *
 * Licensed under the Apache License, Version 2.0.
 * See LICENSE for details.
 *
 * File: userland/include/xdg-shell-client-protocol.h
 * Layer: Userland / Wayland xdg-shell protocol
 *
 * Responsibilities:
 * - Declare typed client bindings for the stable xdg-shell protocol.
 * - Expose window-role configuration required by graphical applications.
 *
 * Notes:
 * - Protocol names follow the standardized Wayland public contract.
 * - Version 1 is the current compositor interoperability baseline.
 */

#ifndef ARMOS_XDG_SHELL_CLIENT_PROTOCOL_H
#define ARMOS_XDG_SHELL_CLIENT_PROTOCOL_H

#include <stdint.h>
#include <wayland-client.h>

struct xdg_wm_base;
struct xdg_surface;
struct xdg_toplevel;

extern const struct wl_interface xdg_wm_base_interface;
extern const struct wl_interface xdg_surface_interface;
extern const struct wl_interface xdg_toplevel_interface;

struct xdg_wm_base_listener {
    void (*ping)(void *data, struct xdg_wm_base *xdg_wm_base,
                 uint32_t serial);
};

struct xdg_surface_listener {
    void (*configure)(void *data, struct xdg_surface *xdg_surface,
                      uint32_t serial);
};

struct xdg_toplevel_listener {
    void (*configure)(void *data, struct xdg_toplevel *xdg_toplevel,
                      int32_t width, int32_t height,
                      struct wl_array *states);
    void (*close)(void *data, struct xdg_toplevel *xdg_toplevel);
};

int xdg_wm_base_add_listener(
    struct xdg_wm_base *xdg_wm_base,
    const struct xdg_wm_base_listener *listener, void *data);
void xdg_wm_base_pong(struct xdg_wm_base *xdg_wm_base, uint32_t serial);
struct xdg_surface *xdg_wm_base_get_xdg_surface(
    struct xdg_wm_base *xdg_wm_base, struct wl_surface *surface);
void xdg_wm_base_destroy(struct xdg_wm_base *xdg_wm_base);

int xdg_surface_add_listener(
    struct xdg_surface *xdg_surface,
    const struct xdg_surface_listener *listener, void *data);
struct xdg_toplevel *xdg_surface_get_toplevel(
    struct xdg_surface *xdg_surface);
void xdg_surface_set_window_geometry(struct xdg_surface *xdg_surface,
                                     int32_t x, int32_t y,
                                     int32_t width, int32_t height);
void xdg_surface_ack_configure(struct xdg_surface *xdg_surface,
                               uint32_t serial);
void xdg_surface_destroy(struct xdg_surface *xdg_surface);

int xdg_toplevel_add_listener(
    struct xdg_toplevel *xdg_toplevel,
    const struct xdg_toplevel_listener *listener, void *data);
void xdg_toplevel_set_title(struct xdg_toplevel *xdg_toplevel,
                            const char *title);
void xdg_toplevel_set_app_id(struct xdg_toplevel *xdg_toplevel,
                             const char *app_id);
void xdg_toplevel_set_max_size(struct xdg_toplevel *xdg_toplevel,
                               int32_t width, int32_t height);
void xdg_toplevel_set_min_size(struct xdg_toplevel *xdg_toplevel,
                               int32_t width, int32_t height);
void xdg_toplevel_set_maximized(struct xdg_toplevel *xdg_toplevel);
void xdg_toplevel_unset_maximized(struct xdg_toplevel *xdg_toplevel);
void xdg_toplevel_set_fullscreen(struct xdg_toplevel *xdg_toplevel,
                                 struct wl_output *output);
void xdg_toplevel_unset_fullscreen(struct xdg_toplevel *xdg_toplevel);
void xdg_toplevel_set_minimized(struct xdg_toplevel *xdg_toplevel);
void xdg_toplevel_destroy(struct xdg_toplevel *xdg_toplevel);

#endif /* ARMOS_XDG_SHELL_CLIENT_PROTOCOL_H */
