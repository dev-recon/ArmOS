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
struct armgl_surface;
struct armgl_context;

struct armgl_surface_config {
   uint32_t width;
   uint32_t height;
   bool alpha;
   bool depth_stencil;
   bool double_buffered;
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
/* Contexts and surfaces must be destroyed before their display. */
void armgl_display_destroy(struct armgl_display *display);

struct armgl_surface *armgl_surface_create(
   struct armgl_display *display,
   const struct armgl_surface_config *config);
void armgl_surface_destroy(struct armgl_surface *surface);
bool armgl_surface_resize(struct armgl_surface *surface,
                          uint32_t width, uint32_t height);

struct armgl_context *armgl_context_create(
   struct armgl_display *display,
   const struct armgl_context_config *config,
   struct armgl_context *shared);
void armgl_context_destroy(struct armgl_context *context);

bool armgl_make_current(struct armgl_context *context,
                        struct armgl_surface *draw,
                        struct armgl_surface *read);
bool armgl_flush(struct armgl_context *context, bool wait);

struct st_context *armgl_state_tracker_context(
   struct armgl_context *context);

#endif /* ARMOS_MESA_ARMGL_FRONTEND_H */
