/*
 * ArmOS
 * Copyright (c) 2026 Mohamed Ennassiri
 *
 * Licensed under the Apache License, Version 2.0.
 * See LICENSE for details.
 *
 * File: include/kernel/syscalls.h
 * Layer: Kernel / public internal interface
 *
 * Responsibilities:
 * - Declare kernel types, constants, and subsystem contracts.
 * - Keep cross-module ABI and structure expectations explicit.
 *
 * Notes:
 * - Header changes can ripple across kernel and user ABI glue.
 */

#ifndef _KERNEL_SYSCALLS_H
#define _KERNEL_SYSCALLS_H

#include <kernel/types.h>
#include <kernel/syscall_dispatch.h>
#include <kernel/vfs.h>
#include <kernel/signal.h>  /* Pour sig_handler_t et sigaction_t */
#include <kernel/dirent.h>
#include <uapi/armos/file.h>
#include <uapi/armos/futex.h>
#include <uapi/armos/resource.h>
#include <uapi/armos/statvfs.h>
#include <uapi/armos/syscall.h>
#include <uapi/armos/thread.h>
#include <uapi/armos/time.h>
#include <uapi/armos/tls.h>

/* Forward declarations */
struct process;

/* Stable ArmOS syscall number space, historically based on Linux ARM EABI. */
#define __NR_restart_syscall      0
#define __NR_exit                 ARMOS_NR_EXIT
#define __NR_fork                 2
#define __NR_read                 3
#define __NR_write                ARMOS_NR_WRITE
#define __NR_open                 5
#define __NR_close                6
#define __NR_waitpid              7
#define __NR_creat                8
#define __NR_link                 9
#define __NR_unlink              10
#define __NR_execve              11
#define __NR_chdir               12
#define __NR_time                13
#define __NR_mknod               14
#define __NR_chmod               15
#define __NR_lchown              16
#define __NR_chown               __NR_lchown
#define __NR_break               17
#define __NR_oldstat             18
#define __NR_lseek               19
#define __NR_getpid              20
#define __NR_mount               21
#define __NR_umount              22
#define __NR_setuid              23
#define __NR_getuid              24
#define __NR_stime               25
#define __NR_ptrace              26
#define __NR_alarm               27
#define __NR_oldfstat            28
#define __NR_pause               29
#define __NR_utime               30
#define __NR_stty                31
#define __NR_gtty                32
#define __NR_access              33
#define __NR_nice                34
#define __NR_ftime               35
#define __NR_sync                36
#define __NR_kill                37
#define __NR_rename              38
#define __NR_mkdir               39
#define __NR_rmdir               40
#define __NR_dup                 41
#define __NR_pipe                42
#define __NR_times               43
#define __NR_prof                44
#define __NR_brk                 45
#define __NR_setgid              46
#define __NR_getgid              47
#define __NR_signal              48
#define __NR_geteuid             49
#define __NR_getegid             50
#define __NR_ioctl               54
#define __NR_fcntl               55
#define __NR_setpgid             57
#define __NR_umask               60
#define __NR_dup2                63      /* dup2 */
#define __NR_getpgrp             65
#define __NR_setsid              66
#define __NR_sigaction           67
#define __NR_sigsuspend          72
#define __NR_sigpending          73
#define __NR_setrlimit           ARMOS_NR_SETRLIMIT
#define __NR_getrlimit           ARMOS_NR_GETRLIMIT
#define __NR_gettimeofday        78
#define __NR_getrusage           77
#define __NR_symlink             83
#define __NR_readlink            85
#define __NR_truncate            92
#define __NR_ftruncate           93
#define __NR_fchmod              ARMOS_NR_FCHMOD
#define __NR_fchown              ARMOS_NR_FCHOWN
#define __NR_getpriority         96
#define __NR_setpriority         97
#define __NR_statfs              99
#define __NR_statvfs             ARMOS_NR_STATVFS
#define __NR_fstatvfs            ARMOS_NR_FSTATVFS
#define __NR_utimensat           ARMOS_NR_UTIMENSAT
#define __NR_futimens            ARMOS_NR_FUTIMENS
#define __NR_stat               106
#define __NR_lstat              107
#define __NR_fstat              108
#define __NR_wait4              114
#define __NR_fsync              118
#define __NR_mprotect           125
#define __NR_uname              122
#define __NR_sigprocmask        126
#define __NR_getppid            119  /* Moved to avoid conflicts */
#define __NR_clone              ARMOS_NR_CLONE
#define __NR_print              121
#define __NR_getdents           141
#define __NR_select             142
#define __NR_readv              145
#define __NR_writev             146
#define __NR_getsid             147
#define __NR_fdatasync          ARMOS_NR_FDATASYNC
#define __NR_sched_yield        ARMOS_NR_SCHED_YIELD
#define __NR_nanosleep          162
#define __NR_poll               168
#define __NR_pread              ARMOS_NR_PREAD
#define __NR_pwrite             ARMOS_NR_PWRITE
#define __NR_rt_sigreturn       173
#define __NR_getcwd             183     /* getcwd */
#define __NR_shm_open           190
#define __NR_shm_unlink         191
#define __NR_shm_map            192
#define __NR_shm_unmap          193
#define __NR_shutdown           194
#define __NR_mmap               195
#define __NR_munmap             196
#define __NR_sysconf            ARMOS_NR_SYSCONF
#define __NR_gettid             ARMOS_NR_GETTID
#define __NR_clock_gettime      ARMOS_NR_CLOCK_GETTIME
#define __NR_clock_getres       ARMOS_NR_CLOCK_GETRES
#define __NR_clock_nanosleep    ARMOS_NR_CLOCK_NANOSLEEP
#define __NR_openat             ARMOS_NR_OPENAT
#define __NR_mkdirat            ARMOS_NR_MKDIRAT
#define __NR_fstatat            ARMOS_NR_FSTATAT
#define __NR_unlinkat           ARMOS_NR_UNLINKAT
#define __NR_renameat           ARMOS_NR_RENAMEAT
#define __NR_socket             281
#define __NR_bind               282
#define __NR_connect            283
#define __NR_listen             284
#define __NR_accept             285
#define __NR_sendto             ARMOS_NR_SENDTO
#define __NR_recvfrom           ARMOS_NR_RECVFROM
#define __NR_socket_shutdown    ARMOS_NR_SOCKET_SHUTDOWN
#define __NR_resolve            ARMOS_NR_RESOLVE
#define __NR_thread_exit        ARMOS_NR_THREAD_EXIT
#define __NR_futex              ARMOS_NR_FUTEX
#define __NR_set_tls            ARMOS_NR_SET_TLS
#define __NR_get_tls_info       ARMOS_NR_GET_TLS_INFO
#define __NR_socketpair         ARMOS_NR_SOCKETPAIR
#define __NR_sendmsg            ARMOS_NR_SENDMSG
#define __NR_recvmsg            ARMOS_NR_RECVMSG
#define __NR_eventfd2           ARMOS_NR_EVENTFD2
#define __NR_timerfd_create     ARMOS_NR_TIMERFD_CREATE
#define __NR_timerfd_settime    ARMOS_NR_TIMERFD_SETTIME
#define __NR_timerfd_gettime    ARMOS_NR_TIMERFD_GETTIME
#define __NR_sysinfo            116     /* reused for getprocs — remplacer par /proc plus tard */

