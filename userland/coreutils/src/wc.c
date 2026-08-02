/*
 * ArmOS
 * Copyright (c) 2026 Mohamed Ennassiri
 *
 * Licensed under the Apache License, Version 2.0.
 * See LICENSE for details.
 *
 * File: userland/coreutils/src/wc.c
 * Layer: Userland / core utility
 * Description: POSIX-like command-line utility for ArmOS.
 */

#include <fcntl.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

typedef struct counts {
    unsigned long lines;
    unsigned long words;
    unsigned long bytes;
    unsigned long characters;
} counts_t;

typedef struct output_fields {
    int lines;
    int words;
    int bytes;
    int characters;
} output_fields_t;

static int wc_fd(int fd, counts_t *c)
{
    char buf[512];
    int n;
    int in_word = 0;

    while ((n = read(fd, buf, sizeof(buf))) > 0) {
        c->bytes += (unsigned long)n;
        for (int i = 0; i < n; i++) {
            char ch = buf[i];
            unsigned char byte = (unsigned char)ch;
            int sep = ch == ' ' || ch == '\t' || ch == '\n' ||
                      ch == '\r' || ch == '\v' || ch == '\f';
            if ((byte & 0xc0u) != 0x80u)
                c->characters++;
            if (ch == '\n')
                c->lines++;
            if (sep) {
                in_word = 0;
            } else if (!in_word) {
                c->words++;
                in_word = 1;
            }
        }
    }

    return n < 0 ? 1 : 0;
}

static void print_counts(const counts_t *c, const output_fields_t *fields,
                         const char *name)
{
    if (fields->lines)
        printf("%7lu", c->lines);
    if (fields->words)
        printf("%7lu", c->words);
    if (fields->bytes)
        printf("%7lu", c->bytes);
    if (fields->characters)
        printf("%7lu", c->characters);
    if (name)
        printf(" %s", name);
    putchar('\n');
}

int main(int argc, char **argv)
{
    counts_t total = {0, 0, 0, 0};
    output_fields_t fields = {0, 0, 0, 0};
    int first_file = 1;
    int file_count;
    int status = 0;

    while (first_file < argc && argv[first_file][0] == '-' &&
           argv[first_file][1] != '\0') {
        const char *option = argv[first_file] + 1;

        if (strcmp(argv[first_file], "--") == 0) {
            first_file++;
            break;
        }
        while (*option) {
            switch (*option++) {
            case 'c': fields.bytes = 1; break;
            case 'l': fields.lines = 1; break;
            case 'm': fields.characters = 1; break;
            case 'w': fields.words = 1; break;
            default:
                fprintf(stderr, "usage: wc [-clmw] [file ...]\n");
                return 1;
            }
        }
        first_file++;
    }
    if (!fields.lines && !fields.words && !fields.bytes && !fields.characters)
        fields = (output_fields_t){1, 1, 1, 0};

    file_count = argc - first_file;
    if (file_count == 0) {
        counts_t c = {0, 0, 0, 0};
        status = wc_fd(STDIN_FILENO, &c);
        print_counts(&c, &fields, NULL);
        return status;
    }

    for (int i = first_file; i < argc; i++) {
        counts_t c = {0, 0, 0, 0};
        int fd = strcmp(argv[i], "-") == 0 ? STDIN_FILENO :
                                             open(argv[i], O_RDONLY, 0);
        if (fd < 0) {
            fprintf(stderr, "wc: %s: %s\n", argv[i], strerror(errno));
            status = 1;
            continue;
        }
        if (wc_fd(fd, &c) != 0)
            status = 1;
        if (fd != STDIN_FILENO)
            close(fd);
        print_counts(&c, &fields, argv[i]);
        total.lines += c.lines;
        total.words += c.words;
        total.bytes += c.bytes;
        total.characters += c.characters;
    }

    if (file_count > 1)
        print_counts(&total, &fields, "total");

    return status;
}
