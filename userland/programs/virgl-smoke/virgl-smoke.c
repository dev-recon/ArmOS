/*
 * ArmOS
 * Copyright (c) 2026 Mohamed Ennassiri
 *
 * Licensed under the Apache License, Version 2.0.
 * See LICENSE for details.
 *
 * File: userland/programs/virgl-smoke/virgl-smoke.c
 * Layer: Userland / GPU diagnostics
 *
 * Responsibilities:
 * - Validate the ArmOS VirGL winsys from capability discovery to readback.
 * - Render one off-screen triangle through SUBMIT_3D.
 * - Verify distinct background and triangle pixels after GPU-to-CPU transfer.
 */

#include <armos/virgl_winsys.h>

#include <errno.h>
#include <stdio.h>
#include <string.h>

#define TEST_WIDTH 64u
#define TEST_HEIGHT 64u
#define TEST_STRIDE (TEST_WIDTH * 4u)
#define TEST_SIZE (TEST_STRIDE * TEST_HEIGHT)

#define VIRGL_CCMD_CREATE_OBJECT 1u
#define VIRGL_CCMD_BIND_OBJECT 2u
#define VIRGL_CCMD_SET_VIEWPORT_STATE 4u
#define VIRGL_CCMD_SET_FRAMEBUFFER_STATE 5u
#define VIRGL_CCMD_SET_VERTEX_BUFFERS 6u
#define VIRGL_CCMD_CLEAR 7u
#define VIRGL_CCMD_DRAW_VBO 8u
#define VIRGL_CCMD_BIND_SHADER 31u

#define VIRGL_OBJECT_BLEND 1u
#define VIRGL_OBJECT_RASTERIZER 2u
#define VIRGL_OBJECT_DSA 3u
#define VIRGL_OBJECT_SHADER 4u
#define VIRGL_OBJECT_VERTEX_ELEMENTS 5u
#define VIRGL_OBJECT_SURFACE 8u

#define VIRGL_FORMAT_BGRA8888 1u
#define VIRGL_FORMAT_R32G32B32_FLOAT 30u
#define PIPE_BUFFER 0u
#define PIPE_TEXTURE_2D 2u
#define PIPE_FORMAT_NONE 0u
#define PIPE_BIND_RENDER_TARGET (1u << 1)
#define PIPE_BIND_VERTEX_BUFFER (1u << 4)
#define PIPE_SHADER_VERTEX 0u
#define PIPE_SHADER_FRAGMENT 1u
#define PIPE_PRIM_TRIANGLES 4u
#define PIPE_CLEAR_COLOR0 (1u << 2)

#define SURFACE_HANDLE 1u
#define VERTEX_SHADER_HANDLE 2u
#define FRAGMENT_SHADER_HANDLE 3u
#define VERTEX_ELEMENTS_HANDLE 4u
#define RASTERIZER_HANDLE 5u
#define BLEND_HANDLE 6u
#define DSA_HANDLE 7u

typedef struct command_builder {
    uint32_t words[512];
    size_t count;
    int failed;
} command_builder_t;

static uint32_t float_word(float value)
{
    union {
        float value;
        uint32_t word;
    } bits;

    bits.value = value;
    return bits.word;
}

static void emit_word(command_builder_t *builder, uint32_t word)
{
    if (builder->count >= sizeof(builder->words) / sizeof(builder->words[0])) {
        builder->failed = 1;
        return;
    }
    builder->words[builder->count++] = word;
}

static void emit_command(command_builder_t *builder, uint32_t command,
                         uint32_t object, uint32_t length)
{
    emit_word(builder, command | (object << 8) | (length << 16));
}

static void emit_float(command_builder_t *builder, float value)
{
    emit_word(builder, float_word(value));
}

static void emit_bind_object(command_builder_t *builder, uint32_t object,
                             uint32_t handle)
{
    emit_command(builder, VIRGL_CCMD_BIND_OBJECT, object, 1u);
    emit_word(builder, handle);
}

