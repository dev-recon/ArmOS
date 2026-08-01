/*
 * ArmOS
 * Copyright (c) 2026 Mohamed Ennassiri
 *
 * Licensed under the Apache License, Version 2.0.
 * See LICENSE for details.
 *
 * File: userland/opt/mesa/armos/egl_armos.c
 * Layer: Third-party port / Mesa EGL platform
 *
 * Responsibilities:
 * - Expose Mesa GLES contexts through EGL on ArmOS and Wayland.
 * - Adapt pbuffer and window swapchain lifetime to the common ArmGL frontend.
 * - Select the current render-node pipe screen without exposing it to clients.
 *
 * Notes:
 * - GPU buffers cross the Wayland boundary as capability descriptors.
 * - Explicit fences protect acquisition; compositor release protects reuse.
 */

#include <errno.h>
#include <poll.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <uapi/armos/drm.h>
#include <wayland-armos-gpu-client-protocol.h>
#include <wayland-client-core.h>
#include <wayland-client-protocol.h>
#include <wayland-egl-backend.h>

#include "eglconfig.h"
#include "eglcontext.h"
#include "eglcurrent.h"
#include "egldisplay.h"
#include "egldriver.h"
#include "egllog.h"
#include "eglsurface.h"

#include "armgl_frontend.h"
#include "pipe/p_screen.h"
#include "pipe_loader_armos.h"
#include "util/u_atomic.h"

#define ARMOS_RENDER_NODE "/dev/dri/renderD128"
#define ARMOS_EGL_SWAPCHAIN_IMAGES 3u

_EGL_DRIVER_STANDARD_TYPECASTS(armos_egl)

struct armos_egl_display {
   struct armgl_display *armgl;
   struct wl_display *wayland;
   struct wl_event_queue *queue;
   struct armos_gpu_buffer_manager_v1 *gpu_manager;
   int references;
};

struct armos_egl_config {
   _EGLConfig base;
};

struct armos_egl_context {
   _EGLContext base;
   struct armos_egl_display *owner;
   struct armgl_context *armgl;
};

struct armos_egl_surface;

struct armos_egl_image {
   struct armos_egl_surface *owner;
   struct armgl_surface *armgl;
   struct wl_buffer *buffer;
   uint32_t width;
   uint32_t height;
   int release_fence_fd;
   bool busy;
   bool release_failed;
};

struct armos_egl_surface {
   _EGLSurface base;
   struct armos_egl_display *owner;
   struct armgl_surface *armgl;
   struct wl_egl_window *native_window;
   struct wl_surface *wayland_surface;
   struct armos_egl_image images[ARMOS_EGL_SWAPCHAIN_IMAGES];
   unsigned current_image;
};

static void
armos_egl_display_reference(struct armos_egl_display *display)
{
   p_atomic_inc(&display->references);
}

static void
armos_egl_display_release(struct armos_egl_display *display)
{
   if (!display || p_atomic_dec_return(&display->references))
      return;

   if (display->gpu_manager)
      armos_gpu_buffer_manager_v1_destroy(display->gpu_manager);
   if (display->queue)
      wl_event_queue_destroy(display->queue);
   armgl_display_destroy(display->armgl);
   free(display);
}

static struct armos_egl_image *
armos_egl_image_from_buffer(struct wl_buffer *buffer)
{
   return buffer ? wl_proxy_get_user_data((struct wl_proxy *)buffer) : NULL;
}

static void
armos_egl_gpu_fenced_release(
   void *data, struct armos_gpu_buffer_manager_v1 *manager,
   struct wl_buffer *buffer, int fence_fd)
{
   struct armos_egl_image *image = armos_egl_image_from_buffer(buffer);

   (void)data;
   (void)manager;
   if (!image || fence_fd < 0) {
      if (fence_fd >= 0)
         close(fence_fd);
      return;
   }
   if (image->release_fence_fd >= 0)
      close(image->release_fence_fd);
   image->release_fence_fd = fence_fd;
   image->busy = true;
}

