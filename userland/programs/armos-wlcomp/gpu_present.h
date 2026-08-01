/*
 * ArmOS
 * Copyright (c) 2026 Mohamed Ennassiri
 *
 * Licensed under the Apache License, Version 2.0.
 * See LICENSE for details.
 *
 * File: userland/programs/armos-wlcomp/gpu_present.h
 * Layer: Userland / compositor presentation contract
 *
 * Responsibilities:
 * - Import a rendered GPU output into the common DRM scanout node.
 * - Wait on explicit completion fences before presenting a frame.
 * - Keep scanout ownership independent from the selected GPU provider.
 */

#ifndef ARMOS_WLCOMP_GPU_PRESENT_H
#define ARMOS_WLCOMP_GPU_PRESENT_H

#include <stdbool.h>
#include <stdint.h>

#include "gpu_backend.h"

struct wl_gpu_presenter {
    int card_fd;
    uint32_t handles[WL_GPU_MAX_OUTPUT_BUFFERS];
    uint32_t buffer_count;
    uint32_t width;
    uint32_t height;
    uint32_t stride;
    enum wl_gpu_present_error {
        WL_GPU_PRESENT_ERROR_NONE = 0,
        WL_GPU_PRESENT_ERROR_ARGUMENT,
        WL_GPU_PRESENT_ERROR_FLUSH,
        WL_GPU_PRESENT_ERROR_EXPORT,
        WL_GPU_PRESENT_ERROR_FENCE,
        WL_GPU_PRESENT_ERROR_IMPORT,
        WL_GPU_PRESENT_ERROR_SCANOUT,
    } last_error;
};

bool wl_gpu_presenter_init(struct wl_gpu_presenter *presenter,
                           const char *card_node,
                           uint32_t width, uint32_t height);
void wl_gpu_presenter_destroy(struct wl_gpu_presenter *presenter);
bool wl_gpu_presenter_present(struct wl_gpu_presenter *presenter,
                              struct wl_gpu_backend *backend,
                              const struct wl_gpu_rect *damage);
const char *wl_gpu_presenter_error_string(
    const struct wl_gpu_presenter *presenter);

#endif /* ARMOS_WLCOMP_GPU_PRESENT_H */
