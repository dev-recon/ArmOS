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

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include "frontend/api.h"
#include "frontend/winsys_handle.h"
#include "pipe/p_context.h"
#include "pipe/p_defines.h"
#include "pipe/p_screen.h"
#include "state_tracker/st_context.h"
#include "util/u_atomic.h"
#include "util/u_inlines.h"

struct armgl_display {
   struct pipe_frontend_screen frontend;
};

struct armgl_image {
   struct armgl_display *display;
   struct pipe_resource *resource;
   uint32_t width;
   uint32_t height;
   uint32_t stride;
   uint32_t usage;
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
   bool exportable;
   bool scanout;
};

struct armgl_context {
   struct armgl_display *display;
   struct st_context *state_tracker;
};

static uint32_t armgl_next_drawable_id;

static unsigned
armgl_surface_color_bind(const struct armgl_surface *surface)
{
   unsigned bind = PIPE_BIND_RENDER_TARGET | PIPE_BIND_SAMPLER_VIEW;

   if (surface->exportable)
      bind |= PIPE_BIND_SHARED;
   if (surface->scanout)
      bind |= PIPE_BIND_SCANOUT;
   return bind;
}

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
      resource.bind = armgl_surface_color_bind(surface);
   }

   if (resource.format == PIPE_FORMAT_NONE)
      return false;

   errno = 0;
   surface->attachments[attachment] =
      surface->drawable.fscreen->screen->resource_create(
         surface->drawable.fscreen->screen, &resource);
   if (!surface->attachments[attachment] && errno == 0)
      errno = EIO;
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

struct armgl_image *
armgl_image_import_fd(struct armgl_display *display,
                      const struct armgl_image_config *config,
                      int buffer_fd)
{
   struct armgl_image *image;
   struct pipe_resource resource;
   struct winsys_handle handle;
   struct pipe_screen *screen;
   unsigned handle_usage = 0;

   if (!display || !config || buffer_fd < 0 || !config->width ||
       !config->height || config->width > UINT32_MAX / 4u ||
       config->stride < config->width * 4u ||
       !(config->usage &
         (ARMGL_IMAGE_USAGE_SAMPLED | ARMGL_IMAGE_USAGE_RENDER_TARGET)) ||
       (config->usage &
        ~(ARMGL_IMAGE_USAGE_SAMPLED | ARMGL_IMAGE_USAGE_RENDER_TARGET)))
      return NULL;

   screen = display->frontend.screen;
   if (!screen || !screen->resource_from_handle)
      return NULL;

   image = calloc(1, sizeof(*image));
   if (!image)
      return NULL;

   memset(&resource, 0, sizeof(resource));
   resource.target = PIPE_TEXTURE_2D;
   resource.format = config->alpha
      ? PIPE_FORMAT_BGRA8888_UNORM : PIPE_FORMAT_BGRX8888_UNORM;
   resource.width0 = config->width;
   resource.height0 = config->height;
   resource.depth0 = 1;
   resource.array_size = 1;
   resource.bind = 0;
   if (config->usage & ARMGL_IMAGE_USAGE_SAMPLED)
      resource.bind |= PIPE_BIND_SAMPLER_VIEW;
   if (config->usage & ARMGL_IMAGE_USAGE_RENDER_TARGET) {
      resource.bind |= PIPE_BIND_RENDER_TARGET;
      handle_usage |= PIPE_HANDLE_USAGE_FRAMEBUFFER_WRITE;
   }
   /* An imported FD is shared by definition.  Preserve that property in
    * Gallium so VirGL never substitutes a private staging resource.
    */
   resource.bind |= PIPE_BIND_SHARED;

   memset(&handle, 0, sizeof(handle));
   handle.type = WINSYS_HANDLE_TYPE_FD;
   handle.handle = (unsigned)buffer_fd;
   handle.stride = config->stride;
   image->resource = screen->resource_from_handle(
      screen, &resource, &handle, handle_usage);
   if (!image->resource) {
      free(image);
      return NULL;
   }

   image->display = display;
   image->width = config->width;
   image->height = config->height;
   image->stride = config->stride;
   image->usage = config->usage;
   return image;
}

