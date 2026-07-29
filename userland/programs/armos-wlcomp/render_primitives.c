/*
 * ArmOS
 * Copyright (c) 2026 Mohamed Ennassiri
 *
 * Licensed under the Apache License, Version 2.0.
 * See LICENSE for details.
 *
 * File: userland/programs/armos-wlcomp/render_primitives.c
 * Layer: Userland / graphical services
 *
 * Responsibilities:
 * - Provide the architecture-neutral software rendering primitives.
 * - Keep opaque copy, solid fill and alpha blending out of window policy.
 * - Establish the replacement boundary for future SIMD implementations.
 *
 * Notes:
 * - Pitches are expressed in pixels, not bytes.
 * - Callers clip rectangles before invoking these primitives.
 */

#include "armos_wlcomp.h"

#include <string.h>

uint32_t wl_render_blend_pixel(uint32_t destination, uint32_t source)
{
    uint32_t alpha = source >> 24;
    uint32_t inverse;
    uint32_t red;
    uint32_t green;
    uint32_t blue;

    if (alpha == 255u)
        return source;
    if (alpha == 0u)
        return destination;

    inverse = 255u - alpha;
    red = ((((source >> 16) & 0xffu) * alpha) +
           (((destination >> 16) & 0xffu) * inverse)) / 255u;
    green = ((((source >> 8) & 0xffu) * alpha) +
             (((destination >> 8) & 0xffu) * inverse)) / 255u;
    blue = (((source & 0xffu) * alpha) +
            ((destination & 0xffu) * inverse)) / 255u;
    return 0xff000000u | (red << 16) | (green << 8) | blue;
}

void wl_render_fill_rect(uint32_t *destination, uint32_t destination_pitch,
                         uint32_t width, uint32_t height, uint32_t color)
{
    if (!destination || width == 0u || height == 0u)
        return;

    for (uint32_t x = 0u; x < width; x++)
        destination[x] = color;
    for (uint32_t y = 1u; y < height; y++)
        memcpy(destination + (size_t)y * destination_pitch,
               destination, (size_t)width * sizeof(*destination));
}

void wl_render_copy_rect(uint32_t *destination, uint32_t destination_pitch,
                         const uint32_t *source, uint32_t source_pitch,
                         uint32_t width, uint32_t height)
{
    if (!destination || !source || width == 0u || height == 0u)
        return;

    for (uint32_t y = 0u; y < height; y++)
        memcpy(destination + (size_t)y * destination_pitch,
               source + (size_t)y * source_pitch,
               (size_t)width * sizeof(*destination));
}

void wl_render_blend_rect(uint32_t *destination, uint32_t destination_pitch,
                          const uint32_t *source, uint32_t source_pitch,
                          uint32_t width, uint32_t height)
{
    if (!destination || !source || width == 0u || height == 0u)
        return;

    for (uint32_t y = 0u; y < height; y++) {
        uint32_t *destination_row =
            destination + (size_t)y * destination_pitch;
        const uint32_t *source_row = source + (size_t)y * source_pitch;
        uint32_t x = 0u;

        while (x < width) {
            uint32_t alpha = source_row[x] >> 24;

            if (alpha == 255u) {
                uint32_t run_start = x++;

                while (x < width && (source_row[x] >> 24) == 255u)
                    x++;
                memcpy(destination_row + run_start,
                       source_row + run_start,
                       (size_t)(x - run_start) * sizeof(*destination_row));
                continue;
            }
            if (alpha == 0u) {
                x++;
                continue;
            }
            destination_row[x] = wl_render_blend_pixel(
                destination_row[x], source_row[x]);
            x++;
        }
    }
}
