/*
 * ArmOS
 * Copyright (c) 2026 Mohamed Ennassiri
 *
 * Licensed under the Apache License, Version 2.0.
 * See LICENSE for details.
 *
 * File: arch/arm32/asm-offsets.c
 * Layer: Kernel / build-time ABI generation
 *
 * Responsibilities:
 * - Export C structure offsets consumed by ARM assembly.
 * - Keep syscall, IRQ return, and task-switch code tied to task_context_t.
 *
 * Notes:
 * - This file is compiled to assembly only; it is never linked into the kernel.
 * - The Makefile extracts the "->SYMBOL VALUE" records into GAS .equ lines.
 */

#include <kernel/task.h>

#define DEFINE(sym, val) \
    __asm__ volatile("\n.ascii \"->" #sym " %0 " #val "\"" : : "i" (val))

_Static_assert(offsetof(task_t, context) == 48,
               "task_t.context offset must match ARM32 assembly");
_Static_assert(sizeof(task_context_t) == 176,
               "task_context_t layout must match ARM32 assembly");

void arm_os_emit_asm_offsets(void)
{
    DEFINE(TASK_CONTEXT_OFF, offsetof(task_t, context));

    DEFINE(CTX_R0, offsetof(task_context_t, r0));
    DEFINE(CTX_R1, offsetof(task_context_t, r1));
    DEFINE(CTX_R2, offsetof(task_context_t, r2));
    DEFINE(CTX_R3, offsetof(task_context_t, r3));
    DEFINE(CTX_R4, offsetof(task_context_t, r4));
    DEFINE(CTX_R5, offsetof(task_context_t, r5));
    DEFINE(CTX_R6, offsetof(task_context_t, r6));
    DEFINE(CTX_R7, offsetof(task_context_t, r7));
    DEFINE(CTX_R8, offsetof(task_context_t, r8));
    DEFINE(CTX_R9, offsetof(task_context_t, r9));
    DEFINE(CTX_R10, offsetof(task_context_t, r10));
    DEFINE(CTX_R11, offsetof(task_context_t, r11));
    DEFINE(CTX_R12, offsetof(task_context_t, r12));
    DEFINE(CTX_SP, offsetof(task_context_t, sp));
    DEFINE(CTX_LR, offsetof(task_context_t, lr));
    DEFINE(CTX_PC, offsetof(task_context_t, pc));
    DEFINE(CTX_CPSR, offsetof(task_context_t, cpsr));
    DEFINE(CTX_FIRST, offsetof(task_context_t, is_first_run));
    DEFINE(CTX_TTBR0, offsetof(task_context_t, ttbr0));
    DEFINE(CTX_ASID, offsetof(task_context_t, asid));
    DEFINE(CTX_SPSR, offsetof(task_context_t, spsr));
    DEFINE(CTX_RET_TO_USER, offsetof(task_context_t, returns_to_user));

    DEFINE(CTX_USR_R0, offsetof(task_context_t, usr_r[0]));
    DEFINE(CTX_USR_R1, offsetof(task_context_t, usr_r[1]));
    DEFINE(CTX_USR_R2, offsetof(task_context_t, usr_r[2]));
    DEFINE(CTX_USR_R3, offsetof(task_context_t, usr_r[3]));
    DEFINE(CTX_USR_R4, offsetof(task_context_t, usr_r[4]));
    DEFINE(CTX_USR_R5, offsetof(task_context_t, usr_r[5]));
    DEFINE(CTX_USR_R6, offsetof(task_context_t, usr_r[6]));
    DEFINE(CTX_USR_R7, offsetof(task_context_t, usr_r[7]));
    DEFINE(CTX_USR_R8, offsetof(task_context_t, usr_r[8]));
    DEFINE(CTX_USR_R9, offsetof(task_context_t, usr_r[9]));
    DEFINE(CTX_USR_R10, offsetof(task_context_t, usr_r[10]));
    DEFINE(CTX_USR_R11, offsetof(task_context_t, usr_r[11]));
    DEFINE(CTX_USR_R12, offsetof(task_context_t, usr_r[12]));
    DEFINE(CTX_USR_SP, offsetof(task_context_t, usr_sp));
    DEFINE(CTX_USR_LR, offsetof(task_context_t, usr_lr));
    DEFINE(CTX_USR_PC, offsetof(task_context_t, usr_pc));
    DEFINE(CTX_USR_CPSR, offsetof(task_context_t, usr_cpsr));
    DEFINE(CTX_SVC_SP_TOP, offsetof(task_context_t, svc_sp_top));
    DEFINE(CTX_SVC_SP, offsetof(task_context_t, svc_sp));
    DEFINE(CTX_SVC_LR_SAVED, offsetof(task_context_t, svc_lr_saved));
    DEFINE(CTX_TLS, offsetof(task_context_t, tls_base));
    DEFINE(CTX_SIZE, sizeof(task_context_t));
}