static void emit_shader(command_builder_t *builder, uint32_t handle,
                        uint32_t type, const char *text)
{
    uint32_t text_size = (uint32_t)strlen(text) + 1u;
    uint32_t text_words = (text_size + 3u) / 4u;

    emit_command(builder, VIRGL_CCMD_CREATE_OBJECT, VIRGL_OBJECT_SHADER,
                 5u + text_words);
    emit_word(builder, handle);
    emit_word(builder, type);
    emit_word(builder, text_size);
    emit_word(builder, 4096u);
    emit_word(builder, 0u);
    for (uint32_t index = 0; index < text_words; index++) {
        uint32_t word = 0;
        size_t offset = (size_t)index * sizeof(word);
        size_t remaining = text_size > offset ? text_size - offset : 0;
        size_t amount = remaining > sizeof(word) ? sizeof(word) : remaining;

        if (amount != 0)
            memcpy(&word, text + offset, amount);
        emit_word(builder, word);
    }
    emit_command(builder, VIRGL_CCMD_BIND_SHADER, 0u, 2u);
    emit_word(builder, handle);
    emit_word(builder, type);
}

static int build_triangle_commands(command_builder_t *builder,
                                   uint32_t target_resource,
                                   uint32_t vertex_resource)
{
    static const char vertex_shader[] =
        "VERT\n"
        "DCL IN[0]\n"
        "DCL OUT[0], POSITION\n"
        "MOV OUT[0], IN[0]\n"
        "END\n";
    static const char fragment_shader[] =
        "FRAG\n"
        "DCL OUT[0], COLOR\n"
        "IMM[0] FLT32 { 1.0, 0.0, 0.0, 1.0 }\n"
        "MOV OUT[0], IMM[0]\n"
        "END\n";

    memset(builder, 0, sizeof(*builder));
    emit_shader(builder, VERTEX_SHADER_HANDLE, PIPE_SHADER_VERTEX,
                vertex_shader);
    emit_shader(builder, FRAGMENT_SHADER_HANDLE, PIPE_SHADER_FRAGMENT,
                fragment_shader);

    emit_command(builder, VIRGL_CCMD_CREATE_OBJECT,
                 VIRGL_OBJECT_SURFACE, 5u);
    emit_word(builder, SURFACE_HANDLE);
    emit_word(builder, target_resource);
    emit_word(builder, VIRGL_FORMAT_BGRA8888);
    emit_word(builder, 0u);
    emit_word(builder, 0u);

    emit_command(builder, VIRGL_CCMD_CREATE_OBJECT,
                 VIRGL_OBJECT_VERTEX_ELEMENTS, 5u);
    emit_word(builder, VERTEX_ELEMENTS_HANDLE);
    emit_word(builder, 0u);
    emit_word(builder, 0u);
    emit_word(builder, 0u);
    emit_word(builder, VIRGL_FORMAT_R32G32B32_FLOAT);
    emit_bind_object(builder, VIRGL_OBJECT_VERTEX_ELEMENTS,
                     VERTEX_ELEMENTS_HANDLE);

    emit_command(builder, VIRGL_CCMD_CREATE_OBJECT,
                 VIRGL_OBJECT_RASTERIZER, 9u);
    emit_word(builder, RASTERIZER_HANDLE);
    emit_word(builder, (1u << 1) | (1u << 29));
    emit_float(builder, 1.0f);
    emit_word(builder, 0u);
    emit_word(builder, 0u);
    emit_float(builder, 1.0f);
    emit_float(builder, 0.0f);
    emit_float(builder, 0.0f);
    emit_float(builder, 0.0f);
    emit_bind_object(builder, VIRGL_OBJECT_RASTERIZER, RASTERIZER_HANDLE);

    emit_command(builder, VIRGL_CCMD_CREATE_OBJECT, VIRGL_OBJECT_BLEND, 11u);
    emit_word(builder, BLEND_HANDLE);
    emit_word(builder, 0u);
    emit_word(builder, 0u);
    emit_word(builder, 0xfu << 27);
    for (uint32_t index = 1; index < 8u; index++)
        emit_word(builder, 0u);
    emit_bind_object(builder, VIRGL_OBJECT_BLEND, BLEND_HANDLE);

    emit_command(builder, VIRGL_CCMD_CREATE_OBJECT, VIRGL_OBJECT_DSA, 5u);
    emit_word(builder, DSA_HANDLE);
    emit_word(builder, 0u);
    emit_word(builder, 0u);
    emit_word(builder, 0u);
    emit_word(builder, 0u);
    emit_bind_object(builder, VIRGL_OBJECT_DSA, DSA_HANDLE);

    emit_command(builder, VIRGL_CCMD_SET_FRAMEBUFFER_STATE, 0u, 3u);
    emit_word(builder, 1u);
    emit_word(builder, 0u);
    emit_word(builder, SURFACE_HANDLE);

    emit_command(builder, VIRGL_CCMD_SET_VIEWPORT_STATE, 0u, 7u);
    emit_word(builder, 0u);
    emit_float(builder, (float)TEST_WIDTH / 2.0f);
    emit_float(builder, (float)TEST_HEIGHT / 2.0f);
    emit_float(builder, 0.5f);
    emit_float(builder, (float)TEST_WIDTH / 2.0f);
    emit_float(builder, (float)TEST_HEIGHT / 2.0f);
    emit_float(builder, 0.5f);

    emit_command(builder, VIRGL_CCMD_SET_VERTEX_BUFFERS, 0u, 3u);
    emit_word(builder, 12u);
    emit_word(builder, 0u);
    emit_word(builder, vertex_resource);

    emit_command(builder, VIRGL_CCMD_CLEAR, 0u, 8u);
    emit_word(builder, PIPE_CLEAR_COLOR0);
    emit_float(builder, 0.0f);
    emit_float(builder, 0.0f);
    emit_float(builder, 0.0f);
    emit_float(builder, 1.0f);
    emit_word(builder, 0u);
    emit_word(builder, 0x3ff00000u);
    emit_word(builder, 0u);

    emit_command(builder, VIRGL_CCMD_DRAW_VBO, 0u, 12u);
    emit_word(builder, 0u);
    emit_word(builder, 3u);
    emit_word(builder, PIPE_PRIM_TRIANGLES);
    emit_word(builder, 0u);
    emit_word(builder, 1u);
    emit_word(builder, 0u);
    emit_word(builder, 0u);
    emit_word(builder, 0u);
    emit_word(builder, 0u);
    emit_word(builder, 0u);
    emit_word(builder, 0xffffffffu);
    emit_word(builder, 0u);
    return builder->failed ? -1 : 0;
}

