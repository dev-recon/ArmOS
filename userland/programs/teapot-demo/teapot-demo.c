/*
 * ArmOS
 * Copyright (c) 2026 Mohamed Ennassiri
 *
 * Licensed under the Apache License, Version 2.0.
 * See LICENSE for details.
 *
 * File: userland/programs/teapot-demo/teapot-demo.c
 * Layer: Userland / Wayland graphical demonstrations
 *
 * Responsibilities:
 * - Render the classic bicubic-patch Utah teapot in software.
 * - Animate the model below a fixed directional light.
 * - Rotate the model in response to the four arrow keys.
 * - Compare five educational software-rasterization algorithms.
 * - Browse and safely load Wavefront OBJ files through libarmmesh.
 *
 * Notes:
 * - Geometry is tessellated from the supplied bicubic Bezier patches.
 * - A Nuklear panel selects mesh, flat, Gouraud and Phong rendering.
 * - The built-in teapot remains available when external assets fail to load.
 * - No OpenGL, EGL or GPU-specific API is required.
 */

#include <math.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <armmesh/armmesh.h>
#include <armui/armui.h>
#include <armui/file_dialog.h>

#include "teapot_model.h"

#define WINDOW_WIDTH  480
#define WINDOW_HEIGHT 360
#define MINIMUM_WIDTH 320
#define MINIMUM_HEIGHT 240
#define MENU_HEIGHT    42
#define PATCH_STEPS       5
#define MAX_TRIANGLES  65536
#define MESH_NAME_MAX  64

enum render_algorithm {
    RENDER_MESH,
    RENDER_MESH_HIDDEN,
    RENDER_FLAT,
    RENDER_GOURAUD,
    RENDER_PHONG
};

struct vec3 {
    float x;
    float y;
    float z;
};

struct mesh_vertex {
    struct vec3 point;
    struct vec3 normal;
};

struct triangle {
    struct mesh_vertex vertex[3];
};

struct projected {
    float x;
    float y;
    float z;
};

struct rotation {
    float yaw_cosine;
    float yaw_sine;
    float pitch_cosine;
    float pitch_sine;
};

struct vertex_color {
    float red;
    float green;
    float blue;
};

struct teapot_mesh {
    struct triangle *triangles;
    size_t count;
    size_t capacity;
};

struct app {
    uint32_t *pixels;
    float *depth;
    int width;
    int height;
    size_t pixel_count;
    int closed;
    float yaw;
    float pitch;
    float yaw_speed;
    float pitch_speed;
    struct teapot_mesh mesh;
    struct armui_file_dialog *file_dialog;
    enum render_algorithm algorithm;
    int profile_enabled;
    uint64_t last_animation_us;
    uint64_t profile_started_us;
    uint64_t profile_last_frame_us;
    uint64_t profile_render_us;
    uint64_t profile_frame_us;
    uint64_t profile_frames;
    uint64_t profile_intervals;
    char mesh_name[MESH_NAME_MAX];
    char mesh_status[ARMMESH_ERROR_CAPACITY];
};

static uint64_t monotonic_us(void)
{
    struct timespec now;

    if (clock_gettime(CLOCK_MONOTONIC, &now) < 0)
        return 0u;
    return (uint64_t)now.tv_sec * 1000000u +
           (uint64_t)now.tv_nsec / 1000u;
}

static void profile_frame(struct app *app, uint64_t render_started_us)
{
    uint64_t now_us;

    if (!app->profile_enabled || render_started_us == 0u)
        return;
    now_us = monotonic_us();
    if (now_us == 0u || now_us < render_started_us)
        return;
    app->profile_render_us += now_us - render_started_us;
    if (app->profile_last_frame_us != 0u &&
        now_us >= app->profile_last_frame_us) {
        app->profile_frame_us += now_us - app->profile_last_frame_us;
        app->profile_intervals++;
    }
    app->profile_last_frame_us = now_us;
    app->profile_frames++;
    if (app->profile_started_us == 0u)
        app->profile_started_us = now_us;
    if (now_us - app->profile_started_us < 1000000u)
        return;
    fprintf(stderr,
            "TEAPOTPROFILE frames=%llu render_avg_us=%llu "
            "frame_avg_us=%llu\n",
            (unsigned long long)app->profile_frames,
            (unsigned long long)(app->profile_render_us /
                                 app->profile_frames),
            (unsigned long long)(app->profile_frame_us /
                                 (app->profile_intervals != 0u ?
                                  app->profile_intervals : 1u)));
    app->profile_started_us = now_us;
    app->profile_render_us = 0u;
    app->profile_frame_us = 0u;
    app->profile_frames = 0u;
    app->profile_intervals = 0u;
}

static struct vec3 vec3_add(struct vec3 left, struct vec3 right)
{
    return (struct vec3){
        left.x + right.x, left.y + right.y, left.z + right.z
    };
}

static struct vec3 vec3_sub(struct vec3 left, struct vec3 right)
{
    return (struct vec3){
        left.x - right.x, left.y - right.y, left.z - right.z
    };
}

