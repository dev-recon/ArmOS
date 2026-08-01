/*
 * ArmOS
 * Copyright (c) 2026 Mohamed Ennassiri
 *
 * Licensed under the Apache License, Version 2.0.
 * See LICENSE for details.
 *
 * File: userland/opt/mesa/armos/virgl_armos_winsys.c
 * Layer: Third-party port / Mesa Gallium VirGL winsys
 *
 * Responsibilities:
 * - Translate Mesa VirGL resources, transfers, submissions and fences to the
 *   architecture-neutral ArmOS DRM userspace library.
 * - Track command-buffer resource references until asynchronous submission.
 * - Preserve explicit resource and fence lifetimes without Linux DRM ioctls.
 *
 * Notes:
 * - This file is copied into the selected Mesa source tree by the bundle build.
 * - Window-system buffer exchange belongs to the later EGL/Wayland layer.
 * - VirtIO transport values enter only the negotiated VirGL descriptor.
 */

#include "virgl_armos_winsys.h"

#include <armos/virgl_winsys.h>
#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "pipe/p_state.h"
#include "frontend/winsys_handle.h"
#include "util/os_time.h"
#include "util/u_atomic.h"
#include "util/u_inlines.h"
#include "virgl/virgl_public.h"
#include "virgl/virgl_winsys.h"
#include "virtio-gpu/virgl_hw.h"

struct virgl_armos_fence {
   struct pipe_reference reference;
   armos_virgl_device_t *device;
   uint64_t id;
};

struct virgl_hw_res {
   struct pipe_reference reference;
   armos_virgl_buffer_t buffer;
   struct virgl_armos_fence *last_fence;
   int num_cs_references;
   uint32_t format;
   uint32_t bind;
   uint32_t size;
};

struct virgl_armos_winsys {
   struct virgl_winsys base;
   armos_virgl_device_t device;
   uint32_t context_id;
   unsigned live_resources;
};

struct virgl_armos_cmd_buf {
   struct virgl_cmd_buf base;
   struct virgl_armos_winsys *winsys;
   struct virgl_hw_res **resources;
   unsigned resource_count;
   unsigned resource_capacity;
   unsigned dword_capacity;
};

static struct virgl_armos_winsys *armos_winsys(struct virgl_winsys *winsys)
{
   return (struct virgl_armos_winsys *)winsys;
}

static struct virgl_armos_cmd_buf *armos_cmd_buf(struct virgl_cmd_buf *buffer)
{
   return (struct virgl_armos_cmd_buf *)buffer;
}

static struct virgl_armos_fence *armos_fence(struct pipe_fence_handle *fence)
{
   return (struct virgl_armos_fence *)fence;
}

static void armos_fence_release(struct virgl_armos_fence *fence)
{
   if (!fence)
      return;
   (void)armos_virgl_fence_destroy(fence->device, fence->id);
   free(fence);
}

static void armos_fence_reference_internal(struct virgl_armos_fence **dst,
                                           struct virgl_armos_fence *src)
{
   struct virgl_armos_fence *old = dst ? *dst : NULL;

   if (pipe_reference(old ? &old->reference : NULL,
                      src ? &src->reference : NULL))
      armos_fence_release(old);
   if (dst)
      *dst = src;
}

static struct virgl_armos_fence *armos_fence_create(
   struct virgl_armos_winsys *winsys, uint64_t id)
{
   struct virgl_armos_fence *fence = calloc(1, sizeof(*fence));

   if (!fence)
      return NULL;
   pipe_reference_init(&fence->reference, 1);
   fence->device = &winsys->device;
   fence->id = id;
   return fence;
}

static bool armos_fence_wait_internal(struct virgl_armos_fence *fence,
                                      uint64_t timeout)
{
   int64_t timeout_ns;

   if (!fence)
      return true;
   timeout_ns = timeout == OS_TIMEOUT_INFINITE ? -1 :
                timeout > INT64_MAX ? INT64_MAX : (int64_t)timeout;
   return armos_virgl_fence_wait(fence->device, fence->id, timeout_ns) == 0;
}

