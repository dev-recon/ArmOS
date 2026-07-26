/*
 * ArmOS
 * Copyright (c) 2026 Mohamed Ennassiri
 *
 * Licensed under the Apache License, Version 2.0.
 * See LICENSE for details.
 *
 * File: kernel/drivers/display.c
 * Layer: Kernel / terminal and character devices
 *
 * Responsibilities:
 * - Render the common framebuffer console and expose /dev/fb0.
 * - Delegate hardware flush, resize, and orientation to display backends.
 *
 * Notes:
 * - Logical TTY selection remains a platform registration decision.
 */

#include <kernel/display.h>
#include <kernel/memory.h>
#include <kernel/address_space.h>
#include <kernel/uart.h>
#include <kernel/kprintf.h>
#include <kernel/tty.h>
#include <kernel/timer.h>
#include <kernel/string.h>
#include <kernel/task.h>
#include <kernel/spinlock.h>
#include <kernel/arch_memory.h>

extern file_t* create_file(void);

/* displayd frame period: ~60 Hz coalesces bursts into one GPU flush. */
#define DISPLAYD_FRAME_MS 16

/*
 * displayd is short-lived work but latency-sensitive: if it stays at normal
 * user priority, CPU stress can starve graphical refresh while tty0 remains
 * perfectly alive. Give it a modest kernel priority and let task_sleep_ms()
 * keep its CPU cost bounded.
 */
#define DISPLAYD_PRIORITY 5

static display_state_t display = {0};
static const display_backend_ops_t *display_backend;
static uint32_t framebuffer_capacity;
static uint32_t framebuffer_orientation = ARMOS_FB_ORIENTATION_PORTRAIT;
static int framebuffer_tty_id = -1;
static spinlock_t display_geometry_lock = SPINLOCK_INIT("fb_geometry");
static spinlock_t display_console_lock = SPINLOCK_INIT("fb_console");
static spinlock_t framebuffer_owner_lock = SPINLOCK_INIT("fb_owner");
static file_t *framebuffer_owner;
static volatile bool framebuffer_user_owned;

/*
 * Dirty rectangle shared between producers and the flusher.
 *
 * Producers (TTY drain in IRQ context, console writes on any CPU) only render
 * into the framebuffer and extend this rectangle under dirty_lock. Once
 * displayd is running it is the only GPU submitter: no VirtIO command is ever
 * issued from IRQ context, and a burst of output costs a single frame flush.
 */
static spinlock_t dirty_lock = SPINLOCK_INIT("fb_dirty");
static volatile bool displayd_running = false;
static bool framebuffer_dirty = false;
static uint32_t dirty_x0;
static uint32_t dirty_y0;
static uint32_t dirty_x1;
static uint32_t dirty_y1;

typedef struct {
    bool registered;
    const char *name;
    uint8_t *framebuffer;
    uint32_t width;
    uint32_t height;
    uint32_t pitch;
    uint32_t bpp;
    uint32_t size;
    uint32_t orientation;
    const display_backend_ops_t *backend;
    bool dirty;
    uint32_t dirty_x0;
    uint32_t dirty_y0;
    uint32_t dirty_x1;
    uint32_t dirty_y1;
    spinlock_t lock;
} auxiliary_framebuffer_t;

static auxiliary_framebuffer_t auxiliary_fb = {
    .lock = SPINLOCK_INIT("fb1"),
};

/* Variable globale pour le framebuffer */
uint8_t* framebuffer_base = NULL;
paddr_t framebuffer_phys = 0;

#define CONSOLE_MAX_COLS 240
#define CONSOLE_MAX_ROWS 68
#define SCROLLBACK_ROWS 256

static const font_t *display_font_for_geometry(uint32_t width, uint32_t height)
{
    if (width >= font_vga_8x16.width && height >= font_vga_8x16.height &&
        width / font_vga_8x16.width <= CONSOLE_MAX_COLS &&
        height / font_vga_8x16.height <= CONSOLE_MAX_ROWS)
        return &font_vga_8x16;
    if (width >= font_meslo_10x20.width && height >= font_meslo_10x20.height &&
        width / font_meslo_10x20.width <= CONSOLE_MAX_COLS &&
        height / font_meslo_10x20.height <= CONSOLE_MAX_ROWS)
        return &font_meslo_10x20;
    if (width >= font_meslo_12x24.width && height >= font_meslo_12x24.height &&
        width / font_meslo_12x24.width <= CONSOLE_MAX_COLS &&
        height / font_meslo_12x24.height <= CONSOLE_MAX_ROWS)
        return &font_meslo_12x24;
    return NULL;
}

typedef struct {
    char ch;
    uint32_t fg;
    uint32_t bg;
} console_cell_t;

static console_cell_t console_cells[CONSOLE_MAX_ROWS][CONSOLE_MAX_COLS];
static console_cell_t scrollback_cells[SCROLLBACK_ROWS][CONSOLE_MAX_COLS];
static bool console_cells_ready = false;
static uint32_t scrollback_head;
static uint32_t scrollback_count;
static uint32_t scrollback_offset;
static volatile int32_t scrollback_pending_lines;

typedef enum {
    ANSI_STATE_NORMAL = 0,
    ANSI_STATE_ESC,
    ANSI_STATE_CSI,
} ansi_state_t;

static ansi_state_t ansi_state = ANSI_STATE_NORMAL;
static uint32_t ansi_params[8];
static uint32_t ansi_param_count;
static bool ansi_param_active;
static bool ansi_reverse = false;
static bool cursor_visible = true;
static bool cursor_drawn = false;
static bool cursor_blink_on = true;
static bool wrap_enabled = true;
static bool pending_wrap = false;
static uint32_t cursor_drawn_x;
static uint32_t cursor_drawn_y;
static uint32_t cursor_blink_last_tick;
static uint32_t saved_cursor_x;
static uint32_t saved_cursor_y;
static task_t *displayd_task;

static void console_put_cell(uint32_t col, uint32_t row, char c,
                             uint32_t fg, uint32_t bg);
static void console_erase_cursor(void);
static void console_render_scrollback(void);
static void display_process_scrollback_request(void);

#define CURSOR_BLINK_TICKS      (TIMER_FREQ / 2)
#define CURSOR_BAR_MIN_WIDTH    2

static const uint32_t ansi_colors[8] = {
    0xFF000000, /* black */
    0xFFAA0000, /* red */
    0xFF00AA00, /* green */
    0xFFAA5500, /* yellow/brown */
    0xFF0000AA, /* blue */
    0xFFAA00AA, /* magenta */
    0xFF00AAAA, /* cyan */
    0xFFAAAAAA, /* white */
};

static const uint32_t ansi_bright_colors[8] = {
    0xFF555555, /* bright black */
    0xFFFF5555, /* bright red */
    0xFF55FF55, /* bright green */
    0xFFFFFF55, /* bright yellow */
    0xFF5555FF, /* bright blue */
    0xFFFF55FF, /* bright magenta */
    0xFF55FFFF, /* bright cyan */
    0xFFFFFFFF, /* bright white */
};

static const uint32_t default_fg_color = 0xFFFFFFFF;
static const uint32_t default_bg_color = 0xFF000000;

static void console_reset_attrs(void)
{
    display.fg_color = default_fg_color;
    display.bg_color = default_bg_color;
    ansi_reverse = false;
}

static void framebuffer_mark_dirty(uint32_t x, uint32_t y,
                                   uint32_t width, uint32_t height)
{
    unsigned long flags;

    if (!framebuffer_base || width == 0 || height == 0)
        return;

    if (x >= display.width || y >= display.height)
        return;
    if (x + width > display.width)
        width = display.width - x;
    if (y + height > display.height)
        height = display.height - y;

    spin_lock_irqsave(&dirty_lock, &flags);
    if (!framebuffer_dirty) {
        dirty_x0 = x;
        dirty_y0 = y;
        dirty_x1 = x + width;
        dirty_y1 = y + height;
        framebuffer_dirty = true;
    } else {
        if (x < dirty_x0)
            dirty_x0 = x;
        if (y < dirty_y0)
            dirty_y0 = y;
        if (x + width > dirty_x1)
            dirty_x1 = x + width;
        if (y + height > dirty_y1)
            dirty_y1 = y + height;
    }
    spin_unlock_irqrestore(&dirty_lock, flags);
}

