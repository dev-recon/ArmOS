/*
 * ArmOS
 * Copyright (c) 2026 Mohamed Ennassiri
 *
 * Licensed under the Apache License, Version 2.0.
 * See LICENSE for details.
 *
 * File: userland/opt/harfbuzz/test/harfbuzz-test.c
 * Layer: Userland / HarfBuzz regression test
 *
 * Responsibilities:
 * - Validate the public HarfBuzz C API on the target.
 * - Shape a real UTF-8 text run through the FreeType integration.
 */

#include <ft2build.h>
#include FT_FREETYPE_H
#include <hb-ft.h>
#include <hb.h>
#include <stdio.h>
#include <string.h>

#define TEST_FONT "/usr/share/fonts/armos/MesloLGS-NF-Regular.ttf"

int
main(void)
{
    static const char sample[] = "office \xC3\xA9";
    FT_Library ft;
    FT_Face face;
    hb_font_t *font;
    hb_buffer_t *buffer;
    unsigned int glyph_count;

    if (strcmp(hb_version_string(), HB_VERSION_STRING) != 0) {
        fprintf(stderr, "harfbuzz-test: version mismatch\n");
        return 1;
    }
    if (FT_Init_FreeType(&ft) != 0 ||
        FT_New_Face(ft, TEST_FONT, 0, &face) != 0) {
        fprintf(stderr, "harfbuzz-test: cannot load %s\n", TEST_FONT);
        return 1;
    }

    font = hb_ft_font_create_referenced(face);
    buffer = hb_buffer_create();
    if (font == NULL || buffer == NULL) {
        fprintf(stderr, "harfbuzz-test: allocation failed\n");
        return 1;
    }

    hb_buffer_add_utf8(buffer, sample, -1, 0, -1);
    hb_buffer_guess_segment_properties(buffer);
    hb_shape(font, buffer, NULL, 0);
    glyph_count = hb_buffer_get_length(buffer);

    hb_buffer_destroy(buffer);
    hb_font_destroy(font);
    FT_Done_Face(face);
    FT_Done_FreeType(ft);

    if (glyph_count == 0) {
        fprintf(stderr, "harfbuzz-test: no shaped glyphs\n");
        return 1;
    }

    printf("harfbuzz-test: PASS version=%s glyphs=%u\n",
           HB_VERSION_STRING, glyph_count);
    return 0;
}