static uint32_t armos_bo_flags(uint32_t bind)
{
   uint32_t flags = ARMOS_DRM_BO_CPU_READ | ARMOS_DRM_BO_CPU_WRITE;

   if (bind & (VIRGL_BIND_RENDER_TARGET | VIRGL_BIND_DEPTH_STENCIL))
      flags |= ARMOS_DRM_BO_RENDER_TARGET;
   if (bind & VIRGL_BIND_SAMPLER_VIEW)
      flags |= ARMOS_DRM_BO_TEXTURE;
   if (bind & VIRGL_BIND_VERTEX_BUFFER)
      flags |= ARMOS_DRM_BO_VERTEX;
   if (bind & VIRGL_BIND_INDEX_BUFFER)
      flags |= ARMOS_DRM_BO_INDEX;
   if (bind & VIRGL_BIND_CONSTANT_BUFFER)
      flags |= ARMOS_DRM_BO_CONSTANT;
   if (bind & VIRGL_BIND_SHADER_BUFFER)
      flags |= ARMOS_DRM_BO_SHADER_STORAGE;
   if (bind & VIRGL_BIND_SCANOUT)
      flags |= ARMOS_DRM_BO_SCANOUT;
   if ((flags & ~(ARMOS_DRM_BO_CPU_READ | ARMOS_DRM_BO_CPU_WRITE)) == 0)
      flags |= ARMOS_DRM_BO_COMMAND;
   return flags;
}

static uint32_t armos_drm_format(enum pipe_format format)
{
   if (format == PIPE_FORMAT_BGRA8888_UNORM ||
       format == PIPE_FORMAT_BGRX8888_UNORM)
      return ARMOS_DRM_FORMAT_BGRA8888;
   return 0;
}

static int armos_transfer(struct virgl_winsys *base,
                          struct virgl_hw_res *resource,
                          const struct pipe_box *box,
                          uint32_t stride, uint32_t layer_stride,
                          uint32_t offset, uint32_t level,
                          uint32_t direction)
{
   struct virgl_armos_winsys *winsys = armos_winsys(base);

   if (!resource || !box || box->x < 0 || box->y < 0 || box->z < 0 ||
       box->width <= 0 || box->height <= 0 || box->depth <= 0) {
      errno = EINVAL;
      return -1;
   }
   return armos_virgl_buffer_transfer(
      &winsys->device, winsys->context_id, &resource->buffer, direction,
      level, (uint32_t)box->x, (uint32_t)box->y, (uint32_t)box->z,
      (uint32_t)box->width, (uint32_t)box->height, (uint32_t)box->depth,
      offset, stride, layer_stride);
}

static int armos_transfer_put(struct virgl_winsys *winsys,
                              struct virgl_hw_res *resource,
                              const struct pipe_box *box,
                              uint32_t stride, uint32_t layer_stride,
                              uint32_t offset, uint32_t level)
{
   return armos_transfer(winsys, resource, box, stride, layer_stride,
                         offset, level, ARMOS_DRM_TRANSFER_CPU_TO_DEVICE);
}

static int armos_transfer_get(struct virgl_winsys *winsys,
                              struct virgl_hw_res *resource,
                              const struct pipe_box *box,
                              uint32_t stride, uint32_t layer_stride,
                              uint32_t offset, uint32_t level)
{
   return armos_transfer(winsys, resource, box, stride, layer_stride,
                         offset, level, ARMOS_DRM_TRANSFER_DEVICE_TO_CPU);
}

static void armos_resource_destroy(struct virgl_armos_winsys *winsys,
                                   struct virgl_hw_res *resource)
{
   if (!resource)
      return;
   armos_fence_reference_internal(&resource->last_fence, NULL);
   (void)armos_virgl_buffer_detach(&winsys->device, winsys->context_id,
                                   &resource->buffer);
   (void)armos_virgl_buffer_destroy(&winsys->device, &resource->buffer);
   if (winsys->live_resources > 0)
      winsys->live_resources--;
   free(resource);
}

