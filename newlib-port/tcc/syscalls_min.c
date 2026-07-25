/*
 * ArmOS
 * Copyright (c) 2026 Mohamed Ennassiri
 *
 * Licensed under the Apache License, Version 2.0.
 * See LICENSE for details.
 *
 * File: newlib-port/tcc/syscalls_min.c
 * Layer: Userland / TinyCC bring-up
 *
 * Responsibilities:
 * - Provide the smallest newlib syscall surface needed to link a TinyCC-built
 *   ArmOS hello-world binary.
 * - Avoid high-level POSIX wrappers already provided by newlib, so TCC's
 *   stricter linker does not hit duplicate symbols such as signal/_signal_r.
 *
 * Notes:
 * - This file is experimental and intentionally separate from the stable
 *   newlib-port/syscalls.c used by normal ArmOS userland builds.
 */

#include <errno.h>
#include <dirent.h>
#include <fcntl.h>
#include <limits.h>
#include <libgen.h>
#include <netdb.h>
#include <netinet/in.h>
#include <pwd.h>
#include <signal.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <time.h>
#include <uapi/armos/file.h>
#include <uapi/armos/resource.h>
#include <uapi/armos/statvfs.h>
#include <uapi/armos/time.h>
#include <sys/mman.h>
#include <poll.h>
#include <pthread.h>
#include <sys/resource.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/time.h>
#include <sys/times.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/utsname.h>
#include <sys/uio.h>
#include <unistd.h>

#include "arm_os_abi.h"

#ifndef CLOCK_MONOTONIC
#define CLOCK_MONOTONIC ((clockid_t)4)
#endif
#ifndef TIMER_ABSTIME
#define TIMER_ABSTIME 4
#endif

extern long sys_read(int fd, void *buf, unsigned long count);
extern long sys_write(int fd, const void *buf, unsigned long count);
extern long sys_pread(int fd, void *buf, unsigned long count,
                      const armos_offset_t *offset);
extern long sys_pwrite(int fd, const void *buf, unsigned long count,
                       const armos_offset_t *offset);
extern long sys_open(const char *pathname, int flags, int mode);
extern long sys_creat(const char *pathname, int mode);
extern long sys_close(int fd);
extern long sys_mount(const char *source, const char *target,
                      const char *filesystemtype, unsigned long mountflags,
                      const void *data);
extern long sys_umount(const char *target);
extern long sys_link(const char *oldpath, const char *newpath);
extern long sys_unlink(const char *pathname);
extern long sys_execve(const char *pathname, char *const argv[], char *const envp[]);
extern long sys_chdir(const char *path);
extern long sys_time(void *tloc);
extern long sys_chmod(const char *path, int mode);
extern long sys_chown(const char *path, int owner, int group);
extern long sys_lseek(int fd, long offset, int whence);
extern long sys_getuid(void);
extern long sys_getgid(void);
extern long sys_setuid(int uid);
extern long sys_nice(int inc);
extern long sys_sync(void);
extern long sys_rename(const char *oldpath, const char *newpath);
extern long sys_mkdir(const char *pathname, int mode);
extern long sys_rmdir(const char *pathname);
extern long sys_times(void *buf);
extern long sys_setgid(int gid);
extern long sys_umask(int mask);
extern long sys_dup(int oldfd);
extern long sys_dup2(int oldfd, int newfd);
extern long sys_pipe(int pipefd[2]);
extern long sys_setpgid(int pid, int pgid);
extern long sys_getpgrp(void);
extern long sys_getppid(void);
extern long sys_stty(int request, int value, int value2);
extern long sys_gtty(int request, int value);
extern long sys_access(const char *pathname, int mode);
extern long sys_symlink(const char *target, const char *linkpath);
extern long sys_readlink(const char *pathname, char *buf, unsigned long bufsiz);
extern long sys_truncate(const char *pathname, long length);
extern long sys_stat(const char *pathname, void *st);
extern long sys_lstat(const char *pathname, void *st);
extern long sys_fstat(int fd, void *st);
extern long sys_wait4(int pid, int *status, int options, void *rusage);
extern long sys_sysinfo(void *resp);
extern long sys_gettimeofday(struct timeval *tv, void *tz);
extern long sys_getrusage(int who, void *usage);
extern long sys_getrlimit(int resource, armos_rlimit_t *limit);
extern long sys_setrlimit(int resource, const armos_rlimit_t *limit);
extern long sys_getpriority(int which, int who);
extern long sys_setpriority(int which, int who, int prio);
extern long sys_statfs(const char *path, void *buf);
extern long sys_statvfs(const char *path, armos_statvfs_t *buf);
extern long sys_fstatvfs(int fd, armos_statvfs_t *buf);
extern long sys_ftruncate(int fd, long length);
extern long sys_fsync(int fd);
extern long sys_uname(void *name);
extern long sys_getdents(int fd, void *dirp, unsigned long count);
extern long sys_select(int nfds, void *readfds, void *writefds, void *exceptfds, void *timeout);
extern long sys_readv(int fd, const void *iov, int iovcnt);
extern long sys_writev(int fd, const void *iov, int iovcnt);
extern long sys_poll(void *fds, unsigned long nfds, int timeout);
extern long sys_sigaction(int sig, const void *act, void *oldact);
extern long sys_sigprocmask(int how, const void *set, void *oldset);
extern long sys_sigpending(void *set);
extern long sys_nanosleep(const armos_timespec32_t *req,
                          armos_timespec32_t *rem);
extern long sys_clock_gettime(int clock_id, armos_timespec_t *tp);
extern long sys_clock_getres(int clock_id, armos_timespec_t *res);
extern long sys_clock_nanosleep(int clock_id, int flags,
                                const armos_timespec_t *req,
                                armos_timespec_t *rem);
extern long sys_utimensat(int dirfd, const char *pathname,
                          const armos_timespec_t *times, int flags);
extern long sys_futimens(int fd, const armos_timespec_t *times);
extern long sys_getcwd(char *buf, unsigned long size);
extern long sys_shm_open(const char *name, unsigned long size, int flags);
extern long sys_shm_unlink(const char *name);
extern long sys_shm_map(int id, void *addr, int flags);
extern long sys_shm_unmap(void *addr, unsigned long size);
extern long sys_mmap(void *addr, unsigned long length, int prot, int flags, int fd);
extern long sys_munmap(void *addr, unsigned long length);
extern long sys_mprotect(void *addr, unsigned long length, int prot);
extern long sys_socket(int domain, int type, int protocol);
extern long sys_socketpair(int domain, int type, int protocol, int sockets[2]);
extern long sys_bind(int sockfd, const void *addr, unsigned long addrlen);
extern long sys_listen(int sockfd, int backlog);
extern long sys_accept(int sockfd, void *addr, unsigned long *addrlen);
extern long sys_connect(int sockfd, const void *addr, unsigned long addrlen);
extern long sys_sendto(int sockfd, const void *buf, unsigned long length,
                       int flags, const void *addr, unsigned long addrlen);
