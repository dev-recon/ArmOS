/*
 * ArmOS
 * Copyright (c) 2026 Mohamed Ennassiri
 *
 * Licensed under the Apache License, Version 2.0.
 * See LICENSE for details.
 *
 * File: userland/lib/wayland/client.c
 * Layer: Userland / Wayland client runtime
 *
 * Responsibilities:
 * - Open and own a client connection to an ArmOS Wayland compositor.
 * - Marshal core display and registry requests on the Wayland wire.
 * - Dispatch registry, callback and display events to typed listeners.
 * - Resolve conventional WAYLAND_DISPLAY and XDG_RUNTIME_DIR names.
 *
 * Notes:
 * - Additional generated protocol bindings use the same proxy object model.
 * - This is an original ArmOS implementation of the public Wayland API.
 */

#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <wayland-client.h>
#include <xdg-shell-client-protocol.h>

#define WL_CLIENT_MAX_OBJECTS     512u
#define WL_CLIENT_MAX_PENDING_FDS 16u
#define WL_WIRE_HEADER_SIZE       8u
#define WL_WIRE_MAX_MESSAGE       65535u

union wl_control_buffer {
    struct cmsghdr alignment;
    uint8_t bytes[
        CMSG_SPACE(sizeof(int) * WL_CLIENT_MAX_PENDING_FDS)];
};

struct wl_proxy {
    uint32_t id;
    uint32_t version;
    const struct wl_interface *interface;
    void *user_data;
    void (**listener)(void);
    void *listener_data;
    struct wl_display *display;
};

struct wl_display {
    struct wl_proxy proxy;
    int fd;
    int error;
    uint32_t next_id;
    int pending_fds[WL_CLIENT_MAX_PENDING_FDS];
    size_t pending_fd_count;
    struct wl_proxy *objects[WL_CLIENT_MAX_OBJECTS];
};

struct wl_registry {
    struct wl_proxy proxy;
};

struct wl_callback {
    struct wl_proxy proxy;
};

struct wl_compositor { struct wl_proxy proxy; };
struct wl_surface { struct wl_proxy proxy; };
struct wl_region { struct wl_proxy proxy; };
struct wl_shm { struct wl_proxy proxy; };
struct wl_shm_pool { struct wl_proxy proxy; };
struct wl_buffer { struct wl_proxy proxy; };
struct wl_seat { struct wl_proxy proxy; };
struct wl_pointer { struct wl_proxy proxy; };
struct wl_keyboard { struct wl_proxy proxy; };
struct wl_output { struct wl_proxy proxy; };
struct wl_data_device_manager { struct wl_proxy proxy; };
struct wl_data_source { struct wl_proxy proxy; };
struct wl_data_device { struct wl_proxy proxy; };
struct wl_data_offer { struct wl_proxy proxy; };
struct xdg_wm_base { struct wl_proxy proxy; };
struct xdg_surface { struct wl_proxy proxy; };
struct xdg_toplevel { struct wl_proxy proxy; };

const struct wl_interface wl_display_interface = {
    "wl_display", 1, 2, NULL, 2, NULL
};

const struct wl_interface wl_registry_interface = {
    "wl_registry", 1, 1, NULL, 2, NULL
};

const struct wl_interface wl_callback_interface = {
    "wl_callback", 1, 0, NULL, 1, NULL
};

const struct wl_interface wl_compositor_interface = {
    "wl_compositor", 1, 2, NULL, 0, NULL
};

const struct wl_interface wl_surface_interface = {
    "wl_surface", 1, 7, NULL, 2, NULL
};

const struct wl_interface wl_region_interface = {
    "wl_region", 1, 3, NULL, 0, NULL
};

const struct wl_interface wl_shm_interface = {
    "wl_shm", 1, 1, NULL, 1, NULL
};

const struct wl_interface wl_shm_pool_interface = {
    "wl_shm_pool", 1, 3, NULL, 0, NULL
};

const struct wl_interface wl_buffer_interface = {
    "wl_buffer", 1, 1, NULL, 1, NULL
};

const struct wl_interface wl_seat_interface = {
    "wl_seat", 4, 3, NULL, 2, NULL
};

const struct wl_interface wl_pointer_interface = {
    "wl_pointer", 4, 2, NULL, 5, NULL
};

const struct wl_interface wl_keyboard_interface = {
    "wl_keyboard", 4, 1, NULL, 6, NULL
};

const struct wl_interface wl_output_interface = {
    "wl_output", 2, 0, NULL, 4, NULL
};

const struct wl_interface wl_data_device_manager_interface = {
    "wl_data_device_manager", 1, 2, NULL, 0, NULL
};

const struct wl_interface wl_data_source_interface = {
    "wl_data_source", 1, 2, NULL, 3, NULL
};

const struct wl_interface wl_data_device_interface = {
    "wl_data_device", 1, 2, NULL, 6, NULL
};

const struct wl_interface wl_data_offer_interface = {
    "wl_data_offer", 1, 3, NULL, 1, NULL
};

const struct wl_interface xdg_wm_base_interface = {
    "xdg_wm_base", 1, 4, NULL, 1, NULL
};

const struct wl_interface xdg_surface_interface = {
    "xdg_surface", 1, 5, NULL, 1, NULL
};

const struct wl_interface xdg_toplevel_interface = {
    "xdg_toplevel", 1, 14, NULL, 2, NULL
};

static uint32_t wl_load_u32(const uint8_t *data)
{
    uint32_t value;

    memcpy(&value, data, sizeof(value));
    return value;
}

static void wl_store_u32(uint8_t *data, uint32_t value)
{
    memcpy(data, &value, sizeof(value));
}

static uint32_t wl_align_u32(uint32_t value)
{
    return (value + 3u) & ~3u;
}

static int wl_proxy_is(const struct wl_proxy *proxy,
                       const struct wl_interface *interface)
{
    return proxy && proxy->interface && interface && interface->name &&
        strcmp(proxy->interface->name, interface->name) == 0;
}

static struct wl_proxy *wl_proxy_find(struct wl_display *display,
                                      uint32_t id)
{
    if (!display || id == 0u)
        return NULL;
    for (size_t index = 0u; index < WL_CLIENT_MAX_OBJECTS; index++) {
        struct wl_proxy *proxy = display->objects[index];

        if (proxy && proxy->id == id)
            return proxy;
    }
    return NULL;
}

static int wl_proxy_store(struct wl_display *display, struct wl_proxy *proxy)
{
    if (!display || !proxy || proxy->id == 0u ||
        wl_proxy_find(display, proxy->id)) {
        errno = EINVAL;
        return -1;
    }
    for (size_t index = 0u; index < WL_CLIENT_MAX_OBJECTS; index++) {
        if (!display->objects[index]) {
            display->objects[index] = proxy;
            return 0;
        }
    }
    errno = EMFILE;
    return -1;
}

static int wl_display_queue_control_fds(struct wl_display *display,
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
            if (display->pending_fd_count >= WL_CLIENT_MAX_PENDING_FDS) {
                close(fds[index]);
                errno = EMFILE;
                return -1;
            }
            display->pending_fds[display->pending_fd_count++] = fds[index];
        }
    }
    return 0;
}

static int wl_display_read_full(struct wl_display *display, void *buffer,
                                size_t size)
{
    uint8_t *cursor = buffer;
    size_t done = 0;

    while (done < size) {
        union wl_control_buffer control;
        struct iovec iov;
        struct msghdr message;
        ssize_t count;

        memset(&message, 0, sizeof(message));
        memset(&control, 0, sizeof(control));
        iov.iov_base = cursor + done;
        iov.iov_len = size - done;
        message.msg_iov = &iov;
        message.msg_iovlen = 1u;
        message.msg_control = control.bytes;
        message.msg_controllen = sizeof(control.bytes);
        count = recvmsg(display->fd, &message, MSG_CMSG_CLOEXEC);
        if (count < 0 && errno == EINTR)
            continue;
        if (count <= 0)
            return -1;
        if ((message.msg_flags & MSG_CTRUNC) != 0 ||
            wl_display_queue_control_fds(display, &message) < 0)
            return -1;
        done += (size_t)count;
    }
    return 0;
}

static int wl_display_take_fd(struct wl_display *display)
{
    int fd;

    if (!display || display->pending_fd_count == 0u) {
        errno = EPROTO;
        return -1;
    }
    fd = display->pending_fds[0];
    display->pending_fd_count--;
    if (display->pending_fd_count > 0u) {
        memmove(display->pending_fds, display->pending_fds + 1,
                display->pending_fd_count *
                    sizeof(display->pending_fds[0]));
    }
    return fd;
}

static int wl_write_full(int fd, const void *buffer, size_t size)
{
    const uint8_t *cursor = buffer;
    size_t done = 0;

    while (done < size) {
        ssize_t count = write(fd, cursor + done, size - done);

        if (count < 0 && errno == EINTR)
            continue;
        if (count <= 0)
            return -1;
        done += (size_t)count;
    }
    return 0;
}

static struct wl_proxy *wl_proxy_allocate(struct wl_display *display,
                                           size_t size,
                                           const struct wl_interface *interface,
                                           uint32_t version)
{
    struct wl_proxy *proxy;
    uint32_t id;

    if (!display || size < sizeof(*proxy)) {
        errno = EINVAL;
        return NULL;
    }
    id = display->next_id;
    while (wl_proxy_find(display, id)) {
        id++;
        if (id >= 0xff000000u)
            id = 2u;
        if (id == display->next_id) {
            errno = EMFILE;
            return NULL;
        }
    }
    display->next_id = id + 1u;
    if (display->next_id >= 0xff000000u)
        display->next_id = 2u;
    proxy = calloc(1, size);
    if (!proxy)
        return NULL;
    proxy->id = id;
    proxy->version = version;
    proxy->interface = interface;
    proxy->display = display;
    if (wl_proxy_store(display, proxy) < 0) {
        free(proxy);
        return NULL;
    }
    return proxy;
}