static int verify_pixels(const uint32_t *pixels)
{
    uint32_t corner = pixels[2u * TEST_WIDTH + 2u];
    uint32_t center = pixels[(TEST_HEIGHT / 2u) * TEST_WIDTH +
                             TEST_WIDTH / 2u];
    unsigned changed = 0;

    for (unsigned index = 0; index < TEST_WIDTH * TEST_HEIGHT; index++) {
        if (pixels[index] != corner)
            changed++;
    }
    printf("virgl-smoke: corner=%08x center=%08x changed=%u\n",
           (unsigned int)corner, (unsigned int)center, changed);
    if (center == corner || changed < 128u ||
        (center & 0x00ff0000u) == 0u) {
        errno = EIO;
        return -1;
    }
    return 0;
}

int main(void)
{
    static const float vertices[9] = {
        -0.75f, -0.75f, 0.0f,
         0.75f, -0.75f, 0.0f,
         0.0f,   0.75f, 0.0f,
    };
    armos_virgl_device_t device;
    armos_virgl_buffer_t target;
    armos_virgl_buffer_t vertex;
    armos_drm_virgl_resource_descriptor_t target_desc;
    armos_drm_virgl_resource_descriptor_t vertex_desc;
    command_builder_t commands;
    uint32_t context_id = 0;
    uint64_t fence_id = 0;
    int target_attached = 0;
    int vertex_attached = 0;
    int result = 1;

    memset(&target, 0, sizeof(target));
    memset(&vertex, 0, sizeof(vertex));
    memset(&target_desc, 0, sizeof(target_desc));
    target_desc.abi_version = ARMOS_DRM_VIRGL_RESOURCE_ABI_VERSION;
    target_desc.struct_size = sizeof(target_desc);
    target_desc.target = PIPE_TEXTURE_2D;
    target_desc.format = VIRGL_FORMAT_BGRA8888;
    target_desc.bind = PIPE_BIND_RENDER_TARGET;
    target_desc.width = TEST_WIDTH;
    target_desc.height = TEST_HEIGHT;
    target_desc.depth = 1u;
    target_desc.array_size = 1u;
    memset(&vertex_desc, 0, sizeof(vertex_desc));
    vertex_desc.abi_version = ARMOS_DRM_VIRGL_RESOURCE_ABI_VERSION;
    vertex_desc.struct_size = sizeof(vertex_desc);
    vertex_desc.target = PIPE_BUFFER;
    vertex_desc.format = PIPE_FORMAT_NONE;
    vertex_desc.bind = PIPE_BIND_VERTEX_BUFFER;
    vertex_desc.width = sizeof(vertices);
    vertex_desc.height = 1u;
    vertex_desc.depth = 1u;
    vertex_desc.array_size = 1u;
    if (armos_virgl_open(&device, NULL) < 0) {
        fprintf(stderr, "virgl-smoke: open: %s\n", strerror(errno));
        return 1;
    }
    printf("virgl-smoke: command-set=%s caps-version=%u caps-size=%u\n",
           (const char *)device.info.command_set,
           (unsigned int)device.command_caps_version,
           (unsigned int)device.command_caps_size);
    if (armos_virgl_context_create(&device, &context_id) < 0 ||
        armos_virgl_resource_create(
            &device, &target, TEST_SIZE,
            ARMOS_DRM_BO_CPU_READ | ARMOS_DRM_BO_CPU_WRITE |
                ARMOS_DRM_BO_RENDER_TARGET,
            &target_desc) < 0 ||
        armos_virgl_resource_create(
            &device, &vertex, sizeof(vertices),
            ARMOS_DRM_BO_CPU_READ | ARMOS_DRM_BO_CPU_WRITE |
                ARMOS_DRM_BO_VERTEX, &vertex_desc) < 0) {
        fprintf(stderr, "virgl-smoke: setup: %s\n", strerror(errno));
        goto out;
    }
    if (armos_virgl_buffer_map(&device, &target) < 0 ||
        armos_virgl_buffer_map(&device, &vertex) < 0) {
        fprintf(stderr, "virgl-smoke: mmap: %s\n", strerror(errno));
        goto out;
    }
    memset(target.mapping, 0x5a, TEST_SIZE);
    memcpy(vertex.mapping, vertices, sizeof(vertices));
    if (armos_virgl_buffer_attach(&device, context_id, &target) < 0)
        goto operation_failed;
    target_attached = 1;
    if (armos_virgl_buffer_attach(&device, context_id, &vertex) < 0)
        goto operation_failed;
    vertex_attached = 1;
    if (armos_virgl_buffer_transfer(
            &device, context_id, &vertex,
            ARMOS_DRM_TRANSFER_CPU_TO_DEVICE, 0, 0, 0, 0,
            sizeof(vertices), 1, 1, 0, 0, 0) < 0 ||
        build_triangle_commands(&commands, target.command_handle,
                                vertex.command_handle) < 0 ||
        armos_virgl_submit(&device, context_id, commands.words,
                           (uint32_t)(commands.count * sizeof(uint32_t)),
                           &fence_id) < 0 ||
        armos_virgl_fence_wait(&device, fence_id, 2000000000LL) < 0 ||
        armos_virgl_fence_destroy(&device, fence_id) < 0)
        goto operation_failed;
    fence_id = 0;
    if (armos_virgl_buffer_transfer(
            &device, context_id, &target,
            ARMOS_DRM_TRANSFER_DEVICE_TO_CPU, 0, 0, 0, 0,
            TEST_WIDTH, TEST_HEIGHT, 1, 0, TEST_STRIDE,
            TEST_SIZE) < 0 ||
        verify_pixels((const uint32_t *)target.mapping) < 0)
        goto operation_failed;
    printf("virgl-smoke: off-screen triangle verified\n");
    result = 0;
    goto out;

operation_failed:
    fprintf(stderr, "virgl-smoke: render/readback: %s\n", strerror(errno));
out:
    if (fence_id != 0)
        (void)armos_virgl_fence_destroy(&device, fence_id);
    if (vertex_attached)
        (void)armos_virgl_buffer_detach(&device, context_id, &vertex);
    if (target_attached)
        (void)armos_virgl_buffer_detach(&device, context_id, &target);
    if (vertex.handle != 0)
        (void)armos_virgl_buffer_destroy(&device, &vertex);
    if (target.handle != 0)
        (void)armos_virgl_buffer_destroy(&device, &target);
    if (context_id != 0)
        (void)armos_virgl_context_destroy(&device, context_id);
    armos_virgl_close(&device);
    return result;
}
