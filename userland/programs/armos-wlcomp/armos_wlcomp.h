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
#include <uapi/armos/drm.h>
#include <wayland-server-core.h>

#define ARMOS_WLCOMP_SOCKET_PATH "/tmp/wayland-0"

#define WL_SERVER_MAX_CLIENTS       8u
#define WL_SERVER_MAX_OBJECTS       256u
#define WL_SERVER_MAX_POOLS         16u
#define WL_SERVER_MAX_BUFFERS       64u
#define WL_SERVER_MAX_SURFACES      32u
#define WL_SERVER_MAX_REGIONS       32u
#define WL_SERVER_MAX_REGION_RECTS  16u
#define WL_SERVER_MAX_CALLBACKS     16u
#define WL_SERVER_MAX_CONFIGURES    16u
#define WL_SERVER_MAX_DAMAGE_RECTS  16u
#define WL_SERVER_MAX_RECEIVE       (64u * 1024u)
#define WL_SERVER_MAX_PENDING_FDS   16u
#define WL_SERVER_MAX_OUTPUT_BYTES  (128u * 1024u)
#define WL_SERVER_MAX_OUTPUT_MESSAGES 128u
#define WL_SERVER_MAX_DATA_SOURCES  8u
#define WL_SERVER_MAX_DATA_OFFERS   8u
#define WL_SERVER_MAX_POSITIONERS   16u
#define WL_SERVER_MAX_MIME_TYPES    8u
#define WL_SERVER_MAX_MIME_LENGTH   64u
#define WL_SERVER_MAX_TITLE_LENGTH  96u
#define WL_SERVER_CLIENT_DISPATCH_BUDGET 8u

#define WL_RENDER_TILE_SIZE 32u
#define WL_RENDER_FRAME_INTERVAL_US 16667u
#define WL_RENDER_OVERDUE_COALESCE_MS 2
#define WL_WINDOW_TITLE_HEIGHT 28u

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
#define WL_GLOBAL_ARMOS_SHELL    10u
#define WL_GLOBAL_ARMOS_GPU_BUFFER 11u

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
    WL_SERVER_OBJECT_XDG_POSITIONER,
    WL_SERVER_OBJECT_XDG_SURFACE,
    WL_SERVER_OBJECT_XDG_TOPLEVEL,
    WL_SERVER_OBJECT_XDG_POPUP,
    WL_SERVER_OBJECT_ARMOS_GPU_BUFFER_MANAGER,
    WL_SERVER_OBJECT_ARMOS_SHELL,
    WL_SERVER_OBJECT_ARMOS_SHELL_PANEL,
    WL_SERVER_OBJECT_XDG_DECORATION_MANAGER,
    WL_SERVER_OBJECT_XDG_TOPLEVEL_DECORATION
};

struct wl_server_pool;
struct wl_server_buffer;
struct wl_server_surface;
struct wl_server_client;
struct wl_server;
struct wl_gpu_image;

struct wl_renderer_rect {
    int32_t x0;
    int32_t y0;
    int32_t x1;
    int32_t y1;
};

struct wl_server_region_state {
    size_t rect_count;
    struct wl_renderer_rect rects[WL_SERVER_MAX_REGION_RECTS];
};

struct wl_server_region {
    bool used;
    uint32_t object_id;
    struct wl_server_region_state state;
};

struct wl_render_profile {
    uint64_t fill_pixels;
    uint64_t copy_pixels;
    uint64_t blend_pixels;
};

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

struct wl_server_positioner {
    bool used;
    uint32_t object_id;
    int32_t width;
    int32_t height;
    int32_t anchor_x;
    int32_t anchor_y;
    int32_t anchor_width;
    int32_t anchor_height;
    uint32_t anchor;
    uint32_t gravity;
    uint32_t constraint_adjustment;
    int32_t offset_x;
    int32_t offset_y;
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
    bool gpu_backed;
    uint32_t manager_object_id;
    uint32_t drm_handle;
    uint32_t drm_command_handle;
    uint8_t *drm_mapping;
    size_t drm_size;
    struct wl_gpu_image *gpu_image;
};