static struct wl_proxy *wl_proxy_allocate_server(
    struct wl_display *display, size_t size,
    const struct wl_interface *interface, uint32_t version, uint32_t id)
{
    struct wl_proxy *proxy;

    if (!display || !interface || size < sizeof(*proxy) ||
        id < 0xff000000u || wl_proxy_find(display, id)) {
        errno = EPROTO;
        return NULL;
    }
    proxy = calloc(1, size);
    if (!proxy)
        return NULL;
    proxy->id = id;
    proxy->version = version;
    proxy->interface = interface;
    proxy->display = display;
    if (wl_proxy_store(display, proxy) < 0) {
        free(proxy);
        return NULL;
    }
    return proxy;
}

static int wl_send_words(struct wl_display *display, uint32_t object_id,
                         uint16_t opcode, const uint32_t *words,
                         size_t word_count)
{
    uint8_t message[WL_WIRE_HEADER_SIZE + 64u];
    size_t size = WL_WIRE_HEADER_SIZE + word_count * sizeof(uint32_t);

    if (!display || display->fd < 0 || size > sizeof(message)) {
        errno = EINVAL;
        return -1;
    }
    wl_store_u32(message, object_id);
    wl_store_u32(message + 4u, ((uint32_t)size << 16) | opcode);
    if (word_count)
        memcpy(message + WL_WIRE_HEADER_SIZE, words,
               word_count * sizeof(uint32_t));
    if (wl_write_full(display->fd, message, size) < 0) {
        display->error = errno ? errno : EPIPE;
        return -1;
    }
    return 0;
}

static int wl_send_fd_words(struct wl_display *display, uint32_t object_id,
                            uint16_t opcode, const uint32_t *words,
                            size_t word_count, int fd)
{
    uint8_t message[WL_WIRE_HEADER_SIZE + 24u];
    union wl_control_buffer control;
    struct cmsghdr *header;
    struct iovec iov;
    struct msghdr packet;
    size_t size = WL_WIRE_HEADER_SIZE + word_count * sizeof(uint32_t);
    ssize_t sent;

    if (!display || display->fd < 0 || fd < 0 || size > sizeof(message)) {
        errno = EINVAL;
        return -1;
    }
    wl_store_u32(message, object_id);
    wl_store_u32(message + 4u, ((uint32_t)size << 16) | opcode);
    if (word_count)
        memcpy(message + WL_WIRE_HEADER_SIZE, words,
               word_count * sizeof(uint32_t));
    memset(&control, 0, sizeof(control));
    memset(&packet, 0, sizeof(packet));
    iov.iov_base = message;
    iov.iov_len = size;
    packet.msg_iov = &iov;
    packet.msg_iovlen = 1u;
    packet.msg_control = control.bytes;
    packet.msg_controllen = CMSG_SPACE(sizeof(int));
    header = CMSG_FIRSTHDR(&packet);
    header->cmsg_len = CMSG_LEN(sizeof(int));
    header->cmsg_level = SOL_SOCKET;
    header->cmsg_type = SCM_RIGHTS;
    memcpy(CMSG_DATA(header), &fd, sizeof(fd));
    do {
        sent = sendmsg(display->fd, &packet, 0);
    } while (sent < 0 && errno == EINTR);
    if (sent != (ssize_t)size) {
        display->error = errno ? errno : EPIPE;
        return -1;
    }
    return 0;
}

static int wl_send_string(struct wl_proxy *proxy, uint16_t opcode,
                          const char *text)
{
    uint8_t *message;
    uint32_t text_size;
    uint32_t padded;
    uint32_t size;
    int result;

    if (!proxy || !text) {
        errno = EINVAL;
        return -1;
    }
    text_size = (uint32_t)strlen(text) + 1u;
    padded = wl_align_u32(text_size);
    size = WL_WIRE_HEADER_SIZE + 4u + padded;
    if (size > WL_WIRE_MAX_MESSAGE) {
        errno = EMSGSIZE;
        return -1;
    }
    message = calloc(1, size);
    if (!message)
        return -1;
    wl_store_u32(message, proxy->id);
    wl_store_u32(message + 4u, (size << 16) | opcode);
    wl_store_u32(message + 8u, text_size);
    memcpy(message + 12u, text, text_size);
    result = wl_write_full(proxy->display->fd, message, size);
    if (result < 0)
        proxy->display->error = errno ? errno : EPIPE;
    free(message);
    return result;
}

static int wl_send_prefixed_string(struct wl_proxy *proxy, uint16_t opcode,
                                   const uint32_t *words, size_t word_count,
                                   const char *text, bool nullable, int fd)
{
    union wl_control_buffer control;
    struct cmsghdr *header;
    struct iovec iov;
    struct msghdr packet;
    uint8_t *message;
    uint32_t text_size;
    uint32_t padded;
    uint32_t size;
    uint32_t cursor;
    ssize_t sent;
    int result;

    if (!proxy || word_count > 8u || (!text && !nullable) || fd < -1) {
        errno = EINVAL;
        return -1;
    }
    text_size = text ? (uint32_t)strlen(text) + 1u : 0u;
    padded = wl_align_u32(text_size);
    size = WL_WIRE_HEADER_SIZE + (uint32_t)word_count * 4u + 4u + padded;
    if (size > WL_WIRE_MAX_MESSAGE) {
        errno = EMSGSIZE;
        return -1;
    }
    message = calloc(1, size);
    if (!message)
        return -1;
    wl_store_u32(message, proxy->id);
    wl_store_u32(message + 4u, (size << 16) | opcode);
    cursor = WL_WIRE_HEADER_SIZE;
    if (word_count) {
        memcpy(message + cursor, words, word_count * sizeof(uint32_t));
        cursor += (uint32_t)word_count * 4u;
    }
    wl_store_u32(message + cursor, text_size);
    cursor += 4u;
    if (text_size)
        memcpy(message + cursor, text, text_size);
    if (fd < 0) {
        result = wl_write_full(proxy->display->fd, message, size);
        free(message);
        return result;
    }
    memset(&control, 0, sizeof(control));
    memset(&packet, 0, sizeof(packet));
    iov.iov_base = message;
    iov.iov_len = size;
    packet.msg_iov = &iov;
    packet.msg_iovlen = 1u;
    packet.msg_control = control.bytes;
    packet.msg_controllen = CMSG_SPACE(sizeof(int));
    header = CMSG_FIRSTHDR(&packet);
    header->cmsg_len = CMSG_LEN(sizeof(int));
    header->cmsg_level = SOL_SOCKET;
    header->cmsg_type = SCM_RIGHTS;
    memcpy(CMSG_DATA(header), &fd, sizeof(fd));
    do {
        sent = sendmsg(proxy->display->fd, &packet, 0);
    } while (sent < 0 && errno == EINTR);
    free(message);
    if (sent != (ssize_t)size) {
        proxy->display->error = errno ? errno : EPIPE;
        return -1;
    }
    return 0;
}

static int wl_decode_string(const uint8_t *payload, size_t size,
                            size_t *cursor, const char **text)
{
    uint32_t length;
    uint32_t padded;

    if (*cursor + 4u > size)
        return -1;
    length = wl_load_u32(payload + *cursor);
    *cursor += 4u;
    padded = wl_align_u32(length);
    if (length == 0u || *cursor + padded > size ||
        payload[*cursor + length - 1u] != '\0')
        return -1;
    *text = (const char *)(payload + *cursor);
    *cursor += padded;
    return 0;
}

static int wl_decode_nullable_string(const uint8_t *payload, size_t size,
                                     size_t *cursor, const char **text)
{
    uint32_t length;

    if (!payload || !cursor || !text || *cursor + 4u > size)
        return -1;
    length = wl_load_u32(payload + *cursor);
    if (length == 0u) {
        *cursor += 4u;
        *text = NULL;
        return 0;
    }
    return wl_decode_string(payload, size, cursor, text);
}

static int wl_client_socket_path(const char *name, char *path, size_t size)
{
    const char *display_name = name;
    const char *runtime;
    int count;

    if (!display_name || display_name[0] == '\0')
        display_name = getenv("WAYLAND_DISPLAY");
    if (!display_name || display_name[0] == '\0')
        display_name = "wayland-0";
    if (display_name[0] == '/') {
        count = snprintf(path, size, "%s", display_name);
    } else {
        runtime = getenv("XDG_RUNTIME_DIR");
        if (!runtime || runtime[0] == '\0')
            runtime = "/tmp";
        count = snprintf(path, size, "%s/%s", runtime, display_name);
    }
    return count >= 0 && (size_t)count < size ? 0 : -1;
}

struct wl_display *wl_display_connect(const char *name)
{
    struct sockaddr_un address;
    struct wl_display *display;
    char path[sizeof(address.sun_path)];
    int fd;

