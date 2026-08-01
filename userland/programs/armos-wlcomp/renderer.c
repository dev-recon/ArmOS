/*
 * ArmOS
 * Copyright (c) 2026 Mohamed Ennassiri
 *
 * Licensed under the Apache License, Version 2.0.
 * See LICENSE for details.
 *
 * File: userland/programs/armos-wlcomp/renderer.c
 * Layer: Userland / graphical services
 *
 * Responsibilities:
 * - Composite committed Wayland SHM or GPU surfaces into an ARGB8888 canvas.
 * - Present the canvas through the architecture-neutral framebuffer ABI.
 * - Retain committed pixels independently from client buffer lifetimes.
 *
 * Notes:
 * - Headless mode uses the same compositor path without touching /dev/fb0.
 * - Surface placement is temporary policy for the first server milestone.
 */

#include "armos_wlcomp.h"
#include "gpu_backend.h"
#include "gpu_present.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#define WL_HEADLESS_WIDTH  800u
#define WL_HEADLESS_HEIGHT 600u
#define WL_BACKGROUND      0xffd8dde5u
#define WL_WINDOW_MINIMIZED_WIDTH 220u
#define WL_WINDOW_RADIUS       10u
#define WL_WINDOW_TITLE_ACTIVE   0xff5ac8fau
#define WL_WINDOW_TITLE_INACTIVE 0xffe8e8eau

static const char *const wl_pointer_shape[] = {
    "X...........",
    "XX..........",
    "XOX.........",
    "XOOX........",
    "XOOOX.......",
    "XOOOOX......",
    "XOOOOOX.....",
    "XOOOOOOX....",
    "XOOOOOOOX...",
    "XOOOOXXXXX..",
    "XOOXOOX.....",
    "XOX.XOOX....",
    "XX..XOOX....",
    "X....XOOX...",
    ".....XOOX...",
    "......XX...."
};

#define WL_POINTER_WIDTH         12u
#define WL_POINTER_HEIGHT        16u
#define WL_POINTER_DAMAGE_OFFSET 10
#define WL_POINTER_DAMAGE_SIZE   32u

struct wl_renderer_scene {
    struct wl_server_surface *ordered[
        WL_SERVER_MAX_CLIENTS * WL_SERVER_MAX_SURFACES];
    size_t count;
};

struct wl_gpu_frame_buffers {
    struct wl_server_buffer *items[
        WL_SERVER_MAX_CLIENTS * WL_SERVER_MAX_SURFACES];
    size_t count;
};

struct wl_gpu_surface_cache {
    const struct wl_server_surface *surface;
    struct wl_gpu_image *image;
    uint32_t *pixels;
    uint32_t width;
    uint32_t height;
    uint32_t stride;
    int32_t offset_x;
    int32_t offset_y;
    uint64_t signature;
};

static uint64_t wl_renderer_monotonic_us(void)
{
    struct timespec now;

    if (clock_gettime(CLOCK_MONOTONIC, &now) < 0)
        return 0u;
    return (uint64_t)now.tv_sec * UINT64_C(1000000) +
           (uint64_t)now.tv_nsec / UINT64_C(1000);
}

static int wl_surface_content_origin(
    const struct wl_server_surface *surface, int32_t *x, int32_t *y);
static bool wl_renderer_tile_dirty(
    const struct wl_server_renderer *renderer,
    uint32_t column, uint32_t row);
static void wl_renderer_mark_rect_in_tiles(
    const struct wl_server_renderer *renderer, uint64_t *tiles,
    int32_t x, int32_t y, uint32_t width, uint32_t height);
static struct wl_renderer_rect wl_renderer_tile_rect(
    const struct wl_server_renderer *renderer,
    uint32_t column, uint32_t row);

static bool wl_gpu_frame_track_buffer(
    struct wl_gpu_frame_buffers *buffers,
    struct wl_server_buffer *buffer)
{
    if (!buffers || !buffer)
        return false;
    for (size_t index = 0u; index < buffers->count; index++) {
        if (buffers->items[index] == buffer)
            return true;
    }
    if (buffers->count >=
        sizeof(buffers->items) / sizeof(buffers->items[0])) {
        errno = ENOSPC;
        return false;
    }
    buffers->items[buffers->count++] = buffer;
    return true;
}

static bool wl_gpu_frame_publish_release_fence(
    const struct wl_gpu_frame_buffers *buffers, int fence_fd)
{
    int duplicated[WL_SERVER_MAX_CLIENTS * WL_SERVER_MAX_SURFACES];

    if (!buffers || fence_fd < 0) {
        errno = EINVAL;
        return false;
    }
    for (size_t index = 0u; index < buffers->count; index++) {
        duplicated[index] = dup(fence_fd);
        if (duplicated[index] < 0) {
            while (index > 0u)
                close(duplicated[--index]);
            return false;
        }
    }
    for (size_t index = 0u; index < buffers->count; index++) {
        struct wl_server_buffer *buffer = buffers->items[index];

        if (buffer->release_fence_fd >= 0)
            close(buffer->release_fence_fd);
        buffer->release_fence_fd = duplicated[index];
    }
    return true;
}
static void wl_gpu_surface_cache_reset(
    struct wl_server_renderer *renderer,
    struct wl_gpu_surface_cache *cache);

static int wl_renderer_try_gpu(struct wl_server_renderer *renderer)
{
    const uint64_t required = ARMOS_DRM_CAP_SCANOUT |
        ARMOS_DRM_CAP_SHARED_BUFFERS | ARMOS_DRM_CAP_SYNC_FDS;
    struct wl_gpu_backend_config backend_config;
    struct wl_gpu_presenter *presenter = NULL;
    struct wl_gpu_backend *backend = NULL;
    armos_drm_context_create_t context_create;
    armos_drm_info_t info;
    uint64_t size;
    int fd = -1;

    fd = open("/dev/dri/card0", O_RDWR, 0);
    if (fd < 0)
        return -1;
    memset(&info, 0, sizeof(info));
    if (ioctl(fd, ARMOS_DRM_IOCTL_GET_INFO, &info) < 0 ||
        info.abi_version != ARMOS_DRM_ABI_VERSION ||
        info.struct_size < sizeof(info) ||
        (info.capabilities & required) != required ||
        !info.scanout_count || !info.scanout_width ||
        !info.scanout_height ||
        info.scanout_width > UINT32_MAX / sizeof(uint32_t)) {
        errno = ENOTSUP;
        goto fail;
    }
    if ((wl_gpu_backend_supported_capabilities() &
         WL_GPU_COMPOSITOR_REQUIRED_CAPS) !=
        WL_GPU_COMPOSITOR_REQUIRED_CAPS) {
        errno = ENOTSUP;
        goto fail;
    }
    size = (uint64_t)info.scanout_width * sizeof(uint32_t) *
        info.scanout_height;
    if (!size || size > SIZE_MAX || size > UINT32_MAX) {
        errno = EOVERFLOW;
        goto fail;
    }
    memset(&backend_config, 0, sizeof(backend_config));
    backend_config.render_node = "/dev/dri/renderD128";
    backend_config.width = info.scanout_width;
    backend_config.height = info.scanout_height;
    backend = wl_gpu_backend_create(&backend_config);
    if (!backend) {
        errno = ENOTSUP;
        goto fail;
    }
    /*
     * Never substitute the GPU path for the reference renderer on the
     * strength of function pointers alone.  Decorations, translucent client
     * pixels, cursor ordering and triple-buffer damage all depend on these
     * exact semantic guarantees.
     */
    if ((wl_gpu_backend_capabilities(backend) &
         WL_GPU_COMPOSITOR_REQUIRED_CAPS) !=
        WL_GPU_COMPOSITOR_REQUIRED_CAPS) {
        errno = ENOTSUP;
        goto fail;
    }
    presenter = calloc(1, sizeof(*presenter));
    if (!presenter)
        goto fail;
    presenter->card_fd = -1;
    if (!wl_gpu_presenter_init_fd(
            presenter, fd, true, info.scanout_width,
            info.scanout_height))
        goto fail;

    renderer->output_backend = WL_RENDERER_OUTPUT_GPU;
    renderer->framebuffer.width = info.scanout_width;
    renderer->framebuffer.height = info.scanout_height;
    renderer->framebuffer.pitch =
        info.scanout_width * sizeof(uint32_t);
    renderer->framebuffer.bpp = 32u;
    renderer->framebuffer.size = (uint32_t)size;
    renderer->framebuffer.format = ARMOS_FB_FORMAT_ARGB8888;
    renderer->gpu_backend = backend;
    renderer->gpu_presenter = presenter;
    renderer->drm_fd = fd;
    renderer->present_buffer_count = WL_GPU_MAX_OUTPUT_BUFFERS;
    renderer->direct_present = false;

    if ((info.capabilities &
         (ARMOS_DRM_CAP_CONTEXTS | ARMOS_DRM_CAP_RESOURCE_TRANSFER)) ==
        (ARMOS_DRM_CAP_CONTEXTS | ARMOS_DRM_CAP_RESOURCE_TRANSFER)) {
        memset(&context_create, 0, sizeof(context_create));
        context_create.abi_version = ARMOS_DRM_ABI_VERSION;
        if (ioctl(fd, ARMOS_DRM_IOCTL_CONTEXT_CREATE,
                  &context_create) == 0) {
            renderer->drm_context_id = context_create.context_id;
            renderer->gpu_buffer_import = true;
        }
    }
    return 0;

fail:
    if (presenter) {
        wl_gpu_presenter_destroy(presenter);
        free(presenter);
        fd = -1;
    }
    wl_gpu_backend_destroy(backend);
    if (fd >= 0)
        close(fd);
    return -1;
}

static void wl_renderer_select_draw_buffer(
    struct wl_server_renderer *renderer)
{
    if (!renderer || !renderer->direct_present ||
        renderer->present_buffer_count == 0u)
        return;
    renderer->present_draw_buffer =
        renderer->present_buffer_count > 1u ?
        (renderer->present_front_buffer + 1u) %
            renderer->present_buffer_count :
        renderer->present_front_buffer;
    renderer->canvas = (uint32_t *)(void *)(
        (uint8_t *)(void *)renderer->mapped_buffers +
        (size_t)renderer->present_draw_buffer * renderer->canvas_size);
    renderer->dirty_tiles = renderer->dirty_tile_storage +
        (size_t)renderer->present_draw_buffer *
        renderer->dirty_tile_word_count;
}

static void wl_renderer_mark_all_buffers_dirty(
    struct wl_server_renderer *renderer)
{
    if (!renderer || !renderer->dirty_tile_storage)
        return;
    memset(renderer->dirty_tile_storage, 0xff,
           renderer->dirty_tile_word_count *
           renderer->present_buffer_count *
           sizeof(*renderer->dirty_tile_storage));
}

static int wl_renderer_try_drm(struct wl_server_renderer *renderer)
{
    const unsigned long long required =
        ARMOS_DRM_CAP_SCANOUT |
        ARMOS_DRM_CAP_TRANSFER_2D |
        ARMOS_DRM_CAP_BUFFER_OBJECTS |
        ARMOS_DRM_CAP_CPU_MAPPABLE;
    armos_drm_info_t info;
    armos_drm_bo_create_t create;
    armos_drm_context_create_t context_create;
    uint64_t size;
    void *mapping;
    int fd;

    fd = open("/dev/dri/card0", O_RDWR, 0);
    if (fd < 0)
        return -1;
    memset(&info, 0, sizeof(info));
    if (ioctl(fd, ARMOS_DRM_IOCTL_GET_INFO, &info) < 0 ||
        info.abi_version != ARMOS_DRM_ABI_VERSION ||
        info.struct_size < sizeof(info) ||
        (info.capabilities & required) != required ||
        info.scanout_count == 0u ||
        info.scanout_width == 0u || info.scanout_height == 0u ||
        info.scanout_width > UINT32_MAX / sizeof(uint32_t)) {
        close(fd);
        errno = ENOTSUP;
        return -1;
    }
    size = (uint64_t)info.scanout_width * sizeof(uint32_t) *
           info.scanout_height;
    if (size == 0u || size > SIZE_MAX || size > UINT32_MAX) {
        close(fd);
        errno = EOVERFLOW;
        return -1;
    }
    memset(&create, 0, sizeof(create));
    create.abi_version = ARMOS_DRM_ABI_VERSION;
    create.flags = ARMOS_DRM_BO_CPU_READ |
                   ARMOS_DRM_BO_CPU_WRITE |
                   ARMOS_DRM_BO_SCANOUT;
    create.size = size;
    create.width = info.scanout_width;
    create.height = info.scanout_height;
    create.stride = info.scanout_width * sizeof(uint32_t);
    create.format = ARMOS_DRM_FORMAT_BGRA8888;
    if (ioctl(fd, ARMOS_DRM_IOCTL_BO_CREATE, &create) < 0) {
        close(fd);
        return -1;
    }
    mapping = mmap(NULL, (size_t)size, PROT_READ | PROT_WRITE,
                   MAP_SHARED, fd, (off_t)create.map_offset);
    if (mapping == MAP_FAILED) {
        armos_drm_bo_destroy_t destroy = {
            .handle = create.handle,
        };

        (void)ioctl(fd, ARMOS_DRM_IOCTL_BO_DESTROY, &destroy);
        close(fd);
        return -1;
    }
    renderer->drm_fd = fd;
    renderer->drm_handle = create.handle;
    renderer->output_backend = WL_RENDERER_OUTPUT_DRM;
    renderer->framebuffer.width = info.scanout_width;
    renderer->framebuffer.height = info.scanout_height;
    renderer->framebuffer.pitch = create.stride;
    renderer->framebuffer.bpp = 32u;
    renderer->framebuffer.size = (uint32_t)size;
    renderer->framebuffer.format = ARMOS_FB_FORMAT_ARGB8888;
    renderer->mapped_buffers = mapping;
    renderer->mapped_buffers_size = (size_t)size;
    renderer->present_buffer_count = 1u;
    renderer->present_front_buffer = 0u;
    renderer->present_draw_buffer = 0u;
    renderer->direct_present = true;
    renderer->canvas = mapping;
    if ((info.capabilities &
         (ARMOS_DRM_CAP_CONTEXTS |
          ARMOS_DRM_CAP_RESOURCE_TRANSFER |
          ARMOS_DRM_CAP_SHARED_BUFFERS |
          ARMOS_DRM_CAP_SYNC_FDS)) ==
        (ARMOS_DRM_CAP_CONTEXTS |
         ARMOS_DRM_CAP_RESOURCE_TRANSFER |
         ARMOS_DRM_CAP_SHARED_BUFFERS |
         ARMOS_DRM_CAP_SYNC_FDS)) {
        memset(&context_create, 0, sizeof(context_create));
        context_create.abi_version = ARMOS_DRM_ABI_VERSION;
        if (ioctl(fd, ARMOS_DRM_IOCTL_CONTEXT_CREATE,
                  &context_create) == 0) {
            renderer->drm_context_id = context_create.context_id;
            renderer->gpu_buffer_import = true;
        }
    }
    return 0;
}

