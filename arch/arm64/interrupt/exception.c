/*
 * ArmOS
 * Copyright (c) 2026 Mohamed Ennassiri
 *
 * Licensed under the Apache License, Version 2.0.
 * See LICENSE for details.
 *
 * File: arch/arm64/interrupt/exception.c
 * Layer: ARM64 / exception dispatch
 *
 * Responsibilities:
 * - Decode AArch64 exception classes and the EL0 syscall register ABI.
 * - Route syscalls, page faults and device IRQs into common kernel services.
 * - Preserve the current task's EL0 register image across kernel entry.
 *
 * Notes:
 * - Process, VFS, signal and scheduling policy remain architecture-neutral.
 * - Timer preemption is requested by the common timer and serviced at safe
 *   task-switch points.
 */

#include <asm/exception_frame.h>
#include <kernel/arch_cpu.h>
#include <kernel/interrupt.h>
#include <kernel/kprintf.h>
#include <kernel/memory.h>
#include <kernel/signal.h>
#include <kernel/smp.h>
#include <kernel/syscalls.h>
#include <kernel/task.h>
#include <kernel/timer.h>

#define ESR_EC_SHIFT             26u
#define ESR_EC_MASK              0x3fu
#define ESR_EC_SVC64             0x15u
#define ESR_EC_INSN_ABORT_LOWER  0x20u
#define ESR_EC_DATA_ABORT_LOWER  0x24u
#define ARM64_VECTOR_IRQ_SPX      5u
#define ARM64_VECTOR_SYNC_LOWER   8u
#define ARM64_VECTOR_IRQ_LOWER    9u

extern void arm64_exception_depth_enter(void);
extern void arm64_exception_depth_leave(void);

static void halt_exception(const arm64_exception_frame_t *frame)
{
    task_t *task = task_current_local();
    uint64_t ec = (frame->esr >> ESR_EC_SHIFT) & ESR_EC_MASK;

    KERROR("ARM64 exception vector=%lu EC=0x%lX ESR=0x%lX\n",
           (unsigned long)frame->vector, (unsigned long)ec,
           (unsigned long)frame->esr);
    KERROR("ELR=0x%lX FAR=0x%lX SPSR=0x%lX\n",
           (unsigned long)frame->user.pc, (unsigned long)frame->far,
           (unsigned long)frame->user.pstate);
    KERROR("X0=0x%lX X1=0x%lX X2=0x%lX X29=0x%lX X30=0x%lX SP0=0x%lX\n",
           (unsigned long)frame->user.x[0],
           (unsigned long)frame->user.x[1],
           (unsigned long)frame->user.x[2],
           (unsigned long)frame->user.x[29],
           (unsigned long)frame->user.x[30],
           (unsigned long)frame->user.sp);
    if (task) {
        KERROR("CPU=%u TID=%u PID=%d TASK=%s STATE=%u FLAGS=0x%X\n",
               smp_processor_id(), task->task_id,
               task->type == TASK_TYPE_PROCESS && task->process
                   ? task->process->pid : -1,
               task->name,
               (unsigned int)task->state, task->context.flags);
        KERROR("TASK EL0 PC=0x%lX SP=0x%lX X29=0x%lX X30=0x%lX\n",
               (unsigned long)task->context.user.pc,
               (unsigned long)task->context.user.sp,
               (unsigned long)task->context.user.x[29],
               (unsigned long)task->context.user.x[30]);
    }
    arch_disable_interrupts();
    for (;;)
        arch_wait_for_interrupt();
}

static void dispatch_syscall(arm64_exception_frame_t *frame)
{
    syscall_request_t request;
    task_t *task = task_current_local();
    syscall_result_t result;
    uint32_t index;

    if (task) {
        task->context.user = frame->user;
        arch_task_context_set_returns_to_user(&task->context, true);
    }
    request.number = (uint32_t)frame->user.x[8];
    for (index = 0; index < ARMOS_SYSCALL_ARGUMENT_COUNT; index++)
        request.arguments[index] = (syscall_word_t)frame->user.x[index];
    result = syscall_dispatch_common_request(&request);

    /*
     * The common dispatcher owns the canonical user context. It may install a
     * signal frame, restore rt_sigreturn state, or yield before returning.
     * Always restore that final context into the architectural exception frame
     * instead of overwriting it with the stale syscall-entry registers.
     */
    task = task_current_local();
    if (task && task->type == TASK_TYPE_PROCESS) {
        frame->user = task->context.user;
        arch_task_context_set_returns_to_user(&task->context, true);
    } else {
        frame->user.x[0] = (uint64_t)result;
    }
}

