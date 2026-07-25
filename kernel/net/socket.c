/*
 * ArmOS
 * Copyright (c) 2026 Mohamed Ennassiri
 *
 * Licensed under the Apache License, Version 2.0.
 * See LICENSE for details.
 *
 * File: kernel/net/socket.c
 * Layer: Kernel / network core
 *
 * Responsibilities:
 * - Implement common IPv4 TCP and UDP sockets over any registered net device.
 * - Provide active/passive TCP, bounded retransmission, flow control and close.
 * - Resolve DNS A records through the DNS server learned from DHCP.
 *
 * Notes:
 * - TCP deliberately begins with one outstanding segment. This preserves a
 *   small state machine while still honoring peer windows, ACKs and retries.
 * - Hardware drivers only exchange Ethernet frames; no transport policy is
 *   allowed in VirtIO, SDIO or platform code.
 */

#include <kernel/kprintf.h>
#include <kernel/file.h>
#include <kernel/memory.h>
#include <kernel/net/socket.h>
#include <kernel/net/stack.h>
#include <kernel/net/armos_socket.h>
#include <kernel/spinlock.h>
#include <kernel/string.h>
#include <kernel/syscalls.h>
#include <kernel/task.h>
#include <kernel/timer.h>
#include <kernel/userspace.h>
#include <kernel/vfs.h>

#define NET_AF_INET              2
#define NET_SOCK_STREAM          1
#define NET_SOCK_DGRAM           2
#define NET_IPPROTO_TCP          6u
#define NET_IPPROTO_UDP          17u
#define NET_TCP_FIN              0x01u
#define NET_TCP_SYN              0x02u
#define NET_TCP_RST              0x04u
#define NET_TCP_PSH              0x08u
#define NET_TCP_ACK              0x10u
#define NET_TCP_MSS              1200u
#define NET_TCP_RX_SIZE          8192u
#define NET_UDP_RX_SIZE          2048u
#define NET_TCP_TIMEOUT_MS       750u
#define NET_TCP_CONNECT_RETRIES  4u
#define NET_TCP_DATA_RETRIES     5u
#define NET_TCP_FIN_RETRIES      3u
#define NET_TCP_ZERO_WINDOW_MS   5000u
#define NET_TCP_HALF_OPEN_MS     10000u
#define NET_SOCKET_WAIT_MS       10u
#define NET_DNS_TIMEOUT_MS       1500u
#define NET_DNS_RETRIES          3u
#define NET_EPHEMERAL_FIRST      49152u
#define NET_EPHEMERAL_LAST       65535u

typedef struct net_sockaddr_in {
    uint16_t family;
    uint16_t port;
    uint32_t address;
    uint8_t zero[8];
} __attribute__((packed)) net_sockaddr_in_t;

typedef struct net_tcp_header {
    uint16_t source_port;
    uint16_t destination_port;
    uint32_t sequence;
    uint32_t acknowledgement;
    uint8_t data_offset;
    uint8_t flags;
    uint16_t window;
    uint16_t checksum;
    uint16_t urgent;
} __attribute__((packed)) net_tcp_header_t;

typedef struct net_udp_header {
    uint16_t source_port;
    uint16_t destination_port;
    uint16_t length;
    uint16_t checksum;
} __attribute__((packed)) net_udp_header_t;

typedef enum net_socket_state {
    NET_SOCKET_CREATED = 0,
    NET_SOCKET_BOUND,
    NET_SOCKET_LISTEN,
    NET_SOCKET_SYN_SENT,
    NET_SOCKET_SYN_RECEIVED,
    NET_SOCKET_ESTABLISHED,
    NET_SOCKET_CLOSE_WAIT,
    NET_SOCKET_FIN_WAIT,
    NET_SOCKET_CLOSED,
    NET_SOCKET_RESET,
} net_socket_state_t;

typedef struct net_socket net_socket_t;

struct net_socket {
    net_socket_t *next;
    net_socket_t *accept_next;
    net_socket_t *accept_head;
    net_socket_t *accept_tail;
    net_socket_t *listener;
    net_device_t *device;
    net_socket_state_t state;
    uint8_t type;
    uint8_t backlog;
    uint8_t pending_count;
    bool accepted;
    bool peer_closed;
    bool local_closed;
    int error;
    uint32_t local_address;
    uint32_t remote_address;
    uint16_t local_port;
    uint16_t remote_port;
    uint32_t snd_una;
    uint32_t snd_nxt;
    uint32_t rcv_nxt;
    uint32_t state_since;
    uint16_t peer_window;
    uint8_t rx[NET_TCP_RX_SIZE];
    uint32_t rx_length;
    uint32_t udp_source_address;
    uint16_t udp_source_port;
};

typedef struct net_dns_pending {
    bool active;
    bool complete;
    uint16_t identifier;
    uint16_t port;
    uint32_t result;
    int error;
} net_dns_pending_t;

static spinlock_t socket_lock = SPINLOCK_INIT("net_socket");
static net_socket_t *socket_list;
static uint16_t next_ephemeral = NET_EPHEMERAL_FIRST;
static net_dns_pending_t dns_pending;
static uint32_t tcp_rx_bytes;

extern file_t *create_file(void);
extern inode_t *create_inode(void);

static uint16_t net_be16(uint16_t value)
{
    return (uint16_t)((value << 8) | (value >> 8));
}

static uint32_t net_be32(uint32_t value)
{
    return ((value & 0x000000ffu) << 24) |
           ((value & 0x0000ff00u) << 8) |
           ((value & 0x00ff0000u) >> 8) |
           ((value & 0xff000000u) >> 24);
}

static uint32_t net_checksum_add(uint32_t sum, const void *data,
                                 uint32_t length)
{
    const uint8_t *bytes = data;

    while (length >= 2u) {
        sum += ((uint16_t)bytes[0] << 8) | bytes[1];
        bytes += 2;
        length -= 2u;
    }
    if (length != 0u)
        sum += (uint16_t)bytes[0] << 8;
    return sum;
}

static uint16_t net_transport_checksum(uint32_t source, uint32_t destination,
                                       uint8_t protocol, const void *payload,
                                       uint32_t length)
{
    uint32_t sum = 0u;

    sum += (source >> 16) & 0xffffu;
    sum += source & 0xffffu;
    sum += (destination >> 16) & 0xffffu;
    sum += destination & 0xffffu;
    sum += protocol;
    sum += length;
    sum = net_checksum_add(sum, payload, length);
    while ((sum >> 16) != 0u)
        sum = (sum & 0xffffu) + (sum >> 16);
    return (uint16_t)~sum;
}