static void
armos_egl_gpu_immediate_release(
   void *data, struct armos_gpu_buffer_manager_v1 *manager,
   struct wl_buffer *buffer)
{
   struct armos_egl_image *image = armos_egl_image_from_buffer(buffer);

   (void)data;
   (void)manager;
   if (!image)
      return;
   if (image->release_fence_fd >= 0) {
      close(image->release_fence_fd);
      image->release_fence_fd = -1;
   }
   image->busy = false;
   image->release_failed = false;
}

static const struct armos_gpu_buffer_manager_v1_listener
armos_egl_gpu_listener = {
   .fenced_release = armos_egl_gpu_fenced_release,
   .immediate_release = armos_egl_gpu_immediate_release,
};

static void
armos_egl_registry_global(void *data, struct wl_registry *registry,
                          uint32_t name, const char *interface,
                          uint32_t version)
{
   struct armos_egl_display *display = data;

   if (display->gpu_manager ||
       strcmp(interface, "armos_gpu_buffer_manager_v1") != 0)
      return;
   display->gpu_manager = wl_registry_bind(
      registry, name, &armos_gpu_buffer_manager_v1_interface,
      version < 1u ? version : 1u);
   if (!display->gpu_manager)
      return;
   wl_proxy_set_queue((struct wl_proxy *)display->gpu_manager,
                      display->queue);
   if (armos_gpu_buffer_manager_v1_add_listener(
          display->gpu_manager, &armos_egl_gpu_listener, display) < 0) {
      armos_gpu_buffer_manager_v1_destroy(display->gpu_manager);
      display->gpu_manager = NULL;
   }
}

static void
armos_egl_registry_remove(void *data, struct wl_registry *registry,
                          uint32_t name)
{
   (void)data;
   (void)registry;
   (void)name;
}

static const struct wl_registry_listener armos_egl_registry_listener = {
   .global = armos_egl_registry_global,
   .global_remove = armos_egl_registry_remove,
};

static bool
armos_egl_wayland_initialize(struct armos_egl_display *display,
                             struct wl_display *wayland)
{
   struct wl_registry *registry;
   int result;

   if (!display || !wayland)
      return false;
   display->wayland = wayland;
   display->queue = wl_display_create_queue(wayland);
   if (!display->queue)
      return false;
   registry = wl_display_get_registry(wayland);
   if (!registry)
      return false;
   wl_proxy_set_queue((struct wl_proxy *)registry, display->queue);
   result = wl_registry_add_listener(
      registry, &armos_egl_registry_listener, display);
   if (result == 0)
      result = wl_display_roundtrip_queue(wayland, display->queue);
   wl_registry_destroy(registry);
   return result >= 0 && display->gpu_manager != NULL;
}

static void
armos_egl_image_reset(struct armos_egl_image *image)
{
   if (!image)
      return;
   if (image->release_fence_fd >= 0)
      close(image->release_fence_fd);
   image->release_fence_fd = -1;
   if (image->buffer) {
      wl_proxy_set_user_data((struct wl_proxy *)image->buffer, NULL);
      wl_buffer_destroy(image->buffer);
   }
   armgl_surface_destroy(image->armgl);
   image->buffer = NULL;
   image->armgl = NULL;
   image->width = 0;
   image->height = 0;
   image->busy = false;
   image->release_failed = false;
}

static bool
armos_egl_image_finish_release(struct armos_egl_image *image, bool wait)
{
   armos_drm_fence_result_t result;
   struct pollfd descriptor;
   ssize_t count;

   if (!image || !image->busy || image->release_fence_fd < 0)
      return image && !image->busy;
   descriptor.fd = image->release_fence_fd;
   descriptor.events = POLLIN;
   descriptor.revents = 0;
   do {
      count = poll(&descriptor, 1u, wait ? -1 : 0);
   } while (count < 0 && errno == EINTR);
   if (count <= 0 || !(descriptor.revents & (POLLIN | POLLHUP | POLLERR)))
      return false;
   do {
      count = read(image->release_fence_fd, &result, sizeof(result));
   } while (count < 0 && errno == EINTR);
   close(image->release_fence_fd);
   image->release_fence_fd = -1;
   image->busy = false;
   image->release_failed =
      count != (ssize_t)sizeof(result) || result.status != 0;
   return true;
}