static void armos_resource_reference(struct virgl_winsys *base,
                                     struct virgl_hw_res **dst,
                                     struct virgl_hw_res *src)
{
   struct virgl_hw_res *old = dst ? *dst : NULL;

   if (pipe_reference(old ? &old->reference : NULL,
                      src ? &src->reference : NULL))
      armos_resource_destroy(armos_winsys(base), old);
   if (dst)
      *dst = src;
}

static struct virgl_hw_res *armos_resource_create(
   struct virgl_winsys *base, enum pipe_texture_target target,
   const void *map_front_private, uint32_t format, uint32_t bind,
   uint32_t width, uint32_t height, uint32_t depth, uint32_t array_size,
   uint32_t last_level, uint32_t nr_samples, uint32_t flags, uint32_t size)
{
   struct virgl_armos_winsys *winsys = armos_winsys(base);
   struct virgl_hw_res *resource;
   armos_drm_virgl_resource_descriptor_t descriptor;

   (void)map_front_private;
   if (size == 0 || width == 0 || height == 0 || depth == 0 ||
       array_size == 0)
      return NULL;
   resource = calloc(1, sizeof(*resource));
   if (!resource)
      return NULL;
   memset(&descriptor, 0, sizeof(descriptor));
   descriptor.abi_version = ARMOS_DRM_VIRGL_RESOURCE_ABI_VERSION;
   descriptor.struct_size = sizeof(descriptor);
   descriptor.target = target;
   descriptor.format = pipe_to_virgl_format(format);
   descriptor.bind = bind;
   descriptor.width = width;
   descriptor.height = height;
   descriptor.depth = depth;
   descriptor.array_size = array_size;
   descriptor.last_level = last_level;
   descriptor.nr_samples = nr_samples;
   descriptor.flags = flags;
   if (armos_virgl_resource_create(&winsys->device, &resource->buffer, size,
                                   armos_bo_flags(bind), &descriptor) < 0 ||
       armos_virgl_buffer_attach(&winsys->device, winsys->context_id,
                                 &resource->buffer) < 0) {
      if (resource->buffer.handle != 0)
         (void)armos_virgl_buffer_destroy(&winsys->device, &resource->buffer);
      free(resource);
      return NULL;
   }
   pipe_reference_init(&resource->reference, 1);
   resource->format = format;
   resource->bind = bind;
   resource->size = size;
   winsys->live_resources++;
   return resource;
}