static uint16_t net_allocate_ephemeral_locked(void)
{
    uint16_t first = next_ephemeral;

    do {
        uint16_t candidate = next_ephemeral++;
        net_socket_t *socket;
        bool used = false;

        if (next_ephemeral < NET_EPHEMERAL_FIRST)
            next_ephemeral = NET_EPHEMERAL_FIRST;
        for (socket = socket_list; socket; socket = socket->next) {
            if (socket->local_port == candidate &&
                socket->state != NET_SOCKET_CLOSED) {
                used = true;
                break;
            }
        }
        if (!used && (!dns_pending.active || dns_pending.port != candidate))
            return candidate;
    } while (next_ephemeral != first);
    return 0u;
}

static void net_socket_insert_locked(net_socket_t *socket)
{
    socket->next = socket_list;
    socket_list = socket;
}

static void net_socket_remove_locked(net_socket_t *socket)
{
    net_socket_t **link = &socket_list;

    while (*link) {
        if (*link == socket) {
            *link = socket->next;
            socket->next = NULL;
            return;
        }
        link = &(*link)->next;
    }
}

static net_socket_t *net_find_connection_locked(uint8_t type,
                                                uint32_t local_address,
                                                uint16_t local_port,
                                                uint32_t remote_address,
                                                uint16_t remote_port)
{
    net_socket_t *socket;

    for (socket = socket_list; socket; socket = socket->next) {
        if (socket->type != type || socket->local_port != local_port)
            continue;
        if (socket->local_address != 0u &&
            socket->local_address != local_address)
            continue;
        if (socket->remote_address != remote_address ||
            socket->remote_port != remote_port)
            continue;
        if (socket->state != NET_SOCKET_CLOSED)
            return socket;
    }
    return NULL;
}

static net_socket_t *net_find_listener_locked(uint32_t local_address,
                                              uint16_t local_port)
{
    net_socket_t *socket;

    for (socket = socket_list; socket; socket = socket->next) {
        if (socket->type == NET_SOCK_STREAM &&
            socket->state == NET_SOCKET_LISTEN &&
            socket->local_port == local_port &&
            (socket->local_address == 0u ||
             socket->local_address == local_address))
            return socket;
    }
    return NULL;
}

static int net_tcp_emit_endpoint(net_device_t *device, uint32_t local_address,
                                 uint16_t local_port,
                                 uint32_t remote_address,
                                 uint16_t remote_port, uint32_t rx_length,
                                 uint32_t sequence, uint32_t acknowledgement,
                                 uint8_t flags, const void *payload,
                                 uint32_t length)
{
    uint8_t packet[sizeof(net_tcp_header_t) + NET_TCP_MSS];
    net_tcp_header_t *tcp = (net_tcp_header_t *)packet;
    uint32_t packet_length = sizeof(*tcp) + length;

    if (!device || length > NET_TCP_MSS)
        return -EINVAL;
    memset(packet, 0, sizeof(*tcp));
    tcp->source_port = net_be16(local_port);
    tcp->destination_port = net_be16(remote_port);
    tcp->sequence = net_be32(sequence);
    tcp->acknowledgement = net_be32(acknowledgement);
    tcp->data_offset = (uint8_t)((sizeof(*tcp) / 4u) << 4);
    tcp->flags = flags;
    tcp->window = net_be16((uint16_t)(NET_TCP_RX_SIZE - rx_length));
    if (length != 0u)
        memcpy(packet + sizeof(*tcp), payload, length);
    tcp->checksum = net_be16(net_transport_checksum(local_address,
        remote_address, NET_IPPROTO_TCP, packet, packet_length));
    return net_stack_send_ipv4(device, remote_address,
                               NET_IPPROTO_TCP, packet, packet_length);
}

static int net_tcp_emit(net_socket_t *socket, uint32_t sequence,
                        uint32_t acknowledgement, uint8_t flags,
                        const void *payload, uint32_t length)
{
    if (!socket)
        return -EINVAL;
    return net_tcp_emit_endpoint(socket->device, socket->local_address,
                                 socket->local_port, socket->remote_address,
                                 socket->remote_port, socket->rx_length,
                                 sequence, acknowledgement, flags,
                                 payload, length);
}

static void net_tcp_reset_closed(net_device_t *device, uint32_t source,
                                 uint16_t source_port, uint32_t destination,
                                 uint16_t destination_port, uint32_t sequence,
                                 uint32_t acknowledgement, uint8_t flags,
                                 uint32_t data_length)
{
    uint8_t reply_flags = NET_TCP_RST;
    uint32_t reply_sequence = 0u;
    uint32_t reply_acknowledgement = 0u;

    if ((flags & NET_TCP_RST) != 0u)
        return;
    if ((flags & NET_TCP_ACK) != 0u) {
        reply_sequence = acknowledgement;
    } else {
        reply_flags |= NET_TCP_ACK;
        reply_acknowledgement = sequence + data_length;
        if ((flags & NET_TCP_SYN) != 0u)
            reply_acknowledgement++;
        if ((flags & NET_TCP_FIN) != 0u)
            reply_acknowledgement++;
    }
    (void)net_tcp_emit_endpoint(device, destination, destination_port,
                                source, source_port, 0u, reply_sequence,
                                reply_acknowledgement, reply_flags, NULL, 0u);
}

static int net_udp_emit(net_device_t *device, uint32_t source,
                        uint16_t source_port, uint32_t destination,
                        uint16_t destination_port, const void *payload,
                        uint32_t length)
{
    uint8_t packet[sizeof(net_udp_header_t) + NET_UDP_RX_SIZE];
    net_udp_header_t *udp = (net_udp_header_t *)packet;
    uint32_t packet_length = sizeof(*udp) + length;
    uint16_t checksum;

    if (!device || !payload || length > NET_UDP_RX_SIZE)
        return -EMSGSIZE;
    memset(udp, 0, sizeof(*udp));
    udp->source_port = net_be16(source_port);
    udp->destination_port = net_be16(destination_port);
    udp->length = net_be16((uint16_t)packet_length);
    memcpy(packet + sizeof(*udp), payload, length);
    checksum = net_transport_checksum(source, destination, NET_IPPROTO_UDP,
                                      packet, packet_length);
    udp->checksum = net_be16(checksum == 0u ? 0xffffu : checksum);
    return net_stack_send_ipv4(device, destination, NET_IPPROTO_UDP,
                               packet, packet_length);
}

static int net_wait_for_state(net_socket_t *socket,
                              net_socket_state_t state,
                              uint32_t timeout_ms)
{
    uint32_t start = get_time_ms();

    while ((uint32_t)(get_time_ms() - start) < timeout_ms) {
        if (socket->state == state)
            return 0;
        if (socket->state == NET_SOCKET_RESET)
            return socket->error != 0 ? socket->error : -ECONNRESET;
        if (socket->state == NET_SOCKET_CLOSED)
            return socket->error != 0 ? socket->error : -ECONNABORTED;
        if (task_sleep_interruptible_ms(NET_SOCKET_WAIT_MS) < 0)
            return -EINTR;
    }
    return -ETIMEDOUT;
}

