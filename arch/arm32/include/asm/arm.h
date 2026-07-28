/*
 * ArmOS
 * Copyright (c) 2026 Mohamed Ennassiri
 *
 * Licensed under the Apache License, Version 2.0.
 * See LICENSE for details.
 *
 * File: arch/arm32/include/asm/arm.h
 * Layer: Kernel / ARMv7-A architecture interface
 *
 * Responsibilities:
 * - Define ARM CPU modes, CPSR bits, and architecture helpers.
 * - Provide low-level constants shared by boot, exceptions, and task code.
 *
 * Notes:
 * - These declarations are part of privileged ARM kernel plumbing.
 */

#ifndef _ASM_ARM_H
#define _ASM_ARM_H

#include <asm/cpu_features.h>

#include <kernel/types.h>

/* ARM32 specific definitions */
#define ARM_MODE_USR    0x10
#define ARM_MODE_FIQ    0x11
#define ARM_MODE_IRQ    0x12
#define ARM_MODE_SVC    0x13
#define ARM_MODE_ABT    0x17
#define ARM_MODE_UND    0x1B
#define ARM_MODE_SYS    0x1F

/* CPSR flags */
#define ARM_CPSR_I      (1 << 7)    /* IRQ disable */
#define ARM_CPSR_F      (1 << 6)    /* FIQ disable */
#define ARM_CPSR_T      (1 << 5)    /* Thumb state */
#define ARM_CPSR_MODE   0x1F        /* Mode mask */

/* Cortex-A15 specific feature aliases. */
#define ARM_CORTEX_A15_FEATURES     ARCH_CORTEX_A15_FEATURES
#define ARM_HAS_NEON                ARCH_HAS_NEON
#define ARM_HAS_VFP                 ARCH_HAS_VFP
#define ARM_HAS_GENERIC_TIMER       ARCH_HAS_GENERIC_TIMER
#define ARM_HAS_LARGE_PHYS_ADDR     ARCH_HAS_LARGE_PHYS_ADDR
#define ARM_HAS_VIRTUALIZATION      ARCH_HAS_VIRTUALIZATION

#define ARM_CPU_PART_CORTEX_A7      0xC07u
#define ARM_CPU_PART_CORTEX_A15     0xC0Fu
#define ARM_CPU_PART_CORTEX_A53     0xD03u

/* ACTLR bits for Cortex-A7/A15-style SMP coherency. */
#define ACTLR_SMP   (1 << 6)    /* SMP mode */
#define ACTLR_L1PEN (1 << 2)    /* L1 prefetch enable */
#define ACTLR_L2PEN (1 << 1)    /* L2 prefetch enable */
#define ACTLR_FW    (1 << 0)    /* Cache and TLB maintenance broadcast */

#ifdef __GNUC__
#define INLINE static __inline__
#else
#define INLINE static
#endif

/* Inline assembly helpers - Compatible GNU89 et optimise pour Cortex-A15 */
INLINE void enable_interrupts(void) 
{
    __asm__ volatile("cpsie i" ::: "memory");
}

INLINE void disable_interrupts(void) 
{
    __asm__ volatile("cpsid i" ::: "memory");
}

INLINE uint32_t arm_disable_irq_save(void)
{
    uint32_t cpsr;

    __asm__ volatile(
        "mrs %0, cpsr\n"
        "cpsid i"
        : "=r"(cpsr)
        :
        : "memory");

    return cpsr;
}

INLINE uint32_t arm_disable_irq_fiq_save(void)
{
    uint32_t cpsr;

    __asm__ volatile(
        "mrs %0, cpsr\n"
        "cpsid if"
        : "=r"(cpsr)
        :
        : "memory");

    return cpsr;
}

INLINE void arm_restore_cpsr_control(uint32_t cpsr)
{
    __asm__ volatile("msr cpsr_c, %0" : : "r"(cpsr) : "memory");
}

INLINE void arm_restore_irq_mask(uint32_t saved_flags, uint32_t mask)
{
    uint32_t cpsr_now;
    uint32_t to_write;

    __asm__ volatile("mrs %0, cpsr" : "=r"(cpsr_now));

    to_write = (cpsr_now & ~mask) | (saved_flags & mask);
    __asm__ volatile("msr cpsr_c, %0" : : "r"(to_write) : "memory");
}

INLINE void enable_fiq(void) 
{
    __asm__ volatile("cpsie f" ::: "memory");
}

INLINE void disable_fiq(void) 
{
    __asm__ volatile("cpsid f" ::: "memory");
}

INLINE void enable_async_abort(void)
{
    __asm__ volatile("cpsie a" ::: "memory");
}

INLINE void enable_async_abort_irq_fiq(void)
{
    __asm__ volatile("cpsie aif" ::: "memory");
}

INLINE void wait_for_interrupt(void) 
{
    __asm__ volatile("wfi" ::: "memory");
}

