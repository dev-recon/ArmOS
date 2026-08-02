/*
 * ArmOS
 * Copyright (c) 2026 Mohamed Ennassiri
 *
 * Licensed under the Apache License, Version 2.0.
 * See LICENSE for details.
 *
 * File: kernel/process/fork.c
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
#include <kernel/syscalls.h>
#include <kernel/memory.h>
#include <kernel/vfs.h>
#include <kernel/kprintf.h>

/* Forward declarations de toutes les fonctions statiques */
task_t* find_zombie_child(task_t* parent, pid_t pid);
bool has_children(task_t* parent, pid_t pid);
void remove_child_from_parent(task_t* parent, task_t* child);
void orphan_children(task_t* proc);
/* Supprime: static bool has_pending_signals(process_t* proc); */
void copy_process_files(task_t* parent, task_t* child);


/* ========================================================================= */
/* FONCTIONS HELPER - Adaptees a task_t */
/* ========================================================================= */

/**
 * Copier les fichiers ouverts - ADAPTe
 */
void copy_process_files(task_t* parent, task_t* child)
{
    int i;
    
    if (!parent || !child || !parent->process || !child->process ||
        parent->type != TASK_TYPE_PROCESS || 
        child->type != TASK_TYPE_PROCESS) {
        KERROR("copy_process_files: NULL Proc\n");
        return;
    }
    
    for (i = 0; i < MAX_FILES; i++) {
        if (child->process->files[i]) {
            close_file(child->process->files[i]);
            child->process->files[i] = NULL;
            child->process->fd_flags[i] = 0;
        }
    }

    for (i = 0; i < MAX_FILES; i++) {
        if (parent->process->files[i]) {
            child->process->files[i] = get_file(parent->process->files[i]);
            if (child->process->files[i])
                child->process->fd_flags[i] = parent->process->fd_flags[i];
            else
                child->process->fd_flags[i] = 0;
        } else {
            child->process->fd_flags[i] = 0;
        }
    }
}

void close_all_process_files(task_t* proc) {

    if(!proc || !proc->process){
        KERROR("Wake up : no parent or no process structure\n");
        KERROR("NULL PROC\n");
        return;
    }

    for (int i = 0; i < MAX_FILES; i++) {
        if (proc->process->files[i]) {
            close_file(proc->process->files[i]);
            proc->process->files[i] = NULL;
            proc->process->fd_flags[i] = 0;
        }
    }
}

/**
 * Retourner au code appelant avec une valeur - ADAPTe
 */
void return_to_caller_with_value(int return_value)
{
    task_t* proc = task_current_local();
    
    if (!proc || proc->type != TASK_TYPE_PROCESS) {
        KERROR("[WAITPID] Invalid process\n");
        KERROR("NULL PROC\n");
        return;
    }
    
    KDEBUG("[WAITPID] Retour avec valeur: %d\n", return_value);
    
    arch_task_context_set_kernel_return_value(&proc->context, (uint32_t)return_value);
    
    /* Utiliser l'adresse stockee lors de l'appel initial - ACCeS CORRECT */
    if (proc->process->waitpid_caller_lr != 0) {
        arch_task_context_set_kernel_pc(&proc->context,
                                        (vaddr_t)proc->process->waitpid_caller_lr);
        KDEBUG("[WAITPID] PC mis a caller_lr: 0x%08X\n", proc->process->waitpid_caller_lr);
    } else {
        /* Fallback: utiliser LR si disponible */
        vaddr_t lr = arch_task_context_kernel_lr(&proc->context);

        if (lr != 0) {
            arch_task_context_set_kernel_pc(&proc->context, lr);
            KDEBUG("[WAITPID] PC set to LR: 0x%lX\n",
                   (unsigned long)lr);
        } else {
            KDEBUG("[WAITPID] Ni caller_lr ni LR disponible - continuation normale\n");
        }
    }
}



/**
 * Nettoyer la liste des enfants - ADAPTe
 */
