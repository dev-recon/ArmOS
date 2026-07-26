/*
 * ArmOS
 * Copyright (c) 2026 Mohamed Ennassiri
 *
 * Licensed under the Apache License, Version 2.0.
 * See LICENSE for details.
 *
 * File: kernel/task/task.c
 * Layer: Kernel / scheduler and tasking
 *
 * Responsibilities:
 * - Create, schedule, block, wake, and destroy tasks.
 * - Track scheduling and lifecycle diagnostics.
 *
 * Notes:
 * - Scheduler invariants are shared with timer preemption and wait paths.
 */

#include <kernel/task.h>
#include <kernel/memory.h>
#include <kernel/address_space.h>
#include <kernel/fd.h>
#include <kernel/panic.h>
#include <kernel/kprintf.h>
#include <kernel/stddef.h>
#include <kernel/string.h>
#include <kernel/uart.h>
#include <kernel/tty.h>
#include <kernel/syscalls.h>
#include <kernel/signal.h>
#include <kernel/timer.h>
#include <kernel/process.h>
#include <kernel/smp.h>
#include <kernel/arch_barrier.h>
#include <kernel/arch_cpu.h>
#include <kernel/arch_memory.h>
#include <kernel/file.h>

//const uint32_t TASK_CONTEXT_OFF = offsetof(task_t, context);

/* Variables globales du scheduler */
task_t* task_list_head = NULL;
typedef struct runqueue {
    task_t* head[TASK_PRIORITY_LEVELS];
    task_t* tail[TASK_PRIORITY_LEVELS];
    uint32_t count[TASK_PRIORITY_LEVELS];
    uint32_t nr_running;
} runqueue_t;

static runqueue_t ready_queue;
static uint32_t next_task_id = 1;
static uint32_t next_pid = 1;
uint32_t task_count = 0;
static bool scheduler_initialized = false;
DEFINE_SPINLOCK(task_lock);

static bool task_header_plausible(task_t* task);
static uint32_t task_reserve_task_id(void);
static uint32_t task_reserve_pid(void);

static inline vaddr_t task_stack_addr(const void* ptr)
{
    return (vaddr_t)(uintptr_t)ptr;
}

#define TASK_ADDR(value) ((unsigned long)(uintptr_t)(value))

static volatile int legacy_need_resched = 0;
static volatile int need_resched_cpu[ARMOS_MAX_CPUS];
static volatile uint32_t idle_work_seen_count[ARMOS_MAX_CPUS];
static volatile uint32_t idle_schedule_count[ARMOS_MAX_CPUS];
static volatile uint32_t idle_fallback_count[ARMOS_MAX_CPUS];
static volatile uint32_t secondary_scheduler_done_mask;
static volatile uint32_t secondary_scheduler_warned_mask;

//static spinlock_t task_lock = {0};

static uint32_t task_reserve_task_id(void)
{
    unsigned long flags;
    uint32_t id;

    spin_lock_irqsave(&task_lock, &flags);
    id = next_task_id++;
    spin_unlock_irqrestore(&task_lock, flags);
    return id;
}

static uint32_t task_reserve_pid(void)
{
    unsigned long flags;
    uint32_t pid;

    spin_lock_irqsave(&task_lock, &flags);
    pid = next_pid++;
    spin_unlock_irqrestore(&task_lock, flags);
    return pid;
}

void scheduler_request_resched_current_cpu(void)
{
    uint32_t cpu = smp_processor_id();

    if (cpu < ARMOS_MAX_CPUS)
        need_resched_cpu[cpu] = 1;

    /*
     * Keep the legacy global mirror alive for CPU0-only paths while the syscall
     * path is being migrated. Secondary CPUs must rely on their per-CPU flag or
     * they can consume another CPU's reschedule request.
     */
    if (smp_is_boot_cpu())
        legacy_need_resched = 1;
}

bool scheduler_take_resched_current_cpu(void)
{
    uint32_t cpu = smp_processor_id();
    bool requested = false;

    if (cpu < ARMOS_MAX_CPUS && need_resched_cpu[cpu]) {
        need_resched_cpu[cpu] = 0;
        requested = true;
    }

    if (smp_is_boot_cpu() && legacy_need_resched) {
        legacy_need_resched = 0;
        requested = true;
    }

    return requested;
}

bool scheduler_resched_pending_on_cpu(uint32_t cpu_id)
{
    if (cpu_id >= ARMOS_MAX_CPUS)
        return false;

    return need_resched_cpu[cpu_id] != 0;
}

uint32_t scheduler_idle_work_seen_count(uint32_t cpu_id)
{
    if (cpu_id >= ARMOS_MAX_CPUS)
        return 0;
    return idle_work_seen_count[cpu_id];
}

uint32_t scheduler_idle_schedule_count(uint32_t cpu_id)
{
    if (cpu_id >= ARMOS_MAX_CPUS)
        return 0;
    return idle_schedule_count[cpu_id];
}

uint32_t scheduler_idle_fallback_count(uint32_t cpu_id)
{
    if (cpu_id >= ARMOS_MAX_CPUS)
        return 0;
    return idle_fallback_count[cpu_id];
}

/* Tache idle et processus init */
task_t* idle_task = NULL;
task_t* idle_tasks[ARMOS_MAX_CPUS] = { NULL };
task_t* init_process = NULL;

task_t* task_idle_on_cpu(uint32_t cpu_id)
{
    if (cpu_id >= ARMOS_MAX_CPUS)
        return NULL;

    return idle_tasks[cpu_id];
}

bool task_is_idle_task(task_t* task)
{
    if (!task)
        return false;

    for (uint32_t cpu = 0; cpu < ARMOS_MAX_CPUS; cpu++) {
        if (idle_tasks[cpu] == task)
            return true;
    }

    return task == idle_task;
}

void task_register_idle_cpu(uint32_t cpu_id, task_t* task)
{
    if (cpu_id >= ARMOS_MAX_CPUS)
        return;

    idle_tasks[cpu_id] = task;
    if (cpu_id == smp_boot_cpu_id())
        idle_task = task;
}
volatile kernel_lifecycle_stats_t kernel_lifecycle_stats;
static spinlock_t sched_trace_lock = SPINLOCK_INIT("sched_trace");
static sched_trace_event_t sched_trace[SCHED_TRACE_SIZE];
static uint32_t sched_trace_seq;
static uint32_t sched_aging_selections;
static uint32_t sched_debt_selections;
static uint32_t sched_last_pick_reason;
static uint32_t sched_last_pick_tid;
static uint32_t sched_last_pick_pid;
static uint32_t sched_last_pick_priority;
static uint32_t sched_last_pick_effective_priority;
static uint32_t sched_last_pick_debt;
static uint32_t sched_last_pick_waited_ticks;
static uint32_t sched_last_scan_tasks;
//static int yield_count = 0;

/* === NOUVELLES VARIABLES POUR PROCESSUS === */
//task_t* current_process = NULL;  /* Alias vers current_task */

/* Forward declarations */
void idle_task_func(void* arg);
void init_process_main(void* arg);
static task_t* schedule_next_task(void);
static bool task_stack_metadata_valid(task_t* task);
static bool kstack_guard_valid(const task_t *task);
static bool kstack_guard_fully_valid(const task_t *task);
static bool task_is_schedulable(task_t* task);
static bool runqueue_contains_locked(task_t* task);
static bool runqueue_link_plausible_locked(task_t* task, uint32_t prio);
static bool runqueue_membership_valid_locked(task_t* task);
static void runqueue_reset_task_locked(task_t* task);
static void runqueue_append_raw_locked(task_t* task, uint32_t prio);
static void runqueue_enqueue_tail_locked(task_t* task);
static void runqueue_remove_locked(task_t* task);
static void runqueue_clear_locked(void);
static void runqueue_rebuild_locked(task_t* exclude);
static bool runqueue_validate_locked(const char* caller);
static bool scheduler_has_ready_work(void);
void task_make_ready_under_lock(task_t* task);
static void scheduler_mark_running_locked(task_t* task);
static void add_task_to_list_locked(task_t* task);
void add_task_to_list(task_t* task);
void remove_task_from_list(task_t* task);
void setup_task_context(task_t* task);
bool is_in_ready_queue(task_t* task);

uint64_t task_runtime_ticks(task_t* task)
{
    if (!task)
        return 0;

    return task->total_runtime;
}

static uint32_t task_trace_pid(task_t* task)
{
    return (task && task->type == TASK_TYPE_PROCESS && task->process)
        ? (uint32_t)task->process->pid
        : 0;
}

static bool task_pointer_plausible(task_t* task)
{
    uintptr_t p = (uintptr_t)task;

    if (!task)
        return false;
    if (p & 7u)
        return false;
    if (p < physical_ram_start())
        return false;
    if (p > (uintptr_t)physical_ram_end() - sizeof(task_t))
        return false;
    return true;
}

static bool task_header_plausible(task_t* task)
{
    if (!task_pointer_plausible(task))
        return false;
    if (task->magic != TASK_MAGIC_ALIVE)
        return false;
    if (task->state > TASK_STOPPED)
        return false;
    if (task->type != TASK_TYPE_PROCESS &&
        task->type != TASK_TYPE_THREAD &&
        task->type != TASK_TYPE_KERNEL)
        return false;
    return true;
}

bool task_is_valid(task_t* task)
{
    return task_header_plausible(task);
}

void sched_trace_record(sched_trace_event_type_t event, task_t* task)
{
    unsigned long flags;
    sched_trace_event_t* dst;
    task_t* current = task_current_local();
    bool unsafe_task = event == SCHED_TRACE_REFUSE_BROKEN_LIST ||
                       event == SCHED_TRACE_REFUSE_NULL_NEXT ||
                       event == SCHED_TRACE_READY_REFUSE_CORRUPT;
    task_t* safe_task = (!unsafe_task && task_header_plausible(task)) ? task : NULL;
    task_t* safe_current = task_header_plausible(current) ? current : NULL;

    spin_lock_irqsave(&sched_trace_lock, &flags);
    dst = &sched_trace[sched_trace_seq % SCHED_TRACE_SIZE];
    memset(dst, 0, sizeof(*dst));
    dst->seq = sched_trace_seq + 1;
    dst->tick = get_system_ticks();
    dst->event = event;
    dst->syscall = safe_task ? safe_task->current_syscall : 0;
    dst->pid = task_trace_pid(safe_task);
    dst->tid = safe_task ? safe_task->task_id : 0;
    dst->state = safe_task ? (uint32_t)safe_task->state : 0xffffffffu;
    dst->wakeup_time = safe_task ? safe_task->wakeup_time : 0;
    dst->current_pid = task_trace_pid(safe_current);
    dst->current_tid = safe_current ? safe_current->task_id : 0;
    dst->current_syscall = safe_current ? safe_current->current_syscall : 0;
    dst->task_ptr = (uintptr_t)task;
    dst->next_ptr = safe_task ? (uintptr_t)safe_task->next : 0;
    dst->prev_ptr = safe_task ? (uintptr_t)safe_task->prev : 0;
    if (safe_task)
        strncpy(dst->name, safe_task->name, TASK_NAME_MAX - 1);
    if (safe_current)
        strncpy(dst->current_name, safe_current->name, TASK_NAME_MAX - 1);
    sched_trace_seq++;
    spin_unlock_irqrestore(&sched_trace_lock, flags);
}

void sched_trace_snapshot(sched_trace_event_t* out, uint32_t max,
                          uint32_t* total, uint32_t* written)
{
    unsigned long flags;
    uint32_t seq;
    uint32_t count;
    uint32_t start;

    if (total)
        *total = 0;
    if (written)
        *written = 0;
    if (!out || max == 0)
        return;

    spin_lock_irqsave(&sched_trace_lock, &flags);
    seq = sched_trace_seq;
    count = seq < SCHED_TRACE_SIZE ? seq : SCHED_TRACE_SIZE;
    if (count > max)
        count = max;
    start = seq >= count ? seq - count : 0;

    for (uint32_t i = 0; i < count; i++)
        out[i] = sched_trace[(start + i) % SCHED_TRACE_SIZE];

    spin_unlock_irqrestore(&sched_trace_lock, flags);

    if (total)
        *total = seq;
    if (written)
        *written = count;
}

void scheduler_get_stats(scheduler_stats_t* stats)
{
    unsigned long flags;
    task_t* current;
    process_t* proc;
    uint32_t now;
    uint32_t total_debt = 0;
    uint32_t debt_count = 0;

    if (!stats)
        return;

    memset(stats, 0, sizeof(*stats));

    spin_lock_irqsave(&task_lock, &flags);
    current = task_current_local();

    stats->nr_running = ready_queue.nr_running;
    stats->policy_levels = TASK_PRIORITY_LEVELS;
    stats->default_priority = TASK_DEFAULT_PRIORITY;
    stats->idle_priority = TASK_IDLE_PRIORITY;
    stats->nice_min = TASK_NICE_MIN;
    stats->nice_max = TASK_NICE_MAX;
    stats->quantum_ticks = QUANTUM_TICKS;
    stats->aging_step_ticks = SCHED_AGING_STEP_TICKS;
    stats->aging_max_bonus = SCHED_AGING_MAX_BONUS;
    stats->aging_selections = sched_aging_selections;
    stats->debt_decay_ticks = SCHED_DEBT_DECAY_TICKS;
    stats->debt_selections = sched_debt_selections;
    stats->last_pick_reason = sched_last_pick_reason;
    stats->last_pick_tid = sched_last_pick_tid;
    stats->last_pick_pid = sched_last_pick_pid;
    stats->last_pick_priority = sched_last_pick_priority;
    stats->last_pick_effective_priority = sched_last_pick_effective_priority;
    stats->last_pick_debt = sched_last_pick_debt;
    stats->last_pick_waited_ticks = sched_last_pick_waited_ticks;
    stats->last_scan_tasks = sched_last_scan_tasks;
    stats->highest_ready_priority = TASK_PRIORITY_LEVELS;
    stats->lowest_ready_priority = TASK_PRIORITY_LEVELS;
    now = get_system_ticks();

    if (!runqueue_validate_locked("scheduler_get_stats")) {
        kernel_lifecycle_stats.scheduler_refused++;
        runqueue_rebuild_locked(NULL);
    }

    for (uint32_t prio = 0; prio < TASK_PRIORITY_LEVELS; prio++) {
        task_t* task;

        stats->priority_counts[prio] = ready_queue.count[prio];
        if (ready_queue.count[prio] > 0) {
            stats->nonempty_queues++;
            if (stats->highest_ready_priority == TASK_PRIORITY_LEVELS)
                stats->highest_ready_priority = prio;
            stats->lowest_ready_priority = prio;
        }

        task = ready_queue.head[prio];
        while (task && runqueue_link_plausible_locked(task, prio)) {
            uint32_t debt = task->sched_debt;

            if (task->ready_since_tick && now >= task->ready_since_tick) {
                uint32_t waited = now - task->ready_since_tick;
                uint32_t decay = waited / SCHED_DEBT_DECAY_TICKS;
                debt = decay >= debt ? 0 : debt - decay;
            }

            if (debt > stats->max_ready_debt)
                stats->max_ready_debt = debt;
            if (0xffffffffu - total_debt < debt)
                total_debt = 0xffffffffu;
            else
                total_debt += debt;
            debt_count++;
            task = task->rq_next;
        }
    }

    if (debt_count)
        stats->avg_ready_debt = (uint32_t)(total_debt / debt_count);

    if (current) {
        proc = (current->type == TASK_TYPE_PROCESS) ? current->process : NULL;
        stats->current_tid = current->task_id;
        stats->current_pid = proc ? (uint32_t)proc->pid : 0;
        stats->current_priority = current->priority;
        strncpy(stats->current_name, current->name, TASK_NAME_MAX - 1);
    }

    spin_unlock_irqrestore(&task_lock, flags);
}

void debug_context_switch_entry(void);
void debug_context_switch_middle(void);
void debug_context_switch_first_exec(void);
void debug_context_switch_restore(void);
void debug_null_old_ctx(void);
void debug_null_new_ctx(void);
void debug_task_detailed(task_t *current_task);

/* Fonctions assembleur externes */
extern void __task_first_switch_v2(task_context_t* new_ctx);
extern void __task_switch_asm_debug(task_context_t* old_ctx, task_context_t* new_ctx);
extern void __task_switch(task_context_t* old_ctx, task_context_t* new_ctx);
extern void print_system_stats(void);


static bool task_stack_metadata_valid(task_t* task)
{
    if (!task_header_plausible(task))
        return false;

    if (!task || !task->stack_base || !task->stack_top || task->stack_size == 0) {
        return false;
    }

    if (task->stack_top != (uint8_t*)task->stack_base + task->stack_size) {
        KERROR("TASK: %s stack metadata inconsistent base=%p top=%p size=%u\n",
               task->name, task->stack_base, task->stack_top, task->stack_size);
        return false;
    }

    if (!kstack_guard_valid(task)) {
        KERROR("TASK: %s kernel stack guard corrupted\n", task->name);
        return false;
    }

    vaddr_t stack_base = task_stack_addr(task->stack_base);
    vaddr_t stack_top = task_stack_addr(task->stack_top);
    vaddr_t sp = arch_task_context_kernel_sp(&task->context);

    if (sp < stack_base || sp >= stack_top) {
        KERROR("TASK: %s context SP 0x%lX outside stack 0x%lX-0x%lX\n",
               task->name, TASK_ADDR(sp), TASK_ADDR(stack_base),
               TASK_ADDR(stack_top));
        return false;
    }

    return true;
}

