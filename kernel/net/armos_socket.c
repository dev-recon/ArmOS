/*
 * ArmOS
 * Copyright (c) 2026 Mohamed Ennassiri
 *
 * Licensed under the Apache License, Version 2.0.
 * See LICENSE for details.
 *
 * File: kernel/net/armos_socket.c
 * Layer: Kernel / local inter-process communication
 *
 * Responsibilities:
 * - Implement local stream sockets, socket pairs and named endpoints.
 * - Provide architecture-neutral descriptor, blocking and poll semantics.
 *
 * Notes:
 * - POSIX address-family names are accepted only at the public syscall ABI.
 * - Storage, scheduling and synchronization use common kernel facilities.
 */

#include <kernel/file.h>
#include <kernel/memory.h>
#include <kernel/net/armos_socket.h>
#include <kernel/spinlock.h>
#include <kernel/string.h>
#include <kernel/syscalls.h>
#include <kernel/task.h>
#include <kernel/timer.h>
#include <kernel/userspace.h>
#include <kernel/vfs.h>

#define ARMOS_SOCKET_PATH_MAX       108u
#define ARMOS_SOCKET_BUFFER_SIZE    16384u
#define ARMOS_SOCKET_BACKLOG_MAX    16u
#define ARMOS_SOCKET_IOV_MAX        64u
#define ARMOS_SOCKET_MESSAGE_MAX    (1024u * 1024u)
#define ARMOS_SOCKET_RIGHTS_MAX     16u
#define ARMOS_SOCKET_CONTROL_MAX    64u

#define ARMOS_SOL_SOCKET            1
#define ARMOS_SCM_RIGHTS            1
#define ARMOS_MSG_CTRUNC            0x0008
#define ARMOS_MSG_CMSG_CLOEXEC      0x40000000

typedef struct armos_socket_address {
    uint16_t family;
    char path[ARMOS_SOCKET_PATH_MAX];
} armos_socket_address_t;

typedef enum armos_socket_state {
    ARMOS_SOCKET_CREATED = 0,
    ARMOS_SOCKET_BOUND,
    ARMOS_SOCKET_LISTENING,
    ARMOS_SOCKET_CONNECTED,
    ARMOS_SOCKET_CLOSED,
} armos_socket_state_t;

typedef struct armos_socket_control {
    struct armos_socket_control *next;
    uint64_t stream_offset;
    uint32_t file_count;
    file_t *files[ARMOS_SOCKET_RIGHTS_MAX];
} armos_socket_control_t;

typedef struct armos_socket_channel {
    uint8_t *data[2];
    uint32_t read_pos[2];
    uint32_t write_pos[2];
    uint32_t count[2];
    uint64_t read_sequence[2];
    uint64_t write_sequence[2];
    armos_socket_control_t *control_head[2];
    armos_socket_control_t *control_tail[2];
    uint32_t control_count[2];
    uint8_t read_closed[2];
    uint8_t write_closed[2];
    uint8_t refs;
    struct armos_socket *endpoint[2];
} armos_socket_channel_t;

typedef struct armos_socket armos_socket_t;

struct armos_socket {
    armos_socket_t *bound_next;
    armos_socket_t *accept_next;
    armos_socket_t *accept_head;
    armos_socket_t *accept_tail;
    armos_socket_channel_t *channel;
    armos_socket_state_t state;
    uint8_t type;
    uint8_t side;
    uint8_t backlog;
    uint8_t pending;
    char path[ARMOS_SOCKET_PATH_MAX];
};

static spinlock_t armos_socket_lock = SPINLOCK_INIT("armos_socket");
static armos_socket_t *armos_bound_list;

extern file_t *create_file(void);
extern inode_t *create_inode(void);

static void armos_socket_channel_notify(armos_socket_channel_t *channel)
{
    if (!channel)
        return;
    if (channel->endpoint[0])
        task_poll_notify_key(channel->endpoint[0]);
    if (channel->endpoint[1])
        task_poll_notify_key(channel->endpoint[1]);
}

static int armos_socket_wait(armos_socket_t *socket, uint32_t generation)
{
    const void *key = socket;

    return task_poll_wait(task_current_local(), generation, 0u, &key, 1u);
}

static armos_socket_t *armos_socket_from_fd(int fd)
{
    task_t *task = task_current_local();
    file_t *file;

    if (!task || !task->process || fd < 0 || fd >= MAX_FILES)
        return NULL;
    file = task->process->files[fd];
    if (!file || file->type != FILE_TYPE_ARMOS_SOCKET)
        return NULL;
    return file->private_data;
}

bool armos_socket_is_fd(int fd)
{
    return armos_socket_from_fd(fd) != NULL;
}

static armos_socket_channel_t *armos_socket_channel_create(void)
{
    armos_socket_channel_t *channel = kzalloc(sizeof(*channel));

    if (!channel)
        return NULL;
    channel->data[0] = kmalloc(ARMOS_SOCKET_BUFFER_SIZE);
    channel->data[1] = kmalloc(ARMOS_SOCKET_BUFFER_SIZE);
    if (!channel->data[0] || !channel->data[1]) {
        kfree(channel->data[0]);
        kfree(channel->data[1]);
        kfree(channel);
        return NULL;
    }
    channel->refs = 2u;
    return channel;
}

