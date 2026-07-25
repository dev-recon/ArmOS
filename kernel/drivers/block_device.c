/*
 * ArmOS
 * Copyright (c) 2026 Mohamed Ennassiri
 *
 * Licensed under the Apache License, Version 2.0.
 * See LICENSE for details.
 *
 * File: kernel/drivers/block_device.c
 * Layer: Kernel / block device core
 *
 * Responsibilities:
 * - Own the active boot block-device dispatch table.
 * - Keep filesystem code independent from VirtIO or SD/MMC implementation
 *   details.
 *
 * Notes:
 * - This deliberately starts as a single-device core. A real device model can
 *   grow later once ArmOS has more than one storage controller live at once.
 */

#include <kernel/block_device.h>
#include <kernel/kprintf.h>
#include <kernel/spinlock.h>
#include <kernel/string.h>

static block_device_t *active_block;
static spinlock_t block_lock = SPINLOCK_INIT("blockdev");
static block_device_stats_t block_stats;

static void blk_account_request(bool write, uint32_t count)
{
    unsigned long flags;

    spin_lock_irqsave(&block_lock, &flags);
    if (write) {
        block_stats.write_requests++;
        block_stats.write_sectors += count;
        if (count > block_stats.max_write_sectors)
            block_stats.max_write_sectors = count;
    } else {
        block_stats.read_requests++;
        block_stats.read_sectors += count;
        if (count > block_stats.max_read_sectors)
            block_stats.max_read_sectors = count;
    }
    spin_unlock_irqrestore(&block_lock, flags);
}

static void blk_account_error(bool write)
{
    unsigned long flags;

    spin_lock_irqsave(&block_lock, &flags);
    if (write)
        block_stats.write_errors++;
    else
        block_stats.read_errors++;
    spin_unlock_irqrestore(&block_lock, flags);
}

bool blk_register(block_device_t *dev)
{
    unsigned long flags;

    if (!dev || !dev->ops || !dev->ops->read_sectors ||
        !dev->name || dev->sector_size == 0 || dev->capacity_sectors == 0) {
        return false;
    }

    spin_lock_irqsave(&block_lock, &flags);
    if (active_block && active_block != dev) {
        spin_unlock_irqrestore(&block_lock, flags);
        KERROR("block: active device already registered: %s\n",
               active_block->name ? active_block->name : "?");
        return false;
    }

    active_block = dev;
    memset(&block_stats, 0, sizeof(block_stats));
    spin_unlock_irqrestore(&block_lock, flags);
    return true;
}

void blk_unregister(block_device_t *dev)
{
    unsigned long flags;

    spin_lock_irqsave(&block_lock, &flags);
    if (active_block == dev)
        active_block = NULL;
    spin_unlock_irqrestore(&block_lock, flags);
}

static block_device_t *blk_active(void)
{
    return active_block;
}

int blk_read_sectors(uint64_t lba, uint32_t count, void *buffer)
{
    block_device_t *dev = blk_active();
    int ret;

    if (!dev || !dev->ops || !dev->ops->read_sectors || !buffer || count == 0)
        return -1;
    if (lba >= dev->capacity_sectors ||
        (uint64_t)count > dev->capacity_sectors - lba)
        return -1;

    blk_account_request(false, count);
    ret = dev->ops->read_sectors(dev, lba, count, buffer);
    if (ret < 0)
        blk_account_error(false);
    return ret;
}

int blk_write_sectors(uint64_t lba, uint32_t count, void *buffer)
{
    block_device_t *dev = blk_active();
    int ret;

    if (!dev || !dev->ops || !dev->ops->write_sectors || !buffer || count == 0)
        return -1;
    if (dev->read_only)
        return -1;
    if (lba >= dev->capacity_sectors ||
        (uint64_t)count > dev->capacity_sectors - lba)
        return -1;

    blk_account_request(true, count);
    ret = dev->ops->write_sectors(dev, lba, count, buffer);
    if (ret < 0)
        blk_account_error(true);
    return ret;
}

int blk_read_sector(uint64_t lba, void *buffer)
{
    return blk_read_sectors(lba, 1, buffer);
}

int blk_write_sector(uint64_t lba, void *buffer)
{
    return blk_write_sectors(lba, 1, buffer);
}

int blk_flush(void)
{
    block_device_t *dev = blk_active();
    unsigned long flags;
    int ret;

    if (!dev)
        return -1;
    spin_lock_irqsave(&block_lock, &flags);
    block_stats.flush_requests++;
    spin_unlock_irqrestore(&block_lock, flags);

    ret = (!dev->ops || !dev->ops->flush) ? 0 : dev->ops->flush(dev);
    if (ret < 0) {
        spin_lock_irqsave(&block_lock, &flags);
        block_stats.flush_errors++;
        spin_unlock_irqrestore(&block_lock, flags);
    }
    return ret;
}

void blk_shutdown(void)
{
    block_device_t *dev = blk_active();

    if (dev && dev->ops && dev->ops->shutdown)
        dev->ops->shutdown(dev);
}

bool blk_is_initialized(void)
{
    return active_block != NULL;
}

const char *blk_get_name(void)
{
    block_device_t *dev = blk_active();

    return dev && dev->name ? dev->name : "none";
}

const char *blk_get_transport_name(void)
{
    block_device_t *dev = blk_active();

    return dev && dev->transport_name ? dev->transport_name : "unknown";
}

uint64_t blk_get_capacity_sectors(void)
{
    block_device_t *dev = blk_active();

    return dev ? dev->capacity_sectors : 0;
}

uint32_t blk_get_sector_size(void)
{
    block_device_t *dev = blk_active();

    return dev ? dev->sector_size : 0;
}

bool blk_is_readonly(void)
{
    block_device_t *dev = blk_active();

    return dev ? dev->read_only : false;
}

void blk_get_stats(block_device_stats_t *stats)
{
    unsigned long flags;

    if (!stats)
        return;

    spin_lock_irqsave(&block_lock, &flags);
    *stats = block_stats;
    spin_unlock_irqrestore(&block_lock, flags);
}

void blk_get_transport_stats(block_transport_stats_t *stats)
{
    block_device_t *dev;
    block_device_stats_t logical;

    if (!stats)
        return;

    dev = blk_active();
    memset(stats, 0, sizeof(*stats));
    if (dev && dev->ops && dev->ops->get_transport_stats) {
        dev->ops->get_transport_stats(dev, stats);
        return;
    }

    /*
     * Most transports issue one hardware request for each block-layer
     * request. Drivers that split or merge requests override this snapshot.
     */
    blk_get_stats(&logical);
    stats->read_commands = logical.read_requests;
    stats->read_sectors = logical.read_sectors;
    stats->read_errors = logical.read_errors;
    stats->write_commands = logical.write_requests;
    stats->write_sectors = logical.write_sectors;
    stats->write_errors = logical.write_errors;
    stats->max_read_sectors = logical.max_read_sectors;
    stats->max_write_sectors = logical.max_write_sectors;
}
