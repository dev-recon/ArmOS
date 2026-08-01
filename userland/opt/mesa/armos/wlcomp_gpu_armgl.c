/*
 * ArmOS
 * Copyright (c) 2026 Mohamed Ennassiri
 *
 * Licensed under the Apache License, Version 2.0.
 * See LICENSE for details.
 *
 * File: userland/opt/mesa/armos/wlcomp_gpu_armgl.c
 * Layer: Third-party port / Mesa compositor provider
 *
 * Responsibilities:
 * - Implement the hardware-neutral compositor GPU contract over ArmGL.
 * - Own one scanout-capable composition target and its Gallium context.
 * - Import client images, upload SHM damage and expose explicit fences.
 *
 * Notes:
 * - No VirtIO, VirGL, VC4, V3D or architecture-specific API appears here.
 * - Device selection remains in the ArmOS Gallium pipe loader.
 */

#include "gpu_backend_provider.h"

#include <stdlib.h>
#include <string.h>

#include "armgl_frontend.h"
#include "pipe/p_screen.h"
#include "pipe_loader_armos.h"

struct wl_gpu_armgl_backend {
    struct wl_gpu_backend base;
    struct armgl_display *display;
    struct armgl_context *context;
    struct armgl_surface *outputs[WL_GPU_MAX_OUTPUT_BUFFERS];
    uint32_t output_count;
    uint32_t draw_index;
    bool frame_active;
};

struct wl_gpu_image {
    struct armgl_image *image;
};

static struct wl_gpu_armgl_backend *
wl_gpu_armgl_backend(struct wl_gpu_backend *backend)
{
    return (struct wl_gpu_armgl_backend *)(void *)backend;
}

static struct armgl_rect
wl_gpu_armgl_rect(const struct wl_gpu_rect *rect)
{
    struct armgl_rect result;

    memset(&result, 0, sizeof(result));
    if (rect) {
        result.x = rect->x;
        result.y = rect->y;
        result.width = rect->width;
        result.height = rect->height;
    }
    return result;
}

static void
wl_gpu_armgl_destroy(struct wl_gpu_backend *base)
{
    struct wl_gpu_armgl_backend *backend = wl_gpu_armgl_backend(base);

    if (!backend)
        return;
    armgl_context_destroy(backend->context);
    for (uint32_t index = 0u; index < backend->output_count; index++)
        armgl_surface_destroy(backend->outputs[index]);
    armgl_display_destroy(backend->display);
    free(backend);
}

static bool
wl_gpu_armgl_begin_frame(struct wl_gpu_backend *base)
{
    struct wl_gpu_armgl_backend *backend = wl_gpu_armgl_backend(base);

    if (!backend || backend->frame_active || backend->output_count == 0u)
        return false;
    backend->draw_index =
        (backend->draw_index + 1u) % backend->output_count;
    if (!armgl_make_current(backend->context,
                            backend->outputs[backend->draw_index],
                            backend->outputs[backend->draw_index]))
        return false;
    backend->frame_active = true;
    return true;
}

static void
wl_gpu_armgl_end_frame(struct wl_gpu_backend *base)
{
    struct wl_gpu_armgl_backend *backend = wl_gpu_armgl_backend(base);

    if (backend)
        backend->frame_active = false;
}

static bool
wl_gpu_armgl_export_output(struct wl_gpu_backend *base,
                           struct wl_gpu_output *output)
{
    struct wl_gpu_armgl_backend *backend = wl_gpu_armgl_backend(base);

    if (!backend || !backend->frame_active || !output)
        return false;
    memset(output, 0, sizeof(*output));
    output->buffer_fd = -1;
    output->index = backend->draw_index;
    output->count = backend->output_count;
    return armgl_surface_export_color_fd(
        backend->outputs[backend->draw_index], backend->context,
        &output->buffer_fd, &output->stride);
}

static bool
wl_gpu_armgl_flush(struct wl_gpu_backend *base, int *fence_fd)
{
    struct wl_gpu_armgl_backend *backend = wl_gpu_armgl_backend(base);

    return backend && backend->frame_active &&
        armgl_flush_fence_fd(backend->context, fence_fd);
}

static struct wl_gpu_image *
wl_gpu_armgl_create_image(struct wl_gpu_backend *base,
                          const struct wl_gpu_image_config *config)
{
    struct wl_gpu_armgl_backend *backend = wl_gpu_armgl_backend(base);
    struct armgl_image_config armgl_config;
    struct wl_gpu_image *image;

    if (!backend || !config)
        return NULL;
    image = calloc(1, sizeof(*image));
    if (!image)
        return NULL;
    memset(&armgl_config, 0, sizeof(armgl_config));
    armgl_config.width = config->width;
    armgl_config.height = config->height;
    armgl_config.stride = config->stride;
    armgl_config.usage = ARMGL_IMAGE_USAGE_SAMPLED;
    armgl_config.alpha = config->alpha;
    image->image = armgl_image_create(backend->display, &armgl_config);
    if (!image->image) {
        free(image);
        return NULL;
    }
    return image;
}

