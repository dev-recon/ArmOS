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
#include <kernel/fdt.h>
#include <kernel/kprintf.h>
#include <kernel/keyboard.h>
#include <kernel/memory.h>
#include <kernel/platform_devices.h>
#include <kernel/string.h>
#include <kernel/tty.h>
#include <kernel/uart.h>
#include <kernel/virtio_block.h>
#include "virtio_gpu.h"
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

static bool qemu_bootarg_u16(const char *bootargs, uint32_t length,
                             const char *key, uint16_t *value)
{
    uint32_t key_length = (uint32_t)strlen(key);

    if (!bootargs || !key || !value)
        return false;

    for (uint32_t offset = 0; offset + key_length < length; offset++) {
        uint32_t cursor;
        uint32_t parsed = 0;

        if (offset != 0 && bootargs[offset - 1] != ' ')
            continue;
        if (strncmp(bootargs + offset, key, key_length) != 0)
            continue;

        cursor = offset + key_length;
        if (cursor >= length || bootargs[cursor] < '0' ||
            bootargs[cursor] > '9')
            return false;

        while (cursor < length && bootargs[cursor] >= '0' &&
               bootargs[cursor] <= '9') {
            parsed = parsed * 10u + (uint32_t)(bootargs[cursor] - '0');
            if (parsed > 65535u)
                return false;
            cursor++;
        }
        if (cursor < length && bootargs[cursor] != ' ' &&
            bootargs[cursor] != '\0')
            return false;
        if (parsed == 0)
            return false;

        *value = (uint16_t)parsed;
        return true;
    }
    return false;
}

static void qemu_console_apply_boot_geometry(void)
{
    void *dtb = (void *)(uintptr_t)dtb_address;
    void *chosen;
    const char *bootargs;
    uint32_t length = 0;
    uint16_t rows;
    uint16_t cols;

    if (!fdt_check_header(dtb))
        return;
    chosen = fdt_find_node_by_name(dtb, "chosen");
    bootargs = (const char *)fdt_get_property(dtb, chosen, "bootargs", &length);
    if (!qemu_bootarg_u16(bootargs, length, "armos.tty0.rows=", &rows) ||
        !qemu_bootarg_u16(bootargs, length, "armos.tty0.cols=", &cols))
        return;

    (void)tty_set_winsize_for_id(TTY_CONSOLE_ID, rows, cols, 0, 0);
}

void platform_console_early_init(void)
{
    uart_init();
    uart_attach_tty_backend_to(TTY_CONSOLE_ID);
    tty_set_kernel_console(TTY_CONSOLE_ID, false);
    qemu_console_apply_boot_geometry();
}

void platform_console_enable_rx(void)
{
    uart_enable_rx_interrupts();
}

platform_devices_state_t platform_devices_init(void)
{
    platform_devices_state_t state = {0};

    init_keyboard();

    if (virtio_gpu_init()) {
        display_set_backend(&qemu_display_backend);
        KBOOT_OKF("GPU: virtio-gpu %ux%ux%u",
                  virtio_gpu_width(), virtio_gpu_height(), FB_BPP);
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

bool platform_device_irq_dispatch(uint32_t irq)
{
    if (!virtio_gpu_is_initialized() || irq != virtio_gpu_get_irq())
        return false;
    virtio_gpu_irq_handler();
    return true;
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