    if (wl_client_socket_path(name, path, sizeof(path)) < 0) {
        errno = ENAMETOOLONG;
        return NULL;
    }
    fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0)
        return NULL;
    if (fcntl(fd, F_SETFD, FD_CLOEXEC) < 0) {
        close(fd);
        return NULL;
    }

    memset(&address, 0, sizeof(address));
    address.sun_family = AF_UNIX;
    memcpy(address.sun_path, path, strlen(path) + 1u);
    if (connect(fd, (struct sockaddr *)&address, sizeof(address)) < 0) {
        close(fd);
        return NULL;
    }

    display = calloc(1, sizeof(*display));
    if (!display) {
        close(fd);
        return NULL;
    }
    display->proxy.id = 1u;
    display->proxy.version = 1u;
    display->proxy.interface = &wl_display_interface;
    display->proxy.display = display;
    display->fd = fd;
    display->next_id = 2u;
    display->objects[0] = &display->proxy;
    return display;
}

void wl_display_disconnect(struct wl_display *display)
{
    if (!display)
        return;
    if (display->fd >= 0)
        close(display->fd);
    for (size_t index = 0; index < display->pending_fd_count; index++)
        close(display->pending_fds[index]);
    for (size_t index = 0u; index < WL_CLIENT_MAX_OBJECTS; index++) {
        if (display->objects[index] != &display->proxy)
            free(display->objects[index]);
    }
    free(display);
}

int wl_display_get_fd(struct wl_display *display)
{
    if (!display) {
        errno = EINVAL;
        return -1;
    }
    return display->fd;
}

int wl_display_get_error(struct wl_display *display)
{
    return display ? display->error : EINVAL;
}

int wl_display_flush(struct wl_display *display)
{
    if (!display || display->fd < 0) {
        errno = EINVAL;
        return -1;
    }
    return 0;
}

int wl_proxy_add_listener(struct wl_proxy *proxy,
                          void (**implementation)(void), void *data)
{
    if (!proxy || !implementation || proxy->listener) {
        errno = EINVAL;
        return -1;
    }
    proxy->listener = implementation;
    proxy->listener_data = data;
    return 0;
}

struct wl_proxy *wl_proxy_marshal_flags(
    struct wl_proxy *proxy, uint32_t opcode,
    const struct wl_interface *interface, uint32_t version,
    uint32_t flags, ...)
{
    const struct wl_message *method;
    struct wl_proxy *constructed = NULL;
    struct wl_display *display;
    uint8_t *message;
    uint32_t cursor = WL_WIRE_HEADER_SIZE;
    int descriptors[8];
    size_t descriptor_count = 0u;
    const char *signature;
    va_list arguments;
    int result = -1;

    if (!proxy || !proxy->display || !proxy->interface ||
        opcode >= (uint32_t)proxy->interface->method_count ||
        !proxy->interface->methods) {
        errno = EINVAL;
        return NULL;
    }
    method = &proxy->interface->methods[opcode];
    if (!method->signature) {
        errno = EINVAL;
        return NULL;
    }
    display = proxy->display;
    if (interface) {
        constructed = wl_proxy_allocate(display, sizeof(*constructed),
                                         interface, version);
        if (!constructed)
            return NULL;
    }
    message = calloc(1, WL_WIRE_MAX_MESSAGE);
    if (!message)
        goto out;

    va_start(arguments, flags);
    for (signature = method->signature; *signature; signature++) {
        uint32_t word;

        if ((*signature >= '0' && *signature <= '9') ||
            *signature == '?')
            continue;
        switch (*signature) {
        case 'i':
            word = (uint32_t)va_arg(arguments, int);
            goto store_word;
        case 'u':
            word = va_arg(arguments, uint32_t);
            goto store_word;
        case 'f':
            word = (uint32_t)va_arg(arguments, wl_fixed_t);
            goto store_word;
        case 'o': {
            struct wl_proxy *object = va_arg(arguments, struct wl_proxy *);

            word = object ? object->id : 0u;
            goto store_word;
        }
        case 'n':
            (void)va_arg(arguments, void *);
            if (!constructed)
                goto invalid;
            word = constructed->id;
            goto store_word;
        case 's': {
            const char *text = va_arg(arguments, const char *);
            uint32_t text_size = text ? (uint32_t)strlen(text) + 1u : 0u;
            uint32_t padded = wl_align_u32(text_size);

            if (cursor + 4u + padded > WL_WIRE_MAX_MESSAGE)
                goto too_large;
            wl_store_u32(message + cursor, text_size);
            cursor += 4u;
            if (text_size)
                memcpy(message + cursor, text, text_size);
            cursor += padded;
            break;
        }
        case 'a': {
            const struct wl_array *array =
                va_arg(arguments, const struct wl_array *);
            uint32_t array_size =
                array ? (uint32_t)array->size : 0u;
            uint32_t padded = wl_align_u32(array_size);

            if ((array && array->size > UINT32_MAX) ||
                cursor + 4u + padded > WL_WIRE_MAX_MESSAGE)
                goto too_large;
            wl_store_u32(message + cursor, array_size);
            cursor += 4u;
            if (array_size)
                memcpy(message + cursor, array->data, array_size);
            cursor += padded;
            break;
        }
        case 'h':
            if (descriptor_count >=
                sizeof(descriptors) / sizeof(descriptors[0]))
                goto too_large;
            descriptors[descriptor_count++] = va_arg(arguments, int);
            break;
        default:
            goto invalid;
        }
        continue;

store_word:
        if (cursor + 4u > WL_WIRE_MAX_MESSAGE)
            goto too_large;
        wl_store_u32(message + cursor, word);
        cursor += 4u;
    }
    va_end(arguments);

    wl_store_u32(message, proxy->id);
    wl_store_u32(message + 4u, (cursor << 16) | (opcode & 0xffffu));
    if (descriptor_count == 0u) {
        result = wl_write_full(display->fd, message, cursor);
    } else {
        union wl_control_buffer control;
        struct cmsghdr *header;
        struct iovec iov;
        struct msghdr packet;
        ssize_t sent;

        memset(&control, 0, sizeof(control));
        memset(&packet, 0, sizeof(packet));
        iov.iov_base = message;
        iov.iov_len = cursor;
        packet.msg_iov = &iov;
        packet.msg_iovlen = 1u;
        packet.msg_control = control.bytes;
        packet.msg_controllen =
            CMSG_SPACE(descriptor_count * sizeof(descriptors[0]));
        header = CMSG_FIRSTHDR(&packet);
        header->cmsg_len =
            CMSG_LEN(descriptor_count * sizeof(descriptors[0]));
        header->cmsg_level = SOL_SOCKET;
        header->cmsg_type = SCM_RIGHTS;
        memcpy(CMSG_DATA(header), descriptors,
               descriptor_count * sizeof(descriptors[0]));
        do {
            sent = sendmsg(display->fd, &packet, 0);
        } while (sent < 0 && errno == EINTR);
        result = sent == (ssize_t)cursor ? 0 : -1;
    }
    if (result < 0) {
        display->error = errno ? errno : EPIPE;
        goto out;
    }
    free(message);
    if ((flags & WL_MARSHAL_FLAG_DESTROY) != 0u)
        wl_proxy_destroy(proxy);
    return constructed;

invalid:
    errno = EINVAL;
    va_end(arguments);
    goto out;
too_large:
    errno = EMSGSIZE;
    va_end(arguments);
out:
    free(message);
    if (constructed)
        wl_proxy_destroy(constructed);
    return NULL;
}

void wl_proxy_destroy(struct wl_proxy *proxy)
{
    struct wl_display *display;

    if (!proxy || proxy->id == 1u)
        return;
    display = proxy->display;
    if (display) {
        for (size_t index = 0u; index < WL_CLIENT_MAX_OBJECTS; index++) {
            if (display->objects[index] == proxy) {
                display->objects[index] = NULL;
                break;
            }
        }
    }
    free(proxy);
}

void wl_proxy_set_user_data(struct wl_proxy *proxy, void *user_data)
{
    if (proxy)
        proxy->user_data = user_data;
}

void *wl_proxy_get_user_data(struct wl_proxy *proxy)
{
    return proxy ? proxy->user_data : NULL;
}

uint32_t wl_proxy_get_id(struct wl_proxy *proxy)
{
    return proxy ? proxy->id : 0u;
}

uint32_t wl_proxy_get_version(struct wl_proxy *proxy)
{
    return proxy ? proxy->version : 0u;
}

const char *wl_proxy_get_class(struct wl_proxy *proxy)
{
    return proxy && proxy->interface ? proxy->interface->name : "wl_display";
}

struct wl_registry *wl_display_get_registry(struct wl_display *display)
{
    struct wl_registry *registry;
    uint32_t id;

    registry = (struct wl_registry *)wl_proxy_allocate(
        display, sizeof(*registry), &wl_registry_interface, 1u);
    if (!registry)
        return NULL;
    id = registry->proxy.id;
    if (wl_send_words(display, 1u, 1u, &id, 1u) < 0) {
        wl_proxy_destroy(&registry->proxy);
        return NULL;
    }
    return registry;
}

struct wl_callback *wl_display_sync(struct wl_display *display)
{
    struct wl_callback *callback;
    uint32_t id;

    callback = (struct wl_callback *)wl_proxy_allocate(
        display, sizeof(*callback), &wl_callback_interface, 1u);
    if (!callback)
        return NULL;
    id = callback->proxy.id;
    if (wl_send_words(display, 1u, 0u, &id, 1u) < 0) {
        wl_proxy_destroy(&callback->proxy);
        return NULL;
    }
    return callback;
}

