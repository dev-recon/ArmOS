/*
 * ArmOS
 * Copyright (c) 2026 Mohamed Ennassiri
 *
 * Licensed under the Apache License, Version 2.0.
 * See LICENSE for details.
 *
 * File: arch/arm32/task/task_switch.S
 * Layer: Kernel / ARMv7-A context switching
 *
 * Responsibilities:
 * - Save and restore kernel and user task contexts.
 * - Preserve banked ARM registers across SVC/user transitions.
 *
 * Notes:
 * - Small changes here can affect syscall return, preemption, and signals.
 */

.text
.align 4

.global current_task
.global task_context_save_complete

/*
 * Generated from arch/arm32/asm-offsets.c. Keep task_context_t layout changes in C;
 * assembly consumes the generated constants.
 */
.include "asm-offsets.h"


/*
 * __task_switch - ARM32 context switch.
 *
 * This routine does not return to its call site in the normal C sense.
 * It saves old_ctx, loads new_ctx, updates current_task, then resumes the
 * selected task either in kernel/SVC mode or by exception-returning to user.
 *
 * ABI:
 *   r0 = old_ctx, or NULL when there is no previous task to save
 *   r1 = new_ctx
 *
 * Supported transitions:
 *   - first-run user task
 *   - resumed user task
 *   - first-run kernel task
 *   - resumed kernel task
 *
 * Critical invariant:
 * A task that yielded while still inside a syscall must resume through
 * __restore_to_kernel, using the saved deep SVC stack pointer in ctx->sp.
 * Returning directly to user in that case would reset SVC SP to svc_sp_top
 * and abandon the syscall stack frame. That was the root cause of the old
 * wait/sleep stack corruption bug.
 */


.global __task_switch
.align 4
.type __task_switch, %function
.syntax unified


#define USE_RTUSR_SNAP 0

/*
 * Release the previous task only after this CPU has switched to the new
 * kernel/SVC stack.  The old task may already be queued as READY, but while
 * this routine still runs on its saved stack another CPU must not be allowed
 * to restore that same SP.
 *
 * r0 = old_ctx or NULL
 * r12 = new_ctx, preserved
 */
.macro finish_old_stack_handoff
    push    {r12, lr}
    bl      task_context_save_complete
    pop     {r12, lr}
.endm

/* Entry: r0 = old_ctx or NULL, r1 = new_ctx. */
/* r11 is not used as a scratch register here; r12 holds the context base. */

