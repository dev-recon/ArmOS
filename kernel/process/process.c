/*
 * ArmOS
 * Copyright (c) 2026 Mohamed Ennassiri
 *
 * Licensed under the Apache License, Version 2.0.
 * See LICENSE for details.
 *
 * File: kernel/process/process.c
 * Layer: Kernel / process lifecycle
 *
 * Responsibilities:
 * - Manage process ownership, exec/fork/exit semantics, and signals.
 * - Maintain Unix-like parent/child and zombie invariants.
 *
 * Notes:
 * - Changes here can affect init reaping and job control.
 */

#include <kernel/process.h>
#include <kernel/memory.h>
#include <kernel/panic.h>
#include <kernel/vfs.h>
#include <kernel/syscalls.h>
#include <kernel/uart.h>
#include <kernel/kprintf.h>
#include <kernel/fat32.h>
#include <kernel/string.h>
#include <kernel/task.h>
#include <kernel/signal.h>
#include <kernel/timer.h>
#include <kernel/smp.h>
#include <kernel/arch_memory.h>



/* Variables globales - reutilisant vos declarations */
extern task_t* current_task;      /* Defini dans votre task.c */
extern task_t* task_list_head;    /* Defini dans votre task.c */
extern uint32_t task_count;       /* Defini dans votre task.c */
extern spinlock_t task_lock; /* Defini dans votre task.c */
extern task_t* idle_task;         /* Defini dans votre task.c */
extern task_t* init_process;      /* Defini dans votre task.c */
extern void switch_to_idle_stack(void);

static bool init_process_released;

/**
 * Point d'entree principal du kernel - adapte a votre architecture
 */
void init_main(void)
{
    /* Initialiser le systeme unifie process/task */
    init_process_system();
    
    /* Lancer votre scheduler existant */
    sched_start(); 
    
    /* Ne devrait jamais arriver ici */
    panic("Returned from unified scheduler!");
}

/**
 * Initialiser le systeme unifie process/task
 */
static void configure_idle_task_for_cpu(uint32_t cpu_id, task_t* task)
{
    if (!task)
        return;

    task_register_idle_cpu(cpu_id, task);
    arch_task_context_mark_first_run(&task->context);
    arch_task_context_set_address_space(&task->context,
                                        arch_kernel_address_space_context(),
                                        ASID_KERNEL);
    arch_task_context_set_returns_to_user(&task->context, false);
}

static void create_idle_tasks(void)
{
    extern void idle_task_func(void* arg);

    idle_task = task_create_process("idle", idle_task_func, NULL, 255, TASK_TYPE_KERNEL);
    if (!idle_task) {
        panic("Failed to create idle task");
    }
    configure_idle_task_for_cpu(smp_boot_cpu_id(), idle_task);

    for (uint32_t cpu = 0; cpu < smp_possible_cpu_count(); cpu++) {
        char idle_name[TASK_NAME_MAX];
        task_t* secondary_idle;

        if (cpu == smp_boot_cpu_id())
            continue;

        snprintf(idle_name, sizeof(idle_name), "idle%u", cpu);
        secondary_idle = task_create_process(idle_name, idle_task_func, NULL,
                                             TASK_IDLE_PRIORITY, TASK_TYPE_KERNEL);
        if (!secondary_idle) {
            KWARN("SMP: failed to create idle task for CPU%u\n", cpu);
            continue;
        }

        configure_idle_task_for_cpu(cpu, secondary_idle);
        /*
         * Secondary idle tasks are not runnable from the global runqueue, but
         * they must be fully published before a secondary CPU can switch to its
         * private idle context and before /proc walks the task list.
         */
        add_task_to_list(secondary_idle);
        task_set_blocked(secondary_idle);
    }
}

void init_kernel_task_system(void)
{
    init_signal_stack_allocator();
    init_task_system();
    create_idle_tasks();
    add_to_ready_queue(idle_task);
}

