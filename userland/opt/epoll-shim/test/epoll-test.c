/*
 * ArmOS
 * Copyright (c) 2026 Mohamed Ennassiri
 *
 * Licensed under the Apache License, Version 2.0.
 * See LICENSE for details.
 *
 * File: userland/opt/epoll-shim/test/epoll-test.c
 * Layer: Userland / compatibility tests
 *
 * Responsibilities:
 * - Validate level-triggered epoll control and readiness operations.
 * - Exercise eventfd and timerfd through the epoll compatibility layer.
 */

#include <sys/epoll.h>

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/eventfd.h>
#include <sys/timerfd.h>
#include <time.h>
#include <unistd.h>

static int test_failure(const char *operation)
{
    printf("epoll-test: FAIL: %s: %s\n", operation, strerror(errno));
    return 1;
}

int main(void)
{
    const uint64_t event_tag = 0x4556454e544644ULL;
    const uint64_t modified_tag = 0x4d4f444946494544ULL;
    const uint64_t timer_tag = 0x54494d45524644ULL;
    struct epoll_event event;
    struct epoll_event ready[2];
    struct itimerspec timer_value;
    eventfd_t counter;
    int epfd;
    int event_fd;
    int timer_fd;
    int result;

    epfd = epoll_create1(EPOLL_CLOEXEC);
    if (epfd < 0)
        return test_failure("epoll_create1");
    event_fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (event_fd < 0)
        return test_failure("eventfd");

    event.events = EPOLLIN;
    event.data.u64 = event_tag;
    if (epoll_ctl(epfd, EPOLL_CTL_ADD, event_fd, &event) < 0)
        return test_failure("EPOLL_CTL_ADD eventfd");
    if (epoll_wait(epfd, ready, 2, 0) != 0) {
        errno = EIO;
        return test_failure("empty epoll_wait");
    }
    if (epoll_ctl(epfd, EPOLL_CTL_ADD, event_fd, &event) == 0 ||
        errno != EEXIST) {
        errno = EIO;
        return test_failure("duplicate EPOLL_CTL_ADD");
    }

    if (eventfd_write(event_fd, 1) < 0)
        return test_failure("eventfd_write");
    result = epoll_wait(epfd, ready, 2, 100);
    if (result != 1 || (ready[0].events & EPOLLIN) == 0 ||
        ready[0].data.u64 != event_tag) {
        errno = EIO;
        return test_failure("eventfd readiness");
    }
    if (eventfd_read(event_fd, &counter) < 0 || counter != 1)
        return test_failure("eventfd_read");

    event.events = EPOLLIN;
    event.data.u64 = modified_tag;
    if (epoll_ctl(epfd, EPOLL_CTL_MOD, event_fd, &event) < 0)
        return test_failure("EPOLL_CTL_MOD eventfd");
    if (eventfd_write(event_fd, 1) < 0)
        return test_failure("second eventfd_write");
    result = epoll_pwait(epfd, ready, 2, 100, NULL);
    if (result != 1 || ready[0].data.u64 != modified_tag) {
        errno = EIO;
        return test_failure("modified event data");
    }
    if (eventfd_read(event_fd, &counter) < 0)
        return test_failure("second eventfd_read");

    event.events = EPOLLOUT;
    event.data.u64 = modified_tag;
    if (epoll_ctl(epfd, EPOLL_CTL_MOD, event_fd, &event) < 0)
        return test_failure("EPOLL_CTL_MOD writable eventfd");
    result = epoll_wait(epfd, ready, 2, 0);
    if (result != 1 || (ready[0].events & EPOLLOUT) == 0 ||
        ready[0].data.u64 != modified_tag) {
        errno = EIO;
        return test_failure("eventfd write readiness");
    }
    if (epoll_ctl(epfd, EPOLL_CTL_DEL, event_fd, NULL) < 0)
        return test_failure("EPOLL_CTL_DEL eventfd");

    timer_fd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
    if (timer_fd < 0)
        return test_failure("timerfd_create");
    event.events = EPOLLIN;
    event.data.u64 = timer_tag;
    if (epoll_ctl(epfd, EPOLL_CTL_ADD, timer_fd, &event) < 0)
        return test_failure("EPOLL_CTL_ADD timerfd");

    memset(&timer_value, 0, sizeof(timer_value));
    timer_value.it_value.tv_nsec = 10000000L;
    if (timerfd_settime(timer_fd, 0, &timer_value, NULL) < 0)
        return test_failure("timerfd_settime");
    result = epoll_wait(epfd, ready, 2, 250);
    if (result != 1 || (ready[0].events & EPOLLIN) == 0 ||
        ready[0].data.u64 != timer_tag) {
        errno = EIO;
        return test_failure("timerfd readiness");
    }
    if (read(timer_fd, &counter, sizeof(counter)) != (ssize_t)sizeof(counter))
        return test_failure("timerfd read");

    if (epoll_ctl(epfd, EPOLL_CTL_DEL, timer_fd, NULL) < 0)
        return test_failure("EPOLL_CTL_DEL timerfd");
    if (epoll_ctl(epfd, EPOLL_CTL_DEL, timer_fd, NULL) == 0 ||
        errno != ENOENT) {
        errno = EIO;
        return test_failure("missing EPOLL_CTL_DEL");
    }

    close(timer_fd);
    close(event_fd);
    close(epfd);
    printf("epoll-test: PASS\n");
    return 0;
}
