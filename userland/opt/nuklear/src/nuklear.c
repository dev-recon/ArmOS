/*
 * ArmOS
 * Copyright (c) 2026 Mohamed Ennassiri
 *
 * Licensed under the Apache License, Version 2.0.
 * See LICENSE for details.
 *
 * File: userland/opt/nuklear/src/nuklear.c
 * Layer: Userland / third-party graphical libraries
 *
 * Responsibilities:
 * - Build the pinned Nuklear single-header implementation once.
 * - Select the portable C facilities provided by the ArmOS newlib runtime.
 * - Expose a reusable static library to ArmOS graphical applications.
 *
 * Notes:
 * - The unmodified upstream header and its license live in the parent bundle.
 * - Renderer, window-system and input integration intentionally remain outside
 *   this third-party translation unit.
 */

#include <armos_nuklear_config.h>
#define NK_IMPLEMENTATION
#include <nuklear.h>
