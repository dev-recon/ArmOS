/*
 * ArmOS
 * Copyright (c) 2026 Mohamed Ennassiri
 *
 * Licensed under the Apache License, Version 2.0.
 * See LICENSE for details.
 *
 * File: include/kernel/drm.h
 * Layer: Kernel / common ArmOS DRM device model
 *
 * Responsibilities:
 * - Expose architecture-neutral card and render nodes below /dev/dri.
 * - Register one platform backend behind the common GPU contract.
 * - Validate and translate the stable userspace GPU ABI.
 *
 * Notes:
 * - This interface must never include platform or architecture headers.
 * - Hardware discovery and command encoding belong to platform backends.
 */

#ifndef _KERNEL_DRM_H
#define _KERNEL_DRM_H

#include <kernel/drm_backend.h>
#include <kernel/task.h>
#include <kernel/types.h>

typedef enum armos_drm_node {
    ARMOS_DRM_NODE_INVALID = 0,
    ARMOS_DRM_NODE_CARD,
    ARMOS_DRM_NODE_RENDER,
} armos_drm_node_t;

int armos_drm_backend_register(const armos_drm_backend_ops_t *ops,
                               void *context);
bool armos_drm_device_available(void);
armos_drm_node_t armos_drm_node_from_path(const char *path);
void fill_armos_drm_device_stat(struct stat *st, armos_drm_node_t node);
int create_armos_drm_device_file(const char *name, int flags,
                                 armos_drm_node_t node, file_t **out_file);
int armos_drm_device_ioctl(file_t *file, uint32_t request, uintptr_t arg);
void *armos_drm_map_fd(int fd, void *hint, size_t length,
                       uint32_t vma_flags, uint64_t offset);
void armos_drm_fence_complete(uint64_t fence_id, int status);
bool armos_drm_fence_file_read_ready(file_t *file);

#endif /* _KERNEL_DRM_H */
