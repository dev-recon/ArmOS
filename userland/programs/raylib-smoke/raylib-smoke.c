/*
 * ArmOS
 * Copyright (c) 2026 Mohamed Ennassiri
 *
 * Licensed under the Apache License, Version 2.0.
 * See LICENSE for details.
 *
 * File: userland/programs/raylib-smoke/raylib-smoke.c
 * Layer: Userland / graphics diagnostics
 *
 * Responsibilities:
 * - Validate the public Raylib API over ArmOS Wayland and EGL/OpenGL ES 2.
 * - Exercise pointer input, configure-driven resizing and GPU presentation.
 * - Remain independent of the active DRM hardware backend.
 */

#include <raylib.h>

int main(void)
{
    Vector2 center;
    float angle = 0.0f;

    SetConfigFlags(FLAG_WINDOW_RESIZABLE | FLAG_VSYNC_HINT);
    InitWindow(720, 480, "ArmOS Raylib smoke");
    SetTargetFPS(60);

    while (!WindowShouldClose())
    {
        center = (Vector2){ GetScreenWidth()/2.0f, GetScreenHeight()/2.0f };
        angle += 0.8f;
        BeginDrawing();
        ClearBackground((Color){ 22, 29, 37, 255 });
        DrawText("Raylib on ArmOS - Wayland + EGL/GLES2", 24, 24, 20,
            (Color){ 88, 190, 235, 255 });
        DrawPoly(center, 3, 100.0f, angle, (Color){ 255, 194, 64, 255 });
        DrawCircleV(GetMousePosition(), 8.0f, (Color){ 255, 94, 87, 255 });
        DrawText("Move the pointer, resize the window, ESC to quit",
            24, GetScreenHeight() - 44, 18, RAYWHITE);
        EndDrawing();
    }
    CloseWindow();
    return 0;
}
