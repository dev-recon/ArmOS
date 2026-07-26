/*
 * ArmOS
 * Copyright (c) 2026 Mohamed Ennassiri
 *
 * Licensed under the Apache License, Version 2.0.
 * See LICENSE for details.
 *
 * File: kernel/platform/raspberrypi/devices.c
 * Layer: Kernel / Raspberry Pi platform devices
 *
 * Responsibilities:
 * - Publish the devices shared by supported BCM2836/BCM2837 boards.
 * - Avoid probing qemu-virt-only devices on Raspberry Pi hardware.
 * - Reserve the primary framebuffer for the shell console milestone.
 */

#include <kernel/block_device.h>
#include <kernel/arch_platform.h>
#include <kernel/disk_layout.h>
#include <kernel/display.h>
#include <kernel/fdt.h>
#include <kernel/ili9341.h>
#include <kernel/kprintf.h>
#include <kernel/memory.h>
#include <kernel/mmc/bcm2835_emmc.h>
#include <kernel/mmc/bcm2835_sdhost.h>
#include <kernel/mmc/bcm2835_sdio.h>
#include <kernel/net/cyw43.h>
#include <kernel/platform_devices.h>
#include <kernel/raspberrypi_hdmi.h>
#include <kernel/string.h>
#include <kernel/tty.h>
#include <kernel/uart.h>
#include <kernel/usb.h>
#include <kernel/usb/dwc2.h>

static bool raspberrypi_block_uses_sdhost;
#if defined(ARMOS_PLATFORM_RASPI3) && defined(ARMOS_ENABLE_WIFI)
static bool raspberrypi_wifi_sdio_ready;
static bool raspberrypi_wifi_chip_ready;
#endif

#if defined(ARMOS_ENABLE_USB)
static bool raspberrypi_usb_node_available(void)
{
    void *dtb = (void *)(uintptr_t)dtb_address;
    void *node;
    const char *status;
    uint32_t status_length = 0;

    if (!arch_platform_has_usb() || !dtb_address || !fdt_check_header(dtb))
        return false;
    node = fdt_find_node_by_name(dtb, "usb");
    if (!node)
        return false;

    status = (const char *)fdt_get_property(dtb, node, "status",
                                            &status_length);
    if (!status || status_length == 0u)
        return true;
    return strcmp(status, "okay") == 0 || strcmp(status, "ok") == 0;
}
#endif

void platform_console_early_init(void)
{
    uart_init();
    uart_attach_tty_backend_to(TTY_SERIAL_ID);
}

void platform_console_enable_rx(void)
{
    uart_enable_rx_interrupts();
}

static void raspberrypi_use_uart_fallback_console(void)
{
    uart_attach_tty_backend_to(TTY_CONSOLE_ID);
    tty_set_active(TTY_CONSOLE_ID);
    tty_set_kernel_console(TTY_CONSOLE_ID, false);
    KBOOT_WARN("TTY: display unavailable, tty0 using PL011 uart0");
}

