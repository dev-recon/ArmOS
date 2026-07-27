/*
 * ArmOS
 * Copyright (c) 2026 Mohamed Ennassiri
 *
 * Licensed under the Apache License, Version 2.0.
 * See LICENSE for details.
 *
 * File: kernel/drivers/usb/dwc2.c
 * Layer: Kernel / Raspberry Pi USB host driver
 *
 * Responsibilities:
 * - Bring up the BCM2837 DesignWare USB 2.0 controller in host mode.
 * - Enumerate the Raspberry Pi 3 internal hub and downstream HID devices.
 * - Service boot-protocol keyboard and mouse endpoints when called by usbd.
 *
 * Notes:
 * - The first milestone deliberately uses one DMA host channel and polling.
 *   This keeps controller, hub, and split-transaction failures observable
 *   before interrupt-driven scheduling is introduced.
 * - Full/low-speed devices behind the Pi 3 high-speed hub use USB split
 *   transactions. The Logitech receiver is expected to exercise this path.
 * - An interrupt-endpoint NAK is an idle poll result. It is returned to usbd
 *   immediately so one quiet HID endpoint cannot stall the other devices.
 */

#include <kernel/arch_barrier.h>
#include <kernel/arch_memory.h>
#include <kernel/arch_platform.h>
#include <kernel/address_space.h>
#include <kernel/display.h>
#include <kernel/input.h>
#include <kernel/kprintf.h>
#include <kernel/raspberrypi_mailbox.h>
#include <kernel/string.h>
#include <kernel/timer.h>
#include <kernel/tty.h>
#include <kernel/usb.h>
#include <kernel/usb/dwc2.h>

#define DWC2_GOTGCTL       0x000u
#define DWC2_GAHBCFG       0x008u
#define DWC2_GUSBCFG       0x00cu
#define DWC2_GRSTCTL       0x010u
#define DWC2_GINTSTS       0x014u
#define DWC2_GINTMSK       0x018u
#define DWC2_GRXFSIZ       0x024u
#define DWC2_GNPTXFSIZ     0x028u
#define DWC2_GSNPSID       0x040u
#define DWC2_GHWCFG2       0x048u
#define DWC2_GHWCFG3       0x04cu
#define DWC2_HPTXFSIZ      0x100u
#define DWC2_HCFG          0x400u
#define DWC2_HFIR          0x404u
#define DWC2_HFNUM         0x408u
#define DWC2_HAINTMSK      0x418u
#define DWC2_HPRT          0x440u
#define DWC2_HC_BASE       0x500u
#define DWC2_HC_STRIDE     0x020u
#define DWC2_HCCHAR        0x00u
#define DWC2_HCSPLT        0x04u
#define DWC2_HCINT         0x08u
#define DWC2_HCINTMSK      0x0cu
#define DWC2_HCTSIZ        0x10u
#define DWC2_HCDMA         0x14u
#define DWC2_PCGCCTL       0xe00u

#define GRSTCTL_CSFTRST    (1u << 0)
#define GRSTCTL_RXFFLSH    (1u << 4)
#define GRSTCTL_TXFFLSH    (1u << 5)
#define GRSTCTL_TXFNUM_SHIFT 6u
#define GRSTCTL_TXFNUM_ALL (0x10u << GRSTCTL_TXFNUM_SHIFT)
#define GRSTCTL_AHBIDLE    (1u << 31)
#define GOTGCTL_HSTSETHNPEN (1u << 10)
#define GINTSTS_HOST_MODE  (1u << 0)
#define GUSBCFG_PHY_IF_16  (1u << 3)
#define GUSBCFG_ULPI_PHY   (1u << 4)
#define GUSBCFG_FS_PHY     (1u << 6)
#define GUSBCFG_SRP_CAP    (1u << 8)
#define GUSBCFG_HNP_CAP    (1u << 9)
#define GUSBCFG_ULPI_FSLS  (1u << 17)
#define GUSBCFG_ULPI_CLK_SUS (1u << 19)
#define GUSBCFG_ULPI_EXT_VBUS (1u << 20)
#define GUSBCFG_TERM_SEL_DL_PULSE (1u << 22)
#define GUSBCFG_FORCE_DEV  (1u << 30)
#define GAHBCFG_GLOBAL_INTR_EN (1u << 0)
#define GAHBCFG_MAX_AXI_BURST_MASK (3u << 1)
#define GAHBCFG_WAIT_AXI_WRITES (1u << 4)
#define GAHBCFG_DMA_EN     (1u << 5)
#define GHWCFG2_DYNAMIC_FIFO (1u << 19)
#define GHWCFG2_ARCH_SHIFT 3u
#define GHWCFG2_ARCH_MASK  (3u << GHWCFG2_ARCH_SHIFT)
#define GHWCFG2_ARCH_INTERNAL_DMA 2u
#define GHWCFG2_HOST_CHANNEL_SHIFT 14u
#define GHWCFG2_HOST_CHANNEL_MASK  (0x0fu << GHWCFG2_HOST_CHANNEL_SHIFT)
#define GHWCFG2_HS_PHY_SHIFT 6u
#define GHWCFG2_HS_PHY_MASK  (3u << GHWCFG2_HS_PHY_SHIFT)
#define GHWCFG2_FS_PHY_SHIFT 8u
#define GHWCFG2_FS_PHY_MASK  (3u << GHWCFG2_FS_PHY_SHIFT)
#define GHWCFG2_HS_PHY_ULPI  2u
#define GHWCFG2_HS_PHY_NONE  0u
#define GHWCFG2_FS_PHY_DEDICATED 1u
#define GHWCFG3_DFIFO_DEPTH_SHIFT 16u
#define GHWCFG3_DFIFO_DEPTH_MASK  (0xffffu << GHWCFG3_DFIFO_DEPTH_SHIFT)
#define HCFG_FSLS_CLOCK_MASK 3u
#define HCFG_FSLS_CLOCK_30_60_MHZ 0u
#define HCFG_FSLS_CLOCK_48_MHZ 1u
#define HPRT_CONN          (1u << 0)
#define HPRT_CONN_CHG      (1u << 1)
#define HPRT_ENABLE        (1u << 2)
#define HPRT_ENABLE_CHG    (1u << 3)
#define HPRT_OVERCUR_CHG   (1u << 5)
#define HPRT_RESET         (1u << 8)
#define HPRT_POWER         (1u << 12)
#define HPRT_SPEED_SHIFT   17u
#define HPRT_SPEED_MASK    (3u << HPRT_SPEED_SHIFT)

#define HCCHAR_EP_SHIFT    11u
#define HCCHAR_DIR_IN      (1u << 15)
#define HCCHAR_LOW_SPEED   (1u << 17)
#define HCCHAR_TYPE_SHIFT  18u
#define HCCHAR_MULTICNT_1  (1u << 20)
#define HCCHAR_ODD_FRAME   (1u << 29)
#define HCCHAR_DEV_SHIFT   22u
#define HCCHAR_CHDIS       (1u << 30)
#define HCCHAR_CHENA       (1u << 31)
#define HCTSIZ_PKTCNT_SHIFT 19u
#define HCTSIZ_PID_SHIFT   29u
#define HCSPLT_PORT_SHIFT  0u
#define HCSPLT_HUB_SHIFT   7u
#define HCSPLT_XACT_ALL    (3u << 14)
#define HCSPLT_COMPLETE    (1u << 16)
#define HCSPLT_ENABLE      (1u << 31)

#define HCINT_XFRC         (1u << 0)
#define HCINT_CHH          (1u << 1)
#define HCINT_STALL        (1u << 3)
#define HCINT_NAK          (1u << 4)
#define HCINT_ACK          (1u << 5)
#define HCINT_NYET         (1u << 6)
#define HCINT_XACTERR      (1u << 7)
#define HCINT_FRMOVRUN     (1u << 9)
#define HCINT_FATAL_ERRORS ((1u << 2) | (1u << 8) | (1u << 10))

#define USB_DIR_IN         0x80u
#define USB_TYPE_CLASS     0x20u
#define USB_RECIP_INTERFACE 0x01u
#define USB_RECIP_OTHER    0x03u
#define USB_REQ_GET_STATUS 0x00u
#define USB_REQ_CLEAR_FEATURE 0x01u
#define USB_REQ_SET_FEATURE 0x03u
#define USB_REQ_SET_ADDRESS 0x05u
#define USB_REQ_GET_DESCRIPTOR 0x06u
#define USB_REQ_SET_CONFIGURATION 0x09u
#define USB_REQ_SET_IDLE   0x0au
#define USB_REQ_SET_PROTOCOL 0x0bu
#define USB_DESC_DEVICE    0x01u
#define USB_DESC_CONFIG    0x02u
#define USB_DESC_STRING    0x03u
#define USB_DESC_INTERFACE 0x04u
#define USB_DESC_ENDPOINT  0x05u
#define USB_DESC_HUB       0x29u
#define USB_CLASS_HUB      0x09u
#define USB_CLASS_HID      0x03u
#define USB_HID_BOOT       0x01u
#define USB_HID_KEYBOARD   0x01u
#define USB_HID_MOUSE      0x02u
#define USB_HUB_PORT_POWER 8u
#define USB_HUB_PORT_RESET 4u
#define USB_HUB_C_PORT_CONNECTION 16u
#define USB_HUB_C_PORT_RESET 20u
#define USB_HUB_PORT_CONNECTION (1u << 0)
#define USB_HUB_PORT_ENABLE     (1u << 1)
#define USB_HUB_PORT_RESETTING  (1u << 4)
#define USB_HUB_PORT_LOW_SPEED  (1u << 9)
#define USB_HUB_PORT_HIGH_SPEED (1u << 10)
#define USB_POWER_USB_HCD  3u
#define USB_POWER_ON       1u