struct wl_server_callback {
    bool used;
    uint32_t object_id;
};

enum wl_server_surface_role {
    WL_SERVER_SURFACE_ROLE_NONE = 0,
    WL_SERVER_SURFACE_ROLE_TOPLEVEL,
    WL_SERVER_SURFACE_ROLE_SUBSURFACE,
    WL_SERVER_SURFACE_ROLE_POPUP,
    WL_SERVER_SURFACE_ROLE_PANEL,
    WL_SERVER_SURFACE_ROLE_CURSOR
};

enum wl_server_pointer_cursor {
    WL_SERVER_POINTER_CURSOR_ARROW = 0,
    WL_SERVER_POINTER_CURSOR_RESIZE_EW,
    WL_SERVER_POINTER_CURSOR_RESIZE_NS,
    WL_SERVER_POINTER_CURSOR_RESIZE_NWSE,
    WL_SERVER_POINTER_CURSOR_RESIZE_NESW
};

struct wl_server_surface {
    bool used;
    uint32_t object_id;
    uint64_t z_order;
    enum wl_server_surface_role role;
    bool subsurface_synchronized;
    bool subsurface_commit_pending;
    bool subsurface_position_pending;
    int32_t subsurface_x;
    int32_t subsurface_y;
    int32_t pending_subsurface_x;
    int32_t pending_subsurface_y;
    struct wl_server_surface *parent;
    struct wl_server_buffer *pending_buffer;
    struct wl_server_buffer *current_buffer;
    bool pending_attach;
    int pending_acquire_fence_fd;
    struct wl_event_source *acquire_fence_source;
    struct wl_server_client *acquire_client;
    bool acquire_commit_pending;
    bool gpu_content_ready;
    uint64_t gpu_content_generation;
    bool mapped;
    bool opaque;
    bool buffer_held;
    bool pending_opaque_region_set;
    struct wl_server_region_state pending_opaque_region;
    struct wl_server_region_state opaque_region;
    bool server_decorated;
    bool maximized;
    bool fullscreen;
    bool minimized;
    bool shaded;
    bool xdg_initial_commit_received;
    bool xdg_initial_configure_sent;
    bool xdg_configure_acked;
    uint32_t xdg_pending_configure_serial;
    uint32_t xdg_acked_configure_serial;
    uint32_t xdg_configure_serials[WL_SERVER_MAX_CONFIGURES];
    size_t xdg_configure_count;
    bool resize_from_left;
    bool resize_from_top;
    int32_t x;
    int32_t y;
    int32_t restore_x;
    int32_t restore_y;
    int32_t minimize_restore_x;
    int32_t minimize_restore_y;
    int32_t resize_anchor_right;
    int32_t resize_anchor_bottom;
    uint32_t width;
    uint32_t height;
    uint32_t restore_width;
    uint32_t restore_height;
    uint32_t minimum_width;
    uint32_t minimum_height;
    uint32_t maximum_width;
    uint32_t maximum_height;
    uint32_t *pixels;
    uint32_t pixels_pitch;
    size_t pending_damage_count;
    struct wl_renderer_rect pending_damage[WL_SERVER_MAX_DAMAGE_RECTS];
    char title[WL_SERVER_MAX_TITLE_LENGTH];
    struct wl_server_callback pending_callbacks[WL_SERVER_MAX_CALLBACKS];
    struct wl_server_callback callbacks[WL_SERVER_MAX_CALLBACKS];
};

static inline bool wl_surface_is_child_role(
    const struct wl_server_surface *surface)
{
    return surface &&
        (surface->role == WL_SERVER_SURFACE_ROLE_SUBSURFACE ||
         surface->role == WL_SERVER_SURFACE_ROLE_POPUP);
}

struct wl_server_outgoing {
    struct wl_server_outgoing *next;
    size_t size;
    size_t offset;
    int fd;
    uint8_t data[];
};

static inline bool wl_surface_has_server_decoration(
    const struct wl_server_surface *surface)
{
    return surface && surface->server_decorated && !surface->fullscreen;
}

