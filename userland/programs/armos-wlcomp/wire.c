/*
 * ArmOS
 * Copyright (c) 2026 Mohamed Ennassiri
 *
 * Licensed under the Apache License, Version 2.0.
 * See LICENSE for details.
 *
 * File: userland/programs/armos-wlcomp/wire.c
 * Layer: Userland / graphical services
 *
 * Responsibilities:
 * - Encode and decode the native-endian Wayland wire framing.
 * - Track per-client protocol object identifiers.
 * - Receive descriptor-bearing requests over ArmOS local sockets.
 *
 * Notes:
 * - Wayland messages are 32-bit aligned and carry size in the upper header.
 * - File descriptors remain outside the byte stream and are queued in order.
 */

#include "armos_wlcomp.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <unistd.h>

union wl_server_control_buffer {
    struct cmsghdr alignment;
    uint8_t bytes[
        CMSG_SPACE(sizeof(int) * WL_SERVER_MAX_PENDING_FDS)];
};

#define WL_WIRE_HEADER_SIZE 8u
#define WL_WIRE_MAX_EVENT   1024u

uint32_t wl_wire_align(uint32_t size)
{
    return (size + 3u) & ~3u;
}

uint32_t wl_wire_u32(const uint8_t *data)
{
    uint32_t value;

    memcpy(&value, data, sizeof(value));
    return value;
}

void wl_wire_store_u32(uint8_t *data, uint32_t value)
{
    memcpy(data, &value, sizeof(value));
}

static int wl_client_update_output_watch(struct wl_server_client *client)
{
    uint32_t mask = WL_EVENT_HANGUP | WL_EVENT_ERROR;

    if (!client || !client->event_source)
        return 0;
    if (!client->dispatch_blocked)
        mask |= WL_EVENT_READABLE;
    if (client->output_head)
        mask |= WL_EVENT_WRITABLE;
    return wl_event_source_fd_update(client->event_source, mask);
}

int wl_client_set_dispatch_blocked(struct wl_server_client *client,
                                   bool blocked)
{
    if (!client)
        return -1;
    client->dispatch_blocked = blocked;
    return wl_client_update_output_watch(client);
}

static int wl_client_queue_output(struct wl_server_client *client,
                                  const void *buffer, size_t size, int fd)
{
    struct wl_server_outgoing *outgoing;
    int owned_fd = -1;

    if (!client || client->fd < 0 || !buffer || size == 0u ||
        size > WL_WIRE_MAX_EVENT ||
        client->output_messages >= WL_SERVER_MAX_OUTPUT_MESSAGES ||
        client->output_bytes > WL_SERVER_MAX_OUTPUT_BYTES - size) {
        errno = ENOBUFS;
        return -1;
    }
    if (fd >= 0) {
        owned_fd = dup(fd);
        if (owned_fd < 0)
            return -1;
    }
    outgoing = malloc(sizeof(*outgoing) + size);
    if (!outgoing) {
        if (owned_fd >= 0)
            close(owned_fd);
        return -1;
    }
    outgoing->next = NULL;
    outgoing->size = size;
    outgoing->offset = 0u;
    outgoing->fd = owned_fd;
    memcpy(outgoing->data, buffer, size);
    if (client->output_tail)
        client->output_tail->next = outgoing;
    else
        client->output_head = outgoing;
    client->output_tail = outgoing;
    client->output_bytes += size;
    client->output_messages++;
    if (wl_client_update_output_watch(client) < 0)
        return -1;
    return wl_client_flush_output(client);
}

