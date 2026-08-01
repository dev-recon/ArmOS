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
 * - Launch an initial Foot terminal without tying compositor lifetime to it.
 * - Provide a headless mode for deterministic protocol validation.
 * - Support silent supervised startup without writing over shell prompts.
 *
 * Notes:
 * - The compositor is a root userland service, not a kernel subsystem.
 * - Platform-specific display details remain behind /dev/fb0.
 */

#include "armos_wlcomp.h"
#include <armos/spawn.h>
#include "gpu_present.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static volatile sig_atomic_t wl_child_changed;

static int wl_server_client_event(int fd, uint32_t mask, void *data);

static void wl_server_client_dispatch_idle(void *data);

int wl_server_defer_client_dispatch(struct wl_server_client *client)
{
    if (!client || !client->server)
        return -1;
    if (client->dispatch_idle)
        return 0;
    client->dispatch_idle = wl_event_loop_add_idle(
        client->server->event_loop, wl_server_client_dispatch_idle, client);
    return client->dispatch_idle ? 0 : -1;
}

static uint64_t wl_monotonic_us(void)
{
    struct timespec now;

    if (clock_gettime(CLOCK_MONOTONIC, &now) < 0)
        return 0u;
    return (uint64_t)now.tv_sec * 1000000u +
           (uint64_t)now.tv_nsec / 1000u;
}

static void wl_server_profile_startup(const struct wl_server *server,
                                      bool enabled, const char *phase)
{
    uint64_t now_us;

    if (!server || !enabled || !phase || server->startup_started_us == 0u)
        return;
    now_us = wl_monotonic_us();
    if (now_us >= server->startup_started_us)
        fprintf(stderr, "armos-wlcomp: startup %s=%llums\n", phase,
                (unsigned long long)(
                    (now_us - server->startup_started_us) / 1000u));
}

static void wl_server_profile_frame(struct wl_server *server,
                                    uint64_t started_us)
{
    struct wl_server_renderer *renderer = &server->renderer;
    struct wl_render_profile primitives;
    uint64_t now_us;
    uint64_t compose_us;

    if (!renderer->profile_enabled || started_us == 0u)
        return;
    now_us = wl_monotonic_us();
    if (now_us == 0u || now_us < started_us)
        return;
    server->profile_render_us += now_us - started_us;
    server->profile_frames++;
    if (server->profile_started_us == 0u)
        server->profile_started_us = now_us;
    if (now_us - server->profile_started_us < 1000000u)
        return;
    wl_render_profile_take(&primitives);
    compose_us = server->profile_render_us;
    /* GPU presentation completes in a separate event-loop transaction. */
    if (renderer->output_backend != WL_RENDERER_OUTPUT_GPU &&
        compose_us >= renderer->profile_present_us)
        compose_us -= renderer->profile_present_us;
    fprintf(stderr,
            "WLPROFILE frames=%llu compose_avg_us=%llu "
            "present_avg_us=%llu present_pixels=%llu "
            "gpu_imports_total=%llu gpu_direct_blits=%llu "
            "gpu_fenced_releases=%llu gpu_immediate_releases=%llu "
            "fill_pixels=%llu copy_pixels=%llu blend_pixels=%llu\n",
            (unsigned long long)server->profile_frames,
            (unsigned long long)(compose_us / server->profile_frames),
            (unsigned long long)(renderer->profile_present_us /
                                 server->profile_frames),
            (unsigned long long)renderer->profile_present_pixels,
            (unsigned long long)renderer->profile_gpu_imports,
            (unsigned long long)renderer->profile_gpu_direct_blits,
            (unsigned long long)renderer->profile_gpu_fenced_releases,
            (unsigned long long)renderer->profile_gpu_immediate_releases,
            (unsigned long long)primitives.fill_pixels,
            (unsigned long long)primitives.copy_pixels,
            (unsigned long long)primitives.blend_pixels);
    server->profile_started_us = now_us;
    server->profile_render_us = 0u;
    server->profile_frames = 0u;
    renderer->profile_present_us = 0u;
    renderer->profile_present_pixels = 0u;
    renderer->profile_gpu_direct_blits = 0u;
    renderer->profile_gpu_fenced_releases = 0u;
    renderer->profile_gpu_immediate_releases = 0u;
}

static void wl_server_child_signal(int signal_number)
{
    (void)signal_number;
    wl_child_changed = 1;
}

