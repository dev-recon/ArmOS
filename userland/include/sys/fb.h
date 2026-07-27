/*
 * ArmOS
 * Copyright (c) 2026 Mohamed Ennassiri
 *
 * Licensed under the Apache License, Version 2.0.
 * See LICENSE for details.
 *
 * File: userland/include/sys/fb.h
 * Layer: Userland / public header
 * Description: Userspace ABI for the ArmOS framebuffer device.
 */

#ifndef ARM_OS_NEWLIB_SYS_FB_H
#define ARM_OS_NEWLIB_SYS_FB_H

#include <stdint.h>

#define ARMOS_FBIOGET_INFO      0x4600u
#define ARMOS_FBIOGET_ORIENTATION 0x4601u
#define ARMOS_FBIOSET_ORIENTATION 0x4602u
#define ARMOS_FBIOSET_MODE      0x4603u
#define ARMOS_FBIOACQUIRE       0x4604u
#define ARMOS_FBIORELEASE       0x4605u
#define ARMOS_FBIOBLIT          0x4606u
#define ARMOS_FB_FORMAT_ARGB8888 1u

#define ARMOS_FB_ORIENTATION_PORTRAIT  0u
#define ARMOS_FB_ORIENTATION_LANDSCAPE 1u

struct armos_fb_info {
    uint32_t width;
    uint32_t height;
    uint32_t pitch;
    uint32_t bpp;
    uint32_t size;
    uint32_t format;
};

struct armos_fb_orientation {
    uint32_t orientation;
};

struct armos_fb_mode {
    uint32_t width;
    uint32_t height;
};

struct armos_fb_blit {
    uint32_t x;
    uint32_t y;
    uint32_t width;
    uint32_t height;
    uint32_t source_pitch;
    uint64_t source;
};

#endif
