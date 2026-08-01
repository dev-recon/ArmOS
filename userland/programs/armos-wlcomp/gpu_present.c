/*
 * ArmOS
 * Copyright (c) 2026 Mohamed Ennassiri
 *
 * Licensed under the Apache License, Version 2.0.
 * See LICENSE for details.
 *
 * File: userland/programs/armos-wlcomp/gpu_present.c
 * Layer: Userland / compositor presentation contract
 *
 * Responsibilities:
 * - Bridge provider-owned output images to the common DRM scanout ABI.
 * - Enforce explicit render-complete ordering with pollable fence handles.
 * - Release imported scanout ownership deterministically.
 *
 * Notes:
 * - This file contains no VirtIO, VirGL, VC4, V3D or architecture details.
 * - Provider buffer and fence descriptors are consumed by this layer.
 */

#include "gpu_present.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <uapi/armos/drm.h>

static bool
wl_gpu_wait_fence(int fence_fd)
{
    struct pollfd descriptor;
    armos_drm_fence_result_t result;
    ssize_t count;
    int ready;

    if (fence_fd < 0)
        return false;
    memset(&descriptor, 0, sizeof(descriptor));
    descriptor.fd = fence_fd;
    descriptor.events = POLLIN;
    do {
        ready = poll(&descriptor, 1u, -1);
    } while (ready < 0 && errno == EINTR);
    if (ready != 1 || (descriptor.revents & POLLIN) == 0 ||
        (descriptor.revents & (POLLERR | POLLNVAL)) != 0) {
        if (ready >= 0)
            errno = EIO;
        close(fence_fd);
        return false;
    }
    do {
        count = read(fence_fd, &result, sizeof(result));
    } while (count < 0 && errno == EINTR);
    close(fence_fd);
    if (count != (ssize_t)sizeof(result)) {
        if (count >= 0)
            errno = EIO;
        return false;
    }
    if (result.status != 0) {
        errno = result.status < 0 ? -result.status : EIO;
        return false;
    }
    return true;
}

bool
wl_gpu_presenter_init_fd(struct wl_gpu_presenter *presenter,
                         int card_fd, bool take_ownership,
                         uint32_t width, uint32_t height)
{
    if (!presenter || card_fd < 0 || !width || !height)
        return false;
    memset(presenter, 0, sizeof(*presenter));
    presenter->card_fd = card_fd;
    for (uint32_t index = 0u;
         index < WL_GPU_MAX_OUTPUT_BUFFERS; index++) {
        presenter->pending[index].fence_fd = -1;
        presenter->pending[index].output.buffer_fd = -1;
    }
    presenter->owns_card_fd = take_ownership;
    presenter->width = width;
    presenter->height = height;
    return true;
}

bool
wl_gpu_presenter_init(struct wl_gpu_presenter *presenter,
                      const char *card_node,
                      uint32_t width, uint32_t height)
{
    int card_fd;

    if (!presenter || !card_node || !width || !height)
        return false;
    card_fd = open(card_node, O_RDWR, 0);
    if (card_fd < 0)
        return false;
    if (!wl_gpu_presenter_init_fd(
            presenter, card_fd, true, width, height)) {
        close(card_fd);
        return false;
    }
    return true;
}

void
wl_gpu_presenter_destroy(struct wl_gpu_presenter *presenter)
{
    if (!presenter)
        return;
    wl_gpu_presenter_cancel(presenter);
    if (presenter->card_fd >= 0) {
        for (uint32_t index = 0u;
             index < WL_GPU_MAX_OUTPUT_BUFFERS; index++) {
            armos_drm_bo_destroy_t destroy;

            if (presenter->handles[index] == 0u)
                continue;
            memset(&destroy, 0, sizeof(destroy));
            destroy.handle = presenter->handles[index];
            (void)ioctl(presenter->card_fd,
                        ARMOS_DRM_IOCTL_BO_DESTROY, &destroy);
        }
    }
    if (presenter->card_fd >= 0 && presenter->owns_card_fd)
        close(presenter->card_fd);
    memset(presenter, 0, sizeof(*presenter));
    presenter->card_fd = -1;
    for (uint32_t index = 0u;
         index < WL_GPU_MAX_OUTPUT_BUFFERS; index++) {
        presenter->pending[index].fence_fd = -1;
        presenter->pending[index].output.buffer_fd = -1;
    }
}

