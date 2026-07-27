/*
 * ArmOS
 * Copyright (c) 2026 Mohamed Ennassiri
 *
 * Licensed under the Apache License, Version 2.0.
 * See LICENSE for details.
 *
 * File: userland/programs/armos-wlcomp/armos_wlcomp.h
 * Layer: Userland / graphical services
 *
 * Responsibilities:
 * - Define the bounded state used by the ArmOS Wayland compositor.
 * - Keep Wayland wire objects independent from kernel and platform details.
 * - Share protocol, rendering and event-loop contracts across server modules.
 *
 * Notes:
 * - Names beginning with wl_ are public Wayland protocol names.
 * - Core protocol versions advance only with matching client/server tests.
 */

#ifndef ARMOS_WLCOMP_H
#define ARMOS_WLCOMP_H

#include <poll.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/fb.h>
#include <sys/input.h>
#include <wayland-server-core.h>

#define ARMOS_WLCOMP_SOCKET_PATH "/tmp/wayland-0"

#define WL_SERVER_MAX_CLIENTS       8u
#define WL_SERVER_MAX_OBJECTS       256u
#define WL_SERVER_MAX_POOLS         16u
#define WL_SERVER_MAX_BUFFERS       64u
#define WL_SERVER_MAX_SURFACES      32u
#define WL_SERVER_MAX_CALLBACKS     16u
#define WL_SERVER_MAX_DAMAGE_RECTS  16u
#define WL_SERVER_MAX_RECEIVE       (64u * 1024u)
#define WL_SERVER_MAX_PENDING_FDS   16u
#define WL_SERVER_MAX_DATA_SOURCES  8u
#define WL_SERVER_MAX_DATA_OFFERS   8u
#define WL_SERVER_MAX_MIME_TYPES    8u
#define WL_SERVER_MAX_MIME_LENGTH   64u
#define WL_SERVER_MAX_TITLE_LENGTH  96u

#define WL_DISPLAY_ID 1u

#define WL_GLOBAL_COMPOSITOR 1u
#define WL_GLOBAL_SHM        2u
#define WL_GLOBAL_SEAT       3u
#define WL_GLOBAL_XDG_SHELL  4u
#define WL_GLOBAL_OUTPUT     5u
#define WL_GLOBAL_DATA_DEVICE 6u
#define WL_GLOBAL_SUBCOMPOSITOR 7u
#define WL_GLOBAL_XDG_OUTPUT  8u
#define WL_GLOBAL_XDG_DECORATION 9u

#define WL_SHM_FORMAT_ARGB8888 0u
#define WL_SHM_FORMAT_XRGB8888 1u

enum wl_server_object_type {
    WL_SERVER_OBJECT_NONE = 0,
    WL_SERVER_OBJECT_DISPLAY,
    WL_SERVER_OBJECT_REGISTRY,
    WL_SERVER_OBJECT_CALLBACK,
    WL_SERVER_OBJECT_COMPOSITOR,
    WL_SERVER_OBJECT_SUBCOMPOSITOR,
    WL_SERVER_OBJECT_SUBSURFACE,
    WL_SERVER_OBJECT_REGION,
    WL_SERVER_OBJECT_SHM,
    WL_SERVER_OBJECT_SHM_POOL,
    WL_SERVER_OBJECT_BUFFER,
    WL_SERVER_OBJECT_SURFACE,
    WL_SERVER_OBJECT_SEAT,
    WL_SERVER_OBJECT_POINTER,
    WL_SERVER_OBJECT_KEYBOARD,
    WL_SERVER_OBJECT_TOUCH,
    WL_SERVER_OBJECT_OUTPUT,
    WL_SERVER_OBJECT_XDG_OUTPUT_MANAGER,
    WL_SERVER_OBJECT_XDG_OUTPUT,
    WL_SERVER_OBJECT_DATA_DEVICE_MANAGER,
    WL_SERVER_OBJECT_DATA_SOURCE,
    WL_SERVER_OBJECT_DATA_DEVICE,
    WL_SERVER_OBJECT_DATA_OFFER,
    WL_SERVER_OBJECT_XDG_WM_BASE,
    WL_SERVER_OBJECT_XDG_SURFACE,
    WL_SERVER_OBJECT_XDG_TOPLEVEL,
    WL_SERVER_OBJECT_XDG_DECORATION_MANAGER,
    WL_SERVER_OBJECT_XDG_TOPLEVEL_DECORATION
};

