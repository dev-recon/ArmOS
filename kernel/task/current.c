/*
 * ArmOS
 * Copyright (c) 2026 Mohamed Ennassiri
 *
 * Licensed under the Apache License, Version 2.0.
 * See LICENSE for details.
 *
 * File: kernel/task/current.c
 * Layer: Kernel / task identity
 *
 * Responsibilities:
 * - Publish the current task through the architecture CPU-local register.
 * - Maintain per-CPU and boot-CPU mirrors for diagnostics and early fallback.
 * - Provide the architecture-neutral current-task lookup used by syscalls.
 * - Route short interruptible waits to the active scheduler implementation.
 *
 * Notes:
 * - Scheduling policy and context switching remain outside this module.
 */

#include <kernel/smp.h>
#include <kernel/task.h>

task_t* current_task = NULL;
task_t* current_tasks[ARMOS_MAX_CPUS];
static task_poll_wait_handler_t poll_wait_handler;
static void *poll_wait_owner;

process_t* task_get_process(task_t* task)
{
    if (!task || (task->type != TASK_TYPE_PROCESS &&
                  task->type != TASK_TYPE_THREAD))
        return NULL;
    return task->process;
}

task_t* task_get_process_leader(task_t* task)
{
    process_t* process = task_get_process(task);

    return process ? process->leader : NULL;
}

bool task_is_user_context(task_t* task)
{
    return task_get_process(task) != NULL;
}

int task_register_poll_wait_handler(task_poll_wait_handler_t handler,
                                    void *owner)
{
    if (!handler)
        return -EINVAL;
    poll_wait_handler = handler;
    poll_wait_owner = owner;
    return 0;
}

int task_poll_wait_once(void)
{
    return poll_wait_handler ? poll_wait_handler(poll_wait_owner) : -ENOSYS;
}

uid_t current_uid(void)
{
    process_t* process = task_get_process(task_current_local());

    return process ? process->uid : 0;
}

gid_t current_gid(void)
{
    process_t* process = task_get_process(task_current_local());

    return process ? process->gid : 0;
}

static bool current_task_header_valid(task_t* task)
{
    uintptr_t address = (uintptr_t)task;

    if (!task || (address & 7u) != 0 || task->magic != TASK_MAGIC_ALIVE)
        return false;
    if (task->state > TASK_STOPPED)
        return false;
    return task->type == TASK_TYPE_PROCESS ||
           task->type == TASK_TYPE_THREAD ||
           task->type == TASK_TYPE_KERNEL;
}

task_t* task_current_on_cpu(uint32_t cpu_id)
{
    task_t* task;

    if (cpu_id >= ARMOS_MAX_CPUS)
        return NULL;
    task = current_tasks[cpu_id];
    return current_task_header_valid(task) ? task : NULL;
}

task_t* task_current_local(void)
{
    task_t* task = (task_t*)arch_task_current_pointer();

    if (current_task_header_valid(task))
        return task;
    return task_current_on_cpu(smp_processor_id());
}

int task_current_publish(uint32_t cpu_id, task_t* task)
{
    if (cpu_id >= ARMOS_MAX_CPUS || !current_task_header_valid(task))
        return -EINVAL;

    current_tasks[cpu_id] = task;
    if (cpu_id == ARMOS_BOOT_CPU)
        current_task = task;
    arch_task_set_current_pointer((uintptr_t)task);
    return 0;
}

void task_current_clear_all(void)
{
    uint32_t cpu;

    arch_task_set_current_pointer(0);
    current_task = NULL;
    for (cpu = 0; cpu < ARMOS_MAX_CPUS; cpu++)
        current_tasks[cpu] = NULL;
}