#define USB_SPEED_HIGH     0u
#define USB_SPEED_FULL     1u
#define USB_SPEED_LOW      2u
#define USB_PID_DATA0      0u
#define USB_PID_DATA1      2u
#define USB_PID_SETUP      3u
#define USB_EP_CONTROL     0u
#define USB_EP_INTERRUPT   3u
#define USB_MAX_CONFIG     512u
#define USB_MAX_HID        4u
#define USB_ROOT_CONNECT_WAIT_MS 1000u
#define USB_ROOT_ENABLE_WAIT_MS   500u
#define USB_HUB_RESET_WAIT_MS     200u
#define USB_CHANNEL_WAIT_MS       1000u
#define USB_TRANSACTION_RETRIES   3u
#define USB_MAX_HUB_DEPTH         4u
#define USB_MAX_ADDRESS           127u
#define USB_KEY_REPEAT_DELAY_MS   500u
#define USB_KEY_REPEAT_RATE_MS    33u
#define USB_HID_MIN_POLL_MS       8u
#define USB_IDLE_POLL_MS          100u

/* Raspberry Pi's legacy DWC driver defaults, in 32-bit FIFO words. */
#define DWC2_HOST_RX_FIFO_WORDS   774u
#define DWC2_HOST_NPTX_FIFO_WORDS 256u
#define DWC2_HOST_PTX_FIFO_WORDS  512u

/* BCM2837 /soc dma-ranges maps ARM RAM at bus address 0xc0000000. */
#define RPI_DMA_BUS_ALIAS         0xc0000000u
#define RPI_DMA_PHYSICAL_LIMIT    0x3f000000u

typedef struct usb_setup_packet {
    uint8_t request_type;
    uint8_t request;
    uint16_t value;
    uint16_t index;
    uint16_t length;
} __attribute__((packed)) usb_setup_packet_t;

typedef struct usb_device {
    uint8_t address;
    uint8_t speed;
    uint8_t max_packet;
    uint8_t hub_address;
    uint8_t hub_port;
    uint8_t parent_address;
    uint8_t parent_port;
} usb_device_t;

typedef struct usb_hid_endpoint {
    usb_device_t device;
    uint8_t interface_number;
    uint8_t protocol;
    uint8_t endpoint;
    uint8_t interval;
    uint16_t max_packet;
    uint8_t data_pid;
    uint16_t poll_interval_ms;
    uint64_t next_poll_tick;
    uint64_t repeat_next_tick;
    uint8_t repeat_usage;
    uint8_t previous[8];
    uint8_t report[16] __attribute__((aligned(64)));
} usb_hid_endpoint_t;

static volatile uint32_t *dwc2;
static usb_hid_endpoint_t hid_endpoints[USB_MAX_HID];
static uint32_t hid_count;
static uint32_t keyboard_count;
static uint32_t mouse_count;
static int usb_tty_id;
static bool usb_ready;
static usb_setup_packet_t setup_packet __attribute__((aligned(64)));
static uint8_t descriptor_buffer[USB_MAX_CONFIG] __attribute__((aligned(64)));
static uint8_t control_dma_buffer[USB_MAX_CONFIG] __attribute__((aligned(64)));
static uint8_t status_dma_buffer[64] __attribute__((aligned(64)));
static const char *last_control_phase = "none";
static uint32_t expected_usb_config;
static uint32_t expected_ahb_config;
static uint32_t expected_host_config;
static uint8_t next_usb_address;

static inline uint32_t reg_read(uint32_t offset)
{
    return dwc2[offset / 4u];
}

static inline void reg_write(uint32_t offset, uint32_t value)
{
    dwc2[offset / 4u] = value;
}

static bool usb_init_register_failure(const char *stage)
{
    KERROR("USB DWC2: %s base=%p snpsid=0x%08X grstctl=0x%08X "
           "gusbcfg=0x%08X gahbcfg=0x%08X ghwcfg2=0x%08X hcfg=0x%08X "
           "gintsts=0x%08X hprt=0x%08X hfir=0x%08X grxfsiz=0x%08X "
           "gnptxfsiz=0x%08X hptxfsiz=0x%08X\n",
           stage, (void *)(uintptr_t)dwc2,
           reg_read(DWC2_GSNPSID), reg_read(DWC2_GRSTCTL),
           reg_read(DWC2_GUSBCFG), reg_read(DWC2_GAHBCFG),
           reg_read(DWC2_GHWCFG2), reg_read(DWC2_HCFG),
           reg_read(DWC2_GINTSTS), reg_read(DWC2_HPRT),
           reg_read(DWC2_HFIR),
           reg_read(DWC2_GRXFSIZ), reg_read(DWC2_GNPTXFSIZ),
           reg_read(DWC2_HPTXFSIZ));
    return false;
}

static bool usb_enumeration_failure(const char *stage, int result)
{
    uint32_t base = DWC2_HC_BASE;

    KERROR("USB DWC2: %s failed ret=%d phase=%s hcint=0x%08X "
           "hctsiz=0x%08X hcchar=0x%08X hcsplt=0x%08X hcdma=0x%08X\n",
           stage, result, last_control_phase,
           reg_read(base + DWC2_HCINT), reg_read(base + DWC2_HCTSIZ),
           reg_read(base + DWC2_HCCHAR), reg_read(base + DWC2_HCSPLT),
           reg_read(base + DWC2_HCDMA));
    return false;
}

static void usb_delay_ms(uint32_t milliseconds)
{
    uint64_t start = get_timer_count();
    uint32_t frequency = get_timer_frequency();
    uint32_t per_ms = frequency / 1000u;
    uint32_t remainder = frequency % 1000u;
    uint64_t duration = (uint64_t)per_ms * milliseconds +
        (remainder * milliseconds + 999u) / 1000u;

    if (!duration)
        duration = 1;
    while ((get_timer_count() - start) < duration)
        arch_cpu_relax();
}

static uint64_t timer_ticks_from_ms(uint32_t milliseconds)
{
    uint32_t frequency = get_timer_frequency();
    uint32_t per_ms = frequency / 1000u;
    uint32_t remainder = frequency % 1000u;
    uint64_t ticks = (uint64_t)per_ms * milliseconds +
        (remainder * milliseconds + 999u) / 1000u;

    return ticks != 0u ? ticks : 1u;
}

static uint32_t timer_ms_until(uint64_t now, uint64_t deadline)
{
    uint32_t frequency;
    uint32_t ticks_per_ms;
    uint32_t milliseconds = 1u;
    uint64_t delta;

    if (deadline <= now)
        return 1u;
    frequency = get_timer_frequency();
    if (frequency == 0u)
        return 1u;
    ticks_per_ms = frequency / 1000u;
    if (ticks_per_ms == 0u)
        ticks_per_ms = 1u;
    delta = deadline - now;
    while (delta > ticks_per_ms && milliseconds < USB_IDLE_POLL_MS) {
        delta -= ticks_per_ms;
        milliseconds++;
    }
    return milliseconds;
}

static void usb_delay_us(uint32_t microseconds)
{
    uint64_t start = get_timer_count();
    uint32_t frequency = get_timer_frequency();
    uint32_t per_us = frequency / 1000000u;
    uint32_t remainder = frequency % 1000000u;
    uint64_t duration = (uint64_t)per_us * microseconds +
        (remainder * microseconds + 999999u) / 1000000u;

    if (!duration)
        duration = 1;
    while ((get_timer_count() - start) < duration)
        arch_cpu_relax();
}

static bool dwc2_restore_host_state(const char *stage)
{
    uint32_t usb_config = reg_read(DWC2_GUSBCFG);
    uint32_t ahb_config = reg_read(DWC2_GAHBCFG);
    uint32_t host_config = reg_read(DWC2_HCFG);

    if (usb_config == expected_usb_config &&
        ahb_config == expected_ahb_config &&
        host_config == expected_host_config)
        return true;

    KWARN("USB DWC2: host state changed at %s "
          "gusbcfg=0x%08X->0x%08X gahbcfg=0x%08X->0x%08X "
          "hcfg=0x%08X->0x%08X\n",
          stage, usb_config, expected_usb_config,
          ahb_config, expected_ahb_config,
          host_config, expected_host_config);
    reg_write(DWC2_GUSBCFG, expected_usb_config);
    reg_write(DWC2_GAHBCFG, expected_ahb_config);
    reg_write(DWC2_HCFG, expected_host_config);
    arch_data_sync_barrier();

    return reg_read(DWC2_GUSBCFG) == expected_usb_config &&
           reg_read(DWC2_GAHBCFG) == expected_ahb_config &&
           reg_read(DWC2_HCFG) == expected_host_config;
}

static bool dwc2_core_reset(void)
{
    uint32_t timeout = 1000000u;

    while (!(reg_read(DWC2_GRSTCTL) & GRSTCTL_AHBIDLE) && --timeout)
        arch_cpu_relax();
    if (!timeout)
        return false;

    reg_write(DWC2_GRSTCTL, GRSTCTL_CSFTRST);
    timeout = 1000000u;
    while ((reg_read(DWC2_GRSTCTL) & GRSTCTL_CSFTRST) && --timeout)
        arch_cpu_relax();
    if (!timeout)
        return false;

    /* DWC2 requires a long settling delay to remain in host mode. */
    usb_delay_ms(100);
    return true;
}

