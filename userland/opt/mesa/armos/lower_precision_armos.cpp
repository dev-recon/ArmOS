/*
 * ArmOS
 * Copyright (c) 2026 Mohamed Ennassiri
 *
 * Licensed under the Apache License, Version 2.0.
 * See LICENSE for details.
 *
 * File: userland/opt/mesa/armos/lower_precision_armos.cpp
 * Layer: Userland / Mesa target adaptation
 *
 * Responsibilities:
 * - Preserve Mesa's GLSL lowering contract without requiring target STL.
 * - Keep GLES precision qualifiers valid by retaining full precision.
 *
 * Notes:
 * - This is a conservative optimization fallback, not a semantic shortcut:
 *   high precision satisfies every mediump/lowp minimum required by GLES 2.
 * - It is copied over Mesa's lower_precision.cpp only for ArmOS builds.
 */

#include "pipe/p_screen.h"
#include "compiler/glsl_types.h"
#include "ir.h"
#include "ir_optimization.h"

void
lower_precision(const struct pipe_screen *screen,
                mesa_shader_stage stage,
                ir_exec_list *instructions)
{
   (void)screen;
   (void)stage;
   (void)instructions;
}