static bool
armgl_image_config_valid(const struct armgl_image_config *config)
{
   return config && config->width && config->height &&
      config->width <= UINT32_MAX / 4u &&
      config->stride >= config->width * 4u &&
      (config->usage &
       (ARMGL_IMAGE_USAGE_SAMPLED | ARMGL_IMAGE_USAGE_RENDER_TARGET)) &&
      !(config->usage &
        ~(ARMGL_IMAGE_USAGE_SAMPLED | ARMGL_IMAGE_USAGE_RENDER_TARGET));
}

struct armgl_image *
armgl_image_create(struct armgl_display *display,
                   const struct armgl_image_config *config)
{
   struct armgl_image *image;
   struct pipe_resource resource;
   struct pipe_screen *screen;

   if (!display || !armgl_image_config_valid(config))
      return NULL;
   screen = display->frontend.screen;
   if (!screen || !screen->resource_create)
      return NULL;

   image = calloc(1, sizeof(*image));
   if (!image)
      return NULL;

   memset(&resource, 0, sizeof(resource));
   resource.target = PIPE_TEXTURE_2D;
   resource.format = config->alpha
      ? PIPE_FORMAT_BGRA8888_UNORM : PIPE_FORMAT_BGRX8888_UNORM;
   resource.width0 = config->width;
   resource.height0 = config->height;
   resource.depth0 = 1;
   resource.array_size = 1;
   if (config->usage & ARMGL_IMAGE_USAGE_SAMPLED)
      resource.bind |= PIPE_BIND_SAMPLER_VIEW;
   if (config->usage & ARMGL_IMAGE_USAGE_RENDER_TARGET)
      resource.bind |= PIPE_BIND_RENDER_TARGET;

   image->resource = screen->resource_create(screen, &resource);
   if (!image->resource) {
      free(image);
      return NULL;
   }
   image->display = display;
   image->width = config->width;
   image->height = config->height;
   image->stride = config->stride;
   image->usage = config->usage;
   return image;
}

void
armgl_image_destroy(struct armgl_image *image)
{
   if (!image)
      return;
   pipe_resource_reference(&image->resource, NULL);
   free(image);
}

