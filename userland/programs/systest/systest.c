/*
 * ArmOS
 * Copyright (c) 2026 Mohamed Ennassiri
 *
 * Licensed under the Apache License, Version 2.0.
 * See LICENSE for details.
 *
 * File: userland/programs/systest/systest.c
 * Layer: Userland / test or sample program
 * Description: Userland test, diagnostic, or sample application.
 */

#include <errno.h>
#include <dirent.h>
#include <fcntl.h>
#include <signal.h>
#include <poll.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/eventfd.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/timerfd.h>
#include <sys/ioctl.h>
#include <sys/time.h>
#include <sys/uio.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <time.h>
#include <termios.h>
#include <unistd.h>
#include <arm_os_abi.h>
#include <armos/limits.h>
#include <armos/spawn.h>

extern int sched_yield(void);
extern ssize_t pread(int fd, void *buf, size_t count, off_t offset);
extern ssize_t pwrite(int fd, const void *buf, size_t count, off_t offset);

#ifndef AT_FDCWD
#define AT_FDCWD (-2)
#endif
#ifndef AT_SYMLINK_NOFOLLOW
#define AT_SYMLINK_NOFOLLOW 0x0002
#endif
#ifndef AT_REMOVEDIR
#define AT_REMOVEDIR 0x0008
#endif

extern char *realpath(const char *path, char *resolved_path);

#define COLOR_GREEN "\033[32m"
#define COLOR_RED   "\033[31m"
#define COLOR_RESET "\033[0m"

#ifndef SIGTTIN
#define SIGTTIN 21
#endif

static int failures = 0;
static int verbose = 1;
static volatile int cow_shared_value = 11;
static volatile int sleep_signal_seen = 0;
static volatile int winch_signal_seen = 0;
static volatile int compat_signal_seen = 0;
static volatile sig_atomic_t pipe_signal_seen = 0;
static volatile sig_atomic_t compat_sigchld_value = 0;
static struct sysinfo_response sysinfo_scratch;
static char systest_tmp_root[64];

static const char *tmp_path(const char *name)
{
    static char paths[32][128];
    static int slot = 0;
    char *out = paths[slot++ & 31];

    snprintf(out, 128, "%s/%s", systest_tmp_root, name);
    return out;
}

static void pass(const char *name)
{
    if (!verbose)
        return;
    printf("pid %d " COLOR_GREEN "[OK]" COLOR_RESET " %s\n", getpid(), name);
}

static void fail(const char *name, int value)
{
    printf("pid %d " COLOR_RED "[KO]" COLOR_RESET " %s (%d, errno=%d)\n",
           getpid(), name, value, errno);
    failures++;
}

static void skip(const char *name)
{
    if (!verbose)
        return;
    printf("pid %d [SKIP] %s\n", getpid(), name);
}

static int stdin_is_foreground_tty(void)
{
    int fg_pgrp = tcgetpgrp(STDIN_FILENO);

    if (fg_pgrp < 0)
        return 0;

    return fg_pgrp == getpgrp();
}

static int expect(int cond, const char *name, int value)
{
    if (cond) {
        pass(name);
        return 0;
    }
    fail(name, value);
    return -1;
}

static int status_exited(int status, int code)
{
    return WIFEXITED(status) && WEXITSTATUS(status) == code;
}

static int status_signaled(int status, int sig)
{
    return WIFSIGNALED(status) && WTERMSIG(status) == sig;
}

static void systest_winch_signal_handler(int sig)
{
    (void)sig;
    winch_signal_seen++;
}

static void systest_compat_signal_handler(int sig)
{
    (void)sig;
    compat_signal_seen++;
}

static void systest_pipe_signal_handler(int sig)
{
    if (sig == SIGPIPE)
        pipe_signal_seen = 1;
}

static void systest_sigchld_handler(int sig)
{
    compat_sigchld_value = sig;
}

static int read_file(const char *path, char *buf, int size)
{
    int fd;
    int n;

    fd = open(path, O_RDONLY, 0);
    if (fd < 0)
        return -1;

    n = read(fd, buf, size - 1);
    close(fd);

    if (n < 0)
        return -1;

    buf[n] = '\0';
    return n;
}

static int run_command_common(char *const argv[], int quiet)
{
    char *const envp[] = { NULL };
    int status = -1;
    int pid = fork();

    if (pid == 0) {
        if (quiet) {
            int fd = open("/dev/null", O_WRONLY, 0);
            if (fd >= 0) {
                dup2(fd, STDOUT_FILENO);
                dup2(fd, STDERR_FILENO);
                close(fd);
            }
        }
        execve(argv[0], argv, envp);
        exit(127);
    }

    if (pid < 0)
        return -1;

    if (waitpid(pid, &status, 0) != pid)
        return -1;

    return status;
}

static int run_command(char *const argv[])
{
    return run_command_common(argv, 0);
}

static int run_command_quiet(char *const argv[])
{
    return run_command_common(argv, 1);
}

static int run_rm1(const char *arg)
{
    char *argv[] = { "/bin/rm", (char *)arg, NULL };
    return run_command_quiet(argv);
}

static int run_rm2(const char *opt, const char *arg)
{
    char *argv[] = { "/bin/rm", (char *)opt, (char *)arg, NULL };
    return run_command(argv);
}

static int run_ln2(const char *target, const char *link_name)
{
    char *argv[] = { "/bin/ln", (char *)target, (char *)link_name, NULL };
    return run_command(argv);
}

static int run_ln3(const char *opt, const char *target, const char *link_name)
{
    char *argv[] = { "/bin/ln", (char *)opt, (char *)target, (char *)link_name, NULL };
    return run_command(argv);
}

static int remove_tree_local(const char *path)
{
    struct stat st;

    if (lstat(path, &st) < 0)
        return errno == ENOENT ? 0 : -1;

    if (S_ISDIR(st.st_mode)) {
        char dents[512];
        int fd;
        int n;

        fd = open(path, O_RDONLY | O_DIRECTORY, 0);
        if (fd < 0)
            return -1;

        while ((n = getdents(fd, dents, sizeof(dents))) > 0) {
            int pos = 0;

            while (pos < n) {
                struct linux_dirent *de = (struct linux_dirent *)(dents + pos);
                char child[192];

                if (de->d_reclen == 0)
                    break;

                if (strcmp(de->d_name, ".") != 0 &&
                    strcmp(de->d_name, "..") != 0) {
                    snprintf(child, sizeof(child), "%s/%s", path, de->d_name);
                    if (remove_tree_local(child) < 0) {
                        close(fd);
                        return -1;
                    }
                }

                pos += de->d_reclen;
            }
        }

        close(fd);
        if (n < 0)
            return -1;
        return rmdir(path);
    }

    return unlink(path);
}

static void test_file_io(void)
{
    const char *path = tmp_path("systest.txt");
    char buf[64];
    int fd;
    int n;

    unlink(path);

    fd = open(path, O_CREAT | O_WRONLY | O_TRUNC, 0644);
    if (expect(fd >= 0, "open create/trunc", fd) < 0)
        return;

    expect(write(fd, "first\n", 6) == 6, "write binary-safe buffer", 0);
    close(fd);

    n = read_file(path, buf, sizeof(buf));
    expect(n == 6 && strcmp(buf, "first\n") == 0, "read back created file", n);

    fd = open(path, O_WRONLY | O_TRUNC, 0);
    if (expect(fd >= 0, "open existing with O_TRUNC", fd) < 0)
        return;

    expect(write(fd, "second\n", 7) == 7, "write after trunc", 0);
    close(fd);

    n = read_file(path, buf, sizeof(buf));
    expect(n == 7 && strcmp(buf, "second\n") == 0, "O_TRUNC removed old contents", n);

    fd = open(path, O_WRONLY | O_APPEND, 0);
    if (expect(fd >= 0, "open existing with O_APPEND", fd) < 0)
        return;

    expect(write(fd, "third\n", 6) == 6, "write append", 0);
    close(fd);

    n = read_file(path, buf, sizeof(buf));
    expect(n == 13 && strcmp(buf, "second\nthird\n") == 0, "O_APPEND extends file", n);

    fd = open(path, O_RDWR, 0);
    if (expect(fd >= 0, "open existing for ftruncate", fd) < 0)
        return;

    expect(write(fd, "tiny", 4) == 4, "write shorter replacement", 0);
    expect(ftruncate(fd, 4) == 0, "ftruncate shrinks file", 0);
    close(fd);

    n = read_file(path, buf, sizeof(buf));
    expect(n == 4 && strcmp(buf, "tiny") == 0, "ftruncate removed old tail", n);
}

static void test_positioned_io(void)
{
    const char *path = tmp_path("positioned-io.txt");
    char buf[32];
    int pipefd[2];
    int fd;
    int n;

    unlink(path);
    fd = open(path, O_CREAT | O_RDWR | O_TRUNC, 0644);
    if (expect(fd >= 0, "positioned I/O create file", fd) < 0)
        return;

    expect(write(fd, "abcdefghij", 10) == 10,
           "positioned I/O seed file", errno);
    expect(lseek(fd, 2, SEEK_SET) == 2,
           "positioned I/O set shared offset", errno);

    memset(buf, 0, sizeof(buf));
    expect(pread(fd, buf, 3, 5) == 3 && memcmp(buf, "fgh", 3) == 0,
           "pread reads explicit offset", errno);
    expect(lseek(fd, 0, SEEK_CUR) == 2,
           "pread preserves shared offset", errno);

    expect(pwrite(fd, "XYZ", 3, 4) == 3,
           "pwrite writes explicit offset", errno);
    expect(lseek(fd, 0, SEEK_CUR) == 2,
           "pwrite preserves shared offset", errno);

    memset(buf, 0, sizeof(buf));
    expect(read(fd, buf, 2) == 2 && memcmp(buf, "cd", 2) == 0,
           "ordinary read resumes at shared offset", errno);
    close(fd);

    n = read_file(path, buf, sizeof(buf));
    expect(n == 10 && memcmp(buf, "abcdXYZhij", 10) == 0,
           "pwrite changed only requested bytes", n);

    fd = open(path, O_WRONLY | O_APPEND, 0);
    if (expect(fd >= 0, "positioned I/O open append handle", fd) >= 0) {
        expect(pwrite(fd, "Q", 1, 1) == 1,
               "pwrite ignores append position", errno);
        expect(lseek(fd, 0, SEEK_CUR) == 10,
               "pwrite preserves append handle offset", errno);
        close(fd);
    }

    n = read_file(path, buf, sizeof(buf));
    expect(n == 10 && memcmp(buf, "aQcdXYZhij", 10) == 0,
           "pwrite on append handle uses explicit offset", n);

    errno = 0;
    expect(pread(-1, buf, 1, 0) < 0 && errno == EBADF,
           "pread rejects invalid descriptor", errno);
    errno = 0;
    expect(pwrite(-1, "x", 1, -1) < 0 && errno == EINVAL,
           "pwrite rejects negative offset", errno);

    if (expect(pipe(pipefd) == 0, "positioned I/O create pipe", errno) == 0) {
        errno = 0;
        expect(pread(pipefd[0], buf, 1, 0) < 0 && errno == ESPIPE,
               "pread rejects non-seekable pipe", errno);
        close(pipefd[0]);
        close(pipefd[1]);
    }

    unlink(path);
}

static void test_directory_relative_io(void)
{
    const char *root = tmp_path("at-root");
    char original_cwd[128];
    char absolute_file[160];
    char absolute_link[160];
    struct stat st;
    int dirfd = -1;
    int subfd = -1;
    int fd = -1;

    remove_tree_local(root);
    if (expect(mkdir(root, 0755) == 0, "*at create base directory", errno) < 0)
        return;
    dirfd = open(root, O_RDONLY | O_DIRECTORY, 0);
    if (expect(dirfd >= 0, "*at open base directory", dirfd) < 0)
        goto cleanup;

    fd = openat(dirfd, "alpha.txt", O_CREAT | O_RDWR | O_TRUNC, 0644);
    if (expect(fd >= 0, "openat creates relative file", fd) == 0) {
        expect(write(fd, "relative", 8) == 8,
               "openat file accepts ordinary I/O", errno);
        close(fd);
        fd = -1;
    }
    expect(fstatat(dirfd, "alpha.txt", &st, 0) == 0 && st.st_size == 8,
           "fstatat reads relative metadata", errno);
    expect(mkdirat(dirfd, "sub", 0755) == 0,
           "mkdirat creates relative directory", errno);
    snprintf(absolute_link, sizeof(absolute_link), "%s/sub/link", root);
    if (expect(symlink("../alpha.txt", absolute_link) == 0,
               "*at create relative symlink", errno) == 0) {
        expect(fstatat(dirfd, "sub/link", &st, AT_SYMLINK_NOFOLLOW) == 0 &&
               S_ISLNK(st.st_mode), "fstatat can inspect final symlink", errno);
        expect(fstatat(dirfd, "sub/link", &st, 0) == 0 &&
               S_ISREG(st.st_mode) && st.st_size == 8,
               "fstatat follows final symlink by default", errno);
        expect(unlinkat(dirfd, "sub/link", 0) == 0,
               "unlinkat removes relative symlink", errno);
    }

    if (getcwd(original_cwd, sizeof(original_cwd)) != NULL &&
        expect(chdir("/") == 0, "*at change cwd away from dirfd", errno) == 0) {
        fd = openat(dirfd, "alpha.txt", O_RDONLY, 0);
        expect(fd >= 0, "openat dirfd survives chdir", fd);
        if (fd >= 0) {
            close(fd);
            fd = -1;
        }
        expect(chdir(original_cwd) == 0, "*at restore cwd", errno);
    }

    snprintf(absolute_file, sizeof(absolute_file), "%s/alpha.txt", root);
    expect(fstatat(-1, absolute_file, &st, 0) == 0 && st.st_size == 8,
           "fstatat absolute path ignores dirfd", errno);

    subfd = openat(dirfd, "sub", O_RDONLY | O_DIRECTORY, 0);
    if (expect(subfd >= 0, "openat opens relative directory", subfd) == 0) {
        expect(renameat(dirfd, "alpha.txt", subfd, "beta.txt") == 0,
               "renameat moves across directory descriptors", errno);
        expect(fstatat(subfd, "beta.txt", &st, 0) == 0 && st.st_size == 8,
               "fstatat sees renamed file", errno);
    }

    errno = 0;
    expect(openat(-1, "relative", O_RDONLY, 0) < 0 && errno == EBADF,
           "openat rejects invalid relative dirfd", errno);
    fd = openat(subfd, "beta.txt", O_RDONLY, 0);
    if (fd >= 0) {
        errno = 0;
        expect(openat(fd, "relative", O_RDONLY, 0) < 0 && errno == ENOTDIR,
               "openat rejects non-directory dirfd", errno);
        close(fd);
        fd = -1;
    }
    if (subfd >= 0) {
        close(subfd);
        subfd = -1;
    }

    expect(unlinkat(dirfd, "sub/beta.txt", 0) == 0,
           "unlinkat removes relative file", errno);
    expect(unlinkat(dirfd, "sub", AT_REMOVEDIR) == 0,
           "unlinkat AT_REMOVEDIR removes directory", errno);
    errno = 0;
    expect(unlinkat(dirfd, "missing", 0x4000) < 0 && errno == EINVAL,
           "unlinkat rejects unknown flags", errno);

cleanup:
    if (fd >= 0)
        close(fd);
    if (subfd >= 0)
        close(subfd);
    if (dirfd >= 0)
        close(dirfd);
    remove_tree_local(root);
}

