/*
 * ArmOS
 * Copyright (c) 2026 Mohamed Ennassiri
 *
 * Licensed under the Apache License, Version 2.0.
 * See LICENSE for details.
 *
 * File: kernel/platform/qemu_virt/virtio_gpu.h
 * Layer: Kernel / QEMU virt platform interface
 *
 * Responsibilities:
 * - Declare the private interface of the QEMU VirtIO-GPU backend.
 * - Keep VirtIO details inside the qemu-virt platform layer.
 *
 * Notes:
 * - Common kernel code must not include this header.
 */

#ifndef _KERNEL_VIRTIO_GPU_H
#define _KERNEL_VIRTIO_GPU_H

#include <kernel/types.h>

bool virtio_gpu_init(void);
bool virtio_gpu_is_initialized(void);
uint32_t virtio_gpu_width(void);
uint32_t virtio_gpu_height(void);
int virtio_gpu_flush(void);
bool virtio_gpu_check_resize(void);
int virtio_gpu_flush_rect(uint32_t x, uint32_t y, uint32_t width, uint32_t height);
void virtio_gpu_draw_test_pattern(void);
uint32_t virtio_gpu_get_irq(void);
void virtio_gpu_irq_handler(void);

#endif