static bool
armos_egl_image_create(struct armos_egl_surface *surface,
                       struct armos_egl_image *image,
                       uint32_t width, uint32_t height)
{
   struct armgl_surface_config config;

   if (!surface || !image || !width || !height || image->busy)
      return false;
   armos_egl_image_reset(image);
   memset(&config, 0, sizeof(config));
   config.width = width;
   config.height = height;
   config.alpha = surface->base.Config->AlphaSize != 0;
   config.depth_bits = surface->base.Config->DepthSize;
   config.stencil_bits = surface->base.Config->StencilSize;
   config.double_buffered = false;
   config.exportable = true;
   image->armgl = armgl_surface_create(surface->owner->armgl, &config);
   if (!image->armgl)
      return false;
   image->owner = surface;
   image->width = width;
   image->height = height;
   image->release_fence_fd = -1;
   return true;
}

static bool
armos_egl_dispatch_releases(struct armos_egl_display *display, bool wait)
{
   struct pollfd descriptor;
   int result;

   if (wl_display_dispatch_queue_pending(display->wayland,
                                         display->queue) < 0)
      return false;
   if (!wait)
      return true;
   descriptor.fd = wl_display_get_fd(display->wayland);
   descriptor.events = POLLIN;
   descriptor.revents = 0;
   do {
      result = poll(&descriptor, 1u, -1);
   } while (result < 0 && errno == EINTR);
   if (result <= 0)
      return false;
   return wl_display_dispatch_queue(display->wayland,
                                    display->queue) >= 0;
}

static struct armos_egl_image *
armos_egl_acquire_image(struct armos_egl_surface *surface,
                        unsigned excluded,
                        uint32_t width, uint32_t height)
{
   for (;;) {
      unsigned index;

      if (!armos_egl_dispatch_releases(surface->owner, false))
         return NULL;
      for (index = 0; index < ARMOS_EGL_SWAPCHAIN_IMAGES; ++index) {
         struct armos_egl_image *image = &surface->images[index];

         if (index == excluded)
            continue;
         (void)armos_egl_image_finish_release(image, false);
         if (image->busy)
            continue;
         if (image->release_failed)
            image->release_failed = false;
         if ((!image->armgl || image->width != width ||
              image->height != height) &&
             !armos_egl_image_create(surface, image, width, height))
            return NULL;
         return image;
      }
      {
         struct pollfd descriptors[1u + ARMOS_EGL_SWAPCHAIN_IMAGES];
         unsigned descriptor_count = 1u;
         int result;

         descriptors[0].fd = wl_display_get_fd(surface->owner->wayland);
         descriptors[0].events = POLLIN;
         descriptors[0].revents = 0;
         for (index = 0; index < ARMOS_EGL_SWAPCHAIN_IMAGES; ++index) {
            struct armos_egl_image *image = &surface->images[index];

            if (image->busy && image->release_fence_fd >= 0) {
               descriptors[descriptor_count].fd = image->release_fence_fd;
               descriptors[descriptor_count].events = POLLIN;
               descriptors[descriptor_count].revents = 0;
               descriptor_count++;
            }
         }
         do {
            result = poll(descriptors, descriptor_count, -1);
         } while (result < 0 && errno == EINTR);
         if (result <= 0)
            return NULL;
         if ((descriptors[0].revents &
              (POLLIN | POLLHUP | POLLERR)) != 0 &&
             wl_display_dispatch_queue(surface->owner->wayland,
                                       surface->owner->queue) < 0)
            return NULL;
         for (index = 0; index < ARMOS_EGL_SWAPCHAIN_IMAGES; ++index)
            (void)armos_egl_image_finish_release(
               &surface->images[index], false);
      }
   }
}

static bool
armos_egl_image_create_buffer(struct armos_egl_surface *surface,
                              struct armos_egl_image *image,
                              struct armos_egl_context *context)
{
   uint32_t stride;
   int fd;

