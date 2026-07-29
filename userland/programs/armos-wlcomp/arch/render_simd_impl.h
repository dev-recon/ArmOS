/*
 * ArmOS
 * Copyright (c) 2026 Mohamed Ennassiri
 *
 * Licensed under the Apache License, Version 2.0.
 * See LICENSE for details.
 *
 * File: userland/programs/armos-wlcomp/arch/render_simd_impl.h
 * Layer: Userland / architecture-specific rendering
 *
 * Responsibilities:
 * - Implement 128-bit premultiplied-alpha blending.
 * - Share the integer SIMD algorithm between ARM32 NEON and ARM64 ASIMD.
 * - Retain scalar tails for rectangles that are not multiples of four pixels.
 *
 * Notes:
 * - This file is included by exactly one target-specific translation unit.
 * - The public compositor and scene code remain architecture neutral.
 * - The vector type has pixel alignment so clipped rows need no 16-byte peel.
 */

#ifndef ARMOS_WLCOMP_RENDER_SIMD_IMPL_H
#define ARMOS_WLCOMP_RENDER_SIMD_IMPL_H

#include "../armos_wlcomp.h"

typedef uint32_t wl_render_u32x4
    __attribute__((vector_size(16), aligned(4), __may_alias__));

void wl_render_arch_blend(uint32_t *destination, const uint32_t *source,
                          size_t pixels)
{
    const wl_render_u32x4 byte_mask = {
        0xffu, 0xffu, 0xffu, 0xffu
    };
    const wl_render_u32x4 full_alpha = {
        255u, 255u, 255u, 255u
    };
    const wl_render_u32x4 rounding = {
        128u, 128u, 128u, 128u
    };

    while (pixels >= 4u) {
        wl_render_u32x4 source_pixels =
            *(const wl_render_u32x4 *)(const void *)source;
        wl_render_u32x4 destination_pixels =
            *(const wl_render_u32x4 *)(const void *)destination;
        wl_render_u32x4 alpha = source_pixels >> 24;
        wl_render_u32x4 inverse = full_alpha - alpha;
        wl_render_u32x4 blue =
            (destination_pixels & byte_mask) * inverse;
        wl_render_u32x4 green =
            ((destination_pixels >> 8) & byte_mask) * inverse;
        wl_render_u32x4 red =
            ((destination_pixels >> 16) & byte_mask) * inverse;
        wl_render_u32x4 destination_alpha =
            (destination_pixels >> 24) * inverse;

        blue += rounding;
        blue = (blue + (blue >> 8)) >> 8;
        green += rounding;
        green = (green + (green >> 8)) >> 8;
        red += rounding;
        red = (red + (red >> 8)) >> 8;
        destination_alpha += rounding;
        destination_alpha =
            (destination_alpha + (destination_alpha >> 8)) >> 8;

        *(wl_render_u32x4 *)(void *)destination =
            source_pixels + blue + (green << 8) + (red << 16) +
            (destination_alpha << 24);
        destination += 4;
        source += 4;
        pixels -= 4u;
    }
    while (pixels-- != 0u) {
        *destination = wl_render_blend_pixel(*destination, *source);
        destination++;
        source++;
    }
}

#endif /* ARMOS_WLCOMP_RENDER_SIMD_IMPL_H */
