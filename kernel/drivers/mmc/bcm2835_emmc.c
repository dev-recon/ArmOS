/*
 * ArmOS
 * Copyright (c) 2026 Mohamed Ennassiri
 *
 * Licensed under the Apache License, Version 2.0.
 * See LICENSE for details.
 *
 * File: kernel/drivers/mmc/bcm2835_emmc.c
 * Layer: Kernel / block drivers
 *
 * Responsibilities:
 * - Drive the BCM2835/BCM2836 EMMC controller used by Raspberry Pi SD cards.
 * - Expose the SD card as the active ArmOS block device through block_device.
 *
 * Notes:
 * - Transfers remain polling PIO, but contiguous requests use SD multi-block
 *   commands and 4-bit mode when the card accepts them.
 * - Multi-block failures disable that path and retry with the established
 *   single-block commands so a controller quirk cannot make the card unusable.
 */

#include <kernel/arch_barrier.h>
#include <kernel/arch_cpu.h>
#include <kernel/arch_platform.h>
#include <kernel/block_device.h>
#include <kernel/kprintf.h>
#include <kernel/mmc/bcm2835_emmc.h>
#include <kernel/spinlock.h>
#include <kernel/task.h>
#include <kernel/timer.h>
#include <kernel/types.h>

#define EMMC_ARG2            0x00u
#define EMMC_BLKSIZECNT      0x04u
#define EMMC_ARG1            0x08u
#define EMMC_CMDTM           0x0Cu
#define EMMC_RESP0           0x10u
#define EMMC_RESP1           0x14u
#define EMMC_RESP2           0x18u
#define EMMC_RESP3           0x1Cu
#define EMMC_DATA            0x20u
#define EMMC_STATUS          0x24u
#define EMMC_CONTROL0        0x28u
#define EMMC_CONTROL1        0x2Cu
#define EMMC_INTERRUPT       0x30u
#define EMMC_IRPT_MASK       0x34u
#define EMMC_IRPT_EN         0x38u
#define EMMC_CONTROL2        0x3Cu
#define EMMC_SLOTISR_VER     0xFCu

#define EMMC_STATUS_CMD_INHIBIT      (1u << 0)
#define EMMC_STATUS_DAT_INHIBIT      (1u << 1)
#define EMMC_STATUS_WRITE_AVAILABLE  (1u << 10)
#define EMMC_STATUS_READ_AVAILABLE   (1u << 11)

#define EMMC_CONTROL0_POWER_330_ON   (0x0Fu << 8)
#define EMMC_CONTROL0_HCTL_DWIDTH    (1u << 1)

#define EMMC_CONTROL1_CLK_INTLEN     (1u << 0)
#define EMMC_CONTROL1_CLK_STABLE     (1u << 1)
#define EMMC_CONTROL1_CLK_EN         (1u << 2)
#define EMMC_CONTROL1_CLK_FREQ_MS2   (3u << 6)
#define EMMC_CONTROL1_CLK_FREQ8      (0xFFu << 8)
#define EMMC_CONTROL1_DATA_TIMEOUT   (0xEu << 16)
#define EMMC_CONTROL1_SRST_HC        (1u << 24)
#define EMMC_CONTROL1_SRST_CMD       (1u << 25)
#define EMMC_CONTROL1_SRST_DATA      (1u << 26)

#define EMMC_INT_CMD_DONE            (1u << 0)
#define EMMC_INT_DATA_DONE           (1u << 1)
#define EMMC_INT_WRITE_RDY           (1u << 4)
#define EMMC_INT_READ_RDY            (1u << 5)
#define EMMC_INT_ERROR_MASK          0xFFFF8000u
#define EMMC_INT_ALL                 0xFFFFFFFFu

