/*
 * ArmOS
 * Copyright (c) 2026 Mohamed Ennassiri
 *
 * Licensed under the Apache License, Version 2.0.
 * See LICENSE for details.
 *
 * File: include/kernel/virtio_input.h
 * Layer: Kernel / public internal interface
 *
 * Responsibilities:
 * - Declare kernel types, constants, and subsystem contracts.
 * - Keep cross-module ABI and structure expectations explicit.
 *
 * Notes:
 * - Header changes can ripple across kernel and user ABI glue.
 */

#ifndef _KERNEL_VIRTIO_INPUT_H
#define _KERNEL_VIRTIO_INPUT_H

#include <kernel/types.h>

bool virtio_input_init(int tty_id);
void virtio_input_irq_handler(uint32_t irq);
uint32_t virtio_input_get_irq(void);
bool virtio_input_handles_irq(uint32_t irq);
bool virtio_input_is_initialized(void);
void virtio_input_get_stats(uint32_t *irq_count, uint32_t *used_count,
                            uint32_t *key_events, uint32_t *emitted_chars,
                            uint32_t *last_type, uint32_t *last_code,
                            uint32_t *last_value, uint32_t *last_irq_status,
                            uint32_t *queue_size, uint32_t *last_used_idx,
                            uint32_t *status);

#endif