static void framebuffer_flush_dirty(void)
{
    const display_backend_ops_t *backend;
    uint8_t *framebuffer;
    uint32_t pitch;
    unsigned long console_flags;

    if (!display_backend || !display_backend->flush_rect)
        return;

    spin_lock_irqsave(&display_console_lock, &console_flags);
    if (!framebuffer_user_owned) {
        if (cursor_drawn && !cursor_blink_on)
            console_erase_cursor();

        if (cursor_visible && framebuffer_base && display.font) {
            if (cursor_drawn &&
                (cursor_drawn_x != display.cursor_x ||
                 cursor_drawn_y != display.cursor_y))
                console_erase_cursor();
        } else if (cursor_drawn) {
            console_erase_cursor();
        }

        if (cursor_visible && framebuffer_base && display.font &&
            scrollback_offset == 0 &&
            cursor_blink_on &&
            display.cursor_x < display.text_cols &&
            display.cursor_y < display.text_rows &&
            display.cursor_x < CONSOLE_MAX_COLS &&
            display.cursor_y < CONSOLE_MAX_ROWS &&
            !cursor_drawn) {
            uint32_t x = display.cursor_x * display.font->width;
            uint32_t y = display.cursor_y * display.font->height;
            uint32_t bar_width = display.font->width / 5;

            if (bar_width < CURSOR_BAR_MIN_WIDTH)
                bar_width = CURSOR_BAR_MIN_WIDTH;
            if (bar_width > display.font->width)
                bar_width = display.font->width;

            for (uint32_t yy = 0; yy < display.font->height; yy++) {
                for (uint32_t xx = 0; xx < bar_width; xx++)
                    put_pixel(x + xx, y + yy, default_fg_color);
            }
            framebuffer_mark_dirty(x, y, bar_width, display.font->height);
            cursor_drawn = true;
            cursor_drawn_x = display.cursor_x;
            cursor_drawn_y = display.cursor_y;
        }
    }
    spin_unlock_irqrestore(&display_console_lock, console_flags);

    uint32_t x0, y0, x1, y1;
    unsigned long flags;

    spin_lock(&display_geometry_lock);
    spin_lock_irqsave(&dirty_lock, &flags);
    if (!framebuffer_dirty) {
        spin_unlock_irqrestore(&dirty_lock, flags);
        spin_unlock(&display_geometry_lock);
        return;
    }
    x0 = dirty_x0;
    y0 = dirty_y0;
    x1 = dirty_x1;
    y1 = dirty_y1;
    backend = display_backend;
    framebuffer = framebuffer_base;
    pitch = display.pitch;
    framebuffer_dirty = false;
    spin_unlock_irqrestore(&dirty_lock, flags);
    spin_unlock(&display_geometry_lock);

    /* Submit outside the lock: device I/O is the slow part. */
    backend->flush_rect(framebuffer, pitch, x0, y0, x1 - x0, y1 - y0);
}

void display_set_backend(const display_backend_ops_t *backend)
{
    display_backend = backend;
}

const char *display_backend_name(void)
{
    return display_backend && display_backend->name ?
           display_backend->name : "none";
}

void display_flush_all(void)
{
    if (!framebuffer_base)
        return;

    framebuffer_mark_dirty(0, 0, display.width, display.height);
    framebuffer_flush_dirty();
}

void display_cursor_tick(void)
{
    uint32_t now;

    if (!framebuffer_base || !display.font || !cursor_visible)
        return;

    now = get_system_ticks();
    if ((uint32_t)(now - cursor_blink_last_tick) < CURSOR_BLINK_TICKS)
        return;

    cursor_blink_last_tick = now;
    cursor_blink_on = !cursor_blink_on;
    /* displayd flushes right after this tick; no dedicated flush needed. */
}

/*
 * Request a screen update after rendering.
 *
 * Once displayd runs it is the sole GPU submitter: producers only extend the
 * dirty rectangle and the next ~16 ms frame flush picks it up. Before the
 * daemon starts (early boot), flush synchronously so boot messages stay
 * visible even if the system never reaches the scheduler.
 */
static void display_request_flush(void)
{
    if (!displayd_running)
        framebuffer_flush_dirty();
}

int display_register_auxiliary_framebuffer(
    const auxiliary_framebuffer_config_t *config)
{
    uint64_t required;

    if (!config || !config->framebuffer || !config->backend ||
        !config->backend->flush_rect || !config->width || !config->height ||
        config->bpp != 32u || config->pitch != config->width * 4u)
        return -EINVAL;

    required = (uint64_t)config->pitch * config->height;
    if (required > config->size || required > 0xffffffffu)
        return -EINVAL;

    spin_lock(&auxiliary_fb.lock);
    auxiliary_fb.registered = true;
    auxiliary_fb.name = config->name;
    auxiliary_fb.framebuffer = config->framebuffer;
    auxiliary_fb.width = config->width;
    auxiliary_fb.height = config->height;
    auxiliary_fb.pitch = config->pitch;
    auxiliary_fb.bpp = config->bpp;
    auxiliary_fb.size = config->size;
    auxiliary_fb.orientation = config->orientation;
    auxiliary_fb.backend = config->backend;
    auxiliary_fb.dirty = true;
    auxiliary_fb.dirty_x0 = 0;
    auxiliary_fb.dirty_y0 = 0;
    auxiliary_fb.dirty_x1 = config->width;
    auxiliary_fb.dirty_y1 = config->height;
    spin_unlock(&auxiliary_fb.lock);
    return 0;
}

void display_mark_auxiliary_dirty(uint32_t x, uint32_t y,
                                  uint32_t width, uint32_t height)
{
    uint32_t x1;
    uint32_t y1;

    spin_lock(&auxiliary_fb.lock);
    if (!auxiliary_fb.registered || !width || !height ||
        x >= auxiliary_fb.width || y >= auxiliary_fb.height) {
        spin_unlock(&auxiliary_fb.lock);
        return;
    }

    x1 = MIN(x + width, auxiliary_fb.width);
    y1 = MIN(y + height, auxiliary_fb.height);
    if (!auxiliary_fb.dirty) {
        auxiliary_fb.dirty_x0 = x;
        auxiliary_fb.dirty_y0 = y;
        auxiliary_fb.dirty_x1 = x1;
        auxiliary_fb.dirty_y1 = y1;
        auxiliary_fb.dirty = true;
    } else {
        auxiliary_fb.dirty_x0 = MIN(auxiliary_fb.dirty_x0, x);
        auxiliary_fb.dirty_y0 = MIN(auxiliary_fb.dirty_y0, y);
        auxiliary_fb.dirty_x1 = MAX(auxiliary_fb.dirty_x1, x1);
        auxiliary_fb.dirty_y1 = MAX(auxiliary_fb.dirty_y1, y1);
    }
    spin_unlock(&auxiliary_fb.lock);
}

static void display_flush_auxiliary(void)
{
    const display_backend_ops_t *backend;
    uint8_t *buffer;
    uint32_t pitch;
    uint32_t x;
    uint32_t y;
    uint32_t width;
    uint32_t height;

    spin_lock(&auxiliary_fb.lock);
    if (!auxiliary_fb.registered || !auxiliary_fb.dirty) {
        spin_unlock(&auxiliary_fb.lock);
        return;
    }

    backend = auxiliary_fb.backend;
    buffer = auxiliary_fb.framebuffer;
    pitch = auxiliary_fb.pitch;
    x = auxiliary_fb.dirty_x0;
    y = auxiliary_fb.dirty_y0;
    width = auxiliary_fb.dirty_x1 - x;
    height = auxiliary_fb.dirty_y1 - y;
    auxiliary_fb.dirty = false;
    spin_unlock(&auxiliary_fb.lock);

    backend->flush_rect(buffer, pitch, x, y, width, height);
}

static void displayd_main(void *arg)
{
    (void)arg;

    displayd_running = true;

    while (1) {
        task_sleep_ms(DISPLAYD_FRAME_MS);

        if (display_backend && display_backend->check_resize &&
            display_backend->check_resize())
            framebuffer_mark_dirty(0, 0, display.width, display.height);

        display_process_scrollback_request();
        display_cursor_tick();
        framebuffer_flush_dirty();
        display_flush_auxiliary();
    }
}

int display_start_daemon(void)
{
    if (!framebuffer_base)
        return -ENODEV;

    if (displayd_task)
        return 0;

    displayd_task = task_create_process("displayd", displayd_main, NULL,
                                        DISPLAYD_PRIORITY, TASK_TYPE_KERNEL);
    if (!displayd_task)
        return -ENOMEM;

    arch_task_context_mark_first_run(&displayd_task->context);
    arch_task_context_set_address_space(&displayd_task->context,
                                        arch_kernel_address_space_context(),
                                        ASID_KERNEL);
    arch_task_context_set_returns_to_user(&displayd_task->context, false);
    add_to_ready_queue(displayd_task);
    return 0;
}

