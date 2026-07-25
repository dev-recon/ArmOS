/*
 * ArmOS
 * Copyright (c) 2026 Mohamed Ennassiri
 *
 * Licensed under the Apache License, Version 2.0.
 * See LICENSE for details.
 *
 * File: include/kernel/block_device.h
 * Layer: Kernel / block device core
 *
 * Responsibilities:
 * - Provide the single sector I/O API used by filesystems.
 * - Decouple ext2/fat32/VFS from concrete transports such as VirtIO or SD/MMC.
 *
 * Notes:
 * - ArmOS currently supports one boot block device at a time. This is enough
 *   for qemu-virt and the first Raspberry Pi 2 SD-card milestone.
 */

#ifndef _KERNEL_BLOCK_DEVICE_H
#define _KERNEL_BLOCK_DEVICE_H

#include <kernel/types.h>

typedef struct block_device block_device_t;

typedef struct {
    uint64_t read_requests;
    uint64_t read_sectors;
    uint64_t read_errors;
    uint64_t write_requests;
    uint64_t write_sectors;
    uint64_t write_errors;
    uint64_t flush_requests;
    uint64_t flush_errors;
    uint32_t max_read_sectors;
    uint32_t max_write_sectors;
} block_device_stats_t;

typedef struct {
    uint64_t read_commands;
    uint64_t read_sectors;
    uint64_t read_errors;
    uint64_t write_commands;
    uint64_t write_sectors;
    uint64_t write_errors;
    uint64_t multiblock_fallbacks;
    uint32_t max_read_sectors;
    uint32_t max_write_sectors;
} block_transport_stats_t;

typedef struct {
    int (*read_sectors)(block_device_t *dev, uint64_t lba,
                        uint32_t count, void *buffer);
    int (*write_sectors)(block_device_t *dev, uint64_t lba,
                         uint32_t count, const void *buffer);
    int (*flush)(block_device_t *dev);
    void (*shutdown)(block_device_t *dev);
    void (*get_transport_stats)(block_device_t *dev,
                                block_transport_stats_t *stats);
} block_device_ops_t;

struct block_device {
    const char *name;
    const char *transport_name;
    uint64_t capacity_sectors;
    uint32_t sector_size;
    bool read_only;
    void *driver_data;
    const block_device_ops_t *ops;
};

bool blk_register(block_device_t *dev);
void blk_unregister(block_device_t *dev);

int blk_read_sectors(uint64_t lba, uint32_t count, void *buffer);
int blk_write_sectors(uint64_t lba, uint32_t count, void *buffer);
int blk_read_sector(uint64_t lba, void *buffer);
int blk_write_sector(uint64_t lba, void *buffer);
int blk_flush(void);
void blk_shutdown(void);

bool blk_is_initialized(void);
const char *blk_get_name(void);
const char *blk_get_transport_name(void);
uint64_t blk_get_capacity_sectors(void);
uint32_t blk_get_sector_size(void);
bool blk_is_readonly(void);
void blk_get_stats(block_device_stats_t *stats);
void blk_get_transport_stats(block_transport_stats_t *stats);

#endif /* _KERNEL_BLOCK_DEVICE_H */
