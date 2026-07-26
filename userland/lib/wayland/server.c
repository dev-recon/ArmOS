/*
 * ArmOS
 * Copyright (c) 2026 Mohamed Ennassiri
 *
 * Licensed under the Apache License, Version 2.0.
 * See LICENSE for details.
 *
 * File: userland/lib/wayland/server.c
 * Layer: Userland / Wayland server runtime
 *
 * Responsibilities:
 * - Create a server display and its local listening socket.
 * - Resolve conventional Wayland socket names on ArmOS.
 * - Own and release server-side transport resources.
 *
 * Notes:
 * - Resource dispatch and globals are added after this transport base.
 * - No platform or architecture-specific code belongs in this library.
 */

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <wayland-server-core.h>

struct wl_event_source {
    struct wl_event_loop *loop;
    struct wl_event_source *next;
    int fd;
    uint32_t mask;
    wl_event_loop_fd_func_t func;
    void *data;
    bool removed;
};

struct wl_event_loop {
    struct wl_event_source *sources;
    unsigned int dispatch_depth;
};

struct wl_display {
    int listen_fd;
    char socket_path[sizeof(((struct sockaddr_un *)0)->sun_path)];
    struct wl_event_loop event_loop;
    bool terminated;
};

static void wl_event_loop_cleanup(struct wl_event_loop *loop)
{
    struct wl_event_source **link;

    if (!loop || loop->dispatch_depth != 0u)
        return;
    link = &loop->sources;
    while (*link) {
        struct wl_event_source *source = *link;

        if (!source->removed) {
            link = &source->next;
            continue;
        }
        *link = source->next;
        source->loop = NULL;
        free(source);
    }
}

static int wl_server_socket_path(const char *name, char *path, size_t size)
{
    const char *runtime;
    int count;

    if (!name || name[0] == '\0')
        name = "wayland-0";
    if (name[0] == '/') {
        count = snprintf(path, size, "%s", name);
    } else {
        runtime = getenv("XDG_RUNTIME_DIR");
        if (!runtime || runtime[0] == '\0')
            runtime = "/tmp";
        count = snprintf(path, size, "%s/%s", runtime, name);
    }
    return count >= 0 && (size_t)count < size ? 0 : -1;
}

struct wl_display *wl_display_create(void)
{
    struct wl_display *display = calloc(1, sizeof(*display));

    if (display)
        display->listen_fd = -1;
    return display;
}

void wl_display_destroy(struct wl_display *display)
{
    struct wl_event_source *source;

    if (!display)
        return;
    if (display->listen_fd >= 0)
        close(display->listen_fd);
    source = display->event_loop.sources;
    while (source) {
        struct wl_event_source *next = source->next;

        source->loop = NULL;
        free(source);
        source = next;
    }
    free(display);
}

int wl_display_add_socket(struct wl_display *display, const char *name)
{
    struct sockaddr_un address;
    int fd;

    if (!display || display->listen_fd >= 0) {
        errno = EINVAL;
        return -1;
    }
    if (wl_server_socket_path(name, display->socket_path,
                              sizeof(display->socket_path)) < 0) {
        errno = ENAMETOOLONG;
        return -1;
    }

    fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0)
        return -1;
    if (fcntl(fd, F_SETFD, FD_CLOEXEC) < 0) {
        close(fd);
        return -1;
    }
    memset(&address, 0, sizeof(address));
    address.sun_family = AF_UNIX;
    memcpy(address.sun_path, display->socket_path,
           strlen(display->socket_path) + 1u);
    if (bind(fd, (struct sockaddr *)&address, sizeof(address)) < 0 ||
        listen(fd, 16) < 0) {
        close(fd);
        return -1;
    }
    display->listen_fd = fd;
    return 0;
}

int wl_display_get_server_fd(struct wl_display *display)
{
    if (!display || display->listen_fd < 0) {
        errno = EINVAL;
        return -1;
    }
    return display->listen_fd;
}

struct wl_event_loop *wl_display_get_event_loop(struct wl_display *display)
{
    if (!display) {
        errno = EINVAL;
        return NULL;
    }
    return &display->event_loop;
}