static void console_reset_cells(void)
{
    uint32_t rows = display.text_rows;
    uint32_t cols = display.text_cols;

    if (rows > CONSOLE_MAX_ROWS)
        rows = CONSOLE_MAX_ROWS;
    if (cols > CONSOLE_MAX_COLS)
        cols = CONSOLE_MAX_COLS;

    for (uint32_t y = 0; y < rows; y++) {
        for (uint32_t x = 0; x < cols; x++) {
            console_cells[y][x].ch = ' ';
            console_cells[y][x].fg = display.fg_color;
            console_cells[y][x].bg = display.bg_color;
        }
    }
    console_cells_ready = true;
    cursor_drawn = false;
    scrollback_offset = 0;
}

static void console_resize_cells(uint32_t old_rows, uint32_t old_cols,
                                 uint32_t new_rows, uint32_t new_cols)
{
    uint32_t keep_rows = old_rows < new_rows ? old_rows : new_rows;
    uint32_t keep_cols = old_cols < new_cols ? old_cols : new_cols;

    if (keep_rows > CONSOLE_MAX_ROWS)
        keep_rows = CONSOLE_MAX_ROWS;
    if (keep_cols > CONSOLE_MAX_COLS)
        keep_cols = CONSOLE_MAX_COLS;

    for (uint32_t row = 0; row < CONSOLE_MAX_ROWS; row++) {
        for (uint32_t col = 0; col < CONSOLE_MAX_COLS; col++) {
            if (row < keep_rows && col < keep_cols)
                continue;
            console_cells[row][col].ch = ' ';
            console_cells[row][col].fg = default_fg_color;
            console_cells[row][col].bg = default_bg_color;
        }
    }
}

static void console_reset_scrollback(void)
{
    scrollback_head = 0;
    scrollback_count = 0;
    scrollback_offset = 0;
    scrollback_pending_lines = 0;
}

static uint32_t scrollback_oldest_index(void)
{
    if (scrollback_count < SCROLLBACK_ROWS)
        return 0;
    return scrollback_head;
}

static uint32_t scrollback_index(uint32_t logical_line)
{
    return (scrollback_oldest_index() + logical_line) % SCROLLBACK_ROWS;
}

static void scrollback_push_line(const console_cell_t *line, uint32_t cols)
{
    if (!line || cols == 0)
        return;

    if (cols > CONSOLE_MAX_COLS)
        cols = CONSOLE_MAX_COLS;

    memcpy(scrollback_cells[scrollback_head], line,
           cols * sizeof(console_cell_t));
    for (uint32_t col = cols; col < CONSOLE_MAX_COLS; col++) {
        scrollback_cells[scrollback_head][col].ch = ' ';
        scrollback_cells[scrollback_head][col].fg = default_fg_color;
        scrollback_cells[scrollback_head][col].bg = default_bg_color;
    }

    scrollback_head = (scrollback_head + 1) % SCROLLBACK_ROWS;
    if (scrollback_count < SCROLLBACK_ROWS)
        scrollback_count++;
}

static void draw_cell_snapshot(uint32_t col, uint32_t row,
                               const console_cell_t *cell)
{
    char ch = cell && cell->ch ? cell->ch : ' ';
    uint32_t fg = cell ? cell->fg : default_fg_color;
    uint32_t bg = cell ? cell->bg : default_bg_color;

    draw_char(col * display.font->width,
              row * display.font->height,
              ch, fg, bg);
}

static void console_render_scrollback(void)
{
    uint32_t rows = display.text_rows;
    uint32_t cols = display.text_cols;
    uint32_t total_lines;
    uint32_t bottom_start;
    uint32_t start_line;

    if (!display.font || !console_cells_ready)
        return;

    if (rows > CONSOLE_MAX_ROWS)
        rows = CONSOLE_MAX_ROWS;
    if (cols > CONSOLE_MAX_COLS)
        cols = CONSOLE_MAX_COLS;
    if (rows == 0 || cols == 0)
        return;

    console_erase_cursor();

    total_lines = scrollback_count + rows;
    bottom_start = total_lines > rows ? total_lines - rows : 0;
    if (scrollback_offset > bottom_start)
        scrollback_offset = bottom_start;
    start_line = bottom_start - scrollback_offset;

    for (uint32_t row = 0; row < rows; row++) {
        uint32_t logical = start_line + row;

        if (logical < scrollback_count) {
            uint32_t idx = scrollback_index(logical);
            for (uint32_t col = 0; col < cols; col++)
                draw_cell_snapshot(col, row, &scrollback_cells[idx][col]);
        } else {
            uint32_t visible_row = logical - scrollback_count;
            for (uint32_t col = 0; col < cols; col++)
                draw_cell_snapshot(col, row, &console_cells[visible_row][col]);
        }
    }

    framebuffer_mark_dirty(0, 0, display.width, display.height);
}

static void console_scrollback_reset_view(void)
{
    if (scrollback_offset == 0)
        return;

    scrollback_offset = 0;
    console_render_scrollback();
}

static void display_scrollback_apply_up(uint32_t lines)
{
    if (!framebuffer_base || !display.font || lines == 0 || scrollback_count == 0)
        return;

    if (scrollback_offset + lines > scrollback_count)
        scrollback_offset = scrollback_count;
    else
        scrollback_offset += lines;

    console_render_scrollback();
}

static void display_scrollback_apply_down(uint32_t lines)
{
    if (!framebuffer_base || !display.font || lines == 0 || scrollback_offset == 0)
        return;

    if (lines >= scrollback_offset)
        scrollback_offset = 0;
    else
        scrollback_offset -= lines;

    console_render_scrollback();
}

void display_scrollback_up(uint32_t lines)
{
    uint32_t irq_flags;

    if (lines > SCROLLBACK_ROWS)
        lines = SCROLLBACK_ROWS;

    irq_flags = disable_interrupts_save();
    scrollback_pending_lines += (int32_t)lines;
    restore_interrupts(irq_flags);
}

void display_scrollback_down(uint32_t lines)
{
    uint32_t irq_flags;

    if (lines > SCROLLBACK_ROWS)
        lines = SCROLLBACK_ROWS;

    irq_flags = disable_interrupts_save();
    scrollback_pending_lines -= (int32_t)lines;
    restore_interrupts(irq_flags);
}

static void display_process_scrollback_request(void)
{
    unsigned long console_flags;
    uint32_t irq_flags;
    int32_t lines;

    irq_flags = disable_interrupts_save();
    lines = scrollback_pending_lines;
    scrollback_pending_lines = 0;
    restore_interrupts(irq_flags);

    if (lines == 0)
        return;

    spin_lock_irqsave(&display_console_lock, &console_flags);
    if (lines > 0)
        display_scrollback_apply_up((uint32_t)lines);
    else
        display_scrollback_apply_down((uint32_t)-lines);
    spin_unlock_irqrestore(&display_console_lock, console_flags);
}

static uint32_t ansi_param_or(uint32_t index, uint32_t fallback)
{
    if (index >= ansi_param_count)
        return fallback;
    return ansi_params[index] ? ansi_params[index] : fallback;
}

static uint32_t console_fg(void)
{
    return ansi_reverse ? display.bg_color : display.fg_color;
}

static uint32_t console_bg(void)
{
    return ansi_reverse ? display.fg_color : display.bg_color;
}

static void console_clear_cell(uint32_t col, uint32_t row)
{
    console_put_cell(col, row, ' ', console_fg(), console_bg());
}

static void console_clear_line_range(uint32_t row, uint32_t first_col,
                                     uint32_t last_col)
{
    if (!display.font || row >= display.text_rows ||
        first_col >= display.text_cols)
        return;

    if (last_col >= display.text_cols)
        last_col = display.text_cols - 1;

    for (uint32_t col = first_col; col <= last_col; col++)
        console_clear_cell(col, row);
}

static void console_clear_line_from_cursor(void)
{
    console_clear_line_range(display.cursor_y, display.cursor_x,
                             display.text_cols - 1);
}

static void console_clear_line_to_cursor(void)
{
    console_clear_line_range(display.cursor_y, 0, display.cursor_x);
}

static void console_clear_line_full(void)
{
    console_clear_line_range(display.cursor_y, 0, display.text_cols - 1);
}

static void console_clear_from_cursor_down(void)
{
    if (!display.font)
        return;

    console_clear_line_from_cursor();
    for (uint32_t row = display.cursor_y + 1; row < display.text_rows; row++)
        console_clear_line_range(row, 0, display.text_cols - 1);
}

static void console_clear_to_cursor_up(void)
{
    if (!display.font)
        return;

    for (uint32_t row = 0; row < display.cursor_y; row++)
        console_clear_line_range(row, 0, display.text_cols - 1);
    console_clear_line_to_cursor();
}

static void console_save_cursor(void)
{
    saved_cursor_x = display.cursor_x;
    saved_cursor_y = display.cursor_y;
}

