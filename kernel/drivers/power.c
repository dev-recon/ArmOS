/*
 * ArmOS
 * Copyright (c) 2026 Mohamed Ennassiri
 *
 * Licensed under the Apache License, Version 2.0.
 * See LICENSE for details.
 *
 * File: kernel/drivers/power.c
 * Layer: Kernel / device drivers
 *
 * Responsibilities:
 * - Expose hardware or pseudo-device services to the kernel and VFS.
 * - Translate device-specific state into stable kernel interfaces.
 *
 * Notes:
 * - Driver failures should degrade without hiding tty0 diagnostics.
 */

#include <kernel/power.h>
#include <kernel/kprintf.h>
#include <kernel/memory.h>
#include <kernel/types.h>
#include <kernel/task.h>
#include <kernel/process.h>
#include <kernel/platform_devices.h>
#include <kernel/vfs.h>
#include <kernel/signal.h>
#include <kernel/timer.h>
#include <kernel/smp.h>
#include <kernel/arch_cpu.h>
#include <kernel/arch_power.h>

#define SHUTDOWN_TERM_GRACE_MS 200
#define SHUTDOWN_KILL_GRACE_MS 100
#define SHUTDOWN_SMP_PARK_GRACE_MS 1000

static volatile bool shutdown_in_progress = false;

static bool shutdown_process_target(task_t *task)
{
    if (!task || task == task_current_local())
        return false;
    if (task->type != TASK_TYPE_PROCESS || !task->process)
        return false;
    if (task->process->pid == 1)
        return false;
    if (task->state == TASK_ZOMBIE || task->state == TASK_TERMINATED)
        return false;

    return true;
}

static bool shutdown_signal_target(task_t *task)
{
    if (!shutdown_process_target(task))
        return false;

    /*
     * PID 1 is notified by /sbin/shutdown before entering the kernel poweroff
     * path. Once init is in shutdown mode, login shells must receive SIGTERM so
     * they can persist userland state such as command history before VFS sync.
     */
    return true;
}

static unsigned shutdown_collect_targets(task_t **targets, unsigned max_targets)
{
    task_t *task;
    unsigned found = 0;
    unsigned walked = 0;
    unsigned long flags;

    if (!targets || max_targets == 0)
        return 0;

    spin_lock_irqsave(&task_lock, &flags);
    task = task_list_head;
    if (!task) {
        spin_unlock_irqrestore(&task_lock, flags);
        return 0;
    }

    do {
        if (shutdown_signal_target(task) && found < max_targets)
            targets[found++] = task;

        task = task->next;
        walked++;
    } while (task && task != task_list_head && walked < MAX_TASKS);

    spin_unlock_irqrestore(&task_lock, flags);
    return found;
}

static unsigned shutdown_count_live_targets(void)
{
    task_t *task;
    unsigned found = 0;
    unsigned walked = 0;
    unsigned long flags;

    spin_lock_irqsave(&task_lock, &flags);
    task = task_list_head;
    if (task) {
        do {
            if (shutdown_signal_target(task))
                found++;
            task = task->next;
            walked++;
        } while (task && task != task_list_head && walked < MAX_TASKS);
    }
    spin_unlock_irqrestore(&task_lock, flags);
    return found;
}

static unsigned shutdown_signal_targets(int sig)
{
    task_t **targets;
    unsigned target_count;
    unsigned delivered = 0;
    unsigned i;

    targets = kmalloc(sizeof(*targets) * MAX_TASKS);
    if (!targets)
        return 0;
    target_count = shutdown_collect_targets(targets, MAX_TASKS);

    for (i = 0; i < target_count; i++) {
        if (send_signal(targets[i], sig) == 0)
            delivered++;
    }

    kfree(targets);
    return delivered;
}

static void shutdown_wait_for_targets(const char *phase, uint32_t grace_ms)
{
    uint32_t deadline = get_system_ticks() + (grace_ms * TIMER_FREQ + 999) / 1000;
    unsigned live;

    do {
        live = shutdown_count_live_targets();
        if (live == 0) {
            kprintf("Shutdown: %s complete\n", phase);
            return;
        }
        task_sleep_ms(10);
    } while (get_system_ticks() < deadline);

    kprintf("Shutdown: %s timeout, %u process(es) still alive\n",
            phase, live);
}

static unsigned shutdown_force_terminate_targets(void)
{
    task_t **targets;
    unsigned target_count;
    unsigned stopped = 0;
    unsigned i;

    targets = kmalloc(sizeof(*targets) * MAX_TASKS);
    if (!targets)
        return 0;
    target_count = shutdown_collect_targets(targets, MAX_TASKS);

    for (i = 0; i < target_count; i++) {
        task_t *task = targets[i];
        if (shutdown_signal_target(task)) {
            close_all_process_files(task);
            remove_from_ready_queue(task);
            task_set_terminated(task);
            stopped++;
        }
    }

    kfree(targets);
    return stopped;
}

static void shutdown_processes(void)
{
    unsigned delivered;
    unsigned forced;

    kprintf("Shutdown: sending SIGTERM to user processes\n");
    delivered = shutdown_signal_targets(SIGTERM);
    kprintf("Shutdown: SIGTERM delivered to %u process(es)\n", delivered);
    shutdown_wait_for_targets("SIGTERM grace", SHUTDOWN_TERM_GRACE_MS);

    if (shutdown_count_live_targets() > 0) {
        kprintf("Shutdown: sending SIGKILL to remaining processes\n");
        delivered = shutdown_signal_targets(SIGKILL);
        kprintf("Shutdown: SIGKILL delivered to %u process(es)\n", delivered);
        shutdown_wait_for_targets("SIGKILL grace", SHUTDOWN_KILL_GRACE_MS);
    }

    forced = shutdown_force_terminate_targets();
    if (forced)
        kprintf("Shutdown: force-terminated %u process(es)\n", forced);

    task_t *task = task_current_local();
    if (task && task->type == TASK_TYPE_PROCESS && task->process) {
        close_all_process_files(task);
    }
}

static void shutdown_drivers(void)
{
    if (vfs_shutdown() < 0)
        KERROR("Shutdown: VFS shutdown completed with errors\n");

    kprintf("Shutdown: flushing and stopping block device\n");
    platform_block_shutdown();
    kprintf("Shutdown: block device stopped\n");
}

static void shutdown_park_secondary_cpus(void)
{
    uint32_t deadline;

    if (smp_possible_cpu_count() <= 1)
        return;

    kprintf("Shutdown: parking secondary CPUs\n");
    smp_request_shutdown_park_secondary_cpus();

    deadline = get_system_ticks() +
               (SHUTDOWN_SMP_PARK_GRACE_MS * TIMER_FREQ + 999) / 1000;
    while (!smp_shutdown_secondary_cpus_parked() &&
           get_system_ticks() < deadline) {
        task_sleep_ms(10);
    }

    if (!smp_shutdown_secondary_cpus_parked())
        KERROR("Shutdown: secondary CPU park timeout\n");
    else
        kprintf("Shutdown: secondary CPUs parked\n");
}

void kernel_poweroff(void)
{
    if (shutdown_in_progress) {
        arch_system_off();
    }

    shutdown_in_progress = true;
    kprintf("System shutdown requested\n");

    shutdown_processes();
    shutdown_park_secondary_cpus();
    shutdown_drivers();
    arch_disable_interrupts();
    kprintf("Shutdown: interrupts disabled\n");
    kprintf("Shutdown: entering platform poweroff\n");
    arch_system_off();
    __builtin_unreachable();
}

int sys_shutdown(void)
{
    kernel_poweroff();
}
