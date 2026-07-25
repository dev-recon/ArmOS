/*
 * ArmOS
 * Copyright (c) 2026 Mohamed Ennassiri
 *
 * Licensed under the Apache License, Version 2.0.
 * See LICENSE for details.
 *
 * File: kernel/drivers/mmc/bcm2835_sdhost.c
 * Layer: Kernel / block drivers
 *
 * Responsibilities:
 * - Drive the BCM2835 custom SDHOST controller connected to the Pi SD slot.
 * - Expose the system SD card through the architecture-neutral block API.
 * - Leave the Arasan SDHCI controller available for the Pi 3 SDIO radio.
 *
 * Notes:
 * - The controller is polling-only and uses PIO. Contiguous requests use
 *   CMD18/CMD25 and an explicit CMD12 stop.
 * - A failed multi-block request permanently falls back to the established
 *   CMD17/CMD24 path for the rest of the boot.
 * - GPIO48-53 are selected as the documented ALT0 SDHOST pin group.
 * - The stable Arasan driver remains available as a platform fallback.
 */

#include <kernel/arch_barrier.h>
#include <kernel/arch_cpu.h>
#include <kernel/arch_platform.h>
#include <kernel/block_device.h>
#include <kernel/gpio.h>
#include <kernel/kprintf.h>
#include <kernel/mmc/bcm2835_sdhost.h>
#include <kernel/spinlock.h>
#include <kernel/string.h>
#include <kernel/task.h>
#include <kernel/timer.h>
#include <kernel/types.h>

#define SDHOST_SDCMD       0x00u
#define SDHOST_SDARG       0x04u
#define SDHOST_SDTOUT      0x08u
#define SDHOST_SDCDIV      0x0cu
#define SDHOST_SDRSP0      0x10u
#define SDHOST_SDRSP1      0x14u
#define SDHOST_SDRSP2      0x18u
#define SDHOST_SDRSP3      0x1cu
#define SDHOST_SDHSTS      0x20u
#define SDHOST_SDVDD       0x30u
#define SDHOST_SDEDM       0x34u
#define SDHOST_SDHCFG      0x38u
#define SDHOST_SDHBCT      0x3cu
#define SDHOST_SDDATA      0x40u
#define SDHOST_SDHBLC      0x50u

#define SDCMD_NEW          (1u << 15)
#define SDCMD_FAIL         (1u << 14)
#define SDCMD_BUSYWAIT     (1u << 11)
#define SDCMD_NORESP       (1u << 10)
#define SDCMD_LONGRESP     (1u << 9)
#define SDCMD_WRITE        (1u << 7)
#define SDCMD_READ         (1u << 6)
#define SDCMD_INDEX_MASK   0x3fu

#define SDHSTS_BUSY        (1u << 10)
#define SDHSTS_BLOCK       (1u << 9)
#define SDHSTS_SDIO        (1u << 8)
#define SDHSTS_REW_TIMEOUT (1u << 7)
#define SDHSTS_CMD_TIMEOUT (1u << 6)
#define SDHSTS_CRC16       (1u << 5)
#define SDHSTS_CRC7        (1u << 4)
#define SDHSTS_FIFO        (1u << 3)
#define SDHSTS_CLEAR       (SDHSTS_BUSY | SDHSTS_BLOCK | SDHSTS_SDIO | \
                            SDHSTS_REW_TIMEOUT | SDHSTS_CMD_TIMEOUT | \
                            SDHSTS_CRC16 | SDHSTS_CRC7 | SDHSTS_FIFO)
#define SDHSTS_ERRORS      (SDHSTS_REW_TIMEOUT | SDHSTS_CMD_TIMEOUT | \
                            SDHSTS_CRC16 | SDHSTS_CRC7 | SDHSTS_FIFO)