static struct vec3 vec3_scale(struct vec3 value, float scale)
{
    return (struct vec3){
        value.x * scale, value.y * scale, value.z * scale
    };
}

static float vec3_dot(struct vec3 left, struct vec3 right)
{
    return left.x * right.x + left.y * right.y + left.z * right.z;
}

static struct vec3 vec3_cross(struct vec3 left, struct vec3 right)
{
    return (struct vec3){
        left.y * right.z - left.z * right.y,
        left.z * right.x - left.x * right.z,
        left.x * right.y - left.y * right.x
    };
}

static struct vec3 vec3_normalize(struct vec3 value)
{
    float length = sqrtf(vec3_dot(value, value));

    if (length < 0.00001f)
        return (struct vec3){ 0.0f, 0.0f, 1.0f };
    return vec3_scale(value, 1.0f / length);
}

static struct vec3 rotate_model(struct vec3 point,
                                const struct rotation *rotation)
{
    float x = point.x * rotation->yaw_cosine +
        point.z * rotation->yaw_sine;
    float z = -point.x * rotation->yaw_sine +
        point.z * rotation->yaw_cosine;

    return (struct vec3){
        x,
        point.y * rotation->pitch_cosine -
            z * rotation->pitch_sine,
        point.y * rotation->pitch_sine +
            z * rotation->pitch_cosine
    };
}

static int mesh_add_triangle(struct teapot_mesh *mesh,
                             struct mesh_vertex first,
                             struct mesh_vertex second,
                             struct mesh_vertex third)
{
    struct triangle *triangles;
    size_t capacity;

    if (mesh->count >= MAX_TRIANGLES)
        return -1;
    if (mesh->count == mesh->capacity) {
        capacity = mesh->capacity ? mesh->capacity * 2u : 2048u;
        if (capacity > MAX_TRIANGLES)
            capacity = MAX_TRIANGLES;
        triangles = realloc(mesh->triangles,
                            capacity * sizeof(*triangles));
        if (!triangles)
            return -1;
        mesh->triangles = triangles;
        mesh->capacity = capacity;
    }
    mesh->triangles[mesh->count++] = (struct triangle){
        { first, second, third }
    };
    return 0;
}

static void mesh_release(struct teapot_mesh *mesh)
{
    if (!mesh)
        return;
    free(mesh->triangles);
    memset(mesh, 0, sizeof(*mesh));
}

static int mesh_add_quad(struct teapot_mesh *mesh,
                         struct mesh_vertex first,
                         struct mesh_vertex second,
                         struct mesh_vertex third,
                         struct mesh_vertex fourth)
{
    if (mesh_add_triangle(mesh, first, second, third) < 0 ||
        mesh_add_triangle(mesh, first, third, fourth) < 0)
        return -1;
    return 0;
}

static void cubic_weights(float parameter, float weights[4])
{
    float inverse = 1.0f - parameter;
    float inverse_squared = inverse * inverse;
    float parameter_squared = parameter * parameter;

    weights[0] = inverse_squared * inverse;
    weights[1] = 3.0f * parameter * inverse_squared;
    weights[2] = 3.0f * parameter_squared * inverse;
    weights[3] = parameter_squared * parameter;
}

static void cubic_derivative_weights(float parameter, float weights[4])
{
    float inverse = 1.0f - parameter;

    weights[0] = -3.0f * inverse * inverse;
    weights[1] = 3.0f * inverse * inverse -
        6.0f * parameter * inverse;
    weights[2] = 6.0f * parameter * inverse -
        3.0f * parameter * parameter;
    weights[3] = 3.0f * parameter * parameter;
}

static struct vec3 model_vector(const float source[3])
{
    return (struct vec3){
        source[0] / 60.0f,
        source[2] / 60.0f,
        source[1] / 60.0f
    };
}

static struct mesh_vertex patch_vertex(size_t patch, float u, float v)
{
    float u_weights[4];
    float v_weights[4];
    float normal_u_weights[4];
    float normal_v_weights[4];
    float u_derivatives[4];
    float v_derivatives[4];
    float source[3] = { 0.0f, 0.0f, 0.0f };
    float tangent_u[3] = { 0.0f, 0.0f, 0.0f };
    float tangent_v[3] = { 0.0f, 0.0f, 0.0f };
    float normal_u = fminf(fmaxf(u, 0.001f), 0.999f);
    float normal_v = fminf(fmaxf(v, 0.001f), 0.999f);
    int row;
    int column;

    cubic_weights(u, u_weights);
    cubic_weights(v, v_weights);
    cubic_weights(normal_u, normal_u_weights);
    cubic_weights(normal_v, normal_v_weights);
    cubic_derivative_weights(normal_u, u_derivatives);
    cubic_derivative_weights(normal_v, v_derivatives);
    for (row = 0; row < 4; ++row) {
        for (column = 0; column < 4; ++column) {
            float weight = u_weights[row] * v_weights[column];
            int point = row * 4 + column;
            float normal_weight_u =
                u_derivatives[row] * normal_v_weights[column];
            float normal_weight_v =
                normal_u_weights[row] * v_derivatives[column];

            for (int axis = 0; axis < 3; ++axis) {
                float control = teapot_patches[patch][point][axis];

                source[axis] += control * weight;
                tangent_u[axis] += control * normal_weight_u;
                tangent_v[axis] += control * normal_weight_v;
            }
        }
    }

