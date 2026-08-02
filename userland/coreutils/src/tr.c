/*
 * ArmOS
 * Copyright (c) 2026 Mohamed Ennassiri
 *
 * Licensed under the Apache License, Version 2.0.
 * See LICENSE for details.
 *
 * File: userland/coreutils/src/tr.c
 * Layer: Userland / POSIX core utility
 * Description: Byte-oriented translate, delete and squeeze utility.
 */

#include <stdio.h>
#include <string.h>

static size_t expand_set(const char *source, unsigned char output[256])
{
    size_t count = 0;

    while (*source && count < 256) {
        unsigned char first;

        if (*source == '\\') {
            source++;
            if (*source >= '0' && *source <= '7') {
                unsigned value = 0;
                int digits = 0;
                while (digits < 3 && *source >= '0' && *source <= '7') {
                    value = value * 8u + (unsigned)(*source++ - '0');
                    digits++;
                }
                first = (unsigned char)value;
            } else {
                first = (unsigned char)(*source ? *source++ : '\\');
            }
        } else {
            first = (unsigned char)*source++;
        }
        if (*source == '-' && source[1]) {
            unsigned char last = (unsigned char)source[1];
            source += 2;
            if (first <= last) {
                for (unsigned value = first; value <= last && count < 256; value++)
                    output[count++] = (unsigned char)value;
                continue;
            }
        }
        output[count++] = first;
    }
    return count;
}

int main(int argc, char **argv)
{
    int delete_mode = 0;
    int squeeze = 0;
    int first = 1;
    unsigned char from[256], to[256], map[256], selected[256] = {0};
    size_t from_count, to_count = 0;
    int previous = -1;
    int character;

    if (argc > 1 && argv[1][0] == '-') {
        for (const char *option = argv[1] + 1; *option; option++) {
            if (*option == 'd') delete_mode = 1;
            else if (*option == 's') squeeze = 1;
            else {
                fprintf(stderr, "usage: tr [-ds] string1 [string2]\n");
                return 1;
            }
        }
        first++;
    }
    if (first >= argc || (!delete_mode && first + 1 >= argc)) {
        fprintf(stderr, "usage: tr [-ds] string1 [string2]\n");
        return 1;
    }
    from_count = expand_set(argv[first], from);
    if (first + 1 < argc)
        to_count = expand_set(argv[first + 1], to);
    for (unsigned i = 0; i < 256; i++)
        map[i] = (unsigned char)i;
    for (size_t i = 0; i < from_count; i++) {
        selected[from[i]] = 1;
        if (!delete_mode && to_count)
            map[from[i]] = to[i < to_count ? i : to_count - 1];
    }
    while ((character = getchar()) != EOF) {
        unsigned char value = (unsigned char)character;
        unsigned char translated;

        if (delete_mode && selected[value])
            continue;
        translated = map[value];
        if (squeeze && previous == translated &&
            (delete_mode ? selected[translated] : selected[value]))
            continue;
        if (putchar(translated) == EOF)
            return 1;
        previous = translated;
    }
    return ferror(stdin) || ferror(stdout);
}
