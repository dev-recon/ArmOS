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
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <wayland-client.h>

#define WL_CLIENT_MAX_OBJECTS 512u
#define WL_WIRE_HEADER_SIZE   8u
#define WL_WIRE_MAX_MESSAGE   65535u

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
    struct wl_proxy *objects[WL_CLIENT_MAX_OBJECTS];
};

struct wl_registry {
    struct wl_proxy proxy;
};

struct wl_callback {
    struct wl_proxy proxy;
};

const struct wl_interface wl_display_interface = {
    "wl_display", 1, 2, NULL, 2, NULL
};

const struct wl_interface wl_registry_interface = {
    "wl_registry", 1, 1, NULL, 2, NULL
};

const struct wl_interface wl_callback_interface = {
    "wl_callback", 1, 0, NULL, 1, NULL
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

static int wl_read_full(int fd, void *buffer, size_t size)
{
    uint8_t *cursor = buffer;
    size_t done = 0;

    while (done < size) {
        ssize_t count = read(fd, cursor + done, size - done);

        if (count < 0 && errno == EINTR)
            continue;
        if (count <= 0)
            return -1;
        done += (size_t)count;
    }
    return 0;
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
    id = display->next_id++;
    if (id >= WL_CLIENT_MAX_OBJECTS) {
        errno = EMFILE;
        return NULL;
    }
    proxy = calloc(1, size);
    if (!proxy)
        return NULL;
    proxy->id = id;
    proxy->version = version;
    proxy->interface = interface;
    proxy->display = display;
    display->objects[id] = proxy;
    return proxy;
}

static int wl_send_words(struct wl_display *display, uint32_t object_id,
                         uint16_t opcode, const uint32_t *words,
                         size_t word_count)
{
    uint8_t message[WL_WIRE_HEADER_SIZE + 16u];
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
    display->objects[1] = &display->proxy;
    return display;
}

void wl_display_disconnect(struct wl_display *display)
{
    if (!display)
        return;
    if (display->fd >= 0)
        close(display->fd);
    for (size_t index = 2u; index < WL_CLIENT_MAX_OBJECTS; index++)
        free(display->objects[index]);
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

void wl_proxy_destroy(struct wl_proxy *proxy)
{
    struct wl_display *display;

    if (!proxy || proxy->id == 1u)
        return;
    display = proxy->display;
    if (display && proxy->id < WL_CLIENT_MAX_OBJECTS &&
        display->objects[proxy->id] == proxy)
        display->objects[proxy->id] = NULL;
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
    if (wl_read_full(display->fd, header, sizeof(header)) < 0)
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
        if (wl_read_full(display->fd, payload, size) < 0)
            goto transport_error;
    }
    if (object_id >= WL_CLIENT_MAX_OBJECTS ||
        !(proxy = display->objects[object_id]))
        goto protocol_error;
    if (proxy->interface == &wl_registry_interface)
        result = wl_dispatch_registry_event(proxy, opcode, payload, size);
    else if (proxy->interface == &wl_callback_interface &&
             opcode == 0u && size == 4u) {
        const struct wl_callback_listener *listener =
            (const struct wl_callback_listener *)proxy->listener;

        if (listener && listener->done)
            listener->done(proxy->listener_data,
                           (struct wl_callback *)proxy,
                           wl_load_u32(payload));
        result = 0;
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
