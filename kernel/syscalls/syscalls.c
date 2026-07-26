/*
 * ArmOS
 * Copyright (c) 2026 Mohamed Ennassiri
 *
 * Licensed under the Apache License, Version 2.0.
 * See LICENSE for details.
 *
 * File: kernel/syscalls/syscalls.c
 * Layer: Kernel / syscall implementation
 *
 * Responsibilities:
 * - Validate user-facing syscall requests.
 * - Bridge user ABI arguments to kernel subsystems.
 *
 * Notes:
 * - Never trust user pointers; copy through checked helpers.
 */

#include <kernel/syscalls.h>
#include <kernel/process.h>
#include <kernel/vfs.h>
#include <kernel/kprintf.h>
#include <kernel/stddef.h>
#include <kernel/string.h>
#include <kernel/task.h>
#include <kernel/exec.h>
#include <kernel/userspace.h>
#include <kernel/shm.h>
#include <kernel/power.h>
#include <kernel/tty.h>
#include <kernel/signal.h>
#include <kernel/file.h>
#include <kernel/mount.h>
#include <kernel/virtio_net.h>
#include <kernel/arch_barrier.h>
#include <kernel/arch_cpu.h>
#include <kernel/timer.h>

/* Syscall table */
typedef syscall_word_t (*syscall_func_t)(
    syscall_word_t, syscall_word_t, syscall_word_t,
    syscall_word_t, syscall_word_t, syscall_word_t);

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wcast-function-type"