static int net_wait_for_ack(net_socket_t *socket, uint32_t expected,
                            uint32_t timeout_ms)
{
    uint32_t start = get_time_ms();

    while ((uint32_t)(get_time_ms() - start) < timeout_ms) {
        if ((int32_t)(socket->snd_una - expected) >= 0)
            return 0;
        if (socket->state == NET_SOCKET_RESET)
            return socket->error != 0 ? socket->error : -ECONNRESET;
        if (task_sleep_interruptible_ms(NET_SOCKET_WAIT_MS) < 0)
            return -EINTR;
    }
    return -ETIMEDOUT;
}

static int net_tcp_send_fin(net_socket_t *socket)
{
    uint32_t sequence;
    uint32_t retry;
    int ret = -ETIMEDOUT;

    if (socket->local_closed)
        return 0;
    sequence = socket->snd_nxt++;
    socket->local_closed = true;
    socket->state = NET_SOCKET_FIN_WAIT;
    socket->state_since = get_time_ms();
    for (retry = 0u; retry < NET_TCP_FIN_RETRIES; retry++) {
        ret = net_tcp_emit(socket, sequence, socket->rcv_nxt,
                           NET_TCP_FIN | NET_TCP_ACK, NULL, 0u);
        if (ret < 0)
            continue;
        ret = net_wait_for_ack(socket, socket->snd_nxt,
                               NET_TCP_TIMEOUT_MS);
        if (ret == 0)
            break;
        if (ret == -EINTR)
            return ret;
    }
    return ret;
}

static void net_tcp_queue_payload_locked(net_socket_t *socket,
                                         const uint8_t *payload,
                                         uint32_t length)
{
    uint32_t space = NET_TCP_RX_SIZE - socket->rx_length;

    if (length > space)
        length = space;
    if (length != 0u) {
        memcpy(socket->rx + socket->rx_length, payload, length);
        socket->rx_length += length;
        tcp_rx_bytes += length;
    }
}

static bool net_tcp_receive(net_device_t *device, uint32_t source,
                            uint32_t destination, const uint8_t *payload,
                            uint32_t length)
{
    const net_tcp_header_t *tcp;
    net_socket_t *socket;
    net_socket_t *listener;
    uint32_t sequence;
    uint32_t acknowledgement;
    uint32_t header_length;
    uint32_t data_length;
    uint16_t source_port;
    uint16_t destination_port;
    uint8_t reply_flags = 0u;
    uint32_t reply_sequence = 0u;
    uint32_t reply_acknowledgement = 0u;
    uint32_t reply_local_address = 0u;
    uint32_t reply_remote_address = 0u;
    uint32_t reply_rx_length = 0u;
    uint16_t reply_local_port = 0u;
    uint16_t reply_remote_port = 0u;
    unsigned long irq_flags;

    if (length < sizeof(*tcp) ||
        net_transport_checksum(source, destination, NET_IPPROTO_TCP,
                               payload, length) != 0u)
        return true;
    tcp = (const net_tcp_header_t *)payload;
    header_length = (uint32_t)(tcp->data_offset >> 4) * 4u;
    if (header_length < sizeof(*tcp) || header_length > length)
        return true;
    source_port = net_be16(tcp->source_port);
    destination_port = net_be16(tcp->destination_port);
    sequence = net_be32(tcp->sequence);
    acknowledgement = net_be32(tcp->acknowledgement);
    data_length = length - header_length;

    spin_lock_irqsave(&socket_lock, &irq_flags);
    socket = net_find_connection_locked(NET_SOCK_STREAM, destination,
                                        destination_port, source, source_port);
    if (socket) {
        socket->peer_window = net_be16(tcp->window);
        if ((tcp->flags & NET_TCP_RST) != 0u) {
            socket->error = -ECONNRESET;
            socket->state = NET_SOCKET_RESET;
            spin_unlock_irqrestore(&socket_lock, irq_flags);
            return true;
        }
        if ((tcp->flags & NET_TCP_ACK) != 0u &&
            (int32_t)(acknowledgement - socket->snd_una) > 0 &&
            (int32_t)(socket->snd_nxt - acknowledgement) >= 0)
            socket->snd_una = acknowledgement;
        if (socket->state == NET_SOCKET_SYN_SENT &&
            (tcp->flags & (NET_TCP_SYN | NET_TCP_ACK)) ==
                (NET_TCP_SYN | NET_TCP_ACK) &&
            acknowledgement == socket->snd_nxt) {
            socket->rcv_nxt = sequence + 1u;
            socket->snd_una = acknowledgement;
            socket->state = NET_SOCKET_ESTABLISHED;
            socket->state_since = get_time_ms();
            reply_flags = NET_TCP_ACK;
            reply_sequence = socket->snd_nxt;
            reply_acknowledgement = socket->rcv_nxt;
        } else if (socket->state == NET_SOCKET_SYN_RECEIVED &&
                   (tcp->flags & NET_TCP_ACK) != 0u &&
                   acknowledgement == socket->snd_nxt) {
            socket->snd_una = acknowledgement;
            socket->state = NET_SOCKET_ESTABLISHED;
            socket->state_since = get_time_ms();
            listener = socket->listener;
            if (listener && !socket->accepted &&
                listener->pending_count < listener->backlog) {
                if (listener->accept_tail)
                    listener->accept_tail->accept_next = socket;
                else
                    listener->accept_head = socket;
                listener->accept_tail = socket;
                listener->pending_count++;
            }
        }
        if (data_length != 0u && sequence == socket->rcv_nxt) {
            net_tcp_queue_payload_locked(socket, payload + header_length,
                                         data_length);
            socket->rcv_nxt += data_length;
            reply_flags = NET_TCP_ACK;
            reply_sequence = socket->snd_nxt;
            reply_acknowledgement = socket->rcv_nxt;
        }
        if ((tcp->flags & NET_TCP_FIN) != 0u &&
            sequence + data_length == socket->rcv_nxt) {
            socket->rcv_nxt++;
            socket->peer_closed = true;
            if (socket->state == NET_SOCKET_ESTABLISHED)
                socket->state = NET_SOCKET_CLOSE_WAIT;
            else if (socket->state == NET_SOCKET_FIN_WAIT)
                socket->state = NET_SOCKET_CLOSED;
            socket->state_since = get_time_ms();
            reply_flags = NET_TCP_ACK;
            reply_sequence = socket->snd_nxt;
            reply_acknowledgement = socket->rcv_nxt;
        }
        if (reply_flags != 0u) {
            reply_local_address = socket->local_address;
            reply_remote_address = socket->remote_address;
            reply_local_port = socket->local_port;
            reply_remote_port = socket->remote_port;
            reply_rx_length = socket->rx_length;
        }
        spin_unlock_irqrestore(&socket_lock, irq_flags);
        if (reply_flags != 0u)
            (void)net_tcp_emit_endpoint(device, reply_local_address,
                reply_local_port, reply_remote_address, reply_remote_port,
                reply_rx_length, reply_sequence, reply_acknowledgement,
                reply_flags, NULL, 0u);
        return true;
    }

    listener = net_find_listener_locked(destination, destination_port);
    if (!listener || (tcp->flags & NET_TCP_SYN) == 0u ||
        listener->pending_count >= listener->backlog) {
        spin_unlock_irqrestore(&socket_lock, irq_flags);
        net_tcp_reset_closed(device, source, source_port, destination,
                             destination_port, sequence, acknowledgement,
                             tcp->flags, data_length);
        return true;
    }
    spin_unlock_irqrestore(&socket_lock, irq_flags);

    socket = kmalloc(sizeof(*socket));
    if (!socket)
        return true;
    memset(socket, 0, sizeof(*socket));
    socket->type = NET_SOCK_STREAM;
    socket->device = device;
    socket->local_address = destination;
    socket->remote_address = source;
    socket->local_port = destination_port;
    socket->remote_port = source_port;
    socket->rcv_nxt = sequence + 1u;
    socket->snd_una = 0x41524d00u ^ get_time_ms();
    socket->snd_nxt = socket->snd_una + 1u;
    socket->peer_window = net_be16(tcp->window);
    socket->state = NET_SOCKET_SYN_RECEIVED;
    socket->state_since = get_time_ms();
    socket->listener = listener;

    spin_lock_irqsave(&socket_lock, &irq_flags);
    if (net_find_connection_locked(NET_SOCK_STREAM, destination,
                                   destination_port, source, source_port)) {
        spin_unlock_irqrestore(&socket_lock, irq_flags);
        kfree(socket);
        return true;
    }
    net_socket_insert_locked(socket);
    spin_unlock_irqrestore(&socket_lock, irq_flags);
    (void)net_tcp_emit(socket, socket->snd_una, socket->rcv_nxt,
                       NET_TCP_SYN | NET_TCP_ACK, NULL, 0u);
    return true;
}