    return (struct mesh_vertex){
        {
            (source[0] - 10.0f) / 60.0f,
            (source[2] - 55.0f) / 60.0f,
            source[1] / 60.0f
        },
        vec3_normalize(vec3_cross(model_vector(tangent_u),
                                  model_vector(tangent_v)))
    };
}

static int mesh_build_teapot(struct teapot_mesh *mesh)
{
    size_t patch;
    int row;
    int column;

    mesh->count = 0u;
    for (patch = 0; patch < TEAPOT_PATCH_COUNT; ++patch) {
        for (row = 0; row < PATCH_STEPS; ++row) {
            float u0 = (float)row / PATCH_STEPS;
            float u1 = (float)(row + 1) / PATCH_STEPS;

            for (column = 0; column < PATCH_STEPS; ++column) {
                float v0 = (float)column / PATCH_STEPS;
                float v1 = (float)(column + 1) / PATCH_STEPS;

                if (mesh_add_quad(mesh,
                                  patch_vertex(patch, u0, v0),
                                  patch_vertex(patch, u1, v0),
                                  patch_vertex(patch, u1, v1),
                                  patch_vertex(patch, u0, v1)) < 0)
                    return -1;
            }
        }
    }
    return 0;
}

static int mesh_obj_triangle(const struct armmesh_triangle *source,
                             void *data)
{
    struct teapot_mesh *mesh = data;
    struct mesh_vertex vertices[3];

    for (int corner = 0; corner < 3; corner++) {
        vertices[corner].point = (struct vec3){
            source->vertices[corner].position[0],
            source->vertices[corner].position[1],
            source->vertices[corner].position[2]
        };
        vertices[corner].normal = (struct vec3){
            source->vertices[corner].normal[0],
            source->vertices[corner].normal[1],
            source->vertices[corner].normal[2]
        };
    }
    return mesh_add_triangle(mesh, vertices[0], vertices[1], vertices[2]);
}

static void mesh_set_status(struct app *app, const char *format, ...)
{
    va_list arguments;

    va_start(arguments, format);
    (void)vsnprintf(app->mesh_status, sizeof(app->mesh_status),
                    format, arguments);
    va_end(arguments);
}

static int mesh_use_builtin(struct app *app)
{
    struct teapot_mesh candidate = { 0 };

    if (mesh_build_teapot(&candidate) < 0) {
        mesh_release(&candidate);
        mesh_set_status(app, "Impossible de construire la theiere");
        return -1;
    }
    mesh_release(&app->mesh);
    app->mesh = candidate;
    (void)snprintf(app->mesh_name, sizeof(app->mesh_name),
                   "Theiere integree");
    mesh_set_status(app, "%u triangles",
                    (unsigned int)app->mesh.count);
    return 0;
}

static int mesh_load_file(struct app *app, const char *path)
{
    struct teapot_mesh candidate = { 0 };
    struct armmesh_stats stats;
    char error[ARMMESH_ERROR_CAPACITY];
    const char *name = strrchr(path, '/');

    name = name ? name + 1 : path;
    if (armmesh_load_obj(path, mesh_obj_triangle, &candidate, &stats,
                         error, sizeof(error)) < 0) {
        mesh_release(&candidate);
        mesh_set_status(app, "Erreur: %s", error);
        fprintf(stderr, "teapot-demo: %s\n", error);
        return -1;
    }
    mesh_release(&app->mesh);
    app->mesh = candidate;
    (void)snprintf(app->mesh_name, sizeof(app->mesh_name), "%s", name);
    mesh_set_status(app, "%u sommets, %u triangles",
                    (unsigned int)stats.vertices,
                    (unsigned int)stats.triangles);
    app->yaw = -0.45f;
    app->pitch = -0.18f;
    return 0;
}

static struct projected project_point(const struct app *app,
                                      struct vec3 point)
{
    const float camera_distance = 6.0f;
    float content_height = (float)(app->height - MENU_HEIGHT);
    float focal_length = fminf((float)app->width,
                               content_height) * 1.02f;
    float depth = point.z + camera_distance;

    return (struct projected){
        app->width * 0.5f + point.x * focal_length / depth,
        MENU_HEIGHT + content_height * 0.53f -
            point.y * focal_length / depth,
        depth
    };
}

static struct vertex_color light_vertex(struct vec3 normal,
                                        struct vec3 light,
                                        struct vec3 half_direction)
{
    const struct vec3 view_direction = { 0.0f, 0.0f, -1.0f };
    float diffuse;
    float specular;
    float intensity;
    float red;
    float green;
    float blue;