#define EMMC_CMD_RSP_NONE            0u
#define EMMC_CMD_RSP_136             (1u << 16)
#define EMMC_CMD_RSP_48              (2u << 16)
#define EMMC_CMD_RSP_48_BUSY         (3u << 16)
#define EMMC_CMD_CRCCHK              (1u << 19)
#define EMMC_CMD_IXCHK               (1u << 20)
#define EMMC_CMD_ISDATA              (1u << 21)
#define EMMC_CMD_BLKCNT_EN           (1u << 1)
#define EMMC_CMD_AUTO_CMD12          (1u << 2)
#define EMMC_CMD_READ                (1u << 4)
#define EMMC_CMD_MULTI_BLOCK         (1u << 5)

#define EMMC_BLOCK_SIZE              512u
#define EMMC_WORDS_PER_BLOCK         (EMMC_BLOCK_SIZE / sizeof(uint32_t))
#define EMMC_BASE_CLOCK_HZ           250000000u
#define EMMC_INIT_CLOCK_HZ           400000u
#define EMMC_TRANSFER_CLOCK_HZ       25000000u
#define EMMC_TIMEOUT_MS              1000u
#define EMMC_INIT_TIMEOUT_MS         2000u
#define EMMC_FALLBACK_SECTORS        (64ULL * 1024ULL * 1024ULL) /* 32 GiB */

typedef struct {
    volatile uint8_t *base;
    uint32_t rca;
    bool high_capacity;
    bool wide_bus;
    bool multiblock;
    bool ready;
} bcm2835_emmc_state_t;

static bcm2835_emmc_state_t emmc_state;
static spinlock_t emmc_lock = SPINLOCK_INIT("bcm_emmc");
static volatile bool emmc_busy;
static task_t *emmc_owner;

static int emmc_blockdev_read(block_device_t *dev, uint64_t lba,
                              uint32_t count, void *buffer);
static int emmc_blockdev_write(block_device_t *dev, uint64_t lba,
                               uint32_t count, const void *buffer);
static int emmc_blockdev_flush(block_device_t *dev);
static void emmc_blockdev_shutdown(block_device_t *dev);

static const block_device_ops_t emmc_block_ops = {
    .read_sectors = emmc_blockdev_read,
    .write_sectors = emmc_blockdev_write,
    .flush = emmc_blockdev_flush,
    .shutdown = emmc_blockdev_shutdown,
};

static block_device_t emmc_block_dev = {
    .name = "sd0",
    .transport_name = "bcm2835-emmc",
    .sector_size = EMMC_BLOCK_SIZE,
    .capacity_sectors = EMMC_FALLBACK_SECTORS,
    .read_only = false,
    .ops = &emmc_block_ops,
};

static inline uint32_t emmc_read(uint32_t offset)
{
    return *(volatile uint32_t *)(emmc_state.base + offset);
}

static inline void emmc_write(uint32_t offset, uint32_t value)
{
    *(volatile uint32_t *)(emmc_state.base + offset) = value;
    arch_data_memory_barrier();
}

static inline void emmc_write16(uint32_t offset, uint16_t value)
{
    *(volatile uint16_t *)(emmc_state.base + offset) = value;
    arch_data_memory_barrier();
}

static void emmc_write_cmdtm(uint32_t cmdtm)
{
    /*
     * SDHCI defines this area as two 16-bit registers: transfer mode at 0x0c
     * and command at 0x0e.  The BCM datasheet documents CMDTM as a combined
     * 32-bit view, but QEMU's raspi2 model follows SDHCI register access
     * semantics and triggers command execution on the 16-bit command write.
     */
    emmc_write16(EMMC_CMDTM, (uint16_t)(cmdtm & 0xFFFFu));
    emmc_write16(EMMC_CMDTM + 2u, (uint16_t)(cmdtm >> 16));
}

static uint64_t timeout_counter_delta(uint32_t timeout_ms)
{
    uint32_t freq = arch_timer_frequency();
    uint32_t per_ms;
    uint32_t remainder;
    uint32_t delta;

    if (freq == 0)
        freq = TIMER_FALLBACK_FREQ;

    /*
     * Keep this calculation 32-bit: the freestanding kernel intentionally
     * avoids pulling libgcc 64-bit division helpers into early boot code.
     */
    per_ms = freq / 1000u;
    remainder = freq % 1000u;
    delta = per_ms * timeout_ms + (remainder * timeout_ms + 999u) / 1000u;
    return delta ? delta : 1u;
}