INLINE void wait_for_event(void) 
{
    __asm__ volatile("wfe" ::: "memory");
}

INLINE void send_event(void) 
{
    __asm__ volatile("sev" ::: "memory");
}

INLINE void cpu_relax(void)
{
    __asm__ volatile("yield" ::: "memory");
}

INLINE void compiler_barrier(void)
{
    __asm__ volatile("" ::: "memory");
}

INLINE vaddr_t arm_current_sp(void)
{
    vaddr_t sp;
    __asm__ volatile("mov %0, sp" : "=r"(sp));
    return sp;
}

INLINE vaddr_t arm_current_lr(void)
{
    vaddr_t lr;
    __asm__ volatile("mov %0, lr" : "=r"(lr));
    return lr;
}

INLINE void arm_set_sp(vaddr_t sp)
{
    __asm__ volatile("mov sp, %0" : : "r"(sp) : "memory");
}

INLINE vaddr_t arm_current_pc_plus_16(void)
{
    vaddr_t pc;
    __asm__ volatile("add %0, pc, #16" : "=r"(pc));
    return pc;
}

INLINE void arm_read_user_sp_lr_from_irq(vaddr_t *usr_sp, vaddr_t *usr_lr)
{
    vaddr_t sp;
    vaddr_t lr;

    /*
     * IRQ mode has its own banked SP/LR. Switch briefly to SYS mode to read
     * the user register bank, then return to IRQ mode before the caller uses
     * its active stack again.
     */
    __asm__ volatile(
        "cps    #0x1F\n"
        "mov    %0, sp\n"
        "mov    %1, lr\n"
        "cps    #0x12\n"
        : "=r"(sp), "=r"(lr)
        :
        : "memory", "cc");

    *usr_sp = sp;
    *usr_lr = lr;
}

INLINE uint32_t arm_hvc_call(uint32_t function_id, uint32_t arg0, uint32_t arg1, uint32_t arg2)
{
    register uint32_t r0 __asm__("r0") = function_id;
    register uint32_t r1 __asm__("r1") = arg0;
    register uint32_t r2 __asm__("r2") = arg1;
    register uint32_t r3 __asm__("r3") = arg2;

    __asm__ volatile("hvc #0"
                     : "+r"(r0)
                     : "r"(r1), "r"(r2), "r"(r3)
                     : "memory");

    return r0;
}

INLINE int arm_spin_try_acquire(volatile uint32_t *locked)
{
    uint32_t old;
    uint32_t status;

    /*
     * ARMv7 exclusive monitor sequence:
     * - LDREX observes the current lock word.
     * - If it is zero, STREX tries to store one.
     * - STREX succeeds only if no other CPU modified the word meanwhile.
     *
     * CLREX is used on the already-locked path so this CPU does not keep a
     * stale exclusive reservation while it goes into WFE.
     */
    __asm__ volatile(
        "ldrex  %0, [%2]\n"
        "cmp    %0, #0\n"
        "bne    1f\n"
        "strex  %1, %3, [%2]\n"
        "b      2f\n"
        "1:\n"
        "clrex\n"
        "mov    %1, #1\n"
        "2:\n"
        : "=&r"(old), "=&r"(status)
        : "r"(locked), "r"(1U)
        : "cc", "memory");

    return old == 0 && status == 0;
}

/* Memory barriers optimisees pour Cortex-A15 */
INLINE void data_sync_barrier(void) 
{
    __asm__ volatile("dsb sy" ::: "memory");
}

INLINE void data_sync_barrier_read(void) 
{
    __asm__ volatile("dsb" ::: "memory");
}

INLINE void data_sync_barrier_write(void) 
{
    __asm__ volatile("dsb" ::: "memory");
}

INLINE void data_sync_barrier_inner_shareable(void)
{
    __asm__ volatile("dsb ish" ::: "memory");
}

INLINE void data_sync_barrier_inner_shareable_write(void)
{
    __asm__ volatile("dsb ishst" ::: "memory");
}

INLINE void instruction_sync_barrier(void) 
{
    __asm__ volatile("isb" ::: "memory");
}

INLINE void set_vbar(uint32_t vbar)
{
    __asm__ volatile("mcr p15, 0, %0, c12, c0, 0" :: "r"(vbar));
    data_sync_barrier();
    instruction_sync_barrier();
}

INLINE void data_memory_barrier(void) 
{
    __asm__ volatile("dmb sy" ::: "memory");
}

INLINE void data_memory_barrier_read(void) 
{
    __asm__ volatile("dmb" ::: "memory");
}

INLINE void data_memory_barrier_write(void) 
{
    __asm__ volatile("dmb" ::: "memory");
}

INLINE void data_memory_barrier_inner_shareable(void)
{
    __asm__ volatile("dmb ish" ::: "memory");
}

