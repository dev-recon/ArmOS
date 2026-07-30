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

#define ARMOS_DRM_DRIVER_NAME_LENGTH     32u
#define ARMOS_DRM_COMMAND_SET_LENGTH     16u
#define ARMOS_DRM_MAX_COMMAND_SIZE       (256u * 1024u)

#define ARMOS_DRM_IOCTL_GET_INFO         0x4400u
#define ARMOS_DRM_IOCTL_CONTEXT_CREATE   0x4401u
#define ARMOS_DRM_IOCTL_CONTEXT_DESTROY  0x4402u
#define ARMOS_DRM_IOCTL_SUBMIT           0x4403u
#define ARMOS_DRM_IOCTL_FENCE_WAIT       0x4404u

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
    unsigned int reserved[8];
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

#endif /* _UAPI_ARMOS_DRM_H */
