/*
 * ArmOS
 * Copyright (c) 2026 Mohamed Ennassiri
 *
 * Licensed under the Apache License, Version 2.0.
 * See LICENSE for details.
 *
 * File: userland/lib/virgl/winsys.c
 * Layer: Userland / VirGL winsys
 *
 * Responsibilities:
 * - Adapt ArmOS DRM objects and fences to a compact VirGL-facing API.
 * - Validate the required command protocol and retrieve its capability blob.
 * - Provide deterministic cleanup paths suitable for a Mesa winsys adapter.
 */

#include <armos/virgl_winsys.h>

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

static int armos_virgl_ioctl(int fd, unsigned long request, void *argument)
{
    if (fd < 0 || !argument) {
        errno = EINVAL;
        return -1;
    }
    return ioctl(fd, request, argument);
}

int armos_virgl_open(armos_virgl_device_t *device, const char *path)
{
    const uint64_t required = ARMOS_DRM_CAP_BUFFER_OBJECTS |
                              ARMOS_DRM_CAP_CONTEXTS |
                              ARMOS_DRM_CAP_COMMAND_SUBMIT |
                              ARMOS_DRM_CAP_FENCES |
                              ARMOS_DRM_CAP_CPU_MAPPABLE |
                              ARMOS_DRM_CAP_RENDER_3D |
                              ARMOS_DRM_CAP_RESOURCE_TRANSFER;
    armos_drm_command_caps_t request;
    int saved_errno;

    if (!device) {
        errno = EINVAL;
        return -1;
    }
    memset(device, 0, sizeof(*device));
    device->fd = -1;
    device->fd = open(path ? path : "/dev/dri/renderD128", O_RDWR, 0);
    if (device->fd < 0)
        return -1;
    if (armos_virgl_ioctl(device->fd, ARMOS_DRM_IOCTL_GET_INFO,
                          &device->info) < 0)
        goto failed;
    if (device->info.abi_version != ARMOS_DRM_ABI_VERSION ||
        device->info.struct_size < sizeof(device->info) ||
        (device->info.capabilities & required) != required ||
        strncmp((const char *)device->info.command_set,
                "virgl", sizeof("virgl") - 1u) != 0 ||
        device->info.command_caps_size == 0 ||
        device->info.command_caps_size > ARMOS_DRM_MAX_COMMAND_CAPS_SIZE) {
        errno = ENOTSUP;
        goto failed;
    }
    device->command_caps = calloc(1u, device->info.command_caps_size);
    if (!device->command_caps)
        goto failed;
    memset(&request, 0, sizeof(request));
    request.abi_version = ARMOS_DRM_ABI_VERSION;
    request.version = device->info.command_caps_max_version;
    request.size = device->info.command_caps_size;
    request.address =
        (unsigned long long)(uintptr_t)device->command_caps;
    if (armos_virgl_ioctl(device->fd, ARMOS_DRM_IOCTL_GET_COMMAND_CAPS,
                          &request) < 0)
        goto failed;
    device->command_caps_size = request.size;
    device->command_caps_version = request.version;
    return 0;

failed:
    saved_errno = errno;
    armos_virgl_close(device);
    errno = saved_errno;
    return -1;
}

void armos_virgl_close(armos_virgl_device_t *device)
{
    if (!device)
        return;
    free(device->command_caps);
    if (device->fd >= 0)
        close(device->fd);
    memset(device, 0, sizeof(*device));
    device->fd = -1;
}

int armos_virgl_context_create(armos_virgl_device_t *device,
                               uint32_t *context_id)
{
    armos_drm_context_create_t request;

    if (!device || !context_id) {
        errno = EINVAL;
        return -1;
    }
    memset(&request, 0, sizeof(request));
    request.abi_version = ARMOS_DRM_ABI_VERSION;
    if (armos_virgl_ioctl(device->fd, ARMOS_DRM_IOCTL_CONTEXT_CREATE,
                          &request) < 0)
        return -1;
    *context_id = request.context_id;
    return 0;
}

int armos_virgl_context_destroy(armos_virgl_device_t *device,
                                uint32_t context_id)
{
    armos_drm_context_destroy_t request;

    memset(&request, 0, sizeof(request));
    request.context_id = context_id;
    return armos_virgl_ioctl(device ? device->fd : -1,
                             ARMOS_DRM_IOCTL_CONTEXT_DESTROY, &request);
}

int armos_virgl_buffer_create(armos_virgl_device_t *device,
                              armos_virgl_buffer_t *buffer,
                              uint64_t size, uint32_t flags,
                              uint32_t width, uint32_t height,
                              uint32_t stride, uint32_t format)
{
    armos_drm_bo_create_t request;

    if (!device || !buffer) {
        errno = EINVAL;
        return -1;
    }
    memset(buffer, 0, sizeof(*buffer));
    memset(&request, 0, sizeof(request));
    request.abi_version = ARMOS_DRM_ABI_VERSION;
    request.flags = flags;
    request.size = size;
    request.width = width;
    request.height = height;
    request.stride = stride;
    request.format = format;
    if (armos_virgl_ioctl(device->fd, ARMOS_DRM_IOCTL_BO_CREATE,
                          &request) < 0)
        return -1;
    buffer->handle = request.handle;
    buffer->command_handle = request.command_handle;
    buffer->flags = flags;
    buffer->width = width;
    buffer->height = height;
    buffer->stride = stride;
    buffer->format = format;
    buffer->size = size;
    buffer->map_offset = request.map_offset;
    return 0;
}

