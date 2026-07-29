/*
 * ArmOS
 * Copyright (c) 2026 Mohamed Ennassiri
 *
 * Licensed under the Apache License, Version 2.0.
 * See LICENSE for details.
 *
 * File: userland/programs/armos-wlcomp/arch/arm64/render_simd.c
 * Layer: Userland / ARM64 rendering
 *
 * Responsibilities:
 * - Select the common 128-bit integer renderer for mandatory ARM64 ASIMD.
 */

#include "../render_simd_impl.h"