#define SDHCFG_BUSY_IRQ_EN (1u << 10)
#define SDHCFG_BLOCK_IRQ_EN (1u << 8)
#define SDHCFG_SLOW_CARD   (1u << 3)
#define SDHCFG_WIDE_EXT    (1u << 2)
#define SDHCFG_WIDE_INT    (1u << 1)

#define SDEDM_FORCE_DATA   (1u << 19)
#define SDEDM_FIFO_SHIFT   4u
#define SDEDM_FIFO_MASK    0x1fu
#define SDEDM_WRITE_SHIFT  9u
#define SDEDM_READ_SHIFT   14u
#define SDEDM_THRESHOLD    4u
#define SDEDM_FSM_MASK     0x0fu
#define SDEDM_IDENT        0x00u
#define SDEDM_DATA         0x01u
#define SDEDM_READDATA     0x02u
#define SDEDM_WRITEDATA    0x03u
#define SDEDM_READWAIT     0x04u
#define SDEDM_READCRC      0x05u
#define SDEDM_WRITECRC     0x06u
#define SDEDM_WRITEWAIT1   0x07u
#define SDEDM_WRITESTART1  0x0au
#define SDEDM_WRITESTART2  0x0bu
#define SDEDM_WRITEWAIT2   0x0du

#define SDHOST_RESP_NONE   (1u << 0)
#define SDHOST_RESP_LONG   (1u << 1)
#define SDHOST_RESP_BUSY   (1u << 2)
#define SDHOST_DATA_READ   (1u << 3)
#define SDHOST_DATA_WRITE  (1u << 4)

#define SDHOST_BLOCK_SIZE        512u
#define SDHOST_FIFO_WORDS        16u
#define SDHOST_CORE_CLOCK_HZ     250000000u
#define SDHOST_INIT_CLOCK_HZ     400000u
#define SDHOST_TRANSFER_CLOCK_HZ 25000000u
#define SDHOST_TIMEOUT_MS        1000u
#define SDHOST_FALLBACK_SECTORS  (64ULL * 1024ULL * 1024ULL)

typedef struct {
    volatile uint8_t *base;
    uint32_t rca;
    bool high_capacity;
    bool wide_bus;
    bool multiblock;
    bool ready;
} bcm2835_sdhost_state_t;

static bcm2835_sdhost_state_t sdhost_state;
static spinlock_t sdhost_lock = SPINLOCK_INIT("bcm_sdhost");
static spinlock_t sdhost_stats_lock = SPINLOCK_INIT("bcm_sdhost_stats");
static volatile bool sdhost_busy;
static task_t *sdhost_owner;
static block_transport_stats_t sdhost_transport_stats;

static int sdhost_blockdev_read(block_device_t *dev, uint64_t lba,
                                uint32_t count, void *buffer);
static int sdhost_blockdev_write(block_device_t *dev, uint64_t lba,
                                 uint32_t count, const void *buffer);
static int sdhost_blockdev_flush(block_device_t *dev);
static void sdhost_blockdev_shutdown(block_device_t *dev);
static void sdhost_blockdev_get_transport_stats(
    block_device_t *dev, block_transport_stats_t *stats);

static const block_device_ops_t sdhost_block_ops = {
    .read_sectors = sdhost_blockdev_read,
    .write_sectors = sdhost_blockdev_write,
    .flush = sdhost_blockdev_flush,
    .shutdown = sdhost_blockdev_shutdown,
    .get_transport_stats = sdhost_blockdev_get_transport_stats,
};

static block_device_t sdhost_block_dev = {
    .name = "sd0",
    .transport_name = "bcm2835-sdhost",
    .sector_size = SDHOST_BLOCK_SIZE,
    .capacity_sectors = SDHOST_FALLBACK_SECTORS,
    .read_only = false,
    .ops = &sdhost_block_ops,
};

static inline uint32_t sdhost_read(uint32_t offset)
{
    return *(volatile uint32_t *)(sdhost_state.base + offset);
}

