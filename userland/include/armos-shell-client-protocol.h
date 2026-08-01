/*
 * ArmOS
 * Copyright (c) 2026 Mohamed Ennassiri
 *
 * Licensed under the Apache License, Version 2.0.
 * See LICENSE for details.
 *
 * File: userland/include/armos-shell-client-protocol.h
 * Layer: Userland / privileged desktop protocol
 *
 * Responsibilities:
 * - Declare the narrow protocol used by the trusted ArmOS system shell.
 * - Assign a panel role without exposing compositor internals to clients.
 * - Keep authentication and placement policy inside the compositor.
 */

#ifndef ARMOS_SHELL_CLIENT_PROTOCOL_H
#define ARMOS_SHELL_CLIENT_PROTOCOL_H

#include <stdint.h>
#include <wayland-client.h>

struct armos_shell_v1;
struct armos_shell_panel_v1;

extern const struct wl_interface armos_shell_v1_interface;
extern const struct wl_interface armos_shell_panel_v1_interface;

struct armos_shell_panel_v1_listener {
    void (*configure)(void *data,
                      struct armos_shell_panel_v1 *panel,
                      int32_t width, int32_t height);
    void (*closed)(void *data,
                   struct armos_shell_panel_v1 *panel);
};

void armos_shell_v1_authenticate(
    struct armos_shell_v1 *shell, uint32_t token);
struct armos_shell_panel_v1 *armos_shell_v1_get_panel(
    struct armos_shell_v1 *shell, struct wl_surface *surface,
    int32_t height);
void armos_shell_v1_destroy(struct armos_shell_v1 *shell);

int armos_shell_panel_v1_add_listener(
    struct armos_shell_panel_v1 *panel,
    const struct armos_shell_panel_v1_listener *listener,
    void *data);
void armos_shell_panel_v1_destroy(
    struct armos_shell_panel_v1 *panel);

#endif /* ARMOS_SHELL_CLIENT_PROTOCOL_H */
