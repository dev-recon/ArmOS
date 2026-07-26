/*
 * ArmOS
 * Copyright (c) 2026 Mohamed Ennassiri
 *
 * Licensed under the Apache License, Version 2.0.
 * See LICENSE for details.
 *
 * File: userland/opt/fontconfig/test/fontconfig-test.c
 * Layer: Userland / third-party library validation
 *
 * Responsibilities:
 * - Validate Fontconfig initialization over Expat and FreeType.
 * - Match the generic monospace family to the bundled Meslo font.
 *
 * Notes:
 * - The test avoids a pre-generated architecture-dependent font cache.
 * - Font discovery therefore exercises the live target filesystem.
 */

#include <stdio.h>
#include <string.h>

#include <fontconfig/fontconfig.h>

#define TEST_FONT "/usr/share/fonts/armos/MesloLGS-NF-Regular.ttf"

int main(void)
{
    FcConfig *config;
    FcPattern *request;
    FcPattern *match;
    FcResult result;
    FcChar8 *family = NULL;
    FcChar8 *file = NULL;

    config = FcInitLoadConfig();
    if (config == NULL ||
        !FcConfigAppFontAddFile(config, (const FcChar8 *)TEST_FONT) ||
        !FcConfigBuildFonts(config)) {
        printf("fontconfig-test: FAIL: initialize configuration\n");
        FcConfigDestroy(config);
        return 1;
    }

    request = FcNameParse((const FcChar8 *)"monospace:size=14");
    if (request == NULL) {
        printf("fontconfig-test: FAIL: create pattern\n");
        FcConfigDestroy(config);
        return 1;
    }
    FcConfigSubstitute(config, request, FcMatchPattern);
    FcDefaultSubstitute(request);
    match = FcFontMatch(config, request, &result);

    if (match == NULL ||
        FcPatternGetString(match, FC_FAMILY, 0, &family) != FcResultMatch ||
        FcPatternGetString(match, FC_FILE, 0, &file) != FcResultMatch ||
        family == NULL || file == NULL ||
        strstr((const char *)family, "Meslo") == NULL ||
        strcmp((const char *)file, TEST_FONT) != 0) {
        printf("fontconfig-test: FAIL: match monospace\n");
        FcPatternDestroy(match);
        FcPatternDestroy(request);
        FcConfigDestroy(config);
        return 1;
    }

    printf("fontconfig-test: PASS: %s\n", family);
    FcPatternDestroy(match);
    FcPatternDestroy(request);
    FcConfigDestroy(config);
    FcFini();
    return 0;
}
