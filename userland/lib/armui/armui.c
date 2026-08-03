/*
 * ArmOS
 * Copyright (c) 2026 Mohamed Ennassiri
 *
 * Licensed under the Apache License, Version 2.0.
 * See LICENSE for details.
 *
 * File: userland/lib/armui/armui.c
 * Layer: Userland / graphical libraries
 *
 * Responsibilities:
 * - Implement the opaque ArmUI API over the private Nuklear engine.
 * - Own font-atlas lifetime and translate portable ArmUI input.
 * - Feed the common software renderer without exposing Nuklear publicly.
 */

#include <stdlib.h>
#include <string.h>

#include <armos_nuklear_config.h>
#include <nuklear.h>

#include <armui/armui.h>

#include "nuklear_software.h"

struct armui_context {
    struct nk_context ui;
    struct nk_font_atlas atlas;
    struct nk_font *font;
    unsigned char *font_pixels;
    int font_width;
    int font_height;
    int disabled;
    int repeater;
    int translucent_panel;
    struct nk_style_item saved_window_background;
};

static nk_flags text_alignment(enum armui_align alignment)
{
    if (alignment == ARMUI_ALIGN_CENTER)
        return NK_TEXT_CENTERED;
    if (alignment == ARMUI_ALIGN_RIGHT)
        return NK_TEXT_RIGHT;
    return NK_TEXT_LEFT;
}

struct armui_context *armui_create(void)
{
    static const nk_rune glyph_ranges[] = {
        0x0020u, 0x00ffu,
        0x20acu, 0x20acu,
        0u
    };
    struct armui_context *context = calloc(1u, sizeof(*context));
    struct nk_font_config font_config = nk_font_config(16.0f);
    const void *font_pixels;

    if (!context)
        return NULL;
    nk_font_atlas_init_default(&context->atlas);
    nk_font_atlas_begin(&context->atlas);
    font_config.range = glyph_ranges;
    /* Small UI text is clearer when glyph origins stay on whole pixels. */
    font_config.pixel_snap = 1;
    font_config.oversample_h = 1;
    font_config.oversample_v = 1;
    context->font = nk_font_atlas_add_from_file(
        &context->atlas,
        "/usr/share/fonts/armos/MesloLGS-NF-Regular.ttf",
        16.0f, &font_config);
    if (!context->font)
        context->font = nk_font_atlas_add_default(
            &context->atlas, 16.0f, &font_config);
    font_pixels = nk_font_atlas_bake(&context->atlas,
                                     &context->font_width,
                                     &context->font_height,
                                     NK_FONT_ATLAS_ALPHA8);
    if (!context->font || !font_pixels)
        goto failed;
    context->font_pixels = malloc(
        (size_t)context->font_width * (size_t)context->font_height);
    if (!context->font_pixels)
        goto failed;
    memcpy(context->font_pixels, font_pixels,
           (size_t)context->font_width * (size_t)context->font_height);
    nk_font_atlas_end(&context->atlas, nk_handle_id(1), NULL);
    if (!nk_init_default(&context->ui, &context->font->handle))
        goto failed;
    /* Do not inherit Nuklear's low-contrast gray default theme. */
    armui_set_contrast(context, 0);
    return context;

failed:
    free(context->font_pixels);
    nk_font_atlas_clear(&context->atlas);
    free(context);
    return NULL;
}

void armui_destroy(struct armui_context *context)
{
    if (!context)
        return;
    nk_free(&context->ui);
    nk_font_atlas_clear(&context->atlas);
    free(context->font_pixels);
    free(context);
}

void armui_input_begin(struct armui_context *context)
{
    if (context)
        nk_input_begin(&context->ui);
}

void armui_input_end(struct armui_context *context)
{
    if (context)
        nk_input_end(&context->ui);
}

void armui_input_motion(struct armui_context *context, int x, int y)
{
    if (context)
        nk_input_motion(&context->ui, x, y);
}

void armui_input_button(struct armui_context *context,
                        enum armui_button button, int x, int y, int pressed)
{
    if (context)
        nk_input_button(&context->ui,
                        button == ARMUI_BUTTON_RIGHT ?
                        NK_BUTTON_RIGHT : NK_BUTTON_LEFT,
                        x, y, pressed != 0);
}

void armui_input_scroll(struct armui_context *context, float x, float y)
{
    if (context)
        nk_input_scroll(&context->ui, nk_vec2(x, y));
}

void armui_input_key(struct armui_context *context,
                     enum armui_key key, int pressed)
{
    static const enum nk_keys keys[] = {
        NK_KEY_ENTER, NK_KEY_BACKSPACE, NK_KEY_UP,
        NK_KEY_DOWN, NK_KEY_LEFT, NK_KEY_RIGHT
    };

    if (context && (unsigned int)key < sizeof(keys) / sizeof(keys[0]))
        nk_input_key(&context->ui, keys[key], pressed != 0);
}