extern long sys_recvfrom(int sockfd, void *buf, unsigned long length,
                         int flags, void *addr, unsigned long *addrlen);
extern long sys_socket_shutdown(int sockfd, int how);
extern long sys_resolve(const char *name, uint32_t *address);
extern long sys_ioctl(int fd, unsigned long request, void *arg);
extern long sys_fork(void);
extern long sys_waitpid(int pid, int *status, int options);
extern long sys_brk(unsigned long brk);
extern long sys_getpid(void);
extern long sys_kill(int pid, int sig);
extern void sys_exit(int status);

#define OS_SIGBUS   7
#define OS_SIGUSR1  10
#define OS_SIGUSR2  12
#define OS_SIGCHLD  17
#define OS_SIGCONT  18
#define OS_SIGSTOP  19
#define OS_SIGTSTP  20

#define TTY_STTY_SET_FOREGROUND_PGID_FD 2
#define TTY_GTTY_GET_FOREGROUND_PGID_FD 2

struct __dirstream {
    int fd;
    size_t pos;
    size_t len;
    struct dirent current;
    char buffer[4096];
};

struct os_sigaction {
    void (*sa_handler)(int);
    uint32_t sa_mask;
    int sa_flags;
    void (*sa_restorer)(void);
};

extern void __signal_return_trampoline(void);

#ifndef PATH_MAX
#define PATH_MAX 1024
#endif

#define ARMOS_OPEN_MAX 256

static const char *program_name = "program";
static int cancel_state = PTHREAD_CANCEL_ENABLE;
extern char **environ;

struct os_stat {
    uint32_t st_dev;
    uint32_t st_ino;
    uint32_t st_mode;
    uint32_t st_nlink;
    uint32_t st_uid;
    uint32_t st_gid;
    uint32_t st_rdev;
    int32_t  st_size;
    uint32_t st_blksize;
    uint32_t st_blocks;
    uint32_t os_atime;
    uint32_t os_mtime;
    uint32_t os_ctime;
};

static int ret_errno(long ret)
{
    if (ret < 0) {
        errno = (int)-ret;
        return -1;
    }
    return (int)ret;
}

void __armos_runtime_init(void)
{
    /*
     * TinyCC's minimal runtime is single-threaded and uses newlib's initial
     * process reentrancy object.  The full runtime installs per-thread TLS.
     */
}

int pthread_setcancelstate(int state, int *oldstate)
{
    if (state != PTHREAD_CANCEL_ENABLE &&
        state != PTHREAD_CANCEL_DISABLE)
        return EINVAL;
    if (oldstate)
        *oldstate = cancel_state;
    cancel_state = state;
    return 0;
}

void __armos_init_program_name(const char *argv0)
{
    const char *base;

    if (!argv0 || !*argv0)
        return;

    base = strrchr(argv0, '/');
    program_name = (base && base[1]) ? base + 1 : argv0;
}

const char *getprogname(void)
{
    return program_name;
}

int getdtablesize(void)
{
    return ARMOS_OPEN_MAX;
}

char *dirname(char *path)
{
    static char dot[] = ".";
    char *end;
    char *slash;

    if (!path || !*path)
        return dot;

    end = path + strlen(path);
    while (end > path + 1 && end[-1] == '/')
        *--end = '\0';

    slash = strrchr(path, '/');
    if (!slash)
        return dot;

    if (slash == path) {
        slash[1] = '\0';
        return path;
    }

    while (slash > path && slash[-1] == '/')
        slash--;
    *slash = '\0';
    return path;
}

static int passwd_fd = -1;
static char passwd_line[512];
static struct passwd passwd_entry;

static char *passwd_next_field(char **cursor)
{
    char *start;
    char *colon;

    if (!cursor || !*cursor)
        return NULL;

    start = *cursor;
    colon = strchr(start, ':');
    if (colon) {
        *colon = '\0';
        *cursor = colon + 1;
    } else {
        *cursor = NULL;
    }

    return start;
}

static int passwd_read_line(char *buf, size_t buf_size)
{
    size_t pos = 0;
    char ch;
    long ret;
    int saw_data = 0;

    if (passwd_fd < 0 || buf_size == 0)
        return 0;

    for (;;) {
        ret = sys_read(passwd_fd, &ch, 1);
        if (ret < 0) {
            errno = (int)-ret;
            return -1;
        }
        if (ret == 0)
            break;

        saw_data = 1;
        if (ch == '\n')
            break;
        if (pos + 1 < buf_size)
            buf[pos++] = ch;
    }

    if (!saw_data)
        return 0;

    buf[pos] = '\0';
    return 1;
}

static struct passwd *passwd_parse_line(char *line)
{
    char *cursor = line;
    char *name = passwd_next_field(&cursor);
    char *password = passwd_next_field(&cursor);
    char *uid = passwd_next_field(&cursor);
    char *gid = passwd_next_field(&cursor);
    char *gecos = passwd_next_field(&cursor);
    char *home = passwd_next_field(&cursor);
    char *shell = passwd_next_field(&cursor);

    if (!name || !password || !uid || !gid || !gecos || !home || !shell)
        return NULL;

    passwd_entry.pw_name = name;
    passwd_entry.pw_passwd = password;
    passwd_entry.pw_uid = (uid_t)strtoul(uid, NULL, 10);
    passwd_entry.pw_gid = (gid_t)strtoul(gid, NULL, 10);
    passwd_entry.pw_comment = gecos;
    passwd_entry.pw_gecos = gecos;
    passwd_entry.pw_dir = home;
    passwd_entry.pw_shell = shell;
    return &passwd_entry;
}

void setpwent(void)
{
    long ret;

    if (passwd_fd >= 0) {
        sys_lseek(passwd_fd, 0, SEEK_SET);
        return;
    }

    ret = sys_open("/etc/passwd", O_RDONLY, 0);
    if (ret < 0) {
        errno = (int)-ret;
        passwd_fd = -1;
        return;
    }
    passwd_fd = (int)ret;
}

void endpwent(void)
{
    if (passwd_fd >= 0) {
        sys_close(passwd_fd);
        passwd_fd = -1;
    }
}

struct passwd *getpwent(void)
{
    int ret;
    struct passwd *entry;

    if (passwd_fd < 0)
        setpwent();
    if (passwd_fd < 0)
        return NULL;

    while ((ret = passwd_read_line(passwd_line, sizeof(passwd_line))) > 0) {
        entry = passwd_parse_line(passwd_line);
        if (entry)
            return entry;
    }

    return NULL;
}

struct passwd *getpwuid(uid_t uid)
{
    struct passwd *entry;