static uint32_t net_dns_skip_name(const uint8_t *message, uint32_t length,
                                  uint32_t offset)
{
    uint32_t labels = 0u;

    while (offset < length && labels++ < 128u) {
        uint8_t span = message[offset++];

        if (span == 0u)
            return offset;
        if ((span & 0xc0u) == 0xc0u)
            return offset < length ? offset + 1u : 0u;
        if ((span & 0xc0u) != 0u || span > length - offset)
            return 0u;
        offset += span;
    }
    return 0u;
}

static bool net_dns_receive(uint16_t destination_port, const uint8_t *payload,
                            uint32_t length)
{
    uint16_t identifier;
    uint16_t flags;
    uint16_t questions;
    uint16_t answers;
    uint32_t offset = 12u;
    unsigned long irq_flags;

    if (length < 12u)
        return false;
    identifier = ((uint16_t)payload[0] << 8) | payload[1];
    spin_lock_irqsave(&socket_lock, &irq_flags);
    if (!dns_pending.active || dns_pending.port != destination_port ||
        dns_pending.identifier != identifier) {
        spin_unlock_irqrestore(&socket_lock, irq_flags);
        return false;
    }
    flags = ((uint16_t)payload[2] << 8) | payload[3];
    questions = ((uint16_t)payload[4] << 8) | payload[5];
    answers = ((uint16_t)payload[6] << 8) | payload[7];
    spin_unlock_irqrestore(&socket_lock, irq_flags);
    if ((flags & 0x8000u) == 0u)
        return true;
    while (questions-- != 0u) {
        offset = net_dns_skip_name(payload, length, offset);
        if (offset == 0u || offset + 4u > length)
            goto malformed;
        offset += 4u;
    }
    while (answers-- != 0u) {
        uint16_t type;
        uint16_t klass;
        uint16_t data_length;

        offset = net_dns_skip_name(payload, length, offset);
        if (offset == 0u || offset + 10u > length)
            goto malformed;
        type = ((uint16_t)payload[offset] << 8) | payload[offset + 1u];
        klass = ((uint16_t)payload[offset + 2u] << 8) |
                payload[offset + 3u];
        data_length = ((uint16_t)payload[offset + 8u] << 8) |
                      payload[offset + 9u];
        offset += 10u;
        if (data_length > length - offset)
            goto malformed;
        if (type == 1u && klass == 1u && data_length == 4u) {
            uint32_t result = ((uint32_t)payload[offset] << 24) |
                ((uint32_t)payload[offset + 1u] << 16) |
                ((uint32_t)payload[offset + 2u] << 8) |
                payload[offset + 3u];

            spin_lock_irqsave(&socket_lock, &irq_flags);
            dns_pending.result = result;
            dns_pending.error = 0;
            dns_pending.complete = true;
            spin_unlock_irqrestore(&socket_lock, irq_flags);
            return true;
        }
        offset += data_length;
    }
malformed:
    spin_lock_irqsave(&socket_lock, &irq_flags);
    dns_pending.error = -ENOENT;
    dns_pending.complete = true;
    spin_unlock_irqrestore(&socket_lock, irq_flags);
    return true;
}

static bool net_udp_receive(net_device_t *device, uint32_t source,
                            uint32_t destination, const uint8_t *payload,
                            uint32_t length)
{
    const net_udp_header_t *udp;
    net_socket_t *socket;
    uint16_t source_port;
    uint16_t destination_port;
    uint32_t udp_length;
    unsigned long irq_flags;

    (void)device;
    if (length < sizeof(*udp))
        return true;
    udp = (const net_udp_header_t *)payload;
    udp_length = net_be16(udp->length);
    if (udp_length < sizeof(*udp) || udp_length > length)
        return true;
    if (udp->checksum != 0u &&
        net_transport_checksum(source, destination, NET_IPPROTO_UDP,
                               payload, udp_length) != 0u)
        return true;
    source_port = net_be16(udp->source_port);
    destination_port = net_be16(udp->destination_port);
    payload += sizeof(*udp);
    udp_length -= sizeof(*udp);
    if (source_port == 53u &&
        net_dns_receive(destination_port, payload, udp_length))
        return true;

    spin_lock_irqsave(&socket_lock, &irq_flags);
    for (socket = socket_list; socket; socket = socket->next) {
        if (socket->type != NET_SOCK_DGRAM ||
            socket->local_port != destination_port)
            continue;
        if (socket->remote_address != 0u &&
            (socket->remote_address != source ||
             socket->remote_port != source_port))
            continue;
        if (socket->rx_length == 0u) {
            if (udp_length > NET_UDP_RX_SIZE)
                udp_length = NET_UDP_RX_SIZE;
            memcpy(socket->rx, payload, udp_length);
            socket->rx_length = udp_length;
            socket->udp_source_address = source;
            socket->udp_source_port = source_port;
        }
        spin_unlock_irqrestore(&socket_lock, irq_flags);
        return true;
    }
    spin_unlock_irqrestore(&socket_lock, irq_flags);
    return false;
}