static void test_access_umask(void)
{
    const char *path = tmp_path("umask.txt");
    int old_mask;
    int fd;

    expect(access("/usr/bin/systest", F_OK) == 0, "access existing file", 0);
    expect(access("/bin/nope", F_OK) < 0, "access missing file fails", 0);

    old_mask = umask(077);
    expect(old_mask >= 0, "umask returns previous mask", old_mask);

    unlink(path);
    fd = open(path, O_CREAT | O_WRONLY | O_TRUNC, 0666);
    expect(fd >= 0, "create file with process umask", fd);
    if (fd >= 0)
        close(fd);

    umask(old_mask);
}

static void test_open_permission_enforcement(void)
{
    const char *path = tmp_path("open-perms.txt");
    char buf[32];
    int fd;
    int n;

    unlink(path);
    fd = open(path, O_CREAT | O_WRONLY | O_TRUNC, 0644);
    if (expect(fd >= 0, "open perms create file", fd) < 0)
        return;
    expect(write(fd, "secret", 6) == 6, "open perms seed file", 0);
    close(fd);

    if (getuid() == 0) {
        skip("open permission denial checks skipped for root");
        unlink(path);
        return;
    }

    expect(chmod(path, 0400) == 0, "open perms chmod read-only", 0);
    fd = open(path, O_RDONLY, 0);
    if (expect(fd >= 0, "open read allowed by mode", fd) >= 0)
        close(fd);
    expect(open(path, O_WRONLY, 0) < 0, "open write denied by mode", 0);
    expect(open(path, O_WRONLY | O_TRUNC, 0) < 0,
           "open truncate denied by mode", 0);
    n = read_file(path, buf, sizeof(buf));
    expect(n == 6 && strcmp(buf, "secret") == 0,
           "failed O_TRUNC preserves contents", n);

    expect(chmod(path, 0200) == 0, "open perms chmod write-only", 0);
    expect(open(path, O_RDONLY, 0) < 0, "open read denied by mode", 0);
    fd = open(path, O_WRONLY, 0);
    if (expect(fd >= 0, "open write allowed by mode", fd) >= 0)
        close(fd);

    expect(chmod(path, 0000) == 0, "open perms chmod none", 0);
    expect(open(path, O_RDONLY, 0) < 0, "open read denied with 000", 0);
    expect(open(path, O_WRONLY, 0) < 0, "open write denied with 000", 0);

    chmod(path, 0600);
    unlink(path);
}

static void test_pipe_dup2(void)
{
    int pipefd[2];
    int cloexec_fd;
    char buf[32];
    int n;
    int saved_stdout;
    int fd;
    int dup_result;
    int write_result;
    int restore_result;

    if (expect(pipe(pipefd) == 0, "pipe create", 0) < 0)
        return;

    expect(write(pipefd[1], "pipe-ok", 7) == 7, "pipe write", 0);
    n = read(pipefd[0], buf, sizeof(buf) - 1);
    if (n >= 0)
        buf[n] = '\0';
    expect(n == 7 && strcmp(buf, "pipe-ok") == 0, "pipe read", n);
    close(pipefd[0]);
    close(pipefd[1]);

    if (expect(pipe2(pipefd, O_CLOEXEC | O_NONBLOCK) == 0,
               "pipe2 create", errno) == 0) {
        expect((fcntl(pipefd[0], F_GETFD) & FD_CLOEXEC) != 0 &&
               (fcntl(pipefd[1], F_GETFD) & FD_CLOEXEC) != 0,
               "pipe2 close-on-exec", 0);
        expect((fcntl(pipefd[0], F_GETFL) & O_NONBLOCK) != 0 &&
               (fcntl(pipefd[1], F_GETFL) & O_NONBLOCK) != 0,
               "pipe2 non-blocking", 0);
        errno = 0;
        expect(read(pipefd[0], buf, sizeof(buf)) < 0 && errno == EAGAIN,
               "pipe2 empty non-blocking read", errno);
        cloexec_fd = fcntl(pipefd[0], F_DUPFD_CLOEXEC, 8);
        if (expect(cloexec_fd >= 8, "fcntl duplicate close-on-exec",
                   cloexec_fd) == 0) {
            expect((fcntl(cloexec_fd, F_GETFD) & FD_CLOEXEC) != 0,
                   "fcntl duplicate flag", 0);
            close(cloexec_fd);
        }
        close(pipefd[0]);
        close(pipefd[1]);
    }

    saved_stdout = dup(STDOUT_FILENO);
    fd = open(tmp_path("dup2.txt"), O_CREAT | O_WRONLY | O_TRUNC, 0644);
    if (expect(saved_stdout >= 0 && fd >= 0, "dup/open for dup2", fd) < 0)
        return;

    dup_result = dup2(fd, STDOUT_FILENO);
    write_result = write(STDOUT_FILENO, "dup2-ok\n", 8);
    restore_result = dup2(saved_stdout, STDOUT_FILENO);
    close(saved_stdout);
    close(fd);

    expect(dup_result == STDOUT_FILENO, "dup2 stdout", dup_result);
    expect(write_result == 8, "dup2 redirected write syscall", write_result);
    expect(restore_result == STDOUT_FILENO, "restore stdout with dup2", restore_result);

    n = read_file(tmp_path("dup2.txt"), buf, sizeof(buf));
    expect(n == 8 && strcmp(buf, "dup2-ok\n") == 0, "dup2 redirected write", n);

    if (expect(pipe(pipefd) == 0, "broken pipe create", errno) == 0) {
        void (*old_handler)(int);

        old_handler = signal(SIGPIPE, systest_pipe_signal_handler);
        pipe_signal_seen = 0;
        close(pipefd[0]);
        errno = 0;
        expect(write(pipefd[1], "x", 1) < 0 && errno == EPIPE,
               "broken pipe returns EPIPE", errno);
        expect(pipe_signal_seen == 1,
               "broken pipe delivers SIGPIPE", pipe_signal_seen);
        close(pipefd[1]);
        signal(SIGPIPE, old_handler);
    }
}

static void test_named_fifo(void)
{
    const char *path = tmp_path("named-fifo");
    struct stat st;
    struct pollfd pfd;
    char buffer[8] = {0};
    int reader;
    int writer;

    unlink(path);
    if (expect(mkfifo(path, 0600) == 0, "mkfifo creates FIFO", errno) < 0)
        return;
    if (expect(stat(path, &st) == 0 && S_ISFIFO(st.st_mode),
               "stat reports FIFO type", errno) < 0) {
        unlink(path);
        return;
    }

    errno = 0;
    writer = open(path, O_WRONLY | O_NONBLOCK);
    expect(writer < 0 && errno == ENXIO,
           "nonblocking FIFO writer requires reader", errno);

    reader = open(path, O_RDONLY | O_NONBLOCK);
    if (expect(reader >= 0, "open nonblocking FIFO reader", errno) < 0) {
        unlink(path);
        return;
    }
    errno = 0;
    expect(lseek(reader, 0, SEEK_SET) < 0 && errno == ESPIPE,
           "FIFO rejects lseek", errno);

    writer = open(path, O_WRONLY | O_NONBLOCK);
    if (expect(writer >= 0, "open FIFO writer with reader", errno) == 0) {
        expect(write(writer, "fifo", 4) == 4, "write named FIFO", errno);
        pfd.fd = reader;
        pfd.events = POLLIN;
        pfd.revents = 0;
        expect(poll(&pfd, 1, 100) == 1 && (pfd.revents & POLLIN) != 0,
               "poll reports named FIFO data", pfd.revents);
        expect(read(reader, buffer, sizeof(buffer)) == 4 &&
               memcmp(buffer, "fifo", 4) == 0,
               "read named FIFO data", errno);
        close(writer);
    }
    close(reader);
    expect(unlink(path) == 0, "unlink named FIFO", errno);
}

static void test_fd_access_modes(void)
{
    const char *path = tmp_path("fd-modes.txt");
    char c = 0;
    int fd;

    unlink(path);

    fd = open(path, O_CREAT | O_WRONLY | O_TRUNC, 0644);
    if (expect(fd >= 0, "fd mode open write-only", fd) < 0)
        return;

    expect(write(fd, "x", 1) == 1, "fd mode write to write-only", fd);
    expect(read(fd, &c, 1) < 0, "fd mode read from write-only fails", fd);
    close(fd);

    fd = open(path, O_RDONLY, 0);
    if (expect(fd >= 0, "fd mode open read-only", fd) < 0)
        return;

    expect(read(fd, &c, 1) == 1 && c == 'x', "fd mode read from read-only", c);
    expect(write(fd, "y", 1) < 0, "fd mode write to read-only fails", fd);
    close(fd);
}

static void test_stat_syscalls(void)
{
    struct stat st;
    struct stat fst;
    int fd;

    expect(stat("/usr/bin/systest", &st) == 0, "stat existing executable", 0);
    expect(S_ISREG(st.st_mode), "stat reports regular file", st.st_mode);
    expect(st.st_size > 0, "stat reports file size", (int)st.st_size);
    expect(st.st_blksize > 0, "stat reports IO block size", (int)st.st_blksize);
    expect(st.st_blocks > 0, "stat reports allocated blocks", (int)st.st_blocks);

    fd = open("/usr/bin/systest", O_RDONLY, 0);
    if (expect(fd >= 0, "open executable for fstat", fd) >= 0) {
        expect(fstat(fd, &fst) == 0, "fstat open executable", 0);
        expect(fst.st_size == st.st_size, "fstat size matches stat", (int)fst.st_size);
        expect((fst.st_mode & S_IFMT) == (st.st_mode & S_IFMT),
               "fstat type matches stat", fst.st_mode);
        close(fd);
    }

    expect(stat("/", &st) == 0, "stat root directory", 0);
    expect(S_ISDIR(st.st_mode), "stat reports directory", st.st_mode);
    expect(stat("/bin/does-not-exist", &st) < 0, "stat missing file fails", 0);
}

static void test_statvfs_syscalls(void)
{
    struct statvfs path_st;
    struct statvfs fd_st;
    int root_ok;
    int fd;

    root_ok = statvfs("/", &path_st) == 0;
    if (expect(root_ok, "statvfs root filesystem", errno) == 0) {
        expect(path_st.f_bsize > 0, "statvfs block size", (int)path_st.f_bsize);
        expect(path_st.f_frsize > 0, "statvfs fragment size", (int)path_st.f_frsize);
        expect(path_st.f_blocks > 0, "statvfs block count", (int)path_st.f_blocks);
        expect(path_st.f_bfree <= path_st.f_blocks,
               "statvfs free blocks bounded", (int)path_st.f_bfree);
        expect(path_st.f_bavail <= path_st.f_bfree,
               "statvfs available blocks bounded", (int)path_st.f_bavail);
        expect(path_st.f_namemax > 0,
               "statvfs filename limit", (int)path_st.f_namemax);
    }

    fd = open("/usr/bin/systest", O_RDONLY, 0);
    if (expect(fd >= 0, "open executable for fstatvfs", fd) >= 0) {
        if (expect(fstatvfs(fd, &fd_st) == 0,
                   "fstatvfs open executable", errno) == 0 && root_ok) {
            expect(fd_st.f_bsize == path_st.f_bsize,
                   "fstatvfs block size matches root", (int)fd_st.f_bsize);
            expect(fd_st.f_fsid == path_st.f_fsid,
                   "fstatvfs filesystem id matches root", (int)fd_st.f_fsid);
        }
        close(fd);
    }

    errno = 0;
    expect(statvfs("/does-not-exist", &path_st) < 0 && errno == ENOENT,
           "statvfs missing path reports ENOENT", errno);
    errno = 0;
    expect(fstatvfs(-1, &fd_st) < 0 && errno == EBADF,
           "fstatvfs invalid descriptor reports EBADF", errno);
    errno = 0;
    expect(statvfs("/", NULL) < 0 && errno == EFAULT,
           "statvfs null result reports EFAULT", errno);
}

static void test_dev_null(void)
{
    struct stat st;
    struct stat fst;
    char c = 0x5a;
    int fd;

    if (expect(stat("/dev/null", &st) == 0, "stat /dev/null", 0) == 0) {
        expect(S_ISCHR(st.st_mode), "/dev/null is char device", st.st_mode);
        expect((st.st_mode & 0777) == 0666, "/dev/null mode is 666", st.st_mode & 0777);
    }

    fd = open("/dev/null", O_RDONLY, 0);
    if (expect(fd >= 0, "open /dev/null read-only", fd) >= 0) {
        expect(read(fd, &c, 1) == 0, "read /dev/null returns EOF", c);
        if (expect(fstat(fd, &fst) == 0, "fstat /dev/null", 0) == 0)
            expect(S_ISCHR(fst.st_mode), "fstat /dev/null is char device", fst.st_mode);
        close(fd);
    }

    fd = open("/dev/null", O_WRONLY, 0);
    if (expect(fd >= 0, "open /dev/null write-only", fd) >= 0) {
        expect(write(fd, "discard", 7) == 7, "write /dev/null discards bytes", 0);
        close(fd);
    }

    fd = open("/dev/null", O_RDWR, 0);
    if (expect(fd >= 0, "open /dev/null read-write", fd) >= 0) {
        expect(write(fd, "x", 1) == 1, "write /dev/null read-write", 0);
        expect(read(fd, &c, 1) == 0, "read /dev/null read-write EOF", c);
        close(fd);
    }
}

static void test_dev_tty(void)
{
    struct stat st;
    struct stat fst;
    volatile uintptr_t invalid_address = UINTPTR_MAX & ~(uintptr_t)0xfffu;
    int fd;

    if (expect(stat("/dev/tty0", &st) == 0, "stat /dev/tty0", 0) == 0) {
        expect(S_ISCHR(st.st_mode), "/dev/tty0 is char device", st.st_mode);
        expect((st.st_mode & 0777) == 0666, "/dev/tty0 mode is 666", st.st_mode & 0777);
    }

    if (expect(stat("/dev/tty", &st) == 0, "stat /dev/tty", 0) == 0) {
        expect(S_ISCHR(st.st_mode), "/dev/tty is char device", st.st_mode);
        expect((st.st_mode & 0777) == 0666, "/dev/tty mode is 666", st.st_mode & 0777);
    }

    if (expect(stat("/dev/console", &st) == 0, "stat /dev/console", 0) == 0) {
        expect(S_ISCHR(st.st_mode), "/dev/console is char device", st.st_mode);
        expect((st.st_mode & 0777) == 0666, "/dev/console mode is 666", st.st_mode & 0777);
    }

    expect(access("/dev/tty", F_OK) == 0, "access /dev/tty exists", 0);
    expect(access("/dev/tty", R_OK | W_OK) == 0, "access /dev/tty read-write", 0);
    expect(access("/dev/tty", X_OK) < 0, "access /dev/tty execute fails", 0);
    expect(access("/dev/tty0", F_OK) == 0, "access /dev/tty0 exists", 0);
    expect(access("/dev/tty0", R_OK | W_OK) == 0, "access /dev/tty0 read-write", 0);
    expect(access("/dev/tty0", X_OK) < 0, "access /dev/tty0 execute fails", 0);
    expect(access("/dev/console", F_OK) == 0, "access /dev/console exists", 0);

    fd = open("/dev/tty", O_WRONLY, 0);
    if (expect(fd >= 0, "open /dev/tty write-only", fd) >= 0) {
        if (expect(fstat(fd, &fst) == 0, "fstat /dev/tty", 0) == 0)
            expect(S_ISCHR(fst.st_mode), "fstat /dev/tty is char device", fst.st_mode);
        errno = 0;
        expect(write(fd, (const void *)invalid_address, 1u) < 0 &&
               errno == EFAULT,
               "tty write rejects invalid user pointer", errno);
        close(fd);
    }

    fd = open("/dev/tty0", O_WRONLY, 0);
    if (expect(fd >= 0, "open /dev/tty0 write-only", fd) >= 0) {
        if (expect(fstat(fd, &fst) == 0, "fstat /dev/tty0", 0) == 0)
            expect(S_ISCHR(fst.st_mode), "fstat /dev/tty0 is char device", fst.st_mode);
        close(fd);
    }

    fd = open("/dev/console", O_WRONLY, 0);
    if (expect(fd >= 0, "open /dev/console write-only", fd) >= 0) {
        if (expect(fstat(fd, &fst) == 0, "fstat /dev/console", 0) == 0)
            expect(S_ISCHR(fst.st_mode), "fstat /dev/console is char device", fst.st_mode);
        close(fd);
    }
}