    if (vec3_dot(normal, view_direction) < 0.0f)
        normal = vec3_scale(normal, -1.0f);
    diffuse = fmaxf(vec3_dot(normal, light), 0.0f);
    specular = fmaxf(vec3_dot(normal, half_direction), 0.0f);
    specular *= specular;
    specular *= specular;
    specular *= specular;
    intensity = 0.18f + diffuse * 0.82f;
    red = 40.0f + intensity * 55.0f + specular * 100.0f;
    green = 95.0f + intensity * 120.0f + specular * 100.0f;
    blue = 125.0f + intensity * 105.0f + specular * 95.0f;

    if (red > 255.0f)
        red = 255.0f;
    if (green > 255.0f)
        green = 255.0f;
    if (blue > 255.0f)
        blue = 255.0f;
    return (struct vertex_color){ red, green, blue };
}

static float edge_function(float ax, float ay, float bx, float by,
                           float px, float py)
{
    return (px - ax) * (by - ay) - (py - ay) * (bx - ax);
}

static void rasterize_gouraud_triangle(struct app *app,
                                       const struct triangle *triangle,
                                       const struct rotation *rotation,
                                       struct vec3 light,
                                       struct vec3 half_direction)
{
    struct vec3 view[3];
    struct projected screen[3];
    struct vertex_color color[3];
    float area;
    float inverse_area;
    float minimum_x;
    float maximum_x;
    float minimum_y;
    float maximum_y;
    int x0;
    int x1;
    int y0;
    int y1;
    int index;
    float first_step_x;
    float first_step_y;
    float second_step_x;
    float second_step_y;
    float first_row;
    float second_row;

    for (index = 0; index < 3; ++index) {
        struct vec3 normal;

        view[index] = rotate_model(triangle->vertex[index].point,
                                   rotation);
        normal = rotate_model(triangle->vertex[index].normal, rotation);
        screen[index] = project_point(app, view[index]);
        color[index] = light_vertex(normal, light, half_direction);
    }

    area = edge_function(screen[0].x, screen[0].y,
                         screen[1].x, screen[1].y,
                         screen[2].x, screen[2].y);
    if (fabsf(area) < 0.01f)
        return;
    inverse_area = 1.0f / area;

    minimum_x = fminf(screen[0].x, fminf(screen[1].x, screen[2].x));
    maximum_x = fmaxf(screen[0].x, fmaxf(screen[1].x, screen[2].x));
    minimum_y = fminf(screen[0].y, fminf(screen[1].y, screen[2].y));
    maximum_y = fmaxf(screen[0].y, fmaxf(screen[1].y, screen[2].y));
    x0 = (int)floorf(minimum_x);
    x1 = (int)ceilf(maximum_x);
    y0 = (int)floorf(minimum_y);
    y1 = (int)ceilf(maximum_y);
    if (x0 < 0)
        x0 = 0;
    if (y0 < 0)
        y0 = 0;
    if (x1 >= app->width)
        x1 = app->width - 1;
    if (y1 >= app->height)
        y1 = app->height - 1;

    first_step_x = screen[2].y - screen[1].y;
    first_step_y = screen[1].x - screen[2].x;
    second_step_x = screen[0].y - screen[2].y;
    second_step_y = screen[2].x - screen[0].x;
    first_row = edge_function(screen[1].x, screen[1].y,
                              screen[2].x, screen[2].y,
                              (float)x0 + 0.5f, (float)y0 + 0.5f);
    second_row = edge_function(screen[2].x, screen[2].y,
                               screen[0].x, screen[0].y,
                               (float)x0 + 0.5f, (float)y0 + 0.5f);
    for (int y = y0; y <= y1; ++y) {
        float first_edge = first_row;
        float second_edge = second_row;

        for (int x = x0; x <= x1; ++x) {
            float first = first_edge * inverse_area;
            float second = second_edge * inverse_area;
            float third = 1.0f - first - second;
            float depth;
            size_t pixel;

            first_edge += first_step_x;
            second_edge += second_step_x;
            if (first < 0.0f || second < 0.0f || third < 0.0f)
                continue;
            depth = first * screen[0].z +
                second * screen[1].z + third * screen[2].z;
            pixel = (size_t)y * (size_t)app->width + (size_t)x;
            if (depth < app->depth[pixel]) {
                uint32_t red = (uint32_t)(
                    first * color[0].red +
                    second * color[1].red +
                    third * color[2].red);
                uint32_t green = (uint32_t)(
                    first * color[0].green +
                    second * color[1].green +
                    third * color[2].green);
                uint32_t blue = (uint32_t)(
                    first * color[0].blue +
                    second * color[1].blue +
                    third * color[2].blue);

                app->depth[pixel] = depth;
                app->pixels[pixel] =
                    0xff000000u | (red << 16) | (green << 8) | blue;
            }
        }
        first_row += first_step_y;
        second_row += second_step_y;
    }
}

static void transform_triangle(const struct app *app,
                               const struct triangle *triangle,
                               const struct rotation *rotation,
                               struct vec3 view[3],
                               struct vec3 normals[3],
                               struct projected screen[3])
{
    for (int index = 0; index < 3; ++index) {
        view[index] = rotate_model(triangle->vertex[index].point, rotation);
        normals[index] =
            rotate_model(triangle->vertex[index].normal, rotation);
        screen[index] = project_point(app, view[index]);
    }
}

