/*
 * ArmOS
 * Copyright (c) 2026 Mohamed Ennassiri
 *
 * Licensed under the Apache License, Version 2.0.
 * See LICENSE for details.
 *
 * File: kernel/syscalls/thread.c
 * Layer: Kernel / thread syscalls
 *
 * Responsibilities:
 * - Create schedulable user threads in an existing process address space.
 * - Publish native TIDs and terminate a single member thread safely on SMP.
 *
 * Notes:
 * - This is the native foundation for libpthread, not a complete Linux clone
 *   compatibility layer.
 * - The ABI exposes a native TLS base and Linux-like futex wait/wake semantics.
 */

#include <kernel/memory.h>
#include <kernel/task.h>
#include <kernel/syscalls.h>
#include <kernel/timer.h>
#include <kernel/userspace.h>

static bool clone_tid_pointer_valid(process_t* process, unsigned long address)
{
    return process && process->vm && address &&
           vm_validate_user_range(process->vm, (vaddr_t)address,
                                  sizeof(uint32_t), VMA_WRITE);
}

int sys_clone(const armos_clone_args_t* user_args, size_t args_size)
{
    armos_clone_args_t args;
    task_t* creator = task_current_local();
    process_t* process = task_get_process(creator);
    task_t* child;
    uint32_t tid;
    vaddr_t stack_top;
    vaddr_t stack_base;

    if (!creator || !process || !process->vm ||
        !arch_task_context_returns_to_user(&creator->context))
        return -EINVAL;
    if (!user_args)
        return -EFAULT;
    if (args_size < sizeof(args))
        return -EINVAL;
    if (copy_from_user(&args, user_args, sizeof(args)) < 0)
        return -EFAULT;
    if ((args.flags & ARMOS_CLONE_THREAD_REQUIRED) !=
        ARMOS_CLONE_THREAD_REQUIRED)
        return -EINVAL;
    if (args.flags & ~ARMOS_CLONE_THREAD_SUPPORTED)
        return -EINVAL;
    stack_top = (vaddr_t)args.stack;
    if (!args.entry || !stack_top || args.stack_size < PAGE_SIZE ||
        stack_top & (sizeof(uintptr_t) * 2u - 1u) ||
        (vaddr_t)args.stack_size >= stack_top)
        return -EINVAL;
    stack_base = stack_top - (vaddr_t)args.stack_size;
    if (!vm_validate_user_range(process->vm, stack_base,
                                (size_t)args.stack_size,
                                VMA_READ | VMA_WRITE) ||
        !vm_validate_user_range(process->vm, (vaddr_t)args.entry, 1u,
                                VMA_EXEC))
        return -EFAULT;
    if (args.tls &&
        !vm_validate_user_range(process->vm, (vaddr_t)args.tls, 1u,
                                VMA_READ | VMA_WRITE))
        return -EFAULT;

    if ((args.flags & ARMOS_CLONE_PARENT_SETTID) &&
        !clone_tid_pointer_valid(process, args.parent_tid))
        return -EFAULT;
    if ((args.flags & (ARMOS_CLONE_CHILD_SETTID |
                       ARMOS_CLONE_CHILD_CLEARTID)) &&
        !clone_tid_pointer_valid(process, args.child_tid))
        return -EFAULT;

    child = task_create_user_thread(
        creator,
        (vaddr_t)args.entry,
        (vaddr_t)args.argument,
        stack_top,
        (vaddr_t)args.tls,
        (args.flags & ARMOS_CLONE_CHILD_CLEARTID) ?
            (vaddr_t)args.child_tid : 0);
    if (!child)
        return -ENOMEM;

    tid = child->task_id;
    if ((args.flags & ARMOS_CLONE_PARENT_SETTID) &&
        copy_to_user((void *)(uintptr_t)args.parent_tid,
                     &tid, sizeof(tid)) < 0)
        goto fail;
    if ((args.flags & ARMOS_CLONE_CHILD_SETTID) &&
        copy_to_user((void *)(uintptr_t)args.child_tid,
                     &tid, sizeof(tid)) < 0)
        goto fail;

    task_publish_user_thread(child);
    return (int)tid;

fail:
    task_abort_user_thread(child);
    return -EFAULT;
}

