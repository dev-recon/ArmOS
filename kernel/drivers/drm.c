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
#include <kernel/file.h>
#include <kernel/drm.h>
#include <kernel/memory.h>
#include <kernel/spinlock.h>
#include <kernel/string.h>
#include <kernel/syscalls.h>
#include <kernel/timer.h>
#include <uapi/armos/drm.h>

#define DEV_DRI_CARD0_RDEV      0xE200u
#define DEV_DRI_RENDER128_RDEV  0xE280u
#define ARMOS_DRM_MAX_FILE_CONTEXTS 32u

typedef struct armos_drm_file_state {
    armos_drm_node_t node;
    uint32_t contexts[ARMOS_DRM_MAX_FILE_CONTEXTS];
    uint32_t context_count;
    uint64_t last_completed_fence;
    spinlock_t lock;
    bool busy;
    bool closing;
    task_t *owner;
} armos_drm_file_state_t;

static spinlock_t drm_lock = SPINLOCK_INIT("armos_drm");
static const armos_drm_backend_ops_t *drm_ops;
static void *drm_context;
static uint32_t drm_next_context_id = 1u;
static uint64_t drm_next_fence_id = 1u;

static int armos_drm_file_close(file_t *file);

static file_operations_t drm_file_ops = {
    .read = NULL,
    .write = NULL,
    .open = NULL,
    .close = armos_drm_file_close,
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

file_t *create_armos_drm_device_file(const char *name, int flags,
                                     armos_drm_node_t node)
{
    file_t *file;
    inode_t *inode;
    armos_drm_file_state_t *state;
    struct stat st;

    if (!armos_drm_device_available() || node == ARMOS_DRM_NODE_INVALID)
        return NULL;
    file = create_file();
    if (!file)
        return NULL;
    inode = create_inode();
    if (!inode) {
        kfree(file);
        return NULL;
    }
    state = kmalloc(sizeof(*state));
    if (!state) {
        put_inode(inode);
        kfree(file);
        return NULL;
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
    return file;
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
    if (ops && ops->context_destroy) {
        while (state->context_count != 0) {
            uint32_t context_id =
                state->contexts[state->context_count - 1u];

            (void)ops->context_destroy(context, context_id);
            state->context_count--;
        }
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
    info.scanout_count = backend_info.scanout_count;
    info.scanout_width = backend_info.scanout_width;
    info.scanout_height = backend_info.scanout_height;
    info.max_resource_width = backend_info.max_resource_width;
    info.max_resource_height = backend_info.max_resource_height;
    if (backend_info.driver_name) {
        strncpy(info.driver_name, backend_info.driver_name,
                sizeof(info.driver_name) - 1u);
    }
    memcpy(info.command_set, backend_info.command_set,
           sizeof(info.command_set));
    return copy_to_user((void *)arg, &info, sizeof(info)) < 0 ?
        -EFAULT : 0;
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
    result = ops->context_destroy(context, request.context_id);
    if (result < 0)
        goto out;
    state->contexts[index] = state->contexts[state->context_count - 1u];
    state->context_count--;

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
    spin_lock(&drm_lock);
    fence_id = drm_next_fence_id++;
    if (drm_next_fence_id == 0)
        drm_next_fence_id = 1u;
    spin_unlock(&drm_lock);
    result = ops->submit(context, request.context_id, commands,
                         request.command_size, fence_id);
    if (result < 0)
        goto out;
    state->last_completed_fence = fence_id;
    request.fence_id = fence_id;
    if (copy_to_user((void *)arg, &request, sizeof(request)) < 0)
        result = -EFAULT;

out:
    drm_state_release(state);
    kfree(commands);
    return result;
}

static int armos_drm_fence_wait(file_t *file, uintptr_t arg)
{
    armos_drm_file_state_t *state = file->private_data;
    armos_drm_fence_wait_t request;
    int result;

    if (!arg || copy_from_user(&request, (void *)arg, sizeof(request)) < 0)
        return -EFAULT;
    if (request.flags != 0 || request.timeout_ns < -1)
        return -EINVAL;
    result = drm_state_acquire(state);
    if (result < 0)
        return result;
    result = request.fence_id != 0 &&
             request.fence_id <= state->last_completed_fence ?
        0 : (request.timeout_ns == 0 ? -EAGAIN : -ETIMEDOUT);
    drm_state_release(state);
    return result;
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
    default:
        return -ENOTTY;
    }
}