   if (image->buffer)
      return true;
   if (!armgl_surface_export_color_fd(image->armgl, context->armgl,
                                      &fd, &stride))
      return false;
   (void)stride;
   image->buffer = armos_gpu_buffer_manager_v1_create_buffer(
      surface->owner->gpu_manager, fd);
   close(fd);
   if (!image->buffer)
      return false;
   wl_proxy_set_queue((struct wl_proxy *)image->buffer,
                      surface->owner->queue);
   wl_proxy_set_user_data((struct wl_proxy *)image->buffer, image);
   return true;
}

static EGLBoolean
armos_egl_add_config(_EGLDisplay *display, EGLint config_id,
                     EGLint depth_size, EGLint stencil_size)
{
   struct armos_egl_config *config = calloc(1, sizeof(*config));

   if (!config)
      return _eglError(EGL_BAD_ALLOC, "ArmOS EGL config");

   _eglInitConfig(&config->base, display, config_id);
   config->base.BufferSize = 32;
   config->base.RedSize = 8;
   config->base.GreenSize = 8;
   config->base.BlueSize = 8;
   config->base.AlphaSize = 8;
   config->base.DepthSize = depth_size;
   config->base.StencilSize = stencil_size;
   config->base.ColorBufferType = EGL_RGB_BUFFER;
   config->base.ConfigCaveat = EGL_NONE;
   config->base.ConfigID = config_id;
   config->base.Level = 0;
   config->base.NativeRenderable = EGL_FALSE;
   config->base.NativeVisualID = 0;
   config->base.NativeVisualType = EGL_NONE;
   config->base.RenderableType = EGL_OPENGL_ES2_BIT;
   config->base.Conformant = EGL_OPENGL_ES2_BIT;
   config->base.SurfaceType = EGL_PBUFFER_BIT;
   if (armos_egl_display(display)->gpu_manager)
      config->base.SurfaceType |= EGL_WINDOW_BIT;
   config->base.TransparentType = EGL_NONE;
   config->base.MaxPbufferWidth = _EGL_MAX_PBUFFER_WIDTH;
   config->base.MaxPbufferHeight = _EGL_MAX_PBUFFER_HEIGHT;
   config->base.MaxPbufferPixels =
      _EGL_MAX_PBUFFER_WIDTH * _EGL_MAX_PBUFFER_HEIGHT;
   config->base.MinSwapInterval = 0;
   config->base.MaxSwapInterval = 0;

   if (!_eglValidateConfig(&config->base, EGL_FALSE)) {
      free(config);
      return _eglError(EGL_BAD_CONFIG, "ArmOS EGL config validation");
   }

   _eglLinkConfig(&config->base);
   return EGL_TRUE;
}

