/*
 * ArmOS
 * Copyright (c) 2026 Mohamed Ennassiri
 *
 * Licensed under the Apache License, Version 2.0.
 * See LICENSE for details.
 *
 * File: kernel/fs/procfs.c
 * Layer: Kernel / VFS and filesystems
 *
 * Responsibilities:
 * - Provide filesystem-independent VFS operations.
 * - Implement persistent ext2/FAT32/procfs behavior.
 *
 * Notes:
 * - Keep file descriptor and inode ownership rules explicit.
 */

#include <kernel/procfs.h>
#include <kernel/vfs.h>
#include <kernel/memory.h>
#include <kernel/string.h>
#include <kernel/kprintf.h>
#include <kernel/stdarg.h>
#include <kernel/timer.h>
#include <kernel/file.h>
#include <kernel/disk_layout.h>
#include <kernel/tty.h>
#include <kernel/virtio_input.h>
#include <kernel/interrupt.h>
#include <kernel/virtio_block.h>
#include <kernel/virtio_net.h>
#include <kernel/uart.h>
#include <kernel/ext2.h>
#include <kernel/fat32.h>
#include <kernel/syscalls.h>
#include <kernel/smp.h>
#include <kernel/tlb.h>
#include <kernel/arch_cpu.h>
#include <kernel/arch_mmu_debug.h>
#include <kernel/arch_platform.h>
#include <kernel/block_device.h>
#include <kernel/usb.h>
#include <kernel/net/device.h>
#include <kernel/net/socket.h>
#include <kernel/net/stack.h>

extern uint32_t task_count;
extern task_t* task_list_head;
extern spinlock_t task_lock;

static inode_operations_t procfs_inode_ops;
static file_operations_t procfs_file_ops;
static file_operations_t procfs_dir_ops;

#define PROC_FILE_BUFFER_SIZE 16384u

#define PROC_INO_ROOT       1u
#define PROC_INO_MEMINFO    2u
#define PROC_INO_UPTIME     3u
#define PROC_INO_MOUNTS     4u
#define PROC_INO_STAT       5u
#define PROC_INO_TASKS      6u
#define PROC_INO_CPUINFO    7u
#define PROC_INO_FILESYSTEMS 8u
#define PROC_INO_PARTITIONS 9u
#define PROC_INO_SELF       10u
#define PROC_INO_DMESG      11u
#define PROC_INO_INTERRUPTS 12u
#define PROC_INO_TTY        13u
#define PROC_INO_NET_DIR    14u
#define PROC_INO_NET_DEV    15u
#define PROC_INO_NET_TCP    16u
#define PROC_INO_FS_DIR     17u
#define PROC_INO_FS_EXT2    18u
#define PROC_INO_FS_EXT2_STATS 19u
#define PROC_INO_FS_EXT2_CHECK 20u
#define PROC_INO_SCHED_TRACE 21u
#define PROC_INO_SCHED      22u
#define PROC_INO_SMP        23u
#define PROC_INO_SMP_IPI    24u
#define PROC_INO_DISKSTATS  25u
#define PROC_INO_FS_FAT32   26u
#define PROC_INO_FS_FAT32_STATS 27u
#define PROC_INO_USB        28u
#define PROC_INO_LOADAVG    29u
#define PROC_PID_BASE       100000u
#define PROC_PID_STRIDE     512u
#define PROC_PID_DIR        0u
#define PROC_PID_STATUS     1u
#define PROC_PID_STAT       2u
#define PROC_PID_MAPS       3u
#define PROC_PID_FD_DIR     4u
#define PROC_PID_CMDLINE    5u
#define PROC_PID_ENVIRON    6u
#define PROC_PID_CWD        7u
#define PROC_PID_EXE        8u
#define PROC_PID_ROOT       9u
#define PROC_PID_FD_BASE    128u

typedef struct proc_file_data {
    char* data;
    size_t size;
} proc_file_data_t;

static bool proc_is_digit(char c)
{
    return c >= '0' && c <= '9';
}

static bool proc_parse_pid(const char* name, pid_t* out)
{
    uint32_t pid = 0;

    if (!name || !name[0]) return false;

    for (const char* p = name; *p; p++) {
        if (!proc_is_digit(*p)) return false;
        pid = pid * 10u + (uint32_t)(*p - '0');
    }

    if (pid == 0) return false;
    if (out) *out = (pid_t)pid;
    return true;
}

static bool proc_parse_uint(const char* name, uint32_t* out)
{
    uint32_t value = 0;

    if (!name || !name[0]) return false;

    for (const char* p = name; *p; p++) {
        if (!proc_is_digit(*p)) return false;
        value = value * 10u + (uint32_t)(*p - '0');
    }

    if (out) *out = value;
    return true;
}

static uint32_t proc_pid_ino(pid_t pid, uint32_t type)
{
    return PROC_PID_BASE + ((uint32_t)pid * PROC_PID_STRIDE) + type;
}

static uint32_t proc_pid_fd_ino(pid_t pid, int fd)
{
    return proc_pid_ino(pid, PROC_PID_FD_BASE + (uint32_t)fd);
}

static pid_t proc_ino_pid(uint32_t ino)
{
    if (ino < PROC_PID_BASE) return 0;
    return (pid_t)((ino - PROC_PID_BASE) / PROC_PID_STRIDE);
}

static uint32_t proc_ino_type(uint32_t ino)
{
    if (ino < PROC_PID_BASE) return ino;
    return (ino - PROC_PID_BASE) % PROC_PID_STRIDE;
}

static int proc_ino_fd(uint32_t ino)
{
    uint32_t type = proc_ino_type(ino);
    if (type < PROC_PID_FD_BASE ||
        type >= PROC_PID_FD_BASE + (uint32_t)MAX_FILES)
        return -1;
    return (int)(type - PROC_PID_FD_BASE);
}

static const char* proc_task_state_name(task_state_t state)
{
    switch (state) {
        case TASK_READY:           return "ready";
        case TASK_RUNNING:         return "running";
        case TASK_BLOCKED:         return "blocked";
        case TASK_ZOMBIE:          return "zombie";
        case TASK_TERMINATED:      return "terminated";
        case TASK_INTERRUPTIBLE:   return "sleeping";
        case TASK_UNINTERRUPTIBLE: return "disk-sleep";
        case TASK_STOPPED:         return "stopped";
    }
    return "unknown";
}

static char proc_task_state_char(task_state_t state)
{
    switch (state) {
        case TASK_READY:
        case TASK_RUNNING:         return 'R';
        case TASK_ZOMBIE:          return 'Z';
        case TASK_TERMINATED:      return 'X';
        case TASK_UNINTERRUPTIBLE: return 'D';
        case TASK_STOPPED:         return 'T';
        default:                   return 'S';
    }
}

static process_t* proc_task_process(task_t* task)
{
    return task_get_process(task);
}

static task_t* proc_find_task_locked(pid_t pid)
{
    task_t* task = task_list_head;
    uint32_t count = 0;

    if (!task) return NULL;

    do {
        process_t* proc = proc_task_process(task);
        if (proc && proc->pid == pid)
            return task;

        task = task->next;
        count++;
    } while (task && task != task_list_head && count < MAX_TASKS);

    return NULL;
}

static bool proc_try_lock_tasks(unsigned long* flags)
{
    if (!flags) return false;

    *flags = disable_interrupts_save();
    if (!spin_trylock(&task_lock)) {
        restore_interrupts((uint32_t)*flags);
        return false;
    }

    return true;
}

static bool proc_pid_exists(pid_t pid)
{
    bool found;
    unsigned long flags;

    spin_lock_irqsave(&task_lock, &flags);
    found = proc_find_task_locked(pid) != NULL;
    spin_unlock_irqrestore(&task_lock, flags);
    return found;
}

static pid_t proc_current_pid(void)
{
    task_t *task = task_current_local();

    if (task && task->process)
        return task->process->pid;
    return 0;
}

static void proc_append(char* buf, size_t cap, size_t* len, const char* fmt, ...)
{
    va_list args;
    int written;

    if (!buf || !len || *len >= cap) return;

    va_start(args, fmt);
    written = vsnprintf(buf + *len, (int)(cap - *len), fmt, args);
    va_end(args);

    if (written < 0) return;

    if ((size_t)written >= cap - *len)
        *len = cap - 1;
    else
        *len += (size_t)written;
}

static inode_t* proc_make_inode(uint32_t ino, uint16_t mode, uint32_t size)
{
    inode_t* inode = create_inode();
    if (!inode) return NULL;

    inode->mode = mode;
    inode->uid = 0;
    inode->gid = 0;
    inode->size = size;
    inode->blocks = (size + 511) / 512;
    inode->nlink = S_ISDIR(mode) ? 2 : 1;
    inode->first_cluster = ino;
    inode->i_op = &procfs_inode_ops;
    inode->f_op = S_ISDIR(mode) ? &procfs_dir_ops : &procfs_file_ops;
    return inode;
}