int armos_virgl_buffer_map(armos_virgl_device_t *device,
                           armos_virgl_buffer_t *buffer)
{
    int protection = 0;

    if (!device || !buffer || buffer->handle == 0 || buffer->mapping) {
        errno = EINVAL;
        return -1;
    }
    if (buffer->flags & ARMOS_DRM_BO_CPU_READ)
        protection |= PROT_READ;
    if (buffer->flags & ARMOS_DRM_BO_CPU_WRITE)
        protection |= PROT_WRITE;
    if (protection == 0) {
        errno = EACCES;
        return -1;
    }
    buffer->mapping = mmap(NULL, (size_t)buffer->size, protection,
                           MAP_SHARED, device->fd,
                           (off_t)buffer->map_offset);
    if (buffer->mapping == MAP_FAILED) {
        buffer->mapping = NULL;
        return -1;
    }
    return 0;
}

int armos_virgl_buffer_unmap(armos_virgl_buffer_t *buffer)
{
    if (!buffer || !buffer->mapping) {
        errno = EINVAL;
        return -1;
    }
    if (munmap(buffer->mapping, (size_t)buffer->size) < 0)
        return -1;
    buffer->mapping = NULL;
    return 0;
}

int armos_virgl_buffer_destroy(armos_virgl_device_t *device,
                               armos_virgl_buffer_t *buffer)
{
    armos_drm_bo_destroy_t request;
    int result;

    if (!device || !buffer || buffer->handle == 0) {
        errno = EINVAL;
        return -1;
    }
    if (buffer->mapping && armos_virgl_buffer_unmap(buffer) < 0)
        return -1;
    memset(&request, 0, sizeof(request));
    request.handle = buffer->handle;
    result = armos_virgl_ioctl(device->fd, ARMOS_DRM_IOCTL_BO_DESTROY,
                               &request);
    if (result == 0)
        memset(buffer, 0, sizeof(*buffer));
    return result;
}

static int armos_virgl_buffer_change(armos_virgl_device_t *device,
                                     uint32_t context_id,
                                     const armos_virgl_buffer_t *buffer,
                                     unsigned long operation)
{
    armos_drm_resource_attachment_t request;

    if (!device || !buffer || context_id == 0 || buffer->handle == 0) {
        errno = EINVAL;
        return -1;
    }
    memset(&request, 0, sizeof(request));
    request.context_id = context_id;
    request.handle = buffer->handle;
    return armos_virgl_ioctl(device->fd, operation, &request);
}

int armos_virgl_buffer_attach(armos_virgl_device_t *device,
                              uint32_t context_id,
                              const armos_virgl_buffer_t *buffer)
{
    return armos_virgl_buffer_change(device, context_id, buffer,
                                     ARMOS_DRM_IOCTL_RESOURCE_ATTACH);
}

int armos_virgl_buffer_detach(armos_virgl_device_t *device,
                              uint32_t context_id,
                              const armos_virgl_buffer_t *buffer)
{
    return armos_virgl_buffer_change(device, context_id, buffer,
                                     ARMOS_DRM_IOCTL_RESOURCE_DETACH);
}

int armos_virgl_buffer_transfer(armos_virgl_device_t *device,
                                uint32_t context_id,
                                const armos_virgl_buffer_t *buffer,
                                uint32_t direction, uint32_t level,
                                uint32_t x, uint32_t y, uint32_t z,
                                uint32_t width, uint32_t height,
                                uint32_t depth, uint64_t offset,
                                uint32_t stride, uint32_t layer_stride)
{
    armos_drm_bo_transfer_t request;

    if (!device || !buffer || buffer->handle == 0 ||
        buffer->command_handle == 0 || context_id == 0) {
        errno = EINVAL;
        return -1;
    }
    memset(&request, 0, sizeof(request));
    request.context_id = context_id;
    request.handle = buffer->handle;
    request.direction = direction;
    request.level = level;
    request.x = x;
    request.y = y;
    request.z = z;
    request.width = width;
    request.height = height;
    request.depth = depth;
    request.offset = offset;
    request.stride = stride;
    request.layer_stride = layer_stride;
    return armos_virgl_ioctl(device->fd, ARMOS_DRM_IOCTL_BO_TRANSFER,
                             &request);
}

int armos_virgl_submit(armos_virgl_device_t *device, uint32_t context_id,
                       const void *commands, uint32_t command_size,
                       uint64_t *fence_id)
{
    armos_drm_submit_t request;

    if (!device || !commands || !fence_id ||
        context_id == 0 || command_size == 0) {
        errno = EINVAL;
        return -1;
    }
    memset(&request, 0, sizeof(request));
    request.context_id = context_id;
    request.command_address =
        (unsigned long long)(uintptr_t)commands;
    request.command_size = command_size;
    if (armos_virgl_ioctl(device->fd, ARMOS_DRM_IOCTL_SUBMIT,
                          &request) < 0)
        return -1;
    *fence_id = request.fence_id;
    return 0;
}

int armos_virgl_fence_wait(armos_virgl_device_t *device, uint64_t fence_id,
                           int64_t timeout_ns)
{
    armos_drm_fence_wait_t request;

    memset(&request, 0, sizeof(request));
    request.fence_id = fence_id;
    request.timeout_ns = timeout_ns;
    return armos_virgl_ioctl(device ? device->fd : -1,
                             ARMOS_DRM_IOCTL_FENCE_WAIT, &request);
}

int armos_virgl_fence_destroy(armos_virgl_device_t *device,
                              uint64_t fence_id)
{
    armos_drm_fence_destroy_t request;

    memset(&request, 0, sizeof(request));
    request.fence_id = fence_id;
    return armos_virgl_ioctl(device ? device->fd : -1,
                             ARMOS_DRM_IOCTL_FENCE_DESTROY, &request);
}
