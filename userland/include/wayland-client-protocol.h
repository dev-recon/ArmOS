/*
 * ArmOS
 * Copyright (c) 2026 Mohamed Ennassiri
 *
 * Licensed under the Apache License, Version 2.0.
 * See LICENSE for details.
 *
 * File: userland/include/wayland-client-protocol.h
 * Layer: Userland / Wayland core protocol
 *
 * Responsibilities:
 * - Declare typed client bindings for wl_display, wl_registry and wl_callback.
 * - Expose core interface descriptors used by generated protocol bindings.
 *
 * Notes:
 * - Further core objects are added as their dispatch paths become available.
 */

#ifndef ARMOS_WAYLAND_CLIENT_PROTOCOL_H
#define ARMOS_WAYLAND_CLIENT_PROTOCOL_H

#include <stdint.h>
#include <wayland-client-core.h>

struct wl_registry;
struct wl_callback;

extern const struct wl_interface wl_display_interface;
extern const struct wl_interface wl_registry_interface;
extern const struct wl_interface wl_callback_interface;

struct wl_registry_listener {
    void (*global)(void *data, struct wl_registry *registry, uint32_t name,
                   const char *interface, uint32_t version);
    void (*global_remove)(void *data, struct wl_registry *registry,
                          uint32_t name);
};

struct wl_callback_listener {
    void (*done)(void *data, struct wl_callback *callback,
                 uint32_t callback_data);
};

struct wl_registry *wl_display_get_registry(struct wl_display *display);
struct wl_callback *wl_display_sync(struct wl_display *display);
int wl_display_dispatch(struct wl_display *display);
int wl_display_dispatch_pending(struct wl_display *display);
int wl_display_roundtrip(struct wl_display *display);

int wl_registry_add_listener(struct wl_registry *registry,
                             const struct wl_registry_listener *listener,
                             void *data);
void *wl_registry_bind(struct wl_registry *registry, uint32_t name,
                       const struct wl_interface *interface,
                       uint32_t version);
void wl_registry_destroy(struct wl_registry *registry);

int wl_callback_add_listener(struct wl_callback *callback,
                             const struct wl_callback_listener *listener,
                             void *data);
void wl_callback_destroy(struct wl_callback *callback);

#endif /* ARMOS_WAYLAND_CLIENT_PROTOCOL_H */
