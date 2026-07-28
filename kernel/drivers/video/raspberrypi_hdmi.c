/*
 * ArmOS
 * Copyright (c) 2026 Mohamed Ennassiri
 *
 * Licensed under the Apache License, Version 2.0.
 * See LICENSE for details.
 *
 * File: kernel/drivers/video/raspberrypi_hdmi.c
 * Layer: Kernel / Raspberry Pi display driver
 *
 * Responsibilities:
 * - Configure and allocate a 32-bit HDMI framebuffer through VideoCore.
 * - Make CPU-rendered dirty rectangles visible to the firmware display scanout.
 *
 * Notes:
 * - This is the firmware framebuffer path, intentionally preceding a native
 *   VC4/KMS driver. It supports ordinary HDMI monitors without a GPU stack.
 */

#include <kernel/address_space.h>
#include <kernel/arch_barrier.h>
#include <kernel/kprintf.h>
#include <kernel/raspberrypi_hdmi.h>
#include <kernel/raspberrypi_mailbox.h>
#include <kernel/string.h>

#define RPI_TAG_SET_PHYSICAL_SIZE 0x00048003u
#define RPI_TAG_SET_VIRTUAL_SIZE  0x00048004u
#define RPI_TAG_SET_DEPTH         0x00048005u
#define RPI_TAG_SET_PIXEL_ORDER   0x00048006u
#define RPI_TAG_SET_VIRTUAL_OFF   0x00048009u
#define RPI_TAG_ALLOCATE_BUFFER   0x00040001u
#define RPI_TAG_GET_PITCH         0x00040008u
#define RPI_PIXEL_ORDER_BGR       0u
#define RPI_GPU_BUS_ADDRESS_MASK  0x3fffffffu
#define RPI_HDMI_SCROLL_BUFFERS   2u

static volatile uint32_t hdmi_message[35] __attribute__((aligned(16)));
static volatile uint32_t hdmi_offset_message[8]
    __attribute__((aligned(16)));
static raspberrypi_hdmi_info_t hdmi;
static paddr_t hdmi_allocation_physical;
static uint8_t *hdmi_allocation_virtual;
static uint32_t hdmi_offset_y;
static bool hdmi_ready;

static bool hdmi_request_mode(uint32_t width, uint32_t height,
                              uint32_t virtual_height,
                              raspberrypi_hdmi_info_t *result)
{
    raspberrypi_hdmi_info_t candidate;
    uint32_t bus_address;

    if (!width || !height || virtual_height < height || !result)
        return false;

    for (uint32_t i = 0; i < 35u; i++)
        hdmi_message[i] = 0;

    hdmi_message[0] = sizeof(hdmi_message);
    hdmi_message[1] = 0;
    hdmi_message[2] = RPI_TAG_SET_PHYSICAL_SIZE;
    hdmi_message[3] = 8; hdmi_message[4] = 0;
    hdmi_message[5] = width; hdmi_message[6] = height;
    hdmi_message[7] = RPI_TAG_SET_VIRTUAL_SIZE;
    hdmi_message[8] = 8; hdmi_message[9] = 0;
    hdmi_message[10] = width; hdmi_message[11] = virtual_height;
    hdmi_message[12] = RPI_TAG_SET_VIRTUAL_OFF;
    hdmi_message[13] = 8; hdmi_message[14] = 0;
    hdmi_message[15] = 0; hdmi_message[16] = 0;
    hdmi_message[17] = RPI_TAG_SET_DEPTH;
    hdmi_message[18] = 4; hdmi_message[19] = 0; hdmi_message[20] = 32;
    hdmi_message[21] = RPI_TAG_SET_PIXEL_ORDER;
    hdmi_message[22] = 4; hdmi_message[23] = 0;
    /*
     * ArmOS exposes framebuffer pixels as 0xAARRGGBB.  On little-endian ARM,
     * requesting the firmware BGR byte order makes that in-memory contract
     * appear as RGB on the display.  Requesting RGB swaps red and blue.
     */
    hdmi_message[24] = RPI_PIXEL_ORDER_BGR;
    hdmi_message[25] = RPI_TAG_ALLOCATE_BUFFER;
    hdmi_message[26] = 8; hdmi_message[27] = 0;
    hdmi_message[28] = 4096; hdmi_message[29] = 0;
    hdmi_message[30] = RPI_TAG_GET_PITCH;
    hdmi_message[31] = 4; hdmi_message[32] = 0; hdmi_message[33] = 0;
    hdmi_message[34] = 0;

    if (!raspberrypi_property_call(hdmi_message, sizeof(hdmi_message)))
        return false;

    bus_address = hdmi_message[28];
    candidate.width = hdmi_message[5];
    candidate.height = hdmi_message[6];
    candidate.bpp = hdmi_message[20];
    candidate.size = hdmi_message[29];
    candidate.virtual_height = hdmi_message[11];
    candidate.pitch = hdmi_message[33];
    candidate.physical = (paddr_t)(bus_address & RPI_GPU_BUS_ADDRESS_MASK);
    candidate.virtual_address = (uint8_t *)phys_to_virt(candidate.physical);

    if (!bus_address || !candidate.size || candidate.bpp != 32u ||
        candidate.width != width || candidate.height != height ||
        candidate.virtual_height < height ||
        hdmi_message[24] != RPI_PIXEL_ORDER_BGR ||
        candidate.pitch != candidate.width * 4u ||
        (uint64_t)candidate.size <
            (uint64_t)candidate.pitch * candidate.virtual_height ||
        !phys_in_direct_map(candidate.physical) ||
        !phys_in_direct_map(candidate.physical + candidate.size - 1u)) {
        KERROR("HDMI: invalid firmware framebuffer bus=0x%08X phys=%p %ux%u pitch=%u size=%u\n",
               bus_address, (void *)(uintptr_t)candidate.physical,
               candidate.width, candidate.height,
               candidate.pitch, candidate.size);
        return false;
    }

    *result = candidate;
    return true;
}

