/*
 * ArmOS
 * Copyright (c) 2026 Mohamed Ennassiri
 *
 * Licensed under the Apache License, Version 2.0.
 * See LICENSE for details.
 *
 * File: kernel/drivers/drm.c
 * Layer: Kernel / common ArmOS DRM device model
 *
 * Responsibilities:
 * - Own architecture-neutral card and render nodes and backend registration.
 * - Translate generic backend information into the stable userspace ABI.
 * - Keep all hardware, transport, and command-stream details out of common code.
 *
 * Notes:
 * - Registration happens once during platform device initialization.
 * - Per-file context ownership guarantees cleanup on descriptor close.
 */

#include <kernel/arch_barrier.h>
#include <kernel/address_space.h>
#include <kernel/file.h>
#include <kernel/drm.h>
#include <kernel/memory.h>
#include <kernel/spinlock.h>
#include <kernel/string.h>
#include <kernel/syscalls.h>
#include <kernel/timer.h>
#include <kernel/vfs.h>
#include <uapi/armos/drm.h>

typedef char armos_drm_bo_create_abi_size_must_remain_64[
    sizeof(armos_drm_bo_create_t) == 64u ? 1 : -1];
typedef char armos_drm_bo_export_abi_size_must_remain_32[
    sizeof(armos_drm_bo_export_t) == 32u ? 1 : -1];
typedef char armos_drm_bo_import_abi_size_must_remain_72[
    sizeof(armos_drm_bo_import_t) == 72u ? 1 : -1];
typedef char armos_drm_bo_set_metadata_abi_size_must_remain_40[
    sizeof(armos_drm_bo_set_metadata_t) == 40u ? 1 : -1];
typedef char armos_drm_fence_export_abi_size_must_remain_32[
    sizeof(armos_drm_fence_export_t) == 32u ? 1 : -1];
typedef char armos_drm_fence_result_abi_size_must_remain_8[
    sizeof(armos_drm_fence_result_t) == 8u ? 1 : -1];

#define DEV_DRI_CARD0_RDEV      0xE200u
#define DEV_DRI_RENDER128_RDEV  0xE280u
#define ARMOS_DRM_MAX_FILE_CONTEXTS 32u
#define ARMOS_DRM_MAX_FILE_BUFFERS 64u
#define ARMOS_DRM_MAX_GLOBAL_BUFFERS 256u
#define ARMOS_DRM_MAX_ATTACHMENTS 128u
#define ARMOS_DRM_MAX_FENCES 128u
#define ARMOS_DRM_MAX_BUFFER_SIZE (64u * 1024u * 1024u)
#define ARMOS_DRM_MAX_MMAP_HANDLE 0x0007ffffu

typedef struct armos_drm_buffer {
    uint32_t handle;
    uint32_t command_handle;
    armos_drm_buffer_desc_t desc;
    paddr_t *pages;
    uint32_t page_count;
    uint32_t mapping_count;
    uint32_t owner_count;
    uint32_t export_count;
    bool retired;
    bool cleanup_complete;
    bool backend_created;
    uint32_t command_descriptor_size;
    uint8_t command_descriptor[ARMOS_DRM_MAX_RESOURCE_DESCRIPTOR_SIZE];
    spinlock_t lock;
} armos_drm_buffer_t;

typedef struct armos_drm_attachment {
    uint32_t context_id;
    uint32_t handle;
} armos_drm_attachment_t;

typedef struct armos_drm_fence {
    uint64_t id;
    int status;
    bool completed;
    uint32_t owner_count;
    uint32_t export_count;
    bool retired;
} armos_drm_fence_t;

typedef struct armos_drm_file_state {
    armos_drm_node_t node;
    uint32_t contexts[ARMOS_DRM_MAX_FILE_CONTEXTS];
    uint32_t context_count;
    armos_drm_buffer_t *buffers[ARMOS_DRM_MAX_FILE_BUFFERS];
    uint32_t buffer_count;
    armos_drm_attachment_t attachments[ARMOS_DRM_MAX_ATTACHMENTS];
    uint32_t attachment_count;
    armos_drm_fence_t *fences[ARMOS_DRM_MAX_FENCES];
    uint32_t fence_count;
    spinlock_t lock;
    bool busy;
    bool closing;
    task_t *owner;
} armos_drm_file_state_t;

static spinlock_t drm_lock = SPINLOCK_INIT("armos_drm");
static const armos_drm_backend_ops_t *drm_ops;
static void *drm_context;
static uint32_t drm_next_context_id = 1u;
static uint32_t drm_next_buffer_handle = 1u;
static uint64_t drm_next_fence_id = 1u;
static armos_drm_buffer_t *drm_buffers[ARMOS_DRM_MAX_GLOBAL_BUFFERS];
static armos_drm_fence_t *drm_fences[ARMOS_DRM_MAX_FENCES];
static armos_drm_file_state_t *drm_card_owner;

static int armos_drm_file_close(file_t *file);
static int armos_drm_bo_file_close(file_t *file);
static int armos_drm_fence_file_close(file_t *file);
static ssize_t armos_drm_fence_file_read(file_t *file, void *buffer,
                                         size_t count);
static void drm_unregister_buffer(armos_drm_buffer_t *buffer);

static file_operations_t drm_file_ops = {
    .read = NULL,
    .write = NULL,
    .open = NULL,
    .close = armos_drm_file_close,
    .lseek = NULL,
    .readdir = NULL,
    .truncate = NULL,
};

static file_operations_t drm_bo_file_ops = {
    .read = NULL,
    .write = NULL,
    .open = NULL,
    .close = armos_drm_bo_file_close,
    .lseek = NULL,
    .readdir = NULL,
    .truncate = NULL,
};

static file_operations_t drm_fence_file_ops = {
    .read = armos_drm_fence_file_read,
    .write = NULL,
    .open = NULL,
    .close = armos_drm_fence_file_close,
    .lseek = NULL,
    .readdir = NULL,
    .truncate = NULL,
};

static int drm_state_acquire(armos_drm_file_state_t *state)
{
    task_t *task = task_current_local();

    if (!state)
        return -EINVAL;
    while (1) {
        spin_lock(&state->lock);
        if (state->closing) {
            spin_unlock(&state->lock);
            return -ENODEV;
        }
        if (!state->busy) {
            state->busy = true;
            state->owner = task;
            spin_unlock(&state->lock);
            return 0;
        }
        if (task && state->owner == task) {
            spin_unlock(&state->lock);
            return -EBUSY;
        }
        spin_unlock(&state->lock);
        if (task)
            yield();
        else
            arch_cpu_relax();
    }
}

static void drm_state_release(armos_drm_file_state_t *state)
{
    if (!state)
        return;
    spin_lock(&state->lock);
    state->owner = NULL;
    state->busy = false;
    spin_unlock(&state->lock);
}

static void drm_backend_snapshot(const armos_drm_backend_ops_t **ops,
                                 void **context)
{
    spin_lock(&drm_lock);
    *ops = drm_ops;
    *context = drm_context;
    spin_unlock(&drm_lock);
}

static bool drm_state_has_context(const armos_drm_file_state_t *state,
                                  uint32_t context_id,
                                  uint32_t *index_out)
{
    for (uint32_t index = 0; index < state->context_count; index++) {
        if (state->contexts[index] == context_id) {
            if (index_out)
                *index_out = index;
            return true;
        }
    }
    return false;
}

static armos_drm_buffer_t *drm_state_find_buffer(
    const armos_drm_file_state_t *state, uint32_t handle, uint32_t *index_out)
{
    for (uint32_t index = 0; index < state->buffer_count; index++) {
        armos_drm_buffer_t *buffer = state->buffers[index];

        if (buffer && buffer->handle == handle) {
            if (index_out)
                *index_out = index;
            return buffer;
        }
    }
    return NULL;
}

static int drm_find_attachment(const armos_drm_file_state_t *state,
                               uint32_t context_id, uint32_t handle)
{
    for (uint32_t index = 0; index < state->attachment_count; index++) {
        if (state->attachments[index].context_id == context_id &&
            state->attachments[index].handle == handle)
            return (int)index;
    }
    return -1;
}

static void drm_buffer_free(armos_drm_buffer_t *buffer)
{
    if (!buffer)
        return;
    for (uint32_t index = 0; index < buffer->page_count; index++) {
        if (buffer->pages[index])
            free_page((void *)(uintptr_t)buffer->pages[index]);
    }
    kfree(buffer->pages);
    kfree(buffer);
}

