/*
 * ArmOS
 * Copyright (c) 2026 Mohamed Ennassiri
 *
 * Licensed under the Apache License, Version 2.0.
 * See LICENSE for details.
 *
 * File: userland/lib/armui/nuklear_software.c
 * Layer: Userland / graphical libraries
 *
 * Responsibilities:
 * - Rasterize Nuklear command buffers into an XRGB8888 software target.
 * - Clip every primitive to the current Nuklear scissor rectangle.
 * - Render the shared baked font atlas without platform-specific code.
 */

#include <stdint.h>
#include <stdlib.h>

#include <armos_nuklear_config.h>
#include "nuklear_software.h"

struct clip_rect {
    int x0;
    int y0;
    int x1;
    int y1;
};

static uint32_t pack_color(struct nk_color color)
{
    return ((uint32_t)color.r << 16) |
           ((uint32_t)color.g << 8) |
           (uint32_t)color.b;
}

static int maximum(int left, int right)
{
    return left > right ? left : right;
}

static int minimum(int left, int right)
{
    return left < right ? left : right;
}

static int point_in_clip(const struct clip_rect *clip, int x, int y)
{
    return x >= clip->x0 && y >= clip->y0 &&
           x < clip->x1 && y < clip->y1;
}

static void blend_pixel(const struct armui_nk_target *target,
                        const struct clip_rect *clip, int x, int y,
                        struct nk_color color, unsigned int coverage)
{
    uint32_t *destination;
    uint32_t previous;
    unsigned int alpha;
    unsigned int inverse;
    unsigned int red;
    unsigned int green;
    unsigned int blue;

    if (!point_in_clip(clip, x, y))
        return;
    alpha = ((unsigned int)color.a * coverage + 127u) / 255u;
    destination = &target->pixels[(size_t)y * target->stride + (size_t)x];
    if (alpha >= 255u) {
        *destination = pack_color(color);
        return;
    }
    if (alpha == 0u)
        return;

    previous = *destination;
    inverse = 255u - alpha;
    red = ((previous >> 16) & 0xffu) * inverse + color.r * alpha;
    green = ((previous >> 8) & 0xffu) * inverse + color.g * alpha;
    blue = (previous & 0xffu) * inverse + color.b * alpha;
    *destination = ((red / 255u) << 16) |
                   ((green / 255u) << 8) |
                   (blue / 255u);
}

static void fill_rect(const struct armui_nk_target *target,
                      const struct clip_rect *clip,
                      int x, int y, int width, int height,
                      struct nk_color color)
{
    int x0 = maximum(x, clip->x0);
    int y0 = maximum(y, clip->y0);
    int x1 = minimum(x + width, clip->x1);
    int y1 = minimum(y + height, clip->y1);

    for (int row = y0; row < y1; row++) {
        uint32_t *line =
            target->pixels + (size_t)row * (size_t)target->stride;

        if (color.a == 255u) {
            uint32_t packed = pack_color(color);

            for (int column = x0; column < x1; column++)
                line[column] = packed;
        } else {
            for (int column = x0; column < x1; column++)
                blend_pixel(target, clip, column, row, color, 255u);
        }
    }
}

static void draw_line(const struct armui_nk_target *target,
                      const struct clip_rect *clip,
                      int x0, int y0, int x1, int y1,
                      int thickness, struct nk_color color)
{
    int dx = abs(x1 - x0);
    int sx = x0 < x1 ? 1 : -1;
    int dy = -abs(y1 - y0);
    int sy = y0 < y1 ? 1 : -1;
    int error = dx + dy;
    int radius = maximum(thickness, 1) / 2;

    for (;;) {
        fill_rect(target, clip, x0 - radius, y0 - radius,
                  maximum(thickness, 1), maximum(thickness, 1), color);
        if (x0 == x1 && y0 == y1)
            break;
        if (2 * error >= dy) {
            error += dy;
            x0 += sx;
        }
        if (2 * error <= dx) {
            error += dx;
            y0 += sy;
        }
    }
}

static int edge_value(int ax, int ay, int bx, int by, int px, int py)
{
    return (px - ax) * (by - ay) - (py - ay) * (bx - ax);
}

