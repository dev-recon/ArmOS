/*
 * ArmOS
 * Copyright (c) 2026 Mohamed Ennassiri
 *
 * Licensed under the Apache License, Version 2.0.
 * See LICENSE for details.
 *
 * File: include/uapi/armos/drm.h
 * Layer: UAPI / ArmOS DRM ABI
 *
 * Responsibilities:
 * - Describe GPU capabilities without exposing a hardware or transport ABI.
 * - Keep the discovery contract identical on every architecture and platform.
 * - Reserve an extensible, versioned layout for future object and queue APIs.
 *
 * Notes:
 * - Driver names are diagnostic only and must not control application logic.
 * - command_set identifies an opaque userspace command protocol. A zero value
 *   means that only generic display/transfer services are available.
 */

#ifndef _UAPI_ARMOS_DRM_H
#define _UAPI_ARMOS_DRM_H

#define ARMOS_DRM_ABI_VERSION 1u

#define ARMOS_DRM_BACKEND_SOFTWARE       0u
#define ARMOS_DRM_BACKEND_PARAVIRTUAL    1u
#define ARMOS_DRM_BACKEND_NATIVE         2u

#define ARMOS_DRM_CAP_SCANOUT            (1ULL << 0)
#define ARMOS_DRM_CAP_TRANSFER_2D        (1ULL << 1)
#define ARMOS_DRM_CAP_BUFFER_OBJECTS     (1ULL << 2)
#define ARMOS_DRM_CAP_CONTEXTS           (1ULL << 3)
#define ARMOS_DRM_CAP_COMMAND_SUBMIT     (1ULL << 4)
#define ARMOS_DRM_CAP_FENCES             (1ULL << 5)
#define ARMOS_DRM_CAP_CPU_MAPPABLE       (1ULL << 6)
#define ARMOS_DRM_CAP_RENDER_3D          (1ULL << 7)
#define ARMOS_DRM_CAP_RESOURCE_TRANSFER  (1ULL << 8)

#define ARMOS_DRM_DRIVER_NAME_LENGTH     32u
#define ARMOS_DRM_COMMAND_SET_LENGTH     16u
#define ARMOS_DRM_MAX_COMMAND_SIZE       (256u * 1024u)
#define ARMOS_DRM_MAX_COMMAND_CAPS_SIZE  (16u * 1024u)

#define ARMOS_DRM_IOCTL_GET_INFO         0x4400u
#define ARMOS_DRM_IOCTL_CONTEXT_CREATE   0x4401u
#define ARMOS_DRM_IOCTL_CONTEXT_DESTROY  0x4402u
#define ARMOS_DRM_IOCTL_SUBMIT           0x4403u
#define ARMOS_DRM_IOCTL_FENCE_WAIT       0x4404u
#define ARMOS_DRM_IOCTL_BO_CREATE        0x4405u
#define ARMOS_DRM_IOCTL_BO_DESTROY       0x4406u
#define ARMOS_DRM_IOCTL_BO_MAP           0x4407u
#define ARMOS_DRM_IOCTL_RESOURCE_ATTACH  0x4408u
#define ARMOS_DRM_IOCTL_RESOURCE_DETACH  0x4409u
#define ARMOS_DRM_IOCTL_FENCE_DESTROY    0x440au
#define ARMOS_DRM_IOCTL_BO_PRESENT       0x440bu
#define ARMOS_DRM_IOCTL_GET_COMMAND_CAPS 0x440cu
#define ARMOS_DRM_IOCTL_BO_TRANSFER      0x440du

#define ARMOS_DRM_BO_CPU_READ            (1u << 0)
#define ARMOS_DRM_BO_CPU_WRITE           (1u << 1)
#define ARMOS_DRM_BO_COMMAND             (1u << 2)
#define ARMOS_DRM_BO_RENDER_TARGET       (1u << 3)
#define ARMOS_DRM_BO_TEXTURE             (1u << 4)
#define ARMOS_DRM_BO_SCANOUT             (1u << 5)
#define ARMOS_DRM_BO_VERTEX              (1u << 6)
#define ARMOS_DRM_BO_INDEX               (1u << 7)
#define ARMOS_DRM_BO_CONSTANT            (1u << 8)
#define ARMOS_DRM_BO_SHADER_STORAGE      (1u << 9)
#define ARMOS_DRM_BO_VALID_FLAGS         0x3ffu

