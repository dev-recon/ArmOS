/*
 * ArmOS
 * Copyright (c) 2026 Mohamed Ennassiri
 *
 * Licensed under the Apache License, Version 2.0.
 * See LICENSE for details.
 *
 * File: userland/programs/armos-wlcomp/framebuffer_backend.c
 * Layer: Userland / graphical services
 *
 * Responsibilities:
 * - Present clipped canvas rectangles through the common framebuffer ABI.
 * - Keep framebuffer compatibility I/O separate from scene composition.
 * - Preserve one backend contract across QEMU and Raspberry Pi displays.
 *
 * Notes:
 * - Mapped buffers plus ARMOS_FBIOPRESENT avoid syscall-side pixel copies.
 * - ARMOS_FBIOBLIT preserves compatibility with older kernels.
 * - The row-write fallback supports kernels predating the blit operation.
 */

#include "armos_wlcomp.h"

#include <errno.h>
#include <stdint.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

static uint64_t wl_backend_monotonic_us(void)
{
    struct timespec now;

    if (clock_gettime(CLOCK_MONOTONIC, &now) < 0)
        return 0u;
    return (uint64_t)now.tv_sec * 1000000u +
           (uint64_t)now.tv_nsec / 1000u;
}

static int wl_backend_write_full(int fd, const void *buffer, size_t size)
{
    const uint8_t *cursor = (const uint8_t *)buffer;
    size_t done = 0u;

    while (done < size) {
        ssize_t count = write(fd, cursor + done, size - done);

        if (count < 0 && errno == EINTR)
            continue;
        if (count <= 0)
            return -1;
        done += (size_t)count;
    }
    return 0;
}

int wl_renderer_backend_present_rect(
    struct wl_server_renderer *renderer, int32_t x, int32_t y,
    uint32_t width, uint32_t height)
{
    struct armos_fb_blit blit;
    struct armos_fb_present present;
    uint64_t started_us;
    int32_t x1;
    int32_t y1;

    if (!renderer || renderer->headless || width == 0u || height == 0u)
        return 0;
    x1 = x + (int32_t)width;
    y1 = y + (int32_t)height;
    if (x < 0)
        x = 0;
    if (y < 0)
        y = 0;
    if (x1 > (int32_t)renderer->framebuffer.width)
        x1 = (int32_t)renderer->framebuffer.width;
    if (y1 > (int32_t)renderer->framebuffer.height)
        y1 = (int32_t)renderer->framebuffer.height;
    if (x >= x1 || y >= y1)
        return 0;

    width = (uint32_t)(x1 - x);
    height = (uint32_t)(y1 - y);
    started_us = renderer->profile_enabled ?
        wl_backend_monotonic_us() : 0u;
    if (renderer->direct_present) {
        present.buffer_index = renderer->present_draw_buffer;
        present.x = (uint32_t)x;
        present.y = (uint32_t)y;
        present.width = width;
        present.height = height;
        if (ioctl(renderer->framebuffer_fd,
                  ARMOS_FBIOPRESENT, &present) < 0)
            return -1;
        if (started_us != 0u) {
            uint64_t finished_us = wl_backend_monotonic_us();

            if (finished_us >= started_us)
                renderer->profile_present_us += finished_us - started_us;
            renderer->profile_present_pixels +=
                (uint64_t)width * height;
        }
        return 0;
    }
    blit.x = (uint32_t)x;
    blit.y = (uint32_t)y;
    blit.width = width;
    blit.height = height;
    blit.source_pitch = renderer->framebuffer.pitch;
    blit.source = (uint64_t)(uintptr_t)(
        (const uint8_t *)renderer->canvas +
        (uint32_t)y * renderer->framebuffer.pitch +
        (uint32_t)x * sizeof(uint32_t));
    if (ioctl(renderer->framebuffer_fd, ARMOS_FBIOBLIT, &blit) == 0) {
        if (started_us != 0u) {
            uint64_t finished_us = wl_backend_monotonic_us();

            if (finished_us >= started_us)
                renderer->profile_present_us += finished_us - started_us;
            renderer->profile_present_pixels +=
                (uint64_t)width * height;
        }
        return 0;
    }
    if (errno != ENOTTY && errno != ENOSYS)
        return -1;

    for (int32_t row = y; row < y1; row++) {
        off_t offset = (off_t)(uint32_t)row * renderer->framebuffer.pitch +
            (off_t)(uint32_t)x * sizeof(uint32_t);
        const uint8_t *source = (const uint8_t *)renderer->canvas + offset;

        if (lseek(renderer->framebuffer_fd, offset, SEEK_SET) < 0 ||
            wl_backend_write_full(
                renderer->framebuffer_fd, source,
                (size_t)width * sizeof(uint32_t)) < 0)
            return -1;
    }
    if (started_us != 0u) {
        uint64_t finished_us = wl_backend_monotonic_us();

        if (finished_us >= started_us)
            renderer->profile_present_us += finished_us - started_us;
        renderer->profile_present_pixels += (uint64_t)width * height;
    }
    return 0;
}