    setpwent();
    while ((entry = getpwent()) != NULL) {
        if (entry->pw_uid == uid) {
            endpwent();
            return entry;
        }
    }
    endpwent();
    return NULL;
}

struct passwd *getpwnam(const char *name)
{
    struct passwd *entry;

    if (!name)
        return NULL;

    setpwent();
    while ((entry = getpwent()) != NULL) {
        if (strcmp(entry->pw_name, name) == 0) {
            endpwent();
            return entry;
        }
    }
    endpwent();
    return NULL;
}

static void copy_stat(struct stat *dst, const struct os_stat *src)
{
    memset(dst, 0, sizeof(*dst));
    dst->st_dev = src->st_dev;
    dst->st_ino = src->st_ino;
    dst->st_mode = src->st_mode;
    dst->st_nlink = src->st_nlink;
    dst->st_uid = src->st_uid;
    dst->st_gid = src->st_gid;
    dst->st_rdev = src->st_rdev;
    dst->st_size = src->st_size;
    dst->st_blksize = src->st_blksize;
    dst->st_blocks = src->st_blocks;
    dst->st_atime = src->os_atime;
    dst->st_mtime = src->os_mtime;
    dst->st_ctime = src->os_ctime;
}

static int signal_newlib_to_os(int sig)
{
    switch (sig) {
    case SIGBUS:
        return OS_SIGBUS;
    case SIGUSR1:
        return OS_SIGUSR1;
    case SIGUSR2:
        return OS_SIGUSR2;
    case SIGCHLD:
        return OS_SIGCHLD;
    case SIGCONT:
        return OS_SIGCONT;
    case SIGSTOP:
        return OS_SIGSTOP;
#ifdef SIGTSTP
    case SIGTSTP:
        return OS_SIGTSTP;
#endif
    default:
        return sig;
    }
}

static int signal_os_to_newlib(int sig)
{
    switch (sig) {
    case OS_SIGBUS:
        return SIGBUS;
    case OS_SIGUSR1:
        return SIGUSR1;
    case OS_SIGUSR2:
        return SIGUSR2;
    case OS_SIGCHLD:
        return SIGCHLD;
    case OS_SIGCONT:
        return SIGCONT;
    case OS_SIGSTOP:
        return SIGSTOP;
#ifdef SIGTSTP
    case OS_SIGTSTP:
        return SIGTSTP;
#endif
    default:
        return sig;
    }
}

static uint32_t sigset_newlib_to_os(sigset_t set)
{
    uint32_t out = 0;

    for (int sig = 1; sig < 32; sig++) {
        if ((uint32_t)set & (1u << sig)) {
            int os_sig = signal_newlib_to_os(sig);
            if (os_sig > 0 && os_sig < 32)
                out |= (1u << os_sig);
        }
    }

    return out;
}

static sigset_t sigset_os_to_newlib(uint32_t set)
{
    sigset_t out = 0;

    for (int sig = 1; sig < 32; sig++) {
        if (set & (1u << sig)) {
            int nl_sig = signal_os_to_newlib(sig);
            if (nl_sig > 0 && nl_sig < 32)
                out |= (sigset_t)(1u << nl_sig);
        }
    }

    return out;
}

static int wait_status_os_to_newlib(int status)
{
    if ((status & 0xff) == 0x7f) {
        int sig = signal_os_to_newlib((status >> 8) & 0xff);
        return 0x7f | ((sig & 0xff) << 8);
    }

    if ((status & 0x7f) != 0) {
        int sig = signal_os_to_newlib(status & 0x7f);
        return sig & 0x7f;
    }

    return status;
}

static int append_path_component(char *dst, size_t dst_size,
                                 const char *component, size_t len)
{
    size_t cur;

    if (len == 0 || (len == 1 && component[0] == '.'))
        return 0;

    if (len == 2 && component[0] == '.' && component[1] == '.') {
        char *slash;
        cur = strlen(dst);
        if (cur <= 1) {
            dst[0] = '/';
            dst[1] = '\0';
            return 0;
        }
        slash = strrchr(dst, '/');
        if (!slash || slash == dst)
            dst[1] = '\0';
        else
            *slash = '\0';
        return 0;
    }

    cur = strlen(dst);
    if (cur > 1) {
        if (cur + 1 >= dst_size)
            return -1;
        dst[cur++] = '/';
        dst[cur] = '\0';
    }

    if (cur + len >= dst_size)
        return -1;

    memcpy(dst + cur, component, len);
    dst[cur + len] = '\0';
    return 0;
}

static int normalize_absolute_path(const char *path, char *dst, size_t dst_size)
{
    const char *p = path;

    if (!path || path[0] != '/' || dst_size < 2)
        return -1;

    dst[0] = '/';
    dst[1] = '\0';

    while (*p) {
        const char *start;
        size_t len;

        while (*p == '/')
            p++;
        start = p;
        while (*p && *p != '/')
            p++;
        len = (size_t)(p - start);

        if (append_path_component(dst, dst_size, start, len) < 0)
            return -1;
    }

    return 0;
}

int _read(int fd, void *buf, size_t count)
{
    return ret_errno(sys_read(fd, buf, count));
}

int _write(int fd, const void *buf, size_t count)
{
    return ret_errno(sys_write(fd, buf, count));
}

ssize_t pread(int fd, void *buf, size_t count, off_t offset)
{
    armos_offset_t positioned;

    if (offset < 0) {
        errno = EINVAL;
        return -1;
    }
    if ((unsigned long long)offset > ARMOS_FILE_OFFSET_MAX) {
        errno = EOVERFLOW;
        return -1;
    }

    positioned.value = (signed long long)offset;
    return ret_errno(sys_pread(fd, buf, count, &positioned));
}

ssize_t pwrite(int fd, const void *buf, size_t count, off_t offset)
{
    armos_offset_t positioned;

    if (offset < 0) {
        errno = EINVAL;
        return -1;
    }
    if ((unsigned long long)offset > ARMOS_FILE_OFFSET_MAX) {
        errno = EOVERFLOW;
        return -1;
    }

    positioned.value = (signed long long)offset;
    return ret_errno(sys_pwrite(fd, buf, count, &positioned));
}

int _open(const char *pathname, int flags, int mode)
{
    return ret_errno(sys_open(pathname, flags, mode));
}

int creat(const char *pathname, mode_t mode)
{
    return ret_errno(sys_creat(pathname, (int)mode));
}

int _close(int fd)
{
    return ret_errno(sys_close(fd));
}

int mount(const char *source, const char *target, const char *filesystemtype,
          unsigned long mountflags, const void *data)
{
    return ret_errno(sys_mount(source, target, filesystemtype, mountflags, data));
}

int umount(const char *target)
{
    return ret_errno(sys_umount(target));
}