#define MAX_SYSCALLS            ARMOS_SYSCALL_MAX

/* Informations sur un processus */
struct proc_info {
    uint32_t tid;
    int      pid;
    int      ppid;
    int      sid;
    int      tty;
    uint32_t uid;
    uint32_t gid;
    uint32_t priority;
    uint32_t switches;
    uint32_t cpu_pct_x10;   /* %CPU * 10  (ex: 875 = 87.5%) */
    uint32_t stack_kb;
    uint32_t heap_kb;
    uint32_t vm_kb;
    uint32_t rss_kb;
    uint32_t l2_tables;
    uint32_t page_faults;
    uint32_t cow_faults;
    uint32_t stack_faults;
    char     name[32];
    char     state;          /* R=run S=sleep Z=zombie T=term D=uninterruptible */
    char     type;           /* P=process K=kernel T=thread */
    char     _pad[2];
};

/* Réponse complète de sys_sysinfo */
struct sysinfo_response {
    uint32_t        mem_total_kb;
    uint32_t        mem_free_kb;
    int             proc_count;
    uint32_t        _pad;
    uint32_t        tasks_created;
    uint32_t        tasks_destroyed;
    uint32_t        zombies_created;
    uint32_t        zombies_reaped;
    uint32_t        failed_forks;
    uint32_t        scheduler_refused;
    uint32_t        ready_queue_refused;
    uint32_t        stack_pages_allocated;
    uint32_t        stack_pages_freed;
    uint32_t        phys_pages_allocated;
    uint32_t        phys_pages_freed;
    uint32_t        asid_rollovers;
    uint32_t        state_sync_repairs;
    uint32_t        blocked_signal_wakeups;
    uint32_t        tty_stale_waiters;
    uint32_t        fs_wait_timeouts;
    struct proc_info procs[64];
};