static int triangle_pixel_bounds(const struct app *app,
                                 const struct projected screen[3],
                                 int *x0, int *x1, int *y0, int *y1,
                                 float *inverse_area)
{
    float area = edge_function(screen[0].x, screen[0].y,
                               screen[1].x, screen[1].y,
                               screen[2].x, screen[2].y);

    if (fabsf(area) < 0.01f)
        return 0;
    *inverse_area = 1.0f / area;
    *x0 = (int)floorf(fminf(screen[0].x,
                            fminf(screen[1].x, screen[2].x)));
    *x1 = (int)ceilf(fmaxf(screen[0].x,
                           fmaxf(screen[1].x, screen[2].x)));
    *y0 = (int)floorf(fminf(screen[0].y,
                            fminf(screen[1].y, screen[2].y)));
    *y1 = (int)ceilf(fmaxf(screen[0].y,
                           fmaxf(screen[1].y, screen[2].y)));
    if (*x0 < 0)
        *x0 = 0;
    if (*y0 < 0)
        *y0 = 0;
    if (*x1 >= app->width)
        *x1 = app->width - 1;
    if (*y1 >= app->height)
        *y1 = app->height - 1;
    return *x0 <= *x1 && *y0 <= *y1;
}

/*
 * Flat shading evaluates one face normal. Every pixel in the triangle
 * therefore receives exactly the same lighting value.
 */
static void rasterize_flat_triangle(struct app *app,
                                    const struct triangle *triangle,
                                    const struct rotation *rotation,
                                    struct vec3 light,
                                    struct vec3 half_direction)
{
    struct vec3 view[3];
    struct vec3 normals[3];
    struct projected screen[3];
    struct vec3 face_normal;
    struct vertex_color color;
    float inverse_area;
    int x0;
    int x1;
    int y0;
    int y1;

    transform_triangle(app, triangle, rotation, view, normals, screen);
    face_normal = vec3_normalize(
        vec3_cross(vec3_sub(view[1], view[0]),
                   vec3_sub(view[2], view[0])));
    color = light_vertex(face_normal, light, half_direction);
    if (!triangle_pixel_bounds(app, screen, &x0, &x1, &y0, &y1,
                               &inverse_area))
        return;

    for (int y = y0; y <= y1; ++y) {
        for (int x = x0; x <= x1; ++x) {
            float first = edge_function(screen[1].x, screen[1].y,
                                        screen[2].x, screen[2].y,
                                        (float)x + 0.5f,
                                        (float)y + 0.5f) * inverse_area;
            float second = edge_function(screen[2].x, screen[2].y,
                                         screen[0].x, screen[0].y,
                                         (float)x + 0.5f,
                                         (float)y + 0.5f) * inverse_area;
            float third = 1.0f - first - second;
            float depth;
            size_t pixel;

            if (first < 0.0f || second < 0.0f || third < 0.0f)
                continue;
            depth = first * screen[0].z +
                    second * screen[1].z + third * screen[2].z;
            pixel = (size_t)y * (size_t)app->width + (size_t)x;
            if (depth >= app->depth[pixel])
                continue;
            app->depth[pixel] = depth;
            app->pixels[pixel] =
                0xff000000u |
                ((uint32_t)color.red << 16) |
                ((uint32_t)color.green << 8) |
                (uint32_t)color.blue;
        }
    }
}

/*
 * Phong shading interpolates the three vertex normals, normalizes the result
 * for every covered pixel, and only then evaluates the light equation.
 */
static void rasterize_phong_triangle(struct app *app,
                                     const struct triangle *triangle,
                                     const struct rotation *rotation,
                                     struct vec3 light,
                                     struct vec3 half_direction)
{
    struct vec3 view[3];
    struct vec3 normals[3];
    struct projected screen[3];
    float inverse_area;
    int x0;
    int x1;
    int y0;
    int y1;

    transform_triangle(app, triangle, rotation, view, normals, screen);
    if (!triangle_pixel_bounds(app, screen, &x0, &x1, &y0, &y1,
                               &inverse_area))
        return;

    for (int y = y0; y <= y1; ++y) {
        for (int x = x0; x <= x1; ++x) {
            float first = edge_function(screen[1].x, screen[1].y,
                                        screen[2].x, screen[2].y,
                                        (float)x + 0.5f,
                                        (float)y + 0.5f) * inverse_area;
            float second = edge_function(screen[2].x, screen[2].y,
                                         screen[0].x, screen[0].y,
                                         (float)x + 0.5f,
                                         (float)y + 0.5f) * inverse_area;
            float third = 1.0f - first - second;
            float depth;
            size_t pixel;
            struct vec3 normal;
            struct vertex_color color;

            if (first < 0.0f || second < 0.0f || third < 0.0f)
                continue;
            depth = first * screen[0].z +
                    second * screen[1].z + third * screen[2].z;
            pixel = (size_t)y * (size_t)app->width + (size_t)x;
            if (depth >= app->depth[pixel])
                continue;

            normal = vec3_normalize((struct vec3){
                first * normals[0].x + second * normals[1].x +
                    third * normals[2].x,
                first * normals[0].y + second * normals[1].y +
                    third * normals[2].y,
                first * normals[0].z + second * normals[1].z +
                    third * normals[2].z
            });
            color = light_vertex(normal, light, half_direction);
            app->depth[pixel] = depth;
            app->pixels[pixel] =
                0xff000000u |
                ((uint32_t)color.red << 16) |
                ((uint32_t)color.green << 8) |
                (uint32_t)color.blue;
        }
    }
}