static syscall_func_t syscall_table[MAX_SYSCALLS] = {
    [__NR_exit] = (syscall_func_t)sys_exit,
    [__NR_fork] = (syscall_func_t)sys_fork,
    [__NR_clone] = (syscall_func_t)sys_clone,
    [__NR_set_tls] = (syscall_func_t)sys_set_tls,
    [__NR_get_tls_info] = (syscall_func_t)sys_get_tls_info,
    [__NR_socketpair] = (syscall_func_t)sys_socketpair,
    [__NR_sendmsg] = (syscall_func_t)sys_sendmsg,
    [__NR_recvmsg] = (syscall_func_t)sys_recvmsg,
    [__NR_read] = (syscall_func_t)sys_read,
    [__NR_write] = (syscall_func_t)sys_write,
    [__NR_open] = (syscall_func_t)sys_open,
    [__NR_creat] = (syscall_func_t)sys_creat,
    [__NR_close] = (syscall_func_t)sys_close,
    [__NR_waitpid] = (syscall_func_t)sys_waitpid,
    [__NR_mount] = (syscall_func_t)sys_mount,
    [__NR_umount] = (syscall_func_t)sys_umount,
    [__NR_link] = (syscall_func_t)sys_link,
    [__NR_execve] = (syscall_func_t)sys_execve,
    [__NR_lseek] = (syscall_func_t)sys_lseek,
    [__NR_time] = (syscall_func_t)sys_time,
    [__NR_mknod] = (syscall_func_t)sys_mknod,
    [__NR_alarm] = (syscall_func_t)sys_alarm,
    [__NR_pause] = (syscall_func_t)sys_pause,
    [__NR_utime] = (syscall_func_t)sys_utime,
    [__NR_getpid] = (syscall_func_t)sys_getpid,
    [__NR_gettid] = (syscall_func_t)sys_gettid,
    [__NR_getppid] = (syscall_func_t)sys_getppid,
    [__NR_setuid] = (syscall_func_t)sys_setuid,
    [__NR_getuid] = (syscall_func_t)sys_getuid,
    [__NR_geteuid] = (syscall_func_t)sys_geteuid,
    [__NR_setgid] = (syscall_func_t)sys_setgid,
    [__NR_getgid] = (syscall_func_t)sys_getgid,
    [__NR_getegid] = (syscall_func_t)sys_getegid,
    [__NR_nice] = (syscall_func_t)sys_nice,
    [__NR_times] = (syscall_func_t)sys_times,
    [__NR_getpriority] = (syscall_func_t)sys_getpriority,
    [__NR_setpriority] = (syscall_func_t)sys_setpriority,
    [__NR_getrusage] = (syscall_func_t)sys_getrusage,
    [__NR_setpgid] = (syscall_func_t)sys_setpgid,
    [__NR_getpgrp] = (syscall_func_t)sys_getpgrp,
    [__NR_setsid] = (syscall_func_t)sys_setsid,
    [__NR_kill] = (syscall_func_t)sys_kill,
    [__NR_signal] = (syscall_func_t)sys_signal,
    [__NR_sigaction] = (syscall_func_t)sys_sigaction,
    [__NR_setrlimit] = (syscall_func_t)sys_setrlimit,
    [__NR_getrlimit] = (syscall_func_t)sys_getrlimit,
    [__NR_sigsuspend] = (syscall_func_t)sys_sigsuspend,
    [__NR_sigpending] = (syscall_func_t)sys_sigpending,
    [__NR_ioctl] = (syscall_func_t)sys_ioctl,
    [__NR_fcntl] = (syscall_func_t)sys_fcntl,
    [__NR_print] = (syscall_func_t)sys_print,
    [__NR_rt_sigreturn] = (syscall_func_t)sys_sigreturn,
    [__NR_brk] = (syscall_func_t)sys_brk,
    [__NR_rename] = (syscall_func_t)sys_rename,
    [__NR_mkdir] = (syscall_func_t)sys_mkdir,
    [__NR_rmdir] = (syscall_func_t)sys_rmdir,
    [__NR_symlink] = (syscall_func_t)sys_symlink,
    [__NR_readlink] = (syscall_func_t)sys_readlink,
    [__NR_truncate] = (syscall_func_t)sys_truncate,
    [__NR_ftruncate] = (syscall_func_t)sys_ftruncate,
    [__NR_fsync] = (syscall_func_t)sys_fsync,
    [__NR_fdatasync] = (syscall_func_t)sys_fdatasync,
    [__NR_statfs] = (syscall_func_t)sys_statfs,
    [__NR_statvfs] = (syscall_func_t)sys_statvfs,
    [__NR_fstatvfs] = (syscall_func_t)sys_fstatvfs,
    [__NR_utimensat] = (syscall_func_t)sys_utimensat,
    [__NR_futimens] = (syscall_func_t)sys_futimens,
    [__NR_unlink] = (syscall_func_t)sys_unlink,
    [__NR_access] = (syscall_func_t)sys_access,
    [__NR_sync] = (syscall_func_t)sys_sync,
    [__NR_umask] = (syscall_func_t)sys_umask,
    [__NR_chmod] = (syscall_func_t)sys_chmod,
    [__NR_fchmod] = (syscall_func_t)sys_fchmod,
    [__NR_chown] = (syscall_func_t)sys_chown,
    [__NR_fchown] = (syscall_func_t)sys_fchown,
    [__NR_dup2] = (syscall_func_t)sys_dup2,
    [__NR_dup] = (syscall_func_t)sys_dup,
    [__NR_getcwd] = (syscall_func_t)sys_getcwd,
    [__NR_chdir] = (syscall_func_t)sys_chdir,
    [__NR_pipe] = (syscall_func_t)sys_pipe,
    [__NR_stat] = (syscall_func_t)sys_stat,
    [__NR_lstat] = (syscall_func_t)sys_lstat,
    [__NR_fstat] = (syscall_func_t)sys_fstat,
    [__NR_getdents] = (syscall_func_t)sys_getdents,
    [__NR_select] = (syscall_func_t)sys_select,
    [__NR_readv] = (syscall_func_t)sys_readv,
    [__NR_writev] = (syscall_func_t)sys_writev,
    [__NR_pread] = (syscall_func_t)sys_pread,
    [__NR_pwrite] = (syscall_func_t)sys_pwrite,
    [__NR_poll] = (syscall_func_t)sys_poll,
    [__NR_stty]     = (syscall_func_t)sys_stty,
    [__NR_gtty]     = (syscall_func_t)sys_gtty,
    [__NR_gettimeofday] = (syscall_func_t)sys_gettimeofday,
    [__NR_uname] = (syscall_func_t)sys_uname,
    [__NR_sigprocmask] = (syscall_func_t)sys_sigprocmask,
    [__NR_getsid] = (syscall_func_t)sys_getsid,
    [__NR_sched_yield] = (syscall_func_t)sys_sched_yield,
    [__NR_nanosleep] = (syscall_func_t)sys_nanosleep,
    [__NR_clock_gettime] = (syscall_func_t)sys_clock_gettime,
    [__NR_clock_getres] = (syscall_func_t)sys_clock_getres,
    [__NR_clock_nanosleep] = (syscall_func_t)sys_clock_nanosleep,
    [__NR_openat] = (syscall_func_t)sys_openat,
    [__NR_mkdirat] = (syscall_func_t)sys_mkdirat,
    [__NR_fstatat] = (syscall_func_t)sys_fstatat,
    [__NR_unlinkat] = (syscall_func_t)sys_unlinkat,
    [__NR_renameat] = (syscall_func_t)sys_renameat,
    [__NR_sysinfo]   = (syscall_func_t)sys_sysinfo,
    [__NR_shm_open]  = (syscall_func_t)sys_shm_open,
    [__NR_shm_unlink] = (syscall_func_t)sys_shm_unlink,
    [__NR_shm_map]   = (syscall_func_t)sys_shm_map,
    [__NR_shm_unmap] = (syscall_func_t)sys_shm_unmap,
    [__NR_shutdown]  = (syscall_func_t)sys_shutdown,
    [__NR_mmap]      = (syscall_func_t)sys_mmap,
    [__NR_munmap]    = (syscall_func_t)sys_munmap,
    [__NR_sysconf]   = (syscall_func_t)sys_sysconf,
    [__NR_mprotect]  = (syscall_func_t)sys_mprotect,
    [__NR_wait4]     = (syscall_func_t)sys_wait4,
    [__NR_socket]    = (syscall_func_t)sys_socket,
    [__NR_bind]      = (syscall_func_t)sys_bind,
    [__NR_connect]   = (syscall_func_t)sys_connect,
    [__NR_listen]    = (syscall_func_t)sys_listen,
    [__NR_accept]    = (syscall_func_t)sys_accept,
    [__NR_sendto]    = (syscall_func_t)sys_sendto,
    [__NR_recvfrom]  = (syscall_func_t)sys_recvfrom,
    [__NR_socket_shutdown] = (syscall_func_t)sys_socket_shutdown,
    [__NR_resolve]   = (syscall_func_t)sys_resolve,
    [__NR_thread_exit] = (syscall_func_t)sys_thread_exit,
    [__NR_futex]       = (syscall_func_t)sys_futex,

};

#pragma GCC diagnostic pop

/* Forward declarations for low-level task-switch helpers. */
extern void orphan_children(task_t* proc);
extern void switch_to_idle_stack(void);

extern void __task_switch_to_user(task_context_t* new_ctx);
extern void __task_switch(task_context_t* old_ctx, task_context_t* new_ctx);

static void cleanup_failed_fork_child(task_t* parent, task_t* child)
{
    task_t* iter;

    if (!child)
        return;

    if (parent && parent->process) {
        if (parent->process->children == child) {
            parent->process->children = child->process ? child->process->sibling_next : NULL;
        } else {
            iter = parent->process->children;
            while (iter && iter->process && iter->process->sibling_next != child)
                iter = iter->process->sibling_next;

            if (iter && iter->process)
                iter->process->sibling_next = child->process ? child->process->sibling_next : NULL;
        }
    }

    if (child->process) {
        if (child->process->vm) {
            destroy_vm_space(child->process->vm);
            child->process->vm = NULL;
        }
        kfree(child->process);
        child->process = NULL;
    }

    task_free_kernel_stack(child);

    child->magic = TASK_MAGIC_DEAD;
    kfree(child);
    kernel_lifecycle_stats.tasks_destroyed++;
}

