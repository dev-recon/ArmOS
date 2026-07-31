/*
 * ArmOS
 * Copyright (c) 2026 Mohamed Ennassiri
 *
 * Licensed under the Apache License, Version 2.0.
 * See LICENSE for details.
 *
 * File: userland/opt/mesa/armos/armgl_frontend.c
 * Layer: Third-party port / Mesa Gallium frontend
 *
 * Responsibilities:
 * - Bridge Gallium pipe screens to Mesa's state tracker.
 * - Allocate and validate color/depth attachments for off-screen rendering.
 * - Manage GLES context binding and explicit fence-backed flushes.
 *
 * Notes:
 * - The implementation is original ArmOS integration code, not a DRI shim.
 * - Presentation and native-window ownership deliberately do not live here.
 */

#include "armgl_frontend.h"

#include <stdlib.h>
#include <string.h>

#include "frontend/api.h"
#include "pipe/p_context.h"
#include "pipe/p_defines.h"
#include "pipe/p_screen.h"
#include "state_tracker/st_context.h"
#include "util/u_atomic.h"
#include "util/u_inlines.h"

struct armgl_display {
   struct pipe_frontend_screen frontend;
};

struct armgl_surface {
   struct pipe_frontend_drawable drawable;
   struct st_visual visual;
   struct pipe_resource *attachments[ST_ATTACHMENT_COUNT];
   uint32_t width;
   uint32_t height;
   uint32_t allocated_width;
   uint32_t allocated_height;
   uint32_t allocated_mask;
};

struct armgl_context {
   struct armgl_display *display;
   struct st_context *state_tracker;
};

static uint32_t armgl_next_drawable_id;

static struct armgl_surface *
armgl_surface_from_drawable(struct pipe_frontend_drawable *drawable)
{
   return (struct armgl_surface *)drawable;
}

static int
armgl_get_manager_param(struct pipe_frontend_screen *frontend,
                        enum st_manager_param param)
{
   (void)frontend;
   (void)param;
   return 0;
}

static bool
armgl_flush_front(struct st_context *state_tracker,
                  struct pipe_frontend_drawable *drawable,
                  enum st_attachment_type attachment)
{
   (void)state_tracker;
   (void)drawable;
   return attachment == ST_ATTACHMENT_FRONT_LEFT;
}

static void
armgl_release_attachments(struct armgl_surface *surface)
{
   unsigned index;

   for (index = 0; index < ST_ATTACHMENT_COUNT; ++index)
      pipe_resource_reference(&surface->attachments[index], NULL);

   surface->allocated_width = 0;
   surface->allocated_height = 0;
   surface->allocated_mask = 0;
}

static bool
armgl_create_attachment(struct armgl_surface *surface,
                        enum st_attachment_type attachment)
{
   struct pipe_resource resource;

   memset(&resource, 0, sizeof(resource));
   resource.target = PIPE_TEXTURE_2D;
   resource.width0 = surface->width;
   resource.height0 = surface->height;
   resource.depth0 = 1;
   resource.array_size = 1;

   if (attachment == ST_ATTACHMENT_DEPTH_STENCIL) {
      resource.format = surface->visual.depth_stencil_format;
      resource.bind = PIPE_BIND_DEPTH_STENCIL;
   } else {
      resource.format = surface->visual.color_format;
      resource.bind = PIPE_BIND_RENDER_TARGET | PIPE_BIND_SAMPLER_VIEW;
   }

   if (resource.format == PIPE_FORMAT_NONE)
      return false;

   surface->attachments[attachment] =
      surface->drawable.fscreen->screen->resource_create(
         surface->drawable.fscreen->screen, &resource);
   return surface->attachments[attachment] != NULL;
}

static bool
armgl_validate(struct st_context *state_tracker,
               struct pipe_frontend_drawable *drawable,
               const enum st_attachment_type *attachments,
               unsigned count,
               struct pipe_resource **output,
               struct pipe_resource **resolve)
{
   struct armgl_surface *surface = armgl_surface_from_drawable(drawable);
   uint32_t requested_mask = 0;
   unsigned index;

   (void)state_tracker;

   if (!surface->width || !surface->height)
      return false;

   for (index = 0; index < count; ++index) {
      if (attachments[index] < 0 || attachments[index] >= ST_ATTACHMENT_COUNT)
         return false;
      requested_mask |= 1u << attachments[index];
   }
   if (requested_mask & ~surface->visual.buffer_mask)
      return false;

   if (surface->allocated_width != surface->width ||
       surface->allocated_height != surface->height)
      armgl_release_attachments(surface);

   for (index = 0; index < count; ++index) {
      enum st_attachment_type attachment = attachments[index];

      if (!surface->attachments[attachment] &&
          !armgl_create_attachment(surface, attachment))
         return false;

      pipe_resource_reference(&output[index],
                              surface->attachments[attachment]);
      if (resolve)
         resolve[index] = NULL;
   }

   surface->allocated_width = surface->width;
   surface->allocated_height = surface->height;
   surface->allocated_mask |= requested_mask;
   return true;
}

struct armgl_display *
armgl_display_create(struct pipe_screen *screen)
{
   struct armgl_display *display;

   if (!screen)
      return NULL;

   display = calloc(1, sizeof(*display));
   if (!display)
      return NULL;

   display->frontend.screen = screen;
   display->frontend.get_param = armgl_get_manager_param;
   return display;
}

void
armgl_display_destroy(struct armgl_display *display)
{
   struct pipe_screen *screen;

   if (!display)
      return;

   screen = display->frontend.screen;
   st_screen_destroy(&display->frontend);
   free(display);
   if (screen)
      screen->destroy(screen);
}