static void rasterize_depth_triangle(struct app *app,
                                     const struct triangle *triangle,
                                     const struct rotation *rotation)
{
    struct vec3 view[3];
    struct vec3 normals[3];
    struct projected screen[3];
    float inverse_area;
    int x0;
    int x1;
    int y0;
    int y1;

    transform_triangle(app, triangle, rotation, view, normals, screen);
    if (!triangle_pixel_bounds(app, screen, &x0, &x1, &y0, &y1,
                               &inverse_area))
        return;
    for (int y = y0; y <= y1; ++y) {
        for (int x = x0; x <= x1; ++x) {
            float first = edge_function(screen[1].x, screen[1].y,
                                        screen[2].x, screen[2].y,
                                        (float)x + 0.5f,
                                        (float)y + 0.5f) * inverse_area;
            float second = edge_function(screen[2].x, screen[2].y,
                                         screen[0].x, screen[0].y,
                                         (float)x + 0.5f,
                                         (float)y + 0.5f) * inverse_area;
            float third = 1.0f - first - second;
            float depth;
            size_t pixel;

            if (first < 0.0f || second < 0.0f || third < 0.0f)
                continue;
            depth = first * screen[0].z +
                    second * screen[1].z + third * screen[2].z;
            pixel = (size_t)y * (size_t)app->width + (size_t)x;
            if (depth < app->depth[pixel])
                app->depth[pixel] = depth;
        }
    }
}

static void draw_mesh_edge(struct app *app,
                           struct projected first,
                           struct projected second,
                           int hide_occluded)
{
    float dx = second.x - first.x;
    float dy = second.y - first.y;
    int steps = (int)ceilf(fmaxf(fabsf(dx), fabsf(dy)));

    if (steps < 1)
        steps = 1;
    for (int step = 0; step <= steps; ++step) {
        float amount = (float)step / steps;
        int x = (int)(first.x + dx * amount + 0.5f);
        int y = (int)(first.y + dy * amount + 0.5f);
        float depth = first.z + (second.z - first.z) * amount;
        size_t pixel;

        if (x < 0 || x >= app->width || y < 0 || y >= app->height)
            continue;
        pixel = (size_t)y * (size_t)app->width + (size_t)x;
        if (!hide_occluded || depth <= app->depth[pixel] + 0.025f)
            app->pixels[pixel] = 0xff68e7f2u;
    }
}

static void draw_mesh_triangle(struct app *app,
                               const struct triangle *triangle,
                               const struct rotation *rotation,
                               int hide_occluded)
{
    struct projected screen[3];

    for (int index = 0; index < 3; ++index) {
        struct vec3 view =
            rotate_model(triangle->vertex[index].point, rotation);

        screen[index] = project_point(app, view);
    }
    draw_mesh_edge(app, screen[0], screen[1], hide_occluded);
    draw_mesh_edge(app, screen[1], screen[2], hide_occluded);
    draw_mesh_edge(app, screen[2], screen[0], hide_occluded);
}

/* Plain wireframe: every triangle edge is visible, including rear faces. */
static void render_mesh(struct app *app, const struct rotation *rotation)
{
    for (size_t index = 0; index < app->mesh.count; ++index)
        draw_mesh_triangle(app, &app->mesh.triangles[index],
                           rotation, 0);
}

/*
 * Hidden-line wireframe: a depth-only prepass builds the visible surface,
 * then the same edges are emitted only when they lie on that surface.
 */
static void render_mesh_hidden(struct app *app,
                               const struct rotation *rotation)
{
    for (size_t index = 0; index < app->mesh.count; ++index)
        rasterize_depth_triangle(app, &app->mesh.triangles[index],
                                 rotation);
    for (size_t index = 0; index < app->mesh.count; ++index)
        draw_mesh_triangle(app, &app->mesh.triangles[index],
                           rotation, 1);
}

static void render_flat(struct app *app, const struct rotation *rotation,
                        struct vec3 light, struct vec3 half_direction)
{
    for (size_t index = 0; index < app->mesh.count; ++index)
        rasterize_flat_triangle(app, &app->mesh.triangles[index],
                                rotation, light, half_direction);
}

static void render_gouraud(struct app *app, const struct rotation *rotation,
                           struct vec3 light, struct vec3 half_direction)
{
    for (size_t index = 0; index < app->mesh.count; ++index)
        rasterize_gouraud_triangle(app, &app->mesh.triangles[index],
                                   rotation, light, half_direction);
}