static inode_t* procfs_lookup(inode_t* dir, const char* name)
{
    uint32_t dir_ino;
    pid_t pid;

    if (!dir || !name) return NULL;

    dir_ino = dir->first_cluster;

    if (dir_ino == PROC_INO_ROOT) {
        if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0)
            return proc_make_inode(PROC_INO_ROOT, S_IFDIR | 0555, 0);
        if (strcmp(name, "meminfo") == 0)
            return proc_make_inode(PROC_INO_MEMINFO, S_IFREG | 0444, 0);
        if (strcmp(name, "uptime") == 0)
            return proc_make_inode(PROC_INO_UPTIME, S_IFREG | 0444, 0);
        if (strcmp(name, "mounts") == 0)
            return proc_make_inode(PROC_INO_MOUNTS, S_IFREG | 0444, 0);
        if (strcmp(name, "stat") == 0)
            return proc_make_inode(PROC_INO_STAT, S_IFREG | 0444, 0);
        if (strcmp(name, "loadavg") == 0)
            return proc_make_inode(PROC_INO_LOADAVG, S_IFREG | 0444, 0);
        if (strcmp(name, "tasks") == 0)
            return proc_make_inode(PROC_INO_TASKS, S_IFREG | 0444, 0);
        if (strcmp(name, "cpuinfo") == 0)
            return proc_make_inode(PROC_INO_CPUINFO, S_IFREG | 0444, 0);
        if (strcmp(name, "filesystems") == 0)
            return proc_make_inode(PROC_INO_FILESYSTEMS, S_IFREG | 0444, 0);
        if (strcmp(name, "partitions") == 0)
            return proc_make_inode(PROC_INO_PARTITIONS, S_IFREG | 0444, 0);
        if (strcmp(name, "diskstats") == 0)
            return proc_make_inode(PROC_INO_DISKSTATS, S_IFREG | 0444, 0);
        if (strcmp(name, "usb") == 0)
            return proc_make_inode(PROC_INO_USB, S_IFREG | 0444, 0);
        if (strcmp(name, "dmesg") == 0)
            return proc_make_inode(PROC_INO_DMESG, S_IFREG | 0444, 0);
        if (strcmp(name, "interrupts") == 0)
            return proc_make_inode(PROC_INO_INTERRUPTS, S_IFREG | 0444, 0);
        if (strcmp(name, "tty") == 0)
            return proc_make_inode(PROC_INO_TTY, S_IFREG | 0444, 0);
        if (strcmp(name, "net") == 0)
            return proc_make_inode(PROC_INO_NET_DIR, S_IFDIR | 0555, 0);
        if (strcmp(name, "fs") == 0)
            return proc_make_inode(PROC_INO_FS_DIR, S_IFDIR | 0555, 0);
        if (strcmp(name, "sched") == 0)
            return proc_make_inode(PROC_INO_SCHED, S_IFREG | 0444, 0);
        if (strcmp(name, "sched_trace") == 0)
            return proc_make_inode(PROC_INO_SCHED_TRACE, S_IFREG | 0444, 0);
        if (strcmp(name, "smp") == 0)
            return proc_make_inode(PROC_INO_SMP, S_IFREG | 0444, 0);
        if (strcmp(name, "smp_ipi") == 0)
            return proc_make_inode(PROC_INO_SMP_IPI, S_IFREG | 0666, 0);
        if (strcmp(name, "self") == 0)
            return proc_make_inode(PROC_INO_SELF, S_IFLNK | 0777, 0);
        if (proc_parse_pid(name, &pid) && proc_pid_exists(pid))
            return proc_make_inode(proc_pid_ino(pid, PROC_PID_DIR), S_IFDIR | 0555, 0);
        return NULL;
    }

    if (dir_ino == PROC_INO_NET_DIR) {
        if (strcmp(name, ".") == 0)
            return proc_make_inode(PROC_INO_NET_DIR, S_IFDIR | 0555, 0);
        if (strcmp(name, "..") == 0)
            return proc_make_inode(PROC_INO_ROOT, S_IFDIR | 0555, 0);
        if (strcmp(name, "dev") == 0)
            return proc_make_inode(PROC_INO_NET_DEV, S_IFREG | 0444, 0);
        if (strcmp(name, "tcp") == 0)
            return proc_make_inode(PROC_INO_NET_TCP, S_IFREG | 0444, 0);
        return NULL;
    }

    if (dir_ino == PROC_INO_FS_DIR) {
        if (strcmp(name, ".") == 0)
            return proc_make_inode(PROC_INO_FS_DIR, S_IFDIR | 0555, 0);
        if (strcmp(name, "..") == 0)
            return proc_make_inode(PROC_INO_ROOT, S_IFDIR | 0555, 0);
        if (strcmp(name, "ext2") == 0)
            return proc_make_inode(PROC_INO_FS_EXT2, S_IFDIR | 0555, 0);
        if (strcmp(name, "fat32") == 0)
            return proc_make_inode(PROC_INO_FS_FAT32, S_IFDIR | 0555, 0);
        return NULL;
    }

    if (dir_ino == PROC_INO_FS_EXT2) {
        if (strcmp(name, ".") == 0)
            return proc_make_inode(PROC_INO_FS_EXT2, S_IFDIR | 0555, 0);
        if (strcmp(name, "..") == 0)
            return proc_make_inode(PROC_INO_FS_DIR, S_IFDIR | 0555, 0);
        if (strcmp(name, "stats") == 0)
            return proc_make_inode(PROC_INO_FS_EXT2_STATS, S_IFREG | 0444, 0);
        if (strcmp(name, "check") == 0)
            return proc_make_inode(PROC_INO_FS_EXT2_CHECK, S_IFREG | 0444, 0);
        return NULL;
    }

    if (dir_ino == PROC_INO_FS_FAT32) {
        if (strcmp(name, ".") == 0)
            return proc_make_inode(PROC_INO_FS_FAT32, S_IFDIR | 0555, 0);
        if (strcmp(name, "..") == 0)
            return proc_make_inode(PROC_INO_FS_DIR, S_IFDIR | 0555, 0);
        if (strcmp(name, "stats") == 0)
            return proc_make_inode(PROC_INO_FS_FAT32_STATS, S_IFREG | 0444, 0);
        return NULL;
    }

    if (dir_ino >= PROC_PID_BASE && proc_ino_type(dir_ino) == PROC_PID_DIR) {
        pid = proc_ino_pid(dir_ino);
        if (!proc_pid_exists(pid)) return NULL;

        if (strcmp(name, ".") == 0)
            return proc_make_inode(proc_pid_ino(pid, PROC_PID_DIR), S_IFDIR | 0555, 0);
        if (strcmp(name, "..") == 0)
            return proc_make_inode(PROC_INO_ROOT, S_IFDIR | 0555, 0);
        if (strcmp(name, "status") == 0)
            return proc_make_inode(proc_pid_ino(pid, PROC_PID_STATUS), S_IFREG | 0444, 0);
        if (strcmp(name, "stat") == 0)
            return proc_make_inode(proc_pid_ino(pid, PROC_PID_STAT), S_IFREG | 0444, 0);
        if (strcmp(name, "maps") == 0)
            return proc_make_inode(proc_pid_ino(pid, PROC_PID_MAPS), S_IFREG | 0444, 0);
        if (strcmp(name, "fd") == 0)
            return proc_make_inode(proc_pid_ino(pid, PROC_PID_FD_DIR), S_IFDIR | 0555, 0);
        if (strcmp(name, "cmdline") == 0)
            return proc_make_inode(proc_pid_ino(pid, PROC_PID_CMDLINE), S_IFREG | 0444, 0);
        if (strcmp(name, "environ") == 0)
            return proc_make_inode(proc_pid_ino(pid, PROC_PID_ENVIRON), S_IFREG | 0400, 0);
        if (strcmp(name, "cwd") == 0)
            return proc_make_inode(proc_pid_ino(pid, PROC_PID_CWD), S_IFLNK | 0777, 0);
        if (strcmp(name, "exe") == 0)
            return proc_make_inode(proc_pid_ino(pid, PROC_PID_EXE), S_IFLNK | 0777, 0);
        if (strcmp(name, "root") == 0)
            return proc_make_inode(proc_pid_ino(pid, PROC_PID_ROOT), S_IFLNK | 0777, 0);
    }

    if (dir_ino >= PROC_PID_BASE && proc_ino_type(dir_ino) == PROC_PID_FD_DIR) {
        int fd;
        uint32_t parsed_fd;
        task_t* task;
        process_t* proc;
        unsigned long flags;

        pid = proc_ino_pid(dir_ino);
        if (strcmp(name, ".") == 0)
            return proc_make_inode(proc_pid_ino(pid, PROC_PID_FD_DIR), S_IFDIR | 0555, 0);
        if (strcmp(name, "..") == 0)
            return proc_make_inode(proc_pid_ino(pid, PROC_PID_DIR), S_IFDIR | 0555, 0);
        if (!proc_parse_uint(name, &parsed_fd))
            return NULL;
        fd = (int)parsed_fd;
        if (fd < 0 || fd >= MAX_FILES)
            return NULL;

        spin_lock_irqsave(&task_lock, &flags);
        task = proc_find_task_locked(pid);
        proc = task ? proc_task_process(task) : NULL;
        if (!proc || !proc->files[fd]) {
            spin_unlock_irqrestore(&task_lock, flags);
            return NULL;
        }
        spin_unlock_irqrestore(&task_lock, flags);
        return proc_make_inode(proc_pid_fd_ino(pid, fd), S_IFLNK | 0777, 0);
    }

    return NULL;
}

static int procfs_readonly_create(inode_t* dir, const char* name, uint16_t mode)
{
    (void)dir; (void)name; (void)mode;
    return -EROFS;
}

static int procfs_readonly_unlink(inode_t* dir, const char* name)
{
    (void)dir; (void)name;
    return -EROFS;
}

static int procfs_readonly_rename(inode_t* old_dir, const char* old_name,
                                  inode_t* new_dir, const char* new_name)
{
    (void)old_dir; (void)old_name; (void)new_dir; (void)new_name;
    return -EROFS;
}

static int proc_copy_link(char* buf, size_t bufsiz, const char* target)
{
    size_t len;

    if (!buf || bufsiz == 0 || !target)
        return -EINVAL;

    len = strlen(target);
    if (len > bufsiz)
        len = bufsiz;
    memcpy(buf, target, len);
    return (int)len;
}

static int proc_fd_target(process_t* proc, int fd, char* target, size_t size)
{
    file_t* file;
    int tty_id;

    if (!proc || fd < 0 || fd >= MAX_FILES || !target || size == 0)
        return -ENOENT;

    file = proc->files[fd];
    if (!file)
        return -ENOENT;

    if (file->inode && file->inode->first_cluster >= PROC_PID_BASE) {
        uint32_t ino = file->inode->first_cluster;
        pid_t file_pid = proc_ino_pid(ino);
        uint32_t type = proc_ino_type(ino);

        if (type == PROC_PID_DIR)
            snprintf(target, size, "/proc/%d", file_pid);
        else if (type == PROC_PID_FD_DIR)
            snprintf(target, size, "/proc/%d/fd", file_pid);
        else if (type == PROC_PID_STATUS)
            snprintf(target, size, "/proc/%d/status", file_pid);
        else if (type == PROC_PID_STAT)
            snprintf(target, size, "/proc/%d/stat", file_pid);
        else if (type == PROC_PID_MAPS)
            snprintf(target, size, "/proc/%d/maps", file_pid);
        else if (type == PROC_PID_CMDLINE)
            snprintf(target, size, "/proc/%d/cmdline", file_pid);
        else if (type == PROC_PID_ENVIRON)
            snprintf(target, size, "/proc/%d/environ", file_pid);
        else
            snprintf(target, size, "/proc");
        return 0;
    }

    if (file_is_tty(file)) {
        tty_id = tty_id_from_file(file);
        if (tty_id == TTY_GRAPHICS_ID)
            snprintf(target, size, "/dev/tty1");
        else if (tty_id == TTY_SERIAL_ID)
            snprintf(target, size, "/dev/ttyS0");
        else if (tty_id == TTY_CONSOLE_ID)
            snprintf(target, size, "/dev/tty0");
        else
            return tty_id;
        return 0;
    }

    if (file->name[0] == '/') {
        snprintf(target, size, "%s", file->name);
        return 0;
    }

    if (file->name[0]) {
        snprintf(target, size, "%s", file->name);
        return 0;
    }

    snprintf(target, size, "anon_inode:[file]");
    return 0;
}

static int procfs_readlink(inode_t* inode, char* buf, size_t bufsiz)
{
    uint32_t ino;
    uint32_t type;
    pid_t pid;
    int fd;
    char target[MAX_PATH];
    unsigned long flags;
    task_t* task;
    process_t* proc;

    if (!inode || !buf)
        return -EINVAL;

    ino = inode->first_cluster;
    if (ino == PROC_INO_SELF) {
        snprintf(target, sizeof(target), "%d", proc_current_pid());
        return proc_copy_link(buf, bufsiz, target);
    }

    if (ino < PROC_PID_BASE)
        return -EINVAL;

    pid = proc_ino_pid(ino);
    type = proc_ino_type(ino);
    fd = proc_ino_fd(ino);

    spin_lock_irqsave(&task_lock, &flags);
    task = proc_find_task_locked(pid);
    proc = task ? proc_task_process(task) : NULL;
    if (!proc) {
        spin_unlock_irqrestore(&task_lock, flags);
        return -ENOENT;
    }

    if (type == PROC_PID_CWD) {
        snprintf(target, sizeof(target), "%s", proc->cwd[0] ? proc->cwd : "/");
    } else if (type == PROC_PID_EXE) {
        snprintf(target, sizeof(target), "%s", proc->exe_path[0] ? proc->exe_path : task->name);
    } else if (type == PROC_PID_ROOT) {
        snprintf(target, sizeof(target), "/");
    } else if (fd >= 0) {
        int ret = proc_fd_target(proc, fd, target, sizeof(target));
        if (ret < 0) {
            spin_unlock_irqrestore(&task_lock, flags);
            return ret;
        }
    } else {
        spin_unlock_irqrestore(&task_lock, flags);
        return -EINVAL;
    }

    spin_unlock_irqrestore(&task_lock, flags);
    return proc_copy_link(buf, bufsiz, target);
}

static void proc_fill_meminfo(char* buf, size_t cap, size_t* len)
{
    uint32_t total = (get_total_page_count() * PAGE_SIZE) / 1024;
    uint32_t free = (get_free_page_count() * PAGE_SIZE) / 1024;
    uint32_t used = total > free ? total - free : 0;

    proc_append(buf, cap, len, "MemTotal:       %u kB\n", total);
    proc_append(buf, cap, len, "MemFree:        %u kB\n", free);
    proc_append(buf, cap, len, "MemAvailable:   %u kB\n", free);
    proc_append(buf, cap, len, "Buffers:        0 kB\n");
    proc_append(buf, cap, len, "Cached:         0 kB\n");
    proc_append(buf, cap, len, "MemUsed:        %u kB\n", used);
    proc_append(buf, cap, len, "PageSize:       %u\n", PAGE_SIZE);
    proc_append(buf, cap, len, "PhysAllocPages: %u\n", get_allocated_page_count());
    proc_append(buf, cap, len, "PhysFreePages:  %u\n", get_freed_page_count());
}

static void proc_fill_uptime(char* buf, size_t cap, size_t* len)
{
    uint32_t ticks = get_system_ticks();
    proc_append(buf, cap, len, "%u.%02u 0.00\n",
                ticks / TIMER_FREQ,
                ((ticks % TIMER_FREQ) * 100u) / TIMER_FREQ);
}

static void proc_append_loadavg_value(char* buf, size_t cap, size_t* len,
                                      uint32_t value)
{
    uint32_t whole = value / SCHED_LOADAVG_SCALE;
    uint32_t fraction = ((value % SCHED_LOADAVG_SCALE) * 100u +
                         SCHED_LOADAVG_SCALE / 2u) / SCHED_LOADAVG_SCALE;

    if (fraction >= 100u) {
        whole++;
        fraction -= 100u;
    }
    proc_append(buf, cap, len, "%u.%02u", whole, fraction);
}

static void proc_fill_loadavg(char* buf, size_t cap, size_t* len)
{
    uint32_t averages[3];
    uint32_t runnable;

    scheduler_get_loadavg(averages, &runnable);
    proc_append_loadavg_value(buf, cap, len, averages[0]);
    proc_append(buf, cap, len, " ");
    proc_append_loadavg_value(buf, cap, len, averages[1]);
    proc_append(buf, cap, len, " ");
    proc_append_loadavg_value(buf, cap, len, averages[2]);
    proc_append(buf, cap, len, " %u/%u 0\n", runnable, task_get_count());
}

static void proc_fill_mounts(char* buf, size_t cap, size_t* len)
{
    vfs_format_mounts(buf, cap, len);
}

static void proc_fill_cpuinfo(char* buf, size_t cap, size_t* len)
{
    arch_cpuinfo_t cpuinfo;

    arch_get_cpuinfo(&cpuinfo);

    proc_append(buf, cap, len, "processor\t: 0\n");
    proc_append(buf, cap, len, "model name\t: %s\n", cpuinfo.model_name);
    proc_append(buf, cap, len, "BogoMIPS\t: 125.00\n");
    proc_append(buf, cap, len, "Features\t: %s\n", cpuinfo.features);
    proc_append(buf, cap, len, "CPU implementer\t: 0x%02x\n", cpuinfo.implementer);
    proc_append(buf, cap, len, "CPU architecture: %u\n", cpuinfo.architecture);
    proc_append(buf, cap, len, "CPU part\t: 0x%03x\n", cpuinfo.part);
    proc_append(buf, cap, len, "CPU revision\t: %u\n", cpuinfo.revision);
    proc_append(buf, cap, len, "Hardware\t: %s\n", cpuinfo.hardware);
    proc_append(buf, cap, len, "Revision\t: 0000\n");
    proc_append(buf, cap, len, "MPIDR\t\t: 0x%08x\n", cpuinfo.mpidr);
}

