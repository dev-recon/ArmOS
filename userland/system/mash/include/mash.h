/*
 * ArmOS
 * Copyright (c) 2026 Mohamed Ennassiri
 *
 * Licensed under the Apache License, Version 2.0.
 * See LICENSE for details.
 *
 * File: userland/system/mash/include/mash.h
 * Layer: Userland / system service
 * Description: System-level userspace component for ArmOS.
 */

#ifndef _MASH_H
#define _MASH_H

#include <stdio.h>
#include <unistd.h>
#include <errno.h>

static inline int putc_tty(char c) {
    return write(1, &c, 1);
}

static inline int getc_tty(void) {
    char c;
    int n;

    errno = 0;
    n = read(0, &c, 1);
    if (n == 1)
        return (unsigned char)c;
    if (n < 0 && errno == EINTR)
        return -2;
    return -1;
}

static inline void pflush(void) {
    fflush(stdout);
}

// Signatures des fonctions de commandes
typedef int (*command_func_t)(int argc, char* argv[]);

// Structure pour enregistrer les commandes
typedef struct {
    const char* name;
    const char* description;
    command_func_t function;
} command_entry_t;

int register_command(const char* name, const char* desc, command_func_t function);
command_entry_t* find_command(const char* name);
void list_commands(void);
int command_init(void);
int command_count_registered(void);
const char* command_name_at(int index);
const char* shell_getenv(const char* name);
int shell_setenv(const char* name, const char* value);
int shell_unsetenv(const char* name);
int shell_env_count_registered(void);
const char* shell_env_name_at(int index);
const char* shell_env_value_at(int index);
int shell_last_status(void);
int shell_execute_line(char* line);
int shell_source_file(const char* path, int argc, char* argv[]);
int shell_push_script_args(const char* name, int argc, char* argv[]);
void shell_pop_script_args(void);
void shell_print_prompt(void);
char* shell_read_line(void);
int shell_line_was_eof(void);
void shell_line_edit_init(void);
void shell_line_edit_shutdown(void);
int shell_termination_requested(void);
void shell_restore_tty_mode(void);
int shell_repair_tty_mode(void);

#define SHELL_BUFFER_SIZE 1024
#define SHELL_MAX_ARGS      64

// Codes de retour des commandes
#define SHELL_OK            0
#define SHELL_ERROR         1
#define SHELL_EXIT          (-1000)

extern char input_buffer[SHELL_BUFFER_SIZE];
extern char* argv_buffer[SHELL_MAX_ARGS];
extern int shell_running;

#endif
