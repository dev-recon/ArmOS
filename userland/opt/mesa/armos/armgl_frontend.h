/*
 * ArmOS
 * Copyright (c) 2026 Mohamed Ennassiri
 *
 * Licensed under the Apache License, Version 2.0.
 * See LICENSE for details.
 *
 * File: userland/opt/mesa/armos/armgl_frontend.h
 * Layer: Third-party port / Mesa Gallium frontend
 *
 * Responsibilities:
 * - Own the architecture-neutral Mesa state-tracker lifecycle on ArmOS.
 * - Provide GLES contexts and off-screen render targets over a Gallium screen.
 * - Keep EGL, Wayland and application policy outside the Gallium frontend.
 *
 * Notes:
 * - This interface contains no VirtIO, VC4, V3D or architecture-specific ABI.
 * - Window surfaces and presentation are added by higher-level EGL adapters.
 */

#ifndef ARMOS_MESA_ARMGL_FRONTEND_H
#define ARMOS_MESA_ARMGL_FRONTEND_H

#include <stdbool.h>
#include <stdint.h>

struct pipe_screen;
struct st_context;

struct armgl_display;
struct armgl_image;
struct armgl_surface;
struct armgl_context;

enum armgl_image_usage {
   ARMGL_IMAGE_USAGE_SAMPLED = 1u << 0,
   ARMGL_IMAGE_USAGE_RENDER_TARGET = 1u << 1,
};

struct armgl_image_config {
   uint32_t width;
   uint32_t height;
   uint32_t stride;
   uint32_t usage;
   bool alpha;
};

struct armgl_rect {
   uint32_t x;
   uint32_t y;
   uint32_t width;
   uint32_t height;
};

struct armgl_surface_config {
   uint32_t width;
   uint32_t height;
   bool alpha;
   bool depth_stencil;
   bool double_buffered;
   /* The resource may leave this ArmGL display through an FD export. */
   bool exportable;
   bool scanout;
};

struct armgl_context_config {
   uint32_t major;
   uint32_t minor;
   bool alpha;
   bool depth_stencil;
   bool double_buffered;
   bool debug;
   bool no_error;
};

struct armgl_display *armgl_display_create(struct pipe_screen *screen);
/* Contexts, images and surfaces must be destroyed before their display. */
void armgl_display_destroy(struct armgl_display *display);

/* The caller retains ownership of buffer_fd. */
struct armgl_image *armgl_image_import_fd(
   struct armgl_display *display,
   const struct armgl_image_config *config,
   int buffer_fd);
struct armgl_image *armgl_image_create(
   struct armgl_display *display,
   const struct armgl_image_config *config);
void armgl_image_destroy(struct armgl_image *image);
bool armgl_image_upload(struct armgl_context *context,
                        struct armgl_image *image,
                        const struct armgl_rect *destination_rect,
                        const void *pixels, uint32_t stride);

struct armgl_surface *armgl_surface_create(
   struct armgl_display *display,
   const struct armgl_surface_config *config);
void armgl_surface_destroy(struct armgl_surface *surface);
bool armgl_surface_resize(struct armgl_surface *surface,
                          uint32_t width, uint32_t height);
/*
 * Replaces the front color attachment and adopts the image dimensions.
 * The surface retains the resource, so the image wrapper may then be freed.
 */
bool armgl_surface_set_color_image(struct armgl_surface *surface,
                                   struct armgl_image *image);
bool armgl_surface_blit_image(struct armgl_context *context,
                              struct armgl_surface *destination,
                              struct armgl_image *source,
                              const struct armgl_rect *source_rect,
                              const struct armgl_rect *destination_rect,
                              bool alpha_blend);
bool armgl_surface_fill_rect(struct armgl_context *context,
                             struct armgl_surface *destination,
                             const struct armgl_rect *destination_rect,
                             uint32_t argb8888);

struct armgl_context *armgl_context_create(
   struct armgl_display *display,
   const struct armgl_context_config *config,
   struct armgl_context *shared);
void armgl_context_destroy(struct armgl_context *context);

bool armgl_make_current(struct armgl_context *context,
                        struct armgl_surface *draw,
                        struct armgl_surface *read);
bool armgl_flush(struct armgl_context *context, bool wait);
bool armgl_flush_fence_fd(struct armgl_context *context, int *fence_fd);
bool armgl_surface_export_color_fd(struct armgl_surface *surface,
                                   struct armgl_context *context,
                                   int *buffer_fd, uint32_t *stride);

struct st_context *armgl_state_tracker_context(
   struct armgl_context *context);

#endif /* ARMOS_MESA_ARMGL_FRONTEND_H */