static void render_phong(struct app *app, const struct rotation *rotation,
                         struct vec3 light, struct vec3 half_direction)
{
    for (size_t index = 0; index < app->mesh.count; ++index)
        rasterize_phong_triangle(app, &app->mesh.triangles[index],
                                 rotation, light, half_direction);
}

static void draw_light_marker(struct app *app)
{
    const int center_x = 45;
    const int center_y = MENU_HEIGHT + 28;

    for (int y = -9; y <= 9; ++y) {
        for (int x = -9; x <= 9; ++x) {
            int distance = x * x + y * y;
            int px = center_x + x;
            int py = center_y + y;

            if (distance <= 81)
                app->pixels[(size_t)py * (size_t)app->width +
                            (size_t)px] =
                    distance < 30 ? 0xfffff2a0u : 0xffffc84au;
        }
    }
}

static void render_frame(struct app *app)
{
    const struct vec3 view_direction = { 0.0f, 0.0f, -1.0f };
    struct rotation rotation = {
        cosf(app->yaw),
        sinf(app->yaw),
        cosf(app->pitch),
        sinf(app->pitch)
    };
    struct vec3 light = vec3_normalize(
        (struct vec3){ -0.45f, 0.78f, -0.55f });
    struct vec3 half_direction =
        vec3_normalize(vec3_add(light, view_direction));
    for (int y = 0; y < app->height; ++y) {
        uint32_t shade = (uint32_t)(16 + y * 18 / app->height);
        uint32_t background =
            0xff000000u | (shade << 16) | ((shade + 5u) << 8) |
            (shade + 10u);

        for (int x = 0; x < app->width; ++x) {
            size_t pixel =
                (size_t)y * (size_t)app->width + (size_t)x;

            app->pixels[pixel] = background;
            app->depth[pixel] = 1000000.0f;
        }
    }

    switch (app->algorithm) {
    case RENDER_MESH:
        render_mesh(app, &rotation);
        break;
    case RENDER_MESH_HIDDEN:
        render_mesh_hidden(app, &rotation);
        break;
    case RENDER_FLAT:
        render_flat(app, &rotation, light, half_direction);
        break;
    case RENDER_PHONG:
        render_phong(app, &rotation, light, half_direction);
        break;
    case RENDER_GOURAUD:
    default:
        render_gouraud(app, &rotation, light, half_direction);
        break;
    }
    draw_light_marker(app);
}

static const char *algorithm_name(enum render_algorithm algorithm)
{
    static const char *const names[] = {
        "Mesh", "Mesh-hidden", "Flat", "Gouraud", "Phong"
    };

    return (unsigned int)algorithm <
        sizeof(names) / sizeof(names[0]) ? names[algorithm] : "Gouraud";
}

static void build_menu_bar(struct app *app, struct armui_context *ui,
                           const struct armui_target *target)
{
    char scene_label[MESH_NAME_MAX + 24u];
    int panel_open = armui_panel_begin(
        ui, "TeapotMenu", 0.0f, 0.0f,
        (float)target->width, MENU_HEIGHT, 0, 1);

    if (panel_open) {
        armui_menubar_begin(ui);
        armui_row(ui, 28.0f, 4);
        if (armui_menu_begin(ui, "Fichier", 250.0f, 112.0f)) {
            armui_label(ui, app->mesh_status, ARMUI_ALIGN_LEFT);
            if (armui_menu_item(ui, "Ouvrir...")) {
                const struct armui_file_dialog_config config = {
                    "Ouvrir un maillage", "~/mesh", ".obj"
                };

                (void)armui_file_dialog_open(app->file_dialog, &config);
            }
            if (armui_menu_item(ui, "Theiere integree"))
                (void)mesh_use_builtin(app);
            armui_menu_end(ui);
        }
        if (armui_menu_begin(ui, "Rendu", 170.0f, 190.0f)) {
            if (armui_menu_item(ui, "Mesh"))
                app->algorithm = RENDER_MESH;
            if (armui_menu_item(ui, "Mesh-hidden"))
                app->algorithm = RENDER_MESH_HIDDEN;
            if (armui_menu_item(ui, "Flat"))
                app->algorithm = RENDER_FLAT;
            if (armui_menu_item(ui, "Gouraud"))
                app->algorithm = RENDER_GOURAUD;
            if (armui_menu_item(ui, "Phong"))
                app->algorithm = RENDER_PHONG;
            armui_menu_end(ui);
        }
        (void)snprintf(scene_label, sizeof(scene_label), "%s - %s",
                       app->mesh_name, algorithm_name(app->algorithm));
        armui_label(ui, scene_label, ARMUI_ALIGN_CENTER);
        if (armui_button_label(ui, "Quitter"))
            app->closed = 1;
        armui_menubar_end(ui);
    }
    armui_panel_end(ui);
}

