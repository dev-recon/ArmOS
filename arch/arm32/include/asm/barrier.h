/*
 * ArmOS
 * Copyright (c) 2026 Mohamed Ennassiri
 *
 * Licensed under the Apache License, Version 2.0.
 * See LICENSE for details.
 *
 * File: arch/arm32/include/asm/barrier.h
 * Layer: ARM32 / memory ordering and cache maintenance
 *
 * Responsibilities:
 * - Provide ARM32 barrier and cache-maintenance primitives to the generic
 *   architecture boundary.
 * - Keep ARM helper names local to the ARM32 include tree.
 */

#ifndef _ASM_ARM32_BARRIER_H
#define _ASM_ARM32_BARRIER_H

#include <kernel/types.h>
#include <asm/arm.h>

static inline void asm_cpu_relax(void)
{
    cpu_relax();
}

static inline void asm_data_memory_barrier_inner_shareable(void)
{
    data_memory_barrier_inner_shareable();
}

static inline void asm_data_memory_barrier(void)
{
    data_memory_barrier();
}

static inline void asm_data_sync_barrier(void)
{
    data_sync_barrier();
}

static inline void asm_data_sync_barrier_inner_shareable_write(void)
{
    data_sync_barrier_inner_shareable_write();
}

static inline void asm_instruction_sync_barrier(void)
{
    instruction_sync_barrier();
}

static inline void asm_send_event(void)
{
    send_event();
}

static inline void asm_clean_dcache_by_mva(const void *addr, size_t size)
{
    clean_dcache_by_mva(addr, size);
}

static inline void asm_clean_dcache_2d_by_mva(
    const void *addr, size_t stride, size_t row_size, size_t rows)
{
    clean_dcache_2d_by_mva(addr, stride, row_size, rows);
}

static inline void asm_invalidate_dcache_by_mva(const void *addr, size_t size)
{
    invalidate_dcache_by_mva(addr, size);
}

static inline void asm_clean_invalidate_dcache_by_mva(const void *addr, size_t size)
{
    clean_invalidate_dcache_by_mva(addr, size);
}

#endif /* _ASM_ARM32_BARRIER_H */