/* CPU mode switching helpers */
INLINE uint32_t get_cpsr(void)
{
    uint32_t cpsr;
    __asm__ volatile("mrs %0, cpsr" : "=r"(cpsr));
    return cpsr;
}

INLINE void set_cpsr(uint32_t cpsr)
{
    __asm__ volatile("msr cpsr_cxsf, %0" : : "r"(cpsr));
}

INLINE uint32_t get_spsr(void)
{
    uint32_t spsr;
    __asm__ volatile("mrs %0, spsr" : "=r"(spsr));
    return spsr;
}

INLINE vaddr_t arm_read_banked_lr_svc(void)
{
    vaddr_t lr_svc;
    __asm__ volatile("mrs %0, lr_svc" : "=r"(lr_svc));
    return lr_svc;
}

INLINE vaddr_t arm_read_banked_sp_svc(void)
{
    vaddr_t sp_svc;
    __asm__ volatile("mrs %0, sp_svc" : "=r"(sp_svc));
    return sp_svc;
}

INLINE void set_spsr(uint32_t spsr)
{
    __asm__ volatile("msr spsr_cxsf, %0" : : "r"(spsr));
}

/* CPU identification registers. Keep raw reads here; higher layers decide
 * whether they need the full register value or a decoded logical CPU id. */
INLINE uint32_t arm_read_mpidr(void)
{
    uint32_t mpidr;
    __asm__ volatile("mrc p15, 0, %0, c0, c0, 5" : "=r"(mpidr));
    return mpidr;
}

INLINE uint32_t arm_read_midr(void)
{
    uint32_t midr;
    __asm__ volatile("mrc p15, 0, %0, c0, c0, 0" : "=r"(midr));
    return midr;
}

INLINE uint32_t arm_midr_part(uint32_t midr)
{
    return (midr >> 4) & 0xfffu;
}

INLINE uint32_t arm_read_ctr(void)
{
    uint32_t ctr;
    __asm__ volatile("mrc p15, 0, %0, c0, c0, 1" : "=r"(ctr));
    return ctr;
}

INLINE uint32_t arm_read_tlbtr(void)
{
    uint32_t tlbtr;
    __asm__ volatile("mrc p15, 0, %0, c0, c0, 3" : "=r"(tlbtr));
    return tlbtr;
}

INLINE uint32_t arm_read_dfr0(void)
{
    uint32_t dfr0;
    __asm__ volatile("mrc p15, 0, %0, c0, c1, 2" : "=r"(dfr0));
    return dfr0;
}

INLINE uint32_t arm_read_mmfr0(void)
{
    uint32_t mmfr0;
    __asm__ volatile("mrc p15, 0, %0, c0, c1, 4" : "=r"(mmfr0));
    return mmfr0;
}

INLINE uint32_t get_cpu_id(void)
{
    return arm_read_mpidr() & 0x3;
}

INLINE uint32_t get_main_id(void)
{
    return arm_read_midr();
}

/* Cache operations pour ARMv7-A */
INLINE void flush_icache(void)
{
    __asm__ volatile("mcr p15, 0, %0, c7, c5, 0" : : "r"(0));
    instruction_sync_barrier();
}

INLINE void flush_icache_inner_shareable(void)
{
    __asm__ volatile("mcr p15, 0, %0, c7, c1, 0" : : "r"(0) : "memory");
    data_sync_barrier_inner_shareable();
    instruction_sync_barrier();
}

/* Branch predictor operations */
INLINE void flush_branch_predictor(void)
{
    __asm__ volatile("mcr p15, 0, %0, c7, c5, 6" : : "r"(0));
    instruction_sync_barrier();
}

INLINE uint32_t arm_read_clidr(void)
{
    uint32_t clidr;
    __asm__ volatile("mrc p15, 1, %0, c0, c0, 1" : "=r"(clidr));
    return clidr;
}

INLINE void arm_write_csselr(uint32_t csselr)
{
    __asm__ volatile("mcr p15, 2, %0, c0, c0, 0" : : "r"(csselr) : "memory");
    instruction_sync_barrier();
}

INLINE uint32_t arm_read_ccsidr(void)
{
    uint32_t ccsidr;
    __asm__ volatile("mrc p15, 1, %0, c0, c0, 0" : "=r"(ccsidr));
    return ccsidr;
}

INLINE void arm_dcache_clean_invalidate_setway(uint32_t setway)
{
    __asm__ volatile("mcr p15, 0, %0, c7, c14, 2" : : "r"(setway) : "memory");
}

INLINE uint32_t arm_clz(uint32_t value)
{
    uint32_t count;
    __asm__ volatile("clz %0, %1" : "=r"(count) : "r"(value));
    return count;
}

