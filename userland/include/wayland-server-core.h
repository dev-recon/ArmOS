/*
 * ArmOS
 * Copyright (c) 2026 Mohamed Ennassiri
 *
 * Licensed under the Apache License, Version 2.0.
 * See LICENSE for details.
 *
 * File: userland/include/wayland-server-core.h
 * Layer: Userland / Wayland server API
 *
 * Responsibilities:
 * - Declare the server display and listening-socket lifecycle.
 * - Establish the migration boundary for armos-wlcomp.
 *
 * Notes:
 * - Event loops and resources are added on top of this base.
 */

#ifndef ARMOS_WAYLAND_SERVER_CORE_H
#define ARMOS_WAYLAND_SERVER_CORE_H

#include <wayland-util.h>

#ifdef __cplusplus
extern "C" {
#endif

struct wl_display;

struct wl_display *wl_display_create(void);
void wl_display_destroy(struct wl_display *display);
int wl_display_add_socket(struct wl_display *display, const char *name);
int wl_display_get_server_fd(struct wl_display *display);

#ifdef __cplusplus
}
#endif

#endif /* ARMOS_WAYLAND_SERVER_CORE_H */
