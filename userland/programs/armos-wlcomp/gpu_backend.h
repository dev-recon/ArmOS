/*
 * ArmOS
 * Copyright (c) 2026 Mohamed Ennassiri
 *
 * Licensed under the Apache License, Version 2.0.
 * See LICENSE for details.
 *
 * File: userland/programs/armos-wlcomp/gpu_backend.h
 * Layer: Userland / compositor rendering contract
 *
 * Responsibilities:
 * - Define the hardware-neutral GPU composition contract.
 * - Keep Gallium, VirGL, VC4 and platform APIs out of the compositor core.
 * - Model output, image, damage and fence ownership explicitly.
 *
 * Notes:
 * - Providers are optional; the software renderer remains the fallback.
 * - Coordinates use the compositor's top-left origin.
 * - Output indices rotate without reusing a resource still in flight.
 */

#ifndef ARMOS_WLCOMP_GPU_BACKEND_H
#define ARMOS_WLCOMP_GPU_BACKEND_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct wl_gpu_backend;
struct wl_gpu_image;

struct wl_gpu_backend_config {
    const char *render_node;
    uint32_t width;
    uint32_t height;
};

struct wl_gpu_image_config {
    uint32_t width;
    uint32_t height;
    uint32_t stride;
    bool alpha;
};

struct wl_gpu_rect {
    uint32_t x;
    uint32_t y;
    uint32_t width;
    uint32_t height;
};

#define WL_GPU_MAX_OUTPUT_BUFFERS 3u

/*
 * A provider may accelerate the compositor only when it guarantees the same
 * observable result as the reference software renderer.  These are semantic
 * guarantees, not merely indications that an operation exists.
 */
#define WL_GPU_CAP_ORDERED_COMPOSITION UINT64_C(1)
#define WL_GPU_CAP_SOURCE_OVER         UINT64_C(2)
#define WL_GPU_CAP_DAMAGE_PRESERVATION UINT64_C(4)
#define WL_GPU_COMPOSITOR_REQUIRED_CAPS \
    (WL_GPU_CAP_ORDERED_COMPOSITION | WL_GPU_CAP_SOURCE_OVER | \
     WL_GPU_CAP_DAMAGE_PRESERVATION)

struct wl_gpu_output {
    int buffer_fd;
    uint32_t stride;
    uint32_t index;
    uint32_t count;
};

struct wl_gpu_frame {
    uint32_t output_index;
    uint32_t output_count;
};

/* Returns NULL when no provider is linked or the device is unsupported. */
struct wl_gpu_backend *wl_gpu_backend_create(
    const struct wl_gpu_backend_config *config);
void wl_gpu_backend_destroy(struct wl_gpu_backend *backend);
uint64_t wl_gpu_backend_supported_capabilities(void);
uint64_t wl_gpu_backend_capabilities(const struct wl_gpu_backend *backend);

/*
 * A frame is an explicit command-generation transaction. Rendering operations
 * are valid only between begin_frame() and end_frame(). After flush/export,
 * the returned descriptors retain the output resource and completion fence
 * independently, so command generation may end before asynchronous scanout.
 */
bool wl_gpu_backend_begin_frame(struct wl_gpu_backend *backend,
                                struct wl_gpu_frame *frame);
void wl_gpu_backend_end_frame(struct wl_gpu_backend *backend);

/* The caller owns the returned output descriptor and fence descriptor. */
bool wl_gpu_backend_export_output(struct wl_gpu_backend *backend,
                                  struct wl_gpu_output *output);
bool wl_gpu_backend_flush(struct wl_gpu_backend *backend, int *fence_fd);

struct wl_gpu_image *wl_gpu_backend_create_image(
    struct wl_gpu_backend *backend,
    const struct wl_gpu_image_config *config);
struct wl_gpu_image *wl_gpu_backend_import_image(
    struct wl_gpu_backend *backend,
    const struct wl_gpu_image_config *config, int buffer_fd);
void wl_gpu_backend_destroy_image(struct wl_gpu_backend *backend,
                                  struct wl_gpu_image *image);
bool wl_gpu_backend_upload(struct wl_gpu_backend *backend,
                           struct wl_gpu_image *image,
                           const struct wl_gpu_rect *destination,
                           const void *pixels, uint32_t stride);
bool wl_gpu_backend_fill(struct wl_gpu_backend *backend,
                         const struct wl_gpu_rect *destination,
                         uint32_t argb8888);
bool wl_gpu_backend_blit(struct wl_gpu_backend *backend,
                         struct wl_gpu_image *source,
                         const struct wl_gpu_rect *source_rect,
                         const struct wl_gpu_rect *destination_rect,
                         bool alpha_blend);

#endif /* ARMOS_WLCOMP_GPU_BACKEND_H */