static bool deadline_expired(uint64_t start, uint32_t timeout_ms)
{
    return (arch_timer_counter() - start) >= timeout_counter_delta(timeout_ms);
}

static bool emmc_wait_clear(uint32_t reg, uint32_t mask, uint32_t timeout_ms)
{
    uint64_t start = arch_timer_counter();

    while (emmc_read(reg) & mask) {
        if (deadline_expired(start, timeout_ms))
            return false;
        arch_cpu_relax();
    }

    return true;
}

static bool emmc_wait_set(uint32_t reg, uint32_t mask, uint32_t timeout_ms)
{
    uint64_t start = arch_timer_counter();

    while ((emmc_read(reg) & mask) != mask) {
        if (deadline_expired(start, timeout_ms))
            return false;
        arch_cpu_relax();
    }

    return true;
}

static int emmc_wait_interrupt(uint32_t mask, uint32_t timeout_ms)
{
    uint64_t start = arch_timer_counter();

    while (1) {
        uint32_t irpt = emmc_read(EMMC_INTERRUPT);

        if (irpt & EMMC_INT_ERROR_MASK) {
            KERROR("bcm_emmc: interrupt error 0x%08X\n", irpt);
            emmc_write(EMMC_INTERRUPT, irpt);
            return -EIO;
        }
        if (irpt & mask)
            return 0;
        if (deadline_expired(start, timeout_ms))
            return -ETIMEDOUT;

        arch_cpu_relax();
    }
}

static void emmc_reset_cmd_data(void)
{
    uint32_t c1 = emmc_read(EMMC_CONTROL1);

    emmc_write(EMMC_CONTROL1, c1 | EMMC_CONTROL1_SRST_CMD | EMMC_CONTROL1_SRST_DATA);
    (void)emmc_wait_clear(EMMC_CONTROL1,
                          EMMC_CONTROL1_SRST_CMD | EMMC_CONTROL1_SRST_DATA,
                          EMMC_TIMEOUT_MS);
    emmc_write(EMMC_INTERRUPT, EMMC_INT_ALL);
}

static bool emmc_set_clock(uint32_t target_hz)
{
    uint32_t divisor;
    uint32_t c1;

    if (target_hz == 0)
        return false;

    c1 = emmc_read(EMMC_CONTROL1);
    c1 &= ~EMMC_CONTROL1_CLK_EN;
    emmc_write(EMMC_CONTROL1, c1);

    if (target_hz >= EMMC_BASE_CLOCK_HZ) {
        divisor = 1;
    } else {
        divisor = (EMMC_BASE_CLOCK_HZ + target_hz - 1u) / target_hz;
        if (divisor > 0x3FFu)
            divisor = 0x3FFu;
        if (divisor == 0)
            divisor = 1;
    }

    c1 &= ~(EMMC_CONTROL1_CLK_FREQ8 | EMMC_CONTROL1_CLK_FREQ_MS2);
    c1 |= EMMC_CONTROL1_CLK_INTLEN | EMMC_CONTROL1_DATA_TIMEOUT;
    c1 |= (divisor & 0xFFu) << 8;
    c1 |= ((divisor >> 8) & 0x3u) << 6;
    emmc_write(EMMC_CONTROL1, c1);

    if (!emmc_wait_set(EMMC_CONTROL1, EMMC_CONTROL1_CLK_STABLE, EMMC_TIMEOUT_MS)) {
        KERROR("bcm_emmc: clock did not become stable\n");
        return false;
    }

    emmc_write(EMMC_CONTROL1, c1 | EMMC_CONTROL1_CLK_EN);
    return true;
}

