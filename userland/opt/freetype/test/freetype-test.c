/*
 * ArmOS
 * Copyright (c) 2026 Mohamed Ennassiri
 *
 * Licensed under the Apache License, Version 2.0.
 * See LICENSE for details.
 *
 * File: userland/opt/freetype/test/freetype-test.c
 * Layer: Userland / third-party library validation
 *
 * Responsibilities:
 * - Validate the FreeType ABI used by the Foot terminal port.
 * - Load the bundled Meslo face and rasterize representative glyphs.
 *
 * Notes:
 * - The test links against the official unmodified FreeType library.
 * - It exercises the target filesystem rather than embedding font data.
 */

#include <stdio.h>
#include <string.h>

#include <ft2build.h>
#include FT_FREETYPE_H

#define TEST_FONT "/usr/share/fonts/armos/MesloLGS-NF-Regular.ttf"

static int check(int condition, const char *operation)
{
    if (condition)
        return 0;
    printf("freetype-test: FAIL: %s\n", operation);
    return -1;
}

static int render_glyph(FT_Face face, FT_ULong codepoint)
{
    FT_GlyphSlot glyph;
    unsigned int nonzero = 0;
    unsigned int row;
    unsigned int column;

    if (FT_Load_Char(face, codepoint, FT_LOAD_RENDER) != 0)
        return -1;
    glyph = face->glyph;
    for (row = 0; row < glyph->bitmap.rows; row++) {
        const unsigned char *pixels =
            glyph->bitmap.buffer + row * (unsigned int)glyph->bitmap.pitch;

        for (column = 0; column < glyph->bitmap.width; column++)
            nonzero += pixels[column] != 0;
    }

    return glyph->bitmap.width > 0 && glyph->bitmap.rows > 0 && nonzero > 0
               ? 0
               : -1;
}

int main(void)
{
    FT_Library library;
    FT_Face face;
    FT_Int major;
    FT_Int minor;
    FT_Int patch;

    if (check(FT_Init_FreeType(&library) == 0, "initialize library") < 0)
        return 1;
    FT_Library_Version(library, &major, &minor, &patch);
    if (check(major == 2 && minor == 14 && patch == 3,
              "library version") < 0 ||
        check(FT_New_Face(library, TEST_FONT, 0, &face) == 0,
              "open Meslo face") < 0) {
        FT_Done_FreeType(library);
        return 1;
    }

    if (check(face->family_name != NULL &&
                  strstr(face->family_name, "Meslo") != NULL,
              "font family") < 0 ||
        check(FT_Set_Pixel_Sizes(face, 0, 18) == 0,
              "set terminal cell size") < 0 ||
        check(render_glyph(face, 'A') == 0, "render ASCII glyph") < 0 ||
        check(render_glyph(face, 0x00e9) == 0,
              "render accented glyph") < 0) {
        FT_Done_Face(face);
        FT_Done_FreeType(library);
        return 1;
    }

    FT_Done_Face(face);
    FT_Done_FreeType(library);
    printf("freetype-test: PASS\n");
    return 0;
}