static void wl_server_usage(const char *program)
{
    fprintf(stderr,
            "usage: %s [--headless] [--quiet] [--profile] "
            "[--socket path]\n", program);
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
    int flags;

    if (fd < 0)
        return -1;
    flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
        close(fd);
        return -1;
    }
    client = wl_server_free_client(server);
    if (!client) {
        close(fd);
        errno = EMFILE;
        return -1;
    }
    memset(client, 0, sizeof(*client));
    client->used = true;
    client->fd = fd;
    client->server = server;
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
    uint64_t frame_started_us = wl_monotonic_us();
    uint64_t profile_started_us = server->renderer.profile_enabled ?
        frame_started_us : 0u;
    int result;

    server->render_pending = false;
    /*
     * Drain pending key releases before composing. Software presentation is
     * synchronous; GPU presentation completes later through its fence source,
     * but command generation can still be a substantial non-preemptible user
     * workload for this event-loop turn.
     */
    if (server->input_fd >= 0) {
        result = wl_server_handle_input(server);
        if (result < 0) {
            server->fatal_error = true;
            return -1;
        }
    }
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
    } else if (!server->pointer_presented ||
               server->presented_pointer_x != server->pointer_x ||
               server->presented_pointer_y != server->pointer_y ||
               server->presented_pointer_cursor !=
                   server->pointer_cursor) {
        result = wl_renderer_compose_pointer(server);
    } else {
        result = 0;
    }
    if (result < 0) {
        server->fatal_error = true;
        return -1;
    }
    /*
     * Software frames are complete synchronously.  GPU callbacks tagged with
     * a non-zero presentation serial are completed by their output fence;
     * only callbacks not associated with a submitted GPU frame remain here.
     */
    if (wl_server_complete_frame_callbacks(server, 0u) < 0) {
        server->fatal_error = true;
        return -1;
    }
    wl_server_profile_frame(server, profile_started_us);
    {
        /*
         * Frame pacing is anchored to the start of composition.  Anchoring
         * it to completion would add a second frame interval after every
         * expensive frame (16 ms rendering + 16 ms sleep ~= 30 Hz).  A frame
         * that overruns its deadline is therefore eligible for the short
         * overdue coalescing window in wl_server_schedule_render().
         */
        if (frame_started_us != 0u)
            server->next_frame_us =
                frame_started_us + WL_RENDER_FRAME_INTERVAL_US;
    }
    return 0;
}