struct wl_server_pool;
struct wl_server_buffer;
struct wl_server_surface;
struct wl_server_client;

struct wl_server_data_source {
    bool used;
    uint32_t object_id;
    size_t mime_count;
    char mime_types[WL_SERVER_MAX_MIME_TYPES][WL_SERVER_MAX_MIME_LENGTH];
};

struct wl_server_data_offer {
    bool used;
    uint32_t object_id;
    struct wl_server_client *source_client;
    struct wl_server_data_source *source;
};

struct wl_server_object {
    uint32_t id;
    uint32_t version;
    enum wl_server_object_type type;
    void *resource;
};

struct wl_server_pool {
    bool used;
    bool object_alive;
    uint32_t object_id;
    int fd;
    uint8_t *mapping;
    size_t size;
};

struct wl_server_buffer {
    bool used;
    bool object_alive;
    uint32_t object_id;
    struct wl_server_pool *pool;
    uint32_t offset;
    uint32_t width;
    uint32_t height;
    uint32_t stride;
    uint32_t format;
};

struct wl_server_callback {
    bool used;
    uint32_t object_id;
};

enum wl_server_surface_role {
    WL_SERVER_SURFACE_ROLE_NONE = 0,
    WL_SERVER_SURFACE_ROLE_TOPLEVEL,
    WL_SERVER_SURFACE_ROLE_SUBSURFACE,
    WL_SERVER_SURFACE_ROLE_CURSOR
};

struct wl_server_surface {
    bool used;
    uint32_t object_id;
    uint64_t z_order;
    enum wl_server_surface_role role;
    bool subsurface_synchronized;
    int32_t subsurface_x;
    int32_t subsurface_y;
    struct wl_server_surface *parent;
    struct wl_server_buffer *pending_buffer;
    bool pending_attach;
    bool mapped;
    bool opaque;
    bool server_decorated;
    int32_t x;
    int32_t y;
    uint32_t width;
    uint32_t height;
    uint32_t *pixels;
    size_t pixels_size;
    char title[WL_SERVER_MAX_TITLE_LENGTH];
    struct wl_server_callback callbacks[WL_SERVER_MAX_CALLBACKS];
};

struct wl_server_client {
    bool used;
    int fd;
    struct wl_event_source *event_source;
    uint8_t receive[WL_SERVER_MAX_RECEIVE];
    size_t receive_length;
    int pending_fds[WL_SERVER_MAX_PENDING_FDS];
    size_t pending_fd_count;
    uint32_t next_server_id;
    struct wl_server_object objects[WL_SERVER_MAX_OBJECTS];
    struct wl_server_pool pools[WL_SERVER_MAX_POOLS];
    struct wl_server_buffer buffers[WL_SERVER_MAX_BUFFERS];
    struct wl_server_surface surfaces[WL_SERVER_MAX_SURFACES];
    struct wl_server_data_source data_sources[WL_SERVER_MAX_DATA_SOURCES];
    struct wl_server_data_offer data_offers[WL_SERVER_MAX_DATA_OFFERS];
};

struct wl_server_renderer {
    bool headless;
    int framebuffer_fd;
    struct armos_fb_info framebuffer;
    uint32_t *canvas;
    size_t canvas_size;
    bool clip_enabled;
    int32_t clip_x0;
    int32_t clip_y0;
    int32_t clip_x1;
    int32_t clip_y1;
    uint32_t pointer_backing[12u * 16u];
    bool pointer_backing_valid;
};

struct wl_renderer_rect {
    int32_t x0;
    int32_t y0;
    int32_t x1;
    int32_t y1;
};