static bool dwc2_flush_fifos(void)
{
    uint32_t timeout = 1000000u;

    reg_write(DWC2_GRSTCTL, GRSTCTL_TXFFLSH | GRSTCTL_TXFNUM_ALL);
    while ((reg_read(DWC2_GRSTCTL) & GRSTCTL_TXFFLSH) && --timeout)
        arch_cpu_relax();
    if (!timeout)
        return false;
    usb_delay_us(1);

    timeout = 1000000u;
    reg_write(DWC2_GRSTCTL, GRSTCTL_RXFFLSH);
    while ((reg_read(DWC2_GRSTCTL) & GRSTCTL_RXFFLSH) && --timeout)
        arch_cpu_relax();
    if (!timeout)
        return false;
    usb_delay_us(1);
    return true;
}

static bool dwc2_configure_host_fifos(void)
{
    uint32_t hw_config2 = reg_read(DWC2_GHWCFG2);
    uint32_t fifo_depth =
        (reg_read(DWC2_GHWCFG3) & GHWCFG3_DFIFO_DEPTH_MASK) >>
        GHWCFG3_DFIFO_DEPTH_SHIFT;
    uint32_t required = DWC2_HOST_RX_FIFO_WORDS +
        DWC2_HOST_NPTX_FIFO_WORDS + DWC2_HOST_PTX_FIFO_WORDS;
    uint32_t nptx = (DWC2_HOST_NPTX_FIFO_WORDS << 16) |
        DWC2_HOST_RX_FIFO_WORDS;
    uint32_t ptx = (DWC2_HOST_PTX_FIFO_WORDS << 16) |
        (DWC2_HOST_RX_FIFO_WORDS + DWC2_HOST_NPTX_FIFO_WORDS);

    if (!(hw_config2 & GHWCFG2_DYNAMIC_FIFO))
        return true;
    if (fifo_depth < required)
        return false;

    reg_write(DWC2_GRXFSIZ, DWC2_HOST_RX_FIFO_WORDS);
    reg_write(DWC2_GNPTXFSIZ, nptx);
    reg_write(DWC2_HPTXFSIZ, ptx);
    arch_data_sync_barrier();

    return reg_read(DWC2_GRXFSIZ) == DWC2_HOST_RX_FIFO_WORDS &&
           reg_read(DWC2_GNPTXFSIZ) == nptx &&
           reg_read(DWC2_HPTXFSIZ) == ptx;
}

static bool dwc2_reset_host_channels(void)
{
    uint32_t channel_count =
        ((reg_read(DWC2_GHWCFG2) & GHWCFG2_HOST_CHANNEL_MASK) >>
         GHWCFG2_HOST_CHANNEL_SHIFT) + 1u;

    if (channel_count == 0u || channel_count > 16u)
        return false;

    /*
     * Drain firmware requests first, then halt every channel.  This is the
     * host-init sequence shared by Circle, CSUD and U-Boot for DWC2.
     */
    for (uint32_t channel = 0; channel < channel_count; channel++) {
        uint32_t base = DWC2_HC_BASE + channel * DWC2_HC_STRIDE;
        uint32_t value = reg_read(base + DWC2_HCCHAR);

        value &= ~(HCCHAR_CHENA | HCCHAR_DIR_IN);
        value |= HCCHAR_CHDIS;
        reg_write(base + DWC2_HCCHAR, value);
    }

    for (uint32_t channel = 0; channel < channel_count; channel++) {
        uint32_t base = DWC2_HC_BASE + channel * DWC2_HC_STRIDE;
        uint32_t value = reg_read(base + DWC2_HCCHAR);
        uint32_t timeout = 1000000u;

        value &= ~HCCHAR_DIR_IN;
        value |= HCCHAR_CHENA | HCCHAR_CHDIS;
        reg_write(base + DWC2_HCCHAR, value);
        while ((reg_read(base + DWC2_HCCHAR) & HCCHAR_CHENA) && --timeout)
            arch_cpu_relax();
        if (!timeout)
            return false;
        reg_write(base + DWC2_HCINT, 0xffffffffu);
        reg_write(base + DWC2_HCINTMSK, 0u);
    }

    return true;
}

static uint32_t hprt_write_value(uint32_t value)
{
    /*
     * HPRT is not a conventional read/write register.  Change bits are
     * write-one-to-clear and writing one to PRTENA disables the port.  Keep
     * those status bits out of every control write assembled from a read.
     */
    value &= ~(HPRT_CONN_CHG | HPRT_ENABLE | HPRT_ENABLE_CHG |
               HPRT_OVERCUR_CHG);
    return value;
}

static int channel_wait(uint32_t channel, uint32_t *status)
{
    uint32_t base = DWC2_HC_BASE + channel * DWC2_HC_STRIDE;
    uint64_t start = get_timer_count();
    uint64_t timeout_ticks = timer_ticks_from_ms(USB_CHANNEL_WAIT_MS);

    while ((get_timer_count() - start) < timeout_ticks) {
        uint32_t value = reg_read(base + DWC2_HCINT);
        if (value & HCINT_CHH) {
            *status = value;
            reg_write(base + DWC2_HCINT, value);
            return 0;
        }
        arch_cpu_relax();
    }
    return -ETIMEDOUT;
}

static uint32_t current_microframe(void)
{
    return reg_read(DWC2_HFNUM) & 7u;
}

static bool wait_for_microframe(uint32_t target)
{
    uint64_t start = get_timer_count();
    uint64_t timeout_ticks = timer_ticks_from_ms(2u);

    target &= 7u;
    while ((get_timer_count() - start) < timeout_ticks) {
        if (current_microframe() == target)
            return true;
        arch_cpu_relax();
    }
    return false;
}

static uint32_t next_split_microframe(void)
{
    uint32_t target = (current_microframe() + 1u) & 7u;

    /* Periodic start-splits must not be issued in microframe 6. */
    if (target == 6u)
        target = 7u;
    return target;
}

static int channel_once(const usb_device_t *device, uint8_t endpoint,
                        bool input, uint8_t endpoint_type, uint8_t pid,
                        void *buffer, uint32_t length, uint16_t max_packet,
                        bool complete_split, uint32_t *status,
                        uint32_t *actual_length)
{
    uint32_t base = DWC2_HC_BASE;
    uint32_t packets = length ? (length + max_packet - 1u) / max_packet : 1u;
    uint32_t hcchar = max_packet |
        ((uint32_t)endpoint << HCCHAR_EP_SHIFT) |
        ((uint32_t)endpoint_type << HCCHAR_TYPE_SHIFT) |
        HCCHAR_MULTICNT_1 |
        ((uint32_t)device->address << HCCHAR_DEV_SHIFT);
    uint32_t split = 0;
    paddr_t physical = virt_to_phys((vaddr_t)(uintptr_t)buffer);
    uint32_t dma_address;

    if (!dwc2_restore_host_state("channel start"))
        return -EIO;

    if (physical >= RPI_DMA_PHYSICAL_LIMIT ||
        length > RPI_DMA_PHYSICAL_LIMIT - (uint32_t)physical)
        return -EFAULT;
    dma_address = RPI_DMA_BUS_ALIAS | (uint32_t)physical;

    if (input)
        hcchar |= HCCHAR_DIR_IN;
    if (endpoint_type == USB_EP_INTERRUPT &&
        !(reg_read(DWC2_HFNUM) & 1u))
        hcchar |= HCCHAR_ODD_FRAME;
    if (device->speed == USB_SPEED_LOW)
        hcchar |= HCCHAR_LOW_SPEED;
    if (device->hub_address) {
        split = HCSPLT_ENABLE | HCSPLT_XACT_ALL |
            ((uint32_t)device->hub_port << HCSPLT_PORT_SHIFT) |
            ((uint32_t)device->hub_address << HCSPLT_HUB_SHIFT);
        if (complete_split)
            split |= HCSPLT_COMPLETE;
    }

    arch_clean_invalidate_dcache_by_mva(buffer, length ? length : 1u);
    arch_data_sync_barrier();

    reg_write(base + DWC2_HCINT, 0xffffffffu);
    reg_write(base + DWC2_HCINTMSK,
              HCINT_XFRC | HCINT_CHH | HCINT_STALL | HCINT_NAK |
              HCINT_ACK | HCINT_NYET | HCINT_XACTERR | HCINT_FRMOVRUN |
              HCINT_FATAL_ERRORS);
    reg_write(base + DWC2_HCSPLT, split);
    reg_write(base + DWC2_HCTSIZ,
              (length & 0x7ffffu) | (packets << HCTSIZ_PKTCNT_SHIFT) |
              ((uint32_t)pid << HCTSIZ_PID_SHIFT));
    reg_write(base + DWC2_HCDMA, dma_address);
    reg_write(base + DWC2_HCCHAR, hcchar);
    hcchar = reg_read(base + DWC2_HCCHAR);
    hcchar &= ~(HCCHAR_CHENA | HCCHAR_CHDIS);
    hcchar |= HCCHAR_CHENA;
    reg_write(base + DWC2_HCCHAR, hcchar);
    arch_data_sync_barrier();

    if (channel_wait(0, status) != 0)
        return -ETIMEDOUT;
    if (actual_length) {
        uint32_t remaining = reg_read(base + DWC2_HCTSIZ) & 0x7ffffu;

        *actual_length = remaining <= length ? length - remaining : 0u;
    }
    if (input) {
        arch_invalidate_dcache_by_mva(buffer, length ? length : 1u);
        arch_data_sync_barrier();
    }
    return 0;
}

