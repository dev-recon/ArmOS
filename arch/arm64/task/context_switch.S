/*
 * ArmOS
 * Copyright (c) 2026 Mohamed Ennassiri
 *
 * Licensed under the Apache License, Version 2.0.
 * See LICENSE for details.
 *
 * File: arch/arm64/task/context_switch.S
 * Layer: ARM64 / task switching
 *
 * Responsibilities:
 * - Save and restore AAPCS64 kernel callee-saved state and stack position.
 * - Preserve the complete FP/SIMD register file between common kernel tasks.
 * - Enter new kernel and EL0 tasks through architecture trampolines.
 *
 * Notes:
 * - Scheduling policy and task state transitions belong to kernel/task.
 * - A null previous context is valid when the scheduler leaves boot context.
 */

.include "asm-offsets.h"

.section .text
.align 4
.extern task_context_save_complete
.global arm64_task_context_switch
.type arm64_task_context_switch, %function
arm64_task_context_switch:
    msr daifset, #2
    isb
    cbz x0, .Lrestore
    stp q0,  q1,  [x0, #ARM64_TASK_SIMD_Q0 + 0]
    stp q2,  q3,  [x0, #ARM64_TASK_SIMD_Q0 + 32]
    stp q4,  q5,  [x0, #ARM64_TASK_SIMD_Q0 + 64]
    stp q6,  q7,  [x0, #ARM64_TASK_SIMD_Q0 + 96]
    stp q8,  q9,  [x0, #ARM64_TASK_SIMD_Q0 + 128]
    stp q10, q11, [x0, #ARM64_TASK_SIMD_Q0 + 160]
    stp q12, q13, [x0, #ARM64_TASK_SIMD_Q0 + 192]
    stp q14, q15, [x0, #ARM64_TASK_SIMD_Q0 + 224]
    stp q16, q17, [x0, #ARM64_TASK_SIMD_Q0 + 256]
    stp q18, q19, [x0, #ARM64_TASK_SIMD_Q0 + 288]
    stp q20, q21, [x0, #ARM64_TASK_SIMD_Q0 + 320]
    stp q22, q23, [x0, #ARM64_TASK_SIMD_Q0 + 352]
    stp q24, q25, [x0, #ARM64_TASK_SIMD_Q0 + 384]
    stp q26, q27, [x0, #ARM64_TASK_SIMD_Q0 + 416]
    stp q28, q29, [x0, #ARM64_TASK_SIMD_Q0 + 448]
    stp q30, q31, [x0, #ARM64_TASK_SIMD_Q0 + 480]
    mrs x2, fpcr
    str x2, [x0, #ARM64_TASK_SIMD_FPCR]
    mrs x2, fpsr
    str x2, [x0, #ARM64_TASK_SIMD_FPSR]
    mrs x2, tpidr_el0
    str x2, [x0, #ARM64_TASK_TLS]
    stp x19, x20, [x0, #ARM64_TASK_X19]
    stp x21, x22, [x0, #ARM64_TASK_X21]
    stp x23, x24, [x0, #ARM64_TASK_X23]
    stp x25, x26, [x0, #ARM64_TASK_X25]
    stp x27, x28, [x0, #ARM64_TASK_X27]
    stp x29, x30, [x0, #ARM64_TASK_X29]
    mov x2, sp
    str x2, [x0, #ARM64_TASK_SP]
    adr x2, .Lcontext_resume
    str x2, [x0, #ARM64_TASK_PC]

.Lrestore:
    /*
     * Release the previous task only after this CPU runs on the selected
     * task's kernel stack. A READY task still marked as owned by this CPU is
     * intentionally not schedulable, so omitting this handoff strands it.
     */
    mov x19, x0
    mov x20, x1
    ldr x3, [x20, #ARM64_TASK_SP]
    mov sp, x3
    mov x0, x19
    bl task_context_save_complete
    mov x1, x20

    ldp x19, x20, [x1, #ARM64_TASK_X19]
    ldp x21, x22, [x1, #ARM64_TASK_X21]
    ldp x23, x24, [x1, #ARM64_TASK_X23]
    ldp x25, x26, [x1, #ARM64_TASK_X25]
    ldp x27, x28, [x1, #ARM64_TASK_X27]
    ldp x29, x30, [x1, #ARM64_TASK_X29]
    ldp q0,  q1,  [x1, #ARM64_TASK_SIMD_Q0 + 0]
    ldp q2,  q3,  [x1, #ARM64_TASK_SIMD_Q0 + 32]
    ldp q4,  q5,  [x1, #ARM64_TASK_SIMD_Q0 + 64]
    ldp q6,  q7,  [x1, #ARM64_TASK_SIMD_Q0 + 96]
    ldp q8,  q9,  [x1, #ARM64_TASK_SIMD_Q0 + 128]
    ldp q10, q11, [x1, #ARM64_TASK_SIMD_Q0 + 160]
    ldp q12, q13, [x1, #ARM64_TASK_SIMD_Q0 + 192]
    ldp q14, q15, [x1, #ARM64_TASK_SIMD_Q0 + 224]
    ldp q16, q17, [x1, #ARM64_TASK_SIMD_Q0 + 256]
    ldp q18, q19, [x1, #ARM64_TASK_SIMD_Q0 + 288]
    ldp q20, q21, [x1, #ARM64_TASK_SIMD_Q0 + 320]
    ldp q22, q23, [x1, #ARM64_TASK_SIMD_Q0 + 352]
    ldp q24, q25, [x1, #ARM64_TASK_SIMD_Q0 + 384]
    ldp q26, q27, [x1, #ARM64_TASK_SIMD_Q0 + 416]
    ldp q28, q29, [x1, #ARM64_TASK_SIMD_Q0 + 448]
    ldp q30, q31, [x1, #ARM64_TASK_SIMD_Q0 + 480]
    ldr x2, [x1, #ARM64_TASK_SIMD_FPCR]
    msr fpcr, x2
    ldr x2, [x1, #ARM64_TASK_SIMD_FPSR]
    msr fpsr, x2
    ldr x2, [x1, #ARM64_TASK_TLS]
    msr tpidr_el0, x2
    isb
    ldr x2, [x1, #ARM64_TASK_PC]
    br x2

.Lcontext_resume:
    ret
.size arm64_task_context_switch, . - arm64_task_context_switch

.align 4
.global arm64_task_kernel_entry_trampoline
.type arm64_task_kernel_entry_trampoline, %function
arm64_task_kernel_entry_trampoline:
    msr daifclr, #2
    isb
    mov x0, x20
    blr x19
1:
    wfe
    b 1b
.size arm64_task_kernel_entry_trampoline, . - arm64_task_kernel_entry_trampoline

.align 4
.global arm64_task_user_entry_trampoline
.type arm64_task_user_entry_trampoline, %function
arm64_task_user_entry_trampoline:
    mov x0, x19
    b arm64_enter_el0
.size arm64_task_user_entry_trampoline, . - arm64_task_user_entry_trampoline

.align 4
.global arm64_simd_context_capture
.type arm64_simd_context_capture, %function
arm64_simd_context_capture:
    stp q0,  q1,  [x0, #0]
    stp q2,  q3,  [x0, #32]
    stp q4,  q5,  [x0, #64]
    stp q6,  q7,  [x0, #96]
    stp q8,  q9,  [x0, #128]
    stp q10, q11, [x0, #160]
    stp q12, q13, [x0, #192]
    stp q14, q15, [x0, #224]
    stp q16, q17, [x0, #256]
    stp q18, q19, [x0, #288]
    stp q20, q21, [x0, #320]
    stp q22, q23, [x0, #352]
    stp q24, q25, [x0, #384]
    stp q26, q27, [x0, #416]
    stp q28, q29, [x0, #448]
    stp q30, q31, [x0, #480]
    mrs x1, fpcr
    str x1, [x0, #512]
    mrs x1, fpsr
    str x1, [x0, #520]
    ret
.size arm64_simd_context_capture, . - arm64_simd_context_capture