static EGLBoolean
armos_egl_initialize(_EGLDisplay *display)
{
   struct armos_egl_display *armos = armos_egl_display(display);
   struct pipe_screen *screen;

   if (armos)
      return EGL_TRUE;

   if (display->Platform != _EGL_PLATFORM_SURFACELESS &&
       display->Platform != _EGL_PLATFORM_WAYLAND)
      return _eglError(EGL_BAD_PARAMETER, "ArmOS EGL platform");

   armos = calloc(1, sizeof(*armos));
   if (!armos)
      return _eglError(EGL_BAD_ALLOC, "ArmOS EGL display");

   screen = armos_pipe_screen_create(ARMOS_RENDER_NODE);
   if (!screen) {
      free(armos);
      return _eglError(EGL_NOT_INITIALIZED, "ArmOS Gallium screen");
   }

   armos->armgl = armgl_display_create(screen);
   if (!armos->armgl) {
      screen->destroy(screen);
      free(armos);
      return _eglError(EGL_NOT_INITIALIZED, "ArmOS ArmGL display");
   }

   armos->references = 1;
   if (display->Platform == _EGL_PLATFORM_WAYLAND &&
       !armos_egl_wayland_initialize(armos, display->PlatformDisplay)) {
      armos_egl_display_release(armos);
      return _eglError(EGL_NOT_INITIALIZED,
                       "ArmOS Wayland GPU buffer protocol");
   }
   display->DriverData = armos;
   display->ClientAPIs = EGL_OPENGL_ES_BIT;
   display->Extensions.KHR_create_context = EGL_TRUE;
   display->Extensions.KHR_no_config_context = EGL_TRUE;
   display->Extensions.KHR_surfaceless_context = EGL_TRUE;
   display->Extensions.MESA_query_driver = EGL_TRUE;

   /*
    * Keep the default configuration color-only.  Applications which do not
    * request a depth or stencil buffer must not inherit one: besides wasting
    * memory, some VirGL hosts cannot create Z24S8 resources even though the
    * advertised capset contains the format.  A distinct configuration keeps
    * the EGL contract explicit.  Z24/S8 is deliberately not advertised until
    * the backend can validate resource creation, rendering and export rather
    * than trusting a potentially optimistic VirGL capset.
    */
   if (!armos_egl_add_config(display, 1, 0, 0)) {
      display->DriverData = NULL;
      armos_egl_display_release(armos);
      return EGL_FALSE;
   }

   /* Advertise depth only after the selected DRM/Gallium provider confirms
    * the exact resource format.  Z16 is the portable GLES2 baseline and
    * avoids relying on optimistic packed Z24S8 VirGL capsets. */
   if (screen->is_format_supported &&
       screen->is_format_supported(screen, PIPE_FORMAT_Z16_UNORM,
                                   PIPE_TEXTURE_2D, 0, 0,
                                   PIPE_BIND_DEPTH_STENCIL) &&
       !armos_egl_add_config(display, 2, 16, 0)) {
      display->DriverData = NULL;
      armos_egl_display_release(armos);
      return EGL_FALSE;
   }

   return EGL_TRUE;
}

static EGLBoolean
armos_egl_terminate(_EGLDisplay *display)
{
   struct armos_egl_display *armos = armos_egl_display(display);

   if (!armos)
      return EGL_TRUE;

   _eglReleaseDisplayResources(display);
   display->DriverData = NULL;
   armos_egl_display_release(armos);
   return EGL_TRUE;
}

static _EGLSurface *
armos_egl_create_pbuffer(_EGLDisplay *display, _EGLConfig *config,
                         const EGLint *attributes)
{
   struct armos_egl_surface *surface;
   struct armgl_surface_config armgl_config;

   surface = calloc(1, sizeof(*surface));
   if (!surface)
      return NULL;

   if (!_eglInitSurface(&surface->base, display, EGL_PBUFFER_BIT,
                        config, attributes, NULL)) {
      free(surface);
      return NULL;
   }

   memset(&armgl_config, 0, sizeof(armgl_config));
   armgl_config.width = surface->base.Width;
   armgl_config.height = surface->base.Height;
   armgl_config.alpha = config->AlphaSize != 0;
   armgl_config.depth_bits = config->DepthSize;
   armgl_config.stencil_bits = config->StencilSize;
   armgl_config.double_buffered = false;
   surface->armgl = armgl_surface_create(
      armos_egl_display(display)->armgl, &armgl_config);
   if (!surface->armgl) {
      free(surface);
      _eglError(EGL_BAD_ALLOC, "ArmOS EGL pbuffer");
      return NULL;
   }

   surface->owner = armos_egl_display(display);
   armos_egl_display_reference(surface->owner);

   return &surface->base;
}