int _unlink(const char *pathname)
{
    return ret_errno(sys_unlink(pathname));
}

int _link(const char *oldpath, const char *newpath)
{
    return ret_errno(sys_link(oldpath, newpath));
}

int _rename(const char *oldpath, const char *newpath)
{
    return ret_errno(sys_rename(oldpath, newpath));
}

int rename(const char *oldpath, const char *newpath)
{
    return _rename(oldpath, newpath);
}

int mkdir(const char *pathname, mode_t mode)
{
    return ret_errno(sys_mkdir(pathname, (int)mode));
}

int rmdir(const char *pathname)
{
    return ret_errno(sys_rmdir(pathname));
}

int symlink(const char *target, const char *linkpath)
{
    return ret_errno(sys_symlink(target, linkpath));
}

ssize_t readlink(const char *pathname, char *buf, size_t bufsiz)
{
    long ret = sys_readlink(pathname, buf, bufsiz);
    if (ret < 0) {
        errno = (int)-ret;
        return -1;
    }
    return (ssize_t)ret;
}

off_t _lseek(int fd, off_t offset, int whence)
{
    long ret = sys_lseek(fd, offset, whence);
    if (ret < 0) {
        errno = (int)-ret;
        return (off_t)-1;
    }
    return (off_t)ret;
}

int _stat(const char *pathname, struct stat *st)
{
    struct os_stat os_st;
    long ret;

    if (!st) {
        errno = EFAULT;
        return -1;
    }

    ret = sys_stat(pathname, &os_st);
    if (ret < 0) {
        errno = (int)-ret;
        return -1;
    }

    copy_stat(st, &os_st);
    return 0;
}

int _lstat(const char *pathname, struct stat *st)
{
    struct os_stat os_st;
    long ret;

    if (!st) {
        errno = EFAULT;
        return -1;
    }

    ret = sys_lstat(pathname, &os_st);
    if (ret < 0) {
        errno = (int)-ret;
        return -1;
    }

    copy_stat(st, &os_st);
    return 0;
}

int _fstat(int fd, struct stat *st)
{
    struct os_stat os_st;
    long ret;

    if (!st) {
        errno = EFAULT;
        return -1;
    }

    ret = sys_fstat(fd, &os_st);
    if (ret < 0) {
        errno = (int)-ret;
        return -1;
    }

    copy_stat(st, &os_st);
    return 0;
}

int stat(const char *pathname, struct stat *st)
{
    return _stat(pathname, st);
}

int lstat(const char *pathname, struct stat *st)
{
    return _lstat(pathname, st);
}

int fstat(int fd, struct stat *st)
{
    return _fstat(fd, st);
}

int ftruncate(int fd, off_t length)
{
    return ret_errno(sys_ftruncate(fd, (long)length));
}

int _ftruncate(int fd, off_t length)
{
    return ftruncate(fd, length);
}

int truncate(const char *pathname, off_t length)
{
    return ret_errno(sys_truncate(pathname, (long)length));
}

int fsync(int fd)
{
    return ret_errno(sys_fsync(fd));
}

int statfs(const char *path, struct statfs *buf)
{
    return ret_errno(sys_statfs(path, buf));
}

static void copy_statvfs(struct statvfs *dst, const armos_statvfs_t *src)
{
    dst->f_bsize = (unsigned long)src->f_bsize;
    dst->f_frsize = (unsigned long)src->f_frsize;
    dst->f_blocks = (fsblkcnt_t)src->f_blocks;
    dst->f_bfree = (fsblkcnt_t)src->f_bfree;
    dst->f_bavail = (fsblkcnt_t)src->f_bavail;
    dst->f_files = (fsfilcnt_t)src->f_files;
    dst->f_ffree = (fsfilcnt_t)src->f_ffree;
    dst->f_favail = (fsfilcnt_t)src->f_favail;
    dst->f_fsid = (unsigned long)src->f_fsid;
    dst->f_flag = (unsigned long)src->f_flag;
    dst->f_namemax = (unsigned long)src->f_namemax;
}

int statvfs(const char *path, struct statvfs *buf)
{
    armos_statvfs_t os_st;
    long ret;

    if (!buf) {
        errno = EFAULT;
        return -1;
    }
    ret = sys_statvfs(path, &os_st);
    if (ret < 0) {
        errno = (int)-ret;
        return -1;
    }
    copy_statvfs(buf, &os_st);
    return 0;
}

static const armos_timespec_t* copy_utimens_times(
    const struct timespec times[2], armos_timespec_t out[2])
{
    int i;

    if (!times)
        return NULL;
    for (i = 0; i < 2; i++) {
        out[i].sec = (signed long long)times[i].tv_sec;
        out[i].nsec = (signed long long)times[i].tv_nsec;
    }
    return out;
}

int utimensat(int dirfd, const char *pathname,
              const struct timespec times[2], int flags)
{
    armos_timespec_t os_times[2];

    return ret_errno(sys_utimensat(dirfd, pathname,
        copy_utimens_times(times, os_times), flags));
}

int futimens(int fd, const struct timespec times[2])
{
    armos_timespec_t os_times[2];

    return ret_errno(sys_futimens(fd, copy_utimens_times(times, os_times)));
}

int fstatvfs(int fd, struct statvfs *buf)
{
    armos_statvfs_t os_st;
    long ret;

    if (!buf) {
        errno = EFAULT;
        return -1;
    }
    ret = sys_fstatvfs(fd, &os_st);
    if (ret < 0) {
        errno = (int)-ret;
        return -1;
    }
    copy_statvfs(buf, &os_st);
    return 0;
}

int uname(struct utsname *name)
{
    if (!name) {
        errno = EFAULT;
        return -1;
    }
    return ret_errno(sys_uname(name));
}

time_t time(time_t *tloc)
{
    long ret = sys_time(NULL);

    if (ret < 0) {
        errno = (int)-ret;
        return (time_t)-1;
    }

    if (tloc)
        *tloc = (time_t)ret;
    return (time_t)ret;
}

int _gettimeofday(struct timeval *tv, void *tz)
{
    return ret_errno(sys_gettimeofday(tv, tz));
}

int access(const char *pathname, int mode)
{
    return ret_errno(sys_access(pathname, mode));
}

int pipe(int pipefd[2])
{
    return ret_errno(sys_pipe(pipefd));
}

int dup(int oldfd)
{
    return ret_errno(sys_dup(oldfd));
}

int dup2(int oldfd, int newfd)
{
    return ret_errno(sys_dup2(oldfd, newfd));
}

int socket(int domain, int type, int protocol)
{
    return ret_errno(sys_socket(domain, type, protocol));
}

int socketpair(int domain, int type, int protocol, int sockets[2])
{
    return ret_errno(sys_socketpair(domain, type, protocol, sockets));
}

