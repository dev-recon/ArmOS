/*
 * ArmOS
 * Copyright (c) 2026 Mohamed Ennassiri
 *
 * Licensed under the Apache License, Version 2.0.
 * See LICENSE for details.
 *
 * File: userland/include/armui/armui.h
 * Layer: Userland / public graphical API
 *
 * Responsibilities:
 * - Expose the small, engine-independent ArmUI C interface.
 * - Keep Nuklear and renderer structures private to libarmui.
 * - Describe portable input, widgets and XRGB8888 presentation.
 */

#ifndef ARMOS_ARMUI_H
#define ARMOS_ARMUI_H

#include <stddef.h>
#include <stdint.h>

struct armui_context;
struct armui_application;

enum armui_align {
    ARMUI_ALIGN_LEFT,
    ARMUI_ALIGN_CENTER,
    ARMUI_ALIGN_RIGHT
};

enum armui_button {
    ARMUI_BUTTON_LEFT,
    ARMUI_BUTTON_RIGHT
};

enum armui_key {
    ARMUI_KEY_ENTER,
    ARMUI_KEY_BACKSPACE,
    ARMUI_KEY_UP,
    ARMUI_KEY_DOWN,
    ARMUI_KEY_LEFT,
    ARMUI_KEY_RIGHT
};

typedef void (*armui_application_key_fn)(
    struct armui_application *application,
    enum armui_key key,
    int pressed,
    void *data);

struct armui_target {
    uint32_t *pixels;
    int width;
    int height;
    int stride;
};

enum armui_frame_result {
    ARMUI_FRAME_IDLE = 0u,
    ARMUI_FRAME_CONTINUE = 1u << 0,
    ARMUI_FRAME_CLOSE = 1u << 1,
    ARMUI_FRAME_ERROR = 1u << 2
};

enum armui_application_role {
    ARMUI_APPLICATION_WINDOW = 0,
    ARMUI_APPLICATION_SYSTEM_BAR
};

struct armui_application_config {
    const char *title;
    const char *app_id;
    int width;
    int height;
    int minimum_width;
    int minimum_height;
    int clear_target;
    uint32_t background;
    armui_application_key_fn key;
    enum armui_application_role role;
};

typedef unsigned int (*armui_application_frame_fn)(
    struct armui_application *application,
    struct armui_context *context,
    struct armui_target *target,
    uint32_t time_ms,
    void *data);

struct armui_context *armui_create(void);
void armui_destroy(struct armui_context *context);

void armui_input_begin(struct armui_context *context);
void armui_input_end(struct armui_context *context);
void armui_input_motion(struct armui_context *context, int x, int y);
void armui_input_button(struct armui_context *context,
                        enum armui_button button, int x, int y, int pressed);
void armui_input_scroll(struct armui_context *context, float x, float y);
void armui_input_key(struct armui_context *context,
                     enum armui_key key, int pressed);
void armui_input_unicode(struct armui_context *context, uint32_t codepoint);

int armui_window_begin(struct armui_context *context, const char *title,
                       float x, float y, float width, float height,
                       int scrolling);
void armui_window_end(struct armui_context *context);
int armui_panel_begin(struct armui_context *context, const char *identifier,
                      float x, float y, float width, float height,
                      int scrolling, int translucent);
void armui_panel_end(struct armui_context *context);
void armui_row(struct armui_context *context, float height, int columns);
void armui_row_static(struct armui_context *context, float height,
                      float item_width, int columns);
void armui_label(struct armui_context *context, const char *text,
                 enum armui_align alignment);
void armui_label_wrap(struct armui_context *context, const char *text);
void armui_label_int(struct armui_context *context, const char *label,
                     int value);
int armui_edit_string(struct armui_context *context, char *text,
                      size_t capacity, size_t *length);
int armui_button_label(struct armui_context *context, const char *label);
int armui_option(struct armui_context *context, const char *label, int active);
int armui_checkbox(struct armui_context *context,
                   const char *label, int active);
int armui_property_float(struct armui_context *context, const char *label,
                         float minimum, float *value, float maximum,
                         float step, float pixels_per_step);
void armui_progress(struct armui_context *context,
                    size_t value, size_t maximum);
void armui_set_enabled(struct armui_context *context, int enabled);
void armui_set_contrast(struct armui_context *context, int contrast);
void armui_set_repeater(struct armui_context *context, int enabled);
void armui_menubar_begin(struct armui_context *context);
void armui_menubar_end(struct armui_context *context);
int armui_menu_begin(struct armui_context *context, const char *label,
                     float width, float height);
int armui_menu_item(struct armui_context *context, const char *label);
void armui_menu_end(struct armui_context *context);

void armui_render(struct armui_context *context,
                  const struct armui_target *target,
                  int clear_target, uint32_t background);

int armui_application_run(
    const struct armui_application_config *config,
    armui_application_frame_fn frame,
    void *data);
void armui_application_request_redraw(
    struct armui_application *application);
void armui_application_close(struct armui_application *application);
void armui_application_set_background(
    struct armui_application *application,
    int clear_target, uint32_t background);
int armui_application_pointer_down(
    const struct armui_application *application);
int armui_application_repeat_pulse(
    const struct armui_application *application);

#endif