static struct virgl_hw_res *armos_resource_create_from_handle(
   struct virgl_winsys *base, struct winsys_handle *handle,
   struct pipe_resource *templ, uint32_t *plane, uint32_t *stride,
   uint32_t *plane_offset, uint64_t *modifier, uint32_t *blob_mem)
{
   struct virgl_armos_winsys *winsys = armos_winsys(base);
   struct virgl_hw_res *resource;
   uint32_t expected_format;
   uint32_t required_flags;
   uint64_t required_size;

   if (!handle || !templ || handle->type != WINSYS_HANDLE_TYPE_FD ||
       (int)handle->handle < 0)
      return NULL;
   resource = calloc(1, sizeof(*resource));
   if (!resource)
      return NULL;
   if (armos_virgl_buffer_import(&winsys->device, &resource->buffer,
                                 (int)handle->handle) < 0)
      goto fail;

   expected_format = armos_drm_format(templ->format);
   required_flags = armos_bo_flags(templ->bind) &
      ~(ARMOS_DRM_BO_CPU_READ | ARMOS_DRM_BO_CPU_WRITE |
        ARMOS_DRM_BO_COMMAND);
   required_size = (uint64_t)resource->buffer.stride * templ->height0;
   if (!expected_format || !templ->width0 || !templ->height0 ||
       templ->width0 > UINT32_MAX / 4u ||
       resource->buffer.width != templ->width0 ||
       resource->buffer.height != templ->height0 ||
       resource->buffer.stride < templ->width0 * 4u ||
       (handle->stride && resource->buffer.stride != handle->stride) ||
       resource->buffer.format != expected_format ||
       (resource->buffer.flags & required_flags) != required_flags ||
       required_size > resource->buffer.size ||
       armos_virgl_buffer_attach(&winsys->device, winsys->context_id,
                                 &resource->buffer) < 0)
      goto fail;
   pipe_reference_init(&resource->reference, 1);
   resource->format = templ->format;
   resource->bind = templ->bind;
   resource->size = resource->buffer.size > UINT32_MAX ?
                    UINT32_MAX : (uint32_t)resource->buffer.size;
   winsys->live_resources++;
   if (plane)
      *plane = 0;
   if (stride)
      *stride = resource->buffer.stride;
   if (plane_offset)
      *plane_offset = 0;
   if (modifier)
      *modifier = 0;
   if (blob_mem)
      *blob_mem = 0;
   return resource;

fail:
   if (resource->buffer.handle != 0)
      (void)armos_virgl_buffer_destroy(&winsys->device,
                                       &resource->buffer);
   free(resource);
   return NULL;
}

static bool armos_resource_get_handle(struct virgl_winsys *base,
                                      struct virgl_hw_res *resource,
                                      uint32_t stride,
                                      struct winsys_handle *handle)
{
   struct virgl_armos_winsys *winsys = armos_winsys(base);
   uint32_t drm_format;
   int fd;

   if (!resource || !handle || handle->type != WINSYS_HANDLE_TYPE_FD)
      return false;
   drm_format = armos_drm_format(resource->format);
   if (!drm_format)
      return false;
   if (!stride ||
       armos_virgl_buffer_set_metadata(
          &winsys->device, &resource->buffer,
          resource->buffer.width, resource->buffer.height,
          stride, drm_format) < 0)
      return false;
   fd = armos_virgl_buffer_export(&winsys->device, &resource->buffer, 1);
   if (fd < 0)
      return false;
   handle->handle = (unsigned)fd;
   handle->stride = stride ? stride : resource->buffer.stride;
   handle->offset = 0;
   handle->modifier = 0;
   handle->size = resource->buffer.size;
   return true;
}

static void *armos_resource_map(struct virgl_winsys *base,
                                struct virgl_hw_res *resource)
{
   struct virgl_armos_winsys *winsys = armos_winsys(base);

   if (!resource)
      return NULL;
   if (resource->last_fence &&
       !armos_fence_wait_internal(resource->last_fence,
                                  OS_TIMEOUT_INFINITE))
      return NULL;
   armos_fence_reference_internal(&resource->last_fence, NULL);
   if (!resource->buffer.mapping &&
       armos_virgl_buffer_map(&winsys->device, &resource->buffer) < 0)
      return NULL;
   return resource->buffer.mapping;
}

static void armos_resource_wait(struct virgl_winsys *base,
                                struct virgl_hw_res *resource)
{
   (void)base;
   if (resource && armos_fence_wait_internal(resource->last_fence,
                                              OS_TIMEOUT_INFINITE))
      armos_fence_reference_internal(&resource->last_fence, NULL);
}

static bool armos_resource_is_busy(struct virgl_winsys *base,
                                   struct virgl_hw_res *resource)
{
   (void)base;
   if (!resource || !resource->last_fence)
      return false;
   if (!armos_fence_wait_internal(resource->last_fence, 0))
      return true;
   armos_fence_reference_internal(&resource->last_fence, NULL);
   return false;
}

