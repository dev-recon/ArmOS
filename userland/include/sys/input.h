/*
 * ArmOS
 * Copyright (c) 2026 Mohamed Ennassiri
 *
 * Licensed under the Apache License, Version 2.0.
 * See LICENSE for details.
 *
 * File: userland/include/sys/input.h
 * Layer: Userland / public device ABI
 *
 * Responsibilities:
 * - Describe events read from /dev/input0.
 * - Keep graphical services independent from physical input transports.
 * - Publish stable key, pointer, and text event identifiers.
 *
 * Notes:
 * - Key codes use the ArmOS common keyboard namespace.
 * - Text events contain one byte-sized character in value for now.
 */

#ifndef SYS_INPUT_H
#define SYS_INPUT_H

#include <stdint.h>
#include <uapi/armos/input.h>

#define ARMOS_INPUT_EVENT_SYNC       0u
#define ARMOS_INPUT_EVENT_KEY        1u
#define ARMOS_INPUT_EVENT_RELATIVE   2u
#define ARMOS_INPUT_EVENT_ABSOLUTE   3u
#define ARMOS_INPUT_EVENT_TEXT       4u

#define ARMOS_INPUT_AXIS_X           0u
#define ARMOS_INPUT_AXIS_Y           1u
#define ARMOS_INPUT_AXIS_WHEEL       2u
#define ARMOS_INPUT_ABSOLUTE_MAX     65535u

#define ARMOS_INPUT_BUTTON_LEFT      0x110u
#define ARMOS_INPUT_BUTTON_RIGHT     0x111u
#define ARMOS_INPUT_BUTTON_MIDDLE    0x112u

#define ARMOS_INPUT_KEY_C            46u
#define ARMOS_INPUT_KEY_LEFTCTRL     29u
#define ARMOS_INPUT_KEY_LEFTSHIFT    42u
#define ARMOS_INPUT_KEY_RIGHTSHIFT   54u
#define ARMOS_INPUT_KEY_LEFTALT      56u
#define ARMOS_INPUT_KEY_CAPSLOCK     58u
#define ARMOS_INPUT_KEY_RIGHTCTRL    97u
#define ARMOS_INPUT_KEY_RIGHTALT     100u
#define ARMOS_INPUT_KEY_LEFTMETA     125u
#define ARMOS_INPUT_KEY_RIGHTMETA    126u

struct armos_input_event {
    uint32_t timestamp_ms;
    uint16_t type;
    uint16_t code;
    int32_t value;
};

#endif /* SYS_INPUT_H */
