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
struct wl_event_loop;
struct wl_event_source;

enum wl_event_loop_fd_mask {
    WL_EVENT_READABLE = 0x01,
    WL_EVENT_WRITABLE = 0x02,
    WL_EVENT_HANGUP = 0x04,
    WL_EVENT_ERROR = 0x08
};

typedef int (*wl_event_loop_fd_func_t)(int fd, uint32_t mask, void *data);
typedef int (*wl_event_loop_timer_func_t)(void *data);
typedef void (*wl_event_loop_idle_func_t)(void *data);

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

#ifdef __cplusplus
}
#endif

#endif /* ARMOS_WAYLAND_SERVER_CORE_H */