INLINE void flush_dcache(void)
{
    uint32_t clidr = arm_read_clidr();
    uint32_t loc = (clidr >> 24) & 0x7u;

    for (uint32_t level = 0; level < loc; ++level) {
        uint32_t ctype = (clidr >> (level * 3u)) & 0x7u;

        if (ctype < 2u)
            continue;

        arm_write_csselr(level << 1);
        uint32_t ccsidr = arm_read_ccsidr();
        uint32_t line_shift = (ccsidr & 0x7u) + 4u;
        uint32_t ways = ((ccsidr >> 3) & 0x3ffu) + 1u;
        uint32_t sets = ((ccsidr >> 13) & 0x7fffu) + 1u;
        uint32_t way_shift = arm_clz(ways - 1u);

        for (uint32_t way = ways; way > 0; --way) {
            uint32_t way_index = way - 1u;

            for (uint32_t set = sets; set > 0; --set) {
                uint32_t set_index = set - 1u;
                uint32_t setway = (level << 1) |
                                  (set_index << line_shift) |
                                  (way_index << way_shift);

                arm_dcache_clean_invalidate_setway(setway);
            }
        }
    }

    data_sync_barrier();
    instruction_sync_barrier();
}

INLINE void flush_cache_all(void)
{
    flush_dcache();
    flush_icache();
    flush_branch_predictor();
}

INLINE void clean_dcache_range(vaddr_t start, vaddr_t end)
{
    vaddr_t addr;
    for (addr = start & ~63; addr < end; addr += 64) {
        __asm__ volatile("mcr p15, 0, %0, c7, c10, 1" : : "r"(addr));
    }
    data_sync_barrier();
}

INLINE void invalidate_dcache_range(vaddr_t start, vaddr_t end)
{
    vaddr_t addr;
    for (addr = start & ~63; addr < end; addr += 64) {
        __asm__ volatile("mcr p15, 0, %0, c7, c6, 1" : : "r"(addr));
    }
    data_sync_barrier();
}

/* TLB operations */
INLINE void flush_tlb(void)
{
    __asm__ volatile("mcr p15, 0, %0, c8, c7, 0" : : "r"(0));
    data_sync_barrier();
    instruction_sync_barrier();
}

INLINE void flush_tlb_page(vaddr_t addr)
{
    __asm__ volatile("mcr p15, 0, %0, c8, c7, 1" : : "r"(addr));
    data_sync_barrier();
    instruction_sync_barrier();
}

INLINE void tlb_flush_all_debug(void)
{
    __asm__ volatile("dsb ish; mcr p15,0,%0,c8,c3,0; dsb ish; isb" :: "r"(0) : "memory");
}

INLINE void dc_clean_mva(void *va)
{
    __asm__ volatile("mcr p15,0,%0,c7,c10,1" :: "r"(va) : "memory");
}

/* MMU control */
INLINE uint32_t get_sctlr(void)
{
    uint32_t sctlr;
    __asm__ volatile("mrc p15, 0, %0, c1, c0, 0" : "=r"(sctlr));
    return sctlr;
}

INLINE void set_sctlr(uint32_t sctlr)
{
    __asm__ volatile("mcr p15, 0, %0, c1, c0, 0" : : "r"(sctlr));
    instruction_sync_barrier();
}

INLINE int sctlr_smp_enabled(void)
{
    uint32_t sctlr = get_sctlr();
    return (sctlr >> 6) & 1;
}

INLINE void sctlr_set_smp(void)
{
    uint32_t sctlr = get_sctlr();
    sctlr |= (1u << 6);
    __asm__ volatile("mcr p15,0,%0,c1,c0,0" :: "r"(sctlr) : "memory");
    instruction_sync_barrier();
    data_sync_barrier();
}

/*
 * NOTE: get_ttbr0, set_ttbr0, invalidate_tlb_all, invalidate_tlb_page
 * are public memory-management APIs. The low-level CP15 accessors stay here
 * so generic MMU code can call named architecture primitives.
 */

INLINE uint32_t arm_read_ttbr0(void)
{
    uint32_t ttbr0;
    __asm__ volatile("mrc p15, 0, %0, c2, c0, 0" : "=r"(ttbr0));
    return ttbr0;
}

INLINE void arm_write_ttbr0(uint32_t ttbr0)
{
    __asm__ volatile("mcr p15, 0, %0, c2, c0, 0" : : "r"(ttbr0));
    instruction_sync_barrier();
}

INLINE uint32_t get_lr(void)
{
    uint32_t return_address;
    __asm__ volatile("mov %0, lr" : "=r"(return_address));
    return return_address;
}

INLINE uint32_t get_ttbr1(void)
{
    uint32_t ttbr1;
    __asm__ volatile("mrc p15, 0, %0, c2, c0, 1" : "=r"(ttbr1));
    return ttbr1;
}

INLINE void set_ttbr1(uint32_t ttbr1)
{
    __asm__ volatile("mcr p15, 0, %0, c2, c0, 1" : : "r"(ttbr1));
    instruction_sync_barrier();
}