void armui_input_unicode(struct armui_context *context, uint32_t codepoint)
{
    if (context && codepoint != 0u)
        nk_input_unicode(&context->ui, codepoint);
}

int armui_window_begin(struct armui_context *context, const char *title,
                       float x, float y, float width, float height,
                       int scrolling)
{
    nk_flags flags = NK_WINDOW_BORDER | NK_WINDOW_TITLE;

    if (!context)
        return 0;
    if (!scrolling)
        flags |= NK_WINDOW_NO_SCROLLBAR;
    return nk_begin(&context->ui, title, nk_rect(x, y, width, height), flags);
}

void armui_window_end(struct armui_context *context)
{
    if (context)
        nk_end(&context->ui);
}

int armui_panel_begin(struct armui_context *context, const char *identifier,
                      float x, float y, float width, float height,
                      int scrolling, int translucent)
{
    nk_flags flags = 0u;

    if (!context || context->translucent_panel)
        return 0;
    if (!scrolling)
        flags |= NK_WINDOW_NO_SCROLLBAR;
    if (translucent) {
        context->saved_window_background =
            context->ui.style.window.fixed_background;
        context->ui.style.window.fixed_background =
            nk_style_item_color(nk_rgba(27, 32, 39, 208));
        context->translucent_panel = 1;
    }
    return nk_begin(&context->ui, identifier,
                    nk_rect(x, y, width, height), flags);
}

void armui_panel_end(struct armui_context *context)
{
    if (!context)
        return;
    nk_end(&context->ui);
    if (context->translucent_panel) {
        context->ui.style.window.fixed_background =
            context->saved_window_background;
        context->translucent_panel = 0;
    }
}

void armui_row(struct armui_context *context, float height, int columns)
{
    if (context)
        nk_layout_row_dynamic(&context->ui, height, columns);
}

void armui_row_static(struct armui_context *context, float height,
                      float item_width, int columns)
{
    if (context)
        nk_layout_row_static(&context->ui, height,
                             (int)item_width, columns);
}

void armui_label(struct armui_context *context, const char *text,
                 enum armui_align alignment)
{
    if (context)
        nk_label(&context->ui, text, text_alignment(alignment));
}

void armui_label_wrap(struct armui_context *context, const char *text)
{
    if (context)
        nk_label_wrap(&context->ui, text);
}

void armui_label_int(struct armui_context *context, const char *label,
                     int value)
{
    if (context)
        nk_labelf(&context->ui, NK_TEXT_CENTERED, "%s: %d", label, value);
}

int armui_edit_string(struct armui_context *context, char *text,
                      size_t capacity, size_t *length)
{
    int current;
    int result;

    if (!context || !text || !length || capacity < 2u)
        return 0;
    current = (int)*length;
    if (current < 0 || (size_t)current >= capacity)
        current = (int)strnlen(text, capacity - 1u);
    result = nk_edit_string(&context->ui, NK_EDIT_FIELD, text, &current,
                            (int)capacity - 1, nk_filter_default);
    text[current] = '\0';
    *length = (size_t)current;
    return result;
}

int armui_button_label(struct armui_context *context, const char *label)
{
    return context ? nk_button_label(&context->ui, label) : 0;
}

int armui_option(struct armui_context *context, const char *label, int active)
{
    return context ? nk_option_label(&context->ui, label, active != 0) : 0;
}

int armui_checkbox(struct armui_context *context,
                   const char *label, int active)
{
    return context ? nk_check_label(&context->ui, label, active != 0) : 0;
}

int armui_property_float(struct armui_context *context, const char *label,
                         float minimum, float *value, float maximum,
                         float step, float pixels_per_step)
{
    return context ? nk_property_float(&context->ui, label, minimum, value,
                                       maximum, step, pixels_per_step) : 0;
}

void armui_progress(struct armui_context *context,
                    size_t value, size_t maximum)
{
    nk_size current = value;

    if (context)
        (void)nk_progress(&context->ui, &current, maximum, nk_false);
}

void armui_set_enabled(struct armui_context *context, int enabled)
{
    if (!context || context->disabled == !enabled)
        return;
    if (enabled)
        nk_widget_disable_end(&context->ui);
    else
        nk_widget_disable_begin(&context->ui);
    context->disabled = !enabled;
}