static struct virgl_cmd_buf *armos_cmd_buf_create(struct virgl_winsys *base,
                                                   uint32_t dwords)
{
   struct virgl_armos_cmd_buf *buffer;

   if (dwords == 0 || dwords > VIRGL_MAX_CMDBUF_DWORDS)
      return NULL;
   buffer = calloc(1, sizeof(*buffer));
   if (!buffer)
      return NULL;
   buffer->base.buf = calloc(dwords, sizeof(uint32_t));
   buffer->resource_capacity = 64;
   buffer->resources = calloc(buffer->resource_capacity,
                              sizeof(*buffer->resources));
   if (!buffer->base.buf || !buffer->resources) {
      free(buffer->resources);
      free(buffer->base.buf);
      free(buffer);
      return NULL;
   }
   buffer->winsys = armos_winsys(base);
   buffer->dword_capacity = dwords;
   return &buffer->base;
}

static void armos_cmd_buf_release_resources(struct virgl_armos_cmd_buf *buffer)
{
   unsigned index;

   for (index = 0; index < buffer->resource_count; index++) {
      p_atomic_dec(&buffer->resources[index]->num_cs_references);
      armos_resource_reference(&buffer->winsys->base,
                               &buffer->resources[index], NULL);
   }
   buffer->resource_count = 0;
}

static void armos_cmd_buf_destroy(struct virgl_cmd_buf *base)
{
   struct virgl_armos_cmd_buf *buffer = armos_cmd_buf(base);

   if (!buffer)
      return;
   armos_cmd_buf_release_resources(buffer);
   free(buffer->resources);
   free(buffer->base.buf);
   free(buffer);
}

static bool armos_cmd_buf_has_resource(struct virgl_armos_cmd_buf *buffer,
                                       struct virgl_hw_res *resource)
{
   unsigned index;

   for (index = 0; index < buffer->resource_count; index++) {
      if (buffer->resources[index] == resource)
         return true;
   }
   return false;
}

static bool armos_cmd_buf_add_resource(struct virgl_armos_cmd_buf *buffer,
                                       struct virgl_hw_res *resource)
{
   struct virgl_hw_res **resources;
   unsigned capacity;

   if (armos_cmd_buf_has_resource(buffer, resource))
      return true;
   if (buffer->resource_count == buffer->resource_capacity) {
      capacity = buffer->resource_capacity * 2;
      resources = realloc(buffer->resources, capacity * sizeof(*resources));
      if (!resources)
         return false;
      memset(resources + buffer->resource_capacity, 0,
             (capacity - buffer->resource_capacity) * sizeof(*resources));
      buffer->resources = resources;
      buffer->resource_capacity = capacity;
   }
   buffer->resources[buffer->resource_count] = NULL;
   armos_resource_reference(&buffer->winsys->base,
                            &buffer->resources[buffer->resource_count],
                            resource);
   p_atomic_inc(&resource->num_cs_references);
   buffer->resource_count++;
   return true;
}

static void armos_emit_resource(struct virgl_winsys *base,
                                struct virgl_cmd_buf *cmd,
                                struct virgl_hw_res *resource,
                                bool write_handle)
{
   struct virgl_armos_cmd_buf *buffer = armos_cmd_buf(cmd);

   (void)base;
   if (!buffer || !resource ||
       (write_handle && buffer->base.cdw >= buffer->dword_capacity) ||
       !armos_cmd_buf_add_resource(buffer, resource))
      return;
   if (write_handle)
      buffer->base.buf[buffer->base.cdw++] = resource->buffer.command_handle;
}

static bool armos_resource_is_referenced(struct virgl_winsys *base,
                                         struct virgl_cmd_buf *cmd,
                                         struct virgl_hw_res *resource)
{
   (void)base;
   return cmd && resource &&
          armos_cmd_buf_has_resource(armos_cmd_buf(cmd), resource);
}

static int armos_submit_cmd(struct virgl_winsys *base,
                            struct virgl_cmd_buf *cmd,
                            struct pipe_fence_handle **out_fence)
{
   struct virgl_armos_winsys *winsys = armos_winsys(base);
   struct virgl_armos_cmd_buf *buffer = armos_cmd_buf(cmd);
   struct virgl_armos_fence *fence;
   uint64_t fence_id;
   unsigned index;
   int result;