int wl_server_schedule_render(struct wl_server *server, bool scene_damage)
{
    uint64_t now_us;
    uint64_t delay_us;
    int delay_ms;

    if (!server || !server->render_timer)
        return -1;
    if (scene_damage)
        server->scene_damage_pending = true;
    if (server->render_pending)
        return 0;
    server->render_pending = true;
    /*
     * One GPU frame may be awaiting its explicit completion fence. Preserve
     * the accumulated damage without arming a polling timer; the fence event
     * schedules the next frame once the output buffer is reusable.
     */
    if (wl_gpu_presenter_pending(server->renderer.gpu_presenter) &&
        !wl_gpu_presenter_can_submit(server->renderer.gpu_presenter))
        return 0;
    now_us = wl_monotonic_us();
    if (now_us == 0u || server->next_frame_us == 0u ||
        now_us >= server->next_frame_us) {
        /*
         * Keep a short batching window even after a missed deadline. An
         * immediate timer can otherwise run between two clients becoming
         * readable, causing separate Foot and animated-client compositions.
         * This retains the old input/damage coalescing property without
         * reintroducing a cumulative 16 ms delay.
         */
        delay_ms = WL_RENDER_OVERDUE_COALESCE_MS;
    } else {
        delay_us = server->next_frame_us - now_us;
        delay_ms = (int)((delay_us + 999u) / 1000u);
    }
    if (wl_event_source_timer_update(server->render_timer, delay_ms) < 0) {
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
    if (result > 0 &&
        (server->scene_damage_pending || server->damage_pending ||
         !server->pointer_presented ||
         server->presented_pointer_x != server->pointer_x ||
         server->presented_pointer_y != server->pointer_y ||
         server->presented_pointer_cursor !=
             server->pointer_cursor) &&
        wl_server_schedule_render(server, false) < 0) {
        server->fatal_error = true;
        return -1;
    }
    return 0;
}

static int wl_server_client_event(int fd, uint32_t mask, void *data)
{
    struct wl_server *server = data;
    struct wl_server_client *client = NULL;
    int result;

    for (size_t index = 0u; index < WL_SERVER_MAX_CLIENTS; index++) {
        if (server->clients[index].used &&
            server->clients[index].fd == fd) {
            client = &server->clients[index];
            break;
        }
    }
    if (!client)
        return -1;
    result = 0;
    if ((mask & WL_EVENT_READABLE) != 0u)
        result = wl_server_receive_client(server, client);
    if (result >= 0 && (mask & WL_EVENT_WRITABLE) != 0u)
        result = wl_client_flush_output(client);
    if (result < 0 ||
        (mask & (WL_EVENT_HANGUP | WL_EVENT_ERROR)) != 0u) {
        wl_server_disconnect_client(server, client);
        /*
         * A client may disappear without completing its Wayland teardown
         * (SIGKILL, crash, broken pipe).  Its resources are already detached
         * above; repaint the remaining scene on the normal frame boundary.
         * A client failure must never become a compositor failure.
         */
        if (wl_server_schedule_render(server, true) < 0)
            fprintf(stderr,
                    "armos-wlcomp: cannot schedule client removal repaint\n");
        return 0;
    }
    if (result > 0 && wl_server_defer_client_dispatch(client) < 0) {
        wl_server_disconnect_client(server, client);
        return 0;
    }
    return 0;
}

static void wl_server_client_dispatch_idle(void *data)
{
    struct wl_server_client *client = data;
    struct wl_server *server;
    int result;

    if (!client || !client->used || !(server = client->server))
        return;
    client->dispatch_idle = NULL;
    result = wl_server_dispatch_client_pending(server, client);
    if (result < 0) {
        wl_server_disconnect_client(server, client);
        return;
    }
    if (result > 0 && wl_server_defer_client_dispatch(client) < 0)
        wl_server_disconnect_client(server, client);
}

static pid_t wl_server_launch_terminal(void)
{
    static char *const argv[] = {"foot", NULL};
    static char *const envp[] = {
        "PATH=/sbin:/bin:/usr/bin",
        "HOME=/home/user",
        "USER=user",
        "LOGNAME=user",
        "LANG=C.UTF-8",
        "SHELL=/sbin/mash",
        "MASH_PROTECT=1",
        "WAYLAND_DISPLAY=wayland-0",
        "XDG_RUNTIME_DIR=/tmp",
        NULL
    };
    armos_spawn_attributes_t attributes = {
        .abi_version = ARMOS_SPAWN_ABI_VERSION,
        .flags = ARMOS_SPAWN_SET_UID | ARMOS_SPAWN_SET_GID |
                 ARMOS_SPAWN_SET_CWD,
        .uid = 1000,
        .gid = 1000,
        .cwd = "/home/user"
    };

    return armos_spawnve("/usr/bin/foot", argv, envp, &attributes);
}

static pid_t wl_server_launch_shell(uint32_t token)
{
    static char *const argv[] = {"armos-shell", NULL};
    char token_environment[48];
    char *envp[] = {
        "PATH=/sbin:/bin:/usr/bin",
        "HOME=/root",
        "USER=root",
        "LOGNAME=root",
        "LANG=C.UTF-8",
        "WAYLAND_DISPLAY=wayland-0",
        "XDG_RUNTIME_DIR=/tmp",
        token_environment,
        NULL
    };
    armos_spawn_attributes_t attributes = {
        .abi_version = ARMOS_SPAWN_ABI_VERSION,
        .flags = ARMOS_SPAWN_SET_CWD,
        .cwd = "/"
    };

    snprintf(token_environment, sizeof(token_environment),
             "ARMOS_SHELL_TOKEN=%u", (unsigned)token);
    return armos_spawnve("/sbin/armos-shell", argv, envp, &attributes);
}

static int wl_server_run(struct wl_server *server, pid_t *terminal_pid,
                         pid_t *shell_pid)
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
    while (!server->fatal_error && !server->exit_requested) {
        if (wl_child_changed) {
            int status;

            wl_child_changed = 0;
            if (shell_pid && *shell_pid > 0 &&
                waitpid(*shell_pid, &status, WNOHANG) == *shell_pid) {
                *shell_pid = -1;
                server->shell_client = NULL;
                server->panel_height = 0u;
                (void)wl_server_schedule_render(server, true);
            }
            if (terminal_pid && *terminal_pid > 0 &&
                waitpid(*terminal_pid, &status, WNOHANG) ==
                    *terminal_pid) {
                *terminal_pid = -1;
            }
        }
        if (wl_event_loop_dispatch(server->event_loop, -1) < 0 &&
            errno != EINTR)
            return -1;
    }
    return server->exit_requested ? 0 : -1;
}

