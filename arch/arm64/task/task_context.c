/*
 * ArmOS
 * Copyright (c) 2026 Mohamed Ennassiri
 *
 * Licensed under the Apache License, Version 2.0.
 * See LICENSE for details.
 *
 * File: arch/arm64/task/task_context.c
 * Layer: ARM64 / task switching
 *
 * Responsibilities:
 * - Validate the address-space identity carried by a task context.
 * - Resolve user TTBR0/ASID identities through the ARM64 VM backend.
 * - Activate TTBR0 and its ASID before switching kernel register state.
 *
 * Notes:
 * - A zero TTBR0/ASID pair denotes a kernel-only context.
 * - Residency decisions are delegated to the owned user-VM object.
 */

#include <asm/mmu.h>
#include <asm/task_context.h>
#include <asm/user_vm.h>

int arm64_task_context_switch_address_space(
    arm64_task_context_t *previous,
    arm64_task_context_t *next)
{
    int result;

    if (!next)
        return -1;

    if (next->ttbr0 != 0 || next->asid != 0) {
        if (next->ttbr0 == 0)
            return -2;
        result = next->asid == 0 ?
            arm64_mmu_switch_ttbr0(next->ttbr0) :
            arm64_user_vm_activate_identity(next->ttbr0, next->asid,
                                            &next->asid);
        if (result != 0)
            return -3;
    } else {
        arm64_user_vm_deactivate_cpu();
    }

    arm64_task_context_switch(previous, next);
    return 0;
}

void __task_switch(arm64_task_context_t *previous,
                   arm64_task_context_t *next)
{
    if (arm64_task_context_switch_address_space(previous, next) != 0) {
        for (;;)
            __asm__ volatile("wfe");
    }
}