static void test_chmod_chown_syscalls(void)
{
    const char *path = tmp_path("chmod-chown.txt");
    struct stat st;
    uid_t original_uid = 0;
    int fd;

    unlink(path);
    fd = open(path, O_CREAT | O_RDWR | O_TRUNC, 0644);
    if (expect(fd >= 0, "chmod/chown create test file", fd) < 0)
        return;

    expect(fchmod(fd, 0640) == 0, "fchmod descriptor syscall", 0);
    if (expect(fstat(fd, &st) == 0, "fchmod descriptor fstat", 0) == 0)
        expect((st.st_mode & 0777) == 0640,
               "fchmod descriptor mode visible", st.st_mode & 0777);

    expect(fdatasync(fd) == 0, "fdatasync descriptor syscall", 0);

    expect(chmod(path, 0600) == 0, "chmod octal syscall", 0);
    if (expect(stat(path, &st) == 0, "chmod octal stat", 0) == 0)
        expect((st.st_mode & 0777) == 0600, "chmod octal mode visible", st.st_mode & 0777);

    expect(chmod(path, 0755) == 0, "chmod 755 syscall", 0);
    if (expect(stat(path, &st) == 0, "chmod 755 stat", 0) == 0) {
        expect((st.st_mode & 0777) == 0755, "chmod 755 mode visible", st.st_mode & 0777);
        original_uid = st.st_uid;
    }

    if (getuid() == 0) {
        expect(fchown(fd, (uid_t)-1, 321) == 0,
               "fchown gid-only syscall", 0);
        if (expect(fstat(fd, &st) == 0, "fchown gid-only fstat", 0) == 0) {
            expect(st.st_gid == 321, "fchown gid-only updates gid", st.st_gid);
            expect(st.st_uid == original_uid,
                   "fchown gid-only keeps uid", st.st_uid);
        }

        expect(chown(path, (uid_t)-1, 789) == 0, "chown gid-only syscall", 0);
        if (expect(stat(path, &st) == 0, "chown gid-only stat", 0) == 0) {
            expect(st.st_gid == 789, "chown gid-only updates gid", st.st_gid);
            expect(st.st_uid == original_uid, "chown gid-only keeps uid", st.st_uid);
        }

        expect(chown(path, 123, 456) == 0, "chown uid/gid syscall", 0);
        if (expect(stat(path, &st) == 0, "chown stat", 0) == 0) {
            expect(st.st_uid == 123, "chown uid visible", st.st_uid);
            expect(st.st_gid == 456, "chown gid visible", st.st_gid);
        }
    } else {
        expect(fchown(fd, (uid_t)-1, 321) < 0,
               "fchown gid-only denied for non-root", 0);
        expect(chown(path, (uid_t)-1, 789) < 0,
               "chown gid-only denied for non-root", 0);
        expect(chown(path, 123, 456) < 0,
               "chown uid/gid denied for non-root", 0);
    }

    errno = 0;
    expect(fchmod(-1, 0600) < 0 && errno == EBADF,
           "fchmod invalid descriptor", errno);
    errno = 0;
    expect(fchown(-1, (uid_t)-1, (gid_t)-1) < 0 && errno == EBADF,
           "fchown invalid descriptor", errno);
    errno = 0;
    expect(fdatasync(-1) < 0 && errno == EBADF,
           "fdatasync invalid descriptor", errno);

    close(fd);
    chmod(path, 0600);
    unlink(path);
}

static void test_utimens_syscalls(void)
{
    const char *path = tmp_path("utimens.txt");
    const char *link_path = tmp_path("utimens-link");
    struct timespec times[2];
    struct timespec before;
    struct timespec after;
    struct stat st;
    struct stat target_st;
    int dirfd;
    int fd;

    unlink(link_path);
    unlink(path);
    fd = open(path, O_CREAT | O_RDWR | O_TRUNC, 0644);
    if (expect(fd >= 0, "utimens create test file", fd) < 0)
        return;

    times[0].tv_sec = 100;
    times[0].tv_nsec = 123456789;
    times[1].tv_sec = 200;
    times[1].tv_nsec = 987654321;
    expect(utimensat(AT_FDCWD, path, times, 0) == 0,
           "utimensat explicit timestamps", errno);
    if (expect(stat(path, &st) == 0, "utimensat stat result", errno) == 0) {
        expect(st.st_atime == 100, "utimensat stores access seconds", (int)st.st_atime);
        expect(st.st_mtime == 200, "utimensat stores modify seconds", (int)st.st_mtime);
    }

    dirfd = open(systest_tmp_root, O_RDONLY | O_DIRECTORY, 0);
    if (expect(dirfd >= 0, "utimensat open directory fd", dirfd) == 0) {
        times[0].tv_sec = 700;
        times[0].tv_nsec = 0;
        times[1].tv_sec = 800;
        times[1].tv_nsec = 0;
        expect(utimensat(dirfd, "utimens.txt", times, 0) == 0,
               "utimensat directory-relative timestamps", errno);
        if (expect(fstat(fd, &st) == 0,
                   "utimensat directory-relative fstat", errno) == 0)
            expect(st.st_mtime == 800,
                   "utimensat resolves relative to dirfd", (int)st.st_mtime);
        close(dirfd);
    }

    times[0].tv_sec = 300;
    times[0].tv_nsec = 0;
    times[1].tv_sec = 400;
    times[1].tv_nsec = 0;
    expect(futimens(fd, times) == 0, "futimens descriptor timestamps", errno);
    if (expect(fstat(fd, &st) == 0, "futimens fstat result", errno) == 0) {
        expect(st.st_atime == 300, "futimens stores access seconds", (int)st.st_atime);
        expect(st.st_mtime == 400, "futimens stores modify seconds", (int)st.st_mtime);
    }

    clock_gettime(CLOCK_REALTIME, &before);
    times[0].tv_sec = 0;
    times[0].tv_nsec = UTIME_OMIT;
    times[1].tv_sec = 0;
    times[1].tv_nsec = UTIME_NOW;
    expect(futimens(fd, times) == 0, "futimens NOW and OMIT", errno);
    clock_gettime(CLOCK_REALTIME, &after);
    if (expect(fstat(fd, &st) == 0, "futimens NOW and OMIT stat", errno) == 0) {
        expect(st.st_atime == 300, "UTIME_OMIT preserves access time", (int)st.st_atime);
        expect(st.st_mtime >= before.tv_sec && st.st_mtime <= after.tv_sec + 1,
               "UTIME_NOW uses realtime clock", (int)st.st_mtime);
    }

    if (expect(symlink(path, link_path) == 0,
               "utimens create symlink", errno) == 0) {
        times[0].tv_sec = 500;
        times[0].tv_nsec = 0;
        times[1].tv_sec = 600;
        times[1].tv_nsec = 0;
        expect(utimensat(AT_FDCWD, link_path, times, AT_SYMLINK_NOFOLLOW) == 0,
               "utimensat updates symlink itself", errno);
        if (expect(lstat(link_path, &st) == 0,
                   "utimensat lstat symlink", errno) == 0)
            expect(st.st_mtime == 600,
                   "utimensat nofollow symlink timestamp", (int)st.st_mtime);
        if (expect(stat(path, &target_st) == 0,
                   "utimensat target stat", errno) == 0)
            expect(target_st.st_mtime != 600,
                   "utimensat nofollow preserves target", (int)target_st.st_mtime);
    }

    times[0].tv_sec = 0;
    times[0].tv_nsec = 1000000000L;
    times[1].tv_sec = 0;
    times[1].tv_nsec = 0;
    errno = 0;
    expect(futimens(fd, times) < 0 && errno == EINVAL,
           "futimens rejects invalid nanoseconds", errno);
    errno = 0;
    expect(futimens(-1, NULL) < 0 && errno == EBADF,
           "futimens rejects invalid descriptor", errno);

    close(fd);
    unlink(link_path);
    unlink(path);
}

static void test_proc_net_syscalls(void)
{
    char buf[512];
    int n;

    n = read_file("/proc/net/dev", buf, sizeof(buf));
    expect(n > 0 && strstr(buf, "Inter-|") != NULL,
           "proc net dev readable", n);

    n = read_file("/proc/net/tcp", buf, sizeof(buf));
    expect(n > 0 && strstr(buf, "local_address") != NULL,
           "proc net tcp readable", n);
}

