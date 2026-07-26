/*
 * ArmOS
 * Copyright (c) 2026 Mohamed Ennassiri
 *
 * Licensed under the Apache License, Version 2.0.
 * See LICENSE for details.
 *
 * File: userland/opt/epoll-shim/src/epoll.c
 * Layer: Userland / descriptor compatibility
 *
 * Responsibilities:
 * - Provide level-triggered epoll semantics over the common ppoll(2) API.
 * - Preserve caller event data across descriptor readiness notifications.
 * - Serialize registry updates made by multiple threads in one process.
 *
 * Notes:
 * - The implementation is original ArmOS code and has no platform dependency.
 * - Edge-triggered, one-shot and exclusive wakeup modes are not yet supported.
 */

#include <sys/epoll.h>

#include <errno.h>
#include <poll.h>
#include <stddef.h>
#include <sys/eventfd.h>
#include <time.h>
#include <unistd.h>

#define ARMOS_EPOLL_MAX_INSTANCES 8
#define ARMOS_EPOLL_MAX_WATCHES   256

struct armos_epoll_watch {
    int fd;
    uint32_t events;
    epoll_data_t data;
};

struct armos_epoll_instance {
    int active;
    int token_fd;
    unsigned int watch_count;
    struct armos_epoll_watch watches[ARMOS_EPOLL_MAX_WATCHES];
};

static struct armos_epoll_instance armos_epoll_instances[
    ARMOS_EPOLL_MAX_INSTANCES];
static volatile int armos_epoll_registry_lock;

static void armos_epoll_lock(void)
{
    while (__sync_lock_test_and_set(&armos_epoll_registry_lock, 1) != 0) {
        while (armos_epoll_registry_lock != 0)
            ;
    }
}

static void armos_epoll_unlock(void)
{
    __sync_synchronize();
    armos_epoll_registry_lock = 0;
}

static struct armos_epoll_instance *armos_epoll_find_locked(int epfd)
{
    unsigned int index;

    for (index = 0; index < ARMOS_EPOLL_MAX_INSTANCES; index++) {
        if (armos_epoll_instances[index].active &&
            armos_epoll_instances[index].token_fd == epfd)
            return &armos_epoll_instances[index];
    }
    return NULL;
}

static int armos_epoll_watch_index(const struct armos_epoll_instance *instance,
                                   int fd)
{
    unsigned int index;

    for (index = 0; index < instance->watch_count; index++) {
        if (instance->watches[index].fd == fd)
            return (int)index;
    }
    return -1;
}

static short armos_epoll_poll_events(uint32_t events)
{
    short poll_events = 0;

    if ((events & EPOLLIN) != 0)
        poll_events |= POLLIN;
    if ((events & EPOLLPRI) != 0)
        poll_events |= POLLPRI;
    if ((events & EPOLLOUT) != 0)
        poll_events |= POLLOUT;
    return poll_events;
}

static uint32_t armos_epoll_ready_events(short revents)
{
    uint32_t events = 0;

    if ((revents & POLLIN) != 0)
        events |= EPOLLIN;
    if ((revents & POLLPRI) != 0)
        events |= EPOLLPRI;
    if ((revents & POLLOUT) != 0)
        events |= EPOLLOUT;
    if ((revents & POLLHUP) != 0)
        events |= EPOLLHUP;
    if ((revents & (POLLERR | POLLNVAL)) != 0)
        events |= EPOLLERR;
    return events;
}

int epoll_create1(int flags)
{
    struct armos_epoll_instance *instance = NULL;
    int token_fd;
    unsigned int index;

    if ((flags & ~EPOLL_CLOEXEC) != 0) {
        errno = EINVAL;
        return -1;
    }

    token_fd = eventfd(0, EFD_NONBLOCK |
        ((flags & EPOLL_CLOEXEC) != 0 ? EFD_CLOEXEC : 0));
    if (token_fd < 0)
        return -1;

    armos_epoll_lock();
    for (index = 0; index < ARMOS_EPOLL_MAX_INSTANCES; index++) {
        if (!armos_epoll_instances[index].active) {
            instance = &armos_epoll_instances[index];
            instance->active = 1;
            instance->token_fd = token_fd;
            instance->watch_count = 0;
            break;
        }
    }
    armos_epoll_unlock();

    if (instance == NULL) {
        close(token_fd);
        errno = EMFILE;
        return -1;
    }
    return token_fd;
}

