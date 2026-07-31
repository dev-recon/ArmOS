/*
 * ArmOS
 * Copyright (c) 2026 Mohamed Ennassiri
 *
 * Licensed under the Apache License, Version 2.0.
 * See LICENSE for details.
 *
 * File: kernel/platform/qemu_virt/virtio_gpu.c
 * Layer: Kernel / QEMU virt GPU backend
 *
 * Responsibilities:
 * - Negotiate the QEMU VirtIO-GPU device and its control queue.
 * - Implement the qemu-virt side of the common GPU backend contract.
 *
 * Notes:
 * - Device ordering and cache coherency matter under preemption.
 */

#include <kernel/types.h>
#include <kernel/address_space.h>
#include <kernel/fdt.h>
#include "virtio_gpu.h"
#include <kernel/drm_backend.h>
#include <kernel/drm.h>
#include <kernel/virtio_block.h>
#include <kernel/display.h>
#include <kernel/memory.h>
#include <kernel/string.h>
#include <kernel/kprintf.h>
#include <kernel/timer.h>
#include <kernel/spinlock.h>
#include <kernel/task.h>
#include <kernel/arch_barrier.h>
#include <kernel/arch_cpu.h>
#include <kernel/arch_platform.h>
#include <kernel/interrupt.h>
#include <uapi/armos/drm.h>

#define VIRTIO_ID_GPU 16

#define VIRTIO_GPU_F_VIRGL    0
#define VIRTIO_GPU_F_EDID     1
#define VIRTIO_GPU_F_RESOURCE_UUID 2
#define VIRTIO_GPU_F_RESOURCE_BLOB 3
#define VIRTIO_GPU_F_CONTEXT_INIT  4

#define VIRTIO_GPU_CMD_GET_DISPLAY_INFO        0x0100
#define VIRTIO_GPU_CMD_RESOURCE_CREATE_2D      0x0101
#define VIRTIO_GPU_CMD_RESOURCE_UNREF          0x0102
#define VIRTIO_GPU_CMD_SET_SCANOUT             0x0103
#define VIRTIO_GPU_CMD_RESOURCE_FLUSH          0x0104
#define VIRTIO_GPU_CMD_TRANSFER_TO_HOST_2D     0x0105
#define VIRTIO_GPU_CMD_RESOURCE_ATTACH_BACKING 0x0106
#define VIRTIO_GPU_CMD_GET_CAPSET_INFO          0x0108
#define VIRTIO_GPU_CMD_GET_CAPSET               0x0109
#define VIRTIO_GPU_CMD_CTX_CREATE               0x0200
#define VIRTIO_GPU_CMD_CTX_DESTROY              0x0201
#define VIRTIO_GPU_CMD_CTX_ATTACH_RESOURCE      0x0202
#define VIRTIO_GPU_CMD_CTX_DETACH_RESOURCE      0x0203
#define VIRTIO_GPU_CMD_RESOURCE_CREATE_3D       0x0204
#define VIRTIO_GPU_CMD_TRANSFER_TO_HOST_3D      0x0205
#define VIRTIO_GPU_CMD_TRANSFER_FROM_HOST_3D    0x0206
#define VIRTIO_GPU_CMD_SUBMIT_3D                 0x0207

#define VIRTIO_GPU_RESP_OK_NODATA              0x1100
#define VIRTIO_GPU_RESP_OK_DISPLAY_INFO        0x1101
#define VIRTIO_GPU_RESP_OK_CAPSET_INFO         0x1102
#define VIRTIO_GPU_RESP_OK_CAPSET              0x1103

#define VIRTIO_GPU_FORMAT_B8G8R8A8_UNORM       1

#define VIRTIO_GPU_CAPSET_VIRGL                1u
#define VIRTIO_GPU_CAPSET_VIRGL2               2u
#define VIRTIO_GPU_MAX_CAPSETS                 32u
#define VIRTIO_GPU_FLAG_FENCE                  (1u << 0)

/* Gallium resource constants carried by the VirtIO-GPU VirGL protocol. */
#define PIPE_BUFFER                            0u
#define PIPE_TEXTURE_2D                        2u
#define PIPE_FORMAT_NONE                       0u
#define PIPE_FORMAT_B8G8R8A8_UNORM             1u
#define PIPE_BIND_DEPTH_STENCIL                (1u << 0)
#define PIPE_BIND_RENDER_TARGET                (1u << 1)
#define PIPE_BIND_SAMPLER_VIEW                 (1u << 3)
#define PIPE_BIND_VERTEX_BUFFER                (1u << 4)
#define PIPE_BIND_INDEX_BUFFER                 (1u << 5)
#define PIPE_BIND_CONSTANT_BUFFER              (1u << 6)
#define PIPE_BIND_DISPLAY_TARGET               (1u << 7)
#define PIPE_BIND_SHADER_BUFFER                (1u << 19)
#define VIRTIO_GPU_CONTEXT_NAME_LENGTH         64u

#define VIRTIO_GPU_CFG_NUM_SCANOUTS            (VIRTIO_MMIO_CONFIG + 0x08)
#define VIRTIO_GPU_CFG_NUM_CAPSETS             (VIRTIO_MMIO_CONFIG + 0x0C)

#define VIRTIO_GPU_RESOURCE_ID                 1
#define VIRTIO_GPU_DRM_RESOURCE_BASE           0x1000u
#define VIRTIO_GPU_SCANOUT_ID                  0
#define VIRTIO_GPU_VQ_SIZE                     8
/*
 * GPU flushes are not on a correctness-critical path like block I/O.  Give
 * QEMU enough room under SMP/host load, then fail the graphics backend cleanly
 * instead of reusing descriptors from an uncertain control queue.
 */
#define VIRTIO_GPU_TIMEOUT_MS                  10000
#define VIRTIO_GPU_DMA_BUF_SIZE                512
#define VIRTIO_GPU_WAIT_SPIN_BUDGET            1024
#define VIRTIO_GPU_DRIFT_WARN_LIMIT            4

/* VirtIO MMIO interrupt status bits. */
#define VIRTIO_GPU_INT_USED_RING               0x1u
#define VIRTIO_GPU_INT_CONFIG                  0x2u

typedef struct {
    uint32_t x;
    uint32_t y;
    uint32_t width;
    uint32_t height;
} __attribute__((packed)) virtio_gpu_rect_t;

typedef struct {
    uint32_t type;
    uint32_t flags;
    uint64_t fence_id;
    uint32_t ctx_id;
    uint32_t padding;
} __attribute__((packed)) virtio_gpu_ctrl_hdr_t;

typedef struct {
    virtio_gpu_rect_t r;
    uint32_t enabled;
    uint32_t flags;
} __attribute__((packed)) virtio_gpu_display_one_t;

typedef struct {
    virtio_gpu_ctrl_hdr_t hdr;
    virtio_gpu_display_one_t pmodes[16];
} __attribute__((packed)) virtio_gpu_resp_display_info_t;

typedef struct {
    virtio_gpu_ctrl_hdr_t hdr;
    uint32_t resource_id;
    uint32_t format;
    uint32_t width;
    uint32_t height;
} __attribute__((packed)) virtio_gpu_resource_create_2d_t;

typedef struct {
    virtio_gpu_ctrl_hdr_t hdr;
    virtio_gpu_rect_t r;
    uint32_t scanout_id;
    uint32_t resource_id;
} __attribute__((packed)) virtio_gpu_set_scanout_t;

typedef struct {
    uint64_t addr;
    uint32_t length;
    uint32_t padding;
} __attribute__((packed)) virtio_gpu_mem_entry_t;

typedef struct {
    virtio_gpu_ctrl_hdr_t hdr;
    uint32_t resource_id;
    uint32_t nr_entries;
} __attribute__((packed)) virtio_gpu_resource_attach_backing_t;

typedef struct {
    virtio_gpu_ctrl_hdr_t hdr;
    virtio_gpu_rect_t r;
    uint64_t offset;
    uint32_t resource_id;
    uint32_t padding;
} __attribute__((packed)) virtio_gpu_transfer_to_host_2d_t;

typedef struct {
    virtio_gpu_ctrl_hdr_t hdr;
    virtio_gpu_rect_t r;
    uint32_t resource_id;
    uint32_t padding;
} __attribute__((packed)) virtio_gpu_resource_flush_t;

typedef struct {
    virtio_gpu_ctrl_hdr_t hdr;
    uint32_t capset_index;
    uint32_t padding;
} __attribute__((packed)) virtio_gpu_get_capset_info_t;

typedef struct {
    virtio_gpu_ctrl_hdr_t hdr;
    uint32_t capset_id;
    uint32_t capset_max_version;
    uint32_t capset_max_size;
    uint32_t padding;
} __attribute__((packed)) virtio_gpu_resp_capset_info_t;

typedef struct {
    virtio_gpu_ctrl_hdr_t hdr;
    uint32_t capset_id;
    uint32_t capset_version;
} __attribute__((packed)) virtio_gpu_get_capset_t;

typedef struct {
    virtio_gpu_ctrl_hdr_t hdr;
    uint32_t name_length;
    uint32_t context_init;
    char debug_name[VIRTIO_GPU_CONTEXT_NAME_LENGTH];
} __attribute__((packed)) virtio_gpu_ctx_create_t;

typedef struct {
    virtio_gpu_ctrl_hdr_t hdr;
    uint32_t size;
    uint32_t padding;
} __attribute__((packed)) virtio_gpu_submit_3d_t;

