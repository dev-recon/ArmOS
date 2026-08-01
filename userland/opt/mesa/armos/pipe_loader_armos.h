/*
 * ArmOS
 * Copyright (c) 2026 Mohamed Ennassiri
 *
 * Licensed under the Apache License, Version 2.0.
 * See LICENSE for details.
 *
 * File: userland/opt/mesa/armos/pipe_loader_armos.h
 * Layer: Third-party port / Mesa Gallium target selection
 *
 * Responsibilities:
 * - Select a Gallium screen from the render-node capability contract.
 * - Keep concrete GPU drivers out of EGL and window-system adapters.
 *
 * Notes:
 * - Selection is based on negotiated command sets, never driver names.
 * - New native GPU providers are registered in this userland boundary.
 */

#ifndef ARMOS_MESA_PIPE_LOADER_ARMOS_H
#define ARMOS_MESA_PIPE_LOADER_ARMOS_H

struct pipe_screen;

struct pipe_screen *armos_pipe_screen_create(const char *render_node);

#endif /* ARMOS_MESA_PIPE_LOADER_ARMOS_H */