static int syscall_tty_id_from_fd(int fd)
{
    file_t *file;

    task_t *task = task_current_local();

    if (!task || !task->process)
        return -EINVAL;
    if (fd < 0 || fd >= MAX_FILES)
        return -EBADF;

    file = task->process->files[fd];
    if (!file)
        return -EBADF;
    if (!file_is_tty(file))
        return -ENOTTY;

    return tty_id_from_file(file);
}

int sys_stty(int cmd, uint32_t arg, uint32_t arg2)
{
    int tty_id;

    switch (cmd) {
    case TTY_STTY_SET_FOREGROUND_PGID:
        return tty_set_foreground_pgid((pid_t)arg);
    case TTY_STTY_SET_FOREGROUND_PGID_FD:
        tty_id = syscall_tty_id_from_fd((int)arg);
        if (tty_id < 0)
            return tty_id;
        return tty_set_foreground_pgid_for_id(tty_id, (pid_t)arg2);
    default:
        return -EINVAL;
    }
}

int sys_gtty(int cmd, uint32_t arg)
{
    int tty_id;

    switch (cmd) {
    case TTY_GTTY_GET_FOREGROUND_PGID:
        return tty_get_foreground_pgid();
    case TTY_GTTY_GET_FOREGROUND_PGID_FD:
        tty_id = syscall_tty_id_from_fd((int)arg);
        if (tty_id < 0)
            return tty_id;
        return tty_get_foreground_pgid_for_id(tty_id);
    default:
        return -EINVAL;
    }
}

static void rename_task_from_exec_path(task_t *task, const char *path)
{
    const char *base;

    if (!task || !path || !path[0])
        return;

    base = path;
    for (const char *p = path; *p; p++) {
        if (*p == '/' && p[1])
            base = p + 1;
    }

    strncpy(task->name, base, TASK_NAME_MAX - 1);
    task->name[TASK_NAME_MAX - 1] = '\0';
}

static size_t snapshot_exec_vector(char *dst, size_t cap, char **vec)
{
    size_t len = 0;

    if (!dst || cap == 0)
        return 0;

    dst[0] = '\0';
    if (!vec)
        return 0;

    for (uint32_t i = 0; vec[i]; i++) {
        size_t slen = strlen(vec[i]) + 1;
        if (len + slen > cap)
            break;
        memcpy(dst + len, vec[i], slen);
        len += slen;
    }

    return len;
}

static void snapshot_exec_metadata(task_t *task, const char *filename,
                                   char **argv, char **envp)
{
    process_t *proc;

    if (!task || !task->process)
        return;

    proc = task->process;

    strncpy(proc->exe_path, filename ? filename : task->name, MAX_PATH - 1);
    proc->exe_path[MAX_PATH - 1] = '\0';

    proc->cmdline_len = snapshot_exec_vector(proc->cmdline,
                                             sizeof(proc->cmdline), argv);
    if (proc->cmdline_len == 0 && filename) {
        strncpy(proc->cmdline, filename, sizeof(proc->cmdline) - 1);
        proc->cmdline[sizeof(proc->cmdline) - 1] = '\0';
        proc->cmdline_len = strlen(proc->cmdline) + 1;
    }

    proc->environ_len = snapshot_exec_vector(proc->environ,
                                             sizeof(proc->environ), envp);
}

void dump_svc_stack(task_t *task, uint32_t *sp) {
    kprintf("SVC stack @%p:\n", sp);
    for (int i=0;i<12;i++) {
        kprintf(" +%02x: %08x\n", i*4, sp[i]);
    }
    kprintf("Offset of context = %lu\n",
            (unsigned long)((uintptr_t)&task->context - (uintptr_t)task));
}

void print_cpu_mode(void){
    uint32_t mode = arch_current_mode();
    
    kprintf("\n\n**************************************\n");
    kprintf("Current CPU MODE = Mode: 0x%02X, -->: %s\n", 
            mode, arch_mode_name(mode));
    kprintf("**************************************\n\n");

}

static int count_exec_vector(char* const vector[], bool from_user, uint32_t* count)
{
    uint32_t i;

    *count = 0;
    if (!vector)
        return 0;

    for (i = 0; i < 32; i++) {
        char* value;

        if (from_user) {
            if (copy_from_user(&value, &vector[i], sizeof(value)) < 0)
                return -EFAULT;
        } else {
            value = vector[i];
        }

        if (!value) {
            *count = i;
            return 0;
        }
    }

    return -E2BIG;
}

static char** copy_exec_vector(char* const vector[], uint32_t count, bool from_user)
{
    char** copy = kmalloc((count + 1) * sizeof(char*));
    uint32_t i;

    if (!copy)
        return NULL;

    memset(copy, 0, (count + 1) * sizeof(char*));

    for (i = 0; i < count; i++) {
        char* value;

        if (from_user) {
            if (copy_from_user(&value, &vector[i], sizeof(value)) < 0)
                goto fail;
            copy[i] = copy_string_from_user(value);
        } else {
            value = vector[i];
            copy[i] = value ? strdup(value) : NULL;
        }

        if (!copy[i])
            goto fail;
    }

    return copy;

fail:
    cleanup_exec_args(NULL, copy, NULL);
    return NULL;
}

/**
 * sys_execve - Executer un nouveau programme - ADAPTe
 */