static void proc_fill_smp(char* buf, size_t cap, size_t* len)
{
    uint32_t current = smp_processor_id();
    uint32_t boot = smp_boot_cpu_id();
    uint32_t possible = smp_possible_cpu_count();
    uint32_t seen = smp_seen_cpu_mask();
    arch_cpuinfo_t cpuinfo;
    arch_mmu_debug_state_t mmu;

    arch_get_cpuinfo(&cpuinfo);
    arch_mmu_debug_snapshot(&mmu);

    proc_append(buf, cap, len, "current_cpu: %u\n", current);
    proc_append(buf, cap, len, "boot_cpu:    %u\n", boot);
    proc_append(buf, cap, len, "possible:    %u\n", possible);
    proc_append(buf, cap, len, "online:      %u\n", smp_online_cpu_count());
    proc_append(buf, cap, len, "seen_mask:   0x%08x\n", seen);
    proc_append(buf, cap, len, "sched_mask:  0x%08x\n", smp_scheduler_cpu_mask());
    proc_append(buf, cap, len, "tlb_total:   %u\n", tlb_shootdown_total_count());
    proc_append(buf, cap, len, "tlb_remote:  %u\n", tlb_shootdown_remote_count());
    proc_append(buf, cap, len, "tlb_defer:   %u\n", tlb_shootdown_deferred_count());
    proc_append(buf, cap, len, "tlb_gen:     %u\n", tlb_shootdown_generation());
    proc_append(buf, cap, len, "sched_guard: %u\n", smp_scheduler_reject_count());
    proc_append(buf, cap, len,
                "sysreg_cpu%u: midr_impl=0x%02x part=0x%03x sctlr=%p actlr=%p ttbr0=%p ttbr1=%p tcr=%p access=%p\n",
                current, cpuinfo.implementer, cpuinfo.part,
                (void *)(uintptr_t)mmu.control,
                (void *)(uintptr_t)mmu.auxiliary,
                (void *)(uintptr_t)mmu.root0,
                (void *)(uintptr_t)mmu.root1,
                (void *)(uintptr_t)mmu.translation_control,
                (void *)(uintptr_t)mmu.access_control);
    proc_append(buf, cap, len, "\n");
    proc_append(buf, cap, len, "cpu state   seen sched rq      irq  ipi    timer    hb tlb idle  cur_tid cur_pid pri current      psci idle-work idle-sched idle-fb\n");

    for (uint32_t cpu = 0; cpu < possible; cpu++) {
        const smp_cpu_info_t* info = smp_cpu_info(cpu);
        task_t* current_task_on_cpu = task_current_on_cpu(cpu);
        task_t* idle_task_on_cpu = task_idle_on_cpu(cpu);
        process_t* current_proc = current_task_on_cpu && current_task_on_cpu->type == TASK_TYPE_PROCESS
                                ? current_task_on_cpu->process
                                : NULL;

        proc_append(buf, cap, len, "%3u %-7s %4s %5s %2u %8u %4u %8u %5u %3u %4u %8u %7u %3u %-12s %d %9u %10u %7u\n",
                    cpu,
                    smp_cpu_state_name(cpu),
                    smp_cpu_seen(cpu) ? "yes" : "no",
                    smp_scheduler_cpu_enabled(cpu) ? "yes" : "no",
                    scheduler_resched_pending_on_cpu(cpu) ? 1u : 0u,
                    info ? info->irq_count : 0,
                    info ? info->ipi_count : 0,
                    timer_cpu_tick_count(cpu),
                    info ? info->park_heartbeat : 0,
                    tlb_shootdown_cpu_ack(cpu),
                    idle_task_on_cpu ? idle_task_on_cpu->task_id : 0u,
                    current_task_on_cpu ? current_task_on_cpu->task_id : 0u,
                    current_proc ? (uint32_t)current_proc->pid : 0u,
                    current_task_on_cpu ? current_task_on_cpu->priority : 0u,
                    current_task_on_cpu ? current_task_on_cpu->name : "-",
                    smp_cpu_start_result(cpu),
                    scheduler_idle_work_seen_count(cpu),
                    scheduler_idle_schedule_count(cpu),
                    scheduler_idle_fallback_count(cpu));
    }

    proc_append(buf, cap, len, "\n");
    proc_append(buf, cap, len, "release diagnostics\n");
    proc_append(buf, cap, len, "cpu entry      mbox3_rd  spin_lo\n");
    for (uint32_t cpu = 0; cpu < possible; cpu++) {
        proc_append(buf, cap, len, "%3u 0x%08x 0x%08x 0x%08x\n",
                    cpu,
                    smp_cpu_release_entry(cpu),
                    smp_cpu_release_mailbox3_value(cpu),
                    smp_cpu_release_spin_table_value(cpu));
    }
}

static void proc_fill_smp_ipi(char* buf, size_t cap, size_t* len)
{
    proc_append(buf, cap, len,
                "SMP IPI test endpoint\n"
                "write \"self\" or \"0\" to send IRQ_SGI_TLB_SHOOTDOWN to CPU0\n"
                "write \"others\" to send it to all non-boot CPU interfaces\n"
                "write \"tlb\" to run a full TLB shootdown rendezvous\n");
}

static void proc_fill_filesystems(char* buf, size_t cap, size_t* len)
{
    proc_append(buf, cap, len, "nodev\tproc\n");
    proc_append(buf, cap, len, "\text2\n");
    proc_append(buf, cap, len, "\tfat32\n");
}

static void proc_fill_partitions(char* buf, size_t cap, size_t* len)
{
    proc_append(buf, cap, len, "major minor  #blocks  name\n\n");

    for (uint32_t i = 0; i < DISK_PART_COUNT; i++) {
        const disk_partition_t* part = disk_partition_get((disk_partition_id_t)i);
        if (!part)
            continue;
        proc_append(buf, cap, len, " 254 %5u %8u %s\n",
                    i + 1,
                    (uint32_t)(part->sector_count >> 1),
                    part->name);
    }
}

static void proc_fill_diskstats(char* buf, size_t cap, size_t* len)
{
    block_device_stats_t st;
    block_transport_stats_t transport;

    blk_get_stats(&st);
    blk_get_transport_stats(&transport);
    proc_append(buf, cap, len, "device %s\n", blk_get_name());
    proc_append(buf, cap, len, "transport %s\n", blk_get_transport_name());
    proc_append(buf, cap, len, "sector_size %u\n", blk_get_sector_size());
    proc_append(buf, cap, len, "capacity_sectors %llu\n",
                (unsigned long long)blk_get_capacity_sectors());
    proc_append(buf, cap, len, "read_requests %llu\n",
                (unsigned long long)st.read_requests);
    proc_append(buf, cap, len, "read_sectors %llu\n",
                (unsigned long long)st.read_sectors);
    proc_append(buf, cap, len, "read_errors %llu\n",
                (unsigned long long)st.read_errors);
    proc_append(buf, cap, len, "write_requests %llu\n",
                (unsigned long long)st.write_requests);
    proc_append(buf, cap, len, "write_sectors %llu\n",
                (unsigned long long)st.write_sectors);
    proc_append(buf, cap, len, "write_errors %llu\n",
                (unsigned long long)st.write_errors);
    proc_append(buf, cap, len, "flush_requests %llu\n",
                (unsigned long long)st.flush_requests);
    proc_append(buf, cap, len, "flush_errors %llu\n",
                (unsigned long long)st.flush_errors);
    proc_append(buf, cap, len, "max_read_sectors %u\n", st.max_read_sectors);
    proc_append(buf, cap, len, "max_write_sectors %u\n", st.max_write_sectors);
    proc_append(buf, cap, len, "transport_read_commands %llu\n",
                (unsigned long long)transport.read_commands);
    proc_append(buf, cap, len, "transport_read_sectors %llu\n",
                (unsigned long long)transport.read_sectors);
    proc_append(buf, cap, len, "transport_read_errors %llu\n",
                (unsigned long long)transport.read_errors);
    proc_append(buf, cap, len, "transport_write_commands %llu\n",
                (unsigned long long)transport.write_commands);
    proc_append(buf, cap, len, "transport_write_sectors %llu\n",
                (unsigned long long)transport.write_sectors);
    proc_append(buf, cap, len, "transport_write_errors %llu\n",
                (unsigned long long)transport.write_errors);
    proc_append(buf, cap, len, "transport_multiblock_fallbacks %llu\n",
                (unsigned long long)transport.multiblock_fallbacks);
    proc_append(buf, cap, len, "transport_max_read_sectors %u\n",
                transport.max_read_sectors);
    proc_append(buf, cap, len, "transport_max_write_sectors %u\n",
                transport.max_write_sectors);
}

static void proc_fill_usb(char* buf, size_t cap, size_t* len)
{
    usb_device_info_t *devices;
    size_t count;

    proc_append(buf, cap, len,
                "bus|address|parent|port|speed|max_packet|class|subclass|"
                "protocol|usb|vendor|product|device|configs|interfaces|"
                "ports|keyboards|mice|manufacturer|product_name|serial\n");
    devices = kmalloc(sizeof(*devices) * USB_TOPOLOGY_MAX_DEVICES);
    if (!devices)
        return;

    count = usb_topology_snapshot(devices, USB_TOPOLOGY_MAX_DEVICES);
    for (size_t i = 0; i < count; i++) {
        const usb_device_info_t *dev = &devices[i];

        proc_append(buf, cap, len,
                    "%u|%u|%u|%u|%s|%u|%02x|%02x|%02x|%04x|%04x|%04x|"
                    "%04x|%u|%u|%u|%u|%u|%s|%s|%s\n",
                    dev->bus, dev->address, dev->parent_address,
                    dev->parent_port, usb_speed_name(dev->speed),
                    dev->max_packet_size, dev->device_class,
                    dev->device_subclass, dev->device_protocol,
                    dev->usb_version, dev->vendor_id, dev->product_id,
                    dev->device_version, dev->configuration_count,
                    dev->interface_count, dev->port_count,
                    dev->keyboard_interfaces, dev->mouse_interfaces,
                    dev->manufacturer, dev->product, dev->serial);
    }
    kfree(devices);
}

static void proc_fill_dmesg(char* buf, size_t cap, size_t* len)
{
    *len = kmsg_read(buf, cap > 0 ? cap - 1 : 0);
    if (cap > 0)
        buf[*len] = '\0';
}

static void proc_fill_interrupts(char* buf, size_t cap, size_t* len)
{
    uint32_t virtio_irq = virtio_blk_get_irq();
    uint32_t virtio_net_irq = virtio_net_get_irq();
    uint32_t timer_irq = arch_platform_timer_irq();
    uint32_t uart_irq = arch_platform_uart_irq();
    const char* irq_name = irq_controller_name();

    proc_append(buf, cap, len, "           CPU0\n");
    if (arch_platform_has_smp_ipi()) {
        proc_append(buf, cap, len, "%3u: %10u %s  ipi-tlb\n",
                    IRQ_SGI_TLB_SHOOTDOWN,
                    irq_get_count(IRQ_SGI_TLB_SHOOTDOWN),
                    irq_name);
    }
    proc_append(buf, cap, len, "%3u: %10u %s  timer\n",
                timer_irq,
                irq_get_count(timer_irq),
                irq_name);
    proc_append(buf, cap, len, "%3u: %10u %s  uart0\n",
                uart_irq,
                irq_get_count(uart_irq),
                irq_name);
    if (IRQ_KEYBOARD != 0u) {
        proc_append(buf, cap, len, "%3u: %10u %s  uart0-legacy\n",
                    IRQ_KEYBOARD,
                    irq_get_count(IRQ_KEYBOARD),
                    irq_name);
    }
    if (virtio_irq != 0u) {
        proc_append(buf, cap, len, "%3u: %10u %s  virtio-blk\n",
                    virtio_irq,
                    irq_get_count(virtio_irq),
                    irq_name);
    }
    if (virtio_net_is_initialized() && virtio_net_irq != 0u) {
        proc_append(buf, cap, len, "%3u: %10u %s  virtio-net\n",
                    virtio_net_irq,
                    irq_get_count(virtio_net_irq),
                    irq_name);
    }
    proc_append(buf, cap, len, "TOT: %10u\n", irq_get_total_count());
    proc_append(buf, cap, len, "LAST:%10u\n", irq_get_last_id());
}