typedef struct {
    virtio_gpu_ctrl_hdr_t hdr;
    uint32_t resource_id;
    uint32_t target;
    uint32_t format;
    uint32_t bind;
    uint32_t width;
    uint32_t height;
    uint32_t depth;
    uint32_t array_size;
    uint32_t last_level;
    uint32_t nr_samples;
    uint32_t flags;
    uint32_t padding;
} __attribute__((packed)) virtio_gpu_resource_create_3d_t;

typedef struct {
    virtio_gpu_ctrl_hdr_t hdr;
    uint32_t resource_id;
    uint32_t padding;
} __attribute__((packed)) virtio_gpu_resource_ref_t;

typedef struct {
    uint32_t x;
    uint32_t y;
    uint32_t z;
    uint32_t width;
    uint32_t height;
    uint32_t depth;
} __attribute__((packed)) virtio_gpu_box_t;

typedef struct {
    virtio_gpu_ctrl_hdr_t hdr;
    virtio_gpu_box_t box;
    uint64_t offset;
    uint32_t resource_id;
    uint32_t level;
    uint32_t stride;
    uint32_t layer_stride;
} __attribute__((packed)) virtio_gpu_transfer_host_3d_t;

typedef struct {
    paddr_t phys;
    uint32_t irq;
    volatile uint32_t *mmio;
    vq_legacy_t vq;
    uint16_t next_desc;
    uint32_t framebuffer_size;
    uint32_t device_features;
    uint32_t negotiated_features;
    uint32_t num_scanouts;
    uint32_t num_capsets;
    uint32_t width;
    uint32_t height;
    uint32_t pitch;
    uint32_t scanout_resource_id;
    uint32_t command_capset_id;
    uint32_t command_capset_max_version;
    uint32_t command_capset_size;
    bool has_virgl_capset;
    bool has_virgl2_capset;
    bool initialized;
    bool failed;
    bool config_pending;
} virtio_gpu_state_t;

static virtio_gpu_state_t gpu = {0};
static spinlock_t gpu_lock = SPINLOCK_INIT("virtio_gpu");
static volatile bool gpu_busy = false;
static task_t *gpu_owner = NULL;
static uint32_t gpu_used_drift_warnings;

typedef struct {
    bool active;
    uint16_t head;
    uint16_t prev_used;
    uint32_t response_size;
    uint64_t fence_id;
} virtio_gpu_async_request_t;

static virtio_gpu_async_request_t gpu_async;
static uint8_t gpu_async_cmd_dma[
    ARMOS_DRM_MAX_COMMAND_SIZE + sizeof(virtio_gpu_submit_3d_t)]
    __attribute__((aligned(PAGE_SIZE)));
static uint8_t gpu_async_resp_dma[PAGE_SIZE]
    __attribute__((aligned(PAGE_SIZE)));

static bool gpu_has_virgl(void);
static void gpu_fill_hdr(virtio_gpu_ctrl_hdr_t *hdr, uint32_t type);
static int gpu_submit_simple(void *cmd, uint32_t cmd_len, const char *name);
static int gpu_submit_async(void *cmd, uint32_t cmd_len, uint64_t fence_id);
static int gpu_set_scanout_resource(uint32_t scanout_id,
                                    uint32_t resource_id,
                                    uint32_t width, uint32_t height);

static int virtio_gpu_backend_get_info(void *context,
                                       armos_drm_backend_info_t *info)
{
    (void)context;

    if (!info || !gpu.initialized)
        return -ENODEV;
    memset(info, 0, sizeof(*info));
    info->backend_class = ARMOS_DRM_BACKEND_PARAVIRTUAL;
    info->capabilities = ARMOS_DRM_CAP_SCANOUT |
                         ARMOS_DRM_CAP_TRANSFER_2D;
    if (gpu_has_virgl()) {
        info->capabilities |= ARMOS_DRM_CAP_CONTEXTS |
                              ARMOS_DRM_CAP_BUFFER_OBJECTS |
                              ARMOS_DRM_CAP_CPU_MAPPABLE |
                              ARMOS_DRM_CAP_COMMAND_SUBMIT |
                              ARMOS_DRM_CAP_FENCES |
                              ARMOS_DRM_CAP_RENDER_3D |
                              ARMOS_DRM_CAP_RESOURCE_TRANSFER;
        if (gpu.has_virgl2_capset)
            memcpy(info->command_set, "virgl2", sizeof("virgl2"));
        else
            memcpy(info->command_set, "virgl", sizeof("virgl"));
    }
    info->scanout_count = gpu.num_scanouts ? gpu.num_scanouts : 1u;
    info->scanout_width = gpu.width;
    info->scanout_height = gpu.height;
    info->max_resource_width = gpu.width;
    info->max_resource_height = gpu.height;
    info->command_caps_max_version = gpu.command_capset_max_version;
    info->command_caps_size = gpu.command_capset_size;
    info->driver_name = "virtio-gpu";
    return 0;
}

static uint32_t gpu_drm_resource_id(uint32_t handle)
{
    if (handle == 0 || handle > 0xffffffffu - VIRTIO_GPU_DRM_RESOURCE_BASE)
        return 0;
    return VIRTIO_GPU_DRM_RESOURCE_BASE + handle;
}

static int virtio_gpu_backend_buffer_create(
    void *context, uint32_t handle, const armos_drm_buffer_desc_t *desc,
    const armos_drm_memory_segment_t *segments, uint32_t segment_count,
    uint32_t *command_handle)
{
    virtio_gpu_resource_create_2d_t create_2d;
    virtio_gpu_resource_create_3d_t create;
    virtio_gpu_resource_attach_backing_t *attach;
    virtio_gpu_mem_entry_t *entries;
    virtio_gpu_resource_ref_t unref;
    uint32_t resource_id = gpu_drm_resource_id(handle);
    uint32_t request_size;
    int result;

    (void)context;
    if (!gpu_has_virgl() || !resource_id || !desc || !segments ||
        !command_handle ||
        segment_count == 0 || desc->size == 0 ||
        desc->size > 0xffffffffu)
        return -EINVAL;
    if (desc->width != 0 &&
        desc->format != ARMOS_DRM_FORMAT_BGRA8888)
        return -ENOTSUP;
    if ((desc->flags & ARMOS_DRM_BO_SCANOUT) != 0) {
        if (desc->width == 0 || desc->height == 0 ||
            desc->stride != desc->width * sizeof(uint32_t))
            return -EINVAL;
        memset(&create_2d, 0, sizeof(create_2d));
        gpu_fill_hdr(&create_2d.hdr, VIRTIO_GPU_CMD_RESOURCE_CREATE_2D);
        create_2d.resource_id = resource_id;
        create_2d.format = VIRTIO_GPU_FORMAT_B8G8R8A8_UNORM;
        create_2d.width = desc->width;
        create_2d.height = desc->height;
        if (gpu_submit_simple(&create_2d, sizeof(create_2d),
                              "drm_resource_create_2d") < 0)
            return -EIO;
    } else {
        memset(&create, 0, sizeof(create));
        gpu_fill_hdr(&create.hdr, VIRTIO_GPU_CMD_RESOURCE_CREATE_3D);
        create.resource_id = resource_id;
        if (desc->width && desc->height) {
            create.target = PIPE_TEXTURE_2D;
            create.format = PIPE_FORMAT_B8G8R8A8_UNORM;
            create.width = desc->width;
            create.height = desc->height;
            create.depth = 1u;
            create.array_size = 1u;
            if (desc->flags & ARMOS_DRM_BO_RENDER_TARGET)
                create.bind |= PIPE_BIND_RENDER_TARGET;
            if (desc->flags & ARMOS_DRM_BO_TEXTURE)
                create.bind |= PIPE_BIND_SAMPLER_VIEW;
        } else {
            create.target = PIPE_BUFFER;
            create.format = PIPE_FORMAT_NONE;
            create.width = (uint32_t)desc->size;
            create.height = 1u;
            create.depth = 1u;
            create.array_size = 1u;
            if (desc->flags & ARMOS_DRM_BO_VERTEX)
                create.bind |= PIPE_BIND_VERTEX_BUFFER;
            if (desc->flags & ARMOS_DRM_BO_INDEX)
                create.bind |= PIPE_BIND_INDEX_BUFFER;
            if (desc->flags & ARMOS_DRM_BO_CONSTANT)
                create.bind |= PIPE_BIND_CONSTANT_BUFFER;
            if (desc->flags & ARMOS_DRM_BO_SHADER_STORAGE)
                create.bind |= PIPE_BIND_SHADER_BUFFER;
            /*
             * A command/staging BO is not consumed as a Gallium resource,
             * but VirtIO-GPU still requires a legal bind when the common BO
             * lifecycle creates its backing resource.
             */
            if (create.bind == 0 &&
                (desc->flags & ARMOS_DRM_BO_COMMAND))
                create.bind = PIPE_BIND_VERTEX_BUFFER;
            if (create.bind == 0)
                return -EINVAL;
        }
        if (gpu_submit_simple(&create, sizeof(create),
                              "resource_create_3d") < 0)
            return -EIO;
    }

    if (segment_count >
        (0xffffffffu - sizeof(*attach)) / sizeof(*entries)) {
        result = -EOVERFLOW;
        goto failed;
    }
    request_size = sizeof(*attach) + segment_count * sizeof(*entries);
    attach = kzalloc(request_size);
    if (!attach) {
        result = -ENOMEM;
        goto failed;
    }
    gpu_fill_hdr(&attach->hdr, VIRTIO_GPU_CMD_RESOURCE_ATTACH_BACKING);
    attach->resource_id = resource_id;
    attach->nr_entries = segment_count;
    entries = (virtio_gpu_mem_entry_t *)(attach + 1);
    for (uint32_t index = 0; index < segment_count; index++) {
        entries[index].addr = segments[index].address;
        entries[index].length = segments[index].length;
    }
    result = gpu_submit_simple(attach, request_size,
                               "resource_attach_backing");
    kfree(attach);
    if (result < 0) {
        result = -EIO;
        goto failed;
    }
    *command_handle = resource_id;
    return 0;

failed:
    memset(&unref, 0, sizeof(unref));
    gpu_fill_hdr(&unref.hdr, VIRTIO_GPU_CMD_RESOURCE_UNREF);
    unref.resource_id = resource_id;
    (void)gpu_submit_simple(&unref, sizeof(unref), "resource_unref");
    return result;
}