static inline void sdhost_write(uint32_t offset, uint32_t value)
{
    *(volatile uint32_t *)(sdhost_state.base + offset) = value;
    arch_data_memory_barrier();
}

static uint64_t sdhost_timeout_delta(uint32_t milliseconds)
{
    uint32_t frequency = arch_timer_frequency();
    uint32_t per_ms;
    uint32_t remainder;
    uint32_t delta;

    if (!frequency)
        frequency = TIMER_FALLBACK_FREQ;
    per_ms = frequency / 1000u;
    remainder = frequency % 1000u;
    delta = per_ms * milliseconds +
            (remainder * milliseconds + 999u) / 1000u;
    return delta ? delta : 1u;
}

static bool sdhost_deadline_expired(uint64_t start, uint32_t milliseconds)
{
    return arch_timer_counter() - start >= sdhost_timeout_delta(milliseconds);
}

static void sdhost_delay_ms(uint32_t milliseconds)
{
    uint64_t start = arch_timer_counter();

    while (!sdhost_deadline_expired(start, milliseconds))
        arch_cpu_relax();
}

static bool sdhost_wait_command_idle(void)
{
    uint64_t start = arch_timer_counter();

    while (sdhost_read(SDHOST_SDCMD) & SDCMD_NEW) {
        if (sdhost_deadline_expired(start, SDHOST_TIMEOUT_MS))
            return false;
        arch_cpu_relax();
    }
    return true;
}

static bool sdhost_fsm_is_write(uint32_t fsm)
{
    return fsm == SDEDM_WRITEDATA || fsm == SDEDM_WRITECRC ||
           fsm == SDEDM_WRITEWAIT1 || fsm == SDEDM_WRITESTART1 ||
           fsm == SDEDM_WRITESTART2 || fsm == SDEDM_WRITEWAIT2;
}

static int sdhost_wait_transfer_complete(void)
{
    uint64_t start = arch_timer_counter();

    while (1) {
        uint32_t edm = sdhost_read(SDHOST_SDEDM);
        uint32_t fsm = edm & SDEDM_FSM_MASK;
        uint32_t status = sdhost_read(SDHOST_SDHSTS);

        if (status & SDHSTS_ERRORS)
            return (status & (SDHSTS_CMD_TIMEOUT | SDHSTS_REW_TIMEOUT)) ?
                   -ETIMEDOUT : -EIO;
        if (fsm == SDEDM_IDENT || fsm == SDEDM_DATA)
            return 0;
        if (fsm == SDEDM_READWAIT || fsm == SDEDM_WRITESTART1 ||
            fsm == SDEDM_READDATA) {
            sdhost_write(SDHOST_SDEDM, edm | SDEDM_FORCE_DATA);
            return 0;
        }
        if (sdhost_deadline_expired(start, SDHOST_TIMEOUT_MS))
            return -ETIMEDOUT;
        arch_cpu_relax();
    }
}