struct wl_server_client {
    bool used;
    int fd;
    struct wl_event_source *event_source;
    struct wl_event_source *dispatch_idle;
    struct wl_server *server;
    bool dispatch_blocked;
    struct wl_server_surface *blocked_commit_root;
    uint8_t receive[WL_SERVER_MAX_RECEIVE];
    size_t receive_length;
    int pending_fds[WL_SERVER_MAX_PENDING_FDS];
    size_t pending_fd_count;
    struct wl_server_outgoing *output_head;
    struct wl_server_outgoing *output_tail;
    size_t output_bytes;
    size_t output_messages;
    uint32_t next_server_id;
    bool shell_authenticated;
    struct wl_server_object objects[WL_SERVER_MAX_OBJECTS];
    struct wl_server_pool pools[WL_SERVER_MAX_POOLS];
    struct wl_server_buffer buffers[WL_SERVER_MAX_BUFFERS];
    struct wl_server_surface surfaces[WL_SERVER_MAX_SURFACES];
    struct wl_server_region regions[WL_SERVER_MAX_REGIONS];
    struct wl_server_data_source data_sources[WL_SERVER_MAX_DATA_SOURCES];
    struct wl_server_data_offer data_offers[WL_SERVER_MAX_DATA_OFFERS];
    struct wl_server_positioner positioners[WL_SERVER_MAX_POSITIONERS];
};

enum wl_renderer_output_backend {
    WL_RENDERER_OUTPUT_HEADLESS = 0,
    WL_RENDERER_OUTPUT_FRAMEBUFFER,
    WL_RENDERER_OUTPUT_DRM,
    WL_RENDERER_OUTPUT_GPU
};

struct wl_gpu_backend;
struct wl_gpu_image;
struct wl_gpu_presenter;
struct wl_gpu_surface_cache;

