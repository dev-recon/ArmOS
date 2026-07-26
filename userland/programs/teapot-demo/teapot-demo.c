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
 *
 * Notes:
 * - Geometry is tessellated from the supplied bicubic Bezier patches.
 * - Rendering uses a depth buffer and Gouraud Lambert/specular shading.
 * - No external mesh, OpenGL, EGL or GPU-specific API is required.
 */

#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <wayland-client.h>
#include <xdg-shell-client-protocol.h>

#include "teapot_model.h"

#define WINDOW_WIDTH  480
#define WINDOW_HEIGHT 360
#define PIXEL_COUNT ((size_t)WINDOW_WIDTH * WINDOW_HEIGHT)
#define BUFFER_BYTES ((int32_t)(PIXEL_COUNT * sizeof(uint32_t)))

#define PATCH_STEPS       5
#define MAX_TRIANGLES   1800

#define KEY_ESC   1u
#define KEY_UP    103u
#define KEY_LEFT  105u
#define KEY_RIGHT 106u
#define KEY_DOWN  108u

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
    struct triangle triangles[MAX_TRIANGLES];
    size_t count;
};

struct app {
    struct wl_display *display;
    struct wl_registry *registry;
    struct wl_compositor *compositor;
    struct wl_shm *shm;
    struct wl_seat *seat;
    struct wl_keyboard *keyboard;
    struct xdg_wm_base *wm_base;
    struct wl_surface *surface;
    struct xdg_surface *xdg_surface;
    struct xdg_toplevel *toplevel;
    struct wl_shm_pool *pool;
    struct wl_buffer *buffer;
    struct wl_callback *frame_callback;
    uint32_t *pixels;
    float *depth;
    int shm_fd;
    int configured;
    int frame_ready;
    int buffer_busy;
    int closed;
    float yaw;
    float pitch;
    float yaw_speed;
    float pitch_speed;
    struct teapot_mesh mesh;
};

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
    if (mesh->count >= MAX_TRIANGLES)
        return -1;
    mesh->triangles[mesh->count++] = (struct triangle){
        { first, second, third }
    };
    return 0;
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

    memset(mesh, 0, sizeof(*mesh));
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

static struct projected project_point(struct vec3 point)
{
    const float camera_distance = 6.0f;
    const float focal_length = 325.0f;
    float depth = point.z + camera_distance;