static bool dispatch_user_page_fault(arm64_exception_frame_t *frame,
                                     uint64_t ec, uint64_t iss)
{
    task_t *task = task_current_local();
    process_t *process = task_get_process(task);
    uint32_t status = (uint32_t)(iss & 0x3fu);
    bool translation = status >= 4u && status <= 7u;
    bool permission = status >= 12u && status <= 15u;
    bool write = ec == ESR_EC_DATA_ABORT_LOWER && (iss & (1u << 6)) != 0;

    if (translation) {
        if (handle_user_stack_fault((vaddr_t)frame->far) == 0)
            return true;
        if (handle_lazy_anon_fault((vaddr_t)frame->far, write) == 0)
            return true;
    }
    if (permission && write) {
        int result = handle_cow_fault((vaddr_t)frame->far);

        if (result == 0) {
            if (task) {
                task->page_faults++;
                task->cow_faults++;
            }
            return true;
        }
        KERROR("ARM64 COW fault unresolved: pid=%d address=0x%lX result=%d\n",
               process ? process->pid : -1,
               (unsigned long)frame->far, result);
    }
    return false;
}

static bool task_state_requires_reschedule(const task_t *task)
{
    if (!task)
        return false;

    return task->state == TASK_BLOCKED ||
           task->state == TASK_INTERRUPTIBLE ||
           task->state == TASK_UNINTERRUPTIBLE ||
           task->state == TASK_STOPPED ||
           task->state == TASK_ZOMBIE ||
           task->state == TASK_TERMINATED;
}

static void dispatch_irq_return_to_user(arm64_exception_frame_t *frame)
{
    task_t *task = task_current_local();
    signal_check_result_t signal_result = SIGNAL_CHECK_NONE;
    uint32_t cpu = smp_processor_id();

    if (!task || task->type != TASK_TYPE_PROCESS || !task->process ||
        !smp_scheduler_cpu_enabled(cpu) || get_critical_section())
        return;
    if (!scheduler_resched_pending_on_cpu(cpu) &&
        !has_pending_signals(task))
        return;

    /*
     * The exception frame lives on this task's kernel stack, so it remains
     * valid while the common scheduler runs another task. Preserve the EL0
     * image in the canonical task context before making the task switchable.
     */
    task->context.user = frame->user;
    arch_task_context_set_returns_to_user(&task->context, true);

    arch_enable_interrupts();
    if (has_pending_signals(task)) {
        signal_result = check_pending_signals();
        task = task_current_local();
    }
    signal_consume_user_return_override();

    task = task_current_local();
    if (task &&
        (signal_result == SIGNAL_CHECK_EXITED ||
         signal_result == SIGNAL_CHECK_STOPPED ||
         task_state_requires_reschedule(task))) {
        yield();
    } else if (scheduler_take_resched_current_cpu()) {
        yield();
    }

    /* vectors.S restores the final EL0 image directly from this frame. */
    arch_disable_interrupts();
    task = task_current_local();
    if (task && task->type == TASK_TYPE_PROCESS)
        frame->user = task->context.user;
}

void arm64_exception_dispatch(arm64_exception_frame_t *frame)
{
    uint64_t ec = (frame->esr >> ESR_EC_SHIFT) & ESR_EC_MASK;
    uint64_t iss = frame->esr & 0x01ffffffu;

    if (frame->vector == ARM64_VECTOR_IRQ_SPX ||
        frame->vector == ARM64_VECTOR_IRQ_LOWER) {
        arm64_exception_depth_enter();
        timer_accounting_irq_enter(frame->vector == ARM64_VECTOR_IRQ_LOWER);
        irq_c_handler();
        timer_accounting_irq_exit();
        arm64_exception_depth_leave();
        if (frame->vector == ARM64_VECTOR_IRQ_LOWER)
            dispatch_irq_return_to_user(frame);
        return;
    }

    if (frame->vector == ARM64_VECTOR_SYNC_LOWER && ec == ESR_EC_SVC64 &&
        (iss & 0xffffu) == 0) {
        dispatch_syscall(frame);
        return;
    }

    if (frame->vector == ARM64_VECTOR_SYNC_LOWER &&
        (ec == ESR_EC_DATA_ABORT_LOWER || ec == ESR_EC_INSN_ABORT_LOWER) &&
        dispatch_user_page_fault(frame, ec, iss)) {
        return;
    }

    halt_exception(frame);
}