INLINE uint32_t get_ttbcr(void)
{
    uint32_t ttbcr;
    __asm__ volatile("mrc p15, 0, %0, c2, c0, 2" : "=r"(ttbcr));
    return ttbcr;
}

INLINE void set_ttbcr(uint32_t ttbcr)
{
    __asm__ volatile("mcr p15, 0, %0, c2, c0, 2" : : "r"(ttbcr));
    instruction_sync_barrier();
}

INLINE uint32_t get_dacr(void)
{
    uint32_t dacr;
    __asm__ volatile("mrc p15, 0, %0, c3, c0, 0" : "=r"(dacr));
    return dacr;
}

INLINE void set_dacr(uint32_t dacr)
{
    __asm__ volatile("mcr p15, 0, %0, c3, c0, 0" : : "r"(dacr));
    instruction_sync_barrier();
}

/* Fault registers */
INLINE uint32_t get_dfar(void)
{
    uint32_t dfar;
    __asm__ volatile("mrc p15, 0, %0, c6, c0, 0" : "=r"(dfar));
    return dfar;
}

INLINE uint32_t get_ifar(void)
{
    uint32_t ifar;
    __asm__ volatile("mrc p15, 0, %0, c6, c0, 2" : "=r"(ifar));
    return ifar;
}

INLINE uint32_t arm_translate_privileged_read_par(vaddr_t va)
{
    uint32_t par;

    __asm__ volatile(
        "mcr p15,0,%1,c7,c8,0\n"
        "isb\n"
        "mrc p15,0,%0,c7,c4,0\n"
        : "=r"(par)
        : "r"(va)
        : "memory");

    return par;
}

INLINE uint32_t get_dfsr(void)
{
    uint32_t dfsr;
    __asm__ volatile("mrc p15, 0, %0, c5, c0, 0" : "=r"(dfsr));
    return dfsr;
}

INLINE uint32_t get_ifsr(void)
{
    uint32_t ifsr;
    __asm__ volatile("mrc p15, 0, %0, c5, c0, 1" : "=r"(ifsr));
    return ifsr;
}

/* ARM Generic Timer support pour Cortex-A15 */
INLINE uint32_t get_cntfrq(void)
{
    uint32_t freq;
    __asm__ volatile("mrc p15, 0, %0, c14, c0, 0" : "=r"(freq));
    return freq;
}

INLINE void set_cntfrq(uint32_t freq)
{
    __asm__ volatile("mcr p15, 0, %0, c14, c0, 0" : : "r"(freq));
}

INLINE uint64_t get_cntpct(void)
{
    uint32_t low, high;
    __asm__ volatile("mrrc p15, 0, %0, %1, c14" : "=r"(low), "=r"(high));
    return ((uint64_t)high << 32) | low;
}

INLINE uint64_t get_cntvct(void)
{
    uint64_t val;
    instruction_sync_barrier();
    __asm__ volatile("mrrc p15, 1, %Q0, %R0, c14" : "=r"(val));
    return val;
}

INLINE uint32_t get_cntp_ctl(void)
{
    uint32_t ctl;
    __asm__ volatile("mrc p15, 0, %0, c14, c2, 1" : "=r"(ctl));
    return ctl;
}

INLINE void set_cntp_ctl(uint32_t ctl)
{
    __asm__ volatile("mcr p15, 0, %0, c14, c2, 1" : : "r"(ctl));
}

INLINE uint32_t get_cntp_tval(void)
{
    uint32_t tval;
    __asm__ volatile("mrc p15, 0, %0, c14, c2, 0" : "=r"(tval));
    return tval;
}

INLINE void set_cntp_tval(uint32_t tval)
{
    __asm__ volatile("mcr p15, 0, %0, c14, c2, 0" : : "r"(tval));
}

INLINE uint64_t get_cntp_cval(void)
{
    uint32_t low, high;
    __asm__ volatile("mrrc p15, 2, %0, %1, c14" : "=r"(low), "=r"(high));
    return ((uint64_t)high << 32) | low;
}

INLINE void set_cntp_cval(uint64_t cval)
{
    uint32_t low = (uint32_t)cval;
    uint32_t high = (uint32_t)(cval >> 32);
    __asm__ volatile("mcrr p15, 2, %0, %1, c14" : : "r"(low), "r"(high));
}

INLINE void set_cntv_cval(uint64_t cval)
{
    __asm__ volatile("mcrr p15, 3, %Q0, %R0, c14" :: "r"(cval));
    data_sync_barrier();
    instruction_sync_barrier();
}

/* Performance monitoring pour Cortex-A15 */
INLINE uint32_t get_pmcr(void)
{
    uint32_t pmcr;
    __asm__ volatile("mrc p15, 0, %0, c9, c12, 0" : "=r"(pmcr));
    return pmcr;
}

INLINE void set_pmcr(uint32_t pmcr)
{
    __asm__ volatile("mcr p15, 0, %0, c9, c12, 0" : : "r"(pmcr));
}