static void drm_buffer_put_mapping(uintptr_t cookie)
{
    armos_drm_buffer_t *buffer = (armos_drm_buffer_t *)cookie;
    bool release = false;

    if (!buffer)
        return;
    spin_lock(&buffer->lock);
    if (buffer->mapping_count != 0)
        buffer->mapping_count--;
    release = buffer->retired && buffer->cleanup_complete &&
              buffer->mapping_count == 0;
    spin_unlock(&buffer->lock);
    if (release)
        drm_buffer_free(buffer);
}

static void drm_buffer_retire(armos_drm_buffer_t *buffer)
{
    const armos_drm_backend_ops_t *ops;
    void *context;
    bool release;

    if (!buffer)
        return;
    drm_backend_snapshot(&ops, &context);
    if (buffer->backend_created && ops && ops->buffer_destroy) {
        (void)ops->buffer_destroy(context, buffer->handle);
        buffer->backend_created = false;
    }
    drm_unregister_buffer(buffer);
    spin_lock(&buffer->lock);
    buffer->cleanup_complete = true;
    release = buffer->mapping_count == 0;
    spin_unlock(&buffer->lock);
    if (release)
        drm_buffer_free(buffer);
}

static int drm_buffer_add_owner(armos_drm_buffer_t *buffer)
{
    int result = 0;

    if (!buffer)
        return -EINVAL;
    spin_lock(&buffer->lock);
    if (buffer->retired || buffer->owner_count == 0xffffffffu)
        result = buffer->retired ? -ENOENT : -EOVERFLOW;
    else
        buffer->owner_count++;
    spin_unlock(&buffer->lock);
    return result;
}

static void drm_buffer_drop_owner(armos_drm_buffer_t *buffer)
{
    bool retire = false;

    if (!buffer)
        return;
    spin_lock(&buffer->lock);
    if (buffer->owner_count != 0)
        buffer->owner_count--;
    if (!buffer->retired && buffer->owner_count == 0 &&
        buffer->export_count == 0) {
        buffer->retired = true;
        retire = true;
    }
    spin_unlock(&buffer->lock);
    if (retire)
        drm_buffer_retire(buffer);
}

static int drm_buffer_add_export(armos_drm_buffer_t *buffer)
{
    int result = 0;

    if (!buffer)
        return -EINVAL;
    spin_lock(&buffer->lock);
    if (buffer->retired || buffer->export_count == 0xffffffffu)
        result = buffer->retired ? -ENOENT : -EOVERFLOW;
    else
        buffer->export_count++;
    spin_unlock(&buffer->lock);
    return result;
}

static void drm_buffer_drop_export(armos_drm_buffer_t *buffer)
{
    bool retire = false;

    if (!buffer)
        return;
    spin_lock(&buffer->lock);
    if (buffer->export_count != 0)
        buffer->export_count--;
    if (!buffer->retired && buffer->owner_count == 0 &&
        buffer->export_count == 0) {
        buffer->retired = true;
        retire = true;
    }
    spin_unlock(&buffer->lock);
    if (retire)
        drm_buffer_retire(buffer);
}

static int drm_register_buffer(armos_drm_buffer_t *buffer)
{
    int slot = -1;

    if (!buffer)
        return -EINVAL;
    spin_lock(&drm_lock);
    for (uint32_t index = 0; index < ARMOS_DRM_MAX_GLOBAL_BUFFERS; index++) {
        if (!drm_buffers[index]) {
            slot = (int)index;
            break;
        }
    }
    if (slot >= 0) {
        for (uint32_t attempt = 0;
             attempt < ARMOS_DRM_MAX_MMAP_HANDLE; attempt++) {
            uint32_t candidate = drm_next_buffer_handle++;
            bool used = false;

            if (drm_next_buffer_handle == 0 ||
                drm_next_buffer_handle > ARMOS_DRM_MAX_MMAP_HANDLE)
                drm_next_buffer_handle = 1u;
            for (uint32_t index = 0;
                 index < ARMOS_DRM_MAX_GLOBAL_BUFFERS; index++) {
                if (drm_buffers[index] &&
                    drm_buffers[index]->handle == candidate) {
                    used = true;
                    break;
                }
            }
            if (!used) {
                buffer->handle = candidate;
                drm_buffers[slot] = buffer;
                break;
            }
        }
        if (buffer->handle == 0)
            slot = -1;
    }
    spin_unlock(&drm_lock);
    return slot >= 0 ? 0 : -ENOSPC;
}

static void drm_unregister_buffer(armos_drm_buffer_t *buffer)
{
    if (!buffer)
        return;
    spin_lock(&drm_lock);
    for (uint32_t index = 0; index < ARMOS_DRM_MAX_GLOBAL_BUFFERS; index++) {
        if (drm_buffers[index] == buffer) {
            drm_buffers[index] = NULL;
            break;
        }
    }
    spin_unlock(&drm_lock);
}

static bool drm_fence_retire_locked(armos_drm_fence_t *fence)
{
    if (fence && !fence->retired && fence->owner_count == 0 &&
        fence->export_count == 0) {
        for (uint32_t index = 0; index < ARMOS_DRM_MAX_FENCES; index++) {
            if (drm_fences[index] == fence) {
                drm_fences[index] = NULL;
                fence->retired = true;
                return true;
            }
        }
    }
    return false;
}

static void drm_fence_drop_owner(armos_drm_fence_t *fence)
{
    bool release;

    if (!fence)
        return;
    spin_lock(&drm_lock);
    if (fence->owner_count != 0)
        fence->owner_count--;
    release = drm_fence_retire_locked(fence);
    spin_unlock(&drm_lock);
    if (release)
        kfree(fence);
}

static int drm_fence_add_export(armos_drm_fence_t *fence)
{
    int result = 0;

    if (!fence)
        return -EINVAL;
    spin_lock(&drm_lock);
    if (fence->retired || fence->export_count == 0xffffffffu)
        result = fence->retired ? -ENOENT : -EOVERFLOW;
    else
        fence->export_count++;
    spin_unlock(&drm_lock);
    return result;
}

static void drm_fence_drop_export(armos_drm_fence_t *fence)
{
    bool release;

    if (!fence)
        return;
    spin_lock(&drm_lock);
    if (fence->export_count != 0)
        fence->export_count--;
    release = drm_fence_retire_locked(fence);
    spin_unlock(&drm_lock);
    if (release)
        kfree(fence);
}

static uint64_t drm_divide_u64_u32(uint64_t dividend, uint32_t divisor)
{
    uint64_t quotient = 0;
    uint64_t remainder = 0;

    if (divisor == 0)
        return 0;
    for (int bit = 63; bit >= 0; bit--) {
        remainder = (remainder << 1) | ((dividend >> bit) & 1u);
        if (remainder >= divisor) {
            remainder -= divisor;
            quotient |= 1ULL << bit;
        }
    }
    return quotient;
}

void armos_drm_fence_complete(uint64_t fence_id, int status)
{
    armos_drm_fence_t *fence = NULL;

    if (fence_id == 0)
        return;
    spin_lock(&drm_lock);
    for (uint32_t index = 0; index < ARMOS_DRM_MAX_FENCES; index++) {
        if (drm_fences[index] && drm_fences[index]->id == fence_id) {
            fence = drm_fences[index];
            fence->status = status;
            fence->completed = true;
            break;
        }
    }
    spin_unlock(&drm_lock);
    if (fence)
        task_poll_notify();
}

int armos_drm_backend_register(const armos_drm_backend_ops_t *ops,
                               void *context)
{
    int result = 0;

    if (!ops || !ops->get_info)
        return -EINVAL;
    spin_lock(&drm_lock);
    if (drm_ops)
        result = -EBUSY;
    else {
        drm_context = context;
        drm_ops = ops;
    }
    spin_unlock(&drm_lock);
    return result;
}

bool armos_drm_device_available(void)
{
    bool available;

    spin_lock(&drm_lock);
    available = drm_ops != NULL;
    spin_unlock(&drm_lock);
    return available;
}

armos_drm_node_t armos_drm_node_from_path(const char *path)
{
    if (!path)
        return ARMOS_DRM_NODE_INVALID;
    if (strcmp(path, "/dev/dri/card0") == 0)
        return ARMOS_DRM_NODE_CARD;
    if (strcmp(path, "/dev/dri/renderD128") == 0)
        return ARMOS_DRM_NODE_RENDER;
    return ARMOS_DRM_NODE_INVALID;
}

