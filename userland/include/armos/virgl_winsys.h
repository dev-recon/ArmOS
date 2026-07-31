/*
 * ArmOS
 * Copyright (c) 2026 Mohamed Ennassiri
 *
 * Licensed under the Apache License, Version 2.0.
 * See LICENSE for details.
 *
 * File: userland/include/armos/virgl_winsys.h
 * Layer: Userland / VirGL winsys
 *
 * Responsibilities:
 * - Wrap the architecture-neutral ArmOS DRM ABI for the Mesa VirGL driver.
 * - Own command-capability discovery and explicit fence lifetimes.
 * - Keep VirtIO transport details out of Mesa, Raylib and applications.
 *
 * Notes:
 * - This is a transport/winsys API, not an OpenGL implementation.
 * - Callers still encode commands according to the discovered command set.
 */

#ifndef _ARMOS_VIRGL_WINSYS_H
#define _ARMOS_VIRGL_WINSYS_H

#include <stddef.h>
#include <stdint.h>
#include <uapi/armos/drm.h>
#include <uapi/armos/drm_virgl.h>

typedef struct armos_virgl_device {
    int fd;
    armos_drm_info_t info;
    void *command_caps;
    uint32_t command_caps_size;
    uint32_t command_caps_version;
} armos_virgl_device_t;

typedef struct armos_virgl_buffer {
    uint32_t handle;
    uint32_t command_handle;
    uint32_t flags;
    uint32_t width;
    uint32_t height;
    uint32_t stride;
    uint32_t format;
    uint64_t size;
    uint64_t map_offset;
    void *mapping;
} armos_virgl_buffer_t;

int armos_virgl_open(armos_virgl_device_t *device, const char *path);
void armos_virgl_close(armos_virgl_device_t *device);

int armos_virgl_context_create(armos_virgl_device_t *device,
                               uint32_t *context_id);
int armos_virgl_context_destroy(armos_virgl_device_t *device,
                                uint32_t context_id);

int armos_virgl_buffer_create(armos_virgl_device_t *device,
                              armos_virgl_buffer_t *buffer,
                              uint64_t size, uint32_t flags,
                              uint32_t width, uint32_t height,
                              uint32_t stride, uint32_t format);
int armos_virgl_resource_create(
    armos_virgl_device_t *device, armos_virgl_buffer_t *buffer,
    uint64_t size, uint32_t flags,
    const armos_drm_virgl_resource_descriptor_t *descriptor);
int armos_virgl_buffer_map(armos_virgl_device_t *device,
                           armos_virgl_buffer_t *buffer);
int armos_virgl_buffer_unmap(armos_virgl_buffer_t *buffer);
int armos_virgl_buffer_destroy(armos_virgl_device_t *device,
                               armos_virgl_buffer_t *buffer);
int armos_virgl_buffer_attach(armos_virgl_device_t *device,
                              uint32_t context_id,
                              const armos_virgl_buffer_t *buffer);
int armos_virgl_buffer_detach(armos_virgl_device_t *device,
                              uint32_t context_id,
                              const armos_virgl_buffer_t *buffer);
int armos_virgl_buffer_transfer(armos_virgl_device_t *device,
                                uint32_t context_id,
                                const armos_virgl_buffer_t *buffer,
                                uint32_t direction, uint32_t level,
                                uint32_t x, uint32_t y, uint32_t z,
                                uint32_t width, uint32_t height,
                                uint32_t depth, uint64_t offset,
                                uint32_t stride, uint32_t layer_stride);

int armos_virgl_submit(armos_virgl_device_t *device, uint32_t context_id,
                       const void *commands, uint32_t command_size,
                       uint64_t *fence_id);
int armos_virgl_fence_wait(armos_virgl_device_t *device, uint64_t fence_id,
                           int64_t timeout_ns);
int armos_virgl_fence_destroy(armos_virgl_device_t *device,
                              uint64_t fence_id);

#endif /* _ARMOS_VIRGL_WINSYS_H */