int wl_client_flush_output(struct wl_server_client *client)
{
    while (client && client->output_head) {
        struct wl_server_outgoing *outgoing = client->output_head;
        ssize_t sent;

        if (outgoing->fd >= 0) {
            union wl_server_control_buffer control;
            struct cmsghdr *header;
            struct iovec iov;
            struct msghdr packet;

            memset(&control, 0, sizeof(control));
            memset(&packet, 0, sizeof(packet));
            iov.iov_base = outgoing->data + outgoing->offset;
            iov.iov_len = outgoing->size - outgoing->offset;
            packet.msg_iov = &iov;
            packet.msg_iovlen = 1u;
            packet.msg_control = control.bytes;
            packet.msg_controllen = CMSG_SPACE(sizeof(int));
            header = CMSG_FIRSTHDR(&packet);
            header->cmsg_len = CMSG_LEN(sizeof(int));
            header->cmsg_level = SOL_SOCKET;
            header->cmsg_type = SCM_RIGHTS;
            memcpy(CMSG_DATA(header), &outgoing->fd, sizeof(outgoing->fd));
            do {
                sent = sendmsg(client->fd, &packet, 0);
            } while (sent < 0 && errno == EINTR);
            if (sent > 0) {
                close(outgoing->fd);
                outgoing->fd = -1;
            }
        } else {
            do {
                sent = write(client->fd,
                             outgoing->data + outgoing->offset,
                             outgoing->size - outgoing->offset);
            } while (sent < 0 && errno == EINTR);
        }
        if (sent < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
            return wl_client_update_output_watch(client);
        if (sent <= 0)
            return -1;
        outgoing->offset += (size_t)sent;
        if (outgoing->offset < outgoing->size)
            continue;
        client->output_head = outgoing->next;
        if (!client->output_head)
            client->output_tail = NULL;
        client->output_bytes -= outgoing->size;
        client->output_messages--;
        free(outgoing);
    }
    return client ? wl_client_update_output_watch(client) : -1;
}

int wl_client_send_words(struct wl_server_client *client, uint32_t object_id,
                         uint16_t opcode, const uint32_t *words,
                         size_t word_count)
{
    uint8_t message[WL_WIRE_MAX_EVENT];
    size_t size = WL_WIRE_HEADER_SIZE + word_count * sizeof(uint32_t);

    if (!client || client->fd < 0 || size > sizeof(message) ||
        size > 0xffffu)
        return -1;

    wl_wire_store_u32(message, object_id);
    wl_wire_store_u32(message + 4,
                      ((uint32_t)size << 16) | (uint32_t)opcode);
    if (word_count > 0)
        memcpy(message + WL_WIRE_HEADER_SIZE, words,
               word_count * sizeof(uint32_t));
    return wl_client_queue_output(client, message, size, -1);
}

int wl_client_send_fd_words(struct wl_server_client *client,
                            uint32_t object_id, uint16_t opcode,
                            const uint32_t *words, size_t word_count, int fd)
{
    uint8_t message[WL_WIRE_MAX_EVENT];
    size_t size = WL_WIRE_HEADER_SIZE + word_count * sizeof(uint32_t);

    if (!client || client->fd < 0 || fd < 0 ||
        size > sizeof(message) || size > 0xffffu)
        return -1;
    wl_wire_store_u32(message, object_id);
    wl_wire_store_u32(message + 4,
                      ((uint32_t)size << 16) | (uint32_t)opcode);
    if (word_count > 0u)
        memcpy(message + WL_WIRE_HEADER_SIZE, words,
               word_count * sizeof(uint32_t));
    return wl_client_queue_output(client, message, size, fd);
}

static int wl_client_build_string_message(uint8_t *message, size_t capacity,
                                          uint32_t object_id,
                                          uint16_t opcode,
                                          const char *text,
                                          size_t *message_size)
{
    uint32_t text_size;
    uint32_t padded;
    uint32_t size;

    if (!message || !text || !message_size)
        return -1;
    text_size = (uint32_t)strlen(text) + 1u;
    padded = wl_wire_align(text_size);
    size = WL_WIRE_HEADER_SIZE + 4u + padded;
    if (size > capacity || size > 0xffffu)
        return -1;
    memset(message, 0, size);
    wl_wire_store_u32(message, object_id);
    wl_wire_store_u32(message + 4u, (size << 16) | opcode);
    wl_wire_store_u32(message + 8u, text_size);
    memcpy(message + 12u, text, text_size);
    *message_size = size;
    return 0;
}

int wl_client_send_string(struct wl_server_client *client,
                          uint32_t object_id, uint16_t opcode,
                          const char *text)
{
    uint8_t message[WL_WIRE_MAX_EVENT];
    size_t size;

    if (!client || client->fd < 0 ||
        wl_client_build_string_message(message, sizeof(message), object_id,
                                       opcode, text, &size) < 0)
        return -1;
    return wl_client_queue_output(client, message, size, -1);
}

int wl_client_send_fd_string(struct wl_server_client *client,
                             uint32_t object_id, uint16_t opcode,
                             const char *text, int fd)
{
    uint8_t message[WL_WIRE_MAX_EVENT];
    size_t size;

    if (!client || client->fd < 0 || fd < 0 ||
        wl_client_build_string_message(message, sizeof(message), object_id,
                                       opcode, text, &size) < 0)
        return -1;
    return wl_client_queue_output(client, message, size, fd);
}

static int wl_client_send_string_event(struct wl_server_client *client,
                                       uint32_t object_id, uint16_t opcode,
                                       uint32_t first, const char *text,
                                       uint32_t last)
{
    uint8_t message[WL_WIRE_MAX_EVENT];
    uint32_t text_size;
    uint32_t padded;
    uint32_t size;

    if (!client || !text)
        return -1;
    text_size = (uint32_t)strlen(text) + 1u;
    padded = wl_wire_align(text_size);
    size = WL_WIRE_HEADER_SIZE + 4u + 4u + padded + 4u;
    if (size > sizeof(message))
        return -1;

    memset(message, 0, size);
    wl_wire_store_u32(message, object_id);
    wl_wire_store_u32(message + 4, (size << 16) | opcode);
    wl_wire_store_u32(message + 8, first);
    wl_wire_store_u32(message + 12, text_size);
    memcpy(message + 16, text, text_size);
    wl_wire_store_u32(message + 16 + padded, last);
    return wl_client_queue_output(client, message, size, -1);
}

int wl_client_send_global(struct wl_server_client *client,
                          uint32_t registry_id, uint32_t name,
                          const char *interface_name, uint32_t version)
{
    return wl_client_send_string_event(client, registry_id, 0, name,
                                       interface_name, version);
}

int wl_client_send_error(struct wl_server_client *client, uint32_t object_id,
                         uint32_t code, const char *message_text)
{
    uint8_t message[WL_WIRE_MAX_EVENT];
    uint32_t text_size;
    uint32_t padded;
    uint32_t size;

    if (!client || !message_text)
        return -1;
    text_size = (uint32_t)strlen(message_text) + 1u;
    padded = wl_wire_align(text_size);
    size = WL_WIRE_HEADER_SIZE + 8u + 4u + padded;
    if (size > sizeof(message))
        return -1;

    memset(message, 0, size);
    wl_wire_store_u32(message, WL_DISPLAY_ID);
    wl_wire_store_u32(message + 4, (size << 16) | 0u);
    wl_wire_store_u32(message + 8, object_id);
    wl_wire_store_u32(message + 12, code);
    wl_wire_store_u32(message + 16, text_size);
    memcpy(message + 20, message_text, text_size);
    return wl_client_queue_output(client, message, size, -1);
}

int wl_client_send_delete_id(struct wl_server_client *client,
                             uint32_t object_id)
{
    return wl_client_send_words(client, WL_DISPLAY_ID, 1, &object_id, 1);
}

struct wl_server_object *wl_client_find_object(
    struct wl_server_client *client, uint32_t object_id)
{
    if (!client || object_id == 0)
        return NULL;

    for (size_t index = 0; index < WL_SERVER_MAX_OBJECTS; index++) {
        if (client->objects[index].type != WL_SERVER_OBJECT_NONE &&
            client->objects[index].id == object_id)
            return &client->objects[index];
    }
    return NULL;
}

int wl_client_add_object(struct wl_server_client *client, uint32_t object_id,
                         enum wl_server_object_type type, uint32_t version,
                         void *resource)
{
    if (!client || object_id == 0 || type == WL_SERVER_OBJECT_NONE ||
        wl_client_find_object(client, object_id))
        return -1;

    for (size_t index = 0; index < WL_SERVER_MAX_OBJECTS; index++) {
        if (client->objects[index].type == WL_SERVER_OBJECT_NONE) {
            client->objects[index].id = object_id;
            client->objects[index].version = version;
            client->objects[index].type = type;
            client->objects[index].resource = resource;
            return 0;
        }
    }
    return -1;
}

void wl_client_remove_object(struct wl_server_client *client,
                             uint32_t object_id, bool notify)
{
    struct wl_server_object *object = wl_client_find_object(client, object_id);

    if (!object)
        return;
    memset(object, 0, sizeof(*object));
    if (notify)
        (void)wl_client_send_delete_id(client, object_id);
}

int wl_client_take_fd(struct wl_server_client *client)
{
    int fd;

    if (!client || client->pending_fd_count == 0)
        return -1;
    fd = client->pending_fds[0];
    client->pending_fd_count--;
    if (client->pending_fd_count > 0) {
        memmove(client->pending_fds, client->pending_fds + 1,
                client->pending_fd_count * sizeof(client->pending_fds[0]));
    }
    return fd;
}

static int wl_client_queue_control_fds(struct wl_server_client *client,
                                       struct msghdr *message)
{
    struct cmsghdr *header;

    for (header = CMSG_FIRSTHDR(message); header;
         header = CMSG_NXTHDR(message, header)) {
        size_t payload;
        size_t count;
        int *fds;

        if (header->cmsg_level != SOL_SOCKET ||
            header->cmsg_type != SCM_RIGHTS ||
            header->cmsg_len < CMSG_LEN(0))
            continue;
        payload = header->cmsg_len - CMSG_LEN(0);
        count = payload / sizeof(int);
        fds = (int *)(void *)CMSG_DATA(header);
        for (size_t index = 0; index < count; index++) {
            if (client->pending_fd_count >= WL_SERVER_MAX_PENDING_FDS) {
                close(fds[index]);
                return -1;
            }
            client->pending_fds[client->pending_fd_count++] = fds[index];
        }
    }
    return 0;
}

int wl_server_dispatch_client_pending(struct wl_server *server,
                                      struct wl_server_client *client)
{
    size_t dispatched = 0u;

    if (!server || !client)
        return -1;
    while (!client->dispatch_blocked &&
           client->receive_length >= WL_WIRE_HEADER_SIZE &&
           dispatched < WL_SERVER_CLIENT_DISPATCH_BUDGET) {
        uint32_t header = wl_wire_u32(client->receive + 4);
        uint32_t size = header >> 16;

        if (size < WL_WIRE_HEADER_SIZE || (size & 3u) != 0 ||
            size > sizeof(client->receive))
            return -1;
        if (client->receive_length < size)
            return 0;
        if (wl_server_dispatch_message(server, client, client->receive,
                                       size) < 0)
            return -1;
        client->receive_length -= size;
        if (client->receive_length > 0) {
            memmove(client->receive, client->receive + size,
                    client->receive_length);
        }
        dispatched++;
    }
    if (!client->dispatch_blocked &&
        client->receive_length >= WL_WIRE_HEADER_SIZE) {
        uint32_t header = wl_wire_u32(client->receive + 4);
        uint32_t size = header >> 16;

        if (size < WL_WIRE_HEADER_SIZE || (size & 3u) != 0 ||
            size > sizeof(client->receive))
            return -1;
        if (client->receive_length >= size)
            return 1;
    }
    return 0;
}

int wl_server_receive_client(struct wl_server *server,
                             struct wl_server_client *client)
{
    union wl_server_control_buffer control;
    struct iovec iov;
    struct msghdr message;
    ssize_t count;
    int pending;

    if (!server || !client)
        return -1;
    if (client->dispatch_blocked)
        return 0;
    pending = wl_server_dispatch_client_pending(server, client);
    if (pending != 0)
        return pending;
    if (client->receive_length >= sizeof(client->receive))
        return -1;

    memset(&message, 0, sizeof(message));
    memset(&control, 0, sizeof(control));
    iov.iov_base = client->receive + client->receive_length;
    iov.iov_len = sizeof(client->receive) - client->receive_length;
    message.msg_iov = &iov;
    message.msg_iovlen = 1;
    message.msg_control = control.bytes;
    message.msg_controllen = sizeof(control.bytes);

    do {
        count = recvmsg(client->fd, &message, MSG_CMSG_CLOEXEC);
    } while (count < 0 && errno == EINTR);
    if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
        return 0;
    if (count <= 0)
        return -1;
    if ((message.msg_flags & MSG_CTRUNC) != 0 ||
        wl_client_queue_control_fds(client, &message) < 0)
        return -1;
    client->receive_length += (size_t)count;

    return wl_server_dispatch_client_pending(server, client);
}

void wl_server_disconnect_client(struct wl_server *server,
                                 struct wl_server_client *client)
{
    struct wl_server_outgoing *outgoing;

    if (!client)
        return;

    if (server) {
        wl_server_drop_client_selection(server, client);
        if (server->focus_client == client) {
            server->focus_client = NULL;
            server->focus_surface = NULL;
        }
        if (server->pointer_client == client) {
            server->pointer_client = NULL;
            server->pointer_surface = NULL;
        }
        if (server->drag_client == client) {
            server->drag_client = NULL;
            server->drag_surface = NULL;
        }
        if (server->shell_client == client) {
            server->shell_client = NULL;
            server->panel_height = 0u;
        }
        for (size_t client_index = 0u;
             client_index < WL_SERVER_MAX_CLIENTS; client_index++) {
            struct wl_server_client *other = &server->clients[client_index];

            if (!other->used || other == client)
                continue;
            for (size_t offer_index = 0u;
                 offer_index < WL_SERVER_MAX_DATA_OFFERS; offer_index++) {
                struct wl_server_data_offer *offer =
                    &other->data_offers[offer_index];

                if (offer->used && offer->source_client == client)
                    offer->source = NULL;
            }
        }
    }
    wl_client_destroy_buffers(client);
    for (size_t index = 0; index < WL_SERVER_MAX_POOLS; index++) {
        if (!client->pools[index].used)
            continue;
        if (client->pools[index].mapping &&
            client->pools[index].mapping != MAP_FAILED) {
            munmap(client->pools[index].mapping,
                   client->pools[index].size);
        }
        if (client->pools[index].fd >= 0)
            close(client->pools[index].fd);
    }
    for (size_t index = 0; index < client->pending_fd_count; index++)
        close(client->pending_fds[index]);
    outgoing = client->output_head;
    while (outgoing) {
        struct wl_server_outgoing *next = outgoing->next;

        if (outgoing->fd >= 0)
            close(outgoing->fd);
        free(outgoing);
        outgoing = next;
    }
    if (client->event_source) {
        (void)wl_event_source_remove(client->event_source);
        client->event_source = NULL;
    }
    if (client->dispatch_idle) {
        (void)wl_event_source_remove(client->dispatch_idle);
        client->dispatch_idle = NULL;
    }
    if (client->fd >= 0)
        close(client->fd);
    memset(client, 0, sizeof(*client));
    client->fd = -1;

    /*
     * Removing a client changes the scene even when the process disappeared
     * before destroying its Wayland objects.  Schedule this centrally so all
     * disconnect paths (hangup, SIGKILL and failed frame callback) repaint the
     * surfaces that were just removed.
     */
    if (server)
        (void)wl_server_schedule_render(server, true);
}