int wl_renderer_init(struct wl_server_renderer *renderer, bool headless)
{
    struct armos_fb_map map = {0};
    uint64_t tile_count;
    uint64_t tile_word_count;
    uint64_t dirty_word_count;

    if (!renderer)
        return -1;
    memset(renderer, 0, sizeof(*renderer));
    renderer->framebuffer_fd = -1;
    renderer->drm_fd = -1;
    renderer->headless = headless;
    renderer->present_buffer_count = 1u;

    if (headless) {
        renderer->output_backend = WL_RENDERER_OUTPUT_HEADLESS;
        renderer->framebuffer.width = WL_HEADLESS_WIDTH;
        renderer->framebuffer.height = WL_HEADLESS_HEIGHT;
        renderer->framebuffer.pitch = WL_HEADLESS_WIDTH * 4u;
        renderer->framebuffer.bpp = 32u;
        renderer->framebuffer.size =
            renderer->framebuffer.pitch * WL_HEADLESS_HEIGHT;
        renderer->framebuffer.format = ARMOS_FB_FORMAT_ARGB8888;
    } else {
        if (wl_renderer_try_gpu(renderer) == 0)
            goto output_ready;
        if (wl_renderer_try_drm(renderer) == 0)
            goto output_ready;
        renderer->output_backend = WL_RENDERER_OUTPUT_FRAMEBUFFER;
        renderer->framebuffer_fd = open("/dev/fb0", O_RDWR, 0);
        if (renderer->framebuffer_fd < 0)
            return -1;
        if (ioctl(renderer->framebuffer_fd, ARMOS_FBIOACQUIRE, NULL) < 0)
            goto fail;
        if (ioctl(renderer->framebuffer_fd, ARMOS_FBIOGET_INFO,
                  &renderer->framebuffer) < 0)
            goto fail;
        if (renderer->framebuffer.bpp != 32u ||
            renderer->framebuffer.format != ARMOS_FB_FORMAT_ARGB8888 ||
            renderer->framebuffer.pitch < renderer->framebuffer.width * 4u ||
            renderer->framebuffer.size <
                renderer->framebuffer.pitch * renderer->framebuffer.height) {
            errno = ENOTSUP;
            goto fail;
        }
        if (ioctl(renderer->framebuffer_fd, ARMOS_FBIOGET_MAP, &map) == 0 &&
            map.buffer_count != 0u &&
            map.front_buffer < map.buffer_count &&
            map.buffer_size == renderer->framebuffer.size &&
            (uint64_t)map.mapping_size ==
                (uint64_t)map.buffer_size * map.buffer_count) {
            renderer->mapped_buffers = mmap(
                NULL, map.mapping_size, PROT_READ | PROT_WRITE,
                MAP_SHARED, renderer->framebuffer_fd, 0);
            if (renderer->mapped_buffers != MAP_FAILED) {
                renderer->mapped_buffers_size = map.mapping_size;
                renderer->present_buffer_count = map.buffer_count;
                renderer->present_front_buffer = map.front_buffer;
                renderer->present_draw_buffer = map.front_buffer;
                renderer->direct_present = true;
                renderer->canvas = (uint32_t *)(void *)(
                    (uint8_t *)(void *)renderer->mapped_buffers +
                    (size_t)map.front_buffer * map.buffer_size);
            } else {
                renderer->mapped_buffers = NULL;
            }
        }
    }

output_ready:
    renderer->canvas_size = renderer->framebuffer.size;
    if (!renderer->direct_present) {
        renderer->canvas = malloc(renderer->canvas_size);
        if (!renderer->canvas)
            goto fail;
    }
    renderer->tile_columns =
        (renderer->framebuffer.width + WL_RENDER_TILE_SIZE - 1u) /
        WL_RENDER_TILE_SIZE;
    renderer->tile_rows =
        (renderer->framebuffer.height + WL_RENDER_TILE_SIZE - 1u) /
        WL_RENDER_TILE_SIZE;
    tile_count = (uint64_t)renderer->tile_columns * renderer->tile_rows;
    tile_word_count = (tile_count + 63u) / 64u;
    if (tile_count == 0u ||
        tile_word_count > SIZE_MAX / sizeof(uint64_t)) {
        errno = EOVERFLOW;
        goto fail;
    }
    renderer->dirty_tile_word_count = (size_t)tile_word_count;
    dirty_word_count = tile_word_count * renderer->present_buffer_count;
    if (renderer->present_buffer_count != 0u &&
        dirty_word_count / renderer->present_buffer_count !=
            tile_word_count) {
        errno = EOVERFLOW;
        goto fail;
    }
    if (dirty_word_count > SIZE_MAX / sizeof(uint64_t)) {
        errno = EOVERFLOW;
        goto fail;
    }
    renderer->dirty_tile_storage = calloc(
        (size_t)dirty_word_count, sizeof(*renderer->dirty_tile_storage));
    if (!renderer->dirty_tile_storage)
        goto fail;
    renderer->dirty_tiles = renderer->dirty_tile_storage +
        (size_t)renderer->present_draw_buffer *
        renderer->dirty_tile_word_count;
    return 0;

fail:
    wl_renderer_destroy(renderer);
    return -1;
}

void wl_renderer_destroy(struct wl_server_renderer *renderer)
{
    if (!renderer)
        return;
    free(renderer->dirty_tile_storage);
    renderer->dirty_tile_storage = NULL;
    renderer->dirty_tiles = NULL;
    renderer->dirty_tile_word_count = 0u;
    renderer->tile_columns = 0u;
    renderer->tile_rows = 0u;
    if (renderer->mapped_buffers) {
        (void)munmap(renderer->mapped_buffers,
                     renderer->mapped_buffers_size);
    } else {
        free(renderer->canvas);
    }
    renderer->mapped_buffers = NULL;
    renderer->mapped_buffers_size = 0u;
    renderer->canvas = NULL;
    if (renderer->gpu_surface_cache) {
        for (size_t index = 0u;
             index < renderer->gpu_surface_cache_count; index++)
            wl_gpu_surface_cache_reset(
                renderer, &renderer->gpu_surface_cache[index]);
        free(renderer->gpu_surface_cache);
        renderer->gpu_surface_cache = NULL;
        renderer->gpu_surface_cache_count = 0u;
    }
    for (size_t index = 0u;
         index < sizeof(renderer->gpu_pointer_images) /
                 sizeof(renderer->gpu_pointer_images[0]); index++) {
        wl_gpu_backend_destroy_image(
            renderer->gpu_backend, renderer->gpu_pointer_images[index]);
        renderer->gpu_pointer_images[index] = NULL;
    }
    if (renderer->drm_fd >= 0) {
        if (renderer->drm_context_id != 0u) {
            armos_drm_context_destroy_t context_destroy = {
                .context_id = renderer->drm_context_id,
            };

            (void)ioctl(renderer->drm_fd,
                        ARMOS_DRM_IOCTL_CONTEXT_DESTROY,
                        &context_destroy);
        }
        if (renderer->drm_handle != 0u) {
            armos_drm_bo_destroy_t destroy = {
                .handle = renderer->drm_handle,
            };

            (void)ioctl(renderer->drm_fd, ARMOS_DRM_IOCTL_BO_DESTROY,
                        &destroy);
        }
        if (renderer->output_backend != WL_RENDERER_OUTPUT_GPU)
            close(renderer->drm_fd);
    }
    if (renderer->gpu_presenter) {
        wl_gpu_presenter_destroy(renderer->gpu_presenter);
        free(renderer->gpu_presenter);
        renderer->gpu_presenter = NULL;
    }
    wl_gpu_backend_destroy(renderer->gpu_backend);
    renderer->gpu_backend = NULL;
    renderer->drm_fd = -1;
    renderer->drm_handle = 0u;
    renderer->drm_context_id = 0u;
    renderer->gpu_buffer_import = false;
    if (renderer->framebuffer_fd >= 0)
        close(renderer->framebuffer_fd);
    renderer->framebuffer_fd = -1;
}

static void wl_renderer_put_pixel(struct wl_server_renderer *renderer,
                                  int32_t x, int32_t y, uint32_t color)
{
    uint32_t canvas_width = renderer->framebuffer.pitch / 4u;

    if (x < 0 || y < 0 ||
        (uint32_t)x >= renderer->framebuffer.width ||
        (uint32_t)y >= renderer->framebuffer.height)
        return;
    if (renderer->clip_enabled &&
        (x < renderer->clip_x0 || y < renderer->clip_y0 ||
         x >= renderer->clip_x1 || y >= renderer->clip_y1))
        return;
    renderer->canvas[(uint32_t)y * canvas_width + (uint32_t)x] =
        wl_render_blend_pixel(
            renderer->canvas[(uint32_t)y * canvas_width + (uint32_t)x],
            color);
}

static int wl_point_in_rounded_rect(uint32_t x, uint32_t y,
                                    uint32_t width, uint32_t height,
                                    uint32_t radius)
{
    int32_t dx;
    int32_t dy;

    if (x >= width || y >= height)
        return 0;
    if ((x >= radius && x < width - radius) ||
        (y >= radius && y < height - radius))
        return 1;
    dx = x < radius ? (int32_t)radius - 1 - (int32_t)x :
         (int32_t)x - (int32_t)(width - radius);
    dy = y < radius ? (int32_t)radius - 1 - (int32_t)y :
         (int32_t)y - (int32_t)(height - radius);
    return dx * dx + dy * dy < (int32_t)(radius * radius);
}

static bool wl_renderer_local_clip(
    const struct wl_server_renderer *renderer,
    int32_t x, int32_t y, uint32_t width, uint32_t height,
    uint32_t *column0, uint32_t *row0,
    uint32_t *column1, uint32_t *row1)
{
    int32_t x0 = x < 0 ? -x : 0;
    int32_t y0 = y < 0 ? -y : 0;
    int32_t x1 = (int32_t)width;
    int32_t y1 = (int32_t)height;

    if (x + x1 > (int32_t)renderer->framebuffer.width)
        x1 = (int32_t)renderer->framebuffer.width - x;
    if (y + y1 > (int32_t)renderer->framebuffer.height)
        y1 = (int32_t)renderer->framebuffer.height - y;
    if (renderer->clip_enabled) {
        if (x + x0 < renderer->clip_x0)
            x0 = renderer->clip_x0 - x;
        if (y + y0 < renderer->clip_y0)
            y0 = renderer->clip_y0 - y;
        if (x + x1 > renderer->clip_x1)
            x1 = renderer->clip_x1 - x;
        if (y + y1 > renderer->clip_y1)
            y1 = renderer->clip_y1 - y;
    }
    if (x0 >= x1 || y0 >= y1)
        return false;
    *column0 = (uint32_t)x0;
    *row0 = (uint32_t)y0;
    *column1 = (uint32_t)x1;
    *row1 = (uint32_t)y1;
    return true;
}

static void wl_renderer_rounded_rect(struct wl_server_renderer *renderer,
                                     int32_t x, int32_t y,
                                     uint32_t width, uint32_t height,
                                     uint32_t radius, uint32_t color)
{
    uint32_t column0;
    uint32_t row0;
    uint32_t column1;
    uint32_t row1;

    if (width <= radius * 2u || height <= radius * 2u)
        radius = 0u;
    if (!wl_renderer_local_clip(renderer, x, y, width, height,
                                &column0, &row0, &column1, &row1))
        return;
    for (uint32_t row = row0; row < row1; row++) {
        for (uint32_t column = column0; column < column1; column++) {
            if (radius == 0u ||
                wl_point_in_rounded_rect(column, row, width, height, radius)) {
                wl_renderer_put_pixel(renderer, x + (int32_t)column,
                                      y + (int32_t)row, color);
            }
        }
    }
}

static void wl_renderer_shadow_ring(
    struct wl_server_renderer *renderer,
    int32_t x, int32_t y, uint32_t width, uint32_t height,
    uint32_t radius, int32_t inner_x, int32_t inner_y,
    uint32_t inner_width, uint32_t inner_height,
    uint32_t color)
{
    uint32_t column0;
    uint32_t row0;
    uint32_t column1;
    uint32_t row1;
    uint32_t right_column;
    uint32_t bottom_row;
    int32_t right_offset;
    int32_t bottom_offset;

    if (!wl_renderer_local_clip(renderer, x, y, width, height,
                                &column0, &row0, &column1, &row1))
        return;

    right_offset = inner_x + (int32_t)inner_width - x;
    if (right_offset <= (int32_t)column0)
        right_column = column0;
    else if (right_offset >= (int32_t)column1)
        right_column = column1;
    else
        right_column = (uint32_t)right_offset;
    bottom_offset = inner_y + (int32_t)inner_height - y;
    if (bottom_offset <= (int32_t)row0)
        bottom_row = row0;
    else if (bottom_offset >= (int32_t)row1)
        bottom_row = row1;
    else
        bottom_row = (uint32_t)bottom_offset;

    /* Right band, including the bottom-right corner. */
    for (uint32_t row = row0; row < row1; row++) {
        for (uint32_t column = right_column;
             column < column1; column++) {
            if (wl_point_in_rounded_rect(
                    column, row, width, height, radius))
                wl_renderer_put_pixel(
                    renderer, x + (int32_t)column,
                    y + (int32_t)row, color);
        }
    }
    /* Bottom band stops before the right band to avoid blending twice. */
    for (uint32_t row = bottom_row; row < row1; row++) {
        for (uint32_t column = column0;
             column < right_column; column++) {
            if (wl_point_in_rounded_rect(
                    column, row, width, height, radius))
                wl_renderer_put_pixel(
                    renderer, x + (int32_t)column,
                    y + (int32_t)row, color);
        }
    }
}