struct timeval {
    time_t tv_sec;
    uint32_t tv_usec;
};

struct timezone {
    int tz_minuteswest;
    int tz_dsttime;
};

struct iovec_kernel {
    void *iov_base;
    size_t iov_len;
};

struct armos_msghdr_kernel {
    void *msg_name;
    uint32_t msg_namelen;
    struct iovec_kernel *msg_iov;
    size_t msg_iovlen;
    void *msg_control;
    size_t msg_controllen;
    int msg_flags;
};

struct pollfd_kernel {
    int fd;
    int16_t events;
    int16_t revents;
};

struct rusage_kernel {
    struct timeval ru_utime;
    struct timeval ru_stime;
    int32_t ru_maxrss;
    int32_t ru_ixrss;
    int32_t ru_idrss;
    int32_t ru_isrss;
    int32_t ru_minflt;
    int32_t ru_majflt;
    int32_t ru_nswap;
    int32_t ru_inblock;
    int32_t ru_oublock;
    int32_t ru_msgsnd;
    int32_t ru_msgrcv;
    int32_t ru_nsignals;
    int32_t ru_nvcsw;
    int32_t ru_nivcsw;
};

struct utsname_kernel {
    char sysname[65];
    char nodename[65];
    char release[65];
    char version[65];
    char machine[65];
};

/* Syscall handler */
int syscall_handler(uint32_t syscall_num, uint32_t arg1, uint32_t arg2, 
                   uint32_t arg3, uint32_t arg4, uint32_t arg5);
syscall_result_t syscall_dispatch_common_request(
    const syscall_request_t *request);
syscall_result_t syscall_dispatch_common_handler(
    void *owner, const syscall_request_t *request);

/* File syscalls */
int sys_read(int fd, void* buf, size_t count);
int sys_write(int fd, const void* buf, size_t count);
int kernel_write(int fd, const void* kernel_buf, size_t count);
int sys_pread(int fd, void* buf, size_t count,
              const armos_offset_t* offset);
int sys_pwrite(int fd, const void* buf, size_t count,
               const armos_offset_t* offset);
int sys_open(const char* pathname, int flags, mode_t mode);
int sys_openat(int dirfd, const char* pathname, int flags, mode_t mode);
int sys_open_vfs(const char* pathname, int flags, mode_t mode);
int sys_creat(const char* pathname, mode_t mode);
int sys_close(int fd);
int sys_fcntl(int fd, int cmd, uintptr_t arg);
int sys_ioctl(int fd, uint32_t request, uintptr_t arg);
off_t sys_lseek(int fd, off_t offset, int whence);
int sys_stat(const char* pathname, struct stat* statbuf);
int sys_lstat(const char* pathname, struct stat* statbuf);
int sys_stat_vfs(const char* pathname, struct stat* statbuf);
int sys_lstat_vfs(const char* pathname, struct stat* statbuf);
int sys_fstat(int fd, struct stat* statbuf);
int sys_fstatat(int dirfd, const char* pathname, struct stat* statbuf,
                int flags);