struct wl_server_renderer {
    bool headless;
    bool profile_enabled;
    enum wl_renderer_output_backend output_backend;
    int framebuffer_fd;
    int drm_fd;
    uint32_t drm_handle;
    uint32_t drm_context_id;
    bool gpu_buffer_import;
    struct armos_fb_info framebuffer;
    uint32_t *canvas;
    size_t canvas_size;
    uint32_t *mapped_buffers;
    size_t mapped_buffers_size;
    uint32_t present_buffer_count;
    uint32_t present_front_buffer;
    uint32_t present_draw_buffer;
    bool direct_present;
    uint64_t *dirty_tiles;
    uint64_t *dirty_tile_storage;
    size_t dirty_tile_word_count;
    uint32_t tile_columns;
    uint32_t tile_rows;
    bool clip_enabled;
    int32_t clip_x0;
    int32_t clip_y0;
    int32_t clip_x1;
    int32_t clip_y1;
    uint64_t profile_present_us;
    uint64_t profile_present_pixels;
    uint64_t profile_gpu_imports;
    uint64_t profile_gpu_direct_blits;
    struct wl_gpu_backend *gpu_backend;
    struct wl_gpu_presenter *gpu_presenter;
    struct wl_gpu_surface_cache *gpu_surface_cache;
    size_t gpu_surface_cache_count;
    struct wl_gpu_image *gpu_pointer_images[5];
    uint64_t gpu_content_generation;
    uint32_t gpu_output_initialized_mask;
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
    uint64_t next_frame_us;
    uint64_t startup_started_us;
    bool fatal_error;
    bool exit_requested;
    bool pointer_presented;
    int32_t presented_pointer_x;
    int32_t presented_pointer_y;
    enum wl_server_pointer_cursor presented_pointer_cursor;
    uint32_t serial;
    uint32_t next_surface_position;
    uint64_t next_surface_z;
    int32_t pointer_x;
    int32_t pointer_y;
    enum wl_server_pointer_cursor pointer_cursor;
    bool pointer_left;
    uint32_t pointer_grab_serial;
    uint32_t modifiers_depressed;
    uint32_t modifiers_locked;
    uint32_t keyboard_layout;
    uint32_t shell_token;
    uint32_t panel_height;
    int32_t drag_offset_x;
    int32_t drag_offset_y;
    uint32_t resize_edges;
    int32_t resize_pointer_x;
    int32_t resize_pointer_y;
    int32_t resize_x;
    int32_t resize_y;
    uint32_t resize_width;
    uint32_t resize_height;
    uint32_t resize_initial_width;
    uint32_t resize_initial_height;
    bool damage_pending;
    uint64_t profile_started_us;
    uint64_t profile_render_us;
    uint64_t profile_frames;
    struct wl_server_client *focus_client;
    struct wl_server_surface *focus_surface;
    struct wl_server_client *pointer_client;
    struct wl_server_surface *pointer_surface;
    struct wl_server_client *drag_client;
    struct wl_server_surface *drag_surface;
    struct wl_server_client *resize_client;
    struct wl_server_surface *resize_surface;
    struct wl_server_client *selection_client;
    struct wl_server_client *shell_client;
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
int wl_client_flush_output(struct wl_server_client *client);
int wl_client_set_dispatch_blocked(struct wl_server_client *client,
                                   bool blocked);
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
int wl_server_configure_toplevel(struct wl_server *server,
    struct wl_server_client *client, struct wl_server_surface *surface,
    uint32_t width, uint32_t height, uint32_t state);
int wl_server_set_surface_minimized(struct wl_server *server,
    struct wl_server_client *client, struct wl_server_surface *surface,
    bool minimized);
void wl_server_drop_client_selection(struct wl_server *server,
                                     struct wl_server_client *client);
int wl_server_receive_client(struct wl_server *server,
                             struct wl_server_client *client);
int wl_server_dispatch_client_pending(struct wl_server *server,
                                      struct wl_server_client *client);
int wl_server_defer_client_dispatch(struct wl_server_client *client);
void wl_server_disconnect_client(struct wl_server *server,
                                 struct wl_server_client *client);

int wl_renderer_init(struct wl_server_renderer *renderer, bool headless);
void wl_renderer_destroy(struct wl_server_renderer *renderer);
void wl_render_fill_rect(uint32_t *destination, uint32_t destination_pitch,
                         uint32_t width, uint32_t height, uint32_t color);
void wl_render_copy_rect(uint32_t *destination, uint32_t destination_pitch,
                         const uint32_t *source, uint32_t source_pitch,
                         uint32_t width, uint32_t height);
void wl_render_blend_rect(uint32_t *destination, uint32_t destination_pitch,
                          const uint32_t *source, uint32_t source_pitch,
                          uint32_t width, uint32_t height);
uint32_t wl_render_blend_pixel(uint32_t destination, uint32_t source);
void wl_render_profile_set_enabled(bool enabled);
void wl_render_profile_take(struct wl_render_profile *profile);
void wl_render_arch_blend(uint32_t *destination, const uint32_t *source,
                          size_t pixels);
int wl_renderer_backend_present_rect(
    struct wl_server_renderer *renderer, int32_t x, int32_t y,
    uint32_t width, uint32_t height);
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
int wl_surface_apply_commit_tree(
    struct wl_server *server, struct wl_server_client *client,
    struct wl_server_surface *surface, bool commit_surface);
int wl_surface_release_buffer(struct wl_server_client *client,
                              struct wl_server_surface *surface);
void wl_renderer_release_surface_gpu(
    struct wl_server_renderer *renderer,
    const struct wl_server_surface *surface);
void wl_client_reclaim_buffers(struct wl_server_client *client);
void wl_client_destroy_buffers(struct wl_server_client *client);
int wl_server_handle_input(struct wl_server *server);
int wl_server_send_keymap(struct wl_server *server,
                          struct wl_server_client *client,
                          uint32_t keyboard_id);
int wl_server_broadcast_keymap(struct wl_server *server);
int wl_server_bind_output(struct wl_server *server,
                          struct wl_server_client *client,
                          uint32_t output_id, uint32_t version);
int wl_server_surface_enter_output(struct wl_server_client *client,
                                   uint32_t surface_id);

#endif /* ARMOS_WLCOMP_H */