int sys_execve(const char* filename, char* const argv[], char* const envp[])
{
    task_t* proc = task_current_local();
    char* kernel_filename;
    char** kernel_argv;
    char** kernel_envp;
    inode_t* exe_inode;
    vaddr_t entry;
    vm_space_t* old_vm;
    vm_space_t* new_vm;
    uid_t exec_uid;
    gid_t exec_gid;
    mode_t exec_mode;
    exec_image_layout_t image_layout;
    uint32_t argc = 0;
    uint32_t envpc = 0;
    int result;
    
    /* Verification processus - ADAPTe */
    if (!proc || proc->type != TASK_TYPE_PROCESS || !proc->process) {
        KERROR("sys_execve: Current task is not a process\n");
        KERROR("sys_execve: NULL Proc\n");
        return -EINVAL;
    }

        /* Verification processus - ADAPTe */
    if (!filename) {
        KERROR("sys_execve: filename is NULL\n");
        return -EINVAL;
    }

    /*
     * The task context is the canonical syscall-origin record. On AArch64,
     * SPSR_EL1 is not banked across a nested EL1 IRQ, so reading it here can
     * misclassify an EL0 syscall as a kernel call after timer preemption.
     */
    bool from_user = arch_task_context_returns_to_user(&proc->context);

    result = count_exec_vector(argv, from_user, &argc);
    if (result < 0)
        return result;

    result = count_exec_vector(envp, from_user, &envpc);
    if (result < 0)
        return result;

    kernel_filename = from_user ? copy_string_from_user(filename) : strdup(filename);
    if (!kernel_filename)
        return -EFAULT;

    kernel_argv = copy_exec_vector(argv, argc, from_user);
    if (!kernel_argv) {
        kfree(kernel_filename);
        return -ENOMEM;
    }

    kernel_envp = copy_exec_vector(envp, envpc, from_user);
    if (!kernel_envp) {
        cleanup_exec_args(kernel_filename, kernel_argv, NULL);
        return -ENOMEM;
    }

    /* Ouvrir le fichier executable */
    exe_inode = path_lookup(kernel_filename);
    if (!exe_inode) {
        //KDEBUG("sys_execve: File not found: %s\n", kernel_filename);
        cleanup_exec_args(kernel_filename, kernel_argv, kernel_envp);
        return -ENOENT;
    }

    if (!inode_permission(exe_inode, MAY_EXEC)) {
        put_inode(exe_inode);
        cleanup_exec_args(kernel_filename, kernel_argv, kernel_envp);
        return -EACCES;
    }

    exec_uid = exe_inode->uid;
    exec_gid = exe_inode->gid;
    exec_mode = exe_inode->mode;

    /* Sauvegarder l'ancien espace memoire pour rollback - ACCeS CORRECT */
    old_vm = proc->process->vm;
    
    /* Creer un nouvel espace memoire */
    new_vm = create_vm_space();
    if (!new_vm) {
        KERROR("sys_execve: Failed to create new VM space\n");
        put_inode(exe_inode);
        cleanup_exec_args(kernel_filename, kernel_argv, kernel_envp);
        return -ENOMEM;
    }
    
    /* Configurer la pile utilisateur avec arguments */
    if (setup_user_stack(new_vm, kernel_argv, kernel_envp) < 0) {
        KERROR("sys_execve: Failed to setup user stack\n");
        destroy_vm_space(new_vm);
        put_inode(exe_inode);
        cleanup_exec_args(kernel_filename, kernel_argv, kernel_envp);
        return -ENOMEM;
    }

    /* Load through the common VFS/VM path and active ELF ABI parser. */
    memset(&image_layout, 0, sizeof(image_layout));
    if (exec_load_image(exe_inode, new_vm, &entry, &image_layout) < 0) {
        KERROR("sys_execve: Failed to load ELF segments\n");
        destroy_vm_space(new_vm);
        put_inode(exe_inode);
        cleanup_exec_args(kernel_filename, kernel_argv, kernel_envp);
        return -ENOEXEC;
    }
    
    /*
     * La signal stack est mappee dans le TTBR0 courant mais n'est pas une VMA.
     * Elle doit donc etre liberee avant de detruire l'ancien espace, puis
     * recreee dans le nouveau vm_space apres exec.
     */
    cleanup_process_signals(proc);

    /*
     * Publish the new VM under task_lock before freeing the old one. /proc and
     * sysinfo readers hold task_lock while taking VM snapshots; this prevents
     * them from walking a VMA list that execve() is about to destroy.  The
     * actual free happens after switching TTBR0 away from old_vm.
     */
    {
        unsigned long vm_flags;

        spin_lock_irqsave(&task_lock, &vm_flags);
        old_vm = proc->process->vm;
        proc->process->vm = new_vm;
        proc->process->tls_image = image_layout.tls_image;
        proc->process->tls_file_size =
            (size_t)image_layout.tls_file_size;
        proc->process->tls_memory_size =
            (size_t)image_layout.tls_memory_size;
        proc->process->tls_alignment =
            (size_t)image_layout.tls_alignment;
        spin_unlock_irqrestore(&task_lock, vm_flags);
    }
    init_process_signals(proc);

    /* Reinitialiser le contexte CPU - ADAPTe a VOTRE STRUCTURE */
    memset(&proc->context, 0, sizeof(task_context_t));

    arch_task_context_init_user_entry(&proc->context,
                                      (uintptr_t)new_vm->pgdir,
                                      new_vm->asid,
                                      (vaddr_t)(uintptr_t)proc->stack_top,
                                      entry,
                                      new_vm->stack_start);

    /* Arguments initiaux (argc, argv, etc.) */
    arch_task_context_set_user_register(&proc->context, 0,
                                        (uintptr_t)kernel_filename);
    arch_task_context_set_user_register(&proc->context, 1,
                                        (uintptr_t)kernel_argv);
    arch_task_context_set_user_register(&proc->context, 2,
                                        (uintptr_t)kernel_envp);
    arch_task_context_set_user_register(&proc->context, 3, argc);

   /* Fermer tous les fichiers CLOEXEC - ACCeS CORRECT */
    close_cloexec_files(proc);

    switch_to_vm_space(new_vm);
    destroy_vm_space(old_vm);

    //tlb_flush_all_debug();
    arch_data_memory_barrier();
    arch_instruction_sync_barrier();

    rename_task_from_exec_path(proc, kernel_filename);
    if (exec_mode & S_ISUID)
        proc->process->uid = exec_uid;
    if (exec_mode & S_ISGID)
        proc->process->gid = exec_gid;
    snapshot_exec_metadata(proc, kernel_filename, kernel_argv, kernel_envp);

    /* Nettoyer les ressources temporaires */
    put_inode(exe_inode);
    cleanup_exec_args(kernel_filename, kernel_argv, kernel_envp);

    //__task_switch_to_user(&proc->context);
    __task_switch(NULL, &proc->context);

    /* Cette fonction ne retourne JAMAIS */
    __builtin_unreachable();
}