static bool
wl_gpu_presenter_import_output(struct wl_gpu_presenter *presenter,
                               const struct wl_gpu_output *output)
{
    armos_drm_bo_import_t import;

    if (!presenter || !output || output->buffer_fd < 0 ||
        output->count == 0u ||
        output->count > WL_GPU_MAX_OUTPUT_BUFFERS ||
        output->index >= output->count ||
        output->stride < presenter->width * sizeof(uint32_t) ||
        (presenter->buffer_count != 0u &&
         presenter->buffer_count != output->count) ||
        (presenter->stride != 0u &&
         presenter->stride != output->stride)) {
        errno = EINVAL;
        return false;
    }
    if (presenter->handles[output->index] != 0u)
        return true;
    memset(&import, 0, sizeof(import));
    /*
     * BO_IMPORT creates a handle in this DRM file, not a descriptor.
     * Descriptor flags such as CLOEXEC belong exclusively to BO_EXPORT;
     * the common import ABI deliberately requires flags to remain zero.
     */
    import.abi_version = ARMOS_DRM_ABI_VERSION;
    import.fd = output->buffer_fd;
    if (ioctl(presenter->card_fd, ARMOS_DRM_IOCTL_BO_IMPORT,
              &import) < 0)
        return false;
    if (import.width != presenter->width ||
        import.height != presenter->height ||
        import.stride != output->stride ||
        import.format != ARMOS_DRM_FORMAT_BGRA8888 ||
        (import.bo_flags & ARMOS_DRM_BO_SCANOUT) == 0u) {
        armos_drm_bo_destroy_t destroy = {.handle = import.handle};

        (void)ioctl(presenter->card_fd,
                    ARMOS_DRM_IOCTL_BO_DESTROY, &destroy);
        return false;
    }
    presenter->handles[output->index] = import.handle;
    presenter->buffer_count = output->count;
    presenter->stride = output->stride;
    return true;
}