static struct wl_gpu_image *
wl_gpu_armgl_import_image(struct wl_gpu_backend *base,
                          const struct wl_gpu_image_config *config,
                          int buffer_fd)
{
    struct wl_gpu_armgl_backend *backend = wl_gpu_armgl_backend(base);
    struct armgl_image_config armgl_config;
    struct wl_gpu_image *image;

    if (!backend || !config || buffer_fd < 0)
        return NULL;
    image = calloc(1, sizeof(*image));
    if (!image)
        return NULL;
    memset(&armgl_config, 0, sizeof(armgl_config));
    armgl_config.width = config->width;
    armgl_config.height = config->height;
    armgl_config.stride = config->stride;
    armgl_config.usage = ARMGL_IMAGE_USAGE_SAMPLED;
    armgl_config.alpha = config->alpha;
    image->image = armgl_image_import_fd(
        backend->display, &armgl_config, buffer_fd);
    if (!image->image) {
        free(image);
        return NULL;
    }
    return image;
}

static void
wl_gpu_armgl_destroy_image(struct wl_gpu_backend *base,
                           struct wl_gpu_image *image)
{
    (void)base;
    if (!image)
        return;
    armgl_image_destroy(image->image);
    free(image);
}

static bool
wl_gpu_armgl_upload(struct wl_gpu_backend *base,
                    struct wl_gpu_image *image,
                    const struct wl_gpu_rect *destination,
                    const void *pixels, uint32_t stride)
{
    struct wl_gpu_armgl_backend *backend = wl_gpu_armgl_backend(base);
    struct armgl_rect rect = wl_gpu_armgl_rect(destination);

    return backend && backend->frame_active && image && armgl_image_upload(
        backend->context, image->image, &rect, pixels, stride);
}

static bool
wl_gpu_armgl_fill(struct wl_gpu_backend *base,
                  const struct wl_gpu_rect *destination,
                  uint32_t argb8888)
{
    struct wl_gpu_armgl_backend *backend = wl_gpu_armgl_backend(base);
    struct armgl_rect rect = wl_gpu_armgl_rect(destination);

    return backend && backend->frame_active && armgl_surface_fill_rect(
        backend->context, backend->outputs[backend->draw_index],
        &rect, argb8888);
}

static bool
wl_gpu_armgl_blit(struct wl_gpu_backend *base,
                  struct wl_gpu_image *source,
                  const struct wl_gpu_rect *source_rect,
                  const struct wl_gpu_rect *destination_rect,
                  bool alpha_blend)
{
    struct wl_gpu_armgl_backend *backend = wl_gpu_armgl_backend(base);
    struct armgl_rect source_box = wl_gpu_armgl_rect(source_rect);
    struct armgl_rect destination_box = wl_gpu_armgl_rect(destination_rect);

    return backend && backend->frame_active && source &&
        armgl_surface_blit_image(
        backend->context, backend->outputs[backend->draw_index], source->image,
        &source_box, &destination_box, alpha_blend);
}

static const struct wl_gpu_backend_ops wl_gpu_armgl_ops = {
    .destroy = wl_gpu_armgl_destroy,
    .begin_frame = wl_gpu_armgl_begin_frame,
    .end_frame = wl_gpu_armgl_end_frame,
    .export_output = wl_gpu_armgl_export_output,
    .flush = wl_gpu_armgl_flush,
    .create_image = wl_gpu_armgl_create_image,
    .import_image = wl_gpu_armgl_import_image,
    .destroy_image = wl_gpu_armgl_destroy_image,
    .upload = wl_gpu_armgl_upload,
    .fill = wl_gpu_armgl_fill,
    .blit = wl_gpu_armgl_blit,
};

struct wl_gpu_backend *
wl_gpu_backend_provider_create(const struct wl_gpu_backend_config *config)
{
    struct armgl_context_config context_config;
    struct armgl_surface_config surface_config;
    struct wl_gpu_armgl_backend *backend;
    struct pipe_screen *screen;

    if (!config || !config->render_node || !config->width || !config->height)
        return NULL;
    backend = calloc(1, sizeof(*backend));
    if (!backend)
        return NULL;
    backend->base.ops = &wl_gpu_armgl_ops;
    backend->output_count = WL_GPU_MAX_OUTPUT_BUFFERS;
    backend->draw_index = backend->output_count - 1u;

    screen = armos_pipe_screen_create(config->render_node);
    if (!screen)
        goto fail;
    backend->display = armgl_display_create(screen);
    if (!backend->display) {
        screen->destroy(screen);
        goto fail;
    }

    memset(&surface_config, 0, sizeof(surface_config));
    surface_config.width = config->width;
    surface_config.height = config->height;
    surface_config.alpha = true;
    surface_config.exportable = true;
    surface_config.scanout = true;
    for (uint32_t index = 0u; index < backend->output_count; index++) {
        backend->outputs[index] = armgl_surface_create(
            backend->display, &surface_config);
        if (!backend->outputs[index])
            goto fail;
    }

    memset(&context_config, 0, sizeof(context_config));
    context_config.major = 2u;
    context_config.alpha = true;
    backend->context = armgl_context_create(
        backend->display, &context_config, NULL);
    if (!backend->context)
        goto fail;
    return &backend->base;

fail:
    wl_gpu_armgl_destroy(&backend->base);
    return NULL;
}