static int sdhost_transfer_block(bool read, uint8_t *buffer)
{
    uint32_t words_left = SDHOST_BLOCK_SIZE / sizeof(uint32_t);
    uint64_t start = arch_timer_counter();

    while (words_left) {
        uint32_t edm = sdhost_read(SDHOST_SDEDM);
        uint32_t fsm = edm & SDEDM_FSM_MASK;
        uint32_t fill = (edm >> SDEDM_FIFO_SHIFT) & SDEDM_FIFO_MASK;
        uint32_t available = read ? fill : SDHOST_FIFO_WORDS - fill;
        uint32_t status = sdhost_read(SDHOST_SDHSTS);

        if (status & SDHSTS_ERRORS)
            return (status & (SDHSTS_CMD_TIMEOUT | SDHSTS_REW_TIMEOUT)) ?
                   -ETIMEDOUT : -EIO;
        if (read) {
            if (fsm != SDEDM_READDATA && fsm != SDEDM_READWAIT &&
                fsm != SDEDM_READCRC && fsm != SDEDM_DATA)
                available = 0;
        } else if (!sdhost_fsm_is_write(fsm) && fsm != SDEDM_DATA) {
            available = 0;
        }

        if (available > words_left)
            available = words_left;
        while (available--) {
            uint32_t offset = (SDHOST_BLOCK_SIZE / sizeof(uint32_t) -
                               words_left) * sizeof(uint32_t);
            uint32_t word;

            if (read) {
                word = sdhost_read(SDHOST_SDDATA);
                buffer[offset + 0u] = (uint8_t)word;
                buffer[offset + 1u] = (uint8_t)(word >> 8);
                buffer[offset + 2u] = (uint8_t)(word >> 16);
                buffer[offset + 3u] = (uint8_t)(word >> 24);
            } else {
                word = (uint32_t)buffer[offset + 0u] |
                       ((uint32_t)buffer[offset + 1u] << 8) |
                       ((uint32_t)buffer[offset + 2u] << 16) |
                       ((uint32_t)buffer[offset + 3u] << 24);
                sdhost_write(SDHOST_SDDATA, word);
            }
            words_left--;
        }

        if (sdhost_deadline_expired(start, SDHOST_TIMEOUT_MS))
            return -ETIMEDOUT;
        arch_cpu_relax();
    }

    return 0;
}

static int sdhost_send_command_blocks(uint32_t command, uint32_t argument,
                                      uint32_t flags, uint8_t *data,
                                      uint32_t blocks,
                                      uint32_t response[4])
{
    uint32_t command_word = command & SDCMD_INDEX_MASK;
    uint32_t status;

    if ((data && blocks == 0u) || (!data && blocks != 0u) ||
        blocks > 0xffffu)
        return -EINVAL;

    if (!sdhost_wait_command_idle())
        return -ETIMEDOUT;

    status = sdhost_read(SDHOST_SDHSTS);
    if (status & SDHSTS_CLEAR)
        sdhost_write(SDHOST_SDHSTS, status & SDHSTS_CLEAR);

    if (flags & SDHOST_RESP_NONE)
        command_word |= SDCMD_NORESP;
    if (flags & SDHOST_RESP_LONG)
        command_word |= SDCMD_LONGRESP;
    if (flags & SDHOST_RESP_BUSY)
        command_word |= SDCMD_BUSYWAIT;
    if (flags & SDHOST_DATA_READ)
        command_word |= SDCMD_READ;
    if (flags & SDHOST_DATA_WRITE)
        command_word |= SDCMD_WRITE;

    if (data) {
        sdhost_write(SDHOST_SDHBCT, SDHOST_BLOCK_SIZE);
        sdhost_write(SDHOST_SDHBLC, blocks);
    }
    sdhost_write(SDHOST_SDARG, argument);
    sdhost_write(SDHOST_SDCMD, command_word | SDCMD_NEW);

    if (!sdhost_wait_command_idle())
        return -ETIMEDOUT;

    command_word = sdhost_read(SDHOST_SDCMD);
    status = sdhost_read(SDHOST_SDHSTS);
    if (command_word & SDCMD_FAIL) {
        /* ACMD41 has no valid CRC field while the card negotiates its OCR. */
        if (command != 41u || (status & ~SDHSTS_CRC7)) {
            sdhost_write(SDHOST_SDHSTS, status & SDHSTS_CLEAR);
            return (status & SDHSTS_CMD_TIMEOUT) ? -ETIMEDOUT : -EIO;
        }
        sdhost_write(SDHOST_SDHSTS, SDHSTS_CRC7);
    }

    if (response) {
        if (flags & SDHOST_RESP_LONG) {
            response[3] = sdhost_read(SDHOST_SDRSP0);
            response[2] = sdhost_read(SDHOST_SDRSP1);
            response[1] = sdhost_read(SDHOST_SDRSP2);
            response[0] = sdhost_read(SDHOST_SDRSP3);
        } else {
            response[0] = sdhost_read(SDHOST_SDRSP0);
            response[1] = response[2] = response[3] = 0u;
        }
    }

    if (data) {
        bool read = (flags & SDHOST_DATA_READ) != 0;

        for (uint32_t block = 0; block < blocks; block++) {
            int ret = sdhost_transfer_block(
                read, data + block * SDHOST_BLOCK_SIZE);

            if (ret < 0)
                return ret;
        }
    }

    if (flags & SDHOST_RESP_BUSY) {
        uint64_t start = arch_timer_counter();

        while (!(sdhost_read(SDHOST_SDHSTS) & SDHSTS_BUSY)) {
            if (sdhost_deadline_expired(start, SDHOST_TIMEOUT_MS))
                return -ETIMEDOUT;
            arch_cpu_relax();
        }
        sdhost_write(SDHOST_SDHSTS, SDHSTS_BUSY);
    }
    return 0;
}

