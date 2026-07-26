/*
 * ArmOS
 * Copyright (c) 2026 Mohamed Ennassiri
 *
 * Licensed under the Apache License, Version 2.0.
 * See LICENSE for details.
 *
 * File: userland/programs/ptytest/ptytest.c
 * Layer: Userland / system validation
 *
 * Responsibilities:
 * - Validate POSIX PTY allocation and slave discovery.
 * - Exercise bidirectional master/slave data transfer.
 * - Check poll, termios and window-size operations.
 */

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

static int fail(const char *operation)
{
    fprintf(stderr, "PTYTEST_FAILED %s errno=%d\n", operation, errno);
    return 1;
}

static int test_child_session(int master, const char *slave_name)
{
    struct pollfd descriptor = {master, POLLIN, 0};
    char buffer[32];
    int status;
    pid_t child;
    ssize_t count;

    child = fork();
    if (child < 0)
        return fail("fork");
    if (child == 0) {
        int slave;

        close(master);
        if (setsid() < 0)
            _exit(10);
        slave = open(slave_name, O_RDWR);
        if (slave < 0 || ioctl(slave, TIOCSCTTY, 0) < 0)
            _exit(11);
        if (dup2(slave, STDIN_FILENO) < 0 ||
            dup2(slave, STDOUT_FILENO) < 0 ||
            dup2(slave, STDERR_FILENO) < 0)
            _exit(12);
        if (slave > STDERR_FILENO)
            close(slave);
        if (write(STDOUT_FILENO, "child\n", 6u) != 6)
            _exit(13);
        _exit(0);
    }

    if (poll(&descriptor, 1u, 1000) != 1 ||
        (descriptor.revents & POLLIN) == 0)
        return fail("poll-child");
    count = read(master, buffer, sizeof(buffer));
    if (count != 7 || memcmp(buffer, "child\r\n", 7u) != 0)
        return fail("child-output");
    if (waitpid(child, &status, 0) != child ||
        !WIFEXITED(status) || WEXITSTATUS(status) != 0)
        return fail("wait-child");
    return 0;
}

int main(void)
{
    struct pollfd descriptor;
    struct winsize size = {30u, 100u, 800u, 600u};
    struct winsize observed;
    char buffer[32];
    char *slave_name;
    int master;
    int slave;
    ssize_t count;

    master = posix_openpt(O_RDWR | O_NOCTTY);
    if (master < 0)
        return fail("posix_openpt");
    if (grantpt(master) < 0 || unlockpt(master) < 0)
        return fail("grant-unlock");
    slave_name = ptsname(master);
    if (!slave_name)
        return fail("ptsname");
    slave = open(slave_name, O_RDWR | O_NOCTTY);
    if (slave < 0)
        return fail("open-slave");
    if (!isatty(master) || !isatty(slave))
        return fail("isatty");
    if (ioctl(master, TIOCSWINSZ, &size) < 0 ||
        ioctl(slave, TIOCGWINSZ, &observed) < 0 ||
        memcmp(&size, &observed, sizeof(size)) != 0)
        return fail("winsize");

    if (write(master, "input\n", 6u) != 6)
        return fail("master-write");
    count = read(slave, buffer, sizeof(buffer));
    if (count != 6 || memcmp(buffer, "input\n", 6u) != 0)
        return fail("slave-read");

    if (write(slave, "output\n", 7u) != 7)
        return fail("slave-write");
    descriptor.fd = master;
    descriptor.events = POLLIN;
    descriptor.revents = 0;
    if (poll(&descriptor, 1u, 1000) != 1 ||
        (descriptor.revents & POLLIN) == 0)
        return fail("poll-master");
    count = read(master, buffer, sizeof(buffer));
    if (count != 8 || memcmp(buffer, "output\r\n", 8u) != 0)
        return fail("master-read");

    close(slave);
    if (test_child_session(master, slave_name) != 0)
        return 1;
    close(master);
    printf("PTYTEST_OK master slave poll termios winsize session\n");
    return 0;
}
