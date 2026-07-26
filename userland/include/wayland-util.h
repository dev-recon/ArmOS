/*
 * ArmOS
 * Copyright (c) 2026 Mohamed Ennassiri
 *
 * Licensed under the Apache License, Version 2.0.
 * See LICENSE for details.
 *
 * File: userland/include/wayland-util.h
 * Layer: Userland / Wayland compatibility
 *
 * Responsibilities:
 * - Define the public data types shared by Wayland client and server APIs.
 * - Preserve source compatibility with generated Wayland protocol headers.
 *
 * Notes:
 * - Protocol and API names follow the public Wayland contract.
 * - The implementation behind these declarations is native ArmOS code.
 */

#ifndef ARMOS_WAYLAND_UTIL_H
#define ARMOS_WAYLAND_UTIL_H

#include <stddef.h>
#include <stdint.h>

typedef int32_t wl_fixed_t;

struct wl_interface;

struct wl_message {
    const char *name;
    const char *signature;
    const struct wl_interface **types;
};

struct wl_interface {
    const char *name;
    int version;
    int method_count;
    const struct wl_message *methods;
    int event_count;
    const struct wl_message *events;
};

struct wl_array {
    size_t size;
    size_t alloc;
    void *data;
};

static inline double wl_fixed_to_double(wl_fixed_t value)
{
    return (double)value / 256.0;
}

static inline wl_fixed_t wl_fixed_from_int(int value)
{
    return (wl_fixed_t)(value * 256);
}

#endif /* ARMOS_WAYLAND_UTIL_H */