platform_devices_state_t platform_devices_init(void)
{
    /*
     * Keep compositor_allowed false: the Raspberry Pi primary framebuffer
     * remains owned by the HDMI/ILI9341 shell until Wayland support on the
     * physical platforms is explicitly enabled and validated.
     */
    platform_devices_state_t state = {0};
#if defined(ARMOS_ENABLE_HDMI) || \
    (defined(ARMOS_PLATFORM_RASPI3) && defined(ARMOS_ENABLE_ILI9341))
    bool primary_display_ready = false;
#endif

    KBOOT_OK("TTY: recovery serial on PL011 /dev/ttyS0");
#if defined(ARMOS_ENABLE_HDMI)
    if (raspberrypi_hdmi_init(ARMOS_HDMI_WIDTH, ARMOS_HDMI_HEIGHT)) {
        const raspberrypi_hdmi_info_t *hdmi = raspberrypi_hdmi_get_info();

        if (hdmi && init_display_external(hdmi->width, hdmi->height,
                                          hdmi->bpp, hdmi->pitch,
                                          hdmi->physical,
                                          hdmi->virtual_address,
                                          hdmi->size)) {
            display_set_backend(raspberrypi_hdmi_display_backend());
            display_flush_all();
            KBOOT_OKF("GPU: firmware HDMI %ux%ux%u, virtual height %u",
                      hdmi->width, hdmi->height, hdmi->bpp,
                      hdmi->virtual_height);
            if (framebuffer_attach_tty_backend(TTY_CONSOLE_ID) == 0) {
                state.display_ready = true;
                primary_display_ready = true;
                tty_set_active(TTY_CONSOLE_ID);
                tty_set_kernel_console(TTY_CONSOLE_ID, true);
                if (uart_mirror_tty_output_to(TTY_CONSOLE_ID) == 0) {
                    KBOOT_OK("TTY: console tty0 on HDMI /dev/fb0");
                    KBOOT_OK("TTY: tty0 output mirrored on PL011 /dev/ttyS0");
                } else {
                    KBOOT_OK("TTY: console tty0 on HDMI /dev/fb0");
                    KBOOT_WARN("TTY: PL011 output mirror unavailable");
                }
            } else {
                KBOOT_WARN("TTY: tty0 framebuffer backend unavailable");
            }
        } else {
            KBOOT_WARN("GPU: HDMI framebuffer attachment failed");
        }
    } else {
        KBOOT_WARN("GPU: firmware HDMI unavailable");
    }
#endif

#if defined(ARMOS_PLATFORM_RASPI3) && defined(ARMOS_ENABLE_ILI9341)
    if (primary_display_ready) {
        if (ili9341_init() &&
            ili9341_attach_auxiliary_tty(TTY_GRAPHICS_ID) == 0) {
            KBOOT_OKF("GPU: auxiliary ILI9341 %ux%ux%u on /dev/fb1",
                      ILI9341_WIDTH, ILI9341_HEIGHT, FB_BPP);
            KBOOT_OK("TTY: output-only tty1 on ILI9341 /dev/fb1");
        } else {
            KBOOT_WARN("GPU: auxiliary ILI9341 unavailable");
        }
    } else if (ili9341_init() &&
               init_display(ILI9341_WIDTH, ILI9341_HEIGHT, FB_BPP)) {
        display_set_backend(ili9341_display_backend());
        display_flush_all();
        KBOOT_OKF("GPU: ILI9341 GPIO parallel %ux%ux%u",
                  ILI9341_WIDTH, ILI9341_HEIGHT, FB_BPP);
        if (framebuffer_attach_tty_backend(TTY_CONSOLE_ID) == 0) {
            state.display_ready = true;
            primary_display_ready = true;
            tty_set_active(TTY_CONSOLE_ID);
            tty_set_kernel_console(TTY_CONSOLE_ID, true);
            KBOOT_OK("TTY: console tty0 on ILI9341 /dev/fb0");
        } else {
            KBOOT_WARN("TTY: tty0 framebuffer backend unavailable");
        }
    } else {
        KBOOT_WARN("GPU: ILI9341 GPIO display unavailable");
    }
    KBOOT_WARN("Input: GPIO display is output-only");
#endif

#if defined(ARMOS_ENABLE_HDMI) || \
    (defined(ARMOS_PLATFORM_RASPI3) && defined(ARMOS_ENABLE_ILI9341))
    if (!primary_display_ready) {
#if !defined(ARMOS_ENABLE_HDMI) && !defined(ARMOS_ENABLE_ILI9341)
        KBOOT_WARN("GPU: Raspberry Pi display disabled");
        KBOOT_WARNF("Input: unavailable on %s milestone 1",
                    arch_platform_name());
#else
        KBOOT_WARN("GPU: no configured Raspberry Pi display available");
#endif
    }
#else
    KBOOT_WARNF("GPU: unavailable on %s milestone 1", arch_platform_name());
    KBOOT_WARNF("Input: unavailable on %s milestone 1", arch_platform_name());
#endif

    if (!state.display_ready)
        raspberrypi_use_uart_fallback_console();

#if defined(ARMOS_PLATFORM_RASPI3) && defined(ARMOS_ENABLE_WIFI)
    KBOOT_WARN("Net: CYW43455 configured, probe follows block init");
#else
    KBOOT_WARNF("Net: unavailable on %s milestone 1", arch_platform_name());
#endif

#if defined(ARMOS_ENABLE_USB)
    if (!raspberrypi_usb_node_available()) {
        KBOOT_WARN("USB: enabled DWC2 controller not found in boot DTB");
    } else if (dwc2_usb_register(TTY_CONSOLE_ID) == 0) {
        KBOOT_OK("USB: DWC2 host registered from boot DTB");
    } else {
        KBOOT_WARN("USB: DWC2 host registration failed");
    }
#endif

    return state;
}