static void armos_socket_control_free(armos_socket_control_t *control)
{
    while (control) {
        armos_socket_control_t *next = control->next;

        for (uint32_t index = 0u; index < control->file_count; index++)
            close_file(control->files[index]);
        kfree(control);
        control = next;
    }
}

static void armos_socket_channel_free(armos_socket_channel_t *channel)
{
    if (!channel)
        return;
    armos_socket_control_free(channel->control_head[0]);
    armos_socket_control_free(channel->control_head[1]);
    kfree(channel->data[0]);
    kfree(channel->data[1]);
    kfree(channel);
}

static armos_socket_t *armos_socket_allocate(void)
{
    armos_socket_t *socket = kzalloc(sizeof(*socket));

    if (!socket)
        return NULL;
    socket->type = ARMOS_SOCK_STREAM;
    socket->state = ARMOS_SOCKET_CREATED;
    return socket;
}

static void armos_bound_remove_locked(armos_socket_t *socket)
{
    armos_socket_t **link = &armos_bound_list;

    while (*link) {
        if (*link == socket) {
            *link = socket->bound_next;
            socket->bound_next = NULL;
            return;
        }
        link = &(*link)->bound_next;
    }
}

static armos_socket_t *armos_bound_find_locked(const char *path)
{
    armos_socket_t *socket;

    for (socket = armos_bound_list; socket; socket = socket->bound_next) {
        if (strcmp(socket->path, path) == 0)
            return socket;
    }
    return NULL;
}

static void armos_socket_destroy(armos_socket_t *socket)
{
    armos_socket_t *pending;
    armos_socket_t *peer_to_notify = NULL;
    armos_socket_control_t *discard_control = NULL;
    armos_socket_channel_t *free_channel = NULL;
    unsigned long flags;

    if (!socket)
        return;
    spin_lock_irqsave(&armos_socket_lock, &flags);
    armos_bound_remove_locked(socket);
    pending = socket->accept_head;
    socket->accept_head = NULL;
    socket->accept_tail = NULL;
    socket->pending = 0u;
    if (socket->channel) {
        armos_socket_channel_t *channel = socket->channel;
        uint8_t side = socket->side;

        peer_to_notify = channel->endpoint[side ^ 1u];
        channel->endpoint[side] = NULL;

        channel->read_closed[side] = 1u;
        channel->write_closed[side] = 1u;
        discard_control = channel->control_head[side];
        channel->control_head[side] = NULL;
        channel->control_tail[side] = NULL;
        channel->control_count[side] = 0u;
        if (channel->refs > 0u)
            channel->refs--;
        if (channel->refs == 0u)
            free_channel = channel;
        socket->channel = NULL;
    }
    socket->state = ARMOS_SOCKET_CLOSED;
    spin_unlock_irqrestore(&armos_socket_lock, flags);
    task_poll_notify_key(socket);
    if (peer_to_notify)
        task_poll_notify_key(peer_to_notify);

    while (pending) {
        armos_socket_t *next = pending->accept_next;

        pending->accept_next = NULL;
        armos_socket_destroy(pending);
        pending = next;
    }
    armos_socket_control_free(discard_control);
    armos_socket_channel_free(free_channel);
    kfree(socket);
}

static armos_socket_control_t *
armos_socket_detach_control_locked(armos_socket_channel_t *channel,
                                   uint8_t side)
{
    armos_socket_control_t *head = NULL;
    armos_socket_control_t *tail = NULL;

    while (channel->control_head[side] &&
           channel->control_head[side]->stream_offset <
               channel->read_sequence[side]) {
        armos_socket_control_t *control = channel->control_head[side];

        channel->control_head[side] = control->next;
        if (!channel->control_head[side])
            channel->control_tail[side] = NULL;
        if (channel->control_count[side] > 0u)
            channel->control_count[side]--;
        control->next = NULL;
        if (tail)
            tail->next = control;
        else
            head = control;
        tail = control;
    }
    return head;
}

static ssize_t
armos_socket_channel_read(armos_socket_t *socket, void *buffer, size_t length,
                          armos_socket_control_t **received_control)
{
    size_t read = 0u;

    if (received_control)
        *received_control = NULL;
    if (!socket || !buffer || length == 0u)
        return !socket || !buffer ? -EINVAL : 0;
    while (1) {
        armos_socket_channel_t *channel;
        armos_socket_control_t *control = NULL;
        uint8_t side;
        uint8_t peer;
        unsigned long flags;
        uint32_t generation = task_poll_generation();

        spin_lock_irqsave(&armos_socket_lock, &flags);
        channel = socket->channel;
        side = socket->side;
        peer = side ^ 1u;
        if (!channel || socket->state != ARMOS_SOCKET_CONNECTED) {
            spin_unlock_irqrestore(&armos_socket_lock, flags);
            return -ENOTCONN;
        }
        if (channel->read_closed[side]) {
            spin_unlock_irqrestore(&armos_socket_lock, flags);
            return 0;
        }
        while (read < length && channel->count[side] > 0u) {
            ((uint8_t *)buffer)[read++] =
                channel->data[side][channel->read_pos[side]];
            channel->read_pos[side] =
                (channel->read_pos[side] + 1u) % ARMOS_SOCKET_BUFFER_SIZE;
            channel->count[side]--;
            channel->read_sequence[side]++;
        }
        if (read > 0u)
            control = armos_socket_detach_control_locked(channel, side);
        if (read > 0u)
            spin_unlock_irqrestore(&armos_socket_lock, flags);
        if (read > 0u) {
            if (received_control)
                *received_control = control;
            else
                armos_socket_control_free(control);
            armos_socket_channel_notify(channel);
            return (ssize_t)read;
        }
        if (channel->write_closed[peer]) {
            spin_unlock_irqrestore(&armos_socket_lock, flags);
            return 0;
        }
        spin_unlock_irqrestore(&armos_socket_lock, flags);
        if (armos_socket_wait(socket, generation) != 0)
            return -EINTR;
    }
}