static bool task_kernel_sp_valid(task_t* task, vaddr_t sp)
{
    if (!task || !task->stack_base || !task->stack_top) {
        return false;
    }

    vaddr_t stack_base = task_stack_addr(task->stack_base);
    vaddr_t stack_top = task_stack_addr(task->stack_top);
    vaddr_t stack_sp = (vaddr_t)sp;

    return stack_sp >= stack_base && stack_sp < stack_top;
}

static bool task_is_schedulable(task_t* task)
{
    if (!task) {
        return false;
    }

    if (task->state != TASK_READY) {
        return false;
    }

    return task_stack_metadata_valid(task);
}

static bool runqueue_contains_locked(task_t* task)
{
    return task && task->rq_priority < TASK_PRIORITY_LEVELS;
}

static void scheduler_clear_nonrunnable_debt_locked(task_t* task, task_state_t state)
{
    if (!task)
        return;

    if (state == TASK_READY || state == TASK_RUNNING)
        return;

    /*
     * Debt is a fairness hint for runnable tasks only.  Once a task sleeps,
     * blocks, stops, or exits, forget the consumed-CPU penalty so long-lived
     * tools such as top do not keep fossilized debt while waiting for input or
     * a refresh timeout.
     */
    task->sched_debt = 0;
    task->ready_since_tick = 0;
}

static bool runqueue_link_plausible_locked(task_t* task, uint32_t prio)
{
    if (!task_header_plausible(task))
        return false;
    if (task->rq_priority != prio)
        return false;
    return true;
}

static uint32_t task_runqueue_priority(task_t* task)
{
    if (!task)
        return TASK_PRIORITY_LEVELS - 1;

    if (task->priority >= TASK_PRIORITY_LEVELS)
        return TASK_PRIORITY_LEVELS - 1;

    return task->priority;
}

static bool runqueue_membership_valid_locked(task_t* task)
{
    uint32_t prio;

    if (!task_header_plausible(task) || !runqueue_contains_locked(task))
        return false;

    prio = task->rq_priority;
    if (prio >= TASK_PRIORITY_LEVELS)
        return false;

    if (task->rq_prev &&
        (!runqueue_link_plausible_locked(task->rq_prev, prio) ||
         task->rq_prev->rq_next != task))
        return false;

    if (task->rq_next &&
        (!runqueue_link_plausible_locked(task->rq_next, prio) ||
         task->rq_next->rq_prev != task))
        return false;

    if (!task->rq_prev && ready_queue.head[prio] != task)
        return false;

    if (!task->rq_next && ready_queue.tail[prio] != task)
        return false;

    return true;
}

static void runqueue_reset_task_locked(task_t* task)
{
    if (!task_header_plausible(task))
        return;

    task->rq_next = NULL;
    task->rq_prev = NULL;
    task->rq_priority = TASK_PRIORITY_LEVELS;
    task->ready_since_tick = 0;
}

static void runqueue_append_raw_locked(task_t* task, uint32_t prio)
{
    if (!task_header_plausible(task) || prio >= TASK_PRIORITY_LEVELS)
        return;

    if (ready_queue.tail[prio] &&
        !runqueue_link_plausible_locked(ready_queue.tail[prio], prio)) {
        KERROR("runqueue_append_raw: corrupt tail %p at prio %u\n",
               ready_queue.tail[prio], prio);
        kernel_lifecycle_stats.ready_queue_refused++;
        return;
    }

    task->ready_since_tick = get_system_ticks();
    task->rq_next = NULL;
    task->rq_prev = ready_queue.tail[prio];
    task->rq_priority = prio;

    if (ready_queue.tail[prio])
        ready_queue.tail[prio]->rq_next = task;
    else
        ready_queue.head[prio] = task;

    ready_queue.tail[prio] = task;
    ready_queue.count[prio]++;
    ready_queue.nr_running++;
}

static void runqueue_enqueue_tail_locked(task_t* task)
{
    uint32_t prio;

    if (!task_header_plausible(task) || task_is_idle_task(task))
        return;

    if (runqueue_contains_locked(task)) {
        if (runqueue_membership_valid_locked(task))
            return;
        KERROR("runqueue_enqueue: stale membership for task %p (%s), rebuilding\n",
               task, task->name);
        kernel_lifecycle_stats.ready_queue_refused++;
        runqueue_reset_task_locked(task);
        runqueue_rebuild_locked(task);
    }

    prio = task_runqueue_priority(task);
    if (ready_queue.tail[prio] &&
        !runqueue_link_plausible_locked(ready_queue.tail[prio], prio)) {
        KERROR("runqueue_enqueue: corrupt tail %p at prio %u, rebuilding\n",
               ready_queue.tail[prio], prio);
        kernel_lifecycle_stats.ready_queue_refused++;
        runqueue_rebuild_locked(task);
    }

    if (ready_queue.tail[prio] &&
        !runqueue_link_plausible_locked(ready_queue.tail[prio], prio)) {
        KERROR("runqueue_enqueue: refusing enqueue after failed rebuild task=%p prio=%u tail=%p\n",
               task, prio, ready_queue.tail[prio]);
        kernel_lifecycle_stats.ready_queue_refused++;
        return;
    }

    runqueue_append_raw_locked(task, prio);
}

static void runqueue_remove_locked(task_t* task)
{
    uint32_t prio;

    if (!task_header_plausible(task) || !runqueue_contains_locked(task))
        return;

    prio = task->rq_priority;
    if (prio >= TASK_PRIORITY_LEVELS) {
        runqueue_reset_task_locked(task);
        return;
    }

    if ((task->rq_prev && (!runqueue_link_plausible_locked(task->rq_prev, prio) ||
                           task->rq_prev->rq_next != task)) ||
        (task->rq_next && (!runqueue_link_plausible_locked(task->rq_next, prio) ||
                           task->rq_next->rq_prev != task)) ||
        (!task->rq_prev && ready_queue.head[prio] != task) ||
        (!task->rq_next && ready_queue.tail[prio] != task)) {
        KERROR("runqueue_remove: corrupt links for task %p (%s) at prio %u, rebuilding\n",
               task, task->name, prio);
        kernel_lifecycle_stats.ready_queue_refused++;
        runqueue_reset_task_locked(task);
        runqueue_rebuild_locked(task);
        return;
    }

    if (task->rq_prev)
        task->rq_prev->rq_next = task->rq_next;
    else
        ready_queue.head[prio] = task->rq_next;

    if (task->rq_next)
        task->rq_next->rq_prev = task->rq_prev;
    else
        ready_queue.tail[prio] = task->rq_prev;

    runqueue_reset_task_locked(task);
    if (ready_queue.count[prio] > 0)
        ready_queue.count[prio]--;
    if (ready_queue.nr_running > 0)
        ready_queue.nr_running--;
}

static void runqueue_clear_locked(void)
{
    memset(&ready_queue, 0, sizeof(ready_queue));
}

static void runqueue_rebuild_locked(task_t* exclude)
{
    task_t* task;
    task_t* start;
    int count = 0;

    start = task_list_head;
    memset(&ready_queue, 0, sizeof(ready_queue));

    if (!task_header_plausible(start))
        return;

    task = start;
    do {
        task_t* next;

        if (!task_header_plausible(task))
            break;

        next = task->next;
        runqueue_reset_task_locked(task);

        if (task != exclude &&
            task->state == TASK_READY &&
            task->running_cpu == TASK_CPU_NONE &&
            !task_is_idle_task(task) &&
            task_stack_metadata_valid(task))
            runqueue_append_raw_locked(task, task_runqueue_priority(task));

        task = next;
        count++;
    } while (task != start && count < MAX_TASKS);
}

static bool runqueue_validate_locked(const char* caller)
{
    uint32_t total = 0;

    for (uint32_t prio = 0; prio < TASK_PRIORITY_LEVELS; prio++) {
        task_t* task = ready_queue.head[prio];
        task_t* prev = NULL;
        uint32_t count = 0;

        if (!task && ready_queue.tail[prio]) {
            KERROR("%s: runqueue prio %u has NULL head but tail=%p\n",
                   caller, prio, ready_queue.tail[prio]);
            return false;
        }

        while (task) {
            task_t* next;

            if (!runqueue_link_plausible_locked(task, prio)) {
                KERROR("%s: corrupt runqueue entry %p at prio %u head=%p tail=%p prev=%p\n",
                       caller, task, prio, ready_queue.head[prio],
                       ready_queue.tail[prio], prev);
                return false;
            }

            if (task->rq_prev != prev) {
                KERROR("%s: corrupt runqueue prev link task=%p (%s) prio=%u rq_prev=%p expected=%p\n",
                       caller, task, task->name, prio, task->rq_prev, prev);
                return false;
            }

            next = task->rq_next;
            if (!next && ready_queue.tail[prio] != task) {
                KERROR("%s: corrupt runqueue tail link task=%p (%s) prio=%u tail=%p\n",
                       caller, task, task->name, prio, ready_queue.tail[prio]);
                return false;
            }

            prev = task;
            task = next;
            count++;
            total++;

            if (count > MAX_TASKS || total > MAX_TASKS) {
                KERROR("%s: runqueue loop at prio %u count=%u total=%u\n",
                       caller, prio, count, total);
                return false;
            }
        }

        if (ready_queue.count[prio] != count) {
            KERROR("%s: runqueue count mismatch prio=%u stored=%u actual=%u\n",
                   caller, prio, ready_queue.count[prio], count);
            return false;
        }
    }

    if (ready_queue.nr_running != total) {
        KERROR("%s: runqueue total mismatch stored=%u actual=%u\n",
               caller, ready_queue.nr_running, total);
        return false;
    }

    return true;
}

void task_make_ready_under_lock(task_t* task)
{
    uint32_t cpu = smp_processor_id();

    if (!task_header_plausible(task)) {
        kernel_lifecycle_stats.ready_queue_refused++;
        sched_trace_record(SCHED_TRACE_READY_REFUSE_CORRUPT, task);
        return;
    }

    if (task->state == TASK_ZOMBIE || task->state == TASK_TERMINATED)
        return;

    /*
     * SMP handoff invariant:
     * a running task owns exactly one kernel stack. When the current CPU yields,
     * the task is queued as READY before __task_switch() saves the final SVC
     * frame, but running_cpu stays set until __task_switch() has moved to the
     * next task's stack and calls task_context_save_complete(). Other CPUs must
     * not be allowed to run it during that handoff window.
     */
    if (task->state == TASK_RUNNING && task->running_cpu != cpu) {
        kernel_lifecycle_stats.ready_queue_refused++;
        sched_trace_record(SCHED_TRACE_READY_REFUSE_REMOTE_RUNNING, task);
        return;
    }

    task->state = TASK_READY;
    if (task->type == TASK_TYPE_PROCESS && task->process)
        task->process->state = (proc_state_t)PROC_READY;

    runqueue_enqueue_tail_locked(task);
}

void task_context_save_complete(task_context_t* ctx)
{
    task_t* task;
    uint32_t cpu;
    unsigned long flags;

    if (!ctx)
        return;

    task = (task_t*)((char*)ctx - offsetof(task_t, context));
    cpu = smp_processor_id();

    if (!task_header_plausible(task))
        return;

    spin_lock_irqsave(&task_lock, &flags);
    if (task->state != TASK_RUNNING && task->running_cpu == cpu) {
        task->running_cpu = TASK_CPU_NONE;
    }
    spin_unlock_irqrestore(&task_lock, flags);
}

#define KERNEL_TASK_STACK_PAGES \
    ((KERNEL_TASK_STACK_SIZE + PAGE_SIZE - 1) / PAGE_SIZE)
#define KERNEL_TASK_STACK_GUARD_PAGES \
    ((KERNEL_TASK_STACK_GUARD_SIZE + PAGE_SIZE - 1) / PAGE_SIZE)
#define KERNEL_TASK_STACK_TOTAL_PAGES \
    (KERNEL_TASK_STACK_GUARD_PAGES + KERNEL_TASK_STACK_PAGES)
#define KERNEL_TASK_STACK_CANARY 0xA55A3CC3u

static void kstack_guard_initialize(void *guard)
{
    uint32_t *words = guard;
    size_t count = KERNEL_TASK_STACK_GUARD_SIZE / sizeof(*words);

    for (size_t i = 0; i < count; i++)
        words[i] = KERNEL_TASK_STACK_CANARY;
}

static bool kstack_guard_valid(const task_t *task)
{
    const uint32_t *words;
    size_t count;

    if (!task || !task->stack_base)
        return false;
    words = (const uint32_t *)((const uint8_t *)task->stack_base -
                              KERNEL_TASK_STACK_GUARD_SIZE);
    count = KERNEL_TASK_STACK_GUARD_SIZE / sizeof(*words);

    /*
     * Check the cache line immediately below the usable stack on every
     * scheduling validation. The complete guard is checked when reclaimed.
     */
    for (size_t i = count - 16u; i < count; i++) {
        if (words[i] != KERNEL_TASK_STACK_CANARY)
            return false;
    }
    return true;
}

static bool kstack_guard_fully_valid(const task_t *task)
{
    const uint32_t *words;
    size_t count;

    if (!task || !task->stack_base)
        return false;
    words = (const uint32_t *)((const uint8_t *)task->stack_base -
                              KERNEL_TASK_STACK_GUARD_SIZE);
    count = KERNEL_TASK_STACK_GUARD_SIZE / sizeof(*words);
    for (size_t i = 0; i < count; i++) {
        if (words[i] != KERNEL_TASK_STACK_CANARY)
            return false;
    }
    return true;
}

static void *kstack_alloc(void **out_phys)
{
    void *phys = allocate_pages(KERNEL_TASK_STACK_TOTAL_PAGES);
    void *virt;

    if (!phys)
        return NULL;

    virt = (void *)phys_to_virt((paddr_t)phys);
    kstack_guard_initialize(virt);
    virt = (uint8_t *)virt + KERNEL_TASK_STACK_GUARD_SIZE;
    memset(virt, 0, KERNEL_TASK_STACK_SIZE);
    if (out_phys)
        *out_phys = phys;
    return virt;
}

static void kstack_free(void *p)
{
    if (p)
        free_pages(p, KERNEL_TASK_STACK_TOTAL_PAGES);
}

static bool task_alloc_kernel_stack(task_t *task)
{
    if (!task)
        return false;

    task->stack_base = kstack_alloc(&task->stack_phys_base);
    if (!task->stack_base)
        return false;

    task->stack_size = KERNEL_TASK_STACK_SIZE;
    task->stack_top = (uint8_t *)task->stack_base + task->stack_size;
    kernel_lifecycle_stats.stack_pages_allocated +=
        KERNEL_TASK_STACK_TOTAL_PAGES;
    return true;
}

void task_free_kernel_stack(task_t *task)
{
    if (!task || !task->stack_base)
        return;

    if (!kstack_guard_fully_valid(task))
        KERROR("TASK: %s kernel stack guard corrupted\n", task->name);
    kstack_free(task->stack_phys_base);
    task->stack_base = NULL;
    task->stack_top = NULL;
    task->stack_phys_base = NULL;
    task->stack_size = 0;
    kernel_lifecycle_stats.stack_pages_freed +=
        KERNEL_TASK_STACK_TOTAL_PAGES;
}


// Construit une pile noyau "propre" pour un PROCESS (utile pour from_user=true)
static void build_clean_kernel_stack(task_t *t)
{
    if(!t->stack_base && !task_alloc_kernel_stack(t))
        return;

    vaddr_t stack_top = task_stack_addr(t->stack_top);
    arch_task_context_prepare_kernel_stack(&t->context, stack_top);
}