static int virtio_gpu_backend_buffer_destroy(void *context, uint32_t handle)
{
    virtio_gpu_resource_ref_t cmd;
    uint32_t resource_id = gpu_drm_resource_id(handle);
    bool restore_boot_scanout;

    (void)context;
    if (!resource_id)
        return -EINVAL;
    restore_boot_scanout = gpu.scanout_resource_id == resource_id;
    if (restore_boot_scanout) {
        if (gpu_set_scanout_resource(VIRTIO_GPU_SCANOUT_ID,
                                     VIRTIO_GPU_RESOURCE_ID,
                                     gpu.width, gpu.height) < 0)
            return -EIO;
        /*
         * The boot framebuffer may not have been presented since userland
         * acquired the scanout. Publish it completely before destroying the
         * former owner so console recovery cannot expose stale host contents.
         */
        if (virtio_gpu_flush() < 0)
            return -EIO;
    }
    memset(&cmd, 0, sizeof(cmd));
    gpu_fill_hdr(&cmd.hdr, VIRTIO_GPU_CMD_RESOURCE_UNREF);
    cmd.resource_id = resource_id;
    return gpu_submit_simple(&cmd, sizeof(cmd), "resource_unref") < 0 ?
        -EIO : 0;
}

static int virtio_gpu_backend_resource_change(void *context,
                                               uint32_t context_id,
                                               uint32_t handle,
                                               bool attach)
{
    virtio_gpu_resource_ref_t cmd;
    uint32_t resource_id = gpu_drm_resource_id(handle);

    (void)context;
    if (!gpu_has_virgl() || context_id == 0 || !resource_id)
        return -EINVAL;
    memset(&cmd, 0, sizeof(cmd));
    gpu_fill_hdr(&cmd.hdr, attach ?
                 VIRTIO_GPU_CMD_CTX_ATTACH_RESOURCE :
                 VIRTIO_GPU_CMD_CTX_DETACH_RESOURCE);
    cmd.hdr.ctx_id = context_id;
    cmd.resource_id = resource_id;
    return gpu_submit_simple(&cmd, sizeof(cmd),
                             attach ? "ctx_attach_resource" :
                                      "ctx_detach_resource") < 0 ?
        -EIO : 0;
}

static int virtio_gpu_backend_resource_attach(void *context,
                                               uint32_t context_id,
                                               uint32_t handle)
{
    return virtio_gpu_backend_resource_change(context, context_id, handle,
                                               true);
}

static int virtio_gpu_backend_resource_detach(void *context,
                                               uint32_t context_id,
                                               uint32_t handle)
{
    return virtio_gpu_backend_resource_change(context, context_id, handle,
                                               false);
}

static int virtio_gpu_backend_context_create(void *context,
                                             uint32_t context_id);
static int virtio_gpu_backend_context_destroy(void *context,
                                              uint32_t context_id);
static int virtio_gpu_backend_submit(void *context, uint32_t context_id,
                                     const void *commands,
                                     uint32_t command_size,
                                     uint64_t fence_id);
static int virtio_gpu_backend_buffer_present(
    void *context, uint32_t handle, const armos_drm_buffer_desc_t *desc,
    uint32_t scanout_id, uint32_t x, uint32_t y,
    uint32_t width, uint32_t height);
static int virtio_gpu_backend_get_command_caps(
    void *context, uint32_t version, void *data, uint32_t size);
static int virtio_gpu_backend_buffer_transfer(
    void *context, uint32_t context_id, uint32_t handle,
    uint32_t direction, uint32_t level, uint32_t x, uint32_t y,
    uint32_t z, uint32_t width, uint32_t height, uint32_t depth,
    uint64_t offset, uint32_t stride, uint32_t layer_stride);

static const armos_drm_backend_ops_t virtio_gpu_backend_ops = {
    .get_info = virtio_gpu_backend_get_info,
    .context_create = virtio_gpu_backend_context_create,
    .context_destroy = virtio_gpu_backend_context_destroy,
    .buffer_create = virtio_gpu_backend_buffer_create,
    .buffer_destroy = virtio_gpu_backend_buffer_destroy,
    .resource_attach = virtio_gpu_backend_resource_attach,
    .resource_detach = virtio_gpu_backend_resource_detach,
    .buffer_present = virtio_gpu_backend_buffer_present,
    .get_command_caps = virtio_gpu_backend_get_command_caps,
    .buffer_transfer = virtio_gpu_backend_buffer_transfer,
    .submit = virtio_gpu_backend_submit,
};

/*
 * The control queue is single-flight, so one dedicated request/response pair is
 * enough. Keep DMA targets away from kernel stacks: invalidating a cache line
 * around a stack-local response can discard unrelated live stack data.
 */
static uint8_t gpu_cmd_dma[VIRTIO_GPU_DMA_BUF_SIZE] __attribute__((aligned(PAGE_SIZE)));
static uint8_t gpu_resp_dma[VIRTIO_GPU_DMA_BUF_SIZE] __attribute__((aligned(PAGE_SIZE)));

static struct vring_desc *gpu_desc_ptr(vq_legacy_t *vq, unsigned i)
{
    return (struct vring_desc *)((uint8_t *)(uintptr_t)vq->va_desc +
                                 i * sizeof(struct vring_desc));
}

static struct vring_avail *gpu_avail_ptr(vq_legacy_t *vq)
{
    return (struct vring_avail *)((uint8_t *)(uintptr_t)vq->va_avail);
}

static struct vring_used *gpu_used_ptr(vq_legacy_t *vq)
{
    return (struct vring_used *)((uint8_t *)(uintptr_t)vq->va_used);
}

static void *gpu_alloc_dma_pages(size_t npages, paddr_t *out_pa)
{
    paddr_t pa = (paddr_t)allocate_pages(npages);
    if (!pa)
        return NULL;
    if (out_pa)
        *out_pa = pa;
    return (void *)phys_to_virt(pa);
}

static bool gpu_vq_alloc(vq_legacy_t *vq, uint16_t qsize)
{
    uint32_t desc_sz = 16u * qsize;
    uint32_t avail_sz = ALIGN_UP(6u + 2u * qsize, 2u);
    uint32_t used_sz = ALIGN_UP(6u + 8u * qsize, VQ_ALIGN);
    uint32_t total = ALIGN_UP(desc_sz, 16) +
                     ALIGN_UP(avail_sz, 2) +
                     ALIGN_UP(used_sz, VQ_ALIGN);
    size_t npages = (total + PAGE_SIZE - 1) / PAGE_SIZE;
    paddr_t pa_base = 0;
    uint8_t *va_base = gpu_alloc_dma_pages(npages, &pa_base);
    uint32_t off = 0;

    if (!va_base)
        return false;

    memset(va_base, 0, npages * PAGE_SIZE);

    vq->pa_base = pa_base;
    vq->va_base = (uintptr_t)va_base;

    vq->pa_desc = pa_base + off;
    vq->va_desc = (uintptr_t)(va_base + off);
    vq->desc_size = desc_sz;
    off = ALIGN_UP(off + desc_sz, 16);

    vq->pa_avail = pa_base + off;
    vq->va_avail = (uintptr_t)(va_base + off);
    vq->avail_size = avail_sz;
    off = ALIGN_UP(off + avail_sz, 2);

    off = ALIGN_UP(off, VQ_ALIGN);
    vq->pa_used = pa_base + off;
    vq->va_used = (uintptr_t)(va_base + off);
    vq->used_size = used_sz;
    vq->qsize = qsize;
    vq->last_used_idx = 0;

    arch_clean_dcache_by_mva(va_base, npages * PAGE_SIZE);
    return true;
}

static bool gpu_probe_from_dtb(paddr_t *out_phys, uint32_t *out_irq)
{
    paddr_t phys = 0;
    bool edge = true;

    if (!fdt_find_virtio_mmio_device(VIRTIO_ID_GPU, &phys, out_irq, &edge))
        return false;

    *out_phys = phys;
    return true;
}

