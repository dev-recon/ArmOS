/*
 * ArmOS
 * Copyright (c) 2026 Mohamed Ennassiri
 *
 * Licensed under the Apache License, Version 2.0.
 * See LICENSE for details.
 *
 * File: userland/programs/armgl-compositor-smoke/armgl-compositor-smoke.c
 * Layer: Userland / GPU validation
 *
 * Responsibilities:
 * - Exercise the exact hardware-neutral compositor GPU contract.
 * - Validate render-node output export, card-node import and explicit fences.
 * - Rotate and present every output slot without depending on the window
 *   server.
 */

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <uapi/armos/drm.h>

#include "gpu_backend.h"
#include "gpu_present.h"

#define TEST_IMAGE_SIZE 64u

static int
read_scanout_size(uint32_t *width, uint32_t *height)
{
    armos_drm_info_t info;
    int fd;

    fd = open("/dev/dri/card0", O_RDWR, 0);
    if (fd < 0)
        return -1;
    memset(&info, 0, sizeof(info));
    if (ioctl(fd, ARMOS_DRM_IOCTL_GET_INFO, &info) < 0 ||
        info.abi_version != ARMOS_DRM_ABI_VERSION ||
        (info.capabilities & ARMOS_DRM_CAP_SCANOUT) == 0u ||
        !info.scanout_width || !info.scanout_height) {
        close(fd);
        return -1;
    }
    close(fd);
    *width = info.scanout_width;
    *height = info.scanout_height;
    return 0;
}

int
main(void)
{
    struct wl_gpu_backend_config backend_config;
    struct wl_gpu_image_config image_config;
    struct wl_gpu_presenter presenter;
    struct wl_gpu_backend *backend = NULL;
    struct wl_gpu_image *image = NULL;
    struct wl_gpu_rect rect;
    uint32_t *pixels = NULL;
    uint32_t width;
    uint32_t height;
    bool frame_active = false;
    int status = 1;

    memset(&presenter, 0, sizeof(presenter));
    presenter.card_fd = -1;
    if (read_scanout_size(&width, &height) < 0) {
        perror("armgl-compositor-smoke: scanout");
        goto out;
    }
    if (width < TEST_IMAGE_SIZE || height < TEST_IMAGE_SIZE) {
        fputs("armgl-compositor-smoke: scanout is too small\n", stderr);
        goto out;
    }
    memset(&backend_config, 0, sizeof(backend_config));
    backend_config.render_node = "/dev/dri/renderD128";
    backend_config.width = width;
    backend_config.height = height;
    backend = wl_gpu_backend_create(&backend_config);
    if (!backend) {
        fputs("armgl-compositor-smoke: backend creation failed\n", stderr);
        goto out;
    }
    if (!wl_gpu_presenter_init(&presenter, "/dev/dri/card0",
                               width, height)) {
        perror("armgl-compositor-smoke: output import");
        goto out;
    }
    pixels = malloc(TEST_IMAGE_SIZE * TEST_IMAGE_SIZE * sizeof(*pixels));
    if (!pixels)
        goto out;
    for (uint32_t y = 0u; y < TEST_IMAGE_SIZE; y++) {
        for (uint32_t x = 0u; x < TEST_IMAGE_SIZE; x++) {
            uint32_t red = x * 255u / (TEST_IMAGE_SIZE - 1u);
            uint32_t green = y * 255u / (TEST_IMAGE_SIZE - 1u);

            pixels[y * TEST_IMAGE_SIZE + x] =
                0xff0000ffu | (red << 16) | (green << 8);
        }
    }
    memset(&image_config, 0, sizeof(image_config));
    image_config.width = TEST_IMAGE_SIZE;
    image_config.height = TEST_IMAGE_SIZE;
    image_config.stride = TEST_IMAGE_SIZE * sizeof(uint32_t);
    image_config.alpha = false;
    image = wl_gpu_backend_create_image(backend, &image_config);
    if (!image) {
        fputs("armgl-compositor-smoke: image creation failed\n", stderr);
        goto out;
    }
    for (uint32_t frame = 0u;
         frame < WL_GPU_MAX_OUTPUT_BUFFERS; frame++) {
        struct wl_gpu_frame frame_info;
        struct wl_gpu_rect destination;
        static const uint32_t backgrounds[WL_GPU_MAX_OUTPUT_BUFFERS] = {
            0xff18232fu, 0xff203448u, 0xff28475cu,
        };

        if (!wl_gpu_backend_begin_frame(backend, &frame_info)) {
            fputs("armgl-compositor-smoke: frame begin failed\n", stderr);
            goto out;
        }
        frame_active = true;
        memset(&rect, 0, sizeof(rect));
        rect.width = width;
        rect.height = height;
        if (!wl_gpu_backend_fill(backend, &rect, backgrounds[frame])) {
            perror("armgl-compositor-smoke: output fill");
            goto out;
        }
        memset(&rect, 0, sizeof(rect));
        rect.width = TEST_IMAGE_SIZE;
        rect.height = TEST_IMAGE_SIZE;
        if (frame == 0u &&
            !wl_gpu_backend_upload(backend, image, &rect, pixels,
                                   image_config.stride)) {
            fputs("armgl-compositor-smoke: image upload failed\n", stderr);
            goto out;
        }
        destination = rect;
        destination.x = (width - TEST_IMAGE_SIZE) / 2u + frame * 8u;
        if (destination.x + destination.width > width)
            destination.x = width - destination.width;
        destination.y = (height - TEST_IMAGE_SIZE) / 2u;
        if (!wl_gpu_backend_blit(backend, image, &rect,
                                 &destination, false)) {
            fputs("armgl-compositor-smoke: image blit failed\n", stderr);
            goto out;
        }
        if (!wl_gpu_presenter_present(&presenter, backend, NULL)) {
            frame_active = false;
            fprintf(stderr,
                    "armgl-compositor-smoke: presentation failed at %s: %s\n",
                    wl_gpu_presenter_error_string(&presenter),
                    strerror(errno));
            goto out;
        }
        frame_active = false;
    }
    puts("armgl-compositor-smoke: three GPU buffers exported, fenced and "
         "presented");
    status = 0;

out:
    if (frame_active)
        wl_gpu_backend_end_frame(backend);
    free(pixels);
    wl_gpu_backend_destroy_image(backend, image);
    wl_gpu_presenter_destroy(&presenter);
    wl_gpu_backend_destroy(backend);
    return status;
}