task_t* set_process_stack(task_t* parent, task_t* child, bool from_user)
{
    /* Allouer une nouvelle pile */
    if (!task_alloc_kernel_stack(child)) {
        KERROR("task_create_copy: Failed to allocate child stack\n");
        //kfree(child);
        return NULL;
    }

    //KDEBUG("task_create_copy: parent_is_user_process=%s\n", 
    //   from_user ? "YES" : "NO");
    //KDEBUG("  Parent SP: 0x%lX\n", parent->context.sp);
    //KDEBUG("  Parent Stack Base: 0x%lX\n", parent->stack_base);
    //KDEBUG("  Parent Stack Top : 0x%lX\n", parent->stack_top);


    if (from_user) {
        /* Pour les processus utilisateur, ne pas copier la stack kernel */
        /* Créer une stack kernel propre pour l'enfant */
        
        //KDEBUG("Creating clean kernel stack for user process child\n");
        
        /* Stack kernel propre */
        //memset(child->stack_base, 0, KERNEL_TASK_STACK_SIZE);
        //child->context.sp = (uint32_t)child->stack_top - 512;

        build_clean_kernel_stack(child);

        arch_task_context_set_returns_to_user(&child->context, true);
        
        /* IMPORTANT : Copier l'espace mémoire utilisateur séparément */
        /* Ceci sera fait dans la partie VM space copy */
        
        //KDEBUG("  Child kernel stack: %p - %p\n", 
        //    child->stack_base, child->stack_top);
        //KDEBUG("  Child kernel SP: 0x%lX\n", child->context.sp);
    
    } else {
        /*  COPIE DE PILE AVEC AJUSTEMENT SP */
        if (parent->stack_base && parent->stack_size > 0) {
            /*  Calculer l'offset DEPUIS LE HAUT de la pile parent */
            vaddr_t parent_stack_base = task_stack_addr(parent->stack_base);
            vaddr_t parent_stack_top = parent_stack_base + parent->stack_size;
            vaddr_t parent_sp = arch_task_context_kernel_sp(&parent->context);
            uint32_t parent_sp_offset_from_top = (uint32_t)(parent_stack_top - parent_sp);
            
            //KDEBUG("Parent stack analysis:\n");
            //KDEBUG("  Parent stack: %p - %p\n", parent->stack_base, (uint8_t*)parent->stack_base + parent->stack_size);
            //KDEBUG("  Parent SP: 0x%lX\n", parent->context.sp);
            //KDEBUG("  Offset from top: %u bytes\n", parent_sp_offset_from_top);
            
            /* Verifier que le SP parent est valide AVANT de copier */
            if (parent_sp < parent_stack_base || parent_sp >= parent_stack_top) {
                //KERROR("task_create_copy: Parent SP invalid! Cannot copy stack\n");
                //KERROR("  Parent SP: 0x%lX, Range: %p - 0x%lX\n",
                //    parent->context.sp, parent->stack_base, parent_stack_top);
                
                /* Utiliser pile propre au lieu d'echouer */
                memset(child->stack_base, 0, KERNEL_TASK_STACK_SIZE);
                vaddr_t child_stack_top = task_stack_addr(child->stack_top);
                arch_task_context_set_kernel_sp(&child->context,
                    arch_task_stack_align(child_stack_top -
                                          ARCH_TASK_KERNEL_STACK_RESERVE));
            } else {
                /*  Copier le contenu de la pile */
                memcpy(child->stack_base, parent->stack_base, KERNEL_TASK_STACK_SIZE);
                
                /*  Calculer le nouveau SP avec le MeME offset depuis le haut */
                vaddr_t child_stack_top = task_stack_addr(child->stack_top);
                arch_task_context_set_kernel_sp(&child->context,
                    child_stack_top - parent_sp_offset_from_top);

                /* NOUVEAU : Corriger les adresses dans le contenu copié */
                vaddr_t child_stack_base = task_stack_addr(child->stack_base);
                uint32_t stack_offset = child_stack_base - parent_stack_base;

                //KDEBUG("Fixing stack addresses: offset=%d bytes\n", stack_offset);
            
                /* Parcourir la pile et corriger les pointeurs vers pile parent */
                uint32_t* stack_words = (uint32_t*)child->stack_base;
                uint32_t stack_size_words = KERNEL_TASK_STACK_SIZE / sizeof(uint32_t);
                uint32_t corrections = 0;
                
                for (uint32_t i = 0; i < stack_size_words; i++) {
                    uint32_t value = stack_words[i];
                    
                    /* Si cette valeur pointe dans la pile du parent, la corriger */
                    if ((vaddr_t)value >= parent_stack_base && (vaddr_t)value < parent_stack_top) {
                        uint32_t new_value = value + stack_offset;
                        stack_words[i] = new_value;
                        corrections++;
                        
                        if (corrections <= 5) {  /* Limiter les logs */
                            //KDEBUG("  Fix [+%u]: 0x%lX -> 0x%lX\n",
                            //    i * 4, value, new_value);
                        }
                    }
                }

                KDEBUG("Stack address corrections: %u pointers fixed\n", corrections);
                
                KDEBUG("Child stack analysis:\n");
                KDEBUG("  Child stack: %p - %p\n", child->stack_base, child->stack_top);
                KDEBUG("  Child SP: 0x%lX\n",
                       TASK_ADDR(arch_task_context_kernel_sp(&child->context)));
                KDEBUG("  Copied stack with SP offset %u from top\n", parent_sp_offset_from_top);
            }
        } else {
            /* Pile propre si parent invalide */
            KWARN("Parent has no valid stack, creating clean stack\n");
            memset(child->stack_base, 0, KERNEL_TASK_STACK_SIZE);
            vaddr_t child_stack_top = task_stack_addr(child->stack_top);
            arch_task_context_set_kernel_sp(&child->context,
                arch_task_stack_align(child_stack_top -
                                      ARCH_TASK_KERNEL_STACK_RESERVE));
        }

        arch_task_context_set_kernel_stack(&child->context,
                                           task_stack_addr(child->stack_top),
                                           arch_task_context_kernel_sp(&child->context));
        arch_task_context_set_returns_to_user(&child->context, false);
    }
    
    /*  Alignement final */
    arch_task_context_set_kernel_sp(&child->context,
                                    arch_task_context_kernel_sp(&child->context));
    arch_task_context_set_svc_sp(&child->context,
                                 arch_task_context_svc_sp(&child->context));

    return child;

}

/**
 * Creer une copie d'une tache pour fork()
 */
task_t* task_create_copy(task_t* parent, bool from_user)
{
    task_t* child;
    unsigned long flags;
    
    if (!parent || !parent->process) {
        KERROR("task_create_copy: parent NULL\n");
        KERROR("task_create_copy: Parent NULL Proc\n");
        return NULL;
    }

    spin_lock_irqsave(&task_lock, &flags);
    if (task_count >= MAX_TASKS) {
        spin_unlock_irqrestore(&task_lock, flags);
        KERROR("task_create_copy: Maximum task count reached (%d)\n", MAX_TASKS);
        return NULL;
    }
    spin_unlock_irqrestore(&task_lock, flags);
    
    /* Allouer la structure de tache */
    child = (task_t*)kmalloc(sizeof(task_t));
    if (!child) {
        KERROR("task_create_copy: Failed to allocate child task\n");
        return NULL;
    }

    /* Copier la structure parent */
    memcpy(child, parent, sizeof(task_t));
    child->magic = TASK_MAGIC_DEAD;
    
    /* Reinitialiser les champs specifiques a l'enfant */
    child->task_id = task_reserve_task_id();
    strncpy(child->name, parent->name, TASK_NAME_MAX - 1);
    child->name[TASK_NAME_MAX - 1] = '\0';
    
    if(!set_process_stack(parent,child, from_user)) {
        child->magic = TASK_MAGIC_DEAD;
        kfree(child);
        return NULL;
    }

    /* Configuration processus pour l'enfant */
    if (parent->type == TASK_TYPE_PROCESS) {

        child->process = (process_t *)kmalloc(sizeof(process_t));
        if(child->process){
            child->type = TASK_TYPE_PROCESS;
            child->process->pid = task_reserve_pid();
            child->process->ppid = parent->process->pid;
            child->process->pgid = parent->process->pgid;
            child->process->sid = parent->process->sid;
            child->process->controlling_tty = parent->process->controlling_tty;
            child->process->parent = parent;
            child->process->children = NULL;
            child->process->sibling_next = NULL;
            child->process->exit_code = 0;
            child->process->term_signal = 0;
            child->process->stop_signal = 0;
            child->process->stop_reported = 0;
            child->process->uid = parent->process->uid;
            child->process->gid = parent->process->gid;
            child->process->umask = parent->process->umask;
            child->process->rlimit_nofile_cur = parent->process->rlimit_nofile_cur;
            child->process->rlimit_nofile_max = parent->process->rlimit_nofile_max;
            child->process->alarm_expire_tick = 0;
            child->process->alarm_active = 0;
            child->process->leader = child;
            child->process->thread_count = 1u;
            child->process->tls_image = parent->process->tls_image;
            child->process->tls_file_size =
                parent->process->tls_file_size;
            child->process->tls_memory_size =
                parent->process->tls_memory_size;
            child->process->tls_alignment =
                parent->process->tls_alignment;
            child->process->state = (proc_state_t)PROC_READY;
            strcpy(child->process->cwd, parent->process->cwd);    // Setting Current Working Directory
            strcpy(child->process->exe_path, parent->process->exe_path);
            memcpy(child->process->cmdline, parent->process->cmdline, sizeof(child->process->cmdline));
            child->process->cmdline_len = parent->process->cmdline_len;
            memcpy(child->process->environ, parent->process->environ, sizeof(child->process->environ));
            child->process->environ_len = parent->process->environ_len;
            
            /* Initialiser la table des fichiers (sera copiee plus tard) */
            memset(child->process->files, 0, sizeof(child->process->files));
            memset(child->process->fd_flags, 0, sizeof(child->process->fd_flags));
            
            /* La VM sera copiee avec COW dans sys_fork() */
            child->process->vm = NULL;

            child->process->waitpid_pid = 0;
            child->process->waitpid_status = NULL;
            child->process->waitpid_options = 0;
            child->process->waitpid_iteration = 0;
            child->process->waitpid_caller_lr = 0;

        }
        else {
            child->magic = TASK_MAGIC_DEAD;
            task_free_kernel_stack(child);
            kfree(child);
            panic("Task Create Copy - cannot allocate Process Structure");
        }
    } else {
        /* Pour les threads kernel, garder le meme type */
        child->type = parent->type;
    }
    
    /*
     * Fork completes VM, parent/child links and file-table setup in sys_fork()
     * before publishing the child to the runqueue.
     */
    child->state = TASK_BLOCKED;
    child->next = NULL;
    child->prev = NULL;
    child->rq_next = NULL;
    child->rq_prev = NULL;
    child->rq_priority = TASK_PRIORITY_LEVELS;
    
    /* Statistiques */
    child->created_time = get_current_time();
    child->total_runtime = 0;
    child->user_runtime = 0;
    child->system_runtime = 0;
    child->switch_count = 0;
    child->page_faults = 0;
    child->cow_faults = 0;
    child->stack_faults = 0;
    child->lazy_faults = 0;
    arch_task_context_mark_first_run(&child->context);
    arch_task_context_set_kernel_return_value(&child->context, 0);
    child->wakeup_time = 0;
    child->quantum_left = QUANTUM_TICKS;
    child->ready_since_tick = 0;
    child->sched_debt = 0;
    child->running_cpu = TASK_CPU_NONE;
    child->last_cpu = TASK_CPU_NONE;
    child->clear_child_tid = 0;
    child->futex_wait_address = 0;
    child->futex_wait_active = 0;
    child->magic = TASK_MAGIC_ALIVE;

    kernel_lifecycle_stats.tasks_created++;
    return child;
}





void validate_task_list(const char* location)
{
    task_t* task = task_list_head;
    int count = 0;
    
    KDEBUG("=== TASK LIST VALIDATION [%s] ===\n", location);
    
    if (!task_list_head) {
        KDEBUG("Empty task list\n");
        return;
    }
    
    do {
        KDEBUG("Task %d: %s (next=%p, prev=%p)\n", 
               count, task->name, task->next, task->prev);
        
        // Verifications de base
        if (!task->next) {
            KERROR("KO Task %s has NULL next pointer!\n", task->name);
            break;
        }
        
        if (count > 0 && task->next->prev != task) {
            KERROR("KO Task list integrity broken at %s!\n", task->name);
            break;
        }
        
        task = task->next;
        count++;
        
        if (count > MAX_TASKS) {
            KERROR("KO Task list loop detected!\n");
            break;
        }
        
    } while (task != task_list_head);
    
    KDEBUG("Task list OK: %d tasks\n", count);
}


void switch_to_idle_stack(void)
{
    if (!idle_task) {
        panic("No idle task for stack switch");
    }
    
    //KINFO("Switching to idle stack...\n");
    
    vaddr_t current_sp;
    
    /* Copier quelques données importantes sur la nouvelle pile */
    vaddr_t new_sp = arch_task_context_kernel_sp(&idle_task->context);
    
    /* Réserver de l'espace sur la pile idle pour les variables locales */
    new_sp -= 64;  /* 64 bytes de marge */
    new_sp = arch_task_stack_align(new_sp);
    
    /* CRITIQUE: Switcher vers la pile idle */
    arch_set_stack_pointer(new_sp);
    
    /* Mettre à jour le SP d'idle */
    arch_task_context_set_kernel_sp(&idle_task->context, new_sp);
    
    /* Vérification */
    current_sp = arch_current_stack_pointer();
    
    if (current_sp < task_stack_addr(idle_task->stack_base) ||
        current_sp >= task_stack_addr(idle_task->stack_top)) {
        panic("Failed to switch to idle stack");
    }
    
}


/**
 * Initialiser le systeme de taches
 */
static int task_legacy_poll_wait(void *owner)
{
    task_t *task = task_current_local();
    task_t *leader;

    (void)owner;
    if (!task)
        return -EINTR;
    leader = task_get_process_leader(task);
    task_set_interruptible_until(task, get_system_ticks() + 1);
    yield();
    task_set_wakeup_time(task, 0);
    return leader && has_pending_signals(leader) ? -EINTR : 0;
}

void init_task_system(void)
{
    if (scheduler_initialized) {
        KWARN("Task system already initialized\n");
        return;
    }
    
    KINFO("Initializing task system...\n");
    
    /* Initialiser les variables globales */
    task_current_clear_all();
    task_list_head = NULL;
    runqueue_clear_locked();
    next_task_id = 1;
    task_count = 0;
    (void)task_register_poll_wait_handler(task_legacy_poll_wait, NULL);
    
  
    scheduler_initialized = true;
    
    //KINFO("Task system initialized. Current task: %s (ID=%u)\n", 
    //      current_task->name, current_task->task_id);
}

/**
 * Nettoyer le systeme de taches
 */
void cleanup_task_system(void)
{
    if (!scheduler_initialized) return;
    
    spin_lock(&task_lock);
    
    /* Detruire toutes les taches sauf idle */
    task_t* task = task_list_head;
    task_t* next;
    
    while (task) {
        next = task->next;
        if (!task_is_idle_task(task)) {
            task_destroy(task);
        }
        task = (next == task_list_head) ? NULL : next;
    }
    
    /* Detruire idle en dernier */
    if (idle_task) {
        task_destroy(idle_task);
        idle_task = NULL;
    }
    memset(idle_tasks, 0, sizeof(idle_tasks));
    
    /* Reinitialiser les variables */
    task_current_clear_all();
    task_list_head = NULL;
    runqueue_clear_locked();
    next_task_id = 1;
    task_count = 0;
    scheduler_initialized = false;
    
    spin_unlock(&task_lock);
    
    KINFO("Task system cleaned up\n");
}

/* Fonction de validation a ajouter */
bool validate_task_stack_safe(task_t* task)
{
    if (!task || !task->stack_base || !task->stack_top) {
        KERROR("validate_task_stack: Invalid task or stack pointers\n");
        return false;
    }
    
    vaddr_t base = task_stack_addr(task->stack_base);
    vaddr_t top = task_stack_addr(task->stack_top);
    vaddr_t sp = arch_task_context_kernel_sp(&task->context);
    
    /* Verification fondamentale */
    if (base >= top) {
        KERROR("Task %s: Invalid stack layout (base >= top)\n", task->name);
        return false;
    }
    
    /* SP dans les limites */
    if (sp <= base || sp >= top) {
        KERROR("Task %s: SP out of bounds (SP=0x%lX, base=0x%lX, top=0x%lX)\n",
               task->name, TASK_ADDR(sp), TASK_ADDR(base), TASK_ADDR(top));
        return false;
    }
    
    /* Marges de securite */
    if (sp - base < 256) {
        KERROR("Task %s: SP too close to base (margin=%lu bytes)\n",
               task->name, TASK_ADDR(sp - base));
        return false;
    }
    
    if (top - sp < 256) {
        KERROR("Task %s: SP too close to top (margin=%lu bytes)\n",
               task->name, TASK_ADDR(top - sp));
        return false;
    }
    
    /* Alignement */
    if (sp & 7) {
        KERROR("Task %s: SP not aligned (SP=0x%lX)\n", task->name,
               TASK_ADDR(sp));
        return false;
    }
    
    return true;
}

const char* task_type_to_string(task_type_t type)
{
    switch( type ){
        case TASK_TYPE_KERNEL : return "KERNEL" ;
        case TASK_TYPE_PROCESS : return "PROCESS" ;
        case TASK_TYPE_THREAD : return "THREAD" ;
        default : return "NO TYPE SET";
    }
}