INLINE uint32_t get_pmccntr(void)
{
    uint32_t pmccntr;
    __asm__ volatile("mrc p15, 0, %0, c9, c13, 0" : "=r"(pmccntr));
    return pmccntr;
}

INLINE void set_pmccntr(uint32_t pmccntr)
{
    __asm__ volatile("mcr p15, 0, %0, c9, c13, 0" : : "r"(pmccntr));
}

/* VFP/NEON support pour Cortex-A15 */
INLINE uint32_t get_cpacr(void)
{
    uint32_t cpacr;
    __asm__ volatile("mrc p15, 0, %0, c1, c0, 2" : "=r"(cpacr));
    return cpacr;
}

INLINE void set_cpacr(uint32_t cpacr)
{
    __asm__ volatile("mcr p15, 0, %0, c1, c0, 2" : : "r"(cpacr));
    instruction_sync_barrier();
}

INLINE void enable_vfp(void)
{
    uint32_t cpacr = get_cpacr();
    cpacr |= (0xF << 20);  /* Enable CP10 and CP11 */
    set_cpacr(cpacr);
}

INLINE void disable_vfp(void)
{
    uint32_t cpacr = get_cpacr();
    cpacr &= ~(0xF << 20);  /* Disable CP10 and CP11 */
    set_cpacr(cpacr);
}

/* Auxiliary Control Register pour Cortex-A15 */
INLINE uint32_t get_actlr(void)
{
    uint32_t actlr;
    __asm__ volatile("mrc p15, 0, %0, c1, c0, 1" : "=r"(actlr));
    return actlr;
}

INLINE void set_actlr(uint32_t actlr)
{
    __asm__ volatile("mcr p15, 0, %0, c1, c0, 1" : : "r"(actlr));
    instruction_sync_barrier();
}

INLINE void arm_enable_smp_coherency(void)
{
    set_actlr(get_actlr() | ACTLR_SMP | ACTLR_FW);
    data_sync_barrier();
    instruction_sync_barrier();
}

/* Cache size identification pour Cortex-A15 */
INLINE uint32_t get_ccsidr(void)
{
    uint32_t ccsidr;
    __asm__ volatile("mrc p15, 1, %0, c0, c0, 0" : "=r"(ccsidr));
    return ccsidr;
}

INLINE uint32_t get_csselr(void)
{
    uint32_t csselr;
    __asm__ volatile("mrc p15, 2, %0, c0, c0, 0" : "=r"(csselr));
    return csselr;
}

INLINE void set_csselr(uint32_t csselr)
{
    __asm__ volatile("mcr p15, 2, %0, c0, c0, 0" : : "r"(csselr));
    instruction_sync_barrier();
}

INLINE uint32_t dcache_line_size_bytes(void)
{
    uint32_t ccsidr;
    uint32_t line_sz_enc;
    uint32_t line_bytes;

    set_csselr(0);
    ccsidr = get_ccsidr();

    line_sz_enc = ccsidr & 0x7;
    line_bytes = 1u << (line_sz_enc + 4u);
    if (line_bytes == 0 || line_bytes > 256)
        line_bytes = 64;
    return line_bytes;
}

INLINE void dcache_clean_by_va(void *va, size_t len)
{
    uintptr_t p = (uintptr_t)va & ~63u;
    uintptr_t end = ((uintptr_t)va + len + 63u) & ~63u;

    for (; p < end; p += 64)
        __asm__ volatile("mcr p15, 0, %0, c7, c10, 1" :: "r"(p) : "memory");
    data_sync_barrier_inner_shareable();
}

INLINE void sync_icache_for_exec(void)
{
    /* A recycled executable page may next run on any CPU in the cluster. */
    flush_icache_inner_shareable();
}

INLINE int ats1cpr_probe(uint32_t va, uint32_t *pa_out, uint32_t *par_out)
{
    uint32_t par;

    __asm__ volatile("mcr p15,0,%0,c7,c8,0" :: "r"(va));
    __asm__ volatile("mrc p15,0,%0,c7,c4,0" : "=r"(par));
    if (par_out)
        *par_out = par;
    if (par & 1)
        return 0;
    if (pa_out)
        *pa_out = (par & 0xFFFFF000u) | (va & 0xFFFu);
    return 1;
}

INLINE void clean_dcache_by_mva(const void *addr, size_t size)
{
    if (size == 0)
        return;

    uint32_t line = dcache_line_size_bytes();
    uintptr_t start = ((uintptr_t)addr) & ~(uintptr_t)(line - 1u);
    uintptr_t end = ((uintptr_t)addr + size + line - 1u) & ~(uintptr_t)(line - 1u);

    data_sync_barrier_inner_shareable();
    for (uintptr_t p = start; p < end; p += line)
        __asm__ volatile("mcr p15, 0, %0, c7, c10, 1" :: "r"(p) : "memory");
    data_sync_barrier_inner_shareable();
}