static void test_posix_compat_syscalls(void)
{
    const char *path = tmp_path("compat-syscalls.txt");
    const char *rename_src = tmp_path("compat-rename-src.txt");
    const char *rename_dst = tmp_path("compat-rename-dst.txt");
    struct stat st;
    struct timeval tv;
    struct timezone tz;
    struct termios tio;
    struct winsize wsz;
    struct winsize original_wsz;
    time_t now = 0;
    int fd;
    int dupfd;
    int flags;
    struct iovec iov[2];
    char readv_buf1[8];
    char readv_buf2[8];
    struct pollfd pfd;
    int pipefd[2];
    fd_set rfds;
    struct timeval zero_tv;
    struct timespec zero_ts;
    struct rusage usage;
    struct flock fl;
    struct utsname uts;
    struct sigaction chld_action;
    struct sigaction old_chld_action;
    sigset_t mask;
    sigset_t oldmask;
    sigset_t pending;
    char *map;
    char fixed_realpath[128];
    char cwd_before[128];
    char fdopen_buf[16];
    char *allocated_realpath;
    FILE *compat_fp;
    pid_t child;
    int child_status;
    int chld_handler_installed;
    int chld_gate[2] = { -1, -1 };

    unlink(path);
    unlink(rename_src);
    unlink(rename_dst);

    fd = creat(path, 0644);
    if (expect(fd >= 0, "creat creates file", fd) >= 0) {
        expect(write(fd, "compat", 6) == 6, "creat fd is writable", fd);
        close(fd);
    }

    expect(lstat(path, &st) == 0, "lstat existing file", 0);
    expect(S_ISREG(st.st_mode), "lstat reports regular file", st.st_mode);
    expect(realpath(path, fixed_realpath) == fixed_realpath &&
           fixed_realpath[0] == '/', "realpath resolves absolute file", errno);

    if (expect(getcwd(cwd_before, sizeof(cwd_before)) != NULL,
               "getcwd before relative realpath", errno) == 0 &&
        expect(chdir(systest_tmp_root) == 0, "chdir for relative realpath", errno) == 0) {
        allocated_realpath = realpath("compat-syscalls.txt", NULL);
        expect(allocated_realpath != NULL &&
               strcmp(allocated_realpath, fixed_realpath) == 0,
               "realpath allocates relative canonical path", errno);
        free(allocated_realpath);
        expect(chdir(cwd_before) == 0, "chdir restores cwd after realpath", errno);
    }

    fd = open(path, O_RDONLY, 0);
    if (expect(fd >= 0, "fdopen source open", fd) >= 0) {
        compat_fp = fdopen(fd, "r");
        if (expect(compat_fp != NULL, "fdopen wraps descriptor", errno) == 0) {
            memset(fdopen_buf, 0, sizeof(fdopen_buf));
            expect(fread(fdopen_buf, 1, 6, compat_fp) == 6,
                   "fdopen fread reads data", ferror(compat_fp));
            expect(strcmp(fdopen_buf, "compat") == 0,
                   "fdopen preserves file contents", fdopen_buf[0]);
            expect(fclose(compat_fp) == 0, "fdopen fclose succeeds", errno);
        } else {
            close(fd);
        }
    }

    fd = creat(rename_src, 0644);
    if (expect(fd >= 0, "rename source create", fd) >= 0) {
        close(fd);
        expect(rename(rename_src, rename_dst) == 0, "rename moves file", errno);
        expect(access(rename_dst, F_OK) == 0, "rename destination exists", errno);
        expect(remove(rename_dst) == 0, "remove unlinks renamed file", errno);
    }

    fd = open(path, O_RDWR, 0);
    if (expect(fd >= 0, "fcntl test open", fd) >= 0) {
        expect(fcntl(fd, F_GETFD, 0) == 0, "fcntl F_GETFD default", fd);
        expect(fcntl(fd, F_SETFD, FD_CLOEXEC) == 0, "fcntl F_SETFD cloexec", fd);
        expect((fcntl(fd, F_GETFD, 0) & FD_CLOEXEC) != 0,
               "fcntl F_GETFD observes cloexec", fd);

        flags = fcntl(fd, F_GETFL, 0);
        expect((flags & O_ACCMODE) == O_RDWR, "fcntl F_GETFL access mode", flags);
        expect(fcntl(fd, F_SETFL, flags | O_APPEND) == 0, "fcntl F_SETFL append", flags);
        expect((fcntl(fd, F_GETFL, 0) & O_APPEND) != 0,
               "fcntl F_GETFL observes append", fd);

        dupfd = fcntl(fd, F_DUPFD, 10);
        if (expect(dupfd >= 10, "fcntl F_DUPFD min fd", dupfd) >= 0)
            close(dupfd);

        memset(&fl, 0, sizeof(fl));
        fl.l_type = F_WRLCK;
        fl.l_whence = SEEK_SET;
        expect(fcntl(fd, F_SETLK, &fl) == 0, "fcntl F_SETLK advisory lock", errno);
        expect(fcntl(fd, F_GETLK, &fl) == 0, "fcntl F_GETLK advisory lock", errno);
        expect(fl.l_type == F_UNLCK, "fcntl F_GETLK reports unlocked", fl.l_type);

        expect(ioctl(fd, TCGETS, &tio) < 0 && errno == ENOTTY,
               "ioctl TCGETS rejects regular file", errno);
        expect(tcflush(fd, TCIFLUSH) < 0 && errno == ENOTTY,
               "tcflush rejects regular file", errno);
        close(fd);
    }

    fd = open(path, O_RDWR | O_TRUNC, 0);
    if (expect(fd >= 0, "readv/writev open", fd) >= 0) {
        iov[0].iov_base = "vec";
        iov[0].iov_len = 3;
        iov[1].iov_base = "tor";
        iov[1].iov_len = 3;
        expect(writev(fd, iov, 2) == 6, "writev writes multiple buffers", errno);
        lseek(fd, 0, SEEK_SET);
        memset(readv_buf1, 0, sizeof(readv_buf1));
        memset(readv_buf2, 0, sizeof(readv_buf2));
        iov[0].iov_base = readv_buf1;
        iov[0].iov_len = 3;
        iov[1].iov_base = readv_buf2;
        iov[1].iov_len = 3;
        expect(readv(fd, iov, 2) == 6, "readv reads multiple buffers", errno);
        expect(strcmp(readv_buf1, "vec") == 0 && strcmp(readv_buf2, "tor") == 0,
               "readv/writev preserve data", 0);
        close(fd);
    }

    if (expect(pipe(pipefd) == 0, "poll/select pipe create", errno) == 0) {
        memset(&pfd, 0, sizeof(pfd));
        pfd.fd = pipefd[0];
        pfd.events = POLLIN;
        expect(poll(&pfd, 1, 0) == 0, "poll empty pipe times out", pfd.revents);
        write(pipefd[1], "x", 1);
        expect(poll(&pfd, 1, 0) == 1 && (pfd.revents & POLLIN),
               "poll detects readable pipe", pfd.revents);
        FD_ZERO(&rfds);
        FD_SET(pipefd[0], &rfds);
        zero_tv.tv_sec = 0;
        zero_tv.tv_usec = 0;
        expect(select(pipefd[0] + 1, &rfds, NULL, NULL, &zero_tv) == 1 &&
               FD_ISSET(pipefd[0], &rfds),
               "select detects readable pipe", errno);
        zero_ts.tv_sec = 0;
        zero_ts.tv_nsec = 0;
        pfd.revents = 0;
        expect(ppoll(&pfd, 1, &zero_ts, NULL) == 1 && (pfd.revents & POLLIN),
               "ppoll detects readable pipe", pfd.revents);
        FD_ZERO(&rfds);
        FD_SET(pipefd[0], &rfds);
        expect(pselect(pipefd[0] + 1, &rfds, NULL, NULL, &zero_ts, NULL) == 1 &&
               FD_ISSET(pipefd[0], &rfds),
               "pselect detects readable pipe", errno);
        read(pipefd[0], readv_buf1, 1);
        close(pipefd[0]);
        close(pipefd[1]);
    }

    expect(getrusage(RUSAGE_SELF, &usage) == 0, "getrusage RUSAGE_SELF", errno);

    if (expect(uname(&uts) == 0, "uname syscall", errno) == 0) {
        expect(strcmp(uts.sysname, "ArmOS") == 0, "uname sysname", uts.sysname[0]);
        expect(strcmp(uts.machine, "armv7l") == 0 ||
               strcmp(uts.machine, "aarch64") == 0,
               "uname machine", uts.machine[0]);
    }

    if (expect(sigemptyset(&mask) == 0, "sigemptyset", errno) == 0 &&
        expect(sigaddset(&mask, SIGUSR1) == 0, "sigaddset SIGUSR1", errno) == 0 &&
        expect(sigprocmask(SIG_BLOCK, &mask, &oldmask) == 0,
               "sigprocmask blocks SIGUSR1", errno) == 0) {
        compat_signal_seen = 0;
        signal(SIGUSR1, systest_compat_signal_handler);
        kill(getpid(), SIGUSR1);
        expect(sigpending(&pending) == 0 && sigismember(&pending, SIGUSR1) == 1,
               "sigpending sees blocked SIGUSR1", errno);
        expect(compat_signal_seen == 0, "blocked SIGUSR1 not delivered early",
               compat_signal_seen);
        sigprocmask(SIG_SETMASK, &oldmask, NULL);
        sleep(0);
        expect(compat_signal_seen == 1, "unblocked SIGUSR1 delivered",
               compat_signal_seen);
        signal(SIGUSR1, SIG_DFL);
    }

    map = mmap(NULL, 4096, PROT_READ | PROT_WRITE,
               MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (expect(map != MAP_FAILED, "mmap for mprotect", errno) == 0) {
        map[0] = 'm';
        expect(mprotect(map, 4096, PROT_READ) == 0, "mprotect whole mapping read-only", errno);
        munmap(map, 4096);
    }

    child = fork();
    if (child == 0)
        exit(33);
    if (expect(child > 0, "wait4 fork child", child) == 0) {
        expect(wait4(child, &child_status, 0, &usage) == child,
               "wait4 collects child", errno);
        expect(WIFEXITED(child_status) && WEXITSTATUS(child_status) == 33,
               "wait4 status reports exit code", child_status);
    }

    compat_sigchld_value = 0;
    signal(SIGCHLD, systest_sigchld_handler);
    child = fork();
    if (child == 0)
        _exit(34);
    if (expect(child > 0, "SIGCHLD translation fork child", child) == 0) {
        expect(waitpid(child, &child_status, 0) == child,
               "SIGCHLD translation collects child", errno);
        expect(compat_sigchld_value == SIGCHLD,
               "SIGCHLD handler receives newlib signal number",
               compat_sigchld_value);
    }
    signal(SIGCHLD, SIG_DFL);

    /*
     * POSIX shells avoid the waitpid scan/sleep race by blocking SIGCHLD,
     * polling with WNOHANG, then atomically unblocking in sigsuspend().
     * A child exit must wake this path even though the parent is blocked in
     * a syscall other than waitpid().
     */
    memset(&chld_action, 0, sizeof(chld_action));
    memset(&old_chld_action, 0, sizeof(old_chld_action));
    chld_action.sa_handler = systest_sigchld_handler;
    sigemptyset(&chld_action.sa_mask);
    compat_sigchld_value = 0;
    sigemptyset(&mask);
    sigaddset(&mask, SIGCHLD);
    chld_handler_installed =
        sigaction(SIGCHLD, &chld_action, &old_chld_action) == 0;
    if (expect(chld_handler_installed,
               "SIGCHLD sigsuspend handler install", errno) == 0 &&
        expect(sigprocmask(SIG_BLOCK, &mask, &oldmask) == 0,
               "SIGCHLD blocked before child scan", errno) == 0 &&
        expect(pipe(chld_gate) == 0,
               "SIGCHLD sigsuspend synchronization pipe", errno) == 0) {
        child = fork();
        if (child == 0) {
            char token;

            close(chld_gate[1]);
            if (read(chld_gate[0], &token, 1) != 1)
                _exit(125);
            close(chld_gate[0]);
            _exit(35);
        }
        close(chld_gate[0]);
        chld_gate[0] = -1;
        if (expect(child > 0, "SIGCHLD sigsuspend fork child", child) == 0) {
            int suspend_errno;
            int suspend_result;

            child_status = -1;
            expect(waitpid(child, &child_status, WNOHANG) == 0,
                   "SIGCHLD child remains live before sigsuspend",
                   child_status);
            expect(write(chld_gate[1], "x", 1) == 1,
                   "SIGCHLD child release before sigsuspend", errno);
            close(chld_gate[1]);
            chld_gate[1] = -1;
            errno = 0;
            suspend_result = sigsuspend(&oldmask);
            suspend_errno = errno;
            expect(suspend_result == -1 && suspend_errno == EINTR,
                   "sigsuspend returns EINTR after SIGCHLD",
                   suspend_result == -1 ? suspend_errno : suspend_result);
            if (!WIFEXITED(child_status))
                (void)waitpid(child, &child_status, 0);
            expect(compat_sigchld_value == SIGCHLD,
                   "SIGCHLD wakes sigsuspend waiter",
                   compat_sigchld_value);
            expect(WIFEXITED(child_status) && WEXITSTATUS(child_status) == 35,
                   "SIGCHLD sigsuspend child status", child_status);
        } else {
            close(chld_gate[1]);
            chld_gate[1] = -1;
        }
        (void)sigprocmask(SIG_SETMASK, &oldmask, NULL);
    }
    if (chld_gate[0] >= 0)
        close(chld_gate[0]);
    if (chld_gate[1] >= 0)
        close(chld_gate[1]);
    if (chld_handler_installed)
        (void)sigaction(SIGCHLD, &old_chld_action, NULL);

    if (!stdin_is_foreground_tty()) {
        skip("tty ioctl/termios mutation (not foreground job)");
        unlink(path);
        return;
    }

    expect(ioctl(STDIN_FILENO, TCGETS, &tio) == 0, "ioctl TCGETS accepts tty", 0);
    memset(&original_wsz, 0, sizeof(original_wsz));
    if (expect(ioctl(STDIN_FILENO, TIOCGWINSZ, &wsz) == 0,
               "ioctl TIOCGWINSZ accepts tty", 0) == 0) {
        original_wsz = wsz;
        expect(wsz.ws_col > 0, "ioctl TIOCGWINSZ reports columns", wsz.ws_col);
        expect(wsz.ws_row > 0, "ioctl TIOCGWINSZ reports rows", wsz.ws_row);
    }
    wsz.ws_row = 25;
    wsz.ws_col = 100;
    wsz.ws_xpixel = 0;
    wsz.ws_ypixel = 0;
    winch_signal_seen = 0;
    signal(SIGWINCH, systest_winch_signal_handler);
    expect(ioctl(STDIN_FILENO, TIOCSWINSZ, &wsz) == 0,
           "ioctl TIOCSWINSZ accepts tty", 0);
    expect(winch_signal_seen == 1, "ioctl TIOCSWINSZ delivers SIGWINCH", winch_signal_seen);
    memset(&wsz, 0, sizeof(wsz));
    if (expect(ioctl(STDIN_FILENO, TIOCGWINSZ, &wsz) == 0,
               "ioctl TIOCGWINSZ reads updated size", 0) == 0) {
        expect(wsz.ws_col == 100, "ioctl TIOCSWINSZ updates columns", wsz.ws_col);
        expect(wsz.ws_row == 25, "ioctl TIOCSWINSZ updates rows", wsz.ws_row);
    }
    expect(ioctl(STDIN_FILENO, TIOCSWINSZ, &wsz) == 0,
           "ioctl TIOCSWINSZ accepts unchanged size", 0);
    expect(winch_signal_seen == 1, "ioctl unchanged winsize skips SIGWINCH", winch_signal_seen);
    signal(SIGWINCH, SIG_DFL);
    wsz = original_wsz;
    expect(ioctl(STDIN_FILENO, TIOCSWINSZ, &wsz) == 0,
           "ioctl TIOCSWINSZ restores default size", 0);
    expect(tcgetattr(STDIN_FILENO, &tio) == 0, "tcgetattr accepts tty", 0);
    expect((tio.c_lflag & ICANON) != 0, "tcgetattr reports canonical mode", tio.c_lflag);
    expect((tio.c_lflag & ISIG) != 0, "tcgetattr reports signal mode", tio.c_lflag);
    expect((tio.c_iflag & ICRNL) != 0, "tcgetattr reports CR-to-NL input mode", tio.c_iflag);
    expect((tio.c_oflag & OPOST) != 0, "tcgetattr reports output post-processing", tio.c_oflag);
    expect((tio.c_oflag & ONLCR) != 0, "tcgetattr reports NL-to-CRNL output mode", tio.c_oflag);
    expect(tio.c_cc[VMIN] == 1, "tcgetattr reports VMIN=1", tio.c_cc[VMIN]);
    expect(tio.c_cc[VINTR] == 3, "tcgetattr reports VINTR Ctrl-C", tio.c_cc[VINTR]);
    expect(tcflush(STDIN_FILENO, TCIFLUSH) == 0, "tcflush accepts tty input queue", 0);
    {
        struct termios saved_tio = tio;
        struct termios raw_tio = tio;
        struct termios check_tio;

        raw_tio.c_lflag &= ~(ICANON | ECHO);
        raw_tio.c_iflag &= ~ICRNL;
        raw_tio.c_oflag &= ~OPOST;
        expect(tcsetattr(STDIN_FILENO, TCSANOW, &raw_tio) == 0,
               "tcsetattr clears canonical/echo", 0);
        if (expect(tcgetattr(STDIN_FILENO, &check_tio) == 0,
                   "tcgetattr observes tcsetattr", 0) == 0) {
            expect((check_tio.c_lflag & ICANON) == 0,
                   "tcgetattr observes noncanonical mode", check_tio.c_lflag);
            expect((check_tio.c_lflag & ECHO) == 0,
                   "tcgetattr observes echo disabled", check_tio.c_lflag);
            expect((check_tio.c_lflag & ISIG) != 0,
                   "tcsetattr preserves signal mode", check_tio.c_lflag);
            expect((check_tio.c_iflag & ICRNL) == 0,
                   "tcgetattr observes ICRNL disabled", check_tio.c_iflag);
            expect((check_tio.c_oflag & OPOST) == 0,
                   "tcgetattr observes OPOST disabled", check_tio.c_oflag);
        }
        expect(tcsetattr(STDIN_FILENO, TCSANOW, &saved_tio) == 0,
               "tcsetattr restores tty mode", 0);
    }

    expect(time(&now) != (time_t)-1 && now != 0, "time returns timestamp", (int)now);
    expect(gettimeofday(&tv, &tz) == 0, "gettimeofday succeeds", 0);
    expect(tv.tv_sec != 0 && tv.tv_usec < 1000000,
           "gettimeofday reports sane value", (int)tv.tv_usec);

    unlink(path);
}

static void test_ext2_links_and_dirents(void)
{
    const char *path = tmp_path("link-target.txt");
    const char *hard = tmp_path("link-hard.txt");
    const char *sym = tmp_path("link-sym.txt");
    const char *cmd_hard = tmp_path("ln-cmd-hard.txt");
    const char *cmd_sym = tmp_path("ln-cmd-sym.txt");
    struct stat st_path;
    struct stat st_hard;
    struct stat st_sym;
    char buf[64];
    char dents[512];
    int fd;
    int n;
    int saw_dot = 0;
    int saw_dotdot = 0;

    unlink(cmd_sym);
    unlink(cmd_hard);
    unlink(sym);
    unlink(hard);
    unlink(path);

    fd = open(path, O_CREAT | O_WRONLY | O_TRUNC, 0644);
    if (expect(fd >= 0, "link target create", fd) < 0)
        return;
    expect(write(fd, "linked", 6) == 6, "link target write", fd);
    close(fd);

    expect(link(path, hard) == 0, "hard link syscall", 0);
    if (expect(stat(path, &st_path) == 0, "hard link stat original", 0) == 0 &&
        expect(stat(hard, &st_hard) == 0, "hard link stat alias", 0) == 0) {
        expect(st_path.st_ino == st_hard.st_ino, "hard link shares inode", st_hard.st_ino);
        expect(st_path.st_nlink >= 2 && st_hard.st_nlink >= 2,
               "hard link increments nlink", st_hard.st_nlink);
    }

    expect(unlink(path) == 0, "unlink original hard link", 0);
    n = read_file(hard, buf, sizeof(buf));
    expect(n == 6 && strcmp(buf, "linked") == 0,
           "hard link survives original unlink", n);

    expect(symlink(hard, sym) == 0, "symlink syscall", 0);
    memset(buf, 0, sizeof(buf));
    n = readlink(sym, buf, sizeof(buf) - 1);
    if (expect(n == (int)strlen(hard), "readlink returns target length", n) == 0) {
        buf[n] = '\0';
        expect(strcmp(buf, hard) == 0, "readlink target bytes", n);
    }

    if (expect(lstat(sym, &st_sym) == 0, "lstat symlink", 0) == 0)
        expect(S_ISLNK(st_sym.st_mode), "lstat reports symlink", st_sym.st_mode);

    n = read_file(sym, buf, sizeof(buf));
    expect(n == 6 && strcmp(buf, "linked") == 0, "stat/open follows symlink", n);

    expect(run_ln2(hard, cmd_hard) == 0, "ln command hard link", 0);
    n = read_file(cmd_hard, buf, sizeof(buf));
    expect(n == 6 && strcmp(buf, "linked") == 0, "ln hard link readable", n);

    expect(run_ln3("-s", hard, cmd_sym) == 0, "ln -s command symlink", 0);
    memset(buf, 0, sizeof(buf));
    n = readlink(cmd_sym, buf, sizeof(buf) - 1);
    if (expect(n == (int)strlen(hard), "ln -s readlink length", n) == 0) {
        buf[n] = '\0';
        expect(strcmp(buf, hard) == 0, "ln -s readlink target", n);
    }

    fd = open(systest_tmp_root, O_RDONLY | O_DIRECTORY, 0);
    if (expect(fd >= 0, "getdents open /tmp", fd) >= 0) {
        n = getdents(fd, dents, sizeof(dents));
        close(fd);
        if (expect(n > 0, "getdents returns entries", n) == 0) {
            int pos = 0;
            while (pos < n) {
                struct linux_dirent *de = (struct linux_dirent *)(dents + pos);
                if (strcmp(de->d_name, ".") == 0)
                    saw_dot = 1;
                if (strcmp(de->d_name, "..") == 0)
                    saw_dotdot = 1;
                if (de->d_reclen == 0)
                    break;
                pos += de->d_reclen;
            }
            expect(saw_dot, "getdents exposes dot", saw_dot);
            expect(saw_dotdot, "getdents exposes dotdot", saw_dotdot);
        }
    }

    unlink(cmd_sym);
    unlink(cmd_hard);
    unlink(sym);
    unlink(hard);
}

static void test_ext2_write_edges(void)
{
    char buf[64];
    char bigbuf[512];
    struct stat st;
    int fd;
    int n;
    int total;

    unlink(tmp_path("ext2-renamed.txt"));
    unlink(tmp_path("ext2-edge.txt"));
    unlink(tmp_path("ext2-big.bin"));
    rmdir(tmp_path("ext2-edge-dir"));

    expect(mkdir(tmp_path("ext2-edge-dir"), 0755) == 0, "ext2 mkdir edge dir", 0);
    if (expect(stat(tmp_path("ext2-edge-dir"), &st) == 0, "ext2 stat new dir", 0) == 0)
        expect(S_ISDIR(st.st_mode) && st.st_nlink >= 2,
               "ext2 new dir link count", (int)st.st_nlink);

    fd = open(tmp_path("ext2-edge-dir/file.txt"), O_CREAT | O_WRONLY | O_TRUNC, 0644);
    if (expect(fd >= 0, "ext2 create file in new dir", fd) >= 0) {
        expect(write(fd, "abcdef", 6) == 6, "ext2 write new file", fd);
        close(fd);
    }
    if (expect(stat(tmp_path("ext2-edge-dir/file.txt"), &st) == 0, "ext2 stat new file", 0) == 0)
        expect(S_ISREG(st.st_mode) && st.st_nlink == 1,
               "ext2 new file link count", (int)st.st_nlink);

    fd = open(tmp_path("ext2-edge-dir/file.txt"), O_RDONLY | O_DIRECTORY, 0);
    expect(fd < 0, "open O_DIRECTORY rejects regular file", fd);
    if (fd >= 0)
        close(fd);

    expect(rmdir(tmp_path("ext2-edge-dir")) < 0, "ext2 rmdir non-empty fails", 0);
    expect(rename(tmp_path("ext2-edge-dir/file.txt"), tmp_path("ext2-renamed.txt")) == 0,
           "ext2 rename file out of dir", 0);
    expect(rmdir(tmp_path("ext2-edge-dir")) == 0, "ext2 rmdir empty dir", 0);

    fd = open(tmp_path("ext2-renamed.txt"), O_WRONLY | O_TRUNC, 0);
    if (expect(fd >= 0, "ext2 open existing with trunc", fd) >= 0) {
        expect(write(fd, "xy", 2) == 2, "ext2 write after trunc", fd);
        close(fd);
    }

    fd = open(tmp_path("ext2-renamed.txt"), O_WRONLY | O_APPEND, 0);
    if (expect(fd >= 0, "ext2 open existing with append", fd) >= 0) {
        expect(write(fd, "z", 1) == 1, "ext2 append write", fd);
        close(fd);
    }

    n = read_file(tmp_path("ext2-renamed.txt"), buf, sizeof(buf));
    expect(n == 3 && strcmp(buf, "xyz") == 0, "ext2 read truncate+append result", n);

    memset(bigbuf, 'A', sizeof(bigbuf));
    fd = open(tmp_path("ext2-big.bin"), O_CREAT | O_RDWR | O_TRUNC, 0644);
    if (expect(fd >= 0, "ext2 double-indirect create", fd) >= 0) {
        total = 0;
        while (total < 300 * 1024) {
            if (write(fd, bigbuf, sizeof(bigbuf)) != (int)sizeof(bigbuf))
                break;
            total += sizeof(bigbuf);
        }
        expect(total == 300 * 1024, "ext2 double-indirect write", total);
        expect(lseek(fd, 299 * 1024, SEEK_SET) == 299 * 1024,
               "ext2 double-indirect seek", 0);
        memset(buf, 0, sizeof(buf));
        n = read(fd, buf, 16);
        expect(n == 16 && buf[0] == 'A' && buf[15] == 'A',
               "ext2 double-indirect read tail", n);
        close(fd);
    }
    unlink(tmp_path("ext2-big.bin"));

    expect(unlink(tmp_path("ext2-renamed.txt")) == 0, "ext2 unlink renamed file", 0);
    expect(open(tmp_path("ext2-renamed.txt"), O_RDONLY, 0) < 0, "ext2 unlinked file missing", 0);
}

static void test_rm_recursive_utility(void)
{
    struct stat st;
    int fd;

    unlink(tmp_path("rm-dir-link"));
    unlink(tmp_path("rm-tree/sub/file.txt"));
    rmdir(tmp_path("rm-tree/sub"));
    rmdir(tmp_path("rm-tree"));

    expect(mkdir(tmp_path("rm-tree"), 0755) == 0, "rm -r mkdir root", 0);
    expect(mkdir(tmp_path("rm-tree/sub"), 0755) == 0, "rm -r mkdir child", 0);

    fd = open(tmp_path("rm-tree/sub/file.txt"), O_CREAT | O_WRONLY | O_TRUNC, 0644);
    if (expect(fd >= 0, "rm -r create nested file", fd) >= 0) {
        expect(write(fd, "x", 1) == 1, "rm -r write nested file", fd);
        close(fd);
    }

    expect(run_rm1(tmp_path("rm-tree")) != 0, "rm refuses directory without -r", 0);
    expect(stat(tmp_path("rm-tree/sub/file.txt"), &st) == 0, "rm refusal preserves tree", 0);

    expect(symlink(tmp_path("rm-tree"), tmp_path("rm-dir-link")) == 0, "rm symlink-to-dir setup", 0);
    expect(run_rm2("-rf", tmp_path("rm-dir-link")) == 0, "rm -rf removes symlink to dir only", 0);
    expect(lstat(tmp_path("rm-dir-link"), &st) < 0, "rm removed symlink itself", 0);
    expect(stat(tmp_path("rm-tree/sub/file.txt"), &st) == 0,
           "rm symlink target tree survives", 0);

    expect(run_rm2("-rf", tmp_path("rm-tree")) == 0, "rm -rf removes directory tree", 0);
    expect(stat(tmp_path("rm-tree"), &st) < 0, "rm -rf removed root directory", 0);
}

static void test_fork_wait_kill(void)
{
    int status = -1;
    int pid;
    int waited;

    pid = fork();
    if (pid == 0)
        exit(42);

    if (expect(pid > 0, "fork child", pid) < 0)
        return;

    waited = waitpid(pid, &status, 0);
    expect(waited == pid && status_exited(status, 42),
           "waitpid collects exit status", status);

    pid = fork();
    if (pid == 0) {
        while (1)
            usleep(10000);
    }

    if (expect(pid > 0, "fork kill child", pid) < 0)
        return;

    expect(kill(pid, SIGKILL) == 0, "kill SIGKILL", 0);
    waited = waitpid(pid, &status, 0);
    expect(waited == pid && status_signaled(status, SIGKILL),
           "waitpid collects killed child", status);
}

static void test_sigchld_discard_reaps_children(void)
{
    enum { CHILDREN = 24 };
    void (*old_handler)(int);
    int pids[CHILDREN];
    int created = 0;
    int unreaped = 0;

    old_handler = signal(SIGCHLD, SIG_IGN);
    if (expect(old_handler != SIG_ERR,
               "SIGCHLD ignore installs auto-reap", errno) < 0)
        return;

    for (int i = 0; i < CHILDREN; i++) {
        int pid = fork();

        if (pid == 0)
            _exit(i & 0x7f);
        if (pid < 0)
            break;
        pids[created++] = pid;
    }

    for (int round = 0; round < 200; round++) {
        unreaped = 0;
        for (int i = 0; i < created; i++) {
            int status = 0;
            int waited;

            if (pids[i] < 0)
                continue;
            errno = 0;
            waited = waitpid(pids[i], &status, WNOHANG);
            if (waited == 0) {
                unreaped++;
            } else if (waited == pids[i]) {
                fail("SIGCHLD ignore exposed waitable zombie", pids[i]);
                pids[i] = -1;
            } else if (waited < 0 && errno == ECHILD) {
                pids[i] = -1;
            } else if (pids[i] >= 0) {
                fail("SIGCHLD ignore waitpid result", errno);
                pids[i] = -1;
            }
        }
        if (unreaped == 0)
            break;
        usleep(1000);
    }

    expect(created == CHILDREN,
           "SIGCHLD ignore created child batch", created);
    expect(unreaped == 0,
           "SIGCHLD ignore auto-reaps child batch", unreaped);
    (void)signal(SIGCHLD, old_handler);
}

static void test_process_groups(void)
{
    int pids[2] = {-1, -1};
    int pgid = 0;
    int status = -1;
    int reaped = 0;

    for (int i = 0; i < 2; i++) {
        int pid = fork();
        if (pid == 0) {
            while (1)
                usleep(50000);
        }

        if (pid < 0)
            break;

        if (i == 0)
            pgid = pid;
        pids[i] = pid;
        setpgid(pid, pgid);
    }

    if (expect(pids[0] > 0 && pids[1] > 0, "process group fork children", pids[1]) < 0)
        return;

    expect(kill(-pgid, SIGKILL) == 0, "process group kill -pgid", pgid);

    for (int i = 0; i < 2; i++) {
        status = -1;
        if (waitpid(pids[i], &status, 0) == pids[i])
            reaped++;
    }

    expect(reaped == 2, "process group reaps killed children", reaped);
}

static void test_waitpid_process_group(void)
{
    int pids[2] = {-1, -1};
    int pgid = 0;
    int status = -1;
    int waited;
    int reaped = 0;

    for (int i = 0; i < 2; i++) {
        int pid = fork();
        if (pid == 0) {
            if (i == 0) {
                usleep(50000); /* Let the parent set the target process group. */
                exit(61);
            }
            while (1)
                usleep(50000);
        }

        if (pid < 0)
            break;

        if (i == 0)
            pgid = pid;
        pids[i] = pid;
        setpgid(pid, pgid);
    }

    if (expect(pids[0] > 0 && pids[1] > 0, "waitpid -pgid fork children", pids[1]) < 0)
        return;

    waited = waitpid(-pgid, &status, 0);
    expect(waited == pids[0] && status_exited(status, 61),
           "waitpid -pgid reaps group child", status);

    expect(kill(-pgid, SIGKILL) == 0, "waitpid -pgid kill remaining group", pgid);
    for (int i = 0; i < 2; i++) {
        if (pids[i] == waited)
            continue;
        status = -1;
        if (waitpid(pids[i], &status, 0) == pids[i])
            reaped++;
    }

    expect(reaped == 1, "waitpid -pgid reaps remaining child", reaped);
}

static void test_waitpid_wuntraced_continue(void)
{
    int pid;
    int status = -1;
    int waited;

    pid = fork();
    if (pid == 0) {
        while (1)
            usleep(50000);
    }

    if (expect(pid > 0, "WUNTRACED fork child", pid) < 0)
        return;

    setpgid(pid, pid);
    expect(kill(pid, SIGSTOP) == 0, "kill SIGSTOP", pid);
    waited = waitpid(-pid, &status, WUNTRACED);
    expect(waited == pid, "waitpid WUNTRACED reports stopped child", waited);
    expect(WIFSTOPPED(status) && WSTOPSIG(status) == SIGSTOP,
           "waitpid stopped status encodes SIGSTOP", status);

    expect(kill(pid, SIGCONT) == 0, "kill SIGCONT stopped child", pid);
    waited = waitpid(pid, &status, WCONTINUED);
    expect(waited == pid, "waitpid WCONTINUED reports resumed child", waited);
    expect(WIFCONTINUED(status), "waitpid continued status is canonical", status);
    waited = waitpid(pid, &status, WCONTINUED | WNOHANG);
    expect(waited == 0, "waitpid WCONTINUED is one-shot", waited);
    expect(kill(pid, SIGKILL) == 0, "kill SIGKILL continued child", pid);
    waited = waitpid(pid, &status, 0);
    expect(waited == pid, "waitpid reaps continued killed child", waited);
}

static void test_waitpid_group_stop_reports_all(void)
{
    int pids[2] = {-1, -1};
    int pgid = 0;
    int stopped = 0;
    int reaped = 0;
    int status = -1;

    for (int i = 0; i < 2; i++) {
        int pid = fork();
        if (pid == 0) {
            while (1)
                usleep(50000);
        }

        if (pid < 0)
            break;

        if (i == 0)
            pgid = pid;
        pids[i] = pid;
        setpgid(pid, pgid);
    }

    if (expect(pids[0] > 0 && pids[1] > 0, "WUNTRACED group fork children", pids[1]) < 0)
        return;

    expect(kill(-pgid, SIGSTOP) == 0, "WUNTRACED group SIGSTOP", pgid);
    while (stopped < 2) {
        int waited = waitpid(-pgid, &status, WUNTRACED);
        if (waited <= 0)
            break;
        if (WIFSTOPPED(status) && WSTOPSIG(status) == SIGSTOP)
            stopped++;
    }
    expect(stopped == 2, "WUNTRACED group reports all stopped children", stopped);

    expect(kill(-pgid, SIGCONT) == 0, "WUNTRACED group SIGCONT", pgid);
    expect(kill(-pgid, SIGKILL) == 0, "WUNTRACED group cleanup kill", pgid);

    for (int i = 0; i < 2; i++) {
        status = -1;
        if (waitpid(pids[i], &status, 0) == pids[i])
            reaped++;
    }
    expect(reaped == 2, "WUNTRACED group cleanup reap", reaped);
}

static void test_sleep_survives_stop_continue(void)
{
    int pid;
    int status = -1;
    int waited;

    pid = fork();
    if (pid == 0) {
        sleep(8);
        exit(77);
    }

    if (expect(pid > 0, "sleep stop/continue fork child", pid) < 0)
        return;

    setpgid(pid, pid);
    usleep(1000000);
    expect(kill(pid, SIGSTOP) == 0, "sleep stop/continue SIGSTOP", pid);
    waited = waitpid(-pid, &status, WUNTRACED);
    expect(waited == pid && WIFSTOPPED(status), "sleep stop/continue wait stopped", waited);

    expect(kill(pid, SIGCONT) == 0, "sleep stop/continue SIGCONT", pid);
    usleep(500000);
    waited = waitpid(pid, &status, WNOHANG);
    expect(waited == 0, "sleep continues after SIGCONT", waited);

    expect(kill(pid, SIGKILL) == 0, "sleep stop/continue cleanup kill", pid);
    waited = waitpid(pid, &status, 0);
    expect(waited == pid, "sleep stop/continue cleanup reap", waited);
}

static void systest_sleep_signal_handler(int sig)
{
    (void)sig;
    sleep_signal_seen = 1;
}

static int timespec_after(const struct timespec *after,
                          const struct timespec *before)
{
    return after->tv_sec > before->tv_sec ||
        (after->tv_sec == before->tv_sec &&
         after->tv_nsec > before->tv_nsec);
}

static int timespec_is_valid(const struct timespec *value)
{
    return value->tv_sec >= 0 && value->tv_nsec >= 0 &&
        value->tv_nsec < 1000000000L;
}

static void test_posix_clocks_and_capabilities(void)
{
    struct timespec before;
    struct timespec after;
    struct timespec resolution;
    struct timespec delay;
    struct timespec deadline;
    long value;
    int rc;

    expect(sched_yield() == 0, "sched_yield yields successfully", errno);

    if (expect(clock_getres(CLOCK_MONOTONIC, &resolution) == 0 &&
               timespec_is_valid(&resolution) &&
               (resolution.tv_sec > 0 || resolution.tv_nsec > 0),
               "clock_getres monotonic reports resolution", errno) == 0) {
        expect(resolution.tv_sec == 0,
               "monotonic clock has subsecond resolution",
               (int)resolution.tv_sec);
    }

    if (expect(clock_gettime(CLOCK_MONOTONIC, &before) == 0 &&
               timespec_is_valid(&before),
               "clock_gettime monotonic", errno) == 0) {
        delay.tv_sec = 0;
        delay.tv_nsec = 20000000L;
        expect(nanosleep(&delay, NULL) == 0,
               "nanosleep accepts a subsecond duration", errno);
        expect(clock_gettime(CLOCK_MONOTONIC, &after) == 0 &&
               timespec_after(&after, &before),
               "monotonic clock advances", errno);

        delay.tv_sec = 0;
        delay.tv_nsec = 10000000L;
        expect(clock_nanosleep(CLOCK_MONOTONIC, 0, &delay, NULL) == 0,
               "clock_nanosleep relative monotonic", 0);

        expect(clock_gettime(CLOCK_MONOTONIC, &deadline) == 0,
               "clock_nanosleep reads absolute deadline", errno);
        deadline.tv_nsec += 20000000L;
        if (deadline.tv_nsec >= 1000000000L) {
            deadline.tv_sec++;
            deadline.tv_nsec -= 1000000000L;
        }
        expect(clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME,
                               &deadline, NULL) == 0,
               "clock_nanosleep absolute monotonic", 0);
        expect(clock_gettime(CLOCK_MONOTONIC, &after) == 0 &&
               !timespec_after(&deadline, &after),
               "clock_nanosleep reaches absolute deadline", errno);

        expect(clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME,
                               &before, NULL) == 0,
               "clock_nanosleep accepts past deadline", 0);
    }

    if (expect(clock_getres(CLOCK_REALTIME, &resolution) == 0 &&
               timespec_is_valid(&resolution) &&
               resolution.tv_sec == 0 && resolution.tv_nsec > 0 &&
               resolution.tv_nsec <= 1000000L,
               "clock_getres realtime reports millisecond resolution",
               errno) == 0 &&
        expect(clock_gettime(CLOCK_REALTIME, &before) == 0 &&
               timespec_is_valid(&before),
               "clock_gettime realtime", errno) == 0) {
        delay.tv_sec = 0;
        delay.tv_nsec = 20000000L;
        expect(nanosleep(&delay, NULL) == 0,
               "nanosleep advances realtime", errno);
        expect(clock_gettime(CLOCK_REALTIME, &after) == 0 &&
               timespec_is_valid(&after) &&
               timespec_after(&after, &before),
               "realtime clock advances below one second", errno);
    }

    errno = 0;
    expect(clock_gettime((clockid_t)999, &after) < 0 && errno == EINVAL,
           "clock_gettime rejects unknown clock", errno);
    errno = 0;
    expect(clock_gettime(CLOCK_MONOTONIC, NULL) < 0 && errno == EFAULT,
           "clock_gettime rejects null result", errno);

    delay.tv_sec = 0;
    delay.tv_nsec = 1000000L;
    errno = EBUSY;
    rc = clock_nanosleep((clockid_t)999, 0, &delay, NULL);
    expect(rc == EINVAL && errno == EBUSY,
           "clock_nanosleep returns errors without errno", rc);
    rc = clock_nanosleep(CLOCK_MONOTONIC, 2, &delay, NULL);
    expect(rc == EINVAL, "clock_nanosleep rejects unknown flags", rc);
    rc = clock_nanosleep(CLOCK_MONOTONIC, 0, NULL, NULL);
    expect(rc == EFAULT, "clock_nanosleep rejects null request", rc);
    delay.tv_nsec = -1;
    rc = clock_nanosleep(CLOCK_MONOTONIC, 0, &delay, NULL);
    expect(rc == EINVAL, "clock_nanosleep rejects invalid request", rc);

    expect(sysconf(_SC_CLK_TCK) > 0, "sysconf clock ticks", errno);
    expect(sysconf(_SC_PAGESIZE) == 4096, "sysconf page size", errno);
    expect(sysconf(_SC_NPROCESSORS_CONF) >= 1,
           "sysconf configured CPUs", errno);
    expect(sysconf(_SC_NPROCESSORS_ONLN) >= 1,
           "sysconf online CPUs", errno);
    expect(sysconf(_SC_MONOTONIC_CLOCK) > 0,
           "sysconf reports monotonic clock", errno);
    expect(sysconf(_SC_IOV_MAX) == 64, "sysconf IOV_MAX", errno);

    errno = EBUSY;
    value = sysconf(_SC_TIMERS);
    expect(value == -1 && errno == EBUSY,
           "sysconf does not claim POSIX timers", errno);
    errno = ENOTTY;
    value = sysconf(_SC_SHARED_MEMORY_OBJECTS);
    expect(value == -1 && errno == ENOTTY,
           "sysconf does not claim shared memory objects", errno);
    errno = 0;
    value = sysconf(9999);
    expect(value == -1 && errno == EINVAL,
           "sysconf rejects unknown selector", errno);
}