static int sdhost_send_command(uint32_t command, uint32_t argument,
                               uint32_t flags, uint8_t *data,
                               uint32_t response[4])
{
    int ret = sdhost_send_command_blocks(command, argument, flags, data,
                                         data ? 1u : 0u, response);

    if (ret == 0 && data)
        ret = sdhost_wait_transfer_complete();
    return ret;
}

static int sdhost_app_command(uint32_t rca, uint32_t command,
                              uint32_t argument, uint32_t flags,
                              uint32_t response[4])
{
    int ret = sdhost_send_command(55u, rca << 16, 0u, NULL, NULL);

    if (ret < 0)
        return ret;
    return sdhost_send_command(command, argument, flags, NULL, response);
}

static bool sdhost_set_clock(uint32_t target_hz)
{
    uint32_t divisor;
    uint32_t actual_hz;

    if (!target_hz)
        return false;
    divisor = (SDHOST_CORE_CLOCK_HZ + target_hz - 1u) / target_hz;
    divisor = divisor > 2u ? divisor - 2u : 0u;
    if (divisor > 0x7ffu)
        divisor = 0x7ffu;
    sdhost_write(SDHOST_SDCDIV, divisor);
    actual_hz = SDHOST_CORE_CLOCK_HZ / (divisor + 2u);
    sdhost_write(SDHOST_SDTOUT, actual_hz / 2u);
    return true;
}

static bool sdhost_configure_pins(void)
{
    if (!bcm283x_gpio_init())
        return false;
    if (gpio_configure(48u, GPIO_FUNCTION_ALT0, GPIO_PULL_NONE) < 0)
        return false;
    for (uint32_t pin = 49u; pin <= 53u; pin++) {
        if (gpio_configure(pin, GPIO_FUNCTION_ALT0, GPIO_PULL_UP) < 0)
            return false;
    }
    return true;
}

static bool sdhost_reset(void)
{
    uint32_t edm;

    sdhost_write(SDHOST_SDVDD, 0u);
    sdhost_write(SDHOST_SDCMD, 0u);
    sdhost_write(SDHOST_SDARG, 0u);
    sdhost_write(SDHOST_SDTOUT, 0x00f00000u);
    sdhost_write(SDHOST_SDHCFG, 0u);
    sdhost_write(SDHOST_SDHSTS, SDHSTS_CLEAR);

    edm = sdhost_read(SDHOST_SDEDM);
    edm &= ~((0x1fu << SDEDM_WRITE_SHIFT) |
             (0x1fu << SDEDM_READ_SHIFT));
    edm |= SDEDM_THRESHOLD << SDEDM_WRITE_SHIFT;
    edm |= SDEDM_THRESHOLD << SDEDM_READ_SHIFT;
    sdhost_write(SDHOST_SDEDM, edm);
    sdhost_delay_ms(20u);

    sdhost_write(SDHOST_SDVDD, 1u);
    sdhost_delay_ms(20u);
    sdhost_write(SDHOST_SDHCFG, SDHCFG_SLOW_CARD | SDHCFG_WIDE_INT |
                                  SDHCFG_BUSY_IRQ_EN | SDHCFG_BLOCK_IRQ_EN);
    return sdhost_set_clock(SDHOST_INIT_CLOCK_HZ);
}

