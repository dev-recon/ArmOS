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
 * - TLS and futex wake-on-clear are intentionally deferred to the next layer.
 */

#include <kernel/memory.h>
#include <kernel/task.h>
#include <kernel/syscalls.h>
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
    if (args.tls != 0)
        return -ENOTSUP;

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

    if (!thread || thread->type != TASK_TYPE_THREAD)
        task_exit_current_thread(status);

    if (thread->clear_child_tid) {
        uint32_t zero = 0;

        (void)copy_to_user((void *)(uintptr_t)thread->clear_child_tid,
                           &zero, sizeof(zero));
        thread->clear_child_tid = 0;
    }

    task_exit_current_thread(status);
}