bool net_transport_receive(net_device_t *device, uint8_t protocol,
                           uint32_t source, uint32_t destination,
                           const uint8_t *payload, uint32_t length)
{
    if (protocol == NET_IPPROTO_TCP)
        return net_tcp_receive(device, source, destination, payload, length);
    if (protocol == NET_IPPROTO_UDP)
        return net_udp_receive(device, source, destination, payload, length);
    return false;
}

void net_transport_tick(uint32_t now_ms)
{
    net_socket_t **link;
    net_socket_t *expired = NULL;
    unsigned long irq_flags;

    spin_lock_irqsave(&socket_lock, &irq_flags);
    link = &socket_list;
    while (*link) {
        net_socket_t *socket = *link;

        if (socket->state == NET_SOCKET_SYN_RECEIVED &&
            !socket->accepted &&
            (uint32_t)(now_ms - socket->state_since) >=
                NET_TCP_HALF_OPEN_MS) {
            *link = socket->next;
            socket->next = expired;
            expired = socket;
            continue;
        }
        link = &socket->next;
    }
    spin_unlock_irqrestore(&socket_lock, irq_flags);
    while (expired) {
        net_socket_t *next = expired->next;

        kfree(expired);
        expired = next;
    }
}

static ssize_t net_socket_read(file_t *file, void *buffer, size_t count)
{
    net_socket_t *socket = file ? file->private_data : NULL;
    uint32_t length;
    unsigned long irq_flags;

    if (!socket || !buffer)
        return -EINVAL;
    while (socket->rx_length == 0u && !socket->peer_closed &&
           socket->state != NET_SOCKET_RESET) {
        if (task_sleep_interruptible_ms(NET_SOCKET_WAIT_MS) < 0)
            return -EINTR;
    }
    if (socket->state == NET_SOCKET_RESET)
        return socket->error != 0 ? socket->error : -ECONNRESET;
    if (socket->rx_length == 0u && socket->peer_closed)
        return 0;
    spin_lock_irqsave(&socket_lock, &irq_flags);
    length = socket->rx_length;
    if (length > count)
        length = (uint32_t)count;
    memcpy(buffer, socket->rx, length);
    if (length < socket->rx_length)
        memmove(socket->rx, socket->rx + length,
                socket->rx_length - length);
    socket->rx_length -= length;
    spin_unlock_irqrestore(&socket_lock, irq_flags);
    return (ssize_t)length;
}

static ssize_t net_tcp_write(net_socket_t *socket, const uint8_t *buffer,
                             size_t count)
{
    size_t written = 0u;

    while (written < count) {
        uint32_t segment = (uint32_t)(count - written);
        uint32_t window_start;
        uint32_t sequence;
        uint32_t expected;
        uint32_t retry;
        int ret = -ETIMEDOUT;

        window_start = get_time_ms();
        while (socket->peer_window == 0u && !socket->peer_closed &&
               socket->state != NET_SOCKET_RESET &&
               (uint32_t)(get_time_ms() - window_start) <
                   NET_TCP_ZERO_WINDOW_MS) {
            if (task_sleep_interruptible_ms(NET_SOCKET_WAIT_MS) < 0)
                return written != 0u ? (ssize_t)written : -EINTR;
        }
        if (socket->state == NET_SOCKET_RESET)
            return written != 0u ? (ssize_t)written :
                (socket->error != 0 ? socket->error : -ECONNRESET);
        if (socket->peer_window == 0u)
            return written != 0u ? (ssize_t)written : -ETIMEDOUT;
        if (socket->peer_closed)
            return written != 0u ? (ssize_t)written : -EPIPE;
        if (segment > NET_TCP_MSS)
            segment = NET_TCP_MSS;
        if (segment > socket->peer_window)
            segment = socket->peer_window;
        sequence = socket->snd_nxt;
        socket->snd_nxt += segment;
        expected = socket->snd_nxt;
        for (retry = 0u; retry < NET_TCP_DATA_RETRIES; retry++) {
            ret = net_tcp_emit(socket, sequence, socket->rcv_nxt,
                               NET_TCP_ACK | NET_TCP_PSH,
                               buffer + written, segment);
            if (ret < 0)
                continue;
            ret = net_wait_for_ack(socket, expected, NET_TCP_TIMEOUT_MS);
            if (ret == 0)
                break;
            if (ret == -EINTR)
                return written != 0u ? (ssize_t)written : ret;
        }
        if (ret < 0) {
            socket->error = ret;
            socket->state = NET_SOCKET_RESET;
            return written != 0u ? (ssize_t)written : ret;
        }
        written += segment;
    }
    return (ssize_t)written;
}

static ssize_t net_socket_write(file_t *file, const void *buffer, size_t count)
{
    net_socket_t *socket = file ? file->private_data : NULL;

    if (!socket || (!buffer && count != 0u))
        return -EINVAL;
    if (count == 0u)
        return 0;
    if (socket->type == NET_SOCK_STREAM) {
        if (socket->state != NET_SOCKET_ESTABLISHED &&
            socket->state != NET_SOCKET_CLOSE_WAIT)
            return -ENOTCONN;
        return net_tcp_write(socket, buffer, count);
    }
    if (socket->remote_address == 0u || socket->remote_port == 0u)
        return -EDESTADDRREQ;
    if (count > NET_UDP_RX_SIZE)
        return -EMSGSIZE;
    if (net_udp_emit(socket->device, socket->local_address,
                     socket->local_port, socket->remote_address,
                     socket->remote_port, buffer, (uint32_t)count) < 0)
        return -EIO;
    return (ssize_t)count;
}