void fill_armos_drm_device_stat(struct stat *st, armos_drm_node_t node)
{
    uint32_t now;
    uint32_t rdev;
    uint16_t permissions;

    if (!st || node == ARMOS_DRM_NODE_INVALID)
        return;
    rdev = node == ARMOS_DRM_NODE_CARD ?
        DEV_DRI_CARD0_RDEV : DEV_DRI_RENDER128_RDEV;
    permissions = node == ARMOS_DRM_NODE_CARD ? 0600 : 0666;
    now = get_current_time();
    memset(st, 0, sizeof(*st));
    st->st_ino = rdev;
    st->st_mode = S_IFCHR | permissions;
    st->st_nlink = 1;
    st->st_uid = 0;
    st->st_gid = 0;
    st->st_rdev = rdev;
    st->st_blksize = 4096u;
    st->st_atime = now;
    st->st_mtime = now;
    st->st_ctime = now;
}

int create_armos_drm_device_file(const char *name, int flags,
                                 armos_drm_node_t node, file_t **out_file)
{
    file_t *file;
    inode_t *inode;
    armos_drm_file_state_t *state;
    struct stat st;

    if (!out_file)
        return -EINVAL;
    *out_file = NULL;
    if (!armos_drm_device_available() || node == ARMOS_DRM_NODE_INVALID)
        return -ENODEV;
    file = create_file();
    if (!file)
        return -ENOMEM;
    inode = create_inode();
    if (!inode) {
        kfree(file);
        return -ENOMEM;
    }
    state = kmalloc(sizeof(*state));
    if (!state) {
        put_inode(inode);
        kfree(file);
        return -ENOMEM;
    }
    memset(state, 0, sizeof(*state));
    state->node = node;
    init_spinlock_named(&state->lock, "armos_drm_file");
    fill_armos_drm_device_stat(&st, node);
    inode->mode = st.st_mode;
    inode->uid = st.st_uid;
    inode->gid = st.st_gid;
    inode->nlink = st.st_nlink;
    inode->parent_cluster = st.st_rdev;
    inode->f_op = &drm_file_ops;
    file->f_op = &drm_file_ops;
    file->flags = flags;
    file->type = FILE_TYPE_DRM;
    file->inode = inode;
    file->private_data = state;
    if (name) {
        strncpy(file->name, name, sizeof(file->name) - 1u);
        file->name[sizeof(file->name) - 1u] = '\0';
    }
    if (node == ARMOS_DRM_NODE_CARD) {
        spin_lock(&drm_lock);
        if (drm_card_owner) {
            spin_unlock(&drm_lock);
            file->private_data = NULL;
            put_inode(inode);
            kfree(state);
            kfree(file);
            return -EBUSY;
        }
        drm_card_owner = state;
        spin_unlock(&drm_lock);
    }
    *out_file = file;
    return 0;
}

static file_t *drm_buffer_create_export_file(armos_drm_buffer_t *buffer)
{
    file_t *file;
    int result;

    result = drm_buffer_add_export(buffer);
    if (result < 0)
        return NULL;
    file = create_file();
    if (!file) {
        drm_buffer_drop_export(buffer);
        return NULL;
    }
    file->f_op = &drm_bo_file_ops;
    file->flags = O_RDWR;
    file->type = FILE_TYPE_DRM_BO;
    file->private_data = buffer;
    strncpy(file->name, "drm-bo", sizeof(file->name) - 1u);
    return file;
}

static int armos_drm_bo_file_close(file_t *file)
{
    armos_drm_buffer_t *buffer = file ? file->private_data : NULL;

    if (!buffer)
        return 0;
    file->private_data = NULL;
    drm_buffer_drop_export(buffer);
    return 0;
}

static file_t *drm_fence_create_export_file(armos_drm_fence_t *fence)
{
    file_t *file;
    int result;

    result = drm_fence_add_export(fence);
    if (result < 0)
        return NULL;
    file = create_file();
    if (!file) {
        drm_fence_drop_export(fence);
        return NULL;
    }
    file->f_op = &drm_fence_file_ops;
    file->flags = O_RDONLY;
    file->type = FILE_TYPE_DRM_FENCE;
    file->private_data = fence;
    strncpy(file->name, "drm-fence", sizeof(file->name) - 1u);
    return file;
}

static int armos_drm_fence_file_close(file_t *file)
{
    armos_drm_fence_t *fence = file ? file->private_data : NULL;

    if (!fence)
        return 0;
    file->private_data = NULL;
    drm_fence_drop_export(fence);
    return 0;
}

bool armos_drm_fence_file_read_ready(file_t *file)
{
    armos_drm_fence_t *fence = file ? file->private_data : NULL;
    bool ready;

    if (!fence || file->type != FILE_TYPE_DRM_FENCE)
        return false;
    spin_lock(&drm_lock);
    ready = fence->completed;
    spin_unlock(&drm_lock);
    return ready;
}

static ssize_t armos_drm_fence_file_read(file_t *file, void *buffer,
                                         size_t count)
{
    armos_drm_fence_t *fence = file ? file->private_data : NULL;
    armos_drm_fence_result_t result;

    if (!fence || !buffer || count != sizeof(result))
        return -EINVAL;
    while (1) {
        spin_lock(&drm_lock);
        if (fence->completed) {
            result.status = fence->status;
            result.flags = 0;
            spin_unlock(&drm_lock);
            memcpy(buffer, &result, sizeof(result));
            return (ssize_t)sizeof(result);
        }
        spin_unlock(&drm_lock);
        if ((file->flags & O_NONBLOCK) != 0)
            return -EAGAIN;
        if (task_poll_wait_once() != 0)
            return -EINTR;
    }
}

static int armos_drm_file_close(file_t *file)
{
    armos_drm_file_state_t *state =
        file ? file->private_data : NULL;
    const armos_drm_backend_ops_t *ops;
    void *context;

    if (!state)
        return 0;
    if (drm_state_acquire(state) < 0)
        return -EBUSY;
    spin_lock(&state->lock);
    state->closing = true;
    spin_unlock(&state->lock);
    drm_backend_snapshot(&ops, &context);
    if (ops && ops->resource_detach) {
        while (state->attachment_count != 0) {
            armos_drm_attachment_t *attachment =
                &state->attachments[state->attachment_count - 1u];

            (void)ops->resource_detach(context, attachment->context_id,
                                       attachment->handle);
            state->attachment_count--;
        }
    }
    if (ops && ops->context_destroy) {
        while (state->context_count != 0) {
            uint32_t context_id =
                state->contexts[state->context_count - 1u];

            (void)ops->context_destroy(context, context_id);
            state->context_count--;
        }
    }
    while (state->buffer_count != 0) {
        armos_drm_buffer_t *buffer =
            state->buffers[state->buffer_count - 1u];

        state->buffer_count--;
        drm_buffer_drop_owner(buffer);
    }
    while (state->fence_count != 0) {
        armos_drm_fence_t *fence =
            state->fences[state->fence_count - 1u];

        state->fence_count--;
        drm_fence_drop_owner(fence);
    }
    if (state->node == ARMOS_DRM_NODE_CARD) {
        spin_lock(&drm_lock);
        if (drm_card_owner == state)
            drm_card_owner = NULL;
        spin_unlock(&drm_lock);
    }
    file->private_data = NULL;
    kfree(state);
    return 0;
}

static int armos_drm_get_info(uintptr_t arg)
{
    const armos_drm_backend_ops_t *ops;
    void *context;
    armos_drm_backend_info_t backend_info;
    armos_drm_info_t info;
    int result;

    if (!arg)
        return -EFAULT;
    drm_backend_snapshot(&ops, &context);
    if (!ops || !ops->get_info)
        return -ENODEV;

    memset(&backend_info, 0, sizeof(backend_info));
    result = ops->get_info(context, &backend_info);
    if (result < 0)
        return result;

    memset(&info, 0, sizeof(info));
    info.abi_version = ARMOS_DRM_ABI_VERSION;
    info.struct_size = sizeof(info);
    info.backend_class = backend_info.backend_class;
    info.capabilities = backend_info.capabilities;
    if (info.capabilities & ARMOS_DRM_CAP_BUFFER_OBJECTS)
        info.capabilities |= ARMOS_DRM_CAP_SHARED_BUFFERS;
    if (info.capabilities & ARMOS_DRM_CAP_FENCES)
        info.capabilities |= ARMOS_DRM_CAP_SYNC_FDS;
    info.scanout_count = backend_info.scanout_count;
    info.scanout_width = backend_info.scanout_width;
    info.scanout_height = backend_info.scanout_height;
    info.max_resource_width = backend_info.max_resource_width;
    info.max_resource_height = backend_info.max_resource_height;
    info.command_caps_max_version =
        backend_info.command_caps_max_version;
    info.command_caps_size = backend_info.command_caps_size;
    if (backend_info.driver_name) {
        strncpy(info.driver_name, backend_info.driver_name,
                sizeof(info.driver_name) - 1u);
    }
    memcpy(info.command_set, backend_info.command_set,
           sizeof(info.command_set));
    return copy_to_user((void *)arg, &info, sizeof(info)) < 0 ?
        -EFAULT : 0;
}