void init_process_system(void)
{
    /* Initialiser le gestionnaire de signal stacks */
    init_signal_stack_allocator();
    
    /* Initialiser d'abord votre systeme de taches de base existant */
    init_task_system();
    init_process_released = false;

    /* Creer le processus init (PID 1) */
    init_process = task_create_process("init", init_process_main, NULL,
                                       TASK_DEFAULT_PRIORITY, TASK_TYPE_PROCESS);
    //init_process = create_process("init");
    if (!init_process) {
        panic("Failed to create init process");
    }
    
    /* Forcer PID 1 pour init */
    //init_process->process.pid = 1;
    //next_pid = 2;  /* Prochain PID sera 2 */
    
    //init_process->process.ppid = 0;
    //init_process->process.parent = NULL;
    //init_process->entry_point = init_process_main;
    //init_process->entry_arg = NULL;

    //init_process->context.sp = new_vm->stack_start;             /* Stack pointer */
    arch_task_context_mark_first_run(&init_process->context);
    arch_task_context_set_returns_to_user(&init_process->context, false);

    //KDEBUG("[INIT] SCV STACK TOP = 0x%08X\n", init_process->context.svc_sp_top);
    //KDEBUG("[INIT] SCV STACK SP = 0x%08X\n", init_process->context.svc_sp);
     
    /* Configurer le contexte d'init avec votre fonction existante */
    //setup_task_context(init_process);

    create_idle_tasks();


    /* PID 1 is released after secondary scheduler admission completes. */
    add_to_ready_queue(idle_task);

}

void process_release_init(void)
{
    if (!init_process || init_process_released)
        return;

    init_process_released = true;
    add_to_ready_queue(init_process);
}

/**
 * Fonction principale du processus init - adaptee a votre structure
 */
void init_process_main(void* arg)
{
    (void)arg;
    const char* init_path = "/sbin/init";
    char* init_name = "init";
    char* const init_argv[] = { init_name, NULL };
    char* const init_envp[] = {
        "PATH=/sbin:/bin:/usr/bin:/opt/kilo/bin",
        "HOME=/home/user",
        "USER=user",
        "PS1=mash$> ",
        NULL
    };
    int result;
    
    /* First milestone toward a Unix-like userspace init: PID 1 execs /sbin/init. */
    init_process->process->uid = 0;
    init_process->process->gid = 0;
    strcpy(init_process->process->cwd, "/home/user");

    KBOOT_OKF("Init: starting %s", init_path);
    result = sys_execve(init_path, init_argv, init_envp);

    KWARN("Userland init exec failed with %d, falling back to kernel launcher\n",
          result);
    
    /* Fallback legacy path: create a kernel-launched shell process. */
    task_t* shell_proc  = task_create_process("shell", simple_shell_task, NULL,
                                              TASK_DEFAULT_PRIORITY, TASK_TYPE_PROCESS);
    if (!shell_proc) {
        panic("Failed to create fallback shell process");
    }

    arch_task_context_mark_first_run(&shell_proc->context);
    arch_task_context_set_returns_to_user(&shell_proc->context, false);
    shell_proc->process->uid = 1000;
    shell_proc->process->gid = 1000;
    strcpy(shell_proc->process->cwd, "/home/user");

    add_to_ready_queue(shell_proc);
    
    /* Legacy kernel reaper loop used only if /sbin/init could not exec. */
    int reaped_count = 0;

    while (1) {
        bool has_children = (init_process->process->children != NULL);
        
        if (has_children) {
            /* Attendre n'importe quel enfant */
            int status;
            pid_t child_pid = kernel_waitpid(-1, &status, 0, init_process);
            
            if (child_pid > 0) {
                reaped_count++;
                //KDEBUG("[INIT] Reaped orphan child PID=%d (status=%d) [total=%d]\n",
                //       child_pid, status, reaped_count);
            }
        } else {
            /* PAS D'ENFANTS : Ne pas faire waitpid */
            //KDEBUG("[INIT] No children, sleeping...\n");
            task_sleep_ms(1000);  /* Dormir au lieu de waitpid */
        }
        
        /* Status periodique */
        static int status_counter = 0;
        if (++status_counter % 500 == 0) {
            //KINFO("[INIT] Status: %d children reaped, system running\n", reaped_count);
            //list_all_processes();
        }
        
        /* Pause courte */
        task_sleep_ms(200);
        yield();
    }
}



