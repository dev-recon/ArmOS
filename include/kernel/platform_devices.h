/*
 * ArmOS
 * Copyright (c) 2026 Mohamed Ennassiri
 *
 * Licensed under the Apache License, Version 2.0.
 * See LICENSE for details.
 *
 * File: include/kernel/platform_devices.h
 * Layer: Kernel / platform boundary
 *
 * Responsibilities:
 * - Expose board-specific device bring-up to the generic boot path.
 * - Report whether the primary framebuffer may be owned by a compositor.
 * - Keep kernel/main.c from knowing concrete QEMU, Raspberry Pi, or VirtIO
 *   probing details.
 *
 * Notes:
 * - Platforms register transports; the common TTY layer owns terminal
 *   semantics and the logical console policy.
 * - A display can be ready while remaining reserved for its shell console.
 */

#ifndef _KERNEL_PLATFORM_DEVICES_H
#define _KERNEL_PLATFORM_DEVICES_H

#include <kernel/types.h>

typedef struct {
    bool display_ready;
    bool compositor_allowed;
} platform_devices_state_t;

void platform_console_early_init(void);
void platform_console_enable_rx(void);
platform_devices_state_t platform_devices_init(void);
bool platform_device_irq_dispatch(uint32_t irq);
bool platform_block_init(void);
void platform_block_shutdown(void);

#endif /* _KERNEL_PLATFORM_DEVICES_H */
