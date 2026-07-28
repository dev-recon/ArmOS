/*
 * ArmOS
 * Copyright (c) 2026 Mohamed Ennassiri
 *
 * Licensed under the Apache License, Version 2.0.
 * See LICENSE for details.
 *
 * File: arch/arm64/include/asm/barrier.h
 * Layer: ARM64 / CPU ordering and cache maintenance
 *
 * Responsibilities:
 * - Provide AArch64 barrier primitives to common kernel code.
 * - Perform range-based data-cache maintenance at the CPU boundary.
 *
 * Notes:
 * - Callers own device and DMA policy; these helpers only express the
 *   requested architectural ordering operation.
 */

#ifndef ASM_ARM64_BARRIER_H
#define ASM_ARM64_BARRIER_H

#include <asm/cache.h>
#include <kernel/types.h>

static inline void asm_cpu_relax(void)
{
    __asm__ volatile("yield" ::: "memory");
}

static inline void asm_data_memory_barrier_inner_shareable(void)
{
    __asm__ volatile("dmb ish" ::: "memory");
}

static inline void asm_data_memory_barrier(void)
{
    __asm__ volatile("dmb sy" ::: "memory");
}

static inline void asm_data_sync_barrier(void)
{
    __asm__ volatile("dsb sy" ::: "memory");
}

static inline void asm_data_sync_barrier_inner_shareable_write(void)
{
    __asm__ volatile("dsb ishst" ::: "memory");
}

static inline void asm_instruction_sync_barrier(void)
{
    __asm__ volatile("isb" ::: "memory");
}

static inline void asm_send_event(void)
{
    __asm__ volatile("sev" ::: "memory");
}

static inline void asm_cache_range(const void *address, size_t size,
                                   unsigned int operation)
{
    uintptr_t line;
    uintptr_t end;

    if (size == 0)
        return;
    line = (uintptr_t)address &
        ~(uintptr_t)(ARCH_CACHE_LINE_SIZE - 1u);
    end = (uintptr_t)address + size;
    while (line < end) {
        if (operation == 0)
            __asm__ volatile("dc cvac, %0" :: "r"(line) : "memory");
        else if (operation == 1)
            __asm__ volatile("dc ivac, %0" :: "r"(line) : "memory");
        else
            __asm__ volatile("dc civac, %0" :: "r"(line) : "memory");
        line += ARCH_CACHE_LINE_SIZE;
    }
    __asm__ volatile("dsb sy" ::: "memory");
}

static inline void asm_clean_dcache_by_mva(const void *address, size_t size)
{
    asm_cache_range(address, size, 0);
}

static inline void asm_clean_dcache_2d_by_mva(
    const void *address, size_t stride, size_t row_size, size_t rows)
{
    uintptr_t row = (uintptr_t)address;

    if (row_size == 0u || rows == 0u)
        return;
    for (size_t index = 0u; index < rows; index++, row += stride) {
        uintptr_t line =
            row & ~(uintptr_t)(ARCH_CACHE_LINE_SIZE - 1u);
        uintptr_t end = row + row_size;

        while (line < end) {
            __asm__ volatile("dc cvac, %0" :: "r"(line) : "memory");
            line += ARCH_CACHE_LINE_SIZE;
        }
    }
    __asm__ volatile("dsb sy" ::: "memory");
}

static inline void asm_invalidate_dcache_by_mva(const void *address,
                                                size_t size)
{
    asm_cache_range(address, size, 1);
}

static inline void asm_clean_invalidate_dcache_by_mva(const void *address,
                                                      size_t size)
{
    asm_cache_range(address, size, 2);
}

#endif /* ASM_ARM64_BARRIER_H */