static bool emmc_reset_host(void)
{
    uint32_t c1;

    emmc_write(EMMC_CONTROL0, 0);
    emmc_write(EMMC_IRPT_EN, 0);
    emmc_write(EMMC_IRPT_MASK, 0);
    emmc_write(EMMC_INTERRUPT, EMMC_INT_ALL);

    c1 = emmc_read(EMMC_CONTROL1);
    emmc_write(EMMC_CONTROL1, c1 | EMMC_CONTROL1_SRST_HC);
    if (!emmc_wait_clear(EMMC_CONTROL1, EMMC_CONTROL1_SRST_HC, EMMC_TIMEOUT_MS)) {
        KERROR("bcm_emmc: host reset timeout\n");
        return false;
    }

    emmc_write(EMMC_CONTROL0, EMMC_CONTROL0_POWER_330_ON);
    emmc_write(EMMC_IRPT_EN, 0);
    /*
     * We poll EMMC_INTERRUPT rather than using the IRQ line, but SDHCI-style
     * controllers still need status reporting enabled or command/data done
     * bits may never become visible to software.
     */
    emmc_write(EMMC_IRPT_MASK, EMMC_INT_ALL);
    emmc_write(EMMC_INTERRUPT, EMMC_INT_ALL);

    return emmc_set_clock(EMMC_INIT_CLOCK_HZ);
}

static int emmc_send_command(uint32_t cmd, uint32_t arg,
                             uint32_t flags, uint32_t *resp)
{
    uint32_t inhibit = EMMC_STATUS_CMD_INHIBIT;
    uint32_t cmdtm = (cmd << 24) | flags;

    if (flags & EMMC_CMD_ISDATA)
        inhibit |= EMMC_STATUS_DAT_INHIBIT;

    if (!emmc_wait_clear(EMMC_STATUS, inhibit, EMMC_TIMEOUT_MS)) {
        KERROR("bcm_emmc: command %u inhibit timeout status=0x%08X\n",
               cmd, emmc_read(EMMC_STATUS));
        return -ETIMEDOUT;
    }

    emmc_write(EMMC_INTERRUPT, EMMC_INT_ALL);
    emmc_write(EMMC_ARG1, arg);
    emmc_write_cmdtm(cmdtm);

    int ret = emmc_wait_interrupt(EMMC_INT_CMD_DONE, EMMC_TIMEOUT_MS);
    if (ret < 0) {
        KERROR("bcm_emmc: command %u failed ret=%d interrupt=0x%08X status=0x%08X c0=0x%08X c1=0x%08X cmdtm=0x%08X mask=0x%08X\n",
               cmd, ret, emmc_read(EMMC_INTERRUPT), emmc_read(EMMC_STATUS),
               emmc_read(EMMC_CONTROL0), emmc_read(EMMC_CONTROL1),
               emmc_read(EMMC_CMDTM), emmc_read(EMMC_IRPT_MASK));
        emmc_reset_cmd_data();
        return ret;
    }

    if (resp) {
        resp[0] = emmc_read(EMMC_RESP0);
        resp[1] = emmc_read(EMMC_RESP1);
        resp[2] = emmc_read(EMMC_RESP2);
        resp[3] = emmc_read(EMMC_RESP3);
    }

    emmc_write(EMMC_INTERRUPT, EMMC_INT_CMD_DONE);
    return 0;
}

static int emmc_app_command(uint32_t rca, uint32_t cmd,
                            uint32_t arg, uint32_t flags, uint32_t *resp)
{
    int ret = emmc_send_command(55, rca << 16,
                                EMMC_CMD_RSP_48 | EMMC_CMD_CRCCHK | EMMC_CMD_IXCHK,
                                NULL);
    if (ret < 0)
        return ret;

    return emmc_send_command(cmd, arg, flags, resp);
}