#define ARMOS_DRM_FORMAT_NONE            0u
#define ARMOS_DRM_FORMAT_BGRA8888        1u
#define ARMOS_DRM_FORMAT_RGBA8888        2u

#define ARMOS_DRM_TRANSFER_CPU_TO_DEVICE 1u
#define ARMOS_DRM_TRANSFER_DEVICE_TO_CPU 2u

typedef struct armos_drm_info {
    unsigned int abi_version;
    unsigned int struct_size;
    unsigned int backend_class;
    unsigned int scanout_count;
    unsigned long long capabilities;
    unsigned int scanout_width;
    unsigned int scanout_height;
    unsigned int max_resource_width;
    unsigned int max_resource_height;
    char driver_name[ARMOS_DRM_DRIVER_NAME_LENGTH];
    unsigned char command_set[ARMOS_DRM_COMMAND_SET_LENGTH];
    unsigned int command_caps_max_version;
    unsigned int command_caps_size;
    unsigned int reserved[6];
} armos_drm_info_t;

typedef struct armos_drm_context_create {
    unsigned int abi_version;
    unsigned int flags;
    unsigned int context_id;
    unsigned int reserved0;
    unsigned char command_set[ARMOS_DRM_COMMAND_SET_LENGTH];
    unsigned int reserved[4];
} armos_drm_context_create_t;

typedef struct armos_drm_context_destroy {
    unsigned int context_id;
    unsigned int flags;
    unsigned int reserved[4];
} armos_drm_context_destroy_t;

typedef struct armos_drm_submit {
    unsigned int context_id;
    unsigned int flags;
    unsigned long long command_address;
    unsigned int command_size;
    unsigned int reserved0;
    unsigned long long fence_id;
    unsigned int reserved[4];
} armos_drm_submit_t;

typedef struct armos_drm_fence_wait {
    unsigned long long fence_id;
    long long timeout_ns;
    unsigned int flags;
    unsigned int reserved[5];
} armos_drm_fence_wait_t;

typedef struct armos_drm_fence_destroy {
    unsigned long long fence_id;
    unsigned int flags;
    unsigned int reserved[5];
} armos_drm_fence_destroy_t;

typedef struct armos_drm_bo_create {
    unsigned int abi_version;
    unsigned int flags;
    unsigned long long size;
    unsigned int width;
    unsigned int height;
    unsigned int stride;
    unsigned int format;
    unsigned int handle;
    unsigned int command_handle;
    unsigned long long map_offset;
    unsigned int reserved[4];
} armos_drm_bo_create_t;

typedef struct armos_drm_bo_destroy {
    unsigned int handle;
    unsigned int flags;
    unsigned int reserved[4];
} armos_drm_bo_destroy_t;

typedef struct armos_drm_bo_map {
    unsigned int handle;
    unsigned int flags;
    unsigned long long map_offset;
    unsigned long long size;
    unsigned int reserved[4];
} armos_drm_bo_map_t;

typedef struct armos_drm_resource_attachment {
    unsigned int context_id;
    unsigned int handle;
    unsigned int flags;
    unsigned int reserved[5];
} armos_drm_resource_attachment_t;

typedef struct armos_drm_bo_present {
    unsigned int handle;
    unsigned int scanout_id;
    unsigned int x;
    unsigned int y;
    unsigned int width;
    unsigned int height;
    unsigned int flags;
    unsigned int reserved[5];
} armos_drm_bo_present_t;

typedef struct armos_drm_command_caps {
    unsigned int abi_version;
    unsigned int flags;
    unsigned int version;
    unsigned int size;
    unsigned long long address;
    unsigned int reserved[4];
} armos_drm_command_caps_t;

typedef struct armos_drm_bo_transfer {
    unsigned int context_id;
    unsigned int handle;
    unsigned int direction;
    unsigned int level;
    unsigned int x;
    unsigned int y;
    unsigned int z;
    unsigned int width;
    unsigned int height;
    unsigned int depth;
    unsigned long long offset;
    unsigned int stride;
    unsigned int layer_stride;
    unsigned int flags;
    unsigned int reserved[4];
} armos_drm_bo_transfer_t;

#endif /* _UAPI_ARMOS_DRM_H */
