/*
 * ArmOS
 * Copyright (c) 2026 Mohamed Ennassiri
 *
 * Licensed under the Apache License, Version 2.0.
 * See LICENSE for details.
 *
 * File: kernel/platform/qemu_virt/devices.c
 * Layer: Kernel / QEMU virt platform devices
 *
 * Responsibilities:
 * - Probe and attach QEMU virt devices that are not part of the core CPU/MMU
 *   bring-up.
 * - Keep VirtIO GPU/input/net details out of kernel/main.c.
 * - Register UART as tty0 and optional VirtIO display/input as tty1.
 * - Mark the dedicated VirtIO framebuffer as available to the compositor.
 *
 * Notes:
 * - The implementation is shared by ARM32 and ARM64 QEMU virt. Architecture
 *   code supplies only the MMIO map and interrupt-controller mechanisms.
 */

#include <kernel/display.h>
#include <kernel/disk_layout.h>
#include <kernel/kprintf.h>
#include <kernel/keyboard.h>
#include <kernel/platform_devices.h>
#include <kernel/tty.h>
#include <kernel/uart.h>
#include <kernel/virtio_block.h>
#include <kernel/virtio_gpu.h>
#include <kernel/virtio_input.h>
#include <kernel/virtio_net.h>

static int qemu_display_flush_rect(const uint8_t *framebuffer, uint32_t pitch,
                                   uint32_t x, uint32_t y,
                                   uint32_t width, uint32_t height)
{
    (void)framebuffer;
    (void)pitch;
    return virtio_gpu_flush_rect(x, y, width, height);
}

static const display_backend_ops_t qemu_display_backend = {
    .name = "virtio-gpu",
    .flush_rect = qemu_display_flush_rect,
    .check_resize = virtio_gpu_check_resize,
    .set_orientation = NULL,
    .set_mode = NULL,
};

void platform_console_early_init(void)
{
    uart_init();
    uart_attach_tty_backend_to(TTY_CONSOLE_ID);
    tty_set_kernel_console(TTY_CONSOLE_ID, false);
}

void platform_console_enable_rx(void)
{
    uart_enable_rx_interrupts();
}

platform_devices_state_t platform_devices_init(void)
{
    platform_devices_state_t state = {0};

    init_keyboard();

    if (init_display(FB_WIDTH, FB_HEIGHT, FB_BPP) && virtio_gpu_init()) {
        display_set_backend(&qemu_display_backend);
        KBOOT_OKF("GPU: virtio-gpu %ux%ux%u", FB_WIDTH, FB_HEIGHT, FB_BPP);
        if (framebuffer_attach_tty_backend(TTY_GRAPHICS_ID) == 0) {
            state.display_ready = true;
            state.compositor_allowed = true;
            tty_set_active(TTY_GRAPHICS_ID);
            KBOOT_OKF("TTY: console tty1 on virtio-gpu");
            if (virtio_input_init(TTY_GRAPHICS_ID)) {
                KBOOT_OKF("Input: virtio-keyboard on tty1");
            } else {
                KBOOT_WARN("Input: virtio-keyboard unavailable");
            }
        } else {
            KBOOT_WARN("TTY: tty1 framebuffer backend unavailable");
        }
    } else {
        KBOOT_WARN("GPU: virtio-gpu unavailable");
    }

    KBOOT_OKF("TTY: console tty0 on uart0");

    if (virtio_net_init()) {
        uint8_t mac[6];
        virtio_net_get_mac(mac);
        KBOOT_OKF("Net: virtio-net %02X:%02X:%02X:%02X:%02X:%02X irq %u",
                  mac[0], mac[1], mac[2], mac[3], mac[4], mac[5],
                  virtio_net_get_irq());
    } else {
        KBOOT_WARN("Net: virtio-net unavailable");
    }

    return state;
}

bool platform_block_init(void)
{
    uint64_t disk_sectors;
    uint32_t disk_mb;

    if (!init_blk()) {
        KBOOT_WARN("Block: virtio0 unavailable");
        return false;
    }

    disk_sectors = blk_get_capacity_sectors();
    disk_mb = (uint32_t)(disk_sectors / 2048u);
    KBOOT_OKF("Block: virtio0 %uMB, irq %u", disk_mb, virtio_blk_get_irq());

    if (!disk_layout_init_from_mbr())
        KBOOT_WARN("Partition: using compiled fallback layout");

    return true;
}

void platform_block_shutdown(void)
{
    virtio_blk_shutdown();
}