/**
 * sys_fork corrige - ACCeS CORRECT a la structure
 */
int sys_fork(void)
{
    task_t* parent = task_current_local();
    task_t* child;
    uint32_t return_address;
    uint32_t saved_status;
    bool from_user;
    
    if (!parent || parent->type != TASK_TYPE_PROCESS) {
        KERROR("sys_fork: Current task is not a process\n");
        KERROR("sys_fork: NULL Parent\n");
        return -EINVAL;
    }

    return_address = arch_current_link_register();
    saved_status = arch_task_context_user_status(&parent->context);
    from_user = arch_task_context_returns_to_user(&parent->context);
    
    /* Creer le processus enfant en copiant le parent */
    child = task_create_copy(parent, from_user);
    if (!child) {
        KERROR("sys_fork: Failed to create child process\n");
        kernel_lifecycle_stats.failed_forks++;
        return -ENOMEM;
    }

    /* Copier l'espace memoire avec COW - ACCeS CORRECT */
    if (child->process->vm) {
        destroy_vm_space(child->process->vm);
    }

    child->process->vm = fork_vm_space(parent->process->vm);
    if (!child->process->vm) {
        KERROR("sys_fork: Failed to copy VM space\n");
        cleanup_failed_fork_child(parent, child);
        kernel_lifecycle_stats.failed_forks++;
        return -ENOMEM;
    }

    /* Configuration des relations parent-enfant apres creation VM reussie. */
    {
        unsigned long child_flags;

        spin_lock_irqsave(&task_lock, &child_flags);
        child->process->parent = parent;
        child->process->sibling_next = parent->process->children;
        parent->process->children = child;
        spin_unlock_irqrestore(&task_lock, child_flags);
    }
    
    /* Copier les descripteurs de fichiers */
    copy_process_files(parent, child);

    init_process_signals(child);

    if( from_user )
    {
        arch_task_context_prepare_user_fork(&child->context,
                                            &parent->context,
                                            (uintptr_t)child->process->vm->pgdir,
                                            child->process->vm->asid,
                                            return_address,
                                            saved_status);
    }
    else{
        arch_task_context_prepare_kernel_fork(&child->context,
                                              &parent->context,
                                              (uintptr_t)child->process->vm->pgdir,
                                              child->process->vm->asid,
                                              return_address,
                                              saved_status);
    }

    add_to_ready_queue(child);
    
    return child->process->pid;
}


void sys_exit(int status)
{
    task_t* proc = task_current_local();

    if (!proc) {
        KERROR("sys_exit: No current task\n");
        KERROR("sys_exit: NULL Proc\n");
        return;
    }

    if (proc->type == TASK_TYPE_THREAD) {
        sys_thread_exit(status);
    }

    if (proc->type != TASK_TYPE_PROCESS || !proc->process) {
        KERROR("[EXIT] sys_exit called from non-process task! Name = %s\n", proc->name);
        return;
    }
    
    if (proc->type != TASK_TYPE_PROCESS) {
        //KDEBUG("sys_exit: Kernel task %s terminating\n", proc->name);
        task_destroy(proc);
        return;
    }

    int irq_flags = disable_interrupts_save();
    //set_critical_section();
    //KDEBUG("EXITING TASK %s - Status = %d, with state = %s\n", proc->name, status, task_state_string(proc->state));

    unsigned long task_flags;
    spin_lock_irqsave(&task_lock, &task_flags);
    /* CORRECTION: États cohérents avec sys_waitpid */
    if (proc->process->term_signal > 0) {
        proc->process->exit_code = 0;
    } else {
        proc->process->exit_code = status & 0xff;
        proc->process->term_signal = 0;
    }
    //task_set_state(proc, TASK_ZOMBIE);
    if (proc->state != TASK_ZOMBIE) {
        proc->sched_debt = 0;
        proc->ready_since_tick = 0;
        proc->state = TASK_ZOMBIE;           /* Pas TASK_TERMINATED ! */
        proc->process->state = (proc_state_t)PROC_ZOMBIE;
        /*
         * SMP exit ordering:
         * the parent must not reap/free this task while sys_exit() is still
         * executing on the child's kernel stack. The scheduler will wake the
         * parent after the child has switched away from that stack.
         */
        proc->wakeup_time = get_system_ticks() + 1;
        kernel_lifecycle_stats.zombies_created++;
    }
    spin_unlock_irqrestore(&task_lock, task_flags);

    /* Fermer tous les fichiers ouverts avant de reveiller le parent */
    close_all_process_files(proc);

    /* Retirer le processus zombie de la ready queue */
    remove_from_ready_queue(proc);

    /* Orpheliner tous les enfants vers init (PID 1) - ACCeS CORRECT */
    orphan_children(proc);

    /*
     * Ne pas reactiver les IRQ avant d'avoir quitte la pile du zombie: le
     * parent reveille pourrait sinon liberer proc et sa pile kernel. En SMP,
     * le reveil est differe par scheduler_scan_waiters() pour garantir cet
     * ordre meme si le parent tourne sur un autre CPU.
     */
    (void)irq_flags;
    switch_to_idle();

    /* Cette ligne ne devrait jamais s'executer */
    KERROR("sys_exit: FATAL - Zombie process PID=%u was rescheduled!\n", 
           proc->process->pid);
    
    /* Boucle d'urgence pour eviter la corruption */
    while (1) {
        arch_wait_for_interrupt();
    }
}



