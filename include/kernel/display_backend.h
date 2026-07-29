/*
 * ArmOS
 * Copyright (c) 2026 Mohamed Ennassiri
 *
 * Licensed under the Apache License, Version 2.0.
 * See LICENSE for details.
 *
 * File: include/kernel/display_backend.h
 * Layer: Kernel / display backend contract
 *
 * Responsibilities:
 * - Decouple the common framebuffer console from a concrete display device.
 * - Let VirtIO and physical LCD controllers expose the same /dev/fb0 ABI.
 *
 * Notes:
 * - The in-memory framebuffer remains ARGB8888. Backends may convert pixels
 *   while flushing to their native format.
 * - scroll_up is optional. The common renderer retains a software fallback
 *   for devices that cannot pan their scanout buffer.
 */

#ifndef _KERNEL_DISPLAY_BACKEND_H
#define _KERNEL_DISPLAY_BACKEND_H

#include <kernel/types.h>

#define ARMOS_FB_ORIENTATION_PORTRAIT  0u
#define ARMOS_FB_ORIENTATION_LANDSCAPE 1u

typedef struct display_backend_mode {
    uint32_t width;
    uint32_t height;
    uint32_t pitch;
    uint32_t bpp;
    uint32_t size;
    paddr_t physical;
    uint8_t *virtual_address;
} display_backend_mode_t;

typedef struct display_present_buffers {
    paddr_t physical;
    uint8_t *virtual_address;
    uint32_t buffer_size;
    uint32_t buffer_count;
    uint32_t front_buffer;
} display_present_buffers_t;

typedef struct display_backend_ops {
    const char *name;
    int (*flush_rect)(const uint8_t *framebuffer, uint32_t pitch,
                      uint32_t x, uint32_t y,
                      uint32_t width, uint32_t height);
    bool (*check_resize)(void);
    int (*set_orientation)(uint32_t orientation,
                           uint32_t *width, uint32_t *height);
    int (*set_mode)(uint32_t width, uint32_t height,
                    display_backend_mode_t *mode);
    int (*scroll_up)(uint32_t rows, uint32_t clear_color,
                     display_backend_mode_t *mode);
    int (*get_present_buffers)(display_present_buffers_t *buffers);
    int (*present_buffer)(uint32_t buffer_index,
                          uint32_t x, uint32_t y,
                          uint32_t width, uint32_t height,
                          display_backend_mode_t *mode);
} display_backend_ops_t;

#endif /* _KERNEL_DISPLAY_BACKEND_H */
