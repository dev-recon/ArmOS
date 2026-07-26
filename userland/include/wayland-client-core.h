/*
 * ArmOS
 * Copyright (c) 2026 Mohamed Ennassiri
 *
 * Licensed under the Apache License, Version 2.0.
 * See LICENSE for details.
 *
 * File: userland/include/wayland-client-core.h
 * Layer: Userland / Wayland client API
 *
 * Responsibilities:
 * - Declare display connection and proxy lifecycle entry points.
 * - Provide the stable base needed by generated client protocol bindings.
 *
 * Notes:
 * - Additional proxy marshaling entry points are introduced incrementally.
 */

#ifndef ARMOS_WAYLAND_CLIENT_CORE_H
#define ARMOS_WAYLAND_CLIENT_CORE_H

#include <wayland-util.h>

#ifdef __cplusplus
extern "C" {
#endif

struct wl_display;
struct wl_proxy;
struct wl_event_queue;

struct wl_display *wl_display_connect(const char *name);
void wl_display_disconnect(struct wl_display *display);
int wl_display_get_fd(struct wl_display *display);
int wl_display_get_error(struct wl_display *display);
int wl_display_flush(struct wl_display *display);

int wl_proxy_add_listener(struct wl_proxy *proxy,
                          void (**implementation)(void), void *data);
void wl_proxy_destroy(struct wl_proxy *proxy);
void wl_proxy_set_user_data(struct wl_proxy *proxy, void *user_data);
void *wl_proxy_get_user_data(struct wl_proxy *proxy);
uint32_t wl_proxy_get_id(struct wl_proxy *proxy);
uint32_t wl_proxy_get_version(struct wl_proxy *proxy);
const char *wl_proxy_get_class(struct wl_proxy *proxy);

#ifdef __cplusplus
}
#endif

#endif /* ARMOS_WAYLAND_CLIENT_CORE_H */