int sys_ftruncate(int fd, off_t length);
int sys_truncate(const char* pathname, off_t length);
int sys_fsync(int fd);
int sys_fdatasync(int fd);
int sys_statfs(const char* path, struct statfs* buf);
int sys_statvfs(const char* path, armos_statvfs_t* buf);
int sys_fstatvfs(int fd, armos_statvfs_t* buf);
int sys_print(const char* msg);
int sys_mknod(const char* pathname, mode_t mode, uint32_t dev);
int sys_mount(const char* source, const char* target, const char* fstype,
              uint32_t flags, const void* data);
int sys_umount(const char* target);
int sys_mkdir(const char* pathname, mode_t mode);
int sys_mkdirat(int dirfd, const char* pathname, mode_t mode);
int sys_rmdir(const char* pathname);
int sys_link(const char* oldpath, const char* newpath);
int sys_unlink(const char* pathname);
int sys_unlinkat(int dirfd, const char* pathname, int flags);
int sys_symlink(const char* target, const char* linkpath);
int sys_readlink(const char* pathname, char* buf, size_t bufsiz);
int sys_rename(const char* oldpath, const char* newpath);
int sys_renameat(int olddirfd, const char* oldpath,
                 int newdirfd, const char* newpath);
int sys_getdents(unsigned int fd, struct linux_dirent *dirp, unsigned int count);
int sys_access(const char* pathname, int mode);
int sys_sync(void);
int sys_umask(int mask);
int sys_chmod(const char* pathname, mode_t mode);
int sys_fchmod(int fd, mode_t mode);
int sys_chown(const char* pathname, uid_t owner, gid_t group);
int sys_fchown(int fd, uid_t owner, gid_t group);
int sys_socket(int domain, int type, int protocol);
int sys_socketpair(int domain, int type, int protocol, int* sockets);
ssize_t sys_sendmsg(int sockfd, const struct armos_msghdr_kernel* message,
                    int flags);
ssize_t sys_recvmsg(int sockfd, struct armos_msghdr_kernel* message,
                    int flags);
int sys_bind(int sockfd, const void* addr, uint32_t addrlen);
int sys_connect(int sockfd, const void* addr, uint32_t addrlen);
int sys_listen(int sockfd, int backlog);
int sys_accept(int sockfd, void* addr, uint32_t* addrlen);
ssize_t sys_sendto(int sockfd, const void* buffer, size_t length, int flags,
                   const void* addr, uint32_t addrlen);
ssize_t sys_recvfrom(int sockfd, void* buffer, size_t length, int flags,
                     void* addr, uint32_t* addrlen);
int sys_socket_shutdown(int sockfd, int how);
int sys_resolve(const char* name, uint32_t* address);
int sys_select(int nfds, void* readfds, void* writefds, void* exceptfds, void* timeout);
int sys_poll(struct pollfd_kernel* fds, uint32_t nfds, int timeout_ms);

/* Process syscalls */
#define WNOHANG    1
#define WUNTRACED  2

int sys_fork(void);
int sys_clone(const armos_clone_args_t* args, size_t args_size);
int sys_futex(uint32_t* address, int operation, uint32_t value,
              const armos_timespec_t* timeout);
