/*
 * ArmOS
 * Copyright (c) 2026 Mohamed Ennassiri
 *
 * Licensed under the Apache License, Version 2.0.
 * See LICENSE for details.
 *
 * File: userland/programs/raypot-demo/raypot-demo.c
 * Layer: Userland / GPU graphics demonstrations
 *
 * Responsibilities:
 * - Tessellate the shared Utah teapot Bezier patches once on the CPU.
 * - Render the resulting mesh through Raylib and ArmOS EGL/GLES2.
 * - Exercise GPU depth, vertex buffers, shaders and Wayland presentation.
 * - Provide an accelerated counterpart to the software teapot-demo.
 */

#include <stdlib.h>

#include <raylib.h>
#include <raymath.h>
#include <rlgl.h>

#include "../teapot-demo/teapot_model.h"

#define PATCH_STEPS 8
#define VERTICES_PER_PATCH (PATCH_STEPS * PATCH_STEPS * 6)

static void cubic_weights(float value, float weights[4])
{
    float inverse = 1.0f - value;

    weights[0] = inverse * inverse * inverse;
    weights[1] = 3.0f * value * inverse * inverse;
    weights[2] = 3.0f * value * value * inverse;
    weights[3] = value * value * value;
}

static void cubic_derivative_weights(float value, float weights[4])
{
    float inverse = 1.0f - value;

    weights[0] = -3.0f * inverse * inverse;
    weights[1] = 3.0f * inverse * inverse - 6.0f * value * inverse;
    weights[2] = 6.0f * value * inverse - 3.0f * value * value;
    weights[3] = 3.0f * value * value;
}

static Vector3 teapot_point(size_t patch, float u, float v, Vector3 *normal)
{
    float uw[4], vw[4], duw[4], dvw[4];
    Vector3 point = { 0.0f, 0.0f, 0.0f };
    Vector3 tangentU = { 0.0f, 0.0f, 0.0f };
    Vector3 tangentV = { 0.0f, 0.0f, 0.0f };

    cubic_weights(u, uw);
    cubic_weights(v, vw);
    cubic_derivative_weights(u, duw);
    cubic_derivative_weights(v, dvw);
    for (int row = 0; row < 4; row++)
    {
        for (int column = 0; column < 4; column++)
        {
            const float *control = teapot_patches[patch][row*4 + column];
            float positionWeight = uw[row] * vw[column];
            float tangentUWeight = duw[row] * vw[column];
            float tangentVWeight = uw[row] * dvw[column];
            Vector3 source = {
                (control[0] - 10.0f)/60.0f,
                (control[2] - 55.0f)/60.0f,
                control[1]/60.0f
            };

            point = Vector3Add(point, Vector3Scale(source, positionWeight));
            tangentU = Vector3Add(tangentU,
                Vector3Scale(source, tangentUWeight));
            tangentV = Vector3Add(tangentV,
                Vector3Scale(source, tangentVWeight));
        }
    }
    *normal = Vector3Normalize(Vector3CrossProduct(tangentU, tangentV));
    return point;
}

static void mesh_write_vertex(Mesh *mesh, int *index,
                              Vector3 position, Vector3 normal)
{
    int vertex = (*index)++;

    mesh->vertices[vertex*3] = position.x;
    mesh->vertices[vertex*3 + 1] = position.y;
    mesh->vertices[vertex*3 + 2] = position.z;
    mesh->normals[vertex*3] = normal.x;
    mesh->normals[vertex*3 + 1] = normal.y;
    mesh->normals[vertex*3 + 2] = normal.z;
}

static Mesh build_teapot_mesh(void)
{
    Mesh mesh = { 0 };
    int vertex = 0;

    mesh.vertexCount = (int)TEAPOT_PATCH_COUNT * VERTICES_PER_PATCH;
    mesh.triangleCount = mesh.vertexCount/3;
    mesh.vertices = calloc((size_t)mesh.vertexCount * 3, sizeof(float));
    mesh.normals = calloc((size_t)mesh.vertexCount * 3, sizeof(float));
    if (!mesh.vertices || !mesh.normals)
    {
        free(mesh.vertices);
        free(mesh.normals);
        return (Mesh){ 0 };
    }

    for (size_t patch = 0; patch < TEAPOT_PATCH_COUNT; patch++)
    {
        for (int row = 0; row < PATCH_STEPS; row++)
        {
            float u0 = (float)row/PATCH_STEPS;
            float u1 = (float)(row + 1)/PATCH_STEPS;

            for (int column = 0; column < PATCH_STEPS; column++)
            {
                float v0 = (float)column/PATCH_STEPS;
                float v1 = (float)(column + 1)/PATCH_STEPS;
                Vector3 normals[4];
                Vector3 points[4] = {
                    teapot_point(patch, u0, v0, &normals[0]),
                    teapot_point(patch, u1, v0, &normals[1]),
                    teapot_point(patch, u1, v1, &normals[2]),
                    teapot_point(patch, u0, v1, &normals[3])
                };

                mesh_write_vertex(&mesh, &vertex, points[0], normals[0]);
                mesh_write_vertex(&mesh, &vertex, points[1], normals[1]);
                mesh_write_vertex(&mesh, &vertex, points[2], normals[2]);
                mesh_write_vertex(&mesh, &vertex, points[0], normals[0]);
                mesh_write_vertex(&mesh, &vertex, points[2], normals[2]);
                mesh_write_vertex(&mesh, &vertex, points[3], normals[3]);
            }
        }
    }
    UploadMesh(&mesh, false);
    return mesh;
}