   if (!buffer || buffer->base.cdw == 0)
      return 0;
   result = armos_virgl_submit(&winsys->device, winsys->context_id,
                               buffer->base.buf,
                               buffer->base.cdw * sizeof(uint32_t), &fence_id);
   if (result < 0)
      goto done;
   fence = armos_fence_create(winsys, fence_id);
   if (!fence) {
      (void)armos_virgl_fence_wait(&winsys->device, fence_id, -1);
      (void)armos_virgl_fence_destroy(&winsys->device, fence_id);
      result = -1;
      errno = ENOMEM;
      goto done;
   }
   for (index = 0; index < buffer->resource_count; index++)
      armos_fence_reference_internal(&buffer->resources[index]->last_fence,
                                     fence);
   if (out_fence)
      *out_fence = (struct pipe_fence_handle *)fence;
   else
      armos_fence_reference_internal(&fence, NULL);

done:
   armos_cmd_buf_release_resources(buffer);
   buffer->base.cdw = 0;
   return result;
}

static int armos_get_caps(struct virgl_winsys *base,
                          struct virgl_drm_caps *caps)
{
   struct virgl_armos_winsys *winsys = armos_winsys(base);
   size_t size;

   if (!caps || !winsys->device.command_caps)
      return -1;
   memset(caps, 0, sizeof(*caps));
   virgl_ws_fill_new_caps_defaults(caps);
   size = winsys->device.command_caps_size;
   if (size > sizeof(caps->caps))
      size = sizeof(caps->caps);
   memcpy(&caps->caps, winsys->device.command_caps, size);

   /*
    * Capsets describe the host renderer, while Gallium consumes the
    * intersection of host and guest-winsys capabilities.  ArmOS currently
    * implements the classic transfer_put/transfer_get callbacks, not VirGL's
    * command-buffer encoded transfer protocol.  Advertising the latter would
    * make Mesa select its copy-transfer staging path without initializing the
    * staging manager (supports_encoded_transfers remains false).
    *
    * Keep this filtering at the winsys boundary: neither the common DRM ABI
    * nor the VirtIO-GPU backend should pretend that a userspace Mesa adapter
    * implements commands which it does not yet encode.
    */
   if (!base->supports_encoded_transfers) {
      caps->caps.v2.capability_bits &=
         ~(VIRGL_CAP_TRANSFER | VIRGL_CAP_COPY_TRANSFER);
      caps->caps.v2.capability_bits_v2 &=
         ~VIRGL_CAP_V2_COPY_TRANSFER_BOTH_DIRECTIONS;
   }
   return 0;
}

static bool armos_fence_wait(struct virgl_winsys *base,
                             struct pipe_fence_handle *fence,
                             uint64_t timeout)
{
   (void)base;
   return armos_fence_wait_internal(armos_fence(fence), timeout);
}

static void armos_fence_reference(struct virgl_winsys *base,
                                  struct pipe_fence_handle **dst,
                                  struct pipe_fence_handle *src)
{
   (void)base;
   armos_fence_reference_internal((struct virgl_armos_fence **)dst,
                                  armos_fence(src));
}

static void armos_fence_server_sync(struct virgl_winsys *base,
                                    struct virgl_cmd_buf *cmd,
                                    struct pipe_fence_handle *fence)
{
   (void)base;
   (void)cmd;
   (void)armos_fence_wait_internal(armos_fence(fence), OS_TIMEOUT_INFINITE);
}

static int armos_fence_get_fd(struct virgl_winsys *base,
                              struct pipe_fence_handle *fence)
{
   struct virgl_armos_fence *armos = armos_fence(fence);

   (void)base;
   if (!armos) {
      errno = EINVAL;
      return -1;
   }
   return armos_virgl_fence_export(armos->device, armos->id, 1);
}

