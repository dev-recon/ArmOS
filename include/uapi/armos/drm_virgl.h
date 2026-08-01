/*
 * ArmOS
 * Copyright (c) 2026 Mohamed Ennassiri
 *
 * Licensed under the Apache License, Version 2.0.
 * See LICENSE for details.
 *
 * File: include/uapi/armos/drm_virgl.h
 * Layer: UAPI / optional VirGL command-set contract
 *
 * Responsibilities:
 * - Describe resources required by the VirGL command protocol.
 * - Keep VirGL values out of the common DRM object model.
 * - Provide one shared contract for the qemu-virt backend and Mesa winsys.
 *
 * Notes:
 * - The common DRM core treats this structure as opaque bytes.
 * - Applications must negotiate a virgl command set before using it.
 */

#ifndef _UAPI_ARMOS_DRM_VIRGL_H
#define _UAPI_ARMOS_DRM_VIRGL_H

#define ARMOS_DRM_VIRGL_RESOURCE_ABI_VERSION 1u

typedef struct armos_drm_virgl_resource_descriptor {
    unsigned int abi_version;
    unsigned int struct_size;
    unsigned int target;
    unsigned int format;
    unsigned int bind;
    unsigned int width;
    unsigned int height;
    unsigned int depth;
    unsigned int array_size;
    unsigned int last_level;
    unsigned int nr_samples;
    unsigned int flags;
    unsigned int reserved[4];
} armos_drm_virgl_resource_descriptor_t;

#endif /* _UAPI_ARMOS_DRM_VIRGL_H */
