/*
 * ArmOS
 * Copyright (c) 2026 Mohamed Ennassiri
 *
 * Licensed under the Apache License, Version 2.0.
 * See LICENSE for details.
 *
 * File: userland/coreutils/src/grep.c
 * Layer: Userland / core utility
 * Description: POSIX-like command-line utility for ArmOS.
 */

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef struct grep_options {
    int line_numbers;
    int ignore_case;
    int invert;
    int count_only;
    int quiet;
    int whole_line;
    int always_filename;
    int never_filename;
} grep_options_t;

static void usage(void)
{
    printf("usage: grep [-n] [-i] [-v] [-c] [-q] [-x] [-H] [-h] PATTERN [FILE...]\n");
}

static char lower_ascii(char c)
{
    if (c >= 'A' && c <= 'Z')
        return (char)(c - 'A' + 'a');
    return c;
}

static int char_equal(char a, char b, int ignore_case)
{
    if (ignore_case)
        return lower_ascii(a) == lower_ascii(b);
    return a == b;
}

static int contains_literal(const char* line, const char* pattern,
                            int ignore_case, int whole_line)
{
    size_t line_len = strlen(line);
    size_t pattern_len = strlen(pattern);
    size_t i;
    size_t j;

    if (whole_line) {
        if (pattern_len != line_len)
            return 0;
        for (i = 0; i < line_len; i++) {
            if (!char_equal(line[i], pattern[i], ignore_case))
                return 0;
        }
        return 1;
    }

    if (pattern_len == 0)
        return 1;

    if (pattern_len > line_len)
        return 0;

    for (i = 0; i <= line_len - pattern_len; i++) {
        for (j = 0; j < pattern_len; j++) {
            if (!char_equal(line[i + j], pattern[j], ignore_case))
                break;
        }
        if (j == pattern_len)
            return 1;
    }

    return 0;
}

static int ensure_capacity(char** line, size_t* capacity, size_t needed)
{
    char* grown;
    size_t new_capacity;

    if (needed <= *capacity)
        return 0;

    new_capacity = *capacity ? *capacity : 128;
    while (new_capacity < needed)
        new_capacity *= 2;

    grown = realloc(*line, new_capacity);
    if (!grown)
        return -1;

    *line = grown;
    *capacity = new_capacity;
    return 0;
}

static int read_line_fd(int fd, char** line, size_t* capacity, size_t* length)
{
    char ch;
    size_t len = 0;
    int saw_any = 0;

    while (1) {
        int n = read(fd, &ch, 1);

        if (n < 0)
            return -1;
        if (n == 0)
            break;

        saw_any = 1;
        if (ch == '\n')
            break;

        if (ensure_capacity(line, capacity, len + 2) < 0)
            return -1;
        (*line)[len++] = ch;
    }

    if (!saw_any)
        return 0;

    if (ensure_capacity(line, capacity, len + 1) < 0)
        return -1;

    (*line)[len] = '\0';
    *length = len;
    return 1;
}

static void print_match(const grep_options_t* opts, const char* name,
                        int use_filename, int line_no, const char* line)
{
    if (use_filename)
        printf("%s:", name);
    if (opts->line_numbers)
        printf("%d:", line_no);
    printf("%s\n", line);
}

static int grep_fd(int fd, const char* name, const char* pattern,
                   const grep_options_t* opts, int use_filename,
                   int* matched_any)
{
    char* line = NULL;
    size_t capacity = 0;
    size_t length = 0;
    int line_no = 0;
    int matches = 0;
    int result;

    while ((result = read_line_fd(fd, &line, &capacity, &length)) > 0) {
        int matched;

        (void)length;
        line_no++;
        matched = contains_literal(line, pattern, opts->ignore_case,
                                   opts->whole_line);
        if (opts->invert)
            matched = !matched;

        if (!matched)
            continue;

        matches++;
        *matched_any = 1;

        if (opts->quiet) {
            free(line);
            return 0;
        }

        if (!opts->count_only)
            print_match(opts, name, use_filename, line_no, line);
    }

    if (result < 0) {
        free(line);
        return 2;
    }

    if (opts->count_only) {
        if (use_filename)
            printf("%s:", name);
        printf("%d\n", matches);
    }

    free(line);
    return 0;
}

static int grep_file(const char* path, const char* pattern,
                     const grep_options_t* opts, int use_filename,
                     int* matched_any)
{
    int fd;
    int status;

    if (strcmp(path, "-") == 0)
        return grep_fd(STDIN_FILENO, "standard input", pattern, opts,
                       use_filename, matched_any);

    fd = open(path, O_RDONLY, 0);
    if (fd < 0) {
        printf("grep: cannot open %s\n", path);
        return 2;
    }

    status = grep_fd(fd, path, pattern, opts, use_filename, matched_any);
    close(fd);
    return status;
}

static int parse_options(int argc, char** argv, grep_options_t* opts)
{
    int i;

    for (i = 1; i < argc; i++) {
        int j;

        if (strcmp(argv[i], "--") == 0)
            return i + 1;
        if (strcmp(argv[i], "--help") == 0) {
            usage();
            exit(0);
        }
        if (argv[i][0] != '-' || argv[i][1] == '\0')
            return i;

        for (j = 1; argv[i][j]; j++) {
            switch (argv[i][j]) {
            case 'n':
                opts->line_numbers = 1;
                break;
            case 'i':
                opts->ignore_case = 1;
                break;
            case 'v':
                opts->invert = 1;
                break;
            case 'c':
                opts->count_only = 1;
                break;
            case 'q':
                opts->quiet = 1;
                break;
            case 'x':
                opts->whole_line = 1;
                break;
            case 'H':
                opts->always_filename = 1;
                break;
            case 'h':
                opts->never_filename = 1;
                break;
            default:
                printf("grep: unknown option -%c\n", argv[i][j]);
                usage();
                return -1;
            }
        }
    }

    return i;
}

int main(int argc, char** argv)
{
    grep_options_t opts;
    const char* pattern;
    int first_arg;
    int file_count;
    int matched_any = 0;
    int had_error = 0;
    int i;

    memset(&opts, 0, sizeof(opts));

    first_arg = parse_options(argc, argv, &opts);
    if (first_arg < 0)
        return 2;

    if (first_arg >= argc) {
        usage();
        return 2;
    }

    pattern = argv[first_arg++];
    file_count = argc - first_arg;

    if (file_count == 0) {
        int use_filename = opts.always_filename && !opts.never_filename;
        int status = grep_fd(STDIN_FILENO, "standard input", pattern, &opts,
                             use_filename, &matched_any);
        if (status == 2)
            return 2;
        return matched_any ? 0 : 1;
    }

    for (i = first_arg; i < argc; i++) {
        int use_filename = 0;
        int status;

        if (!opts.never_filename && (opts.always_filename || file_count > 1))
            use_filename = 1;

        status = grep_file(argv[i], pattern, &opts, use_filename, &matched_any);
        if (status == 2)
            had_error = 1;
        if (opts.quiet && matched_any)
            return 0;
    }

    if (had_error)
        return 2;

    return matched_any ? 0 : 1;
}