static void proc_fill_net_dev(char* buf, size_t cap, size_t* len)
{
    uint32_t index;

    proc_append(buf, cap, len,
                "Inter-|   Receive                                                |  Transmit\n");
    proc_append(buf, cap, len,
                " face |bytes    packets errs drop fifo frame compressed multicast|bytes    packets errs drop fifo colls carrier compressed\n");

    for (index = 0u; index < net_device_count(); index++) {
        net_device_t *device = net_device_get(index);
        net_ipv4_config_t config;
        char address[20];

        if (!device)
            continue;
        proc_append(buf, cap, len,
                    "%6s: %8llu %7llu %4u %4llu %4u %5u %10u %9u %8llu %7llu %4u %4llu %4u %5u %7u %10u\n",
                    device->name,
                    (unsigned long long)device->rx_bytes,
                    (unsigned long long)device->rx_packets,
                    0u, (unsigned long long)device->rx_drops,
                    0u, 0u, 0u, 0u,
                    (unsigned long long)device->tx_bytes,
                    (unsigned long long)device->tx_packets,
                    0u, (unsigned long long)device->tx_drops,
                    0u, 0u, 0u, 0u);
        if (net_stack_get_config(device, &config) == 0 &&
            config.configured) {
            net_stack_format_ipv4(config.address, address, sizeof(address));
        } else {
            snprintf(address, sizeof(address), "unconfigured");
        }
        proc_append(buf, cap, len,
                    "        mac %02X:%02X:%02X:%02X:%02X:%02X link %u inet %s mtu %u\n",
                    device->mac[0], device->mac[1], device->mac[2],
                    device->mac[3], device->mac[4], device->mac[5],
                    (uint32_t)device->link_state, address, device->mtu);
    }
}

static uint32_t proc_tcp_addr_le(uint32_t ip)
{
    return ((ip & 0x000000FFu) << 24) |
           ((ip & 0x0000FF00u) << 8)  |
           ((ip & 0x00FF0000u) >> 8)  |
           ((ip & 0xFF000000u) >> 24);
}

static void proc_fill_net_tcp(char* buf, size_t cap, size_t* len)
{
    uint32_t local_ip = 0;
    uint16_t local_port = 0;
    uint32_t listener_state = 0;
    uint32_t pending_accept = 0;
    uint32_t accepted_state = 0;

    proc_append(buf, cap, len,
                "  sl  local_address rem_address   st tx_queue rx_queue tr tm->when retrnsmt   uid  timeout inode\n");

    net_socket_get_tcp_diag(&local_ip, &local_port, &listener_state,
                            &accepted_state, &pending_accept);

    if (listener_state) {
        proc_append(buf, cap, len,
                    "   0: %08X:%04X %08X:%04X %02X %08X:%08X 00:%08X %08X %5u %8u %u\n",
                    proc_tcp_addr_le(local_ip), local_port,
                    0u, 0u, listener_state, 0u, 0u, 0u, 0u, 0u,
                    pending_accept, 0u);
    }

    if (accepted_state) {
        proc_append(buf, cap, len,
                    "   1: %08X:%04X %08X:%04X %02X %08X:%08X 00:%08X %08X %5u %8u %u\n",
                    proc_tcp_addr_le(local_ip), local_port,
                    0u, 0u, accepted_state, 0u, 0u, 0u, 0u, 0u,
                    pending_accept, 0u);
    }
}

static void proc_fill_fs_ext2(char* buf, size_t cap, size_t* len)
{
    ext2_stats_t st;

    ext2_get_stats(&st);
    proc_append(buf, cap, len, "mounted %u\n", st.mounted);
    proc_append(buf, cap, len, "dirty %u\n", st.dirty);
    proc_append(buf, cap, len, "block_size %u\n", st.block_size);
    proc_append(buf, cap, len, "blocks %u\n", st.blocks_count);
    proc_append(buf, cap, len, "groups %u\n", st.groups_count);
    proc_append(buf, cap, len, "blocks_per_group %u\n", st.blocks_per_group);
    proc_append(buf, cap, len, "inodes_per_group %u\n", st.inodes_per_group);
    proc_append(buf, cap, len, "cache_hits %u\n", st.cache_hits);
    proc_append(buf, cap, len, "cache_misses %u\n", st.cache_misses);
    proc_append(buf, cap, len, "cache_writes %u\n", st.cache_writes);
    proc_append(buf, cap, len, "cache_dirty_blocks %u\n", st.cache_dirty_blocks);
    proc_append(buf, cap, len, "writeback_batches %u\n", st.writeback_batches);
    proc_append(buf, cap, len, "cache_waits %u\n", st.cache_waits);
    proc_append(buf, cap, len, "op_waits %u\n", st.op_waits);
    proc_append(buf, cap, len, "read_blocks %u\n", st.read_blocks);
    proc_append(buf, cap, len, "write_blocks %u\n", st.write_blocks);
    proc_append(buf, cap, len, "syncs %u\n", st.syncs);
    proc_append(buf, cap, len, "sync_errors %u\n", st.sync_errors);
    proc_append(buf, cap, len, "check_errors %u\n", st.check_errors);
    proc_append(buf, cap, len, "sb_free_blocks %u\n", st.sb_free_blocks);
    proc_append(buf, cap, len, "sb_free_inodes %u\n", st.sb_free_inodes);
    proc_append(buf, cap, len, "gd_free_blocks_sum %u\n", st.gd_free_blocks_sum);
    proc_append(buf, cap, len, "gd_free_inodes_sum %u\n", st.gd_free_inodes_sum);
    proc_append(buf, cap, len, "gd_used_dirs_sum %u\n", st.gd_used_dirs_sum);
}

static void proc_fill_fs_ext2_check(char* buf, size_t cap, size_t* len)
{
    (void)ext2_check(buf, cap, len);
}

static void proc_fill_fs_fat32(char* buf, size_t cap, size_t* len)
{
    fat32_stats_t st;

    fat32_get_stats(&st);
    proc_append(buf, cap, len, "mounted %u\n", st.mounted);
    proc_append(buf, cap, len, "dirty %u\n", st.dirty);
    proc_append(buf, cap, len, "bytes_per_cluster %u\n", st.bytes_per_cluster);
    proc_append(buf, cap, len, "cluster_reads %u\n", st.cluster_reads);
    proc_append(buf, cap, len, "cluster_writes %u\n", st.cluster_writes);
    proc_append(buf, cap, len, "fat_syncs %u\n", st.fat_syncs);
    proc_append(buf, cap, len, "fat_sync_sectors %u\n", st.fat_sync_sectors);
    if (st.dirty_first_sector == ~0u)
        proc_append(buf, cap, len, "dirty_range none\n");
    else
        proc_append(buf, cap, len, "dirty_range %u-%u\n",
                    st.dirty_first_sector, st.dirty_last_sector);
}

static const char* proc_sched_event_name(uint32_t event)
{
    switch (event) {
        case SCHED_TRACE_FS_WAIT_TIMEOUT: return "fs_wait_timeout";
        case SCHED_TRACE_REFUSE_CRITICAL: return "refuse_critical";
        case SCHED_TRACE_REFUSE_BROKEN_LIST: return "refuse_broken_list";
        case SCHED_TRACE_REFUSE_NULL_NEXT: return "refuse_null_next";
        case SCHED_TRACE_REFUSE_INVALID_TASK: return "refuse_invalid_task";
        case SCHED_TRACE_REFUSE_LOOP: return "refuse_loop";
        case SCHED_TRACE_READY_REFUSE_DEAD: return "ready_refuse_dead";
        case SCHED_TRACE_READY_REFUSE_CORRUPT: return "ready_refuse_corrupt";
        case SCHED_TRACE_READY_REFUSE_REMOTE_RUNNING: return "ready_refuse_remote_running";
        case SCHED_TRACE_SLEEP_OVERSHOOT: return "sleep_overshoot";
        default: return "unknown";
    }
}

static const char* proc_syscall_name(uint32_t nr)
{
    switch (nr) {
        case 0: return "-";
        case __NR_exit: return "exit";
        case __NR_fork: return "fork";
        case __NR_read: return "read";
        case __NR_write: return "write";
        case __NR_open: return "open";
        case __NR_close: return "close";
        case __NR_waitpid: return "waitpid";
        case __NR_mknod: return "mknod";
        case __NR_link: return "link";
        case __NR_unlink: return "unlink";
        case __NR_execve: return "execve";
        case __NR_spawnve: return "spawnve";
        case __NR_chdir: return "chdir";
        case __NR_alarm: return "alarm";
        case __NR_pause: return "pause";
        case __NR_utime: return "utime";
        case __NR_lseek: return "lseek";
        case __NR_times: return "times";
        case __NR_mount: return "mount";
        case __NR_umount: return "umount";
        case __NR_access: return "access";
        case __NR_sync: return "sync";
        case __NR_chmod: return "chmod";
        case __NR_chown: return "chown";
        case __NR_rename: return "rename";
        case __NR_mkdir: return "mkdir";
        case __NR_rmdir: return "rmdir";
        case __NR_dup: return "dup";
        case __NR_dup2: return "dup2";
        case __NR_setsid: return "setsid";
        case __NR_sigaction: return "sigaction";
        case __NR_setrlimit: return "setrlimit";
        case __NR_getrlimit: return "getrlimit";
        case __NR_sigsuspend: return "sigsuspend";
        case __NR_sigpending: return "sigpending";
        case __NR_sigprocmask: return "sigprocmask";
        case __NR_brk: return "brk";
        case __NR_geteuid: return "geteuid";
        case __NR_getegid: return "getegid";
        case __NR_truncate: return "truncate";
        case __NR_ioctl: return "ioctl";
        case __NR_fsync: return "fsync";
        case __NR_fcntl: return "fcntl";
        case __NR_select: return "select";
        case __NR_poll: return "poll";
        case __NR_readv: return "readv";
        case __NR_writev: return "writev";
        case __NR_pread: return "pread";
        case __NR_pwrite: return "pwrite";
        case __NR_wait4: return "wait4";
        case __NR_getrusage: return "getrusage";
        case __NR_mprotect: return "mprotect";
        case __NR_uname: return "uname";
        case __NR_getsid: return "getsid";
        case __NR_pipe: return "pipe";
        case __NR_symlink: return "symlink";
        case __NR_readlink: return "readlink";
        case __NR_ftruncate: return "ftruncate";
        case __NR_statfs: return "statfs";
        case __NR_statvfs: return "statvfs";
        case __NR_fstatvfs: return "fstatvfs";
        case __NR_utimensat: return "utimensat";
        case __NR_futimens: return "futimens";
        case __NR_stat: return "stat";
        case __NR_lstat: return "lstat";
        case __NR_fstat: return "fstat";
        case __NR_getdents: return "getdents";
        case __NR_nanosleep: return "nanosleep";
        case __NR_sched_yield: return "sched_yield";
        case __NR_clock_gettime: return "clock_gettime";
        case __NR_clock_getres: return "clock_getres";
        case __NR_mmap: return "mmap";
        case __NR_munmap: return "munmap";
        case __NR_connect: return "connect";
        default: return "?";
    }
}

static void proc_fill_sched_trace(char* buf, size_t cap, size_t* len)
{
    sched_trace_event_t *events;
    uint32_t total = 0;
    uint32_t written = 0;

    events = kmalloc(sizeof(*events) * SCHED_TRACE_SIZE);
    if (!events) {
        proc_append(buf, cap, len, "sched trace unavailable: out of memory\n");
        return;
    }
    sched_trace_snapshot(events, SCHED_TRACE_SIZE, &total, &written);
    proc_append(buf, cap, len,
                "seq tick event pid tid state wake syscall current_pid current_tid current_syscall name current\n");

    for (uint32_t i = 0; i < written; i++) {
        sched_trace_event_t* e = &events[i];
        proc_append(buf, cap, len,
                    "%u %u %s pid=%u tid=%u st=%u wake=%u sc=%u(%s) cur=%u/%u csc=%u(%s) task=%p next=%p prev=%p name=%s current=%s\n",
                    e->seq,
                    e->tick,
                    proc_sched_event_name(e->event),
                    e->pid,
                    e->tid,
                    e->state,
                    e->wakeup_time,
                    e->syscall,
                    proc_syscall_name(e->syscall),
                    e->current_pid,
                    e->current_tid,
                    e->current_syscall,
                    proc_syscall_name(e->current_syscall),
                    (void*)e->task_ptr,
                    (void*)e->next_ptr,
                    (void*)e->prev_ptr,
                    e->name,
                    e->current_name);
    }
    proc_append(buf, cap, len, "total %u shown %u\n", total, written);
    kfree(events);
}

static const char* proc_sched_pick_reason(uint32_t reason)
{
    switch (reason) {
        case SCHED_PICK_PRIORITY: return "priority";
        case SCHED_PICK_AGING:    return "aging";
        case SCHED_PICK_DEBT:     return "debt";
        case SCHED_PICK_WAIT:     return "wait";
        default:                  return "none";
    }
}