static int emmc_read_block(uint64_t lba, uint8_t *dst)
{
    uint32_t arg;
    int ret;

    if (lba > 0xFFFFFFFFULL)
        return -EINVAL;

    arg = emmc_state.high_capacity ? (uint32_t)lba : (uint32_t)(lba * EMMC_BLOCK_SIZE);
    emmc_write(EMMC_BLKSIZECNT, EMMC_BLOCK_SIZE | (1u << 16));

    ret = emmc_send_command(17, arg,
                            EMMC_CMD_RSP_48 | EMMC_CMD_CRCCHK | EMMC_CMD_IXCHK |
                            EMMC_CMD_ISDATA | EMMC_CMD_BLKCNT_EN | EMMC_CMD_READ,
                            NULL);
    if (ret < 0)
        return ret;

    ret = emmc_wait_interrupt(EMMC_INT_READ_RDY, EMMC_TIMEOUT_MS);
    if (ret < 0) {
        emmc_reset_cmd_data();
        return ret;
    }
    emmc_write(EMMC_INTERRUPT, EMMC_INT_READ_RDY);

    for (uint32_t i = 0; i < EMMC_WORDS_PER_BLOCK; i++) {
        uint32_t word = emmc_read(EMMC_DATA);

        dst[i * 4u + 0u] = (uint8_t)(word & 0xFFu);
        dst[i * 4u + 1u] = (uint8_t)((word >> 8) & 0xFFu);
        dst[i * 4u + 2u] = (uint8_t)((word >> 16) & 0xFFu);
        dst[i * 4u + 3u] = (uint8_t)((word >> 24) & 0xFFu);
    }

    ret = emmc_wait_interrupt(EMMC_INT_DATA_DONE, EMMC_TIMEOUT_MS);
    if (ret < 0) {
        emmc_reset_cmd_data();
        return ret;
    }
    emmc_write(EMMC_INTERRUPT, EMMC_INT_DATA_DONE);

    return 0;
}

static int emmc_write_block(uint64_t lba, const uint8_t *src)
{
    uint32_t arg;
    int ret;

    if (lba > 0xFFFFFFFFULL)
        return -EINVAL;

    arg = emmc_state.high_capacity ? (uint32_t)lba : (uint32_t)(lba * EMMC_BLOCK_SIZE);
    emmc_write(EMMC_BLKSIZECNT, EMMC_BLOCK_SIZE | (1u << 16));

    ret = emmc_send_command(24, arg,
                            EMMC_CMD_RSP_48 | EMMC_CMD_CRCCHK | EMMC_CMD_IXCHK |
                            EMMC_CMD_ISDATA | EMMC_CMD_BLKCNT_EN,
                            NULL);
    if (ret < 0)
        return ret;

    ret = emmc_wait_interrupt(EMMC_INT_WRITE_RDY, EMMC_TIMEOUT_MS);
    if (ret < 0) {
        emmc_reset_cmd_data();
        return ret;
    }
    emmc_write(EMMC_INTERRUPT, EMMC_INT_WRITE_RDY);

    for (uint32_t i = 0; i < EMMC_WORDS_PER_BLOCK; i++) {
        uint32_t word = ((uint32_t)src[i * 4u + 0u]) |
                        ((uint32_t)src[i * 4u + 1u] << 8) |
                        ((uint32_t)src[i * 4u + 2u] << 16) |
                        ((uint32_t)src[i * 4u + 3u] << 24);
        emmc_write(EMMC_DATA, word);
    }

    ret = emmc_wait_interrupt(EMMC_INT_DATA_DONE, EMMC_TIMEOUT_MS);
    if (ret < 0) {
        emmc_reset_cmd_data();
        return ret;
    }
    emmc_write(EMMC_INTERRUPT, EMMC_INT_DATA_DONE);

    return 0;
}