static int armos_drm_get_command_caps(uintptr_t arg)
{
    const armos_drm_backend_ops_t *ops;
    armos_drm_backend_info_t info;
    armos_drm_command_caps_t request;
    void *backend_context;
    void *caps = NULL;
    int result;

    if (!arg || copy_from_user(&request, (void *)arg, sizeof(request)) < 0)
        return -EFAULT;
    if (request.abi_version != ARMOS_DRM_ABI_VERSION ||
        request.flags != 0 || request.address == 0)
        return -EINVAL;
    drm_backend_snapshot(&ops, &backend_context);
    if (!ops || !ops->get_info || !ops->get_command_caps)
        return -ENOTSUP;
    memset(&info, 0, sizeof(info));
    result = ops->get_info(backend_context, &info);
    if (result < 0)
        return result;
    if (info.command_caps_size == 0 ||
        info.command_caps_size > ARMOS_DRM_MAX_COMMAND_CAPS_SIZE ||
        request.version > info.command_caps_max_version)
        return -EINVAL;
    if (request.size < info.command_caps_size)
        return -ENOSPC;
    caps = kmalloc(info.command_caps_size);
    if (!caps)
        return -ENOMEM;
    memset(caps, 0, info.command_caps_size);
    result = ops->get_command_caps(backend_context, request.version,
                                   caps, info.command_caps_size);
    if (result < 0)
        goto out;
    if (copy_to_user((void *)(uintptr_t)request.address, caps,
                     info.command_caps_size) < 0) {
        result = -EFAULT;
        goto out;
    }
    request.size = info.command_caps_size;
    memset(request.reserved, 0, sizeof(request.reserved));
    result = copy_to_user((void *)arg, &request, sizeof(request)) < 0 ?
        -EFAULT : 0;
out:
    kfree(caps);
    return result;
}

static int armos_drm_context_create(file_t *file, uintptr_t arg)
{
    armos_drm_file_state_t *state = file->private_data;
    const armos_drm_backend_ops_t *ops;
    armos_drm_backend_info_t info;
    armos_drm_context_create_t request;
    void *context;
    uint32_t context_id;
    int result;

    if (!arg || copy_from_user(&request, (void *)arg, sizeof(request)) < 0)
        return -EFAULT;
    if (request.abi_version != ARMOS_DRM_ABI_VERSION ||
        request.flags != 0)
        return -EINVAL;
    result = drm_state_acquire(state);
    if (result < 0)
        return result;
    drm_backend_snapshot(&ops, &context);
    if (!ops || !ops->get_info || !ops->context_create) {
        result = -ENOTSUP;
        goto out;
    }
    memset(&info, 0, sizeof(info));
    result = ops->get_info(context, &info);
    if (result < 0)
        goto out;
    if (!(info.capabilities & ARMOS_DRM_CAP_CONTEXTS)) {
        result = -ENOTSUP;
        goto out;
    }
    if (state->context_count >= ARMOS_DRM_MAX_FILE_CONTEXTS) {
        result = -EMFILE;
        goto out;
    }

    spin_lock(&drm_lock);
    context_id = drm_next_context_id++;
    if (drm_next_context_id == 0)
        drm_next_context_id = 1u;
    spin_unlock(&drm_lock);
    result = ops->context_create(context, context_id);
    if (result < 0)
        goto out;

    state->contexts[state->context_count++] = context_id;
    request.context_id = context_id;
    memset(request.command_set, 0, sizeof(request.command_set));
    memcpy(request.command_set, info.command_set,
           sizeof(request.command_set));
    memset(request.reserved, 0, sizeof(request.reserved));
    if (copy_to_user((void *)arg, &request, sizeof(request)) < 0) {
        state->context_count--;
        (void)ops->context_destroy(context, context_id);
        result = -EFAULT;
        goto out;
    }
    result = 0;

out:
    drm_state_release(state);
    return result;
}

static int armos_drm_context_destroy(file_t *file, uintptr_t arg)
{
    armos_drm_file_state_t *state = file->private_data;
    const armos_drm_backend_ops_t *ops;
    armos_drm_context_destroy_t request;
    void *context;
    uint32_t index;
    int result;

    if (!arg || copy_from_user(&request, (void *)arg, sizeof(request)) < 0)
        return -EFAULT;
    if (request.flags != 0)
        return -EINVAL;
    result = drm_state_acquire(state);
    if (result < 0)
        return result;
    if (!drm_state_has_context(state, request.context_id, &index)) {
        result = -ENOENT;
        goto out;
    }
    drm_backend_snapshot(&ops, &context);
    if (!ops || !ops->context_destroy) {
        result = -ENOTSUP;
        goto out;
    }
    for (uint32_t attachment = 0;
         attachment < state->attachment_count;) {
        if (state->attachments[attachment].context_id != request.context_id) {
            attachment++;
            continue;
        }
        if (ops->resource_detach)
            (void)ops->resource_detach(
                context, request.context_id,
                state->attachments[attachment].handle);
        state->attachments[attachment] =
            state->attachments[state->attachment_count - 1u];
        state->attachment_count--;
    }
    result = ops->context_destroy(context, request.context_id);
    if (result < 0)
        goto out;
    state->contexts[index] = state->contexts[state->context_count - 1u];
    state->context_count--;

out:
    drm_state_release(state);
    return result;
}

