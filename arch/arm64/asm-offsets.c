/*
 * ArmOS
 * Copyright (c) 2026 Mohamed Ennassiri
 *
 * Licensed under the Apache License, Version 2.0.
 * See LICENSE for details.
 *
 * File: arch/arm64/asm-offsets.c
 * Layer: ARM64 / build-time ABI generation
 *
 * Responsibilities:
 * - Export C structure offsets consumed by AArch64 assembly.
 * - Keep exception, EL0-entry and task-switch layouts synchronized with C.
 *
 * Notes:
 * - This file is compiled to assembly only and is never linked.
 * - Makefile extracts the emitted records into generated GAS .equ symbols.
 */

#include <asm/exception_frame.h>
#include <asm/task_context.h>
#include <kernel/stddef.h>

#define DEFINE(sym, val) \
    __asm__ volatile("\n.ascii \"->" #sym " %0 " #val "\"" : : "i" (val))

void arm64_emit_asm_offsets(void)
{
    DEFINE(ARM64_CTX_X0, offsetof(arm64_user_context_t, x[0]));
    DEFINE(ARM64_CTX_X30, offsetof(arm64_user_context_t, x[30]));
    DEFINE(ARM64_CTX_SP, offsetof(arm64_user_context_t, sp));
    DEFINE(ARM64_CTX_PC, offsetof(arm64_user_context_t, pc));
    DEFINE(ARM64_CTX_PSTATE, offsetof(arm64_user_context_t, pstate));
    DEFINE(ARM64_CTX_SIZE, sizeof(arm64_user_context_t));

    DEFINE(ARM64_EXC_ESR, offsetof(arm64_exception_frame_t, esr));
    DEFINE(ARM64_EXC_FAR, offsetof(arm64_exception_frame_t, far));
    DEFINE(ARM64_EXC_VECTOR, offsetof(arm64_exception_frame_t, vector));
    DEFINE(ARM64_EXC_RESERVED, offsetof(arm64_exception_frame_t, reserved));
    DEFINE(ARM64_EXC_FRAME_SIZE, sizeof(arm64_exception_frame_t));

    DEFINE(ARM64_TASK_X19,
           offsetof(arm64_task_context_t, kernel.x[0]));
    DEFINE(ARM64_TASK_X21,
           offsetof(arm64_task_context_t, kernel.x[2]));
    DEFINE(ARM64_TASK_X23,
           offsetof(arm64_task_context_t, kernel.x[4]));
    DEFINE(ARM64_TASK_X25,
           offsetof(arm64_task_context_t, kernel.x[6]));
    DEFINE(ARM64_TASK_X27,
           offsetof(arm64_task_context_t, kernel.x[8]));
    DEFINE(ARM64_TASK_X29,
           offsetof(arm64_task_context_t, kernel.x[10]));
    DEFINE(ARM64_TASK_SP,
           offsetof(arm64_task_context_t, kernel.sp));
    DEFINE(ARM64_TASK_PC,
           offsetof(arm64_task_context_t, kernel.pc));
    DEFINE(ARM64_TASK_USER,
           offsetof(arm64_task_context_t, user));
    DEFINE(ARM64_TASK_SIMD_Q0,
           offsetof(arm64_task_context_t, simd.q[0]));
    DEFINE(ARM64_TASK_SIMD_FPCR,
           offsetof(arm64_task_context_t, simd.fpcr));
    DEFINE(ARM64_TASK_SIMD_FPSR,
           offsetof(arm64_task_context_t, simd.fpsr));
    DEFINE(ARM64_TASK_TLS,
           offsetof(arm64_task_context_t, tls_base));
    DEFINE(ARM64_TASK_TTBR0,
           offsetof(arm64_task_context_t, ttbr0));
    DEFINE(ARM64_TASK_ASID,
           offsetof(arm64_task_context_t, asid));
    DEFINE(ARM64_TASK_FLAGS,
           offsetof(arm64_task_context_t, flags));
    DEFINE(ARM64_TASK_SIZE, sizeof(arm64_task_context_t));
}