int bind(int sockfd, const struct sockaddr *addr, socklen_t addrlen)
{
    return ret_errno(sys_bind(sockfd, addr, addrlen));
}

int listen(int sockfd, int backlog)
{
    return ret_errno(sys_listen(sockfd, backlog));
}

int accept(int sockfd, struct sockaddr *addr, socklen_t *addrlen)
{
    return ret_errno(sys_accept(sockfd, addr, (unsigned long *)addrlen));
}

int connect(int sockfd, const struct sockaddr *addr, socklen_t addrlen)
{
    return ret_errno(sys_connect(sockfd, addr, addrlen));
}

ssize_t send(int sockfd, const void *buf, size_t len, int flags)
{
    return ret_errno(sys_sendto(sockfd, buf, len, flags, NULL, 0));
}

ssize_t recv(int sockfd, void *buf, size_t len, int flags)
{
    return ret_errno(sys_recvfrom(sockfd, buf, len, flags, NULL, NULL));
}

ssize_t sendto(int sockfd, const void *buf, size_t len, int flags,
               const struct sockaddr *addr, socklen_t addrlen)
{
    return ret_errno(sys_sendto(sockfd, buf, len, flags, addr, addrlen));
}

ssize_t recvfrom(int sockfd, void *buf, size_t len, int flags,
                 struct sockaddr *addr, socklen_t *addrlen)
{
    return ret_errno(sys_recvfrom(sockfd, buf, len, flags, addr,
                                  (unsigned long *)addrlen));
}

int shutdown(int sockfd, int how)
{
    return ret_errno(sys_socket_shutdown(sockfd, how));
}

int getaddrinfo(const char *node, const char *service,
                const struct addrinfo *hints, struct addrinfo **result)
{
    struct addrinfo *entry;
    struct sockaddr_in *address;
    uint32_t resolved;
    unsigned long port = 0u;
    char *end = NULL;
    long ret;

    if (!node || !result)
        return EAI_NONAME;
    if (hints && hints->ai_family != AF_UNSPEC &&
        hints->ai_family != AF_INET)
        return EAI_FAMILY;
    if (service && *service != '\0') {
        port = strtoul(service, &end, 10);
        if (!end || *end != '\0' || port > 65535u)
            return EAI_SERVICE;
    }
    ret = sys_resolve(node, &resolved);
    if (ret < 0)
        return ret == -ENOENT ? EAI_NONAME : EAI_AGAIN;
    entry = calloc(1u, sizeof(*entry) + sizeof(*address));
    if (!entry)
        return EAI_MEMORY;
    address = (struct sockaddr_in *)(entry + 1);
    address->sin_family = AF_INET;
    address->sin_port = htons((uint16_t)port);
    address->sin_addr.s_addr = htonl(resolved);
    entry->ai_family = AF_INET;
    entry->ai_socktype = hints ? hints->ai_socktype : 0;
    entry->ai_protocol = hints ? hints->ai_protocol : 0;
    entry->ai_addrlen = sizeof(*address);
    entry->ai_addr = (struct sockaddr *)address;
    *result = entry;
    return 0;
}

void freeaddrinfo(struct addrinfo *result)
{
    while (result) {
        struct addrinfo *next = result->ai_next;

        free(result);
        result = next;
    }
}

const char *gai_strerror(int error)
{
    switch (error) {
    case 0: return "success";
    case EAI_AGAIN: return "temporary name resolution failure";
    case EAI_FAMILY: return "address family not supported";
    case EAI_MEMORY: return "out of memory";
    case EAI_NONAME: return "name or service not known";
    case EAI_SERVICE: return "service not supported";
    default: return "address resolution failure";
    }
}

void sync(void)
{
    (void)sys_sync();
}

int chdir(const char *path)
{
    return ret_errno(sys_chdir(path));
}

char *getcwd(char *buf, size_t size)
{
    long ret;

    if (!buf || size == 0) {
        errno = EINVAL;
        return NULL;
    }

    ret = sys_getcwd(buf, size);
    if (ret < 0) {
        errno = (int)-ret;
        return NULL;
    }
    return buf;
}

char *realpath(const char *path, char *resolved_path)
{
    char input[PATH_MAX];
    char normalized[PATH_MAX];
    char cwd[PATH_MAX];
    char *out = resolved_path;
    struct stat st;

    if (!path || path[0] == '\0') {
        errno = EINVAL;
        return NULL;
    }

    if (path[0] == '/') {
        if (strlen(path) >= sizeof(input)) {
            errno = ENAMETOOLONG;
            return NULL;
        }
        strcpy(input, path);
    } else {
        if (!getcwd(cwd, sizeof(cwd)))
            return NULL;
        if (snprintf(input, sizeof(input), "%s/%s", cwd, path) >= (int)sizeof(input)) {
            errno = ENAMETOOLONG;
            return NULL;
        }
    }

    if (normalize_absolute_path(input, normalized, sizeof(normalized)) < 0) {
        errno = ENAMETOOLONG;
        return NULL;
    }

    /*
     * Match the stable newlib glue: collapse "." and "..", then require the
     * final path to exist. Symlink expansion can be added when a port needs
     * exact Linux realpath semantics.
     */
    if (stat(normalized, &st) < 0)
        return NULL;

    if (!out) {
        out = malloc(PATH_MAX);
        if (!out) {
            errno = ENOMEM;
            return NULL;
        }
    }

    strcpy(out, normalized);
    return out;
}

int chmod(const char *path, mode_t mode)
{
    return ret_errno(sys_chmod(path, (int)mode));
}

int chown(const char *path, uid_t owner, gid_t group)
{
    int os_owner = ((unsigned int)owner == (unsigned int)(uid_t)-1) ? -1 : (int)owner;
    int os_group = ((unsigned int)group == (unsigned int)(gid_t)-1) ? -1 : (int)group;

    return ret_errno(sys_chown(path, os_owner, os_group));
}

mode_t umask(mode_t mask)
{
    long ret = sys_umask((int)mask);
    if (ret < 0) {
        errno = (int)-ret;
        return (mode_t)-1;
    }
    return (mode_t)ret;
}

int _isatty(int fd)
{
    struct stat st;
    if (_fstat(fd, &st) < 0)
        return 0;
    return S_ISCHR(st.st_mode) ? 1 : 0;
}

int ioctl(int fd, unsigned long request, ...)
{
    va_list ap;
    void *arg;

    /*
     * Keep TinyCC's runtime glue small: only the classic pointer-argument
     * ioctl shape is needed by current TCC-built programs such as kilo.
     */
    va_start(ap, request);
    arg = va_arg(ap, void *);
    va_end(ap);

    return ret_errno(sys_ioctl(fd, request, arg));
}

int tcgetattr(int fd, struct termios *termios_p)
{
    return ioctl(fd, TCGETS, termios_p);
}

