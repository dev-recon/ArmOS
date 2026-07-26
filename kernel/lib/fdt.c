/*
 * ArmOS
 * Copyright (c) 2026 Mohamed Ennassiri
 *
 * Licensed under the Apache License, Version 2.0.
 * See LICENSE for details.
 *
 * File: kernel/lib/fdt.c
 * Layer: Kernel / device tree
 *
 * Responsibilities:
 * - Parse the small subset of flattened device tree data ArmOS needs today.
 * - Decode common QEMU virt properties used by memory, SMP, and VirtIO MMIO.
 *
 * Notes:
 * - The parser is read-only and linear.  Drivers should not open-code FDT
 *   traversal; add narrow helpers here when another property becomes common.
 */

#include <kernel/fdt.h>
#include <kernel/string.h>
#include <kernel/virtio_block.h>
#include <kernel/arch_platform.h>

extern uintptr_t dtb_address;

uint32_t fdt32_to_cpu(uint32_t x)
{
    return __builtin_bswap32(x);
}

bool fdt_check_header(void *dtb_ptr)
{
    if (!dtb_ptr)
        return false;

    struct fdt_header *fdt = (struct fdt_header *)dtb_ptr;
    return fdt32_to_cpu(fdt->magic) == FDT_MAGIC;
}

bool fdt_node_matches(const char *node_name, const char *prefix)
{
    size_t len;

    if (!node_name || !prefix)
        return false;

    len = strlen(prefix);
    return strncmp(node_name, prefix, len) == 0 &&
           (node_name[len] == '@' || node_name[len] == '\0');
}

bool fdt_for_each_node(void *dtb_ptr, fdt_node_cb_t cb, void *ctx)
{
    if (!fdt_check_header(dtb_ptr) || !cb)
        return false;

    struct fdt_header *fdt = (struct fdt_header *)dtb_ptr;
    uint8_t *struct_block = (uint8_t *)dtb_ptr + fdt32_to_cpu(fdt->off_dt_struct);
    uint32_t *token = (uint32_t *)struct_block;

    while (1) {
        uint32_t tag = fdt32_to_cpu(*token++);

        switch (tag) {
        case FDT_BEGIN_NODE: {
            void *node_ptr = (void *)(token - 1);
            const char *name = (const char *)token;
            size_t len = strlen(name);

            if (cb(dtb_ptr, node_ptr, name, ctx))
                return true;

            token += (len + 4) / 4;
            break;
        }
        case FDT_PROP: {
            uint32_t len = fdt32_to_cpu(*token++);
            token++;
            token += (len + 3) / 4;
            break;
        }
        case FDT_END_NODE:
        case FDT_NOP:
            break;
        case FDT_END:
            return false;
        default:
            return false;
        }
    }
}

typedef struct {
    uint32_t count;
    uint32_t maximum;
} fdt_cpu_count_ctx_t;

static bool fdt_count_cpu_cb(void *dtb_ptr, void *node_ptr,
                             const char *name, void *opaque)
{
    fdt_cpu_count_ctx_t *ctx = opaque;

    (void)dtb_ptr;
    (void)node_ptr;
    if (fdt_node_matches(name, "cpu") && ctx->count < ctx->maximum)
        ctx->count++;
    return ctx->count == ctx->maximum;
}

uint32_t fdt_count_cpus(void *dtb_ptr, uint32_t maximum)
{
    fdt_cpu_count_ctx_t ctx = {
        .count = 0,
        .maximum = maximum,
    };

    if (maximum == 0 || !fdt_check_header(dtb_ptr))
        return 0;
    (void)fdt_for_each_node(dtb_ptr, fdt_count_cpu_cb, &ctx);
    return ctx.count;
}

typedef struct {
    const char *node_name;
    void *node;
} fdt_find_node_ctx_t;

static bool fdt_find_node_cb(void *dtb_ptr, void *node_ptr,
                             const char *name, void *opaque)
{
    (void)dtb_ptr;

    fdt_find_node_ctx_t *ctx = (fdt_find_node_ctx_t *)opaque;
    if (fdt_node_matches(name, ctx->node_name)) {
        ctx->node = node_ptr;
        return true;
    }
    return false;
}

void *fdt_find_node_by_name(void *dtb_ptr, const char *node_name)
{
    fdt_find_node_ctx_t ctx = {
        .node_name = node_name,
        .node = NULL,
    };

    fdt_for_each_node(dtb_ptr, fdt_find_node_cb, &ctx);
    return ctx.node;
}

typedef struct {
    const char *partial_name;
    bool found;
} fdt_present_ctx_t;

static bool fdt_device_present_cb(void *dtb_ptr, void *node_ptr,
                                  const char *name, void *opaque)
{
    (void)dtb_ptr;
    (void)node_ptr;

    fdt_present_ctx_t *ctx = (fdt_present_ctx_t *)opaque;
    if (strstr(name, ctx->partial_name) != NULL) {
        ctx->found = true;
        return true;
    }
    return false;
}

bool fdt_device_present(void *dtb_ptr, const char *partial_name)
{
    fdt_present_ctx_t ctx = {
        .partial_name = partial_name,
        .found = false,
    };

    if (!partial_name)
        return false;

    fdt_for_each_node(dtb_ptr, fdt_device_present_cb, &ctx);
    return ctx.found;
}