int wl_registry_add_listener(struct wl_registry *registry,
                             const struct wl_registry_listener *listener,
                             void *data)
{
    return wl_proxy_add_listener(&registry->proxy,
                                 (void (**)(void))listener, data);
}

void *wl_registry_bind(struct wl_registry *registry, uint32_t name,
                       const struct wl_interface *interface,
                       uint32_t version)
{
    struct wl_proxy *proxy;
    struct wl_display *display;
    uint8_t *message;
    uint32_t text_size;
    uint32_t padded;
    uint32_t size;

    if (!registry || !interface || !interface->name || version == 0u) {
        errno = EINVAL;
        return NULL;
    }
    display = registry->proxy.display;
    if ((int)version > interface->version)
        version = (uint32_t)interface->version;
    proxy = wl_proxy_allocate(display, sizeof(*proxy), interface, version);
    if (!proxy)
        return NULL;
    text_size = (uint32_t)strlen(interface->name) + 1u;
    padded = wl_align_u32(text_size);
    size = 8u + 4u + 4u + padded + 4u + 4u;
    if (size > WL_WIRE_MAX_MESSAGE) {
        wl_proxy_destroy(proxy);
        errno = EMSGSIZE;
        return NULL;
    }
    message = calloc(1, size);
    if (!message) {
        wl_proxy_destroy(proxy);
        return NULL;
    }
    wl_store_u32(message, registry->proxy.id);
    wl_store_u32(message + 4u, (size << 16) | 0u);
    wl_store_u32(message + 8u, name);
    wl_store_u32(message + 12u, text_size);
    memcpy(message + 16u, interface->name, text_size);
    wl_store_u32(message + 16u + padded, version);
    wl_store_u32(message + 20u + padded, proxy->id);
    if (wl_write_full(display->fd, message, size) < 0) {
        display->error = errno ? errno : EPIPE;
        free(message);
        wl_proxy_destroy(proxy);
        return NULL;
    }
    free(message);
    return proxy;
}

void wl_registry_destroy(struct wl_registry *registry)
{
    if (registry)
        wl_proxy_destroy(&registry->proxy);
}

int wl_callback_add_listener(struct wl_callback *callback,
                             const struct wl_callback_listener *listener,
                             void *data)
{
    return wl_proxy_add_listener(&callback->proxy,
                                 (void (**)(void))listener, data);
}

void wl_callback_destroy(struct wl_callback *callback)
{
    if (callback)
        wl_proxy_destroy(&callback->proxy);
}

static struct wl_proxy *wl_create_object(struct wl_proxy *factory,
                                         uint16_t opcode, size_t size,
                                         const struct wl_interface *interface)
{
    struct wl_proxy *proxy;
    uint32_t id;

    uint32_t version;

    if (!factory || !interface)
        return NULL;
    version = factory->version < (uint32_t)interface->version ?
        factory->version : (uint32_t)interface->version;
    if (!(proxy = wl_proxy_allocate(factory->display, size, interface,
                                    version)))
        return NULL;
    id = proxy->id;
    if (wl_send_words(factory->display, factory->id, opcode, &id, 1u) < 0) {
        wl_proxy_destroy(proxy);
        return NULL;
    }
    return proxy;
}

struct wl_surface *wl_compositor_create_surface(
    struct wl_compositor *compositor)
{
    return (struct wl_surface *)wl_create_object(
        &compositor->proxy, 0u, sizeof(struct wl_surface),
        &wl_surface_interface);
}

struct wl_region *wl_compositor_create_region(
    struct wl_compositor *compositor)
{
    return (struct wl_region *)wl_create_object(
        &compositor->proxy, 1u, sizeof(struct wl_region),
        &wl_region_interface);
}

void wl_compositor_destroy(struct wl_compositor *compositor)
{
    if (compositor)
        wl_proxy_destroy(&compositor->proxy);
}

int wl_shm_add_listener(struct wl_shm *shm,
                        const struct wl_shm_listener *listener, void *data)
{
    return wl_proxy_add_listener(&shm->proxy, (void (**)(void))listener,
                                 data);
}

struct wl_shm_pool *wl_shm_create_pool(struct wl_shm *shm, int fd,
                                       int32_t size)
{
    struct wl_shm_pool *pool;
    uint32_t words[2];

    if (!shm || fd < 0 || size <= 0) {
        errno = EINVAL;
        return NULL;
    }
    pool = (struct wl_shm_pool *)wl_proxy_allocate(
        shm->proxy.display, sizeof(*pool), &wl_shm_pool_interface, 1u);
    if (!pool)
        return NULL;
    words[0] = pool->proxy.id;
    words[1] = (uint32_t)size;
    if (wl_send_fd_words(shm->proxy.display, shm->proxy.id, 0u, words, 2u,
                         fd) < 0) {
        wl_proxy_destroy(&pool->proxy);
        return NULL;
    }
    return pool;
}

void wl_shm_destroy(struct wl_shm *shm)
{
    if (shm)
        wl_proxy_destroy(&shm->proxy);
}

struct wl_buffer *wl_shm_pool_create_buffer(struct wl_shm_pool *pool,
                                             int32_t offset, int32_t width,
                                             int32_t height, int32_t stride,
                                             uint32_t format)
{
    struct wl_buffer *buffer;
    uint32_t words[6];

    if (!pool || offset < 0 || width <= 0 || height <= 0 || stride <= 0) {
        errno = EINVAL;
        return NULL;
    }
    buffer = (struct wl_buffer *)wl_proxy_allocate(
        pool->proxy.display, sizeof(*buffer), &wl_buffer_interface, 1u);
    if (!buffer)
        return NULL;
    words[0] = buffer->proxy.id;
    words[1] = (uint32_t)offset;
    words[2] = (uint32_t)width;
    words[3] = (uint32_t)height;
    words[4] = (uint32_t)stride;
    words[5] = format;
    if (wl_send_words(pool->proxy.display, pool->proxy.id, 0u, words, 6u)
        < 0) {
        wl_proxy_destroy(&buffer->proxy);
        return NULL;
    }
    return buffer;
}

void wl_shm_pool_resize(struct wl_shm_pool *pool, int32_t size)
{
    uint32_t word = (uint32_t)size;

    if (pool && size > 0)
        (void)wl_send_words(pool->proxy.display, pool->proxy.id, 2u,
                            &word, 1u);
}

void wl_shm_pool_destroy(struct wl_shm_pool *pool)
{
    if (!pool)
        return;
    (void)wl_send_words(pool->proxy.display, pool->proxy.id, 1u, NULL, 0u);
    wl_proxy_destroy(&pool->proxy);
}

int wl_buffer_add_listener(struct wl_buffer *buffer,
                           const struct wl_buffer_listener *listener,
                           void *data)
{
    return wl_proxy_add_listener(&buffer->proxy, (void (**)(void))listener,
                                 data);
}

void wl_buffer_destroy(struct wl_buffer *buffer)
{
    if (!buffer)
        return;
    (void)wl_send_words(buffer->proxy.display, buffer->proxy.id, 0u,
                        NULL, 0u);
    wl_proxy_destroy(&buffer->proxy);
}

int wl_surface_add_listener(struct wl_surface *surface,
                            const struct wl_surface_listener *listener,
                            void *data)
{
    return wl_proxy_add_listener(&surface->proxy, (void (**)(void))listener,
                                 data);
}

void wl_surface_attach(struct wl_surface *surface, struct wl_buffer *buffer,
                       int32_t x, int32_t y)
{
    uint32_t words[3];

    if (!surface)
        return;
    words[0] = buffer ? buffer->proxy.id : 0u;
    words[1] = (uint32_t)x;
    words[2] = (uint32_t)y;
    (void)wl_send_words(surface->proxy.display, surface->proxy.id, 1u,
                        words, 3u);
}

void wl_surface_damage(struct wl_surface *surface, int32_t x, int32_t y,
                       int32_t width, int32_t height)
{
    uint32_t words[4] = {
        (uint32_t)x, (uint32_t)y, (uint32_t)width, (uint32_t)height
    };

    if (surface)
        (void)wl_send_words(surface->proxy.display, surface->proxy.id, 2u,
                            words, 4u);
}

struct wl_callback *wl_surface_frame(struct wl_surface *surface)
{
    return (struct wl_callback *)wl_create_object(
        &surface->proxy, 3u, sizeof(struct wl_callback),
        &wl_callback_interface);
}

static void wl_surface_set_region(struct wl_surface *surface,
                                  struct wl_region *region, uint16_t opcode)
{
    uint32_t id;

    if (!surface)
        return;
    id = region ? region->proxy.id : 0u;
    (void)wl_send_words(surface->proxy.display, surface->proxy.id, opcode,
                        &id, 1u);
}

void wl_surface_set_opaque_region(struct wl_surface *surface,
                                  struct wl_region *region)
{
    wl_surface_set_region(surface, region, 4u);
}

void wl_surface_set_input_region(struct wl_surface *surface,
                                 struct wl_region *region)
{
    wl_surface_set_region(surface, region, 5u);
}

void wl_surface_commit(struct wl_surface *surface)
{
    if (surface)
        (void)wl_send_words(surface->proxy.display, surface->proxy.id, 6u,
                            NULL, 0u);
}

void wl_surface_destroy(struct wl_surface *surface)
{
    if (!surface)
        return;
    (void)wl_send_words(surface->proxy.display, surface->proxy.id, 0u,
                        NULL, 0u);
    wl_proxy_destroy(&surface->proxy);
}