bool platform_block_init(void)
{
#if defined(ARMOS_PLATFORM_RASPI3) && defined(ARMOS_ENABLE_WIFI)
    if (bcm2835_sdhost_init()) {
        raspberrypi_block_uses_sdhost = true;
        KINFO("WiFi: system SD moved to BCM2835 SDHOST\n");
    } else {
        raspberrypi_block_uses_sdhost = false;
        KBOOT_WARN("WiFi: SDHOST boot path failed, reserving Wi-Fi probe");
        KBOOT_WARN("Block: falling back to Arasan SDHCI for the SD card");
    }
#else
    raspberrypi_block_uses_sdhost = false;
#endif

    if (!raspberrypi_block_uses_sdhost && !bcm2835_emmc_init()) {
        KBOOT_WARN("Block: SD card unavailable");
        return false;
    }

    if (!disk_layout_init_from_mbr()) {
        KBOOT_WARN("Block: SD card present, MBR unavailable");
        return false;
    }

#if defined(ARMOS_PLATFORM_RASPI3) && defined(ARMOS_ENABLE_WIFI)
    if (raspberrypi_block_uses_sdhost) {
        bcm2835_sdio_identity_t identity = {0};
        cyw43_identity_t chip = {0};

        raspberrypi_wifi_sdio_ready = bcm2835_sdio_init(&identity);
        if (raspberrypi_wifi_sdio_ready) {
            KBOOT_OKF("WiFi: SDIO funcs=%u rca=0x%04X id=%04X:%04X "
                      "SDIO=%u CCCR=%u",
                      identity.functions, identity.rca,
                      identity.manufacturer, identity.product,
                      identity.sdio_revision, identity.cccr_revision);
            raspberrypi_wifi_chip_ready = cyw43_probe(&chip);
            if (raspberrypi_wifi_chip_ready) {
                KBOOT_OKF("WiFi: CYW43455 chip=0x%04X rev=%u pkg=%u "
                          "signature=0x%08X",
                          chip.chip_id, chip.chip_revision, chip.package,
                          chip.chip_id_register);
                KBOOT_WARN("Net: CYW43455 firmware and BCDC pending");
            } else {
                KBOOT_WARN("WiFi: Broadcom backplane probe failed");
            }
        } else {
            KBOOT_WARN("WiFi: CYW43455 SDIO probe failed");
        }
    }
#endif

    KBOOT_OKF("Block: %s %uMB on SD",
              blk_get_name(),
              (uint32_t)((blk_get_capacity_sectors() * blk_get_sector_size()) /
                         (1024ULL * 1024ULL)));
    return true;
}

void platform_block_shutdown(void)
{
#if defined(ARMOS_PLATFORM_RASPI3) && defined(ARMOS_ENABLE_WIFI)
    if (raspberrypi_block_uses_sdhost) {
        if (raspberrypi_wifi_chip_ready)
            cyw43_shutdown();
        if (raspberrypi_wifi_sdio_ready)
            bcm2835_sdio_shutdown();
        bcm2835_sdhost_shutdown();
    } else {
        bcm2835_emmc_shutdown();
    }
#else
    bcm2835_emmc_shutdown();
#endif
}