static int emmc_read_blocks(uint64_t lba, uint32_t count, uint8_t *dst)
{
    uint32_t arg;
    int ret;

    if (!dst || count < 2 || count > 0xffffu || lba > 0xffffffffULL)
        return -EINVAL;
    if ((uint64_t)count - 1u > 0xffffffffULL - lba)
        return -EINVAL;

    arg = emmc_state.high_capacity ? (uint32_t)lba :
          (uint32_t)(lba * EMMC_BLOCK_SIZE);
    emmc_write(EMMC_BLKSIZECNT, EMMC_BLOCK_SIZE | (count << 16));

    ret = emmc_send_command(18, arg,
                            EMMC_CMD_RSP_48 | EMMC_CMD_CRCCHK |
                            EMMC_CMD_IXCHK | EMMC_CMD_ISDATA |
                            EMMC_CMD_BLKCNT_EN | EMMC_CMD_AUTO_CMD12 |
                            EMMC_CMD_MULTI_BLOCK | EMMC_CMD_READ,
                            NULL);
    if (ret < 0)
        return ret;

    for (uint32_t block = 0; block < count; block++) {
        ret = emmc_wait_interrupt(EMMC_INT_READ_RDY, EMMC_TIMEOUT_MS);
        if (ret < 0)
            goto fail;
        emmc_write(EMMC_INTERRUPT, EMMC_INT_READ_RDY);

        for (uint32_t i = 0; i < EMMC_WORDS_PER_BLOCK; i++) {
            uint32_t word = emmc_read(EMMC_DATA);
            uint8_t *out = dst + block * EMMC_BLOCK_SIZE + i * 4u;

            out[0] = (uint8_t)(word & 0xffu);
            out[1] = (uint8_t)((word >> 8) & 0xffu);
            out[2] = (uint8_t)((word >> 16) & 0xffu);
            out[3] = (uint8_t)((word >> 24) & 0xffu);
        }
    }

    ret = emmc_wait_interrupt(EMMC_INT_DATA_DONE, EMMC_TIMEOUT_MS);
    if (ret < 0)
        goto fail;
    emmc_write(EMMC_INTERRUPT, EMMC_INT_DATA_DONE);
    return 0;

fail:
    emmc_reset_cmd_data();
    return ret;
}

static int emmc_write_blocks(uint64_t lba, uint32_t count, const uint8_t *src)
{
    uint32_t arg;
    int ret;

    if (!src || count < 2 || count > 0xffffu || lba > 0xffffffffULL)
        return -EINVAL;
    if ((uint64_t)count - 1u > 0xffffffffULL - lba)
        return -EINVAL;

    arg = emmc_state.high_capacity ? (uint32_t)lba :
          (uint32_t)(lba * EMMC_BLOCK_SIZE);
    emmc_write(EMMC_BLKSIZECNT, EMMC_BLOCK_SIZE | (count << 16));

    ret = emmc_send_command(25, arg,
                            EMMC_CMD_RSP_48 | EMMC_CMD_CRCCHK |
                            EMMC_CMD_IXCHK | EMMC_CMD_ISDATA |
                            EMMC_CMD_BLKCNT_EN | EMMC_CMD_AUTO_CMD12 |
                            EMMC_CMD_MULTI_BLOCK,
                            NULL);
    if (ret < 0)
        return ret;

    for (uint32_t block = 0; block < count; block++) {
        ret = emmc_wait_interrupt(EMMC_INT_WRITE_RDY, EMMC_TIMEOUT_MS);
        if (ret < 0)
            goto fail;
        emmc_write(EMMC_INTERRUPT, EMMC_INT_WRITE_RDY);

        for (uint32_t i = 0; i < EMMC_WORDS_PER_BLOCK; i++) {
            const uint8_t *in = src + block * EMMC_BLOCK_SIZE + i * 4u;
            uint32_t word = ((uint32_t)in[0]) |
                            ((uint32_t)in[1] << 8) |
                            ((uint32_t)in[2] << 16) |
                            ((uint32_t)in[3] << 24);
            emmc_write(EMMC_DATA, word);
        }
    }

    ret = emmc_wait_interrupt(EMMC_INT_DATA_DONE, EMMC_TIMEOUT_MS);
    if (ret < 0)
        goto fail;
    emmc_write(EMMC_INTERRUPT, EMMC_INT_DATA_DONE);
    return 0;

fail:
    emmc_reset_cmd_data();
    return ret;
}

