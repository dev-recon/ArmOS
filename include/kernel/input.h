/*
 * ArmOS
 * Copyright (c) 2026 Mohamed Ennassiri
 *
 * Licensed under the Apache License, Version 2.0.
 * See LICENSE for details.
 *
 * File: include/kernel/input.h
 * Layer: Kernel / common device interface
 *
 * Responsibilities:
 * - Define the architecture-neutral input event ABI.
 * - Accept events from platform input drivers.
 * - Expose the shared input stream through /dev/input0.
 *
 * Notes:
 * - Platform drivers translate only their transport-specific identifiers.
 * - Consumers never depend on VirtIO, USB, GPIO, or platform registers.
 */

#ifndef KERNEL_INPUT_H
#define KERNEL_INPUT_H

#include <kernel/task.h>
#include <kernel/types.h>

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

typedef struct armos_input_event {
    uint32_t timestamp_ms;
    uint16_t type;
    uint16_t code;
    int32_t value;
} armos_input_event_t;

void armos_input_emit(uint16_t type, uint16_t code, int32_t value);
bool armos_input_tty_routing_enabled(void);
bool armos_input_read_ready(void);
bool is_input_device_path(const char *path);
void fill_input_device_stat(struct stat *st);
file_t *create_input_device_file(const char *name, int flags, int *error);

#endif /* KERNEL_INPUT_H */
