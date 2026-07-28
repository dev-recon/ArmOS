/*
 * ArmOS
 * Copyright (c) 2026 Mohamed Ennassiri
 *
 * Licensed under the Apache License, Version 2.0.
 * See LICENSE for details.
 *
 * File: userland/include/arm_os_abi.h
 * Layer: Userland / public header
 * Description: Userspace ABI or library declarations for ArmOS programs.
 */

#ifndef ARM_OS_ABI_H
#define ARM_OS_ABI_H

#include <stddef.h>
#include <stdint.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <time.h>

#ifndef SIGTTIN
#define SIGTTIN 21
#endif
#ifndef SIGTTOU
#define SIGTTOU 22
#endif

#ifndef WSTOPSIG
#define WSTOPSIG(status)   (((status) >> 8) & 0xff)
#endif
#undef WIFSTOPPED
#define WIFSTOPPED(status) ((((status) & 0xff) == 0x7f) && WSTOPSIG(status) != 0)

struct proc_info {
    unsigned tid;
    int      pid;
    int      ppid;
    int      sid;
    int      tty;
    unsigned uid;
    unsigned gid;
    unsigned priority;
    unsigned switches;
    unsigned cpu_pct_x10;
    unsigned stack_kb;
    unsigned heap_kb;
    unsigned vm_kb;
    unsigned rss_kb;
    unsigned l2_tables;
    unsigned page_faults;
    unsigned cow_faults;
    unsigned stack_faults;
    char     name[32];
    char     state;
    char     type;
    char     _pad[2];
};

struct sysinfo_response {
    unsigned         mem_total_kb;
    unsigned         mem_free_kb;
    int              proc_count;
    unsigned         _pad;
    unsigned         tasks_created;
    unsigned         tasks_destroyed;
    unsigned         zombies_created;
    unsigned         zombies_reaped;
    unsigned         failed_forks;
    unsigned         scheduler_refused;
    unsigned         ready_queue_refused;
    unsigned         stack_pages_allocated;
    unsigned         stack_pages_freed;
    unsigned         phys_pages_allocated;
    unsigned         phys_pages_freed;
    unsigned         asid_rollovers;
    unsigned         state_sync_repairs;
    unsigned         blocked_signal_wakeups;
    unsigned         tty_stale_waiters;
    unsigned         fs_wait_timeouts;
    struct proc_info procs[64];
};

struct statfs {
    unsigned f_type;
    unsigned f_bsize;
    unsigned f_blocks;
    unsigned f_bfree;
    unsigned f_bavail;
    unsigned f_files;
    unsigned f_ffree;
    unsigned f_namelen;
    unsigned f_frsize;
};

int getsysinfo(struct sysinfo_response *resp);
int statfs(const char *path, struct statfs *buf);
int getdents(int fd, void *dirp, size_t count);
int tcsetpgrp(int fd, pid_t pgrp);
pid_t tcgetpgrp(int fd);
void *shm_map(int fd, void *addr, int flags);
int shm_unmap(void *addr, size_t size);
int nanosleep(const struct timespec *req, struct timespec *rem);
unsigned int sleep(unsigned int seconds);
int usleep(useconds_t useconds);
pid_t wait4(pid_t pid, int *status, int options, struct rusage *rusage);
int lstat(const char *pathname, struct stat *statbuf);
int symlink(const char *target, const char *linkpath);
ssize_t readlink(const char *pathname, char *buf, size_t bufsiz);
int getpgrp(void);
int setpgid(pid_t pid, pid_t pgid);
void sync(void);
int sys_shutdown(void);
int mount(const char *source, const char *target, const char *filesystemtype,
          unsigned long mountflags, const void *data);
int umount(const char *target);

#endif
