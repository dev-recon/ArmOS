/*
 * ArmOS
 * Copyright (c) 2026 Mohamed Ennassiri
 *
 * Licensed under the Apache License, Version 2.0.
 * See LICENSE for details.
 *
 * File: userland/include/wayland-armos-gpu-client-protocol.h
 * Layer: Userland API / Wayland GPU buffer protocol
 *
 * Responsibilities:
 * - Exchange DRM buffer capability descriptors with the compositor.
 * - Associate an acquire fence with the next commit of a wl_surface.
 * - Report immediate or fenced release before a client reuses an image.
 *
 * Notes:
 * - DRM handles never appear on the wire; only transferable descriptors do.
 * - The protocol is independent of VirGL, VC4, V3D and CPU architecture.
 */

#ifndef WAYLAND_ARMOS_GPU_CLIENT_PROTOCOL_H
#define WAYLAND_ARMOS_GPU_CLIENT_PROTOCOL_H

#include <stdint.h>
#include <wayland-client-core.h>

#ifdef __cplusplus
extern "C" {
#endif

struct wl_buffer;
struct wl_surface;
struct armos_gpu_buffer_manager_v1;

extern const struct wl_interface armos_gpu_buffer_manager_v1_interface;

struct armos_gpu_buffer_manager_v1_listener {
    void (*fenced_release)(
        void *data, struct armos_gpu_buffer_manager_v1 *manager,
        struct wl_buffer *buffer, int fence_fd);
    void (*immediate_release)(
        void *data, struct armos_gpu_buffer_manager_v1 *manager,
        struct wl_buffer *buffer);
};

int armos_gpu_buffer_manager_v1_add_listener(
    struct armos_gpu_buffer_manager_v1 *manager,
    const struct armos_gpu_buffer_manager_v1_listener *listener,
    void *data);

struct wl_buffer *armos_gpu_buffer_manager_v1_create_buffer(
    struct armos_gpu_buffer_manager_v1 *manager, int buffer_fd);

int armos_gpu_buffer_manager_v1_set_acquire_fence(
    struct armos_gpu_buffer_manager_v1 *manager,
    struct wl_surface *surface, int fence_fd);

void armos_gpu_buffer_manager_v1_destroy(
    struct armos_gpu_buffer_manager_v1 *manager);

#ifdef __cplusplus
}
#endif

#endif /* WAYLAND_ARMOS_GPU_CLIENT_PROTOCOL_H */