static int gpu_wait_used(uint16_t prev_used)
{
    uint32_t freq = arch_timer_frequency();
    uint32_t spins = 0;
    if (freq == 0)
        freq = TIMER_FALLBACK_FREQ;

    uint64_t timeout_ticks = (uint64_t)VIRTIO_GPU_TIMEOUT_MS * (uint64_t)(freq / 1000);
    uint64_t start = arch_timer_counter();

    while ((arch_timer_counter() - start) < timeout_ticks) {
        arch_invalidate_dcache_by_mva((void *)(uintptr_t)gpu.vq.va_used,
            sizeof(struct vring_used) + gpu.vq.qsize * sizeof(struct vring_used_elem));
        arch_data_memory_barrier_inner_shareable();
        if (gpu_used_ptr(&gpu.vq)->idx != prev_used)
            return 0;

        if (spins++ < VIRTIO_GPU_WAIT_SPIN_BUDGET) {
            arch_cpu_relax();
            continue;
        }

        spins = 0;
        if (task_current_local())
            task_sleep_ms(1);
        else
            arch_cpu_relax();
    }

    return -1;
}

static int gpu_drain_used_ring(uint16_t prev_used, uint16_t expected_id,
                               uint16_t *out_used_idx)
{
    struct vring_used *used;
    uint16_t used_idx;
    uint16_t delta;
    bool found = false;

    arch_invalidate_dcache_by_mva((void *)gpu.vq.va_used,
        sizeof(struct vring_used) + gpu.vq.qsize * sizeof(struct vring_used_elem));
    arch_data_memory_barrier_inner_shareable();

    used = gpu_used_ptr(&gpu.vq);
    used_idx = used->idx;
    delta = (uint16_t)(used_idx - prev_used);
    if (delta == 0)
        return -1;
    if (delta > gpu.vq.qsize)
        return -1;

    for (uint16_t i = 0; i < delta; i++) {
        struct vring_used_elem *elem =
            &used->ring[(uint16_t)(prev_used + i) % gpu.vq.qsize];
        if (elem->id == expected_id)
            found = true;
    }

    if (!found)
        return -1;

    if (delta != 1 && gpu_used_drift_warnings < VIRTIO_GPU_DRIFT_WARN_LIMIT) {
        gpu_used_drift_warnings++;
        KWARN("virtio_gpu: drained %u used-ring entries while waiting id=%u\n",
              delta, expected_id);
    }

    if (out_used_idx)
        *out_used_idx = used_idx;
    return 0;
}

static int gpu_acquire(void)
{
    while (1) {
        task_t *task = task_current_local();
        unsigned long flags;

        spin_lock_irqsave(&gpu_lock, &flags);
        if (!gpu_busy) {
            gpu_busy = true;
            gpu_owner = task;
            spin_unlock_irqrestore(&gpu_lock, flags);
            return 0;
        }
        if (gpu_owner && task && gpu_owner == task) {
            KERROR("virtio_gpu: recursive command submission by %s\n",
                   task->name);
            spin_unlock_irqrestore(&gpu_lock, flags);
            return -1;
        }
        spin_unlock_irqrestore(&gpu_lock, flags);

        /*
         * The legacy control queue is shared by all console paths.  Under SMP,
         * displayd, tty writes, and boot-time drawing can all request flushes;
         * only one command chain may own next_desc/avail/last_used at a time.
         */
        if (task)
            yield();
        else
            arch_cpu_relax();
    }
}

static void gpu_mark_failed(const char *reason, uint32_t cmd_type)
{
    unsigned long flags;
    bool first_failure = false;

    spin_lock_irqsave(&gpu_lock, &flags);
    if (!gpu.failed) {
        gpu.failed = true;
        first_failure = true;
    }
    gpu.initialized = false;
    spin_unlock_irqrestore(&gpu_lock, flags);

    if (!first_failure)
        return;

    KERROR("virtio_gpu: disabling device after %s (cmd=0x%08X)\n",
           reason ? reason : "queue failure", cmd_type);
    if (gpu.mmio)
        mmio_write32(gpu.mmio, VIRTIO_MMIO_STATUS,
                     mmio_read32(gpu.mmio, VIRTIO_MMIO_STATUS) |
                     VIRTIO_STATUS_FAILED);
}

static void gpu_release(void)
{
    task_t *task = task_current_local();
    unsigned long flags;

    spin_lock_irqsave(&gpu_lock, &flags);
    if (!gpu_busy) {
        KERROR("virtio_gpu: release without owner\n");
    } else if (gpu_owner && task && gpu_owner != task) {
        KERROR("virtio_gpu: release by non-owner\n");
    }
    gpu_busy = false;
    gpu_owner = NULL;
    spin_unlock_irqrestore(&gpu_lock, flags);
}

static int gpu_submit(void *cmd, uint32_t cmd_len, void *resp, uint32_t resp_len)
{
    uint8_t *cmd_dma_ptr = gpu_cmd_dma;
    uint8_t *resp_dma_ptr = gpu_resp_dma;
    paddr_t cmd_dma_pa;
    paddr_t resp_dma_pa;
    void *dynamic_cmd = NULL;
    void *dynamic_resp = NULL;
    size_t dynamic_cmd_pages = 0;
    size_t dynamic_resp_pages = 0;
    int ret = -1;
    uint32_t cmd_type = cmd ? ((virtio_gpu_ctrl_hdr_t *)cmd)->type : 0;

    if (!gpu.initialized || gpu.failed || !cmd || cmd_len == 0 ||
        !resp || resp_len == 0)
        return -1;
    if (gpu.vq.qsize < 2)
        return -1;
    if (gpu_acquire() < 0)
        return -1;
    if (!gpu.initialized || gpu.failed) {
        gpu_release();
        return -1;
    }
    if (cmd_len > VIRTIO_GPU_DMA_BUF_SIZE) {
        dynamic_cmd_pages =
            (cmd_len + PAGE_SIZE - 1u) / PAGE_SIZE;
        dynamic_cmd = gpu_alloc_dma_pages(dynamic_cmd_pages, &cmd_dma_pa);
        if (!dynamic_cmd) {
            gpu_release();
            return -ENOMEM;
        }
        cmd_dma_ptr = dynamic_cmd;
    } else {
        cmd_dma_pa = virt_to_phys((vaddr_t)gpu_cmd_dma);
    }
    if (resp_len > VIRTIO_GPU_DMA_BUF_SIZE) {
        dynamic_resp_pages =
            (resp_len + PAGE_SIZE - 1u) / PAGE_SIZE;
        dynamic_resp = gpu_alloc_dma_pages(dynamic_resp_pages, &resp_dma_pa);
        if (!dynamic_resp) {
            if (dynamic_cmd)
                free_pages(dynamic_cmd, dynamic_cmd_pages);
            gpu_release();
            return -ENOMEM;
        }
        resp_dma_ptr = dynamic_resp;
    } else {
        resp_dma_pa = virt_to_phys((vaddr_t)gpu_resp_dma);
    }

    unsigned d0 = gpu.next_desc;
    unsigned d1 = (gpu.next_desc + 1) % gpu.vq.qsize;
    gpu.next_desc = (gpu.next_desc + 2) % gpu.vq.qsize;

    struct vring_desc *desc0 = gpu_desc_ptr(&gpu.vq, d0);
    struct vring_desc *desc1 = gpu_desc_ptr(&gpu.vq, d1);

    memset(resp, 0, resp_len);
    memcpy(cmd_dma_ptr, cmd, cmd_len);
    memset(resp_dma_ptr, 0, resp_len);

    desc0->addr = (uint64_t)cmd_dma_pa;
    desc0->len = cmd_len;
    desc0->flags = VRING_DESC_F_NEXT;
    desc0->next = d1;

    desc1->addr = (uint64_t)resp_dma_pa;
    desc1->len = resp_len;
    desc1->flags = VRING_DESC_F_WRITE;
    desc1->next = 0;

    arch_clean_dcache_by_mva((void *)gpu.vq.va_desc, sizeof(struct vring_desc) * gpu.vq.qsize);
    arch_clean_dcache_by_mva(cmd_dma_ptr, cmd_len);
    arch_clean_invalidate_dcache_by_mva(resp_dma_ptr, resp_len);
    arch_data_memory_barrier_inner_shareable();

    uint16_t prev_used = gpu.vq.last_used_idx;
    struct vring_avail *avail = gpu_avail_ptr(&gpu.vq);
    uint16_t old_idx = avail->idx;
    avail->ring[old_idx % gpu.vq.qsize] = d0;
    arch_clean_dcache_by_mva((void *)gpu.vq.va_avail, gpu.vq.avail_size);
    arch_data_memory_barrier_inner_shareable();
    avail->idx = old_idx + 1;
    arch_clean_dcache_by_mva(&avail->idx, sizeof(avail->idx));
    arch_data_sync_barrier_inner_shareable_write();

    mmio_write32(gpu.mmio, VIRTIO_MMIO_QUEUE_NOTIFY, 0);

    uint16_t new_used_idx = 0;
    if (gpu_wait_used(prev_used) < 0) {
        /*
         * Avoid failing the graphics backend on a boundary race where QEMU
         * posts the used element just as the timeout expires.
         */
        if (gpu_drain_used_ring(prev_used, (uint16_t)d0, &new_used_idx) < 0) {
            gpu_mark_failed("control queue timeout", cmd_type);
            goto out;
        }
        goto complete;
    }

    if (gpu_drain_used_ring(prev_used, (uint16_t)d0, &new_used_idx) < 0) {
        gpu_mark_failed("used ring did not contain expected descriptor", cmd_type);
        goto out;
    }

complete:
    gpu.vq.last_used_idx = new_used_idx;
    arch_invalidate_dcache_by_mva(resp_dma_ptr, resp_len);
    memcpy(resp, resp_dma_ptr, resp_len);

    /*
     * Only acknowledge the used-ring bit here. Acking the whole status word
     * would silently consume config-change events (QEMU window resize),
     * which virtio_gpu_check_resize() polls for separately.
     */
    uint32_t irq_status = mmio_read32(gpu.mmio, VIRTIO_MMIO_INTERRUPT_STATUS);
    if (irq_status & VIRTIO_GPU_INT_USED_RING)
        mmio_write32(gpu.mmio, VIRTIO_MMIO_INTERRUPT_ACK, VIRTIO_GPU_INT_USED_RING);
    ret = 0;

out:
    /*
     * After a queue failure the device may still own the request. Keep a
     * dynamic DMA allocation alive rather than creating a use-after-free.
     */
    if (dynamic_cmd && !gpu.failed)
        free_pages(dynamic_cmd, dynamic_cmd_pages);
    if (dynamic_resp && !gpu.failed)
        free_pages(dynamic_resp, dynamic_resp_pages);
    gpu_release();
    return ret;
}