static _EGLSurface *
armos_egl_create_window(_EGLDisplay *display, _EGLConfig *config,
                        void *native_window, const EGLint *attributes)
{
   struct armos_egl_display *owner = armos_egl_display(display);
   struct wl_egl_window *window = native_window;
   struct armos_egl_surface *surface;
   uint32_t width;
   uint32_t height;
   unsigned index;

   if (!owner->gpu_manager ||
       armos_wl_egl_window_get_abi(window) !=
          ARMOS_WL_EGL_WINDOW_ABI_VERSION ||
       armos_wl_egl_window_get_display(window) != owner->wayland ||
       armos_wl_egl_window_get_size(window, &width, &height) < 0) {
      _eglError(EGL_BAD_NATIVE_WINDOW, "ArmOS Wayland EGL window");
      return NULL;
   }

   surface = calloc(1, sizeof(*surface));
   if (!surface)
      return NULL;
   for (index = 0; index < ARMOS_EGL_SWAPCHAIN_IMAGES; ++index) {
      surface->images[index].owner = surface;
      surface->images[index].release_fence_fd = -1;
   }
   if (!_eglInitSurface(&surface->base, display, EGL_WINDOW_BIT,
                        config, attributes, native_window)) {
      free(surface);
      return NULL;
   }
   surface->owner = owner;
   surface->native_window = window;
   surface->wayland_surface = armos_wl_egl_window_get_surface(window);
   surface->base.Width = (EGLint)width;
   surface->base.Height = (EGLint)height;
   surface->base.SwapInterval = 0;

   for (index = 0; index < ARMOS_EGL_SWAPCHAIN_IMAGES; ++index) {
      if (!armos_egl_image_create(surface, &surface->images[index],
                                  width, height)) {
         while (index > 0u)
            armos_egl_image_reset(&surface->images[--index]);
         free(surface);
         _eglError(EGL_BAD_ALLOC, "ArmOS Wayland EGL swapchain");
         return NULL;
      }
   }
   surface->current_image = 0u;
   surface->armgl = surface->images[0].armgl;
   armos_egl_display_reference(owner);
   return &surface->base;
}

static EGLBoolean
armos_egl_destroy_surface(_EGLDisplay *display, _EGLSurface *base)
{
   (void)display;

   if (_eglPutSurface(base)) {
      struct armos_egl_surface *surface = armos_egl_surface(base);
      if (surface->base.Type == EGL_WINDOW_BIT) {
         unsigned index;

         for (index = 0; index < ARMOS_EGL_SWAPCHAIN_IMAGES; ++index)
            armos_egl_image_reset(&surface->images[index]);
      } else {
         armgl_surface_destroy(surface->armgl);
      }
      armos_egl_display_release(surface->owner);
      free(surface);
   }
   return EGL_TRUE;
}

static EGLBoolean
armos_egl_swap_buffers(_EGLDisplay *display, _EGLSurface *base)
{
   struct armos_egl_surface *surface = armos_egl_surface(base);
   struct armos_egl_context *context =
      armos_egl_context(_eglGetCurrentContext());
   struct armos_egl_surface *read_surface;
   struct armos_egl_image *current;
   struct armos_egl_image *next;
   uint32_t target_width;
   uint32_t target_height;
   unsigned next_index;
   int acquire_fence_fd = -1;

   (void)display;
   if (!surface || surface->base.Type != EGL_WINDOW_BIT || !context ||
       context->owner != surface->owner ||
       context->base.DrawSurface != base ||
       armos_wl_egl_window_get_size(surface->native_window,
                                    &target_width, &target_height) < 0)
      return _eglError(EGL_BAD_SURFACE, "ArmOS EGL swap surface");

   current = &surface->images[surface->current_image];
   next = armos_egl_acquire_image(surface, surface->current_image,
                                  target_width, target_height);
   if (!next)
      return _eglError(EGL_BAD_ALLOC, "ArmOS EGL acquire swap image");
   next_index = (unsigned)(next - surface->images);

   if (!armgl_flush_fence_fd(context->armgl, &acquire_fence_fd) &&
       !armgl_flush(context->armgl, true))
      return _eglError(EGL_CONTEXT_LOST, "ArmOS EGL frame flush");
   if (!armos_egl_image_create_buffer(surface, current, context)) {
      if (acquire_fence_fd >= 0)
         close(acquire_fence_fd);
      return _eglError(EGL_BAD_ALLOC, "ArmOS EGL export swap image");
   }

   read_surface = armos_egl_surface(context->base.ReadSurface);
   if (!read_surface) {
      if (acquire_fence_fd >= 0)
         close(acquire_fence_fd);
      return _eglError(EGL_BAD_MATCH, "ArmOS EGL read surface");
   }
   if (!armgl_make_current(context->armgl, next->armgl,
                           read_surface == surface ? next->armgl :
                           read_surface->armgl)) {
      if (acquire_fence_fd >= 0)
         close(acquire_fence_fd);
      return _eglError(EGL_BAD_MATCH, "ArmOS EGL rotate swapchain");
   }
   if (acquire_fence_fd >= 0) {
      if (armos_gpu_buffer_manager_v1_set_acquire_fence(
             surface->owner->gpu_manager, surface->wayland_surface,
             acquire_fence_fd) < 0) {
         close(acquire_fence_fd);
         (void)armgl_make_current(context->armgl, current->armgl,
                                  read_surface == surface ? current->armgl :
                                  read_surface->armgl);
         return _eglError(EGL_BAD_ACCESS, "ArmOS EGL acquire fence");
      }
      close(acquire_fence_fd);
   }

   wl_surface_attach(surface->wayland_surface, current->buffer, 0, 0);
   wl_surface_damage_buffer(surface->wayland_surface, 0, 0,
                            (int32_t)current->width,
                            (int32_t)current->height);
   current->busy = true;
   wl_surface_commit(surface->wayland_surface);

   surface->current_image = next_index;
   surface->armgl = next->armgl;
   surface->base.Width = (EGLint)target_width;
   surface->base.Height = (EGLint)target_height;
   return EGL_TRUE;
}