INLINE void clean_dcache_2d_by_mva(const void *addr, size_t stride,
                                  size_t row_size, size_t rows)
{
    if (row_size == 0u || rows == 0u)
        return;

    uint32_t line = dcache_line_size_bytes();
    uintptr_t row = (uintptr_t)addr;

    data_sync_barrier_inner_shareable();
    for (size_t index = 0u; index < rows; index++, row += stride) {
        uintptr_t start = row & ~(uintptr_t)(line - 1u);
        uintptr_t end =
            (row + row_size + line - 1u) & ~(uintptr_t)(line - 1u);

        for (uintptr_t p = start; p < end; p += line)
            __asm__ volatile("mcr p15, 0, %0, c7, c10, 1"
                             :: "r"(p) : "memory");
    }
    data_sync_barrier_inner_shareable();
}

INLINE void invalidate_dcache_by_mva(const void *addr, size_t size)
{
    if (size == 0)
        return;

    uint32_t line = dcache_line_size_bytes();
    uintptr_t start = ((uintptr_t)addr) & ~(uintptr_t)(line - 1u);
    uintptr_t end = ((uintptr_t)addr + size + line - 1u) & ~(uintptr_t)(line - 1u);

    data_sync_barrier_inner_shareable();
    for (uintptr_t p = start; p < end; p += line)
        __asm__ volatile("mcr p15, 0, %0, c7, c6, 1" :: "r"(p) : "memory");
    data_sync_barrier_inner_shareable();
}

INLINE void clean_invalidate_dcache_by_mva(const void *addr, size_t size)
{
    if (size == 0)
        return;

    uint32_t line = dcache_line_size_bytes();
    uintptr_t start = ((uintptr_t)addr) & ~(uintptr_t)(line - 1u);
    uintptr_t end = ((uintptr_t)addr + size + line - 1u) & ~(uintptr_t)(line - 1u);

    data_sync_barrier_inner_shareable();
    for (uintptr_t p = start; p < end; p += line)
        __asm__ volatile("mcr p15, 0, %0, c7, c14, 1" :: "r"(p) : "memory");
    data_sync_barrier_inner_shareable();
}

/* Context ID register */
INLINE uint32_t get_contextidr(void)
{
    uint32_t contextidr;
    __asm__ volatile("mrc p15, 0, %0, c13, c0, 1" : "=r"(contextidr));
    return contextidr;
}

INLINE void set_contextidr(uint32_t contextidr)
{
    __asm__ volatile(
        "mcr p15, 0, %0, c13, c0, 1 \n"
        "nop                        \n"
        "nop                        \n"
        "nop                        \n"
        "nop                        \n"
        "isb                        \n"
        :
        : "r"(contextidr)
        : "memory");
}

/* Thread ID registers */
INLINE uint32_t get_tpidrurw(void)
{
    uint32_t tpidrurw;
    __asm__ volatile("mrc p15, 0, %0, c13, c0, 2" : "=r"(tpidrurw));
    return tpidrurw;
}

INLINE void set_tpidrurw(uint32_t tpidrurw)
{
    __asm__ volatile("mcr p15, 0, %0, c13, c0, 2" : : "r"(tpidrurw));
}

INLINE uint32_t get_tpidruro(void)
{
    uint32_t tpidruro;
    __asm__ volatile("mrc p15, 0, %0, c13, c0, 3" : "=r"(tpidruro));
    return tpidruro;
}

INLINE void set_tpidruro(uint32_t tpidruro)
{
    __asm__ volatile("mcr p15, 0, %0, c13, c0, 3" : : "r"(tpidruro));
}

INLINE uint32_t get_tpidrprw(void)
{
    uint32_t tpidrprw;
    __asm__ volatile("mrc p15, 0, %0, c13, c0, 4" : "=r"(tpidrprw));
    return tpidrprw;
}

INLINE void set_tpidrprw(uint32_t tpidrprw)
{
    __asm__ volatile("mcr p15, 0, %0, c13, c0, 4" : : "r"(tpidrprw));
}

/* Helper functions pour les operations courantes */
INLINE void enable_mmu(void)
{
    uint32_t sctlr = get_sctlr();
    sctlr |= (1 << 0);  /* M bit */
    set_sctlr(sctlr);
}

INLINE void disable_mmu(void)
{
    uint32_t sctlr = get_sctlr();
    sctlr &= ~(1 << 0);  /* M bit */
    set_sctlr(sctlr);
}

INLINE void enable_dcache(void)
{
    uint32_t sctlr = get_sctlr();
    sctlr |= (1 << 2);  /* C bit */
    set_sctlr(sctlr);
}

INLINE void disable_dcache(void)
{
    uint32_t sctlr = get_sctlr();
    sctlr &= ~(1 << 2);  /* C bit */
    set_sctlr(sctlr);
}