int main(int argc, char **argv)
{
    static struct wl_server server;
    const char *socket_path = ARMOS_WLCOMP_SOCKET_PATH;
    bool headless = false;
    bool quiet = false;
    bool profile = false;
    pid_t terminal_pid = -1;
    pid_t shell_pid = -1;

    (void)signal(SIGPIPE, SIG_IGN);
    /*
     * Keep input dispatch responsive when a software-rendered client consumes
     * a complete core. The compositor blocks in poll while idle, so this does
     * not turn it into a CPU hog.
     */
    (void)setpriority(PRIO_PROCESS, 0, -5);
    for (int index = 1; index < argc; index++) {
        if (strcmp(argv[index], "--headless") == 0) {
            headless = true;
        } else if (strcmp(argv[index], "--quiet") == 0) {
            quiet = true;
        } else if (strcmp(argv[index], "--profile") == 0) {
            profile = true;
        } else if (strcmp(argv[index], "--socket") == 0 &&
                   index + 1 < argc) {
            socket_path = argv[++index];
        } else {
            wl_server_usage(argv[0]);
            return 2;
        }
    }

    memset(&server, 0, sizeof(server));
    server.startup_started_us = wl_monotonic_us();
    server.shell_token =
        (uint32_t)wl_monotonic_us() ^ (uint32_t)getpid() ^
        0xa53c9e17u;
    if (server.shell_token == 0u)
        server.shell_token = 1u;
    server.listen_fd = -1;
    server.input_fd = -1;
    server.gpu_present_fence_fd = -1;
    for (size_t index = 0; index < WL_SERVER_MAX_CLIENTS; index++)
        server.clients[index].fd = -1;
    if (wl_renderer_init(&server.renderer, headless) < 0) {
        perror("armos-wlcomp: renderer");
        return 1;
    }
    wl_server_profile_startup(&server, profile, "renderer-ready");
    server.pointer_x = (int32_t)server.renderer.framebuffer.width / 2;
    server.pointer_y = (int32_t)server.renderer.framebuffer.height / 2;
    if (!headless) {
        server.input_fd = open("/dev/input0", O_RDONLY | O_NONBLOCK, 0);
        if (server.input_fd < 0) {
            perror("armos-wlcomp: input");
            wl_renderer_destroy(&server.renderer);
            return 1;
        }
        if (ioctl(server.input_fd, ARMOS_INPUT_GET_KEYMAP,
                  &server.keyboard_layout) < 0) {
            perror("armos-wlcomp: keyboard layout");
            close(server.input_fd);
            wl_renderer_destroy(&server.renderer);
            return 1;
        }
    } else {
        server.keyboard_layout = ARMOS_DEFAULT_KEYBOARD_LAYOUT;
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
    wl_server_profile_startup(&server, profile, "initial-frame");
    server.renderer.profile_enabled =
        profile || getenv("ARMOS_WL_PROFILE") != NULL;
    wl_render_profile_set_enabled(server.renderer.profile_enabled);

    if (!quiet) {
        const char *output =
            server.renderer.output_backend == WL_RENDERER_OUTPUT_GPU ?
                ", gpu" :
            server.renderer.output_backend == WL_RENDERER_OUTPUT_DRM ?
                ", drm" :
            server.renderer.output_backend == WL_RENDERER_OUTPUT_FRAMEBUFFER ?
                ", framebuffer" : ", headless";

        printf("armos-wlcomp: ready on %s (%ux%u%s)\n", socket_path,
               (unsigned)server.renderer.framebuffer.width,
               (unsigned)server.renderer.framebuffer.height,
               output);
    }
    if (!headless) {
        wl_child_changed = 0;
        if (signal(SIGCHLD, wl_server_child_signal) == SIG_ERR) {
            perror("armos-wlcomp: SIGCHLD");
            server.fatal_error = true;
        } else {
            shell_pid = wl_server_launch_shell(server.shell_token);
            if (shell_pid < 0) {
                perror("armos-wlcomp: shell");
                shell_pid = -1;
            }
            terminal_pid = wl_server_launch_terminal();
            if (terminal_pid < 0) {
                perror("armos-wlcomp: terminal");
                server.fatal_error = true;
            }
            wl_server_profile_startup(
                &server, server.renderer.profile_enabled,
                "clients-fork");
        }
    }
    if (!server.fatal_error &&
        wl_server_run(&server, &terminal_pid, &shell_pid) < 0)
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
    if (terminal_pid > 0) {
        int status;

        (void)kill(terminal_pid, SIGTERM);
        while (waitpid(terminal_pid, &status, 0) < 0 && errno == EINTR)
            ;
    }
    if (shell_pid > 0) {
        int status;

        (void)kill(shell_pid, SIGTERM);
        while (waitpid(shell_pid, &status, 0) < 0 && errno == EINTR)
            ;
    }
    return server.fatal_error ? 1 : 0;
}
