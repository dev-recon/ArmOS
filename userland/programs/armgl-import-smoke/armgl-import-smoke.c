/*
 * ArmOS
 * Copyright (c) 2026 Mohamed Ennassiri
 *
 * Licensed under the Apache License, Version 2.0.
 * See LICENSE for details.
 *
 * File: userland/programs/armgl-import-smoke/armgl-import-smoke.c
 * Layer: Userland / graphics diagnostics
 *
 * Responsibilities:
 * - Validate cross-context import of an exported GPU image.
 * - Validate image orientation, rectangle blits and alpha composition.
 * - Render through an imported image used as a color attachment.
 * - Verify the result through the architecture-neutral GLES frontend.
 *
 * Notes:
 * - This diagnostic uses Mesa internals only because it validates that layer.
 * - It contains no VirtIO, VirGL, VC4, V3D or platform-specific API.
 */

#include <GLES2/gl2.h>

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "armgl_frontend.h"
#include "pipe/p_screen.h"
#include "pipe_loader_armos.h"

#define IMAGE_WIDTH 64u
#define IMAGE_HEIGHT 64u

static bool
pixel_near(const uint8_t pixel[4], uint8_t red, uint8_t green,
           uint8_t blue, uint8_t alpha, uint8_t tolerance)
{
   return pixel[0] >= red - (red > tolerance ? tolerance : red) &&
      pixel[0] <= red + (red <= 255u - tolerance ? tolerance : 255u - red) &&
      pixel[1] >= green - (green > tolerance ? tolerance : green) &&
      pixel[1] <= green +
         (green <= 255u - tolerance ? tolerance : 255u - green) &&
      pixel[2] >= blue - (blue > tolerance ? tolerance : blue) &&
      pixel[2] <= blue +
         (blue <= 255u - tolerance ? tolerance : 255u - blue) &&
      pixel[3] >= alpha - (alpha > tolerance ? tolerance : alpha) &&
      pixel[3] <= alpha +
         (alpha <= 255u - tolerance ? tolerance : 255u - alpha);
}

static bool
read_pixel(uint32_t x, uint32_t y, uint8_t pixel[4])
{
   memset(pixel, 0, 4u);
   glReadPixels((GLint)x, (GLint)y, 1, 1, GL_RGBA,
                GL_UNSIGNED_BYTE, pixel);
   return glGetError() == GL_NO_ERROR;
}

struct smoke_endpoint {
   struct pipe_screen *screen;
   struct armgl_display *display;
   struct armgl_context *context;
   struct armgl_surface *surface;
};

static void
smoke_endpoint_destroy(struct smoke_endpoint *endpoint)
{
   if (!endpoint)
      return;
   armgl_context_destroy(endpoint->context);
   armgl_surface_destroy(endpoint->surface);
   armgl_display_destroy(endpoint->display);
   memset(endpoint, 0, sizeof(*endpoint));
}

static bool
smoke_endpoint_create(struct smoke_endpoint *endpoint, bool scanout)
{
   struct armgl_context_config context_config;
   struct armgl_surface_config surface_config;

   memset(endpoint, 0, sizeof(*endpoint));
   endpoint->screen = armos_pipe_screen_create("/dev/dri/renderD128");
   if (!endpoint->screen)
      return false;
   endpoint->display = armgl_display_create(endpoint->screen);
   if (!endpoint->display) {
      endpoint->screen->destroy(endpoint->screen);
      endpoint->screen = NULL;
      return false;
   }

   memset(&surface_config, 0, sizeof(surface_config));
   surface_config.width = IMAGE_WIDTH;
   surface_config.height = IMAGE_HEIGHT;
   surface_config.alpha = true;
   surface_config.exportable = true;
   surface_config.scanout = scanout;
   endpoint->surface = armgl_surface_create(endpoint->display,
                                             &surface_config);
   if (!endpoint->surface)
      goto fail;

   memset(&context_config, 0, sizeof(context_config));
   context_config.major = 2;
   context_config.alpha = true;
   endpoint->context = armgl_context_create(endpoint->display,
                                             &context_config, NULL);
   if (!endpoint->context ||
       !armgl_make_current(endpoint->context, endpoint->surface,
                           endpoint->surface))
      goto fail;
   return true;

fail:
   smoke_endpoint_destroy(endpoint);
   return false;
}