static task_t* find_stopped_child_locked(task_t* parent, pid_t pid)
{
    task_t* child;

    if (!parent || parent->type != TASK_TYPE_PROCESS || !parent->process)
        return NULL;

    child = parent->process->children;
    while (child && child->process) {
        int matches = (pid == -1) ||
                      (pid > 0 && child->process->pid == pid) ||
                      (pid == 0 && child->process->pgid == parent->process->pgid) ||
                      (pid < -1 && child->process->pgid == -pid);

        if (matches &&
            child->state == TASK_STOPPED &&
            child->process->state == (proc_state_t)PROC_STOPPED &&
            !child->process->stop_reported) {
            return child;
        }
        child = child->process->sibling_next;
    }

    return NULL;
}

static pid_t consume_stopped_child(task_t* parent, pid_t pid, int* status)
{
    unsigned long flags;
    task_t* stopped;
    pid_t stopped_pid = 0;

    spin_lock_irqsave(&task_lock, &flags);
    stopped = find_stopped_child_locked(parent, pid);
    if (stopped) {
        if (status)
            *status = 0x7f | ((stopped->process->stop_signal & 0xff) << 8);
        stopped->process->stop_reported = 1;
        stopped_pid = stopped->process->pid;
    }
    spin_unlock_irqrestore(&task_lock, flags);

    return stopped_pid;
}

static bool waitpid_prepare_sleep(task_t* parent, pid_t pid, int options,
                                  int* status, pid_t* stopped_pid)
{
    unsigned long flags;
    task_t* stopped = NULL;

    if (stopped_pid)
        *stopped_pid = 0;

    spin_lock_irqsave(&task_lock, &flags);

    /*
     * Close the zombie handoff race atomically with publishing the blocked
     * state.  The scheduler can make a child reapable between the unlocked
     * scan in kernel_waitpid() and this point; sleeping without this recheck
     * would lose that wakeup and leave both parent and zombie stranded.
     */
    if (find_zombie_child_locked(parent, pid)) {
        spin_unlock_irqrestore(&task_lock, flags);
        return false;
    }

    /*
     * Avoid a lost wakeup with WUNTRACED: the child may stop between the
     * caller's last unlocked scan and the moment the parent publishes its
     * wait state. Re-check and consume the stop report in the same critical
     * section that blocks the parent.
     */
    if (options & WUNTRACED) {
        stopped = find_stopped_child_locked(parent, pid);
        if (stopped) {
            if (status)
                *status = 0x7f | ((stopped->process->stop_signal & 0xff) << 8);
            stopped->process->stop_reported = 1;
            if (stopped_pid)
                *stopped_pid = stopped->process->pid;
            spin_unlock_irqrestore(&task_lock, flags);
            return false;
        }
    }

    if (has_pending_signals(parent)) {
        parent->process->waitpid_pid = 0;
        parent->process->waitpid_status = NULL;
        parent->process->waitpid_options = 0;
        spin_unlock_irqrestore(&task_lock, flags);
        return false;
    }

    parent->process->waitpid_pid = pid;
    parent->process->waitpid_status = status;
    parent->process->waitpid_options = options;
    parent->process->waitpid_iteration++;
    task_set_blocked_under_lock(parent);

    spin_unlock_irqrestore(&task_lock, flags);
    return true;
}

int kernel_waitpid(pid_t pid, int* status, int options, task_t* parent)
{
    task_t* zombie = NULL;
    //uint32_t iteration = 1;

    if (!parent || parent->type != TASK_TYPE_PROCESS || !parent->process) {
        KERROR("kernel_waitpid: NULL Proc\n");
        return -EINVAL;
    }

    if (options & ~(WNOHANG | WUNTRACED)) {
        return -EINVAL;
    }
        
    //KDEBUG("ENTERING KERNEL WAITPID LOOP for %s...\n", parent->name);

    while (1) {
        if (has_pending_signals(parent)) {
            parent->process->waitpid_pid = 0;
            parent->process->waitpid_status = NULL;
            parent->process->waitpid_options = 0;
            return -EINTR;
        }

        /* Chercher un processus zombie - ACCeS CORRECT */
        zombie = find_zombie_child(parent, pid);

        //KDEBUG("SEEKING FOR ZOMBIE CHILD...\n");
        if (zombie) {
            /* Zombie trouve - ACCeS CORRECT */
            pid_t child_pid = zombie->process->pid;
            int wait_status;

            if (zombie->process->term_signal > 0) {
                wait_status = zombie->process->term_signal & 0x7f;
            } else {
                wait_status = (zombie->process->exit_code & 0xff) << 8;
            }
            
            /* Copier le statut de sortie */
            if (status) {
                *status = wait_status;
            }
            
            /* Retirer de la liste des enfants - ACCeS CORRECT */
            remove_child_from_parent(parent, zombie);
            
            /* Nettoyer le processus zombie */
            //task_set_state(zombie, TASK_TERMINATED);
            task_set_terminated(zombie);
            kernel_lifecycle_stats.zombies_reaped++;
            destroy_process(zombie);

            return child_pid;
        }

        if (options & WUNTRACED) {
            pid_t stopped_pid = consume_stopped_child(parent, pid, status);
            if (stopped_pid > 0)
                return stopped_pid;
        }
        
        //KDEBUG("NO ZOMBIE CHILD FOUND...\n");

        /* Verifier s'il y a encore des enfants eligibles - ACCeS CORRECT */
        if (!has_children(parent, pid)) {
            if (!(options & WNOHANG)) {
                KDEBUG("kernel_waitpid: No eligible children\n");
            }
            return -ECHILD;
        }

        if (options & WNOHANG) {
            return 0;
        }
        
        {
            pid_t stopped_pid = 0;
            if (!waitpid_prepare_sleep(parent, pid, options, status, &stopped_pid)) {
                if (stopped_pid > 0)
                    return stopped_pid;
                continue;
            }
        }

        //KDEBUG("kernel_waitpid: Parent PID %u going to sleep, waiting for child PID %d\n", 
        //       parent->process->pid, pid);

        // Parent is temporarily restoring to kernel to not resuming too early to user
        yield(); 

        //KDEBUG("kernel_waitpid: Parent PID %u resumed\n", parent->process->pid);
    }
    
    /* Ne devrait jamais arriver, mais pour éviter warning */
    return -EINTR;
}