static _EGLContext *
armos_egl_create_context(_EGLDisplay *display, _EGLConfig *config,
                         _EGLContext *shared_base,
                         const EGLint *attributes)
{
   struct armos_egl_context *context;
   struct armos_egl_context *shared = armos_egl_context(shared_base);
   struct armgl_context_config armgl_config;

   context = calloc(1, sizeof(*context));
   if (!context)
      return NULL;

   if (!_eglInitContext(&context->base, display, config,
                        shared_base, attributes)) {
      free(context);
      return NULL;
   }

   if (context->base.ClientAPI != EGL_OPENGL_ES_API ||
       context->base.ClientMajorVersion != 2) {
      free(context);
      _eglError(EGL_BAD_MATCH, "ArmOS EGL currently exposes OpenGL ES 2");
      return NULL;
   }

   armgl_config.major = context->base.ClientMajorVersion;
   armgl_config.minor = context->base.ClientMinorVersion;
   armgl_config.alpha = !config || config->AlphaSize != 0;
   armgl_config.depth_bits = config ? config->DepthSize : 0;
   armgl_config.stencil_bits = config ? config->StencilSize : 0;
   armgl_config.double_buffered = false;
   armgl_config.debug = (context->base.Flags & EGL_CONTEXT_OPENGL_DEBUG_BIT_KHR) != 0;
   armgl_config.no_error = context->base.NoError;

   context->armgl = armgl_context_create(
      armos_egl_display(display)->armgl, &armgl_config,
      shared ? shared->armgl : NULL);
   if (!context->armgl) {
      free(context);
      _eglError(EGL_BAD_ALLOC, "ArmOS GLES context");
      return NULL;
   }

   context->owner = armos_egl_display(display);
   armos_egl_display_reference(context->owner);

   return &context->base;
}

static EGLBoolean
armos_egl_destroy_context(_EGLDisplay *display, _EGLContext *base)
{
   (void)display;

   if (_eglPutContext(base)) {
      struct armos_egl_context *context = armos_egl_context(base);
      armgl_context_destroy(context->armgl);
      armos_egl_display_release(context->owner);
      free(context);
   }
   return EGL_TRUE;
}

