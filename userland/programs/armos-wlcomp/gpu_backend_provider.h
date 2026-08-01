/*
 * ArmOS
 * Copyright (c) 2026 Mohamed Ennassiri
 *
 * Licensed under the Apache License, Version 2.0.
 * See LICENSE for details.
 *
 * File: userland/programs/armos-wlcomp/gpu_backend_provider.h
 * Layer: Userland / compositor GPU provider ABI
 *
 * Responsibilities:
 * - Define the private provider vtable behind the common GPU contract.
 * - Allow Mesa or future native providers to be built outside the compositor.
 */

#ifndef ARMOS_WLCOMP_GPU_BACKEND_PROVIDER_H
#define ARMOS_WLCOMP_GPU_BACKEND_PROVIDER_H

#include "gpu_backend.h"

struct wl_gpu_backend_ops {
    void (*destroy)(struct wl_gpu_backend *backend);
    bool (*begin_frame)(struct wl_gpu_backend *backend,
                        struct wl_gpu_frame *frame);
    void (*end_frame)(struct wl_gpu_backend *backend);
    bool (*export_output)(struct wl_gpu_backend *backend,
                          struct wl_gpu_output *output);
    bool (*flush)(struct wl_gpu_backend *backend, int *fence_fd);
    struct wl_gpu_image *(*create_image)(
        struct wl_gpu_backend *backend,
        const struct wl_gpu_image_config *config);
    struct wl_gpu_image *(*import_image)(
        struct wl_gpu_backend *backend,
        const struct wl_gpu_image_config *config, int buffer_fd);
    void (*destroy_image)(struct wl_gpu_backend *backend,
                          struct wl_gpu_image *image);
    bool (*upload)(struct wl_gpu_backend *backend,
                   struct wl_gpu_image *image,
                   const struct wl_gpu_rect *destination,
                   const void *pixels, uint32_t stride);
    bool (*fill)(struct wl_gpu_backend *backend,
                 const struct wl_gpu_rect *destination,
                 uint32_t argb8888);
    bool (*blit)(struct wl_gpu_backend *backend,
                 struct wl_gpu_image *source,
                 const struct wl_gpu_rect *source_rect,
                 const struct wl_gpu_rect *destination_rect,
                 bool alpha_blend);
};

struct wl_gpu_backend {
    const struct wl_gpu_backend_ops *ops;
};

struct wl_gpu_backend *wl_gpu_backend_provider_create(
    const struct wl_gpu_backend_config *config);

#endif /* ARMOS_WLCOMP_GPU_BACKEND_PROVIDER_H */
