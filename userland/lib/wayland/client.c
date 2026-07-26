/*
 * ArmOS
 * Copyright (c) 2026 Mohamed Ennassiri
 *
 * Licensed under the Apache License, Version 2.0.
 * See LICENSE for details.
 *
 * File: userland/lib/wayland/client.c
 * Layer: Userland / Wayland client runtime
 *
 * Responsibilities:
 * - Open and own a client connection to an ArmOS Wayland compositor.
 * - Expose the initial display and proxy inspection API.
 * - Resolve conventional WAYLAND_DISPLAY and XDG_RUNTIME_DIR names.
 *
 * Notes:
 * - Wire marshaling and event queues are layered onto this connection object.
 * - This is an original ArmOS implementation of the public Wayland API.
 */

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <wayland-client-core.h>

struct wl_proxy {
    uint32_t id;
    uint32_t version;
    const struct wl_interface *interface;
    void *user_data;
};

struct wl_display {
    struct wl_proxy proxy;
    int fd;
    int error;
};

static int wl_client_socket_path(const char *name, char *path, size_t size)
{
    const char *display_name = name;
    const char *runtime;
    int count;

    if (!display_name || display_name[0] == '\0')
        display_name = getenv("WAYLAND_DISPLAY");
    if (!display_name || display_name[0] == '\0')
        display_name = "wayland-0";
    if (display_name[0] == '/') {
        count = snprintf(path, size, "%s", display_name);
    } else {
        runtime = getenv("XDG_RUNTIME_DIR");
        if (!runtime || runtime[0] == '\0')
            runtime = "/tmp";
        count = snprintf(path, size, "%s/%s", runtime, display_name);
    }
    return count >= 0 && (size_t)count < size ? 0 : -1;
}

struct wl_display *wl_display_connect(const char *name)
{
    struct sockaddr_un address;
    struct wl_display *display;
    char path[sizeof(address.sun_path)];
    int fd;

    if (wl_client_socket_path(name, path, sizeof(path)) < 0) {
        errno = ENAMETOOLONG;
        return NULL;
    }
    fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0)
        return NULL;
    if (fcntl(fd, F_SETFD, FD_CLOEXEC) < 0) {
        close(fd);
        return NULL;
    }

    memset(&address, 0, sizeof(address));
    address.sun_family = AF_UNIX;
    memcpy(address.sun_path, path, strlen(path) + 1u);
    if (connect(fd, (struct sockaddr *)&address, sizeof(address)) < 0) {
        close(fd);
        return NULL;
    }

    display = calloc(1, sizeof(*display));
    if (!display) {
        close(fd);
        return NULL;
    }
    display->proxy.id = 1u;
    display->proxy.version = 1u;
    display->fd = fd;
    return display;
}

void wl_display_disconnect(struct wl_display *display)
{
    if (!display)
        return;
    if (display->fd >= 0)
        close(display->fd);
    free(display);
}

int wl_display_get_fd(struct wl_display *display)
{
    if (!display) {
        errno = EINVAL;
        return -1;
    }
    return display->fd;
}

int wl_display_get_error(struct wl_display *display)
{
    return display ? display->error : EINVAL;
}

int wl_display_flush(struct wl_display *display)
{
    if (!display || display->fd < 0) {
        errno = EINVAL;
        return -1;
    }
    return 0;
}

void wl_proxy_set_user_data(struct wl_proxy *proxy, void *user_data)
{
    if (proxy)
        proxy->user_data = user_data;
}

void *wl_proxy_get_user_data(struct wl_proxy *proxy)
{
    return proxy ? proxy->user_data : NULL;
}

uint32_t wl_proxy_get_id(struct wl_proxy *proxy)
{
    return proxy ? proxy->id : 0u;
}

uint32_t wl_proxy_get_version(struct wl_proxy *proxy)
{
    return proxy ? proxy->version : 0u;
}

const char *wl_proxy_get_class(struct wl_proxy *proxy)
{
    return proxy && proxy->interface ? proxy->interface->name : "wl_display";
}
