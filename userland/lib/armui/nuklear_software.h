/*
 * ArmOS
 * Copyright (c) 2026 Mohamed Ennassiri
 *
 * Licensed under the Apache License, Version 2.0.
 * See LICENSE for details.
 *
 * File: userland/lib/armui/nuklear_software.h
 * Layer: Userland / graphical libraries
 *
 * Responsibilities:
 * - Describe the common software target used to render Nuklear commands.
 * - Keep applications independent from framebuffer and platform details.
 * - Support both complete UI frames and overlays over application pixels.
 */

#ifndef ARMOS_ARMUI_NUKLEAR_SOFTWARE_H
#define ARMOS_ARMUI_NUKLEAR_SOFTWARE_H

#include <stdint.h>

#include <nuklear.h>

struct armui_nk_target {
    uint32_t *pixels;
    int width;
    int height;
    int stride;
    const unsigned char *font_pixels;
    int font_width;
    int font_height;
};

void armui_nk_render(struct nk_context *context,
                     const struct armui_nk_target *target,
                     int clear_target, uint32_t background);

#endif