static void fill_triangle(const struct armui_nk_target *target,
                          const struct clip_rect *clip,
                          int ax, int ay, int bx, int by, int cx, int cy,
                          struct nk_color color)
{
    int x0 = maximum(minimum(ax, minimum(bx, cx)), clip->x0);
    int y0 = maximum(minimum(ay, minimum(by, cy)), clip->y0);
    int x1 = minimum(maximum(ax, maximum(bx, cx)), clip->x1 - 1);
    int y1 = minimum(maximum(ay, maximum(by, cy)), clip->y1 - 1);
    int orientation = edge_value(ax, ay, bx, by, cx, cy);

    for (int y = y0; y <= y1; y++) {
        for (int x = x0; x <= x1; x++) {
            int first = edge_value(ax, ay, bx, by, x, y);
            int second = edge_value(bx, by, cx, cy, x, y);
            int third = edge_value(cx, cy, ax, ay, x, y);

            if ((orientation >= 0 &&
                 first >= 0 && second >= 0 && third >= 0) ||
                (orientation < 0 &&
                 first <= 0 && second <= 0 && third <= 0))
                blend_pixel(target, clip, x, y, color, 255u);
        }
    }
}

static void fill_ellipse(const struct armui_nk_target *target,
                         const struct clip_rect *clip,
                         int x, int y, int width, int height,
                         struct nk_color color)
{
    int radius_x = maximum(width / 2, 1);
    int radius_y = maximum(height / 2, 1);
    int center_x = x + radius_x;
    int center_y = y + radius_y;
    int64_t radius_x_squared = (int64_t)radius_x * radius_x;
    int64_t radius_y_squared = (int64_t)radius_y * radius_y;
    int64_t limit = radius_x_squared * radius_y_squared;

    for (int row = maximum(y, clip->y0);
         row < minimum(y + height, clip->y1); row++) {
        int64_t delta_y = row - center_y;

        for (int column = maximum(x, clip->x0);
             column < minimum(x + width, clip->x1); column++) {
            int64_t delta_x = column - center_x;
            int64_t distance =
                delta_x * delta_x * radius_y_squared +
                delta_y * delta_y * radius_x_squared;

            if (distance <= limit)
                blend_pixel(target, clip, column, row, color, 255u);
        }
    }
}

static void draw_text(const struct armui_nk_target *target,
                      const struct clip_rect *clip,
                      const struct nk_command_text *text)
{
    const struct nk_user_font *font = text->font;
    float cursor_x = (float)text->x;
    int offset = 0;

    if (!font || !font->query || !target->font_pixels)
        return;

    while (offset < text->length) {
        struct nk_user_font_glyph glyph;
        nk_rune codepoint = 0u;
        nk_rune next = 0u;
        int consumed = nk_utf_decode(text->string + offset, &codepoint,
                                     text->length - offset);
        int next_consumed;
        int source_x0;
        int source_y0;
        int source_x1;
        int source_y1;
        int destination_x;
        int destination_y;
        int destination_width;
        int destination_height;
        int source_width;
        int source_height;

        if (consumed <= 0)
            break;
        next_consumed = nk_utf_decode(text->string + offset + consumed, &next,
                                      text->length - offset - consumed);
        if (next_consumed <= 0)
            next = 0u;
        font->query(font->userdata, text->height, &glyph, codepoint, next);

        source_x0 = (int)(glyph.uv[0].x * target->font_width);
        source_y0 = (int)(glyph.uv[0].y * target->font_height);
        source_x1 = (int)(glyph.uv[1].x * target->font_width);
        source_y1 = (int)(glyph.uv[1].y * target->font_height);
        destination_x = (int)(cursor_x + glyph.offset.x);
        destination_y = (int)((float)text->y + glyph.offset.y);
        destination_width = maximum((int)(glyph.width + 0.5f), 1);
        destination_height = maximum((int)(glyph.height + 0.5f), 1);
        source_width = maximum(source_x1 - source_x0, 1);
        source_height = maximum(source_y1 - source_y0, 1);

        for (int y = 0; y < destination_height; y++) {
            int source_y = source_y0 +
                (y * source_height) / destination_height;

            for (int x = 0; x < destination_width; x++) {
                int source_x = source_x0 +
                    (x * source_width) / destination_width;
                unsigned int alpha =
                    target->font_pixels[
                        (size_t)source_y * target->font_width + source_x];

                blend_pixel(target, clip,
                            destination_x + x,
                            destination_y + y,
                            text->foreground, alpha);
            }
        }
        cursor_x += glyph.xadvance;
        offset += consumed;
    }
}

