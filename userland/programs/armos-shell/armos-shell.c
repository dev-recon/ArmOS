/*
 * ArmOS
 * Copyright (c) 2026 Mohamed Ennassiri
 *
 * Licensed under the Apache License, Version 2.0.
 * See LICENSE for details.
 *
 * File: userland/programs/armos-shell/armos-shell.c
 * Layer: Userland / privileged desktop services
 *
 * Responsibilities:
 * - Provide the trusted ArmOS desktop bar as an ArmUI client.
 * - Launch ordinary desktop applications with unprivileged credentials.
 * - Read system state through the stable ArmOS service boundary.
 *
 * Notes:
 * - Panel placement and work-area policy belong to the compositor protocol.
 * - This client never accesses framebuffer or input devices directly.
 */

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>

#include <armos/services.h>
#include <armos/spawn.h>
#include <armui/armui.h>

struct armos_shell {
    struct armos_service_snapshot snapshot;
};

static int shell_launch(const char *path, const char *name,
                        const char *option, const char *argument)
{
    char *const argv[] = {
        (char *)name, (char *)option, (char *)argument, NULL
    };
    char *const envp[] = {
        "PATH=/sbin:/bin:/usr/bin",
        "HOME=/home/user",
        "USER=user",
        "LOGNAME=user",
        "LANG=C.UTF-8",
        "SHELL=/bin/sh",
        "ENV=/home/user/.shrc",
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

    return armos_spawnve(path, argv, envp, &attributes) < 0 ? -1 : 0;
}

static unsigned int shell_frame(
    struct armui_application *application,
    struct armui_context *ui,
    struct armui_target *target,
    uint32_t time_ms,
    void *data)
{
    struct armos_shell *shell = data;
    char status[96];

    (void)application;
    (void)time_ms;
    armui_set_contrast(ui, 1);
    if (armui_panel_begin(
            ui, "armos-system-bar", 0.0f, 0.0f,
            (float)target->width, (float)target->height, 0, 0)) {
        armui_row(ui, (float)target->height - 4.0f, 4);
        armui_label(ui, "ArmOS", ARMUI_ALIGN_LEFT);
        if (armui_button_label(ui, "Terminal"))
            (void)shell_launch(
                "/usr/bin/foot", "foot", "-c",
                "/opt/foot/share/foot.ini");
        if (armui_button_label(ui, "Reglages"))
            (void)shell_launch(
                "/usr/bin/armos-control-center",
                "armos-control-center", NULL, NULL);
        snprintf(status, sizeof(status), "%uM libres  %s",
                 shell->snapshot.memory_free_kb / 1024u,
                 shell->snapshot.network_available ?
                 shell->snapshot.network_interface : "hors ligne");
        armui_label(ui, status, ARMUI_ALIGN_RIGHT);
    }
    armui_panel_end(ui);
    return ARMUI_FRAME_IDLE;
}

int main(void)
{
    const struct armui_application_config config = {
        .title = "ArmOS Shell",
        .app_id = "org.armos.shell",
        .width = 640,
        .height = 36,
        .minimum_width = 320,
        .minimum_height = 24,
        .clear_target = 1,
        .background = 0x00d7dbe2u,
        .key = NULL,
        .role = ARMUI_APPLICATION_SYSTEM_BAR
    };
    struct armos_shell shell;

    (void)signal(SIGCHLD, SIG_IGN);
    memset(&shell, 0, sizeof(shell));
    (void)armos_services_snapshot(&shell.snapshot);
    if (armui_application_run(&config, shell_frame, &shell) < 0) {
        perror("armos-shell");
        return 1;
    }
    return 0;
}