static void proc_fill_sched(char* buf, size_t cap, size_t* len)
{
    scheduler_stats_t stats;

    scheduler_get_stats(&stats);

    proc_append(buf, cap, len, "policy %s\n", SCHED_POLICY_NAME);
    proc_append(buf, cap, len, "class SCHED_OTHER\n");
    proc_append(buf, cap, len, "description priority round-robin with bounded aging and CPU debt fairness\n");
    proc_append(buf, cap, len, "priority_direction lower-is-higher\n");
    proc_append(buf, cap, len, "priority_levels %u\n", stats.policy_levels);
    proc_append(buf, cap, len, "default_priority %u\n", stats.default_priority);
    proc_append(buf, cap, len, "idle_priority %u\n", stats.idle_priority);
    proc_append(buf, cap, len, "nice_range %d %d\n", stats.nice_min, stats.nice_max);
    proc_append(buf, cap, len, "nice_mapping scheduler_priority=nice+%u\n",
                stats.default_priority);
    proc_append(buf, cap, len, "quantum_ticks %u\n", stats.quantum_ticks);
    proc_append(buf, cap, len, "aging_step_ticks %u\n", stats.aging_step_ticks);
    proc_append(buf, cap, len, "aging_max_bonus %u\n", stats.aging_max_bonus);
    proc_append(buf, cap, len, "aging_selections %u\n", stats.aging_selections);
    proc_append(buf, cap, len, "debt_decay_ticks %u\n", stats.debt_decay_ticks);
    proc_append(buf, cap, len, "debt_selections %u\n", stats.debt_selections);
    proc_append(buf, cap, len, "last_pick reason=%s tid=%u pid=%u prio=%u effective=%u debt=%u waited=%u scanned=%u\n",
                proc_sched_pick_reason(stats.last_pick_reason),
                stats.last_pick_tid,
                stats.last_pick_pid,
                stats.last_pick_priority,
                stats.last_pick_effective_priority,
                stats.last_pick_debt,
                stats.last_pick_waited_ticks,
                stats.last_scan_tasks);
    proc_append(buf, cap, len, "ready_debt max=%u avg=%u\n",
                stats.max_ready_debt,
                stats.avg_ready_debt);
    proc_append(buf, cap, len, "nr_running %u\n", stats.nr_running);
    proc_append(buf, cap, len, "nonempty_queues %u\n", stats.nonempty_queues);
    if (stats.highest_ready_priority < TASK_PRIORITY_LEVELS) {
        proc_append(buf, cap, len, "ready_priority_range %u %u\n",
                    stats.highest_ready_priority,
                    stats.lowest_ready_priority);
    } else {
        proc_append(buf, cap, len, "ready_priority_range none\n");
    }
    proc_append(buf, cap, len, "current tid=%u pid=%u prio=%u name=%s\n",
                stats.current_tid,
                stats.current_pid,
                stats.current_priority,
                stats.current_name[0] ? stats.current_name : "-");
    proc_append(buf, cap, len, "\n");
    proc_append(buf, cap, len, "cpu state    sched resched idle_tid tid    pid    prio name\n");

    for (uint32_t cpu = 0; cpu < smp_possible_cpu_count(); cpu++) {
        task_t* current = task_current_on_cpu(cpu);
        task_t* idle = task_idle_on_cpu(cpu);
        process_t* proc = current && current->type == TASK_TYPE_PROCESS ? current->process : NULL;
        bool schedulable = smp_scheduler_cpu_enabled(cpu);

        proc_append(buf, cap, len, "%3u %-8s %5s %7u %8u %6u %6u %4u %s\n",
                    cpu,
                    smp_cpu_state_name(cpu),
                    schedulable ? "yes" : "no",
                    scheduler_resched_pending_on_cpu(cpu) ? 1u : 0u,
                    idle ? idle->task_id : 0u,
                    current ? current->task_id : 0u,
                    proc ? (uint32_t)proc->pid : 0u,
                    current ? current->priority : 0u,
                    current ? current->name : "-");
    }

    proc_append(buf, cap, len, "\n");
    proc_append(buf, cap, len, "priority count\n");

    for (uint32_t prio = 0; prio < TASK_PRIORITY_LEVELS; prio++) {
        if (stats.priority_counts[prio] == 0)
            continue;
        proc_append(buf, cap, len, "%u %u\n", prio, stats.priority_counts[prio]);
    }
}

typedef struct proc_flag_name {
    uint32_t flag;
    const char* name;
} proc_flag_name_t;

static void proc_append_flag_names(char* buf, size_t cap, size_t* len,
                                   const char* label, uint32_t value,
                                   const proc_flag_name_t* names,
                                   size_t name_count)
{
    bool any = false;

    proc_append(buf, cap, len, "%s", label);
    for (size_t i = 0; i < name_count; i++) {
        if (value & names[i].flag) {
            proc_append(buf, cap, len, "%s%s", any ? "," : "", names[i].name);
            any = true;
        }
    }
    if (!any)
        proc_append(buf, cap, len, "none");
    proc_append(buf, cap, len, "\n");
}

static const proc_flag_name_t proc_tty_iflag_names[] = {
    { INLCR, "INLCR" },
    { IGNCR, "IGNCR" },
    { ICRNL, "ICRNL" },
    { IXON,  "IXON" },
    { IXOFF, "IXOFF" },
};

static const proc_flag_name_t proc_tty_oflag_names[] = {
    { OPOST, "OPOST" },
    { ONLCR, "ONLCR" },
    { OCRNL, "OCRNL" },
    { ONOCR, "ONOCR" },
    { ONLRET, "ONLRET" },
};

static const proc_flag_name_t proc_tty_lflag_names[] = {
    { ECHO, "ECHO" },
    { ICANON, "ICANON" },
    { ISIG, "ISIG" },
    { IEXTEN, "IEXTEN" },
    { ECHOE, "ECHOE" },
    { ECHOK, "ECHOK" },
    { ECHOCTL, "ECHOCTL" },
    { ECHOKE, "ECHOKE" },
};

static const proc_flag_name_t proc_tty_cflag_names[] = {
    { CS8, "CS8" },
    { CREAD, "CREAD" },
    { HUPCL, "HUPCL" },
};

static struct tty_struct *proc_tty_by_id(int tty_id)
{
    if (tty_id == TTY_GRAPHICS_ID)
        return &tty1;
    if (tty_id == TTY_SERIAL_ID)
        return &ttyS0;
    return &tty0;
}

static void proc_fill_tty_one(char* buf, size_t cap, size_t* len,
                              int tty_id, const char* name)
{
    uint32_t tty_tx_enqueued = 0;
    uint32_t tty_tx_drained = 0;
    uint32_t tty_tx_full_waits = 0;
    uint32_t tty_tx_drain_calls = 0;
    uint32_t tty_input_depth = 0;
    uint32_t tty_input_capacity = 0;
    uint32_t tty_eof_pending = 0;
    uint32_t tty_input_dropped = 0;
    uint32_t tty_iflag = 0;
    uint32_t tty_oflag = 0;
    uint32_t tty_lflag = 0;
    uint32_t tty_vmin = 0;
    uint32_t tty_vtime = 0;
    uint32_t tty_char_wakeups = 0;
    uint32_t tty_line_wakeups = 0;
    uint32_t tty_eof_wakeups = 0;
    uint32_t input_chars = 0;
    uint32_t ctrl_c_seen = 0;
    uint32_t sigint_delivered = 0;
    uint32_t sigint_missed = 0;
    uint32_t ctrl_z_seen = 0;
    uint32_t sigtstp_delivered = 0;
    uint32_t sigtstp_missed = 0;
    int last_signal = 0;
    pid_t last_signal_pgid = 0;
    int last_signal_delivered = 0;
    uart_stats_t uart_stats;
    struct termios tio;
    struct tty_struct *tty = proc_tty_by_id(tty_id);
    uint16_t rows = 0;
    uint16_t cols = 0;
    uint16_t xpixel = 0;
    uint16_t ypixel = 0;
    bool backend = tty_has_backend_for_id(tty_id);
    bool active = tty_get_active() == tty_id;
    bool pending = tty_output_pending_for_id(tty_id);
    unsigned long flags;

    tty_get_tx_stats_for_id(tty_id, &tty_tx_enqueued, &tty_tx_drained,
                            &tty_tx_full_waits, &tty_tx_drain_calls);
    tty_get_input_stats_for_id(tty_id, &tty_input_depth, &tty_input_capacity,
                               &tty_eof_pending, &tty_iflag, &tty_oflag,
                               &tty_lflag, &tty_vmin, &tty_vtime,
                               &tty_char_wakeups, &tty_line_wakeups,
                               &tty_eof_wakeups);
    tty_get_termios_for_id(tty_id, &tio);
    tty_get_winsize_for_id(tty_id, &rows, &cols, &xpixel, &ypixel);
    memset(&uart_stats, 0, sizeof(uart_stats));
    if (tty_id == uart_attached_tty_id())
        uart_get_stats(&uart_stats);

    spin_lock_irqsave(&tty->lock, &flags);
    input_chars = tty->input_chars;
    tty_input_dropped = tty->input_dropped;
    ctrl_c_seen = tty->ctrl_c_seen;
    sigint_delivered = tty->sigint_delivered;
    sigint_missed = tty->sigint_missed;
    ctrl_z_seen = tty->ctrl_z_seen;
    sigtstp_delivered = tty->sigtstp_delivered;
    sigtstp_missed = tty->sigtstp_missed;
    last_signal = tty->last_signal;
    last_signal_pgid = tty->last_signal_pgid;
    last_signal_delivered = tty->last_signal_delivered;
    spin_unlock_irqrestore(&tty->lock, flags);

    proc_append(buf, cap, len, "%s\n", name);
    proc_append(buf, cap, len, "device backend %s mode %s active %u output_pending %u input_route %s\n",
                backend ? "present" : "none",
                tty_is_output_only_for_id(tty_id) ? "output-only" : "interactive",
                active ? 1 : 0,
                pending ? 1 : 0,
                tty_id == uart_attached_tty_id() ? "uart" :
                (tty_id == TTY_GRAPHICS_ID && virtio_input_is_initialized()) ?
                "virtio-keyboard" : "platform-input");
    proc_append(buf, cap, len, "winsize rows %u cols %u xpixel %u ypixel %u\n",
                rows, cols, xpixel, ypixel);
    proc_append(buf, cap, len,
                "input depth %u capacity %u chars %u dropped %u eof %u\n",
                tty_input_depth, tty_input_capacity, input_chars,
                tty_input_dropped, tty_eof_pending);
    proc_append(buf, cap, len, "wake char %u line %u eof %u\n",
                tty_char_wakeups, tty_line_wakeups, tty_eof_wakeups);
    proc_append(buf, cap, len, "output enq %u drain %u full %u drain_calls %u\n",
                tty_tx_enqueued, tty_tx_drained, tty_tx_full_waits, tty_tx_drain_calls);
    if (tty_id == uart_attached_tty_id()) {
        proc_append(buf, cap, len,
                    "uart irq_rx %u irq_tx %u irq_err %u rx_chars %u\n",
                    uart_stats.rx_irq,
                    uart_stats.tx_irq,
                    uart_stats.err_irq,
                    uart_stats.rx_chars);
        proc_append(buf, cap, len,
                    "uart errors fe %u pe %u be %u oe %u fr 0x%08X rsr 0x%08X imsc 0x%08X mis 0x%08X\n",
                    uart_stats.frame_errors,
                    uart_stats.parity_errors,
                    uart_stats.break_errors,
                    uart_stats.overrun_errors,
                    uart_stats.fr,
                    uart_stats.rsr,
                    uart_stats.imsc,
                    uart_stats.mis);
    }
    proc_append(buf, cap, len, "flags iflag %u oflag %u lflag %u vmin %u vtime %u\n",
                tty_iflag, tty_oflag, tty_lflag, tty_vmin, tty_vtime);
    proc_append(buf, cap, len, "mode %s %s %s\n",
                (tty_lflag & ICANON) ? "canonical" : "raw",
                (tty_lflag & ISIG) ? "signal" : "nosignal",
                (tty_lflag & ECHO) ? "echo" : "noecho");
    proc_append_flag_names(buf, cap, len, "iflag_names ",
                           tty_iflag, proc_tty_iflag_names,
                           sizeof(proc_tty_iflag_names) / sizeof(proc_tty_iflag_names[0]));
    proc_append_flag_names(buf, cap, len, "oflag_names ",
                           tty_oflag, proc_tty_oflag_names,
                           sizeof(proc_tty_oflag_names) / sizeof(proc_tty_oflag_names[0]));
    proc_append_flag_names(buf, cap, len, "lflag_names ",
                           tty_lflag, proc_tty_lflag_names,
                           sizeof(proc_tty_lflag_names) / sizeof(proc_tty_lflag_names[0]));
    proc_append_flag_names(buf, cap, len, "cflag_names ",
                           tio.c_cflag, proc_tty_cflag_names,
                           sizeof(proc_tty_cflag_names) / sizeof(proc_tty_cflag_names[0]));
    proc_append(buf, cap, len, "cc intr %u quit %u erase %u kill %u eof %u susp %u werase %u\n",
                tio.c_cc[VINTR],
                tio.c_cc[VQUIT],
                tio.c_cc[VERASE],
                tio.c_cc[VKILL],
                tio.c_cc[VEOF],
                tio.c_cc[VSUSP],
                tio.c_cc[VWERASE]);
    proc_append(buf, cap, len, "jobctl fg_pgid %d read_wait_pid %d read_wait_state %d\n",
                tty_get_foreground_pgid_for_id(tty_id),
                tty_get_read_wait_pid_for_id(tty_id),
                tty_get_read_wait_state_for_id(tty_id));
    proc_append(buf, cap, len,
                "signal ctrl_c %u delivered %u missed %u ctrl_z %u delivered %u missed %u last %d pgid %d delivered %d\n",
                ctrl_c_seen,
                sigint_delivered,
                sigint_missed,
                ctrl_z_seen,
                sigtstp_delivered,
                sigtstp_missed,
                last_signal,
                last_signal_pgid,
                last_signal_delivered);
    if (tty_id == TTY_GRAPHICS_ID) {
        uint32_t vk_irq = 0;
        uint32_t vk_used = 0;
        uint32_t vk_keys = 0;
        uint32_t vk_emit = 0;
        uint32_t vk_type = 0;
        uint32_t vk_code = 0;
        uint32_t vk_value = 0;
        uint32_t vk_irq_status = 0;
        uint32_t vk_qsize = 0;
        uint32_t vk_last_used = 0;
        uint32_t vk_status = 0;

        virtio_input_get_stats(&vk_irq, &vk_used, &vk_keys, &vk_emit,
                               &vk_type, &vk_code, &vk_value,
                               &vk_irq_status, &vk_qsize, &vk_last_used,
                               &vk_status);
        proc_append(buf, cap, len,
                    "virtio_input init %u route_irq %u irq_count %u used %u keys %u emitted %u qsize %u last_used %u status 0x%02X last_irq 0x%X last_event type %u code %u value %u\n",
                    virtio_input_is_initialized() ? 1 : 0,
                    virtio_input_get_irq(),
                    vk_irq,
                    vk_used,
                    vk_keys,
                    vk_emit,
                    vk_qsize,
                    vk_last_used,
                    vk_status,
                    vk_irq_status,
                    vk_type,
                    vk_code,
                    vk_value);
    }
}