struct armgl_surface *
armgl_surface_create(struct armgl_display *display,
                     const struct armgl_surface_config *config)
{
   struct pipe_screen *screen;
   struct armgl_surface *surface;
   unsigned color_bind;

   if (!display || !config || !config->width || !config->height)
      return NULL;

   surface = calloc(1, sizeof(*surface));
   if (!surface)
      return NULL;

   surface->width = config->width;
   surface->height = config->height;
   surface->exportable = config->exportable || config->scanout;
   surface->scanout = config->scanout;
   surface->visual.color_format = config->alpha
      ? PIPE_FORMAT_BGRA8888_UNORM : PIPE_FORMAT_BGRX8888_UNORM;
   screen = display->frontend.screen;
   color_bind = armgl_surface_color_bind(surface);
   if (!screen || !screen->is_format_supported ||
       !screen->is_format_supported(screen, surface->visual.color_format,
                                    PIPE_TEXTURE_2D, 0, 0, color_bind)) {
      free(surface);
      errno = ENOTSUP;
      return NULL;
   }
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

bool
armgl_surface_set_color_image(struct armgl_surface *surface,
                              struct armgl_image *image)
{
   if (!surface || !image || !image->resource ||
       !(image->usage & ARMGL_IMAGE_USAGE_RENDER_TARGET) ||
       surface->drawable.fscreen != &image->display->frontend)
      return false;

   armgl_release_attachments(surface);
   pipe_resource_reference(
      &surface->attachments[ST_ATTACHMENT_FRONT_LEFT], image->resource);
   surface->width = image->width;
   surface->height = image->height;
   surface->allocated_width = image->width;
   surface->allocated_height = image->height;
   surface->allocated_mask = ST_ATTACHMENT_FRONT_LEFT_MASK;
   p_atomic_inc(&surface->drawable.stamp);
   return true;
}

static bool
armgl_rect_valid(const struct armgl_rect *rect,
                 uint32_t width, uint32_t height)
{
   return rect && rect->width && rect->height && rect->x < width &&
      rect->y < height && rect->width <= width - rect->x &&
      rect->height <= height - rect->y &&
      rect->width <= INT32_MAX && rect->height <= INT32_MAX &&
      rect->x <= INT32_MAX && rect->y <= INT32_MAX;
}

bool
armgl_image_upload(struct armgl_context *context,
                   struct armgl_image *image,
                   const struct armgl_rect *destination_rect,
                   const void *pixels, uint32_t stride)
{
   struct pipe_context *pipe;
   struct pipe_box box;

   if (!context || !context->state_tracker || !image || !image->resource ||
       image->display != context->display || !pixels ||
       !armgl_rect_valid(destination_rect, image->width, image->height) ||
       destination_rect->width > UINT32_MAX / 4u ||
       stride < destination_rect->width * 4u)
      return false;
   pipe = context->state_tracker->pipe;
   if (!pipe || !pipe->texture_subdata)
      return false;

   memset(&box, 0, sizeof(box));
   box.x = (int)destination_rect->x;
   box.y = (int)destination_rect->y;
   box.width = (int)destination_rect->width;
   box.height = (int)destination_rect->height;
   box.depth = 1;
   pipe->texture_subdata(pipe, image->resource, 0,
                         PIPE_MAP_WRITE | PIPE_MAP_DISCARD_RANGE,
                         &box, pixels, stride,
                         (uintptr_t)stride * destination_rect->height);
   return true;
}

bool
armgl_surface_blit_image(struct armgl_context *context,
                         struct armgl_surface *destination,
                         struct armgl_image *source,
                         const struct armgl_rect *source_rect,
                         const struct armgl_rect *destination_rect,
                         bool alpha_blend)
{
   struct pipe_blit_info blit;
   struct pipe_context *pipe;

   if (!context || !context->state_tracker || !destination || !source ||
       !source->resource ||
       !(source->usage & ARMGL_IMAGE_USAGE_SAMPLED) ||
       destination->drawable.fscreen != &context->display->frontend ||
       source->display != context->display ||
       !armgl_rect_valid(source_rect, source->width, source->height) ||
       !armgl_rect_valid(destination_rect, destination->width,
                         destination->height))
      return false;

   if (destination->allocated_width != destination->width ||
       destination->allocated_height != destination->height)
      armgl_release_attachments(destination);
   if (!destination->attachments[ST_ATTACHMENT_FRONT_LEFT] &&
       !armgl_create_attachment(destination, ST_ATTACHMENT_FRONT_LEFT))
      return false;
   destination->allocated_width = destination->width;
   destination->allocated_height = destination->height;
   destination->allocated_mask |= ST_ATTACHMENT_FRONT_LEFT_MASK;

   pipe = context->state_tracker->pipe;
   if (!pipe || !pipe->blit)
      return false;
   memset(&blit, 0, sizeof(blit));
   blit.src.resource = source->resource;
   blit.src.format = source->resource->format;
   blit.src.box.x = (int)source_rect->x;
   blit.src.box.y = (int)source_rect->y;
   blit.src.box.z = 0;
   blit.src.box.width = (int)source_rect->width;
   blit.src.box.height = (int)source_rect->height;
   blit.src.box.depth = 1;
   blit.dst.resource =
      destination->attachments[ST_ATTACHMENT_FRONT_LEFT];
   blit.dst.format = blit.dst.resource->format;
   blit.dst.box.x = (int)destination_rect->x;
   blit.dst.box.y = (int)destination_rect->y;
   blit.dst.box.z = 0;
   blit.dst.box.width = (int)destination_rect->width;
   blit.dst.box.height = (int)destination_rect->height;
   blit.dst.box.depth = 1;
   blit.mask = PIPE_MASK_RGBA;
   blit.filter = PIPE_TEX_FILTER_NEAREST;
   blit.alpha_blend = alpha_blend;
   pipe->blit(pipe, &blit);
   return true;
}

bool
armgl_surface_fill_rect(struct armgl_context *context,
                        struct armgl_surface *destination,
                        const struct armgl_rect *destination_rect,
                        uint32_t argb8888)
{
   union pipe_color_union color;
   struct pipe_surface surface_template;
   struct pipe_surface *surface;
   struct pipe_context *pipe;
   bool surface_owned = false;

   if (!context || !context->state_tracker || !destination ||
       destination->drawable.fscreen != &context->display->frontend ||
       !armgl_rect_valid(destination_rect, destination->width,
                         destination->height))
      return false;
   if (destination->allocated_width != destination->width ||
       destination->allocated_height != destination->height)
      armgl_release_attachments(destination);
   if (!destination->attachments[ST_ATTACHMENT_FRONT_LEFT] &&
       !armgl_create_attachment(destination, ST_ATTACHMENT_FRONT_LEFT))
      return false;
   destination->allocated_width = destination->width;
   destination->allocated_height = destination->height;
   destination->allocated_mask |= ST_ATTACHMENT_FRONT_LEFT_MASK;

   pipe = context->state_tracker->pipe;
   if (!pipe || !pipe->clear_render_target) {
      errno = ENOTSUP;
      return false;
   }
   memset(&surface_template, 0, sizeof(surface_template));
   surface_template.format =
      destination->attachments[ST_ATTACHMENT_FRONT_LEFT]->format;
   surface_template.texture =
      destination->attachments[ST_ATTACHMENT_FRONT_LEFT];
   surface_template.first_layer = 0;
   surface_template.last_layer = 0;
   if (pipe->create_surface) {
      surface = pipe->create_surface(pipe, surface_template.texture,
                                     &surface_template);
      if (!surface) {
         errno = EIO;
         return false;
      }
      surface_owned = true;
   } else {
      /* Modern VirGL consumes the embedded pipe_surface view directly and
       * deliberately leaves create_surface unset.  The clear operation is
       * synchronous, so this local view needs no retained resource.
       */
      surface_template.context = pipe;
      surface = &surface_template;
   }

   color.f[0] = (float)((argb8888 >> 16) & 0xffu) / 255.0f;
   color.f[1] = (float)((argb8888 >> 8) & 0xffu) / 255.0f;
   color.f[2] = (float)(argb8888 & 0xffu) / 255.0f;
   color.f[3] = (float)((argb8888 >> 24) & 0xffu) / 255.0f;
   pipe->clear_render_target(pipe, surface, &color,
                             destination_rect->x,
                             destination_rect->y,
                             destination_rect->width,
                             destination_rect->height, false);
   if (surface_owned)
      pipe_surface_reference(&surface, NULL);
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

   if (!display || !config || config->major < 2 ||
       (shared && shared->display != display))
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

bool
armgl_flush_fence_fd(struct armgl_context *context, int *fence_fd)
{
   struct pipe_fence_handle *fence = NULL;
   struct pipe_screen *screen;
   int fd;

   if (!context || !context->state_tracker || !fence_fd)
      return false;
   *fence_fd = -1;
   screen = context->display->frontend.screen;
   if (!screen->fence_get_fd)
      return false;

   st_context_flush(context->state_tracker,
                    ST_FLUSH_END_OF_FRAME | ST_FLUSH_FENCE_FD,
                    &fence, NULL, NULL);
   if (!fence)
      return false;
   fd = screen->fence_get_fd(screen, fence);
   screen->fence_reference(screen, &fence, NULL);
   if (fd < 0)
      return false;
   *fence_fd = fd;
   return true;
}

bool
armgl_surface_export_color_fd(struct armgl_surface *surface,
                              struct armgl_context *context,
                              int *buffer_fd, uint32_t *stride)
{
   struct winsys_handle handle;
   struct pipe_screen *screen;
   struct pipe_resource *resource;

   if (!surface || !surface->exportable || !context || !buffer_fd || !stride ||
       surface->drawable.fscreen != &context->display->frontend)
      return false;
   *buffer_fd = -1;
   *stride = 0;
   resource = surface->attachments[ST_ATTACHMENT_FRONT_LEFT];
   screen = context->display->frontend.screen;
   if (!resource || !screen->resource_get_handle)
      return false;

   memset(&handle, 0, sizeof(handle));
   handle.type = WINSYS_HANDLE_TYPE_FD;
   if (!screen->resource_get_handle(screen, context->state_tracker->pipe,
                                    resource, &handle,
                                    PIPE_HANDLE_USAGE_FRAMEBUFFER_WRITE))
      return false;
   *buffer_fd = (int)handle.handle;
   *stride = handle.stride;
   return true;
}

struct st_context *
armgl_state_tracker_context(struct armgl_context *context)
{
   return context ? context->state_tracker : NULL;
}
