/*
 * ArmOS
 * Copyright (c) 2026 Mohamed Ennassiri
 *
 * Licensed under the Apache License, Version 2.0.
 * See LICENSE for details.
 *
 * File: userland/opt/utf8proc/test/utf8proc-test.c
 * Layer: Userland / third-party library validation
 *
 * Responsibilities:
 * - Validate the utf8proc ABI used by the Foot terminal port.
 * - Exercise decoding, widths, grapheme boundaries and normalization.
 *
 * Notes:
 * - The test links against the official unmodified utf8proc library.
 * - It is architecture-neutral and runs on every ArmOS target.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <utf8proc.h>

static int check(int condition, const char *operation)
{
    if (condition)
        return 0;
    printf("utf8proc-test: FAIL: %s\n", operation);
    return -1;
}

int main(void)
{
    static const utf8proc_uint8_t french[] = "Th\xc3\xa9i\xc3\xa8re";
    static const utf8proc_uint8_t decomposed[] = "e\xcc\x81";
    static const utf8proc_uint8_t composed[] = "\xc3\xa9";
    static const utf8proc_uint8_t invalid[] = { 0xc3u, 0x28u, 0u };
    utf8proc_int32_t codepoint;
    utf8proc_uint8_t *normalized;
    utf8proc_ssize_t consumed;

    if (check(strcmp(utf8proc_version(), "2.11.3") == 0,
              "library version") < 0 ||
        check(strcmp(utf8proc_unicode_version(), "17.0.0") == 0,
              "Unicode data version") < 0)
        return 1;

    consumed = utf8proc_iterate(french + 2, 2, &codepoint);
    if (check(consumed == 2 && codepoint == 0x00e9,
              "decode multibyte codepoint") < 0 ||
        check(utf8proc_iterate(invalid, 2, &codepoint) ==
                  UTF8PROC_ERROR_INVALIDUTF8,
              "reject invalid UTF-8") < 0 ||
        check(utf8proc_charwidth('A') == 1,
              "ASCII character width") < 0 ||
        check(utf8proc_charwidth(0x754cu) == 2,
              "wide Unicode character") < 0 ||
        check(!utf8proc_grapheme_break('e', 0x0301),
              "combining grapheme cluster") < 0 ||
        check(utf8proc_grapheme_break(0x00e9, 'x'),
              "separate grapheme clusters") < 0)
        return 1;

    normalized = utf8proc_NFC(decomposed);
    if (check(normalized != NULL, "allocate NFC result") < 0)
        return 1;
    if (check(strcmp((const char *)normalized,
                     (const char *)composed) == 0,
              "NFC composition") < 0) {
        free(normalized);
        return 1;
    }
    free(normalized);

    printf("utf8proc-test: PASS\n");
    return 0;
}