static ssize_t
armos_socket_channel_write(armos_socket_t *socket, const void *buffer,
                           size_t length,
                           armos_socket_control_t **pending_control)
{
    size_t written = 0u;

    if (!socket || (!buffer && length != 0u))
        return -EINVAL;
    while (written < length) {
        armos_socket_channel_t *channel;
        uint8_t side;
        uint8_t peer;
        unsigned long flags;
        uint32_t generation = task_poll_generation();

        spin_lock_irqsave(&armos_socket_lock, &flags);
        channel = socket->channel;
        side = socket->side;
        peer = side ^ 1u;
        if (!channel || socket->state != ARMOS_SOCKET_CONNECTED) {
            spin_unlock_irqrestore(&armos_socket_lock, flags);
            return written > 0u ? (ssize_t)written : -ENOTCONN;
        }
        if (channel->write_closed[side] || channel->read_closed[peer]) {
            spin_unlock_irqrestore(&armos_socket_lock, flags);
            return written > 0u ? (ssize_t)written : -EPIPE;
        }
        if (pending_control && *pending_control &&
            channel->control_count[peer] >= ARMOS_SOCKET_CONTROL_MAX) {
            spin_unlock_irqrestore(&armos_socket_lock, flags);
            if (armos_socket_wait(socket, generation) != 0)
                return written > 0u ? (ssize_t)written : -EINTR;
            continue;
        }
        if (pending_control && *pending_control &&
            channel->count[peer] < ARMOS_SOCKET_BUFFER_SIZE) {
            armos_socket_control_t *control = *pending_control;

            control->stream_offset = channel->write_sequence[peer];
            control->next = NULL;
            if (channel->control_tail[peer])
                channel->control_tail[peer]->next = control;
            else
                channel->control_head[peer] = control;
            channel->control_tail[peer] = control;
            channel->control_count[peer]++;
            *pending_control = NULL;
        }
        while (written < length &&
               channel->count[peer] < ARMOS_SOCKET_BUFFER_SIZE) {
            channel->data[peer][channel->write_pos[peer]] =
                ((const uint8_t *)buffer)[written++];
            channel->write_pos[peer] =
                (channel->write_pos[peer] + 1u) % ARMOS_SOCKET_BUFFER_SIZE;
            channel->count[peer]++;
            channel->write_sequence[peer]++;
        }
        spin_unlock_irqrestore(&armos_socket_lock, flags);
        if (written != 0u)
            armos_socket_channel_notify(channel);
        if (written == length)
            return (ssize_t)written;
        if (armos_socket_wait(socket, generation) != 0)
            return written > 0u ? (ssize_t)written : -EINTR;
    }
    return (ssize_t)written;
}

static ssize_t armos_socket_file_read(file_t *file, void *buffer, size_t length)
{
    armos_socket_t *socket = file ? file->private_data : NULL;

    return armos_socket_channel_read(socket, buffer, length, NULL);
}

static ssize_t armos_socket_file_write(file_t *file, const void *buffer,
                                       size_t length)
{
    armos_socket_t *socket = file ? file->private_data : NULL;

    return armos_socket_channel_write(socket, buffer, length, NULL);
}

static int armos_socket_file_close(file_t *file)
{
    armos_socket_t *socket = file ? file->private_data : NULL;

    if (file)
        file->private_data = NULL;
    armos_socket_destroy(socket);
    return 0;
}

static off_t armos_socket_file_lseek(file_t *file, off_t offset, int whence)
{
    (void)file;
    (void)offset;
    (void)whence;
    return -ESPIPE;
}

static file_operations_t armos_socket_file_ops = {
    .read = armos_socket_file_read,
    .write = armos_socket_file_write,
    .close = armos_socket_file_close,
    .lseek = armos_socket_file_lseek,
};

static file_t *armos_socket_create_file(armos_socket_t *socket)
{
    file_t *file = create_file();
    inode_t *inode;
    uint32_t now;

    if (!file)
        return NULL;
    inode = create_inode();
    if (!inode) {
        kfree(file);
        return NULL;
    }
    now = get_current_time();
    inode->mode = S_IFSOCK | 0600;
    inode->uid = current_uid();
    inode->gid = current_gid();
    inode->nlink = 1u;
    inode->atime = now;
    inode->mtime = now;
    inode->ctime = now;
    inode->f_op = &armos_socket_file_ops;
    file->f_op = &armos_socket_file_ops;
    file->flags = O_RDWR;
    file->type = FILE_TYPE_ARMOS_SOCKET;
    file->inode = inode;
    file->private_data = socket;
    strncpy(file->name, "armos-local", sizeof(file->name) - 1u);
    return file;
}