static void console_restore_cursor(void)
{
    display.cursor_x = saved_cursor_x;
    display.cursor_y = saved_cursor_y;
    pending_wrap = false;

    if (display.cursor_x >= display.text_cols)
        display.cursor_x = display.text_cols ? display.text_cols - 1 : 0;
    if (display.cursor_y >= display.text_rows)
        display.cursor_y = display.text_rows ? display.text_rows - 1 : 0;
}

static void console_erase_cursor(void)
{
    if (!cursor_drawn || !display.font)
        return;

    if (cursor_drawn_x < display.text_cols &&
        cursor_drawn_y < display.text_rows &&
        cursor_drawn_x < CONSOLE_MAX_COLS &&
        cursor_drawn_y < CONSOLE_MAX_ROWS) {
        console_cell_t *cell = &console_cells[cursor_drawn_y][cursor_drawn_x];
        draw_char(cursor_drawn_x * display.font->width,
                  cursor_drawn_y * display.font->height,
                  cell->ch, cell->fg, cell->bg);
        framebuffer_mark_dirty(cursor_drawn_x * display.font->width,
                               cursor_drawn_y * display.font->height,
                               display.font->width,
                               display.font->height);
    }

    cursor_drawn = false;
}

static void console_linefeed(void)
{
    display.cursor_y++;
    if (display.cursor_y >= display.text_rows)
        scroll_screen();
}

static void console_printable_char(char c)
{
    if (!display.text_cols || !display.text_rows)
        return;

    if (pending_wrap && wrap_enabled) {
        display.cursor_x = 0;
        console_linefeed();
        pending_wrap = false;
    }

    console_put_cell(display.cursor_x, display.cursor_y, c,
                     console_fg(), console_bg());

    if (display.cursor_x + 1 >= display.text_cols) {
        if (wrap_enabled)
            pending_wrap = true;
    } else {
        display.cursor_x++;
        pending_wrap = false;
    }
}

static void console_apply_sgr(void)
{
    bool bold = false;

    if (ansi_param_count == 0) {
        console_reset_attrs();
        return;
    }

    for (uint32_t i = 0; i < ansi_param_count; i++) {
        uint32_t p = ansi_params[i];

        if (p == 0) {
            bold = false;
            console_reset_attrs();
        } else if (p == 1) {
            bold = true;
        } else if (p == 7) {
            ansi_reverse = true;
        } else if (p == 22) {
            bold = false;
        } else if (p == 27) {
            ansi_reverse = false;
        } else if (p >= 30 && p <= 37) {
            display.fg_color = bold ? ansi_bright_colors[p - 30] :
                                      ansi_colors[p - 30];
        } else if (p == 39) {
            display.fg_color = default_fg_color;
        } else if (p >= 40 && p <= 47) {
            display.bg_color = ansi_colors[p - 40];
        } else if (p == 49) {
            display.bg_color = default_bg_color;
        } else if (p >= 90 && p <= 97) {
            display.fg_color = ansi_bright_colors[p - 90];
        } else if (p >= 100 && p <= 107) {
            display.bg_color = ansi_bright_colors[p - 100];
        }
    }
}

static void console_handle_csi(char final)
{
    uint32_t n;

    switch (final) {
        case 'A':
            n = ansi_param_or(0, 1);
            display.cursor_y = n > display.cursor_y ? 0 : display.cursor_y - n;
            pending_wrap = false;
            break;
        case 'B':
            n = ansi_param_or(0, 1);
            display.cursor_y += n;
            if (display.cursor_y >= display.text_rows)
                display.cursor_y = display.text_rows - 1;
            pending_wrap = false;
            break;
        case 'C':
            n = ansi_param_or(0, 1);
            display.cursor_x += n;
            if (display.cursor_x >= display.text_cols)
                display.cursor_x = display.text_cols - 1;
            pending_wrap = false;
            break;
        case 'D':
            n = ansi_param_or(0, 1);
            display.cursor_x = n > display.cursor_x ? 0 : display.cursor_x - n;
            pending_wrap = false;
            break;
        case 'H':
        case 'f': {
            uint32_t row = ansi_param_or(0, 1);
            uint32_t col = ansi_param_or(1, 1);
            display.cursor_y = row > 0 ? row - 1 : 0;
            display.cursor_x = col > 0 ? col - 1 : 0;
            if (display.cursor_y >= display.text_rows)
                display.cursor_y = display.text_rows - 1;
            if (display.cursor_x >= display.text_cols)
                display.cursor_x = display.text_cols - 1;
            pending_wrap = false;
            break;
        }
        case 'J':
            n = (ansi_param_count == 0) ? 0 : ansi_params[0];
            if (n == 0)
                console_clear_from_cursor_down();
            else if (n == 1)
                console_clear_to_cursor_up();
            else if (n == 2 || n == 3)
                clear_screen();
            break;
        case 'K':
            n = (ansi_param_count == 0) ? 0 : ansi_params[0];
            if (n == 0)
                console_clear_line_from_cursor();
            else if (n == 1)
                console_clear_line_to_cursor();
            else if (n == 2)
                console_clear_line_full();
            break;
        case 'm':
            console_apply_sgr();
            break;
        case 's':
            console_save_cursor();
            break;
        case 'u':
            console_restore_cursor();
            break;
        case 'h':
            if (ansi_params[0] == 25)
                cursor_visible = true;
            else if (ansi_params[0] == 7)
                wrap_enabled = true;
            break;
        case 'l':
            if (ansi_params[0] == 25)
                cursor_visible = false;
            else if (ansi_params[0] == 7)
                wrap_enabled = false;
            break;
        default:
            break;
    }
}

static bool console_is_csi_final(char c)
{
    return c == 'A' || c == 'B' || c == 'C' || c == 'D' ||
           c == 'H' || c == 'f' || c == 'J' || c == 'K' ||
           c == 'm' || c == 'h' || c == 'l' || c == 's' ||
           c == 'u';
}

static bool console_consume_ansi(char c)
{
    switch (ansi_state) {
        case ANSI_STATE_NORMAL:
            if ((uint8_t)c == 0x1B) {
                ansi_state = ANSI_STATE_ESC;
                return true;
            }
            return false;

        case ANSI_STATE_ESC:
            if (c == '[') {
                memset(ansi_params, 0, sizeof(ansi_params));
                ansi_param_count = 0;
                ansi_param_active = false;
                ansi_state = ANSI_STATE_CSI;
                return true;
            }
            if (c == '7') {
                console_save_cursor();
                ansi_state = ANSI_STATE_NORMAL;
                return true;
            }
            if (c == '8') {
                console_restore_cursor();
                ansi_state = ANSI_STATE_NORMAL;
                return true;
            }
            if (c == 'c') {
                console_reset_attrs();
                clear_screen();
                ansi_state = ANSI_STATE_NORMAL;
                return true;
            }
            ansi_state = ANSI_STATE_NORMAL;
            return true;

        case ANSI_STATE_CSI:
            if (c == '?' || c == '>' || c == '=') {
                return true;
            }
            if (c >= '0' && c <= '9') {
                if (ansi_param_count == 0)
                    ansi_param_count = 1;
                ansi_param_active = true;
                ansi_params[ansi_param_count - 1] =
                    ansi_params[ansi_param_count - 1] * 10u + (uint32_t)(c - '0');
                return true;
            }
            if (c == ';') {
                if (ansi_param_count == 0)
                    ansi_param_count = 1;
                if (ansi_param_count < 8)
                    ansi_param_count++;
                ansi_param_active = false;
                return true;
            }
            if (c == ':') {
                return true;
            }
            if (console_is_csi_final(c)) {
                if (ansi_param_active && ansi_param_count == 0)
                    ansi_param_count = 1;
                console_handle_csi(c);
                ansi_state = ANSI_STATE_NORMAL;
                return true;
            }
            ansi_state = ANSI_STATE_NORMAL;
            return true;
    }

    ansi_state = ANSI_STATE_NORMAL;
    return false;
}