static void wl_region_modify(struct wl_region *region, uint16_t opcode,
                             int32_t x, int32_t y, int32_t width,
                             int32_t height)
{
    uint32_t words[4] = {
        (uint32_t)x, (uint32_t)y, (uint32_t)width, (uint32_t)height
    };

    if (region)
        (void)wl_send_words(region->proxy.display, region->proxy.id, opcode,
                            words, 4u);
}

void wl_region_add(struct wl_region *region, int32_t x, int32_t y,
                   int32_t width, int32_t height)
{
    wl_region_modify(region, 1u, x, y, width, height);
}

void wl_region_subtract(struct wl_region *region, int32_t x, int32_t y,
                        int32_t width, int32_t height)
{
    wl_region_modify(region, 2u, x, y, width, height);
}

void wl_region_destroy(struct wl_region *region)
{
    if (!region)
        return;
    (void)wl_send_words(region->proxy.display, region->proxy.id, 0u,
                        NULL, 0u);
    wl_proxy_destroy(&region->proxy);
}

int wl_seat_add_listener(struct wl_seat *seat,
                         const struct wl_seat_listener *listener, void *data)
{
    return wl_proxy_add_listener(&seat->proxy, (void (**)(void))listener,
                                 data);
}

struct wl_pointer *wl_seat_get_pointer(struct wl_seat *seat)
{
    if (!seat) {
        errno = EINVAL;
        return NULL;
    }
    return (struct wl_pointer *)wl_create_object(
        &seat->proxy, 0u, sizeof(struct wl_pointer), &wl_pointer_interface);
}

struct wl_keyboard *wl_seat_get_keyboard(struct wl_seat *seat)
{
    if (!seat) {
        errno = EINVAL;
        return NULL;
    }
    return (struct wl_keyboard *)wl_create_object(
        &seat->proxy, 1u, sizeof(struct wl_keyboard),
        &wl_keyboard_interface);
}

void wl_seat_destroy(struct wl_seat *seat)
{
    if (seat)
        wl_proxy_destroy(&seat->proxy);
}

int wl_pointer_add_listener(struct wl_pointer *pointer,
                            const struct wl_pointer_listener *listener,
                            void *data)
{
    return wl_proxy_add_listener(&pointer->proxy,
                                 (void (**)(void))listener, data);
}

void wl_pointer_set_cursor(struct wl_pointer *pointer, uint32_t serial,
                           struct wl_surface *surface, int32_t hotspot_x,
                           int32_t hotspot_y)
{
    uint32_t words[4];

    if (!pointer)
        return;
    words[0] = serial;
    words[1] = surface ? surface->proxy.id : 0u;
    words[2] = (uint32_t)hotspot_x;
    words[3] = (uint32_t)hotspot_y;
    (void)wl_send_words(pointer->proxy.display, pointer->proxy.id, 0u,
                        words, 4u);
}

void wl_pointer_destroy(struct wl_pointer *pointer)
{
    if (!pointer)
        return;
    if (pointer->proxy.version >= 3u)
        (void)wl_send_words(pointer->proxy.display, pointer->proxy.id, 1u,
                            NULL, 0u);
    wl_proxy_destroy(&pointer->proxy);
}

int wl_keyboard_add_listener(struct wl_keyboard *keyboard,
                             const struct wl_keyboard_listener *listener,
                             void *data)
{
    return wl_proxy_add_listener(&keyboard->proxy,
                                 (void (**)(void))listener, data);
}

void wl_keyboard_destroy(struct wl_keyboard *keyboard)
{
    if (!keyboard)
        return;
    if (keyboard->proxy.version >= 3u)
        (void)wl_send_words(keyboard->proxy.display, keyboard->proxy.id, 0u,
                            NULL, 0u);
    wl_proxy_destroy(&keyboard->proxy);
}

int wl_output_add_listener(struct wl_output *output,
                           const struct wl_output_listener *listener,
                           void *data)
{
    return wl_proxy_add_listener(&output->proxy,
                                 (void (**)(void))listener, data);
}

void wl_output_destroy(struct wl_output *output)
{
    if (output)
        wl_proxy_destroy(&output->proxy);
}

struct wl_data_source *wl_data_device_manager_create_data_source(
    struct wl_data_device_manager *manager)
{
    if (!manager) {
        errno = EINVAL;
        return NULL;
    }
    return (struct wl_data_source *)wl_create_object(
        &manager->proxy, 0u, sizeof(struct wl_data_source),
        &wl_data_source_interface);
}

struct wl_data_device *wl_data_device_manager_get_data_device(
    struct wl_data_device_manager *manager, struct wl_seat *seat)
{
    struct wl_data_device *device;
    uint32_t words[2];

    if (!manager || !seat) {
        errno = EINVAL;
        return NULL;
    }
    device = (struct wl_data_device *)wl_proxy_allocate(
        manager->proxy.display, sizeof(*device),
        &wl_data_device_interface, manager->proxy.version);
    if (!device)
        return NULL;
    words[0] = device->proxy.id;
    words[1] = seat->proxy.id;
    if (wl_send_words(manager->proxy.display, manager->proxy.id, 1u,
                      words, 2u) < 0) {
        wl_proxy_destroy(&device->proxy);
        return NULL;
    }
    return device;
}

void wl_data_device_manager_destroy(struct wl_data_device_manager *manager)
{
    if (manager)
        wl_proxy_destroy(&manager->proxy);
}

int wl_data_source_add_listener(
    struct wl_data_source *source,
    const struct wl_data_source_listener *listener, void *data)
{
    return wl_proxy_add_listener(&source->proxy,
                                 (void (**)(void))listener, data);
}

void wl_data_source_offer(struct wl_data_source *source,
                          const char *mime_type)
{
    if (source && mime_type)
        (void)wl_send_string(&source->proxy, 0u, mime_type);
}

void wl_data_source_destroy(struct wl_data_source *source)
{
    if (!source)
        return;
    (void)wl_send_words(source->proxy.display, source->proxy.id, 1u,
                        NULL, 0u);
    wl_proxy_destroy(&source->proxy);
}

int wl_data_device_add_listener(
    struct wl_data_device *device,
    const struct wl_data_device_listener *listener, void *data)
{
    return wl_proxy_add_listener(&device->proxy,
                                 (void (**)(void))listener, data);
}

void wl_data_device_set_selection(struct wl_data_device *device,
                                  struct wl_data_source *source,
                                  uint32_t serial)
{
    uint32_t words[2];

    if (!device)
        return;
    words[0] = source ? source->proxy.id : 0u;
    words[1] = serial;
    (void)wl_send_words(device->proxy.display, device->proxy.id, 1u,
                        words, 2u);
}

void wl_data_device_destroy(struct wl_data_device *device)
{
    if (device)
        wl_proxy_destroy(&device->proxy);
}

int wl_data_offer_add_listener(
    struct wl_data_offer *offer,
    const struct wl_data_offer_listener *listener, void *data)
{
    return wl_proxy_add_listener(&offer->proxy,
                                 (void (**)(void))listener, data);
}

void wl_data_offer_accept(struct wl_data_offer *offer, uint32_t serial,
                          const char *mime_type)
{
    if (offer)
        (void)wl_send_prefixed_string(&offer->proxy, 0u, &serial, 1u,
                                      mime_type, true, -1);
}

void wl_data_offer_receive(struct wl_data_offer *offer,
                           const char *mime_type, int32_t fd)
{
    if (offer && mime_type && fd >= 0)
        (void)wl_send_prefixed_string(&offer->proxy, 1u, NULL, 0u,
                                      mime_type, false, fd);
}

void wl_data_offer_destroy(struct wl_data_offer *offer)
{
    if (!offer)
        return;
    (void)wl_send_words(offer->proxy.display, offer->proxy.id, 2u,
                        NULL, 0u);
    wl_proxy_destroy(&offer->proxy);
}

int xdg_wm_base_add_listener(
    struct xdg_wm_base *xdg_wm_base,
    const struct xdg_wm_base_listener *listener, void *data)
{
    return wl_proxy_add_listener(&xdg_wm_base->proxy,
                                 (void (**)(void))listener, data);
}

void xdg_wm_base_pong(struct xdg_wm_base *xdg_wm_base, uint32_t serial)
{
    if (xdg_wm_base)
        (void)wl_send_words(xdg_wm_base->proxy.display,
                            xdg_wm_base->proxy.id, 3u, &serial, 1u);
}

struct xdg_surface *xdg_wm_base_get_xdg_surface(
    struct xdg_wm_base *xdg_wm_base, struct wl_surface *surface)
{
    struct xdg_surface *xdg_surface;
    uint32_t words[2];

    if (!xdg_wm_base || !surface) {
        errno = EINVAL;
        return NULL;
    }
    xdg_surface = (struct xdg_surface *)wl_proxy_allocate(
        xdg_wm_base->proxy.display, sizeof(*xdg_surface),
        &xdg_surface_interface, 1u);
    if (!xdg_surface)
        return NULL;
    words[0] = xdg_surface->proxy.id;
    words[1] = surface->proxy.id;
    if (wl_send_words(xdg_wm_base->proxy.display, xdg_wm_base->proxy.id,
                      2u, words, 2u) < 0) {
        wl_proxy_destroy(&xdg_surface->proxy);
        return NULL;
    }
    return xdg_surface;
}

void xdg_wm_base_destroy(struct xdg_wm_base *xdg_wm_base)
{
    if (!xdg_wm_base)
        return;
    (void)wl_send_words(xdg_wm_base->proxy.display,
                        xdg_wm_base->proxy.id, 0u, NULL, 0u);
    wl_proxy_destroy(&xdg_wm_base->proxy);
}