int sys_waitpid(pid_t pid, int* status, int options)
{

    //task_sleep_ms(1000);  // ancien workaround pour le bug returns_to_user, supprime
    //KDEBUG("sys_waitpid: called for = %d - &status = 0x%08X\n", pid, (uint32_t)status);


    task_t *parent = task_current_local();
    //debug_print_ctx(&parent->context, "PARENT CONTEXT BEFORE WAITPID");

    //parent->context.returns_to_user = 0;
    //parent->context.cpsr = 0x60000013;  // SVC mode
    //yield();

    int exit_code;
    pid_t result = kernel_waitpid(pid, &exit_code, options, parent);

    if (result > 0 && status) {
        /* copy_to_user pour les appels depuis l'espace utilisateur */
        if (copy_to_user(status, &exit_code, sizeof(int)) < 0) {
            return -EFAULT;
        }
    }

    //debug_print_ctx(&parent->context, "PARENT CONTEXT AFTER WAITPID");

    //parent->context.returns_to_user = 1;
    //parent->context.cpsr = 0x60000010;  // USER mode
    //yield();
    //KDEBUG("sys_waitpid: returning to callee  = %d\n", sys_getppid());

    
    return result;
}

int sys_wait4(pid_t pid, int* status, int options, struct rusage_kernel* rusage)
{
    struct rusage_kernel local;
    task_t *parent = task_current_local();
    int exit_code;
    pid_t result;

    result = kernel_waitpid(pid, &exit_code, options, parent);
    if (result > 0 && status) {
        if (copy_to_user(status, &exit_code, sizeof(int)) < 0)
            return -EFAULT;
    }

    if (result > 0 && rusage) {
        /*
         * Child lifetime accounting is not persisted after destroy_process()
         * yet. Return a valid zeroed rusage instead of exposing stale memory.
         */
        memset(&local, 0, sizeof(local));
        if (copy_to_user(rusage, &local, sizeof(local)) < 0)
            return -EFAULT;
    }

    return result;
}

void print_task_offsets(void) {
    KDEBUG("=== TASK STRUCTURE OFFSETS ===\n");
    KDEBUG("task_id: %zu\n", offsetof(task_t, task_id));
    KDEBUG("name: %zu\n", offsetof(task_t, name));
    KDEBUG("state: %zu\n", offsetof(task_t, state));
    KDEBUG("priority: %zu\n", offsetof(task_t, priority));
    KDEBUG("context: %zu\n", offsetof(task_t, context));
    KDEBUG("stack_base: %zu\n", offsetof(task_t, stack_base));
    KDEBUG("Total size: %zu\n", sizeof(task_t));
}


static syscall_result_t normalize_syscall_result(uint32_t syscall_num,
                                                 syscall_word_t raw_result)
{
    switch (syscall_num) {
    case __NR_brk:
    case __NR_mmap:
    case __NR_shm_map:
    case __NR_lseek:
    case __NR_readv:
    case __NR_writev:
        return (syscall_result_t)raw_result;
    case __NR_rt_sigreturn:
        return 0;
    default:
        return (syscall_result_t)(int32_t)(uint32_t)raw_result;
    }
}

syscall_result_t syscall_dispatch_common_request(
    const syscall_request_t *request)
{
    task_t *proc;
    signal_check_result_t sig_result = SIGNAL_CHECK_NONE;
    syscall_word_t raw_result;
    syscall_result_t result;
    uint32_t syscall_num;

    if (!request)
        return -EINVAL;
    syscall_num = request->number;
    if (syscall_num >= MAX_SYSCALLS || !syscall_table[syscall_num]) {
        return -ENOSYS;
    }

    proc = task_current_local();
    if (proc) {
        proc->current_syscall = syscall_num;
        proc->last_syscall = syscall_num;
    }
    //uint32_t usr_r11 = proc->context.usr_r[11];

    //if( syscall_num != __NR_read &&
    //    syscall_num != __NR_write ) KDEBUG(" SYSCALL NUM == %u == %s\n", syscall_num, proc->name);


//print_task_offsets();
// print_context_offsets();

    //uint32_t usr_r[13];
    //get_usr_regs(usr_r);
    //store_usr_regs(&proc->context, usr_r);

    //debug_print_ctx(&proc->context, "----->>>>>> ENTER SYSCALL HANDLER");
    
    raw_result = syscall_table[syscall_num](
        request->arguments[0], request->arguments[1],
        request->arguments[2], request->arguments[3],
        request->arguments[4], request->arguments[5]);
    result = normalize_syscall_result(syscall_num, raw_result);

    proc = task_current_local();

    /*
     * Le contexte user canonique doit contenir la valeur de retour avant de
     * construire une eventuelle frame signal. rt_sigreturn a deja restaure r0.
     */
    if (proc && syscall_num != __NR_rt_sigreturn) {
        arch_task_context_set_user_register(&proc->context, 0,
                                            (uintptr_t)result);
    }

    if (proc && syscall_num != __NR_rt_sigreturn) {
        sig_result = check_pending_signals();
        proc = task_current_local();
    }

    if (!proc) {
        return result;
    }

    if (sig_result == SIGNAL_CHECK_EXITED) {
        proc->current_syscall = 0;
        return result;
    }

    if (sig_result == SIGNAL_CHECK_STOPPED ||
        proc->state == TASK_BLOCKED ||
        proc->state == TASK_INTERRUPTIBLE ||
        proc->state == TASK_UNINTERRUPTIBLE ||
        proc->state == TASK_STOPPED ||
        proc->state == TASK_ZOMBIE ||
        proc->state == TASK_TERMINATED) {
        yield();
        proc->current_syscall = 0;
        return result;
    }

    if (scheduler_take_resched_current_cpu()) {
        yield();
    }

    proc->current_syscall = 0;
    return result;
}

syscall_result_t syscall_dispatch_common_handler(
    void *owner, const syscall_request_t *request)
{
    (void)owner;
    return syscall_dispatch_common_request(request);
}