static void sdhost_acquire(void)
{
    while (1) {
        task_t *task = task_current_local();
        unsigned long irq_flags;

        spin_lock_irqsave(&sdhost_lock, &irq_flags);
        if (!sdhost_busy) {
            sdhost_busy = true;
            sdhost_owner = task;
            spin_unlock_irqrestore(&sdhost_lock, irq_flags);
            return;
        }
        spin_unlock_irqrestore(&sdhost_lock, irq_flags);
        if (task)
            yield();
        else
            arch_cpu_relax();
    }
}

static void sdhost_release(void)
{
    task_t *task = task_current_local();
    unsigned long irq_flags;

    spin_lock_irqsave(&sdhost_lock, &irq_flags);
    if (!sdhost_busy)
        KERROR("bcm_sdhost: release without owner\n");
    else if (sdhost_owner && task && sdhost_owner != task)
        KERROR("bcm_sdhost: release by non-owner\n");
    sdhost_busy = false;
    sdhost_owner = NULL;
    spin_unlock_irqrestore(&sdhost_lock, irq_flags);
}

static void sdhost_account_data_command(bool read, uint32_t sectors, int ret)
{
    unsigned long irq_flags;

    spin_lock_irqsave(&sdhost_stats_lock, &irq_flags);
    if (read) {
        sdhost_transport_stats.read_commands++;
        sdhost_transport_stats.read_sectors += sectors;
        if (ret < 0)
            sdhost_transport_stats.read_errors++;
        if (sectors > sdhost_transport_stats.max_read_sectors)
            sdhost_transport_stats.max_read_sectors = sectors;
    } else {
        sdhost_transport_stats.write_commands++;
        sdhost_transport_stats.write_sectors += sectors;
        if (ret < 0)
            sdhost_transport_stats.write_errors++;
        if (sectors > sdhost_transport_stats.max_write_sectors)
            sdhost_transport_stats.max_write_sectors = sectors;
    }
    spin_unlock_irqrestore(&sdhost_stats_lock, irq_flags);
}

static void sdhost_account_multiblock_fallback(void)
{
    unsigned long irq_flags;

    spin_lock_irqsave(&sdhost_stats_lock, &irq_flags);
    sdhost_transport_stats.multiblock_fallbacks++;
    spin_unlock_irqrestore(&sdhost_stats_lock, irq_flags);
}

static int sdhost_data_command(uint32_t command, uint32_t argument,
                               bool read, uint8_t *buffer, uint32_t blocks)
{
    uint32_t flags = read ? SDHOST_DATA_READ : SDHOST_DATA_WRITE;
    int ret;

    ret = sdhost_send_command_blocks(command, argument, flags, buffer,
                                     blocks, NULL);
    if (blocks > 1u) {
        int stop_ret = sdhost_send_command(12u, 0u, SDHOST_RESP_BUSY,
                                           NULL, NULL);
        int complete_ret = sdhost_wait_transfer_complete();

        if (ret == 0)
            ret = stop_ret;
        if (ret == 0)
            ret = complete_ret;
    } else if (ret == 0) {
        ret = sdhost_wait_transfer_complete();
    }

    sdhost_account_data_command(read, blocks, ret);
    return ret;
}

static int sdhost_transfer_sector(uint64_t lba, uint8_t *buffer, bool read)
{
    uint32_t argument;

    if (lba > 0xffffffffULL ||
        (!sdhost_state.high_capacity &&
         lba > 0xffffffffULL / SDHOST_BLOCK_SIZE))
        return -EINVAL;
    argument = sdhost_state.high_capacity ? (uint32_t)lba :
               (uint32_t)(lba * SDHOST_BLOCK_SIZE);
    return sdhost_data_command(read ? 17u : 24u, argument, read, buffer, 1u);
}