int xdg_surface_add_listener(
    struct xdg_surface *xdg_surface,
    const struct xdg_surface_listener *listener, void *data)
{
    return wl_proxy_add_listener(&xdg_surface->proxy,
                                 (void (**)(void))listener, data);
}

struct xdg_toplevel *xdg_surface_get_toplevel(
    struct xdg_surface *xdg_surface)
{
    if (!xdg_surface) {
        errno = EINVAL;
        return NULL;
    }
    return (struct xdg_toplevel *)wl_create_object(
        &xdg_surface->proxy, 1u, sizeof(struct xdg_toplevel),
        &xdg_toplevel_interface);
}

void xdg_surface_set_window_geometry(struct xdg_surface *xdg_surface,
                                     int32_t x, int32_t y,
                                     int32_t width, int32_t height)
{
    uint32_t words[4] = {
        (uint32_t)x, (uint32_t)y, (uint32_t)width, (uint32_t)height
    };

    if (xdg_surface)
        (void)wl_send_words(xdg_surface->proxy.display,
                            xdg_surface->proxy.id, 3u, words, 4u);
}

void xdg_surface_ack_configure(struct xdg_surface *xdg_surface,
                               uint32_t serial)
{
    if (xdg_surface)
        (void)wl_send_words(xdg_surface->proxy.display,
                            xdg_surface->proxy.id, 4u, &serial, 1u);
}

void xdg_surface_destroy(struct xdg_surface *xdg_surface)
{
    if (!xdg_surface)
        return;
    (void)wl_send_words(xdg_surface->proxy.display,
                        xdg_surface->proxy.id, 0u, NULL, 0u);
    wl_proxy_destroy(&xdg_surface->proxy);
}

int xdg_toplevel_add_listener(
    struct xdg_toplevel *xdg_toplevel,
    const struct xdg_toplevel_listener *listener, void *data)
{
    return wl_proxy_add_listener(&xdg_toplevel->proxy,
                                 (void (**)(void))listener, data);
}

void xdg_toplevel_set_title(struct xdg_toplevel *xdg_toplevel,
                            const char *title)
{
    if (xdg_toplevel)
        (void)wl_send_string(&xdg_toplevel->proxy, 2u, title);
}

void xdg_toplevel_set_app_id(struct xdg_toplevel *xdg_toplevel,
                             const char *app_id)
{
    if (xdg_toplevel)
        (void)wl_send_string(&xdg_toplevel->proxy, 3u, app_id);
}

static void xdg_toplevel_set_size(struct xdg_toplevel *xdg_toplevel,
                                  uint16_t opcode, int32_t width,
                                  int32_t height)
{
    uint32_t words[2] = {(uint32_t)width, (uint32_t)height};

    if (xdg_toplevel)
        (void)wl_send_words(xdg_toplevel->proxy.display,
                            xdg_toplevel->proxy.id, opcode, words, 2u);
}

void xdg_toplevel_set_max_size(struct xdg_toplevel *xdg_toplevel,
                               int32_t width, int32_t height)
{
    xdg_toplevel_set_size(xdg_toplevel, 7u, width, height);
}

void xdg_toplevel_set_min_size(struct xdg_toplevel *xdg_toplevel,
                               int32_t width, int32_t height)
{
    xdg_toplevel_set_size(xdg_toplevel, 8u, width, height);
}

static void xdg_toplevel_send_empty(struct xdg_toplevel *xdg_toplevel,
                                    uint16_t opcode)
{
    if (xdg_toplevel)
        (void)wl_send_words(xdg_toplevel->proxy.display,
                            xdg_toplevel->proxy.id, opcode, NULL, 0u);
}

void xdg_toplevel_set_maximized(struct xdg_toplevel *xdg_toplevel)
{
    xdg_toplevel_send_empty(xdg_toplevel, 9u);
}

void xdg_toplevel_unset_maximized(struct xdg_toplevel *xdg_toplevel)
{
    xdg_toplevel_send_empty(xdg_toplevel, 10u);
}

void xdg_toplevel_set_fullscreen(struct xdg_toplevel *xdg_toplevel,
                                 struct wl_output *output)
{
    uint32_t id = output ? ((struct wl_proxy *)output)->id : 0u;

    if (xdg_toplevel)
        (void)wl_send_words(xdg_toplevel->proxy.display,
                            xdg_toplevel->proxy.id, 11u, &id, 1u);
}

void xdg_toplevel_unset_fullscreen(struct xdg_toplevel *xdg_toplevel)
{
    xdg_toplevel_send_empty(xdg_toplevel, 12u);
}

void xdg_toplevel_set_minimized(struct xdg_toplevel *xdg_toplevel)
{
    xdg_toplevel_send_empty(xdg_toplevel, 13u);
}

void xdg_toplevel_destroy(struct xdg_toplevel *xdg_toplevel)
{
    if (!xdg_toplevel)
        return;
    (void)wl_send_words(xdg_toplevel->proxy.display,
                        xdg_toplevel->proxy.id, 0u, NULL, 0u);
    wl_proxy_destroy(&xdg_toplevel->proxy);
}

static struct wl_surface *wl_event_surface(struct wl_display *display,
                                           uint32_t id)
{
    struct wl_proxy *proxy;

    proxy = wl_proxy_find(display, id);
    return wl_proxy_is(proxy, &wl_surface_interface) ?
        (struct wl_surface *)proxy : NULL;
}

static int wl_dispatch_seat_event(struct wl_proxy *proxy, uint16_t opcode,
                                  const uint8_t *payload, size_t size)
{
    const struct wl_seat_listener *listener =
        (const struct wl_seat_listener *)proxy->listener;

    if (opcode == 0u && size == 4u) {
        if (listener && listener->capabilities)
            listener->capabilities(proxy->listener_data,
                                   (struct wl_seat *)proxy,
                                   wl_load_u32(payload));
        return 0;
    }
    if (opcode == 1u) {
        const char *name;
        size_t cursor = 0u;

        if (wl_decode_string(payload, size, &cursor, &name) < 0 ||
            cursor != size)
            return -1;
        if (listener && listener->name)
            listener->name(proxy->listener_data, (struct wl_seat *)proxy,
                           name);
        return 0;
    }
    return -1;
}

static int wl_dispatch_pointer_event(struct wl_proxy *proxy,
                                     uint16_t opcode,
                                     const uint8_t *payload, size_t size)
{
    const struct wl_pointer_listener *listener =
        (const struct wl_pointer_listener *)proxy->listener;
    struct wl_display *display = proxy->display;

    if (opcode == 0u && size == 16u) {
        struct wl_surface *surface =
            wl_event_surface(display, wl_load_u32(payload + 4u));

        if (!surface)
            return -1;
        if (listener && listener->enter)
            listener->enter(proxy->listener_data,
                            (struct wl_pointer *)proxy,
                            wl_load_u32(payload), surface,
                            (wl_fixed_t)wl_load_u32(payload + 8u),
                            (wl_fixed_t)wl_load_u32(payload + 12u));
        return 0;
    }
    if (opcode == 1u && size == 8u) {
        struct wl_surface *surface =
            wl_event_surface(display, wl_load_u32(payload + 4u));

        if (!surface)
            return -1;
        if (listener && listener->leave)
            listener->leave(proxy->listener_data,
                            (struct wl_pointer *)proxy,
                            wl_load_u32(payload), surface);
        return 0;
    }
    if (opcode == 2u && size == 12u) {
        if (listener && listener->motion)
            listener->motion(proxy->listener_data,
                             (struct wl_pointer *)proxy,
                             wl_load_u32(payload),
                             (wl_fixed_t)wl_load_u32(payload + 4u),
                             (wl_fixed_t)wl_load_u32(payload + 8u));
        return 0;
    }
    if (opcode == 3u && size == 16u) {
        if (listener && listener->button)
            listener->button(proxy->listener_data,
                             (struct wl_pointer *)proxy,
                             wl_load_u32(payload),
                             wl_load_u32(payload + 4u),
                             wl_load_u32(payload + 8u),
                             wl_load_u32(payload + 12u));
        return 0;
    }
    if (opcode == 4u && size == 12u) {
        if (listener && listener->axis)
            listener->axis(proxy->listener_data,
                           (struct wl_pointer *)proxy,
                           wl_load_u32(payload),
                           wl_load_u32(payload + 4u),
                           (wl_fixed_t)wl_load_u32(payload + 8u));
        return 0;
    }
    return -1;
}