INLINE void enable_icache(void)
{
    uint32_t sctlr = get_sctlr();
    sctlr |= (1 << 12);  /* I bit */
    set_sctlr(sctlr);
}

INLINE void disable_icache(void)
{
    uint32_t sctlr = get_sctlr();
    sctlr &= ~(1 << 12);  /* I bit */
    set_sctlr(sctlr);
}

INLINE void enable_branch_prediction(void)
{
    uint32_t sctlr = get_sctlr();
    sctlr |= (1 << 11);  /* Z bit */
    set_sctlr(sctlr);
}

INLINE void disable_branch_prediction(void)
{
    uint32_t sctlr = get_sctlr();
    sctlr &= ~(1 << 11);  /* Z bit */
    set_sctlr(sctlr);
}

INLINE uint32_t get_cpu_mode(void)
{
    uint32_t cpu_mode = get_cpsr();
    return cpu_mode & 0x1F;  
}

INLINE uint32_t read_sp_usr(void)
{
    uint32_t sp;
    __asm__ volatile(
        "cps #0x1F\n"
        "mov %0, sp\n"
        "cps #0x13\n"
        : "=r"(sp) :: "memory", "cc");
    return sp;
}

INLINE void write_sp_usr(uint32_t sp)
{
    __asm__ volatile(
        "cps #0x1F\n"
        "mov sp, %0\n"
        "cps #0x13\n"
        :: "r"(sp) : "memory", "cc");
}

INLINE uint32_t read_spsr(void)
{
    return get_spsr();
}

INLINE uint32_t read_cpsr(void)
{
    return get_cpsr();
}

INLINE uint32_t read_spsr_svc(void)
{
    return get_spsr();
}

INLINE uint32_t read_lr_svc(void)
{
    return get_lr();
}

INLINE uint32_t read_lr_usr(void)
{
    uint32_t lr;
    __asm__ volatile(
        "cps #0x1F\n"
        "mov %0, lr\n"
        "cps #0x13\n"
        : "=r"(lr) :: "memory", "cc");
    return lr;
}


/* Macros pour les constantes courantes */
#define SCTLR_M     (1 << 0)    /* MMU enable */
#define SCTLR_A     (1 << 1)    /* Alignment check enable */
#define SCTLR_C     (1 << 2)    /* Data cache enable */
#define SCTLR_W     (1 << 3)    /* Write buffer enable */
#define SCTLR_P     (1 << 4)    /* 32-bit exception handler */
#define SCTLR_D     (1 << 5)    /* 26-bit address exception checking */
#define SCTLR_L     (1 << 6)    /* Late abort on earlier CPUs */
#define SCTLR_B     (1 << 7)    /* Big-endian */
#define SCTLR_S     (1 << 8)    /* System MMU protection */
#define SCTLR_R     (1 << 9)    /* ROM MMU protection */
#define SCTLR_F     (1 << 10)   /* Implementation defined */
#define SCTLR_Z     (1 << 11)   /* Branch prediction enable */
#define SCTLR_I     (1 << 12)   /* Instruction cache enable */
#define SCTLR_V     (1 << 13)   /* Vectors bit */
#define SCTLR_RR    (1 << 14)   /* Round robin replacement */
#define SCTLR_L4    (1 << 15)   /* LDR pc can set T bit */
#define SCTLR_DT    (1 << 16)   /* Global data TCM enable */
#define SCTLR_IT    (1 << 18)   /* Global instruction TCM enable */
#define SCTLR_ST    (1 << 19)   /* Global system TCM enable */
#define SCTLR_FI    (1 << 21)   /* Fast interrupts configuration enable */
#define SCTLR_U     (1 << 22)   /* Unaligned access operation */
#define SCTLR_XP    (1 << 23)   /* Extended page tables */
#define SCTLR_VE    (1 << 24)   /* Vectored interrupts */
#define SCTLR_EE    (1 << 25)   /* Exception endianness */
#define SCTLR_L2    (1 << 26)   /* L2 unified cache enable */
#define SCTLR_NMFI  (1 << 27)   /* Non-maskable FIQ (NMFI) support */
#define SCTLR_TRE   (1 << 28)   /* TEX remap enable */
#define SCTLR_AFE   (1 << 29)   /* Access flag enable */
#define SCTLR_TE    (1 << 30)   /* Thumb exception enable */

/* CPACR bits */
#define CPACR_CP10_MASK (3 << 20)
#define CPACR_CP11_MASK (3 << 22)
#define CPACR_CP10_FULL (3 << 20)
#define CPACR_CP11_FULL (3 << 22)

/* Timer control bits */
#define CNTP_CTL_ENABLE  (1 << 0)
#define CNTP_CTL_IMASK   (1 << 1)
#define CNTP_CTL_ISTATUS (1 << 2)

#endif /* _ASM_ARM_H */