static int channel_packet_transfer(const usb_device_t *device,
                                   uint8_t endpoint, bool input,
                                   uint8_t endpoint_type, uint8_t pid,
                                   void *buffer, uint32_t length,
                                   uint16_t max_packet,
                                   uint32_t *actual_length)
{
    uint32_t status = 0;
    uint32_t transaction_errors = 0;
    uint32_t split_start = 0;
    uint32_t actual = 0;

    for (uint32_t attempt = 0; attempt < 200u; attempt++) {
        if (device->hub_address) {
            split_start = next_split_microframe();
            if (!wait_for_microframe(split_start))
                return -ETIMEDOUT;
        }
        int result = channel_once(device, endpoint, input, endpoint_type, pid,
                                  buffer, length, max_packet, false, &status,
                                  &actual);
        if (result != 0)
            return result;
        if (status & (HCINT_STALL | HCINT_FATAL_ERRORS))
            return -EIO;
        if (status & (HCINT_XACTERR | HCINT_FRMOVRUN)) {
            if (++transaction_errors >= USB_TRANSACTION_RETRIES)
                return -EIO;
            usb_delay_ms((status & HCINT_XACTERR) ? 25u : 1u);
            continue;
        }

        if (!device->hub_address) {
            if (status & HCINT_XFRC) {
                if (actual_length)
                    *actual_length = actual;
                return 0;
            }
            if (status & HCINT_NAK) {
                /* No report is ready; let usbd service the next endpoint. */
                if (endpoint_type == USB_EP_INTERRUPT)
                    return -EAGAIN;
                usb_delay_ms(1);
                continue;
            }
            return -EIO;
        }

        if (status & HCINT_NAK && endpoint_type == USB_EP_INTERRUPT)
            return -EAGAIN;
        if (!(status & (HCINT_ACK | HCINT_XFRC))) {
            usb_delay_ms(1);
            continue;
        }

        /* CSPLIT is tied to the selected SSPLIT, not to poll completion. */
        uint32_t complete_target = (split_start + 2u) & 7u;
        if (!wait_for_microframe(complete_target))
            return -ETIMEDOUT;
        for (uint32_t complete = 0; complete < 4u; complete++) {
            result = channel_once(device, endpoint, input, endpoint_type, pid,
                                  buffer, length, max_packet, true, &status,
                                  &actual);
            if (result != 0)
                return result;
            if (status & HCINT_XFRC) {
                if (actual_length)
                    *actual_length = actual;
                return 0;
            }
            if (status & (HCINT_STALL | HCINT_FATAL_ERRORS))
                return -EIO;
            if (status & (HCINT_XACTERR | HCINT_FRMOVRUN)) {
                if (++transaction_errors >= USB_TRANSACTION_RETRIES)
                    return -EIO;
                usb_delay_ms((status & HCINT_XACTERR) ? 25u : 1u);
                break;
            }
            if (status & HCINT_NAK && endpoint_type == USB_EP_INTERRUPT)
                return -EAGAIN;
            if (status & (HCINT_NYET | HCINT_NAK)) {
                complete_target = (complete_target +
                    ((status & HCINT_NAK) ? 5u : 1u)) & 7u;
                if (!wait_for_microframe(complete_target))
                    return -ETIMEDOUT;
                continue;
            }
            break;
        }
    }
    return -ETIMEDOUT;
}

static int channel_transfer(const usb_device_t *device, uint8_t endpoint,
                            bool input, uint8_t endpoint_type, uint8_t pid,
                            void *buffer, uint32_t length, uint16_t max_packet)
{
    uint8_t *cursor = buffer;
    uint32_t transferred = 0;

    if (!device->hub_address || length <= max_packet) {
        return channel_packet_transfer(device, endpoint, input, endpoint_type,
                                       pid, buffer, length, max_packet, NULL);
    }

    /* A split host-channel transaction carries at most one FS/LS packet. */
    while (transferred < length) {
        uint32_t chunk = length - transferred;
        uint32_t actual = 0;
        int result;

        if (chunk > max_packet)
            chunk = max_packet;
        result = channel_packet_transfer(device, endpoint, input,
                                         endpoint_type, pid,
                                         cursor + transferred, chunk,
                                         max_packet, &actual);
        if (result != 0)
            return result;
        transferred += actual;
        if (actual < chunk)
            return 0;
        pid = pid == USB_PID_DATA0 ? USB_PID_DATA1 : USB_PID_DATA0;
    }
    return 0;
}

static int control_transfer(const usb_device_t *device,
                            uint8_t request_type, uint8_t request,
                            uint16_t value, uint16_t index,
                            void *data, uint16_t length)
{
    bool input = (request_type & USB_DIR_IN) != 0;
    int result;

    if (length > sizeof(control_dma_buffer) || (length && !data))
        return -EINVAL;

    setup_packet.request_type = request_type;
    setup_packet.request = request;
    setup_packet.value = value;
    setup_packet.index = index;
    setup_packet.length = length;

    last_control_phase = "setup";
    result = channel_transfer(device, 0, false, USB_EP_CONTROL, USB_PID_SETUP,
                              &setup_packet, sizeof(setup_packet),
                              device->max_packet);
    if (result != 0)
        return result;
    if (length) {
        if (input)
            memset(control_dma_buffer, 0, length);
        else
            memcpy(control_dma_buffer, data, length);
        last_control_phase = "data";
        result = channel_transfer(device, 0, input, USB_EP_CONTROL,
                                  USB_PID_DATA1, control_dma_buffer, length,
                                  device->max_packet);
        if (result != 0)
            return result;
        if (input)
            memcpy(data, control_dma_buffer, length);
    }
    last_control_phase = "status";
    return channel_transfer(device, 0, !input, USB_EP_CONTROL, USB_PID_DATA1,
                            status_dma_buffer, 0, device->max_packet);
}

static int get_descriptor(const usb_device_t *device, uint8_t type,
                          uint8_t index, void *buffer, uint16_t length)
{
    memset(buffer, 0, length);
    return control_transfer(device, USB_DIR_IN, USB_REQ_GET_DESCRIPTOR,
                            (uint16_t)((uint16_t)type << 8) | index,
                            0, buffer, length);
}

static uint16_t get_string_language(const usb_device_t *device)
{
    uint8_t language_descriptor[4];

    memset(language_descriptor, 0, sizeof(language_descriptor));
    if (control_transfer(device, USB_DIR_IN, USB_REQ_GET_DESCRIPTOR,
                         (uint16_t)USB_DESC_STRING << 8, 0,
                         language_descriptor,
                         sizeof(language_descriptor)) != 0)
        return 0;
    if (language_descriptor[0] < 4u ||
        language_descriptor[1] != USB_DESC_STRING)
        return 0;
    return (uint16_t)language_descriptor[2] |
           ((uint16_t)language_descriptor[3] << 8);
}

static void get_string(const usb_device_t *device, uint8_t index,
                       uint16_t language, char *text, size_t capacity)
{
    uint16_t length;
    size_t output = 0;

    if (!text || capacity == 0u)
        return;
    text[0] = '\0';
    if (index == 0u || language == 0u)
        return;

    memset(descriptor_buffer, 0, USB_MAX_CONFIG);
    if (control_transfer(device, USB_DIR_IN, USB_REQ_GET_DESCRIPTOR,
                         ((uint16_t)USB_DESC_STRING << 8) | index,
                         language, descriptor_buffer, 2u) != 0)
        return;
    length = descriptor_buffer[0];
    if (length < 2u || length > 254u ||
        descriptor_buffer[1] != USB_DESC_STRING)
        return;

    memset(descriptor_buffer, 0, USB_MAX_CONFIG);
    if (control_transfer(device, USB_DIR_IN, USB_REQ_GET_DESCRIPTOR,
                         ((uint16_t)USB_DESC_STRING << 8) | index,
                         language, descriptor_buffer, length) != 0)
        return;
    if (descriptor_buffer[1] != USB_DESC_STRING)
        return;

    for (uint8_t offset = 2u;
         offset + 1u < length && output + 1u < capacity;
         offset += 2u) {
        uint16_t codepoint = (uint16_t)descriptor_buffer[offset] |
                             ((uint16_t)descriptor_buffer[offset + 1u] << 8);
        char c = (codepoint >= 0x20u && codepoint <= 0x7eu) ?
                 (char)codepoint : '?';

        /* /proc/usb uses '|' as its field separator. */
        text[output++] = c == '|' ? '/' : c;
    }
    text[output] = '\0';
}

static int get_initial_device_descriptor(usb_device_t *device)
{
    uint16_t request_length;
    uint8_t max_packet;
    int result;

    /*
     * Linux and U-Boot start full/high-speed enumeration with EP0 guessed at
     * 64 bytes and request one 64-byte packet.  The 18-byte descriptor ends
     * that transfer with a short packet.  This avoids programming DWC2 for
     * several 8-byte packets before bMaxPacketSize0 is known, which notably
     * upsets the LAN7515 hub fitted to the Raspberry Pi 3 Model B+.
     */
    max_packet = device->speed == USB_SPEED_LOW ? 8u : 64u;
    request_length = max_packet;
    device->max_packet = max_packet;
    result = get_descriptor(device, USB_DESC_DEVICE, 0,
                            descriptor_buffer, request_length);
    if (result != 0)
        return result;
    if (descriptor_buffer[0] < 8u ||
        descriptor_buffer[1] != USB_DESC_DEVICE)
        return -EIO;

    max_packet = descriptor_buffer[7];
    if (max_packet != 8u && max_packet != 16u &&
        max_packet != 32u && max_packet != 64u)
        return -EIO;
    device->max_packet = max_packet;
    return 0;
}