int tcsetattr(int fd, int optional_actions, const struct termios *termios_p)
{
    unsigned long request = TCSETS;

    if (optional_actions == TCSADRAIN)
        request = TCSETSW;
    else if (optional_actions == TCSAFLUSH)
        request = TCSETSF;

    return ioctl(fd, request, (void *)termios_p);
}

void *_sbrk(ptrdiff_t incr)
{
    static char *heap_end;
    char *prev;
    long current;
    long next;

    if (!heap_end) {
        current = sys_brk(0);
        if (current < 0) {
            errno = (int)-current;
            return (void *)-1;
        }
        heap_end = (char *)current;
    }

    prev = heap_end;
    next = sys_brk((unsigned long)(heap_end + incr));
    if (next < 0) {
        errno = ENOMEM;
        return (void *)-1;
    }

    heap_end = (char *)next;
    return prev;
}

void _exit(int status)
{
    sys_exit(status);
    for (;;)
        ;
}

int _kill(int pid, int sig)
{
    return ret_errno(sys_kill(pid, signal_newlib_to_os(sig)));
}

pid_t _getpid(void)
{
    return (pid_t)sys_getpid();
}

pid_t getppid(void)
{
    return (pid_t)sys_getppid();
}

uid_t getuid(void)
{
    return (uid_t)sys_getuid();
}

gid_t getgid(void)
{
    return (gid_t)sys_getgid();
}

int setuid(uid_t uid)
{
    return ret_errno(sys_setuid((int)uid));
}

int setgid(gid_t gid)
{
    return ret_errno(sys_setgid((int)gid));
}

int getpriority(int which, id_t who)
{
    long ret = sys_getpriority(which, (int)who);
    if (ret < 0) {
        errno = (int)-ret;
        return -1;
    }
    return 20 - (int)ret;
}

int setpriority(int which, id_t who, int prio)
{
    return ret_errno(sys_setpriority(which, (int)who, prio));
}

int nice(int inc)
{
    long ret = sys_nice(inc);
    if (ret < 0) {
        errno = (int)-ret;
        return -1;
    }
    return getpriority(PRIO_PROCESS, 0);
}

int getpgrp(void)
{
    long ret = sys_getpgrp();
    if (ret < 0) {
        errno = (int)-ret;
        return -1;
    }
    return (int)ret;
}

int setpgid(pid_t pid, pid_t pgid)
{
    return ret_errno(sys_setpgid(pid, pgid));
}

int tcsetpgrp(int fd, pid_t pgrp)
{
    return ret_errno(sys_stty(TTY_STTY_SET_FOREGROUND_PGID_FD, fd, pgrp));
}

pid_t tcgetpgrp(int fd)
{
    long ret = sys_gtty(TTY_GTTY_GET_FOREGROUND_PGID_FD, fd);
    if (ret < 0) {
        errno = (int)-ret;
        return (pid_t)-1;
    }
    return (pid_t)ret;
}

int _fork(void)
{
    return ret_errno(sys_fork());
}

int fork(void)
{
    return _fork();
}

int _execve(const char *pathname, char *const argv[], char *const envp[])
{
    return ret_errno(sys_execve(pathname, argv, envp));
}

int execve(const char *pathname, char *const argv[], char *const envp[])
{
    return _execve(pathname, argv, envp);
}

int execv(const char *pathname, char *const argv[])
{
    return _execve(pathname, argv, environ);
}

int execvp(const char *file, char *const argv[])
{
    const char *path;
    const char *entry;
    int saw_eacces = 0;

    if (!file || !*file) {
        errno = ENOENT;
        return -1;
    }

    if (strchr(file, '/'))
        return execv(file, argv);

    path = getenv("PATH");
    if (!path || !*path)
        path = "/bin:/usr/bin:/sbin:/opt/kilo/bin";

    entry = path;
    while (1) {
        const char *colon = strchr(entry, ':');
        size_t dir_len = colon ? (size_t)(colon - entry) : strlen(entry);
        size_t file_len = strlen(file);
        char candidate[PATH_MAX];

        if (dir_len == 0) {
            if (file_len + 1 > sizeof(candidate)) {
                errno = ENAMETOOLONG;
                return -1;
            }
            memcpy(candidate, file, file_len + 1);
        } else {
            if (dir_len + 1 + file_len + 1 > sizeof(candidate)) {
                errno = ENAMETOOLONG;
                return -1;
            }
            memcpy(candidate, entry, dir_len);
            candidate[dir_len] = '/';
            memcpy(candidate + dir_len + 1, file, file_len + 1);
        }

        execv(candidate, argv);
        if (errno == EACCES)
            saw_eacces = 1;
        else if (errno != ENOENT && errno != ENOTDIR)
            return -1;

        if (!colon)
            break;
        entry = colon + 1;
    }

    errno = saw_eacces ? EACCES : ENOENT;
    return -1;
}

int _wait(int *status)
{
    long ret = sys_waitpid(-1, status, 0);
    if (ret < 0) {
        errno = (int)-ret;
        return -1;
    }
    if (status)
        *status = wait_status_os_to_newlib(*status);
    return (int)ret;
}

pid_t waitpid(pid_t pid, int *status, int options)
{
    long ret = sys_waitpid(pid, status, options);
    if (ret < 0) {
        errno = (int)-ret;
        return (pid_t)-1;
    }
    if (status)
        *status = wait_status_os_to_newlib(*status);
    return (pid_t)ret;
}

pid_t wait4(pid_t pid, int *status, int options, struct rusage *rusage)
{
    long ret = sys_wait4(pid, status, options, rusage);
    if (ret < 0) {
        errno = (int)-ret;
        return (pid_t)-1;
    }
    if (status)
        *status = wait_status_os_to_newlib(*status);
    return (pid_t)ret;
}

int getdents(int fd, void *dirp, size_t count)
{
    return ret_errno(sys_getdents(fd, dirp, count));
}

DIR *opendir(const char *name)
{
    DIR *dir;
    int fd;

    if (!name) {
        errno = EINVAL;
        return NULL;
    }

    fd = open(name, O_RDONLY | O_DIRECTORY);
    if (fd < 0)
        return NULL;

    dir = calloc(1, sizeof(*dir));
    if (!dir) {
        close(fd);
        errno = ENOMEM;
        return NULL;
    }

    dir->fd = fd;
    return dir;
}

struct dirent *readdir(DIR *dirp)
{
    struct linux_dirent *raw;
    size_t name_len;

    if (!dirp) {
        errno = EBADF;
        return NULL;
    }