static void proc_fill_tty(char* buf, size_t cap, size_t* len)
{
    proc_fill_tty_one(buf, cap, len, TTY_CONSOLE_ID, "tty0");
    if (tty_has_backend_for_id(TTY_GRAPHICS_ID)) {
        proc_append(buf, cap, len, "\n");
        proc_fill_tty_one(buf, cap, len, TTY_GRAPHICS_ID, "tty1");
    }
    if (tty_has_backend_for_id(TTY_SERIAL_ID)) {
        proc_append(buf, cap, len, "\n");
        proc_fill_tty_one(buf, cap, len, TTY_SERIAL_ID, "ttyS0");
    }
}

static void proc_fill_stat(char* buf, size_t cap, size_t* len)
{
    timer_cpu_accounting_t cpu_accounting[ARMOS_MAX_CPUS];
    timer_cpu_accounting_t cpu_total = {0};
    uint32_t cpu_count = smp_possible_cpu_count();
    uint32_t live_tasks = kernel_lifecycle_stats.tasks_created -
                          kernel_lifecycle_stats.tasks_destroyed;
    uint32_t live_zombies = kernel_lifecycle_stats.zombies_created -
                            kernel_lifecycle_stats.zombies_reaped;
    uint32_t live_kstack = kernel_lifecycle_stats.stack_pages_allocated -
                           kernel_lifecycle_stats.stack_pages_freed;
    uint32_t phys_alloc = get_allocated_page_count();
    uint32_t phys_free = get_freed_page_count();
    uint32_t live_phys = phys_alloc - phys_free;
    uint32_t tty_tx_enqueued = 0;
    uint32_t tty_tx_drained = 0;
    uint32_t tty_tx_full_waits = 0;
    uint32_t tty_tx_drain_calls = 0;
    uint32_t tty_input_depth = 0;
    uint32_t tty_input_capacity = 0;
    uint32_t tty_eof_pending = 0;
    uint32_t tty_iflag = 0;
    uint32_t tty_oflag = 0;
    uint32_t tty_lflag = 0;
    uint32_t tty_vmin = 0;
    uint32_t tty_vtime = 0;
    uint32_t tty_char_wakeups = 0;
    uint32_t tty_line_wakeups = 0;
    uint32_t tty_eof_wakeups = 0;
    uint32_t tty_input_chars = 0;
    uint32_t tty_ctrl_c_seen = 0;
    uint32_t tty_sigint_delivered = 0;
    uint32_t tty_sigint_missed = 0;
    uint32_t tty_ctrl_z_seen = 0;
    uint32_t tty_sigtstp_delivered = 0;
    uint32_t tty_sigtstp_missed = 0;
    int tty_last_signal = 0;
    pid_t tty_last_signal_pgid = 0;
    int tty_last_signal_delivered = 0;
    pid_t tty_foreground_pgid = 0;
    pid_t tty_read_wait_pid = 0;
    int tty_read_wait_state = -1;
    const int tty_ids[] = {
        TTY_CONSOLE_ID, TTY_GRAPHICS_ID, TTY_SERIAL_ID
    };

    if (cpu_count > ARMOS_MAX_CPUS)
        cpu_count = ARMOS_MAX_CPUS;
    for (uint32_t cpu = 0; cpu < cpu_count; cpu++) {
        timer_cpu_accounting_read(cpu, &cpu_accounting[cpu]);
        cpu_total.user_ticks += cpu_accounting[cpu].user_ticks;
        cpu_total.system_ticks += cpu_accounting[cpu].system_ticks;
        cpu_total.irq_ticks += cpu_accounting[cpu].irq_ticks;
        cpu_total.idle_ticks += cpu_accounting[cpu].idle_ticks;
    }

    for (size_t i = 0; i < sizeof(tty_ids) / sizeof(tty_ids[0]); i++) {
        int tty_id = tty_ids[i];
        struct tty_struct *tty = proc_tty_by_id(tty_id);
        uint32_t tx_enqueued = 0;
        uint32_t tx_drained = 0;
        uint32_t tx_full_waits = 0;
        uint32_t tx_drain_calls = 0;
        uint32_t in_depth = 0;
        uint32_t in_capacity = 0;
        uint32_t eof_pending = 0;
        uint32_t iflag = 0;
        uint32_t oflag = 0;
        uint32_t lflag = 0;
        uint32_t vmin = 0;
        uint32_t vtime = 0;
        uint32_t char_wakeups = 0;
        uint32_t line_wakeups = 0;
        uint32_t eof_wakeups = 0;
        unsigned long flags;

        if (tty_id != TTY_CONSOLE_ID && !tty_has_backend_for_id(tty_id))
            continue;

        tty_get_tx_stats_for_id(tty_id, &tx_enqueued, &tx_drained,
                                &tx_full_waits, &tx_drain_calls);
        tty_get_input_stats_for_id(tty_id, &in_depth, &in_capacity,
                                   &eof_pending, &iflag, &oflag, &lflag,
                                   &vmin, &vtime, &char_wakeups,
                                   &line_wakeups, &eof_wakeups);

        tty_tx_enqueued += tx_enqueued;
        tty_tx_drained += tx_drained;
        tty_tx_full_waits += tx_full_waits;
        tty_tx_drain_calls += tx_drain_calls;
        tty_input_depth += in_depth;
        tty_input_capacity += in_capacity;
        tty_eof_pending += eof_pending;
        tty_char_wakeups += char_wakeups;
        tty_line_wakeups += line_wakeups;
        tty_eof_wakeups += eof_wakeups;

        if (tty_id == TTY_CONSOLE_ID) {
            tty_iflag = iflag;
            tty_oflag = oflag;
            tty_lflag = lflag;
            tty_vmin = vmin;
            tty_vtime = vtime;
            tty_foreground_pgid = tty_get_foreground_pgid_for_id(tty_id);
            tty_read_wait_pid = tty_get_read_wait_pid_for_id(tty_id);
            tty_read_wait_state = tty_get_read_wait_state_for_id(tty_id);
        }

        spin_lock_irqsave(&tty->lock, &flags);
        tty_input_chars += tty->input_chars;
        tty_ctrl_c_seen += tty->ctrl_c_seen;
        tty_sigint_delivered += tty->sigint_delivered;
        tty_sigint_missed += tty->sigint_missed;
        tty_ctrl_z_seen += tty->ctrl_z_seen;
        tty_sigtstp_delivered += tty->sigtstp_delivered;
        tty_sigtstp_missed += tty->sigtstp_missed;
        if (tty->last_signal) {
            tty_last_signal = tty->last_signal;
            tty_last_signal_pgid = tty->last_signal_pgid;
            tty_last_signal_delivered = tty->last_signal_delivered;
        }
        spin_unlock_irqrestore(&tty->lock, flags);
    }

    proc_append(buf, cap, len, "cpu  %u 0 %u %u 0 %u 0 0 0 0\n",
                cpu_total.user_ticks,
                cpu_total.system_ticks,
                cpu_total.idle_ticks,
                cpu_total.irq_ticks);
    for (uint32_t cpu = 0; cpu < cpu_count; cpu++) {
        proc_append(buf, cap, len, "cpu%u %u 0 %u %u 0 %u 0 0 0 0\n",
                    cpu,
                    cpu_accounting[cpu].user_ticks,
                    cpu_accounting[cpu].system_ticks,
                    cpu_accounting[cpu].idle_ticks,
                    cpu_accounting[cpu].irq_ticks);
    }
    proc_append(buf, cap, len, "intr %u\n", irq_get_total_count());
    task_t *task = task_current_local();
    proc_append(buf, cap, len, "ctxt %u\n",
                task ? task->switch_count : 0);
    proc_append(buf, cap, len, "processes %u\n", kernel_lifecycle_stats.tasks_created);
    proc_append(buf, cap, len, "procs_running %u\n", task_count);
    proc_append(buf, cap, len, "procs_blocked 0\n");
    proc_append(buf, cap, len, "uptime_ticks %u\n", get_system_ticks());
    proc_append(buf, cap, len, "tasks %u %u %u\n",
                live_tasks,
                kernel_lifecycle_stats.tasks_created,
                kernel_lifecycle_stats.tasks_destroyed);
    proc_append(buf, cap, len, "zombies %u %u %u\n",
                live_zombies,
                kernel_lifecycle_stats.zombies_created,
                kernel_lifecycle_stats.zombies_reaped);
    proc_append(buf, cap, len, "kstack %u %u %u\n",
                live_kstack,
                kernel_lifecycle_stats.stack_pages_allocated,
                kernel_lifecycle_stats.stack_pages_freed);
    proc_append(buf, cap, len, "phys %u %u %u\n",
                live_phys,
                phys_alloc,
                phys_free);
    proc_append(buf, cap, len, "forkfail %u\n", kernel_lifecycle_stats.failed_forks);
    proc_append(buf, cap, len, "sched_refuse %u\n", kernel_lifecycle_stats.scheduler_refused);
    proc_append(buf, cap, len, "sched_crit_repair %u\n",
                kernel_lifecycle_stats.scheduler_critical_repaired);
    proc_append(buf, cap, len, "ready_refuse %u\n", kernel_lifecycle_stats.ready_queue_refused);
    proc_append(buf, cap, len, "asid_rollovers %u\n", kernel_lifecycle_stats.asid_rollovers);
    proc_append(buf, cap, len, "state_set %u\n", kernel_lifecycle_stats.state_sync_repairs);
    proc_append(buf, cap, len, "signal_wake %u\n", kernel_lifecycle_stats.blocked_signal_wakeups);
    proc_append(buf, cap, len, "tty_stale %u\n", kernel_lifecycle_stats.tty_stale_waiters);
    proc_append(buf, cap, len, "fs_wait_timeout %u\n", kernel_lifecycle_stats.fs_wait_timeouts);
    proc_append(buf, cap, len, "sleep_deadline %u\n", kernel_lifecycle_stats.sleep_deadline_wakeups);
    proc_append(buf, cap, len, "sleep_overshoot %u\n", kernel_lifecycle_stats.sleep_overshoots);
    proc_append(buf, cap, len, "tty_tx %u %u %u %u\n",
                tty_tx_enqueued,
                tty_tx_drained,
                tty_tx_full_waits,
                tty_tx_drain_calls);
    proc_append(buf, cap, len, "tty_in %u %u %u %u %u %u %u %u\n",
                tty_input_depth,
                tty_input_capacity,
                tty_eof_pending,
                tty_iflag,
                tty_oflag,
                tty_lflag,
                tty_vmin,
                tty_vtime);
    proc_append(buf, cap, len, "tty_wake %u %u %u\n",
                tty_char_wakeups,
                tty_line_wakeups,
                tty_eof_wakeups);
    proc_append(buf, cap, len,
                "tty_diag fg_pgid %d read_wait_pid %d read_wait_state %d input %u ctrl_c %u sigint_delivered %u sigint_missed %u ctrl_z %u sigtstp_delivered %u sigtstp_missed %u last_signal %d last_pgid %d last_delivered %d\n",
                tty_foreground_pgid,
                tty_read_wait_pid,
                tty_read_wait_state,
                tty_input_chars,
                tty_ctrl_c_seen,
                tty_sigint_delivered,
                tty_sigint_missed,
                tty_ctrl_z_seen,
                tty_sigtstp_delivered,
                tty_sigtstp_missed,
                tty_last_signal,
                tty_last_signal_pgid,
                tty_last_signal_delivered);
}

static void proc_fill_tasks(char* buf, size_t cap, size_t* len)
{
    task_t* task;
    uint32_t count = 0;
    unsigned long flags;

    proc_append(buf, cap, len, "pid tid ppid state kind pri ctx pf cow stk name\n");

    spin_lock_irqsave(&task_lock, &flags);
    task = task_list_head;
    if (!task) {
        spin_unlock_irqrestore(&task_lock, flags);
        return;
    }

    do {
        process_t* proc = proc_task_process(task);
        proc_append(buf, cap, len, "%d %u %d %c %c %u %u %u %u %u %s\n",
                    proc ? proc->pid : 0,
                    task->task_id,
                    proc ? proc->ppid : 0,
                    proc_task_state_char(task->state),
                    task->type == TASK_TYPE_PROCESS ? 'P' :
                    task->type == TASK_TYPE_THREAD ? 'T' : 'K',
                    task->priority,
                    task->switch_count,
                    task->page_faults,
                    task->cow_faults,
                    task->stack_faults,
                    task->name);
        task = task->next;
        count++;
    } while (task && task != task_list_head && count < MAX_TASKS);

    spin_unlock_irqrestore(&task_lock, flags);
}