static int set_address(usb_device_t *device, uint8_t address)
{
    int result = control_transfer(device, 0, USB_REQ_SET_ADDRESS,
                                  address, 0, NULL, 0);
    if (result == 0) {
        usb_delay_ms(10);
        device->address = address;
    }
    return result;
}

static bool configure_hid_device(usb_device_t *device, uint8_t next_address)
{
    uint32_t first_hid = hid_count;
    uint16_t total_length;
    uint8_t configuration;

    if (device->address == 0u) {
        if (get_initial_device_descriptor(device) != 0)
            return false;
        if (set_address(device, next_address) != 0)
            return false;
    }
    if (get_descriptor(device, USB_DESC_DEVICE, 0, descriptor_buffer, 18) != 0)
        return false;
    if (get_descriptor(device, USB_DESC_CONFIG, 0, descriptor_buffer, 9) != 0)
        return false;

    total_length = (uint16_t)descriptor_buffer[2] |
                   ((uint16_t)descriptor_buffer[3] << 8);
    configuration = descriptor_buffer[5];
    if (total_length > USB_MAX_CONFIG || total_length < 9u)
        return false;
    if (get_descriptor(device, USB_DESC_CONFIG, 0,
                       descriptor_buffer, total_length) != 0)
        return false;
    if (control_transfer(device, 0, USB_REQ_SET_CONFIGURATION,
                         configuration, 0, NULL, 0) != 0)
        return false;

    uint8_t current_interface = 0xffu;
    uint8_t current_protocol = 0u;
    for (uint16_t offset = 0; offset + 2u <= total_length;) {
        uint8_t length = descriptor_buffer[offset];
        uint8_t type = descriptor_buffer[offset + 1u];
        if (length < 2u || offset + length > total_length)
            break;

        if (type == USB_DESC_INTERFACE && length >= 9u) {
            current_interface = descriptor_buffer[offset + 2u];
            if (descriptor_buffer[offset + 5u] == USB_CLASS_HID &&
                descriptor_buffer[offset + 6u] == USB_HID_BOOT &&
                (descriptor_buffer[offset + 7u] == USB_HID_KEYBOARD ||
                 descriptor_buffer[offset + 7u] == USB_HID_MOUSE)) {
                current_protocol = descriptor_buffer[offset + 7u];
            } else {
                current_protocol = 0u;
            }
        } else if (type == USB_DESC_ENDPOINT && length >= 7u &&
                   current_protocol && hid_count < USB_MAX_HID) {
            uint8_t endpoint_address = descriptor_buffer[offset + 2u];
            uint8_t attributes = descriptor_buffer[offset + 3u] & 3u;
            if ((endpoint_address & USB_DIR_IN) && attributes == USB_EP_INTERRUPT) {
                usb_hid_endpoint_t *hid = &hid_endpoints[hid_count++];
                memset(hid, 0, sizeof(*hid));
                hid->device = *device;
                hid->interface_number = current_interface;
                hid->protocol = current_protocol;
                hid->endpoint = endpoint_address & 0x0fu;
                hid->max_packet = (uint16_t)descriptor_buffer[offset + 4u] |
                                  ((uint16_t)descriptor_buffer[offset + 5u] << 8);
                hid->interval = descriptor_buffer[offset + 6u];
                hid->data_pid = USB_PID_DATA0;
                if (device->speed == USB_SPEED_HIGH) {
                    uint8_t exponent = hid->interval > 0u ?
                        hid->interval - 1u : 0u;
                    uint32_t microframes;

                    if (exponent > 15u)
                        exponent = 15u;
                    microframes = 1u << exponent;
                    hid->poll_interval_ms =
                        (uint16_t)((microframes + 7u) / 8u);
                } else {
                    hid->poll_interval_ms = hid->interval ?
                        hid->interval : 1u;
                }
                if (hid->poll_interval_ms == 0u)
                    hid->poll_interval_ms = 1u;
                if (hid->poll_interval_ms < USB_HID_MIN_POLL_MS)
                    hid->poll_interval_ms = USB_HID_MIN_POLL_MS;
                if (hid->max_packet > sizeof(hid->report))
                    hid->max_packet = sizeof(hid->report);
                control_transfer(device, USB_TYPE_CLASS | USB_RECIP_INTERFACE,
                                 USB_REQ_SET_PROTOCOL, 0, current_interface,
                                 NULL, 0);
                control_transfer(device, USB_TYPE_CLASS | USB_RECIP_INTERFACE,
                                 USB_REQ_SET_IDLE, 0, current_interface,
                                 NULL, 0);
                if (current_protocol == USB_HID_KEYBOARD)
                    keyboard_count++;
                else if (current_protocol == USB_HID_MOUSE)
                    mouse_count++;
                usb_topology_note_hid(1u, device->address,
                                      current_protocol);
            }
        }
        offset += length;
    }
    return hid_count > first_hid;
}

static int hub_get_port_status(const usb_device_t *hub, uint8_t port,
                               uint32_t *port_status)
{
    *port_status = 0;
    return control_transfer(hub,
                            USB_DIR_IN | USB_TYPE_CLASS | USB_RECIP_OTHER,
                            USB_REQ_GET_STATUS, 0, port, port_status,
                            sizeof(*port_status));
}

static int hub_reset_port(const usb_device_t *hub, uint8_t port,
                          uint32_t *port_status)
{
    int result = control_transfer(hub, USB_TYPE_CLASS | USB_RECIP_OTHER,
                                  USB_REQ_SET_FEATURE, USB_HUB_PORT_RESET,
                                  port, NULL, 0);

    if (result != 0)
        return result;
    for (uint32_t elapsed = 0; elapsed < USB_HUB_RESET_WAIT_MS; elapsed++) {
        usb_delay_ms(1);
        result = hub_get_port_status(hub, port, port_status);
        if (result != 0)
            return result;
        if (!(*port_status & USB_HUB_PORT_CONNECTION))
            return -ENODEV;
        if (!(*port_status & USB_HUB_PORT_RESETTING) &&
            (*port_status & USB_HUB_PORT_ENABLE))
            break;
    }
    if ((*port_status & (USB_HUB_PORT_RESETTING | USB_HUB_PORT_ENABLE)) !=
        USB_HUB_PORT_ENABLE)
        return -ETIMEDOUT;

    result = control_transfer(hub, USB_TYPE_CLASS | USB_RECIP_OTHER,
                              USB_REQ_CLEAR_FEATURE,
                              USB_HUB_C_PORT_RESET, port, NULL, 0);
    if (result == 0)
        usb_delay_ms(10);
    return result;
}

static uint8_t allocate_usb_address(void)
{
    if (next_usb_address == 0u || next_usb_address > USB_MAX_ADDRESS)
        return 0u;
    return next_usb_address++;
}

static void route_child_through_hub(const usb_device_t *hub, uint8_t port,
                                    usb_device_t *child)
{
    if (child->speed == USB_SPEED_HIGH)
        return;

    if (hub->speed == USB_SPEED_HIGH) {
        child->hub_address = hub->address;
        child->hub_port = port;
        return;
    }

    /* A FS/LS hub has no transaction translator; inherit its upstream TT. */
    child->hub_address = hub->hub_address;
    child->hub_port = hub->hub_port;
}

static int address_usb_device(usb_device_t *device, uint8_t *device_class)
{
    usb_device_info_t info;
    uint8_t device_descriptor[18];
    uint8_t config_descriptor[9];
    uint8_t address;
    uint16_t language;
    int result;

    result = get_initial_device_descriptor(device);
    if (result != 0)
        return result;

    address = allocate_usb_address();
    if (address == 0u)
        return -ENOSPC;
    result = set_address(device, address);
    if (result != 0)
        return result;
    result = get_descriptor(device, USB_DESC_DEVICE, 0,
                            device_descriptor, sizeof(device_descriptor));
    if (result != 0)
        return result;
    if (device_descriptor[0] < 18u ||
        device_descriptor[1] != USB_DESC_DEVICE)
        return -EIO;

    *device_class = device_descriptor[4];
    memset(&info, 0, sizeof(info));
    info.bus = 1u;
    info.address = device->address;
    info.parent_address = device->parent_address;
    info.parent_port = device->parent_port;
    info.speed = device->speed;
    info.max_packet_size = device->max_packet;
    info.usb_version = (uint16_t)device_descriptor[2] |
                       ((uint16_t)device_descriptor[3] << 8);
    info.device_class = device_descriptor[4];
    info.device_subclass = device_descriptor[5];
    info.device_protocol = device_descriptor[6];
    info.vendor_id = (uint16_t)device_descriptor[8] |
                     ((uint16_t)device_descriptor[9] << 8);
    info.product_id = (uint16_t)device_descriptor[10] |
                      ((uint16_t)device_descriptor[11] << 8);
    info.device_version = (uint16_t)device_descriptor[12] |
                          ((uint16_t)device_descriptor[13] << 8);
    info.configuration_count = device_descriptor[17];

    memset(config_descriptor, 0, sizeof(config_descriptor));
    if (get_descriptor(device, USB_DESC_CONFIG, 0, config_descriptor,
                       sizeof(config_descriptor)) == 0 &&
        config_descriptor[0] >= 9u &&
        config_descriptor[1] == USB_DESC_CONFIG)
        info.interface_count = config_descriptor[4];

    language = (device_descriptor[14] != 0u ||
                device_descriptor[15] != 0u ||
                device_descriptor[16] != 0u) ?
               get_string_language(device) : 0u;
    get_string(device, device_descriptor[14], language, info.manufacturer,
               sizeof(info.manufacturer));
    get_string(device, device_descriptor[15], language, info.product,
               sizeof(info.product));
    get_string(device, device_descriptor[16], language, info.serial,
               sizeof(info.serial));
    if (usb_topology_register(&info) != 0)
        KWARN("USB: topology registry full, address %u omitted\n",
              device->address);

    KINFO("USB: address %u class=0x%02X speed=%s route=%u.%u\n",
          device->address, *device_class,
          device->speed == USB_SPEED_HIGH ? "high" :
          (device->speed == USB_SPEED_LOW ? "low" : "full"),
          device->hub_address, device->hub_port);
    return 0;
}