static int net_socket_close(file_t *file)
{
    net_socket_t *socket = file ? file->private_data : NULL;
    net_socket_t *garbage = NULL;
    net_socket_t **link;
    unsigned long irq_flags;

    if (!socket)
        return 0;
    if (socket->type == NET_SOCK_STREAM &&
        (socket->state == NET_SOCKET_ESTABLISHED ||
         socket->state == NET_SOCKET_CLOSE_WAIT) &&
        !socket->local_closed)
        (void)net_tcp_send_fin(socket);
    spin_lock_irqsave(&socket_lock, &irq_flags);
    link = &socket_list;
    while (*link) {
        net_socket_t *child = *link;

        if (child != socket && child->listener == socket &&
            !child->accepted) {
            *link = child->next;
            child->next = garbage;
            garbage = child;
            continue;
        }
        link = &child->next;
    }
    net_socket_remove_locked(socket);
    socket->state = NET_SOCKET_CLOSED;
    spin_unlock_irqrestore(&socket_lock, irq_flags);
    while (garbage) {
        net_socket_t *next = garbage->next;

        kfree(garbage);
        garbage = next;
    }
    kfree(socket);
    file->private_data = NULL;
    return 0;
}

static off_t net_socket_lseek(file_t *file, off_t offset, int whence)
{
    (void)file;
    (void)offset;
    (void)whence;
    return -ESPIPE;
}

static file_operations_t net_socket_file_ops = {
    .read = net_socket_read,
    .write = net_socket_write,
    .open = NULL,
    .close = net_socket_close,
    .lseek = net_socket_lseek,
    .readdir = NULL,
    .truncate = NULL,
};

static file_t *net_socket_create_file(net_socket_t *socket)
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
    inode->mode = S_IFSOCK | 0666;
    inode->uid = current_uid();
    inode->gid = current_gid();
    inode->nlink = 1u;
    inode->atime = now;
    inode->mtime = now;
    inode->ctime = now;
    inode->f_op = &net_socket_file_ops;
    file->f_op = &net_socket_file_ops;
    file->flags = O_RDWR;
    file->type = FILE_TYPE_SOCKET;
    file->inode = inode;
    file->private_data = socket;
    strncpy(file->name, "socket", sizeof(file->name) - 1u);
    file->name[sizeof(file->name) - 1u] = '\0';
    return file;
}

static net_socket_t *net_socket_from_fd(int fd)
{
    task_t *task = task_current_local();
    file_t *file;

    if (!task || !task->process || fd < 0 || fd >= MAX_FILES)
        return NULL;
    file = task->process->files[fd];
    if (!file || file->type != FILE_TYPE_SOCKET)
        return NULL;
    return file->private_data;
}

static int net_socket_install(net_socket_t *socket)
{
    task_t *task = task_current_local();
    file_t *file;
    int fd;

    if (!task || !task->process)
        return -ENODEV;
    file = net_socket_create_file(socket);
    if (!file)
        return -ENOMEM;
    fd = allocate_fd(task);
    if (fd < 0) {
        file->private_data = NULL;
        close_file(file);
        return fd;
    }
    task->process->files[fd] = file;
    task->process->fd_flags[fd] = 0u;
    return fd;
}

int sys_socket(int domain, int type, int protocol)
{
    net_socket_t *socket;
    net_device_t *device;
    net_ipv4_config_t config;
    unsigned long irq_flags;
    int fd;

    if (domain == ARMOS_AF_LOCAL)
        return armos_socket_create(type, protocol);
    if (domain != NET_AF_INET ||
        (type != NET_SOCK_STREAM && type != NET_SOCK_DGRAM) ||
        (protocol != 0 &&
         protocol != (int)(type == NET_SOCK_STREAM ? NET_IPPROTO_TCP :
                                                     NET_IPPROTO_UDP)))
        return -EPROTONOSUPPORT;
    device = net_device_get_default();
    if (!device || net_stack_get_config(device, &config) < 0)
        return -ENODEV;
    socket = kmalloc(sizeof(*socket));
    if (!socket)
        return -ENOMEM;
    memset(socket, 0, sizeof(*socket));
    socket->type = (uint8_t)type;
    socket->device = device;
    socket->local_address = config.address;
    socket->state = NET_SOCKET_CREATED;
    socket->state_since = get_time_ms();
    socket->peer_window = 65535u;
    spin_lock_irqsave(&socket_lock, &irq_flags);
    net_socket_insert_locked(socket);
    spin_unlock_irqrestore(&socket_lock, irq_flags);
    fd = net_socket_install(socket);
    if (fd < 0) {
        spin_lock_irqsave(&socket_lock, &irq_flags);
        net_socket_remove_locked(socket);
        spin_unlock_irqrestore(&socket_lock, irq_flags);
        kfree(socket);
    }
    return fd;
}

int sys_bind(int fd, const void *address, uint32_t address_length)
{
    if (armos_socket_is_fd(fd))
        return armos_socket_bind(fd, address, address_length);
    net_socket_t *socket = net_socket_from_fd(fd);
    net_sockaddr_in_t input;
    uint16_t port;
    uint32_t local_address;
    net_socket_t *other;
    unsigned long irq_flags;

    if (!socket)
        return -EBADF;
    if (!address || address_length < sizeof(input))
        return -EINVAL;
    if (copy_from_user(&input, address, sizeof(input)) < 0)
        return -EFAULT;
    if (input.family != NET_AF_INET)
        return -EAFNOSUPPORT;
    port = net_be16(input.port);
    local_address = net_be32(input.address);
    if (port == 0u) {
        spin_lock_irqsave(&socket_lock, &irq_flags);
        port = net_allocate_ephemeral_locked();
        spin_unlock_irqrestore(&socket_lock, irq_flags);
        if (port == 0u)
            return -EADDRNOTAVAIL;
    }
    spin_lock_irqsave(&socket_lock, &irq_flags);
    for (other = socket_list; other; other = other->next) {
        if (other != socket && other->type == socket->type &&
            other->local_port == port &&
            other->state != NET_SOCKET_CLOSED) {
            spin_unlock_irqrestore(&socket_lock, irq_flags);
            return -EADDRINUSE;
        }
    }
    socket->local_port = port;
    if (local_address != 0u)
        socket->local_address = local_address;
    socket->state = NET_SOCKET_BOUND;
    spin_unlock_irqrestore(&socket_lock, irq_flags);
    return 0;
}