static void proc_fill_pid_status(pid_t pid, char* buf, size_t cap, size_t* len)
{
    unsigned long flags;
    task_t* task;
    process_t* proc;
    vm_space_t* vm;

    spin_lock_irqsave(&task_lock, &flags);
    task = proc_find_task_locked(pid);
    if (!task) {
        spin_unlock_irqrestore(&task_lock, flags);
        proc_append(buf, cap, len, "State:\tX (dead)\n");
        return;
    }

    proc = proc_task_process(task);
    vm = proc ? proc->vm : NULL;
    uint32_t now = get_system_ticks();
    uint32_t ready_wait = 0;
    uint32_t effective_prio = task->priority;
    uint32_t debt_score = task->sched_debt;

    if (task->ready_since_tick && now >= task->ready_since_tick) {
        uint32_t decay;

        ready_wait = now - task->ready_since_tick;
        decay = ready_wait / SCHED_DEBT_DECAY_TICKS;
        debt_score = decay >= task->sched_debt ? 0 : task->sched_debt - decay;

        if (task->priority < TASK_PRIORITY_LEVELS) {
            uint32_t bonus = ready_wait / SCHED_AGING_STEP_TICKS;
            if (bonus > SCHED_AGING_MAX_BONUS)
                bonus = SCHED_AGING_MAX_BONUS;
            if (bonus > effective_prio)
                bonus = effective_prio;
            effective_prio -= bonus;
        }
    }

    proc_append(buf, cap, len, "Name:\t%s\n", task->name);
    proc_append(buf, cap, len, "State:\t%c (%s)\n",
                proc_task_state_char(task->state),
                proc_task_state_name(task->state));
    proc_append(buf, cap, len, "Tgid:\t%d\n", proc ? proc->pid : 0);
    proc_append(buf, cap, len, "Pid:\t%d\n", proc ? proc->pid : 0);
    proc_append(buf, cap, len, "Tid:\t%u\n", task->task_id);
    proc_append(buf, cap, len, "PPid:\t%d\n", proc ? proc->ppid : 0);
    proc_append(buf, cap, len, "PGid:\t%d\n", proc ? proc->pgid : 0);
    proc_append(buf, cap, len, "Sid:\t%d\n", proc ? proc->sid : 0);
    proc_append(buf, cap, len, "Tty:\t%d\n", proc ? proc->controlling_tty : -1);
    proc_append(buf, cap, len, "Uid:\t%u\n", proc ? proc->uid : 0);
    proc_append(buf, cap, len, "Gid:\t%u\n", proc ? proc->gid : 0);
    proc_append(buf, cap, len, "Priority:\t%u\n", task->priority);
    proc_append(buf, cap, len, "EffectivePriority:\t%u\n", effective_prio);
    proc_append(buf, cap, len, "SchedDebt:\t%u\n", task->sched_debt);
    proc_append(buf, cap, len, "DebtScore:\t%u\n", debt_score);
    proc_append(buf, cap, len, "ReadyWaitTicks:\t%u\n", ready_wait);
    proc_append(buf, cap, len, "CPU:\t%d\n",
                task->running_cpu != TASK_CPU_NONE ? (int)task->running_cpu :
                task->last_cpu != TASK_CPU_NONE ? (int)task->last_cpu : -1);
    proc_append(buf, cap, len, "KStack:\t%u kB\n", KERNEL_TASK_STACK_SIZE / 1024);
    proc_append(buf, cap, len, "Heap:\t%u kB\n",
                (vm && vm->brk >= vm->heap_start) ? ((vm->brk - vm->heap_start) / 1024u) : 0);
    proc_append(buf, cap, len, "VmSize:\t%u kB\n", vm_virtual_kb(vm));
    proc_append(buf, cap, len, "VmRSS:\t%u kB\n", vm_resident_kb(vm));
    proc_append(buf, cap, len, "L2Tables:\t%u\n", vm_page_table_count(vm));
    proc_append(buf, cap, len, "CtxSwitches:\t%u\n", task->switch_count);
    proc_append(buf, cap, len, "RuntimeTicks:\t%u\n", (uint32_t)task_runtime_ticks(task));
    proc_append(buf, cap, len, "UserTicks:\t%u\n", (uint32_t)task->user_runtime);
    proc_append(buf, cap, len, "SystemTicks:\t%u\n", (uint32_t)task->system_runtime);
    proc_append(buf, cap, len, "PageFaults:\t%u\n", task->page_faults);
    proc_append(buf, cap, len, "CowFaults:\t%u\n", task->cow_faults);
    proc_append(buf, cap, len, "StackFaults:\t%u\n", task->stack_faults);
    proc_append(buf, cap, len, "LazyFaults:\t%u\n", task->lazy_faults);
    spin_unlock_irqrestore(&task_lock, flags);
}

static void proc_fill_pid_stat(pid_t pid, char* buf, size_t cap, size_t* len)
{
    unsigned long flags;
    task_t* task;
    process_t* proc;

    spin_lock_irqsave(&task_lock, &flags);
    task = proc_find_task_locked(pid);
    if (!task) {
        spin_unlock_irqrestore(&task_lock, flags);
        return;
    }

    proc = proc_task_process(task);
    proc_append(buf, cap, len, "%d (%s) %c %d %d %d %d %u %u %u %u %u %u %u\n",
                proc ? proc->pid : 0,
                task->name,
                proc_task_state_char(task->state),
                proc ? proc->ppid : 0,
                proc ? proc->pgid : 0,
                proc ? proc->sid : 0,
                proc ? proc->controlling_tty : -1,
                task->page_faults,
                task->cow_faults,
                task->stack_faults,
                task->switch_count,
                (uint32_t)task_runtime_ticks(task),
                (uint32_t)task->user_runtime,
                (uint32_t)task->system_runtime);
    spin_unlock_irqrestore(&task_lock, flags);
}

static void proc_fill_pid_maps(pid_t pid, char* buf, size_t cap, size_t* len)
{
    unsigned long flags;
    task_t* task;
    process_t* proc;
    vm_space_t* vm;

    spin_lock_irqsave(&task_lock, &flags);
    task = proc_find_task_locked(pid);
    proc = task ? proc_task_process(task) : NULL;
    vm = proc ? proc->vm : NULL;

    for (vma_t* vma = vm ? vm->vma_list : NULL; vma; vma = vma->next) {
        char perms[5];
        perms[0] = (vma->flags & VMA_READ) ? 'r' : '-';
        perms[1] = (vma->flags & VMA_WRITE) ? 'w' : '-';
        perms[2] = (vma->flags & VMA_EXEC) ? 'x' : '-';
        perms[3] = (vma->flags & VMA_SHARED) ? 's' : 'p';
        perms[4] = '\0';
        proc_append(buf, cap, len, "%08x-%08x %s 00000000 00:00 0\n",
                    vma->start, vma->end, perms);
    }

    spin_unlock_irqrestore(&task_lock, flags);
}

static void proc_fill_pid_blob(pid_t pid, uint32_t type, char* buf, size_t cap, size_t* len)
{
    unsigned long flags;
    task_t* task;
    process_t* proc;
    size_t src_len;
    const char* src;

    spin_lock_irqsave(&task_lock, &flags);
    task = proc_find_task_locked(pid);
    proc = task ? proc_task_process(task) : NULL;
    if (!proc) {
        spin_unlock_irqrestore(&task_lock, flags);
        return;
    }

    if (type == PROC_PID_CMDLINE) {
        src = proc->cmdline;
        src_len = proc->cmdline_len;
    } else {
        src = proc->environ;
        src_len = proc->environ_len;
    }

    if (src_len > cap - *len)
        src_len = cap - *len;
    if (src_len > 0) {
        memcpy(buf + *len, src, src_len);
        *len += src_len;
    }
    spin_unlock_irqrestore(&task_lock, flags);
}

static int proc_generate_file(uint32_t ino, char* buf, size_t cap, size_t* len)
{
    pid_t pid;

    *len = 0;

    switch (ino) {
        case PROC_INO_MEMINFO: proc_fill_meminfo(buf, cap, len); return 0;
        case PROC_INO_UPTIME:  proc_fill_uptime(buf, cap, len);  return 0;
        case PROC_INO_LOADAVG: proc_fill_loadavg(buf, cap, len); return 0;
        case PROC_INO_MOUNTS:  proc_fill_mounts(buf, cap, len);  return 0;
        case PROC_INO_STAT:    proc_fill_stat(buf, cap, len);    return 0;
        case PROC_INO_TASKS:   proc_fill_tasks(buf, cap, len);   return 0;
        case PROC_INO_CPUINFO: proc_fill_cpuinfo(buf, cap, len); return 0;
        case PROC_INO_SMP:     proc_fill_smp(buf, cap, len);     return 0;
        case PROC_INO_SMP_IPI: proc_fill_smp_ipi(buf, cap, len); return 0;
        case PROC_INO_FILESYSTEMS: proc_fill_filesystems(buf, cap, len); return 0;
        case PROC_INO_PARTITIONS: proc_fill_partitions(buf, cap, len); return 0;
        case PROC_INO_DISKSTATS: proc_fill_diskstats(buf, cap, len); return 0;
        case PROC_INO_USB: proc_fill_usb(buf, cap, len); return 0;
        case PROC_INO_DMESG:   proc_fill_dmesg(buf, cap, len); return 0;
        case PROC_INO_INTERRUPTS: proc_fill_interrupts(buf, cap, len); return 0;
        case PROC_INO_TTY:     proc_fill_tty(buf, cap, len); return 0;
        case PROC_INO_NET_DEV: proc_fill_net_dev(buf, cap, len); return 0;
        case PROC_INO_NET_TCP: proc_fill_net_tcp(buf, cap, len); return 0;
        case PROC_INO_FS_EXT2_STATS: proc_fill_fs_ext2(buf, cap, len); return 0;
        case PROC_INO_FS_EXT2_CHECK: proc_fill_fs_ext2_check(buf, cap, len); return 0;
        case PROC_INO_FS_FAT32_STATS: proc_fill_fs_fat32(buf, cap, len); return 0;
        case PROC_INO_SCHED: proc_fill_sched(buf, cap, len); return 0;
        case PROC_INO_SCHED_TRACE: proc_fill_sched_trace(buf, cap, len); return 0;
        default: break;
    }

    if (ino >= PROC_PID_BASE) {
        pid = proc_ino_pid(ino);
        if (!proc_pid_exists(pid))
            return -ENOENT;

        switch (proc_ino_type(ino)) {
            case PROC_PID_STATUS: proc_fill_pid_status(pid, buf, cap, len); return 0;
            case PROC_PID_STAT:   proc_fill_pid_stat(pid, buf, cap, len);   return 0;
            case PROC_PID_MAPS:   proc_fill_pid_maps(pid, buf, cap, len);   return 0;
            case PROC_PID_CMDLINE:
            case PROC_PID_ENVIRON:
                proc_fill_pid_blob(pid, proc_ino_type(ino), buf, cap, len);
                return 0;
        }
    }

    return -EINVAL;
}

static int procfs_open(inode_t* inode, file_t* file)
{
    proc_file_data_t* data;
    int ret;

    if (!inode || !file) return -EINVAL;
    if (S_ISDIR(inode->mode)) return 0;

    data = kmalloc(sizeof(*data));
    if (!data) return -ENOMEM;
    data->data = kmalloc(PROC_FILE_BUFFER_SIZE);
    if (!data->data) {
        kfree(data);
        return -ENOMEM;
    }

    ret = proc_generate_file(inode->first_cluster, data->data,
                             PROC_FILE_BUFFER_SIZE, &data->size);
    if (ret < 0) {
        kfree(data->data);
        kfree(data);
        return ret;
    }

    file->private_data = data;
    file->offset = 0;
    inode->size = data->size;
    inode->blocks = (data->size + 511) / 512;
    return 0;
}

static int procfs_close(file_t* file)
{
    proc_file_data_t* data;

    if (!file) return -EINVAL;

    data = (proc_file_data_t*)file->private_data;
    if (data) {
        if (data->data) kfree(data->data);
        kfree(data);
        file->private_data = NULL;
    }
    return 0;
}

static ssize_t procfs_read(file_t* file, void* buffer, size_t count)
{
    proc_file_data_t* data;
    size_t remaining;

    if (!file || !buffer) return -EINVAL;
    if (S_ISDIR(file->inode->mode)) return -EISDIR;

    data = (proc_file_data_t*)file->private_data;
    if (!data || !data->data) return -EINVAL;
    if (file->offset >= data->size) return 0;

    remaining = data->size - file->offset;
    if (count > remaining) count = remaining;
    memcpy(buffer, data->data + file->offset, count);
    file->offset += count;
    return (ssize_t)count;
}