/* Version amelioree de task_dump_stacks() pour diagnostic */
void task_dump_stacks_detailed(void)
{
    task_t* task;
    task_t* start_task;
    int count = 0;
    int valid_stacks = 0;
    int invalid_stacks = 0;
    
    KINFO("=== DETAILED Stack Analysis ===\n");
    
    if (!task_list_head) {
        KINFO("No tasks in list\n");
        return;
    }
    
    task = task_list_head;
    start_task = task_list_head;
    
    do {
        if (task && task->stack_base && task->stack_top) {
            count++;
            
            vaddr_t base = task_stack_addr(task->stack_base);
            vaddr_t top = task_stack_addr(task->stack_top);
            vaddr_t sp = arch_task_context_kernel_sp(&task->context);
            uint32_t size = task->stack_size;

            print_cpu_mode();
            
            KINFO("Task %u (%s):\n", task->task_id, task->name);
            KINFO("  Stack range: 0x%lX - 0x%lX (%u bytes)\n",
                  TASK_ADDR(base), TASK_ADDR(top), size);
            KINFO("  Current SP:  0x%lX\n", TASK_ADDR(sp));
            
            /* Calculs detailles */
            if (sp > base && sp < top) {
                uint32_t used_from_top = top - sp;
                uint32_t available_below = sp - base;
                
                KINFO("  Space used from top: %u bytes\n", used_from_top);
                KINFO("  Space available below: %u bytes\n", available_below);
                KINFO("  Stack utilization: %u%%\n", 
                      used_from_top / size * 100);
                
                /* etat de la stack */
                if (validate_task_stack_safe(task)) {
                    KINFO("  Status: OK VALID\n");
                    valid_stacks++;
                } else {
                    KINFO("  Status: KO INVALID\n");
                    invalid_stacks++;
                }
                
                /* Alertes */
                if (used_from_top > size * 8 / 10) {
                    KWARN("  WARNING WARNING: Stack >80%% used!\n");
                }
                
                if (available_below < 512) {
                    KWARN("  WARNING WARNING: <512 bytes to stack base!\n");
                }
                
            } else {
                KERROR("  Status: KO CRITICAL - SP out of bounds!\n");
                invalid_stacks++;
            }
            
            /* Alignement */
            KINFO("  SP alignment: %s\n", (sp & 7) ? "KO Misaligned" : "OK Aligned");
            KINFO("  Task Type : %s\n", task_type_to_string(task->type) );
            KINFO("  Task State : %s\n", task_state_string(task->state) );
            KINFO("  Task Context LR : 0x%lX\n",
                  TASK_ADDR(arch_task_context_kernel_lr(&task->context)));
            KINFO("  Task Context PC : 0x%lX\n",
                  TASK_ADDR(arch_task_context_kernel_pc(&task->context)));
            KINFO("  Task Context status : 0x%lX\n",
                  TASK_ADDR(arch_task_context_kernel_cpsr(&task->context)));
            KINFO("  Task Context IS FIRST RUN : %u\n",
                  arch_task_context_is_first_run(&task->context));
            KINFO("  Task Context NEXT : 0x%lX\n", TASK_ADDR(task->next));
            KINFO("  Task Context PREVIOUS : 0x%lX\n",
                  TASK_ADDR(task->prev));
            if(task->process){
                KINFO("  Task Context Process VM Stack Start : 0x%lX\n",
                      TASK_ADDR(task->process->vm->stack_start));
                KINFO("  Task Context Process VM Heap Start : 0x%lX\n",
                      TASK_ADDR(task->process->vm->heap_start));
                KINFO("  Task Context Process PID : %d\n", task->process->pid );
                KINFO("  Task Context Process PPID : %d\n", task->process->ppid );
                KINFO("  Task Context Process WAIT PID : %d\n", task->process->waitpid_pid );
                KINFO("  Task Context Process WAIT PID CALLER LR : 0x%lX\n",
                      TASK_ADDR(task->process->waitpid_caller_lr));
            }

            KINFO("\n");
        }
        
        task = task->next;
        
        if (count > 10) {
            KWARN("*** Loop protection activated ***\n");
            break;
        }
        
    } while (task && task != start_task);
    
    /* Resume */
    KINFO("=== Stack Analysis Summary ===\n");
    KINFO("Total tasks analyzed: %d\n", count);
    KINFO("Valid stacks:        %d\n", valid_stacks);
    KINFO("Invalid stacks:      %d\n", invalid_stacks);
    
    if (invalid_stacks > 0) {
        KERROR("DONE CRITICAL: %d tasks have invalid stacks!\n", invalid_stacks);
    } else {
        KINFO("OK All stacks are valid\n");
    }
}


/**
 * Creer une nouvelle tache
 */
task_t* task_create(const char* name, void (*entry)(void* arg), void* arg, uint32_t priority)
{
    task_t* task;
    
    /*if (!entry) {
        KERROR("task_create: NULL entry point\n");
        return NULL;
    }*/
    
    if (task_count >= MAX_TASKS) {
        KERROR("task_create: Maximum task count reached (%d)\n", MAX_TASKS);
        return NULL;
    }
    
    /* Allouer la structure de tache */
    task = (task_t*)kmalloc(sizeof(task_t));
    if (!task) {
        KERROR("task_create: Failed to allocate task structure\n");
        return NULL;
    }
    task->magic = TASK_MAGIC_DEAD;
    
    /* Allouer la stack */
    if (!task_alloc_kernel_stack(task)) {
        KERROR("task_create: Failed to allocate stack\n");
        task->magic = TASK_MAGIC_DEAD;
        kfree(task);
        return NULL;
    }
    
    /* Initialiser la structure */
    task->task_id = task_reserve_task_id();
    strncpy(task->name, name ? name : "unnamed", TASK_NAME_MAX - 1);
    task->name[TASK_NAME_MAX - 1] = '\0';
    
    /*
     * A freshly allocated task is private to its creator.  It must not enter
     * task_list_head or the runqueue until the caller has finished all process,
     * VM, FD and signal metadata.  SMP schedulers scan task_list_head from
     * several CPUs, so publishing a half-built task is observable state.
     */
    task->state = TASK_BLOCKED;
    task->priority = priority;
    
    task->entry_point = entry;
    task->entry_arg = arg;
    
    task->next = NULL;
    task->prev = NULL;
    task->rq_next = NULL;
    task->rq_prev = NULL;
    task->rq_priority = TASK_PRIORITY_LEVELS;
    
    /* Statistiques */
    task->created_time = 0;
    task->total_runtime = 0;
    task->user_runtime = 0;
    task->system_runtime = 0;
    task->switch_count = 0;
    task->page_faults = 0;
    task->cow_faults = 0;
    task->stack_faults = 0;
    task->lazy_faults = 0;

    task->type = TASK_TYPE_PROCESS;  /* Nouvelle ligne */
    //task->process->pid = task->task_id;  /* Nouvelle ligne */
    /* === CONFIGURATION CORRIGeE DU CONTEXTE === */
    setup_task_context(task);

    task->quantum_left = QUANTUM_TICKS;
    task->wakeup_time = 0;
    task->ready_since_tick = 0;
    task->sched_debt = 0;
    task->running_cpu = TASK_CPU_NONE;
    task->last_cpu = TASK_CPU_NONE;
    task->clear_child_tid = 0;
    task->futex_wait_address = 0;
    task->futex_wait_active = 0;
    task->magic = TASK_MAGIC_ALIVE;
    task->process = NULL;  /* Par defaut, pas de processus associe */

    //debug_context_registers(&task->context, "AFTER_setup_task_context");
    
    /* === VALIDATION CRITIQUE === */
    if (!validate_task_stack_safe(task)) {
        KERROR("KO Stack validation failed for task %s\n", task->name);
        task->magic = TASK_MAGIC_DEAD;
        task_free_kernel_stack(task);
        kfree(task);
        return NULL;
    }
    
    kernel_lifecycle_stats.tasks_created++;
    
    KINFO("OK Created task '%s' (ID=%u, priority=%u) - Stack validated\n", 
          task->name, task->task_id, task->priority);
    
    return task;
}

void init_standard_files(process_t* process) {
    // stdin (fd = 0) - TTY logique, backend UART
    process->files[STDIN_FILENO] = create_tty_console_file("stdin", O_RDONLY);
    process->fd_flags[STDIN_FILENO] = 0;
    
    // stdout (fd = 1) - TTY logique, backend UART
    process->files[STDOUT_FILENO] = create_tty_console_file("stdout", O_WRONLY);
    process->fd_flags[STDOUT_FILENO] = 0;
    
    // stderr (fd = 2) - TTY logique, backend UART
    process->files[STDERR_FILENO] = create_tty_console_file("stderr", O_WRONLY);
    process->fd_flags[STDERR_FILENO] = 0;
}


/**
 * Adapter task_create pour supporter les processus
 */
task_t* task_create_process(const char* name, void (*entry)(void* arg), 
                                  void* arg, uint32_t priority, task_type_t type)
{
    task_t* parent = task_current_local();
    task_t* task = task_create(name, entry, arg, priority);
    if (!task) return NULL;
    
    /* Configurer le type */
    task->type = type;
    
    if (type == TASK_TYPE_PROCESS) {

        task->process = (process_t *)kmalloc(sizeof(process_t));
        if (!task->process) {
            KERROR("Task Crete Copy : cannot allocate process structure...");
            task_destroy(task);
            return NULL;
        }

        /* Initialiser les champs processus */
        task->process->pid = task_reserve_pid();
        task->process->ppid = (parent && parent->type == TASK_TYPE_PROCESS) ?
                             parent->process->pid : 0;
        task->process->pgid = task->process->pid;
        task->process->sid = task->process->pid;
        task->process->controlling_tty = 0;
        task->process->parent = (parent && parent->type == TASK_TYPE_PROCESS) ?
                               parent : NULL;
        task->process->children = NULL;
        task->process->sibling_next = NULL;
        task->process->exit_code = 0;
        task->process->term_signal = 0;
        task->process->stop_signal = 0;
        task->process->stop_reported = 0;
        task->process->uid = 0;
        task->process->gid = 0;
        task->process->umask = 022;
        task->process->rlimit_nofile_cur = MAX_FILES;
        task->process->rlimit_nofile_max = MAX_FILES;
        task->process->alarm_expire_tick = 0;
        task->process->alarm_active = 0;
        task->process->leader = task;
        task->process->thread_count = 1u;
        task->process->tls_image = 0;
        task->process->tls_file_size = 0;
        task->process->tls_memory_size = 0;
        task->process->tls_alignment = 0;
        task->process->state = (proc_state_t)PROC_READY;
        
        /* Creer l'espace memoire */
        task->process->vm = create_vm_space();

        if (!task->process->vm) {
            task_destroy(task);
            return NULL;
        }

        /*
         * A newly-created process still starts in a kernel entry function
         * (init launcher, shell launcher, etc.).  Keep its CPU context on the
         * kernel TTBR0 until execve() has built a real user context.  This is
         * required on platforms where the kernel link address sits below the
         * TTBR split.
         */
        arch_task_context_set_address_space(&task->context,
                                            arch_kernel_address_space_context(),
                                            ASID_KERNEL);
        
        /* Initialiser les fichiers */
        memset(task->process->files, 0, sizeof(task->process->files));
        memset(task->process->fd_flags, 0, sizeof(task->process->fd_flags));

        init_standard_files(task->process);

        /* Initialiser les champs waitpid */
        task->process->waitpid_pid = 0;
        task->process->waitpid_status = NULL;
        task->process->waitpid_options = 0;
        task->process->waitpid_iteration = 0;
        task->process->waitpid_caller_lr = 0;
        strcpy(task->process->cwd, "/");    // Setting Current Working Directory
        strncpy(task->process->exe_path, name ? name : task->name, MAX_PATH - 1);
        task->process->exe_path[MAX_PATH - 1] = '\0';
        strncpy(task->process->cmdline, name ? name : task->name, PROC_CMDLINE_MAX - 1);
        task->process->cmdline[PROC_CMDLINE_MAX - 1] = '\0';
        task->process->cmdline_len = strlen(task->process->cmdline) + 1;
        task->process->environ[0] = '\0';
        task->process->environ_len = 0;
        
        /* Initialiser les signaux */
        init_process_signals(task);
        
    } else {
        // Mettre toute la structure process à zéro
        task->process = NULL; // KERNEL TASK
    }
    
    return task;
}

task_t* task_create_user_thread(task_t* creator, vaddr_t entry,
                                vaddr_t argument, vaddr_t user_stack,
                                vaddr_t tls_base,
                                vaddr_t clear_child_tid)
{
    process_t* process = task_get_process(creator);
    task_t* leader = task_get_process_leader(creator);
    task_t* thread;
    unsigned long flags;

    if (!process || !process->vm || !leader || !entry || !user_stack ||
        !arch_task_context_returns_to_user(&creator->context))
        return NULL;

    thread = task_create("thread", NULL, NULL, creator->priority);
    if (!thread)
        return NULL;

    snprintf(thread->name, sizeof(thread->name), "thr%u", thread->task_id);
    thread->type = TASK_TYPE_THREAD;
    thread->thread.process = process;
    thread->thread.leader = leader;
    thread->clear_child_tid = clear_child_tid;
    thread->entry_point = (void (*)(void*))(uintptr_t)entry;
    thread->entry_arg = (void *)(uintptr_t)argument;

    memset(&thread->context, 0, sizeof(thread->context));
    arch_task_context_init_user_entry(
        &thread->context,
        (uintptr_t)process->vm->pgdir,
        process->vm->asid,
        task_stack_addr(thread->stack_top),
        entry,
        user_stack);
    arch_task_context_set_user_register(&thread->context, 0,
                                        (uintptr_t)argument);
    arch_task_context_set_tls(&thread->context, (uintptr_t)tls_base);

    spin_lock_irqsave(&task_lock, &flags);
    process->thread_count++;
    spin_unlock_irqrestore(&task_lock, flags);

    return thread;
}

int task_futex_wait(task_t* task, vaddr_t address, uint32_t expected,
                    uint32_t deadline)
{
    volatile uint32_t* user_word = (volatile uint32_t *)(uintptr_t)address;
    unsigned long flags;
    bool timed_out;

    if (!task || !task_get_process(task) || !address)
        return -EINVAL;

    spin_lock_irqsave(&task_lock, &flags);
    /*
     * The syscall faulted the page in before taking task_lock. This second
     * comparison closes the lost-wakeup window against FUTEX_WAKE.
     * Concurrent munmap remains forbidden until VM mutation locking lands.
     */
    if (__atomic_load_n(user_word, __ATOMIC_ACQUIRE) != expected) {
        spin_unlock_irqrestore(&task_lock, flags);
        return -EAGAIN;
    }

    runqueue_remove_locked(task);
    scheduler_clear_nonrunnable_debt_locked(task, TASK_INTERRUPTIBLE);
    task->futex_wait_address = address;
    task->futex_wait_active = 1u;
    task->wakeup_time = deadline;
    task->state = TASK_INTERRUPTIBLE;
    spin_unlock_irqrestore(&task_lock, flags);

    yield();

    spin_lock_irqsave(&task_lock, &flags);
    timed_out = task->futex_wait_active && deadline != 0 &&
        (int32_t)(get_system_ticks() - deadline) >= 0;
    task->futex_wait_active = 0;
    task->futex_wait_address = 0;
    task->wakeup_time = 0;
    spin_unlock_irqrestore(&task_lock, flags);

    if (timed_out)
        return -ETIMEDOUT;
    if (task->type == TASK_TYPE_PROCESS && has_pending_signals(task))
        return -EINTR;
    return 0;
}

uint32_t task_futex_wake(process_t* process, vaddr_t address,
                         uint32_t max_count)
{
    task_t* task;
    task_t* start;
    uint32_t woken = 0;
    uint32_t scanned = 0;
    unsigned long flags;

    if (!process || !address || max_count == 0)
        return 0;

    spin_lock_irqsave(&task_lock, &flags);
    start = task_list_head;
    task = start;
    while (task && scanned < MAX_TASKS && woken < max_count) {
        task_t* next = task->next;

        if (task->process == process &&
            task->futex_wait_active &&
            task->futex_wait_address == address &&
            (task->state == TASK_INTERRUPTIBLE ||
             task->state == TASK_UNINTERRUPTIBLE)) {
            task->futex_wait_active = 0;
            task->futex_wait_address = 0;
            task->wakeup_time = 0;
            task_make_ready_under_lock(task);
            woken++;
        }

        task = next;
        scanned++;
        if (task == start)
            break;
    }
    spin_unlock_irqrestore(&task_lock, flags);
    return woken;
}

void task_publish_user_thread(task_t* thread)
{
    if (!thread || thread->type != TASK_TYPE_THREAD ||
        thread->state != TASK_BLOCKED || !thread->process)
        return;
    add_to_ready_queue(thread);
}

void task_abort_user_thread(task_t* thread)
{
    unsigned long flags;

    if (!thread || thread->type != TASK_TYPE_THREAD ||
        thread->next || thread->prev)
        return;

    spin_lock_irqsave(&task_lock, &flags);
    if (thread->process && thread->process->thread_count > 1u)
        thread->process->thread_count--;
    spin_unlock_irqrestore(&task_lock, flags);

    thread->magic = TASK_MAGIC_DEAD;
    task_free_kernel_stack(thread);
    kfree(thread);
    kernel_lifecycle_stats.tasks_destroyed++;
}

void task_exit_current_thread(int status)
{
    task_t* thread = task_current_local();
    unsigned long flags;

    (void)status;
    if (!thread || thread->type != TASK_TYPE_THREAD || !thread->process)
        panic("task_exit_current_thread called outside a user thread");

    disable_interrupts_save();
    spin_lock_irqsave(&task_lock, &flags);
    runqueue_remove_locked(thread);
    thread->sched_debt = 0;
    thread->ready_since_tick = 0;
    thread->state = TASK_ZOMBIE;
    /*
     * Give the context switch one timer boundary to publish running_cpu=NONE
     * before another CPU reclaims this task and its private kernel stack.
     */
    thread->wakeup_time = get_system_ticks() + 1u;
    spin_unlock_irqrestore(&task_lock, flags);

    switch_to_idle();
    __builtin_unreachable();
}


/**
 * Configurer le contexte initial d'une tache
 */