bool
wl_gpu_presenter_submit(struct wl_gpu_presenter *presenter,
                        struct wl_gpu_backend *backend,
                        const struct wl_gpu_rect *damage,
                        int *submitted_fence_fd,
                        uint32_t *submitted_output_index)
{
    struct wl_gpu_output output;
    struct wl_gpu_rect full;
    uint64_t x1;
    uint64_t y1;
    int fence_fd = -1;

    memset(&output, 0, sizeof(output));
    output.buffer_fd = -1;
    if (presenter)
        presenter->last_error = WL_GPU_PRESENT_ERROR_NONE;
    if (!backend || !submitted_fence_fd || !submitted_output_index) {
        if (presenter)
            presenter->last_error = WL_GPU_PRESENT_ERROR_ARGUMENT;
        errno = EINVAL;
        return false;
    }
    if (!presenter || presenter->card_fd < 0 ||
        !wl_gpu_presenter_can_submit(presenter)) {
        if (presenter)
            presenter->last_error = WL_GPU_PRESENT_ERROR_ARGUMENT;
        errno = presenter && presenter->card_fd >= 0 ? EBUSY : EINVAL;
        wl_gpu_backend_end_frame(backend);
        return false;
    }
    if (!damage) {
        memset(&full, 0, sizeof(full));
        full.width = presenter->width;
        full.height = presenter->height;
        damage = &full;
    }
    x1 = (uint64_t)damage->x + damage->width;
    y1 = (uint64_t)damage->y + damage->height;
    if (!damage->width || !damage->height ||
        x1 > presenter->width || y1 > presenter->height) {
        presenter->last_error = WL_GPU_PRESENT_ERROR_ARGUMENT;
        errno = EINVAL;
        goto fail;
    }
    errno = 0;
    if (!wl_gpu_backend_flush(backend, &fence_fd)) {
        presenter->last_error = WL_GPU_PRESENT_ERROR_FLUSH;
        if (errno == 0)
            errno = EIO;
        goto fail;
    }
    if (!wl_gpu_backend_export_output(backend, &output) ||
        output.buffer_fd < 0) {
        presenter->last_error = WL_GPU_PRESENT_ERROR_EXPORT;
        if (errno == 0)
            errno = EIO;
        goto fail;
    }
    if (output.count == 0u ||
        output.count > WL_GPU_MAX_OUTPUT_BUFFERS ||
        output.index >= output.count ||
        (presenter->buffer_count != 0u &&
         presenter->buffer_count != output.count) ||
        presenter->pending[output.index].active) {
        presenter->last_error = WL_GPU_PRESENT_ERROR_ARGUMENT;
        errno = EBUSY;
        goto fail;
    }
    presenter->buffer_count = output.count;
    presenter->pending[output.index].output = output;
    presenter->pending[output.index].damage = *damage;
    presenter->pending[output.index].fence_fd = fence_fd;
    presenter->pending[output.index].sequence =
        presenter->next_sequence++;
    presenter->pending[output.index].active = true;
    presenter->pending_count++;
    *submitted_fence_fd = fence_fd;
    *submitted_output_index = output.index;
    fence_fd = -1;
    output.buffer_fd = -1;
    wl_gpu_backend_end_frame(backend);
    return true;

fail:
    if (fence_fd >= 0)
        close(fence_fd);
    if (output.buffer_fd >= 0)
        close(output.buffer_fd);
    wl_gpu_backend_end_frame(backend);
    return false;
}

bool
wl_gpu_presenter_next_fence(const struct wl_gpu_presenter *presenter,
                            int *fence_fd, uint32_t *output_index)
{
    const struct wl_gpu_present_pending *oldest = NULL;
    uint32_t oldest_index = 0u;

    if (!presenter || !fence_fd || !output_index) {
        errno = EINVAL;
        return false;
    }
    for (uint32_t index = 0u;
         index < WL_GPU_MAX_OUTPUT_BUFFERS; index++) {
        const struct wl_gpu_present_pending *candidate =
            &presenter->pending[index];

        if (!candidate->active || candidate->fence_fd < 0)
            continue;
        if (!oldest || candidate->sequence < oldest->sequence) {
            oldest = candidate;
            oldest_index = index;
        }
    }
    if (!oldest) {
        errno = ENOENT;
        return false;
    }
    *fence_fd = oldest->fence_fd;
    *output_index = oldest_index;
    return true;
}