static int armos_socket_install(armos_socket_t *socket)
{
    task_t *task = task_current_local();
    file_t *file;
    int fd;

    if (!task || !task->process)
        return -ENODEV;
    file = armos_socket_create_file(socket);
    if (!file)
        return -ENOMEM;
    fd = vfs_install_file(task, file, 0u);
    if (fd < 0) {
        file->private_data = NULL;
        close_file(file);
        return fd;
    }
    return fd;
}

static int armos_socket_address_copy(const void *user_address, uint32_t length,
                             armos_socket_address_t *address)
{
    uint32_t copy_length;
    uint32_t path_length;

    if (!user_address || !address ||
        length < sizeof(address->family) + 1u)
        return -EINVAL;
    memset(address, 0, sizeof(*address));
    copy_length = length < sizeof(*address) ? length : sizeof(*address);
    if (copy_from_user(address, user_address, copy_length) < 0)
        return -EFAULT;
    if (address->family != ARMOS_AF_LOCAL)
        return -EAFNOSUPPORT;
    path_length = copy_length - sizeof(address->family);
    if (address->path[0] == '\0')
        return -ENOTSUP;
    for (uint32_t index = 0u; index < path_length; index++) {
        if (address->path[index] == '\0')
            return 0;
    }
    return path_length < ARMOS_SOCKET_PATH_MAX ? -EINVAL : -ENAMETOOLONG;
}

bool armos_socket_read_ready(file_t *file)
{
    armos_socket_t *socket = file ? file->private_data : NULL;
    bool ready = false;
    unsigned long flags;

    if (!socket)
        return false;
    spin_lock_irqsave(&armos_socket_lock, &flags);
    if (socket->state == ARMOS_SOCKET_LISTENING) {
        ready = socket->pending > 0u;
    } else if (socket->state == ARMOS_SOCKET_CONNECTED && socket->channel) {
        uint8_t side = socket->side;
        uint8_t peer = side ^ 1u;

        ready = socket->channel->count[side] > 0u ||
                socket->channel->read_closed[side] ||
                socket->channel->write_closed[peer];
    }
    spin_unlock_irqrestore(&armos_socket_lock, flags);
    return ready;
}

bool armos_socket_write_ready(file_t *file)
{
    armos_socket_t *socket = file ? file->private_data : NULL;
    bool ready = false;
    unsigned long flags;

    if (!socket)
        return false;
    spin_lock_irqsave(&armos_socket_lock, &flags);
    if (socket->state == ARMOS_SOCKET_CONNECTED && socket->channel) {
        uint8_t side = socket->side;
        uint8_t peer = side ^ 1u;

        ready = !socket->channel->write_closed[side] &&
                !socket->channel->read_closed[peer] &&
                socket->channel->count[peer] < ARMOS_SOCKET_BUFFER_SIZE;
    }
    spin_unlock_irqrestore(&armos_socket_lock, flags);
    return ready;
}

int armos_socket_create(int type, int protocol)
{
    armos_socket_t *socket;
    int fd;

    if (type != ARMOS_SOCK_STREAM || protocol != 0)
        return -EPROTONOSUPPORT;
    socket = armos_socket_allocate();
    if (!socket)
        return -ENOMEM;
    fd = armos_socket_install(socket);
    if (fd < 0)
        armos_socket_destroy(socket);
    return fd;
}

int armos_socket_pair(int domain, int type, int protocol, int *user_sockets)
{
    armos_socket_t *first = NULL;
    armos_socket_t *second = NULL;
    armos_socket_channel_t *channel = NULL;
    int descriptors[2];

    if (domain != ARMOS_AF_LOCAL)
        return -EAFNOSUPPORT;
    if (type != ARMOS_SOCK_STREAM || protocol != 0)
        return -EPROTONOSUPPORT;
    if (!user_sockets)
        return -EFAULT;
    first = armos_socket_allocate();
    second = armos_socket_allocate();
    channel = armos_socket_channel_create();
    if (!first || !second || !channel)
        goto no_memory;
    first->channel = channel;
    first->side = 0u;
    first->state = ARMOS_SOCKET_CONNECTED;
    second->channel = channel;
    second->side = 1u;
    channel->endpoint[0] = first;
    channel->endpoint[1] = second;
    second->state = ARMOS_SOCKET_CONNECTED;
    descriptors[0] = armos_socket_install(first);
    if (descriptors[0] < 0)
        goto install_failed;
    descriptors[1] = armos_socket_install(second);
    if (descriptors[1] < 0) {
        (void)sys_close(descriptors[0]);
        armos_socket_destroy(second);
        return descriptors[1];
    }
    if (copy_to_user(user_sockets, descriptors, sizeof(descriptors)) < 0) {
        (void)sys_close(descriptors[0]);
        (void)sys_close(descriptors[1]);
        return -EFAULT;
    }
    return 0;

install_failed:
    armos_socket_destroy(first);
    armos_socket_destroy(second);
    return descriptors[0];
no_memory:
    kfree(first);
    kfree(second);
    armos_socket_channel_free(channel);
    return -ENOMEM;
}

