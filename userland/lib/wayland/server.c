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
 * - Protocol-specific request decoding remains outside this core runtime.
 * - No platform or architecture-specific code belongs in this library.
 */

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdbool.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/uio.h>
#include <time.h>
#include <unistd.h>
#include <wayland-server-core.h>

enum wl_event_source_type {
    WL_EVENT_SOURCE_FD,
    WL_EVENT_SOURCE_TIMER,
    WL_EVENT_SOURCE_IDLE
};

#define WL_SERVER_RECEIVE_CAPACITY (64u * 1024u)
#define WL_SERVER_PENDING_FDS      16u
#define WL_SERVER_MAX_ARGUMENTS    32u

struct wl_event_source {
    struct wl_event_loop *loop;
    struct wl_event_source *next;
    enum wl_event_source_type type;
    int fd;
    uint32_t mask;
    union {
        wl_event_loop_fd_func_t fd;
        wl_event_loop_timer_func_t timer;
        wl_event_loop_idle_func_t idle;
    } func;
    void *data;
    uint64_t deadline_ms;
    bool armed;
    bool removed;
};

struct wl_event_loop {
    struct wl_event_source *sources;
    unsigned int dispatch_depth;
};

struct wl_resource {
    struct wl_client *client;
    struct wl_resource *next;
    const struct wl_interface *interface;
    wl_dispatcher_func_t dispatcher;
    const void *implementation;
    void *data;
    wl_resource_destroy_func_t destroy;
    uint32_t id;
    int version;
    bool destroying;
};

struct wl_client {
    struct wl_display *display;
    struct wl_client *next;
    struct wl_resource *resources;
    struct wl_event_source *source;
    uint8_t receive[WL_SERVER_RECEIVE_CAPACITY];
    size_t receive_length;
    int pending_fds[WL_SERVER_PENDING_FDS];
    size_t pending_fd_count;
    int fd;
    bool dispatching;
    bool destroy_pending;
    bool destroying;
};

struct wl_global {
    struct wl_display *display;
    struct wl_global *next;
    const struct wl_interface *interface;
    wl_global_bind_func_t bind;
    void *data;
    uint32_t name;
    uint32_t version;
};

struct wl_display {
    int listen_fd;
    char socket_path[sizeof(((struct sockaddr_un *)0)->sun_path)];
    struct wl_event_loop event_loop;
    struct wl_event_source *listen_source;
    struct wl_client *clients;
    struct wl_global *globals;
    uint32_t next_global_name;
    uint32_t serial;
    bool terminated;
};

static const struct wl_message wl_server_display_methods[] = {
    {"sync", "n", NULL},
    {"get_registry", "n", NULL}
};

static const struct wl_message wl_server_display_events[] = {
    {"error", "ous", NULL},
    {"delete_id", "u", NULL}
};

static const struct wl_interface wl_server_display_interface = {
    "wl_display", 1, 2, wl_server_display_methods,
    2, wl_server_display_events
};

static const struct wl_message wl_server_registry_methods[] = {
    {"bind", "usun", NULL}
};

static const struct wl_message wl_server_registry_events[] = {
    {"global", "usu", NULL},
    {"global_remove", "u", NULL}
};

static const struct wl_interface wl_server_registry_interface = {
    "wl_registry", 1, 1, wl_server_registry_methods,
    2, wl_server_registry_events
};

static const struct wl_message wl_server_callback_events[] = {
    {"done", "u", NULL}
};

static const struct wl_interface wl_server_callback_interface = {
    "wl_callback", 1, 0, NULL, 1, wl_server_callback_events
};

static int wl_display_resource_dispatch(
    const void *implementation, void *target, uint32_t opcode,
    const struct wl_message *message, union wl_argument *arguments);
static int wl_registry_resource_dispatch(
    const void *implementation, void *target, uint32_t opcode,
    const struct wl_message *message, union wl_argument *arguments);
static int wl_display_accept_event(int fd, uint32_t mask, void *data);
static int wl_resource_implementation_dispatch(
    const void *implementation, void *target, uint32_t opcode,
    const struct wl_message *message, union wl_argument *arguments);
static int wl_resource_send_message(struct wl_client *client,
                                    const uint8_t *message, size_t size,
                                    const int *fds, size_t fd_count);

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

static uint64_t wl_event_loop_now_ms(void)
{
    struct timespec now;

    if (clock_gettime(CLOCK_MONOTONIC, &now) < 0)
        return 0u;
    return (uint64_t)now.tv_sec * 1000u +
        (uint64_t)now.tv_nsec / 1000000u;
}

static struct wl_event_source *wl_event_source_create(
    struct wl_event_loop *loop, enum wl_event_source_type type, void *data)
{
    struct wl_event_source *source;

    source = calloc(1, sizeof(*source));
    if (!source)
        return NULL;
    source->loop = loop;
    source->type = type;
    source->fd = -1;
    source->data = data;
    source->next = loop->sources;
    loop->sources = source;
    return source;
}

static int wl_client_event(int fd, uint32_t mask, void *data);

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

    if (display) {
        display->listen_fd = -1;
        display->next_global_name = 1u;
    }
    return display;
}

