/*
 * ArmOS
 * Copyright (c) 2026 Mohamed Ennassiri
 *
 * Licensed under the Apache License, Version 2.0.
 * See LICENSE for details.
 *
 * File: include/kernel/arch_barrier.h
 * Layer: Kernel / architecture boundary
 *
 * Responsibilities:
 * - Expose architecture ordering and cache-maintenance primitives needed by
 *   generic drivers.
 * - Keep driver code from including ARM-specific helper headers directly.
 *
 * Notes:
 * - Every architecture provides equivalent ordering and cache semantics at
 *   this boundary.
 */

#ifndef _KERNEL_ARCH_BARRIER_H
#define _KERNEL_ARCH_BARRIER_H

#include <kernel/types.h>
#include <asm/barrier.h>

static inline void arch_cpu_relax(void)
{
    asm_cpu_relax();
}

static inline void arch_data_memory_barrier_inner_shareable(void)
{
    asm_data_memory_barrier_inner_shareable();
}

static inline void arch_data_memory_barrier(void)
{
    asm_data_memory_barrier();
}

static inline void arch_data_sync_barrier(void)
{
    asm_data_sync_barrier();
}

static inline void arch_data_sync_barrier_inner_shareable_write(void)
{
    asm_data_sync_barrier_inner_shareable_write();
}

static inline void arch_instruction_sync_barrier(void)
{
    asm_instruction_sync_barrier();
}

static inline void arch_send_event(void)
{
    asm_send_event();
}

static inline void arch_clean_dcache_by_mva(const void *addr, size_t size)
{
    asm_clean_dcache_by_mva(addr, size);
}

static inline void arch_clean_dcache_2d_by_mva(
    const void *addr, size_t stride, size_t row_size, size_t rows)
{
    asm_clean_dcache_2d_by_mva(addr, stride, row_size, rows);
}

static inline void arch_invalidate_dcache_by_mva(const void *addr, size_t size)
{
    asm_invalidate_dcache_by_mva(addr, size);
}

static inline void arch_clean_invalidate_dcache_by_mva(const void *addr, size_t size)
{
    asm_clean_invalidate_dcache_by_mva(addr, size);
}

#endif /* _KERNEL_ARCH_BARRIER_H */
