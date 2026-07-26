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
 * - Protocol-specific request dispatch remains owned by server implementations.
 * - No architecture or platform-specific contract is exposed here.
 */

#ifndef ARMOS_WAYLAND_SERVER_CORE_H
#define ARMOS_WAYLAND_SERVER_CORE_H

#include <wayland-util.h>

#ifdef __cplusplus
extern "C" {
#endif

struct wl_display;
struct wl_event_loop;
struct wl_event_source;
struct wl_client;
struct wl_global;
struct wl_resource;

enum wl_event_loop_fd_mask {
    WL_EVENT_READABLE = 0x01,
    WL_EVENT_WRITABLE = 0x02,
    WL_EVENT_HANGUP = 0x04,
    WL_EVENT_ERROR = 0x08
};

typedef int (*wl_event_loop_fd_func_t)(int fd, uint32_t mask, void *data);
typedef int (*wl_event_loop_timer_func_t)(void *data);
typedef void (*wl_event_loop_idle_func_t)(void *data);
typedef void (*wl_global_bind_func_t)(struct wl_client *client, void *data,
                                     uint32_t version, uint32_t id);
typedef void (*wl_resource_destroy_func_t)(struct wl_resource *resource);

struct wl_display *wl_display_create(void);
void wl_display_destroy(struct wl_display *display);
int wl_display_add_socket(struct wl_display *display, const char *name);
int wl_display_get_server_fd(struct wl_display *display);
struct wl_event_loop *wl_display_get_event_loop(struct wl_display *display);
void wl_display_run(struct wl_display *display);
void wl_display_terminate(struct wl_display *display);

struct wl_event_source *wl_event_loop_add_fd(
    struct wl_event_loop *loop, int fd, uint32_t mask,
    wl_event_loop_fd_func_t func, void *data);
int wl_event_source_fd_update(struct wl_event_source *source, uint32_t mask);
struct wl_event_source *wl_event_loop_add_timer(
    struct wl_event_loop *loop, wl_event_loop_timer_func_t func, void *data);
int wl_event_source_timer_update(struct wl_event_source *source, int ms_delay);
struct wl_event_source *wl_event_loop_add_idle(
    struct wl_event_loop *loop, wl_event_loop_idle_func_t func, void *data);
int wl_event_source_remove(struct wl_event_source *source);
int wl_event_loop_dispatch(struct wl_event_loop *loop, int timeout);

struct wl_client *wl_client_create(struct wl_display *display, int fd);
void wl_client_destroy(struct wl_client *client);
void wl_client_flush(struct wl_client *client);
int wl_client_get_fd(struct wl_client *client);
struct wl_display *wl_client_get_display(struct wl_client *client);
struct wl_resource *wl_client_get_object(struct wl_client *client,
                                         uint32_t id);

struct wl_global *wl_global_create(
    struct wl_display *display, const struct wl_interface *interface,
    int version, void *data, wl_global_bind_func_t bind);
void wl_global_destroy(struct wl_global *global);
const struct wl_interface *wl_global_get_interface(
    const struct wl_global *global);
uint32_t wl_global_get_name(const struct wl_global *global);
uint32_t wl_global_get_version(const struct wl_global *global);
void *wl_global_get_user_data(const struct wl_global *global);

struct wl_resource *wl_resource_create(
    struct wl_client *client, const struct wl_interface *interface,
    int version, uint32_t id);
void wl_resource_set_implementation(
    struct wl_resource *resource, const void *implementation, void *data,
    wl_resource_destroy_func_t destroy);
void wl_resource_post_event(struct wl_resource *resource, uint32_t opcode,
                            ...);
void wl_resource_post_event_array(struct wl_resource *resource,
                                  uint32_t opcode,
                                  union wl_argument *arguments);
void wl_resource_destroy(struct wl_resource *resource);
uint32_t wl_resource_get_id(struct wl_resource *resource);
int wl_resource_get_version(struct wl_resource *resource);
const char *wl_resource_get_class(struct wl_resource *resource);
struct wl_client *wl_resource_get_client(struct wl_resource *resource);
void wl_resource_set_user_data(struct wl_resource *resource, void *data);
void *wl_resource_get_user_data(struct wl_resource *resource);
int wl_resource_instance_of(struct wl_resource *resource,
                            const struct wl_interface *interface,
                            const void *implementation);

#ifdef __cplusplus
}
#endif

#endif /* ARMOS_WAYLAND_SERVER_CORE_H */