int sys_set_tls(unsigned long tls_base);
int sys_get_tls_info(armos_tls_info_t* info);
int sys_execve(const char* filename, char* const argv[], char* const envp[]);
void sys_exit(int status);
void sys_thread_exit(int status) __attribute__((noreturn));
int sys_waitpid(pid_t pid, int* status, int options);
int sys_wait4(pid_t pid, int* status, int options, struct rusage_kernel* rusage);
int kernel_waitpid(pid_t pid, int* status, int options, task_t* parent);
int kernel_open(char* kernel_path, int flags, mode_t mode);
int kernel_open_existing(char* kernel_path, int flags);
int sys_stty(int cmd, uint32_t arg, uint32_t arg2);
int sys_gtty(int cmd, uint32_t arg);

/* Process info syscalls */
int sys_getpid(void);
int sys_gettid(void);
int sys_getppid(void);
int sys_setuid(uid_t uid);
int sys_getuid(void);
int sys_geteuid(void);
int sys_setgid(gid_t gid);
int sys_getgid(void);
int sys_getegid(void);
int sys_nice(int inc);
int sys_getpriority(int which, int who);
int sys_setpriority(int which, int who, int prio);
int sys_time(time_t* tloc);
int sys_gettimeofday(struct timeval* tv, struct timezone* tz);
int sys_setpgid(pid_t pid, pid_t pgid);
int sys_getpgrp(void);
int sys_setsid(void);
int sys_getsid(pid_t pid);
int sys_times(void* buf);
int sys_getrusage(int who, struct rusage_kernel* usage);
int sys_getrlimit(int resource, armos_rlimit_t* limit);
int sys_setrlimit(int resource, const armos_rlimit_t* limit);
int sys_alarm(uint32_t seconds);
int sys_pause(void);
int sys_utime(const char* pathname, const void* times);
int sys_utimensat(int dirfd, const char* pathname,
                  const armos_timespec_t* times, int flags);
int sys_futimens(int fd, const armos_timespec_t* times);

/* Signal syscalls */
int sys_kill(pid_t pid, int sig);
int sys_signal(int sig, sig_handler_t handler);
int sys_sigaction(int sig, const sigaction_t* act, sigaction_t* oldact);
int sys_sigprocmask(int how, const sigset_t* set, sigset_t* oldset);
int sys_sigpending(sigset_t* set);
int sys_sigsuspend(const sigset_t* mask);
void sys_sigreturn(void);
int sys_sched_yield(void);
int sys_nanosleep(const armos_timespec32_t *req, armos_timespec32_t *rem);
int sys_clock_gettime(int clock_id, armos_timespec_t *tp);
int sys_clock_getres(int clock_id, armos_timespec_t *res);
int sys_clock_nanosleep(int clock_id, int flags,
                        const armos_timespec_t *req,
                        armos_timespec_t *rem);
int sys_eventfd2(uint32_t initial_value, int flags);
int sys_timerfd_create(int clock_id, int flags);
int sys_timerfd_settime(int fd, int flags,
                        const struct armos_itimerspec *new_value,
                        struct armos_itimerspec *old_value);
int sys_timerfd_gettime(int fd, struct armos_itimerspec *current_value);

/* Memory syscalls */
long sys_brk(void* addr);
int sys_shm_open(const char *name, size_t size, int flags);
int sys_shm_unlink(const char *name);
void *sys_shm_map(int fd, void *addr, int flags);
int sys_shm_unmap(void *addr, size_t size);
int sys_shutdown(void);
void* sys_mmap(void* addr, size_t length, int prot, int flags, int fd,
               uintptr_t offset);
int sys_munmap(void* addr, size_t length);
int sys_mprotect(void* addr, size_t length, int prot);
ssize_t sys_readv(int fd, const struct iovec_kernel* iov, int iovcnt);
ssize_t sys_writev(int fd, const struct iovec_kernel* iov, int iovcnt);

/* Additional process syscalls */
int sys_dup(int oldfd);
int sys_dup2(int oldfd, int newfd);
int sys_pipe(int pipefd[2]);
int sys_chdir(const char* path);
int sys_getcwd(char* buf, size_t size);
int sys_sysinfo(struct sysinfo_response *resp);
int sys_sysconf(int name);
int sys_uname(struct utsname_kernel *name);

#endif
