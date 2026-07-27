/*
 * ArmOS
 * Copyright (c) 2026 Mohamed Ennassiri
 *
 * Licensed under the Apache License, Version 2.0.
 * See LICENSE for details.
 *
 * File: newlib-port/arm64/crt0_newlib.S
 * Layer: Userland / newlib port / AArch64
 *
 * Responsibilities:
 * - Decode the ArmOS ELF64 initial stack into argc, argv and envp.
 * - Publish environ and enter a newlib-backed AArch64 main function.
 * - Terminate through newlib exit when main returns.
 *
 * Notes:
 * - The kernel provides an AAPCS64 16-byte-aligned stack.
 */

.section .text
.align 4
.global _start
.type _start, %function

_start:
    ldr x0, [sp]
    add x1, sp, #8
    add x2, x1, x0, lsl #3
    add x2, x2, #8
    adrp x3, environ
    add x3, x3, :lo12:environ
    str x2, [x3]

    stp x0, x1, [sp, #-32]!
    str x2, [sp, #16]
    bl __armos_runtime_init
    bl __armos_runtime_run_init_array
    ldp x0, x1, [sp]
    ldr x2, [sp, #16]
    cbz x0, 1f
    ldr x0, [x1]
    bl __armos_init_program_name
1:
    ldp x0, x1, [sp]
    ldr x2, [sp, #16]
    add sp, sp, #32
    bl main
    bl exit
2:
    b 2b

.size _start, . - _start
