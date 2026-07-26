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
struct wl_seat;
struct wl_pointer;
struct wl_keyboard;

extern const struct wl_interface wl_display_interface;
extern const struct wl_interface wl_registry_interface;
extern const struct wl_interface wl_callback_interface;
extern const struct wl_interface wl_compositor_interface;
extern const struct wl_interface wl_surface_interface;
extern const struct wl_interface wl_region_interface;
extern const struct wl_interface wl_shm_interface;
extern const struct wl_interface wl_shm_pool_interface;
extern const struct wl_interface wl_buffer_interface;
extern const struct wl_interface wl_seat_interface;
extern const struct wl_interface wl_pointer_interface;
extern const struct wl_interface wl_keyboard_interface;

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

struct wl_seat_listener {
    void (*capabilities)(void *data, struct wl_seat *seat,
                         uint32_t capabilities);
    void (*name)(void *data, struct wl_seat *seat, const char *name);
};

struct wl_pointer_listener {
    void (*enter)(void *data, struct wl_pointer *pointer, uint32_t serial,
                  struct wl_surface *surface, wl_fixed_t surface_x,
                  wl_fixed_t surface_y);
    void (*leave)(void *data, struct wl_pointer *pointer, uint32_t serial,
                  struct wl_surface *surface);
    void (*motion)(void *data, struct wl_pointer *pointer, uint32_t time,
                   wl_fixed_t surface_x, wl_fixed_t surface_y);
    void (*button)(void *data, struct wl_pointer *pointer, uint32_t serial,
                   uint32_t time, uint32_t button, uint32_t state);
    void (*axis)(void *data, struct wl_pointer *pointer, uint32_t time,
                 uint32_t axis, wl_fixed_t value);
};

struct wl_keyboard_listener {
    void (*keymap)(void *data, struct wl_keyboard *keyboard, uint32_t format,
                   int32_t fd, uint32_t size);
    void (*enter)(void *data, struct wl_keyboard *keyboard, uint32_t serial,
                  struct wl_surface *surface, struct wl_array *keys);
    void (*leave)(void *data, struct wl_keyboard *keyboard, uint32_t serial,
                  struct wl_surface *surface);
    void (*key)(void *data, struct wl_keyboard *keyboard, uint32_t serial,
                uint32_t time, uint32_t key, uint32_t state);
    void (*modifiers)(void *data, struct wl_keyboard *keyboard,
                      uint32_t serial, uint32_t mods_depressed,
                      uint32_t mods_latched, uint32_t mods_locked,
                      uint32_t group);
    void (*repeat_info)(void *data, struct wl_keyboard *keyboard,
                        int32_t rate, int32_t delay);
};

enum wl_shm_format {
    WL_SHM_FORMAT_ARGB8888 = 0,
    WL_SHM_FORMAT_XRGB8888 = 1
};

enum wl_seat_capability {
    WL_SEAT_CAPABILITY_POINTER = 1,
    WL_SEAT_CAPABILITY_KEYBOARD = 2,
    WL_SEAT_CAPABILITY_TOUCH = 4
};

enum wl_pointer_button_state {
    WL_POINTER_BUTTON_STATE_RELEASED = 0,
    WL_POINTER_BUTTON_STATE_PRESSED = 1
};

enum wl_pointer_axis {
    WL_POINTER_AXIS_VERTICAL_SCROLL = 0,
    WL_POINTER_AXIS_HORIZONTAL_SCROLL = 1
};

enum wl_keyboard_key_state {
    WL_KEYBOARD_KEY_STATE_RELEASED = 0,
    WL_KEYBOARD_KEY_STATE_PRESSED = 1
};

enum wl_keyboard_keymap_format {
    WL_KEYBOARD_KEYMAP_FORMAT_NO_KEYMAP = 0,
    WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1 = 1
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

int wl_seat_add_listener(struct wl_seat *seat,
                         const struct wl_seat_listener *listener, void *data);
struct wl_pointer *wl_seat_get_pointer(struct wl_seat *seat);
struct wl_keyboard *wl_seat_get_keyboard(struct wl_seat *seat);
void wl_seat_destroy(struct wl_seat *seat);

int wl_pointer_add_listener(struct wl_pointer *pointer,
                            const struct wl_pointer_listener *listener,
                            void *data);
void wl_pointer_set_cursor(struct wl_pointer *pointer, uint32_t serial,
                           struct wl_surface *surface, int32_t hotspot_x,
                           int32_t hotspot_y);
void wl_pointer_destroy(struct wl_pointer *pointer);

int wl_keyboard_add_listener(struct wl_keyboard *keyboard,
                             const struct wl_keyboard_listener *listener,
                             void *data);
void wl_keyboard_destroy(struct wl_keyboard *keyboard);

#endif /* ARMOS_WAYLAND_CLIENT_PROTOCOL_H */