static void wl_renderer_circle(struct wl_server_renderer *renderer,
                               int32_t center_x, int32_t center_y,
                               uint32_t radius, uint32_t color)
{
    int32_t limit = (int32_t)radius;

    for (int32_t y = -limit; y <= limit; y++) {
        for (int32_t x = -limit; x <= limit; x++) {
            if (x * x + y * y <= (int32_t)(radius * radius))
                wl_renderer_put_pixel(renderer, center_x + x, center_y + y,
                                      color);
        }
    }
}

static uint8_t wl_title_glyph_row(char character, uint32_t row)
{
    static const uint8_t letters[26][7] = {
        {14,17,17,31,17,17,17}, {30,17,17,30,17,17,30},
        {14,17,16,16,16,17,14}, {30,17,17,17,17,17,30},
        {31,16,16,30,16,16,31}, {31,16,16,30,16,16,16},
        {14,17,16,23,17,17,15}, {17,17,17,31,17,17,17},
        {14,4,4,4,4,4,14}, {7,2,2,2,18,18,12},
        {17,18,20,24,20,18,17}, {16,16,16,16,16,16,31},
        {17,27,21,21,17,17,17}, {17,25,21,19,17,17,17},
        {14,17,17,17,17,17,14}, {30,17,17,30,16,16,16},
        {14,17,17,17,21,18,13}, {30,17,17,30,20,18,17},
        {15,16,16,14,1,1,30}, {31,4,4,4,4,4,4},
        {17,17,17,17,17,17,14}, {17,17,17,17,17,10,4},
        {17,17,17,21,21,21,10}, {17,17,10,4,10,17,17},
        {17,17,10,4,4,4,4}, {31,1,2,4,8,16,31}
    };
    static const uint8_t digits[10][7] = {
        {14,17,19,21,25,17,14}, {4,12,4,4,4,4,14},
        {14,17,1,2,4,8,31}, {30,1,1,14,1,1,30},
        {2,6,10,18,31,2,2}, {31,16,16,30,1,1,30},
        {14,16,16,30,17,17,14}, {31,1,2,4,8,8,8},
        {14,17,17,14,17,17,14}, {14,17,17,15,1,1,14}
    };

    if (row >= 7u)
        return 0u;
    if (character >= 'a' && character <= 'z')
        character = (char)(character - 'a' + 'A');
    if (character >= 'A' && character <= 'Z')
        return letters[(uint32_t)(character - 'A')][row];
    if (character >= '0' && character <= '9')
        return digits[(uint32_t)(character - '0')][row];
    if (character == '-')
        return row == 3u ? 31u : 0u;
    if (character == '.')
        return row == 6u ? 4u : 0u;
    return 0u;
}

static uint32_t wl_renderer_surface_frame_width(
    const struct wl_server_surface *surface)
{
    if (surface && surface->minimized &&
        surface->width > WL_WINDOW_MINIMIZED_WIDTH)
        return WL_WINDOW_MINIMIZED_WIDTH;
    return surface ? surface->width : 0u;
}

static void wl_renderer_title(struct wl_server_renderer *renderer,
                              const struct wl_server_surface *surface)
{
    const uint32_t glyph_width = 6u;
    uint32_t frame_width = wl_renderer_surface_frame_width(surface);
    uint32_t maximum = frame_width > 70u ?
        (frame_width - 70u) / glyph_width : 0u;
    size_t length = strnlen(surface->title, sizeof(surface->title));
    int32_t x;

    if (length > maximum)
        length = maximum;
    x = surface->x + (int32_t)((frame_width -
                                (uint32_t)length * glyph_width) / 2u);
    if (x < surface->x + 62)
        x = surface->x + 62;
    for (size_t character = 0u; character < length; character++) {
        for (uint32_t row = 0u; row < 7u; row++) {
            uint8_t bits = wl_title_glyph_row(
                surface->title[character], row);

            for (uint32_t column = 0u; column < 5u; column++) {
                if (bits & (1u << (4u - column))) {
                    wl_renderer_put_pixel(
                        renderer, x + (int32_t)column,
                        surface->y + 10 + (int32_t)row,
                        0xff343438u);
                }
            }
        }
        x += (int32_t)glyph_width;
    }
}

static bool wl_renderer_surface_clip_opaque(
    const struct wl_server_renderer *renderer,
    const struct wl_server_surface *surface)
{
    int32_t content_x;
    int32_t content_y;
    uint32_t source_x0;
    uint32_t source_y0;
    uint32_t width;
    uint32_t height;

    if (!renderer->clip_enabled || surface->shaded ||
        wl_surface_is_child_role(surface))
        return false;
    content_x = surface->x;
    content_y = surface->y +
        (wl_surface_has_server_decoration(surface) ?
         (int32_t)WL_WINDOW_TITLE_HEIGHT : 0);
    if (renderer->clip_x0 < content_x ||
        renderer->clip_y0 < content_y ||
        renderer->clip_x1 > content_x + (int32_t)surface->width ||
        renderer->clip_y1 > content_y + (int32_t)surface->height)
        return false;
    if (surface->opaque)
        return true;

    source_x0 = (uint32_t)(renderer->clip_x0 - content_x);
    source_y0 = (uint32_t)(renderer->clip_y0 - content_y);
    width = (uint32_t)(renderer->clip_x1 - renderer->clip_x0);
    height = (uint32_t)(renderer->clip_y1 - renderer->clip_y0);
    for (size_t index = 0u;
         index < surface->opaque_region.rect_count; index++) {
        const struct wl_renderer_rect *rect =
            &surface->opaque_region.rects[index];

        if (rect->x0 <= (int32_t)source_x0 &&
            rect->y0 <= (int32_t)source_y0 &&
            rect->x1 >= (int32_t)(source_x0 + width) &&
            rect->y1 >= (int32_t)(source_y0 + height))
            return true;
    }
    if (!surface->pixels)
        return false;
    for (uint32_t row = 0u; row < height; row++) {
        const uint32_t *source = surface->pixels +
            (size_t)(source_y0 + row) * surface->pixels_pitch + source_x0;

        for (uint32_t column = 0u; column < width; column++) {
            if ((source[column] >> 24) != 255u)
                return false;
        }
    }
    return true;
}

static void wl_renderer_draw_surface_frame(
    struct wl_server_renderer *renderer,
    const struct wl_server_surface *surface, bool active,
    uint32_t title_height, uint32_t frame_width, uint32_t frame_height)
{
    for (uint32_t spread = 8u; spread > 0u; spread -= 2u) {
        uint32_t alpha = 8u + (8u - spread) * 3u;

        wl_renderer_shadow_ring(
            renderer, surface->x - (int32_t)spread,
            surface->y - (int32_t)spread + 4,
            frame_width + spread * 2u, frame_height + spread * 2u,
            WL_WINDOW_RADIUS + spread,
            surface->x, surface->y, frame_width, frame_height,
            alpha << 24);
    }
    wl_renderer_rounded_rect(renderer, surface->x, surface->y,
                             frame_width, frame_height, WL_WINDOW_RADIUS,
                             0xfff4f4f5u);
    wl_renderer_rounded_rect(
        renderer, surface->x + 1, surface->y + 1,
        frame_width > 2u ? frame_width - 2u : frame_width,
        title_height,
        WL_WINDOW_RADIUS > 1u ? WL_WINDOW_RADIUS - 1u : 0u,
        active ? WL_WINDOW_TITLE_ACTIVE : WL_WINDOW_TITLE_INACTIVE);
    wl_renderer_circle(renderer, surface->x + 14, surface->y + 14,
                       5u, 0xffff5f57u);
    wl_renderer_circle(renderer, surface->x + 30, surface->y + 14,
                       5u, 0xffffbd2eu);
    wl_renderer_circle(renderer, surface->x + 46, surface->y + 14,
                       5u, 0xff28c840u);
    wl_renderer_title(renderer, surface);
}

static void wl_renderer_draw_surface(struct wl_server_renderer *renderer,
                                     const struct wl_server_surface *surface,
                                     bool active, bool opaque_clip)
{
    uint32_t title_height =
        surface->role == WL_SERVER_SURFACE_ROLE_TOPLEVEL &&
        wl_surface_has_server_decoration(surface) ?
        WL_WINDOW_TITLE_HEIGHT : 0u;
    uint32_t frame_width = wl_renderer_surface_frame_width(surface);
    uint32_t frame_height = surface->shaded ?
        title_height : surface->height + title_height;
    int32_t content_y = surface->y + (int32_t)title_height;
    int32_t content_x = surface->x;
    bool content_only_damage = false;

    if (wl_surface_is_child_role(surface)) {
        const struct wl_server_surface *ancestor = surface;
        size_t depth = 0u;

        content_x = surface->subsurface_x;
        content_y = surface->subsurface_y;
        while (wl_surface_is_child_role(ancestor) &&
               ancestor->parent &&
               depth++ < WL_SERVER_MAX_SURFACES) {
            ancestor = ancestor->parent;
            if (wl_surface_is_child_role(ancestor)) {
                content_x += ancestor->subsurface_x;
                content_y += ancestor->subsurface_y;
            } else {
                content_x += ancestor->x;
                content_y += ancestor->y +
                    (wl_surface_has_server_decoration(ancestor) ?
                     (int32_t)WL_WINDOW_TITLE_HEIGHT : 0);
            }
        }
        if (depth > WL_SERVER_MAX_SURFACES)
            return;
        goto draw_content;
    }

    if (!surface->shaded && (opaque_clip ||
        (renderer->clip_enabled && surface->opaque &&
         renderer->clip_x0 >= content_x &&
         renderer->clip_y0 >= content_y &&
         renderer->clip_x1 <= content_x + (int32_t)surface->width &&
         renderer->clip_y1 <= content_y + (int32_t)surface->height)))
        content_only_damage = true;

    if (!content_only_damage && title_height != 0u) {
        wl_renderer_draw_surface_frame(
            renderer, surface, active,
            title_height, frame_width, frame_height);
    }
    if (surface->shaded)
        return;

draw_content:
    {
        int32_t source_x0 = content_x < 0 ? -content_x : 0;
        int32_t source_y0 = content_y < 0 ? -content_y : 0;
        int32_t source_x1 = (int32_t)surface->width;
        int32_t source_y1 = (int32_t)surface->height;
        uint32_t canvas_width = renderer->framebuffer.pitch /
            sizeof(uint32_t);

        if (content_x + source_x1 > (int32_t)renderer->framebuffer.width)
            source_x1 = (int32_t)renderer->framebuffer.width - content_x;
        if (content_y + source_y1 > (int32_t)renderer->framebuffer.height)
            source_y1 = (int32_t)renderer->framebuffer.height - content_y;
        if (renderer->clip_enabled) {
            if (content_x + source_x0 < renderer->clip_x0)
                source_x0 = renderer->clip_x0 - content_x;
            if (content_y + source_y0 < renderer->clip_y0)
                source_y0 = renderer->clip_y0 - content_y;
            if (content_x + source_x1 > renderer->clip_x1)
                source_x1 = renderer->clip_x1 - content_x;
            if (content_y + source_y1 > renderer->clip_y1)
                source_y1 = renderer->clip_y1 - content_y;
        }
        if (source_x0 >= source_x1 || source_y0 >= source_y1)
            return;
        {
            uint32_t *destination = renderer->canvas +
                (uint32_t)(content_y + source_y0) * canvas_width +
                (uint32_t)(content_x + source_x0);
            const uint32_t *source = surface->pixels +
                (size_t)(uint32_t)source_y0 * surface->pixels_pitch +
                (uint32_t)source_x0;
            uint32_t width = (uint32_t)(source_x1 - source_x0);
            uint32_t height = (uint32_t)(source_y1 - source_y0);

            if (surface->opaque || opaque_clip)
                wl_render_copy_rect(destination, canvas_width,
                                    source, surface->pixels_pitch,
                                    width, height);
            else
                wl_render_blend_rect(destination, canvas_width,
                                     source, surface->pixels_pitch,
                                     width, height);
        }
    }
}

static void wl_renderer_line(struct wl_server_renderer *renderer,
                             int32_t x0, int32_t y0,
                             int32_t x1, int32_t y1,
                             uint32_t color, int32_t radius)
{
    int32_t dx = x1 >= x0 ? x1 - x0 : x0 - x1;
    int32_t sx = x0 < x1 ? 1 : -1;
    int32_t dy = y1 >= y0 ? y0 - y1 : y1 - y0;
    int32_t sy = y0 < y1 ? 1 : -1;
    int32_t error = dx + dy;

    for (;;) {
        for (int32_t row = -radius; row <= radius; row++) {
            for (int32_t column = -radius; column <= radius; column++)
                wl_renderer_put_pixel(
                    renderer, x0 + column, y0 + row, color);
        }
        if (x0 == x1 && y0 == y1)
            break;
        {
            int32_t twice_error = error * 2;

            if (twice_error >= dy) {
                error += dy;
                x0 += sx;
            }
            if (twice_error <= dx) {
                error += dx;
                y0 += sy;
            }
        }
    }
}

