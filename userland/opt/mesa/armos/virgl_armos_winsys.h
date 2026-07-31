/*
 * ArmOS
 * Copyright (c) 2026 Mohamed Ennassiri
 *
 * Licensed under the Apache License, Version 2.0.
 * See LICENSE for details.
 *
 * File: userland/opt/mesa/armos/virgl_armos_winsys.h
 * Layer: Third-party port / Mesa Gallium VirGL winsys
 *
 * Responsibilities:
 * - Instantiate Mesa's VirGL winsys on the ArmOS DRM rendering node.
 * - Keep Mesa independent from ArmOS kernel and platform implementation details.
 * - Expose the same Gallium screen entry point on ARM32 and ARM64.
 */

#ifndef VIRGL_ARMOS_WINSYS_H
#define VIRGL_ARMOS_WINSYS_H

struct pipe_screen;
struct pipe_screen_config;
struct virgl_winsys;

struct virgl_winsys *virgl_armos_winsys_create(const char *render_node);
struct pipe_screen *virgl_armos_screen_create(
    const char *render_node, const struct pipe_screen_config *config);

#endif /* VIRGL_ARMOS_WINSYS_H */