void clean_children_list(task_t* parent)
{
    task_t* child;
    task_t* prev = NULL;
    int count = 0;
    const int MAX_CHILDREN = 50;
    
    if (!parent || parent->type != TASK_TYPE_PROCESS || !parent->process) {
        KERROR("clean_children_list: NULL Proc\n");
        return;
    }
    
    /* ACCeS CORRECT */
    child = parent->process->children;
    
    while (child && count < MAX_CHILDREN) {
        task_t* next = child->process->sibling_next;
        
        /* Verifier la coherence */
        if (next == child) {
            KERROR("[WAITPID] Boucle circulaire detectee - correction\n");
            child->process->sibling_next = NULL;
            break;
        }
        
        /* Verifier que l'enfant a bien ce parent */
        if (child->process->parent != parent) {
            KERROR("[WAITPID] Enfant orphelin detecte - correction\n");
            if (prev) {
                prev->process->sibling_next = next;
            } else {
                parent->process->children = next;
            }
        } else {
            prev = child;
        }
        
        child = next;
        count++;
    }
    
    if (count >= MAX_CHILDREN) {
        KERROR("[WAITPID] Trop d'enfants - nettoyage force\n");
        parent->process->children = NULL;
    }
}


static bool child_matches_waitpid(task_t* parent, task_t* child, pid_t pid)
{
    if (!parent || !parent->process || !child || !child->process)
        return false;

    if (pid == -1)
        return true;
    if (pid > 0)
        return child->process->pid == pid;
    if (pid == 0)
        return child->process->pgid == parent->process->pgid;

    return child->process->pgid == -pid;
}

task_t* find_zombie_child_locked(task_t* parent, pid_t pid)
{
    task_t* child;

    if (!parent || parent->type != TASK_TYPE_PROCESS || !parent->process)
        return NULL;

    child = parent->process->children;

    while (child) {
        /*
         * sys_exit() publishes TASK_ZOMBIE before switching away from the
         * exiting task's kernel stack.  A WNOHANG waiter can poll during that
         * handoff window, so do not expose the child as reapable until the
         * context switch has released CPU ownership and the scheduler has
         * cleared the deferred-wakeup marker.  Reaping earlier frees the VM
         * and kernel stack while another CPU can still be executing on them.
         */
        if (child_matches_waitpid(parent, child, pid) &&
            child->state == TASK_ZOMBIE &&
            child->process->state == (proc_state_t)PROC_ZOMBIE &&
            child->running_cpu == TASK_CPU_NONE &&
            child->wakeup_time == 0) {
            return child;
        }
        child = child->process->sibling_next;
    }

    return NULL;
}

task_t* find_zombie_child(task_t* parent, pid_t pid)
{
    unsigned long flags;
    task_t* zombie;

    if (!parent || parent->type != TASK_TYPE_PROCESS || !parent->process)
        return NULL;

    spin_lock_irqsave(&task_lock, &flags);
    zombie = find_zombie_child_locked(parent, pid);
    spin_unlock_irqrestore(&task_lock, flags);

    return zombie;
}


bool has_children(task_t* parent, pid_t pid)
{
    unsigned long flags;
    bool found = false;
    task_t* child;

    if (!parent) {
        KERROR("has_children: NULL Parent\n");
        return false;
    }

    if (parent->type != TASK_TYPE_PROCESS) {
        KERROR("has_children: Parent not Process Task\n");
        return false;
    }

    if (!parent->process) {
        KERROR("has_children: NULL Proc\n");
        return false;
    }

    spin_lock_irqsave(&task_lock, &flags);
    child = parent->process->children;

    while (child) {
        if (child_matches_waitpid(parent, child, pid)) {
            found = true;
            break;
        }
        child = child->process->sibling_next;
    }
    spin_unlock_irqrestore(&task_lock, flags);

    return found;
}


/**
 * Retirer un enfant de la liste du parent - ADAPTe
 */