static Shader load_teapot_shader(void)
{
    static const char vertexShader[] =
        "#version 100\n"
        "attribute vec3 vertexPosition;\n"
        "attribute vec3 vertexNormal;\n"
        "uniform mat4 mvp;\n"
        "uniform mat4 matModel;\n"
        "uniform mat4 matNormal;\n"
        "varying vec3 worldPosition;\n"
        "varying vec3 worldNormal;\n"
        "void main(void) {\n"
        "  vec4 world = matModel*vec4(vertexPosition, 1.0);\n"
        "  worldPosition = world.xyz;\n"
        "  worldNormal = normalize((matNormal*vec4(vertexNormal, 0.0)).xyz);\n"
        "  gl_Position = mvp*vec4(vertexPosition, 1.0);\n"
        "}\n";
    static const char fragmentShader[] =
        "#version 100\n"
        "precision mediump float;\n"
        "varying vec3 worldPosition;\n"
        "varying vec3 worldNormal;\n"
        "uniform vec3 viewPos;\n"
        "void main(void) {\n"
        "  vec3 n = normalize(worldNormal);\n"
        "  vec3 light = normalize(vec3(-0.45, 0.75, -0.55));\n"
        "  vec3 view = normalize(viewPos - worldPosition);\n"
        "  vec3 halfVector = normalize(light + view);\n"
        "  float diffuse = max(dot(n, light), 0.0);\n"
        "  float specular = pow(max(dot(n, halfVector), 0.0), 24.0);\n"
        "  vec3 base = vec3(0.18, 0.65, 0.78);\n"
        "  gl_FragColor = vec4(base*(0.20 + 0.80*diffuse) + 0.55*specular, 1.0);\n"
        "}\n";

    return LoadShaderFromMemory(vertexShader, fragmentShader);
}

int main(void)
{
    Camera3D camera = {
        .position = { 0.0f, 0.3f, 5.2f },
        .target = { 0.0f, 0.2f, 0.0f },
        .up = { 0.0f, 1.0f, 0.0f },
        .fovy = 42.0f,
        .projection = CAMERA_PERSPECTIVE
    };
    float yaw = 0.0f;
    float pitch = -0.15f;
    bool wireframe = false;

    SetConfigFlags(FLAG_WINDOW_RESIZABLE | FLAG_VSYNC_HINT);
    InitWindow(800, 600, "ArmOS Raylib Utah teapot");
    if (!IsWindowReady()) return 1;

    Mesh mesh = build_teapot_mesh();
    if (mesh.vertexCount == 0)
    {
        CloseWindow();
        return 1;
    }
    Model model = LoadModelFromMesh(mesh);
    Shader shader = load_teapot_shader();
    if (!IsShaderValid(shader))
    {
        UnloadModel(model);
        CloseWindow();
        return 1;
    }
    shader.locs[SHADER_LOC_VECTOR_VIEW] =
        GetShaderLocation(shader, "viewPos");
    model.materials[0].shader = shader;
    SetTargetFPS(60);

    while (!WindowShouldClose())
    {
        float step = 1.4f * GetFrameTime();

        if (IsKeyDown(KEY_LEFT)) yaw -= step;
        else if (IsKeyDown(KEY_RIGHT)) yaw += step;
        else yaw += 0.35f * GetFrameTime();
        if (IsKeyDown(KEY_UP)) pitch -= step;
        if (IsKeyDown(KEY_DOWN)) pitch += step;
        if (IsKeyPressed(KEY_W)) wireframe = !wireframe;
        model.transform = MatrixMultiply(MatrixRotateX(pitch),
                                         MatrixRotateY(yaw));
        SetShaderValue(shader, shader.locs[SHADER_LOC_VECTOR_VIEW],
                       &camera.position.x, SHADER_UNIFORM_VEC3);

        BeginDrawing();
        ClearBackground((Color){ 20, 27, 35, 255 });
        BeginMode3D(camera);
        /* The patch data is two-sided and does not guarantee a uniform
         * winding across every Bezier patch.  Keep depth writes authoritative
         * while avoiding topology loss from back-face culling. */
        rlEnableDepthMask();
        rlEnableDepthTest();
        rlDisableBackfaceCulling();
        DrawModel(model, (Vector3){ 0.0f, 0.0f, 0.0f }, 1.0f, WHITE);
        if (wireframe)
            DrawModelWires(model, (Vector3){ 0.0f, 0.0f, 0.0f },
                           1.002f, (Color){ 90, 230, 245, 255 });
        rlEnableBackfaceCulling();
        EndMode3D();
        DrawText("Raypot - GPU Raylib/EGL/VirGL", 18, 16, 20,
                 (Color){ 88, 190, 235, 255 });
        DrawText("Arrow keys: rotate   W: wireframe   ESC: quit",
                 18, GetScreenHeight() - 38, 18, RAYWHITE);
        EndDrawing();
    }

    UnloadModel(model);
    UnloadShader(shader);
    CloseWindow();
    return 0;
}
