/*
 * ArmOS
 * Copyright (c) 2026 Mohamed Ennassiri
 *
 * Licensed under the Apache License, Version 2.0.
 * See LICENSE for details.
 *
 * File: userland/programs/tcc/tcc.c
 * Layer: Userland / program
 * Description: ArmOS TinyCC wrapper.
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

extern char **environ;

#define REAL_TCC "/opt/tcc/bin/tcc"
#define TCC_MAX_ARGS 128

#ifndef ARMOS_USER_TEXT_ADDRESS
#define ARMOS_USER_TEXT_ADDRESS 0x8000
#endif

#define STRINGIFY_VALUE(value) #value
#define STRINGIFY(value) STRINGIFY_VALUE(value)
#define ARMOS_TCC_TEXT_OPTION \
    "-Wl,-Ttext=" STRINGIFY(ARMOS_USER_TEXT_ADDRESS)

static int arg_is(const char *arg, const char *name)
{
    return arg && strcmp(arg, name) == 0;
}

static int is_compile_only(int argc, char **argv)
{
    int i;

    for (i = 1; i < argc; i++) {
        if (arg_is(argv[i], "-c") ||
            arg_is(argv[i], "-E") ||
            arg_is(argv[i], "-S"))
            return 1;
    }

    return 0;
}

static int should_passthrough_raw(int argc, char **argv)
{
    int i;

    /*
     * Manual low-level modes must not be rewritten by the wrapper.  Compile
     * modes are handled separately so we can still add ArmOS include paths.
     */
    for (i = 1; i < argc; i++) {
        if (arg_is(argv[i], "-v") ||
            arg_is(argv[i], "-vv") ||
            arg_is(argv[i], "-h") ||
            arg_is(argv[i], "--help") ||
            arg_is(argv[i], "-nostdlib") ||
            arg_is(argv[i], "-nostartfiles"))
            return 1;
    }

    return 0;
}

static int exec_real_tcc(int argc, char **argv, int add_include_paths)
{
    char *real_argv[TCC_MAX_ARGS];
    int i, n = 0;

    if (argc + (add_include_paths ? 6 : 0) >= TCC_MAX_ARGS) {
        fprintf(stderr, "tcc: too many arguments\n");
        return 1;
    }

    real_argv[n++] = (char *)REAL_TCC;
    if (add_include_paths) {
        real_argv[n++] = "-I/opt/tcc/lib/tcc/include";
        real_argv[n++] = "-I/opt/tcc/include/armos";
        real_argv[n++] = "-I/opt/tcc/include";
        real_argv[n++] = "-I/opt/ncurses/include";
        real_argv[n++] = "-I/opt/ncurses/include/ncurses";
    }
    for (i = 1; i < argc; i++)
        real_argv[n++] = argv[i];
    real_argv[n] = NULL;

    execve(REAL_TCC, real_argv, environ);
    fprintf(stderr, "tcc: cannot exec %s: %s\n", REAL_TCC, strerror(errno));
    return 127;
}

static int exec_armos_link(int argc, char **argv)
{
    char *real_argv[TCC_MAX_ARGS];
    int n = 0;
    int i;

    if (argc + 18 >= TCC_MAX_ARGS) {
        fprintf(stderr, "tcc: too many arguments\n");
        return 1;
    }

    real_argv[n++] = (char *)REAL_TCC;
    real_argv[n++] = "-static";
    real_argv[n++] = "-nostdlib";
    real_argv[n++] = ARMOS_TCC_TEXT_OPTION;
    real_argv[n++] = "-Wl,-e,_start";
    real_argv[n++] = "-I/opt/tcc/lib/tcc/include";
    real_argv[n++] = "-I/opt/tcc/include/armos";
    real_argv[n++] = "-I/opt/tcc/include";
    real_argv[n++] = "-I/opt/ncurses/include";
    real_argv[n++] = "-I/opt/ncurses/include/ncurses";
    real_argv[n++] = "/opt/tcc/lib/crt0_newlib.o";
    real_argv[n++] = "/opt/tcc/lib/syscall_raw.o";
    real_argv[n++] = "/opt/tcc/lib/syscalls_min.o";
    real_argv[n++] = "/opt/tcc/lib/stdio_lock.o";

    for (i = 1; i < argc; i++) {
        if (arg_is(argv[i], "-lncurses") || arg_is(argv[i], "-lcurses"))
            real_argv[n++] = "/opt/ncurses/lib/libncurses.a";
        else
            real_argv[n++] = argv[i];
    }

    real_argv[n++] = "/opt/tcc/lib/libm.a";
    real_argv[n++] = "/opt/tcc/lib/libc.a";
    real_argv[n++] = "/opt/tcc/lib/libgcc.a";
    real_argv[n] = NULL;

    execve(REAL_TCC, real_argv, environ);
    fprintf(stderr, "tcc: cannot exec %s: %s\n", REAL_TCC, strerror(errno));
    return 127;
}

int main(int argc, char **argv)
{
    if (argc <= 1 || should_passthrough_raw(argc, argv))
        return exec_real_tcc(argc, argv, 0);

    if (is_compile_only(argc, argv))
        return exec_real_tcc(argc, argv, 1);

    return exec_armos_link(argc, argv);
}