__task_switch:
    /* ---------- Phase 1: save old_ctx ---------- */
    cmp     r0, #0
    beq     __load_new_context

    /* r0 = &old_ctx */
    stmia   r0, {r0-r12}                   // save r0..r12

    /* Retourne-t-on en mode user ? */
    //ldr     r2, [r0, #CTX_RET_TO_USER]
    //cmp     r2, #1
    //beq     __user_ctx

    str     sp, [r0, #CTX_SP]              // current SVC/kernel stack pointer
    str     sp, [r0, #CTX_SVC_SP]          // diagnostic copy of the same SVC SP

__user_ctx:
    str     lr, [r0, #CTX_LR]              // kernel resume address
    str     lr, [r0, #CTX_PC]              // optional PC mirror
    mrs     r2, cpsr
    str     r2, [r0, #CTX_CPSR]            // CPSR for the saved kernel context
    mov     r2, #0
    str     r2, [r0, #CTX_FIRST]           // is_first_run = 0
    str     r2, [r0, #CTX_RET_TO_USER]     // force __restore_to_kernel on resume
    mrc     p15, 0, r2, c13, c0, 3         // TPIDRURO
    str     r2, [r0, #CTX_TLS]

    /* usr_r[] is owned by SVC entry; do not refresh it from kernel registers. */
    //add     r3, r0, #CTX_USR_R0
    //stmia   r3, {r0-r12}                   // usr_r[0..12] = r0..r12

__load_new_context:
    /* ---------- Phase 2: load new_ctx ---------- */
    mov     r12, r1                         // r12 = &new_ctx (base ctx)

    /* Address-space context. */
    ldr     r2, [r12, #CTX_TTBR0]
    ldr     r3, [r12, #CTX_ASID]
    /*
     * ctx->ttbr0 stores the aligned table base. Page-table walks must use the
     * same SMP attributes as the C MMU path: shareable, outer WBWA and inner
     * WBWA. Without these bits, another CPU can observe stale L1 entries after
     * a task migrates and fault while the software page-table walk is valid.
     */
    orr     r2, r2, #(1 << 1)              // S: shareable walks
    orr     r2, r2, #(1 << 3)              // RGN: outer WBWA
    orr     r2, r2, #(1 << 6)              // IRGN: inner WBWA
    dsb
    mcr     p15, 0, r2, c2,  c0, 0         // TTBR0
    mcr     p15, 0, r3, c13, c0, 1         // ASID
    ldr     r2, [r12, #CTX_TLS]
    mcr     p15, 0, r2, c13, c0, 3         // TPIDRURO
    /* Keep the proven full local flush while the ASID path is validated. */
    mov     r2, #0
    mcr     p15, 0, r2, c8,  c7, 0         // TLBIALL
    dsb
    isb

    /*
     * Publish the task pointer after the MMU context is selected.
     *
     * current_task is a legacy CPU0-visible alias. In SMP mode, publishing a
     * secondary CPU task there would make old diagnostics observe the wrong
     * task, so only CPU0 updates it. The authoritative storage is
     * current_tasks[cpu].
     */
    sub    r3, r12, #TASK_CONTEXT_OFF
    mrc    p15, 0, r4, c0, c0, 5
    and    r4, r4, #3
    ldr    r2, =current_tasks
    str    r3, [r2, r4, lsl #2]
    mcr    p15, 0, r3, c13, c0, 4        @ TPIDRPRW = current task for this CPU
    cmp    r4, #0
    ldreq  r2, =current_task
    streq  r3, [r2]
    dmb

    /* First run? */
    ldr     r2, [r12, #CTX_FIRST]
    cmp     r2, #1
    beq     __first_run_setup

    /* Resume directly to user only for contexts explicitly prepared that way. */
    ldr     r2, [r12, #CTX_RET_TO_USER]
    cmp     r2, #1
    beq     __restore_to_user

    /* ---------- Restore a suspended kernel/SVC context ---------- */
__restore_to_kernel:
    ldr     sp, [r12, #CTX_SP]             // r13_svc = saved kernel stack
    finish_old_stack_handoff
    ldr     r2, [r12, #CTX_CPSR]
    msr     cpsr_cxsf, r2
    ldr     lr, [r12, #CTX_LR]             // r14_svc = kernel resume point

    /*
     * Restore user-banked r13/r14 even though we are resuming in SVC mode.
     * Another task may have changed r13_usr/r14_usr while it ran. When this
     * task later reaches swi_handler's endal "movs pc, lr", ARM will switch
     * back to user mode and consume those banked registers. If r13_usr is
     * stale, the libc syscall wrapper pops its return address from the wrong
     * user stack and can branch to garbage.
     */
    cps     #0x1F                          // SYS mode exposes user-bank SP/LR
    ldr     sp, [r12, #CTX_USR_SP]         // r13_usr = ctx->usr_sp
    ldr     lr, [r12, #CTX_USR_LR]         // r14_usr = ctx->usr_lr
    cps     #0x13                          // back to SVC bank

    ldmia   r12, {r0-r11}              /* restore r0-r11 */
    ldr     r12, [r12, #CTX_R12]       /* restore r12 last */
    bx      lr                         // return inside schedule()/yield path

    /* ---------- Restore a user context prepared for direct exception return ---------- */
__restore_to_user:
    ldr     sp, [r12, #CTX_SVC_SP_TOP]
    finish_old_stack_handoff
    /* 1) SPSR <- usr_cpsr (forcer ARM, T=0) */
    //push {r0-r3, lr}
    //mov r0, r12
    //bl debug_print_sp
    //pop {r0-r3, lr}

    ldr     r2, [r12, #CTX_USR_CPSR]
    //bic     r2, r2, #0x20
    msr     spsr_cxsf, r2

    /* 2) User-banked SP/LR. */
    cps     #0x1F
    ldr     sp, [r12, #CTX_USR_SP]
    ldr     lr, [r12, #CTX_USR_LR]
    //bic     lr, lr, #1
    cps     #0x13

    /* 3) r0..r10 from usr_r[], keeping r12 as the context base. */
    add     r10, r12, #CTX_USR_R0
    ldmia   r10, {r0-r9}
    ldr     r10, [r12, #CTX_USR_R10]

    /* 4) LR_svc = usr_pc (pair en ARM) */
    ldr     lr, [r12, #CTX_USR_PC]
    //bic     lr, lr, #1

    /* 5) Restore r11 and r12 at the very end. */
    ldr     r11, [r12, #CTX_USR_R11]
    ldr     r12, [r12, #CTX_USR_R12]

    /* 6) Exception-return to user mode. */
    movs    pc, lr

    /* ---------- First execution: choose user or kernel entry ---------- */
__first_run_setup:
    ldr     r2, [r12, #CTX_RET_TO_USER]
    cmp     r2, #1
    beq     __first_run_user

__first_run_kernel:
    /* Initial kernel stack. */
    ldr     sp, [r12, #CTX_SP]
    finish_old_stack_handoff
    /* Restore aussi l'state IRQ/FIQ du new contexte kernel. */
    ldr     r2, [r12, #CTX_CPSR]
    msr     cpsr_cxsf, r2
    /* Kernel entry point. */
    ldr     lr, [r12, #CTX_PC]
    mov     r2, #0
    str     r2, [r12, #CTX_FIRST]
    /* r0..r12 init */
    ldmia   r12, {r0-r12}
    bx      lr

__first_run_user:
    /* Kernel stack used for future SVC entries. */
    ldr     sp, [r12, #CTX_SVC_SP_TOP]
    finish_old_stack_handoff
    //ldr     sp, [r12, #CTX_SVC_SP]

    /* SPSR = usr_cpsr (ARM) */
    ldr     r2, [r12, #CTX_USR_CPSR]
    //bic     r2, r2, #0x20
    msr     spsr_cxsf, r2

    /* LR_svc = usr_pc (pair) */
    ldr     lr, [r12, #CTX_USR_PC]
    //bic     lr, lr, #1

    /* Mark as no longer first-run before entering user mode. */
    mov     r3, #0
    str     r3, [r12, #CTX_FIRST]

    /* SP/LR user init */
    cps     #0x1F
    ldr     sp, [r12, #CTX_USR_SP]
    ldr     lr, [r12, #CTX_USR_LR]
    //bic     lr, lr, #1
    cps     #0x13

    /* Initialize r0..r10 from usr_r[]. */
    add     r10, r12, #CTX_USR_R0
    ldmia   r10, {r0-r9}
    ldr     r10, [r12, #CTX_USR_R10]

    /* Restore r11/r12 last, then exception-return to user mode. */
    ldr     r11, [r12, #CTX_USR_R11]
    ldr     r12, [r12, #CTX_USR_R12]
    movs    pc, lr
/* 
 * Usage notes:
 *
 * For a new user task:
 *   - is_first_run = 1
 *   - returns_to_user = 1
 *   - usr_pc = user entry point
 *   - usr_sp = initial user stack pointer
 *   - usr_cpsr = user CPSR, ARM state
 *   - usr_r[0-12] = initial user arguments
 *
 * For a new kernel task:
 *   - is_first_run = 1
 *   - returns_to_user = 0
 *   - pc = kernel entry function
 *   - sp/svc_sp_top = allocated kernel stack
 *   - r0-r12 = entry arguments
 *
 * For a suspended task:
 *   - is_first_run = 0
 *   - ctx->sp is authoritative for the SVC stack
 *   - returns_to_user must be 0 when the task yielded inside the kernel
 */
/*
 * Debug information visible in objdump.
 */
.section .rodata
task_switch_info:
    .ascii "ARM32 Task Switch v1.0 - Cortex-A15 Optimized\0"
    .ascii "Context size: generated from task_context_t\0"
    .ascii "Supports: Full context save/restore\0"