static int sdhost_transfer_sectors(uint64_t lba, uint32_t count,
                                   uint8_t *buffer, bool read)
{
    uint32_t argument;

    if (!buffer || count < 2u || count > 0xffffu ||
        lba > 0xffffffffULL ||
        (uint64_t)count - 1u > 0xffffffffULL - lba)
        return -EINVAL;
    if (!sdhost_state.high_capacity &&
        (lba > 0xffffffffULL / SDHOST_BLOCK_SIZE ||
         (uint64_t)count - 1u >
             0xffffffffULL / SDHOST_BLOCK_SIZE - lba))
        return -EINVAL;

    argument = sdhost_state.high_capacity ? (uint32_t)lba :
               (uint32_t)(lba * SDHOST_BLOCK_SIZE);
    return sdhost_data_command(read ? 18u : 25u, argument, read,
                               buffer, count);
}

static int sdhost_blockdev_read(block_device_t *dev, uint64_t lba,
                                uint32_t count, void *buffer)
{
    uint8_t *bytes = (uint8_t *)buffer;
    int ret = 0;

    (void)dev;
    if (!sdhost_state.ready || !bytes)
        return -ENODEV;
    sdhost_acquire();
    if (count > 1u && sdhost_state.multiblock) {
        ret = sdhost_transfer_sectors(lba, count, bytes, true);
        if (ret < 0) {
            sdhost_state.multiblock = false;
            sdhost_account_multiblock_fallback();
            KWARN("bcm_sdhost: multiblock read unavailable, using CMD17\n");
        }
    }
    if (count == 1u || !sdhost_state.multiblock) {
        ret = 0;
        for (uint32_t i = 0; i < count; i++) {
            ret = sdhost_transfer_sector(lba + i,
                                         bytes + i * SDHOST_BLOCK_SIZE, true);
            if (ret < 0)
                break;
        }
    }
    sdhost_release();
    return ret;
}

static int sdhost_blockdev_write(block_device_t *dev, uint64_t lba,
                                 uint32_t count, const void *buffer)
{
    uint8_t *bytes = (uint8_t *)(uintptr_t)buffer;
    int ret = 0;

    (void)dev;
    if (!sdhost_state.ready || !bytes)
        return -ENODEV;
    sdhost_acquire();
    if (count > 1u && sdhost_state.multiblock) {
        ret = sdhost_transfer_sectors(lba, count, bytes, false);
        if (ret < 0) {
            sdhost_state.multiblock = false;
            sdhost_account_multiblock_fallback();
            KWARN("bcm_sdhost: multiblock write unavailable, using CMD24\n");
        }
    }
    if (count == 1u || !sdhost_state.multiblock) {
        ret = 0;
        for (uint32_t i = 0; i < count; i++) {
            ret = sdhost_transfer_sector(lba + i,
                                         bytes + i * SDHOST_BLOCK_SIZE, false);
            if (ret < 0)
                break;
        }
    }
    sdhost_release();
    return ret;
}

static int sdhost_blockdev_flush(block_device_t *dev)
{
    (void)dev;
    return sdhost_state.ready ? 0 : -ENODEV;
}

static void sdhost_blockdev_shutdown(block_device_t *dev)
{
    (void)dev;
    bcm2835_sdhost_shutdown();
}

static void sdhost_blockdev_get_transport_stats(
    block_device_t *dev, block_transport_stats_t *stats)
{
    unsigned long irq_flags;

    (void)dev;
    if (!stats)
        return;
    spin_lock_irqsave(&sdhost_stats_lock, &irq_flags);
    *stats = sdhost_transport_stats;
    spin_unlock_irqrestore(&sdhost_stats_lock, irq_flags);
}

