/*
 * ArmOS
 * Copyright (c) 2026 Mohamed Ennassiri
 *
 * Licensed under the Apache License, Version 2.0.
 * See LICENSE for details.
 *
 * File: include/kernel/fdt.h
 * Layer: Kernel / device tree
 *
 * Responsibilities:
 * - Provide a tiny flattened device tree reader for early boot and drivers.
 * - Keep byte-order, node iteration, and common property decoding centralized.
 *
 * Notes:
 * - This is intentionally small, not a full libfdt clone.
 */

#ifndef _KERNEL_FDT_H
#define _KERNEL_FDT_H

#include <kernel/types.h>

struct fdt_header {
    uint32_t magic;
    uint32_t totalsize;
    uint32_t off_dt_struct;
    uint32_t off_dt_strings;
    uint32_t off_mem_rsvmap;
    uint32_t version;
    uint32_t last_comp_version;
    uint32_t boot_cpuid_phys;
    uint32_t size_dt_strings;
    uint32_t size_dt_struct;
};

#define FDT_MAGIC         0xd00dfeedu
#define FDT_BEGIN_NODE    0x00000001u
#define FDT_END_NODE      0x00000002u
#define FDT_PROP          0x00000003u
#define FDT_NOP           0x00000004u
#define FDT_END           0x00000009u

#define FDT_MEMORY_MAX_RANGES   8u
#define FDT_RESERVED_MAX_RANGES 16u

typedef struct {
    paddr_t start;
    paddr_t size;
} fdt_memory_range_t;

typedef struct {
    fdt_memory_range_t memory[FDT_MEMORY_MAX_RANGES];
    fdt_memory_range_t reserved[FDT_RESERVED_MAX_RANGES];
    uint32_t memory_count;
    uint32_t reserved_count;
    paddr_t dtb_start;
    paddr_t dtb_size;
} fdt_memory_layout_t;

typedef bool (*fdt_node_cb_t)(void *dtb_ptr, void *node_ptr,
                              const char *name, void *ctx);

uint32_t fdt32_to_cpu(uint32_t x);
bool fdt_check_header(void *dtb_ptr);
bool fdt_node_matches(const char *node_name, const char *prefix);
void *fdt_find_node_by_name(void *dtb_ptr, const char *node_name);
bool fdt_device_present(void *dtb_ptr, const char *partial_name);
void *fdt_get_property(void *dtb_ptr, void *node_ptr,
                       const char *property_name, uint32_t *out_len);
bool fdt_for_each_node(void *dtb_ptr, fdt_node_cb_t cb, void *ctx);
uint32_t fdt_count_cpus(void *dtb_ptr, uint32_t maximum);

bool fdt_decode_reg_addr(const uint32_t *reg, uint32_t len, paddr_t *out_phys);
bool fdt_decode_gic_interrupt(const uint32_t *intr, uint32_t len,
                              uint32_t fallback_irq, uint32_t *out_irq,
                              bool *out_edge);
bool fdt_find_virtio_mmio_device(uint32_t virtio_id, paddr_t *out_phys,
                                 uint32_t *out_irq, bool *out_edge);
bool fdt_find_virtio_mmio_device_at(uint32_t virtio_id, uint32_t index,
                                    paddr_t *out_phys, uint32_t *out_irq,
                                    bool *out_edge);

/* Dependency-free early-boot memory topology reader. */
bool fdt_read_memory_layout(void *dtb_ptr, fdt_memory_layout_t *layout);

#endif /* _KERNEL_FDT_H */