int armos_socket_bind(int fd, const void *user_address, uint32_t length)
{
    armos_socket_t *socket = armos_socket_from_fd(fd);
    armos_socket_address_t address;
    unsigned long flags;
    int result;

    if (!socket)
        return -EBADF;
    result = armos_socket_address_copy(user_address, length, &address);
    if (result < 0)
        return result;
    spin_lock_irqsave(&armos_socket_lock, &flags);
    if (socket->state != ARMOS_SOCKET_CREATED) {
        spin_unlock_irqrestore(&armos_socket_lock, flags);
        return -EINVAL;
    }
    if (armos_bound_find_locked(address.path)) {
        spin_unlock_irqrestore(&armos_socket_lock, flags);
        return -EADDRINUSE;
    }
    strncpy(socket->path, address.path, sizeof(socket->path) - 1u);
    socket->state = ARMOS_SOCKET_BOUND;
    socket->bound_next = armos_bound_list;
    armos_bound_list = socket;
    spin_unlock_irqrestore(&armos_socket_lock, flags);
    return 0;
}

int armos_socket_listen(int fd, int backlog)
{
    armos_socket_t *socket = armos_socket_from_fd(fd);
    unsigned long flags;

    if (!socket)
        return -EBADF;
    if (backlog < 1)
        backlog = 1;
    if (backlog > (int)ARMOS_SOCKET_BACKLOG_MAX)
        backlog = ARMOS_SOCKET_BACKLOG_MAX;
    spin_lock_irqsave(&armos_socket_lock, &flags);
    if (socket->state != ARMOS_SOCKET_BOUND) {
        spin_unlock_irqrestore(&armos_socket_lock, flags);
        return -EINVAL;
    }
    socket->backlog = (uint8_t)backlog;
    socket->state = ARMOS_SOCKET_LISTENING;
    spin_unlock_irqrestore(&armos_socket_lock, flags);
    return 0;
}

int armos_socket_connect(int fd, const void *user_address, uint32_t length)
{
    armos_socket_t *client = armos_socket_from_fd(fd);
    armos_socket_t *server = NULL;
    armos_socket_t *listener;
    armos_socket_channel_t *channel = NULL;
    armos_socket_address_t address;
    unsigned long flags;
    int result;

    if (!client)
        return -EBADF;
    result = armos_socket_address_copy(user_address, length, &address);
    if (result < 0)
        return result;
    server = armos_socket_allocate();
    channel = armos_socket_channel_create();
    if (!server || !channel) {
        kfree(server);
        armos_socket_channel_free(channel);
        return -ENOMEM;
    }
    spin_lock_irqsave(&armos_socket_lock, &flags);
    if (client->state == ARMOS_SOCKET_CONNECTED) {
        spin_unlock_irqrestore(&armos_socket_lock, flags);
        kfree(server);
        armos_socket_channel_free(channel);
        return -EISCONN;
    }
    if (client->state != ARMOS_SOCKET_CREATED) {
        spin_unlock_irqrestore(&armos_socket_lock, flags);
        kfree(server);
        armos_socket_channel_free(channel);
        return -EINVAL;
    }
    listener = armos_bound_find_locked(address.path);
    if (!listener) {
        spin_unlock_irqrestore(&armos_socket_lock, flags);
        kfree(server);
        armos_socket_channel_free(channel);
        return -ENOENT;
    }
    if (listener->state != ARMOS_SOCKET_LISTENING) {
        spin_unlock_irqrestore(&armos_socket_lock, flags);
        kfree(server);
        armos_socket_channel_free(channel);
        return -ECONNREFUSED;
    }
    if (listener->pending >= listener->backlog) {
        spin_unlock_irqrestore(&armos_socket_lock, flags);
        kfree(server);
        armos_socket_channel_free(channel);
        return -EAGAIN;
    }
    client->channel = channel;
    client->side = 0u;
    client->state = ARMOS_SOCKET_CONNECTED;
    server->channel = channel;
    server->side = 1u;
    server->state = ARMOS_SOCKET_CONNECTED;
    channel->endpoint[0] = client;
    channel->endpoint[1] = server;
    server->accept_next = NULL;
    if (listener->accept_tail)
        listener->accept_tail->accept_next = server;
    else
        listener->accept_head = server;
    listener->accept_tail = server;
    listener->pending++;
    spin_unlock_irqrestore(&armos_socket_lock, flags);
    task_poll_notify_key(listener);
    armos_socket_channel_notify(channel);
    return 0;
}