static int armos_drm_bo_create(file_t *file, uintptr_t arg)
{
    armos_drm_file_state_t *state = file->private_data;
    const armos_drm_backend_ops_t *ops;
    armos_drm_bo_create_t request;
    armos_drm_backend_info_t info;
    armos_drm_buffer_t *buffer = NULL;
    armos_drm_memory_segment_t *segments = NULL;
    uint8_t command_descriptor[ARMOS_DRM_MAX_RESOURCE_DESCRIPTOR_SIZE];
    void *context;
    uint64_t rounded;
    int result;

    if (!arg || copy_from_user(&request, (void *)arg, sizeof(request)) < 0)
        return -EFAULT;
    if (request.abi_version != ARMOS_DRM_ABI_VERSION ||
        request.size == 0 || request.size > ARMOS_DRM_MAX_BUFFER_SIZE ||
        (request.flags & ~ARMOS_DRM_BO_VALID_FLAGS) != 0)
        return -EINVAL;
    if ((request.command_descriptor_size == 0) !=
        (request.command_descriptor_address == 0) ||
        request.command_descriptor_size >
            ARMOS_DRM_MAX_RESOURCE_DESCRIPTOR_SIZE ||
        request.reserved0 != 0)
        return -EINVAL;
    if (request.command_descriptor_size != 0 &&
        copy_from_user(command_descriptor,
                       (const void *)(uintptr_t)
                           request.command_descriptor_address,
                       request.command_descriptor_size) < 0)
        return -EFAULT;
    if ((request.width == 0) != (request.height == 0))
        return -EINVAL;
    if (request.width != 0) {
        uint64_t minimum_stride = (uint64_t)request.width * 4u;
        uint64_t minimum_size;

        if ((request.format != ARMOS_DRM_FORMAT_BGRA8888 &&
             request.format != ARMOS_DRM_FORMAT_RGBA8888) ||
            request.stride < minimum_stride)
            return -EINVAL;
        minimum_size = (uint64_t)request.stride * request.height;
        if (minimum_size > request.size)
            return -EINVAL;
    } else if (request.stride != 0 ||
               request.format != ARMOS_DRM_FORMAT_NONE) {
        return -EINVAL;
    }
    rounded = ALIGN_UP(request.size, PAGE_SIZE);
    if (rounded == 0 || rounded > ARMOS_DRM_MAX_BUFFER_SIZE)
        return -EOVERFLOW;

    result = drm_state_acquire(state);
    if (result < 0)
        return result;
    /*
     * Render clients may allocate exportable buffers whose layout is
     * compatible with scanout.  ARMOS_DRM_BO_SCANOUT describes that buffer
     * capability; it does not grant display ownership.  Actual presentation
     * remains restricted to the privileged card node in
     * armos_drm_bo_present().
     */
    if (state->buffer_count >= ARMOS_DRM_MAX_FILE_BUFFERS) {
        result = -EMFILE;
        goto out;
    }
    drm_backend_snapshot(&ops, &context);
    if (!ops || !ops->buffer_create || !ops->buffer_destroy) {
        result = -ENOTSUP;
        goto out;
    }
    memset(&info, 0, sizeof(info));
    if (ops->get_info) {
        result = ops->get_info(context, &info);
        if (result < 0)
            goto out;
        if (request.width != 0 &&
            ((info.max_resource_width &&
              request.width > info.max_resource_width) ||
             (info.max_resource_height &&
              request.height > info.max_resource_height))) {
            result = -E2BIG;
            goto out;
        }
    }
    buffer = kzalloc(sizeof(*buffer));
    if (!buffer) {
        result = -ENOMEM;
        goto out;
    }
    buffer->page_count = (uint32_t)(rounded / PAGE_SIZE);
    buffer->pages = kzalloc(buffer->page_count * sizeof(*buffer->pages));
    segments = kzalloc(buffer->page_count * sizeof(*segments));
    if (!buffer->pages || !segments) {
        result = -ENOMEM;
        goto failed;
    }
    init_spinlock_named(&buffer->lock, "armos_drm_bo");
    buffer->owner_count = 1u;
    result = drm_register_buffer(buffer);
    if (result < 0)
        goto failed;
    buffer->desc.size = request.size;
    buffer->desc.flags = request.flags;
    buffer->desc.width = request.width;
    buffer->desc.height = request.height;
    buffer->desc.stride = request.stride;
    buffer->desc.format = request.format;
    buffer->command_descriptor_size = request.command_descriptor_size;
    if (request.command_descriptor_size != 0)
        memcpy(buffer->command_descriptor, command_descriptor,
               request.command_descriptor_size);
    for (uint32_t index = 0; index < buffer->page_count; index++) {
        void *page = allocate_page();

        if (!page) {
            result = -ENOMEM;
            goto failed;
        }
        memset((void *)phys_to_virt((paddr_t)page), 0, PAGE_SIZE);
        buffer->pages[index] = (paddr_t)page;
        segments[index].address = (paddr_t)page;
        segments[index].length = index + 1u == buffer->page_count ?
            (uint32_t)(request.size -
                (uint64_t)index * PAGE_SIZE) : PAGE_SIZE;
    }
    result = ops->buffer_create(context, buffer->handle, &buffer->desc,
                                segments, buffer->page_count,
                                request.command_descriptor_size != 0 ?
                                    command_descriptor : NULL,
                                request.command_descriptor_size,
                                &buffer->command_handle);
    if (result < 0)
        goto failed;
    buffer->backend_created = true;
    if (buffer->command_handle == 0) {
        result = -EIO;
        goto failed;
    }
    state->buffers[state->buffer_count++] = buffer;
    request.handle = buffer->handle;
    request.command_handle = buffer->command_handle;
    request.map_offset = (uint64_t)buffer->handle * PAGE_SIZE;
    request.command_descriptor_address = 0;
    request.command_descriptor_size = 0;
    request.reserved0 = 0;
    if (copy_to_user((void *)arg, &request, sizeof(request)) < 0) {
        state->buffer_count--;
        result = -EFAULT;
        drm_buffer_drop_owner(buffer);
        kfree(segments);
        drm_state_release(state);
        return result;
    }
    kfree(segments);
    drm_state_release(state);
    return 0;

failed:
    if (buffer && buffer->backend_created && ops && ops->buffer_destroy) {
        (void)ops->buffer_destroy(context, buffer->handle);
        buffer->backend_created = false;
    }
    drm_unregister_buffer(buffer);
    kfree(segments);
    drm_buffer_free(buffer);
out:
    drm_state_release(state);
    return result;
}

static int armos_drm_bo_destroy(file_t *file, uintptr_t arg)
{
    armos_drm_file_state_t *state = file->private_data;
    armos_drm_bo_destroy_t request;
    armos_drm_buffer_t *buffer;
    uint32_t index;
    int result;

    if (!arg || copy_from_user(&request, (void *)arg, sizeof(request)) < 0)
        return -EFAULT;
    if (request.flags != 0)
        return -EINVAL;
    result = drm_state_acquire(state);
    if (result < 0)
        return result;
    buffer = drm_state_find_buffer(state, request.handle, &index);
    if (!buffer) {
        result = -ENOENT;
        goto out;
    }
    for (uint32_t i = 0; i < state->attachment_count; i++) {
        if (state->attachments[i].handle == request.handle) {
            result = -EBUSY;
            goto out;
        }
    }
    state->buffers[index] = state->buffers[state->buffer_count - 1u];
    state->buffer_count--;
    drm_buffer_drop_owner(buffer);
    result = 0;
out:
    drm_state_release(state);
    return result;
}

static int armos_drm_bo_map_info(file_t *file, uintptr_t arg)
{
    armos_drm_file_state_t *state = file->private_data;
    armos_drm_bo_map_t request;
    armos_drm_buffer_t *buffer;
    int result;

    if (!arg || copy_from_user(&request, (void *)arg, sizeof(request)) < 0)
        return -EFAULT;
    if (request.flags != 0)
        return -EINVAL;
    result = drm_state_acquire(state);
    if (result < 0)
        return result;
    buffer = drm_state_find_buffer(state, request.handle, NULL);
    if (!buffer) {
        result = -ENOENT;
        goto out;
    }
    request.map_offset = (uint64_t)buffer->handle * PAGE_SIZE;
    request.size = buffer->desc.size;
    memset(request.reserved, 0, sizeof(request.reserved));
    result = copy_to_user((void *)arg, &request, sizeof(request)) < 0 ?
        -EFAULT : 0;
out:
    drm_state_release(state);
    return result;
}

static int armos_drm_bo_set_metadata(file_t *file, uintptr_t arg)
{
    armos_drm_file_state_t *state = file->private_data;
    armos_drm_bo_set_metadata_t request;
    armos_drm_buffer_t *buffer;
    uint64_t minimum_stride;
    uint64_t minimum_size;
    int result;

    if (!arg || copy_from_user(&request, (void *)arg, sizeof(request)) < 0)
        return -EFAULT;
    if (request.abi_version != ARMOS_DRM_ABI_VERSION ||
        request.flags != 0 || request.width == 0 || request.height == 0 ||
        (request.format != ARMOS_DRM_FORMAT_BGRA8888 &&
         request.format != ARMOS_DRM_FORMAT_RGBA8888))
        return -EINVAL;
    for (uint32_t index = 0; index < 3u; index++) {
        if (request.reserved[index] != 0)
            return -EINVAL;
    }
    minimum_stride = (uint64_t)request.width * 4u;
    minimum_size = (uint64_t)request.stride * request.height;
    if (request.stride < minimum_stride || minimum_size < minimum_stride)
        return -EINVAL;

    result = drm_state_acquire(state);
    if (result < 0)
        return result;
    buffer = drm_state_find_buffer(state, request.handle, NULL);
    if (!buffer) {
        result = -ENOENT;
        goto out;
    }
    spin_lock(&buffer->lock);
    if (buffer->desc.width != 0 || buffer->desc.height != 0 ||
        buffer->desc.stride != 0 ||
        buffer->desc.format != ARMOS_DRM_FORMAT_NONE) {
        result = buffer->desc.width == request.width &&
                 buffer->desc.height == request.height &&
                 buffer->desc.stride == request.stride &&
                 buffer->desc.format == request.format ? 0 : -EBUSY;
    } else if (buffer->export_count != 0) {
        result = -EBUSY;
    } else if (minimum_size > buffer->desc.size) {
        result = -E2BIG;
    } else {
        buffer->desc.width = request.width;
        buffer->desc.height = request.height;
        buffer->desc.stride = request.stride;
        buffer->desc.format = request.format;
        result = 0;
    }
    spin_unlock(&buffer->lock);
out:
    drm_state_release(state);
    return result;
}