void wl_display_destroy(struct wl_display *display)
{
    struct wl_event_source *source;

    if (!display)
        return;
    while (display->clients)
        wl_client_destroy(display->clients);
    while (display->globals)
        wl_global_destroy(display->globals);
    if (display->listen_source) {
        (void)wl_event_source_remove(display->listen_source);
        display->listen_source = NULL;
    }
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
    display->listen_source = wl_event_loop_add_fd(
        &display->event_loop, fd,
        WL_EVENT_READABLE | WL_EVENT_HANGUP | WL_EVENT_ERROR,
        wl_display_accept_event, display);
    if (!display->listen_source) {
        close(fd);
        display->listen_fd = -1;
        return -1;
    }
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
    source = wl_event_source_create(loop, WL_EVENT_SOURCE_FD, data);
    if (!source)
        return NULL;
    source->fd = fd;
    source->mask = mask;
    source->func.fd = func;
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

struct wl_event_source *wl_event_loop_add_timer(
    struct wl_event_loop *loop, wl_event_loop_timer_func_t func, void *data)
{
    struct wl_event_source *source;

    if (!loop || !func) {
        errno = EINVAL;
        return NULL;
    }
    source = wl_event_source_create(loop, WL_EVENT_SOURCE_TIMER, data);
    if (!source)
        return NULL;
    source->func.timer = func;
    return source;
}

int wl_event_source_timer_update(struct wl_event_source *source, int ms_delay)
{
    uint64_t now;

    if (!source || !source->loop || source->removed ||
        source->type != WL_EVENT_SOURCE_TIMER || ms_delay < 0) {
        errno = EINVAL;
        return -1;
    }
    now = wl_event_loop_now_ms();
    source->deadline_ms = now + (uint64_t)ms_delay;
    source->armed = true;
    return 0;
}

struct wl_event_source *wl_event_loop_add_idle(
    struct wl_event_loop *loop, wl_event_loop_idle_func_t func, void *data)
{
    struct wl_event_source *source;

    if (!loop || !func) {
        errno = EINVAL;
        return NULL;
    }
    source = wl_event_source_create(loop, WL_EVENT_SOURCE_IDLE, data);
    if (!source)
        return NULL;
    source->func.idle = func;
    source->armed = true;
    return source;
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
    uint64_t now;

    if (!loop || timeout < -1) {
        errno = EINVAL;
        return -1;
    }
    now = wl_event_loop_now_ms();
    for (source = loop->sources; source; source = source->next) {
        int timer_timeout;

        if (source->removed)
            continue;
        if (source->type == WL_EVENT_SOURCE_FD) {
            count++;
            continue;
        }
        if (source->type == WL_EVENT_SOURCE_IDLE && source->armed) {
            timeout = 0;
            continue;
        }
        if (source->type != WL_EVENT_SOURCE_TIMER || !source->armed)
            continue;
        timer_timeout = source->deadline_ms <= now ? 0 :
            (int)(source->deadline_ms - now);
        if (timeout < 0 || timer_timeout < timeout)
            timeout = timer_timeout;
    }
    descriptors = NULL;
    sources = NULL;
    if (count != 0u) {
        descriptors = calloc(count, sizeof(*descriptors));
        sources = calloc(count, sizeof(*sources));
        if (!descriptors || !sources) {
            free(descriptors);
            free(sources);
            return -1;
        }
    }
    for (source = loop->sources; source; source = source->next) {
        if (source->removed || source->type != WL_EVENT_SOURCE_FD)
            continue;
        descriptors[index].fd = source->fd;
        descriptors[index].events = wl_event_poll_mask(source->mask);
        sources[index] = source;
        index++;
    }
    ready = poll(descriptors, count, timeout);
    if (ready < 0) {
        free(sources);
        free(descriptors);
        return ready;
    }

    loop->dispatch_depth++;
    if (ready > 0) {
        for (index = 0u; index < count; index++) {
            uint32_t mask;

            source = sources[index];
            if (!source || source->removed ||
                descriptors[index].revents == 0)
                continue;
            mask = wl_event_callback_mask(descriptors[index].revents) &
                (source->mask | WL_EVENT_HANGUP | WL_EVENT_ERROR);
            if (mask == 0u)
                continue;
            if (source->func.fd(source->fd, mask, source->data) < 0)
                source->removed = true;
            dispatched++;
        }
    }
    now = wl_event_loop_now_ms();
    for (source = loop->sources; source; source = source->next) {
        if (source->removed || !source->armed)
            continue;
        if (source->type == WL_EVENT_SOURCE_TIMER &&
            source->deadline_ms <= now) {
            source->armed = false;
            if (source->func.timer(source->data) < 0)
                source->removed = true;
            dispatched++;
        } else if (source->type == WL_EVENT_SOURCE_IDLE) {
            source->armed = false;
            source->func.idle(source->data);
            source->removed = true;
            dispatched++;
        }
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

struct wl_client *wl_client_create(struct wl_display *display, int fd)
{
    struct wl_client *client;
    struct wl_resource *display_resource;

    if (!display || fd < 0) {
        errno = EINVAL;
        return NULL;
    }
    client = calloc(1, sizeof(*client));
    if (!client)
        return NULL;
    if (fcntl(fd, F_SETFD, FD_CLOEXEC) < 0) {
        free(client);
        return NULL;
    }
    client->display = display;
    client->fd = fd;
    client->source = wl_event_loop_add_fd(
        &display->event_loop, fd,
        WL_EVENT_READABLE | WL_EVENT_HANGUP | WL_EVENT_ERROR,
        wl_client_event, client);
    if (!client->source) {
        free(client);
        return NULL;
    }
    client->next = display->clients;
    display->clients = client;
    display_resource = wl_resource_create(
        client, &wl_server_display_interface, 1, 1u);
    if (!display_resource) {
        client->fd = -1;
        wl_client_destroy(client);
        return NULL;
    }
    wl_resource_set_dispatcher(
        display_resource, wl_display_resource_dispatch,
        NULL, display, NULL);
    return client;
}

void wl_client_destroy(struct wl_client *client)
{
    struct wl_client **link;

    if (!client || client->destroying)
        return;
    if (client->dispatching) {
        client->destroy_pending = true;
        return;
    }
    client->destroying = true;
    while (client->resources)
        wl_resource_destroy(client->resources);
    if (client->display) {
        link = &client->display->clients;
        while (*link && *link != client)
            link = &(*link)->next;
        if (*link == client)
            *link = client->next;
    }
    if (client->source) {
        (void)wl_event_source_remove(client->source);
        client->source = NULL;
    }
    while (client->pending_fd_count != 0u)
        close(client->pending_fds[--client->pending_fd_count]);
    if (client->fd >= 0)
        close(client->fd);
    client->display = NULL;
    free(client);
}

void wl_client_flush(struct wl_client *client)
{
    (void)client;
}

int wl_client_get_fd(struct wl_client *client)
{
    if (!client) {
        errno = EINVAL;
        return -1;
    }
    return client->fd;
}

struct wl_display *wl_client_get_display(struct wl_client *client)
{
    return client ? client->display : NULL;
}

struct wl_resource *wl_client_get_object(struct wl_client *client,
                                         uint32_t id)
{
    struct wl_resource *resource;

    if (!client)
        return NULL;
    for (resource = client->resources; resource; resource = resource->next) {
        if (!resource->destroying && resource->id == id)
            return resource;
    }
    return NULL;
}

struct wl_global *wl_global_create(
    struct wl_display *display, const struct wl_interface *interface,
    int version, void *data, wl_global_bind_func_t bind)
{
    struct wl_global *global;
    uint32_t name;

    if (!display || !interface || !interface->name || !bind ||
        version < 1 || version > interface->version) {
        errno = EINVAL;
        return NULL;
    }
    name = display->next_global_name++;
    if (name == 0u) {
        errno = ENOSPC;
        return NULL;
    }
    global = calloc(1, sizeof(*global));
    if (!global)
        return NULL;
    global->display = display;
    global->interface = interface;
    global->bind = bind;
    global->data = data;
    global->name = name;
    global->version = (uint32_t)version;
    global->next = display->globals;
    display->globals = global;
    for (struct wl_client *client = display->clients;
         client; client = client->next) {
        for (struct wl_resource *resource = client->resources;
             resource; resource = resource->next) {
            if (resource->interface == &wl_server_registry_interface)
                wl_resource_post_event(resource, 0u, global->name,
                                       global->interface->name,
                                       global->version);
        }
    }
    return global;
}

void wl_global_destroy(struct wl_global *global)
{
    struct wl_global **link;

    if (!global)
        return;
    if (global->display) {
        for (struct wl_client *client = global->display->clients;
             client; client = client->next) {
            for (struct wl_resource *resource = client->resources;
                 resource; resource = resource->next) {
                if (resource->interface == &wl_server_registry_interface)
                    wl_resource_post_event(resource, 1u, global->name);
            }
        }
        link = &global->display->globals;
        while (*link && *link != global)
            link = &(*link)->next;
        if (*link == global)
            *link = global->next;
    }
    global->display = NULL;
    free(global);
}

const struct wl_interface *wl_global_get_interface(
    const struct wl_global *global)
{
    return global ? global->interface : NULL;
}

uint32_t wl_global_get_name(const struct wl_global *global)
{
    return global ? global->name : 0u;
}

uint32_t wl_global_get_version(const struct wl_global *global)
{
    return global ? global->version : 0u;
}

void *wl_global_get_user_data(const struct wl_global *global)
{
    return global ? global->data : NULL;
}

struct wl_resource *wl_resource_create(
    struct wl_client *client, const struct wl_interface *interface,
    int version, uint32_t id)
{
    struct wl_resource *resource;

    if (!client || client->destroying || !interface || !interface->name ||
        version < 1 || version > interface->version || id == 0u ||
        wl_client_get_object(client, id)) {
        errno = EINVAL;
        return NULL;
    }
    resource = calloc(1, sizeof(*resource));
    if (!resource)
        return NULL;
    resource->client = client;
    resource->interface = interface;
    resource->version = version;
    resource->id = id;
    resource->next = client->resources;
    client->resources = resource;
    return resource;
}

void wl_resource_set_implementation(
    struct wl_resource *resource, const void *implementation, void *data,
    wl_resource_destroy_func_t destroy)
{
    if (!resource || resource->destroying)
        return;
    resource->dispatcher = wl_resource_implementation_dispatch;
    resource->implementation = implementation;
    resource->data = data;
    resource->destroy = destroy;
}

void wl_resource_set_dispatcher(
    struct wl_resource *resource, wl_dispatcher_func_t dispatcher,
    const void *implementation, void *data,
    wl_resource_destroy_func_t destroy)
{
    if (!resource || resource->destroying)
        return;
    resource->dispatcher = dispatcher;
    resource->implementation = implementation;
    resource->data = data;
    resource->destroy = destroy;
}

void wl_resource_destroy(struct wl_resource *resource)
{
    struct wl_resource **link;
    wl_resource_destroy_func_t destroy;

    if (!resource || resource->destroying)
        return;
    resource->destroying = true;
    if (resource->client) {
        link = &resource->client->resources;
        while (*link && *link != resource)
            link = &(*link)->next;
        if (*link == resource)
            *link = resource->next;
    }
    if (resource->client && !resource->client->destroying &&
        resource->id != 1u && resource->id < 0xff000000u) {
        uint8_t message[12];
        uint32_t header = (12u << 16) | 1u;

        memcpy(message, &(uint32_t){1u}, sizeof(uint32_t));
        memcpy(message + 4u, &header, sizeof(header));
        memcpy(message + 8u, &resource->id, sizeof(resource->id));
        (void)wl_resource_send_message(resource->client, message,
                                       sizeof(message), NULL, 0u);
    }
    destroy = resource->destroy;
    if (destroy)
        destroy(resource);
    resource->client = NULL;
    free(resource);
}

uint32_t wl_resource_get_id(struct wl_resource *resource)
{
    return resource ? resource->id : 0u;
}

int wl_resource_get_version(struct wl_resource *resource)
{
    return resource ? resource->version : 0;
}

const char *wl_resource_get_class(struct wl_resource *resource)
{
    return resource && resource->interface ? resource->interface->name : NULL;
}

struct wl_client *wl_resource_get_client(struct wl_resource *resource)
{
    return resource ? resource->client : NULL;
}

void wl_resource_set_user_data(struct wl_resource *resource, void *data)
{
    if (resource && !resource->destroying)
        resource->data = data;
}

void *wl_resource_get_user_data(struct wl_resource *resource)
{
    return resource ? resource->data : NULL;
}

int wl_resource_instance_of(struct wl_resource *resource,
                            const struct wl_interface *interface,
                            const void *implementation)
{
    return resource && resource->interface == interface &&
        resource->implementation == implementation;
}

static size_t wl_wire_align(size_t size)
{
    return (size + 3u) & ~(size_t)3u;
}

static const char *wl_signature_next(const char *signature)
{
    while (*signature >= '0' && *signature <= '9')
        signature++;
    if (*signature == '?')
        signature++;
    return signature;
}

typedef void (*wl_implementation_method_t)(void);

static int wl_resource_implementation_dispatch(
    const void *implementation, void *target, uint32_t opcode,
    const struct wl_message *message, union wl_argument *arguments)
{
    struct wl_resource *resource = target;
    struct wl_client *client = wl_resource_get_client(resource);
    wl_implementation_method_t method;
    uintptr_t values[12] = { 0u };
    const char *signature;
    size_t count = 0u;

    if (!implementation || !resource || !client || !message) {
        errno = EINVAL;
        return -1;
    }
    memcpy(&method,
           (const uint8_t *)implementation +
               opcode * sizeof(wl_implementation_method_t),
           sizeof(method));
    if (!method) {
        errno = EPROTO;
        return -1;
    }
    signature = message->signature;
    while (signature && *signature) {
        if (count >= sizeof(values) / sizeof(values[0])) {
            errno = E2BIG;
            return -1;
        }
        signature = wl_signature_next(signature);
        switch (*signature) {
        case 'i':
            values[count] = (uintptr_t)(uint32_t)arguments[count].i;
            break;
        case 'u':
            values[count] = arguments[count].u;
            break;
        case 'f':
            values[count] = (uintptr_t)(uint32_t)arguments[count].f;
            break;
        case 's':
            values[count] = (uintptr_t)arguments[count].s;
            break;
        case 'o':
            values[count] = (uintptr_t)arguments[count].o;
            break;
        case 'n':
            values[count] = arguments[count].n;
            break;
        case 'a':
            values[count] = (uintptr_t)arguments[count].a;
            break;
        case 'h':
            values[count] = (uintptr_t)(uint32_t)arguments[count].h;
            break;
        case '\0':
            continue;
        default:
            errno = EPROTO;
            return -1;
        }
        count++;
        signature++;
    }
    switch (count) {
    case 0u:
        ((void (*)(struct wl_client *, struct wl_resource *))method)(
            client, resource);
        break;
    case 1u:
        ((void (*)(struct wl_client *, struct wl_resource *,
                   uintptr_t))method)(client, resource, values[0]);
        break;
    case 2u:
        ((void (*)(struct wl_client *, struct wl_resource *,
                   uintptr_t, uintptr_t))method)(
            client, resource, values[0], values[1]);
        break;
    case 3u:
        ((void (*)(struct wl_client *, struct wl_resource *,
                   uintptr_t, uintptr_t, uintptr_t))method)(
            client, resource, values[0], values[1], values[2]);
        break;
    case 4u:
        ((void (*)(struct wl_client *, struct wl_resource *,
                   uintptr_t, uintptr_t, uintptr_t, uintptr_t))method)(
            client, resource, values[0], values[1], values[2], values[3]);
        break;
    case 5u:
        ((void (*)(struct wl_client *, struct wl_resource *,
                   uintptr_t, uintptr_t, uintptr_t, uintptr_t,
                   uintptr_t))method)(
            client, resource, values[0], values[1], values[2], values[3],
            values[4]);
        break;
    case 6u:
        ((void (*)(struct wl_client *, struct wl_resource *,
                   uintptr_t, uintptr_t, uintptr_t, uintptr_t,
                   uintptr_t, uintptr_t))method)(
            client, resource, values[0], values[1], values[2], values[3],
            values[4], values[5]);
        break;
    case 7u:
        ((void (*)(struct wl_client *, struct wl_resource *,
                   uintptr_t, uintptr_t, uintptr_t, uintptr_t,
                   uintptr_t, uintptr_t, uintptr_t))method)(
            client, resource, values[0], values[1], values[2], values[3],
            values[4], values[5], values[6]);
        break;
    case 8u:
        ((void (*)(struct wl_client *, struct wl_resource *,
                   uintptr_t, uintptr_t, uintptr_t, uintptr_t,
                   uintptr_t, uintptr_t, uintptr_t, uintptr_t))method)(
            client, resource, values[0], values[1], values[2], values[3],
            values[4], values[5], values[6], values[7]);
        break;
    case 9u:
        ((void (*)(struct wl_client *, struct wl_resource *,
                   uintptr_t, uintptr_t, uintptr_t, uintptr_t,
                   uintptr_t, uintptr_t, uintptr_t, uintptr_t,
                   uintptr_t))method)(
            client, resource, values[0], values[1], values[2], values[3],
            values[4], values[5], values[6], values[7], values[8]);
        break;
    case 10u:
        ((void (*)(struct wl_client *, struct wl_resource *,
                   uintptr_t, uintptr_t, uintptr_t, uintptr_t,
                   uintptr_t, uintptr_t, uintptr_t, uintptr_t,
                   uintptr_t, uintptr_t))method)(
            client, resource, values[0], values[1], values[2], values[3],
            values[4], values[5], values[6], values[7], values[8], values[9]);
        break;
    case 11u:
        ((void (*)(struct wl_client *, struct wl_resource *,
                   uintptr_t, uintptr_t, uintptr_t, uintptr_t,
                   uintptr_t, uintptr_t, uintptr_t, uintptr_t,
                   uintptr_t, uintptr_t, uintptr_t))method)(
            client, resource, values[0], values[1], values[2], values[3],
            values[4], values[5], values[6], values[7], values[8], values[9],
            values[10]);
        break;
    case 12u:
        ((void (*)(struct wl_client *, struct wl_resource *,
                   uintptr_t, uintptr_t, uintptr_t, uintptr_t,
                   uintptr_t, uintptr_t, uintptr_t, uintptr_t,
                   uintptr_t, uintptr_t, uintptr_t, uintptr_t))method)(
            client, resource, values[0], values[1], values[2], values[3],
            values[4], values[5], values[6], values[7], values[8], values[9],
            values[10], values[11]);
        break;
    default:
        errno = E2BIG;
        return -1;
    }
    return 0;
}

static int wl_client_decode_request(struct wl_client *client,
                                    struct wl_resource *resource,
                                    uint32_t opcode,
                                    const uint8_t *payload,
                                    size_t payload_size,
                                    size_t *fd_count_out)
{
    union wl_argument arguments[WL_SERVER_MAX_ARGUMENTS];
    struct wl_array arrays[WL_SERVER_MAX_ARGUMENTS];
    const struct wl_message *method;
    const char *signature;
    size_t argument_index = 0u;
    size_t fd_count = 0u;
    size_t offset = 0u;
    int result;

    if (!resource->interface || opcode >=
        (uint32_t)resource->interface->method_count ||
        !resource->interface->methods || !resource->dispatcher) {
        errno = EPROTO;
        return -1;
    }
    method = &resource->interface->methods[opcode];
    signature = method->signature;
    memset(arguments, 0, sizeof(arguments));
    memset(arrays, 0, sizeof(arrays));
    while (signature && *signature) {
        uint32_t value;

        if (argument_index >= WL_SERVER_MAX_ARGUMENTS) {
            errno = E2BIG;
            return -1;
        }
        signature = wl_signature_next(signature);
        switch (*signature) {
        case 'i':
        case 'u':
        case 'f':
        case 'o':
        case 'n':
        case 's':
        case 'a':
            if (offset > payload_size || payload_size - offset < 4u) {
                errno = EPROTO;
                return -1;
            }
            memcpy(&value, payload + offset, sizeof(value));
            offset += 4u;
            break;
        case 'h':
            if (fd_count >= client->pending_fd_count) {
                errno = EPROTO;
                return -1;
            }
            arguments[argument_index].h = client->pending_fds[fd_count++];
            argument_index++;
            signature++;
            continue;
        case '\0':
            continue;
        default:
            errno = EPROTO;
            return -1;
        }
        switch (*signature) {
        case 'i':
            arguments[argument_index].i = (int32_t)value;
            break;
        case 'u':
            arguments[argument_index].u = value;
            break;
        case 'f':
            arguments[argument_index].f = (wl_fixed_t)value;
            break;
        case 'o': {
            struct wl_resource *object =
                value ? wl_client_get_object(client, value) : NULL;

            if (value != 0u && !object) {
                errno = EPROTO;
                return -1;
            }
            arguments[argument_index].o = (struct wl_object *)object;
            break;
        }
        case 'n':
            if (value == 0u || wl_client_get_object(client, value)) {
                errno = EPROTO;
                return -1;
            }
            arguments[argument_index].n = value;
            break;
        case 's': {
            size_t aligned = wl_wire_align(value);

            if (value == 0u) {
                arguments[argument_index].s = NULL;
                break;
            }
            if (aligned < value || offset > payload_size ||
                aligned > payload_size - offset ||
                payload[offset + value - 1u] != '\0') {
                errno = EPROTO;
                return -1;
            }
            arguments[argument_index].s = (const char *)(payload + offset);
            offset += aligned;
            break;
        }
        case 'a': {
            size_t aligned = wl_wire_align(value);

            if (aligned < value || offset > payload_size ||
                aligned > payload_size - offset) {
                errno = EPROTO;
                return -1;
            }
            arrays[argument_index].size = value;
            arrays[argument_index].alloc = value;
            arrays[argument_index].data = (void *)(payload + offset);
            arguments[argument_index].a = &arrays[argument_index];
            offset += aligned;
            break;
        }
        default:
            break;
        }
        argument_index++;
        signature++;
    }
    if (offset != payload_size) {
        errno = EPROTO;
        return -1;
    }
    result = resource->dispatcher(resource->implementation, resource, opcode,
                                  method, arguments);
    if (fd_count != 0u) {
        memmove(client->pending_fds, client->pending_fds + fd_count,
                (client->pending_fd_count - fd_count) * sizeof(int));
        client->pending_fd_count -= fd_count;
    }
    *fd_count_out = fd_count;
    return result;
}

static int wl_client_dispatch_messages(struct wl_client *client)
{
    while (client->receive_length >= 8u) {
        struct wl_resource *resource;
        uint32_t object_id;
        uint32_t header;
        uint32_t opcode;
        size_t message_size;
        size_t consumed_fds = 0u;

        memcpy(&object_id, client->receive, sizeof(object_id));
        memcpy(&header, client->receive + 4u, sizeof(header));
        opcode = header & 0xffffu;
        message_size = header >> 16;
        if (message_size < 8u || message_size > UINT16_MAX ||
            (message_size & 3u) != 0u) {
            errno = EPROTO;
            return -1;
        }
        if (message_size > client->receive_length)
            return 0;
        resource = wl_client_get_object(client, object_id);
        if (!resource) {
            errno = EPROTO;
            return -1;
        }
        if (wl_client_decode_request(
                client, resource, opcode, client->receive + 8u,
                message_size - 8u, &consumed_fds) < 0)
            return -1;
        (void)consumed_fds;
        memmove(client->receive, client->receive + message_size,
                client->receive_length - message_size);
        client->receive_length -= message_size;
        if (client->destroy_pending)
            return 0;
    }
    return 0;
}

static size_t wl_control_fd_count(struct msghdr *message)
{
    size_t total = 0u;

    for (struct cmsghdr *header = CMSG_FIRSTHDR(message);
         header; header = CMSG_NXTHDR(message, header)) {
        if (header->cmsg_level == SOL_SOCKET &&
            header->cmsg_type == SCM_RIGHTS &&
            header->cmsg_len >= CMSG_LEN(0u))
            total += (header->cmsg_len - CMSG_LEN(0u)) / sizeof(int);
    }
    return total;
}

static void wl_control_close_fds(struct msghdr *message)
{
    for (struct cmsghdr *header = CMSG_FIRSTHDR(message);
         header; header = CMSG_NXTHDR(message, header)) {
        int *descriptors;
        size_t count;

        if (header->cmsg_level != SOL_SOCKET ||
            header->cmsg_type != SCM_RIGHTS ||
            header->cmsg_len < CMSG_LEN(0u))
            continue;
        descriptors = (int *)CMSG_DATA(header);
        count = (header->cmsg_len - CMSG_LEN(0u)) / sizeof(int);
        for (size_t index = 0u; index < count; index++)
            close(descriptors[index]);
    }
}

static int wl_client_receive(struct wl_client *client)
{
    uint8_t control[CMSG_SPACE(WL_SERVER_PENDING_FDS * sizeof(int))];
    struct iovec vector;
    struct msghdr message;
    ssize_t count;

    if (client->receive_length == sizeof(client->receive)) {
        errno = EMSGSIZE;
        return -1;
    }
    memset(&message, 0, sizeof(message));
    memset(control, 0, sizeof(control));
    vector.iov_base = client->receive + client->receive_length;
    vector.iov_len = sizeof(client->receive) - client->receive_length;
    message.msg_iov = &vector;
    message.msg_iovlen = 1u;
    message.msg_control = control;
    message.msg_controllen = sizeof(control);
    count = recvmsg(client->fd, &message, MSG_CMSG_CLOEXEC);
    if (count <= 0)
        return -1;
    if ((message.msg_flags & MSG_CTRUNC) != 0) {
        wl_control_close_fds(&message);
        errno = EMSGSIZE;
        return -1;
    }
    if (wl_control_fd_count(&message) >
        WL_SERVER_PENDING_FDS - client->pending_fd_count) {
        wl_control_close_fds(&message);
        errno = EMSGSIZE;
        return -1;
    }
    for (struct cmsghdr *header = CMSG_FIRSTHDR(&message);
         header; header = CMSG_NXTHDR(&message, header)) {
        size_t bytes;
        size_t descriptors;

        if (header->cmsg_level != SOL_SOCKET ||
            header->cmsg_type != SCM_RIGHTS ||
            header->cmsg_len < CMSG_LEN(0u))
            continue;
        bytes = header->cmsg_len - CMSG_LEN(0u);
        descriptors = bytes / sizeof(int);
        memcpy(client->pending_fds + client->pending_fd_count,
               CMSG_DATA(header), descriptors * sizeof(int));
        client->pending_fd_count += descriptors;
    }
    client->receive_length += (size_t)count;
    return wl_client_dispatch_messages(client);
}

static int wl_client_event(int fd, uint32_t mask, void *data)
{
    struct wl_client *client = data;
    int result = 0;

    (void)fd;
    if (!client || client->destroying)
        return -1;
    client->dispatching = true;
    if ((mask & WL_EVENT_READABLE) != 0u)
        result = wl_client_receive(client);
    if ((mask & (WL_EVENT_HANGUP | WL_EVENT_ERROR)) != 0u)
        result = -1;
    client->dispatching = false;
    if (result < 0 || client->destroy_pending) {
        wl_client_destroy(client);
        return -1;
    }
    return 0;
}

static uint32_t wl_display_next_serial(struct wl_display *display)
{
    display->serial++;
    if (display->serial == 0u)
        display->serial++;
    return display->serial;
}

static int wl_display_resource_dispatch(
    const void *implementation, void *target, uint32_t opcode,
    const struct wl_message *message, union wl_argument *arguments)
{
    struct wl_resource *display_resource = target;
    struct wl_client *client = wl_resource_get_client(display_resource);
    struct wl_display *display = wl_client_get_display(client);
    struct wl_resource *resource;
    uint32_t id;

    (void)implementation;
    (void)message;
    if (!client || !display)
        return -1;
    id = arguments[0].n;
    switch (opcode) {
    case 0u:
        resource = wl_resource_create(
            client, &wl_server_callback_interface, 1, id);
        if (!resource)
            return -1;
        wl_resource_post_event(resource, 0u,
                               wl_display_next_serial(display));
        wl_resource_destroy(resource);
        return 0;
    case 1u:
        resource = wl_resource_create(
            client, &wl_server_registry_interface, 1, id);
        if (!resource)
            return -1;
        wl_resource_set_dispatcher(
            resource, wl_registry_resource_dispatch, NULL, display, NULL);
        for (struct wl_global *global = display->globals;
             global; global = global->next)
            wl_resource_post_event(resource, 0u, global->name,
                                   global->interface->name,
                                   global->version);
        return 0;
    default:
        return -1;
    }
}

static int wl_registry_resource_dispatch(
    const void *implementation, void *target, uint32_t opcode,
    const struct wl_message *message, union wl_argument *arguments)
{
    struct wl_resource *registry_resource = target;
    struct wl_client *client = wl_resource_get_client(registry_resource);
    struct wl_display *display = wl_client_get_display(client);
    struct wl_global *global;
    uint32_t name;
    uint32_t version;
    uint32_t id;

    (void)implementation;
    (void)message;
    if (!client || !display || opcode != 0u)
        return -1;
    name = arguments[0].u;
    version = arguments[2].u;
    id = arguments[3].n;
    for (global = display->globals; global; global = global->next) {
        if (global->name == name)
            break;
    }
    if (!global || !arguments[1].s ||
        strcmp(arguments[1].s, global->interface->name) != 0 ||
        version == 0u || version > global->version)
        return -1;
    global->bind(client, global->data, version, id);
    if (!wl_client_get_object(client, id))
        return -1;
    return 0;
}

static int wl_display_accept_event(int fd, uint32_t mask, void *data)
{
    struct wl_display *display = data;
    int client_fd;

    if (!display || (mask & (WL_EVENT_HANGUP | WL_EVENT_ERROR)) != 0u)
        return -1;
    if ((mask & WL_EVENT_READABLE) == 0u)
        return 0;
    client_fd = accept(fd, NULL, NULL);
    if (client_fd < 0)
        return errno == EINTR ? 0 : -1;
    if (!wl_client_create(display, client_fd)) {
        close(client_fd);
        return 0;
    }
    return 0;
}

static int wl_resource_event_size(const char *signature,
                                  union wl_argument *arguments,
                                  size_t *size_out, size_t *fd_count_out)
{
    size_t argument_index = 0u;
    size_t size = 8u;
    size_t fd_count = 0u;

    while (signature && *signature) {
        size_t addition = 0u;

        signature = wl_signature_next(signature);
        switch (*signature) {
        case 'i':
        case 'u':
        case 'f':
        case 'o':
        case 'n':
            addition = 4u;
            break;
        case 's':
            addition = 4u;
            if (arguments[argument_index].s)
                addition += wl_wire_align(
                    strlen(arguments[argument_index].s) + 1u);
            break;
        case 'a':
            addition = 4u;
            if (arguments[argument_index].a)
                addition += wl_wire_align(arguments[argument_index].a->size);
            break;
        case 'h':
            fd_count++;
            break;
        case '\0':
            continue;
        default:
            errno = EINVAL;
            return -1;
        }
        if (addition > UINT16_MAX || size > UINT16_MAX - addition) {
            errno = EMSGSIZE;
            return -1;
        }
        size += addition;
        argument_index++;
        signature++;
    }
    *size_out = size;
    *fd_count_out = fd_count;
    return 0;
}

static void wl_wire_store_u32(uint8_t *destination, uint32_t value)
{
    memcpy(destination, &value, sizeof(value));
}

static int wl_resource_send_message(struct wl_client *client,
                                    const uint8_t *message, size_t size,
                                    const int *fds, size_t fd_count)
{
    struct msghdr header;
    struct iovec vector;
    uint8_t control[CMSG_SPACE(16u * sizeof(int))];
    size_t sent = 0u;

    if (fd_count > 16u) {
        errno = EMSGSIZE;
        return -1;
    }
    memset(&header, 0, sizeof(header));
    memset(control, 0, sizeof(control));
    vector.iov_base = (void *)message;
    vector.iov_len = size;
    header.msg_iov = &vector;
    header.msg_iovlen = 1u;
    if (fd_count != 0u) {
        struct cmsghdr *control_header;

        header.msg_control = control;
        header.msg_controllen = CMSG_SPACE(fd_count * sizeof(int));
        control_header = CMSG_FIRSTHDR(&header);
        control_header->cmsg_level = SOL_SOCKET;
        control_header->cmsg_type = SCM_RIGHTS;
        control_header->cmsg_len = CMSG_LEN(fd_count * sizeof(int));
        memcpy(CMSG_DATA(control_header), fds, fd_count * sizeof(int));
    }
    while (sent < size) {
        ssize_t result;

        vector.iov_base = (void *)(message + sent);
        vector.iov_len = size - sent;
        result = sendmsg(client->fd, &header, 0);
        if (result < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        if (result == 0) {
            errno = EPIPE;
            return -1;
        }
        sent += (size_t)result;
        header.msg_control = NULL;
        header.msg_controllen = 0u;
    }
    return 0;
}

void wl_resource_post_event_array(struct wl_resource *resource,
                                  uint32_t opcode,
                                  union wl_argument *arguments)
{
    const struct wl_message *event;
    const char *signature;
    uint8_t *message;
    int fds[16];
    size_t size;
    size_t fd_count;
    size_t argument_index = 0u;
    size_t fd_index = 0u;
    size_t offset = 8u;

    if (!resource || resource->destroying || !resource->client ||
        !resource->interface || opcode >=
        (uint32_t)resource->interface->event_count) {
        errno = EINVAL;
        return;
    }
    event = &resource->interface->events[opcode];
    signature = event->signature;
    if (wl_resource_event_size(signature, arguments, &size, &fd_count) < 0)
        return;
    message = calloc(1u, size);
    if (!message)
        return;
    wl_wire_store_u32(message, resource->id);
    wl_wire_store_u32(message + 4u, ((uint32_t)size << 16) | opcode);
    while (signature && *signature) {
        uint32_t value;

        signature = wl_signature_next(signature);
        switch (*signature) {
        case 'i':
            value = (uint32_t)arguments[argument_index].i;
            wl_wire_store_u32(message + offset, value);
            offset += 4u;
            break;
        case 'u':
            wl_wire_store_u32(message + offset,
                              arguments[argument_index].u);
            offset += 4u;
            break;
        case 'f':
            wl_wire_store_u32(message + offset,
                              (uint32_t)arguments[argument_index].f);
            offset += 4u;
            break;
        case 'o': {
            struct wl_resource *object =
                (struct wl_resource *)arguments[argument_index].o;

            wl_wire_store_u32(message + offset,
                              object ? object->id : 0u);
            offset += 4u;
            break;
        }
        case 'n':
            wl_wire_store_u32(message + offset,
                              arguments[argument_index].n);
            offset += 4u;
            break;
        case 's': {
            const char *string = arguments[argument_index].s;
            uint32_t length = string ? (uint32_t)strlen(string) + 1u : 0u;

            wl_wire_store_u32(message + offset, length);
            offset += 4u;
            if (length != 0u) {
                memcpy(message + offset, string, length);
                offset += wl_wire_align(length);
            }
            break;
        }
        case 'a': {
            struct wl_array *array = arguments[argument_index].a;
            uint32_t length = array ? (uint32_t)array->size : 0u;

            wl_wire_store_u32(message + offset, length);
            offset += 4u;
            if (length != 0u) {
                memcpy(message + offset, array->data, length);
                offset += wl_wire_align(length);
            }
            break;
        }
        case 'h':
            fds[fd_index++] = arguments[argument_index].h;
            break;
        case '\0':
            continue;
        default:
            free(message);
            errno = EINVAL;
            return;
        }
        argument_index++;
        signature++;
    }
    (void)wl_resource_send_message(resource->client, message, size,
                                   fds, fd_count);
    free(message);
}

void wl_resource_post_event(struct wl_resource *resource, uint32_t opcode,
                            ...)
{
    union wl_argument arguments[32];
    const char *signature;
    size_t count = 0u;
    va_list values;

    if (!resource || resource->destroying || !resource->interface ||
        opcode >= (uint32_t)resource->interface->event_count) {
        errno = EINVAL;
        return;
    }
    signature = resource->interface->events[opcode].signature;
    va_start(values, opcode);
    while (signature && *signature && count < 32u) {
        signature = wl_signature_next(signature);
        switch (*signature) {
        case 'i':
        case 'f':
            arguments[count].i = va_arg(values, int32_t);
            break;
        case 'u':
            arguments[count].u = va_arg(values, uint32_t);
            break;
        case 'n': {
            struct wl_resource *new_resource =
                va_arg(values, struct wl_resource *);

            arguments[count].n = new_resource ? new_resource->id : 0u;
            break;
        }
        case 's':
            arguments[count].s = va_arg(values, const char *);
            break;
        case 'o':
            arguments[count].o = va_arg(values, struct wl_object *);
            break;
        case 'a':
            arguments[count].a = va_arg(values, struct wl_array *);
            break;
        case 'h':
            arguments[count].h = va_arg(values, int);
            break;
        case '\0':
            continue;
        default:
            va_end(values);
            errno = EINVAL;
            return;
        }
        count++;
        signature++;
    }
    va_end(values);
    if (signature && *signature) {
        errno = E2BIG;
        return;
    }
    wl_resource_post_event_array(resource, opcode, arguments);
}