int armos_socket_accept(int fd, void *user_address, uint32_t *user_length)
{
    armos_socket_t *listener = armos_socket_from_fd(fd);
    armos_socket_t *accepted;
    unsigned long flags;
    int accepted_fd;

    if (!listener)
        return -EBADF;
    while (1) {
        uint32_t generation = task_poll_generation();

        spin_lock_irqsave(&armos_socket_lock, &flags);
        if (listener->state != ARMOS_SOCKET_LISTENING) {
            spin_unlock_irqrestore(&armos_socket_lock, flags);
            return -EINVAL;
        }
        accepted = listener->accept_head;
        if (accepted) {
            listener->accept_head = accepted->accept_next;
            if (!listener->accept_head)
                listener->accept_tail = NULL;
            accepted->accept_next = NULL;
            listener->pending--;
            spin_unlock_irqrestore(&armos_socket_lock, flags);
            task_poll_notify_key(listener);
            break;
        }
        spin_unlock_irqrestore(&armos_socket_lock, flags);
        if (armos_socket_wait(listener, generation) != 0)
            return -EINTR;
    }
    accepted_fd = armos_socket_install(accepted);
    if (accepted_fd < 0) {
        armos_socket_destroy(accepted);
        return accepted_fd;
    }
    if (user_address && user_length) {
        armos_socket_address_t address;
        uint32_t capacity;
        uint32_t output_length = sizeof(address.family) + 1u;

        if (copy_from_user(&capacity, user_length, sizeof(capacity)) < 0) {
            (void)sys_close(accepted_fd);
            return -EFAULT;
        }
        memset(&address, 0, sizeof(address));
        address.family = ARMOS_AF_LOCAL;
        if (capacity >= output_length &&
            copy_to_user(user_address, &address, output_length) < 0) {
            (void)sys_close(accepted_fd);
            return -EFAULT;
        }
        if (copy_to_user(user_length, &output_length,
                         sizeof(output_length)) < 0) {
            (void)sys_close(accepted_fd);
            return -EFAULT;
        }
    }
    return accepted_fd;
}

typedef struct armos_socket_cmsghdr {
    size_t length;
    int level;
    int type;
} armos_socket_cmsghdr_t;

static size_t armos_socket_control_align(size_t length)
{
    return (length + sizeof(size_t) - 1u) & ~(sizeof(size_t) - 1u);
}

static void armos_socket_close_installed(const int *descriptors,
                                         uint32_t count)
{
    for (uint32_t index = 0u; index < count; index++)
        (void)sys_close(descriptors[index]);
}

static int armos_socket_copy_iov(
    const struct armos_msghdr_kernel *message,
    struct iovec_kernel *iov, size_t *total_length)
{
    size_t total = 0u;

    if (message->msg_iovlen > ARMOS_SOCKET_IOV_MAX)
        return -EMSGSIZE;
    if (message->msg_iovlen != 0u && !message->msg_iov)
        return -EFAULT;
    if (message->msg_iovlen != 0u &&
        copy_from_user(iov, message->msg_iov,
                       message->msg_iovlen * sizeof(iov[0])) < 0)
        return -EFAULT;
    for (size_t index = 0u; index < message->msg_iovlen; index++) {
        if (iov[index].iov_len != 0u && !iov[index].iov_base)
            return -EFAULT;
        if (iov[index].iov_len > ARMOS_SOCKET_MESSAGE_MAX - total)
            return -EMSGSIZE;
        total += iov[index].iov_len;
    }
    *total_length = total;
    return 0;
}

static int armos_socket_copy_payload_from_user(
    uint8_t *payload, const struct iovec_kernel *iov, size_t iov_count)
{
    size_t copied = 0u;

    for (size_t index = 0u; index < iov_count; index++) {
        if (iov[index].iov_len == 0u)
            continue;
        if (copy_from_user(payload + copied, iov[index].iov_base,
                           iov[index].iov_len) < 0)
            return -EFAULT;
        copied += iov[index].iov_len;
    }
    return 0;
}

static int armos_socket_copy_payload_to_user(
    const uint8_t *payload, size_t length,
    const struct iovec_kernel *iov, size_t iov_count)
{
    size_t copied = 0u;

    for (size_t index = 0u; index < iov_count && copied < length; index++) {
        size_t chunk = iov[index].iov_len;

        if (chunk > length - copied)
            chunk = length - copied;
        if (chunk != 0u &&
            copy_to_user(iov[index].iov_base, payload + copied, chunk) < 0)
            return -EFAULT;
        copied += chunk;
    }
    return 0;
}

static int armos_socket_control_from_user(
    const struct armos_msghdr_kernel *message,
    armos_socket_control_t **result)
{
    task_t *task = task_current_local();
    armos_socket_control_t *control = NULL;
    uint8_t *buffer = NULL;
    size_t offset = 0u;
    uint32_t descriptor_count = 0u;
    int descriptors[ARMOS_SOCKET_RIGHTS_MAX];
    int error = 0;

    *result = NULL;
    if (message->msg_controllen == 0u)
        return message->msg_control ? 0 : 0;
    if (!message->msg_control)
        return -EFAULT;
    if (message->msg_controllen > 1024u)
        return -EMSGSIZE;
    if (!task || !task->process)
        return -EBADF;

    buffer = kmalloc(message->msg_controllen);
    if (!buffer)
        return -ENOMEM;
    if (copy_from_user(buffer, message->msg_control,
                       message->msg_controllen) < 0) {
        error = -EFAULT;
        goto out;
    }

    while (offset + sizeof(armos_socket_cmsghdr_t) <=
           message->msg_controllen) {
        armos_socket_cmsghdr_t *header =
            (armos_socket_cmsghdr_t *)(void *)(buffer + offset);
        size_t data_offset = armos_socket_control_align(
            sizeof(armos_socket_cmsghdr_t));
        size_t data_length;
        size_t next;

        if (header->length < data_offset ||
            header->length > message->msg_controllen - offset) {
            error = -EINVAL;
            goto out;
        }
        if (header->level != ARMOS_SOL_SOCKET ||
            header->type != ARMOS_SCM_RIGHTS) {
            error = -ENOTSUP;
            goto out;
        }
        data_length = header->length - data_offset;
        if (data_length == 0u || data_length % sizeof(int) != 0u ||
            data_length / sizeof(int) >
                ARMOS_SOCKET_RIGHTS_MAX - descriptor_count) {
            error = -EMSGSIZE;
            goto out;
        }
        memcpy(&descriptors[descriptor_count], buffer + offset + data_offset,
               data_length);
        descriptor_count += (uint32_t)(data_length / sizeof(int));
        next = armos_socket_control_align(header->length);
        if (next > message->msg_controllen - offset) {
            if (header->length != message->msg_controllen - offset) {
                error = -EINVAL;
                goto out;
            }
            offset = message->msg_controllen;
            break;
        }
        offset += next;
    }
    if (offset != message->msg_controllen) {
        error = -EINVAL;
        goto out;
    }
    if (descriptor_count == 0u)
        goto out;

    control = kzalloc(sizeof(*control));
    if (!control) {
        error = -ENOMEM;
        goto out;
    }
    for (uint32_t index = 0u; index < descriptor_count; index++) {
        int descriptor = descriptors[index];
        file_t *file;

        if (descriptor < 0 || descriptor >= MAX_FILES) {
            error = -EBADF;
            goto out;
        }
        file = vfs_get_file_from_fd(task, descriptor);
        control->files[index] = file;
        if (!control->files[index]) {
            error = -EBADF;
            goto out;
        }
        control->file_count++;
    }
    *result = control;
    control = NULL;

out:
    armos_socket_control_free(control);
    kfree(buffer);
    return error;
}

