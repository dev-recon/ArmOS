/*
 * ArmOS
 * Copyright (c) 2026 Mohamed Ennassiri
 *
 * Licensed under the Apache License, Version 2.0.
 * See LICENSE for details.
 *
 * File: userland/opt/nuklear/include/armos_nuklear_config.h
 * Layer: Userland / third-party configuration
 *
 * Responsibilities:
 * - Define the single Nuklear ABI configuration used throughout ArmOS.
 * - Prevent incompatible feature selections between translation units.
 *
 * Notes:
 * - Include this file immediately before every inclusion of nuklear.h.
 * - NK_IMPLEMENTATION remains private to the Nuklear implementation unit.
 */

#ifndef ARMOS_NUKLEAR_CONFIG_H
#define ARMOS_NUKLEAR_CONFIG_H

#define NK_INCLUDE_FIXED_TYPES
#define NK_INCLUDE_STANDARD_IO
#define NK_INCLUDE_STANDARD_VARARGS
#define NK_INCLUDE_DEFAULT_ALLOCATOR
#define NK_INCLUDE_VERTEX_BUFFER_OUTPUT
#define NK_INCLUDE_FONT_BAKING
#define NK_INCLUDE_DEFAULT_FONT
#define NK_BUTTON_TRIGGER_ON_RELEASE

#endif