static int resize_depth_buffer(struct app *app, int width, int height)
{
    size_t pixel_count;
    float *depth;

    if (width < MINIMUM_WIDTH || height < MINIMUM_HEIGHT)
        return -1;
    pixel_count = (size_t)width * (size_t)height;
    if (pixel_count > SIZE_MAX / sizeof(*depth))
        return -1;
    depth = realloc(app->depth, pixel_count * sizeof(*depth));
    if (!depth)
        return -1;
    app->depth = depth;
    app->pixel_count = pixel_count;
    app->width = width;
    app->height = height;
    app->last_animation_us = 0u;
    return 0;
}

static void teapot_key(struct armui_application *application,
                       enum armui_key key, int pressed, void *data)
{
    struct app *app = data;

    (void)application;
    if (!pressed)
        return;
    switch (key) {
    case ARMUI_KEY_LEFT:
        app->yaw -= 0.20f;
        app->yaw_speed = -0.045f;
        break;
    case ARMUI_KEY_RIGHT:
        app->yaw += 0.20f;
        app->yaw_speed = 0.045f;
        break;
    case ARMUI_KEY_UP:
        app->pitch += 0.16f;
        app->pitch_speed = 0.018f;
        break;
    case ARMUI_KEY_DOWN:
        app->pitch -= 0.16f;
        app->pitch_speed = -0.018f;
        break;
    default:
        break;
    }
}

static unsigned int teapot_frame(
    struct armui_application *application,
    struct armui_context *ui,
    struct armui_target *target,
    uint32_t time_ms,
    void *data)
{
    struct app *app = data;
    uint64_t animation_now_us;
    uint64_t render_started_us;
    char selected_path[512];
    float frame_scale = 1.0f;

    (void)application;
    (void)time_ms;
    if (target->width != app->width || target->height != app->height) {
        if (resize_depth_buffer(
                app, target->width, target->height) < 0)
            return ARMUI_FRAME_ERROR;
    }
    animation_now_us = monotonic_us();
    if (app->last_animation_us != 0u) {
        uint64_t elapsed_us =
            animation_now_us - app->last_animation_us;

        if (elapsed_us > 100000u)
            elapsed_us = 100000u;
        frame_scale = (float)elapsed_us / 16000.0f;
    }
    app->last_animation_us = animation_now_us;
    app->yaw += app->yaw_speed * frame_scale;
    app->pitch += app->pitch_speed * frame_scale;
    app->pitch_speed *= powf(0.92f, frame_scale);
    app->pixels = target->pixels;
    render_started_us = app->profile_enabled ? monotonic_us() : 0u;
    render_frame(app);
    if (armui_file_dialog_is_open(app->file_dialog)) {
        enum armui_file_dialog_result result = armui_file_dialog_draw(
            app->file_dialog, ui, target->width, target->height,
            selected_path, sizeof(selected_path));

        if (result == ARMUI_FILE_DIALOG_ACCEPTED)
            (void)mesh_load_file(app, selected_path);
    } else {
        build_menu_bar(app, ui, target);
    }
    profile_frame(app, render_started_us);
    return app->closed ? ARMUI_FRAME_CLOSE : ARMUI_FRAME_CONTINUE;
}

int main(int argc, char **argv)
{
    const struct armui_application_config config = {
        .title = "ArmOS Nuklear teapot - rendering algorithms",
        .app_id = "org.armos.teapot-demo",
        .width = WINDOW_WIDTH,
        .height = WINDOW_HEIGHT,
        .minimum_width = MINIMUM_WIDTH,
        .minimum_height = MINIMUM_HEIGHT,
        .clear_target = 0,
        .background = 0u,
        .key = teapot_key
    };
    struct app app;
    int profile = 0;
    int status;

    if (argc == 2 && strcmp(argv[1], "--profile") == 0) {
        profile = 1;
    } else if (argc != 1) {
        fprintf(stderr, "usage: %s [--profile]\n", argv[0]);
        return 2;
    }

    /*
     * This diagnostic renderer is intentionally CPU-heavy. Let the compositor
     * and terminal win scheduling ties so animation cannot degrade input.
     */
    (void)nice(5);
    memset(&app, 0, sizeof(app));
    app.yaw = -0.45f;
    app.pitch = -0.18f;
    app.yaw_speed = 0.030f;
    app.algorithm = RENDER_GOURAUD;
    app.profile_enabled =
        profile || getenv("ARMOS_TEAPOT_PROFILE") != NULL;
    app.file_dialog = armui_file_dialog_create();
    if (!app.file_dialog) {
        fprintf(stderr, "teapot-demo: file dialog allocation failed\n");
        return 1;
    }
    if (mesh_use_builtin(&app) < 0) {
        fprintf(stderr, "teapot-demo: geometry exceeds triangle budget\n");
        armui_file_dialog_destroy(app.file_dialog);
        return 1;
    }
    status = armui_application_run(&config, teapot_frame, &app);
    armui_file_dialog_destroy(app.file_dialog);
    mesh_release(&app.mesh);
    free(app.depth);
    if (status < 0) {
        perror("teapot-demo");
        return 1;
    }
    return 0;
}
