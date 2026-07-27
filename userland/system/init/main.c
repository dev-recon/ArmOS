/*
 * ArmOS
 * Copyright (c) 2026 Mohamed Ennassiri
 *
 * Licensed under the Apache License, Version 2.0.
 * See LICENSE for details.
 *
 * File: userland/system/init/main.c
 * Layer: Userland / system service
 * Description: System-level userspace component for ArmOS.
 */

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#ifndef SIGTTIN
#define SIGTTIN 21
#endif
#ifndef SIGTTOU
#define SIGTTOU 22
#endif

static char *const init_envp[] = {
    "PATH=/bin:/usr/bin:/opt/kilo/bin:/opt/nano/bin",
    "HOME=/home/user",
    "USER=user",
    "LANG=C.UTF-8",
    "SHELL=/sbin/mash",
    "PS1=mash$> ",
    NULL
};

static volatile sig_atomic_t init_shutting_down = 0;
static volatile sig_atomic_t init_term_sent = 0;
static int shell_pid = -1;
static int graphics_shell_pid = -1;

static void init_write_all(const char *s)
{
    size_t len;

    if (!s)
        return;

    len = strlen(s);
    while (len > 0) {
        ssize_t written = write(STDERR_FILENO, s, len);
        if (written <= 0)
            return;
        s += written;
        len -= (size_t)written;
    }
}

static void init_log_pid(const char *prefix, int pid)
{
    char buf[96];

    snprintf(buf, sizeof(buf), "%s%d\n", prefix, pid);
    init_write_all(buf);
}

static void on_shutdown_signal(int sig)
{
    (void)sig;
    init_shutting_down = 1;
}

static void request_shell_shutdown(void)
{
    if (init_term_sent)
        return;

    init_term_sent = 1;
    init_write_all("init: shutdown requested, stopping login shells\n");

    if (shell_pid > 0)
        kill(shell_pid, SIGTERM);
    if (graphics_shell_pid > 0)
        kill(graphics_shell_pid, SIGTERM);
}

static int attach_stdio_fd(int fd)
{
    if (fd < 0)
        return -1;

    dup2(fd, STDIN_FILENO);
    dup2(fd, STDOUT_FILENO);
    dup2(fd, STDERR_FILENO);
    if (fd > STDERR_FILENO)
        close(fd);
    return 0;
}

static int spawn_user_shell(const char *tty_path, int optional)
{
    char *const argv[] = { "mash", NULL };
    int tty_fd;
    int pid;

    tty_fd = open(tty_path, O_RDWR, 0);
    if (tty_fd < 0) {
        if (!optional)
            fprintf(stderr, "init: cannot open %s\n", tty_path);
        return optional ? 0 : -1;
    }

    pid = fork();
    if (pid < 0) {
        perror("init: fork");
        close(tty_fd);
        return -1;
    }

    if (pid == 0) {
        if (attach_stdio_fd(tty_fd) < 0)
            _exit(127);
        chdir("/home/user");
        if (setgid(1000) < 0)
            perror("init: setgid");
        if (setuid(1000) < 0)
            perror("init: setuid");
        execve("/sbin/mash", argv, init_envp);
        perror("init: exec /sbin/mash");
        _exit(127);
    }

    close(tty_fd);
    return pid;
}

int main(void)
{
    int waited;
    int status;

    signal(SIGTERM, on_shutdown_signal);
    signal(SIGINT, SIG_IGN);
    signal(SIGTSTP, SIG_IGN);
    signal(SIGTTIN, SIG_IGN);
    signal(SIGTTOU, SIG_IGN);
    chdir("/");

    graphics_shell_pid = spawn_user_shell("/dev/tty1", 1);

    while (1) {
        if (init_shutting_down)
            request_shell_shutdown();

        if (!init_shutting_down && shell_pid < 0) {
            shell_pid = spawn_user_shell("/dev/tty0", 0);
            if (shell_pid > 0)
                init_log_pid("init: tty0 shell pid ", shell_pid);
        }

        if (!init_shutting_down && graphics_shell_pid < 0)
            graphics_shell_pid = spawn_user_shell("/dev/tty1", 1);

        status = 0;
        waited = waitpid(-1, &status, 0);

        if (waited < 0) {
            if (errno == EINTR)
                continue;
            if (init_shutting_down && errno == ECHILD) {
                sleep(1);
                continue;
            }
            perror("init: waitpid");
            sleep(1);
            continue;
        }

        if (waited == shell_pid) {
            if (init_shutting_down)
                init_write_all("init: tty0 shell stopped for shutdown\n");
            else
                init_write_all("init: tty0 shell exited, restarting\n");
            shell_pid = -1;
        } else if (waited == graphics_shell_pid) {
            if (init_shutting_down)
                init_write_all("init: tty1 shell stopped for shutdown\n");
            else
                init_write_all("init: tty1 shell exited, restarting\n");
            graphics_shell_pid = -1;
        }

        /*
         * Other children are orphans adopted by PID 1. Reaping them here keeps
         * the system clean without coupling tty0 and tty1 lifetimes.
         */
    }
}