    for (;;) {
        if (dirp->pos >= dirp->len) {
            int n = getdents(dirp->fd, dirp->buffer, sizeof(dirp->buffer));
            if (n <= 0)
                return NULL;
            dirp->pos = 0;
            dirp->len = (size_t)n;
        }

        raw = (struct linux_dirent *)(dirp->buffer + dirp->pos);
        if (raw->d_reclen == 0 || dirp->pos + raw->d_reclen > dirp->len) {
            errno = EIO;
            return NULL;
        }
        dirp->pos += raw->d_reclen;

        dirp->current.d_ino = raw->d_ino;
        dirp->current.d_off = raw->d_off;
        dirp->current.d_reclen = raw->d_reclen;
        dirp->current.d_type = raw->d_type;
        name_len = strnlen(raw->d_name, NAME_MAX);
        memcpy(dirp->current.d_name, raw->d_name, name_len);
        dirp->current.d_name[name_len] = '\0';
        return &dirp->current;
    }
}

void rewinddir(DIR *dirp)
{
    if (!dirp)
        return;
    lseek(dirp->fd, 0, SEEK_SET);
    dirp->pos = 0;
    dirp->len = 0;
}

int closedir(DIR *dirp)
{
    int ret;

    if (!dirp) {
        errno = EBADF;
        return -1;
    }

    ret = close(dirp->fd);
    free(dirp);
    return ret;
}

int dirfd(DIR *dirp)
{
    if (!dirp) {
        errno = EBADF;
        return -1;
    }
    return dirp->fd;
}

int select(int nfds, fd_set *readfds, fd_set *writefds,
           fd_set *exceptfds, struct timeval *timeout)
{
    return ret_errno(sys_select(nfds, readfds, writefds, exceptfds, timeout));
}

int poll(struct pollfd *fds, nfds_t nfds, int timeout)
{
    return ret_errno(sys_poll(fds, nfds, timeout));
}

int pselect(int nfds, fd_set *readfds, fd_set *writefds,
            fd_set *exceptfds, const struct timespec *timeout,
            const sigset_t *sigmask)
{
    struct timeval tv;
    sigset_t oldmask;
    sigset_t tmpmask;
    int ret;

    /*
     * ArmOS currently emulates pselect in userland glue. This keeps TCC-built
     * software link-compatible, but it is not a Linux-style atomic
     * signal-mask-and-wait transition.
     */
    if (sigmask && sigprocmask(SIG_SETMASK, sigmask, &oldmask) < 0)
        return -1;

    if (timeout) {
        if (timeout->tv_nsec < 0 || timeout->tv_nsec >= 1000000000L) {
            if (sigmask)
                sigprocmask(SIG_SETMASK, &oldmask, NULL);
            errno = EINVAL;
            return -1;
        }
        tv.tv_sec = timeout->tv_sec;
        tv.tv_usec = timeout->tv_nsec / 1000L;
    }

    ret = select(nfds, readfds, writefds, exceptfds, timeout ? &tv : NULL);
    if (sigmask) {
        tmpmask = oldmask;
        sigprocmask(SIG_SETMASK, &tmpmask, NULL);
    }
    return ret;
}

int ppoll(struct pollfd *fds, nfds_t nfds, const struct timespec *timeout,
          const sigset_t *sigmask)
{
    sigset_t oldmask;
    sigset_t tmpmask;
    int timeout_ms = -1;
    int ret;

    if (timeout) {
        if (timeout->tv_sec < 0 || timeout->tv_nsec < 0 ||
            timeout->tv_nsec >= 1000000000L) {
            errno = EINVAL;
            return -1;
        }
        timeout_ms = (int)(timeout->tv_sec * 1000L +
                           (timeout->tv_nsec + 999999L) / 1000000L);
    }

    if (sigmask && sigprocmask(SIG_SETMASK, sigmask, &oldmask) < 0)
        return -1;
    ret = poll(fds, nfds, timeout_ms);
    if (sigmask) {
        tmpmask = oldmask;
        sigprocmask(SIG_SETMASK, &tmpmask, NULL);
    }
    return ret;
}

ssize_t readv(int fd, const struct iovec *iov, int iovcnt)
{
    long ret = sys_readv(fd, iov, iovcnt);
    if (ret < 0) {
        errno = (int)-ret;
        return -1;
    }
    return (ssize_t)ret;
}

ssize_t writev(int fd, const struct iovec *iov, int iovcnt)
{
    long ret = sys_writev(fd, iov, iovcnt);
    if (ret < 0) {
        errno = (int)-ret;
        return -1;
    }
    return (ssize_t)ret;
}

int getsysinfo(struct sysinfo_response *resp)
{
    return ret_errno(sys_sysinfo(resp));
}

static int clock_id_to_armos(clockid_t clock_id)
{
    if (clock_id == CLOCK_REALTIME)
        return ARMOS_CLOCK_REALTIME;
    if (clock_id == CLOCK_MONOTONIC)
        return ARMOS_CLOCK_MONOTONIC;
    return -1;
}

int clock_gettime(clockid_t clock_id, struct timespec *tp)
{
    armos_timespec_t value;
    int armos_clock = clock_id_to_armos(clock_id);
    long ret;

    if (!tp) {
        errno = EFAULT;
        return -1;
    }
    if (armos_clock < 0) {
        errno = EINVAL;
        return -1;
    }

    ret = sys_clock_gettime(armos_clock, &value);
    if (ret < 0)
        return ret_errno(ret);
    tp->tv_sec = (time_t)value.sec;
    tp->tv_nsec = (long)value.nsec;
    return 0;
}

int clock_getres(clockid_t clock_id, struct timespec *res)
{
    armos_timespec_t value;
    int armos_clock = clock_id_to_armos(clock_id);
    long ret;

    if (armos_clock < 0) {
        errno = EINVAL;
        return -1;
    }

    ret = sys_clock_getres(armos_clock, res ? &value : NULL);
    if (ret < 0)
        return ret_errno(ret);
    if (res) {
        res->tv_sec = (time_t)value.sec;
        res->tv_nsec = (long)value.nsec;
    }
    return 0;
}

int clock_nanosleep(clockid_t clock_id, int flags,
                    const struct timespec *req, struct timespec *rem)
{
    armos_timespec_t request;
    armos_timespec_t remaining;
    int armos_clock = clock_id_to_armos(clock_id);
    int armos_flags;
    long ret;

    if (armos_clock < 0)
        return EINVAL;
    if (flags != 0 && flags != TIMER_ABSTIME)
        return EINVAL;
    if (!req)
        return EFAULT;
    if (req->tv_sec < 0 || req->tv_nsec < 0 ||
        req->tv_nsec >= 1000000000L)
        return EINVAL;

    request.sec = (signed long long)req->tv_sec;
    request.nsec = (signed long long)req->tv_nsec;
    armos_flags = flags == TIMER_ABSTIME ? ARMOS_TIMER_ABSTIME : 0;
    ret = sys_clock_nanosleep(armos_clock, armos_flags, &request,
                              rem ? &remaining : NULL);
    if (ret < 0) {
        if (ret == -EINTR && rem && armos_flags == 0) {
            rem->tv_sec = (time_t)remaining.sec;
            rem->tv_nsec = (long)remaining.nsec;
        }
        return (int)-ret;
    }
    return 0;
}

