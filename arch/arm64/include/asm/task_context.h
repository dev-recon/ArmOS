/*
 * ArmOS
 * Copyright (c) 2026 Mohamed Ennassiri
 *
 * Licensed under the Apache License, Version 2.0.
 * See LICENSE for details.
 *
 * File: arch/arm64/include/asm/task_context.h
 * Layer: ARM64 / task context ABI
 *
 * Responsibilities:
 * - Group kernel callee-saved state, EL0 state and address-space identity.
 * - Preserve the complete FP/SIMD register file across task switches.
 * - Define the architectural contract consumed by the task-switch boundary.
 *
 * Notes:
 * - task_context_t is the generic-kernel alias for this concrete layout.
 * - Address-space residency is owned by the ARM64 VM backend per CPU.
 */

#ifndef ASM_ARM64_TASK_CONTEXT_H
#define ASM_ARM64_TASK_CONTEXT_H

#include <asm/user_context.h>
#include <kernel/memory.h>
#include <kernel/types.h>

#define ARM64_TASK_FLAG_RETURNS_TO_USER (1u << 0)
#define ARM64_TASK_FLAG_FIRST_RUN       (1u << 1)

typedef struct arm64_kernel_context {
    uint64_t x[12];
    uint64_t sp;
    uint64_t pc;
} arm64_kernel_context_t;

typedef struct arm64_simd_context {
    uint64_t q[64];
    uint64_t fpcr;
    uint64_t fpsr;
} __attribute__((aligned(16))) arm64_simd_context_t;

typedef struct arm64_task_context {
    arm64_kernel_context_t kernel;
    arm64_user_context_t user;
    arm64_simd_context_t simd;
    uint64_t tls_base;
    paddr_t ttbr0;
    uint32_t asid;
    uint32_t flags;
} __attribute__((aligned(16))) arm64_task_context_t;

typedef arm64_task_context_t task_context_t;
typedef arm64_user_context_t arch_task_user_context_t;

typedef struct arch_user_signal_frame {
    arm64_user_context_t user;
    uint32_t old_blocked;
    uint32_t sig;
} __attribute__((aligned(16))) arch_user_signal_frame_t;

#define ARCH_TASK_KERNEL_PSTATE        0x3c5u
#define ARCH_TASK_STACK_ALIGNMENT      16u
#define ARCH_TASK_KERNEL_STACK_RESERVE 512u

_Static_assert(sizeof(arm64_kernel_context_t) == 112,
               "AArch64 kernel context ABI size changed");
_Static_assert(sizeof(arm64_simd_context_t) == 528,
               "AArch64 SIMD context ABI size changed");
_Static_assert(sizeof(arm64_task_context_t) == 944,
               "AArch64 task context ABI size changed");

void arm64_task_context_switch(arm64_task_context_t *previous,
                               const arm64_task_context_t *next);
void arm64_simd_context_capture(arm64_simd_context_t *context);
int arm64_task_context_switch_address_space(
    arm64_task_context_t *previous,
    arm64_task_context_t *next);
void arm64_task_kernel_entry_trampoline(void) __attribute__((noreturn));
void arm64_task_user_entry_trampoline(void) __attribute__((noreturn));

static inline vaddr_t arch_task_stack_align(vaddr_t sp)
{
    return sp & ~(vaddr_t)(ARCH_TASK_STACK_ALIGNMENT - 1u);
}

static inline uintptr_t arch_task_current_pointer(void)
{
    uint64_t pointer;

    __asm__ volatile("mrs %0, tpidr_el1" : "=r"(pointer));
    return (uintptr_t)pointer;
}

static inline void arch_task_set_current_pointer(uintptr_t pointer)
{
    __asm__ volatile("msr tpidr_el1, %0" : : "r"((uint64_t)pointer)
                     : "memory");
}

static inline vaddr_t arch_task_context_kernel_sp(const task_context_t *ctx)
{
    return (vaddr_t)ctx->kernel.sp;
}

static inline vaddr_t arch_task_context_svc_sp(const task_context_t *ctx)
{
    return (vaddr_t)ctx->kernel.sp;
}

static inline void arch_task_context_set_kernel_sp(task_context_t *ctx,
                                                   vaddr_t sp)
{
    ctx->kernel.sp = arch_task_stack_align(sp);
}