    return (struct projected){
        WINDOW_WIDTH * 0.5f + point.x * focal_length / depth,
        WINDOW_HEIGHT * 0.53f - point.y * focal_length / depth,
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

static void rasterize_triangle(struct app *app,
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
        screen[index] = project_point(view[index]);
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
    if (x1 >= WINDOW_WIDTH)
        x1 = WINDOW_WIDTH - 1;
    if (y1 >= WINDOW_HEIGHT)
        y1 = WINDOW_HEIGHT - 1;

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
            pixel = (size_t)y * WINDOW_WIDTH + (size_t)x;
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

static void draw_light_marker(struct app *app)
{
    const int center_x = 45;
    const int center_y = 42;

    for (int y = -9; y <= 9; ++y) {
        for (int x = -9; x <= 9; ++x) {
            int distance = x * x + y * y;
            int px = center_x + x;
            int py = center_y + y;

            if (distance <= 81)
                app->pixels[(size_t)py * WINDOW_WIDTH + (size_t)px] =
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
    size_t index;

    for (int y = 0; y < WINDOW_HEIGHT; ++y) {
        uint32_t shade = (uint32_t)(16 + y * 18 / WINDOW_HEIGHT);
        uint32_t background =
            0xff000000u | (shade << 16) | ((shade + 5u) << 8) |
            (shade + 10u);

        for (int x = 0; x < WINDOW_WIDTH; ++x) {
            size_t pixel = (size_t)y * WINDOW_WIDTH + (size_t)x;

            app->pixels[pixel] = background;
            app->depth[pixel] = 1000000.0f;
        }
    }

    for (index = 0; index < app->mesh.count; ++index) {
        rasterize_triangle(app, &app->mesh.triangles[index],
                           &rotation, light, half_direction);
    }
    draw_light_marker(app);
}

static void buffer_release(void *data, struct wl_buffer *buffer)
{
    struct app *app = data;

    (void)buffer;
    app->buffer_busy = 0;
}

static const struct wl_buffer_listener buffer_listener = {
    buffer_release
};

static void frame_done(void *data, struct wl_callback *callback,
                       uint32_t callback_data)
{
    struct app *app = data;

    (void)callback_data;
    if (callback == app->frame_callback) {
        wl_callback_destroy(callback);
        app->frame_callback = NULL;
    }
    app->frame_ready = 1;
}

static const struct wl_callback_listener frame_listener = {
    frame_done
};

static int present_frame(struct app *app)
{
    if (!app->configured || !app->frame_ready || app->buffer_busy)
        return 0;

    usleep(16000u);
    app->yaw += app->yaw_speed;
    app->pitch += app->pitch_speed;
    app->pitch_speed *= 0.92f;
    render_frame(app);

    app->frame_callback = wl_surface_frame(app->surface);
    if (!app->frame_callback ||
        wl_callback_add_listener(app->frame_callback,
                                 &frame_listener, app) < 0)
        return -1;
    app->frame_ready = 0;
    app->buffer_busy = 1;
    wl_surface_attach(app->surface, app->buffer, 0, 0);
    wl_surface_damage_buffer(app->surface, 0, 0,
                             WINDOW_WIDTH, WINDOW_HEIGHT);
    wl_surface_commit(app->surface);
    return wl_display_flush(app->display);
}

static void keyboard_keymap(void *data, struct wl_keyboard *keyboard,
                            uint32_t format, int32_t fd, uint32_t size)
{
    (void)data;
    (void)keyboard;
    (void)format;
    (void)size;
    close(fd);
}

static void keyboard_enter(void *data, struct wl_keyboard *keyboard,
                           uint32_t serial, struct wl_surface *surface,
                           struct wl_array *keys)
{
    (void)data;
    (void)keyboard;
    (void)serial;
    (void)surface;
    (void)keys;
}

static void keyboard_leave(void *data, struct wl_keyboard *keyboard,
                           uint32_t serial, struct wl_surface *surface)
{
    (void)data;
    (void)keyboard;
    (void)serial;
    (void)surface;
}

static void keyboard_key(void *data, struct wl_keyboard *keyboard,
                         uint32_t serial, uint32_t time,
                         uint32_t key, uint32_t state)
{
    struct app *app = data;

    (void)keyboard;
    (void)serial;
    (void)time;
    if (state != WL_KEYBOARD_KEY_STATE_PRESSED)
        return;

    switch (key) {
    case KEY_LEFT:
        app->yaw -= 0.20f;
        app->yaw_speed = -0.045f;
        break;
    case KEY_RIGHT:
        app->yaw += 0.20f;
        app->yaw_speed = 0.045f;
        break;
    case KEY_UP:
        app->pitch += 0.16f;
        app->pitch_speed = 0.018f;
        break;
    case KEY_DOWN:
        app->pitch -= 0.16f;
        app->pitch_speed = -0.018f;
        break;
    case KEY_ESC:
        app->closed = 1;
        break;
    default:
        break;
    }
}

static void keyboard_modifiers(void *data, struct wl_keyboard *keyboard,
                               uint32_t serial, uint32_t mods_depressed,
                               uint32_t mods_latched, uint32_t mods_locked,
                               uint32_t group)
{
    (void)data;
    (void)keyboard;
    (void)serial;
    (void)mods_depressed;
    (void)mods_latched;
    (void)mods_locked;
    (void)group;
}

static void keyboard_repeat_info(void *data, struct wl_keyboard *keyboard,
                                 int32_t rate, int32_t delay)
{
    (void)data;
    (void)keyboard;
    (void)rate;
    (void)delay;
}

static const struct wl_keyboard_listener keyboard_listener = {
    keyboard_keymap,
    keyboard_enter,
    keyboard_leave,
    keyboard_key,
    keyboard_modifiers,
    keyboard_repeat_info
};

static void seat_capabilities(void *data, struct wl_seat *seat,
                              uint32_t capabilities)
{
    struct app *app = data;

    if ((capabilities & WL_SEAT_CAPABILITY_KEYBOARD) != 0 &&
        !app->keyboard) {
        app->keyboard = wl_seat_get_keyboard(seat);
        if (app->keyboard)
            (void)wl_keyboard_add_listener(app->keyboard,
                                           &keyboard_listener, app);
    }
}

static void seat_name(void *data, struct wl_seat *seat, const char *name)
{
    (void)data;
    (void)seat;
    (void)name;
}

static const struct wl_seat_listener seat_listener = {
    seat_capabilities,
    seat_name
};

static void wm_base_ping(void *data, struct xdg_wm_base *wm_base,
                         uint32_t serial)
{
    (void)data;
    xdg_wm_base_pong(wm_base, serial);
}

static const struct xdg_wm_base_listener wm_base_listener = {
    wm_base_ping
};

static void xdg_surface_configure(void *data,
                                  struct xdg_surface *xdg_surface,
                                  uint32_t serial)
{
    struct app *app = data;

    xdg_surface_ack_configure(xdg_surface, serial);
    app->configured = 1;
    app->frame_ready = 1;
}

static const struct xdg_surface_listener xdg_surface_listener = {
    xdg_surface_configure
};

static void toplevel_configure(void *data,
                               struct xdg_toplevel *toplevel,
                               int32_t width, int32_t height,
                               struct wl_array *states)
{
    (void)data;
    (void)toplevel;
    (void)width;
    (void)height;
    (void)states;
}

static void toplevel_close(void *data, struct xdg_toplevel *toplevel)
{
    struct app *app = data;

    (void)toplevel;
    app->closed = 1;
}

static const struct xdg_toplevel_listener toplevel_listener = {
    toplevel_configure,
    toplevel_close
};

static void registry_global(void *data, struct wl_registry *registry,
                            uint32_t name, const char *interface,
                            uint32_t version)
{
    struct app *app = data;

    if (strcmp(interface, "wl_compositor") == 0) {
        uint32_t bind_version = version < 4u ? version : 4u;

        app->compositor = wl_registry_bind(
            registry, name, &wl_compositor_interface, bind_version);
    } else if (strcmp(interface, "wl_shm") == 0) {
        app->shm = wl_registry_bind(
            registry, name, &wl_shm_interface, 1u);
    } else if (strcmp(interface, "wl_seat") == 0) {
        uint32_t bind_version = version < 5u ? version : 5u;

        app->seat = wl_registry_bind(
            registry, name, &wl_seat_interface, bind_version);
    } else if (strcmp(interface, "xdg_wm_base") == 0) {
        app->wm_base = wl_registry_bind(
            registry, name, &xdg_wm_base_interface, 1u);
    }
}

static void registry_global_remove(void *data, struct wl_registry *registry,
                                   uint32_t name)
{
    (void)data;
    (void)registry;
    (void)name;
}

static const struct wl_registry_listener registry_listener = {
    registry_global,
    registry_global_remove
};

static int create_window(struct app *app)
{
    char shm_name[48];

    app->surface = wl_compositor_create_surface(app->compositor);
    app->xdg_surface =
        xdg_wm_base_get_xdg_surface(app->wm_base, app->surface);
    if (!app->surface || !app->xdg_surface ||
        xdg_surface_add_listener(app->xdg_surface,
                                 &xdg_surface_listener, app) < 0)
        return -1;

    app->toplevel = xdg_surface_get_toplevel(app->xdg_surface);
    if (!app->toplevel ||
        xdg_toplevel_add_listener(app->toplevel,
                                  &toplevel_listener, app) < 0)
        return -1;
    xdg_toplevel_set_title(app->toplevel,
                           "ArmOS Utah teapot - arrow keys rotate");
    xdg_toplevel_set_app_id(app->toplevel, "org.armos.teapot-demo");
    xdg_toplevel_set_min_size(app->toplevel, WINDOW_WIDTH, WINDOW_HEIGHT);
    xdg_toplevel_set_max_size(app->toplevel, WINDOW_WIDTH, WINDOW_HEIGHT);
    xdg_surface_set_window_geometry(app->xdg_surface, 0, 0,
                                    WINDOW_WIDTH, WINDOW_HEIGHT);

    snprintf(shm_name, sizeof(shm_name), "/teapot-demo-%d", getpid());
    app->shm_fd = shm_open(shm_name, O_CREAT | O_EXCL | O_RDWR, 0600);
    if (app->shm_fd < 0 || shm_unlink(shm_name) < 0 ||
        ftruncate(app->shm_fd, BUFFER_BYTES) < 0)
        return -1;
    app->pixels = mmap(NULL, BUFFER_BYTES, PROT_READ | PROT_WRITE,
                       MAP_SHARED, app->shm_fd, 0);
    if (app->pixels == MAP_FAILED)
        return -1;
    app->pool = wl_shm_create_pool(app->shm, app->shm_fd, BUFFER_BYTES);
    app->buffer = app->pool ? wl_shm_pool_create_buffer(
        app->pool, 0, WINDOW_WIDTH, WINDOW_HEIGHT,
        WINDOW_WIDTH * (int32_t)sizeof(uint32_t),
        WL_SHM_FORMAT_XRGB8888) : NULL;
    if (!app->buffer ||
        wl_buffer_add_listener(app->buffer, &buffer_listener, app) < 0)
        return -1;

    app->depth = malloc(PIXEL_COUNT * sizeof(float));
    if (!app->depth)
        return -1;

    render_frame(app);
    app->buffer_busy = 1;
    wl_surface_attach(app->surface, app->buffer, 0, 0);
    wl_surface_damage_buffer(app->surface, 0, 0,
                             WINDOW_WIDTH, WINDOW_HEIGHT);
    wl_surface_commit(app->surface);
    return wl_display_flush(app->display);
}

static void destroy_app(struct app *app)
{
    if (app->frame_callback)
        wl_callback_destroy(app->frame_callback);
    if (app->keyboard)
        wl_keyboard_release(app->keyboard);
    if (app->seat)
        wl_seat_release(app->seat);
    if (app->toplevel)
        xdg_toplevel_destroy(app->toplevel);
    if (app->xdg_surface)
        xdg_surface_destroy(app->xdg_surface);
    if (app->surface)
        wl_surface_destroy(app->surface);
    if (app->buffer)
        wl_buffer_destroy(app->buffer);
    if (app->pool)
        wl_shm_pool_destroy(app->pool);
    if (app->pixels && app->pixels != MAP_FAILED)
        munmap(app->pixels, BUFFER_BYTES);
    if (app->shm_fd >= 0)
        close(app->shm_fd);
    if (app->wm_base)
        xdg_wm_base_destroy(app->wm_base);
    if (app->shm)
        wl_shm_destroy(app->shm);
    if (app->compositor)
        wl_compositor_destroy(app->compositor);
    if (app->registry)
        wl_registry_destroy(app->registry);
    if (app->display)
        wl_display_disconnect(app->display);
    free(app->depth);
}

int main(void)
{
    struct app app;

    memset(&app, 0, sizeof(app));
    app.shm_fd = -1;
    app.pixels = MAP_FAILED;
    app.yaw = -0.45f;
    app.pitch = -0.18f;
    app.yaw_speed = 0.030f;

    if (mesh_build_teapot(&app.mesh) < 0) {
        fprintf(stderr, "teapot-demo: geometry exceeds triangle budget\n");
        return 1;
    }

    app.display = wl_display_connect(NULL);
    if (!app.display) {
        perror("teapot-demo: connect");
        return 1;
    }
    app.registry = wl_display_get_registry(app.display);
    if (!app.registry ||
        wl_registry_add_listener(app.registry, &registry_listener, &app) < 0 ||
        wl_display_roundtrip(app.display) < 0 ||
        !app.compositor || !app.shm || !app.seat || !app.wm_base ||
        wl_seat_add_listener(app.seat, &seat_listener, &app) < 0 ||
        xdg_wm_base_add_listener(app.wm_base,
                                 &wm_base_listener, &app) < 0 ||
        create_window(&app) < 0) {
        perror("teapot-demo: setup");
        destroy_app(&app);
        return 1;
    }

    while (!app.closed) {
        if (present_frame(&app) < 0 ||
            wl_display_dispatch(app.display) < 0) {
            perror("teapot-demo: Wayland");
            destroy_app(&app);
            return 1;
        }
    }

    destroy_app(&app);
    return 0;
}