/*
 * Queue one fenced command without waiting for the used ring.  The legacy
 * control queue remains deliberately single-flight; ownership moves from the
 * submitting task to the IRQ completion record until virtio_gpu_irq_handler()
 * publishes the common DRM fence.
 */
static int gpu_submit_async(void *cmd, uint32_t cmd_len, uint64_t fence_id)
{
    struct vring_desc *desc0;
    struct vring_desc *desc1;
    struct vring_avail *avail;
    uint16_t old_idx;
    unsigned d0;
    unsigned d1;
    unsigned long flags;

    if (!gpu.initialized || gpu.failed || !cmd || cmd_len == 0 ||
        cmd_len > sizeof(gpu_async_cmd_dma) || fence_id == 0 ||
        gpu.vq.qsize < 2)
        return -EINVAL;
    if (gpu_acquire() < 0)
        return -EBUSY;
    if (!gpu.initialized || gpu.failed) {
        gpu_release();
        return -ENODEV;
    }

    d0 = gpu.next_desc;
    d1 = (gpu.next_desc + 1u) % gpu.vq.qsize;
    gpu.next_desc = (gpu.next_desc + 2u) % gpu.vq.qsize;
    desc0 = gpu_desc_ptr(&gpu.vq, d0);
    desc1 = gpu_desc_ptr(&gpu.vq, d1);
    memcpy(gpu_async_cmd_dma, cmd, cmd_len);
    memset(gpu_async_resp_dma, 0, sizeof(virtio_gpu_ctrl_hdr_t));

    desc0->addr = virt_to_phys((vaddr_t)gpu_async_cmd_dma);
    desc0->len = cmd_len;
    desc0->flags = VRING_DESC_F_NEXT;
    desc0->next = d1;
    desc1->addr = virt_to_phys((vaddr_t)gpu_async_resp_dma);
    desc1->len = sizeof(virtio_gpu_ctrl_hdr_t);
    desc1->flags = VRING_DESC_F_WRITE;
    desc1->next = 0;

    arch_clean_dcache_by_mva((void *)gpu.vq.va_desc,
                             sizeof(struct vring_desc) * gpu.vq.qsize);
    arch_clean_dcache_by_mva(gpu_async_cmd_dma, cmd_len);
    arch_clean_invalidate_dcache_by_mva(
        gpu_async_resp_dma, sizeof(virtio_gpu_ctrl_hdr_t));
    arch_data_memory_barrier_inner_shareable();

    spin_lock_irqsave(&gpu_lock, &flags);
    gpu_async.active = true;
    gpu_async.head = (uint16_t)d0;
    gpu_async.prev_used = gpu.vq.last_used_idx;
    gpu_async.response_size = sizeof(virtio_gpu_ctrl_hdr_t);
    gpu_async.fence_id = fence_id;
    gpu_owner = NULL;
    spin_unlock_irqrestore(&gpu_lock, flags);

    avail = gpu_avail_ptr(&gpu.vq);
    old_idx = avail->idx;
    avail->ring[old_idx % gpu.vq.qsize] = d0;
    arch_clean_dcache_by_mva((void *)gpu.vq.va_avail, gpu.vq.avail_size);
    arch_data_memory_barrier_inner_shareable();
    avail->idx = old_idx + 1u;
    arch_clean_dcache_by_mva(&avail->idx, sizeof(avail->idx));
    arch_data_sync_barrier_inner_shareable_write();
    mmio_write32(gpu.mmio, VIRTIO_MMIO_QUEUE_NOTIFY, 0);
    return 0;
}

uint32_t virtio_gpu_get_irq(void)
{
    return gpu.irq;
}

void virtio_gpu_irq_handler(void)
{
    virtio_gpu_ctrl_hdr_t *response;
    uint64_t fence_id = 0;
    uint16_t new_used_idx = 0;
    uint32_t irq_status;
    int status = -EIO;
    unsigned long flags;

    if (!gpu.mmio)
        return;
    irq_status = mmio_read32(gpu.mmio, VIRTIO_MMIO_INTERRUPT_STATUS);
    if (irq_status & VIRTIO_GPU_INT_CONFIG) {
        mmio_write32(gpu.mmio, VIRTIO_MMIO_INTERRUPT_ACK,
                     VIRTIO_GPU_INT_CONFIG);
        spin_lock_irqsave(&gpu_lock, &flags);
        gpu.config_pending = true;
        spin_unlock_irqrestore(&gpu_lock, flags);
    }
    if (irq_status & VIRTIO_GPU_INT_USED_RING)
        mmio_write32(gpu.mmio, VIRTIO_MMIO_INTERRUPT_ACK,
                     VIRTIO_GPU_INT_USED_RING);

    spin_lock_irqsave(&gpu_lock, &flags);
    if (!gpu_async.active) {
        spin_unlock_irqrestore(&gpu_lock, flags);
        return;
    }
    if (gpu_drain_used_ring(gpu_async.prev_used, gpu_async.head,
                            &new_used_idx) < 0) {
        spin_unlock_irqrestore(&gpu_lock, flags);
        return;
    }
    gpu.vq.last_used_idx = new_used_idx;
    arch_invalidate_dcache_by_mva(gpu_async_resp_dma,
                                  gpu_async.response_size);
    response = (virtio_gpu_ctrl_hdr_t *)gpu_async_resp_dma;
    if (response->type == VIRTIO_GPU_RESP_OK_NODATA)
        status = 0;
    fence_id = gpu_async.fence_id;
    memset(&gpu_async, 0, sizeof(gpu_async));
    gpu_busy = false;
    gpu_owner = NULL;
    spin_unlock_irqrestore(&gpu_lock, flags);

    armos_drm_fence_complete(fence_id, status);
}

static int gpu_submit_simple(void *cmd, uint32_t cmd_len, const char *name)
{
    virtio_gpu_ctrl_hdr_t resp;

    if (gpu_submit(cmd, cmd_len, &resp, sizeof(resp)) < 0)
        return -1;

    if (resp.type != VIRTIO_GPU_RESP_OK_NODATA) {
        KERROR("virtio_gpu: %s response=0x%08X\n", name, resp.type);
        return -1;
    }

    return 0;
}

static void gpu_fill_hdr(virtio_gpu_ctrl_hdr_t *hdr, uint32_t type)
{
    memset(hdr, 0, sizeof(*hdr));
    hdr->type = type;
}

static bool gpu_has_virgl(void)
{
    return (gpu.negotiated_features & (1u << VIRTIO_GPU_F_VIRGL)) &&
           gpu.command_capset_id != 0 &&
           gpu.command_capset_size != 0 &&
           gpu.command_capset_size <= ARMOS_DRM_MAX_COMMAND_CAPS_SIZE;
}

static int virtio_gpu_backend_context_create(void *context,
                                             uint32_t context_id)
{
    virtio_gpu_ctx_create_t cmd;
    static const char debug_name[] = "armos-drm";

    (void)context;
    if (!gpu_has_virgl() || context_id == 0)
        return -ENOTSUP;
    memset(&cmd, 0, sizeof(cmd));
    gpu_fill_hdr(&cmd.hdr, VIRTIO_GPU_CMD_CTX_CREATE);
    cmd.hdr.ctx_id = context_id;
    cmd.name_length = sizeof(debug_name) - 1u;
    memcpy(cmd.debug_name, debug_name, sizeof(debug_name) - 1u);
    return gpu_submit_simple(&cmd, sizeof(cmd), "ctx_create");
}

static int virtio_gpu_backend_context_destroy(void *context,
                                              uint32_t context_id)
{
    virtio_gpu_ctrl_hdr_t cmd;

    (void)context;
    if (!gpu_has_virgl() || context_id == 0)
        return -ENOTSUP;
    gpu_fill_hdr(&cmd, VIRTIO_GPU_CMD_CTX_DESTROY);
    cmd.ctx_id = context_id;
    return gpu_submit_simple(&cmd, sizeof(cmd), "ctx_destroy");
}