static int armos_drm_bo_export(file_t *file, uintptr_t arg)
{
    armos_drm_file_state_t *state = file->private_data;
    armos_drm_bo_export_t request;
    armos_drm_buffer_t *buffer;
    file_t *export_file = NULL;
    task_t *task = task_current_local();
    int descriptor = -1;
    int result;

    if (!arg || copy_from_user(&request, (void *)arg, sizeof(request)) < 0)
        return -EFAULT;
    if ((request.flags & ~ARMOS_DRM_SHARE_VALID_FLAGS) != 0)
        return -EINVAL;
    for (uint32_t index = 0; index < 5u; index++) {
        if (request.reserved[index] != 0)
            return -EINVAL;
    }
    if (!task || !task->process)
        return -ESRCH;

    result = drm_state_acquire(state);
    if (result < 0)
        return result;
    buffer = drm_state_find_buffer(state, request.handle, NULL);
    if (!buffer) {
        result = -ENOENT;
        goto out;
    }
    export_file = drm_buffer_create_export_file(buffer);
    if (!export_file) {
        result = -ENOMEM;
        goto out;
    }
    descriptor = vfs_install_file(
        task, export_file,
        (request.flags & ARMOS_DRM_SHARE_CLOEXEC) ? O_CLOEXEC : 0u);
    if (descriptor < 0) {
        close_file(export_file);
        export_file = NULL;
        result = descriptor;
        goto out;
    }
    request.fd = descriptor;
    memset(request.reserved, 0, sizeof(request.reserved));
    if (copy_to_user((void *)arg, &request, sizeof(request)) < 0) {
        export_file = vfs_take_file(task, descriptor);
        close_file(export_file);
        result = -EFAULT;
        goto out;
    }
    result = 0;
out:
    drm_state_release(state);
    return result;
}

static int armos_drm_bo_import(file_t *file, uintptr_t arg)
{
    armos_drm_file_state_t *state = file->private_data;
    armos_drm_bo_import_t request;
    armos_drm_buffer_t *buffer;
    file_t *export_file;
    task_t *task = task_current_local();
    int result;

    if (!arg || copy_from_user(&request, (void *)arg, sizeof(request)) < 0)
        return -EFAULT;
    if (request.abi_version != ARMOS_DRM_ABI_VERSION ||
        request.flags != 0 || request.bo_flags != 0)
        return -EINVAL;
    for (uint32_t index = 0; index < 4u; index++) {
        if (request.reserved[index] != 0)
            return -EINVAL;
    }
    if (!task || !task->process)
        return -ESRCH;
    export_file = vfs_get_file_from_fd(task, request.fd);
    if (!export_file)
        return -EBADF;
    if (export_file->type != FILE_TYPE_DRM_BO ||
        !export_file->private_data) {
        close_file(export_file);
        return -EINVAL;
    }
    buffer = export_file->private_data;

    result = drm_state_acquire(state);
    if (result < 0) {
        close_file(export_file);
        return result;
    }
    if (state->buffer_count >= ARMOS_DRM_MAX_FILE_BUFFERS) {
        result = -EMFILE;
        goto out;
    }
    if (drm_state_find_buffer(state, buffer->handle, NULL)) {
        result = -EEXIST;
        goto out;
    }
    result = drm_buffer_add_owner(buffer);
    if (result < 0)
        goto out;
    state->buffers[state->buffer_count++] = buffer;

    request.handle = buffer->handle;
    request.command_handle = buffer->command_handle;
    request.width = buffer->desc.width;
    request.height = buffer->desc.height;
    request.stride = buffer->desc.stride;
    request.format = buffer->desc.format;
    request.bo_flags = buffer->desc.flags;
    request.size = buffer->desc.size;
    request.map_offset = (uint64_t)buffer->handle * PAGE_SIZE;
    memset(request.reserved, 0, sizeof(request.reserved));
    if (copy_to_user((void *)arg, &request, sizeof(request)) < 0) {
        state->buffer_count--;
        drm_buffer_drop_owner(buffer);
        result = -EFAULT;
        goto out;
    }
    result = 0;
out:
    drm_state_release(state);
    close_file(export_file);
    return result;
}

static int armos_drm_resource_change(file_t *file, uintptr_t arg, bool attach)
{
    armos_drm_file_state_t *state = file->private_data;
    const armos_drm_backend_ops_t *ops;
    armos_drm_resource_attachment_t request;
    void *context;
    int attachment;
    int result;

    if (!arg || copy_from_user(&request, (void *)arg, sizeof(request)) < 0)
        return -EFAULT;
    if (request.flags != 0)
        return -EINVAL;
    result = drm_state_acquire(state);
    if (result < 0)
        return result;
    if (!drm_state_has_context(state, request.context_id, NULL) ||
        !drm_state_find_buffer(state, request.handle, NULL)) {
        result = -ENOENT;
        goto out;
    }
    attachment = drm_find_attachment(state, request.context_id,
                                     request.handle);
    drm_backend_snapshot(&ops, &context);
    if (!ops || !ops->resource_attach || !ops->resource_detach) {
        result = -ENOTSUP;
        goto out;
    }
    if (attach) {
        if (attachment >= 0) {
            result = -EEXIST;
            goto out;
        }
        if (state->attachment_count >= ARMOS_DRM_MAX_ATTACHMENTS) {
            result = -ENOSPC;
            goto out;
        }
        result = ops->resource_attach(context, request.context_id,
                                      request.handle);
        if (result < 0)
            goto out;
        state->attachments[state->attachment_count++] =
            (armos_drm_attachment_t){
                .context_id = request.context_id,
                .handle = request.handle,
            };
    } else {
        if (attachment < 0) {
            result = -ENOENT;
            goto out;
        }
        result = ops->resource_detach(context, request.context_id,
                                      request.handle);
        if (result < 0)
            goto out;
        state->attachments[attachment] =
            state->attachments[state->attachment_count - 1u];
        state->attachment_count--;
    }
    result = 0;
out:
    drm_state_release(state);
    return result;
}

static void drm_buffer_clean_rect(const armos_drm_buffer_t *buffer,
                                  uint32_t x, uint32_t y,
                                  uint32_t width, uint32_t height)
{
    const uint64_t row_size = (uint64_t)width * sizeof(uint32_t);

    for (uint32_t row = 0; row < height; row++) {
        uint64_t offset = (uint64_t)(y + row) * buffer->desc.stride +
                          (uint64_t)x * sizeof(uint32_t);
        uint64_t remaining = row_size;

        while (remaining != 0) {
            uint32_t page_index = (uint32_t)(offset / PAGE_SIZE);
            uint32_t page_offset = (uint32_t)(offset % PAGE_SIZE);
            size_t chunk = PAGE_SIZE - page_offset;
            void *address;

            if ((uint64_t)chunk > remaining)
                chunk = (size_t)remaining;
            address = (uint8_t *)phys_to_virt(buffer->pages[page_index]) +
                      page_offset;
            arch_clean_dcache_by_mva(address, chunk);
            offset += chunk;
            remaining -= chunk;
        }
    }
}

static void drm_buffer_cache_all(const armos_drm_buffer_t *buffer,
                                 bool invalidate)
{
    uint64_t remaining;

    if (!buffer)
        return;
    remaining = buffer->desc.size;
    for (uint32_t index = 0;
         index < buffer->page_count && remaining != 0; index++) {
        size_t length = remaining > PAGE_SIZE ? PAGE_SIZE : (size_t)remaining;
        void *address = (void *)phys_to_virt(buffer->pages[index]);

        if (invalidate)
            arch_invalidate_dcache_by_mva(address, length);
        else
            arch_clean_dcache_by_mva(address, length);
        remaining -= length;
    }
}