static void wl_renderer_draw_resize_cursor(
    struct wl_server_renderer *renderer, int32_t x, int32_t y,
    enum wl_server_pointer_cursor cursor)
{
    static const int8_t horizontal[][4] = {
        {-8, 0, 8, 0}, {-8, 0, -4, -4}, {-8, 0, -4, 4},
        {8, 0, 4, -4}, {8, 0, 4, 4}
    };
    static const int8_t vertical[][4] = {
        {0, -8, 0, 8}, {0, -8, -4, -4}, {0, -8, 4, -4},
        {0, 8, -4, 4}, {0, 8, 4, 4}
    };
    static const int8_t northwest_southeast[][4] = {
        {-7, -7, 7, 7}, {-7, -7, -1, -7}, {-7, -7, -7, -1},
        {7, 7, 1, 7}, {7, 7, 7, 1}
    };
    static const int8_t northeast_southwest[][4] = {
        {-7, 7, 7, -7}, {-7, 7, -1, 7}, {-7, 7, -7, 1},
        {7, -7, 1, -7}, {7, -7, 7, -1}
    };
    const int8_t (*segments)[4];
    size_t count;

    if (cursor == WL_SERVER_POINTER_CURSOR_RESIZE_EW) {
        segments = horizontal;
        count = sizeof(horizontal) / sizeof(horizontal[0]);
    } else if (cursor == WL_SERVER_POINTER_CURSOR_RESIZE_NS) {
        segments = vertical;
        count = sizeof(vertical) / sizeof(vertical[0]);
    } else if (cursor == WL_SERVER_POINTER_CURSOR_RESIZE_NWSE) {
        segments = northwest_southeast;
        count = sizeof(northwest_southeast) /
            sizeof(northwest_southeast[0]);
    } else {
        segments = northeast_southwest;
        count = sizeof(northeast_southwest) /
            sizeof(northeast_southwest[0]);
    }
    for (size_t index = 0u; index < count; index++) {
        wl_renderer_line(
            renderer, x + segments[index][0], y + segments[index][1],
            x + segments[index][2], y + segments[index][3],
            0xff101010u, 1);
    }
    for (size_t index = 0u; index < count; index++) {
        wl_renderer_line(
            renderer, x + segments[index][0], y + segments[index][1],
            x + segments[index][2], y + segments[index][3],
            0xfffafafau, 0);
    }
}

static void wl_renderer_draw_pointer(struct wl_server *server)
{
    struct wl_server_renderer *renderer = &server->renderer;
    int32_t x = server->pointer_x;
    int32_t y = server->pointer_y;

    if (server->pointer_cursor != WL_SERVER_POINTER_CURSOR_ARROW) {
        wl_renderer_draw_resize_cursor(
            renderer, x, y, server->pointer_cursor);
        return;
    }
    for (size_t row = 0;
         row < sizeof(wl_pointer_shape) / sizeof(wl_pointer_shape[0]);
         row++) {
        for (size_t column = 0;
             wl_pointer_shape[row][column] != '\0'; column++) {
            char pixel = wl_pointer_shape[row][column];

            if (pixel != '.')
                wl_renderer_put_pixel(
                    renderer, x + (int32_t)column, y + (int32_t)row,
                    pixel == 'X' ? 0xff101010u : 0xfffafafau);
        }
    }
}

static void wl_renderer_draw_resize_outline(struct wl_server *server)
{
    struct wl_server_renderer *renderer = &server->renderer;
    int32_t x;
    int32_t y;
    int32_t right;
    int32_t bottom;

    if (!server->resize_surface || !server->resize_surface->used)
        return;
    x = server->resize_x;
    y = server->resize_y;
    right = x + (int32_t)server->resize_width - 1;
    bottom = y + (int32_t)server->resize_height +
        (int32_t)WL_WINDOW_TITLE_HEIGHT - 1;

    wl_renderer_line(renderer, x, y, right, y, 0xff20242au, 1);
    wl_renderer_line(renderer, right, y, right, bottom, 0xff20242au, 1);
    wl_renderer_line(renderer, right, bottom, x, bottom, 0xff20242au, 1);
    wl_renderer_line(renderer, x, bottom, x, y, 0xff20242au, 1);
    wl_renderer_line(renderer, x, y, right, y,
                     WL_WINDOW_TITLE_ACTIVE, 0);
    wl_renderer_line(renderer, right, y, right, bottom,
                     WL_WINDOW_TITLE_ACTIVE, 0);
    wl_renderer_line(renderer, right, bottom, x, bottom,
                     WL_WINDOW_TITLE_ACTIVE, 0);
    wl_renderer_line(renderer, x, bottom, x, y,
                     WL_WINDOW_TITLE_ACTIVE, 0);
}

static void wl_renderer_build_scene(struct wl_server *server,
                                    struct wl_renderer_scene *scene)
{
    for (size_t client_index = 0u;
         client_index < WL_SERVER_MAX_CLIENTS; client_index++) {
        struct wl_server_client *client = &server->clients[client_index];

        if (!client->used)
            continue;
        for (size_t index = 0u; index < WL_SERVER_MAX_SURFACES; index++) {
            struct wl_server_surface *surface = &client->surfaces[index];
            size_t position;

            if (!surface->used || !surface->mapped ||
                surface->role == WL_SERVER_SURFACE_ROLE_CURSOR ||
                (!surface->pixels &&
                 (!surface->current_buffer ||
                  !surface->current_buffer->gpu_image)))
                continue;
            position = scene->count;
            while (position > 0u &&
                   scene->ordered[position - 1u]->z_order >
                       surface->z_order) {
                scene->ordered[position] =
                    scene->ordered[position - 1u];
                position--;
            }
            scene->ordered[position] = surface;
            scene->count++;
        }
    }
}

static size_t wl_renderer_scene_first_visible(
    const struct wl_server *server,
    const struct wl_renderer_scene *scene,
    bool *opaque_cover)
{
    const struct wl_server_renderer *renderer = &server->renderer;

    *opaque_cover = false;
    if (!renderer->clip_enabled)
        return 0u;
    /*
     * If an opaque top-level content area completely covers the clip, every
     * surface below it is invisible. Starting there avoids rebuilding an
     * entire hidden scene for animated opaque clients such as teapot-demo.
     */
    for (size_t index = scene->count; index > 0u; index--) {
        const struct wl_server_surface *surface =
            scene->ordered[index - 1u];

        if (wl_renderer_surface_clip_opaque(renderer, surface)) {
            *opaque_cover = true;
            return index - 1u;
        }
    }
    return 0u;
}

static void wl_renderer_draw_scene(
    struct wl_server *server, const struct wl_renderer_scene *scene,
    size_t first, bool first_opaque)
{
    for (size_t index = first; index < scene->count; index++) {
        wl_renderer_draw_surface(
            &server->renderer, scene->ordered[index],
            scene->ordered[index] == server->focus_surface,
            first_opaque && index == first);
    }
}

static uint64_t wl_gpu_signature_mix(uint64_t hash, uint64_t value)
{
    hash ^= value;
    return hash * UINT64_C(1099511628211);
}

static uint64_t wl_gpu_surface_signature(
    const struct wl_server_surface *surface, bool active)
{
    uint64_t hash = UINT64_C(1469598103934665603);

    if (!surface->current_buffer ||
        !surface->current_buffer->gpu_image)
        hash = wl_gpu_signature_mix(
            hash, surface->gpu_content_generation);
    hash = wl_gpu_signature_mix(hash, surface->width);
    hash = wl_gpu_signature_mix(hash, surface->height);
    hash = wl_gpu_signature_mix(hash, surface->role);
    hash = wl_gpu_signature_mix(hash, surface->opaque);
    hash = wl_gpu_signature_mix(hash, surface->server_decorated);
    hash = wl_gpu_signature_mix(hash, surface->fullscreen);
    hash = wl_gpu_signature_mix(hash, surface->minimized);
    hash = wl_gpu_signature_mix(hash, surface->shaded);
    hash = wl_gpu_signature_mix(hash, active);
    for (size_t index = 0u;
         index < sizeof(surface->title) && surface->title[index]; index++)
        hash = wl_gpu_signature_mix(
            hash, (unsigned char)surface->title[index]);
    return hash;
}

static void wl_gpu_surface_cache_reset(
    struct wl_server_renderer *renderer,
    struct wl_gpu_surface_cache *cache)
{
    if (!renderer || !cache)
        return;
    wl_gpu_backend_destroy_image(renderer->gpu_backend, cache->image);
    free(cache->pixels);
    memset(cache, 0, sizeof(*cache));
}

void wl_renderer_release_surface_gpu(
    struct wl_server_renderer *renderer,
    const struct wl_server_surface *surface)
{
    if (!renderer || !renderer->gpu_surface_cache || !surface)
        return;
    for (size_t index = 0u;
         index < renderer->gpu_surface_cache_count; index++) {
        struct wl_gpu_surface_cache *cache =
            &renderer->gpu_surface_cache[index];

        if (cache->surface == surface) {
            wl_gpu_surface_cache_reset(renderer, cache);
            return;
        }
    }
}

static struct wl_gpu_surface_cache *wl_gpu_surface_cache_get(
    struct wl_server_renderer *renderer,
    const struct wl_server_surface *surface)
{
    struct wl_gpu_surface_cache *free_cache = NULL;

    if (!renderer->gpu_surface_cache) {
        size_t count = WL_SERVER_MAX_CLIENTS * WL_SERVER_MAX_SURFACES;

        renderer->gpu_surface_cache = calloc(
            count, sizeof(*renderer->gpu_surface_cache));
        if (!renderer->gpu_surface_cache)
            return NULL;
        renderer->gpu_surface_cache_count = count;
    }
    for (size_t index = 0u;
         index < renderer->gpu_surface_cache_count; index++) {
        struct wl_gpu_surface_cache *cache =
            &renderer->gpu_surface_cache[index];

        if (cache->surface == surface)
            return cache;
        if (!cache->surface && !free_cache)
            free_cache = cache;
    }
    if (free_cache)
        free_cache->surface = surface;
    return free_cache;
}

static bool wl_gpu_surface_cache_configure(
    struct wl_server_renderer *renderer,
    struct wl_gpu_surface_cache *cache,
    uint32_t width, uint32_t height)
{
    struct wl_gpu_image_config config;
    uint64_t stride;
    uint64_t size;

    if (!renderer || !cache || !width || !height)
        return false;
    stride = (uint64_t)width * sizeof(uint32_t);
    size = stride * height;
    if (stride > UINT32_MAX || size > SIZE_MAX)
        return false;
    if (cache->image && cache->width == width && cache->height == height)
        return true;
    wl_gpu_backend_destroy_image(renderer->gpu_backend, cache->image);
    cache->image = NULL;
    free(cache->pixels);
    cache->pixels = NULL;
    cache->signature = 0u;
    cache->pixels = calloc(1u, (size_t)size);
    if (!cache->pixels)
        return false;
    memset(&config, 0, sizeof(config));
    config.width = width;
    config.height = height;
    config.stride = (uint32_t)stride;
    config.alpha = true;
    cache->image = wl_gpu_backend_create_image(
        renderer->gpu_backend, &config);
    if (!cache->image) {
        free(cache->pixels);
        cache->pixels = NULL;
        return false;
    }
    cache->width = width;
    cache->height = height;
    cache->stride = (uint32_t)stride;
    return true;
}

static bool wl_gpu_surface_cache_update(
    struct wl_server *server, struct wl_gpu_surface_cache *cache,
    const struct wl_server_surface *surface, bool active)
{
    struct wl_server_renderer *renderer = &server->renderer;
    struct wl_server_surface local_surface;
    struct wl_server_renderer local_renderer;
    struct wl_gpu_rect full;
    uint64_t signature;
    uint32_t title_height;
    uint32_t frame_width;
    uint32_t frame_height;
    uint32_t margin_x;
    uint32_t margin_y;
    uint32_t width;
    uint32_t height;

    signature = wl_gpu_surface_signature(surface, active);
    if (cache->image && cache->signature == signature)
        return true;
    title_height = surface->role == WL_SERVER_SURFACE_ROLE_TOPLEVEL &&
        wl_surface_has_server_decoration(surface) ?
        WL_WINDOW_TITLE_HEIGHT : 0u;
    frame_width = wl_renderer_surface_frame_width(surface);
    frame_height = surface->shaded ? title_height :
        surface->height + title_height;
    margin_x = title_height ? 8u : 0u;
    margin_y = title_height ? 4u : 0u;
    width = frame_width + margin_x * 2u;
    height = frame_height + (title_height ? 16u : 0u);
    if (!width || !height ||
        !wl_gpu_surface_cache_configure(renderer, cache, width, height))
        return false;
    memset(cache->pixels, 0,
           (size_t)cache->stride * cache->height);

    local_renderer = *renderer;
    local_renderer.canvas = cache->pixels;
    local_renderer.canvas_size = (size_t)cache->stride * cache->height;
    local_renderer.framebuffer.width = cache->width;
    local_renderer.framebuffer.height = cache->height;
    local_renderer.framebuffer.pitch = cache->stride;
    local_renderer.framebuffer.size =
        (uint32_t)local_renderer.canvas_size;
    local_renderer.clip_enabled = false;
    local_surface = *surface;
    local_surface.parent = NULL;
    local_surface.x = (int32_t)margin_x;
    local_surface.y = (int32_t)margin_y;
    if (wl_surface_is_child_role(surface)) {
        local_surface.role = WL_SERVER_SURFACE_ROLE_TOPLEVEL;
        local_surface.server_decorated = false;
    }
    if (surface->current_buffer && surface->current_buffer->gpu_image) {
        uint32_t local_title_height =
            wl_surface_has_server_decoration(&local_surface) ?
            WL_WINDOW_TITLE_HEIGHT : 0u;

        if (local_title_height != 0u) {
            wl_renderer_draw_surface_frame(
                &local_renderer, &local_surface, active,
                local_title_height, frame_width, frame_height);
        }
    } else {
        wl_renderer_draw_surface(
            &local_renderer, &local_surface, active, false);
    }

    if (surface->opaque && !surface->shaded) {
        uint32_t content_y = margin_y + title_height;
        uint32_t content_width = surface->width < cache->width - margin_x ?
            surface->width : cache->width - margin_x;
        uint32_t content_height = surface->height < cache->height - content_y ?
            surface->height : cache->height - content_y;

        for (uint32_t row = 0u; row < content_height; row++) {
            uint32_t *pixels = cache->pixels +
                (size_t)(content_y + row) * (cache->stride / 4u) +
                margin_x;

            for (uint32_t column = 0u;
                 column < content_width; column++)
                pixels[column] |= UINT32_C(0xff000000);
        }
    }
    full.x = 0u;
    full.y = 0u;
    full.width = cache->width;
    full.height = cache->height;
    if (!wl_gpu_backend_upload(
            renderer->gpu_backend, cache->image, &full,
            cache->pixels, cache->stride))
        return false;
    cache->offset_x = -(int32_t)margin_x;
    cache->offset_y = -(int32_t)margin_y;
    cache->signature = signature;
    return true;
}