static int wl_dispatch_keyboard_event(struct wl_proxy *proxy,
                                      uint16_t opcode,
                                      const uint8_t *payload, size_t size)
{
    const struct wl_keyboard_listener *listener =
        (const struct wl_keyboard_listener *)proxy->listener;
    struct wl_display *display = proxy->display;

    if (opcode == 0u && size == 8u) {
        int fd = wl_display_take_fd(display);

        if (fd < 0)
            return -1;
        if (listener && listener->keymap)
            listener->keymap(proxy->listener_data,
                             (struct wl_keyboard *)proxy,
                             wl_load_u32(payload), fd,
                             wl_load_u32(payload + 4u));
        else
            close(fd);
        return 0;
    }
    if (opcode == 1u && size >= 12u) {
        struct wl_surface *surface =
            wl_event_surface(display, wl_load_u32(payload + 4u));
        uint32_t key_size = wl_load_u32(payload + 8u);
        struct wl_array keys;

        if (!surface || key_size > size - 12u ||
            wl_align_u32(key_size) != size - 12u)
            return -1;
        keys.size = key_size;
        keys.alloc = key_size;
        keys.data = key_size ? (void *)(payload + 12u) : NULL;
        if (listener && listener->enter)
            listener->enter(proxy->listener_data,
                            (struct wl_keyboard *)proxy,
                            wl_load_u32(payload), surface, &keys);
        return 0;
    }
    if (opcode == 2u && size == 8u) {
        struct wl_surface *surface =
            wl_event_surface(display, wl_load_u32(payload + 4u));

        if (!surface)
            return -1;
        if (listener && listener->leave)
            listener->leave(proxy->listener_data,
                            (struct wl_keyboard *)proxy,
                            wl_load_u32(payload), surface);
        return 0;
    }
    if (opcode == 3u && size == 16u) {
        if (listener && listener->key)
            listener->key(proxy->listener_data,
                          (struct wl_keyboard *)proxy,
                          wl_load_u32(payload),
                          wl_load_u32(payload + 4u),
                          wl_load_u32(payload + 8u),
                          wl_load_u32(payload + 12u));
        return 0;
    }
    if (opcode == 4u && size == 20u) {
        if (listener && listener->modifiers)
            listener->modifiers(proxy->listener_data,
                                (struct wl_keyboard *)proxy,
                                wl_load_u32(payload),
                                wl_load_u32(payload + 4u),
                                wl_load_u32(payload + 8u),
                                wl_load_u32(payload + 12u),
                                wl_load_u32(payload + 16u));
        return 0;
    }
    if (opcode == 5u && size == 8u) {
        if (listener && listener->repeat_info)
            listener->repeat_info(proxy->listener_data,
                                  (struct wl_keyboard *)proxy,
                                  (int32_t)wl_load_u32(payload),
                                  (int32_t)wl_load_u32(payload + 4u));
        return 0;
    }
    return -1;
}

static int wl_dispatch_output_event(struct wl_proxy *proxy, uint16_t opcode,
                                    const uint8_t *payload, size_t size)
{
    const struct wl_output_listener *listener =
        (const struct wl_output_listener *)proxy->listener;

    if (opcode == 0u && size >= 28u) {
        size_t cursor = 20u;
        const char *make;
        const char *model;
        int32_t transform;

        if (wl_decode_string(payload, size, &cursor, &make) < 0 ||
            wl_decode_string(payload, size, &cursor, &model) < 0 ||
            cursor + 4u != size)
            return -1;
        transform = (int32_t)wl_load_u32(payload + cursor);
        if (listener && listener->geometry)
            listener->geometry(proxy->listener_data,
                               (struct wl_output *)proxy,
                               (int32_t)wl_load_u32(payload),
                               (int32_t)wl_load_u32(payload + 4u),
                               (int32_t)wl_load_u32(payload + 8u),
                               (int32_t)wl_load_u32(payload + 12u),
                               (int32_t)wl_load_u32(payload + 16u),
                               make, model, transform);
        return 0;
    }
    if (opcode == 1u && size == 16u) {
        if (listener && listener->mode)
            listener->mode(proxy->listener_data,
                           (struct wl_output *)proxy,
                           wl_load_u32(payload),
                           (int32_t)wl_load_u32(payload + 4u),
                           (int32_t)wl_load_u32(payload + 8u),
                           (int32_t)wl_load_u32(payload + 12u));
        return 0;
    }
    if (opcode == 2u && size == 0u) {
        if (listener && listener->done)
            listener->done(proxy->listener_data,
                           (struct wl_output *)proxy);
        return 0;
    }
    if (opcode == 3u && size == 4u) {
        if (listener && listener->scale)
            listener->scale(proxy->listener_data,
                            (struct wl_output *)proxy,
                            (int32_t)wl_load_u32(payload));
        return 0;
    }
    return -1;
}

static int wl_dispatch_surface_event(struct wl_proxy *proxy, uint16_t opcode,
                                     const uint8_t *payload, size_t size)
{
    const struct wl_surface_listener *listener =
        (const struct wl_surface_listener *)proxy->listener;
    struct wl_proxy *output;
    uint32_t output_id;

    if ((opcode != 0u && opcode != 1u) || size != 4u)
        return -1;
    output_id = wl_load_u32(payload);
    if (!(output = wl_proxy_find(proxy->display, output_id)) ||
        !wl_proxy_is(output, &wl_output_interface))
        return -1;
    if (opcode == 0u && listener && listener->enter)
        listener->enter(proxy->listener_data, (struct wl_surface *)proxy,
                        (struct wl_output *)output);
    if (opcode == 1u && listener && listener->leave)
        listener->leave(proxy->listener_data, (struct wl_surface *)proxy,
                        (struct wl_output *)output);
    return 0;
}

static int wl_dispatch_data_source_event(struct wl_proxy *proxy,
                                         uint16_t opcode,
                                         const uint8_t *payload, size_t size)
{
    const struct wl_data_source_listener *listener =
        (const struct wl_data_source_listener *)proxy->listener;
    const char *mime_type;
    size_t cursor = 0u;

    if (opcode == 0u &&
        wl_decode_nullable_string(payload, size, &cursor, &mime_type) == 0 &&
        cursor == size) {
        if (listener && listener->target)
            listener->target(proxy->listener_data,
                             (struct wl_data_source *)proxy, mime_type);
        return 0;
    }
    if (opcode == 1u &&
        wl_decode_string(payload, size, &cursor, &mime_type) == 0 &&
        cursor == size) {
        int fd = wl_display_take_fd(proxy->display);

        if (fd < 0)
            return -1;
        if (listener && listener->send)
            listener->send(proxy->listener_data,
                           (struct wl_data_source *)proxy, mime_type, fd);
        else
            close(fd);
        return 0;
    }
    if (opcode == 2u && size == 0u) {
        if (listener && listener->cancelled)
            listener->cancelled(proxy->listener_data,
                                (struct wl_data_source *)proxy);
        return 0;
    }
    return -1;
}

static int wl_dispatch_data_offer_event(struct wl_proxy *proxy,
                                        uint16_t opcode,
                                        const uint8_t *payload, size_t size)
{
    const struct wl_data_offer_listener *listener =
        (const struct wl_data_offer_listener *)proxy->listener;
    const char *mime_type;
    size_t cursor = 0u;

    if (opcode != 0u ||
        wl_decode_string(payload, size, &cursor, &mime_type) < 0 ||
        cursor != size)
        return -1;
    if (listener && listener->offer)
        listener->offer(proxy->listener_data,
                        (struct wl_data_offer *)proxy, mime_type);
    return 0;
}

static int wl_dispatch_data_device_event(struct wl_proxy *proxy,
                                         uint16_t opcode,
                                         const uint8_t *payload, size_t size)
{
    const struct wl_data_device_listener *listener =
        (const struct wl_data_device_listener *)proxy->listener;
    struct wl_display *display = proxy->display;

    if (opcode == 0u && size == 4u) {
        struct wl_data_offer *offer =
            (struct wl_data_offer *)wl_proxy_allocate_server(
                display, sizeof(*offer), &wl_data_offer_interface,
                proxy->version, wl_load_u32(payload));

        if (!offer)
            return -1;
        if (listener && listener->data_offer)
            listener->data_offer(proxy->listener_data,
                                 (struct wl_data_device *)proxy, offer);
        return 0;
    }
    if (opcode == 1u && size == 20u) {
        struct wl_proxy *surface =
            wl_proxy_find(display, wl_load_u32(payload + 4u));
        uint32_t offer_id = wl_load_u32(payload + 16u);
        struct wl_proxy *offer = offer_id ?
            wl_proxy_find(display, offer_id) : NULL;

        if (!surface || !wl_proxy_is(surface, &wl_surface_interface) ||
            (offer_id && (!offer ||
             !wl_proxy_is(offer, &wl_data_offer_interface))))
            return -1;
        if (listener && listener->enter)
            listener->enter(proxy->listener_data,
                            (struct wl_data_device *)proxy,
                            wl_load_u32(payload),
                            (struct wl_surface *)surface,
                            (wl_fixed_t)wl_load_u32(payload + 8u),
                            (wl_fixed_t)wl_load_u32(payload + 12u),
                            (struct wl_data_offer *)offer);
        return 0;
    }
    if (opcode == 2u && size == 0u) {
        if (listener && listener->leave)
            listener->leave(proxy->listener_data,
                            (struct wl_data_device *)proxy);
        return 0;
    }
    if (opcode == 3u && size == 12u) {
        if (listener && listener->motion)
            listener->motion(proxy->listener_data,
                             (struct wl_data_device *)proxy,
                             wl_load_u32(payload),
                             (wl_fixed_t)wl_load_u32(payload + 4u),
                             (wl_fixed_t)wl_load_u32(payload + 8u));
        return 0;
    }
    if (opcode == 4u && size == 0u) {
        if (listener && listener->drop)
            listener->drop(proxy->listener_data,
                           (struct wl_data_device *)proxy);
        return 0;
    }
    if (opcode == 5u && size == 4u) {
        uint32_t offer_id = wl_load_u32(payload);
        struct wl_proxy *offer = offer_id ?
            wl_proxy_find(display, offer_id) : NULL;

        if (offer_id && (!offer ||
            !wl_proxy_is(offer, &wl_data_offer_interface)))
            return -1;
        if (listener && listener->selection)
            listener->selection(proxy->listener_data,
                                (struct wl_data_device *)proxy,
                                (struct wl_data_offer *)offer);
        return 0;
    }
    return -1;
}