/**
 * Creer un processus - CORRIGe
 */
task_t* create_process(const char* name)
{
    task_t* process;
    
    /* Creer la tache de base avec votre fonction existante */
    extern task_t* task_create_process(const char* name, void (*entry)(void* arg), 
                                  void* arg, uint32_t priority, task_type_t type);

    process = task_create_process(name, NULL, NULL,
                                  TASK_DEFAULT_PRIORITY, TASK_TYPE_PROCESS);
    if (!process) {
        KERROR("create_process: NULL PROC\n");
        return NULL;
    }
    
    return process;
}

/**
 * Detruire un processus - CORRIGe
 */
void destroy_process(task_t* process)
{
    int i;
    
    if (!process || process->type != TASK_TYPE_PROCESS || !process->process) {
        KERROR("destroy_process: Invalid process\n");
        KERROR("destroy_process: NULL PROC\n");
        return;
    }
    
    //KDEBUG("destroy_process: Destroying process %s (PID=%u)\n", process->name, process->process->pid);
    
    /* Fermer tous les fichiers - ACCeS CORRECT */
    for (i = 0; i < MAX_FILES; i++) {
        if (process->process->files[i]) {
            close_file(process->process->files[i]); 
            process->process->files[i] = NULL;
            process->process->fd_flags[i] = 0;
        }
    }

     /* Nettoyer les signaux */
    cleanup_process_signals(process);
    
    /*
     * Detach the VM pointer while holding task_lock before freeing it.
     * /proc readers walk task/process state under task_lock; without this
     * ordering they can observe a vm_space whose VMA list is being freed.
     */
    {
        vm_space_t* old_vm = NULL;
        unsigned long flags;

        spin_lock_irqsave(&task_lock, &flags);
        old_vm = process->process->vm;
        process->process->vm = NULL;
        spin_unlock_irqrestore(&task_lock, flags);

        if (old_vm)
            destroy_vm_space(old_vm);
    }
    
    /* Utiliser votre fonction de destruction de tache existante */
    task_destroy(process);
}

/**
 * Trouver un processus par PID - CORRIGe
 */
task_t* find_process_by_pid(pid_t pid)
{
    task_t* task = task_list_head;
    int count = 0;
    unsigned long flags;
    
    if (!task_list_head) return NULL;
    
    spin_lock_irqsave(&task_lock, &flags);
    
    do {
        /* ACCeS CORRECT via l'union */
        if (task->type == TASK_TYPE_PROCESS && task->process->pid == pid) {
            spin_unlock_irqrestore(&task_lock, flags);
            return task;
        }
        task = task->next;
        count++;
    } while (task != task_list_head && count < MAX_TASKS);
    
    spin_unlock_irqrestore(&task_lock, flags);
    return NULL;
}

size_t process_snapshot_live_pids(pid_t* pids, size_t capacity,
                                  pid_t exclude_pid, bool include_init)
{
    task_t* task;
    size_t count = 0;
    uint32_t walked = 0;
    unsigned long flags;

    spin_lock_irqsave(&task_lock, &flags);
    task = task_list_head;
    if (task) {
        do {
            task_t* next = task->next;

            if (task->type == TASK_TYPE_PROCESS && task->process &&
                task->state != TASK_ZOMBIE &&
                task->state != TASK_TERMINATED &&
                task->process->pid != exclude_pid &&
                (include_init || task->process->pid != 1)) {
                if (pids && count < capacity)
                    pids[count] = task->process->pid;
                count++;
            }
            task = next;
            walked++;
        } while (task && task != task_list_head && walked < MAX_TASKS);
    }
    spin_unlock_irqrestore(&task_lock, flags);
    return count;
}

/**
 * Obtenir la tache courante
 */
task_t* get_current_task(void)
{
    task_t *task = task_current_local();

    if (task) {
        return task;
    }
    KERROR("get_current_task: current task is NULL");
    return NULL;
}

/**
 * Obtenir le processus courant
 */
process_t* get_current_process(void)
{
    return task_get_process(task_current_local());
}