static bool wl_gpu_blit_clipped(
    struct wl_server_renderer *renderer, struct wl_gpu_image *image,
    uint32_t image_width, uint32_t image_height,
    int32_t destination_x, int32_t destination_y, bool alpha)
{
    struct wl_gpu_rect source = {0};
    struct wl_gpu_rect destination = {0};
    int64_t x0 = destination_x;
    int64_t y0 = destination_y;
    int64_t x1 = x0 + image_width;
    int64_t y1 = y0 + image_height;

    if (x0 < 0)
        x0 = 0;
    if (y0 < 0)
        y0 = 0;
    if (x1 > renderer->framebuffer.width)
        x1 = renderer->framebuffer.width;
    if (y1 > renderer->framebuffer.height)
        y1 = renderer->framebuffer.height;
    if (renderer->clip_enabled) {
        if (x0 < renderer->clip_x0)
            x0 = renderer->clip_x0;
        if (y0 < renderer->clip_y0)
            y0 = renderer->clip_y0;
        if (x1 > renderer->clip_x1)
            x1 = renderer->clip_x1;
        if (y1 > renderer->clip_y1)
            y1 = renderer->clip_y1;
    }
    if (x0 >= x1 || y0 >= y1)
        return true;
    source.x = (uint32_t)(x0 - destination_x);
    source.y = (uint32_t)(y0 - destination_y);
    source.width = (uint32_t)(x1 - x0);
    source.height = (uint32_t)(y1 - y0);
    destination.x = (uint32_t)x0;
    destination.y = (uint32_t)y0;
    destination.width = source.width;
    destination.height = source.height;
    return wl_gpu_backend_blit(
        renderer->gpu_backend, image, &source, &destination, alpha);
}

static bool wl_gpu_fill_clipped(
    struct wl_server_renderer *renderer, int32_t x, int32_t y,
    int32_t width, int32_t height, uint32_t color)
{
    struct wl_gpu_rect rectangle;
    int64_t x1 = (int64_t)x + width;
    int64_t y1 = (int64_t)y + height;

    if (x < 0)
        x = 0;
    if (y < 0)
        y = 0;
    if (x1 > renderer->framebuffer.width)
        x1 = renderer->framebuffer.width;
    if (y1 > renderer->framebuffer.height)
        y1 = renderer->framebuffer.height;
    if (renderer->clip_enabled) {
        if (x < renderer->clip_x0)
            x = renderer->clip_x0;
        if (y < renderer->clip_y0)
            y = renderer->clip_y0;
        if (x1 > renderer->clip_x1)
            x1 = renderer->clip_x1;
        if (y1 > renderer->clip_y1)
            y1 = renderer->clip_y1;
    }
    if (x >= x1 || y >= y1)
        return true;
    rectangle.x = (uint32_t)x;
    rectangle.y = (uint32_t)y;
    rectangle.width = (uint32_t)(x1 - x);
    rectangle.height = (uint32_t)(y1 - y);
    return wl_gpu_backend_fill(renderer->gpu_backend, &rectangle, color);
}

static bool wl_gpu_draw_resize_outline(struct wl_server *server)
{
    struct wl_server_renderer *renderer = &server->renderer;
    int32_t x;
    int32_t y;
    int32_t width;
    int32_t height;

    if (!server->resize_surface || !server->resize_surface->used)
        return true;
    x = server->resize_x;
    y = server->resize_y;
    width = (int32_t)server->resize_width;
    height = (int32_t)server->resize_height +
        (int32_t)WL_WINDOW_TITLE_HEIGHT;
    return
        wl_gpu_fill_clipped(renderer, x - 1, y - 1, width + 2, 3,
                            0xff20242au) &&
        wl_gpu_fill_clipped(renderer, x + width - 2, y - 1, 3,
                            height + 2, 0xff20242au) &&
        wl_gpu_fill_clipped(renderer, x - 1, y + height - 2,
                            width + 2, 3, 0xff20242au) &&
        wl_gpu_fill_clipped(renderer, x - 1, y - 1, 3,
                            height + 2, 0xff20242au) &&
        wl_gpu_fill_clipped(renderer, x, y, width, 1,
                            WL_WINDOW_TITLE_ACTIVE) &&
        wl_gpu_fill_clipped(renderer, x + width - 1, y, 1, height,
                            WL_WINDOW_TITLE_ACTIVE) &&
        wl_gpu_fill_clipped(renderer, x, y + height - 1, width, 1,
                            WL_WINDOW_TITLE_ACTIVE) &&
        wl_gpu_fill_clipped(renderer, x, y, 1, height,
                            WL_WINDOW_TITLE_ACTIVE);
}

static bool wl_gpu_prepare_pointer(
    struct wl_server *server, struct wl_gpu_image **image)
{
    struct wl_server local_server;
    struct wl_gpu_image_config config;
    struct wl_gpu_rect full;
    uint32_t pixels[WL_POINTER_DAMAGE_SIZE * WL_POINTER_DAMAGE_SIZE];
    uint32_t cursor = (uint32_t)server->pointer_cursor;

    if (cursor >= sizeof(server->renderer.gpu_pointer_images) /
                  sizeof(server->renderer.gpu_pointer_images[0]))
        return false;
    *image = server->renderer.gpu_pointer_images[cursor];
    if (*image)
        return true;
    memset(pixels, 0, sizeof(pixels));
    memset(&local_server, 0, sizeof(local_server));
    local_server.renderer.canvas = pixels;
    local_server.renderer.canvas_size = sizeof(pixels);
    local_server.renderer.framebuffer.width = WL_POINTER_DAMAGE_SIZE;
    local_server.renderer.framebuffer.height = WL_POINTER_DAMAGE_SIZE;
    local_server.renderer.framebuffer.pitch =
        WL_POINTER_DAMAGE_SIZE * sizeof(uint32_t);
    local_server.pointer_x = WL_POINTER_DAMAGE_OFFSET;
    local_server.pointer_y = WL_POINTER_DAMAGE_OFFSET;
    local_server.pointer_cursor = server->pointer_cursor;
    wl_renderer_draw_pointer(&local_server);

    memset(&config, 0, sizeof(config));
    config.width = WL_POINTER_DAMAGE_SIZE;
    config.height = WL_POINTER_DAMAGE_SIZE;
    config.stride = WL_POINTER_DAMAGE_SIZE * sizeof(uint32_t);
    config.alpha = true;
    *image = wl_gpu_backend_create_image(
        server->renderer.gpu_backend, &config);
    if (!*image)
        return false;
    full.x = 0u;
    full.y = 0u;
    full.width = WL_POINTER_DAMAGE_SIZE;
    full.height = WL_POINTER_DAMAGE_SIZE;
    if (!wl_gpu_backend_upload(
            server->renderer.gpu_backend, *image, &full,
            pixels, config.stride)) {
        wl_gpu_backend_destroy_image(
            server->renderer.gpu_backend, *image);
        *image = NULL;
        return false;
    }
    server->renderer.gpu_pointer_images[cursor] = *image;
    return true;
}

static uint64_t wl_server_tag_frame_callbacks(struct wl_server *server)
{
    uint64_t presentation_serial = 0u;

    if (!server)
        return 0u;
    for (size_t client_index = 0u;
         client_index < WL_SERVER_MAX_CLIENTS; client_index++) {
        struct wl_server_client *client = &server->clients[client_index];

        if (!client->used)
            continue;
        for (size_t surface_index = 0u;
             surface_index < WL_SERVER_MAX_SURFACES; surface_index++) {
            struct wl_server_surface *surface =
                &client->surfaces[surface_index];

            if (!surface->used)
                continue;
            for (size_t callback_index = 0u;
                 callback_index < WL_SERVER_MAX_CALLBACKS;
                 callback_index++) {
                struct wl_server_callback *callback =
                    &surface->callbacks[callback_index];

                if (!callback->used || callback->presentation_serial != 0u)
                    continue;
                if (presentation_serial == 0u) {
                    presentation_serial =
                        ++server->next_presentation_serial;
                    if (presentation_serial == 0u)
                        presentation_serial =
                            ++server->next_presentation_serial;
                }
                callback->presentation_serial = presentation_serial;
            }
        }
    }
    return presentation_serial;
}

static int wl_renderer_gpu_present_event(
    int fd, uint32_t mask, void *data)
{
    struct wl_server *server = data;
    struct wl_server_renderer *renderer;
    bool render_again;
    uint32_t output_index = 0u;

    if (!server)
        return -1;
    renderer = &server->renderer;
    if ((mask & WL_EVENT_READABLE) == 0u ||
        (mask & WL_EVENT_ERROR) != 0u ||
        !wl_gpu_presenter_complete(renderer->gpu_presenter, fd,
                                   &output_index)) {
        server->fatal_error = true;
        return -1;
    }
    if (output_index >= WL_GPU_MAX_OUTPUT_BUFFERS ||
        !server->gpu_present_source ||
        server->gpu_present_fence_fd != fd) {
        server->fatal_error = true;
        return -1;
    }
    (void)wl_event_source_remove(server->gpu_present_source);
    server->gpu_present_source = NULL;
    server->gpu_present_fence_fd = -1;
    if (renderer->profile_enabled &&
        server->gpu_present_started_us[output_index] != 0u) {
        uint64_t finished_us = wl_renderer_monotonic_us();

        if (finished_us >=
            server->gpu_present_started_us[output_index])
            renderer->profile_present_us +=
                finished_us -
                server->gpu_present_started_us[output_index];
        renderer->profile_present_pixels +=
            server->gpu_present_pixels[output_index];
    }
    server->gpu_present_started_us[output_index] = 0u;
    server->gpu_present_pixels[output_index] = 0u;
    if (server->gpu_present_serial[output_index] != 0u) {
        if (wl_server_complete_frame_callbacks(
                server, server->gpu_present_serial[output_index]) < 0) {
            server->fatal_error = true;
            return -1;
        }
        server->gpu_present_serial[output_index] = 0u;
    }

    if (wl_gpu_presenter_pending(renderer->gpu_presenter)) {
        int next_fence_fd;
        uint32_t next_output_index;

        if (!wl_gpu_presenter_next_fence(
                renderer->gpu_presenter, &next_fence_fd,
                &next_output_index) ||
            next_output_index >= WL_GPU_MAX_OUTPUT_BUFFERS) {
            server->fatal_error = true;
            return -1;
        }
        server->gpu_present_source = wl_event_loop_add_fd(
            server->event_loop, next_fence_fd,
            WL_EVENT_READABLE | WL_EVENT_HANGUP | WL_EVENT_ERROR,
            wl_renderer_gpu_present_event, server);
        if (!server->gpu_present_source) {
            server->fatal_error = true;
            return -1;
        }
        server->gpu_present_fence_fd = next_fence_fd;
    }

    render_again = server->render_pending ||
        server->scene_damage_pending || server->damage_pending ||
        !server->pointer_presented ||
        server->presented_pointer_x != server->pointer_x ||
        server->presented_pointer_y != server->pointer_y ||
        server->presented_pointer_cursor != server->pointer_cursor;
    server->render_pending = false;
    if (render_again && wl_server_schedule_render(server, false) < 0) {
        server->fatal_error = true;
        return -1;
    }
    return 0;
}