static EGLBoolean
armos_egl_make_current(_EGLDisplay *display,
                       _EGLSurface *draw_base,
                       _EGLSurface *read_base,
                       _EGLContext *context_base)
{
   struct armos_egl_context *context = armos_egl_context(context_base);
   struct armos_egl_surface *draw = armos_egl_surface(draw_base);
   struct armos_egl_surface *read = armos_egl_surface(read_base);
   _EGLContext *old_context;
   _EGLSurface *old_draw;
   _EGLSurface *old_read;
   bool bound;

   if (!_eglBindContext(context_base, draw_base, read_base,
                        &old_context, &old_draw, &old_read))
      return EGL_FALSE;

   bound = armgl_make_current(context ? context->armgl : NULL,
                              draw ? draw->armgl : NULL,
                              read ? read->armgl : NULL);

   if (!bound) {
      _EGLContext *failed_context;
      _EGLSurface *failed_draw;
      _EGLSurface *failed_read;
      struct armos_egl_context *previous = armos_egl_context(old_context);
      struct armos_egl_surface *previous_draw = armos_egl_surface(old_draw);
      struct armos_egl_surface *previous_read = armos_egl_surface(old_read);
      bool restored;

      /* Restore both halves of the binding.  Leaving EGL bound to the new
       * context after the Gallium bind failed would make the next API call
       * target a different context than Mesa's state tracker.
       */
      if (!_eglBindContext(old_context, old_draw, old_read,
                           &failed_context, &failed_draw, &failed_read)) {
         armgl_make_current(NULL, NULL, NULL);
         if (_eglBindContext(NULL, NULL, NULL,
                             &failed_context, &failed_draw, &failed_read)) {
            armos_egl_destroy_surface(display, failed_draw);
            armos_egl_destroy_surface(display, failed_read);
            armos_egl_destroy_context(display, failed_context);
         }
         armos_egl_destroy_surface(display, old_draw);
         armos_egl_destroy_surface(display, old_read);
         armos_egl_destroy_context(display, old_context);
         return _eglError(EGL_BAD_MATCH, "ArmOS EGL binding rollback");
      }

      restored = armgl_make_current(previous ? previous->armgl : NULL,
                                    previous_draw ? previous_draw->armgl : NULL,
                                    previous_read ? previous_read->armgl : NULL);

      armos_egl_destroy_surface(display, failed_draw);
      armos_egl_destroy_surface(display, failed_read);
      armos_egl_destroy_context(display, failed_context);

      if (!restored) {
         _EGLContext *lost_context;
         _EGLSurface *lost_draw;
         _EGLSurface *lost_read;

         /* If even the previous Gallium binding cannot be restored, expose
          * no current EGL context rather than retaining inconsistent state.
          */
         if (_eglBindContext(NULL, NULL, NULL,
                             &lost_context, &lost_draw, &lost_read)) {
            armgl_make_current(NULL, NULL, NULL);
            armos_egl_destroy_surface(display, lost_draw);
            armos_egl_destroy_surface(display, lost_read);
            armos_egl_destroy_context(display, lost_context);
         }
      }

      armos_egl_destroy_surface(display, old_draw);
      armos_egl_destroy_surface(display, old_read);
      armos_egl_destroy_context(display, old_context);
      return _eglError(EGL_BAD_MATCH, "ArmOS EGL make current");
   }

   if (old_draw)
      armos_egl_destroy_surface(display, old_draw);
   if (old_read)
      armos_egl_destroy_surface(display, old_read);
   if (old_context)
      armos_egl_destroy_context(display, old_context);

   return EGL_TRUE;
}

static EGLBoolean
armos_egl_wait_client(_EGLDisplay *display, _EGLContext *base)
{
   struct armos_egl_context *context = armos_egl_context(base);

   (void)display;
   return context && armgl_flush(context->armgl, true);
}

static const char *
armos_egl_query_driver_name(_EGLDisplay *display)
{
   (void)display;
   return "armos";
}

const _EGLDriver _eglDriver = {
   .Initialize = armos_egl_initialize,
   .Terminate = armos_egl_terminate,
   .CreateContext = armos_egl_create_context,
   .DestroyContext = armos_egl_destroy_context,
   .MakeCurrent = armos_egl_make_current,
   .CreateWindowSurface = armos_egl_create_window,
   .CreatePbufferSurface = armos_egl_create_pbuffer,
   .DestroySurface = armos_egl_destroy_surface,
   .SwapBuffers = armos_egl_swap_buffers,
   .WaitClient = armos_egl_wait_client,
   .QueryDriverName = armos_egl_query_driver_name,
};