static int virtio_gpu_backend_submit(void *context, uint32_t context_id,
                                     const void *commands,
                                     uint32_t command_size,
                                     uint64_t fence_id)
{
    virtio_gpu_submit_3d_t *cmd;
    uint32_t total_size;
    int result;

    (void)context;
    if (!gpu_has_virgl())
        return -ENOTSUP;
    if (!commands || context_id == 0 || command_size == 0 ||
        command_size > ARMOS_DRM_MAX_COMMAND_SIZE ||
        (command_size & (sizeof(uint32_t) - 1u)) != 0)
        return -EINVAL;
    if (command_size > 0xFFFFFFFFu - sizeof(*cmd))
        return -EOVERFLOW;

    total_size = sizeof(*cmd) + command_size;
    cmd = kmalloc(total_size);
    if (!cmd)
        return -ENOMEM;
    memset(cmd, 0, sizeof(*cmd));
    gpu_fill_hdr(&cmd->hdr, VIRTIO_GPU_CMD_SUBMIT_3D);
    cmd->hdr.flags = VIRTIO_GPU_FLAG_FENCE;
    cmd->hdr.fence_id = fence_id;
    cmd->hdr.ctx_id = context_id;
    cmd->size = command_size;
    memcpy((uint8_t *)cmd + sizeof(*cmd), commands, command_size);
    result = gpu_submit_async(cmd, total_size, fence_id);
    kfree(cmd);
    return result;
}

static int virtio_gpu_backend_buffer_transfer(
    void *context, uint32_t context_id, uint32_t handle,
    uint32_t direction, uint32_t level, uint32_t x, uint32_t y,
    uint32_t z, uint32_t width, uint32_t height, uint32_t depth,
    uint64_t offset, uint32_t stride, uint32_t layer_stride)
{
    virtio_gpu_transfer_host_3d_t cmd;
    uint32_t resource_id = gpu_drm_resource_id(handle);

    (void)context;
    if (!gpu_has_virgl() || context_id == 0 || resource_id == 0 ||
        (direction != ARMOS_DRM_TRANSFER_CPU_TO_DEVICE &&
         direction != ARMOS_DRM_TRANSFER_DEVICE_TO_CPU))
        return -EINVAL;
    memset(&cmd, 0, sizeof(cmd));
    gpu_fill_hdr(&cmd.hdr,
                 direction == ARMOS_DRM_TRANSFER_CPU_TO_DEVICE ?
                     VIRTIO_GPU_CMD_TRANSFER_TO_HOST_3D :
                     VIRTIO_GPU_CMD_TRANSFER_FROM_HOST_3D);
    cmd.hdr.ctx_id = context_id;
    cmd.box.x = x;
    cmd.box.y = y;
    cmd.box.z = z;
    cmd.box.width = width;
    cmd.box.height = height;
    cmd.box.depth = depth;
    cmd.offset = offset;
    cmd.resource_id = resource_id;
    cmd.level = level;
    cmd.stride = stride;
    cmd.layer_stride = layer_stride;
    return gpu_submit_simple(&cmd, sizeof(cmd),
                             direction == ARMOS_DRM_TRANSFER_CPU_TO_DEVICE ?
                                 "drm_transfer_to_host_3d" :
                                 "drm_transfer_from_host_3d") < 0 ?
        -EIO : 0;
}

static int gpu_get_display_info(bool adopt_mode)
{
    virtio_gpu_ctrl_hdr_t cmd;
    virtio_gpu_resp_display_info_t resp;

    gpu_fill_hdr(&cmd, VIRTIO_GPU_CMD_GET_DISPLAY_INFO);
    if (gpu_submit(&cmd, sizeof(cmd), &resp, sizeof(resp)) < 0)
        return -1;
    if (resp.hdr.type != VIRTIO_GPU_RESP_OK_DISPLAY_INFO) {
        KERROR("virtio_gpu: display info response=0x%08X\n", resp.hdr.type);
        return -1;
    }

    if (!resp.pmodes[0].enabled || !resp.pmodes[0].r.width ||
        !resp.pmodes[0].r.height)
        return -ENODEV;
    if (resp.pmodes[0].r.width > 1920u ||
        resp.pmodes[0].r.height > 1080u)
        return -EOVERFLOW;

    KINFO("VirtIO GPU scanout0: enabled=%u %ux%u at %u,%u\n",
          resp.pmodes[0].enabled, resp.pmodes[0].r.width,
          resp.pmodes[0].r.height, resp.pmodes[0].r.x,
          resp.pmodes[0].r.y);
    if (adopt_mode) {
        gpu.width = resp.pmodes[0].r.width;
        gpu.height = resp.pmodes[0].r.height;
        gpu.pitch = gpu.width * (FB_BPP / 8u);
    }
    return 0;
}

static int gpu_get_capset_info(uint32_t index,
                               virtio_gpu_resp_capset_info_t *info)
{
    virtio_gpu_get_capset_info_t cmd;

    if (!info)
        return -1;
    memset(&cmd, 0, sizeof(cmd));
    gpu_fill_hdr(&cmd.hdr, VIRTIO_GPU_CMD_GET_CAPSET_INFO);
    cmd.capset_index = index;
    if (gpu_submit(&cmd, sizeof(cmd), info, sizeof(*info)) < 0)
        return -1;
    if (info->hdr.type != VIRTIO_GPU_RESP_OK_CAPSET_INFO) {
        KERROR("virtio_gpu: capset[%u] response=0x%08X\n",
               index, info->hdr.type);
        return -1;
    }
    return 0;
}

static void gpu_probe_capsets(void)
{
    uint32_t count;

    gpu.has_virgl_capset = false;
    gpu.has_virgl2_capset = false;
    gpu.command_capset_id = 0;
    gpu.command_capset_max_version = 0;
    gpu.command_capset_size = 0;
    if (!(gpu.negotiated_features & (1u << VIRTIO_GPU_F_VIRGL)))
        return;

    count = gpu.num_capsets;
    if (count > VIRTIO_GPU_MAX_CAPSETS) {
        KWARN("virtio_gpu: limiting capset discovery from %u to %u\n",
              count, VIRTIO_GPU_MAX_CAPSETS);
        count = VIRTIO_GPU_MAX_CAPSETS;
    }
    for (uint32_t index = 0; index < count; index++) {
        virtio_gpu_resp_capset_info_t info;

        memset(&info, 0, sizeof(info));
        if (gpu_get_capset_info(index, &info) < 0)
            continue;
        KINFO("VirtIO GPU capset[%u]: id=%u version=%u size=%u\n",
              index, info.capset_id, info.capset_max_version,
              info.capset_max_size);
        if (info.capset_id == VIRTIO_GPU_CAPSET_VIRGL) {
            gpu.has_virgl_capset = true;
            if (gpu.command_capset_id == 0 &&
                info.capset_max_size != 0 &&
                info.capset_max_size <= ARMOS_DRM_MAX_COMMAND_CAPS_SIZE) {
                gpu.command_capset_id = info.capset_id;
                gpu.command_capset_max_version = info.capset_max_version;
                gpu.command_capset_size = info.capset_max_size;
            }
        } else if (info.capset_id == VIRTIO_GPU_CAPSET_VIRGL2) {
            gpu.has_virgl2_capset = true;
            if (info.capset_max_size != 0 &&
                info.capset_max_size <= ARMOS_DRM_MAX_COMMAND_CAPS_SIZE) {
                gpu.command_capset_id = info.capset_id;
                gpu.command_capset_max_version = info.capset_max_version;
                gpu.command_capset_size = info.capset_max_size;
            }
        }
    }
    if (!gpu_has_virgl())
        KWARN("VirtIO GPU negotiated VirGL without a usable capset\n");
}

static int virtio_gpu_backend_get_command_caps(
    void *context, uint32_t version, void *data, uint32_t size)
{
    virtio_gpu_get_capset_t cmd;
    virtio_gpu_ctrl_hdr_t *response;
    uint32_t response_size;
    int result;

    (void)context;
    if (!gpu_has_virgl() || !data ||
        gpu.command_capset_id == 0 ||
        gpu.command_capset_size == 0 ||
        gpu.command_capset_size > ARMOS_DRM_MAX_COMMAND_CAPS_SIZE ||
        size != gpu.command_capset_size ||
        version > gpu.command_capset_max_version)
        return -EINVAL;
    if (size > 0xffffffffu - sizeof(*response))
        return -EOVERFLOW;
    response_size = sizeof(*response) + size;
    response = kzalloc(response_size);
    if (!response)
        return -ENOMEM;
    memset(&cmd, 0, sizeof(cmd));
    gpu_fill_hdr(&cmd.hdr, VIRTIO_GPU_CMD_GET_CAPSET);
    cmd.capset_id = gpu.command_capset_id;
    cmd.capset_version = version;
    result = gpu_submit(&cmd, sizeof(cmd), response, response_size);
    if (result < 0) {
        result = -EIO;
        goto out;
    }
    if (response->type != VIRTIO_GPU_RESP_OK_CAPSET) {
        KERROR("virtio_gpu: capset data response=0x%08X\n",
               response->type);
        result = -EIO;
        goto out;
    }
    memcpy(data, response + 1, size);
    result = 0;
out:
    kfree(response);
    return result;
}

static int gpu_create_resource(void)
{
    virtio_gpu_resource_create_2d_t cmd;
    gpu_fill_hdr(&cmd.hdr, VIRTIO_GPU_CMD_RESOURCE_CREATE_2D);
    cmd.resource_id = VIRTIO_GPU_RESOURCE_ID;
    cmd.format = VIRTIO_GPU_FORMAT_B8G8R8A8_UNORM;
    cmd.width = gpu.width;
    cmd.height = gpu.height;
    return gpu_submit_simple(&cmd, sizeof(cmd), "resource_create_2d");
}