static void emmc_acquire(void)
{
    while (1) {
        task_t *task = task_current_local();
        unsigned long flags;

        spin_lock_irqsave(&emmc_lock, &flags);
        if (!emmc_busy) {
            emmc_busy = true;
            emmc_owner = task;
            spin_unlock_irqrestore(&emmc_lock, flags);
            return;
        }
        spin_unlock_irqrestore(&emmc_lock, flags);

        if (!task) {
            arch_cpu_relax();
            continue;
        }
        yield();
    }
}

static void emmc_release(void)
{
    task_t *task = task_current_local();
    unsigned long flags;

    spin_lock_irqsave(&emmc_lock, &flags);
    if (!emmc_busy) {
        KERROR("bcm_emmc: release without owner\n");
    } else if (emmc_owner && task && emmc_owner != task) {
        KERROR("bcm_emmc: release by non-owner\n");
    }
    emmc_busy = false;
    emmc_owner = NULL;
    spin_unlock_irqrestore(&emmc_lock, flags);
}

static int emmc_blockdev_read(block_device_t *dev, uint64_t lba,
                              uint32_t count, void *buffer)
{
    uint8_t *dst = (uint8_t *)buffer;
    int ret = 0;

    (void)dev;
    if (!emmc_state.ready || !dst)
        return -ENODEV;

    emmc_acquire();
    if (count > 1 && emmc_state.multiblock) {
        ret = emmc_read_blocks(lba, count, dst);
        if (ret < 0) {
            emmc_state.multiblock = false;
            KWARN("bcm_emmc: multiblock read unavailable, using CMD17\n");
        }
    }
    if (count == 1 || !emmc_state.multiblock) {
        ret = 0;
        for (uint32_t i = 0; i < count; i++) {
            ret = emmc_read_block(lba + i, dst + i * EMMC_BLOCK_SIZE);
            if (ret < 0)
                break;
        }
    }
    emmc_release();

    return ret;
}

static int emmc_blockdev_write(block_device_t *dev, uint64_t lba,
                               uint32_t count, const void *buffer)
{
    const uint8_t *src = (const uint8_t *)buffer;
    int ret = 0;

    (void)dev;
    if (!emmc_state.ready || !src)
        return -ENODEV;

    emmc_acquire();
    if (count > 1 && emmc_state.multiblock) {
        ret = emmc_write_blocks(lba, count, src);
        if (ret < 0) {
            emmc_state.multiblock = false;
            KWARN("bcm_emmc: multiblock write unavailable, using CMD24\n");
        }
    }
    if (count == 1 || !emmc_state.multiblock) {
        ret = 0;
        for (uint32_t i = 0; i < count; i++) {
            ret = emmc_write_block(lba + i, src + i * EMMC_BLOCK_SIZE);
            if (ret < 0)
                break;
        }
    }
    emmc_release();

    return ret;
}

static int emmc_blockdev_flush(block_device_t *dev)
{
    (void)dev;
    return emmc_state.ready ? 0 : -ENODEV;
}

static void emmc_blockdev_shutdown(block_device_t *dev)
{
    (void)dev;
    bcm2835_emmc_shutdown();
}