bool init_display(uint32_t width, uint32_t height, uint32_t bpp)
{
    const font_t *font;

    KINFO("=== DISPLAY INITIALIZATION (RAM-based) ===\n");

    if (!width || !height || bpp != 32u) {
        KERROR("Unsupported framebuffer geometry %ux%ux%u\n",
               width, height, bpp);
        return false;
    }
    font = display_font_for_geometry(width, height);
    if (!font) {
        KERROR("Framebuffer geometry exceeds console limits: %ux%u\n",
               width, height);
        return false;
    }
    if (framebuffer_base)
        return true;
    
    /* Allouer le framebuffer en RAM */
    uint32_t fb_size = width * height * (bpp / 8u);
    KINFO("Allocating framebuffer: %u bytes (%u KB)\n", 
            fb_size, fb_size / 1024);
    
    /* Allouer des pages contigues pour le framebuffer */
    uint32_t pages_needed = (fb_size + PAGE_SIZE - 1) / PAGE_SIZE;
    KINFO("Pages needed: %u\n", pages_needed);
    
    framebuffer_phys = (paddr_t)allocate_pages(pages_needed);
    if (!framebuffer_phys) {
        KERROR("Failed to allocate framebuffer memory\n");
        return false;
    }
    framebuffer_base = (uint8_t*)phys_to_virt(framebuffer_phys);
    framebuffer_capacity = pages_needed * PAGE_SIZE;
    
    KINFO("Framebuffer allocated at: phys=%p virt=%p\n",
          (void *)(uintptr_t)framebuffer_phys, (void *)framebuffer_base);
    
    display.width = width;
    display.height = height;
    display.bpp = bpp;
    display.pitch = width * (bpp / 8u);
    display.framebuffer = framebuffer_base;
    display.font = font;
    framebuffer_orientation = width > height ?
        ARMOS_FB_ORIENTATION_LANDSCAPE : ARMOS_FB_ORIENTATION_PORTRAIT;
    
    /* Test d'acces au framebuffer */
    KINFO("Testing framebuffer access...\n");
    volatile uint32_t* fb_test = (volatile uint32_t*)framebuffer_base;
    
    /* Test d'ecriture */
    *fb_test = 0x12345678;
    uint32_t read_back = *fb_test;
    
    if (read_back == 0x12345678) {
        KINFO("Framebuffer write/read test PASSED\n");
        
        /* Console mode */
        display.text_cols = display.width / display.font->width;
        display.text_rows = display.height / display.font->height;
        display.cursor_x = 0;
        display.cursor_y = 0;
        cursor_blink_on = true;
        cursor_blink_last_tick = get_system_ticks();
        console_reset_attrs();
        
        clear_screen();
        KINFO("Display initialized: %d x %d (RAM-based)\n", 
                display.width, display.height);
        return true;
        
    } else {
        KERROR("Framebuffer write/read test FAILED\n");
        KERROR("   Written: 0x12345678, Read: 0x%08X\n", read_back);
        
        /* Liberer la memoire en cas d'echec */
        free_pages((void*)framebuffer_phys, pages_needed);
        framebuffer_base = NULL;
        framebuffer_phys = 0;
        framebuffer_capacity = 0;
        return false;
    }
}

bool init_display_external(uint32_t width, uint32_t height, uint32_t bpp,
                           uint32_t pitch, paddr_t physical,
                           uint8_t *virtual_address, uint32_t size)
{
    const font_t *font;
    uint64_t required = (uint64_t)pitch * height;

    if (!width || !height || bpp != 32u || !physical || !virtual_address ||
        pitch != width * 4u || required > size || required > 0xffffffffu) {
        KERROR("Unsupported external framebuffer %ux%ux%u pitch=%u size=%u\n",
               width, height, bpp, pitch, size);
        return false;
    }
    font = display_font_for_geometry(width, height);
    if (!font) {
        KERROR("External framebuffer exceeds console limits: %ux%u\n",
               width, height);
        return false;
    }
    if (framebuffer_base)
        return true;

    framebuffer_phys = physical;
    framebuffer_base = virtual_address;
    framebuffer_capacity = size;
    display.width = width;
    display.height = height;
    display.bpp = bpp;
    display.pitch = pitch;
    display.framebuffer = virtual_address;
    display.font = font;
    display.text_cols = width / display.font->width;
    display.text_rows = height / display.font->height;
    display.cursor_x = 0;
    display.cursor_y = 0;
    framebuffer_orientation = width > height ?
        ARMOS_FB_ORIENTATION_LANDSCAPE : ARMOS_FB_ORIENTATION_PORTRAIT;
    cursor_blink_on = true;
    cursor_blink_last_tick = get_system_ticks();
    console_reset_attrs();
    clear_screen();
    return true;
}


void clear_screen(void)
{
    console_erase_cursor();

    uint32_t* fb32 = (uint32_t*)display.framebuffer;
    uint32_t pixels = display.width * display.height;
    uint32_t i;
    
    if (!framebuffer_user_owned) {
        for (i = 0; i < pixels; i++)
            fb32[i] = display.bg_color;
    }
    
    display.cursor_x = 0;
    display.cursor_y = 0;
    pending_wrap = false;
    console_reset_cells();
    framebuffer_mark_dirty(0, 0, display.width, display.height);
}

void put_pixel(uint32_t x, uint32_t y, uint32_t color)
{
    if (framebuffer_user_owned ||
        x >= display.width || y >= display.height)
        return;
    
    uint32_t* fb32 = (uint32_t*)display.framebuffer;
    fb32[y * display.width + x] = color;
}

static uint32_t blend_argb(uint32_t fg, uint32_t bg, uint8_t alpha)
{
    if (alpha == 0)
        return bg;
    if (alpha == 255)
        return fg;

    uint32_t inv = 255u - alpha;
    uint32_t fr = (fg >> 16) & 0xFF;
    uint32_t fg_g = (fg >> 8) & 0xFF;
    uint32_t fb = fg & 0xFF;
    uint32_t br = (bg >> 16) & 0xFF;
    uint32_t bg_g = (bg >> 8) & 0xFF;
    uint32_t bb = bg & 0xFF;
    uint32_t r = (fr * alpha + br * inv + 127) / 255;
    uint32_t g = (fg_g * alpha + bg_g * inv + 127) / 255;
    uint32_t b = (fb * alpha + bb * inv + 127) / 255;

    return 0xFF000000u | (r << 16) | (g << 8) | b;
}

void draw_char(uint32_t x, uint32_t y, char c, uint32_t fg, uint32_t bg)
{
    const font_t *font = display.font;
    if (!font || !font->glyphs)
        return;

    uint32_t code = (uint8_t)c;
    if (code < font->first || code > font->last)
        code = '?';

    const uint8_t *glyph = font->glyphs +
        (code - font->first) * font->width * font->height;

    for (uint32_t row = 0; row < font->height; row++) {
        for (uint32_t col = 0; col < font->width; col++) {
            uint8_t alpha = glyph[row * font->width + col];
            uint32_t color = blend_argb(fg, bg, alpha);
            put_pixel(x + col, y + row, color);
        }
    }
}

static void console_put_cell(uint32_t col, uint32_t row, char c,
                             uint32_t fg, uint32_t bg)
{
    if (!display.font || col >= display.text_cols || row >= display.text_rows)
        return;

    if (col >= CONSOLE_MAX_COLS || row >= CONSOLE_MAX_ROWS)
        return;

    if (cursor_drawn && cursor_drawn_x == col && cursor_drawn_y == row)
        console_erase_cursor();

    if (console_cells_ready &&
        console_cells[row][col].ch == c &&
        console_cells[row][col].fg == fg &&
        console_cells[row][col].bg == bg)
        return;

    draw_char(col * display.font->width,
              row * display.font->height,
              c, fg, bg);

    console_cells[row][col].ch = c;
    console_cells[row][col].fg = fg;
    console_cells[row][col].bg = bg;
    console_cells_ready = true;
    framebuffer_mark_dirty(col * display.font->width,
                           row * display.font->height,
                           display.font->width,
                           display.font->height);
}

void console_putchar(char c)
{
    unsigned long flags;

    spin_lock_irqsave(&display_console_lock, &flags);
    console_scrollback_reset_view();
    console_erase_cursor();

    if (console_consume_ansi(c)) {
        spin_unlock_irqrestore(&display_console_lock, flags);
        return;
    }

    switch (c) {
        case '\n':
            display.cursor_x = 0;
            pending_wrap = false;
            console_linefeed();
            break;
            
        case '\r':
            display.cursor_x = 0;
            pending_wrap = false;
            break;
            
        case '\b':
        case 0x7F:
            pending_wrap = false;
            if (display.cursor_x > 0) {
                display.cursor_x--;
                console_put_cell(display.cursor_x, display.cursor_y, ' ',
                                 console_fg(), console_bg());
            }
            break;
            
        case '\t':
            do {
                console_printable_char(' ');
                if (pending_wrap)
                    break;
            } while ((display.cursor_x & 7) != 0);
            break;
            
        default:
            if (c >= 32 && c <= 126)
                console_printable_char(c);
            break;
    }
    spin_unlock_irqrestore(&display_console_lock, flags);
}

void console_puts(const char* str)
{
    while (*str) {
        console_putchar(*str++);
    }
}

