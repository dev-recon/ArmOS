/*
 * ArmOS
 * Copyright (c) 2026 Mohamed Ennassiri
 *
 * Licensed under the Apache License, Version 2.0.
 * See LICENSE for details.
 *
 * File: userland/programs/armos-wlcomp/main.c
 * Layer: Userland / graphical services
 *
 * Responsibilities:
 * - Own the Wayland local socket and accept bounded client connections.
 * - Drive protocol dispatch and framebuffer presentation through poll(2).
 * - Provide a headless mode for deterministic protocol validation.
 * - Support silent supervised startup without writing over shell prompts.
 *
 * Notes:
 * - The compositor is a root userland service, not a kernel subsystem.
 * - Platform-specific display details remain behind /dev/fb0.
 */

#include "armos_wlcomp.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#define WL_FRAME_INTERVAL_MS 16u

static void wl_server_usage(const char *program)
{
    fprintf(stderr,
            "usage: %s [--headless] [--quiet] [--socket path]\n", program);
}

static int wl_server_open_socket(const char *path)
{
    struct sockaddr_un address;
    int fd;

    if (!path || strlen(path) >= sizeof(address.sun_path)) {
        errno = ENAMETOOLONG;
        return -1;
    }
    fd = socket(AF_LOCAL, SOCK_STREAM, 0);
    if (fd < 0)
        return -1;
    memset(&address, 0, sizeof(address));
    address.sun_family = AF_LOCAL;
    memcpy(address.sun_path, path, strlen(path) + 1u);
    if (bind(fd, (struct sockaddr *)&address, sizeof(address)) < 0 ||
        listen(fd, (int)WL_SERVER_MAX_CLIENTS) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

static struct wl_server_client *wl_server_free_client(
    struct wl_server *server)
{
    for (size_t index = 0; index < WL_SERVER_MAX_CLIENTS; index++) {
        if (!server->clients[index].used)
            return &server->clients[index];
    }
    return NULL;
}

static int wl_server_accept_client(struct wl_server *server)
{
    struct wl_server_client *client;
    int fd = accept(server->listen_fd, NULL, NULL);

    if (fd < 0)
        return -1;
    client = wl_server_free_client(server);
    if (!client) {
        close(fd);
        errno = EMFILE;
        return -1;
    }
    memset(client, 0, sizeof(*client));
    client->used = true;
    client->fd = fd;
    client->next_server_id = 0xff000000u;
    if (wl_client_add_object(client, WL_DISPLAY_ID, WL_SERVER_OBJECT_DISPLAY,
                             1u, NULL) < 0) {
        wl_server_disconnect_client(server, client);
        return -1;
    }
    return 0;
}

static int wl_server_run(struct wl_server *server)
{
    struct pollfd descriptors[2u + WL_SERVER_MAX_CLIENTS];
    struct wl_server_client *owners[2u + WL_SERVER_MAX_CLIENTS];
    uint64_t next_frame_ms = 0;
    bool render_pending = false;

    for (;;) {
        nfds_t count = 2u;
        struct timespec now;
        uint64_t now_ms;
        int poll_timeout = -1;

        if (clock_gettime(CLOCK_MONOTONIC, &now) < 0)
            return -1;
        now_ms = (uint64_t)now.tv_sec * 1000u +
                 (uint64_t)now.tv_nsec / 1000000u;
        if (render_pending) {
            if (now_ms >= next_frame_ms) {
                if (wl_renderer_compose(server) < 0)
                    return -1;
                render_pending = false;
                next_frame_ms = now_ms + WL_FRAME_INTERVAL_MS;
            } else {
                poll_timeout = (int)(next_frame_ms - now_ms);
            }
        }

        memset(descriptors, 0, sizeof(descriptors));
        memset(owners, 0, sizeof(owners));
        descriptors[0].fd = server->listen_fd;
        descriptors[0].events = POLLIN;
        descriptors[1].fd = server->input_fd;
        descriptors[1].events = POLLIN;
        for (size_t index = 0; index < WL_SERVER_MAX_CLIENTS; index++) {
            if (!server->clients[index].used)
                continue;
            descriptors[count].fd = server->clients[index].fd;
            descriptors[count].events = POLLIN;
            owners[count] = &server->clients[index];
            count++;
        }

        if (poll(descriptors, count, poll_timeout) < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        if ((descriptors[0].revents & POLLIN) != 0)
            (void)wl_server_accept_client(server);
        if ((descriptors[1].revents & POLLIN) != 0) {
            int input_result = wl_server_handle_input(server);

            if (input_result < 0)
                return -1;
            if (input_result > 0)
                render_pending = true;
        }
        for (nfds_t index = 2u; index < count; index++) {
            short events = descriptors[index].revents;

            if (events == 0)
                continue;
            if ((events & POLLIN) == 0 ||
                wl_server_receive_client(server, owners[index]) < 0) {
                wl_server_disconnect_client(server, owners[index]);
                (void)wl_renderer_compose(server);
            }
        }
    }
}

int main(int argc, char **argv)
{
    static struct wl_server server;
    const char *socket_path = ARMOS_WLCOMP_SOCKET_PATH;
    bool headless = false;
    bool quiet = false;

    (void)signal(SIGPIPE, SIG_IGN);
    for (int index = 1; index < argc; index++) {
        if (strcmp(argv[index], "--headless") == 0) {
            headless = true;
        } else if (strcmp(argv[index], "--quiet") == 0) {
            quiet = true;
        } else if (strcmp(argv[index], "--socket") == 0 &&
                   index + 1 < argc) {
            socket_path = argv[++index];
        } else {
            wl_server_usage(argv[0]);
            return 2;
        }
    }

    memset(&server, 0, sizeof(server));
    server.listen_fd = -1;
    server.input_fd = -1;
    for (size_t index = 0; index < WL_SERVER_MAX_CLIENTS; index++)
        server.clients[index].fd = -1;
    if (wl_renderer_init(&server.renderer, headless) < 0) {
        perror("armos-wlcomp: renderer");
        return 1;
    }
    server.pointer_x = (int32_t)server.renderer.framebuffer.width / 2;
    server.pointer_y = (int32_t)server.renderer.framebuffer.height / 2;
    if (!headless) {
        server.input_fd = open("/dev/input0", O_RDONLY | O_NONBLOCK, 0);
        if (server.input_fd < 0) {
            perror("armos-wlcomp: input");
            wl_renderer_destroy(&server.renderer);
            return 1;
        }
    }
    server.listen_fd = wl_server_open_socket(socket_path);
    if (server.listen_fd < 0) {
        perror("armos-wlcomp: socket");
        if (server.input_fd >= 0)
            close(server.input_fd);
        wl_renderer_destroy(&server.renderer);
        return 1;
    }
    if (wl_renderer_compose(&server) < 0) {
        perror("armos-wlcomp: initial frame");
        close(server.listen_fd);
        wl_renderer_destroy(&server.renderer);
        return 1;
    }

    if (!quiet) {
        printf("armos-wlcomp: ready on %s (%ux%u%s)\n", socket_path,
               (unsigned)server.renderer.framebuffer.width,
               (unsigned)server.renderer.framebuffer.height,
               headless ? ", headless" : "");
    }
    if (wl_server_run(&server) < 0)
        perror("armos-wlcomp: event loop");
    close(server.listen_fd);
    if (server.input_fd >= 0)
        close(server.input_fd);
    wl_renderer_destroy(&server.renderer);
    return 1;
}