bool bcm2835_emmc_init(void)
{
    uint32_t resp[4] = {0};
    uint32_t ocr = 0;
    int ret;

    if (!arch_platform_has_emmc())
        return false;

    emmc_state.base = (volatile uint8_t *)(uintptr_t)arch_platform_emmc_kernel_base();
    emmc_state.rca = 0;
    emmc_state.high_capacity = false;
    emmc_state.wide_bus = false;
    emmc_state.multiblock = true;
    emmc_state.ready = false;
    emmc_busy = false;
    emmc_owner = NULL;

    if (!emmc_reset_host())
        return false;

    ret = emmc_send_command(0, 0, EMMC_CMD_RSP_NONE, NULL);
    if (ret < 0)
        return false;

    /*
     * CMD8 distinguishes SD v2 cards.  QEMU's raspi2 SD device and modern
     * cards respond; older cards can still continue through ACMD41 without HCS.
     */
    bool sd_v2 = emmc_send_command(8, 0x1AAu,
                                   EMMC_CMD_RSP_48 | EMMC_CMD_CRCCHK | EMMC_CMD_IXCHK,
                                   resp) == 0;

    for (uint32_t retry = 0; retry < 1000u; retry++) {
        uint32_t arg = sd_v2 ? 0x40FF8000u : 0x00FF8000u;

        ret = emmc_app_command(0, 41, arg, EMMC_CMD_RSP_48, resp);
        if (ret < 0)
            return false;

        ocr = resp[0];
        if (ocr & 0x80000000u)
            break;
    }

    if (!(ocr & 0x80000000u)) {
        KERROR("bcm_emmc: card did not leave idle state\n");
        return false;
    }

    emmc_state.high_capacity = (ocr & 0x40000000u) != 0;

    ret = emmc_send_command(2, 0, EMMC_CMD_RSP_136 | EMMC_CMD_CRCCHK, resp);
    if (ret < 0)
        return false;

    ret = emmc_send_command(3, 0,
                            EMMC_CMD_RSP_48 | EMMC_CMD_CRCCHK | EMMC_CMD_IXCHK,
                            resp);
    if (ret < 0)
        return false;
    emmc_state.rca = resp[0] >> 16;

    ret = emmc_send_command(7, emmc_state.rca << 16,
                            EMMC_CMD_RSP_48_BUSY | EMMC_CMD_CRCCHK | EMMC_CMD_IXCHK,
                            NULL);
    if (ret < 0)
        return false;

    if (!emmc_state.high_capacity) {
        ret = emmc_send_command(16, EMMC_BLOCK_SIZE,
                                EMMC_CMD_RSP_48 | EMMC_CMD_CRCCHK | EMMC_CMD_IXCHK,
                                NULL);
        if (ret < 0)
            return false;
    }

    /*
     * Select the SD four-bit data path when both card and host accept it.
     * ACMD6 failure is deliberately non-fatal: old cards and incomplete QEMU
     * board models continue on the validated one-bit path.
     */
    ret = emmc_app_command(emmc_state.rca, 6, 2u,
                           EMMC_CMD_RSP_48 | EMMC_CMD_CRCCHK |
                           EMMC_CMD_IXCHK, NULL);
    if (ret == 0) {
        uint32_t c0 = emmc_read(EMMC_CONTROL0);

        emmc_write(EMMC_CONTROL0, c0 | EMMC_CONTROL0_HCTL_DWIDTH);
        emmc_state.wide_bus = true;
    } else {
        emmc_reset_cmd_data();
        KWARN("bcm_emmc: four-bit bus unavailable, using one-bit mode\n");
    }

    if (!emmc_set_clock(EMMC_TRANSFER_CLOCK_HZ))
        return false;

    emmc_state.ready = true;
    emmc_block_dev.driver_data = &emmc_state;
    emmc_block_dev.capacity_sectors = EMMC_FALLBACK_SECTORS;
    if (!blk_register(&emmc_block_dev)) {
        emmc_state.ready = false;
        return false;
    }

    KINFO("BCM EMMC: rca=0x%04X %s %u-bit multiblock capacity<=%uMB (fallback bound)\n",
          emmc_state.rca,
          emmc_state.high_capacity ? "SDHC/SDXC" : "SDSC",
          emmc_state.wide_bus ? 4u : 1u,
          (uint32_t)((EMMC_FALLBACK_SECTORS * EMMC_BLOCK_SIZE) / (1024ULL * 1024ULL)));
    return true;
}

void bcm2835_emmc_shutdown(void)
{
    if (!emmc_state.ready)
        return;

    emmc_state.ready = false;
    blk_unregister(&emmc_block_dev);
}