static int wl_renderer_compose_gpu(struct wl_server *server)
{
    struct wl_server_renderer *renderer = &server->renderer;
    struct wl_renderer_scene scene = {0};
    struct wl_renderer_rect presentation = {0};
    struct wl_gpu_frame frame;
    struct wl_gpu_image *pointer_image = NULL;
    struct wl_gpu_frame_buffers frame_buffers = {0};
    bool have_damage = false;

    if (!renderer->gpu_backend || !renderer->gpu_presenter ||
        !wl_gpu_backend_begin_frame(renderer->gpu_backend, &frame)) {
        fprintf(stderr, "armos-wlcomp: GPU begin frame failed\n");
        return -1;
    }
    if (frame.output_count != renderer->present_buffer_count ||
        frame.output_index >= frame.output_count ||
        frame.output_index >= WL_GPU_MAX_OUTPUT_BUFFERS ||
        !renderer->dirty_tile_storage) {
        fprintf(stderr, "armos-wlcomp: invalid GPU frame "
                "index=%u count=%u expected=%u tiles=%s\n",
                (unsigned)frame.output_index,
                (unsigned)frame.output_count,
                (unsigned)renderer->present_buffer_count,
                renderer->dirty_tile_storage ? "yes" : "no");
        errno = EPROTO;
        goto fail;
    }
    renderer->present_draw_buffer = frame.output_index;
    renderer->dirty_tiles = renderer->dirty_tile_storage +
        (size_t)frame.output_index * renderer->dirty_tile_word_count;
    if ((renderer->gpu_output_initialized_mask &
         (UINT32_C(1) << frame.output_index)) == 0u)
        memset(renderer->dirty_tiles, 0xff,
               renderer->dirty_tile_word_count *
               sizeof(*renderer->dirty_tiles));

    /*
     * The pointer is part of every output image.  A swapchain image can
     * therefore contain a different historical pointer position from the
     * last image submitted globally.  Damage the position retained by this
     * exact image before reusing it, otherwise old cursors accumulate in
     * triple-buffered output.
     */
    if ((renderer->gpu_pointer_valid_mask &
         (UINT32_C(1) << frame.output_index)) == 0u ||
        renderer->gpu_pointer_x[frame.output_index] != server->pointer_x ||
        renderer->gpu_pointer_y[frame.output_index] != server->pointer_y ||
        renderer->gpu_pointer_cursor[frame.output_index] !=
            server->pointer_cursor) {
        if ((renderer->gpu_pointer_valid_mask &
             (UINT32_C(1) << frame.output_index)) != 0u) {
            wl_renderer_mark_rect_in_tiles(
                renderer, renderer->dirty_tiles,
                renderer->gpu_pointer_x[frame.output_index] -
                    WL_POINTER_DAMAGE_OFFSET,
                renderer->gpu_pointer_y[frame.output_index] -
                    WL_POINTER_DAMAGE_OFFSET,
                WL_POINTER_DAMAGE_SIZE, WL_POINTER_DAMAGE_SIZE);
        }
        wl_renderer_mark_rect_in_tiles(
            renderer, renderer->dirty_tiles,
            server->pointer_x - WL_POINTER_DAMAGE_OFFSET,
            server->pointer_y - WL_POINTER_DAMAGE_OFFSET,
            WL_POINTER_DAMAGE_SIZE, WL_POINTER_DAMAGE_SIZE);
    }

    wl_renderer_build_scene(server, &scene);
    if (!wl_gpu_prepare_pointer(server, &pointer_image))
        goto fail;
    for (uint32_t row = 0u; row < renderer->tile_rows; row++) {
        uint32_t column = 0u;

        while (column < renderer->tile_columns) {
            struct wl_renderer_rect run;
            bool opaque_cover;
            size_t first;
            uint32_t run_end;

            if (!wl_renderer_tile_dirty(renderer, column, row)) {
                column++;
                continue;
            }
            run = wl_renderer_tile_rect(renderer, column, row);
            renderer->clip_enabled = true;
            renderer->clip_x0 = run.x0;
            renderer->clip_y0 = run.y0;
            renderer->clip_x1 = run.x1;
            renderer->clip_y1 = run.y1;
            first = wl_renderer_scene_first_visible(
                server, &scene, &opaque_cover);
            run_end = column;
            while (run_end + 1u < renderer->tile_columns &&
                   wl_renderer_tile_dirty(renderer, run_end + 1u, row)) {
                struct wl_renderer_rect candidate =
                    wl_renderer_tile_rect(renderer, run_end + 1u, row);
                bool candidate_opaque;
                size_t candidate_first;

                renderer->clip_x0 = candidate.x0;
                renderer->clip_y0 = candidate.y0;
                renderer->clip_x1 = candidate.x1;
                renderer->clip_y1 = candidate.y1;
                candidate_first = wl_renderer_scene_first_visible(
                    server, &scene, &candidate_opaque);
                if (candidate_first != first ||
                    candidate_opaque != opaque_cover)
                    break;
                run.x1 = candidate.x1;
                run_end++;
            }
            renderer->clip_x0 = run.x0;
            renderer->clip_y0 = run.y0;
            renderer->clip_x1 = run.x1;
            renderer->clip_y1 = run.y1;
            if (!opaque_cover && !wl_gpu_fill_clipped(
                    renderer, run.x0, run.y0,
                    run.x1 - run.x0, run.y1 - run.y0,
                    WL_BACKGROUND))
                goto fail;
            for (size_t index = first; index < scene.count; index++) {
                const struct wl_server_surface *surface =
                    scene.ordered[index];
                struct wl_server_buffer *buffer =
                    surface->current_buffer;
                struct wl_gpu_surface_cache *cache = NULL;
                int32_t destination_x;
                int32_t destination_y;

                if (wl_surface_is_child_role(surface)) {
                    if (wl_surface_content_origin(
                            surface, &destination_x, &destination_y) < 0)
                        goto fail;
                } else {
                    destination_x = surface->x;
                    destination_y = surface->y;
                }
                if (!buffer || !buffer->gpu_image ||
                    wl_surface_has_server_decoration(surface)) {
                    cache = wl_gpu_surface_cache_get(renderer, surface);
                    if (!cache || !wl_gpu_surface_cache_update(
                            server, cache, surface,
                            surface == server->focus_surface))
                        goto fail;
                    if (!wl_gpu_blit_clipped(
                            renderer, cache->image,
                            cache->width, cache->height,
                            destination_x + cache->offset_x,
                            destination_y + cache->offset_y,
                            !surface->opaque ||
                            wl_surface_has_server_decoration(surface)))
                        goto fail;
                }
                if (buffer && buffer->gpu_image && !surface->shaded) {
                    int32_t content_x;
                    int32_t content_y;

                    if (wl_surface_content_origin(
                            surface, &content_x, &content_y) < 0 ||
                        !wl_gpu_blit_clipped(
                            renderer, buffer->gpu_image,
                            buffer->width, buffer->height,
                            content_x, content_y, !surface->opaque))
                        goto fail;
                    if (!wl_gpu_frame_track_buffer(
                            &frame_buffers, buffer))
                        goto fail;
                    renderer->profile_gpu_direct_blits++;
                }
            }
            if (!wl_gpu_draw_resize_outline(server) ||
                !wl_gpu_blit_clipped(
                    renderer, pointer_image,
                    WL_POINTER_DAMAGE_SIZE, WL_POINTER_DAMAGE_SIZE,
                    server->pointer_x - WL_POINTER_DAMAGE_OFFSET,
                    server->pointer_y - WL_POINTER_DAMAGE_OFFSET, true))
                goto fail;
            if (!have_damage) {
                presentation = run;
                have_damage = true;
            } else {
                if (run.x0 < presentation.x0)
                    presentation.x0 = run.x0;
                if (run.y0 < presentation.y0)
                    presentation.y0 = run.y0;
                if (run.x1 > presentation.x1)
                    presentation.x1 = run.x1;
                if (run.y1 > presentation.y1)
                    presentation.y1 = run.y1;
            }
            column = run_end + 1u;
        }
    }
    renderer->clip_enabled = false;
    if (!have_damage) {
        wl_gpu_backend_end_frame(renderer->gpu_backend);
        return 0;
    }
    {
        struct wl_gpu_rect damage = {
            .x = (uint32_t)presentation.x0,
            .y = (uint32_t)presentation.y0,
            .width = (uint32_t)(presentation.x1 - presentation.x0),
            .height = (uint32_t)(presentation.y1 - presentation.y0),
        };
        uint64_t started_us = renderer->profile_enabled ?
            wl_renderer_monotonic_us() : 0u;

        if (!server->event_loop) {
            uint64_t presentation_serial;

            if (!wl_gpu_presenter_present(
                    renderer->gpu_presenter, renderer->gpu_backend,
                    &damage)) {
                fprintf(stderr, "armos-wlcomp: GPU presentation failed "
                        "at %s\n", wl_gpu_presenter_error_string(
                            renderer->gpu_presenter));
                return -1;
            }
            if (started_us != 0u) {
                uint64_t finished_us = wl_renderer_monotonic_us();

                if (finished_us >= started_us)
                    renderer->profile_present_us +=
                        finished_us - started_us;
                renderer->profile_present_pixels +=
                    (uint64_t)damage.width * damage.height;
            }
            presentation_serial = wl_server_tag_frame_callbacks(server);
            if (presentation_serial != 0u &&
                wl_server_complete_frame_callbacks(
                    server, presentation_serial) < 0)
                return -1;
        } else {
            int fence_fd;
            uint32_t output_index;

            if (!wl_gpu_presenter_submit(
                    renderer->gpu_presenter, renderer->gpu_backend,
                    &damage, &fence_fd, &output_index)) {
                fprintf(stderr, "armos-wlcomp: GPU submission failed "
                        "at %s\n", wl_gpu_presenter_error_string(
                            renderer->gpu_presenter));
                return -1;
            }
            if (output_index >= WL_GPU_MAX_OUTPUT_BUFFERS) {
                wl_gpu_presenter_cancel(renderer->gpu_presenter);
                errno = EBUSY;
                return -1;
            }
            if (!wl_gpu_frame_publish_release_fence(
                    &frame_buffers, fence_fd)) {
                fprintf(stderr, "armos-wlcomp: cannot retain GPU "
                        "release fence: %s\n", strerror(errno));
                wl_gpu_presenter_cancel(renderer->gpu_presenter);
                return -1;
            }
            if (!server->gpu_present_source) {
                int oldest_fence_fd;
                uint32_t oldest_output_index;

                if (!wl_gpu_presenter_next_fence(
                        renderer->gpu_presenter, &oldest_fence_fd,
                        &oldest_output_index) ||
                    oldest_fence_fd != fence_fd ||
                    oldest_output_index != output_index) {
                    wl_gpu_presenter_cancel(renderer->gpu_presenter);
                    errno = EPROTO;
                    return -1;
                }
                server->gpu_present_source = wl_event_loop_add_fd(
                    server->event_loop, fence_fd,
                    WL_EVENT_READABLE | WL_EVENT_HANGUP |
                        WL_EVENT_ERROR,
                    wl_renderer_gpu_present_event, server);
                if (!server->gpu_present_source) {
                    wl_gpu_presenter_cancel(renderer->gpu_presenter);
                    return -1;
                }
                server->gpu_present_fence_fd = fence_fd;
            }
            server->gpu_present_started_us[output_index] = started_us;
            server->gpu_present_pixels[output_index] =
                (uint64_t)damage.width * damage.height;
            if (server->gpu_present_serial[output_index] != 0u) {
                wl_gpu_presenter_cancel(renderer->gpu_presenter);
                errno = EPROTO;
                return -1;
            }
            server->gpu_present_serial[output_index] =
                wl_server_tag_frame_callbacks(server);
        }
    }

    renderer->gpu_output_initialized_mask |=
        UINT32_C(1) << frame.output_index;
    renderer->gpu_pointer_valid_mask |=
        UINT32_C(1) << frame.output_index;
    renderer->gpu_pointer_x[frame.output_index] = server->pointer_x;
    renderer->gpu_pointer_y[frame.output_index] = server->pointer_y;
    renderer->gpu_pointer_cursor[frame.output_index] =
        server->pointer_cursor;
    server->pointer_presented = true;
    server->presented_pointer_x = server->pointer_x;
    server->presented_pointer_y = server->pointer_y;
    server->presented_pointer_cursor = server->pointer_cursor;
    server->damage_pending = false;
    renderer->present_front_buffer = renderer->present_draw_buffer;
    memset(renderer->dirty_tiles, 0,
           renderer->dirty_tile_word_count *
           sizeof(*renderer->dirty_tiles));
    return 0;

fail:
    fprintf(stderr, "armos-wlcomp: GPU composition failed: %s\n",
            strerror(errno));
    renderer->clip_enabled = false;
    wl_gpu_backend_end_frame(renderer->gpu_backend);
    return -1;
}

static int wl_renderer_build_canvas(struct wl_server *server)
{
    struct wl_server_renderer *renderer;
    struct wl_renderer_scene scene = {0};
    uint32_t canvas_width;

    if (!server || !server->renderer.canvas)
        return -1;
    renderer = &server->renderer;
    renderer->clip_enabled = false;
    canvas_width = renderer->framebuffer.pitch / 4u;
    wl_render_fill_rect(renderer->canvas, canvas_width, canvas_width,
                        renderer->framebuffer.height, WL_BACKGROUND);

    wl_renderer_build_scene(server, &scene);
    wl_renderer_draw_scene(server, &scene, 0u, false);
    wl_renderer_draw_resize_outline(server);

    if (!renderer->headless)
        wl_renderer_draw_pointer(server);

    return 0;
}

int wl_renderer_compose(struct wl_server *server)
{
    struct wl_server_renderer *renderer;
    int result;

    if (!server)
        return -1;
    wl_renderer_mark_all_buffers_dirty(&server->renderer);
    wl_renderer_select_draw_buffer(&server->renderer);
    if (server->renderer.output_backend == WL_RENDERER_OUTPUT_GPU)
        return wl_renderer_compose_gpu(server);
    if (wl_renderer_build_canvas(server) < 0)
        return -1;
    renderer = &server->renderer;
    server->pointer_presented = true;
    server->presented_pointer_x = server->pointer_x;
    server->presented_pointer_y = server->pointer_y;
    server->presented_pointer_cursor = server->pointer_cursor;
    result = wl_renderer_backend_present_rect(
        renderer, 0, 0, renderer->framebuffer.width,
        renderer->framebuffer.height);
    if (result == 0) {
        renderer->present_front_buffer =
            renderer->present_draw_buffer;
        /*
         * A complete composition subsumes every queued partial update.
         * Keeping an older damage rectangle would repaint stale geometry on
         * the next timer tick.
         */
        server->damage_pending = false;
        memset(renderer->dirty_tiles, 0,
               renderer->dirty_tile_word_count *
               sizeof(*renderer->dirty_tiles));
    }
    return result;
}

int wl_renderer_compose_pointer(struct wl_server *server)
{
    int32_t old_x;
    int32_t old_y;

    if (!server)
        return -1;
    if (!server->pointer_presented)
        return wl_renderer_compose(server);
    old_x = server->presented_pointer_x;
    old_y = server->presented_pointer_y;
    wl_renderer_damage_rect(
        server, old_x - WL_POINTER_DAMAGE_OFFSET,
        old_y - WL_POINTER_DAMAGE_OFFSET,
        WL_POINTER_DAMAGE_SIZE, WL_POINTER_DAMAGE_SIZE);
    wl_renderer_damage_rect(
        server, server->pointer_x - WL_POINTER_DAMAGE_OFFSET,
        server->pointer_y - WL_POINTER_DAMAGE_OFFSET,
        WL_POINTER_DAMAGE_SIZE, WL_POINTER_DAMAGE_SIZE);
    return wl_renderer_compose_damage(server);
}

static size_t wl_renderer_tile_index(
    const struct wl_server_renderer *renderer,
    uint32_t column, uint32_t row)
{
    return (size_t)row * renderer->tile_columns + column;
}