void sys_thread_exit(int status)
{
    task_t* thread = task_current_local();
    process_t* process = task_get_process(thread);

    if (!thread || thread->type != TASK_TYPE_THREAD)
        task_exit_current_thread(status);

    if (thread->clear_child_tid) {
        uint32_t zero = 0;

        (void)copy_to_user((void *)(uintptr_t)thread->clear_child_tid,
                           &zero, sizeof(zero));
        (void)task_futex_wake(process, thread->clear_child_tid, 1u);
        thread->clear_child_tid = 0;
    }

    task_exit_current_thread(status);
}

static int futex_timeout_deadline(const armos_timespec_t* user_timeout,
                                  uint32_t* deadline)
{
    armos_timespec_t timeout;
    uint32_t ticks;
    uint32_t subsecond_ticks;
    const uint32_t max_ticks = 0x7fffffffu;

    if (!deadline)
        return -EINVAL;
    *deadline = 0;
    if (!user_timeout)
        return 0;
    if (copy_from_user(&timeout, user_timeout, sizeof(timeout)) < 0)
        return -EFAULT;
    if (timeout.sec < 0 || timeout.nsec < 0 ||
        timeout.nsec >= 1000000000LL)
        return -EINVAL;

    if ((uint64_t)timeout.sec > max_ticks / TIMER_FREQ) {
        ticks = max_ticks;
    } else {
        ticks = (uint32_t)timeout.sec * TIMER_FREQ;
        subsecond_ticks =
            ((uint32_t)timeout.nsec + (1000000000u / TIMER_FREQ) - 1u) /
            (1000000000u / TIMER_FREQ);
        if (subsecond_ticks > max_ticks - ticks)
            ticks = max_ticks;
        else
            ticks += subsecond_ticks;
    }
    if (ticks == 0)
        ticks = 1;
    *deadline = get_system_ticks() + ticks;
    return 0;
}

int sys_futex(uint32_t* address, int operation, uint32_t value,
              const armos_timespec_t* timeout)
{
    task_t* task = task_current_local();
    process_t* process = task_get_process(task);
    uint32_t current;
    uint32_t deadline;
    int result;

    if (!process || !process->vm || !address ||
        ((uintptr_t)address & (sizeof(uint32_t) - 1u)) != 0)
        return -EINVAL;
    if (!vm_validate_user_range(process->vm, (vaddr_t)(uintptr_t)address,
                                sizeof(*address), VMA_READ | VMA_WRITE))
        return -EFAULT;

    if (operation == ARMOS_FUTEX_WAKE)
        return (int)task_futex_wake(process,
                                    (vaddr_t)(uintptr_t)address, value);
    if (operation != ARMOS_FUTEX_WAIT)
        return -EINVAL;
    if (copy_from_user(&current, address, sizeof(current)) < 0)
        return -EFAULT;
    if (current != value)
        return -EAGAIN;

    result = futex_timeout_deadline(timeout, &deadline);
    if (result < 0)
        return result;
    return task_futex_wait(task, (vaddr_t)(uintptr_t)address,
                           value, deadline);
}

int sys_set_tls(unsigned long tls_base)
{
    task_t* task = task_current_local();
    process_t* process = task_get_process(task);

    if (!task || !process || !process->vm ||
        !arch_task_context_returns_to_user(&task->context))
        return -EINVAL;
    if (tls_base &&
        !vm_validate_user_range(process->vm, (vaddr_t)tls_base, 1u,
                                VMA_READ | VMA_WRITE))
        return -EFAULT;

    arch_task_context_set_tls(&task->context, (uintptr_t)tls_base);
    arch_task_set_user_tls_current((uintptr_t)tls_base);
    return 0;
}

int sys_get_tls_info(armos_tls_info_t* user_info)
{
    task_t* task = task_current_local();
    process_t* process = task_get_process(task);
    armos_tls_info_t info;

    if (!process || !process->vm)
        return -EINVAL;
    if (!user_info)
        return -EFAULT;

    info.image = (unsigned long)process->tls_image;
    info.file_size = (unsigned long)process->tls_file_size;
    info.memory_size = (unsigned long)process->tls_memory_size;
    info.alignment = (unsigned long)process->tls_alignment;
    return copy_to_user(user_info, &info, sizeof(info)) < 0 ?
        -EFAULT : 0;
}