int nanosleep(const struct timespec *req, struct timespec *rem)
{
    armos_timespec32_t request;
    armos_timespec32_t remaining;
    long ret;

    if (!req) {
        errno = EFAULT;
        return -1;
    }
    if (req->tv_sec < 0 || req->tv_nsec < 0 ||
        req->tv_nsec >= 1000000000L ||
        (unsigned long long)req->tv_sec > 0xffffffffULL) {
        errno = EINVAL;
        return -1;
    }

    request.sec = (unsigned int)req->tv_sec;
    request.nsec = (unsigned int)req->tv_nsec;
    ret = sys_nanosleep(&request, rem ? &remaining : NULL);
    if (ret < 0) {
        if (ret == -EINTR && rem) {
            rem->tv_sec = (time_t)remaining.sec;
            rem->tv_nsec = (long)remaining.nsec;
        }
        return ret_errno(ret);
    }
    return 0;
}

int tcflush(int fd, int queue_selector)
{
    return ret_errno(sys_ioctl(fd, TCFLSH,
                               (void *)(intptr_t)queue_selector));
}

int tcdrain(int fd)
{
    struct termios tio;

    if (tcgetattr(fd, &tio) < 0)
        return -1;

    return tcsetattr(fd, TCSADRAIN, &tio);
}

int usleep(useconds_t usec)
{
    struct timespec req;

    req.tv_sec = usec / 1000000U;
    req.tv_nsec = (long)(usec % 1000000U) * 1000L;
    return nanosleep(&req, NULL);
}

unsigned int sleep(unsigned int seconds)
{
    struct timespec req;
    struct timespec rem;

    req.tv_sec = (time_t)seconds;
    req.tv_nsec = 0;

    while (nanosleep(&req, &rem) < 0) {
        if (errno != EINTR)
            return (unsigned int)req.tv_sec;
        if (rem.tv_sec == 0 && rem.tv_nsec == 0)
            return 0;
        req = rem;
    }

    return 0;
}

int sigaction(int sig, const struct sigaction *act, struct sigaction *oldact)
{
    struct os_sigaction os_old;
    long ret;

    ret = sys_sigaction(signal_newlib_to_os(sig),
                        act ? (void *)&(struct os_sigaction) {
                            .sa_handler = act->sa_handler,
                            .sa_mask = sigset_newlib_to_os(act->sa_mask),
                            .sa_flags = act->sa_flags,
                            .sa_restorer = __signal_return_trampoline,
                        } : NULL,
                        oldact ? &os_old : NULL);
    if (ret < 0) {
        errno = (int)-ret;
        return -1;
    }

    if (oldact) {
        oldact->sa_handler = os_old.sa_handler;
        oldact->sa_mask = sigset_os_to_newlib(os_old.sa_mask);
        oldact->sa_flags = os_old.sa_flags;
    }

    return 0;
}

int sigprocmask(int how, const sigset_t *set, sigset_t *oldset)
{
    uint32_t os_set;
    uint32_t os_old;
    long ret;

    os_set = set ? sigset_newlib_to_os(*set) : 0;
    ret = sys_sigprocmask(how, set ? &os_set : NULL, oldset ? &os_old : NULL);
    if (ret < 0) {
        errno = (int)-ret;
        return -1;
    }
    if (oldset)
        *oldset = sigset_os_to_newlib(os_old);
    return 0;
}

int sigpending(sigset_t *set)
{
    uint32_t os_set;
    long ret;

    if (!set) {
        errno = EFAULT;
        return -1;
    }

    ret = sys_sigpending(&os_set);
    if (ret < 0) {
        errno = (int)-ret;
        return -1;
    }
    *set = sigset_os_to_newlib(os_set);
    return 0;
}

int getrusage(int who, struct rusage *usage)
{
    return ret_errno(sys_getrusage(who, usage));
}

int getrlimit(int resource, struct rlimit *rlim)
{
    armos_rlimit_t os_limit;
    long ret;

    if (!rlim) {
        errno = EFAULT;
        return -1;
    }

    ret = sys_getrlimit(resource, &os_limit);
    if (ret < 0) {
        errno = (int)-ret;
        return -1;
    }
    rlim->rlim_cur = os_limit.current == ARMOS_RLIM_INFINITY ?
        RLIM_INFINITY : (rlim_t)os_limit.current;
    rlim->rlim_max = os_limit.maximum == ARMOS_RLIM_INFINITY ?
        RLIM_INFINITY : (rlim_t)os_limit.maximum;
    return 0;
}

int setrlimit(int resource, const struct rlimit *rlim)
{
    armos_rlimit_t os_limit;

    if (!rlim) {
        errno = EFAULT;
        return -1;
    }

    os_limit.current = rlim->rlim_cur == RLIM_INFINITY ?
        ARMOS_RLIM_INFINITY : (unsigned long long)rlim->rlim_cur;
    os_limit.maximum = rlim->rlim_max == RLIM_INFINITY ?
        ARMOS_RLIM_INFINITY : (unsigned long long)rlim->rlim_max;
    return ret_errno(sys_setrlimit(resource, &os_limit));
}

void *mmap(void *addr, size_t length, int prot, int flags, int fd, off_t offset)
{
    long ret;

    if (offset != 0) {
        errno = ENOSYS;
        return MAP_FAILED;
    }

    ret = sys_mmap(addr, length, prot, flags, fd);
    if (ret < 0) {
        errno = (int)-ret;
        return MAP_FAILED;
    }
    return (void *)ret;
}

int munmap(void *addr, size_t length)
{
    return ret_errno(sys_munmap(addr, length));
}

int mprotect(void *addr, size_t length, int prot)
{
    return ret_errno(sys_mprotect(addr, length, prot));
}

int shm_open(const char *name, size_t size, int flags)
{
    return ret_errno(sys_shm_open(name, size, flags));
}

int shm_unlink(const char *name)
{
    return ret_errno(sys_shm_unlink(name));
}

void *shm_map(int id, void *addr, int flags)
{
    long ret = sys_shm_map(id, addr, flags);
    if (ret < 0) {
        errno = (int)-ret;
        return NULL;
    }
    return (void *)ret;
}

int shm_unmap(void *addr, size_t size)
{
    return ret_errno(sys_shm_unmap(addr, size));
}

clock_t _times(struct tms *buf)
{
    long ret = sys_times(buf);
    if (ret < 0) {
        errno = (int)-ret;
        return (clock_t)-1;
    }
    return (clock_t)ret;
}