static int armos_drm_bo_transfer(file_t *file, uintptr_t arg)
{
    armos_drm_file_state_t *state = file->private_data;
    const armos_drm_backend_ops_t *ops;
    armos_drm_bo_transfer_t request;
    armos_drm_buffer_t *buffer;
    void *context;
    uint64_t last_byte;
    int result;

    if (!arg || copy_from_user(&request, (void *)arg, sizeof(request)) < 0)
        return -EFAULT;
    if (request.flags != 0 ||
        (request.direction != ARMOS_DRM_TRANSFER_CPU_TO_DEVICE &&
         request.direction != ARMOS_DRM_TRANSFER_DEVICE_TO_CPU) ||
        request.context_id == 0 || request.width == 0 ||
        request.height == 0 || request.depth == 0)
        return -EINVAL;

    result = drm_state_acquire(state);
    if (result < 0)
        return result;
    buffer = drm_state_find_buffer(state, request.handle, NULL);
    if (!buffer ||
        !drm_state_has_context(state, request.context_id, NULL) ||
        drm_find_attachment(state, request.context_id,
                            request.handle) < 0) {
        result = -ENOENT;
        goto out;
    }
    if (request.direction == ARMOS_DRM_TRANSFER_CPU_TO_DEVICE &&
        !(buffer->desc.flags & ARMOS_DRM_BO_CPU_WRITE)) {
        result = -EACCES;
        goto out;
    }
    if (request.direction == ARMOS_DRM_TRANSFER_DEVICE_TO_CPU &&
        !(buffer->desc.flags & ARMOS_DRM_BO_CPU_READ)) {
        result = -EACCES;
        goto out;
    }
    if (buffer->command_descriptor_size != 0) {
        last_byte = request.offset;
        if (request.height > 1u) {
            uint64_t rows = request.height - 1u;
            uint64_t row_offset = rows * (uint64_t)request.stride;

            if (request.stride == 0 ||
                row_offset > ~0ULL - last_byte) {
                result = -EINVAL;
                goto out;
            }
            last_byte += row_offset;
        }
        if (request.depth > 1u) {
            uint64_t layers = request.depth - 1u;
            uint64_t layer_offset =
                layers * (uint64_t)request.layer_stride;

            if (request.layer_stride == 0 ||
                layer_offset > ~0ULL - last_byte) {
                result = -EINVAL;
                goto out;
            }
            last_byte += layer_offset;
        }
        if (last_byte < request.offset || last_byte >= buffer->desc.size) {
            result = -EINVAL;
            goto out;
        }
    } else if (buffer->desc.width != 0) {
        if (request.level != 0 || request.z != 0 || request.depth != 1 ||
            request.x >= buffer->desc.width ||
            request.y >= buffer->desc.height ||
            request.width > buffer->desc.width - request.x ||
            request.height > buffer->desc.height - request.y ||
            request.stride < buffer->desc.stride ||
            (uint64_t)request.layer_stride <
                (uint64_t)request.stride * request.height) {
            result = -EINVAL;
            goto out;
        }
        last_byte = request.offset +
                    (uint64_t)(request.height - 1u) * request.stride +
                    (uint64_t)request.width * sizeof(uint32_t);
    } else {
        if (request.level != 0 || request.y != 0 || request.z != 0 ||
            request.height != 1 || request.depth != 1 ||
            request.stride != 0 || request.layer_stride != 0) {
            result = -EINVAL;
            goto out;
        }
        last_byte = request.offset + request.width;
    }
    if (last_byte < request.offset || last_byte > buffer->desc.size) {
        result = -EINVAL;
        goto out;
    }
    drm_backend_snapshot(&ops, &context);
    if (!ops || !ops->buffer_transfer) {
        result = -ENOTSUP;
        goto out;
    }
    if (request.direction == ARMOS_DRM_TRANSFER_CPU_TO_DEVICE)
        drm_buffer_cache_all(buffer, false);
    result = ops->buffer_transfer(
        context, request.context_id, buffer->handle, request.direction,
        request.level, request.x, request.y, request.z, request.width,
        request.height, request.depth, request.offset, request.stride,
        request.layer_stride,
        buffer->command_descriptor_size != 0 ?
            buffer->command_descriptor : NULL,
        buffer->command_descriptor_size);
    if (result == 0 &&
        request.direction == ARMOS_DRM_TRANSFER_DEVICE_TO_CPU)
        drm_buffer_cache_all(buffer, true);
out:
    drm_state_release(state);
    return result;
}

static int armos_drm_bo_present(file_t *file, uintptr_t arg)
{
    armos_drm_file_state_t *state = file->private_data;
    const armos_drm_backend_ops_t *ops;
    armos_drm_bo_present_t request;
    armos_drm_buffer_t *buffer;
    void *context;
    int result;

    if (!arg || copy_from_user(&request, (void *)arg, sizeof(request)) < 0)
        return -EFAULT;
    if (request.flags != 0 || request.width == 0 || request.height == 0)
        return -EINVAL;
    result = drm_state_acquire(state);
    if (result < 0)
        return result;
    if (state->node != ARMOS_DRM_NODE_CARD) {
        result = -EACCES;
        goto out;
    }
    buffer = drm_state_find_buffer(state, request.handle, NULL);
    if (!buffer) {
        result = -ENOENT;
        goto out;
    }
    if (!(buffer->desc.flags & ARMOS_DRM_BO_SCANOUT) ||
        buffer->desc.width == 0 || buffer->desc.height == 0 ||
        request.x >= buffer->desc.width ||
        request.y >= buffer->desc.height ||
        request.width > buffer->desc.width - request.x ||
        request.height > buffer->desc.height - request.y) {
        result = -EINVAL;
        goto out;
    }
    drm_backend_snapshot(&ops, &context);
    if (!ops || !ops->buffer_present) {
        result = -ENOTSUP;
        goto out;
    }
    drm_buffer_clean_rect(buffer, request.x, request.y,
                          request.width, request.height);
    result = ops->buffer_present(context, buffer->handle, &buffer->desc,
                                 request.scanout_id, request.x, request.y,
                                 request.width, request.height);
out:
    drm_state_release(state);
    return result;
}

static int armos_drm_submit(file_t *file, uintptr_t arg)
{
    armos_drm_file_state_t *state = file->private_data;
    const armos_drm_backend_ops_t *ops;
    armos_drm_submit_t request;
    void *context;
    void *commands;
    uint64_t fence_id;
    armos_drm_fence_t *fence = NULL;
    int fence_slot = -1;
    int result;

    if (!arg || copy_from_user(&request, (void *)arg, sizeof(request)) < 0)
        return -EFAULT;
    if (request.flags != 0 || request.command_address == 0 ||
        request.command_size == 0 ||
        request.command_size > ARMOS_DRM_MAX_COMMAND_SIZE)
        return -EINVAL;
    commands = kmalloc(request.command_size);
    if (!commands)
        return -ENOMEM;
    if (copy_from_user(commands, (void *)(uintptr_t)request.command_address,
                       request.command_size) < 0) {
        kfree(commands);
        return -EFAULT;
    }

    result = drm_state_acquire(state);
    if (result < 0) {
        kfree(commands);
        return result;
    }
    if (!drm_state_has_context(state, request.context_id, NULL)) {
        result = -ENOENT;
        goto out;
    }
    drm_backend_snapshot(&ops, &context);
    if (!ops || !ops->submit) {
        result = -ENOTSUP;
        goto out;
    }
    if (state->fence_count >= ARMOS_DRM_MAX_FENCES) {
        result = -ENOSPC;
        goto out;
    }
    fence = kzalloc(sizeof(*fence));
    if (!fence) {
        result = -ENOMEM;
        goto out;
    }
    fence->owner_count = 1u;
    spin_lock(&drm_lock);
    fence_id = drm_next_fence_id++;
    if (drm_next_fence_id == 0)
        drm_next_fence_id = 1u;
    for (uint32_t index = 0; index < ARMOS_DRM_MAX_FENCES; index++) {
        if (!drm_fences[index]) {
            fence_slot = (int)index;
            break;
        }
    }
    if (fence_slot >= 0) {
        fence->id = fence_id;
        drm_fences[fence_slot] = fence;
    }
    spin_unlock(&drm_lock);
    if (fence_slot < 0) {
        kfree(fence);
        fence = NULL;
        result = -ENOSPC;
        goto out;
    }
    state->fences[state->fence_count++] = fence;
    result = ops->submit(context, request.context_id, commands,
                         request.command_size, fence_id);
    if (result < 0) {
        state->fence_count--;
        drm_fence_drop_owner(fence);
        fence = NULL;
        goto out;
    }
    request.fence_id = fence_id;
    if (copy_to_user((void *)arg, &request, sizeof(request)) < 0) {
        state->fence_count--;
        drm_fence_drop_owner(fence);
        fence = NULL;
        result = -EFAULT;
    }

out:
    drm_state_release(state);
    kfree(commands);
    return result;
}

static int armos_drm_fence_wait(file_t *file, uintptr_t arg)
{
    armos_drm_file_state_t *state = file->private_data;
    armos_drm_fence_wait_t request;
    armos_drm_fence_t *fence = NULL;
    uint32_t deadline = 0;
    int result;

    if (!arg || copy_from_user(&request, (void *)arg, sizeof(request)) < 0)
        return -EFAULT;
    if (request.flags != 0 || request.timeout_ns < -1)
        return -EINVAL;
    if (request.timeout_ns > 0) {
        uint64_t timeout_ms = drm_divide_u64_u32(
            (uint64_t)request.timeout_ns + 999999u, 1000000u);
        uint32_t now = get_system_ticks();

        if (timeout_ms > 0x7fffffffu)
            timeout_ms = 0x7fffffffu;
        deadline = now + (uint32_t)timeout_ms;
        if (deadline == 0)
            deadline = 1u;
    }
    while (1) {
        uint32_t generation = task_poll_generation();

        result = drm_state_acquire(state);
        if (result < 0)
            return result;
        for (uint32_t index = 0; index < state->fence_count; index++) {
            if (state->fences[index]->id == request.fence_id) {
                fence = state->fences[index];
                break;
            }
        }
        if (!fence) {
            drm_state_release(state);
            return -ENOENT;
        }
        spin_lock(&drm_lock);
        if (fence->completed)
            result = fence->status;
        else
            result = -EAGAIN;
        spin_unlock(&drm_lock);
        drm_state_release(state);
        if (result != -EAGAIN || request.timeout_ns == 0)
            return result;
        if (deadline && (int32_t)(get_system_ticks() - deadline) >= 0)
            return -ETIMEDOUT;
        result = task_poll_wait(task_current_local(), generation, deadline);
        if (result < 0)
            return result;
    }
}