void setup_task_context(task_t* task)
{
    memset(&task->context, 0, sizeof(task_context_t));
    
    /* Configuration des registres */
    vaddr_t stack_base = task_stack_addr(task->stack_base);
    vaddr_t stack_top = task_stack_addr(task->stack_top);
    arch_task_context_init_kernel_entry(&task->context,
                                        task->entry_point,
                                        task->entry_arg,
                                        stack_top);
    
    /* Validation */
    vaddr_t sp = arch_task_context_kernel_sp(&task->context);
    if (sp >= stack_top || sp <= stack_base) {
        KERROR("KO FATAL: Invalid SP for task %s\n", task->name);
        panic("Stack configuration error");
    }
}

/**
 * Detruire une tache
 */
void task_destroy(task_t* task)
{
    unsigned long flags;
    task_t* current = task_current_local();

    if (!task) {
        task = current;  /* Detruire la tache courante si NULL */
    }

    if (!task)
        return;
    
    if (task_is_idle_task(task)) {
        KERROR("Cannot destroy idle task\n");
        return;
    }

    //KDEBUG("DESTROYING TASK %s, with state = %s\n", task->name, task_state_string(task->state));
    
    spin_lock_irqsave(&task_lock, &flags);
    
    if (task->state != TASK_ZOMBIE && task->state != TASK_TERMINATED) {
        runqueue_remove_locked(task);
        task->state = TASK_ZOMBIE;
        task->running_cpu = TASK_CPU_NONE;
        if (task->type == TASK_TYPE_PROCESS && task->process)
            task->process->state = (proc_state_t)PROC_ZOMBIE;
        kernel_lifecycle_stats.zombies_created++;
    }
    
    /* Si c'est la tache courante, forcer une commutation */
    if (task == current) {
        spin_unlock_irqrestore(&task_lock, flags);
        schedule();  /* Ne reviendra jamais ici */
        /* NOTREACHED */
    }
    
    spin_unlock_irqrestore(&task_lock, flags);

    /* Retirer de la liste */
    remove_task_from_list(task);

    task->magic = TASK_MAGIC_DEAD;
    
    /* Liberer les ressources */
    task_free_kernel_stack(task);

    if (task->type == TASK_TYPE_PROCESS)
        kfree(task->process);

    kfree(task);
    kernel_lifecycle_stats.tasks_destroyed++;
}

/* Ajouter cette vérification dans schedule_next_task */
void verify_ready_queue_integrity(void)
{
    unsigned long flags;
    task_t* task;
    int count = 0;

    KDEBUG("=== READY QUEUE VERIFICATION ===\n");

    spin_lock_irqsave(&task_lock, &flags);

    if (ready_queue.nr_running == 0) {
        KDEBUG("Ready queue is empty\n");
        spin_unlock_irqrestore(&task_lock, flags);
        return;
    }

    for (uint32_t prio = 0; prio < TASK_PRIORITY_LEVELS; prio++) {
        task = ready_queue.head[prio];
        while (task) {
            count++;
            KDEBUG("  [%d] p=%u %s (ID=%u): state=%d, rq_next=0x%lX, rq_prev=0x%lX\n",
                   count, prio, task->name, task->task_id, task->state,
                   TASK_ADDR(task->rq_next), TASK_ADDR(task->rq_prev));

            if (count > MAX_TASKS) {
                KERROR("Ready queue seems corrupted (too many tasks)\n");
                spin_unlock_irqrestore(&task_lock, flags);
                return;
            }

            task = task->rq_next;
        }
    }

    KDEBUG("Total tasks in ready queue: %d (tracked=%u)\n",
           count, ready_queue.nr_running);
    spin_unlock_irqrestore(&task_lock, flags);
}


/**
 * Ceder le CPU volontairement
 */
void yield(void)
{
    unsigned long flags;
    task_t* task = task_current_local();

    if (!scheduler_initialized) {
        return;
    }
    
    spin_lock_irqsave(&task_lock, &flags);
    if (task && task->state == TASK_RUNNING) {
        task_make_ready_under_lock(task);
    }
    spin_unlock_irqrestore(&task_lock, flags);

    //task_sleep_ms(500);  // Pause a bit to avoid race conditions.

    //KDEBUG("Task %s is yielding...\n", current_task->name);
    
    schedule();

}

bool is_on_kernel_stack(vaddr_t sp)
{
    extern uint32_t __stack_bottom, __stack_top;
    vaddr_t kernel_stack_bottom = (vaddr_t)(uintptr_t)&__stack_bottom;
    vaddr_t kernel_stack_top = (vaddr_t)(uintptr_t)&__stack_top;
    
    return ((vaddr_t)sp >= kernel_stack_bottom && (vaddr_t)sp < kernel_stack_top);
}

bool is_on_task_stack(task_t* task, vaddr_t sp)
{
    if (!task) return false;
    
    vaddr_t task_stack_bottom = task_stack_addr(task->stack_base);
    vaddr_t task_stack_top = task_stack_addr(task->stack_top);
    
    return ((vaddr_t)sp >= task_stack_bottom && (vaddr_t)sp < task_stack_top);
}

void debug_idle_corruption_source(void)
{
    task_t* current = task_current_local();

    if (!idle_task) return;
    
    /* Obtenir la trace de la pile */
    vaddr_t lr;
    lr = arch_current_link_register();
    
    KERROR("IDLE CORRUPTION DETECTED!\n");
    KERROR("  Called from: 0x%lX\n", TASK_ADDR(lr));
    KERROR("  Current task: %s\n", current ? current->name : "NULL");
    
    /* Dump de toutes les tâches pour voir qui a un stack_top bizarre */
    task_t* task = task_list_head;
    if (task) {
        do {
            vaddr_t expected_top = task_stack_addr(task->stack_base) + task->stack_size;
            if (task_stack_addr(task->stack_top) != expected_top) {
                KERROR("SUSPECT: Task %s has corrupted stack_top!\n", task->name);
                KERROR("  Expected: 0x%lX, Actual: 0x%lX\n",
                       TASK_ADDR(expected_top),
                       TASK_ADDR(task_stack_addr(task->stack_top)));
            }
            task = task->next;
        } while (task != task_list_head);
    }
}

void protect_idle_task(void)
{
    if (!idle_task) return;
    
    /* Sauvegarder les vraies valeurs d'idle */
    static uint8_t* idle_real_stack_base = NULL;
    static uint8_t* idle_real_stack_top = NULL;
    static uint32_t idle_real_stack_size = 0;
    static bool idle_protection_initialized = false;
    
    if (!idle_protection_initialized) {
        /* Sauvegarder les valeurs originales d'idle */
        idle_real_stack_base = idle_task->stack_base;
        idle_real_stack_top = idle_task->stack_top;
        idle_real_stack_size = idle_task->stack_size;
        idle_protection_initialized = true;
        
        return;  /* Pas de vérification au premier appel */
    }
    
    /* Vérifier et corriger les corruptions */
    bool corrupted = false;
    
    if (idle_task->stack_base != idle_real_stack_base) {
        KERROR("IDLE CORRUPTION: stack_base changed from 0x%lX to 0x%lX\n",
               TASK_ADDR(task_stack_addr(idle_real_stack_base)),
               TASK_ADDR(task_stack_addr(idle_task->stack_base)));
        idle_task->stack_base = idle_real_stack_base;
        corrupted = true;
    }
    
    if (idle_task->stack_top != idle_real_stack_top) {
        KERROR("IDLE CORRUPTION: stack_top changed from 0x%lX to 0x%lX\n",
               TASK_ADDR(task_stack_addr(idle_real_stack_top)),
               TASK_ADDR(task_stack_addr(idle_task->stack_top)));
        idle_task->stack_top = idle_real_stack_top;
        corrupted = true;
    }
    
    if (idle_task->stack_size != idle_real_stack_size) {
        KERROR("IDLE CORRUPTION: stack_size changed from %u to %u\n",
               idle_real_stack_size, idle_task->stack_size);
        idle_task->stack_size = idle_real_stack_size;
        corrupted = true;
    }
    
    /*  SI CORRUPTION DÉTECTÉE, APPELER LE DIAGNOSTIC */
    if (corrupted) {
        debug_idle_corruption_source();  /* ← ICI ! */
        KWARN("IDLE PROTECTION: Corruptions fixed\n");
    }
}

void save_task_context_safe(task_t* task, vaddr_t current_sp)
{
    if (!task) return;
    
    /* Protéger idle avant toute manipulation */
    if (task_is_idle_task(task)) {
        protect_idle_task();
    }

    /* NE PAS sauvegarder les zombies ! */
    if (task->state == TASK_ZOMBIE) {
        //KDEBUG("save_task_context_safe: Skipping zombie task %s\n", task->name);
        return;
    }
    
    if (task_is_idle_task(task)) {
        /* Vérification spéciale pour idle */
        if (is_on_task_stack(task, current_sp)) {
            /* SP valide dans la pile d'idle */
            arch_task_context_set_kernel_sp(&task->context, current_sp);
            //KDEBUG("Saved valid SP for idle: 0x%lX\n", current_sp);
        } else if (is_on_kernel_stack(current_sp)) {
            /* CRITIQUE: Idle sur pile kernel - ne pas sauvegarder ! */
            //KERROR("Idle still on kernel stack! SP=0x%lX\n", current_sp);
            KERROR("Keeping idle SP at: 0x%lX\n",
                   TASK_ADDR(arch_task_context_kernel_sp(&task->context)));
            /* Ne pas écraser task->context.sp */
        } else {
            KERROR("Idle on unknown stack! SP=0x%lX\n",
                   TASK_ADDR(current_sp));
            /* Ne pas écraser task->context.sp */
        }
    } else {
        /* Pour les autres tâches, comportement normal */
        if (is_on_task_stack(task, current_sp)) {
            arch_task_context_save_kernel_sp(&task->context, current_sp);
            //KDEBUG("Saved valid SP for %s: 0x%lX\n", task->name, current_sp);
        } else {
            task_dump_stacks_detailed();
            KERROR("Task %s has invalid SP: 0x%lX - context SP = 0x%lX\n",
                   task->name, TASK_ADDR(current_sp),
                   TASK_ADDR(arch_task_context_kernel_sp(&task->context)));
            //panic("Stopping");
            yield();
            return;
        }
    }
}

void switch_to_idle(void){
    unsigned long flags;
    task_t *current = task_current_local();
    task_t *next_task = schedule_next_task();
    task_t *local_idle = task_idle_on_cpu(smp_processor_id());
    uint32_t cpu = smp_processor_id();

    if (!next_task ||
        next_task == current ||
        next_task->state == TASK_ZOMBIE ||
        next_task->state == TASK_TERMINATED) {
        next_task = local_idle ? local_idle : idle_task;
    }

    spin_lock_irqsave(&task_lock, &flags);
    if (current &&
        (current->state == TASK_ZOMBIE || current->state == TASK_TERMINATED) &&
        current->running_cpu == cpu) {
        current->running_cpu = TASK_CPU_NONE;
    }
    scheduler_mark_running_locked(next_task);
    spin_unlock_irqrestore(&task_lock, flags);

    if (task_current_publish(cpu, next_task) != 0)
        panic("Failed to publish initial scheduler task");

    /*
     * Ne pas sauvegarder une tache morte et ne jamais modifier manuellement
     * le SP d'une tache suspendue.
     */
    __task_switch(NULL, &next_task->context);

    __builtin_unreachable();
}

void task_start_secondary_scheduler(uint32_t cpu_id)
{
    unsigned long flags;
    task_t* local_idle;

    local_idle = task_idle_on_cpu(cpu_id);
    if (!local_idle)
        panic("SMP secondary scheduler without idle task");

    spin_lock_irqsave(&task_lock, &flags);
    scheduler_mark_running_locked(local_idle);
    spin_unlock_irqrestore(&task_lock, flags);

    if (task_current_publish(cpu_id, local_idle) != 0)
        panic("Failed to publish secondary idle task");

    __task_switch(NULL, &local_idle->context);
    __builtin_unreachable();
}

#define SCHED_ALARM_BATCH 16
#define SCHED_THREAD_REAP_BATCH 16
#define SCHED_SLEEP_OVERSHOOT_TRACE_TICKS 50u

static void task_reap_terminated_thread(task_t* thread)
{
    if (!thread || thread->type != TASK_TYPE_THREAD ||
        thread->state != TASK_TERMINATED)
        return;

    remove_task_from_list(thread);
    thread->magic = TASK_MAGIC_DEAD;
    task_free_kernel_stack(thread);
    kfree(thread);
    kernel_lifecycle_stats.tasks_destroyed++;
}

static void scheduler_scan_waiters(task_t* current)
{
    task_t* task;
    task_t* start;
    task_t* alarm_tasks[SCHED_ALARM_BATCH];
    task_t* zombie_wake_tasks[SCHED_ALARM_BATCH];
    task_t* thread_reap_tasks[SCHED_THREAD_REAP_BATCH];
    uint32_t current_time = get_system_ticks();
    uint32_t alarm_count = 0;
    uint32_t zombie_wake_count = 0;
    uint32_t thread_reap_count = 0;
    unsigned long flags;
    int count = 0;

    spin_lock_irqsave(&task_lock, &flags);

    if (!task_list_head) {
        spin_unlock_irqrestore(&task_lock, flags);
        return;
    }

    start = (task_header_plausible(current) &&
             task_header_plausible(current->next)) ? current->next : task_list_head;
    task = start;
    
    /*
     * Walk the global task list while holding task_lock. This keeps wakeup
     * state transitions and runqueue insertion atomic for the future
     * multi-scheduler-CPU model. Signal delivery is collected and performed
     * after dropping the lock, because it may itself wake tasks.
     */
    do {
        if (!task_header_plausible(task)) {
            KERROR("scheduler_scan_waiters: invalid task pointer %p near %p\n",
                   task, current);
            kernel_lifecycle_stats.scheduler_refused++;
            sched_trace_record(SCHED_TRACE_REFUSE_BROKEN_LIST, current);
            spin_unlock_irqrestore(&task_lock, flags);
            return;
        }

        if (!task_header_plausible(task->next) ||
            !task_header_plausible(task->prev) ||
            task->next->prev != task ||
            task->prev->next != task) {
            KERROR("scheduler_scan_waiters: broken task links task=%p next=%p prev=%p\n",
                   task, task->next, task->prev);
            kernel_lifecycle_stats.scheduler_refused++;
            sched_trace_record(SCHED_TRACE_REFUSE_BROKEN_LIST, task);
            spin_unlock_irqrestore(&task_lock, flags);
            return;
        }

        if ((task->state == TASK_INTERRUPTIBLE ||
             task->state == TASK_UNINTERRUPTIBLE) &&
            task->wakeup_time > 0 &&
            current_time >= task->wakeup_time) {
            uint32_t overdue = current_time - task->wakeup_time;

            //KDEBUG("Waking up task %s from sleep\n", task->name);
            
            if (task->state == TASK_UNINTERRUPTIBLE) {
                kernel_lifecycle_stats.fs_wait_timeouts++;
                sched_trace_record(SCHED_TRACE_FS_WAIT_TIMEOUT, task);
            } else {
                kernel_lifecycle_stats.sleep_deadline_wakeups++;
                if (overdue > SCHED_SLEEP_OVERSHOOT_TRACE_TICKS) {
                    kernel_lifecycle_stats.sleep_overshoots++;
                    sched_trace_record(SCHED_TRACE_SLEEP_OVERSHOOT, task);
                }
            }
            task->wakeup_time = 0;
            task_make_ready_under_lock(task);
        }

        if (task->type == TASK_TYPE_PROCESS && task->process &&
            task->process->alarm_active &&
            current_time >= task->process->alarm_expire_tick) {
            task->process->alarm_active = 0;
            task->process->alarm_expire_tick = 0;
            if (alarm_count < SCHED_ALARM_BATCH)
                alarm_tasks[alarm_count++] = task;
        }

        /*
         * sys_exit() cannot wake the parent directly on SMP: the parent may
         * run on another CPU and free the child while the child is still
         * executing on its kernel stack. Once the exiting task has switched
         * away and one timer tick elapsed, wake the parent from here.
         */
        if (task->state == TASK_ZOMBIE &&
            task->type == TASK_TYPE_PROCESS &&
            task->process &&
            task->process->thread_count <= 1u &&
            task->running_cpu == TASK_CPU_NONE &&
            task->wakeup_time > 0 &&
            current_time >= task->wakeup_time) {
            if (zombie_wake_count < SCHED_ALARM_BATCH) {
                task->wakeup_time = 0;
                zombie_wake_tasks[zombie_wake_count++] = task;
            }
        }

        /*
         * User threads have no waitpid-visible zombie. Once the context switch
         * has released their private kernel stack, claim them under task_lock
         * and reclaim them after dropping the lock.
         */
        if (task->state == TASK_ZOMBIE &&
            task->type == TASK_TYPE_THREAD &&
            task->process &&
            task->running_cpu == TASK_CPU_NONE &&
            task->wakeup_time > 0 &&
            current_time >= task->wakeup_time &&
            thread_reap_count < SCHED_THREAD_REAP_BATCH) {
            task->state = TASK_TERMINATED;
            task->wakeup_time = 0;
            if (task->process->thread_count > 1u)
                task->process->thread_count--;
            thread_reap_tasks[thread_reap_count++] = task;
        }

        task = task->next;
        count++;
    } while (task != start && count < MAX_TASKS);

    spin_unlock_irqrestore(&task_lock, flags);

    for (uint32_t i = 0; i < alarm_count; i++)
        send_signal(alarm_tasks[i], SIGALRM);

    /*
     * Remove the last member thread before waking a parent that may reap and
     * free the shared process object immediately on another CPU.
     */
    for (uint32_t i = 0; i < thread_reap_count; i++)
        task_reap_terminated_thread(thread_reap_tasks[i]);

    for (uint32_t i = 0; i < zombie_wake_count; i++) {
        task_t* zombie = zombie_wake_tasks[i];
        task_t* parent = zombie && zombie->process ? zombie->process->parent : NULL;
        bool parent_waiting = parent && parent->process &&
            parent->state == TASK_BLOCKED &&
            parent->process->state == (proc_state_t)PROC_BLOCKED;

        wakeup_parent(zombie);
        if (!parent_waiting && parent && parent->process) {
            sig_handler_t handler = parent->process->signals.actions[SIGCHLD].sa_handler;
            if (handler != SIG_DFL && handler != SIG_IGN)
                send_signal(parent, SIGCHLD);
        }
    }

    return;
}