static int armos_get_fd(struct virgl_winsys *base)
{
   return armos_winsys(base)->device.fd;
}

static uint32_t armos_resource_get_storage_size(struct virgl_winsys *base,
                                                struct virgl_hw_res *resource)
{
   (void)base;
   return resource ? resource->size : 0;
}

static void armos_flush_frontbuffer(struct virgl_winsys *base,
                                    struct virgl_cmd_buf *cmd,
                                    struct virgl_hw_res *resource,
                                    unsigned level, unsigned layer,
                                    void *drawable, struct pipe_box *box)
{
   (void)base;
   (void)cmd;
   (void)resource;
   (void)level;
   (void)layer;
   (void)drawable;
   (void)box;
}

static void armos_winsys_destroy(struct virgl_winsys *base)
{
   struct virgl_armos_winsys *winsys = armos_winsys(base);

   if (!winsys)
      return;
   (void)armos_virgl_context_destroy(&winsys->device, winsys->context_id);
   armos_virgl_close(&winsys->device);
   free(winsys);
}

struct virgl_winsys *virgl_armos_winsys_create(const char *render_node)
{
   struct virgl_armos_winsys *winsys = calloc(1, sizeof(*winsys));

   if (!winsys)
      return NULL;
   if (armos_virgl_open(&winsys->device, render_node) < 0 ||
       armos_virgl_context_create(&winsys->device, &winsys->context_id) < 0) {
      armos_virgl_close(&winsys->device);
      free(winsys);
      return NULL;
   }
   winsys->base.destroy = armos_winsys_destroy;
   winsys->base.get_fd = armos_get_fd;
   winsys->base.transfer_put = armos_transfer_put;
   winsys->base.transfer_get = armos_transfer_get;
   winsys->base.resource_create = armos_resource_create;
   winsys->base.resource_create_from_handle =
      armos_resource_create_from_handle;
   winsys->base.resource_get_handle = armos_resource_get_handle;
   winsys->base.resource_reference = armos_resource_reference;
   winsys->base.resource_map = armos_resource_map;
   winsys->base.resource_wait = armos_resource_wait;
   winsys->base.resource_is_busy = armos_resource_is_busy;
   winsys->base.resource_get_storage_size = armos_resource_get_storage_size;
   winsys->base.cmd_buf_create = armos_cmd_buf_create;
   winsys->base.cmd_buf_destroy = armos_cmd_buf_destroy;
   winsys->base.emit_res = armos_emit_resource;
   winsys->base.submit_cmd = armos_submit_cmd;
   winsys->base.res_is_referenced = armos_resource_is_referenced;
   winsys->base.get_caps = armos_get_caps;
   winsys->base.fence_wait = armos_fence_wait;
   winsys->base.fence_reference = armos_fence_reference;
   winsys->base.fence_server_sync = armos_fence_server_sync;
   winsys->base.fence_get_fd = armos_fence_get_fd;
   winsys->base.flush_frontbuffer = armos_flush_frontbuffer;
   /*
    * fence_get_fd() supports the outgoing half of the contract.  Gallium's
    * supports_fences flag covers both import and export, while ArmOS does not
    * expose FENCE_IMPORT yet.  Keep the aggregate capability disabled until
    * cs_create_fence() can consume an imported descriptor as well.
    */
   winsys->base.supports_fences = 0;
   winsys->base.supports_encoded_transfers = 0;
   winsys->base.supports_coherent = 0;
   return &winsys->base;
}

struct pipe_screen *virgl_armos_screen_create(
   const char *render_node, const struct pipe_screen_config *config)
{
   struct virgl_winsys *winsys = virgl_armos_winsys_create(render_node);
   struct pipe_screen *screen;

   if (!winsys)
      return NULL;
   screen = virgl_create_screen(winsys, config);
   if (!screen)
      winsys->destroy(winsys);
   return screen;
}