static void test_event_timer_descriptors(void)
{
    struct itimerspec requested;
    struct itimerspec observed;
    struct pollfd pfd;
    eventfd_t value;
    int fd;

    fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (expect(fd >= 0, "eventfd create", errno) == 0) {
        expect((fcntl(fd, F_GETFD) & FD_CLOEXEC) != 0,
               "eventfd close-on-exec", errno);
        errno = 0;
        expect(eventfd_read(fd, &value) < 0 && errno == EAGAIN,
               "eventfd empty non-blocking read", errno);
        expect(eventfd_write(fd, 3) == 0, "eventfd write", errno);
        pfd.fd = fd;
        pfd.events = POLLIN;
        pfd.revents = 0;
        expect(poll(&pfd, 1, 0) == 1 && (pfd.revents & POLLIN),
               "poll detects eventfd counter", pfd.revents);
        expect(eventfd_read(fd, &value) == 0 && value == 3,
               "eventfd read drains counter", (int)value);
        close(fd);
    }

    fd = eventfd(2, EFD_SEMAPHORE | EFD_NONBLOCK);
    if (expect(fd >= 0, "eventfd semaphore create", errno) == 0) {
        expect(eventfd_read(fd, &value) == 0 && value == 1,
               "eventfd semaphore first read", (int)value);
        expect(eventfd_read(fd, &value) == 0 && value == 1,
               "eventfd semaphore second read", (int)value);
        errno = 0;
        expect(eventfd_read(fd, &value) < 0 && errno == EAGAIN,
               "eventfd semaphore drains counter", errno);
        close(fd);
    }

    fd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
    if (expect(fd >= 0, "timerfd create", errno) < 0)
        return;

    memset(&observed, 0xff, sizeof(observed));
    expect(timerfd_gettime(fd, &observed) == 0 &&
           observed.it_value.tv_sec == 0 &&
           observed.it_value.tv_nsec == 0,
           "timerfd initially disarmed", errno);

    memset(&requested, 0, sizeof(requested));
    requested.it_value.tv_nsec = 20000000L;
    expect(timerfd_settime(fd, 0, &requested, NULL) == 0,
           "timerfd arm one-shot", errno);
    errno = 0;
    expect(read(fd, &value, sizeof(value)) < 0 && errno == EAGAIN,
           "timerfd pending non-blocking read", errno);

    pfd.fd = fd;
    pfd.events = POLLIN;
    pfd.revents = 0;
    expect(poll(&pfd, 1, 1000) == 1 && (pfd.revents & POLLIN),
           "poll waits for timerfd", pfd.revents);
    expect(read(fd, &value, sizeof(value)) == (ssize_t)sizeof(value) &&
           value >= 1, "timerfd reports expiration", (int)value);

    requested.it_interval.tv_nsec = 10000000L;
    requested.it_value.tv_nsec = 10000000L;
    expect(timerfd_settime(fd, 0, &requested, NULL) == 0,
           "timerfd arm periodic", errno);
    pfd.revents = 0;
    expect(poll(&pfd, 1, 1000) == 1 && (pfd.revents & POLLIN),
           "poll waits for periodic timerfd", pfd.revents);
    expect(read(fd, &value, sizeof(value)) == (ssize_t)sizeof(value) &&
           value >= 1, "timerfd periodic expiration", (int)value);
    expect(timerfd_gettime(fd, &observed) == 0 &&
           observed.it_interval.tv_nsec == 10000000L,
           "timerfd keeps interval", errno);

    memset(&requested, 0, sizeof(requested));
    expect(timerfd_settime(fd, 0, &requested, NULL) == 0,
           "timerfd disarm", errno);
    close(fd);
}