void scroll_screen(void)
{
    uint32_t font_h = display.font ? display.font->height : 16;
    bool backend_scrolled = false;

    if (!framebuffer_user_owned &&
        display_backend && display_backend->scroll_up) {
        display_backend_mode_t mode;

        memset(&mode, 0, sizeof(mode));
        if (display_backend->scroll_up(font_h, display.bg_color, &mode) == 0 &&
            mode.width == display.width && mode.height == display.height &&
            mode.pitch == display.pitch && mode.bpp == display.bpp &&
            mode.physical && mode.virtual_address &&
            mode.size >= mode.pitch * mode.height) {
            framebuffer_phys = mode.physical;
            framebuffer_base = mode.virtual_address;
            framebuffer_capacity = mode.size;
            display.framebuffer = mode.virtual_address;
            backend_scrolled = true;
        }
    }

    if (!backend_scrolled && !framebuffer_user_owned) {
        uint32_t retained_rows = display.height - font_h;
        uint32_t retained_bytes = retained_rows * display.pitch;
        uint32_t *last_line;

        memmove(display.framebuffer,
                display.framebuffer + font_h * display.pitch,
                retained_bytes);
        last_line = (uint32_t *)(void *)(display.framebuffer + retained_bytes);
        for (uint32_t i = 0; i < display.width * font_h; i++)
            last_line[i] = display.bg_color;
    }
    if (console_cells_ready) {
        uint32_t rows = display.text_rows;
        uint32_t cols = display.text_cols;

        if (rows > CONSOLE_MAX_ROWS)
            rows = CONSOLE_MAX_ROWS;
        if (cols > CONSOLE_MAX_COLS)
            cols = CONSOLE_MAX_COLS;

        if (rows > 0 && cols > 0)
            scrollback_push_line(console_cells[0], cols);

        for (uint32_t row = 1; row < rows; row++)
            memcpy(console_cells[row - 1], console_cells[row],
                   cols * sizeof(console_cell_t));

        if (rows > 0) {
            for (uint32_t col = 0; col < cols; col++) {
                console_cells[rows - 1][col].ch = ' ';
                console_cells[rows - 1][col].fg = display.fg_color;
                console_cells[rows - 1][col].bg = display.bg_color;
            }
        }
    }
    
    display.cursor_y = display.text_rows - 1;
    pending_wrap = false;
    if (backend_scrolled) {
        framebuffer_mark_dirty(0, display.height - font_h,
                               display.width, font_h);
    } else {
        framebuffer_mark_dirty(0, 0, display.width, display.height);
    }
}

ssize_t framebuffer_write(file_t* file, const void* buffer, size_t count)
{
    const char* data = (const char*)buffer;
    size_t i;
    
    /* Suppression du warning unused parameter */
    (void)file;
    
    for (i = 0; i < count; i++) {
        console_putchar(data[i]);
    }

    display_request_flush();

    return count;
}

ssize_t framebuffer_read(file_t* file, void* buffer, size_t count)
{
    /* Read raw framebuffer data */
    uint32_t offset = file->offset;
    uint32_t fb_size = display.width * display.height * 4;
    
    if (offset >= fb_size) {
        return 0;
    }
    
    uint32_t available = fb_size - offset;
    uint32_t to_copy = MIN(count, available);
    
    memcpy(buffer, display.framebuffer + offset, to_copy);
    file->offset += to_copy;
    
    return to_copy;
}

static uint32_t framebuffer_size_bytes(void)
{
    if (!framebuffer_base || display.width == 0 || display.height == 0 ||
        display.pitch == 0 || display.bpp == 0)
        return 0;
    return display.pitch * display.height;
}

static int framebuffer_index(const file_t *file)
{
    return file ? (int)(uintptr_t)file->private_data : 0;
}

int framebuffer_get_info(file_t *file, struct armos_fb_info* info)
{
    if (!info)
        return -EINVAL;
    if (framebuffer_index(file) == 1) {
        spin_lock(&auxiliary_fb.lock);
        if (!auxiliary_fb.registered) {
            spin_unlock(&auxiliary_fb.lock);
            return -ENODEV;
        }
        info->width = auxiliary_fb.width;
        info->height = auxiliary_fb.height;
        info->pitch = auxiliary_fb.pitch;
        info->bpp = auxiliary_fb.bpp;
        info->size = auxiliary_fb.pitch * auxiliary_fb.height;
        info->format = ARMOS_FB_FORMAT_ARGB8888;
        spin_unlock(&auxiliary_fb.lock);
        return 0;
    }
    if (!framebuffer_base)
        return -ENODEV;

    spin_lock(&display_geometry_lock);
    info->width = display.width;
    info->height = display.height;
    info->pitch = display.pitch;
    info->bpp = display.bpp;
    info->size = framebuffer_size_bytes();
    info->format = ARMOS_FB_FORMAT_ARGB8888;
    spin_unlock(&display_geometry_lock);
    return 0;
}

int framebuffer_get_orientation(file_t *file,
                                struct armos_fb_orientation *orientation)
{
    if (!orientation)
        return -EINVAL;
    if (framebuffer_index(file) == 1) {
        spin_lock(&auxiliary_fb.lock);
        if (!auxiliary_fb.registered) {
            spin_unlock(&auxiliary_fb.lock);
            return -ENODEV;
        }
        orientation->orientation = auxiliary_fb.orientation;
        spin_unlock(&auxiliary_fb.lock);
        return 0;
    }
    if (!framebuffer_base)
        return -ENODEV;

    spin_lock(&display_geometry_lock);
    orientation->orientation = framebuffer_orientation;
    spin_unlock(&display_geometry_lock);
    return 0;
}

int framebuffer_set_orientation(file_t *file,
                                const struct armos_fb_orientation *orientation)
{
    uint32_t old_orientation;
    uint32_t old_rows;
    uint32_t old_cols;
    uint32_t width;
    uint32_t height;
    uint64_t required;
    uint32_t *pixels;
    unsigned long console_flags;
    int tty_id;
    int ret;

    if (!orientation)
        return -EINVAL;
    if (orientation->orientation != ARMOS_FB_ORIENTATION_PORTRAIT &&
        orientation->orientation != ARMOS_FB_ORIENTATION_LANDSCAPE)
        return -EINVAL;
    if (framebuffer_index(file) == 1) {
        uint32_t aux_width;
        uint32_t aux_height;

        spin_lock(&auxiliary_fb.lock);
        if (!auxiliary_fb.registered || !auxiliary_fb.backend) {
            spin_unlock(&auxiliary_fb.lock);
            return -ENODEV;
        }
        if (!auxiliary_fb.backend->set_orientation) {
            spin_unlock(&auxiliary_fb.lock);
            return -ENOTSUP;
        }
        if (orientation->orientation == auxiliary_fb.orientation) {
            spin_unlock(&auxiliary_fb.lock);
            return 0;
        }

        aux_width = auxiliary_fb.width;
        aux_height = auxiliary_fb.height;
        ret = auxiliary_fb.backend->set_orientation(
            orientation->orientation, &aux_width, &aux_height);
        if (ret < 0 || !aux_width || !aux_height ||
            (uint64_t)aux_width * aux_height * 4u > auxiliary_fb.size) {
            spin_unlock(&auxiliary_fb.lock);
            return ret < 0 ? ret : -EINVAL;
        }
        auxiliary_fb.width = aux_width;
        auxiliary_fb.height = aux_height;
        auxiliary_fb.pitch = aux_width * 4u;
        auxiliary_fb.orientation = orientation->orientation;
        auxiliary_fb.dirty = true;
        auxiliary_fb.dirty_x0 = 0;
        auxiliary_fb.dirty_y0 = 0;
        auxiliary_fb.dirty_x1 = aux_width;
        auxiliary_fb.dirty_y1 = aux_height;
        spin_unlock(&auxiliary_fb.lock);
        return 0;
    }
    if (!framebuffer_base || !display_backend)
        return -ENODEV;
    if (!display_backend->set_orientation)
        return -ENOTSUP;

    spin_lock_irqsave(&display_console_lock, &console_flags);
    spin_lock(&display_geometry_lock);
    if (orientation->orientation == framebuffer_orientation) {
        spin_unlock(&display_geometry_lock);
        spin_unlock_irqrestore(&display_console_lock, console_flags);
        return 0;
    }

    old_orientation = framebuffer_orientation;
    old_rows = display.text_rows;
    old_cols = display.text_cols;
    width = display.width;
    height = display.height;

    ret = display_backend->set_orientation(orientation->orientation,
                                           &width, &height);
    if (ret < 0) {
        spin_unlock(&display_geometry_lock);
        spin_unlock_irqrestore(&display_console_lock, console_flags);
        return ret;
    }

    required = (uint64_t)width * (uint64_t)height *
               (uint64_t)(display.bpp / 8u);
    if (!width || !height || display.bpp != 32u ||
        required > (uint64_t)framebuffer_capacity) {
        uint32_t rollback_width = display.width;
        uint32_t rollback_height = display.height;

        display_backend->set_orientation(old_orientation,
                                         &rollback_width, &rollback_height);
        spin_unlock(&display_geometry_lock);
        spin_unlock_irqrestore(&display_console_lock, console_flags);
        return -EINVAL;
    }

    display.width = width;
    display.height = height;
    display.pitch = width * (display.bpp / 8u);
    display.text_cols = width / display.font->width;
    display.text_rows = height / display.font->height;
    framebuffer_orientation = orientation->orientation;
    cursor_drawn = false;
    pending_wrap = false;

    if (display.cursor_x >= display.text_cols)
        display.cursor_x = display.text_cols - 1u;
    if (display.cursor_y >= display.text_rows)
        display.cursor_y = display.text_rows - 1u;

    console_resize_cells(old_rows, old_cols,
                         display.text_rows, display.text_cols);
    pixels = (uint32_t *)(void *)framebuffer_base;
    for (uint32_t i = 0; i < width * height; i++)
        pixels[i] = display.bg_color;

    console_render_scrollback();
    tty_id = framebuffer_tty_id;
    spin_unlock(&display_geometry_lock);
    spin_unlock_irqrestore(&display_console_lock, console_flags);

    if (tty_id >= 0) {
        tty_set_winsize_for_id(tty_id,
                               (uint16_t)display.text_rows,
                               (uint16_t)display.text_cols,
                               (uint16_t)display.width,
                               (uint16_t)display.height);
    }
    display_request_flush();
    return 0;
}