int
main(void)
{
   struct smoke_endpoint producer;
   struct smoke_endpoint consumer;
   struct armgl_image_config image_config;
   struct armgl_image_config upload_config;
   struct armgl_rect full_image;
   struct armgl_image *imported = NULL;
   struct armgl_image *uploaded = NULL;
   struct armgl_rect upload_source;
   struct armgl_rect upload_destination;
   struct armgl_rect upper_half;
   uint32_t upload_pixels[8u * 8u];
   uint8_t lower_pixel[4];
   uint8_t upper_pixel[4];
   uint32_t stride = 0;
   int image_fd = -1;
   int result = 1;

   memset(&producer, 0, sizeof(producer));
   memset(&consumer, 0, sizeof(consumer));
   if (!smoke_endpoint_create(&producer, true)) {
      fprintf(stderr, "armgl-import-smoke: producer setup failed\n");
      goto out;
   }

   glViewport(0, 0, IMAGE_WIDTH, IMAGE_HEIGHT);
   glClearColor(1.0f, 0.0f, 0.0f, 1.0f);
   glClear(GL_COLOR_BUFFER_BIT);
   glEnable(GL_SCISSOR_TEST);
   glScissor(0, IMAGE_HEIGHT / 2u, IMAGE_WIDTH, IMAGE_HEIGHT / 2u);
   glClearColor(0.0f, 0.0f, 1.0f, 0.5f);
   glClear(GL_COLOR_BUFFER_BIT);
   glDisable(GL_SCISSOR_TEST);
   if (glGetError() != GL_NO_ERROR ||
       !armgl_flush(producer.context, true) ||
       !armgl_surface_export_color_fd(producer.surface, producer.context,
                                      &image_fd, &stride)) {
      fprintf(stderr, "armgl-import-smoke: export failed\n");
      goto out;
   }

   if (!smoke_endpoint_create(&consumer, false)) {
      fprintf(stderr, "armgl-import-smoke: consumer setup failed\n");
      goto out;
   }
   memset(&image_config, 0, sizeof(image_config));
   image_config.width = IMAGE_WIDTH;
   image_config.height = IMAGE_HEIGHT;
   image_config.stride = stride;
   image_config.usage = ARMGL_IMAGE_USAGE_SAMPLED |
                        ARMGL_IMAGE_USAGE_RENDER_TARGET;
   image_config.alpha = true;
   imported = armgl_image_import_fd(consumer.display, &image_config,
                                    image_fd);
   close(image_fd);
   image_fd = -1;
   memset(&full_image, 0, sizeof(full_image));
   full_image.width = IMAGE_WIDTH;
   full_image.height = IMAGE_HEIGHT;
   if (!imported ||
       !armgl_surface_blit_image(consumer.context, consumer.surface,
                                 imported, &full_image, &full_image, false) ||
       !armgl_flush(consumer.context, true)) {
      fprintf(stderr, "armgl-import-smoke: imported source blit failed\n");
      goto out;
   }

   if (!read_pixel(IMAGE_WIDTH / 2u, IMAGE_HEIGHT / 4u, lower_pixel) ||
       !read_pixel(IMAGE_WIDTH / 2u, IMAGE_HEIGHT * 3u / 4u,
                   upper_pixel) ||
       !pixel_near(lower_pixel, 255u, 0u, 0u, 255u, 8u) ||
       !pixel_near(upper_pixel, 0u, 0u, 255u, 128u, 8u)) {
      fprintf(stderr,
              "armgl-import-smoke: orientation mismatch "
              "lower=%u,%u,%u,%u upper=%u,%u,%u,%u\n",
              lower_pixel[0], lower_pixel[1], lower_pixel[2], lower_pixel[3],
              upper_pixel[0], upper_pixel[1], upper_pixel[2], upper_pixel[3]);
      goto out;
   }

   if (!armgl_surface_fill_rect(consumer.context, consumer.surface,
                                &full_image, 0xffff00ffu) ||
       !armgl_flush(consumer.context, true) ||
       !read_pixel(IMAGE_WIDTH / 2u, IMAGE_HEIGHT / 2u, upper_pixel) ||
       !pixel_near(upper_pixel, 255u, 0u, 255u, 255u, 8u)) {
      fprintf(stderr,
              "armgl-import-smoke: fill mismatch %u,%u,%u,%u\n",
              upper_pixel[0], upper_pixel[1], upper_pixel[2], upper_pixel[3]);
      goto out;
   }

   memset(&upload_config, 0, sizeof(upload_config));
   upload_config.width = 8u;
   upload_config.height = 8u;
   upload_config.stride = 8u * sizeof(uint32_t);
   upload_config.usage = ARMGL_IMAGE_USAGE_SAMPLED;
   upload_config.alpha = true;
   uploaded = armgl_image_create(consumer.display, &upload_config);
   for (size_t index = 0u;
        index < sizeof(upload_pixels) / sizeof(upload_pixels[0]); ++index)
      upload_pixels[index] = 0xff00ff00u;
   memset(&upload_source, 0, sizeof(upload_source));
   upload_source.width = upload_config.width;
   upload_source.height = upload_config.height;
   upload_destination = upload_source;
   upload_destination.x = 10u;
   upload_destination.y = 12u;
   if (!uploaded ||
       !armgl_image_upload(consumer.context, uploaded, &upload_source,
                           upload_pixels, upload_config.stride) ||
       !armgl_surface_blit_image(consumer.context, consumer.surface,
                                 uploaded, &upload_source,
                                 &upload_destination, false) ||
       !armgl_flush(consumer.context, true) ||
       !read_pixel(upload_destination.x + 2u,
                   upload_destination.y + 2u, upper_pixel) ||
       !pixel_near(upper_pixel, 0u, 255u, 0u, 255u, 8u)) {
      fprintf(stderr,
              "armgl-import-smoke: upload mismatch %u,%u,%u,%u\n",
              upper_pixel[0], upper_pixel[1], upper_pixel[2], upper_pixel[3]);
      goto out;
   }

   glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
   glClear(GL_COLOR_BUFFER_BIT);
   memset(&upper_half, 0, sizeof(upper_half));
   upper_half.y = IMAGE_HEIGHT / 2u;
   upper_half.width = IMAGE_WIDTH;
   upper_half.height = IMAGE_HEIGHT / 2u;
   if (!armgl_surface_blit_image(consumer.context, consumer.surface,
                                 imported, &upper_half, &full_image, true) ||
       !armgl_flush(consumer.context, true) ||
       !read_pixel(IMAGE_WIDTH / 2u, IMAGE_HEIGHT / 2u, upper_pixel) ||
       !pixel_near(upper_pixel, 0u, 0u, 128u, 255u, 12u)) {
      fprintf(stderr,
              "armgl-import-smoke: alpha blit mismatch %u,%u,%u,%u\n",
              upper_pixel[0], upper_pixel[1], upper_pixel[2], upper_pixel[3]);
      goto out;
   }

   if (!armgl_surface_set_color_image(consumer.surface, imported) ||
       !armgl_make_current(consumer.context, consumer.surface,
                           consumer.surface)) {
      fprintf(stderr, "armgl-import-smoke: imported attachment failed\n");
      goto out;
   }

   glViewport(0, 0, IMAGE_WIDTH, IMAGE_HEIGHT);
   glClearColor(0.0f, 1.0f, 0.0f, 1.0f);
   glClear(GL_COLOR_BUFFER_BIT);
   if (!read_pixel(IMAGE_WIDTH / 2u, IMAGE_HEIGHT / 2u, upper_pixel) ||
       !armgl_flush(consumer.context, true) ||
       !pixel_near(upper_pixel, 0u, 255u, 0u, 255u, 8u)) {
      fprintf(stderr,
              "armgl-import-smoke: pixel mismatch %u,%u,%u,%u\n",
              upper_pixel[0], upper_pixel[1], upper_pixel[2], upper_pixel[3]);
      goto out;
   }

   printf("armgl-import-smoke: import, upload, fill, alpha and render target verified\n");
   result = 0;

out:
   if (image_fd >= 0)
      close(image_fd);
   armgl_image_destroy(uploaded);
   armgl_image_destroy(imported);
   smoke_endpoint_destroy(&consumer);
   smoke_endpoint_destroy(&producer);
   return result;
}