bool bcm2835_sdhost_init(void)
{
    uint32_t response[4] = {0};
    uint32_t ocr = 0;
    bool sd_v2;
    int ret;

    sdhost_state.base = (volatile uint8_t *)(uintptr_t)
                        arch_platform_kernel_mmio_sdhost_base();
    if (!sdhost_state.base)
        return false;
    sdhost_state.rca = 0u;
    sdhost_state.high_capacity = false;
    sdhost_state.wide_bus = false;
    sdhost_state.multiblock = true;
    sdhost_state.ready = false;
    sdhost_busy = false;
    sdhost_owner = NULL;
    memset(&sdhost_transport_stats, 0, sizeof(sdhost_transport_stats));

    if (!sdhost_configure_pins() || !sdhost_reset())
        return false;
    if (sdhost_send_command(0u, 0u, SDHOST_RESP_NONE, NULL, NULL) < 0)
        return false;

    sd_v2 = sdhost_send_command(8u, 0x1aau, 0u, NULL, response) == 0;
    for (uint32_t retry = 0; retry < 1000u; retry++) {
        uint32_t argument = sd_v2 ? 0x40ff8000u : 0x00ff8000u;

        ret = sdhost_app_command(0u, 41u, argument, 0u, response);
        if (ret < 0)
            return false;
        ocr = response[0];
        if (ocr & 0x80000000u)
            break;
        sdhost_delay_ms(1u);
    }
    if (!(ocr & 0x80000000u)) {
        KERROR("bcm_sdhost: card did not leave idle state\n");
        return false;
    }
    sdhost_state.high_capacity = (ocr & 0x40000000u) != 0u;

    if (sdhost_send_command(2u, 0u, SDHOST_RESP_LONG, NULL, response) < 0)
        return false;
    if (sdhost_send_command(3u, 0u, 0u, NULL, response) < 0)
        return false;
    sdhost_state.rca = response[0] >> 16;
    if (sdhost_send_command(7u, sdhost_state.rca << 16,
                            SDHOST_RESP_BUSY, NULL, NULL) < 0)
        return false;

    if (!sdhost_state.high_capacity &&
        sdhost_send_command(16u, SDHOST_BLOCK_SIZE, 0u, NULL, NULL) < 0)
        return false;

    ret = sdhost_app_command(sdhost_state.rca, 6u, 2u, 0u, NULL);
    if (ret == 0) {
        uint32_t config = sdhost_read(SDHOST_SDHCFG);

        sdhost_write(SDHOST_SDHCFG, config | SDHCFG_WIDE_EXT |
                                       SDHCFG_WIDE_INT);
        sdhost_state.wide_bus = true;
    } else {
        sdhost_write(SDHOST_SDHSTS, SDHSTS_CLEAR);
        KWARN("bcm_sdhost: four-bit bus unavailable, using one-bit mode\n");
    }

    if (!sdhost_set_clock(SDHOST_TRANSFER_CLOCK_HZ))
        return false;

    sdhost_state.ready = true;
    sdhost_block_dev.driver_data = &sdhost_state;
    sdhost_block_dev.capacity_sectors = SDHOST_FALLBACK_SECTORS;
    if (!blk_register(&sdhost_block_dev)) {
        sdhost_state.ready = false;
        return false;
    }

    KINFO("BCM SDHOST: rca=0x%04X %s %u-bit multiblock capacity<=%uMB\n",
          sdhost_state.rca,
          sdhost_state.high_capacity ? "SDHC/SDXC" : "SDSC",
          sdhost_state.wide_bus ? 4u : 1u,
          (uint32_t)((SDHOST_FALLBACK_SECTORS * SDHOST_BLOCK_SIZE) /
                     (1024ULL * 1024ULL)));
    return true;
}

void bcm2835_sdhost_shutdown(void)
{
    if (!sdhost_state.ready)
        return;
    sdhost_state.ready = false;
    blk_unregister(&sdhost_block_dev);
    sdhost_write(SDHOST_SDVDD, 0u);
}