int sys_connect(int fd, const void *address, uint32_t address_length)
{
    if (armos_socket_is_fd(fd))
        return armos_socket_connect(fd, address, address_length);
    net_socket_t *socket = net_socket_from_fd(fd);
    net_sockaddr_in_t input;
    unsigned long irq_flags;
    uint32_t retry;
    int ret = -ETIMEDOUT;

    if (!socket)
        return -EBADF;
    if (!address || address_length < sizeof(input))
        return -EINVAL;
    if (copy_from_user(&input, address, sizeof(input)) < 0)
        return -EFAULT;
    if (input.family != NET_AF_INET)
        return -EAFNOSUPPORT;
    socket->remote_address = net_be32(input.address);
    socket->remote_port = net_be16(input.port);
    if (socket->remote_address == 0u || socket->remote_port == 0u)
        return -EDESTADDRREQ;
    spin_lock_irqsave(&socket_lock, &irq_flags);
    if (socket->local_port == 0u)
        socket->local_port = net_allocate_ephemeral_locked();
    spin_unlock_irqrestore(&socket_lock, irq_flags);
    if (socket->local_port == 0u)
        return -EADDRNOTAVAIL;
    if (socket->type == NET_SOCK_DGRAM) {
        socket->state = NET_SOCKET_ESTABLISHED;
        socket->state_since = get_time_ms();
        return 0;
    }
    socket->snd_una = 0x41524d00u ^ get_time_ms() ^
                      ((uint32_t)socket->local_port << 8);
    socket->snd_nxt = socket->snd_una + 1u;
    socket->state = NET_SOCKET_SYN_SENT;
    socket->state_since = get_time_ms();
    for (retry = 0u; retry < NET_TCP_CONNECT_RETRIES; retry++) {
        ret = net_tcp_emit(socket, socket->snd_una, 0u, NET_TCP_SYN,
                           NULL, 0u);
        if (ret < 0)
            continue;
        ret = net_wait_for_state(socket, NET_SOCKET_ESTABLISHED,
                                 NET_TCP_TIMEOUT_MS);
        if (ret == 0)
            return 0;
        if (ret == -EINTR)
            break;
    }
    socket->error = ret;
    socket->state = NET_SOCKET_CLOSED;
    socket->state_since = get_time_ms();
    return ret;
}

int sys_listen(int fd, int backlog)
{
    if (armos_socket_is_fd(fd))
        return armos_socket_listen(fd, backlog);
    net_socket_t *socket = net_socket_from_fd(fd);

    if (!socket)
        return -EBADF;
    if (socket->type != NET_SOCK_STREAM ||
        socket->state != NET_SOCKET_BOUND)
        return -EINVAL;
    if (backlog < 1)
        backlog = 1;
    if (backlog > 8)
        backlog = 8;
    socket->backlog = (uint8_t)backlog;
    socket->state = NET_SOCKET_LISTEN;
    socket->state_since = get_time_ms();
    return 0;
}

int sys_accept(int fd, void *address, uint32_t *address_length)
{
    if (armos_socket_is_fd(fd))
        return armos_socket_accept(fd, address, address_length);
    net_socket_t *listener = net_socket_from_fd(fd);
    net_socket_t *socket;
    net_sockaddr_in_t output;
    uint32_t user_length;
    unsigned long irq_flags;
    int accepted_fd;

    if (!listener || listener->state != NET_SOCKET_LISTEN)
        return -EINVAL;
    while (listener->accept_head == NULL) {
        if (task_sleep_interruptible_ms(NET_SOCKET_WAIT_MS) < 0)
            return -EINTR;
    }
    spin_lock_irqsave(&socket_lock, &irq_flags);
    socket = listener->accept_head;
    listener->accept_head = socket->accept_next;
    if (!listener->accept_head)
        listener->accept_tail = NULL;
    listener->pending_count--;
    socket->accept_next = NULL;
    socket->accepted = true;
    spin_unlock_irqrestore(&socket_lock, irq_flags);
    accepted_fd = net_socket_install(socket);
    if (accepted_fd < 0) {
        spin_lock_irqsave(&socket_lock, &irq_flags);
        net_socket_remove_locked(socket);
        socket->state = NET_SOCKET_CLOSED;
        spin_unlock_irqrestore(&socket_lock, irq_flags);
        kfree(socket);
        return accepted_fd;
    }
    if (address && address_length) {
        if (copy_from_user(&user_length, address_length,
                           sizeof(user_length)) < 0) {
            (void)sys_close(accepted_fd);
            return -EFAULT;
        }
        if (user_length >= sizeof(output)) {
            memset(&output, 0, sizeof(output));
            output.family = NET_AF_INET;
            output.port = net_be16(socket->remote_port);
            output.address = net_be32(socket->remote_address);
            user_length = sizeof(output);
            if (copy_to_user(address, &output, sizeof(output)) < 0 ||
                copy_to_user(address_length, &user_length,
                             sizeof(user_length)) < 0) {
                (void)sys_close(accepted_fd);
                return -EFAULT;
            }
        }
    }
    return accepted_fd;
}

ssize_t sys_sendto(int fd, const void *buffer, size_t length, int flags,
                   const void *address, uint32_t address_length)
{
    if (armos_socket_is_fd(fd))
        return armos_socket_send(fd, buffer, length, flags, address,
                                address_length);
    net_socket_t *socket = net_socket_from_fd(fd);
    net_sockaddr_in_t destination;
    uint8_t *copy;
    uint32_t destination_address;
    uint16_t destination_port;
    unsigned long irq_flags;
    int ret;

    if (!socket)
        return -EBADF;
    if (flags != 0)
        return -ENOTSUP;
    if (socket->type == NET_SOCK_STREAM)
        return sys_write(fd, buffer, length);
    if (!buffer || length > NET_UDP_RX_SIZE)
        return -EMSGSIZE;
    destination_address = socket->remote_address;
    destination_port = socket->remote_port;
    if (address) {
        if (address_length < sizeof(destination) ||
            copy_from_user(&destination, address, sizeof(destination)) < 0)
            return -EFAULT;
        if (destination.family != NET_AF_INET)
            return -EAFNOSUPPORT;
        destination_address = net_be32(destination.address);
        destination_port = net_be16(destination.port);
    }
    if (destination_address == 0u || destination_port == 0u)
        return -EDESTADDRREQ;
    spin_lock_irqsave(&socket_lock, &irq_flags);
    if (socket->local_port == 0u)
        socket->local_port = net_allocate_ephemeral_locked();
    spin_unlock_irqrestore(&socket_lock, irq_flags);
    copy = kmalloc(length);
    if (!copy)
        return -ENOMEM;
    if (copy_from_user(copy, buffer, length) < 0) {
        kfree(copy);
        return -EFAULT;
    }
    ret = net_udp_emit(socket->device, socket->local_address,
                       socket->local_port, destination_address,
                       destination_port, copy, (uint32_t)length);
    kfree(copy);
    return ret < 0 ? ret : (ssize_t)length;
}