struct wl_server {
    int listen_fd;
    int input_fd;
    struct wl_display *event_display;
    struct wl_event_loop *event_loop;
    struct wl_event_source *listen_source;
    struct wl_event_source *input_source;
    struct wl_event_source *render_timer;
    bool render_pending;
    bool scene_damage_pending;
    bool fatal_error;
    bool pointer_presented;
    int32_t presented_pointer_x;
    int32_t presented_pointer_y;
    uint32_t serial;
    uint32_t next_surface_position;
    uint64_t next_surface_z;
    int32_t pointer_x;
    int32_t pointer_y;
    bool pointer_left;
    uint32_t pointer_grab_serial;
    uint32_t modifiers_depressed;
    uint32_t modifiers_locked;
    int32_t drag_offset_x;
    int32_t drag_offset_y;
    bool damage_pending;
    size_t damage_count;
    struct wl_renderer_rect damage[WL_SERVER_MAX_DAMAGE_RECTS];
    struct wl_server_client *focus_client;
    struct wl_server_surface *focus_surface;
    struct wl_server_client *pointer_client;
    struct wl_server_surface *pointer_surface;
    struct wl_server_client *drag_client;
    struct wl_server_surface *drag_surface;
    struct wl_server_client *selection_client;
    struct wl_server_data_source *selection_source;
    struct wl_server_renderer renderer;
    struct wl_server_client clients[WL_SERVER_MAX_CLIENTS];
};

uint32_t wl_wire_align(uint32_t size);
uint32_t wl_wire_u32(const uint8_t *data);
void wl_wire_store_u32(uint8_t *data, uint32_t value);

int wl_client_send_words(struct wl_server_client *client, uint32_t object_id,
                         uint16_t opcode, const uint32_t *words,
                         size_t word_count);
int wl_client_send_fd_words(struct wl_server_client *client,
                            uint32_t object_id, uint16_t opcode,
                            const uint32_t *words, size_t word_count, int fd);
int wl_client_send_string(struct wl_server_client *client,
                          uint32_t object_id, uint16_t opcode,
                          const char *text);
int wl_client_send_fd_string(struct wl_server_client *client,
                             uint32_t object_id, uint16_t opcode,
                             const char *text, int fd);
int wl_client_send_global(struct wl_server_client *client,
                          uint32_t registry_id, uint32_t name,
                          const char *interface_name, uint32_t version);
int wl_client_send_error(struct wl_server_client *client, uint32_t object_id,
                         uint32_t code, const char *message);
int wl_client_send_delete_id(struct wl_server_client *client,
                             uint32_t object_id);

struct wl_server_object *wl_client_find_object(
    struct wl_server_client *client, uint32_t object_id);
int wl_client_add_object(struct wl_server_client *client, uint32_t object_id,
                         enum wl_server_object_type type, uint32_t version,
                         void *resource);
void wl_client_remove_object(struct wl_server_client *client,
                             uint32_t object_id, bool notify);
int wl_client_take_fd(struct wl_server_client *client);

int wl_server_dispatch_message(struct wl_server *server,
                               struct wl_server_client *client,
                               const uint8_t *message, size_t size);
void wl_server_drop_client_selection(struct wl_server *server,
                                     struct wl_server_client *client);
int wl_server_receive_client(struct wl_server *server,
                             struct wl_server_client *client);
void wl_server_disconnect_client(struct wl_server *server,
                                 struct wl_server_client *client);

int wl_renderer_init(struct wl_server_renderer *renderer, bool headless);
void wl_renderer_destroy(struct wl_server_renderer *renderer);
int wl_renderer_compose(struct wl_server *server);
int wl_renderer_compose_pointer(struct wl_server *server);
void wl_renderer_damage_surface_at(
    struct wl_server *server, const struct wl_server_surface *surface,
    int32_t x, int32_t y);
void wl_renderer_damage_rect(struct wl_server *server, int32_t x, int32_t y,
                             uint32_t width, uint32_t height);
int wl_renderer_compose_damage(struct wl_server *server);
int wl_server_complete_frame_callbacks(struct wl_server *server);
int wl_server_schedule_render(struct wl_server *server, bool scene_damage);
int wl_surface_commit(struct wl_server *server,
                      struct wl_server_client *client,
                      struct wl_server_surface *surface);
int wl_server_handle_input(struct wl_server *server);
int wl_server_send_keymap(struct wl_server_client *client,
                          uint32_t keyboard_id);
int wl_server_bind_output(struct wl_server *server,
                          struct wl_server_client *client,
                          uint32_t output_id, uint32_t version);
int wl_server_surface_enter_output(struct wl_server_client *client,
                                   uint32_t surface_id);

#endif /* ARMOS_WLCOMP_H */
