/*
 * ArmOS
 * Copyright (c) 2026 Mohamed Ennassiri
 *
 * Licensed under the Apache License, Version 2.0.
 * See LICENSE for details.
 *
 * File: userland/opt/epoll-shim/src/sys/epoll.h
 * Layer: Userland / descriptor compatibility
 *
 * Responsibilities:
 * - Declare the epoll-compatible event-loop interface used by ported software.
 * - Publish source-compatible event and control constants.
 *
 * Notes:
 * - This is an ArmOS userland compatibility API, not a Linux kernel ABI.
 * - Level-triggered descriptor readiness is implemented on top of ppoll(2).
 */

#ifndef ARMOS_SYS_EPOLL_H
#define ARMOS_SYS_EPOLL_H

#include <fcntl.h>
#include <signal.h>
#include <stdint.h>

typedef union epoll_data {
    void *ptr;
    int fd;
    uint32_t u32;
    uint64_t u64;
} epoll_data_t;

struct epoll_event {
    uint32_t events;
    epoll_data_t data;
};

#define EPOLLIN        0x00000001U
#define EPOLLPRI       0x00000002U
#define EPOLLOUT       0x00000004U
#define EPOLLERR       0x00000008U
#define EPOLLHUP       0x00000010U
#define EPOLLRDHUP     0x00002000U
#define EPOLLEXCLUSIVE 0x10000000U
#define EPOLLWAKEUP    0x20000000U
#define EPOLLONESHOT   0x40000000U
#define EPOLLET        0x80000000U

#define EPOLL_CTL_ADD 1
#define EPOLL_CTL_DEL 2
#define EPOLL_CTL_MOD 3

#define EPOLL_CLOEXEC O_CLOEXEC

int epoll_create(int size);
int epoll_create1(int flags);
int epoll_ctl(int epfd, int operation, int fd, struct epoll_event *event);
int epoll_wait(int epfd, struct epoll_event *events, int maxevents,
               int timeout);
int epoll_pwait(int epfd, struct epoll_event *events, int maxevents,
                int timeout, const sigset_t *signal_mask);

#endif /* ARMOS_SYS_EPOLL_H */