static bool hdmi_set_virtual_offset(uint32_t y)
{
    for (uint32_t i = 0; i < 8u; i++)
        hdmi_offset_message[i] = 0;

    hdmi_offset_message[0] = sizeof(hdmi_offset_message);
    hdmi_offset_message[1] = 0;
    hdmi_offset_message[2] = RPI_TAG_SET_VIRTUAL_OFF;
    hdmi_offset_message[3] = 8;
    hdmi_offset_message[4] = 0;
    hdmi_offset_message[5] = 0;
    hdmi_offset_message[6] = y;
    hdmi_offset_message[7] = 0;

    if (!raspberrypi_property_call(hdmi_offset_message,
                                   sizeof(hdmi_offset_message)))
        return false;
    return hdmi_offset_message[5] == 0 && hdmi_offset_message[6] == y;
}

static int hdmi_flush_rect(const uint8_t *framebuffer, uint32_t pitch,
                           uint32_t x, uint32_t y,
                           uint32_t width, uint32_t height)
{
    if (!hdmi_ready || !framebuffer || !pitch)
        return -ENODEV;
    if (!width || !height || x >= hdmi.width || y >= hdmi.height)
        return 0;
    if (x + width > hdmi.width)
        width = hdmi.width - x;
    if (y + height > hdmi.height)
        height = hdmi.height - y;

    if (x == 0u && width * 4u == pitch) {
        const uint8_t *start = framebuffer + y * pitch;

        arch_clean_dcache_by_mva(start, (size_t)height * pitch);
    } else {
        const uint8_t *start = framebuffer + y * pitch + x * 4u;

        /*
         * Keep narrow damage precise, but issue the completion barrier once
         * for the rectangle rather than once per scanline.
         */
        arch_clean_dcache_2d_by_mva(start, pitch, width * 4u, height);
    }
    return 0;
}