ssize_t armos_socket_send_message(
    int fd, const struct armos_msghdr_kernel *user_message, int flags)
{
    armos_socket_t *socket = armos_socket_from_fd(fd);
    struct armos_msghdr_kernel message;
    struct iovec_kernel iov[ARMOS_SOCKET_IOV_MAX];
    armos_socket_control_t *control = NULL;
    uint8_t *payload = NULL;
    size_t length;
    ssize_t result;
    int error;

    if (!socket)
        return -EBADF;
    if (!user_message)
        return -EFAULT;
    if (flags != 0)
        return -ENOTSUP;
    if (copy_from_user(&message, user_message, sizeof(message)) < 0)
        return -EFAULT;
    if (message.msg_name || message.msg_namelen != 0u)
        return -EISCONN;
    error = armos_socket_copy_iov(&message, iov, &length);
    if (error < 0)
        return error;
    error = armos_socket_control_from_user(&message, &control);
    if (error < 0)
        return error;
    if (control && length == 0u) {
        armos_socket_control_free(control);
        return -EINVAL;
    }
    if (length == 0u)
        return 0;

    payload = kmalloc(length);
    if (!payload) {
        armos_socket_control_free(control);
        return -ENOMEM;
    }
    error = armos_socket_copy_payload_from_user(payload, iov,
                                                 message.msg_iovlen);
    if (error < 0) {
        kfree(payload);
        armos_socket_control_free(control);
        return error;
    }
    result = armos_socket_channel_write(socket, payload, length, &control);
    kfree(payload);
    armos_socket_control_free(control);
    return result;
}

static uint32_t armos_socket_install_control(
    armos_socket_control_t *control, uint32_t capacity, bool close_on_exec,
    int *installed, bool *truncated)
{
    task_t *task = task_current_local();
    uint32_t count = 0u;

    while (control) {
        for (uint32_t index = 0u; index < control->file_count; index++) {
            file_t *file = control->files[index];
            int descriptor = -1;

            if (file && count < capacity &&
                count < ARMOS_SOCKET_RIGHTS_MAX)
                descriptor = vfs_install_file(
                    task, file, close_on_exec ? O_CLOEXEC : 0u);
            if (descriptor >= 0) {
                control->files[index] = NULL;
                installed[count++] = descriptor;
            } else if (file) {
                *truncated = true;
            }
        }
        control = control->next;
    }
    return count;
}

