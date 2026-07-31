/*
 * ArmOS
 * Copyright (c) 2026 Mohamed Ennassiri
 *
 * Licensed under the Apache License, Version 2.0.
 * See LICENSE for details.
 *
 * File: include/kernel/drm_backend.h
 * Layer: Kernel / ArmOS DRM backend contract
 *
 * Responsibilities:
 * - Isolate the common GPU device model from platform GPU implementations.
 * - Publish generic capabilities and limits to the common GPU core.
 * - Avoid hardware identifiers, registers, packets, and architecture details.
 *
 * Notes:
 * - A platform registers one backend after completing hardware initialization.
 * - Operations are extended only with hardware-independent object semantics.
 */

#ifndef _KERNEL_DRM_BACKEND_H
#define _KERNEL_DRM_BACKEND_H

#include <kernel/types.h>

typedef struct armos_drm_backend_info {
    uint32_t backend_class;
    uint64_t capabilities;
    uint32_t scanout_count;
    uint32_t scanout_width;
    uint32_t scanout_height;
    uint32_t max_resource_width;
    uint32_t max_resource_height;
    uint32_t command_caps_max_version;
    uint32_t command_caps_size;
    const char *driver_name;
    uint8_t command_set[16];
} armos_drm_backend_info_t;

typedef struct armos_drm_buffer_desc {
    uint64_t size;
    uint32_t flags;
    uint32_t width;
    uint32_t height;
    uint32_t stride;
    uint32_t format;
} armos_drm_buffer_desc_t;

typedef struct armos_drm_memory_segment {
    paddr_t address;
    uint32_t length;
} armos_drm_memory_segment_t;

typedef struct armos_drm_backend_ops {
    int (*get_info)(void *context, armos_drm_backend_info_t *info);
    int (*context_create)(void *context, uint32_t context_id);
    int (*context_destroy)(void *context, uint32_t context_id);
    int (*buffer_create)(void *context, uint32_t resource_id,
                         const armos_drm_buffer_desc_t *desc,
                         const armos_drm_memory_segment_t *segments,
                         uint32_t segment_count,
                         uint32_t *command_handle);
    int (*buffer_destroy)(void *context, uint32_t resource_id);
    int (*resource_attach)(void *context, uint32_t context_id,
                           uint32_t resource_id);
    int (*resource_detach)(void *context, uint32_t context_id,
                           uint32_t resource_id);
    int (*buffer_present)(void *context, uint32_t resource_id,
                          const armos_drm_buffer_desc_t *desc,
                          uint32_t scanout_id, uint32_t x, uint32_t y,
                          uint32_t width, uint32_t height);
    int (*get_command_caps)(void *context, uint32_t version,
                            void *data, uint32_t size);
    int (*buffer_transfer)(void *context, uint32_t context_id,
                           uint32_t resource_id, uint32_t direction,
                           uint32_t level, uint32_t x, uint32_t y,
                           uint32_t z, uint32_t width, uint32_t height,
                           uint32_t depth, uint64_t offset,
                           uint32_t stride, uint32_t layer_stride);
    int (*submit)(void *context, uint32_t context_id,
                  const void *commands, uint32_t command_size,
                  uint64_t fence_id);
} armos_drm_backend_ops_t;

#endif /* _KERNEL_DRM_BACKEND_H */
