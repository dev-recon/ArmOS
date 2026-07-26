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
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <wayland-server-core.h>

struct wl_display {
    int listen_fd;
    char socket_path[sizeof(((struct sockaddr_un *)0)->sun_path)];
};

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
    if (!display)
        return;
    if (display->listen_fd >= 0)
        close(display->listen_fd);
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
