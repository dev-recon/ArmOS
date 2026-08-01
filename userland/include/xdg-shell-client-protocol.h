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
struct xdg_positioner;
struct xdg_surface;
struct xdg_toplevel;
struct xdg_popup;

extern const struct wl_interface xdg_wm_base_interface;
extern const struct wl_interface xdg_positioner_interface;
extern const struct wl_interface xdg_surface_interface;
extern const struct wl_interface xdg_toplevel_interface;
extern const struct wl_interface xdg_popup_interface;

enum xdg_positioner_anchor {
    XDG_POSITIONER_ANCHOR_NONE = 0,
    XDG_POSITIONER_ANCHOR_TOP = 1,
    XDG_POSITIONER_ANCHOR_BOTTOM = 2,
    XDG_POSITIONER_ANCHOR_LEFT = 4,
    XDG_POSITIONER_ANCHOR_RIGHT = 8
};

enum xdg_positioner_gravity {
    XDG_POSITIONER_GRAVITY_NONE = 0,
    XDG_POSITIONER_GRAVITY_TOP = 1,
    XDG_POSITIONER_GRAVITY_BOTTOM = 2,
    XDG_POSITIONER_GRAVITY_LEFT = 4,
    XDG_POSITIONER_GRAVITY_RIGHT = 8
};

enum xdg_positioner_constraint_adjustment {
    XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_NONE = 0,
    XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_SLIDE_X = 1,
    XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_SLIDE_Y = 2,
    XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_FLIP_X = 4,
    XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_FLIP_Y = 8,
    XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_RESIZE_X = 16,
    XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_RESIZE_Y = 32
};

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

struct xdg_popup_listener {
    void (*configure)(void *data, struct xdg_popup *xdg_popup,
                      int32_t x, int32_t y,
                      int32_t width, int32_t height);
    void (*popup_done)(void *data, struct xdg_popup *xdg_popup);
};

int xdg_wm_base_add_listener(
    struct xdg_wm_base *xdg_wm_base,
    const struct xdg_wm_base_listener *listener, void *data);
void xdg_wm_base_pong(struct xdg_wm_base *xdg_wm_base, uint32_t serial);
struct xdg_positioner *xdg_wm_base_create_positioner(
    struct xdg_wm_base *xdg_wm_base);
struct xdg_surface *xdg_wm_base_get_xdg_surface(
    struct xdg_wm_base *xdg_wm_base, struct wl_surface *surface);
void xdg_wm_base_destroy(struct xdg_wm_base *xdg_wm_base);

void xdg_positioner_set_size(struct xdg_positioner *positioner,
                             int32_t width, int32_t height);
void xdg_positioner_set_anchor_rect(struct xdg_positioner *positioner,
                                    int32_t x, int32_t y,
                                    int32_t width, int32_t height);
void xdg_positioner_set_anchor(struct xdg_positioner *positioner,
                               uint32_t anchor);
void xdg_positioner_set_gravity(struct xdg_positioner *positioner,
                                uint32_t gravity);
void xdg_positioner_set_constraint_adjustment(
    struct xdg_positioner *positioner, uint32_t constraint_adjustment);
void xdg_positioner_set_offset(struct xdg_positioner *positioner,
                               int32_t x, int32_t y);
void xdg_positioner_destroy(struct xdg_positioner *positioner);

int xdg_surface_add_listener(
    struct xdg_surface *xdg_surface,
    const struct xdg_surface_listener *listener, void *data);
struct xdg_toplevel *xdg_surface_get_toplevel(
    struct xdg_surface *xdg_surface);
struct xdg_popup *xdg_surface_get_popup(
    struct xdg_surface *xdg_surface, struct xdg_surface *parent,
    struct xdg_positioner *positioner);
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
void xdg_toplevel_move(struct xdg_toplevel *xdg_toplevel,
                       struct wl_seat *seat, uint32_t serial);
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

int xdg_popup_add_listener(
    struct xdg_popup *xdg_popup,
    const struct xdg_popup_listener *listener, void *data);
void xdg_popup_grab(struct xdg_popup *xdg_popup,
                    struct wl_seat *seat, uint32_t serial);
void xdg_popup_destroy(struct xdg_popup *xdg_popup);

#endif /* ARMOS_XDG_SHELL_CLIENT_PROTOCOL_H */
