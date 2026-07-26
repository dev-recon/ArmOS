/*
 * ArmOS
 * Copyright (c) 2026 Mohamed Ennassiri
 *
 * Licensed under the Apache License, Version 2.0.
 * See LICENSE for details.
 *
 * File: userland/opt/expat/test/expat-test.c
 * Layer: Userland / third-party library validation
 *
 * Responsibilities:
 * - Validate the Expat ABI required by Fontconfig.
 * - Parse representative configuration XML through streaming callbacks.
 *
 * Notes:
 * - The test links against the official unmodified Expat library.
 * - Input is split across calls to exercise incremental parsing.
 */

#include <stdio.h>
#include <string.h>

#include <expat.h>

struct parse_state {
    unsigned int elements;
    unsigned int directories;
    unsigned int characters;
};

static void XMLCALL start_element(void *opaque, const XML_Char *name,
                                  const XML_Char **attributes)
{
    struct parse_state *state = opaque;

    (void)attributes;
    state->elements++;
    if (strcmp(name, "dir") == 0)
        state->directories++;
}

static void XMLCALL character_data(void *opaque, const XML_Char *text,
                                   int length)
{
    struct parse_state *state = opaque;
    int index;

    for (index = 0; index < length; index++)
        state->characters += text[index] > ' ';
}

int main(void)
{
    static const char document[] =
        "<?xml version=\"1.0\"?>"
        "<fontconfig><dir>/usr/share/fonts/armos</dir>"
        "<match target=\"pattern\"><test name=\"family\">"
        "<string>MesloLGS NF</string></test></match></fontconfig>";
    struct parse_state state = { 0, 0, 0 };
    XML_Parser parser;
    XML_Expat_Version version;
    size_t split = sizeof(document) / 2;

    version = XML_ExpatVersionInfo();
    if (version.major != 2 || version.minor != 8 ||
        version.micro != 2) {
        printf("expat-test: FAIL: library version\n");
        return 1;
    }

    parser = XML_ParserCreate(NULL);
    if (parser == NULL) {
        printf("expat-test: FAIL: create parser\n");
        return 1;
    }
    XML_SetUserData(parser, &state);
    XML_SetElementHandler(parser, start_element, NULL);
    XML_SetCharacterDataHandler(parser, character_data);

    if (XML_Parse(parser, document, (int)split, XML_FALSE) !=
            XML_STATUS_OK ||
        XML_Parse(parser, document + split,
                  (int)(sizeof(document) - 1 - split), XML_TRUE) !=
            XML_STATUS_OK) {
        printf("expat-test: FAIL: parse XML: %s\n",
               XML_ErrorString(XML_GetErrorCode(parser)));
        XML_ParserFree(parser);
        return 1;
    }
    XML_ParserFree(parser);

    if (state.elements != 6 || state.directories != 1 ||
        state.characters < 20) {
        printf("expat-test: FAIL: callback counts\n");
        return 1;
    }

    printf("expat-test: PASS\n");
    return 0;
}
