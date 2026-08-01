/*
 * ArmOS
 * Copyright (c) 2026 Mohamed Ennassiri
 *
 * Licensed under the Apache License, Version 2.0.
 * See LICENSE for details.
 *
 * File: userland/opt/mesa/armos/pipe_loader_armos.c
 * Layer: Third-party port / Mesa Gallium target selection
 *
 * Responsibilities:
 * - Probe the architecture-neutral ArmOS DRM discovery ABI.
 * - Select a compiled Gallium provider from its negotiated command set.
 * - Validate the common rendering capabilities required by Mesa.
 *
 * Notes:
 * - VirtIO and native GPU transport details remain in their providers.
 * - This file is shared by surfaceless EGL and later Wayland EGL.
 */

#include "pipe_loader_armos.h"

#include <errno.h>
#include <fcntl.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <uapi/armos/drm.h>

#include "virgl_armos_winsys.h"

static int
armos_pipe_query(const char *render_node, armos_drm_info_t *info)
{
   int fd;
   int saved_errno;

   fd = open(render_node, O_RDWR, 0);
   if (fd < 0)
      return -1;

   memset(info, 0, sizeof(*info));
   if (ioctl(fd, ARMOS_DRM_IOCTL_GET_INFO, info) < 0) {
      saved_errno = errno;
      close(fd);
      errno = saved_errno;
      return -1;
   }
   close(fd);

   if (info->abi_version != ARMOS_DRM_ABI_VERSION ||
       info->struct_size < sizeof(*info)) {
      errno = EPROTO;
      return -1;
   }
   return 0;
}

struct pipe_screen *
armos_pipe_screen_create(const char *render_node)
{
   static const uint64_t required_caps =
      ARMOS_DRM_CAP_BUFFER_OBJECTS |
      ARMOS_DRM_CAP_CONTEXTS |
      ARMOS_DRM_CAP_COMMAND_SUBMIT |
      ARMOS_DRM_CAP_FENCES |
      ARMOS_DRM_CAP_RENDER_3D |
      ARMOS_DRM_CAP_RESOURCE_TRANSFER;
   armos_drm_info_t info;

   if (!render_node) {
      errno = EINVAL;
      return NULL;
   }
   if (armos_pipe_query(render_node, &info) < 0)
      return NULL;
   if ((info.capabilities & required_caps) != required_caps) {
      errno = ENOTSUP;
      return NULL;
   }

   if (strncmp((const char *)info.command_set, "virgl",
               sizeof("virgl") - 1u) == 0)
      return virgl_armos_screen_create(render_node, NULL);

   errno = ENOTSUP;
   return NULL;
}