struct armgl_surface *
armgl_surface_create(struct armgl_display *display,
                     const struct armgl_surface_config *config)
{
   struct armgl_surface *surface;

   if (!display || !config || !config->width || !config->height)
      return NULL;

   surface = calloc(1, sizeof(*surface));
   if (!surface)
      return NULL;

   surface->width = config->width;
   surface->height = config->height;
   surface->visual.color_format = config->alpha
      ? PIPE_FORMAT_BGRA8888_UNORM : PIPE_FORMAT_BGRX8888_UNORM;
   surface->visual.depth_stencil_format = config->depth_stencil
      ? PIPE_FORMAT_Z24_UNORM_S8_UINT : PIPE_FORMAT_NONE;
   surface->visual.accum_format = PIPE_FORMAT_NONE;
   surface->visual.buffer_mask = ST_ATTACHMENT_FRONT_LEFT_MASK;
   if (config->double_buffered)
      surface->visual.buffer_mask |= ST_ATTACHMENT_BACK_LEFT_MASK;
   if (config->depth_stencil)
      surface->visual.buffer_mask |= ST_ATTACHMENT_DEPTH_STENCIL_MASK;

   surface->drawable.ID = p_atomic_inc_return(&armgl_next_drawable_id);
   surface->drawable.fscreen = &display->frontend;
   surface->drawable.visual = &surface->visual;
   surface->drawable.flush_front = armgl_flush_front;
   surface->drawable.validate = armgl_validate;
   p_atomic_set(&surface->drawable.stamp, 1);
   return surface;
}

void
armgl_surface_destroy(struct armgl_surface *surface)
{
   if (!surface)
      return;

   st_api_destroy_drawable(&surface->drawable);
   armgl_release_attachments(surface);
   free(surface);
}

bool
armgl_surface_resize(struct armgl_surface *surface,
                     uint32_t width, uint32_t height)
{
   if (!surface || !width || !height)
      return false;
   if (surface->width == width && surface->height == height)
      return true;

   surface->width = width;
   surface->height = height;
   p_atomic_inc(&surface->drawable.stamp);
   return true;
}

struct armgl_context *
armgl_context_create(struct armgl_display *display,
                     const struct armgl_context_config *config,
                     struct armgl_context *shared)
{
   struct armgl_context *context;
   struct st_context_attribs attributes;
   enum st_context_error error;

   if (!display || !config || config->major < 2)
      return NULL;

   context = calloc(1, sizeof(*context));
   if (!context)
      return NULL;

   memset(&attributes, 0, sizeof(attributes));
   attributes.profile = API_OPENGLES2;
   attributes.major = config->major;
   attributes.minor = config->minor;
   attributes.visual.color_format = config->alpha
      ? PIPE_FORMAT_BGRA8888_UNORM : PIPE_FORMAT_BGRX8888_UNORM;
   attributes.visual.depth_stencil_format = config->depth_stencil
      ? PIPE_FORMAT_Z24_UNORM_S8_UINT : PIPE_FORMAT_NONE;
   attributes.visual.accum_format = PIPE_FORMAT_NONE;
   attributes.visual.buffer_mask = ST_ATTACHMENT_FRONT_LEFT_MASK;
   if (config->double_buffered)
      attributes.visual.buffer_mask |= ST_ATTACHMENT_BACK_LEFT_MASK;
   if (config->depth_stencil)
      attributes.visual.buffer_mask |= ST_ATTACHMENT_DEPTH_STENCIL_MASK;
   if (config->debug)
      attributes.flags |= ST_CONTEXT_FLAG_DEBUG;
   if (config->no_error)
      attributes.flags |= ST_CONTEXT_FLAG_NO_ERROR;

   context->display = display;
   context->state_tracker = st_api_create_context(
      &display->frontend, &attributes, &error,
      shared ? shared->state_tracker : NULL);
   if (!context->state_tracker) {
      free(context);
      return NULL;
   }

   return context;
}

void
armgl_context_destroy(struct armgl_context *context)
{
   if (!context)
      return;

   if (st_api_get_current() == context->state_tracker)
      st_api_make_current(NULL, NULL, NULL);
   st_context_flush(context->state_tracker, 0, NULL, NULL, NULL);
   st_destroy_context(context->state_tracker);
   free(context);
}

bool
armgl_make_current(struct armgl_context *context,
                   struct armgl_surface *draw,
                   struct armgl_surface *read)
{
   if (!context)
      return st_api_make_current(NULL, NULL, NULL);
   if (!draw || !read || draw->drawable.fscreen != &context->display->frontend ||
       read->drawable.fscreen != &context->display->frontend)
      return false;

   return st_api_make_current(context->state_tracker,
                              &draw->drawable, &read->drawable);
}

bool
armgl_flush(struct armgl_context *context, bool wait)
{
   struct pipe_fence_handle *fence = NULL;
   struct pipe_screen *screen;
   unsigned flags = ST_FLUSH_END_OF_FRAME;
   bool completed = true;

   if (!context || !context->state_tracker)
      return false;
   if (wait)
      flags |= ST_FLUSH_WAIT;

   st_context_flush(context->state_tracker, flags,
                    wait ? &fence : NULL, NULL, NULL);
   if (!fence)
      return true;

   screen = context->display->frontend.screen;
   completed = screen->fence_finish(screen, context->state_tracker->pipe,
                                    fence, OS_TIMEOUT_INFINITE);
   screen->fence_reference(screen, &fence, NULL);
   return completed;
}

struct st_context *
armgl_state_tracker_context(struct armgl_context *context)
{
   return context ? context->state_tracker : NULL;
}
