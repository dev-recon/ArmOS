/*
 * ArmOS
 * Copyright (c) 2026 Mohamed Ennassiri
 *
 * Licensed under the Apache License, Version 2.0.
 * See LICENSE for details.
 *
 * File: userland/opt/fcft/test/fcft-test.c
 * Layer: Userland / font stack regression tests
 *
 * Responsibilities:
 * - Load the ArmOS monospace font through Fontconfig and FreeType.
 * - Rasterize Latin and accented glyphs through fcft.
 * - Validate HarfBuzz grapheme and utf8proc text-run shaping.
 */

#include <fcft/fcft.h>
#include <stdio.h>

static int check_glyph(struct fcft_font *font, wchar_t codepoint)
{
    const struct fcft_glyph *glyph;

    glyph = fcft_glyph_rasterize(font, codepoint, FCFT_SUBPIXEL_NONE);
    if (glyph == NULL || glyph->pix == NULL ||
        glyph->width <= 0 || glyph->height <= 0 ||
        glyph->advance.x <= 0) {
        printf("fcft-test: cannot rasterize U+%04lx\n",
               (unsigned long)codepoint);
        return 1;
    }
    return 0;
}

int main(void)
{
    const char *names[] = { "monospace:size=14" };
    struct fcft_font *font;

    fcft_log_init(FCFT_LOG_COLORIZE_NEVER, false, FCFT_LOG_CLASS_WARNING);
    if (fcft_capabilities() !=
        (FCFT_CAPABILITY_GRAPHEME_SHAPING |
         FCFT_CAPABILITY_TEXT_RUN_SHAPING)) {
        puts("fcft-test: shaping capabilities are incomplete");
        return 1;
    }

    font = fcft_from_name(1, names, NULL);
    if (font == NULL || font->height <= 0 ||
        font->max_advance.x <= 0) {
        puts("fcft-test: cannot load monospace font");
        return 1;
    }

    if (check_glyph(font, L'A') != 0 ||
        check_glyph(font, L'\u00e9') != 0) {
        fcft_destroy(font);
        return 1;
    }

    fcft_destroy(font);
    puts("fcft-test: PASS");
    return 0;
}