static int gpu_attach_backing(void)
{
    struct {
        virtio_gpu_resource_attach_backing_t cmd;
        virtio_gpu_mem_entry_t entry;
    } __attribute__((packed)) req;

    memset(&req, 0, sizeof(req));
    gpu_fill_hdr(&req.cmd.hdr, VIRTIO_GPU_CMD_RESOURCE_ATTACH_BACKING);
    req.cmd.resource_id = VIRTIO_GPU_RESOURCE_ID;
    req.cmd.nr_entries = 1;
    req.entry.addr = (uint64_t)framebuffer_phys;
    req.entry.length = gpu.framebuffer_size;
    req.entry.padding = 0;

    return gpu_submit_simple(&req, sizeof(req), "resource_attach_backing");
}

static int gpu_set_scanout(void)
{
    return gpu_set_scanout_resource(VIRTIO_GPU_SCANOUT_ID,
                                    VIRTIO_GPU_RESOURCE_ID,
                                    gpu.width, gpu.height);
}

static int gpu_set_scanout_resource(uint32_t scanout_id,
                                    uint32_t resource_id,
                                    uint32_t width, uint32_t height)
{
    virtio_gpu_set_scanout_t cmd;

    gpu_fill_hdr(&cmd.hdr, VIRTIO_GPU_CMD_SET_SCANOUT);
    cmd.r.x = 0;
    cmd.r.y = 0;
    cmd.r.width = width;
    cmd.r.height = height;
    cmd.scanout_id = scanout_id;
    cmd.resource_id = resource_id;
    if (gpu_submit_simple(&cmd, sizeof(cmd), "set_scanout") < 0)
        return -1;
    gpu.scanout_resource_id = resource_id;
    return 0;
}

static int virtio_gpu_backend_buffer_present(
    void *context, uint32_t handle, const armos_drm_buffer_desc_t *desc,
    uint32_t scanout_id, uint32_t x, uint32_t y,
    uint32_t width, uint32_t height)
{
    virtio_gpu_transfer_to_host_2d_t transfer;
    virtio_gpu_resource_flush_t flush;
    uint32_t resource_id = gpu_drm_resource_id(handle);

    (void)context;
    if (!resource_id || !desc ||
        !(desc->flags & ARMOS_DRM_BO_SCANOUT) ||
        scanout_id >= (gpu.num_scanouts ? gpu.num_scanouts : 1u))
        return -EINVAL;
    memset(&transfer, 0, sizeof(transfer));
    gpu_fill_hdr(&transfer.hdr, VIRTIO_GPU_CMD_TRANSFER_TO_HOST_2D);
    transfer.r.x = x;
    transfer.r.y = y;
    transfer.r.width = width;
    transfer.r.height = height;
    transfer.offset = (uint64_t)y * desc->stride +
                      (uint64_t)x * sizeof(uint32_t);
    transfer.resource_id = resource_id;
    if (gpu_submit_simple(&transfer, sizeof(transfer),
                          "drm_transfer_to_host_2d") < 0)
        return -EIO;
    if (gpu.scanout_resource_id != resource_id &&
        gpu_set_scanout_resource(scanout_id, resource_id,
                                 desc->width, desc->height) < 0)
        return -EIO;
    memset(&flush, 0, sizeof(flush));
    gpu_fill_hdr(&flush.hdr, VIRTIO_GPU_CMD_RESOURCE_FLUSH);
    flush.r = transfer.r;
    flush.resource_id = resource_id;
    return gpu_submit_simple(&flush, sizeof(flush),
                             "drm_resource_flush") < 0 ? -EIO : 0;
}

int virtio_gpu_flush_rect(uint32_t x, uint32_t y, uint32_t width, uint32_t height)
{
    if (!gpu.initialized)
        return -1;

    if (x >= gpu.width || y >= gpu.height || width == 0 || height == 0)
        return 0;

    if (width > gpu.width - x)
        width = gpu.width - x;
    if (height > gpu.height - y)
        height = gpu.height - y;

    for (uint32_t row = 0; row < height; row++) {
        uint8_t *line = framebuffer_base +
            (y + row) * gpu.pitch + x * (FB_BPP / 8u);
        arch_clean_dcache_by_mva(line, width * (FB_BPP / 8));
    }

    virtio_gpu_transfer_to_host_2d_t tx;
    gpu_fill_hdr(&tx.hdr, VIRTIO_GPU_CMD_TRANSFER_TO_HOST_2D);
    tx.r.x = x;
    tx.r.y = y;
    tx.r.width = width;
    tx.r.height = height;
    tx.offset = (uint64_t)y * gpu.pitch +
                (uint64_t)x * (FB_BPP / 8u);
    tx.resource_id = VIRTIO_GPU_RESOURCE_ID;
    tx.padding = 0;
    if (gpu_submit_simple(&tx, sizeof(tx), "transfer_to_host_2d") < 0)
        return -1;

    virtio_gpu_resource_flush_t fl;
    gpu_fill_hdr(&fl.hdr, VIRTIO_GPU_CMD_RESOURCE_FLUSH);
    fl.r.x = x;
    fl.r.y = y;
    fl.r.width = width;
    fl.r.height = height;
    fl.resource_id = VIRTIO_GPU_RESOURCE_ID;
    fl.padding = 0;
    return gpu_submit_simple(&fl, sizeof(fl), "resource_flush");
}

int virtio_gpu_flush(void)
{
    return virtio_gpu_flush_rect(0, 0, gpu.width, gpu.height);
}

/*
 * Poll and handle a display configuration change (QEMU window resize).
 *
 * The guest resource keeps its negotiated boot geometry. A host window resize
 * may disturb the scanout state but does not implicitly change that resource.
 * Re-asserting the current scanout preserves its owner across the event. It
 * must never switch an active DRM scanout back to the boot framebuffer.
 * Must be called from task context (displayd): it submits synchronous GPU
 * commands.
 *
 * Returns true when a config change was seen and handled; the caller should
 * mark the whole framebuffer dirty so the next frame repaints everything.
 */
bool virtio_gpu_check_resize(void)
{
    uint32_t irq_status;
    bool pending;
    unsigned long flags;

    if (!gpu.initialized)
        return false;

    spin_lock_irqsave(&gpu_lock, &flags);
    pending = gpu.config_pending;
    gpu.config_pending = false;
    spin_unlock_irqrestore(&gpu_lock, flags);
    irq_status = mmio_read32(gpu.mmio, VIRTIO_MMIO_INTERRUPT_STATUS);
    if (!pending && !(irq_status & VIRTIO_GPU_INT_CONFIG))
        return false;

    if (irq_status & VIRTIO_GPU_INT_CONFIG)
        mmio_write32(gpu.mmio, VIRTIO_MMIO_INTERRUPT_ACK,
                     VIRTIO_GPU_INT_CONFIG);

    (void)gpu_get_display_info(false);
    if (gpu_set_scanout_resource(
            VIRTIO_GPU_SCANOUT_ID,
            gpu.scanout_resource_id ?
                gpu.scanout_resource_id : VIRTIO_GPU_RESOURCE_ID,
            gpu.width, gpu.height) < 0) {
        KERROR("virtio_gpu: scanout re-assert failed after resize\n");
        return false;
    }

    return true;
}

static void gpu_draw_text_px(uint32_t x, uint32_t y, const char *s,
                             uint32_t fg, uint32_t bg)
{
    while (*s) {
        draw_char(x, y, *s++, fg, bg);
        x += 8;
    }
}

static void gpu_draw_ascii_grid(uint32_t x0, uint32_t y0)
{
    const uint32_t cell_w = 60;
    const uint32_t cell_h = 44;
    const uint32_t dim = 0xFF9AA7B2;
    const uint32_t bg = 0xFF101820;
    static const uint32_t palette[] = {
        0xFFFFFFFF, 0xFFFF5252, 0xFFFFC107, 0xFF4CAF50,
        0xFF00BCD4, 0xFF42A5F5, 0xFF7E57C2, 0xFFFF80AB
    };
    char label[4];

    for (uint32_t ch = 32; ch <= 126; ch++) {
        uint32_t i = ch - 32;
        uint32_t x = x0 + (i % 16) * cell_w;
        uint32_t y = y0 + (i / 16) * cell_h;
        uint32_t fg = palette[i % (sizeof(palette) / sizeof(palette[0]))];

        label[0] = (char)('0' + ((ch / 100) % 10));
        label[1] = (char)('0' + ((ch / 10) % 10));
        label[2] = (char)('0' + (ch % 10));
        label[3] = '\0';

        gpu_draw_text_px(x, y, label, dim, bg);
        draw_char(x + 24, y + 14, (char)ch, fg, bg);
    }
}