struct wl_event_source *wl_event_loop_add_fd(
    struct wl_event_loop *loop, int fd, uint32_t mask,
    wl_event_loop_fd_func_t func, void *data)
{
    const uint32_t valid_mask = WL_EVENT_READABLE | WL_EVENT_WRITABLE |
        WL_EVENT_HANGUP | WL_EVENT_ERROR;
    struct wl_event_source *source;

    if (!loop || fd < 0 || !func || (mask & ~valid_mask) != 0u) {
        errno = EINVAL;
        return NULL;
    }
    source = calloc(1, sizeof(*source));
    if (!source)
        return NULL;
    source->loop = loop;
    source->fd = fd;
    source->mask = mask;
    source->func = func;
    source->data = data;
    source->next = loop->sources;
    loop->sources = source;
    return source;
}

int wl_event_source_fd_update(struct wl_event_source *source, uint32_t mask)
{
    const uint32_t valid_mask = WL_EVENT_READABLE | WL_EVENT_WRITABLE |
        WL_EVENT_HANGUP | WL_EVENT_ERROR;

    if (!source || !source->loop || source->removed ||
        (mask & ~valid_mask) != 0u) {
        errno = EINVAL;
        return -1;
    }
    source->mask = mask;
    return 0;
}

int wl_event_source_remove(struct wl_event_source *source)
{
    struct wl_event_loop *loop;

    if (!source || !(loop = source->loop) || source->removed) {
        errno = EINVAL;
        return -1;
    }
    source->removed = true;
    wl_event_loop_cleanup(loop);
    return 0;
}

static short wl_event_poll_mask(uint32_t mask)
{
    short events = 0;

    if ((mask & WL_EVENT_READABLE) != 0u)
        events |= POLLIN;
    if ((mask & WL_EVENT_WRITABLE) != 0u)
        events |= POLLOUT;
    return events;
}

static uint32_t wl_event_callback_mask(short events)
{
    uint32_t mask = 0u;

    if ((events & POLLIN) != 0)
        mask |= WL_EVENT_READABLE;
    if ((events & POLLOUT) != 0)
        mask |= WL_EVENT_WRITABLE;
    if ((events & POLLHUP) != 0)
        mask |= WL_EVENT_HANGUP;
    if ((events & (POLLERR | POLLNVAL)) != 0)
        mask |= WL_EVENT_ERROR;
    return mask;
}

int wl_event_loop_dispatch(struct wl_event_loop *loop, int timeout)
{
    struct wl_event_source **sources;
    struct wl_event_source *source;
    struct pollfd *descriptors;
    size_t count = 0u;
    size_t index = 0u;
    int ready;
    int dispatched = 0;

    if (!loop || timeout < -1) {
        errno = EINVAL;
        return -1;
    }
    for (source = loop->sources; source; source = source->next) {
        if (!source->removed)
            count++;
    }
    if (count == 0u)
        return poll(NULL, 0u, timeout);
    descriptors = calloc(count, sizeof(*descriptors));
    sources = calloc(count, sizeof(*sources));
    if (!descriptors || !sources) {
        free(descriptors);
        free(sources);
        return -1;
    }
    for (source = loop->sources; source; source = source->next) {
        if (source->removed)
            continue;
        descriptors[index].fd = source->fd;
        descriptors[index].events = wl_event_poll_mask(source->mask);
        sources[index] = source;
        index++;
    }
    ready = poll(descriptors, count, timeout);
    if (ready <= 0) {
        free(sources);
        free(descriptors);
        return ready;
    }

    loop->dispatch_depth++;
    for (index = 0u; index < count; index++) {
        uint32_t mask;

        source = sources[index];
        if (!source || source->removed || descriptors[index].revents == 0)
            continue;
        mask = wl_event_callback_mask(descriptors[index].revents) &
            (source->mask | WL_EVENT_HANGUP | WL_EVENT_ERROR);
        if (mask == 0u)
            continue;
        if (source->func(source->fd, mask, source->data) < 0)
            source->removed = true;
        dispatched++;
    }
    loop->dispatch_depth--;
    wl_event_loop_cleanup(loop);
    free(sources);
    free(descriptors);
    return dispatched;
}

void wl_display_run(struct wl_display *display)
{
    if (!display)
        return;
    display->terminated = false;
    while (!display->terminated) {
        if (wl_event_loop_dispatch(&display->event_loop, -1) < 0 &&
            errno != EINTR)
            break;
    }
}

void wl_display_terminate(struct wl_display *display)
{
    if (display)
        display->terminated = true;
}