void armui_nk_render(struct nk_context *context,
                     const struct armui_nk_target *target,
                     int clear_target, uint32_t background)
{
    const struct nk_command *command;
    struct clip_rect clip;

    if (!context || !target || !target->pixels ||
        target->width <= 0 || target->height <= 0 ||
        target->stride < target->width)
        return;
    clip = (struct clip_rect){ 0, 0, target->width, target->height };
    if (clear_target) {
        struct nk_color color = nk_rgb(
            (background >> 16) & 0xffu,
            (background >> 8) & 0xffu,
            background & 0xffu);

        fill_rect(target, &clip, 0, 0, target->width, target->height, color);
    }

    nk_foreach(command, context) {
        switch (command->type) {
        case NK_COMMAND_SCISSOR: {
            const struct nk_command_scissor *scissor =
                (const struct nk_command_scissor *)command;

            clip.x0 = maximum(scissor->x, 0);
            clip.y0 = maximum(scissor->y, 0);
            clip.x1 = minimum(scissor->x + (int)scissor->w, target->width);
            clip.y1 = minimum(scissor->y + (int)scissor->h, target->height);
            break;
        }
        case NK_COMMAND_LINE: {
            const struct nk_command_line *line =
                (const struct nk_command_line *)command;

            draw_line(target, &clip, line->begin.x, line->begin.y,
                      line->end.x, line->end.y, line->line_thickness,
                      line->color);
            break;
        }
        case NK_COMMAND_RECT: {
            const struct nk_command_rect *rectangle =
                (const struct nk_command_rect *)command;
            int thickness = maximum(rectangle->line_thickness, 1);

            fill_rect(target, &clip, rectangle->x, rectangle->y,
                      rectangle->w, thickness, rectangle->color);
            fill_rect(target, &clip, rectangle->x,
                      rectangle->y + rectangle->h - thickness,
                      rectangle->w, thickness, rectangle->color);
            fill_rect(target, &clip, rectangle->x, rectangle->y,
                      thickness, rectangle->h, rectangle->color);
            fill_rect(target, &clip,
                      rectangle->x + rectangle->w - thickness,
                      rectangle->y, thickness, rectangle->h,
                      rectangle->color);
            break;
        }
        case NK_COMMAND_RECT_FILLED: {
            const struct nk_command_rect_filled *rectangle =
                (const struct nk_command_rect_filled *)command;

            fill_rect(target, &clip, rectangle->x, rectangle->y,
                      rectangle->w, rectangle->h, rectangle->color);
            break;
        }
        case NK_COMMAND_TRIANGLE: {
            const struct nk_command_triangle *triangle =
                (const struct nk_command_triangle *)command;

            draw_line(target, &clip, triangle->a.x, triangle->a.y,
                      triangle->b.x, triangle->b.y,
                      triangle->line_thickness, triangle->color);
            draw_line(target, &clip, triangle->b.x, triangle->b.y,
                      triangle->c.x, triangle->c.y,
                      triangle->line_thickness, triangle->color);
            draw_line(target, &clip, triangle->c.x, triangle->c.y,
                      triangle->a.x, triangle->a.y,
                      triangle->line_thickness, triangle->color);
            break;
        }
        case NK_COMMAND_TRIANGLE_FILLED: {
            const struct nk_command_triangle_filled *triangle =
                (const struct nk_command_triangle_filled *)command;

            fill_triangle(target, &clip,
                          triangle->a.x, triangle->a.y,
                          triangle->b.x, triangle->b.y,
                          triangle->c.x, triangle->c.y,
                          triangle->color);
            break;
        }
        case NK_COMMAND_CIRCLE_FILLED: {
            const struct nk_command_circle_filled *circle =
                (const struct nk_command_circle_filled *)command;

            fill_ellipse(target, &clip, circle->x, circle->y,
                         circle->w, circle->h, circle->color);
            break;
        }
        case NK_COMMAND_TEXT:
            draw_text(target, &clip,
                      (const struct nk_command_text *)command);
            break;
        default:
            break;
        }
    }
    nk_clear(context);
}