int epoll_create(int size)
{
    if (size <= 0) {
        errno = EINVAL;
        return -1;
    }
    return epoll_create1(0);
}

int epoll_ctl(int epfd, int operation, int fd, struct epoll_event *event)
{
    struct armos_epoll_instance *instance;
    int watch_index;
    int result = -1;

    if (fd < 0 || fd == epfd) {
        errno = EINVAL;
        return -1;
    }
    if ((operation == EPOLL_CTL_ADD || operation == EPOLL_CTL_MOD) &&
        event == NULL) {
        errno = EFAULT;
        return -1;
    }
    if (event != NULL &&
        (event->events &
         (EPOLLRDHUP | EPOLLEXCLUSIVE | EPOLLWAKEUP |
          EPOLLONESHOT | EPOLLET)) != 0) {
        errno = EINVAL;
        return -1;
    }

    armos_epoll_lock();
    instance = armos_epoll_find_locked(epfd);
    if (instance == NULL) {
        errno = EBADF;
        goto out;
    }

    watch_index = armos_epoll_watch_index(instance, fd);
    switch (operation) {
    case EPOLL_CTL_ADD:
        if (watch_index >= 0) {
            errno = EEXIST;
            break;
        }
        if (instance->watch_count >= ARMOS_EPOLL_MAX_WATCHES) {
            errno = ENOSPC;
            break;
        }
        watch_index = (int)instance->watch_count++;
        instance->watches[watch_index].fd = fd;
        instance->watches[watch_index].events = event->events;
        instance->watches[watch_index].data = event->data;
        result = 0;
        break;
    case EPOLL_CTL_MOD:
        if (watch_index < 0) {
            errno = ENOENT;
            break;
        }
        instance->watches[watch_index].events = event->events;
        instance->watches[watch_index].data = event->data;
        result = 0;
        break;
    case EPOLL_CTL_DEL:
        if (watch_index < 0) {
            errno = ENOENT;
            break;
        }
        instance->watch_count--;
        if ((unsigned int)watch_index != instance->watch_count) {
            instance->watches[watch_index] =
                instance->watches[instance->watch_count];
        }
        result = 0;
        break;
    default:
        errno = EINVAL;
        break;
    }

out:
    armos_epoll_unlock();
    return result;
}

int epoll_pwait(int epfd, struct epoll_event *events, int maxevents,
                int timeout, const sigset_t *signal_mask)
{
    struct armos_epoll_watch snapshot[ARMOS_EPOLL_MAX_WATCHES];
    struct pollfd poll_fds[ARMOS_EPOLL_MAX_WATCHES];
    struct armos_epoll_instance *instance;
    struct timespec timeout_value;
    const struct timespec *timeout_pointer;
    unsigned int watch_count;
    unsigned int index;
    int ready_count;
    int output_count = 0;

    if (events == NULL) {
        errno = EFAULT;
        return -1;
    }
    if (maxevents <= 0 || timeout < -1) {
        errno = EINVAL;
        return -1;
    }

    armos_epoll_lock();
    instance = armos_epoll_find_locked(epfd);
    if (instance == NULL) {
        armos_epoll_unlock();
        errno = EBADF;
        return -1;
    }
    watch_count = instance->watch_count;
    for (index = 0; index < watch_count; index++)
        snapshot[index] = instance->watches[index];
    armos_epoll_unlock();

    for (index = 0; index < watch_count; index++) {
        poll_fds[index].fd = snapshot[index].fd;
        poll_fds[index].events =
            armos_epoll_poll_events(snapshot[index].events);
        poll_fds[index].revents = 0;
    }

    if (timeout < 0) {
        timeout_pointer = NULL;
    } else {
        timeout_value.tv_sec = timeout / 1000;
        timeout_value.tv_nsec = (long)(timeout % 1000) * 1000000L;
        timeout_pointer = &timeout_value;
    }

    ready_count = ppoll(poll_fds, watch_count, timeout_pointer, signal_mask);
    if (ready_count <= 0)
        return ready_count;

    for (index = 0;
         index < watch_count && output_count < maxevents;
         index++) {
        uint32_t ready_events =
            armos_epoll_ready_events(poll_fds[index].revents);

        if (ready_events == 0)
            continue;
        events[output_count].events = ready_events;
        events[output_count].data = snapshot[index].data;
        output_count++;
    }
    return output_count;
}

int epoll_wait(int epfd, struct epoll_event *events, int maxevents,
               int timeout)
{
    return epoll_pwait(epfd, events, maxevents, timeout, NULL);
}