static void test_nanosleep_signal_interrupt(void)
{
    struct timespec req;
    struct timespec rem;
    int sync_pipe[2];
    int parent_pid;
    int pid;
    int status = -1;
    int rc;
    int waited;
    char token = 'x';

    sleep_signal_seen = 0;
    signal(SIGUSR1, systest_sleep_signal_handler);
    parent_pid = getpid();

    if (expect(pipe(sync_pipe) == 0, "nanosleep interrupt sync pipe", 0) < 0) {
        signal(SIGUSR1, SIG_DFL);
        return;
    }

    pid = fork();
    if (pid == 0) {
        int i;

        close(sync_pipe[1]);
        if (read(sync_pipe[0], &token, 1) != 1)
            exit(1);
        close(sync_pipe[0]);

        for (i = 0; i < 40; i++) {
            usleep(50000);
            kill(parent_pid, SIGUSR1);
        }
        exit(0);
    }

    if (expect(pid > 0, "nanosleep interrupt fork child", pid) < 0) {
        close(sync_pipe[0]);
        close(sync_pipe[1]);
        signal(SIGUSR1, SIG_DFL);
        return;
    }

    close(sync_pipe[0]);
    expect(write(sync_pipe[1], &token, 1) == 1, "nanosleep interrupt arm child", 0);
    close(sync_pipe[1]);

    req.tv_sec = 10;
    req.tv_nsec = 0;
    rem.tv_sec = 0;
    rem.tv_nsec = 0;
    errno = 0;
    rc = nanosleep(&req, &rem);

    expect(rc < 0 && errno == EINTR, "nanosleep interrupted by signal", rc);
    expect(sleep_signal_seen == 1, "nanosleep signal handler ran", sleep_signal_seen);
    expect(rem.tv_sec > 0 || rem.tv_nsec > 0, "nanosleep reports remaining time", (int)rem.tv_sec);
    do {
        waited = waitpid(pid, &status, 0);
    } while (waited < 0 && errno == EINTR);
    expect(waited == pid && status_exited(status, 0),
           "nanosleep signal child reaped", status);
    signal(SIGUSR1, SIG_DFL);
}

static void test_cow_memory(void)
{
    int pid;
    int status = -1;
    int waited;
    unsigned char *heap;
    volatile unsigned char stack_area[8192];

    cow_shared_value = 11;
    pid = fork();
    if (pid == 0) {
        cow_shared_value = 33;
        exit(cow_shared_value == 33 ? 0 : 1);
    }

    if (expect(pid > 0, "COW fork child writer", pid) < 0)
        return;

    waited = waitpid(pid, &status, 0);
    expect(waited == pid && status_exited(status, 0),
           "COW child writes private page", status);
    expect(cow_shared_value == 11, "COW parent value unchanged", cow_shared_value);

    cow_shared_value = 11;
    pid = fork();
    if (pid == 0) {
        usleep(20000);
        exit(cow_shared_value == 11 ? 0 : 1);
    }

    if (expect(pid > 0, "COW fork parent writer", pid) < 0)
        return;

    cow_shared_value = 44;
    waited = waitpid(pid, &status, 0);
    expect(waited == pid && status_exited(status, 0),
           "COW parent writes private page", status);
    expect(cow_shared_value == 44, "COW parent keeps own write", cow_shared_value);

    heap = malloc(8192);
    if (expect(heap != NULL, "COW heap allocation", 0) < 0)
        return;

    heap[0] = 0x11;
    heap[4095] = 0x22;
    heap[4096] = 0x33;
    heap[8191] = 0x44;

    pid = fork();
    if (pid == 0) {
        heap[0] = 0xAA;
        heap[4096] = 0xBB;
        exit(heap[0] == 0xAA && heap[4096] == 0xBB ? 0 : 1);
    }

    if (expect(pid > 0, "COW heap child writer", pid) >= 0) {
        waited = waitpid(pid, &status, 0);
        expect(waited == pid && status_exited(status, 0),
               "COW heap child writes private pages", status);
        expect(heap[0] == 0x11 && heap[4095] == 0x22 &&
               heap[4096] == 0x33 && heap[8191] == 0x44,
               "COW heap parent unchanged", heap[0]);
    }

    heap[0] = 0x11;
    heap[4096] = 0x33;

    pid = fork();
    if (pid == 0) {
        usleep(20000);
        exit(heap[0] == 0x11 && heap[4096] == 0x33 ? 0 : 1);
    }

    if (expect(pid > 0, "COW heap parent writer", pid) >= 0) {
        heap[0] = 0xCC;
        heap[4096] = 0xDD;
        waited = waitpid(pid, &status, 0);
        expect(waited == pid && status_exited(status, 0),
               "COW heap child unchanged", status);
        expect(heap[0] == 0xCC && heap[4096] == 0xDD,
               "COW heap parent keeps own writes", heap[0]);
    }

    free(heap);

    stack_area[0] = 0x12;
    stack_area[4095] = 0x23;
    stack_area[4096] = 0x34;
    stack_area[8191] = 0x45;

    pid = fork();
    if (pid == 0) {
        stack_area[0] = 0xAB;
        stack_area[4096] = 0xBC;
        exit(stack_area[0] == 0xAB && stack_area[4096] == 0xBC ? 0 : 1);
    }

    if (expect(pid > 0, "COW stack child writer", pid) >= 0) {
        waited = waitpid(pid, &status, 0);
        expect(waited == pid && status_exited(status, 0),
               "COW stack child writes private pages", status);
        expect(stack_area[0] == 0x12 && stack_area[4095] == 0x23 &&
               stack_area[4096] == 0x34 && stack_area[8191] == 0x45,
               "COW stack parent unchanged", stack_area[0]);
    }

    stack_area[0] = 0x12;
    stack_area[4096] = 0x34;

    pid = fork();
    if (pid == 0) {
        usleep(20000);
        exit(stack_area[0] == 0x12 && stack_area[4096] == 0x34 ? 0 : 1);
    }

    if (expect(pid > 0, "COW stack parent writer", pid) >= 0) {
        stack_area[0] = 0xCD;
        stack_area[4096] = 0xDE;
        waited = waitpid(pid, &status, 0);
        expect(waited == pid && status_exited(status, 0),
               "COW stack child unchanged", status);
        expect(stack_area[0] == 0xCD && stack_area[4096] == 0xDE,
               "COW stack parent keeps own writes", stack_area[0]);
    }
}