static int armos_drm_fence_destroy(file_t *file, uintptr_t arg)
{
    armos_drm_file_state_t *state = file->private_data;
    armos_drm_fence_destroy_t request;
    armos_drm_fence_t *fence = NULL;
    int result;

    if (!arg || copy_from_user(&request, (void *)arg, sizeof(request)) < 0)
        return -EFAULT;
    if (request.flags != 0)
        return -EINVAL;
    result = drm_state_acquire(state);
    if (result < 0)
        return result;
    for (uint32_t index = 0; index < state->fence_count; index++) {
        if (state->fences[index]->id != request.fence_id)
            continue;
        fence = state->fences[index];
        state->fences[index] = state->fences[state->fence_count - 1u];
        state->fence_count--;
        break;
    }
    drm_state_release(state);
    if (!fence)
        return -ENOENT;
    drm_fence_drop_owner(fence);
    return 0;
}

static int armos_drm_fence_export(file_t *file, uintptr_t arg)
{
    armos_drm_file_state_t *state = file->private_data;
    armos_drm_fence_export_t request;
    armos_drm_fence_t *fence = NULL;
    file_t *export_file = NULL;
    task_t *task = task_current_local();
    int descriptor = -1;
    int result;

    if (!arg || copy_from_user(&request, (void *)arg, sizeof(request)) < 0)
        return -EFAULT;
    if ((request.flags & ~ARMOS_DRM_SHARE_VALID_FLAGS) != 0)
        return -EINVAL;
    for (uint32_t index = 0; index < 4u; index++) {
        if (request.reserved[index] != 0)
            return -EINVAL;
    }
    if (!task || !task->process)
        return -ESRCH;

    result = drm_state_acquire(state);
    if (result < 0)
        return result;
    for (uint32_t index = 0; index < state->fence_count; index++) {
        if (state->fences[index]->id == request.fence_id) {
            fence = state->fences[index];
            break;
        }
    }
    if (!fence) {
        result = -ENOENT;
        goto out;
    }
    export_file = drm_fence_create_export_file(fence);
    if (!export_file) {
        result = -ENOMEM;
        goto out;
    }
    descriptor = vfs_install_file(
        task, export_file,
        (request.flags & ARMOS_DRM_SHARE_CLOEXEC) ? O_CLOEXEC : 0u);
    if (descriptor < 0) {
        close_file(export_file);
        result = descriptor;
        goto out;
    }
    request.fd = descriptor;
    memset(request.reserved, 0, sizeof(request.reserved));
    if (copy_to_user((void *)arg, &request, sizeof(request)) < 0) {
        export_file = vfs_take_file(task, descriptor);
        close_file(export_file);
        result = -EFAULT;
        goto out;
    }
    result = 0;
out:
    drm_state_release(state);
    return result;
}

void *armos_drm_map_fd(int fd, void *hint, size_t length,
                       uint32_t vma_flags, uint64_t offset)
{
    task_t *task = task_current_local();
    file_t *file;
    armos_drm_file_state_t *state;
    armos_drm_buffer_t *buffer;
    vm_space_t *vm;
    vma_t *vma;
    vaddr_t address;
    uint32_t handle;
    size_t size;
    int result;

    if (!task || !task->process || !task->process->vm ||
        offset == 0 || (offset & (PAGE_SIZE - 1u)) != 0)
        return (void *)-EINVAL;
    handle = (uint32_t)(offset / PAGE_SIZE);
    if ((uint64_t)handle * PAGE_SIZE != offset)
        return (void *)-EINVAL;
    file = vfs_get_file_from_fd(task, fd);
    if (!file || file->type != FILE_TYPE_DRM) {
        close_file(file);
        return (void *)-EBADF;
    }
    state = file->private_data;
    result = drm_state_acquire(state);
    if (result < 0) {
        close_file(file);
        return (void *)(intptr_t)result;
    }
    buffer = drm_state_find_buffer(state, handle, NULL);
    if (!buffer ||
        (!(buffer->desc.flags & ARMOS_DRM_BO_CPU_READ) &&
         (vma_flags & VMA_READ)) ||
        (!(buffer->desc.flags & ARMOS_DRM_BO_CPU_WRITE) &&
         (vma_flags & VMA_WRITE))) {
        result = buffer ? -EACCES : -ENOENT;
        goto out;
    }
    size = ALIGN_UP(length, PAGE_SIZE);
    if (length == 0 || size !=
        ALIGN_UP((size_t)buffer->desc.size, PAGE_SIZE)) {
        result = -EINVAL;
        goto out;
    }
    vm = task->process->vm;
    address = vm_find_free_range(vm, (vaddr_t)hint, size,
                                 USER_MMAP_START, USER_MMAP_END);
    if (!address) {
        result = -ENOMEM;
        goto out;
    }
    vma = create_vma(vm, address, size, vma_flags);
    if (!vma) {
        result = -ENOMEM;
        goto out;
    }
    for (uint32_t index = 0; index < buffer->page_count; index++) {
        result = map_user_page(vm->pgdir, address + index * PAGE_SIZE,
                               buffer->pages[index], vma->flags, vm->asid);
        if (result < 0) {
            (void)vm_unmap_range(vm, address, size);
            goto out;
        }
    }
    spin_lock(&buffer->lock);
    buffer->mapping_count++;
    spin_unlock(&buffer->lock);
    vm_set_vma_release(vma, drm_buffer_put_mapping, (uintptr_t)buffer);
    drm_state_release(state);
    close_file(file);
    return (void *)address;
out:
    drm_state_release(state);
    close_file(file);
    return (void *)(intptr_t)result;
}

int armos_drm_device_ioctl(file_t *file, uint32_t request, uintptr_t arg)
{
    if (!file || file->type != FILE_TYPE_DRM)
        return -ENOTTY;
    switch (request) {
    case ARMOS_DRM_IOCTL_GET_INFO:
        return armos_drm_get_info(arg);
    case ARMOS_DRM_IOCTL_CONTEXT_CREATE:
        return armos_drm_context_create(file, arg);
    case ARMOS_DRM_IOCTL_CONTEXT_DESTROY:
        return armos_drm_context_destroy(file, arg);
    case ARMOS_DRM_IOCTL_SUBMIT:
        return armos_drm_submit(file, arg);
    case ARMOS_DRM_IOCTL_FENCE_WAIT:
        return armos_drm_fence_wait(file, arg);
    case ARMOS_DRM_IOCTL_FENCE_DESTROY:
        return armos_drm_fence_destroy(file, arg);
    case ARMOS_DRM_IOCTL_BO_CREATE:
        return armos_drm_bo_create(file, arg);
    case ARMOS_DRM_IOCTL_BO_DESTROY:
        return armos_drm_bo_destroy(file, arg);
    case ARMOS_DRM_IOCTL_BO_MAP:
        return armos_drm_bo_map_info(file, arg);
    case ARMOS_DRM_IOCTL_RESOURCE_ATTACH:
        return armos_drm_resource_change(file, arg, true);
    case ARMOS_DRM_IOCTL_RESOURCE_DETACH:
        return armos_drm_resource_change(file, arg, false);
    case ARMOS_DRM_IOCTL_BO_PRESENT:
        return armos_drm_bo_present(file, arg);
    case ARMOS_DRM_IOCTL_GET_COMMAND_CAPS:
        return armos_drm_get_command_caps(arg);
    case ARMOS_DRM_IOCTL_BO_TRANSFER:
        return armos_drm_bo_transfer(file, arg);
    case ARMOS_DRM_IOCTL_BO_EXPORT:
        return armos_drm_bo_export(file, arg);
    case ARMOS_DRM_IOCTL_BO_IMPORT:
        return armos_drm_bo_import(file, arg);
    case ARMOS_DRM_IOCTL_FENCE_EXPORT:
        return armos_drm_fence_export(file, arg);
    case ARMOS_DRM_IOCTL_BO_SET_METADATA:
        return armos_drm_bo_set_metadata(file, arg);
    default:
        return -ENOTTY;
    }
}