static bool enumerate_hub(usb_device_t *hub, uint32_t depth)
{
    int result;
    uint8_t ports;
    uint32_t power_good_ms;

    if (depth >= USB_MAX_HUB_DEPTH) {
        KWARN("USB: hub address %u exceeds topology depth limit\n",
              hub->address);
        return false;
    }

    result = get_descriptor(hub, USB_DESC_CONFIG, 0, descriptor_buffer, 9);
    if (result != 0)
        return usb_enumeration_failure("hub GET_DESCRIPTOR(config)", result);
    result = control_transfer(hub, 0, USB_REQ_SET_CONFIGURATION,
                              descriptor_buffer[5], 0, NULL, 0);
    if (result != 0)
        return usb_enumeration_failure("hub SET_CONFIGURATION", result);
    result = control_transfer(hub, USB_DIR_IN | USB_TYPE_CLASS,
                              USB_REQ_GET_DESCRIPTOR,
                              (uint16_t)USB_DESC_HUB << 8, 0,
                              descriptor_buffer, 9);
    if (result != 0)
        return usb_enumeration_failure("hub GET_DESCRIPTOR(hub)", result);

    ports = descriptor_buffer[2];
    power_good_ms = (uint32_t)descriptor_buffer[5] * 2u;
    if (ports > 8u)
        ports = 8u;
    usb_topology_set_hub_ports(1u, hub->address, ports);
    for (uint8_t port = 1; port <= ports; port++) {
        control_transfer(hub, USB_TYPE_CLASS | USB_RECIP_OTHER,
                         USB_REQ_SET_FEATURE, USB_HUB_PORT_POWER,
                         port, NULL, 0);
    }
    usb_delay_ms(power_good_ms + 20u);

    KINFO("USB: hub address %u exposes %u port(s), depth %u\n",
          hub->address, ports, depth);

    for (uint8_t port = 1; port <= ports; port++) {
        uint32_t port_status = 0;
        usb_device_t child;
        uint8_t device_class;

        if (hub_get_port_status(hub, port, &port_status) != 0 ||
            !(port_status & USB_HUB_PORT_CONNECTION))
            continue;
        control_transfer(hub, USB_TYPE_CLASS | USB_RECIP_OTHER,
                         USB_REQ_CLEAR_FEATURE,
                         USB_HUB_C_PORT_CONNECTION, port, NULL, 0);
        if (hub_reset_port(hub, port, &port_status) != 0) {
            KWARN("USB: hub %u port %u reset failed status=0x%08X\n",
                  hub->address, port, port_status);
            continue;
        }

        memset(&child, 0, sizeof(child));
        child.max_packet = 8u;
        child.parent_address = hub->address;
        child.parent_port = port;
        if (port_status & USB_HUB_PORT_HIGH_SPEED)
            child.speed = USB_SPEED_HIGH;
        else if (port_status & USB_HUB_PORT_LOW_SPEED)
            child.speed = USB_SPEED_LOW;
        else
            child.speed = USB_SPEED_FULL;
        route_child_through_hub(hub, port, &child);

        result = address_usb_device(&child, &device_class);
        if (result != 0) {
            KWARN("USB: hub %u port %u enumeration failed ret=%d\n",
                  hub->address, port, result);
            continue;
        }
        if (device_class == USB_CLASS_HUB) {
            enumerate_hub(&child, depth + 1u);
        } else if (!configure_hid_device(&child, child.address)) {
            KINFO("USB: address %u has no supported boot HID interface\n",
                  child.address);
        }
    }
    return true;
}

static bool enumerate_root_hub(uint8_t root_speed)
{
    usb_device_t root = { .address = 0, .speed = root_speed,
                          .max_packet = 8, .hub_address = 0, .hub_port = 0,
                          .parent_address = 0, .parent_port = 1 };
    uint8_t device_class;
    int result;

    next_usb_address = 1u;
    result = address_usb_device(&root, &device_class);
    if (result != 0)
        return usb_enumeration_failure("root device enumeration", result);
    if (device_class == USB_CLASS_HUB)
        return enumerate_hub(&root, 0u);
    if (configure_hid_device(&root, root.address))
        return true;

    KERROR("USB DWC2: root device is unsupported class=0x%02X\n",
           device_class);
    return false;
}

static bool key_already_pressed(const uint8_t *previous, uint8_t usage)
{
    for (uint32_t i = 2; i < 8u; i++)
        if (previous[i] == usage)
            return true;
    return false;
}

static void emit_string(const char *text)
{
    if (armos_input_tty_routing_enabled()) {
        while (*text)
            tty_input_char_to_id(usb_tty_id, *text++);
    }
}

static char translate_azerty(uint8_t usage, bool shift, bool altgr)
{
    static const char letters[26] = {
        'q','b','c','d','e','f','g','h','i','j','k','l',',','n','o','p',
        'a','r','s','t','u','v','z','x','y','w'
    };
    static const char numbers[10] = {'&','e','\"','\'','(','-','e','_','c','a'};
    static const char shifted_numbers[10] = {'1','2','3','4','5','6','7','8','9','0'};

    if (altgr) {
        switch (usage) {
        case 0x1fu: return '~';
        case 0x20u: return '#';
        case 0x21u: return '{';
        case 0x22u: return '[';
        case 0x23u: return '|';
        case 0x24u: return '`';
        case 0x25u: return '\\';
        case 0x26u: return '^';
        case 0x27u: return '@';
        case 0x2du: return ']';
        case 0x2eu: return '}';
        default: return 0;
        }
    }
    if (usage >= 0x04u && usage <= 0x1du) {
        char character = letters[usage - 0x04u];

        if (shift && character >= 'a' && character <= 'z')
            character = (char)(character - 'a' + 'A');
        else if (shift && character == ',')
            character = '?';
        return character;
    }
    if (usage >= 0x1eu && usage <= 0x27u)
        return shift ? shifted_numbers[usage - 0x1eu] :
                       numbers[usage - 0x1eu];
    switch (usage) {
    case 0x2cu: return ' ';
    case 0x2du: return shift ? 0 : ')';
    case 0x2eu: return shift ? '+' : '=';
    case 0x2fu: return '^';
    case 0x30u: return shift ? '*' : '$';
    case 0x31u: return '*';
    case 0x33u: return shift ? 'M' : 'm';
    case 0x34u: return shift ? '%' : 'u';
    case 0x36u: return shift ? '.' : ';';
    case 0x37u: return shift ? '/' : ':';
    case 0x38u: return '!';
    case 0x64u: return shift ? '>' : '<';
    default: return 0;
    }
}

static uint16_t hid_usage_to_key(uint8_t usage)
{
    static const uint16_t letters[26] = {
        30u, 48u, 46u, 32u, 18u, 33u, 34u, 35u, 23u, 36u, 37u, 38u,
        50u, 49u, 24u, 25u, 16u, 19u, 31u, 20u, 22u, 47u, 17u, 45u,
        21u, 44u
    };

    if (usage >= 0x04u && usage <= 0x1du)
        return letters[usage - 0x04u];
    if (usage >= 0x1eu && usage <= 0x26u)
        return (uint16_t)(usage - 0x1eu + 2u);
    if (usage == 0x27u)
        return 11u;
    switch (usage) {
    case 0x28u: return 28u;
    case 0x29u: return 1u;
    case 0x2au: return 14u;
    case 0x2bu: return 15u;
    case 0x2cu: return 57u;
    case 0x2du: return 12u;
    case 0x2eu: return 13u;
    case 0x2fu: return 26u;
    case 0x30u: return 27u;
    case 0x31u: return 43u;
    case 0x33u: return 39u;
    case 0x34u: return 40u;
    case 0x36u: return 51u;
    case 0x37u: return 52u;
    case 0x38u: return 53u;
    case 0x4fu: return 106u;
    case 0x50u: return 105u;
    case 0x51u: return 108u;
    case 0x52u: return 103u;
    case 0x64u: return 41u;
    default: return 0u;
    }
}

static void emit_input_text(char character)
{
    armos_input_emit(ARMOS_INPUT_EVENT_TEXT, 0u, (uint8_t)character);
}

