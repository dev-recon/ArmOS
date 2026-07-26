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
 * - Declare typed client bindings for the Wayland core object set.
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
struct wl_compositor;
struct wl_surface;
struct wl_region;
struct wl_shm;
struct wl_shm_pool;
struct wl_buffer;
struct wl_output;

extern const struct wl_interface wl_display_interface;
extern const struct wl_interface wl_registry_interface;
extern const struct wl_interface wl_callback_interface;
extern const struct wl_interface wl_compositor_interface;
extern const struct wl_interface wl_surface_interface;
extern const struct wl_interface wl_region_interface;
extern const struct wl_interface wl_shm_interface;
extern const struct wl_interface wl_shm_pool_interface;
extern const struct wl_interface wl_buffer_interface;

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

struct wl_shm_listener {
    void (*format)(void *data, struct wl_shm *shm, uint32_t format);
};

struct wl_buffer_listener {
    void (*release)(void *data, struct wl_buffer *buffer);
};

struct wl_surface_listener {
    void (*enter)(void *data, struct wl_surface *surface,
                  struct wl_output *output);
    void (*leave)(void *data, struct wl_surface *surface,
                  struct wl_output *output);
};

enum wl_shm_format {
    WL_SHM_FORMAT_ARGB8888 = 0,
    WL_SHM_FORMAT_XRGB8888 = 1
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

struct wl_surface *wl_compositor_create_surface(
    struct wl_compositor *compositor);
struct wl_region *wl_compositor_create_region(
    struct wl_compositor *compositor);
void wl_compositor_destroy(struct wl_compositor *compositor);

int wl_shm_add_listener(struct wl_shm *shm,
                        const struct wl_shm_listener *listener, void *data);
struct wl_shm_pool *wl_shm_create_pool(struct wl_shm *shm, int fd,
                                       int32_t size);
void wl_shm_destroy(struct wl_shm *shm);

struct wl_buffer *wl_shm_pool_create_buffer(struct wl_shm_pool *pool,
                                             int32_t offset, int32_t width,
                                             int32_t height, int32_t stride,
                                             uint32_t format);
void wl_shm_pool_resize(struct wl_shm_pool *pool, int32_t size);
void wl_shm_pool_destroy(struct wl_shm_pool *pool);

int wl_buffer_add_listener(struct wl_buffer *buffer,
                           const struct wl_buffer_listener *listener,
                           void *data);
void wl_buffer_destroy(struct wl_buffer *buffer);

int wl_surface_add_listener(struct wl_surface *surface,
                            const struct wl_surface_listener *listener,
                            void *data);
void wl_surface_attach(struct wl_surface *surface, struct wl_buffer *buffer,
                       int32_t x, int32_t y);
void wl_surface_damage(struct wl_surface *surface, int32_t x, int32_t y,
                       int32_t width, int32_t height);
struct wl_callback *wl_surface_frame(struct wl_surface *surface);
void wl_surface_set_opaque_region(struct wl_surface *surface,
                                  struct wl_region *region);
void wl_surface_set_input_region(struct wl_surface *surface,
                                 struct wl_region *region);
void wl_surface_commit(struct wl_surface *surface);
void wl_surface_destroy(struct wl_surface *surface);

void wl_region_add(struct wl_region *region, int32_t x, int32_t y,
                   int32_t width, int32_t height);
void wl_region_subtract(struct wl_region *region, int32_t x, int32_t y,
                        int32_t width, int32_t height);
void wl_region_destroy(struct wl_region *region);

#endif /* ARMOS_WAYLAND_CLIENT_PROTOCOL_H */
