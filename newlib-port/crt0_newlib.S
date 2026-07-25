/*
 * ArmOS
 * Copyright (c) 2026 Mohamed Ennassiri
 *
 * Licensed under the Apache License, Version 2.0.
 * See LICENSE for details.
 *
 * File: newlib-port/crt0_newlib.S
 * Layer: Userland / newlib port
 * Description: Newlib syscall glue and ArmOS ABI adaptation layer.
 */

.section .text
.align 4
.global _start
.type _start, %function

_start:
    /*
     * Stack layout prepared by the kernel:
     *   sp[0]        = argc
     *   sp[1..]      = argv pointers, NULL terminated
     *   after argv   = envp pointers, NULL terminated
     */
    ldr r0, [sp]              /* argc */
    add r1, sp, #4            /* argv */
    add r2, r1, r0, lsl #2
    add r2, r2, #4            /* envp = argv + argc + NULL */
    bic sp, sp, #7            /* AAPCS 8-byte stack alignment */

    ldr r3, =environ
    str r2, [r3]

    push {r0-r2}
    bl __armos_runtime_init
    pop {r0-r2}

    push {r0-r2}
    cmp r0, #0
    beq 2f
    ldr r0, [r1]
    bl __armos_init_program_name
2:
    pop {r0-r2}

    bl main
    bl exit

1:
    b 1b