bool remove_child_from_parent_locked(task_t* parent,
                                     task_t* child_to_remove)
{
    if (!parent || !child_to_remove ||
        parent->type != TASK_TYPE_PROCESS ||
        child_to_remove->type != TASK_TYPE_PROCESS ||
        !parent->process || !child_to_remove->process)
        return false;

    if (parent->process->children == child_to_remove) {
        parent->process->children = child_to_remove->process->sibling_next;
    } else {
        task_t* current = parent->process->children;
        int count = 0;
        
        while (current && current->process &&
               current->process->sibling_next != child_to_remove &&
               count < MAX_TASKS) {
            current = current->process->sibling_next;
            count++;
        }
        
        if (current && current->process && count < MAX_TASKS) {
            current->process->sibling_next = child_to_remove->process->sibling_next;
        } else {
            return false;
        }
    }

    child_to_remove->process->sibling_next = NULL;
    child_to_remove->process->parent = NULL;
    return true;
}

void remove_child_from_parent(task_t* parent, task_t* child_to_remove)
{
    unsigned long flags;
    bool removed;

    if (!parent || !child_to_remove ||
        parent->type != TASK_TYPE_PROCESS ||
        child_to_remove->type != TASK_TYPE_PROCESS ||
        !parent->process || !child_to_remove->process) {
        KERROR("remove_child_from_parent: NULL Proc\n");
        return;
    }

    spin_lock_irqsave(&task_lock, &flags);
    removed = remove_child_from_parent_locked(parent, child_to_remove);
    spin_unlock_irqrestore(&task_lock, flags);

    if (!removed) {
        KERROR("[REMOVE_CHILD] Child PID %u not found under parent PID %u\n",
               child_to_remove->process->pid, parent->process->pid);
    }
}

/**
 * Orpheliner les enfants vers init - ADAPTe
 */
void orphan_children(task_t* proc)
{
    unsigned long flags;
    task_t* init_proc;
    task_t* child;
    task_t* next;
    bool wake_init = false;
    
    if (!proc || proc->type != TASK_TYPE_PROCESS || !proc->process) {
        KERROR("NULL PROC\n");
        return;
    }
    
    init_proc = find_process_by_pid(1);
    if (!init_proc) {
        KERROR("orphan_children: Init process not found\n");
        return;
    }
    
    /* ACCeS CORRECT */
    spin_lock_irqsave(&task_lock, &flags);
    child = proc->process->children;
    
    while (child) {
        next = child->process->sibling_next;
        
        /* Reassigner a init - ACCeS CORRECT */
        child->process->parent = init_proc;
        child->process->ppid = 1;
        child->process->sibling_next = init_proc->process->children;
        init_proc->process->children = child;
        
        /* Reveiller init si l'enfant est zombie */
        if (child->state == TASK_ZOMBIE && init_proc->state == TASK_BLOCKED)
            wake_init = true;
        
        child = next;
    }
    
    proc->process->children = NULL;
    spin_unlock_irqrestore(&task_lock, flags);

    if (wake_init)
        task_set_ready(init_proc);
}


void wakeup_parent_under_lock(task_t *proc)
{
    task_t *parent;

    if (!proc || !proc->process)
        return;

    /*
     * Parent wakeup is a single scheduler transaction.  The parent pointer,
     * waitpid selector and child relationship are observed while task_lock is
     * held, then the parent is made READY without recursively taking task_lock.
     */
    parent = proc->process->parent;
    if (parent && parent->process &&
        parent->process->waitpid_active &&
        parent->state == TASK_BLOCKED &&
        parent->process->state == (proc_state_t)PROC_BLOCKED) {
        pid_t wait_pid = parent->process->waitpid_pid;
        if (child_matches_waitpid(parent, proc, wait_pid))
            task_make_ready_under_lock(parent);
    }
}

void wakeup_parent(task_t *proc)
{
    unsigned long flags;

    spin_lock_irqsave(&task_lock, &flags);
    wakeup_parent_under_lock(proc);
    spin_unlock_irqrestore(&task_lock, flags);
}