ssize_t armos_socket_receive_message(
    int fd, struct armos_msghdr_kernel *user_message, int flags)
{
    armos_socket_t *socket = armos_socket_from_fd(fd);
    struct armos_msghdr_kernel message;
    struct iovec_kernel iov[ARMOS_SOCKET_IOV_MAX];
    armos_socket_control_t *control = NULL;
    uint8_t control_buffer[256];
    uint8_t *payload = NULL;
    int installed[ARMOS_SOCKET_RIGHTS_MAX];
    uint32_t installed_count = 0u;
    uint32_t descriptor_capacity = 0u;
    size_t control_header_size =
        armos_socket_control_align(sizeof(armos_socket_cmsghdr_t));
    size_t length;
    ssize_t result;
    bool truncated = false;
    int error;

    if (!socket)
        return -EBADF;
    if (!user_message)
        return -EFAULT;
    if (flags & ~ARMOS_MSG_CMSG_CLOEXEC)
        return -ENOTSUP;
    if (copy_from_user(&message, user_message, sizeof(message)) < 0)
        return -EFAULT;
    error = armos_socket_copy_iov(&message, iov, &length);
    if (error < 0)
        return error;
    if (length == 0u)
        return 0;

    payload = kmalloc(length);
    if (!payload)
        return -ENOMEM;
    result = armos_socket_channel_read(socket, payload, length, &control);
    if (result <= 0) {
        kfree(payload);
        armos_socket_control_free(control);
        return result;
    }
    error = armos_socket_copy_payload_to_user(payload, (size_t)result, iov,
                                               message.msg_iovlen);
    kfree(payload);
    if (error < 0) {
        armos_socket_control_free(control);
        return error;
    }

    if (message.msg_control && message.msg_controllen >=
        control_header_size + sizeof(int)) {
        descriptor_capacity = (uint32_t)(
            (message.msg_controllen - control_header_size) / sizeof(int));
        if (descriptor_capacity > ARMOS_SOCKET_RIGHTS_MAX)
            descriptor_capacity = ARMOS_SOCKET_RIGHTS_MAX;
    }
    installed_count = armos_socket_install_control(
        control, descriptor_capacity,
        (flags & ARMOS_MSG_CMSG_CLOEXEC) != 0, installed, &truncated);
    if (control && installed_count == 0u)
        truncated = true;

    message.msg_flags = truncated ? ARMOS_MSG_CTRUNC : 0;
    message.msg_namelen = 0u;
    if (installed_count != 0u) {
        armos_socket_cmsghdr_t *header =
            (armos_socket_cmsghdr_t *)(void *)control_buffer;
        size_t output_length =
            control_header_size + installed_count * sizeof(int);

        memset(control_buffer, 0, sizeof(control_buffer));
        header->length = output_length;
        header->level = ARMOS_SOL_SOCKET;
        header->type = ARMOS_SCM_RIGHTS;
        memcpy(control_buffer + control_header_size, installed,
               installed_count * sizeof(int));
        if (copy_to_user(message.msg_control, control_buffer,
                         output_length) < 0) {
            armos_socket_close_installed(installed, installed_count);
            armos_socket_control_free(control);
            return -EFAULT;
        }
        message.msg_controllen = output_length;
    } else {
        message.msg_controllen = 0u;
    }
    if (copy_to_user(user_message, &message, sizeof(message)) < 0) {
        armos_socket_close_installed(installed, installed_count);
        armos_socket_control_free(control);
        return -EFAULT;
    }
    armos_socket_control_free(control);
    return result;
}

ssize_t armos_socket_send(int fd, const void *buffer, size_t length, int flags,
                         const void *address, uint32_t address_length)
{
    (void)address_length;
    if (!armos_socket_is_fd(fd))
        return -EBADF;
    if (flags != 0)
        return -ENOTSUP;
    if (address)
        return -EISCONN;
    return sys_write(fd, buffer, length);
}

ssize_t armos_socket_receive(int fd, void *buffer, size_t length, int flags,
                            void *address, uint32_t *address_length)
{
    ssize_t result;

    if (!armos_socket_is_fd(fd))
        return -EBADF;
    if (flags != 0)
        return -ENOTSUP;
    result = sys_read(fd, buffer, length);
    if (result >= 0 && address && address_length) {
        armos_socket_address_t source;
        uint32_t capacity;
        uint32_t output_length = sizeof(source.family) + 1u;

        if (copy_from_user(&capacity, address_length, sizeof(capacity)) < 0)
            return -EFAULT;
        memset(&source, 0, sizeof(source));
        source.family = ARMOS_AF_LOCAL;
        if (capacity >= output_length &&
            copy_to_user(address, &source, output_length) < 0)
            return -EFAULT;
        if (copy_to_user(address_length, &output_length,
                         sizeof(output_length)) < 0)
            return -EFAULT;
    }
    return result;
}

int armos_socket_shutdown(int fd, int how)
{
    armos_socket_t *socket = armos_socket_from_fd(fd);
    armos_socket_channel_t *channel;
    armos_socket_control_t *discard_control = NULL;
    unsigned long flags;

    if (!socket)
        return -EBADF;
    if (how < 0 || how > 2)
        return -EINVAL;
    spin_lock_irqsave(&armos_socket_lock, &flags);
    channel = socket->channel;
    if (!channel || socket->state != ARMOS_SOCKET_CONNECTED) {
        spin_unlock_irqrestore(&armos_socket_lock, flags);
        return -ENOTCONN;
    }
    if (how == 0 || how == 2) {
        channel->read_closed[socket->side] = 1u;
        discard_control = channel->control_head[socket->side];
        channel->control_head[socket->side] = NULL;
        channel->control_tail[socket->side] = NULL;
        channel->control_count[socket->side] = 0u;
    }
    if (how == 1 || how == 2)
        channel->write_closed[socket->side] = 1u;
    spin_unlock_irqrestore(&armos_socket_lock, flags);
    armos_socket_channel_notify(channel);
    armos_socket_control_free(discard_control);
    return 0;
}

int sys_socketpair(int domain, int type, int protocol, int *sockets)
{
    return armos_socket_pair(domain, type, protocol, sockets);
}

ssize_t sys_sendmsg(int fd, const struct armos_msghdr_kernel *message,
                    int flags)
{
    if (!armos_socket_is_fd(fd))
        return -ENOTSUP;
    return armos_socket_send_message(fd, message, flags);
}

ssize_t sys_recvmsg(int fd, struct armos_msghdr_kernel *message, int flags)
{
    if (!armos_socket_is_fd(fd))
        return -ENOTSUP;
    return armos_socket_receive_message(fd, message, flags);
}