ssize_t sys_recvfrom(int fd, void *buffer, size_t length, int flags,
                     void *address, uint32_t *address_length)
{
    if (armos_socket_is_fd(fd))
        return armos_socket_receive(fd, buffer, length, flags, address,
                                   address_length);
    net_socket_t *socket = net_socket_from_fd(fd);
    net_sockaddr_in_t source;
    uint32_t user_length;
    uint8_t *copy;
    ssize_t received;

    if (!socket)
        return -EBADF;
    if (flags != 0)
        return -ENOTSUP;
    if (socket->type == NET_SOCK_STREAM)
        return sys_read(fd, buffer, length);
    if (!buffer)
        return -EFAULT;
    copy = kmalloc(length);
    if (!copy)
        return -ENOMEM;
    received = net_socket_read(task_current_local()->process->files[fd],
                               copy, length);
    if (received > 0 &&
        copy_to_user(buffer, copy, (size_t)received) < 0)
        received = -EFAULT;
    kfree(copy);
    if (received >= 0 && address && address_length) {
        if (copy_from_user(&user_length, address_length,
                           sizeof(user_length)) < 0)
            return -EFAULT;
        if (user_length >= sizeof(source)) {
            memset(&source, 0, sizeof(source));
            source.family = NET_AF_INET;
            source.port = net_be16(socket->udp_source_port);
            source.address = net_be32(socket->udp_source_address);
            user_length = sizeof(source);
            if (copy_to_user(address, &source, sizeof(source)) < 0 ||
                copy_to_user(address_length, &user_length,
                             sizeof(user_length)) < 0)
                return -EFAULT;
        }
    }
    return received;
}

int sys_socket_shutdown(int fd, int how)
{
    if (armos_socket_is_fd(fd))
        return armos_socket_shutdown(fd, how);
    net_socket_t *socket = net_socket_from_fd(fd);

    if (!socket)
        return -EBADF;
    if (how < 0 || how > 2)
        return -EINVAL;
    if ((how == 1 || how == 2) && socket->type == NET_SOCK_STREAM &&
        !socket->local_closed)
        return net_tcp_send_fin(socket);
    if (how == 0 || how == 2)
        socket->peer_closed = true;
    return 0;
}

static int net_dns_build_query(const char *name, uint16_t identifier,
                               uint8_t *packet, uint32_t capacity)
{
    const char *label = name;
    uint32_t length = 12u;

    if (!name || !packet || capacity < 18u)
        return -EINVAL;
    memset(packet, 0, capacity);
    packet[0] = (uint8_t)(identifier >> 8);
    packet[1] = (uint8_t)identifier;
    packet[2] = 0x01u;
    packet[5] = 0x01u;
    while (*label != '\0') {
        const char *end = label;
        uint32_t span;

        while (*end != '\0' && *end != '.')
            end++;
        span = (uint32_t)(end - label);
        if (span == 0u || span > 63u || length + span + 6u > capacity)
            return -EINVAL;
        packet[length++] = (uint8_t)span;
        memcpy(packet + length, label, span);
        length += span;
        label = *end == '.' ? end + 1 : end;
    }
    packet[length++] = 0u;
    packet[length++] = 0u;
    packet[length++] = 1u;
    packet[length++] = 0u;
    packet[length++] = 1u;
    return (int)length;
}

int net_dns_resolve(const char *name, uint32_t *address)
{
    net_device_t *device = net_device_get_default();
    net_ipv4_config_t config;
    uint8_t packet[300];
    uint16_t identifier;
    uint16_t port;
    uint32_t retry;
    unsigned long irq_flags;
    int packet_length;
    int ret = -ETIMEDOUT;

    if (!name || !address)
        return -EINVAL;
    if (net_stack_parse_ipv4(name, address) == 0)
        return 0;
    if (!device || net_stack_get_config(device, &config) < 0 ||
        !config.configured || config.dns == 0u)
        return -ENETDOWN;
    spin_lock_irqsave(&socket_lock, &irq_flags);
    if (dns_pending.active) {
        spin_unlock_irqrestore(&socket_lock, irq_flags);
        return -EBUSY;
    }
    identifier = (uint16_t)(0x4100u ^ get_time_ms());
    port = net_allocate_ephemeral_locked();
    memset(&dns_pending, 0, sizeof(dns_pending));
    dns_pending.active = true;
    dns_pending.identifier = identifier;
    dns_pending.port = port;
    spin_unlock_irqrestore(&socket_lock, irq_flags);
    packet_length = net_dns_build_query(name, identifier, packet,
                                        sizeof(packet));
    if (packet_length < 0) {
        ret = packet_length;
        goto out;
    }
    for (retry = 0u; retry < NET_DNS_RETRIES; retry++) {
        uint32_t start;

        ret = net_udp_emit(device, config.address, port, config.dns, 53u,
                           packet, (uint32_t)packet_length);
        if (ret < 0)
            continue;
        start = get_time_ms();
        while ((uint32_t)(get_time_ms() - start) < NET_DNS_TIMEOUT_MS) {
            if (dns_pending.complete) {
                ret = dns_pending.error;
                if (ret == 0)
                    *address = dns_pending.result;
                goto out;
            }
            if (task_sleep_interruptible_ms(NET_SOCKET_WAIT_MS) < 0) {
                ret = -EINTR;
                goto out;
            }
        }
        ret = -ETIMEDOUT;
    }
out:
    spin_lock_irqsave(&socket_lock, &irq_flags);
    dns_pending.active = false;
    spin_unlock_irqrestore(&socket_lock, irq_flags);
    return ret;
}

int sys_resolve(const char *user_name, uint32_t *user_address)
{
    char *name;
    uint32_t address;
    int ret;

    if (!user_name || !user_address)
        return -EFAULT;
    name = copy_string_from_user(user_name);
    if (!name)
        return -EFAULT;
    ret = net_dns_resolve(name, &address);
    kfree(name);
    if (ret < 0)
        return ret;
    if (copy_to_user(user_address, &address, sizeof(address)) < 0)
        return -EFAULT;
    return 0;
}

void net_socket_get_tcp_diag(uint32_t *local_ip, uint16_t *local_port,
                             uint32_t *listener_state,
                             uint32_t *established,
                             uint32_t *rx_bytes)
{
    net_socket_t *socket;
    net_device_t *device = net_device_get_default();
    net_ipv4_config_t config;
    unsigned long irq_flags;

    if (local_ip) {
        *local_ip = 0u;
        if (device && net_stack_get_config(device, &config) == 0)
            *local_ip = config.address;
    }
    if (local_port)
        *local_port = 0u;
    if (listener_state)
        *listener_state = 0u;
    if (established)
        *established = 0u;
    if (rx_bytes)
        *rx_bytes = tcp_rx_bytes;
    spin_lock_irqsave(&socket_lock, &irq_flags);
    for (socket = socket_list; socket; socket = socket->next) {
        if (socket->type != NET_SOCK_STREAM)
            continue;
        if (socket->state == NET_SOCKET_LISTEN) {
            if (local_port)
                *local_port = socket->local_port;
            if (listener_state)
                *listener_state = 1u;
        } else if (socket->state == NET_SOCKET_ESTABLISHED ||
                   socket->state == NET_SOCKET_CLOSE_WAIT) {
            if (established)
                (*established)++;
        }
    }
    spin_unlock_irqrestore(&socket_lock, irq_flags);
}