static bool scheduler_has_ready_work(void)
{
    unsigned long flags;
    bool has_work;

    spin_lock_irqsave(&task_lock, &flags);
    has_work = ready_queue.nr_running > 0;
    spin_unlock_irqrestore(&task_lock, flags);

    return has_work;
}

static void scheduler_enable_secondary_cpus_once(void)
{
    uint32_t target_mask = 0;

    if (!smp_is_boot_cpu())
        return;

    for (uint32_t cpu = 1; cpu < smp_possible_cpu_count() && cpu < ARMOS_MAX_CPUS; cpu++)
        target_mask |= 1u << cpu;

    if ((secondary_scheduler_done_mask & target_mask) == target_mask)
        return;

    for (uint32_t cpu = 1; cpu < smp_possible_cpu_count() && cpu < ARMOS_MAX_CPUS; cpu++) {
        uint32_t bit = 1u << cpu;

        if (secondary_scheduler_done_mask & bit)
            continue;

        if (smp_scheduler_cpu_enabled(cpu)) {
            secondary_scheduler_done_mask |= bit;
            continue;
        }

        if (!smp_cpu_seen(cpu) || smp_cpu_state(cpu) != SMP_CPU_PARKED)
            continue;

        if (smp_enable_scheduler_cpu(cpu) == 0) {
            secondary_scheduler_done_mask |= bit;
            KBOOT_OKF("SMP: scheduler enabled on CPU%u", cpu);
        } else if ((secondary_scheduler_warned_mask & bit) == 0) {
            secondary_scheduler_warned_mask |= bit;
            KBOOT_WARNF("SMP: CPU%u scheduler unavailable (%s, start=%d, seen=%u)",
                        cpu,
                        smp_cpu_state_name(cpu),
                        smp_cpu_start_result(cpu),
                        smp_cpu_seen(cpu) ? 1u : 0u);
        }
    }
}

void scheduler_prepare_secondary_cpus(void)
{
    scheduler_enable_secondary_cpus_once();
}

static bool scheduler_entry_allowed(const char* caller, task_t* requested)
{
    task_t* current = task_current_local();

    if (!smp_scheduler_can_run_on_current_cpu()) {
        KERROR("%s: refusing scheduler entry on CPU%u; SMP scheduler disabled\n",
               caller, smp_processor_id());
        kernel_lifecycle_stats.scheduler_refused++;
        return false;
    }

    if (!scheduler_initialized || !task_header_plausible(current)) {
        KERROR("%s: scheduler not initialized or no current task\n", caller);
        return false;
    }

    if (requested &&
        (!task_header_plausible(requested) ||
         requested->state == TASK_ZOMBIE ||
         requested->state == TASK_TERMINATED ||
         !task_stack_metadata_valid(requested))) {
        KERROR("%s: refusing invalid task %p\n", caller, requested);
        kernel_lifecycle_stats.scheduler_refused++;
        sched_trace_record(SCHED_TRACE_REFUSE_INVALID_TASK, requested);
        return false;
    }

    if (get_critical_section()) {
        kernel_lifecycle_stats.scheduler_critical_repaired++;
        sched_trace_record(SCHED_TRACE_REFUSE_CRITICAL, current);
        unset_critical_section();
        return false;
    }

    if (arch_current_mode_is_interrupt())
        return false;

    return true;
}

static void scheduler_mark_running_locked(task_t* task)
{
    uint32_t cpu = smp_processor_id();

    if (!task)
        return;

    if (task->state == TASK_RUNNING && task->running_cpu == cpu)
        return;

    runqueue_remove_locked(task);
    task->state = TASK_RUNNING;
    task->running_cpu = cpu;
    task->last_cpu = cpu;
    if (task->type == TASK_TYPE_PROCESS && task->process)
        task->process->state = (proc_state_t)PROC_RUNNING;
    task->switch_count++;
}

static bool scheduler_validate_switch(task_t* old_task,
                                      task_t* next_task,
                                      vaddr_t current_sp)
{
    uint32_t cpu = smp_processor_id();

    if (!next_task || !task_stack_metadata_valid(next_task)) {
        kernel_lifecycle_stats.scheduler_refused++;
        sched_trace_record(SCHED_TRACE_REFUSE_INVALID_TASK, next_task);
        return false;
    }

    if (next_task->running_cpu != TASK_CPU_NONE &&
        next_task->running_cpu != cpu) {
        KERROR("SCHED: task %s already running on CPU%u, refusing CPU%u\n",
               next_task->name, next_task->running_cpu, cpu);
        kernel_lifecycle_stats.scheduler_refused++;
        sched_trace_record(SCHED_TRACE_REFUSE_INVALID_TASK, next_task);
        return false;
    }

    if (old_task && !task_kernel_sp_valid(old_task, current_sp)) {
        KERROR("SCHED: old task %s current SP 0x%lX out of range\n",
               old_task->name, TASK_ADDR(current_sp));
        KERROR("      stack 0x%lX-0x%lX context.sp=0x%lX svc_sp=0x%lX\n",
               TASK_ADDR(old_task->stack_base), TASK_ADDR(old_task->stack_top),
               TASK_ADDR(arch_task_context_kernel_sp(&old_task->context)),
               TASK_ADDR(arch_task_context_svc_sp(&old_task->context)));
        kernel_lifecycle_stats.scheduler_refused++;
        sched_trace_record(SCHED_TRACE_REFUSE_INVALID_TASK, old_task);
        return false;
    }

    return true;
}

static void scheduler_finish_no_switch(uint32_t irq_flags)
{
    unset_critical_section();
    restore_interrupts(irq_flags);
}

static void scheduler_restore_current_if_needed(task_t* current)
{
    unsigned long flags;

    spin_lock_irqsave(&task_lock, &flags);
    if (current && current->state == TASK_READY)
        scheduler_mark_running_locked(current);
    spin_unlock_irqrestore(&task_lock, flags);
}

static void scheduler_prepare_current_for_switch(task_t* current)
{
    unsigned long flags;

    spin_lock_irqsave(&task_lock, &flags);
    if (current && current->state == TASK_RUNNING)
        task_make_ready_under_lock(current);
    spin_unlock_irqrestore(&task_lock, flags);
}

static void scheduler_prepare_next_for_switch(task_t* next_task)
{
    unsigned long flags;

    spin_lock_irqsave(&task_lock, &flags);
    scheduler_mark_running_locked(next_task);
    spin_unlock_irqrestore(&task_lock, flags);
}

static void scheduler_switch_to(task_t* next_task,
                                uint32_t irq_flags,
                                bool save_current_context,
                                task_t* current)
{
    uint32_t cpu = smp_processor_id();

    scheduler_prepare_next_for_switch(next_task);
    unset_critical_section();

    if (task_current_publish(cpu, next_task) != 0)
        panic("Failed to publish scheduler task");

    __task_switch(save_current_context ? &current->context : NULL,
                  &next_task->context);

    restore_interrupts(irq_flags);
}

void schedule(void)
{
    uint32_t irq_flags;
    vaddr_t current_sp;
    bool save_current_context;
    task_t* current;
    task_t* next_task;
    
    if (!scheduler_entry_allowed("schedule", NULL))
        return;

    irq_flags = disable_interrupts_save();
    set_critical_section();

    current = task_current_local();
    scheduler_scan_waiters(current);

    current_sp = arch_current_stack_pointer();
    save_current_context = current->state != TASK_ZOMBIE &&
                           current->state != TASK_TERMINATED;

    if (save_current_context)
        scheduler_prepare_current_for_switch(current);

    next_task = schedule_next_task();

    if (next_task == current) {
        scheduler_restore_current_if_needed(current);
        scheduler_finish_no_switch(irq_flags);
        return;
    }

    if (!scheduler_validate_switch(save_current_context ? current : NULL,
                                   next_task,
                                   current_sp)) {
        if (save_current_context)
            scheduler_restore_current_if_needed(current);
        scheduler_finish_no_switch(irq_flags);
        return;
    }

    scheduler_switch_to(next_task, irq_flags, save_current_context, current);

}

void schedule_to(task_t *next_task)
{
    uint32_t irq_flags;
    vaddr_t current_sp;
    bool save_current_context;
    task_t* current;
    
    if (!next_task || !scheduler_entry_allowed("schedule_to", next_task))
        return;

    irq_flags = disable_interrupts_save();
    set_critical_section();

    current = task_current_local();
    scheduler_scan_waiters(current);

    current_sp = arch_current_stack_pointer();
    save_current_context = current->state != TASK_ZOMBIE &&
                           current->state != TASK_TERMINATED;

    if (save_current_context)
        scheduler_prepare_current_for_switch(current);

    if (next_task == current) {
        scheduler_restore_current_if_needed(current);
        scheduler_finish_no_switch(irq_flags);
        return;
    }

    if (!scheduler_validate_switch(save_current_context ? current : NULL,
                                   next_task,
                                   current_sp)) {
        if (save_current_context)
            scheduler_restore_current_if_needed(current);
        scheduler_finish_no_switch(irq_flags);
        return;
    }

    scheduler_switch_to(next_task, irq_flags, save_current_context, current);

}

/**
 * Version etendue de schedule() qui gere les types de taches
 */
void schedule_extended(void)
{
    /* Appeler votre schedule() existant */
    schedule();
}

static void secondary_idle_park_for_shutdown(uint32_t cpu) __attribute__((noreturn));

static void secondary_idle_park_for_shutdown(uint32_t cpu)
{
    task_t* idle = task_current_local();
    unsigned long flags;

    spin_lock_irqsave(&task_lock, &flags);
    if (idle) {
        runqueue_remove_locked(idle);
        idle->state = TASK_BLOCKED;
        idle->running_cpu = TASK_CPU_NONE;
        if (idle->type == TASK_TYPE_PROCESS && idle->process)
            idle->process->state = (proc_state_t)PROC_BLOCKED;
    }
    spin_unlock_irqrestore(&task_lock, flags);

    smp_mark_shutdown_parked_cpu(cpu);
    KINFO("SMP: CPU%u parked for shutdown\n", cpu);

    for (;;) {
        arch_data_sync_barrier();
        arch_wait_for_interrupt();
    }
}



/**
 * Demarrer le scheduler (premiere commutation)
 */
void sched_start(void)
{
    if (!scheduler_initialized) {
        KERROR("Task system not initialized!\n");
        return;
    }
    
    //spin_lock(&task_lock);
    
    if (!task_list_head) {
        //spin_unlock(&task_lock);
        KERROR("No tasks to run!\n");
        return;
    }

    switch_to_idle();
        
/*     // La tache idle devient la tache courante
    current_task = idle_task;
    current_task->state = TASK_RUNNING;

    // Switch vers la pile d'idle MAINTENANT
    switch_to_idle_stack();
    
    spin_unlock(&task_lock);

    __task_switch(NULL, &current_task->context); */
    
    // On ne devrait jamais arriver ici
    KERROR("FATAL: Returned from sched_start!\n");
    while (1)
        arch_wait_for_interrupt();
}


static uint32_t scheduler_effective_priority_locked(task_t* task, uint32_t now)
{
    uint32_t base;
    uint32_t waited;
    uint32_t bonus;

    if (!task)
        return TASK_PRIORITY_LEVELS - 1;

    base = task_runqueue_priority(task);
    if (base == 0 || base >= TASK_PRIORITY_LEVELS)
        return base;

    if (task->ready_since_tick == 0 || now < task->ready_since_tick)
        return base;

    waited = now - task->ready_since_tick;
    bonus = waited / SCHED_AGING_STEP_TICKS;
    if (bonus > SCHED_AGING_MAX_BONUS)
        bonus = SCHED_AGING_MAX_BONUS;
    if (bonus > base)
        bonus = base;

    return base - bonus;
}

static uint32_t scheduler_debt_score_locked(task_t* task, uint32_t now)
{
    uint32_t waited;
    uint32_t decay;

    if (!task)
        return 0xffffffffu;

    if (task->ready_since_tick == 0 || now < task->ready_since_tick)
        return task->sched_debt;

    waited = now - task->ready_since_tick;
    decay = waited / SCHED_DEBT_DECAY_TICKS;
    if (decay >= task->sched_debt)
        return 0;

    return task->sched_debt - decay;
}

/**
 * Priority round-robin scheduler with bounded aging and CPU debt fairness.
 *
 * Lower numeric priorities still mean higher scheduling preference, and tasks
 * at the same base priority remain in FIFO queues. Selection is Linux-like in
 * spirit: we keep the fixed priority model, but also account for how much CPU
 * each runnable task has recently consumed. CPU-bound tasks build debt on timer
 * ticks; waiting tasks see that debt decay while queued, so they get a chance
 * to run without permanently rewriting their visible priority.
 */
static task_t* schedule_next_task(void)
{
    unsigned long flags;
    task_t* task;
    task_t* best;
    uint32_t best_prio;
    uint32_t best_debt;
    uint32_t best_waited;
    uint32_t now;
    uint32_t scanned = 0;
    uint32_t candidate_scan_count;
    uint32_t best_reason;
    bool best_was_head;

    spin_lock_irqsave(&task_lock, &flags);

    if (!runqueue_validate_locked("schedule_next_task")) {
        kernel_lifecycle_stats.scheduler_refused++;
        runqueue_rebuild_locked(NULL);
    }

    while (ready_queue.nr_running > 0 && scanned <= MAX_TASKS) {
restart_scan:
        best = NULL;
        best_prio = TASK_PRIORITY_LEVELS;
        best_debt = 0xffffffffu;
        best_waited = 0;
        best_was_head = true;
        best_reason = SCHED_PICK_NONE;
        candidate_scan_count = 0;
        now = get_system_ticks();

        for (uint32_t prio = 0; prio < TASK_PRIORITY_LEVELS; prio++) {
            task_t* candidate = ready_queue.head[prio];

            while (candidate) {
                task_t* next_candidate;
                uint32_t effective;
                uint32_t debt;
                uint32_t waited;
                uint32_t reason = SCHED_PICK_NONE;

                if (!task_header_plausible(candidate)) {
                    KERROR("schedule_next_task: corrupt runqueue entry %p at prio %u\n",
                           candidate, prio);
                    kernel_lifecycle_stats.scheduler_refused++;
                    sched_trace_record(SCHED_TRACE_REFUSE_INVALID_TASK, candidate);
                    runqueue_rebuild_locked(NULL);
                    scanned++;
                    goto restart_scan;
                }

                next_candidate = candidate->rq_next;
                if (candidate->running_cpu != TASK_CPU_NONE) {
                    candidate = next_candidate;
                    continue;
                }

                effective = scheduler_effective_priority_locked(candidate, now);
                debt = scheduler_debt_score_locked(candidate, now);
                waited = candidate->ready_since_tick && now >= candidate->ready_since_tick ?
                         now - candidate->ready_since_tick : 0;
                candidate_scan_count++;

                if (!best) {
                    reason = SCHED_PICK_PRIORITY;
                } else if (effective < best_prio) {
                    reason = effective < task_runqueue_priority(candidate) ?
                             SCHED_PICK_AGING : SCHED_PICK_PRIORITY;
                } else if (effective == best_prio && debt < best_debt) {
                    reason = SCHED_PICK_DEBT;
                } else if (effective == best_prio && debt == best_debt &&
                           waited > best_waited) {
                    reason = SCHED_PICK_WAIT;
                }

                if (reason != SCHED_PICK_NONE) {
                    best = candidate;
                    best_prio = effective;
                    best_debt = debt;
                    best_waited = waited;
                    best_was_head = (candidate == ready_queue.head[prio]);
                    best_reason = reason;
                }

                candidate = next_candidate;
            }
        }

        if (!best)
            break;

        task = best;
        scanned++;

        if (task_is_schedulable(task)) {
            process_t* proc = (task->type == TASK_TYPE_PROCESS) ? task->process : NULL;

            task->sched_debt = best_debt;
            if (best_prio < task_runqueue_priority(task))
                sched_aging_selections++;
            if (!best_was_head)
                sched_debt_selections++;
            sched_last_pick_reason = best_reason;
            sched_last_pick_tid = task->task_id;
            sched_last_pick_pid = proc ? (uint32_t)proc->pid : 0;
            sched_last_pick_priority = task_runqueue_priority(task);
            sched_last_pick_effective_priority = best_prio;
            sched_last_pick_debt = best_debt;
            sched_last_pick_waited_ticks = best_waited;
            sched_last_scan_tasks = candidate_scan_count;
            scheduler_mark_running_locked(task);
            spin_unlock_irqrestore(&task_lock, flags);
            return task;
        }

        /*
         * A stale runqueue entry means a task changed state without going
         * through the scheduler helpers. Drop it instead of letting a dead or
         * blocked task spin forever at the head of the queue.
         */
        runqueue_remove_locked(task);
        kernel_lifecycle_stats.scheduler_refused++;
        sched_trace_record(SCHED_TRACE_REFUSE_INVALID_TASK, task);
    }

    if (scanned > MAX_TASKS) {
        KERROR("schedule_next_task: runqueue loop protection triggered\n");
        kernel_lifecycle_stats.scheduler_refused++;
        sched_trace_record(SCHED_TRACE_REFUSE_LOOP, NULL);
    }

    idle_fallback_count[smp_processor_id()]++;
    task = task_idle_on_cpu(smp_processor_id());
    if (!task)
        task = idle_task;
    scheduler_mark_running_locked(task);
    spin_unlock_irqrestore(&task_lock, flags);
    return task;
}