static inline void arch_task_context_set_svc_sp(task_context_t *ctx,
                                                vaddr_t sp)
{
    arch_task_context_set_kernel_sp(ctx, sp);
}

static inline void arch_task_context_save_kernel_sp(task_context_t *ctx,
                                                    vaddr_t sp)
{
    ctx->kernel.sp = sp;
}

static inline vaddr_t arch_task_context_kernel_lr(const task_context_t *ctx)
{
    return (vaddr_t)ctx->kernel.x[11];
}

static inline vaddr_t arch_task_context_kernel_pc(const task_context_t *ctx)
{
    return (vaddr_t)ctx->kernel.pc;
}

static inline uint32_t arch_task_context_kernel_cpsr(const task_context_t *ctx)
{
    (void)ctx;
    return ARCH_TASK_KERNEL_PSTATE;
}

static inline bool arch_task_context_is_first_run(const task_context_t *ctx)
{
    return (ctx->flags & ARM64_TASK_FLAG_FIRST_RUN) != 0;
}

static inline void arch_task_context_set_kernel_lr(task_context_t *ctx,
                                                   vaddr_t lr)
{
    ctx->kernel.x[11] = lr;
}

static inline void arch_task_context_set_kernel_pc(task_context_t *ctx,
                                                   vaddr_t pc)
{
    ctx->kernel.pc = pc;
}

static inline void arch_task_context_set_kernel_return_value(
    task_context_t *ctx, uint32_t value)
{
    ctx->user.x[0] = value;
}

static inline void arch_task_context_set_kernel_stack(task_context_t *ctx,
                                                      vaddr_t stack_top,
                                                      vaddr_t sp)
{
    (void)stack_top;
    arch_task_context_set_kernel_sp(ctx, sp);
}

static inline void arch_task_context_prepare_kernel_stack(task_context_t *ctx,
                                                          vaddr_t stack_top)
{
    arch_task_context_set_kernel_stack(
        ctx, stack_top, stack_top - ARCH_TASK_KERNEL_STACK_RESERVE);
}

static inline void arch_task_context_set_returns_to_user(task_context_t *ctx,
                                                         bool enabled)
{
    if (enabled)
        ctx->flags |= ARM64_TASK_FLAG_RETURNS_TO_USER;
    else
        ctx->flags &= ~ARM64_TASK_FLAG_RETURNS_TO_USER;
}

static inline bool arch_task_context_returns_to_user(const task_context_t *ctx)
{
    return (ctx->flags & ARM64_TASK_FLAG_RETURNS_TO_USER) != 0;
}

static inline uint32_t arch_task_context_user_status(const task_context_t *ctx)
{
    return (uint32_t)ctx->user.pstate;
}

static inline void arch_task_context_mark_first_run(task_context_t *ctx)
{
    ctx->flags |= ARM64_TASK_FLAG_FIRST_RUN;
}

static inline void arch_task_context_set_address_space(task_context_t *ctx,
                                                       uintptr_t address_space,
                                                       uint32_t asid)
{
    ctx->ttbr0 = (paddr_t)address_space;
    ctx->asid = asid;
}

static inline void arch_task_context_set_tls(task_context_t *ctx,
                                             uintptr_t tls_base)
{
    ctx->tls_base = (uint64_t)tls_base;
}

static inline void arch_task_set_user_tls_current(uintptr_t tls_base)
{
    __asm__ volatile("msr tpidr_el0, %0\n\tisb"
                     : : "r"((uint64_t)tls_base) : "memory");
}

static inline void arch_task_context_set_user_register(task_context_t *ctx,
                                                       uint32_t reg,
                                                       uintptr_t value)
{
    if (reg < 31u)
        ctx->user.x[reg] = (uint64_t)value;
}

static inline vaddr_t arch_task_context_user_sp(const task_context_t *ctx)
{
    return (vaddr_t)ctx->user.sp;
}

static inline void arch_task_context_capture_user(
    const task_context_t *ctx, arch_task_user_context_t *user)
{
    *user = ctx->user;
}

static inline void arch_task_context_restore_user(
    task_context_t *ctx, const arch_task_user_context_t *user)
{
    ctx->user = *user;
    ctx->user.pstate =
        (ctx->user.pstate & 0xf0000000ULL) | ARM64_USER_PSTATE_EL0T;
    arch_task_context_set_returns_to_user(ctx, true);
}

