/*
 * ArmOS
 * Copyright (c) 2026 Mohamed Ennassiri
 *
 * Licensed under the Apache License, Version 2.0.
 * See LICENSE for details.
 *
 * File: include/kernel/display.h
 * Layer: Kernel / terminal and character devices
 *
 * Responsibilities:
 * - Define the common framebuffer and display-control contract.
 * - Keep rendering policy independent from platform display transports.
 *
 * Notes:
 * - Platform code decides which logical TTY receives a display backend.
 */

#ifndef _KERNEL_DISPLAY_H
#define _KERNEL_DISPLAY_H

#include <kernel/types.h>
#include <kernel/vfs.h>  /* Pour file_t */
#include <kernel/display_backend.h>

/* The common framebuffer format is fixed; geometry belongs to its backend. */
#define FB_BPP          32

/* FB_BASE sera alloue dynamiquement en RAM */
extern uint8_t* framebuffer_base;  /* CPU virtual pointer to the framebuffer */
extern paddr_t framebuffer_phys;   /* Physical/DMA address of framebuffer_base */

#define FB_BASE         ((vaddr_t)(uintptr_t)framebuffer_base)
#define DEV_FB0_RDEV   ((29u << 8) | 0u)
#define DEV_FB1_RDEV   ((29u << 8) | 1u)

#define ARMOS_FBIOGET_INFO      0x4600u
#define ARMOS_FBIOGET_ORIENTATION 0x4601u
#define ARMOS_FBIOSET_ORIENTATION 0x4602u
#define ARMOS_FBIOSET_MODE      0x4603u
#define ARMOS_FBIOACQUIRE       0x4604u
#define ARMOS_FBIORELEASE       0x4605u
#define ARMOS_FBIOBLIT          0x4606u
#define ARMOS_FBIOGET_MAP       0x4607u
#define ARMOS_FBIOPRESENT       0x4608u
#define ARMOS_FB_FORMAT_ARGB8888 1u

struct armos_fb_info {
    uint32_t width;
    uint32_t height;
    uint32_t pitch;
    uint32_t bpp;
    uint32_t size;
    uint32_t format;
};

struct armos_fb_orientation {
    uint32_t orientation;
};

struct armos_fb_mode {
    uint32_t width;
    uint32_t height;
};

struct armos_fb_blit {
    uint32_t x;
    uint32_t y;
    uint32_t width;
    uint32_t height;
    uint32_t source_pitch;
    uint64_t source;
};

struct armos_fb_map {
    uint32_t buffer_count;
    uint32_t front_buffer;
    uint32_t buffer_size;
    uint32_t mapping_size;
};

struct armos_fb_present {
    uint32_t buffer_index;
    uint32_t x;
    uint32_t y;
    uint32_t width;
    uint32_t height;
};

typedef struct {
    uint32_t width;
    uint32_t height;
    uint32_t first;
    uint32_t last;
    const uint8_t *glyphs; /* width * height alpha bytes per glyph */
} font_t;

typedef struct {
    uint32_t width;
    uint32_t height;
    uint32_t pitch;
    uint32_t bpp;
    uint8_t* framebuffer;
    uint32_t text_cols;
    uint32_t text_rows;
    uint32_t cursor_x;
    uint32_t cursor_y;
    uint32_t fg_color;
    uint32_t bg_color;
    const font_t *font;
} display_state_t;

typedef struct {
    const char *name;
    uint8_t *framebuffer;
    uint32_t width;
    uint32_t height;
    uint32_t pitch;
    uint32_t bpp;
    uint32_t size;
    uint32_t orientation;
    const display_backend_ops_t *backend;
} auxiliary_framebuffer_config_t;

extern const font_t font_meslo_12x24;
extern const font_t font_meslo_10x20;
extern const font_t font_meslo_8x16;
extern const font_t font_spleen_8x16;
extern const font_t font_spleen_12x24;
extern const font_t font_vga_8x16;

/* Display functions */
bool init_display(uint32_t width, uint32_t height, uint32_t bpp);
bool init_display_external(uint32_t width, uint32_t height, uint32_t bpp,
                           uint32_t pitch, paddr_t physical,
                           uint8_t *virtual_address, uint32_t size);
void display_set_backend(const display_backend_ops_t *backend);
const char *display_backend_name(void);
void display_flush_all(void);
int display_register_auxiliary_framebuffer(
    const auxiliary_framebuffer_config_t *config);
void display_mark_auxiliary_dirty(uint32_t x, uint32_t y,
                                  uint32_t width, uint32_t height);
void clear_screen(void);
void put_pixel(uint32_t x, uint32_t y, uint32_t color);
void draw_char(uint32_t x, uint32_t y, char c, uint32_t fg, uint32_t bg);
void console_putchar(char c);
void console_puts(const char* str);
void scroll_screen(void);
int framebuffer_attach_tty_backend(int tty_id);
void display_cursor_tick(void);
int display_start_daemon(void);
void display_scrollback_up(uint32_t lines);
void display_scrollback_down(uint32_t lines);

/* Framebuffer file operations */
ssize_t framebuffer_read(file_t* file, void* buffer, size_t count);
ssize_t framebuffer_write(file_t* file, const void* buffer, size_t count);
bool is_framebuffer_device_path(const char* path);
void fill_framebuffer_device_stat(const char *path, struct stat* st);
int framebuffer_get_info(file_t *file, struct armos_fb_info* info);
int framebuffer_get_map(file_t *file, struct armos_fb_map *map);
void *framebuffer_map_fd(int fd, void *addr, size_t length,
                         uint32_t vma_flags);
int framebuffer_present(file_t *file,
                        const struct armos_fb_present *present);
int framebuffer_get_orientation(file_t *file,
                                struct armos_fb_orientation *orientation);
int framebuffer_set_orientation(file_t *file,
                                const struct armos_fb_orientation *orientation);
int framebuffer_set_mode(file_t *file, const struct armos_fb_mode *mode);
int framebuffer_acquire(file_t *file);
int framebuffer_release(file_t *file);
int framebuffer_blit(file_t *file, const struct armos_fb_blit *blit);
file_t* create_framebuffer_device_file(const char* name, int flags);

#endif