static void add_task_to_list_locked(task_t* task)
{
    if (!task)
        return;

    /*
     * A task may be woken from several paths on SMP. Treat list insertion as
     * idempotent under task_lock; inserting the same node twice corrupts the
     * circular list and later makes scheduler_scan_waiters() walk into NULL.
     */
    if (task->next || task->prev || task == task_list_head)
        return;

    task->rq_next = NULL;
    task->rq_prev = NULL;
    task->rq_priority = TASK_PRIORITY_LEVELS;

    if (!task_list_head) {
        /* Premiere tache */
        task_list_head = task;
        task->next = task;
        task->prev = task;
    } else {
        /* Inserer a la fin */
        task->next = task_list_head;
        task->prev = task_list_head->prev;
        task_list_head->prev->next = task;
        task_list_head->prev = task;
    }
    task_count++;
}

/**
 * Ajouter une tache a la liste circulaire
 */
void add_task_to_list(task_t* task)
{
    unsigned long flags;

    if (!task)
        return;

    spin_lock_irqsave(&task_lock, &flags);
    add_task_to_list_locked(task);

    spin_unlock_irqrestore(&task_lock, flags);

}

/**
 * Retirer une tache de la liste circulaire
 */
void remove_task_from_list(task_t* task)
{
    task_t* current;
    bool found = false;
    int count = 0;
    unsigned long flags;

    if (!task || !task_list_head) return;

    spin_lock_irqsave(&task_lock, &flags);

    if (!task->next || !task->prev) {
        runqueue_remove_locked(task);
        spin_unlock_irqrestore(&task_lock, flags);
        return;
    }

    current = task_list_head;
    do {
        if (current == task) {
            found = true;
            break;
        }
        current = current->next;
        count++;
    } while (current && current != task_list_head && count < MAX_TASKS);

    if (!found) {
        runqueue_remove_locked(task);
        spin_unlock_irqrestore(&task_lock, flags);
        return;
    }

    runqueue_remove_locked(task);
    
    if (task->next == task) {
        /* Derniere tache */
        task_list_head = NULL;
    } else {
        task->prev->next = task->next;
        task->next->prev = task->prev;
        
        if (task_list_head == task) {
            task_list_head = task->next;
        }
    }
    
    task->next = NULL;
    task->prev = NULL;
    if (task_count > 0)
        task_count--;

    spin_unlock_irqrestore(&task_lock, flags);

}

/**
 * Fonction de la tache idle
 */
void idle_task_func(void* arg)
{
    (void)arg;
    uint32_t cpu = smp_processor_id();
    
    KINFO("Idle task started\n");
    
    while (1) {
        scheduler_enable_secondary_cpus_once();

        /*
         * Before timer preemption existed, idle yielded in a tight loop. With
         * periodic IRQs that causes idle->idle context-switch storms. We still
         * scan sleepers after each interrupt, but only enter the scheduler when
         * a real task became ready.
         */
        arch_enable_interrupts();
        arch_wait_for_interrupt();
        if (smp_shutdown_park_requested(cpu))
            secondary_idle_park_for_shutdown(cpu);
        scheduler_scan_waiters(task_current_local());
        if (scheduler_has_ready_work()) {
            if (cpu < ARMOS_MAX_CPUS) {
                idle_work_seen_count[cpu]++;
                idle_schedule_count[cpu]++;
            }
            schedule();
        }
    }
}


/**
 * Trouver une tache par ID
 */
task_t* task_find_by_id(uint32_t task_id)
{
    task_t* task = task_list_head;
    int count = 0;
    
    if (!task_list_head) return NULL;
    
    do {
        if (task->task_id == task_id) {
            return task;
        }
        task = task->next;
        count++;
    } while (task != task_list_head && count < MAX_TASKS);
    
    return NULL;
}

/**
 * Trouver une tache par nom
 */
task_t* task_find_by_name(const char* name)
{
    task_t* task = task_list_head;
    int count = 0;
    
    if (!task_list_head || !name) return NULL;
    
    do {
        if (strcmp(task->name, name) == 0) {
            return task;
        }
        task = task->next;
        count++;
    } while (task != task_list_head && count < MAX_TASKS);
    
    return NULL;
}

/**
 * Definir la priorite d'une tache
 */
void task_set_priority(task_t* task, uint32_t priority)
{
    unsigned long flags;
    task_t* current = task_current_local();
    bool queued;

    if (!task) return;
    
    spin_lock_irqsave(&task_lock, &flags);
    queued = runqueue_contains_locked(task);
    if (queued)
        runqueue_remove_locked(task);

    task->priority = priority;

    if (queued && task->state == TASK_READY)
        runqueue_enqueue_tail_locked(task);

    spin_unlock_irqrestore(&task_lock, flags);
    
    /* Si on change la priorite de la tache courante, re-scheduler */
    if (task == current) {
        schedule();
    }
}

/**
 * Obtenir la priorite d'une tache
 */
uint32_t task_get_priority(task_t* task)
{
    return task ? task->priority : 0;
}

/**
 * Definir l'etat d'une tache
 */
static proc_state_t task_state_to_proc_state(task_state_t state)
{
    switch (state) {
        case TASK_READY:           return (proc_state_t)PROC_READY;
        case TASK_RUNNING:         return (proc_state_t)PROC_RUNNING;
        case TASK_BLOCKED:         return (proc_state_t)PROC_BLOCKED;
        case TASK_ZOMBIE:          return (proc_state_t)PROC_ZOMBIE;
        case TASK_TERMINATED:      return (proc_state_t)PROC_DEAD;
        case TASK_INTERRUPTIBLE:   return (proc_state_t)PROC_INTERRUPTIBLE;
        case TASK_UNINTERRUPTIBLE: return (proc_state_t)PROC_UNINTERRUPTIBLE;
        case TASK_STOPPED:         return (proc_state_t)PROC_STOPPED;
    }
    return (proc_state_t)PROC_DEAD;
}

void task_set_state(task_t* task, task_state_t state)
{
    unsigned long flags;

    if (!task) return;
    
    spin_lock_irqsave(&task_lock, &flags);
    if (state == TASK_READY) {
        task_make_ready_under_lock(task);
        spin_unlock_irqrestore(&task_lock, flags);
        return;
    }

    if (task->state == TASK_RUNNING &&
        task->running_cpu != TASK_CPU_NONE &&
        task->running_cpu != smp_processor_id() &&
        state != TASK_RUNNING) {
        kernel_lifecycle_stats.ready_queue_refused++;
        sched_trace_record(SCHED_TRACE_READY_REFUSE_REMOTE_RUNNING, task);
        spin_unlock_irqrestore(&task_lock, flags);
        return;
    }

    runqueue_remove_locked(task);

    scheduler_clear_nonrunnable_debt_locked(task, state);
    task->state = state;
    if (state == TASK_RUNNING) {
        task->running_cpu = smp_processor_id();
        task->last_cpu = task->running_cpu;
    }
    /*
     * Do not clear running_cpu when the currently executing task marks itself
     * sleeping/blocking before calling schedule(). It still owns its kernel
     * stack until task_switch.S saves the final SVC frame and calls
     * task_context_save_complete().
     */
    if (task->type == TASK_TYPE_PROCESS && task->process) {
        proc_state_t proc_state = task_state_to_proc_state(state);
        if (task->process->state != proc_state)
            kernel_lifecycle_stats.state_sync_repairs++;
        task->process->state = proc_state;
    }
    spin_unlock_irqrestore(&task_lock, flags);
}

void task_set_blocked_under_lock(task_t* task)
{
    if (!task_header_plausible(task))
        return;

    if (task->state == TASK_RUNNING &&
        task->running_cpu != TASK_CPU_NONE &&
        task->running_cpu != smp_processor_id()) {
        kernel_lifecycle_stats.ready_queue_refused++;
        sched_trace_record(SCHED_TRACE_READY_REFUSE_REMOTE_RUNNING, task);
        return;
    }

    runqueue_remove_locked(task);
    scheduler_clear_nonrunnable_debt_locked(task, TASK_BLOCKED);
    task->state = TASK_BLOCKED;
    if (task->type == TASK_TYPE_PROCESS && task->process)
        task->process->state = (proc_state_t)PROC_BLOCKED;
}

void task_set_stopped_under_lock(task_t* task)
{
    if (!task_header_plausible(task))
        return;

    if (task->state == TASK_RUNNING &&
        task->running_cpu != TASK_CPU_NONE &&
        task->running_cpu != smp_processor_id()) {
        kernel_lifecycle_stats.ready_queue_refused++;
        sched_trace_record(SCHED_TRACE_READY_REFUSE_REMOTE_RUNNING, task);
        return;
    }

    runqueue_remove_locked(task);
    scheduler_clear_nonrunnable_debt_locked(task, TASK_STOPPED);
    task->state = TASK_STOPPED;
    if (task->type == TASK_TYPE_PROCESS && task->process)
        task->process->state = (proc_state_t)PROC_STOPPED;
}

static void task_set_sleep_state_until(task_t* task, task_state_t state,
                                       uint32_t wakeup_time)
{
    unsigned long flags;

    if (!task)
        return;

    spin_lock_irqsave(&task_lock, &flags);
    if (task->state == TASK_RUNNING &&
        task->running_cpu != TASK_CPU_NONE &&
        task->running_cpu != smp_processor_id()) {
        kernel_lifecycle_stats.ready_queue_refused++;
        sched_trace_record(SCHED_TRACE_READY_REFUSE_REMOTE_RUNNING, task);
        spin_unlock_irqrestore(&task_lock, flags);
        return;
    }

    runqueue_remove_locked(task);
    scheduler_clear_nonrunnable_debt_locked(task, state);
    task->state = state;
    task->wakeup_time = wakeup_time;
    if (task->type == TASK_TYPE_PROCESS && task->process) {
        proc_state_t proc_state = task_state_to_proc_state(state);
        if (task->process->state != proc_state)
            kernel_lifecycle_stats.state_sync_repairs++;
        task->process->state = proc_state;
    }
    spin_unlock_irqrestore(&task_lock, flags);
}

void task_set_wakeup_time(task_t* task, uint32_t wakeup_time)
{
    unsigned long flags;

    if (!task)
        return;

    spin_lock_irqsave(&task_lock, &flags);
    task->wakeup_time = wakeup_time;
    spin_unlock_irqrestore(&task_lock, flags);
}

void task_wake(task_t* task)
{
    unsigned long flags;

    if (!task)
        return;

    spin_lock_irqsave(&task_lock, &flags);
    task->wakeup_time = 0;
    task_make_ready_under_lock(task);
    spin_unlock_irqrestore(&task_lock, flags);
}

void task_set_ready(task_t* task)
{
    task_set_state(task, TASK_READY);
}

void task_set_blocked(task_t* task)
{
    task_set_state(task, TASK_BLOCKED);
}

void task_set_interruptible(task_t* task)
{
    task_set_state(task, TASK_INTERRUPTIBLE);
}

void task_set_interruptible_until(task_t* task, uint32_t wakeup_time)
{
    task_set_sleep_state_until(task, TASK_INTERRUPTIBLE, wakeup_time);
}

void task_set_uninterruptible(task_t* task)
{
    task_set_state(task, TASK_UNINTERRUPTIBLE);
}

void task_set_uninterruptible_until(task_t* task, uint32_t wakeup_time)
{
    task_set_sleep_state_until(task, TASK_UNINTERRUPTIBLE, wakeup_time);
}

void task_set_stopped(task_t* task)
{
    task_set_state(task, TASK_STOPPED);
}

void task_set_zombie(task_t* task)
{
    task_set_state(task, TASK_ZOMBIE);
}

void task_set_terminated(task_t* task)
{
    task_set_state(task, TASK_TERMINATED);
}

/**
 * Obtenir l'etat d'une tache
 */
task_state_t task_get_state(task_t* task)
{
    return task ? task->state : TASK_TERMINATED;
}

/**
 * Convertir un etat en chaine
 */
const char* task_state_string(task_state_t state)
{
    switch (state) {
        case TASK_READY: return "READY";
        case TASK_RUNNING: return "RUNNING";
        case TASK_BLOCKED: return "BLOCKED";
        case TASK_ZOMBIE: return "ZOMBIE";
        case TASK_TERMINATED: return "TERMINATED";
        case TASK_INTERRUPTIBLE: return "INTERRUPTIBLE";
        case TASK_UNINTERRUPTIBLE: return "UNINTERRUPTIBLE";
        case TASK_STOPPED: return "STOPPED";
        default: return "UNKNOWN";
    }
}


const char* proc_state_string(proc_state_t state)
{
    switch (state) {
        case PROC_READY: return "READY";
        case PROC_RUNNING: return "RUNNING";
        case PROC_BLOCKED: return "BLOCKED";
        case PROC_ZOMBIE: return "ZOMBIE";
        case PROC_DEAD: return "DEAD";
        case PROC_INTERRUPTIBLE: return "INTERRUPTIBLE";
        case PROC_UNINTERRUPTIBLE: return "UNINTERRUPTIBLE";
        case PROC_STOPPED: return "STOPPED";
        default: return "UNKNOWN";
    }
}


void task_dump_info(task_t* task)
{
    if (!task) {
        KINFO("task_dump_info: NULL task\n");
        return;
    }
    
    KINFO("=== Task Info ===\n");
    KINFO("  Name:         %s\n", task->name);
    KINFO("  ID:           %u\n", task->task_id);
    KINFO("  State:        %d\n", (int)task->state);
    KINFO("  Priority:     %u\n", task->priority);
    KINFO("  Stack base:   0x%lX\n",
          TASK_ADDR(task_stack_addr(task->stack_base)));
    KINFO("  Stack top:    0x%lX\n",
          TASK_ADDR(task_stack_addr(task->stack_top)));
    KINFO("  Stack size:   %u bytes\n", task->stack_size);
    KINFO("  Entry point:  0x%lX\n", TASK_ADDR(task->entry_point));
    KINFO("  Context SP:   0x%lX\n",
          TASK_ADDR(arch_task_context_kernel_sp(&task->context)));
    KINFO("  Context PC:   0x%lX\n",
          TASK_ADDR(arch_task_context_kernel_pc(&task->context)));
}

void task_dump_stacks(void)
{
    task_t* current = task_current_local();
    task_t* task;
    task_t* start_task;
    int count = 0;
    uint32_t total_stack_memory = 0;
    
    KINFO("=== Task Stack Analysis ===\n");
    KINFO("TaskID  Name         Stack Base   Stack Top    Size     Current SP   Gap to Next\n");
    KINFO("------  -----------  -----------  -----------  -------  -----------  -----------\n");
    
    if (!task_list_head) {
        KINFO("No tasks in list\n");
        return;
    }
    
    /* Parcourir toutes les taches */
    task = task_list_head;
    start_task = task_list_head;
    
    do {
        if (task && task->stack_base && task->stack_top) {
            vaddr_t stack_base = task_stack_addr(task->stack_base);
            vaddr_t stack_top = task_stack_addr(task->stack_top);
            vaddr_t sp = arch_task_context_kernel_sp(&task->context);

            KINFO("%-6u  %-11s  0x%lX   0x%lX   %-7u  0x%lX   ",
                  task->task_id,
                  task->name,
                  TASK_ADDR(stack_base),
                  TASK_ADDR(stack_top),
                  task->stack_size,
                  TASK_ADDR(sp));
            
            /* Calculer l'espace utilise dans la stack */
            uint32_t stack_used = (uint32_t)(stack_top - sp);
            uint32_t stack_free = task->stack_size - stack_used;
            
            /* Verifier la prochaine tache pour calculer l'ecart */
            task_t* next_task = task->next;
            if (next_task && next_task != start_task && next_task->stack_base) {
                int32_t gap = (int32_t)(task_stack_addr(next_task->stack_base) - stack_top);
                KINFO("%-11d\n", (int)gap);
                
                /* Verifier les chevauchements */
                if (stack_top > task_stack_addr(next_task->stack_base)) {
                    KINFO("        *** OVERLAP DETECTED! Stack collision! ***\n");
                }
            } else {
                KINFO("N/A\n");
            }
            
            /* Verifier les debordements de stack */
            if (sp < stack_base) {
                KINFO("        *** STACK UNDERFLOW! SP below base! ***\n");
            }
            if (sp >= stack_top) {
                KINFO("        *** STACK OVERFLOW! SP above top! ***\n");
            }
            
            /* Avertissement si stack presque pleine */
            if (stack_free < 512) {
                KINFO("        *** WARNING: Only %u bytes free in stack! ***\n", stack_free);
            }
            
            total_stack_memory += task->stack_size;
            count++;
        }
        
        task = task->next;
        
        /* Protection contre boucle infinie */
        if (count > 10) {
            KINFO("*** Loop detected, stopping dump ***\n");
            break;
        }
        
    } while (task && task != start_task);
    
    KINFO("\n=== Stack Summary ===\n");
    KINFO("Total tasks:      %d\n", count);
    KINFO("Total stack mem:  %u bytes (%u KB)\n", total_stack_memory, total_stack_memory / 1024);
    KINFO("Average per task: %u bytes\n", count > 0 ? total_stack_memory / count : 0);
    
    /* Afficher la tache courante */
    if (current) {
        vaddr_t current_sp = arch_task_context_kernel_sp(&current->context);

        KINFO("Current task:     %s (ID=%u)\n", current->name, current->task_id);
        KINFO("Current SP:       0x%lX\n", TASK_ADDR(current_sp));
        
        /* Verifier la stack de la tache courante */
        if (current_sp < task_stack_addr(current->stack_base) ||
            current_sp >= task_stack_addr(current->stack_top)) {
            KINFO("*** CURRENT TASK HAS INVALID SP! ***\n");
        }
    }
    
    KINFO("========================\n");
}