static int wl_dispatch_xdg_wm_base_event(struct wl_proxy *proxy,
                                         uint16_t opcode,
                                         const uint8_t *payload, size_t size)
{
    const struct xdg_wm_base_listener *listener =
        (const struct xdg_wm_base_listener *)proxy->listener;

    if (opcode != 0u || size != 4u)
        return -1;
    if (listener && listener->ping)
        listener->ping(proxy->listener_data, (struct xdg_wm_base *)proxy,
                       wl_load_u32(payload));
    return 0;
}

static int wl_dispatch_xdg_surface_event(struct wl_proxy *proxy,
                                         uint16_t opcode,
                                         const uint8_t *payload, size_t size)
{
    const struct xdg_surface_listener *listener =
        (const struct xdg_surface_listener *)proxy->listener;

    if (opcode != 0u || size != 4u)
        return -1;
    if (listener && listener->configure)
        listener->configure(proxy->listener_data,
                            (struct xdg_surface *)proxy,
                            wl_load_u32(payload));
    return 0;
}

static int wl_dispatch_xdg_toplevel_event(struct wl_proxy *proxy,
                                          uint16_t opcode,
                                          const uint8_t *payload, size_t size)
{
    const struct xdg_toplevel_listener *listener =
        (const struct xdg_toplevel_listener *)proxy->listener;

    if (opcode == 0u && size >= 12u) {
        uint32_t states_size = wl_load_u32(payload + 8u);
        struct wl_array states;

        if (states_size > size - 12u ||
            wl_align_u32(states_size) != size - 12u)
            return -1;
        states.size = states_size;
        states.alloc = states_size;
        states.data = states_size ? (void *)(payload + 12u) : NULL;
        if (listener && listener->configure)
            listener->configure(proxy->listener_data,
                                (struct xdg_toplevel *)proxy,
                                (int32_t)wl_load_u32(payload),
                                (int32_t)wl_load_u32(payload + 4u),
                                &states);
        return 0;
    }
    if (opcode == 1u && size == 0u) {
        if (listener && listener->close)
            listener->close(proxy->listener_data,
                            (struct xdg_toplevel *)proxy);
        return 0;
    }
    return -1;
}

static int wl_dispatch_registry_event(struct wl_proxy *proxy,
                                      uint16_t opcode,
                                      const uint8_t *payload, size_t size)
{
    const struct wl_registry_listener *listener =
        (const struct wl_registry_listener *)proxy->listener;
    size_t cursor = 0u;
    uint32_t name;

    if (size < 4u)
        return -1;
    name = wl_load_u32(payload);
    cursor = 4u;
    if (opcode == 0u) {
        const char *interface;
        uint32_t version;

        if (wl_decode_string(payload, size, &cursor, &interface) < 0 ||
            cursor + 4u != size)
            return -1;
        version = wl_load_u32(payload + cursor);
        if (listener && listener->global)
            listener->global(proxy->listener_data,
                             (struct wl_registry *)proxy, name,
                             interface, version);
        return 0;
    }
    if (opcode == 1u && size == 4u) {
        if (listener && listener->global_remove)
            listener->global_remove(proxy->listener_data,
                                    (struct wl_registry *)proxy, name);
        return 0;
    }
    return -1;
}

static int wl_dispatch_display_event(struct wl_display *display,
                                     uint16_t opcode,
                                     const uint8_t *payload, size_t size)
{
    if (opcode == 1u && size == 4u)
        return 0;
    if (opcode == 0u && size >= 12u) {
        const char *message;
        size_t cursor = 8u;

        if (wl_decode_string(payload, size, &cursor, &message) < 0 ||
            cursor != size)
            return -1;
        (void)message;
        display->error = EPROTO;
        errno = EPROTO;
        return -1;
    }
    return -1;
}

int wl_display_dispatch(struct wl_display *display)
{
    uint8_t header[WL_WIRE_HEADER_SIZE];
    uint8_t *payload = NULL;
    struct wl_proxy *proxy;
    uint32_t object_id;
    uint32_t word;
    uint32_t size;
    uint16_t opcode;
    int result = -1;

    if (!display || display->fd < 0) {
        errno = EINVAL;
        return -1;
    }
    if (wl_display_read_full(display, header, sizeof(header)) < 0)
        goto transport_error;
    object_id = wl_load_u32(header);
    word = wl_load_u32(header + 4u);
    opcode = (uint16_t)(word & 0xffffu);
    size = word >> 16;
    if (size < WL_WIRE_HEADER_SIZE || (size & 3u) != 0u ||
        size > WL_WIRE_MAX_MESSAGE)
        goto protocol_error;
    size -= WL_WIRE_HEADER_SIZE;
    if (size) {
        payload = malloc(size);
        if (!payload)
            return -1;
        if (wl_display_read_full(display, payload, size) < 0)
            goto transport_error;
    }
    if (!(proxy = wl_proxy_find(display, object_id)))
        goto protocol_error;
    if (wl_proxy_is(proxy, &wl_registry_interface))
        result = wl_dispatch_registry_event(proxy, opcode, payload, size);
    else if (wl_proxy_is(proxy, &wl_callback_interface) &&
             opcode == 0u && size == 4u) {
        const struct wl_callback_listener *listener =
            (const struct wl_callback_listener *)proxy->listener;

        if (listener && listener->done)
            listener->done(proxy->listener_data,
                           (struct wl_callback *)proxy,
                           wl_load_u32(payload));
        result = 0;
    } else if (wl_proxy_is(proxy, &wl_shm_interface) &&
               opcode == 0u && size == 4u) {
        const struct wl_shm_listener *listener =
            (const struct wl_shm_listener *)proxy->listener;

        if (listener && listener->format)
            listener->format(proxy->listener_data, (struct wl_shm *)proxy,
                             wl_load_u32(payload));
        result = 0;
    } else if (wl_proxy_is(proxy, &wl_buffer_interface) &&
               opcode == 0u && size == 0u) {
        const struct wl_buffer_listener *listener =
            (const struct wl_buffer_listener *)proxy->listener;

        if (listener && listener->release)
            listener->release(proxy->listener_data,
                              (struct wl_buffer *)proxy);
        result = 0;
    } else if (wl_proxy_is(proxy, &wl_seat_interface)) {
        result = wl_dispatch_seat_event(proxy, opcode, payload, size);
    } else if (wl_proxy_is(proxy, &wl_pointer_interface)) {
        result = wl_dispatch_pointer_event(proxy, opcode, payload, size);
    } else if (wl_proxy_is(proxy, &wl_keyboard_interface)) {
        result = wl_dispatch_keyboard_event(proxy, opcode, payload, size);
    } else if (wl_proxy_is(proxy, &wl_output_interface)) {
        result = wl_dispatch_output_event(proxy, opcode, payload, size);
    } else if (wl_proxy_is(proxy, &wl_surface_interface)) {
        result = wl_dispatch_surface_event(proxy, opcode, payload, size);
    } else if (wl_proxy_is(proxy, &wl_data_source_interface)) {
        result = wl_dispatch_data_source_event(proxy, opcode, payload, size);
    } else if (wl_proxy_is(proxy, &wl_data_device_interface)) {
        result = wl_dispatch_data_device_event(proxy, opcode, payload, size);
    } else if (wl_proxy_is(proxy, &wl_data_offer_interface)) {
        result = wl_dispatch_data_offer_event(proxy, opcode, payload, size);
    } else if (wl_proxy_is(proxy, &xdg_wm_base_interface)) {
        result = wl_dispatch_xdg_wm_base_event(proxy, opcode, payload, size);
    } else if (wl_proxy_is(proxy, &xdg_surface_interface)) {
        result = wl_dispatch_xdg_surface_event(proxy, opcode, payload, size);
    } else if (wl_proxy_is(proxy, &xdg_toplevel_interface)) {
        result = wl_dispatch_xdg_toplevel_event(proxy, opcode, payload, size);
    } else if (proxy == &display->proxy)
        result = wl_dispatch_display_event(display, opcode, payload, size);
    if (result < 0)
        goto protocol_error;
    free(payload);
    return 0;

transport_error:
    display->error = errno ? errno : EPIPE;
    free(payload);
    return -1;
protocol_error:
    display->error = EPROTO;
    errno = EPROTO;
    free(payload);
    return -1;
}

int wl_display_dispatch_pending(struct wl_display *display)
{
    if (!display) {
        errno = EINVAL;
        return -1;
    }
    return 0;
}

struct wl_roundtrip_state {
    int done;
};

static void wl_roundtrip_done(void *data, struct wl_callback *callback,
                              uint32_t callback_data)
{
    struct wl_roundtrip_state *state = data;

    (void)callback;
    (void)callback_data;
    state->done = 1;
}

int wl_display_roundtrip(struct wl_display *display)
{
    static const struct wl_callback_listener listener = {
        wl_roundtrip_done
    };
    struct wl_roundtrip_state state = { 0 };
    struct wl_callback *callback;

    callback = wl_display_sync(display);
    if (!callback)
        return -1;
    if (wl_callback_add_listener(callback, &listener, &state) < 0) {
        wl_callback_destroy(callback);
        return -1;
    }
    while (!state.done) {
        if (wl_display_dispatch(display) < 0) {
            wl_callback_destroy(callback);
            return -1;
        }
    }
    wl_callback_destroy(callback);
    return 0;
}