static bool wl_renderer_tile_dirty(
    const struct wl_server_renderer *renderer,
    uint32_t column, uint32_t row)
{
    size_t index = wl_renderer_tile_index(renderer, column, row);

    return (renderer->dirty_tiles[index / 64u] &
            (1ull << (index % 64u))) != 0u;
}

static void wl_renderer_mark_rect_in_tiles(
    const struct wl_server_renderer *renderer, uint64_t *tiles,
    int32_t x, int32_t y, uint32_t width, uint32_t height)
{
    int64_t x1;
    int64_t y1;
    uint32_t column0;
    uint32_t column1;
    uint32_t row0;
    uint32_t row1;

    if (!renderer || !tiles || width == 0u || height == 0u)
        return;
    x1 = (int64_t)x + width;
    y1 = (int64_t)y + height;
    if (x1 <= 0 || y1 <= 0 ||
        x >= (int32_t)renderer->framebuffer.width ||
        y >= (int32_t)renderer->framebuffer.height)
        return;
    if (x < 0)
        x = 0;
    if (y < 0)
        y = 0;
    if (x1 > renderer->framebuffer.width)
        x1 = renderer->framebuffer.width;
    if (y1 > renderer->framebuffer.height)
        y1 = renderer->framebuffer.height;

    column0 = (uint32_t)x / WL_RENDER_TILE_SIZE;
    column1 = ((uint32_t)x1 - 1u) / WL_RENDER_TILE_SIZE;
    row0 = (uint32_t)y / WL_RENDER_TILE_SIZE;
    row1 = ((uint32_t)y1 - 1u) / WL_RENDER_TILE_SIZE;
    for (uint32_t row = row0; row <= row1; row++) {
        for (uint32_t column = column0; column <= column1; column++) {
            size_t index = wl_renderer_tile_index(
                renderer, column, row);

            tiles[index / 64u] |= UINT64_C(1) << (index % 64u);
        }
    }
}

void wl_renderer_damage_rect(struct wl_server *server, int32_t x, int32_t y,
                             uint32_t width, uint32_t height)
{
    struct wl_server_renderer *renderer;

    if (!server || width == 0u || height == 0u)
        return;
    renderer = &server->renderer;
    for (uint32_t buffer = 0u;
         buffer < renderer->present_buffer_count; buffer++) {
        uint64_t *tiles = renderer->dirty_tile_storage +
            (size_t)buffer * renderer->dirty_tile_word_count;

        wl_renderer_mark_rect_in_tiles(
            renderer, tiles, x, y, width, height);
    }
    server->damage_pending = true;
}

static void wl_renderer_damage_surface_extent_at(
    struct wl_server *server, int32_t x, int32_t y,
    uint32_t width, uint32_t height, bool server_decorated)
{
    wl_renderer_damage_rect(
        server, x - 8, y - 4, width + 16u,
        height + (server_decorated ? WL_WINDOW_TITLE_HEIGHT : 0u) + 16u);
}

void wl_renderer_damage_surface_at(
    struct wl_server *server, const struct wl_server_surface *surface,
    int32_t x, int32_t y)
{
    if (!surface)
        return;
    wl_renderer_damage_surface_extent_at(
        server, x, y, surface->width, surface->height,
        wl_surface_has_server_decoration(surface));
}

static struct wl_renderer_rect wl_renderer_tile_rect(
    const struct wl_server_renderer *renderer,
    uint32_t column, uint32_t row)
{
    struct wl_renderer_rect tile;

    tile.x0 = (int32_t)(column * WL_RENDER_TILE_SIZE);
    tile.y0 = (int32_t)(row * WL_RENDER_TILE_SIZE);
    tile.x1 = tile.x0 + (int32_t)WL_RENDER_TILE_SIZE;
    tile.y1 = tile.y0 + (int32_t)WL_RENDER_TILE_SIZE;
    if (tile.x1 > (int32_t)renderer->framebuffer.width)
        tile.x1 = (int32_t)renderer->framebuffer.width;
    if (tile.y1 > (int32_t)renderer->framebuffer.height)
        tile.y1 = (int32_t)renderer->framebuffer.height;
    return tile;
}

static void wl_renderer_clear_rect(struct wl_server_renderer *renderer,
                                   const struct wl_renderer_rect *damage)
{
    uint32_t canvas_width = renderer->framebuffer.pitch / sizeof(uint32_t);
    uint32_t *destination = renderer->canvas +
        (uint32_t)damage->y0 * canvas_width + (uint32_t)damage->x0;

    wl_render_fill_rect(
        destination, canvas_width,
        (uint32_t)(damage->x1 - damage->x0),
        (uint32_t)(damage->y1 - damage->y0), WL_BACKGROUND);
}

static bool wl_renderer_dirty_intersects_rect(
    const struct wl_server_renderer *renderer,
    int32_t x, int32_t y, uint32_t width, uint32_t height)
{
    int64_t x1 = (int64_t)x + width;
    int64_t y1 = (int64_t)y + height;
    uint32_t column0;
    uint32_t column1;
    uint32_t row0;
    uint32_t row1;

    if (width == 0u || height == 0u || x1 <= 0 || y1 <= 0 ||
        x >= (int32_t)renderer->framebuffer.width ||
        y >= (int32_t)renderer->framebuffer.height)
        return false;
    if (x < 0)
        x = 0;
    if (y < 0)
        y = 0;
    if (x1 > renderer->framebuffer.width)
        x1 = renderer->framebuffer.width;
    if (y1 > renderer->framebuffer.height)
        y1 = renderer->framebuffer.height;
    column0 = (uint32_t)x / WL_RENDER_TILE_SIZE;
    column1 = ((uint32_t)x1 - 1u) / WL_RENDER_TILE_SIZE;
    row0 = (uint32_t)y / WL_RENDER_TILE_SIZE;
    row1 = ((uint32_t)y1 - 1u) / WL_RENDER_TILE_SIZE;
    for (uint32_t row = row0; row <= row1; row++) {
        for (uint32_t column = column0; column <= column1; column++) {
            if (wl_renderer_tile_dirty(renderer, column, row))
                return true;
        }
    }
    return false;
}

int wl_renderer_compose_damage(struct wl_server *server)
{
    struct wl_server_renderer *renderer;
    struct wl_renderer_scene scene = {0};
    bool pointer_damage;

    if (!server || !server->renderer.canvas)
        return -1;
    renderer = &server->renderer;
    if (!server->damage_pending)
        return 0;
    wl_renderer_select_draw_buffer(renderer);
    if (renderer->output_backend == WL_RENDERER_OUTPUT_GPU)
        return wl_renderer_compose_gpu(server);

    pointer_damage = !server->pointer_presented ||
        server->presented_pointer_x != server->pointer_x ||
        server->presented_pointer_y != server->pointer_y ||
        server->presented_pointer_cursor != server->pointer_cursor ||
        wl_renderer_dirty_intersects_rect(
            renderer,
            server->presented_pointer_x - WL_POINTER_DAMAGE_OFFSET,
            server->presented_pointer_y - WL_POINTER_DAMAGE_OFFSET,
            WL_POINTER_DAMAGE_SIZE, WL_POINTER_DAMAGE_SIZE);
    if (pointer_damage) {
        wl_renderer_damage_rect(
            server,
            server->presented_pointer_x - WL_POINTER_DAMAGE_OFFSET,
            server->presented_pointer_y - WL_POINTER_DAMAGE_OFFSET,
            WL_POINTER_DAMAGE_SIZE, WL_POINTER_DAMAGE_SIZE);
        wl_renderer_damage_rect(
            server, server->pointer_x - WL_POINTER_DAMAGE_OFFSET,
            server->pointer_y - WL_POINTER_DAMAGE_OFFSET,
            WL_POINTER_DAMAGE_SIZE, WL_POINTER_DAMAGE_SIZE);
    }

    wl_renderer_build_scene(server, &scene);
    /*
     * Determine occlusion per tile, then merge adjacent tiles that share the
     * same first visible surface. This preserves conservative tile coverage
     * while avoiding hundreds of renderer invocations for a large update.
     */
    for (uint32_t row = 0u; row < renderer->tile_rows; row++) {
        uint32_t column = 0u;

        while (column < renderer->tile_columns) {
            struct wl_renderer_rect run;
            bool opaque_cover;
            size_t first;
            uint32_t run_end;

            if (!wl_renderer_tile_dirty(renderer, column, row)) {
                column++;
                continue;
            }
            run = wl_renderer_tile_rect(renderer, column, row);
            renderer->clip_enabled = true;
            renderer->clip_x0 = run.x0;
            renderer->clip_y0 = run.y0;
            renderer->clip_x1 = run.x1;
            renderer->clip_y1 = run.y1;
            first = wl_renderer_scene_first_visible(
                server, &scene, &opaque_cover);
            run_end = column;
            while (run_end + 1u < renderer->tile_columns &&
                   wl_renderer_tile_dirty(renderer, run_end + 1u, row)) {
                struct wl_renderer_rect candidate =
                    wl_renderer_tile_rect(renderer, run_end + 1u, row);
                bool candidate_opaque;
                size_t candidate_first;

                renderer->clip_x0 = candidate.x0;
                renderer->clip_y0 = candidate.y0;
                renderer->clip_x1 = candidate.x1;
                renderer->clip_y1 = candidate.y1;
                candidate_first = wl_renderer_scene_first_visible(
                    server, &scene, &candidate_opaque);
                if (candidate_first != first ||
                    candidate_opaque != opaque_cover)
                    break;
                run.x1 = candidate.x1;
                run_end++;
            }
            renderer->clip_x0 = run.x0;
            renderer->clip_y0 = run.y0;
            renderer->clip_x1 = run.x1;
            renderer->clip_y1 = run.y1;
            if (!opaque_cover)
                wl_renderer_clear_rect(renderer, &run);
            wl_renderer_draw_scene(
                server, &scene, first, opaque_cover);
            wl_renderer_draw_resize_outline(server);
            column = run_end + 1u;
        }
    }
    renderer->clip_enabled = false;
    if (pointer_damage)
        wl_renderer_draw_pointer(server);

    {
        struct wl_renderer_rect presentation = {0};
        bool have_damage = false;

        /*
         * Composition remains tile-local, but publish one final rectangle.
         * Multiple FBIOBLIT calls allowed displayd to expose intermediate
         * cache-clean states on a scanout framebuffer, producing visible
         * stripes and trails on Raspberry Pi.
         */
        for (uint32_t row = 0u; row < renderer->tile_rows; row++) {
            for (uint32_t column = 0u;
                 column < renderer->tile_columns; column++) {
                struct wl_renderer_rect tile;

                if (!wl_renderer_tile_dirty(renderer, column, row))
                    continue;
                tile = wl_renderer_tile_rect(renderer, column, row);
                if (!have_damage) {
                    presentation = tile;
                    have_damage = true;
                } else {
                    if (tile.x0 < presentation.x0)
                        presentation.x0 = tile.x0;
                    if (tile.y0 < presentation.y0)
                        presentation.y0 = tile.y0;
                    if (tile.x1 > presentation.x1)
                        presentation.x1 = tile.x1;
                    if (tile.y1 > presentation.y1)
                        presentation.y1 = tile.y1;
                }
            }
        }
        if (have_damage &&
            wl_renderer_backend_present_rect(
                renderer, presentation.x0, presentation.y0,
                (uint32_t)(presentation.x1 - presentation.x0),
                (uint32_t)(presentation.y1 - presentation.y0)) < 0)
            return -1;
        if (have_damage)
            renderer->present_front_buffer =
                renderer->present_draw_buffer;
        memset(renderer->dirty_tiles, 0,
               renderer->dirty_tile_word_count *
               sizeof(*renderer->dirty_tiles));
    }
    if (pointer_damage) {
        server->pointer_presented = true;
        server->presented_pointer_x = server->pointer_x;
        server->presented_pointer_y = server->pointer_y;
        server->presented_pointer_cursor = server->pointer_cursor;
    }
    server->damage_pending = false;
    return 0;
}

static int wl_surface_buffer_valid(const struct wl_server_buffer *buffer)
{
    uint64_t last_row;
    uint64_t end;

    if (!buffer || !buffer->used ||
        buffer->width == 0 || buffer->height == 0 ||
        buffer->stride < buffer->width * 4u)
        return 0;
    if (buffer->gpu_backed) {
        last_row = (uint64_t)(buffer->height - 1u) * buffer->stride;
        end = last_row + (uint64_t)buffer->width * 4u;
        return (buffer->gpu_image || buffer->drm_mapping) &&
            buffer->drm_size != 0u &&
            end <= buffer->drm_size &&
            (uint64_t)buffer->stride * buffer->height <= UINT32_MAX;
    }
    if (!buffer->pool || !buffer->pool->used ||
        !buffer->pool->mapping)
        return 0;
    last_row = (uint64_t)buffer->offset +
               (uint64_t)(buffer->height - 1u) * buffer->stride;
    end = last_row + (uint64_t)buffer->width * 4u;
    return end <= buffer->pool->size;
}

static int wl_surface_gpu_transfer(
    struct wl_server_renderer *renderer,
    const struct wl_server_buffer *buffer)
{
    armos_drm_bo_transfer_t transfer;

    if (!renderer || !buffer || !buffer->gpu_backed ||
        !wl_surface_buffer_valid(buffer))
        return -1;
    memset(&transfer, 0, sizeof(transfer));
    transfer.context_id = renderer->drm_context_id;
    transfer.handle = buffer->drm_handle;
    transfer.direction = ARMOS_DRM_TRANSFER_DEVICE_TO_CPU;
    transfer.width = buffer->width;
    transfer.height = buffer->height;
    transfer.depth = 1u;
    transfer.stride = buffer->stride;
    transfer.layer_stride = buffer->stride * buffer->height;
    return ioctl(renderer->drm_fd, ARMOS_DRM_IOCTL_BO_TRANSFER, &transfer);
}