int syscall_handler(uint32_t syscall_num, uint32_t arg1, uint32_t arg2,
                    uint32_t arg3, uint32_t arg4, uint32_t arg5)
{
    syscall_request_t request = {
        .number = syscall_num,
        .arguments = {arg1, arg2, arg3, arg4, arg5, 0}
    };

    return (int)syscall_dispatch_common_request(&request);
}

/**
 * Syscalls simples adaptes
 */
int sys_getpid(void)
{
    task_t *proc = task_current_local();
    process_t *process = task_get_process(proc);

    return process ? process->pid : 0;
}

int sys_gettid(void)
{
    task_t *task = task_current_local();

    return task ? (int)task->task_id : 0;
}

int sys_getppid(void)
{
    task_t *proc = task_current_local();
    process_t *process = task_get_process(proc);

    return process ? process->ppid : 0;
}

int sys_getuid(void)
{
    task_t *proc = task_current_local();

    if (proc && proc->type == TASK_TYPE_PROCESS && proc->process) {
        return proc->process->uid;
    }

    return 0;
}

int sys_geteuid(void)
{
    return sys_getuid();
}

int sys_setuid(uid_t uid)
{
    task_t *proc = task_current_local();

    if (!proc || proc->type != TASK_TYPE_PROCESS || !proc->process)
        return -EINVAL;

    if (proc->process->uid != 0 && proc->process->uid != uid)
        return -EPERM;

    proc->process->uid = uid;
    return 0;
}

int sys_getgid(void)
{
    task_t *proc = task_current_local();

    if (proc && proc->type == TASK_TYPE_PROCESS && proc->process) {
        return proc->process->gid;
    }
    return 0;
}

int sys_getegid(void)
{
    return sys_getgid();
}

int sys_setgid(gid_t gid)
{
    task_t *proc = task_current_local();

    if (!proc || proc->type != TASK_TYPE_PROCESS || !proc->process)
        return -EINVAL;

    if (proc->process->uid != 0 && proc->process->gid != gid)
        return -EPERM;

    proc->process->gid = gid;
    return 0;
}

#define PRIO_PROCESS 0

static int scheduler_priority_to_nice(uint32_t priority)
{
    int nice = (int)priority - TASK_DEFAULT_PRIORITY;

    if (nice < TASK_NICE_MIN)
        return TASK_NICE_MIN;
    if (nice > TASK_NICE_MAX)
        return TASK_NICE_MAX;
    return nice;
}

static uint32_t nice_to_scheduler_priority(int nice)
{
    if (nice < TASK_NICE_MIN)
        nice = TASK_NICE_MIN;
    if (nice > TASK_NICE_MAX)
        nice = TASK_NICE_MAX;

    /*
     * ArmOS priorities are scheduler priorities: lower values run first.
     * Unix nice values are user-facing weights: lower values mean "nicer to me,
     * less nice to others". Mapping nice 0 to TASK_DEFAULT_PRIORITY preserves
     * the default used by user processes.
     */
    return (uint32_t)(nice + TASK_DEFAULT_PRIORITY);
}

static bool can_change_task_priority(task_t *caller, task_t *target, int new_nice)
{
    int old_nice;

    if (!caller || !caller->process || !target || !target->process)
        return false;

    if (caller->process->uid == 0)
        return true;

    if (caller->process->uid != target->process->uid)
        return false;

    old_nice = scheduler_priority_to_nice(target->priority);
    return new_nice >= old_nice;
}

int sys_nice(int inc)
{
    task_t *proc = task_current_local();
    int old_nice;
    int new_nice;

    if (!proc || proc->type != TASK_TYPE_PROCESS || !proc->process)
        return -EINVAL;

    old_nice = scheduler_priority_to_nice(proc->priority);
    new_nice = old_nice + inc;
    if (new_nice < TASK_NICE_MIN)
        new_nice = TASK_NICE_MIN;
    if (new_nice > TASK_NICE_MAX)
        new_nice = TASK_NICE_MAX;

    if (!can_change_task_priority(proc, proc, new_nice))
        return -EPERM;

    task_set_priority(proc, nice_to_scheduler_priority(new_nice));
    return 0;
}

int sys_getpriority(int which, int who)
{
    task_t *target;

    if (which != PRIO_PROCESS)
        return -EINVAL;

    if (who == 0) {
        target = task_current_local();
    } else {
        target = find_process_by_pid((pid_t)who);
    }

    if (!target || target->type != TASK_TYPE_PROCESS || !target->process)
        return -ESRCH;

    /*
     * Match the Linux raw-syscall trick: expose 20 - nice so successful
     * negative nice values are not mistaken for -errno by the libc wrapper.
     */
    return 20 - scheduler_priority_to_nice(target->priority);
}

int sys_setpriority(int which, int who, int prio)
{
    task_t *caller = task_current_local();
    task_t *target;

    if (which != PRIO_PROCESS)
        return -EINVAL;
    if (prio < TASK_NICE_MIN || prio > TASK_NICE_MAX)
        return -EINVAL;

    if (who == 0) {
        target = task_current_local();
    } else {
        target = find_process_by_pid((pid_t)who);
    }

    if (!target || target->type != TASK_TYPE_PROCESS || !target->process)
        return -ESRCH;

    if (!can_change_task_priority(caller, target, prio))
        return -EPERM;

    task_set_priority(target, nice_to_scheduler_priority(prio));
    return 0;
}



int sys_print(const char* msg) {

    char *str = NULL;

    bool user_mode = memory_is_user_address((vaddr_t)msg);

    if(user_mode){
        str = copy_string_from_user(msg);
    }
    else{
        str = kmalloc(strlen(msg)+1);
        strcpy(str, msg);
    }


    if(str) {
        kprintf("%s", msg);  // OK car valide
    }
    else {
        if(msg)
            KERROR("Invalid String from userspace %s\n", msg);
        else
            KERROR("Invalid String from userspace: NULL message\n");
        return -1;
    }

    return 0;
}