void task_check_stack_integrity(void)
{
    task_t* task;
    task_t* start_task;
    int issues = 0;
    
    KINFO("=== Stack Integrity Check ===\n");
    
    if (!task_list_head) {
        KINFO("No tasks to check\n");
        return;
    }
    
    task = task_list_head;
    start_task = task_list_head;
    
    do {
        if (task && task->stack_base && task->stack_top) {
            vaddr_t stack_base = task_stack_addr(task->stack_base);
            vaddr_t stack_top = task_stack_addr(task->stack_top);
            vaddr_t sp = arch_task_context_kernel_sp(&task->context);
            vaddr_t pc = arch_task_context_kernel_pc(&task->context);

            /* Verifier l'alignement des adresses */
            if (stack_base % 8 != 0) {
                KERROR("Task %s: Stack base not 8-byte aligned (0x%lX)\n",
                       task->name, TASK_ADDR(stack_base));
                issues++;
            }
            
            /* Verifier la taille de stack */
            uint32_t actual_size = (uint32_t)(stack_top - stack_base);
            if (actual_size != task->stack_size) {
                KERROR("Task %s: Stack size mismatch (expected %u, actual %u)\n",
                       task->name, task->stack_size, actual_size);
                issues++;
            }
            
            /* Verifier SP dans les limites */
            if (sp < stack_base) {
                KERROR("Task %s: SP underflow (SP=0x%lX, base=0x%lX)\n",
                       task->name, TASK_ADDR(sp), TASK_ADDR(stack_base));
                issues++;
            }
            
            if (sp >= stack_top) {
                KERROR("Task %s: SP overflow (SP=0x%lX, top=0x%lX)\n",
                       task->name, TASK_ADDR(sp), TASK_ADDR(stack_top));
                issues++;
            }
            
            /* Verifier que PC est dans une zone valide */
            if (pc != 0 && !memory_is_kernel_address(pc)) {
                KERROR("Task %s: Invalid PC (PC=0x%lX)\n",
                       task->name, TASK_ADDR(pc));
                issues++;
            }
        }
        
        task = task->next;
        
    } while (task && task != start_task);
    
    if (issues == 0) {
        KINFO("OK Stack integrity check passed - no issues found\n");
    } else {
        KERROR("KO Stack integrity check failed - %d issues found\n", issues);
    }
    
    KINFO("=============================\n");
}

void task_list_all(void)
{
    task_t* current = task_current_local();
    task_t* task;
    int count = 0;
    
    KINFO("=== Task List ===\n");
    
    if (!scheduler_initialized) {
        KINFO("Scheduler not initialized\n");
        return;
    }
    
    KINFO("Current task: %s (ID=%u)\n", 
          current ? current->name : "none",
          current ? current->task_id : 0);
    
    spin_lock(&task_lock);
    
    task = task_list_head;
    if (task) {
        do {
            KINFO("  [%d] %s (ID=%u, state=%s, priority=%u)\n",
                  count, task->name, task->task_id, task_state_string(task->state), task->priority);
            task = task->next;
            count++;
        } while (task != task_list_head && count < 100);
    }
    
    spin_unlock(&task_lock);
    
    KINFO("Total tasks: %d\n", count);
}

void task_sleep_ms(uint32_t ms)
{
    task_t* task = task_current_local();
    uint32_t ticks;

    if (!task || ms == 0) {
        yield();
        return;
    }

    if (!scheduler_initialized) {
        volatile uint32_t total = ms * 1000;
        for (volatile uint32_t i = 0; i < total; i++)
            arch_cpu_relax();
        return;
    }

    ticks = (ms * TIMER_FREQ + 999) / 1000;
    if (ticks == 0)
        ticks = 1;

    task_set_interruptible_until(task, get_system_ticks() + ticks);

    schedule();

    task_set_wakeup_time(task, 0);
}

int task_sleep_interruptible_ms(uint32_t ms)
{
    task_t *task = task_current_local();

    if (task && task->type == TASK_TYPE_PROCESS && task->process &&
        has_pending_signals(task))
        return -EINTR;

    task_sleep_ms(ms);

    task = task_current_local();
    if (task && task->type == TASK_TYPE_PROCESS && task->process &&
        has_pending_signals(task))
        return -EINTR;
    return 0;
}

/**
 * Obtenir le nombre de taches
 */
uint32_t task_get_count(void)
{
    return task_count;
}

/**
 * Afficher les statistiques globales
 */
void task_print_stats(void)
{
    task_t* current = task_current_local();

    KINFO("=== Task Statistics ===\n");
    KINFO("Total tasks:     %u\n", task_count);
    KINFO("Max tasks:       %u\n", MAX_TASKS);
    KINFO("Current task:    %s\n", current ? current->name : "none");
    KINFO("Scheduler:       %s\n", scheduler_initialized ? "running" : "stopped");
    KINFO("Next task ID:    %u\n", next_task_id);
}




/* Fonctions de debug C a appeler depuis l'assembleur */
void debug_context_switch_entry(void)
{
    KDEBUG("ASM: Entering __task_switch_asm_debug\n");
}

void debug_context_switch_middle(void)
{
    KDEBUG("ASM: Context saved, about to restore\n");
}

void debug_context_switch_first_exec(void)
{
    KDEBUG("ASM: First execution path - jumping to function\n");
}

void debug_context_switch_restore(void)
{
    KDEBUG("ASM: Suspended task path - returning to yield point\n");
}

void debug_null_old_ctx(void)
{
    KDEBUG("ASM: NULL old_ctx, using first switch\n");
}

void debug_null_new_ctx(void)
{
    KERROR("ASM: NULL new_ctx - CRITICAL ERROR!\n");
}



/* 1. DIAGNOSTIC: Ajouter debug detaille */
void debug_task_detailed(task_t *current_task)
{
    KDEBUG("  current_task pointer: %p\n", current_task);
    
    if (!current_task) {
        KERROR("  KO current_task is NULL!\n");
        return;
    }
    
    /* Verifier que le pointeur est dans une zone valide */
    vaddr_t current_task_addr = (vaddr_t)(uintptr_t)current_task;
    if (!memory_is_kernel_address(current_task_addr)) {
        KERROR("  KO current_task pointer invalid: %p\n", current_task);
        return;
    }
    
    KDEBUG("  ***************************************************************\n");
    KDEBUG("  Task name: %s\n", current_task->name);
    KDEBUG("  Task ID: %u\n", current_task->task_id);
    KDEBUG("  ***************************************************************\n");
    KDEBUG("  KERNEL STACK ---\n");
    KDEBUG("  Context SP: 0x%lX\n",
           TASK_ADDR(arch_task_context_kernel_sp(&current_task->context)));
    KDEBUG("  Stack base: 0x%lX\n",
           TASK_ADDR(task_stack_addr(current_task->stack_base)));
    KDEBUG("  Stack top:  0x%lX\n",
           TASK_ADDR(task_stack_addr(current_task->stack_top)));
    KDEBUG("  is_first_run: %u\n",
           arch_task_context_is_first_run(&current_task->context));
    KDEBUG("  --------------------------\n");
    
    /* Verification des limites de stack */
    vaddr_t sp = arch_task_context_kernel_sp(&current_task->context);
    vaddr_t base = task_stack_addr(current_task->stack_base);
    vaddr_t top = task_stack_addr(current_task->stack_top);
    
    if (sp >= base && sp < top) {
        KDEBUG("  OK KERNEL SP in valid range\n");
    } else {
        KERROR("  KO KERNEL SP OUT OF RANGE! (SP=0x%lX, range=0x%lX-0x%lX)\n",
               TASK_ADDR(sp), TASK_ADDR(base), TASK_ADDR(top));
    }

    if(current_task->process)
    {
        KDEBUG("  USER STACK ---\n");
        KDEBUG("  User SP: 0x%lX\n",
               TASK_ADDR(arch_task_context_user_sp(&current_task->context)));
        KDEBUG("  User Stack base: 0x%lX\n",
               TASK_ADDR(current_task->process->vm->stack_start));
        KDEBUG("  User Stack top:  0x%lX\n",
               TASK_ADDR(current_task->process->vm->stack_start +
                         USER_STACK_SIZE));
        KDEBUG("  --------------------------\n");
        
        /* Verification des limites de stack */
        sp = arch_task_context_user_sp(&current_task->context);
        base = current_task->process->vm->stack_start;
        top = current_task->process->vm->stack_start + USER_STACK_SIZE;
        
        if (sp >= base && sp < top) {
            KDEBUG("  OK USER SP in valid range\n");
        } else {
            KERROR("  KO USER SP OUT OF RANGE! (SP=0x%lX, range=0x%lX-0x%lX)\n",
                   TASK_ADDR(sp), TASK_ADDR(base), TASK_ADDR(top));
        }

    }

}


/* 1. DIAGNOSTIC: Ajouter debug detaille */
void debug_current_task_detailed(const char* location)
{
    task_t* task = task_current_local();

    KDEBUG("[%s] === current_task DEBUG ===\n", location);
    KDEBUG("  current_task pointer: %p\n", task);
    
    if (!task) {
        KERROR("  KO current_task is NULL!\n");
        return;
    }
    
    /* Verifier que le pointeur est dans une zone valide */
    if (!memory_is_kernel_address((vaddr_t)(uintptr_t)task)) {
        KERROR("  KO current_task pointer invalid: %p\n", task);
        return;
    }
    
    KDEBUG("  Task name: %s\n", task->name);
    KDEBUG("  Task ID: %u\n", task->task_id);
    KDEBUG("  Context SP: 0x%lX\n",
           TASK_ADDR(arch_task_context_kernel_sp(&task->context)));
    KDEBUG("  Stack base: 0x%lX\n",
           TASK_ADDR(task_stack_addr(task->stack_base)));
    KDEBUG("  Stack top:  0x%lX\n",
           TASK_ADDR(task_stack_addr(task->stack_top)));
    KDEBUG("  is_first_run: %u\n",
           arch_task_context_is_first_run(&task->context));
    
    /* Verification des limites de stack */
    vaddr_t sp = arch_task_context_kernel_sp(&task->context);
    vaddr_t base = task_stack_addr(task->stack_base);
    vaddr_t top = task_stack_addr(task->stack_top);
    
    if (sp >= base && sp < top) {
        KDEBUG("  OK SP in valid range\n");
    } else {
        KERROR("  KO SP OUT OF RANGE! (SP=0x%lX, range=0x%lX-0x%lX)\n",
               TASK_ADDR(sp), TASK_ADDR(base), TASK_ADDR(top));
    }
}

/**
 * Ajouter un processus a la queue des prets - utilisant vos fonctions
 */
void add_to_ready_queue(task_t* task)
{
    unsigned long flags;

    if (!task) return;

    /* Ne pas ajouter les zombies a la ready queue */
    if (task->state == TASK_ZOMBIE || task->state == TASK_TERMINATED) {
        //KDEBUG("add_to_ready_queue: Ignoring zombie/terminated task %s\n", 
        //       task->name);
        kernel_lifecycle_stats.ready_queue_refused++;
        sched_trace_record(SCHED_TRACE_READY_REFUSE_DEAD, task);
        return;
    }

    spin_lock_irqsave(&task_lock, &flags);

    /*
     * Fork creates the child object off-list. The global task-list insertion
     * and runqueue publication must be one atomic operation under task_lock:
     * otherwise two CPUs can both observe next/prev == NULL and splice the
     * same node twice into the circular task list.
     */
    add_task_to_list_locked(task);
    task_make_ready_under_lock(task);

    spin_unlock_irqrestore(&task_lock, flags);

    //KDEBUG("add_to_ready_queue: Task %s added to ready queue\n", task->name);
}

/**
 * Version securisee avec verifications supplementaires
 */
void remove_from_ready_queue(task_t* task)
{
    unsigned long flags;

    if (!task) {
        KERROR("remove_from_ready_queue: NULL task\n");
        return;
    }
    
    //KDEBUG("remove_from_ready_queue: Marking task %s as non-ready\n", task->name);
    spin_lock_irqsave(&task_lock, &flags);

    runqueue_remove_locked(task);
    if (task->state == TASK_READY) {
        task->state = TASK_BLOCKED;  /* ou autre etat approprie */
        if (task->type == TASK_TYPE_PROCESS && task->process)
            task->process->state = (proc_state_t)PROC_BLOCKED;
    }
    
    spin_unlock_irqrestore(&task_lock, flags);

    //KDEBUG("remove_from_ready_queue: Task %s marked as zombie (state=%d)\n", 
    //       task->name, task->state);
}

/**
 * Supprime definitivement un processus zombie du systeme
 * (appelee depuis sys_waitpid apres recolte)
 */
void destroy_zombie_process(task_t* zombie)
{
    if (!zombie || !zombie->process) {
        KERROR("destroy_zombie_process: NULL zombie\n");
        return;
    }
    
    KDEBUG("destroy_zombie_process: Destroying zombie %s (PID=%u)\n", 
           zombie->name, zombie->process->pid);
    
    /* Maintenant on peut vraiment le retirer de la liste */
    remove_task_from_list(zombie);
    
    /* Liberer les ressources */
    if (zombie->process->vm) {
        //destroy_vm_space(zombie->process->vm);
        zombie->process->vm = NULL;
    }
    
    task_free_kernel_stack(zombie);
    
    /* Mark the zombie as fully dead through the scheduler state helper. */
    task_set_terminated(zombie);
    
    /* Liberer la structure (optionnel, ou garder pour debug) */
    /* kfree(zombie); */
    
    KDEBUG("destroy_zombie_process: Zombie %s destroyed\n", zombie->name);
}

/**
 * Version avec debug et verifications d'integrite
 */
bool is_in_ready_queue(task_t* task)
{
    unsigned long flags;
    bool in_queue;

    if (!task) {
        KERROR("is_in_ready_queue: NULL task\n");
        return false;
    }

    spin_lock_irqsave(&task_lock, &flags);
    in_queue = task->state == TASK_READY && runqueue_contains_locked(task);
    spin_unlock_irqrestore(&task_lock, flags);
    return in_queue;
}

void debug_all_task_stacks(void)
{
    KINFO("=== DIAGNOSTIC TOUTES LES PILES ===\n");
    
    KINFO("Pile KERNEL:\n");
    vaddr_t kernel_start = (vaddr_t)(uintptr_t)&__stack_bottom;
    vaddr_t kernel_end = (vaddr_t)(uintptr_t)&__stack_top;
    KINFO("  Bottom: 0x%lX\n", TASK_ADDR(kernel_start));
    KINFO("  Top:    0x%lX\n", TASK_ADDR(kernel_end));
    KINFO("  Size:   %u bytes\n", (uint32_t)(kernel_end - kernel_start));
    
    task_t* task = task_list_head;
    if (!task) return;
    
    do {
        KINFO("Tache %s:\n", task->name);
        KINFO("  Stack base: 0x%lX\n",
              TASK_ADDR(task_stack_addr(task->stack_base)));
        KINFO("  Stack top:  0x%lX\n",
              TASK_ADDR(task_stack_addr(task->stack_top)));
        KINFO("  Stack size: %u bytes\n", task->stack_size);
        KINFO("  Context SP: 0x%lX\n",
              TASK_ADDR(arch_task_context_kernel_sp(&task->context)));
        
        /* Vérifier chevauchement avec pile kernel */
        vaddr_t task_start = task_stack_addr(task->stack_base);
        vaddr_t task_end = task_stack_addr(task->stack_top);
        
        if ((task_start < kernel_end && task_end > kernel_start)) {
            KERROR("  KO CONFLIT avec pile kernel !\n");
        } else {
            KINFO("  OK Pas de conflit\n");
        }
        
        task = task->next;
    } while (task != task_list_head);
}