static int wl_surface_acquire_fence_event(
    int fd, uint32_t mask, void *data)
{
    struct wl_server_surface *surface = data;
    struct wl_server_client *client;
    struct wl_server *server;
    struct wl_server_surface *root;
    struct wl_server_buffer *buffer;
    struct wl_event_source *source;
    armos_drm_fence_result_t fence_result;
    ssize_t count;
    int result;

    if (!surface || !(client = surface->acquire_client) ||
        !client->used || !(server = client->server))
        return 0;
    source = surface->acquire_fence_source;
    surface->acquire_fence_source = NULL;
    if (source)
        (void)wl_event_source_remove(source);
    memset(&fence_result, 0, sizeof(fence_result));
    count = (mask & WL_EVENT_READABLE) != 0u ?
        read(fd, &fence_result, sizeof(fence_result)) : -1;
    close(fd);
    surface->pending_acquire_fence_fd = -1;
    surface->acquire_fence_source = NULL;
    surface->acquire_client = NULL;
    surface->acquire_commit_pending = false;
    buffer = surface->pending_attach ?
        surface->pending_buffer : surface->current_buffer;
    if (count != (ssize_t)sizeof(fence_result) ||
        fence_result.status != 0 ||
        (mask & (WL_EVENT_HANGUP | WL_EVENT_ERROR)) != 0u ||
        ((!buffer || !buffer->gpu_image) &&
         wl_surface_gpu_transfer(&server->renderer, buffer) < 0)) {
        (void)wl_client_send_error(
            client, surface->object_id, 3u,
            "GPU acquire fence or transfer failed");
        wl_server_disconnect_client(server, client);
        return 0;
    }
    surface->gpu_content_ready = true;
    root = client->blocked_commit_root ?
        client->blocked_commit_root : surface;
    result = wl_surface_apply_commit_tree(server, client, root, true);
    if (result < 0) {
        (void)wl_client_send_error(
            client, surface->object_id, 3u,
            "deferred GPU surface commit failed");
        wl_server_disconnect_client(server, client);
        return 0;
    }
    if (result > 0)
        return 0;
    if (wl_client_set_dispatch_blocked(client, false) < 0) {
        wl_server_disconnect_client(server, client);
        return 0;
    }
    if (client->receive_length > 0u &&
        wl_server_defer_client_dispatch(client) < 0)
        wl_server_disconnect_client(server, client);
    return 0;
}

static int wl_surface_content_origin(
    const struct wl_server_surface *surface, int32_t *x, int32_t *y)
{
    const struct wl_server_surface *ancestor = surface;
    size_t depth = 0u;

    if (!surface || !x || !y)
        return -1;
    if (!wl_surface_is_child_role(surface)) {
        *x = surface->x;
        *y = surface->y +
            (wl_surface_has_server_decoration(surface) ?
             (int32_t)WL_WINDOW_TITLE_HEIGHT : 0);
        return 0;
    }
    *x = surface->subsurface_x;
    *y = surface->subsurface_y;
    while (wl_surface_is_child_role(ancestor) &&
           ancestor->parent &&
           depth++ < WL_SERVER_MAX_SURFACES) {
        ancestor = ancestor->parent;
        if (wl_surface_is_child_role(ancestor)) {
            *x += ancestor->subsurface_x;
            *y += ancestor->subsurface_y;
        } else {
            *x += ancestor->x;
            *y += ancestor->y +
                (wl_surface_has_server_decoration(ancestor) ?
                 (int32_t)WL_WINDOW_TITLE_HEIGHT : 0);
        }
    }
    return depth <= WL_SERVER_MAX_SURFACES ? 0 : -1;
}

static bool wl_surface_clip_local_damage(
    const struct wl_server_surface *surface,
    struct wl_renderer_rect *damage)
{
    if (damage->x0 < 0)
        damage->x0 = 0;
    if (damage->y0 < 0)
        damage->y0 = 0;
    if (damage->x1 > (int32_t)surface->width)
        damage->x1 = (int32_t)surface->width;
    if (damage->y1 > (int32_t)surface->height)
        damage->y1 = (int32_t)surface->height;
    return damage->x0 < damage->x1 && damage->y0 < damage->y1;
}

int wl_surface_release_buffer(struct wl_server_client *client,
                              struct wl_server_surface *surface)
{
    struct wl_server_buffer *buffer;
    struct wl_server_object *manager;
    uint32_t buffer_id;

    if (!client || !surface || !surface->buffer_held)
        return 0;
    buffer = surface->current_buffer;
    surface->buffer_held = false;
    if (!buffer || !buffer->object_alive)
        return 0;
    if (buffer->gpu_backed) {
        manager = wl_client_find_object(
            client, buffer->manager_object_id);
        if (manager &&
            manager->type == WL_SERVER_OBJECT_ARMOS_GPU_BUFFER_MANAGER) {
            buffer_id = buffer->object_id;
            if (buffer->release_fence_fd >= 0) {
                if (wl_client_send_fd_words(
                        client, manager->id, 0u, &buffer_id, 1u,
                        buffer->release_fence_fd) < 0)
                    return -1;
                close(buffer->release_fence_fd);
                buffer->release_fence_fd = -1;
                client->server->renderer.profile_gpu_fenced_releases++;
            } else if (wl_client_send_words(
                           client, manager->id, 1u,
                           &buffer_id, 1u) < 0) {
                return -1;
            } else {
                client->server->renderer.profile_gpu_immediate_releases++;
            }
        }
    }
    return wl_client_send_words(client, buffer->object_id, 0u, NULL, 0u);
}

int wl_surface_commit(struct wl_server *server,
                      struct wl_server_client *client,
                      struct wl_server_surface *surface)
{
    struct wl_server_buffer *buffer;
    uint32_t previous_width;
    uint32_t previous_height;
    bool previous_mapped;
    bool content_changed;
    bool full_copy;
    bool callback_pending = false;
    bool previous_origin_valid = false;
    int32_t previous_origin_x = 0;
    int32_t previous_origin_y = 0;
    int32_t content_x = 0;
    int32_t content_y = 0;
    struct wl_server_buffer *previous_buffer;

    if (!server || !client || !surface)
        return -1;
    content_changed = surface->pending_attach ||
        surface->pending_damage_count != 0u;
    buffer = surface->pending_attach ?
        surface->pending_buffer : surface->current_buffer;
    if (surface->pending_acquire_fence_fd >= 0 &&
        (!buffer || !buffer->gpu_backed || !content_changed))
        return -1;
    if (buffer && buffer->gpu_backed && content_changed &&
        !surface->gpu_content_ready) {
        if (surface->pending_acquire_fence_fd < 0 ||
            surface->acquire_fence_source ||
            surface->acquire_commit_pending)
            return -1;
        surface->acquire_client = client;
        surface->acquire_commit_pending = true;
        surface->acquire_fence_source = wl_event_loop_add_fd(
            server->event_loop, surface->pending_acquire_fence_fd,
            WL_EVENT_READABLE | WL_EVENT_HANGUP | WL_EVENT_ERROR,
            wl_surface_acquire_fence_event, surface);
        if (!surface->acquire_fence_source ||
            wl_client_set_dispatch_blocked(client, true) < 0) {
            if (surface->acquire_fence_source)
                (void)wl_event_source_remove(
                    surface->acquire_fence_source);
            surface->acquire_fence_source = NULL;
            surface->acquire_client = NULL;
            surface->acquire_commit_pending = false;
            return -1;
        }
        return 1;
    }
    for (size_t pending_index = 0u;
         pending_index < WL_SERVER_MAX_CALLBACKS; pending_index++) {
        struct wl_server_callback *pending =
            &surface->pending_callbacks[pending_index];

        if (!pending->used)
            continue;
        for (size_t active_index = 0u;
             active_index < WL_SERVER_MAX_CALLBACKS; active_index++) {
            struct wl_server_callback *active =
                &surface->callbacks[active_index];

            if (active->used)
                continue;
            *active = *pending;
            memset(pending, 0, sizeof(*pending));
            break;
        }
        if (pending->used)
            return -1;
    }
    previous_width = surface->width;
    previous_height = surface->height;
    previous_mapped = surface->mapped;
    previous_buffer = surface->current_buffer;
    if (surface->pending_opaque_region_set) {
        surface->opaque_region = surface->pending_opaque_region;
        surface->pending_opaque_region_set = false;
        memset(&surface->pending_opaque_region, 0,
               sizeof(surface->pending_opaque_region));
    }
    if (previous_mapped) {
        if (wl_surface_is_child_role(surface)) {
            previous_origin_valid =
                wl_surface_content_origin(
                    surface, &previous_origin_x,
                    &previous_origin_y) == 0;
        } else {
            previous_origin_x = surface->x;
            previous_origin_y = surface->y;
            previous_origin_valid = true;
        }
    }
    full_copy = surface->pending_damage_count == 0u;
    if (surface->pending_attach) {
        surface->pending_attach = false;
        surface->pending_buffer = NULL;
        if (previous_buffer && previous_buffer != buffer &&
            wl_surface_release_buffer(client, surface) < 0)
            return -1;
        surface->current_buffer = buffer;
        if (buffer && buffer != previous_buffer)
            surface->buffer_held = true;
        if (!buffer) {
            surface->pixels = NULL;
            surface->pixels_pitch = 0u;
            surface->mapped = false;
            surface->opaque = false;
            surface->width = 0;
            surface->height = 0;
        }
    }
    if (buffer && content_changed) {
        bool same_extent;

        if (!wl_surface_buffer_valid(buffer))
            return -1;
        same_extent = previous_mapped &&
            previous_width == buffer->width &&
            previous_height == buffer->height;
        surface->width = buffer->width;
        surface->height = buffer->height;
        if (surface->resize_from_left)
            surface->x = surface->resize_anchor_right -
                (int32_t)surface->width;
        if (surface->resize_from_top)
            surface->y = surface->resize_anchor_bottom -
                (int32_t)surface->height -
                (wl_surface_has_server_decoration(surface) ?
                 (int32_t)WL_WINDOW_TITLE_HEIGHT : 0);
        surface->resize_from_left = false;
        surface->resize_from_top = false;
        surface->pixels = buffer->gpu_backed ?
            (buffer->gpu_image ? NULL :
             (uint32_t *)(void *)buffer->drm_mapping) :
            (uint32_t *)(void *)(buffer->pool->mapping + buffer->offset);
        surface->pixels_pitch = buffer->stride / sizeof(uint32_t);
        if (!same_extent)
            full_copy = true;
        surface->mapped = true;
        surface->opaque = buffer->format == WL_SHM_FORMAT_XRGB8888;
    }

    if (content_changed) {
        bool same_extent = previous_mapped &&
            (previous_width == surface->width &&
             previous_height == surface->height);

        if (surface->mapped && same_extent &&
            wl_surface_content_origin(surface, &content_x, &content_y) == 0) {
            if (full_copy) {
                wl_renderer_damage_rect(
                    server, content_x, content_y,
                    surface->width, surface->height);
            } else {
                for (size_t index = 0u;
                     index < surface->pending_damage_count; index++) {
                    struct wl_renderer_rect damage =
                        surface->pending_damage[index];

                    if (!wl_surface_clip_local_damage(surface, &damage))
                        continue;
                    wl_renderer_damage_rect(
                        server, content_x + damage.x0,
                        content_y + damage.y0,
                        (uint32_t)(damage.x1 - damage.x0),
                        (uint32_t)(damage.y1 - damage.y0));
                }
            }
            if (wl_server_schedule_render(server, false) < 0)
                return -1;
        } else {
            if (previous_mapped && previous_origin_valid) {
                wl_renderer_damage_surface_extent_at(
                    server, previous_origin_x, previous_origin_y,
                    previous_width, previous_height,
                    wl_surface_has_server_decoration(surface));
            }
            if (surface->mapped) {
                int32_t current_x = surface->x;
                int32_t current_y = surface->y;

                if (!wl_surface_is_child_role(surface) ||
                    wl_surface_content_origin(
                        surface, &current_x, &current_y) == 0) {
                    wl_renderer_damage_surface_at(
                        server, surface, current_x, current_y);
                }
            }
            if (wl_server_schedule_render(server, false) < 0)
                return -1;
        }
        surface->gpu_content_generation =
            ++server->renderer.gpu_content_generation;
    }
    surface->pending_damage_count = 0u;
    surface->gpu_content_ready = false;
    for (size_t index = 0; index < WL_SERVER_MAX_CALLBACKS; index++) {
        if (surface->callbacks[index].used) {
            callback_pending = true;
            break;
        }
    }
    if (callback_pending && !content_changed &&
        wl_server_schedule_render(server, false) < 0)
        return -1;
    wl_client_reclaim_buffers(client);
    return 0;
}

int wl_server_complete_frame_callbacks(struct wl_server *server,
                                       uint64_t presentation_serial)
{
    if (!server)
        return -1;
    for (size_t client_index = 0u;
         client_index < WL_SERVER_MAX_CLIENTS; client_index++) {
        struct wl_server_client *client = &server->clients[client_index];
        bool disconnected = false;

        if (!client->used)
            continue;
        for (size_t surface_index = 0u;
             surface_index < WL_SERVER_MAX_SURFACES; surface_index++) {
            struct wl_server_surface *surface =
                &client->surfaces[surface_index];

            if (!surface->used)
                continue;
            for (size_t callback_index = 0u;
                 callback_index < WL_SERVER_MAX_CALLBACKS;
                 callback_index++) {
                struct wl_server_callback *callback =
                    &surface->callbacks[callback_index];
                uint32_t done;

                if (!callback->used ||
                    callback->presentation_serial != presentation_serial)
                    continue;
                done = ++server->serial;
                if (wl_client_send_words(client, callback->object_id,
                                         0u, &done, 1u) < 0) {
                    /*
                     * An abruptly terminated client may leave a frame
                     * callback pending until its socket reports the hangup.
                     * A broken client connection must not stop the compositor
                     * or disconnect unrelated clients.
                     */
                    wl_server_disconnect_client(server, client);
                    disconnected = true;
                    break;
                }
                wl_client_remove_object(client, callback->object_id, true);
                memset(callback, 0, sizeof(*callback));
            }
            if (disconnected)
                break;
        }
    }
    return 0;
}