void virtio_gpu_draw_test_pattern(void)
{
    if (!framebuffer_base)
        return;

    uint32_t *fb = (uint32_t *)framebuffer_base;
    for (uint32_t y = 0; y < gpu.height; y++) {
        for (uint32_t x = 0; x < gpu.width; x++) {
            uint8_t r = (uint8_t)((x * 255) / gpu.width);
            uint8_t g = (uint8_t)((y * 255) / gpu.height);
            uint32_t extent = gpu.width > gpu.height ?
                gpu.width : gpu.height;
            uint8_t b = (uint8_t)(((x ^ y) * 255) / extent);
            fb[y * gpu.width + x] = 0xFF000000u |
                                    ((uint32_t)r << 16) |
                                    ((uint32_t)g << 8) |
                                    b;
        }
    }

    const uint32_t title_fg = 0xFFFFFFFF;
    const uint32_t title_bg = 0xFF263238;
    const uint32_t text_fg = 0xFFE0E0E0;
    const uint32_t text_bg = 0xFF101820;
    const uint32_t green = 0xFF4CAF50;
    const uint32_t amber = 0xFFFFC107;

    gpu_draw_text_px(32, 32, "ArmOS virtio-gpu framebuffer", title_fg, title_bg);
    gpu_draw_text_px(32, 56, "Meslo 12x24 boot test", green, text_bg);
    gpu_draw_text_px(32, 88, "ABCDEFGHIJKLMNOPQRSTUVWXYZ", text_fg, text_bg);
    gpu_draw_text_px(32, 112, "abcdefghijklmnopqrstuvwxyz", text_fg, text_bg);
    gpu_draw_text_px(32, 136, "0123456789  !?.,;:-_+=*/\\|()[]{}<>@#$%^&~", amber, text_bg);
    gpu_draw_text_px(32, 176, "white  red    amber  green  cyan   blue   violet pink", title_fg, text_bg);
    gpu_draw_text_px(32, 200, "The quick brown fox jumps over the lazy dog.", 0xFFFFFFFF, 0xFF263238);
    gpu_draw_text_px(32, 224, "The quick brown fox jumps over the lazy dog.", 0xFFFF5252, text_bg);
    gpu_draw_text_px(32, 248, "The quick brown fox jumps over the lazy dog.", 0xFFFFC107, text_bg);
    gpu_draw_text_px(32, 272, "The quick brown fox jumps over the lazy dog.", 0xFF4CAF50, text_bg);
    gpu_draw_text_px(32, 296, "The quick brown fox jumps over the lazy dog.", 0xFF00BCD4, text_bg);
    gpu_draw_text_px(32, 320, "The quick brown fox jumps over the lazy dog.", 0xFF42A5F5, text_bg);
    gpu_draw_text_px(32, 344, "The quick brown fox jumps over the lazy dog.", 0xFF7E57C2, text_bg);
    gpu_draw_text_px(32, 368, "The quick brown fox jumps over the lazy dog.", 0xFFFF80AB, text_bg);
    gpu_draw_text_px(32, 408, "ASCII 32..126, colored per glyph:", title_fg, text_bg);
    gpu_draw_ascii_grid(32, 440);
}

bool virtio_gpu_is_initialized(void)
{
    return gpu.initialized;
}

uint32_t virtio_gpu_width(void)
{
    return gpu.width;
}

uint32_t virtio_gpu_height(void)
{
    return gpu.height;
}

bool virtio_gpu_init(void)
{
    paddr_t phys = 0;
    uint32_t irq = 0;

    memset(&gpu, 0, sizeof(gpu));

    if (!gpu_probe_from_dtb(&phys, &irq)) {
        return false;
    }

    gpu.phys = phys;
    gpu.irq = irq;
    gpu.mmio = arch_platform_virtio_mmio_base(phys);
    KINFO("VirtIO GPU found: phys=0x%lX mmio=%p irq=%u\n",
          (unsigned long)gpu.phys, gpu.mmio, gpu.irq);

    uint32_t magic = mmio_read32(gpu.mmio, VIRTIO_MMIO_MAGIC);
    uint32_t version = mmio_read32(gpu.mmio, VIRTIO_MMIO_VERSION);
    uint32_t devid = mmio_read32(gpu.mmio, VIRTIO_MMIO_DEVICE_ID);
    if (magic != 0x74726976 || version != 1 || devid != VIRTIO_ID_GPU) {
        KERROR("virtio_gpu: bad device magic=0x%08X version=%u id=%u\n",
               magic, version, devid);
        return false;
    }

    mmio_write32(gpu.mmio, VIRTIO_MMIO_STATUS, 0);
    mmio_write32(gpu.mmio, VIRTIO_MMIO_STATUS, VIRTIO_STATUS_ACK);
    mmio_write32(gpu.mmio, VIRTIO_MMIO_STATUS,
                 mmio_read32(gpu.mmio, VIRTIO_MMIO_STATUS) | VIRTIO_STATUS_DRIVER);
    gpu.device_features =
        mmio_read32(gpu.mmio, VIRTIO_MMIO_DEVICE_FEATURES);
    gpu.negotiated_features =
        gpu.device_features & (1u << VIRTIO_GPU_F_VIRGL);
    mmio_write32(gpu.mmio, VIRTIO_MMIO_DRIVER_FEATURES,
                 gpu.negotiated_features);
    mmio_write32(gpu.mmio, VIRTIO_MMIO_STATUS,
                 mmio_read32(gpu.mmio, VIRTIO_MMIO_STATUS) | VIRTIO_STATUS_FEATURES_OK);
    if (!(mmio_read32(gpu.mmio, VIRTIO_MMIO_STATUS) & VIRTIO_STATUS_FEATURES_OK)) {
        KERROR("virtio_gpu: features rejected\n");
        return false;
    }

    mmio_write32(gpu.mmio, VIRTIO_REG_GUEST_PAGE_SIZE, PAGE_SIZE);
    mmio_write32(gpu.mmio, VIRTIO_MMIO_QUEUE_SEL, 0);
    uint32_t qmax = mmio_read32(gpu.mmio, VIRTIO_MMIO_QUEUE_NUM_MAX);
    if (qmax < 2) {
        KERROR("virtio_gpu: control queue unavailable\n");
        return false;
    }
    uint16_t qsize = VIRTIO_GPU_VQ_SIZE <= qmax ? VIRTIO_GPU_VQ_SIZE : (uint16_t)qmax;
    mmio_write32(gpu.mmio, VIRTIO_MMIO_QUEUE_NUM, qsize);
    mmio_write32(gpu.mmio, VIRTIO_MMIO_QUEUE_ALIGN_OFF, VQ_ALIGN);

    if (!gpu_vq_alloc(&gpu.vq, qsize)) {
        KERROR("virtio_gpu: virtqueue allocation failed\n");
        return false;
    }
    mmio_write32(gpu.mmio, VIRTIO_MMIO_QUEUE_PFN, gpu.vq.pa_base >> 12);

    gpu.initialized = true;
    mmio_write32(gpu.mmio, VIRTIO_MMIO_STATUS,
                 mmio_read32(gpu.mmio, VIRTIO_MMIO_STATUS) | VIRTIO_STATUS_DRIVER_OK);

    gpu.num_scanouts = mmio_read32(gpu.mmio, VIRTIO_GPU_CFG_NUM_SCANOUTS);
    gpu.num_capsets = mmio_read32(gpu.mmio, VIRTIO_GPU_CFG_NUM_CAPSETS);
    KINFO("VirtIO GPU features: device=0x%08X negotiated=0x%08X "
          "scanouts=%u capsets=%u\n",
          gpu.device_features, gpu.negotiated_features,
          gpu.num_scanouts, gpu.num_capsets);
    gpu_probe_capsets();
    if (gpu_has_virgl()) {
        KINFO("VirtIO GPU acceleration: %s capset=%u version=%u size=%u\n",
              gpu.has_virgl2_capset ? "VirGL2" : "VirGL",
              gpu.command_capset_id, gpu.command_capset_max_version,
              gpu.command_capset_size);
    } else {
        KINFO("VirtIO GPU acceleration: 2D\n");
    }
    if (gpu_get_display_info(true) < 0) {
        KERROR("virtio_gpu: no supported scanout mode\n");
        gpu.initialized = false;
        mmio_write32(gpu.mmio, VIRTIO_MMIO_STATUS,
                     mmio_read32(gpu.mmio, VIRTIO_MMIO_STATUS) |
                     VIRTIO_STATUS_FAILED);
        return false;
    }
    if (!init_display(gpu.width, gpu.height, FB_BPP)) {
        KERROR("virtio_gpu: framebuffer allocation failed for %ux%u\n",
               gpu.width, gpu.height);
        gpu.initialized = false;
        mmio_write32(gpu.mmio, VIRTIO_MMIO_STATUS,
                     mmio_read32(gpu.mmio, VIRTIO_MMIO_STATUS) |
                     VIRTIO_STATUS_FAILED);
        return false;
    }
    gpu.framebuffer_size = gpu.pitch * gpu.height;
    if (gpu_create_resource() < 0 ||
        gpu_attach_backing() < 0 ||
        gpu_set_scanout() < 0) {
        gpu.initialized = false;
        mmio_write32(gpu.mmio, VIRTIO_MMIO_STATUS,
                     mmio_read32(gpu.mmio, VIRTIO_MMIO_STATUS) | VIRTIO_STATUS_FAILED);
        return false;
    }

    clear_screen();
    if (virtio_gpu_flush() < 0) {
        KWARN("VirtIO GPU initialized but initial flush failed\n");
        return false;
    }
    if (armos_drm_backend_register(&virtio_gpu_backend_ops, &gpu) < 0) {
        KERROR("virtio_gpu: ArmOS DRM backend registration failed\n");
        return false;
    }
    irq_enable(gpu.irq);

    KINFO("VirtIO GPU initialized: %ux%ux%u\n",
          gpu.width, gpu.height, FB_BPP);
    return true;
}