static inline void arch_task_context_enter_signal_handler(
    task_context_t *ctx, uint32_t sig, vaddr_t handler, vaddr_t restorer,
    vaddr_t frame_sp, uint32_t saved_status)
{
    ctx->user.x[0] = sig;
    ctx->user.x[30] = restorer;
    ctx->user.sp = frame_sp;
    ctx->user.pc = handler;
    ctx->user.pstate =
        ((uint64_t)saved_status & 0xf0000000ULL) | ARM64_USER_PSTATE_EL0T;
    arch_task_context_set_returns_to_user(ctx, true);
}

static inline void arch_task_context_init_user_entry(
    task_context_t *ctx, uintptr_t address_space, uint32_t asid,
    vaddr_t kernel_stack_top, vaddr_t user_pc, vaddr_t user_sp)
{
    arch_task_context_set_address_space(ctx, address_space, asid);
    arch_task_context_prepare_kernel_stack(ctx, kernel_stack_top);
    ctx->user.sp = user_sp;
    ctx->user.pc = user_pc;
    ctx->user.pstate = ARM64_USER_PSTATE_EL0T;
    ctx->kernel.x[0] = (uint64_t)(uintptr_t)&ctx->user;
    ctx->kernel.pc = (uint64_t)(uintptr_t)arm64_task_user_entry_trampoline;
    arch_task_context_mark_first_run(ctx);
    arch_task_context_set_returns_to_user(ctx, true);
}

static inline void arch_task_context_init_kernel_entry(
    task_context_t *ctx, void (*entry)(void *), void *arg, vaddr_t stack_top)
{
    arch_task_context_prepare_kernel_stack(ctx, stack_top);
    ctx->kernel.x[0] = (uint64_t)(uintptr_t)entry;
    ctx->kernel.x[1] = (uint64_t)(uintptr_t)arg;
    ctx->kernel.pc =
        (uint64_t)(uintptr_t)arm64_task_kernel_entry_trampoline;
    arch_task_context_mark_first_run(ctx);
    arch_task_context_set_returns_to_user(ctx, false);
}

static inline void arch_task_context_prepare_user_fork(
    task_context_t *child, const task_context_t *parent,
    uintptr_t address_space, uint32_t asid, vaddr_t kernel_return_pc,
    uint32_t saved_status)
{
    (void)kernel_return_pc;
    (void)saved_status;
    arch_task_context_set_address_space(child, address_space, asid);
    child->user = parent->user;
    child->user.x[0] = 0;
    child->kernel.x[0] = (uint64_t)(uintptr_t)&child->user;
    child->kernel.pc =
        (uint64_t)(uintptr_t)arm64_task_user_entry_trampoline;
    arch_task_context_mark_first_run(child);
    arch_task_context_set_returns_to_user(child, true);
}

static inline void arch_task_context_prepare_kernel_fork(
    task_context_t *child, const task_context_t *parent,
    uintptr_t address_space, uint32_t asid, vaddr_t kernel_return_pc,
    uint32_t saved_status)
{
    (void)saved_status;
    arch_task_context_set_address_space(child, address_space, asid);
    child->kernel = parent->kernel;
    child->kernel.pc = kernel_return_pc;
    child->user.x[0] = 0;
    arch_task_context_mark_first_run(child);
    arch_task_context_set_returns_to_user(child, false);
}

static inline void arch_task_signal_frame_save(
    arch_user_signal_frame_t *frame, const task_context_t *ctx,
    uint32_t old_blocked, uint32_t sig)
{
    frame->user = ctx->user;
    frame->old_blocked = old_blocked;
    frame->sig = sig;
}

static inline void arch_task_signal_frame_restore(
    task_context_t *ctx, const arch_user_signal_frame_t *frame)
{
    arch_task_context_restore_user(ctx, &frame->user);
}

static inline uint32_t arch_task_signal_frame_status(
    const arch_user_signal_frame_t *frame)
{
    return (uint32_t)frame->user.pstate;
}

static inline uint32_t arch_task_signal_frame_blocked(
    const arch_user_signal_frame_t *frame)
{
    return frame->old_blocked;
}

static inline uint32_t arch_task_signal_frame_signal(
    const arch_user_signal_frame_t *frame)
{
    return frame->sig;
}

#endif
