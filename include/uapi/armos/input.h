/*
 * ArmOS
 * Copyright (c) 2026 Mohamed Ennassiri
 *
 * Licensed under the Apache License, Version 2.0.
 * See LICENSE for details.
 *
 * File: include/uapi/armos/input.h
 * Layer: UAPI / input ABI
 *
 * Responsibilities:
 * - Define keyboard layouts shared by the kernel and userland.
 * - Define the stable ioctl contract used to change the active seat layout.
 *
 * Notes:
 * - The build profile selects only the initial layout.
 * - Runtime selection applies to the common input seat on every platform.
 */

#ifndef _UAPI_ARMOS_INPUT_H
#define _UAPI_ARMOS_INPUT_H

#define ARMOS_KEYBOARD_LAYOUT_US      0u
#define ARMOS_KEYBOARD_LAYOUT_US_MAC  1u
#define ARMOS_KEYBOARD_LAYOUT_FR      2u
#define ARMOS_KEYBOARD_LAYOUT_FR_MAC  3u
#define ARMOS_KEYBOARD_LAYOUT_FR_LEGACY 4u
#define ARMOS_KEYBOARD_LAYOUT_COUNT   5u

#define ARMOS_INPUT_EVENT_CONFIG      5u
#define ARMOS_INPUT_CONFIG_KEYBOARD_LAYOUT 0u

#define ARMOS_TIOCGKEYMAP 0x80045460u
#define ARMOS_TIOCSKEYMAP 0x40045461u
#define ARMOS_INPUT_GET_KEYMAP ARMOS_TIOCGKEYMAP

#endif /* _UAPI_ARMOS_INPUT_H */
