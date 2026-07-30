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
    const char *driver_name;
    uint8_t command_set[16];
} armos_drm_backend_info_t;

typedef struct armos_drm_backend_ops {
    int (*get_info)(void *context, armos_drm_backend_info_t *info);
    int (*context_create)(void *context, uint32_t context_id);
    int (*context_destroy)(void *context, uint32_t context_id);
    int (*submit)(void *context, uint32_t context_id,
                  const void *commands, uint32_t command_size,
                  uint64_t fence_id);
} armos_drm_backend_ops_t;

#endif /* _KERNEL_DRM_BACKEND_H */