static ssize_t procfs_write(file_t* file, const void* buffer, size_t count)
{
    char cmd[16];
    size_t n;

    if (!file || !file->inode || !buffer)
        return -EINVAL;

    if (file->inode->first_cluster != PROC_INO_SMP_IPI)
        return -EROFS;

    n = count;
    if (n >= sizeof(cmd))
        n = sizeof(cmd) - 1;
    memcpy(cmd, buffer, n);
    cmd[n] = '\0';

    while (n > 0 && (cmd[n - 1] == '\n' || cmd[n - 1] == '\r' ||
                     cmd[n - 1] == ' ' || cmd[n - 1] == '\t')) {
        cmd[--n] = '\0';
    }

    if (strcmp(cmd, "self") == 0 || strcmp(cmd, "0") == 0) {
        irq_send_ipi(1u << smp_boot_cpu_id(), IRQ_SGI_TLB_SHOOTDOWN);
        return (ssize_t)count;
    }

    if (strcmp(cmd, "others") == 0) {
        irq_send_ipi_others(IRQ_SGI_TLB_SHOOTDOWN);
        return (ssize_t)count;
    }

    if (strcmp(cmd, "tlb") == 0) {
        tlb_shootdown_all();
        return (ssize_t)count;
    }

    return -EROFS;
}

static off_t procfs_lseek(file_t* file, off_t offset, int whence)
{
    proc_file_data_t* data;
    off_t size;
    off_t pos;

    if (!file) return -EINVAL;
    data = (proc_file_data_t*)file->private_data;
    size = data ? (off_t)data->size : 0;

    switch (whence) {
        case SEEK_SET: pos = offset; break;
        case SEEK_CUR: pos = (off_t)file->offset + offset; break;
        case SEEK_END: pos = size + offset; break;
        default: return -EINVAL;
    }

    if (pos < 0) return -EINVAL;
    file->offset = (uint32_t)pos;
    return pos;
}

static void proc_fill_dirent(dirent_t* dirent, uint32_t ino, uint8_t type, const char* name)
{
    dirent->d_ino = ino;
    dirent->d_type = type;
    dirent->d_reclen = sizeof(*dirent);
    strncpy(dirent->d_name, name, sizeof(dirent->d_name) - 1);
    dirent->d_name[sizeof(dirent->d_name) - 1] = '\0';
}

static int procfs_root_readdir(file_t* file, dirent_t* dirent)
{
    static const struct {
        const char* name;
        uint32_t ino;
        uint8_t type;
    } entries[] = {
        { ".",       PROC_INO_ROOT,    DT_DIR },
        { "..",      PROC_INO_ROOT,    DT_DIR },
        { "meminfo", PROC_INO_MEMINFO, DT_REG },
        { "uptime",  PROC_INO_UPTIME,  DT_REG },
        { "loadavg", PROC_INO_LOADAVG, DT_REG },
        { "mounts",  PROC_INO_MOUNTS,  DT_REG },
        { "stat",    PROC_INO_STAT,    DT_REG },
        { "tasks",   PROC_INO_TASKS,   DT_REG },
        { "cpuinfo", PROC_INO_CPUINFO, DT_REG },
        { "smp",     PROC_INO_SMP,     DT_REG },
        { "filesystems", PROC_INO_FILESYSTEMS, DT_REG },
        { "partitions", PROC_INO_PARTITIONS, DT_REG },
        { "diskstats", PROC_INO_DISKSTATS, DT_REG },
        { "usb",     PROC_INO_USB,       DT_REG },
        { "dmesg",   PROC_INO_DMESG,  DT_REG },
        { "interrupts", PROC_INO_INTERRUPTS, DT_REG },
        { "tty",     PROC_INO_TTY,    DT_REG },
        { "net",     PROC_INO_NET_DIR, DT_DIR },
        { "fs",      PROC_INO_FS_DIR, DT_DIR },
        { "sched",   PROC_INO_SCHED, DT_REG },
        { "sched_trace", PROC_INO_SCHED_TRACE, DT_REG },
        { "smp_ipi", PROC_INO_SMP_IPI, DT_REG },
        { "self",    PROC_INO_SELF,    DT_LNK },
    };
    uint32_t offset = file->offset;
    uint32_t static_count = sizeof(entries) / sizeof(entries[0]);
    task_t* task;
    uint32_t index = 0;
    uint32_t walked = 0;
    pid_t found_pid = 0;
    char pid_name[16];
    unsigned long flags;

    if (offset < static_count) {
        proc_fill_dirent(dirent, entries[offset].ino, entries[offset].type,
                         entries[offset].name);
        file->offset++;
        return 1;
    }

    offset -= static_count;

    if (!proc_try_lock_tasks(&flags))
        return 0;

    task = task_list_head;
    if (!task) {
        spin_unlock_irqrestore(&task_lock, flags);
        return 0;
    }

    do {
        process_t* proc = proc_task_process(task);
        if (proc && proc->pid > 0) {
            if (index == offset) {
                found_pid = proc->pid;
                spin_unlock_irqrestore(&task_lock, flags);
                snprintf(pid_name, sizeof(pid_name), "%d", found_pid);
                proc_fill_dirent(dirent, proc_pid_ino(found_pid, PROC_PID_DIR),
                                 DT_DIR, pid_name);
                file->offset++;
                return 1;
            }
            index++;
        }
        task = task->next;
        walked++;
    } while (task && task != task_list_head && walked < MAX_TASKS);

    spin_unlock_irqrestore(&task_lock, flags);
    return 0;
}

static int procfs_net_readdir(file_t* file, dirent_t* dirent)
{
    static const struct {
        const char* name;
        uint32_t ino;
        uint8_t type;
    } entries[] = {
        { ".",   PROC_INO_NET_DIR, DT_DIR },
        { "..",  PROC_INO_ROOT,    DT_DIR },
        { "dev", PROC_INO_NET_DEV, DT_REG },
        { "tcp", PROC_INO_NET_TCP, DT_REG },
    };
    uint32_t offset = file->offset;

    if (offset >= sizeof(entries) / sizeof(entries[0]))
        return 0;

    proc_fill_dirent(dirent, entries[offset].ino, entries[offset].type,
                     entries[offset].name);
    file->offset++;
    return 1;
}

static int procfs_fs_readdir(file_t* file, dirent_t* dirent)
{
    static const struct {
        const char* name;
        uint32_t ino;
        uint8_t type;
    } entries[] = {
        { ".",    PROC_INO_FS_DIR,  DT_DIR },
        { "..",   PROC_INO_ROOT,    DT_DIR },
        { "ext2", PROC_INO_FS_EXT2, DT_DIR },
        { "fat32", PROC_INO_FS_FAT32, DT_DIR },
    };
    uint32_t offset = file->offset;

    if (offset >= sizeof(entries) / sizeof(entries[0]))
        return 0;

    proc_fill_dirent(dirent, entries[offset].ino, entries[offset].type,
                     entries[offset].name);
    file->offset++;
    return 1;
}

static int procfs_ext2_readdir(file_t* file, dirent_t* dirent)
{
    static const struct {
        const char* name;
        uint32_t ino;
        uint8_t type;
    } entries[] = {
        { ".",     PROC_INO_FS_EXT2,       DT_DIR },
        { "..",    PROC_INO_FS_DIR,        DT_DIR },
        { "stats", PROC_INO_FS_EXT2_STATS, DT_REG },
        { "check", PROC_INO_FS_EXT2_CHECK, DT_REG },
    };
    uint32_t offset = file->offset;

    if (offset >= sizeof(entries) / sizeof(entries[0]))
        return 0;

    proc_fill_dirent(dirent, entries[offset].ino, entries[offset].type,
                     entries[offset].name);
    file->offset++;
    return 1;
}

static int procfs_fat32_readdir(file_t* file, dirent_t* dirent)
{
    static const struct {
        const char* name;
        uint32_t ino;
        uint8_t type;
    } entries[] = {
        { ".",     PROC_INO_FS_FAT32,       DT_DIR },
        { "..",    PROC_INO_FS_DIR,          DT_DIR },
        { "stats", PROC_INO_FS_FAT32_STATS, DT_REG },
    };
    uint32_t offset = file->offset;

    if (offset >= sizeof(entries) / sizeof(entries[0]))
        return 0;
    proc_fill_dirent(dirent, entries[offset].ino, entries[offset].type,
                     entries[offset].name);
    file->offset++;
    return 1;
}

static int procfs_pid_readdir(file_t* file, dirent_t* dirent)
{
    static const struct {
        const char* name;
        uint32_t type;
        uint8_t d_type;
    } entries[] = {
        { ".",      PROC_PID_DIR,    DT_DIR },
        { "..",     PROC_INO_ROOT,   DT_DIR },
        { "status", PROC_PID_STATUS, DT_REG },
        { "stat",   PROC_PID_STAT,   DT_REG },
        { "maps",   PROC_PID_MAPS,   DT_REG },
        { "fd",     PROC_PID_FD_DIR, DT_DIR },
        { "cmdline", PROC_PID_CMDLINE, DT_REG },
        { "environ", PROC_PID_ENVIRON, DT_REG },
        { "cwd",    PROC_PID_CWD,    DT_LNK },
        { "exe",    PROC_PID_EXE,    DT_LNK },
        { "root",   PROC_PID_ROOT,   DT_LNK },
    };
    uint32_t offset = file->offset;
    pid_t pid = proc_ino_pid(file->inode->first_cluster);

    if (offset >= sizeof(entries) / sizeof(entries[0]))
        return 0;

    if (!proc_pid_exists(pid))
        return 0;

    proc_fill_dirent(dirent,
                     entries[offset].type == PROC_INO_ROOT
                         ? PROC_INO_ROOT
                         : proc_pid_ino(pid, entries[offset].type),
                     entries[offset].d_type,
                     entries[offset].name);
    file->offset++;
    return 1;
}

static int procfs_fd_readdir(file_t* file, dirent_t* dirent)
{
    uint32_t offset = file->offset;
    pid_t pid = proc_ino_pid(file->inode->first_cluster);
    task_t* task;
    process_t* proc;
    uint32_t seen = 0;
    char fd_name[16];
    unsigned long flags;

    if (offset == 0) {
        proc_fill_dirent(dirent, file->inode->first_cluster, DT_DIR, ".");
        file->offset++;
        return 1;
    }
    if (offset == 1) {
        proc_fill_dirent(dirent, proc_pid_ino(pid, PROC_PID_DIR), DT_DIR, "..");
        file->offset++;
        return 1;
    }

    offset -= 2;

    spin_lock_irqsave(&task_lock, &flags);
    task = proc_find_task_locked(pid);
    proc = task ? proc_task_process(task) : NULL;
    if (!proc) {
        spin_unlock_irqrestore(&task_lock, flags);
        return 0;
    }

    for (int fd = 0; fd < MAX_FILES; fd++) {
        if (!proc->files[fd])
            continue;
        if (seen == offset) {
            snprintf(fd_name, sizeof(fd_name), "%d", fd);
            proc_fill_dirent(dirent, proc_pid_fd_ino(pid, fd), DT_LNK, fd_name);
            file->offset++;
            spin_unlock_irqrestore(&task_lock, flags);
            return 1;
        }
        seen++;
    }

    spin_unlock_irqrestore(&task_lock, flags);
    return 0;
}

static int procfs_readdir(file_t* file, dirent_t* dirent)
{
    uint32_t ino;

    if (!file || !file->inode || !dirent) return -EINVAL;
    if (!S_ISDIR(file->inode->mode)) return -ENOTDIR;

    ino = file->inode->first_cluster;
    if (ino == PROC_INO_ROOT)
        return procfs_root_readdir(file, dirent);
    if (ino == PROC_INO_NET_DIR)
        return procfs_net_readdir(file, dirent);
    if (ino == PROC_INO_FS_DIR)
        return procfs_fs_readdir(file, dirent);
    if (ino == PROC_INO_FS_EXT2)
        return procfs_ext2_readdir(file, dirent);
    if (ino == PROC_INO_FS_FAT32)
        return procfs_fat32_readdir(file, dirent);
    if (ino >= PROC_PID_BASE && proc_ino_type(ino) == PROC_PID_DIR)
        return procfs_pid_readdir(file, dirent);
    if (ino >= PROC_PID_BASE && proc_ino_type(ino) == PROC_PID_FD_DIR)
        return procfs_fd_readdir(file, dirent);

    return 0;
}

static inode_operations_t procfs_inode_ops = {
    .lookup = procfs_lookup,
    .create = procfs_readonly_create,
    .mkdir = procfs_readonly_create,
    .unlink = procfs_readonly_unlink,
    .rmdir = procfs_readonly_unlink,
    .rename = procfs_readonly_rename,
    .readlink = procfs_readlink,
};

static file_operations_t procfs_file_ops = {
    .read = procfs_read,
    .write = procfs_write,
    .open = procfs_open,
    .close = procfs_close,
    .lseek = procfs_lseek,
    .readdir = NULL,
};

static file_operations_t procfs_dir_ops = {
    .read = NULL,
    .write = procfs_write,
    .open = procfs_open,
    .close = procfs_close,
    .lseek = NULL,
    .readdir = procfs_readdir,
};

inode_t* procfs_mount(void)
{
    return proc_make_inode(PROC_INO_ROOT, S_IFDIR | 0555, 0);
}
