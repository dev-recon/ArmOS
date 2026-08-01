/*
 * ArmOS
 * Copyright (c) 2026 Mohamed Ennassiri
 *
 * Licensed under the Apache License, Version 2.0.
 * See LICENSE for details.
 *
 * File: userland/programs/armos-wlcomp/gpu_backend.c
 * Layer: Userland / compositor rendering contract
 *
 * Responsibilities:
 * - Dispatch the common compositor contract to an optional GPU provider.
 * - Preserve a clean software-only build when no provider is linked.
 * - Validate provider operations before exposing the backend to the renderer.
 */

#include "gpu_backend_provider.h"

#include <stdlib.h>
#include <string.h>

/* Defined by a provider archive when GPU composition is enabled. */
extern struct wl_gpu_backend *wl_gpu_backend_provider_create(
    const struct wl_gpu_backend_config *config) __attribute__((weak));
extern uint64_t wl_gpu_backend_provider_capabilities(void)
    __attribute__((weak));

static bool
wl_gpu_backend_valid(const struct wl_gpu_backend *backend)
{
    return backend && backend->ops && backend->ops->destroy &&
        backend->ops->begin_frame && backend->ops->end_frame &&
        backend->ops->export_output && backend->ops->flush &&
        backend->ops->create_image && backend->ops->import_image &&
        backend->ops->destroy_image && backend->ops->upload &&
        backend->ops->fill && backend->ops->blit;
}

struct wl_gpu_backend *
wl_gpu_backend_create(const struct wl_gpu_backend_config *config)
{
    struct wl_gpu_backend *backend;

    if (!config || !config->render_node || !config->width ||
        !config->height || !wl_gpu_backend_provider_create)
        return NULL;
    backend = wl_gpu_backend_provider_create(config);
    if (!wl_gpu_backend_valid(backend)) {
        if (backend && backend->ops && backend->ops->destroy)
            backend->ops->destroy(backend);
        return NULL;
    }
    return backend;
}

void
wl_gpu_backend_destroy(struct wl_gpu_backend *backend)
{
    if (wl_gpu_backend_valid(backend))
        backend->ops->destroy(backend);
}

uint64_t
wl_gpu_backend_supported_capabilities(void)
{
    if (!wl_gpu_backend_provider_capabilities)
        return 0u;
    return wl_gpu_backend_provider_capabilities();
}

uint64_t
wl_gpu_backend_capabilities(const struct wl_gpu_backend *backend)
{
    return wl_gpu_backend_valid(backend) ? backend->capabilities : 0u;
}

bool
wl_gpu_backend_begin_frame(struct wl_gpu_backend *backend,
                           struct wl_gpu_frame *frame)
{
    if (frame)
        memset(frame, 0, sizeof(*frame));
    return wl_gpu_backend_valid(backend) && frame &&
        backend->ops->begin_frame(backend, frame);
}

void
wl_gpu_backend_end_frame(struct wl_gpu_backend *backend)
{
    if (wl_gpu_backend_valid(backend))
        backend->ops->end_frame(backend);
}

bool
wl_gpu_backend_export_output(struct wl_gpu_backend *backend,
                             struct wl_gpu_output *output)
{
    return wl_gpu_backend_valid(backend) && output &&
        backend->ops->export_output(backend, output);
}

bool
wl_gpu_backend_flush(struct wl_gpu_backend *backend, int *fence_fd)
{
    return wl_gpu_backend_valid(backend) && fence_fd &&
        backend->ops->flush(backend, fence_fd);
}

struct wl_gpu_image *
wl_gpu_backend_create_image(struct wl_gpu_backend *backend,
                            const struct wl_gpu_image_config *config)
{
    return wl_gpu_backend_valid(backend) && config ?
        backend->ops->create_image(backend, config) : NULL;
}

struct wl_gpu_image *
wl_gpu_backend_import_image(struct wl_gpu_backend *backend,
                            const struct wl_gpu_image_config *config,
                            int buffer_fd)
{
    return wl_gpu_backend_valid(backend) && config && buffer_fd >= 0 ?
        backend->ops->import_image(backend, config, buffer_fd) : NULL;
}

void
wl_gpu_backend_destroy_image(struct wl_gpu_backend *backend,
                             struct wl_gpu_image *image)
{
    if (wl_gpu_backend_valid(backend) && image)
        backend->ops->destroy_image(backend, image);
}

bool
wl_gpu_backend_upload(struct wl_gpu_backend *backend,
                      struct wl_gpu_image *image,
                      const struct wl_gpu_rect *destination,
                      const void *pixels, uint32_t stride)
{
    return wl_gpu_backend_valid(backend) && image && destination && pixels &&
        backend->ops->upload(backend, image, destination, pixels, stride);
}

bool
wl_gpu_backend_fill(struct wl_gpu_backend *backend,
                    const struct wl_gpu_rect *destination,
                    uint32_t argb8888)
{
    return wl_gpu_backend_valid(backend) && destination &&
        backend->ops->fill(backend, destination, argb8888);
}

bool
wl_gpu_backend_blit(struct wl_gpu_backend *backend,
                    struct wl_gpu_image *source,
                    const struct wl_gpu_rect *source_rect,
                    const struct wl_gpu_rect *destination_rect,
                    bool alpha_blend)
{
    return wl_gpu_backend_valid(backend) && source && source_rect &&
        destination_rect && backend->ops->blit(
            backend, source, source_rect, destination_rect, alpha_blend);
}
