/*
 * ArmOS
 * Copyright (c) 2026 Mohamed Ennassiri
 *
 * Licensed under the Apache License, Version 2.0.
 * See LICENSE for details.
 *
 * File: include/kernel/process.h
 * Layer: Kernel / public internal interface
 *
 * Responsibilities:
 * - Declare kernel types, constants, and subsystem contracts.
 * - Keep cross-module ABI and structure expectations explicit.
 *
 * Notes:
 * - Header changes can ripple across kernel and user ABI glue.
 */

#ifndef _KERNEL_PROCESS_H
#define _KERNEL_PROCESS_H

#include <kernel/task.h>        /* Votre task.h complet avec process_t defini */
#include <kernel/memory.h>      /* Votre memory.h existant */
#include <kernel/signal.h>      /* Votre signal.h existant */
#include <kernel/types.h>

/* etats de processus - mapping vers vos etats existants */
#define PROC_READY           TASK_READY
#define PROC_RUNNING         TASK_RUNNING
#define PROC_BLOCKED         TASK_BLOCKED
#define PROC_ZOMBIE          TASK_ZOMBIE
#define PROC_DEAD            TASK_TERMINATED
#define PROC_INTERRUPTIBLE   TASK_INTERRUPTIBLE
#define PROC_UNINTERRUPTIBLE TASK_UNINTERRUPTIBLE
#define PROC_STOPPED         TASK_STOPPED

/* Constantes adaptees */
#define PROC_NAME_MAX   TASK_NAME_MAX
#define MAX_PROCESSES   MAX_TASKS

/* Variables globales - utilisant vos declarations existantes */
extern task_t* current_task;        /* Deja defini dans votre task.h */
extern task_t* init_process;        /* Deja defini dans votre task.h */

/* Fonctions process - UTILISANT task_t, PAS process_t */
task_t* create_process(const char* name);
void destroy_process(task_t* process);
task_t* find_process_by_pid(pid_t pid);
size_t process_snapshot_live_pids(pid_t* pids, size_t capacity,
                                  pid_t exclude_pid, bool include_init);
process_t* get_current_process(void);
task_t* get_current_task(void);
void add_to_ready_queue(task_t* task);

/* Fonctions pour fork - utilisant task_t */
task_t* task_create_copy(task_t* parent, bool from_user);
vm_space_t* fork_vm_space(vm_space_t* parent_vm);

/* Fonctions de fichiers - UTILISANT task_t */
void close_cloexec_files(task_t* process);

/* Fonctions d'initialisation */
void init_process_system(void);
void init_kernel_task_system(void);
void process_release_init(void);
void init_main(void);
void list_all_processes(void);

/* Fonction shell de demonstration */
void simple_shell_task(void* arg);
void init_process_main(void* arg);

/* Extensions memoire pour processus */
void zero_fill_bss(vm_space_t* vm, vaddr_t vaddr, uint32_t size);
bool is_valid_user_ptr(const void* ptr);

extern void add_to_ready_queue(task_t* task);

void wakeup_parent(task_t *proc);
void wakeup_parent_under_lock(task_t *proc);
void copy_process_files(task_t* parent, task_t* child);
void close_all_process_files(task_t* proc);
bool has_children(task_t* parent, pid_t pid);
task_t* find_zombie_child(task_t* parent, pid_t pid);
task_t* find_zombie_child_locked(task_t* parent, pid_t pid);
void remove_child_from_parent(task_t* parent, task_t* child_to_remove);

#endif
