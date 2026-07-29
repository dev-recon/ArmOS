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

static bool wl_render_profile_enabled;
static struct wl_render_profile wl_render_profile_data;

void wl_render_profile_set_enabled(bool enabled)
{
    wl_render_profile_enabled = enabled;
    memset(&wl_render_profile_data, 0, sizeof(wl_render_profile_data));
}

void wl_render_profile_take(struct wl_render_profile *profile)
{
    if (!profile)
        return;
    *profile = wl_render_profile_data;
    memset(&wl_render_profile_data, 0, sizeof(wl_render_profile_data));
}

uint32_t wl_render_blend_pixel(uint32_t destination, uint32_t source)
{
    uint32_t alpha = source >> 24;
    uint32_t inverse;
    uint32_t rb;
    uint32_t ag;

    if (alpha == 255u)
        return source;
    if (alpha == 0u)
        return destination;

    /*
     * Wayland ARGB8888 is premultiplied. Scale the destination by 1-alpha
     * and add the source directly. Two channels are processed together.
     */
    inverse = 255u - alpha;
    rb = (destination & 0x00ff00ffu) * inverse;
    rb += 0x00800080u;
    rb = (rb + ((rb >> 8) & 0x00ff00ffu)) >> 8;
    rb &= 0x00ff00ffu;
    ag = ((destination >> 8) & 0x00ff00ffu) * inverse;
    ag += 0x00800080u;
    ag = (ag + ((ag >> 8) & 0x00ff00ffu)) >> 8;
    ag &= 0x00ff00ffu;
    return source + rb + (ag << 8);
}

void wl_render_fill_rect(uint32_t *destination, uint32_t destination_pitch,
                         uint32_t width, uint32_t height, uint32_t color)
{
    uint64_t pixels;

    if (!destination || width == 0u || height == 0u)
        return;
    pixels = (uint64_t)width * height;

    for (uint32_t x = 0u; x < width; x++)
        destination[x] = color;
    for (uint32_t y = 1u; y < height; y++)
        memcpy(destination + (size_t)y * destination_pitch,
               destination, (size_t)width * sizeof(*destination));
    if (wl_render_profile_enabled)
        wl_render_profile_data.fill_pixels += pixels;
}

void wl_render_copy_rect(uint32_t *destination, uint32_t destination_pitch,
                         const uint32_t *source, uint32_t source_pitch,
                         uint32_t width, uint32_t height)
{
    uint64_t pixels;

    if (!destination || !source || width == 0u || height == 0u)
        return;
    pixels = (uint64_t)width * height;

    for (uint32_t y = 0u; y < height; y++)
        memcpy(destination + (size_t)y * destination_pitch,
               source + (size_t)y * source_pitch,
               (size_t)width * sizeof(*destination));
    if (wl_render_profile_enabled)
        wl_render_profile_data.copy_pixels += pixels;
}

void wl_render_blend_rect(uint32_t *destination, uint32_t destination_pitch,
                          const uint32_t *source, uint32_t source_pitch,
                          uint32_t width, uint32_t height)
{
    uint64_t pixels;

    if (!destination || !source || width == 0u || height == 0u)
        return;
    pixels = (uint64_t)width * height;

    for (uint32_t y = 0u; y < height; y++) {
        uint32_t *destination_row =
            destination + (size_t)y * destination_pitch;
        const uint32_t *source_row =
            source + (size_t)y * source_pitch;
        uint32_t x = 0u;

        while (x < width) {
            uint32_t alpha = source_row[x] >> 24;
            uint32_t run_start;

            if (alpha == 255u) {
                run_start = x++;
                while (x < width && (source_row[x] >> 24) == 255u)
                    x++;
                memcpy(destination_row + run_start,
                       source_row + run_start,
                       (size_t)(x - run_start) *
                           sizeof(*destination_row));
                continue;
            }
            if (alpha == 0u) {
                x++;
                continue;
            }
            run_start = x++;
            while (x < width) {
                alpha = source_row[x] >> 24;
                if (alpha == 0u || alpha == 255u)
                    break;
                x++;
            }
            wl_render_arch_blend(
                destination_row + run_start,
                source_row + run_start, x - run_start);
        }
    }
    if (wl_render_profile_enabled)
        wl_render_profile_data.blend_pixels += pixels;
}
