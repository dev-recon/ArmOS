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
 * - Drive protocol dispatch through the shared Wayland server event loop.
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

#define WL_SERVER_FRAME_INTERVAL_MS 16

static int wl_server_client_event(int fd, uint32_t mask, void *data);

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
    client->event_source = wl_event_loop_add_fd(
        server->event_loop, client->fd,
        WL_EVENT_READABLE | WL_EVENT_HANGUP | WL_EVENT_ERROR,
        wl_server_client_event, server);
    if (!client->event_source) {
        wl_server_disconnect_client(server, client);
        return -1;
    }
    return 0;
}

static int wl_server_listen_event(int fd, uint32_t mask, void *data)
{
    struct wl_server *server = data;

    (void)fd;
    if ((mask & (WL_EVENT_HANGUP | WL_EVENT_ERROR)) != 0u) {
        server->fatal_error = true;
        return -1;
    }
    if ((mask & WL_EVENT_READABLE) != 0u)
        (void)wl_server_accept_client(server);
    return 0;
}

static int wl_server_render_event(void *data)
{
    struct wl_server *server = data;
    int result;

    server->render_pending = false;
    /*
     * A scene update may cover several surfaces (focus, stacking, removal).
     * It must win over a queued local update; the full composition consumes
     * the latter as well.
     */
    if (server->scene_damage_pending) {
        result = wl_renderer_compose(server);
        if (result == 0)
            server->scene_damage_pending = false;
    } else if (server->damage_pending) {
        result = wl_renderer_compose_damage(server);
    } else {
        result = wl_renderer_compose_pointer(server);
    }
    if (result < 0) {
        server->fatal_error = true;
        return -1;
    }
    return 0;
}

int wl_server_schedule_render(struct wl_server *server, bool scene_damage)
{
    if (!server || !server->render_timer)
        return -1;
    if (scene_damage)
        server->scene_damage_pending = true;
    if (server->render_pending)
        return 0;
    server->render_pending = true;
    if (wl_event_source_timer_update(server->render_timer,
                                     WL_SERVER_FRAME_INTERVAL_MS) < 0) {
        server->render_pending = false;
        return -1;
    }
    return 0;
}

static int wl_server_input_event(int fd, uint32_t mask, void *data)
{
    struct wl_server *server = data;
    int result;

    (void)fd;
    if ((mask & (WL_EVENT_HANGUP | WL_EVENT_ERROR)) != 0u) {
        server->fatal_error = true;
        return -1;
    }
    if ((mask & WL_EVENT_READABLE) == 0u)
        return 0;
    result = wl_server_handle_input(server);
    if (result < 0) {
        server->fatal_error = true;
        return -1;
    }
    if (result > 0 && wl_server_schedule_render(server, false) < 0) {
        server->fatal_error = true;
        return -1;
    }
    return 0;
}

static int wl_server_client_event(int fd, uint32_t mask, void *data)
{
    struct wl_server *server = data;
    struct wl_server_client *client = NULL;

    for (size_t index = 0u; index < WL_SERVER_MAX_CLIENTS; index++) {
        if (server->clients[index].used &&
            server->clients[index].fd == fd) {
            client = &server->clients[index];
            break;
        }
    }
    if (!client)
        return -1;
    if ((mask & WL_EVENT_READABLE) == 0u ||
        wl_server_receive_client(server, client) < 0 ||
        (mask & (WL_EVENT_HANGUP | WL_EVENT_ERROR)) != 0u) {
        wl_server_disconnect_client(server, client);
        if (wl_renderer_compose(server) < 0)
            server->fatal_error = true;
        return -1;
    }
    return 0;
}

static int wl_server_run(struct wl_server *server)
{
    server->event_display = wl_display_create();
    if (!server->event_display)
        return -1;
    server->event_loop = wl_display_get_event_loop(server->event_display);
    server->listen_source = wl_event_loop_add_fd(
        server->event_loop, server->listen_fd,
        WL_EVENT_READABLE | WL_EVENT_HANGUP | WL_EVENT_ERROR,
        wl_server_listen_event, server);
    server->render_timer = wl_event_loop_add_timer(
        server->event_loop, wl_server_render_event, server);
    if (!server->event_loop || !server->listen_source ||
        !server->render_timer)
        return -1;
    if (server->input_fd >= 0) {
        server->input_source = wl_event_loop_add_fd(
            server->event_loop, server->input_fd,
            WL_EVENT_READABLE | WL_EVENT_HANGUP | WL_EVENT_ERROR,
            wl_server_input_event, server);
        if (!server->input_source)
            return -1;
    }
    while (!server->fatal_error) {
        if (wl_event_loop_dispatch(server->event_loop, -1) < 0 &&
            errno != EINTR)
            return -1;
    }
    return -1;
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
        if (server.input_fd >= 0)
            close(server.input_fd);
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
    for (size_t index = 0u; index < WL_SERVER_MAX_CLIENTS; index++) {
        if (server.clients[index].used)
            wl_server_disconnect_client(&server, &server.clients[index]);
    }
    wl_display_destroy(server.event_display);
    close(server.listen_fd);
    if (server.input_fd >= 0)
        close(server.input_fd);
    wl_renderer_destroy(&server.renderer);
    return 1;
}