int framebuffer_set_mode(file_t *file, const struct armos_fb_mode *mode)
{
    display_backend_mode_t backend_mode;
    const font_t *font;
    uint32_t old_rows;
    uint32_t old_cols;
    uint32_t new_rows;
    uint32_t new_cols;
    uint32_t *pixels;
    uint64_t required;
    unsigned long console_flags;
    unsigned long dirty_flags;
    int tty_id;
    int ret;

    if (!mode || !mode->width || !mode->height)
        return -EINVAL;
    if (framebuffer_index(file) == 1)
        return -ENOTSUP;
    if (!framebuffer_base || !display_backend)
        return -ENODEV;
    if (!display_backend->set_mode)
        return -ENOTSUP;

    font = display_font_for_geometry(mode->width, mode->height);
    if (!font)
        return -EINVAL;

    spin_lock_irqsave(&display_console_lock, &console_flags);
    spin_lock(&display_geometry_lock);

    ret = display_backend->set_mode(mode->width, mode->height,
                                    &backend_mode);
    if (ret < 0) {
        spin_unlock(&display_geometry_lock);
        spin_unlock_irqrestore(&display_console_lock, console_flags);
        return ret;
    }

    required = (uint64_t)backend_mode.pitch * backend_mode.height;
    if (backend_mode.width != mode->width ||
        backend_mode.height != mode->height ||
        backend_mode.bpp != 32u ||
        backend_mode.pitch != backend_mode.width * 4u ||
        !backend_mode.physical || !backend_mode.virtual_address ||
        !backend_mode.size || required > backend_mode.size ||
        required > 0xffffffffu) {
        spin_unlock(&display_geometry_lock);
        spin_unlock_irqrestore(&display_console_lock, console_flags);
        return -EIO;
    }

    old_rows = display.text_rows;
    old_cols = display.text_cols;
    new_cols = backend_mode.width / font->width;
    new_rows = backend_mode.height / font->height;

    framebuffer_phys = backend_mode.physical;
    framebuffer_base = backend_mode.virtual_address;
    framebuffer_capacity = backend_mode.size;
    display.width = backend_mode.width;
    display.height = backend_mode.height;
    display.pitch = backend_mode.pitch;
    display.bpp = backend_mode.bpp;
    display.framebuffer = backend_mode.virtual_address;
    display.font = font;
    display.text_cols = new_cols;
    display.text_rows = new_rows;
    framebuffer_orientation = display.width > display.height ?
        ARMOS_FB_ORIENTATION_LANDSCAPE : ARMOS_FB_ORIENTATION_PORTRAIT;
    cursor_drawn = false;
    pending_wrap = false;

    if (display.cursor_x >= display.text_cols)
        display.cursor_x = display.text_cols - 1u;
    if (display.cursor_y >= display.text_rows)
        display.cursor_y = display.text_rows - 1u;

    console_resize_cells(old_rows, old_cols, new_rows, new_cols);

    spin_lock_irqsave(&dirty_lock, &dirty_flags);
    framebuffer_dirty = false;
    spin_unlock_irqrestore(&dirty_lock, dirty_flags);

    pixels = (uint32_t *)(void *)framebuffer_base;
    for (uint32_t i = 0; i < display.width * display.height; i++)
        pixels[i] = display.bg_color;

    console_render_scrollback();
    tty_id = framebuffer_tty_id;
    new_rows = display.text_rows;
    new_cols = display.text_cols;
    spin_unlock(&display_geometry_lock);
    spin_unlock_irqrestore(&display_console_lock, console_flags);

    if (tty_id >= 0) {
        tty_set_winsize_for_id(tty_id,
                               (uint16_t)new_rows,
                               (uint16_t)new_cols,
                               (uint16_t)mode->width,
                               (uint16_t)mode->height);
    }
    display_request_flush();
    return 0;
}

static ssize_t fb0_read(file_t* file, void* buffer, size_t count)
{
    uint32_t fb_size;
    uint32_t offset;
    uint32_t available;
    uint32_t to_copy;

    if (!file || !buffer)
        return -EINVAL;

    if (framebuffer_index(file) == 1) {
        spin_lock(&auxiliary_fb.lock);
        if (!auxiliary_fb.registered) {
            spin_unlock(&auxiliary_fb.lock);
            return -ENODEV;
        }
        fb_size = auxiliary_fb.pitch * auxiliary_fb.height;
        offset = file->offset;
        if (offset >= fb_size) {
            spin_unlock(&auxiliary_fb.lock);
            return 0;
        }
        available = fb_size - offset;
        to_copy = MIN(count, available);
        memcpy(buffer, auxiliary_fb.framebuffer + offset, to_copy);
        file->offset += to_copy;
        spin_unlock(&auxiliary_fb.lock);
        return (ssize_t)to_copy;
    }
    if (!framebuffer_base)
        return -ENODEV;

    spin_lock(&display_geometry_lock);
    fb_size = framebuffer_size_bytes();
    offset = file->offset;
    if (offset >= fb_size) {
        spin_unlock(&display_geometry_lock);
        return 0;
    }

    available = fb_size - offset;
    to_copy = MIN(count, available);
    memcpy(buffer, framebuffer_base + offset, to_copy);
    file->offset += to_copy;
    spin_unlock(&display_geometry_lock);
    return (ssize_t)to_copy;
}

static ssize_t fb0_write(file_t* file, const void* buffer, size_t count)
{
    uint32_t fb_size;
    uint32_t offset;
    uint32_t available;
    uint32_t to_copy;
    uint32_t dirty_offset;

    if (!file || !buffer)
        return -EINVAL;

    if (framebuffer_index(file) == 1) {
        spin_lock(&auxiliary_fb.lock);
        if (!auxiliary_fb.registered) {
            spin_unlock(&auxiliary_fb.lock);
            return -ENODEV;
        }
        fb_size = auxiliary_fb.pitch * auxiliary_fb.height;
        offset = file->offset;
        if (offset >= fb_size) {
            spin_unlock(&auxiliary_fb.lock);
            return 0;
        }
        available = fb_size - offset;
        to_copy = MIN(count, available);
        memcpy(auxiliary_fb.framebuffer + offset, buffer, to_copy);
        file->offset += to_copy;
        spin_unlock(&auxiliary_fb.lock);
        display_mark_auxiliary_dirty(0, 0, auxiliary_fb.width,
                                     auxiliary_fb.height);
        return (ssize_t)to_copy;
    }
    if (!framebuffer_base)
        return -ENODEV;

    spin_lock(&framebuffer_owner_lock);
    if (framebuffer_owner && framebuffer_owner != file) {
        spin_unlock(&framebuffer_owner_lock);
        return -EBUSY;
    }
    spin_unlock(&framebuffer_owner_lock);

    spin_lock(&display_geometry_lock);
    fb_size = framebuffer_size_bytes();
    offset = file->offset;
    if (offset >= fb_size) {
        spin_unlock(&display_geometry_lock);
        return 0;
    }

    available = fb_size - offset;
    to_copy = MIN(count, available);
    dirty_offset = offset;
    memcpy(framebuffer_base + offset, buffer, to_copy);
    file->offset += to_copy;

    if (to_copy != 0u) {
        uint32_t first_row = dirty_offset / display.pitch;
        uint32_t last_offset = dirty_offset + to_copy - 1u;
        uint32_t last_row = last_offset / display.pitch;

        if (first_row == last_row) {
            uint32_t first_x =
                (dirty_offset % display.pitch) / sizeof(uint32_t);
            uint32_t last_x =
                (last_offset % display.pitch) / sizeof(uint32_t);

            if (first_x < display.width) {
                if (last_x >= display.width)
                    last_x = display.width - 1u;
                framebuffer_mark_dirty(first_x, first_row,
                                       last_x - first_x + 1u, 1u);
            }
        } else {
            framebuffer_mark_dirty(0u, first_row, display.width,
                                   last_row - first_row + 1u);
        }
    }
    spin_unlock(&display_geometry_lock);
    display_request_flush();
    return (ssize_t)to_copy;
}