bool
wl_gpu_presenter_complete(struct wl_gpu_presenter *presenter,
                          int fence_fd, uint32_t *output_index)
{
    armos_drm_bo_present_t request;
    struct wl_gpu_present_pending *pending = NULL;
    int oldest_fence_fd;
    uint32_t oldest_output_index;
    uint32_t index;

    if (!presenter || fence_fd < 0) {
        errno = EINVAL;
        return false;
    }
    if (!wl_gpu_presenter_next_fence(
            presenter, &oldest_fence_fd, &oldest_output_index) ||
        oldest_fence_fd != fence_fd) {
        errno = EBUSY;
        return false;
    }
    for (index = 0u; index < WL_GPU_MAX_OUTPUT_BUFFERS; index++) {
        if (presenter->pending[index].active &&
            presenter->pending[index].fence_fd == fence_fd) {
            pending = &presenter->pending[index];
            break;
        }
    }
    if (!pending) {
        errno = EINVAL;
        return false;
    }
    pending->fence_fd = -1;
    if (!wl_gpu_wait_fence(fence_fd)) {
        presenter->last_error = WL_GPU_PRESENT_ERROR_FENCE;
        goto fail;
    }
    if (!wl_gpu_presenter_import_output(
            presenter, &pending->output)) {
        presenter->last_error = WL_GPU_PRESENT_ERROR_IMPORT;
        goto fail;
    }
    memset(&request, 0, sizeof(request));
    request.handle = presenter->handles[pending->output.index];
    request.x = pending->damage.x;
    request.y = pending->damage.y;
    request.width = pending->damage.width;
    request.height = pending->damage.height;
    if (ioctl(presenter->card_fd, ARMOS_DRM_IOCTL_BO_PRESENT,
              &request) < 0) {
        presenter->last_error = WL_GPU_PRESENT_ERROR_SCANOUT;
        goto fail;
    }
    if (output_index)
        *output_index = oldest_output_index;
    close(pending->output.buffer_fd);
    memset(pending, 0, sizeof(*pending));
    pending->fence_fd = -1;
    pending->output.buffer_fd = -1;
    presenter->pending_count--;
    return true;

fail:
    if (pending->output.buffer_fd >= 0)
        close(pending->output.buffer_fd);
    memset(pending, 0, sizeof(*pending));
    pending->fence_fd = -1;
    pending->output.buffer_fd = -1;
    presenter->pending_count--;
    return false;
}

void
wl_gpu_presenter_cancel(struct wl_gpu_presenter *presenter)
{
    if (!presenter)
        return;
    for (uint32_t index = 0u;
         index < WL_GPU_MAX_OUTPUT_BUFFERS; index++) {
        struct wl_gpu_present_pending *pending =
            &presenter->pending[index];

        if (pending->fence_fd >= 0)
            close(pending->fence_fd);
        if (pending->output.buffer_fd >= 0)
            close(pending->output.buffer_fd);
        memset(pending, 0, sizeof(*pending));
        pending->fence_fd = -1;
        pending->output.buffer_fd = -1;
    }
    presenter->pending_count = 0u;
}

bool
wl_gpu_presenter_pending(const struct wl_gpu_presenter *presenter)
{
    return presenter && presenter->pending_count != 0u;
}

bool
wl_gpu_presenter_can_submit(const struct wl_gpu_presenter *presenter)
{
    uint32_t count;

    if (!presenter)
        return false;
    count = presenter->buffer_count != 0u ?
        presenter->buffer_count : WL_GPU_MAX_OUTPUT_BUFFERS;
    return presenter->pending_count < (count > 1u ? count - 1u : 1u);
}

bool
wl_gpu_presenter_present(struct wl_gpu_presenter *presenter,
                         struct wl_gpu_backend *backend,
                         const struct wl_gpu_rect *damage)
{
    uint32_t output_index;
    int fence_fd;

    if (!wl_gpu_presenter_submit(presenter, backend, damage,
                                 &fence_fd, &output_index))
        return false;
    return wl_gpu_presenter_complete(presenter, fence_fd, &output_index);
}

const char *
wl_gpu_presenter_error_string(const struct wl_gpu_presenter *presenter)
{
    if (!presenter)
        return "argument";
    switch (presenter->last_error) {
    case WL_GPU_PRESENT_ERROR_NONE:
        return "none";
    case WL_GPU_PRESENT_ERROR_ARGUMENT:
        return "argument";
    case WL_GPU_PRESENT_ERROR_FLUSH:
        return "flush";
    case WL_GPU_PRESENT_ERROR_EXPORT:
        return "buffer export";
    case WL_GPU_PRESENT_ERROR_FENCE:
        return "fence wait";
    case WL_GPU_PRESENT_ERROR_IMPORT:
        return "buffer import";
    case WL_GPU_PRESENT_ERROR_SCANOUT:
        return "scanout";
    }
    return "unknown";
}
