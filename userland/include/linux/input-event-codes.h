/*
 * ArmOS
 * Copyright (c) 2026 Mohamed Ennassiri
 *
 * Licensed under the Apache License, Version 2.0.
 * See LICENSE for details.
 *
 * File: userland/include/linux/input-event-codes.h
 * Layer: Userland / Linux input ABI compatibility
 *
 * Responsibilities:
 * - Define the stable pointer-button codes consumed by Wayland clients.
 * - Preserve source compatibility without exposing a Linux kernel interface.
 *
 * Notes:
 * - These numeric values belong to the public evdev/Wayland wire contract.
 * - ArmOS input drivers translate native events before userland sees them.
 */

#ifndef ARMOS_LINUX_INPUT_EVENT_CODES_H
#define ARMOS_LINUX_INPUT_EVENT_CODES_H

#define BTN_LEFT       0x110
#define BTN_RIGHT      0x111
#define BTN_MIDDLE     0x112
#define BTN_SIDE       0x113
#define BTN_EXTRA      0x114
#define BTN_FORWARD    0x115
#define BTN_BACK       0x116
#define BTN_TASK       0x117

#endif /* ARMOS_LINUX_INPUT_EVENT_CODES_H */