static bool emit_keyboard_usage(uint8_t usage, uint8_t modifiers)
{
    bool shift = (modifiers & 0x22u) != 0u;
    bool ctrl = (modifiers & 0x11u) != 0u;
    bool altgr = (modifiers & 0x40u) != 0u;
    char character;

    if (usage == 0x28u) {
        if (armos_input_tty_routing_enabled())
            tty_input_char_to_id(usb_tty_id, '\n');
        emit_input_text('\n');
        return false;
    }
    if (usage == 0x29u) {
        if (armos_input_tty_routing_enabled())
            tty_input_char_to_id(usb_tty_id, 0x1b);
        emit_input_text(0x1b);
        return false;
    }
    if (usage == 0x2au) {
        if (armos_input_tty_routing_enabled())
            tty_input_char_to_id(usb_tty_id, 0x7f);
        emit_input_text(0x7f);
        return true;
    }
    if (usage == 0x2bu) {
        if (armos_input_tty_routing_enabled())
            tty_input_char_to_id(usb_tty_id, '\t');
        emit_input_text('\t');
        return true;
    }
    if (usage == 0x4fu) { emit_string("\033[C"); return true; }
    if (usage == 0x50u) { emit_string("\033[D"); return true; }
    if (usage == 0x51u) { emit_string("\033[B"); return true; }
    if (usage == 0x52u) { emit_string("\033[A"); return true; }

    character = translate_azerty(usage, shift, altgr);
    if (character && ctrl) {
        if (character >= 'a' && character <= 'z')
            character = (char)(character - 'a' + 'A');
        if (character >= '@' && character <= '_')
            character &= 0x1fu;
    }
    if (!character)
        return false;
    if (armos_input_tty_routing_enabled())
        tty_input_char_to_id(usb_tty_id, character);
    emit_input_text(character);
    return true;
}

static void process_keyboard(usb_hid_endpoint_t *hid)
{
    uint64_t now = get_timer_count();
    bool selected_repeat = false;
    static const uint16_t modifier_keys[8] = {
        29u, 42u, 56u, 125u, 97u, 54u, 100u, 126u
    };

    for (uint32_t bit = 0; bit < 8u; bit++) {
        uint8_t mask = (uint8_t)(1u << bit);

        if ((hid->report[0] & mask) != (hid->previous[0] & mask))
            armos_input_emit(ARMOS_INPUT_EVENT_KEY, modifier_keys[bit],
                             (hid->report[0] & mask) ? 1 : 0);
    }

    for (uint32_t i = 2; i < 8u; i++) {
        uint8_t usage = hid->report[i];
        uint16_t key;

        if (!usage || key_already_pressed(hid->previous, usage))
            continue;
        key = hid_usage_to_key(usage);
        if (key)
            armos_input_emit(ARMOS_INPUT_EVENT_KEY, key, 1);
        if (emit_keyboard_usage(usage, hid->report[0])) {
            hid->repeat_usage = usage;
            hid->repeat_next_tick = now +
                timer_ticks_from_ms(USB_KEY_REPEAT_DELAY_MS);
            selected_repeat = true;
        }
    }
    for (uint32_t i = 2; i < 8u; i++) {
        uint8_t usage = hid->previous[i];
        uint16_t key;

        if (!usage || key_already_pressed(hid->report, usage))
            continue;
        key = hid_usage_to_key(usage);
        if (key)
            armos_input_emit(ARMOS_INPUT_EVENT_KEY, key, 0);
    }
    armos_input_emit(ARMOS_INPUT_EVENT_SYNC, 0u, 0);

    if (!selected_repeat && hid->repeat_usage != 0u &&
        !key_already_pressed(hid->report, hid->repeat_usage)) {
        hid->repeat_usage = 0u;
        hid->repeat_next_tick = 0u;
    }
    memcpy(hid->previous, hid->report, sizeof(hid->previous));
}

static void process_keyboard_repeat(usb_hid_endpoint_t *hid, uint64_t now)
{
    if (hid->repeat_usage == 0u || hid->repeat_next_tick == 0u ||
        now < hid->repeat_next_tick)
        return;
    if (!key_already_pressed(hid->previous, hid->repeat_usage)) {
        hid->repeat_usage = 0u;
        hid->repeat_next_tick = 0u;
        return;
    }

    if (!emit_keyboard_usage(hid->repeat_usage, hid->previous[0])) {
        hid->repeat_usage = 0u;
        hid->repeat_next_tick = 0u;
        return;
    }
    armos_input_emit(ARMOS_INPUT_EVENT_KEY,
                     hid_usage_to_key(hid->repeat_usage), 2);
    hid->repeat_next_tick = now +
        timer_ticks_from_ms(USB_KEY_REPEAT_RATE_MS);
}

static void process_mouse(usb_hid_endpoint_t *hid)
{
    uint8_t buttons = hid->report[0];
    uint8_t previous_buttons = hid->previous[0];
    int8_t delta_x = (int8_t)hid->report[1];
    int8_t delta_y = (int8_t)hid->report[2];
    int8_t wheel = (int8_t)hid->report[3];

    if ((buttons & 1u) != (previous_buttons & 1u))
        armos_input_emit(ARMOS_INPUT_EVENT_KEY, ARMOS_INPUT_BUTTON_LEFT,
                         (buttons & 1u) != 0u);
    if ((buttons & 2u) != (previous_buttons & 2u))
        armos_input_emit(ARMOS_INPUT_EVENT_KEY, ARMOS_INPUT_BUTTON_RIGHT,
                         (buttons & 2u) != 0u);
    if ((buttons & 4u) != (previous_buttons & 4u))
        armos_input_emit(ARMOS_INPUT_EVENT_KEY, ARMOS_INPUT_BUTTON_MIDDLE,
                         (buttons & 4u) != 0u);
    if (delta_x)
        armos_input_emit(ARMOS_INPUT_EVENT_RELATIVE, ARMOS_INPUT_AXIS_X,
                         delta_x);
    if (delta_y)
        armos_input_emit(ARMOS_INPUT_EVENT_RELATIVE, ARMOS_INPUT_AXIS_Y,
                         delta_y);
    if (wheel)
        armos_input_emit(ARMOS_INPUT_EVENT_RELATIVE, ARMOS_INPUT_AXIS_WHEEL,
                         wheel);
    armos_input_emit(ARMOS_INPUT_EVENT_SYNC, 0u, 0);
    if (wheel > 0)
        display_scrollback_up((uint32_t)wheel * 3u);
    else if (wheel < 0)
        display_scrollback_down((uint32_t)(-wheel) * 3u);
    memcpy(hid->previous, hid->report, sizeof(hid->previous));
}

static uint32_t dwc2_poll(void *argument)
{
    uint64_t now;
    uint64_t next_deadline = 0u;

    (void)argument;
    if (!usb_ready)
        return USB_IDLE_POLL_MS;

    now = get_timer_count();
    for (uint32_t i = 0; i < hid_count; i++) {
        usb_hid_endpoint_t *hid = &hid_endpoints[i];
        uint64_t interval_ticks;

        if (hid->protocol == USB_HID_KEYBOARD)
            process_keyboard_repeat(hid, now);
        if (hid->next_poll_tick != 0u && now < hid->next_poll_tick) {
            if (hid->repeat_next_tick != 0u &&
                (next_deadline == 0u ||
                 hid->repeat_next_tick < next_deadline))
                next_deadline = hid->repeat_next_tick;
            if (next_deadline == 0u ||
                hid->next_poll_tick < next_deadline)
                next_deadline = hid->next_poll_tick;
            continue;
        }
        interval_ticks = timer_ticks_from_ms(hid->poll_interval_ms);
        hid->next_poll_tick = now + interval_ticks;
        memset(hid->report, 0, sizeof(hid->report));
        if (channel_transfer(&hid->device, hid->endpoint, true,
                             USB_EP_INTERRUPT, hid->data_pid,
                             hid->report, hid->max_packet,
                             hid->max_packet) == 0) {
            hid->data_pid = hid->data_pid == USB_PID_DATA0 ?
                USB_PID_DATA1 : USB_PID_DATA0;
            if (hid->protocol == USB_HID_KEYBOARD)
                process_keyboard(hid);
            else if (hid->protocol == USB_HID_MOUSE)
                process_mouse(hid);
        }
        if (hid->next_poll_tick != 0u &&
            (next_deadline == 0u || hid->next_poll_tick < next_deadline))
            next_deadline = hid->next_poll_tick;
        if (hid->repeat_next_tick != 0u &&
            (next_deadline == 0u || hid->repeat_next_tick < next_deadline))
            next_deadline = hid->repeat_next_tick;
    }
    if (next_deadline == 0u)
        return USB_IDLE_POLL_MS;
    return timer_ms_until(get_timer_count(), next_deadline);
}