void *fdt_get_property(void *dtb_ptr, void *node_ptr,
                       const char *property_name, uint32_t *out_len)
{
    if (!fdt_check_header(dtb_ptr) || !node_ptr || !property_name)
        return NULL;

    struct fdt_header *fdt = (struct fdt_header *)dtb_ptr;
    uint8_t *strings_block = (uint8_t *)dtb_ptr + fdt32_to_cpu(fdt->off_dt_strings);
    uint32_t *token = (uint32_t *)node_ptr;

    token++;
    const char *node_name = (const char *)token;
    token += (strlen(node_name) + 4) / 4;

    while (1) {
        uint32_t tag = fdt32_to_cpu(*token++);

        switch (tag) {
        case FDT_PROP: {
            uint32_t len = fdt32_to_cpu(*token++);
            uint32_t name_off = fdt32_to_cpu(*token++);
            const char *name = (const char *)(strings_block + name_off);

            if (strcmp(name, property_name) == 0) {
                if (out_len)
                    *out_len = len;
                return (void *)token;
            }

            token += (len + 3) / 4;
            break;
        }
        case FDT_END_NODE:
        case FDT_END:
            return NULL;
        case FDT_NOP:
            break;
        default:
            return NULL;
        }
    }
}

bool fdt_decode_reg_addr(const uint32_t *reg, uint32_t len, paddr_t *out_phys)
{
    if (!reg || !out_phys)
        return false;

    if (len >= 16) {
        uint32_t addr_hi = fdt32_to_cpu(reg[0]);
        if (addr_hi != 0)
            return false;
        *out_phys = fdt32_to_cpu(reg[1]);
        return true;
    }

    if (len >= 8) {
        *out_phys = fdt32_to_cpu(reg[0]);
        return true;
    }

    return false;
}

bool fdt_decode_gic_interrupt(const uint32_t *intr, uint32_t len,
                              uint32_t fallback_irq, uint32_t *out_irq,
                              bool *out_edge)
{
    uint32_t irq = fallback_irq;
    bool edge = true;

    if (intr && len >= 12) {
        uint32_t type = fdt32_to_cpu(intr[0]);
        uint32_t cell_irq = fdt32_to_cpu(intr[1]);
        uint32_t flags = fdt32_to_cpu(intr[2]);

        /*
         * ARM GIC binding: SPI stores INTID - 32, PPI stores INTID - 16.
         * VirtIO MMIO devices on QEMU virt are SPIs.
         */
        if (type == 0) {
            irq = cell_irq + 32;
            edge = (flags == 1 || flags == 2);
        } else if (type == 1) {
            irq = cell_irq + 16;
            edge = (flags == 1 || flags == 2);
        }
    } else if (intr && len >= 4) {
        irq = fdt32_to_cpu(intr[0]);
        edge = true;
    } else if (fallback_irq == 0) {
        return false;
    }

    if (out_irq)
        *out_irq = irq;
    if (out_edge)
        *out_edge = edge;
    return out_irq != NULL;
}

typedef struct {
    uint32_t virtio_id;
    uint32_t wanted_index;
    uint32_t match_index;
    paddr_t phys;
    uint32_t irq;
    bool edge;
    bool found;
} fdt_virtio_ctx_t;

static bool fdt_virtio_mmio_cb(void *dtb_ptr, void *node_ptr,
                               const char *name, void *opaque)
{
    fdt_virtio_ctx_t *ctx = (fdt_virtio_ctx_t *)opaque;
    uint32_t reg_len = 0;
    uint32_t intr_len = 0;
    paddr_t phys = 0;
    uint32_t fallback_irq = 0;
    uint32_t irq = 0;
    bool edge = true;

    if (!fdt_node_matches(name, "virtio_mmio"))
        return false;

    uint32_t *reg = (uint32_t *)fdt_get_property(dtb_ptr, node_ptr,
                                                 "reg", &reg_len);
    if (!fdt_decode_reg_addr(reg, reg_len, &phys))
        return false;

    volatile uint32_t *base = arch_platform_virtio_mmio_base(phys);
    if (mmio_read32(base, VIRTIO_MMIO_MAGIC) != 0x74726976)
        return false;
    if (mmio_read32(base, VIRTIO_MMIO_DEVICE_ID) != ctx->virtio_id)
        return false;
    if (ctx->match_index++ != ctx->wanted_index)
        return false;

    (void)arch_platform_virtio_irq_from_phys(phys, &fallback_irq);

    uint32_t *intr = (uint32_t *)fdt_get_property(dtb_ptr, node_ptr,
                                                  "interrupts", &intr_len);
    if (!fdt_decode_gic_interrupt(intr, intr_len, fallback_irq, &irq, &edge))
        return false;

    ctx->phys = phys;
    ctx->irq = irq;
    ctx->edge = edge;
    ctx->found = true;
    return true;
}

bool fdt_find_virtio_mmio_device(uint32_t virtio_id, paddr_t *out_phys,
                                 uint32_t *out_irq, bool *out_edge)
{
    return fdt_find_virtio_mmio_device_at(virtio_id, 0u, out_phys, out_irq,
                                          out_edge);
}

bool fdt_find_virtio_mmio_device_at(uint32_t virtio_id, uint32_t index,
                                    paddr_t *out_phys, uint32_t *out_irq,
                                    bool *out_edge)
{
    fdt_virtio_ctx_t ctx = {
        .virtio_id = virtio_id,
        .wanted_index = index,
        .match_index = 0u,
        .phys = 0,
        .irq = 0,
        .edge = true,
        .found = false,
    };

    if (!out_phys || !out_irq)
        return false;

    fdt_for_each_node((void *)dtb_address, fdt_virtio_mmio_cb, &ctx);
    if (!ctx.found)
        return false;

    *out_phys = ctx.phys;
    *out_irq = ctx.irq;
    if (out_edge)
        *out_edge = ctx.edge;
    return true;
}