void armui_set_contrast(struct armui_context *context, int contrast)
{
    struct nk_color colors[NK_COLOR_COUNT];

    if (!context)
        return;
    colors[NK_COLOR_TEXT] = contrast ?
        nk_rgb(239, 242, 246) : nk_rgb(37, 42, 49);
    colors[NK_COLOR_WINDOW] = contrast ?
        nk_rgb(31, 36, 44) : nk_rgb(244, 246, 249);
    colors[NK_COLOR_HEADER] = contrast ?
        nk_rgb(31, 111, 151) : nk_rgb(89, 189, 235);
    colors[NK_COLOR_BORDER] = contrast ?
        nk_rgb(100, 112, 126) : nk_rgb(171, 178, 188);
    colors[NK_COLOR_BUTTON] = contrast ?
        nk_rgb(54, 62, 73) : nk_rgb(224, 228, 234);
    colors[NK_COLOR_BUTTON_HOVER] = contrast ?
        nk_rgb(65, 91, 108) : nk_rgb(203, 226, 241);
    colors[NK_COLOR_BUTTON_ACTIVE] =
        nk_rgb(45, 166, 221);
    colors[NK_COLOR_TOGGLE] = contrast ?
        nk_rgb(63, 72, 84) : nk_rgb(207, 212, 219);
    colors[NK_COLOR_TOGGLE_HOVER] = contrast ?
        nk_rgb(73, 102, 121) : nk_rgb(189, 216, 234);
    colors[NK_COLOR_TOGGLE_CURSOR] = nk_rgb(45, 166, 221);
    colors[NK_COLOR_SELECT] = contrast ?
        nk_rgb(63, 72, 84) : nk_rgb(207, 212, 219);
    colors[NK_COLOR_SELECT_ACTIVE] = nk_rgb(89, 189, 235);
    colors[NK_COLOR_SLIDER] = contrast ?
        nk_rgb(54, 62, 73) : nk_rgb(207, 212, 219);
    colors[NK_COLOR_SLIDER_CURSOR] = nk_rgb(89, 189, 235);
    colors[NK_COLOR_SLIDER_CURSOR_HOVER] = nk_rgb(45, 166, 221);
    colors[NK_COLOR_SLIDER_CURSOR_ACTIVE] = nk_rgb(30, 132, 184);
    colors[NK_COLOR_PROPERTY] = contrast ?
        nk_rgb(54, 62, 73) : nk_rgb(224, 228, 234);
    colors[NK_COLOR_EDIT] = contrast ?
        nk_rgb(22, 26, 32) : nk_rgb(255, 255, 255);
    colors[NK_COLOR_EDIT_CURSOR] = contrast ?
        nk_rgb(239, 242, 246) : nk_rgb(37, 42, 49);
    colors[NK_COLOR_COMBO] = contrast ?
        nk_rgb(54, 62, 73) : nk_rgb(224, 228, 234);
    colors[NK_COLOR_CHART] = contrast ?
        nk_rgb(54, 62, 73) : nk_rgb(224, 228, 234);
    colors[NK_COLOR_CHART_COLOR] = nk_rgb(89, 189, 235);
    colors[NK_COLOR_CHART_COLOR_HIGHLIGHT] = nk_rgb(255, 95, 86);
    colors[NK_COLOR_SCROLLBAR] = contrast ?
        nk_rgb(42, 48, 57) : nk_rgb(224, 228, 234);
    colors[NK_COLOR_SCROLLBAR_CURSOR] = contrast ?
        nk_rgb(91, 103, 117) : nk_rgb(171, 178, 188);
    colors[NK_COLOR_SCROLLBAR_CURSOR_HOVER] = contrast ?
        nk_rgb(118, 132, 149) : nk_rgb(139, 149, 162);
    colors[NK_COLOR_SCROLLBAR_CURSOR_ACTIVE] = nk_rgb(89, 189, 235);
    colors[NK_COLOR_TAB_HEADER] = contrast ?
        nk_rgb(54, 62, 73) : nk_rgb(224, 228, 234);
    nk_style_from_table(&context->ui, colors);
}

void armui_set_repeater(struct armui_context *context, int enabled)
{
    if (!context || context->repeater == (enabled != 0))
        return;
    if (enabled)
        (void)nk_button_push_behavior(&context->ui, NK_BUTTON_REPEATER);
    else
        (void)nk_button_pop_behavior(&context->ui);
    context->repeater = enabled != 0;
}

void armui_menubar_begin(struct armui_context *context)
{
    if (context)
        nk_menubar_begin(&context->ui);
}

void armui_menubar_end(struct armui_context *context)
{
    if (context)
        nk_menubar_end(&context->ui);
}

int armui_menu_begin(struct armui_context *context, const char *label,
                     float width, float height)
{
    int opened;

    if (!context)
        return 0;
    opened = nk_menu_begin_label(&context->ui, label, NK_TEXT_LEFT,
                                 nk_vec2(width, height));
    if (opened)
        nk_layout_row_dynamic(&context->ui, 28.0f, 1);
    return opened;
}

int armui_menu_item(struct armui_context *context, const char *label)
{
    return context ? nk_menu_item_label(&context->ui, label,
                                         NK_TEXT_LEFT) : 0;
}

void armui_menu_end(struct armui_context *context)
{
    if (context)
        nk_menu_end(&context->ui);
}

void armui_render(struct armui_context *context,
                  const struct armui_target *target,
                  int clear_target, uint32_t background)
{
    struct armui_nk_target private_target;

    if (!context || !target)
        return;
    private_target = (struct armui_nk_target){
        target->pixels, target->width, target->height, target->stride,
        context->font_pixels, context->font_width, context->font_height
    };
    armui_nk_render(&context->ui, &private_target,
                    clear_target, background);
}