static bool dwc2_initialize(int tty_id)
{
    uint32_t ahb_config;
    uint32_t host_config;
    uint32_t hw_config;
    uint32_t usb_config;
    uint32_t port;
    uint32_t timeout;
    uint8_t root_speed;

    if (usb_ready)
        return true;
    if (!arch_platform_has_usb()) {
        KERROR("USB DWC2: platform has no mapped USB controller\n");
        return false;
    }
    memset(hid_endpoints, 0, sizeof(hid_endpoints));
    hid_count = 0;
    keyboard_count = 0;
    mouse_count = 0;
    usb_tty_id = tty_id;
    dwc2 = (volatile uint32_t *)(uintptr_t)arch_platform_kernel_mmio_usb_base();

    /*
     * The Pi 3 firmware accepts the USB power request asynchronously.  The
     * proven SmartStart/CSUD sequence sends POWER_STATE_ON without asking the
     * firmware to wait, then lets the controller power domain settle before
     * touching DWC2.  This also keeps a slow power transition from blocking
     * the shared property mailbox indefinitely.
     */
    if (!raspberrypi_set_power_state(USB_POWER_USB_HCD, USB_POWER_ON)) {
        KERROR("USB DWC2: firmware power-on request failed\n");
        return false;
    }
    usb_delay_ms(100);
    reg_write(DWC2_PCGCCTL, 0);
    usb_delay_ms(20);
    if ((reg_read(DWC2_GSNPSID) & 0xffff0000u) != 0x4f540000u)
        return usb_init_register_failure("controller identity mismatch");
    hw_config = reg_read(DWC2_GHWCFG2);
    if (((hw_config & GHWCFG2_ARCH_MASK) >> GHWCFG2_ARCH_SHIFT) !=
        GHWCFG2_ARCH_INTERNAL_DMA)
        return usb_init_register_failure("internal DMA unavailable");
    if (((hw_config & GHWCFG2_HS_PHY_MASK) >> GHWCFG2_HS_PHY_SHIFT) ==
        GHWCFG2_HS_PHY_NONE)
        return usb_init_register_failure("high-speed PHY unavailable");

    usb_config = reg_read(DWC2_GUSBCFG);
    usb_config &= ~(GUSBCFG_ULPI_EXT_VBUS |
                    GUSBCFG_TERM_SEL_DL_PULSE);
    reg_write(DWC2_GUSBCFG, usb_config);
    if (!dwc2_core_reset())
        return usb_init_register_failure("core reset timeout");

    /* BCM2837 connects an 8-bit UTMI+ high-speed PHY to DWC2. */
    usb_config = reg_read(DWC2_GUSBCFG);
    usb_config &= ~(GUSBCFG_PHY_IF_16 | GUSBCFG_ULPI_PHY);
    reg_write(DWC2_GUSBCFG, usb_config);

    usb_config = reg_read(DWC2_GUSBCFG);
    usb_config &= ~(GUSBCFG_FS_PHY | GUSBCFG_SRP_CAP | GUSBCFG_HNP_CAP |
                    GUSBCFG_ULPI_FSLS | GUSBCFG_ULPI_CLK_SUS |
                    GUSBCFG_FORCE_DEV);
    hw_config = reg_read(DWC2_GHWCFG2);
    host_config = reg_read(DWC2_HCFG) & ~HCFG_FSLS_CLOCK_MASK;
    if (((hw_config & GHWCFG2_HS_PHY_MASK) >> GHWCFG2_HS_PHY_SHIFT) ==
            GHWCFG2_HS_PHY_ULPI &&
        ((hw_config & GHWCFG2_FS_PHY_MASK) >> GHWCFG2_FS_PHY_SHIFT) ==
            GHWCFG2_FS_PHY_DEDICATED) {
        usb_config |= GUSBCFG_ULPI_FSLS | GUSBCFG_ULPI_CLK_SUS;
        host_config |= HCFG_FSLS_CLOCK_48_MHZ;
    } else {
        host_config |= HCFG_FSLS_CLOCK_30_60_MHZ;
    }
    reg_write(DWC2_GUSBCFG, usb_config);
    reg_write(DWC2_HCFG, host_config);
    arch_data_sync_barrier();
    for (timeout = 100u; timeout > 0u; timeout--) {
        usb_delay_ms(1);
        if (reg_read(DWC2_GINTSTS) & GINTSTS_HOST_MODE)
            break;
    }
    if (!(reg_read(DWC2_GINTSTS) & GINTSTS_HOST_MODE))
        return usb_init_register_failure("host mode timeout");

    reg_write(DWC2_GOTGCTL,
              reg_read(DWC2_GOTGCTL) & ~GOTGCTL_HSTSETHNPEN);
    reg_write(DWC2_GINTMSK, 0);
    reg_write(DWC2_GINTSTS, 0xffffffffu);
    /*
     * BCM2837 repurposes this field. Preserve its reset WRESP bit, wait for
     * outstanding AXI writes and cap DMA bursts at four beats.
     */
    ahb_config = reg_read(DWC2_GAHBCFG);
    ahb_config |= GAHBCFG_DMA_EN | GAHBCFG_WAIT_AXI_WRITES;
    ahb_config &= ~GAHBCFG_MAX_AXI_BURST_MASK;
    ahb_config &= ~GAHBCFG_GLOBAL_INTR_EN;
    reg_write(DWC2_GAHBCFG, ahb_config);
    arch_data_sync_barrier();
    if (!(reg_read(DWC2_GAHBCFG) & GAHBCFG_DMA_EN))
        return usb_init_register_failure("DMA enable rejected");
    expected_usb_config = usb_config;
    expected_ahb_config = ahb_config;
    expected_host_config = host_config;
    reg_write(DWC2_HAINTMSK, 1u);
    if (!dwc2_configure_host_fifos())
        return usb_init_register_failure("host FIFO configuration failed");
    if (!dwc2_flush_fifos())
        return usb_init_register_failure("FIFO flush timeout");
    if (!dwc2_reset_host_channels())
        return usb_init_register_failure("host channel reset timeout");
    if (!dwc2_restore_host_state("channel reset"))
        return usb_init_register_failure("host state restore failed");
    KINFO("USB DWC2: DMA polling ready usb=0x%08X ahb=0x%08X "
          "host=0x%08X hfir=%u fifo=%u/%u/%u\n",
          reg_read(DWC2_GUSBCFG), reg_read(DWC2_GAHBCFG),
          reg_read(DWC2_HCFG), reg_read(DWC2_HFIR),
          reg_read(DWC2_GRXFSIZ),
          reg_read(DWC2_GNPTXFSIZ) >> 16,
          reg_read(DWC2_HPTXFSIZ) >> 16);

    port = reg_read(DWC2_HPRT);
    reg_write(DWC2_HPRT, hprt_write_value(port) | HPRT_POWER);
    for (timeout = USB_ROOT_CONNECT_WAIT_MS; timeout > 0u; timeout--) {
        usb_delay_ms(1);
        port = reg_read(DWC2_HPRT);
        if (port & HPRT_CONN)
            break;
    }
    if (!(port & HPRT_CONN))
        return usb_init_register_failure("root port connection timeout");
    reg_write(DWC2_HPRT,
              hprt_write_value(port) | HPRT_POWER | HPRT_CONN_CHG);

    /* USB 2.0 debounce, reset and reset-recovery timings. */
    usb_delay_ms(100);
    port = reg_read(DWC2_HPRT);
    reg_write(DWC2_HPRT, hprt_write_value(port) | HPRT_POWER | HPRT_RESET);
    usb_delay_ms(50);
    reg_write(DWC2_HPRT,
              (hprt_write_value(reg_read(DWC2_HPRT)) & ~HPRT_RESET) |
              HPRT_POWER);
    usb_delay_ms(20);
    for (timeout = USB_ROOT_ENABLE_WAIT_MS; timeout > 0u; timeout--) {
        usb_delay_ms(1);
        port = reg_read(DWC2_HPRT);
        if ((port & (HPRT_CONN | HPRT_ENABLE)) ==
            (HPRT_CONN | HPRT_ENABLE))
            break;
    }
    if ((port & (HPRT_CONN | HPRT_ENABLE)) !=
        (HPRT_CONN | HPRT_ENABLE))
        return usb_init_register_failure("root port enable timeout");
    reg_write(DWC2_HPRT,
              hprt_write_value(port) | HPRT_POWER | HPRT_CONN_CHG |
              HPRT_ENABLE_CHG);
    if (!dwc2_restore_host_state("root port reset"))
        return usb_init_register_failure("host state restore failed");

    switch ((port & HPRT_SPEED_MASK) >> HPRT_SPEED_SHIFT) {
    case 0u: root_speed = USB_SPEED_HIGH; break;
    case 1u: root_speed = USB_SPEED_FULL; break;
    case 2u: root_speed = USB_SPEED_LOW; break;
    default:
        return usb_init_register_failure("invalid root port speed");
    }

    KINFO("USB DWC2: root port ready hprt=0x%08X speed=%s usb=0x%08X "
          "ahb=0x%08X host=0x%08X hfir=%u\n",
          port,
          root_speed == USB_SPEED_HIGH ? "high" :
          root_speed == USB_SPEED_FULL ? "full" : "low",
          reg_read(DWC2_GUSBCFG), reg_read(DWC2_GAHBCFG),
          reg_read(DWC2_HCFG), reg_read(DWC2_HFIR));

    if (!enumerate_root_hub(root_speed))
        return usb_init_register_failure("root hub enumeration failed");
    usb_ready = true;
    return true;
}

static int dwc2_probe(void *context)
{
    (void)context;
    if (!dwc2_initialize(usb_tty_id))
        return -ENODEV;

    KBOOT_OK("USB: DWC2 host and hub initialized");
    if (hid_count != 0u) {
        KBOOT_OKF("USB HID: %u endpoint(s), %u keyboard(s), %u mouse(s)",
                  hid_count, keyboard_count, mouse_count);
    } else {
        KBOOT_WARN("Input: no USB boot HID device found");
    }
    if (hid_count != 0u && keyboard_count == 0u)
        KBOOT_WARN("Input: USB HID present but no boot keyboard found");
    return 0;
}

static const usb_host_controller_ops_t dwc2_host_ops = {
    .name = "DWC2",
    .probe = dwc2_probe,
    .poll = dwc2_poll,
};

int dwc2_usb_register(int tty_id)
{
    usb_tty_id = tty_id;
    return usb_host_controller_register(&dwc2_host_ops, NULL);
}