static void test_shared_memory(void)
{
    char name[32];
    int id;
    int pid;
    int status = -1;
    int waited;
    volatile int *shared;

    snprintf(name, sizeof(name), "/systest-shm-%d", getpid());
    shm_unlink(name);
    id = shm_open(name, O_CREAT | O_EXCL | O_RDWR, 0600);
    if (expect(id >= 0, "shm create", id) < 0)
        return;
    if (expect(ftruncate(id, 4096) == 0, "shm initial ftruncate", errno) < 0) {
        close(id);
        shm_unlink(name);
        return;
    }

    shared = mmap(NULL, 4096, PROT_READ | PROT_WRITE,
                  MAP_SHARED, id, 0);
    if (expect(shared != MAP_FAILED, "shm map parent", id) < 0) {
        close(id);
        shm_unlink(name);
        return;
    }
    expect(close(id) == 0, "shm descriptor close after map", id);

    shared[0] = 1234;
    shared[1] = 0;

    pid = fork();
    if (pid == 0) {
        if (shared[0] != 1234)
            exit(2);
        shared[0] = 5678;
        shared[1] = 42;
        exit(0);
    }

    if (expect(pid > 0, "shm fork child", pid) < 0) {
        shm_unmap((void *)shared, 4096);
        shm_unlink(name);
        return;
    }

    waited = waitpid(pid, &status, 0);
    expect(waited == pid && status_exited(status, 0),
           "shm child sees parent data", status);
    expect(shared[0] == 5678 && shared[1] == 42, "shm parent sees child writes", shared[0]);
    expect(shm_unmap((void *)shared, 4096) == 0, "shm unmap parent", 0);
    expect(shm_unlink(name) == 0, "shm unlink", 0);
}

static void test_growable_shared_memory(void)
{
    const size_t initial_size = 16u * 1024u * 1024u;
    const size_t grown_size = 32u * 1024u * 1024u;
    const size_t prefix_size = 2u * 4096u;
    unsigned char *mapping;
    struct stat st;
    char name[40];
    int fd;

    snprintf(name, sizeof(name), "/systest-shm-grow-%d", getpid());
    shm_unlink(name);
    memset(&st, 0, sizeof(st));
    fd = shm_open(name, O_CREAT | O_EXCL | O_RDWR | O_CLOEXEC, 0600);
    if (expect(fd >= 0, "shm create empty growable object", fd) < 0)
        return;
    expect((fcntl(fd, F_GETFD) & FD_CLOEXEC) != 0,
           "POSIX shm_open close-on-exec", errno);

    if (expect(ftruncate(fd, (off_t)initial_size) == 0,
               "shm ftruncate grows object to 16 MiB", errno) < 0)
        goto out;
    expect(fstat(fd, &st) == 0 && st.st_size == (off_t)initial_size,
           "shm fstat reports grown size", (int)st.st_size);

    mapping = mmap(NULL, prefix_size, PROT_READ | PROT_WRITE,
                   MAP_SHARED, fd, 0);
    if (expect(mapping != MAP_FAILED, "shm maps prefix smaller than object",
               errno) == 0) {
        mapping[0] = 0x2a;
        mapping[prefix_size - 1u] = 0x5c;
        expect(ftruncate(fd, (off_t)grown_size) == 0,
               "shm grows to 32 MiB while prefix is mapped", errno);
        expect(mapping[0] == 0x2a && mapping[prefix_size - 1u] == 0x5c,
               "shm growth preserves mapped prefix", mapping[0]);
        expect(munmap(mapping, prefix_size) == 0,
               "shm unmaps prefix", errno);
    }

    mapping = mmap(NULL, grown_size, PROT_READ | PROT_WRITE,
                   MAP_SHARED, fd, 0);
    if (expect(mapping != MAP_FAILED, "shm maps complete 32 MiB pool",
               errno) == 0) {
        mapping[grown_size - 1u] = 0x7d;
        expect(mapping[grown_size - 1u] == 0x7d,
               "shm final pool page is writable",
               mapping[grown_size - 1u]);
        expect(munmap(mapping, grown_size) == 0,
               "shm unmaps complete pool", errno);
    }

    expect(ftruncate(fd, 0) == 0, "shm shrinks after mappings close", errno);

out:
    close(fd);
    expect(shm_unlink(name) == 0, "shm growable object unlink", errno);
}

static int read_self_mem_kb(unsigned *heap_kb, unsigned *rss_kb,
                            unsigned *pf, unsigned *l2_tables)
{
    int self = getpid();

    if (getsysinfo(&sysinfo_scratch) < 0)
        return -1;

    for (int i = 0; i < sysinfo_scratch.proc_count; i++) {
        if (sysinfo_scratch.procs[i].pid == self) {
            if (heap_kb)
                *heap_kb = sysinfo_scratch.procs[i].heap_kb;
            if (rss_kb)
                *rss_kb = sysinfo_scratch.procs[i].rss_kb;
            if (pf)
                *pf = sysinfo_scratch.procs[i].page_faults;
            if (l2_tables)
                *l2_tables = sysinfo_scratch.procs[i].l2_tables;
            return 0;
        }
    }

    return -1;
}

static void touch_all_pages(unsigned char *ptr, size_t size, unsigned char seed)
{
    for (size_t off = 0; off < size; off += 4096)
        ptr[off] = (unsigned char)(seed + (off >> 12));
    ptr[size - 1] = (unsigned char)(seed ^ 0x5a);
}

static int check_all_pages(unsigned char *ptr, size_t size, unsigned char seed)
{
    for (size_t off = 0; off < size; off += 4096) {
        if (ptr[off] != (unsigned char)(seed + (off >> 12)))
            return 0;
    }
    return ptr[size - 1] == (unsigned char)(seed ^ 0x5a);
}

static void test_malloc_free_stress(void)
{
    enum { BLOCKS = 5 };
    static const size_t sizes[BLOCKS] = { 4096, 8192, 32768, 65536, 131072 };
    unsigned char *blocks[BLOCKS];
    unsigned heap_before = 0;
    unsigned heap_after_alloc = 0;
    unsigned heap_after_reuse = 0;
    unsigned heap_after_free = 0;
    unsigned rss_after_alloc = 0;
    unsigned pf_before = 0;
    unsigned pf_after = 0;
    int ok = 1;

    for (int i = 0; i < BLOCKS; i++)
        blocks[i] = NULL;

    expect(read_self_mem_kb(&heap_before, NULL, &pf_before, NULL) == 0,
           "malloc stress baseline stats", 0);

    for (int i = 0; i < BLOCKS; i++) {
        blocks[i] = malloc(sizes[i]);
        if (!blocks[i]) {
            ok = 0;
            break;
        }
        touch_all_pages(blocks[i], sizes[i], (unsigned char)(0x20 + i * 7));
    }

    expect(ok, "malloc stress multi-page allocations", ok);
    if (!ok)
        goto cleanup;

    for (int i = 0; i < BLOCKS; i++) {
        if (!check_all_pages(blocks[i], sizes[i], (unsigned char)(0x20 + i * 7)))
            ok = 0;
    }
    expect(ok, "malloc stress page contents intact", ok);

    expect(read_self_mem_kb(&heap_after_alloc, &rss_after_alloc, &pf_after, NULL) == 0,
           "malloc stress sysinfo after alloc", 0);
    expect(heap_after_alloc >= heap_before + 128,
           "malloc stress heap visible in ps data", (int)(heap_after_alloc - heap_before));
    expect(rss_after_alloc >= heap_after_alloc,
           "malloc stress rss covers heap", (int)rss_after_alloc);
    expect(pf_after >= pf_before,
           "malloc stress page faults stable", (int)(pf_after - pf_before));

    free(blocks[1]);
    blocks[1] = NULL;
    free(blocks[3]);
    blocks[3] = NULL;

    blocks[1] = malloc(12288);
    blocks[3] = malloc(49152);
    ok = blocks[1] && blocks[3];
    if (ok) {
        touch_all_pages(blocks[1], 12288, 0x71);
        touch_all_pages(blocks[3], 49152, 0x91);
        ok = check_all_pages(blocks[1], 12288, 0x71) &&
             check_all_pages(blocks[3], 49152, 0x91);
    }
    expect(ok, "malloc stress free/reuse blocks", ok);

    expect(read_self_mem_kb(&heap_after_reuse, NULL, NULL, NULL) == 0,
           "malloc stress sysinfo after reuse", 0);
    expect(heap_after_reuse >= heap_after_alloc,
           "malloc stress heap stays mapped while blocks live", (int)heap_after_reuse);

cleanup:
    for (int i = 0; i < BLOCKS; i++) {
        if (blocks[i])
            free(blocks[i]);
    }

    if (heap_after_alloc > heap_before &&
        expect(read_self_mem_kb(&heap_after_free, NULL, NULL, NULL) == 0,
               "malloc stress sysinfo after free", 0) == 0) {
        int heap_delta = (int)heap_after_free - (int)heap_before;
        /*
         * The homegrown malloc returns the terminal heap span to brk(), but
         * newlib's allocator may keep arena pages for reuse. For the newlib
         * variant, keeping the heap mapped after free is acceptable as long as
         * the accounting remains readable and the heap did not grow further.
         */
        expect(heap_after_free <= heap_after_reuse,
               "malloc stress terminal free keeps heap bounded",
               heap_delta);
    }
}

static void test_cow_fork_stress(void)
{
    enum { STRESS_ITERS = 64 };
    unsigned char *heap;
    volatile unsigned char stack_area[12288];
    unsigned before_l2 = 0;
    unsigned after_l2 = 0;
    int loop_ok = 1;
    int l2_delta;
    int pid;
    int status;
    int waited;

    heap = malloc(16384);
    if (expect(heap != NULL, "COW stress heap allocation", 0) < 0)
        return;

    heap[0] = 0x10;
    heap[4096] = 0x20;
    heap[8192] = 0x30;
    heap[12288] = 0x40;
    stack_area[0] = 0x50;
    stack_area[4096] = 0x60;
    stack_area[8192] = 0x70;

    if (expect(read_self_mem_kb(NULL, NULL, NULL, &before_l2) == 0,
               "COW stress baseline L2 tables", 0) < 0) {
        free(heap);
        return;
    }

    for (int i = 0; i < STRESS_ITERS; i++) {
        status = -1;
        pid = fork();
        if (pid == 0) {
            heap[0] = (unsigned char)(0x80 + (i & 0x3F));
            heap[8192] = (unsigned char)(0x40 + (i & 0x3F));
            stack_area[0] = (unsigned char)(0x20 + (i & 0x3F));
            stack_area[8192] = (unsigned char)(0x10 + (i & 0x3F));
            exit(heap[0] != 0 && heap[8192] != 0 &&
                 stack_area[0] != 0 && stack_area[8192] != 0 ? 0 : 1);
        }

        if (pid <= 0) {
            loop_ok = 0;
            break;
        }

        heap[4096] = (unsigned char)(0xA0 + (i & 0x1F));
        heap[12288] = (unsigned char)(0xB0 + (i & 0x1F));
        stack_area[4096] = (unsigned char)(0xC0 + (i & 0x1F));

        waited = waitpid(pid, &status, 0);
        if (waited != pid || !status_exited(status, 0)) {
            loop_ok = 0;
            break;
        }
    }

    expect(loop_ok, "COW stress fork/write/wait loop", loop_ok);
    expect(read_self_mem_kb(NULL, NULL, NULL, &after_l2) == 0,
           "COW stress final L2 tables", 0);
    l2_delta = after_l2 > before_l2 ? (int)(after_l2 - before_l2) : 0;
    expect(l2_delta <= 1, "COW stress parent L2 stable", l2_delta);

    free(heap);
}

static void test_asid_churn(void)
{
    enum { ASID_ITERS = 280 };
    int loop_ok = 1;
    int pid;
    int status;
    int waited;

    for (int i = 0; i < ASID_ITERS; i++) {
        status = -1;
        pid = fork();
        if (pid == 0)
            exit(i & 0x7f);

        if (pid <= 0) {
            loop_ok = 0;
            break;
        }

        waited = waitpid(pid, &status, 0);
        if (waited != pid || !status_exited(status, i & 0x7f)) {
            loop_ok = 0;
            break;
        }
    }

    expect(loop_ok, "ASID churn over 255 forks", loop_ok);
}

static void test_asid_live_saturation(void)
{
    enum { LIVE_LIMIT = 270 };
    int pids[LIVE_LIMIT];
    int count = 0;
    int reaped = 0;
    int pid;
    int status;

    if (!stdin_is_foreground_tty()) {
        skip("ASID live saturation (not foreground job)");
        return;
    }

    for (int i = 0; i < LIVE_LIMIT; i++)
        pids[i] = -1;

    for (int i = 0; i < LIVE_LIMIT; i++) {
        pid = fork();
        if (pid == 0) {
            while (1)
                usleep(50000);
        }

        if (pid < 0)
            break;

        pids[count++] = pid;
    }

    expect(count == LIVE_LIMIT, "ASID generation rollover creates full live set", count);

    for (int i = 0; i < count; i++)
        kill(pids[i], SIGKILL);

    for (int i = 0; i < count; i++) {
        status = -1;
        if (waitpid(pids[i], &status, 0) == pids[i])
            reaped++;
    }

    expect(reaped == count, "ASID live saturation reaps children", reaped);
}

static void test_lifecycle_exec_fail_loop(void)
{
    enum { ITERS = 32 };
    int ok = 1;

    for (int i = 0; i < ITERS; i++) {
        int status = -1;
        int pid = fork();

        if (pid == 0) {
            char *argv[] = { "missing-command", NULL };
            execve("/bin/missing-command", argv, NULL);
            exit(90 + (i & 7));
        }

        if (pid <= 0) {
            ok = 0;
            break;
        }

        if (waitpid(pid, &status, 0) != pid ||
            !status_exited(status, 90 + (i & 7))) {
            ok = 0;
            break;
        }
    }

    expect(ok, "lifecycle repeated exec failure is reaped", ok);
}

static void test_lifecycle_exec_redirect_loop(void)
{
    enum { ITERS = 96 };
    char output_path[160];
    int ok = 1;

    snprintf(output_path, sizeof(output_path), "%s/exec-loop.out",
             systest_tmp_root);

    for (int i = 0; i < ITERS; i++) {
        int status = -1;
        int pid = fork();

        if (pid == 0) {
            char *argv[] = { "true", NULL };
            char *envp[] = { "PATH=/bin:/usr/bin", NULL };
            int fd = open(output_path, O_WRONLY | O_CREAT | O_TRUNC, 0600);

            if (fd < 0 || dup2(fd, STDOUT_FILENO) < 0 ||
                dup2(fd, STDERR_FILENO) < 0)
                _exit(122);
            if (fd > STDERR_FILENO)
                close(fd);
            execve("/bin/true", argv, envp);
            _exit(123);
        }

        if (pid <= 0 || waitpid(pid, &status, 0) != pid ||
            !status_exited(status, 0)) {
            ok = 0;
            break;
        }
    }

    unlink(output_path);
    expect(ok, "lifecycle repeated redirected exec is scheduled", ok);
}