static int hdmi_set_mode(uint32_t width, uint32_t height,
                         display_backend_mode_t *mode)
{
    raspberrypi_hdmi_info_t previous;
    raspberrypi_hdmi_info_t requested;
    raspberrypi_hdmi_info_t restored;

    if (!hdmi_ready || !mode)
        return -ENODEV;
    if (!width || !height)
        return -EINVAL;
    if (width == hdmi.width && height == hdmi.height) {
        requested = hdmi;
    } else {
        previous = hdmi;
        if (!hdmi_request_mode(width, height,
                               height * RPI_HDMI_SCROLL_BUFFERS,
                               &requested) &&
            !hdmi_request_mode(width, height, height, &requested)) {
            if (hdmi_request_mode(previous.width, previous.height,
                                  previous.virtual_height, &restored))
                hdmi = restored;
            return -EIO;
        }
        hdmi = requested;
        hdmi_allocation_physical = requested.physical;
        hdmi_allocation_virtual = requested.virtual_address;
        hdmi_offset_y = 0;
    }

    mode->width = requested.width;
    mode->height = requested.height;
    mode->pitch = requested.pitch;
    mode->bpp = requested.bpp;
    mode->size = requested.size;
    mode->physical = requested.physical;
    mode->virtual_address = requested.virtual_address;
    return 0;
}

static int hdmi_scroll_up(uint32_t rows, uint32_t clear_color,
                          display_backend_mode_t *mode)
{
    uint32_t next_offset;
    uint32_t retained_bytes;
    uint32_t visible_bytes;
    uint8_t *next_buffer;
    uint32_t *clear_start;

    if (!hdmi_ready || !mode)
        return -ENODEV;
    if (!rows || rows >= hdmi.height)
        return -EINVAL;
    if (hdmi.virtual_height < hdmi.height + rows)
        return -ENOTSUP;

    visible_bytes = hdmi.pitch * hdmi.height;
    retained_bytes = hdmi.pitch * (hdmi.height - rows);
    next_offset = hdmi_offset_y + rows;

    if (next_offset + hdmi.height <= hdmi.virtual_height) {
        next_buffer = hdmi_allocation_virtual + next_offset * hdmi.pitch;
        clear_start = (uint32_t *)(void *)(next_buffer + retained_bytes);
        for (uint32_t i = 0; i < hdmi.width * rows; i++)
            clear_start[i] = clear_color;
    } else {
        next_offset = 0;
        next_buffer = hdmi_allocation_virtual;
        memmove(next_buffer,
                hdmi.virtual_address + rows * hdmi.pitch,
                retained_bytes);
        clear_start = (uint32_t *)(void *)(next_buffer + retained_bytes);
        for (uint32_t i = 0; i < hdmi.width * rows; i++)
            clear_start[i] = clear_color;
    }

    /*
     * The retained rows may contain text drawn since displayd's last frame.
     * Make the complete next scanout page visible before changing the
     * firmware offset; otherwise short output bursts can expose stale glyphs.
     */
    arch_clean_dcache_by_mva(next_buffer, visible_bytes);
    arch_data_sync_barrier();

    if (!hdmi_set_virtual_offset(next_offset))
        return -EIO;

    hdmi_offset_y = next_offset;
    hdmi.physical = hdmi_allocation_physical + next_offset * hdmi.pitch;
    hdmi.virtual_address = next_buffer;

    mode->width = hdmi.width;
    mode->height = hdmi.height;
    mode->pitch = hdmi.pitch;
    mode->bpp = hdmi.bpp;
    mode->size = visible_bytes;
    mode->physical = hdmi.physical;
    mode->virtual_address = hdmi.virtual_address;
    return 0;
}

static const display_backend_ops_t hdmi_backend = {
    .name = "raspberrypi-hdmi",
    .flush_rect = hdmi_flush_rect,
    .check_resize = NULL,
    .set_orientation = NULL,
    .set_mode = hdmi_set_mode,
    .scroll_up = hdmi_scroll_up,
};

bool raspberrypi_hdmi_init(uint32_t width, uint32_t height)
{
    if (hdmi_ready)
        return true;
    if (!hdmi_request_mode(width, height,
                           height * RPI_HDMI_SCROLL_BUFFERS, &hdmi) &&
        !hdmi_request_mode(width, height, height, &hdmi))
        return false;

    hdmi_allocation_physical = hdmi.physical;
    hdmi_allocation_virtual = hdmi.virtual_address;
    hdmi_offset_y = 0;
    hdmi_ready = true;
    return true;
}

const raspberrypi_hdmi_info_t *raspberrypi_hdmi_get_info(void)
{
    return hdmi_ready ? &hdmi : NULL;
}

const display_backend_ops_t *raspberrypi_hdmi_display_backend(void)
{
    return &hdmi_backend;
}
