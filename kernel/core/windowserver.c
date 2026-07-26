/*
 * ArmOS
 * Copyright (c) 2026 Mohamed Ennassiri
 *
 * Licensed under the Apache License, Version 2.0.
 * See LICENSE for details.
 *
 * File: kernel/core/windowserver.c
 * Layer: Kernel / service bootstrap
 *
 * Responsibilities:
 * - Run the kernel thread which starts the privileged userland compositor.
 * - Establish the minimal environment and credentials needed by the service.
 * - Report an exec failure without embedding compositor policy in the kernel.
 *
 * Notes:
 * - Display drivers and architecture code do not depend on this launcher.
 * - The process transitions to a normal user address space through execve(2).
 */

#include <kernel/windowserver.h>
#include <kernel/task.h>
#include <kernel/process.h>
#include <kernel/syscalls.h>
#include <kernel/string.h>
#include <kernel/kprintf.h>
#include <kernel/arch_memory.h>

static task_t *windowserverd_task;
static task_t *compositor_task;

static void armos_compositor_main(void *argument)
{
    const char *path = "/sbin/armos-wlcomp";
    char *name = "armos-compositor";
    char *quiet = "--quiet";
    char *const argv[] = {name, quiet, NULL};
    char *const envp[] = {
        "PATH=/sbin:/bin:/usr/bin",
        "HOME=/root",
        "USER=root",
        "WAYLAND_DISPLAY=wayland-0",
        NULL
    };
    int result;

    (void)argument;
    result = sys_execve(path, argv, envp);
    KERROR("WindowServer: exec %s failed with %d\n", path, result);
    sys_exit(result);
}

static int windowserverd_launch_compositor(void)
{
    compositor_task = task_create_process(
        "armos-compositor", armos_compositor_main, NULL,
        TASK_DEFAULT_PRIORITY, TASK_TYPE_PROCESS);
    if (!compositor_task)
        return -1;

    arch_task_context_mark_first_run(&compositor_task->context);
    arch_task_context_set_returns_to_user(&compositor_task->context, false);
    compositor_task->process->uid = 0;
    compositor_task->process->gid = 0;
    strcpy(compositor_task->process->cwd, "/tmp");
    add_to_ready_queue(compositor_task);
    return 0;
}

static void windowserverd_main(void *argument)
{
    (void)argument;

    for (;;) {
        task_t *exited = NULL;
        unsigned long flags;

        if (!compositor_task) {
            if (windowserverd_launch_compositor() < 0)
                KERROR("WindowServer: cannot create userland compositor\n");
            task_sleep_ms(1000);
            continue;
        }

        spin_lock_irqsave(&task_lock, &flags);
        if (compositor_task->state == TASK_ZOMBIE &&
            compositor_task->process &&
            compositor_task->process->state == (proc_state_t)PROC_ZOMBIE &&
            compositor_task->running_cpu == TASK_CPU_NONE &&
            compositor_task->wakeup_time == 0) {
            exited = compositor_task;
            compositor_task = NULL;
        }
        spin_unlock_irqrestore(&task_lock, flags);

        if (exited) {
            task_set_terminated(exited);
            kernel_lifecycle_stats.zombies_reaped++;
            destroy_process(exited);
            KWARN("WindowServer: compositor stopped; restarting\n");
        }
        task_sleep_ms(1000);
    }
}

int windowserverd_start(void)
{
    if (windowserverd_task)
        return 0;

    windowserverd_task = task_create_process(
        "windowserverd", windowserverd_main, NULL,
        TASK_DEFAULT_PRIORITY, TASK_TYPE_KERNEL);
    if (!windowserverd_task)
        return -1;

    arch_task_context_mark_first_run(&windowserverd_task->context);
    arch_task_context_set_address_space(
        &windowserverd_task->context,
        arch_kernel_address_space_context(), ASID_KERNEL);
    arch_task_context_set_returns_to_user(&windowserverd_task->context, false);
    add_to_ready_queue(windowserverd_task);
    return 0;
}