static void test_direct_spawn(void)
{
    char *argv[] = { "true", NULL };
    char *envp[] = { "PATH=/bin:/usr/bin", NULL };
    armos_spawn_attributes_t attributes = {
        .abi_version = ARMOS_SPAWN_ABI_VERSION,
        .flags = ARMOS_SPAWN_SET_CWD,
        .cwd = "/"
    };
    int status = -1;
    pid_t pid;

    pid = armos_spawnve("/bin/true", argv, envp, &attributes);
    if (expect(pid > 0, "direct spawn publishes complete child", pid) == 0)
        expect(waitpid(pid, &status, 0) == pid && status_exited(status, 0),
               "direct spawn child exits and is reaped", status);

    errno = 0;
    pid = armos_spawnve("/bin/missing-spawn-target", argv, envp,
                        &attributes);
    expect(pid < 0 && errno == ENOENT,
           "failed direct spawn leaves no child", errno);
}

static void test_exec_argument_limit(void)
{
    const char *arg0 = "true";
    const size_t vector_bytes = 5u * sizeof(char *);
    const size_t exact_length = ARG_MAX - vector_bytes - strlen(arg0) - 2u;
    char *large = malloc(exact_length + 2u);
    char *argv[] = { (char *)arg0, large, NULL };
    char *envp[] = { NULL };
    int status = -1;
    pid_t pid;

    if (expect(large != NULL, "allocate ARG_MAX exec payload", errno) < 0)
        return;
    memset(large, 'x', exact_length + 1u);
    large[exact_length] = '\0';

    pid = fork();
    if (pid == 0) {
        execve("/bin/true", argv, envp);
        _exit(120);
    }
    if (expect(pid > 0, "fork exact ARG_MAX exec", pid) == 0) {
        expect(waitpid(pid, &status, 0) == pid && status_exited(status, 0),
               "exec accepts exact ARG_MAX payload", status);
    }

    large[exact_length] = 'y';
    large[exact_length + 1u] = '\0';
    pid = fork();
    if (pid == 0) {
        execve("/bin/true", argv, envp);
        _exit(errno == E2BIG ? 0 : 121);
    }
    if (expect(pid > 0, "fork oversized ARG_MAX exec", pid) == 0) {
        expect(waitpid(pid, &status, 0) == pid && status_exited(status, 0),
               "exec rejects payload above ARG_MAX with E2BIG", status);
    }

    free(large);
}

static void test_lifecycle_wnohang_kill(void)
{
    int status = -1;
    int pid = fork();

    if (pid == 0) {
        while (1)
            usleep(50000);
    }

    if (expect(pid > 0, "lifecycle fork live child", pid) < 0)
        return;

    expect(waitpid(pid, &status, WNOHANG) == 0, "waitpid WNOHANG leaves live child", status);
    expect(kill(pid, SIGKILL) == 0, "lifecycle kill live child", pid);
    expect(waitpid(pid, &status, 0) == pid && status_signaled(status, SIGKILL),
           "lifecycle reap killed child", status);
}

static void test_lifecycle_orphan_reaper(void)
{
    enum { ITERS = 8 };
    int ok = 1;

    for (int i = 0; i < ITERS; i++) {
        int status = -1;
        int pid = fork();

        if (pid == 0) {
            int grandchild = fork();
            if (grandchild == 0) {
                usleep(20000);
                exit(40 + i);
            }
            exit(grandchild > 0 ? 30 + i : 1);
        }

        if (pid <= 0) {
            ok = 0;
            break;
        }

        if (waitpid(pid, &status, 0) != pid || !status_exited(status, 30 + i)) {
            ok = 0;
            break;
        }
    }

    usleep(200000);
    expect(ok, "lifecycle orphan children delegated to init", ok);
}

static void test_identity(void)
{
    int uid = getuid();
    int gid = getgid();

    expect(uid == 0 || uid == 1000, "process uid is root or user", uid);
    expect(gid == 0 || gid == 1000, "process gid is root or user", gid);
}

static void test_terminal_process_group(void)
{
    int old_pgrp = tcgetpgrp(STDIN_FILENO);
    int self_pgrp = getpgrp();
    int current;

    if (old_pgrp < 0 && errno == ENOTTY) {
        skip("tcsetpgrp foreground mutation (stdin is not tty)");
        return;
    }

    if (expect(old_pgrp >= 0, "tcgetpgrp returns foreground group", old_pgrp) < 0)
        return;

    if (expect(self_pgrp > 0, "getpgrp returns process group", self_pgrp) < 0)
        return;

    if (old_pgrp != self_pgrp) {
        skip("tcsetpgrp foreground mutation (not foreground job)");
        errno = 0;
        expect(tcgetpgrp(99) < 0 && errno == EBADF, "tcgetpgrp rejects closed fd", errno);
        errno = 0;
        expect(tcsetpgrp(99, self_pgrp) < 0 && errno == EBADF,
               "tcsetpgrp rejects closed fd", errno);
        return;
    }

    expect(tcsetpgrp(STDIN_FILENO, self_pgrp) == 0, "tcsetpgrp sets foreground group", self_pgrp);
    current = tcgetpgrp(STDIN_FILENO);
    expect(current == self_pgrp, "tcgetpgrp observes tcsetpgrp", current);

    errno = 0;
    expect(tcgetpgrp(99) < 0 && errno == EBADF, "tcgetpgrp rejects closed fd", errno);
    errno = 0;
    expect(tcsetpgrp(99, self_pgrp) < 0 && errno == EBADF, "tcsetpgrp rejects closed fd", errno);

    if (old_pgrp >= 0)
        tcsetpgrp(STDIN_FILENO, old_pgrp);
}

static void test_background_tty_read_stops(void)
{
    int old_pgrp = tcgetpgrp(STDIN_FILENO);
    int self_pgrp = getpgrp();
    int sync_pipe[2];
    int status = 0;
    char sync = 0;
    pid_t pid;
    pid_t waited;

    if (old_pgrp < 0 && errno == ENOTTY) {
        skip("background tty read stop (stdin is not tty)");
        return;
    }

    if (old_pgrp != self_pgrp) {
        skip("background tty read stop (not foreground job)");
        return;
    }

    if (expect(pipe(sync_pipe) == 0, "background tty read sync pipe", errno) < 0)
        return;

    pid = fork();
    if (pid == 0) {
        char c;

        close(sync_pipe[0]);
        setpgid(0, 0);
        (void)write(sync_pipe[1], "r", 1);
        close(sync_pipe[1]);
        (void)read(STDIN_FILENO, &c, 1);
        exit(77);
    }

    if (expect(pid > 0, "background tty read fork child", pid) < 0) {
        close(sync_pipe[0]);
        close(sync_pipe[1]);
        return;
    }

    close(sync_pipe[1]);
    setpgid(pid, pid);
    expect(read(sync_pipe[0], &sync, 1) == 1 && sync == 'r',
           "background tty read child reached read point", sync);
    close(sync_pipe[0]);

    waited = 0;
    for (int i = 0; i < 100; i++) {
        waited = waitpid(pid, &status, WUNTRACED | WNOHANG);
        if (waited == pid)
            break;
        usleep(10000);
    }
    expect(waited == pid, "background tty read reports stopped child", waited);
    expect(WIFSTOPPED(status) && WSTOPSIG(status) == SIGTTIN,
           "background tty read stops with SIGTTIN", status);

    kill(-pid, SIGKILL);
    waitpid(pid, &status, 0);
}

static void test_process_session_info(void)
{
    int self = getpid();
    int found = 0;

    if (expect(getsysinfo(&sysinfo_scratch) >= 0, "sysinfo session query", 0) < 0)
        return;

    for (int i = 0; i < sysinfo_scratch.proc_count; i++) {
        if (sysinfo_scratch.procs[i].pid == self) {
            found = 1;
            expect(sysinfo_scratch.procs[i].sid > 0, "sysinfo reports process sid",
                   sysinfo_scratch.procs[i].sid);
            expect(sysinfo_scratch.procs[i].tty >= 0, "sysinfo reports controlling tty",
                   sysinfo_scratch.procs[i].tty);
            break;
        }
    }

    expect(found, "sysinfo contains current process", self);
}

static void test_scheduler_priority_syscalls(void)
{
    int base;
    int target;
    int status = 0;
    int waited;
    int pid;

    errno = 0;
    base = getpriority(PRIO_PROCESS, 0);
    expect(!(base == -1 && errno != 0), "getpriority current process", errno);
    expect(base >= -20 && base <= 19, "getpriority reports nice range", base);

    pid = fork();
    if (pid == 0) {
        int before = getpriority(PRIO_PROCESS, 0);
        int expected = before + 3;
        int after;
        int rc;

        if (expected > 19)
            expected = 19;

        rc = nice(3);
        after = getpriority(PRIO_PROCESS, 0);
        exit(rc == expected && after == expected ? 0 : 1);
    }
    if (expect(pid > 0, "nice fork child", pid) < 0)
        return;

    waited = waitpid(pid, &status, 0);
    expect(waited == pid && status_exited(status, 0),
           "nice adjusts child priority", status);

    pid = fork();
    if (pid == 0) {
        while (1)
            sleep(1);
    }
    if (expect(pid > 0, "setpriority fork child", pid) < 0)
        return;

    target = base + 4;
    if (target > 19)
        target = 19;

    expect(setpriority(PRIO_PROCESS, pid, target) == 0,
           "setpriority child lower priority", target);
    expect(getpriority(PRIO_PROCESS, pid) == target,
           "getpriority observes child priority", target);

    if (getuid() == 0) {
        int root_target = target > -20 ? target - 1 : target;
        expect(setpriority(PRIO_PROCESS, pid, root_target) == 0,
               "root can improve child priority", root_target);
        expect(getpriority(PRIO_PROCESS, pid) == root_target,
               "root priority change visible", root_target);
    } else if (target > -20) {
        errno = 0;
        expect(setpriority(PRIO_PROCESS, pid, target - 1) < 0 && errno == EPERM,
               "user cannot improve child priority", errno);
    } else {
        skip("user priority improvement test at minimum nice");
    }

    kill(pid, SIGKILL);
    waitpid(pid, &status, 0);
}

static void test_resource_limit_syscalls(void)
{
    struct rlimit saved;
    struct rlimit limited;
    struct rlimit observed;
    int fds[16];
    int opened = 0;
    int status;
    pid_t pid;
    int fd;

    if (expect(getrlimit(RLIMIT_NOFILE, &saved) == 0,
               "getrlimit RLIMIT_NOFILE", errno) < 0)
        return;
    expect(saved.rlim_cur >= 8 && saved.rlim_max >= saved.rlim_cur,
           "getrlimit reports usable descriptor limits", (int)saved.rlim_cur);

    limited.rlim_cur = 8;
    limited.rlim_max = saved.rlim_max;
    if (expect(setrlimit(RLIMIT_NOFILE, &limited) == 0,
               "setrlimit lowers RLIMIT_NOFILE", errno) < 0)
        return;
    if (expect(getrlimit(RLIMIT_NOFILE, &observed) == 0,
               "getrlimit observes lowered limit", errno) == 0)
        expect(observed.rlim_cur == 8 && observed.rlim_max == saved.rlim_max,
               "RLIMIT_NOFILE values persist", (int)observed.rlim_cur);
    expect(sysconf(_SC_OPEN_MAX) == 8,
           "sysconf OPEN_MAX follows soft limit", (int)sysconf(_SC_OPEN_MAX));

    errno = 0;
    while (opened < (int)(sizeof(fds) / sizeof(fds[0]))) {
        fd = open("/dev/null", O_RDONLY);
        if (fd < 0)
            break;
        fds[opened++] = fd;
    }
    expect(fd < 0 && errno == EMFILE,
           "RLIMIT_NOFILE enforces descriptor allocation", errno);
    errno = 0;
    expect(dup2(STDIN_FILENO, 8) < 0 && errno == EBADF,
           "dup2 rejects descriptor at soft limit", errno);
    while (opened > 0)
        close(fds[--opened]);

    pid = fork();
    if (pid == 0) {
        struct rlimit child_limit;
        _exit(getrlimit(RLIMIT_NOFILE, &child_limit) == 0 &&
              child_limit.rlim_cur == 8 ? 0 : 1);
    }
    if (expect(pid > 0, "RLIMIT_NOFILE fork child", pid) == 0) {
        waitpid(pid, &status, 0);
        expect(status_exited(status, 0),
               "fork inherits RLIMIT_NOFILE", status);
    }

    limited.rlim_cur = saved.rlim_max + 1;
    limited.rlim_max = saved.rlim_max;
    errno = 0;
    expect(setrlimit(RLIMIT_NOFILE, &limited) < 0 && errno == EINVAL,
           "setrlimit rejects soft limit above hard limit", errno);
    errno = 0;
    expect(setrlimit(RLIMIT_CORE, &saved) < 0 && errno == EINVAL,
           "setrlimit rejects unenforced resources", errno);

    expect(setrlimit(RLIMIT_NOFILE, &saved) == 0,
           "setrlimit restores RLIMIT_NOFILE", errno);
}

static void usage(void)
{
    printf("usage: systest [-q|-v]\n");
}

int main(int argc, char **argv)
{
    int foreground = stdin_is_foreground_tty();

    verbose = foreground ? 1 : 0;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-q") == 0 || strcmp(argv[i], "--quiet") == 0) {
            verbose = 0;
        } else if (strcmp(argv[i], "-v") == 0 || strcmp(argv[i], "--verbose") == 0) {
            verbose = 1;
        } else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            usage();
            exit(0);
        } else {
            usage();
            exit(1);
        }
    }

    if (verbose)
        printf("=== syscall smoke tests ===\n");

    snprintf(systest_tmp_root, sizeof(systest_tmp_root), "/tmp/systest-%d", getpid());
    remove_tree_local(systest_tmp_root);
    mkdir(systest_tmp_root, 0755);

    test_identity();
    test_terminal_process_group();
    test_background_tty_read_stops();
    test_process_session_info();
    test_scheduler_priority_syscalls();
    test_resource_limit_syscalls();
    test_file_io();
    test_positioned_io();
    test_directory_relative_io();
    test_access_umask();
    test_open_permission_enforcement();
    test_pipe_dup2();
    test_named_fifo();
    test_fd_access_modes();
    test_stat_syscalls();
    test_statvfs_syscalls();
    test_dev_null();
    test_dev_tty();
    test_chmod_chown_syscalls();
    test_utimens_syscalls();
    test_proc_net_syscalls();
    test_posix_compat_syscalls();
    test_ext2_links_and_dirents();
    test_ext2_write_edges();
    test_rm_recursive_utility();
    test_fork_wait_kill();
    test_sigchld_discard_reaps_children();
    test_process_groups();
    test_waitpid_process_group();
    test_waitpid_wuntraced_continue();
    test_waitpid_group_stop_reports_all();
    test_sleep_survives_stop_continue();
    test_posix_clocks_and_capabilities();
    test_event_timer_descriptors();
    test_nanosleep_signal_interrupt();
    test_malloc_free_stress();
    test_cow_memory();
    test_shared_memory();
    test_growable_shared_memory();
    test_cow_fork_stress();
    test_asid_churn();
    test_asid_live_saturation();
    test_direct_spawn();
    test_exec_argument_limit();
    test_lifecycle_exec_fail_loop();
    test_lifecycle_exec_redirect_loop();
    test_lifecycle_wnohang_kill();
    test_lifecycle_orphan_reaper();

    if (failures == 0) {
        remove_tree_local(systest_tmp_root);
        printf("pid %d systest: all tests passed\n", getpid());
        exit(0);
    }

    remove_tree_local(systest_tmp_root);
    printf("pid %d systest: %d failure(s)\n", getpid(), failures);
    exit(1);
}