static off_t fb0_lseek(file_t* file, off_t offset, int whence)
{
    off_t base;
    off_t next;
    uint32_t fb_size;

    if (!file)
        return -EINVAL;

    if (framebuffer_index(file) == 1) {
        spin_lock(&auxiliary_fb.lock);
        if (!auxiliary_fb.registered) {
            spin_unlock(&auxiliary_fb.lock);
            return -ENODEV;
        }
        fb_size = auxiliary_fb.pitch * auxiliary_fb.height;
        spin_unlock(&auxiliary_fb.lock);
    } else {
        spin_lock(&display_geometry_lock);
        fb_size = framebuffer_size_bytes();
        spin_unlock(&display_geometry_lock);
    }

    switch (whence) {
    case SEEK_SET:
        base = 0;
        break;
    case SEEK_CUR:
        base = (off_t)file->offset;
        break;
    case SEEK_END:
        base = (off_t)fb_size;
        break;
    default:
        return -EINVAL;
    }

    next = base + offset;
    if (next < 0 || next > (off_t)fb_size) {
        return -EINVAL;
    }

    file->offset = (uint32_t)next;
    return next;
}

int framebuffer_acquire(file_t *file)
{
    if (!file || framebuffer_index(file) != 0)
        return -ENODEV;

    spin_lock(&framebuffer_owner_lock);
    if (framebuffer_owner && framebuffer_owner != file) {
        spin_unlock(&framebuffer_owner_lock);
        return -EBUSY;
    }
    framebuffer_owner = file;
    framebuffer_user_owned = true;
    spin_unlock(&framebuffer_owner_lock);
    return 0;
}

int framebuffer_release(file_t *file)
{
    unsigned long flags;

    if (!file || framebuffer_index(file) != 0)
        return -ENODEV;
    spin_lock(&framebuffer_owner_lock);
    if (framebuffer_owner != file) {
        spin_unlock(&framebuffer_owner_lock);
        return -EPERM;
    }
    framebuffer_owner = NULL;
    framebuffer_user_owned = false;
    spin_unlock(&framebuffer_owner_lock);

    spin_lock_irqsave(&display_console_lock, &flags);
    console_render_scrollback();
    spin_unlock_irqrestore(&display_console_lock, flags);
    display_flush_all();
    return 0;
}

static int fb0_close(file_t *file)
{
    spin_lock(&framebuffer_owner_lock);
    if (framebuffer_owner != file) {
        spin_unlock(&framebuffer_owner_lock);
        return 0;
    }
    spin_unlock(&framebuffer_owner_lock);
    return framebuffer_release(file);
}

static file_operations_t fb0_file_ops = {
    .read = fb0_read,
    .write = fb0_write,
    .open = NULL,
    .close = fb0_close,
    .lseek = fb0_lseek,
    .readdir = NULL,
    .truncate = NULL,
};

bool is_framebuffer_device_path(const char* path)
{
    return path && (strcmp(path, "/dev/fb0") == 0 ||
                    strcmp(path, "/dev/fb1") == 0);
}

void fill_framebuffer_device_stat(const char *path, struct stat* st)
{
    uint32_t now;
    uint32_t fb_size;
    uint32_t pitch;
    uint32_t rdev;

    if (!st)
        return;

    if (path && strcmp(path, "/dev/fb1") == 0) {
        spin_lock(&auxiliary_fb.lock);
        fb_size = auxiliary_fb.registered ?
            auxiliary_fb.pitch * auxiliary_fb.height : 0;
        pitch = auxiliary_fb.registered ? auxiliary_fb.pitch : 0;
        spin_unlock(&auxiliary_fb.lock);
        rdev = DEV_FB1_RDEV;
    } else {
        fb_size = framebuffer_size_bytes();
        pitch = display.pitch;
        rdev = DEV_FB0_RDEV;
    }

    now = get_current_time();
    memset(st, 0, sizeof(*st));
    st->st_dev = 0;
    st->st_ino = rdev;
    st->st_mode = S_IFCHR | 0666;
    st->st_nlink = 1;
    st->st_uid = 0;
    st->st_gid = 0;
    st->st_rdev = rdev;
    st->st_size = fb_size ? (off_t)fb_size : (off_t)FB_SIZE;
    st->st_blksize = pitch ? pitch : FB_WIDTH * (FB_BPP / 8);
    st->st_blocks = (st->st_size + 511) / 512;
    st->st_atime = now;
    st->st_mtime = now;
    st->st_ctime = now;
}

file_t* create_framebuffer_device_file(const char* name, int flags)
{
    file_t* file;
    inode_t* inode;
    struct stat st;

    int framebuffer_id = name && strcmp(name, "fb1") == 0 ? 1 : 0;

    if (framebuffer_id == 0 && !framebuffer_base)
        return NULL;
    if (framebuffer_id == 1 && !auxiliary_fb.registered)
        return NULL;

    file = create_file();
    if (!file)
        return NULL;

    inode = create_inode();
    if (!inode) {
        kfree(file);
        return NULL;
    }

    fill_framebuffer_device_stat(framebuffer_id == 1 ? "/dev/fb1" : "/dev/fb0",
                                 &st);
    inode->mode = st.st_mode;
    inode->uid = st.st_uid;
    inode->gid = st.st_gid;
    inode->size = st.st_size;
    inode->blocks = st.st_blocks;
    inode->nlink = st.st_nlink;
    inode->first_cluster = 0;
    inode->parent_cluster = st.st_rdev;
    inode->atime = st.st_atime;
    inode->mtime = st.st_mtime;
    inode->ctime = st.st_ctime;
    inode->i_op = NULL;
    inode->f_op = &fb0_file_ops;

    file->f_op = &fb0_file_ops;
    file->flags = flags;
    file->type = FILE_TYPE_FRAMEBUFFER;
    file->pos = 0;
    file->offset = 0;
    file->inode = inode;
    file->private_data = (void *)(uintptr_t)framebuffer_id;

    if (name) {
        strncpy(file->name, name, sizeof(file->name) - 1);
        file->name[sizeof(file->name) - 1] = '\0';
    }

    return file;
}

static void framebuffer_tty_putc(char c)
{
    if (!framebuffer_base)
        return;

    console_putchar(c);
    display_request_flush();
}

static bool framebuffer_tty_try_putc(char c)
{
    if (!framebuffer_base)
        return false;

    console_putchar(c);
    return true;
}

static void framebuffer_tty_puts(const char *s)
{
    if (!framebuffer_base || !s)
        return;

    while (*s)
        console_putchar(*s++);
    display_request_flush();
}

static void framebuffer_tty_set_tx_irq_enabled(bool enabled)
{
    /*
     * The UART backend uses this hook to toggle its TX interrupt; the
     * framebuffer used it to flush at end-of-drain, which submitted GPU
     * commands from the timer IRQ. displayd now owns frame flushing, so
     * the TTY drain must not touch the GPU at all. Early boot output is
     * still flushed synchronously through display_request_flush().
     */
    (void)enabled;
    display_request_flush();
}

static bool framebuffer_tty_has_data(void)
{
    return false;
}

static int framebuffer_tty_getc(void)
{
    return -1;
}

static const tty_backend_ops_t framebuffer_tty_backend = {
    .putc = framebuffer_tty_putc,
    .try_putc = framebuffer_tty_try_putc,
    .puts = framebuffer_tty_puts,
    .set_tx_irq_enabled = framebuffer_tty_set_tx_irq_enabled,
    .has_data = framebuffer_tty_has_data,
    .getc = framebuffer_tty_getc,
};

int framebuffer_attach_tty_backend(int tty_id)
{
    if (!framebuffer_base)
        return -ENODEV;

    int ret = tty_attach_backend_to(tty_id, &framebuffer_tty_backend);
    if (ret == 0) {
        framebuffer_tty_id = tty_id;
        tty_set_winsize_for_id(tty_id,
                               (uint16_t)display.text_rows,
                               (uint16_t)display.text_cols,
                               (uint16_t)display.width,
                               (uint16_t)display.height);
    }
    return ret;
}
