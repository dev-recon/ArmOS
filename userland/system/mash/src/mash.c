/*
 * ArmOS
 * Copyright (c) 2026 Mohamed Ennassiri
 *
 * Licensed under the Apache License, Version 2.0.
 * See LICENSE for details.
 *
 * File: userland/system/mash/src/mash.c
 * Layer: Userland / system service
 * Description: System-level userspace component for ArmOS.
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>
#include <string.h>
#include <fcntl.h>
#include <dirent.h>
#include <errno.h>
#include <sys/wait.h>
#include <termios.h>
#include "../include/mash.h"
#include "../include/jobs.h"

#ifndef SA_RESTART
#define SA_RESTART 0x01
#endif
#ifndef SIGHUP
#define SIGHUP 1
#endif
#ifndef SIGTTIN
#define SIGTTIN 21
#endif
#ifndef SIGTTOU
#define SIGTTOU 22
#endif

char input_buffer[SHELL_BUFFER_SIZE];
char* argv_buffer[SHELL_MAX_ARGS];
int shell_running = 0;

static char token_buffer[SHELL_BUFFER_SIZE];
static int shell_status = 0;
static volatile sig_atomic_t shell_terminate_requested = 0;
static volatile sig_atomic_t shell_terminate_signal = 0;

#define SHELL_SCRIPT_ARG_MAX 10
#define SHELL_SCRIPT_STACK_MAX 4

typedef struct shell_script_frame {
    const char* name;
    int argc;
    const char* argv[SHELL_SCRIPT_ARG_MAX];
} shell_script_frame_t;

static shell_script_frame_t script_frames[SHELL_SCRIPT_STACK_MAX];
static int script_frame_depth = 1;

static void shell_term_handler(int sig);

int shell_termination_requested(void)
{
    return shell_terminate_requested != 0;
}

static int shell_is_login_shell(void)
{
    /*
     * The boot shell is created directly by init. Letting it exit leaves the
     * system without an interactive user session, so only child/nested shells
     * are allowed to terminate via the exit builtin.
     */
    return getppid() == 1;
}

static int shell_is_signal_protected(void)
{
    const char *value = shell_getenv("MASH_PROTECT");

    return shell_is_login_shell() ||
           (value && *value && strcmp(value, "0") != 0);
}

static int shell_should_print_banner(void)
{
    const char *value = shell_getenv("MASH_BANNER");

    if (value && *value)
        return strcmp(value, "0") != 0;

    return shell_is_login_shell();
}

static void shell_install_signal_handlers(void)
{
    if (shell_is_signal_protected()) {
        signal(SIGINT, SIG_IGN);
        signal(SIGTSTP, SIG_IGN);
        signal(SIGTTIN, SIG_IGN);
        signal(SIGTTOU, SIG_IGN);
    } else {
        /*
         * A nested interactive shell is just a foreground command. It must not
         * inherit the login shell protection, otherwise Ctrl-C cannot kill a
         * user-started /sbin/mash.
         */
        signal(SIGINT, SIG_DFL);
        signal(SIGTSTP, SIG_DFL);
        signal(SIGTTIN, SIG_DFL);
        signal(SIGTTOU, SIG_DFL);
    }

    /*
     * Background jobs are reaped explicitly with waitpid(WNOHANG). Keeping the
     * default SIGCHLD action avoids injecting a useless user signal frame into
     * the interactive shell for every short-lived background child.
     */
    signal(SIGCHLD, SIG_DFL);
    signal(SIGHUP, shell_term_handler);
    signal(SIGTERM, shell_term_handler);
}

static void shell_restore_child_signal_handlers(void)
{
    signal(SIGHUP, SIG_DFL);
    signal(SIGINT, SIG_DFL);
    signal(SIGTERM, SIG_DFL);
    signal(SIGTSTP, SIG_DFL);
    signal(SIGTTIN, SIG_DFL);
    signal(SIGTTOU, SIG_DFL);
}

#define SHELL_ENV_NAME_LEN  32
typedef struct shell_env {
    char* name;
    char* value;
} shell_env_t;

static shell_env_t* shell_env = NULL;
static int shell_env_count = 0;
static int shell_env_capacity = 0;
static int shell_pgid = 0;

static void shell_term_handler(int sig)
{
    shell_terminate_signal = sig;
    shell_terminate_requested = 1;
}

static int shell_trace_enabled(void)
{
    const char* value = shell_getenv("MASH_TRACE");
    return value && *value && strcmp(value, "0") != 0;
}

#define SHELL_TRACE(fmt, ...) do { \
    if (shell_trace_enabled()) { \
        fprintf(stderr, "[MASH] " fmt "\n", ##__VA_ARGS__); \
        fflush(stderr); \
    } \
} while (0)

static void shell_set_foreground_pgid(int pgid) {
    if (pgid >= 0)
        tcsetpgrp(STDIN_FILENO, pgid);
}

static int shell_set_tty_mode(int action)
{
    struct termios tio;

    if (tcgetattr(STDIN_FILENO, &tio) < 0)
        return -1;

    tio.c_iflag |= ICRNL;
    tio.c_oflag |= OPOST | ONLCR;

    /*
     * mash owns line editing/history/completion in userland. Keep ECHO off so
     * escape sequences and editing keys are consumed by shell_read_line(), not
     * by the terminal line discipline. ICANON remains set for a POSIX-ish
     * default, but the kernel only performs canonical editing when ECHO is on.
     */
    tio.c_lflag |= ICANON | ISIG | ECHOE | ECHOK | ECHOCTL | ECHOKE;
    tio.c_lflag &= ~ECHO;
    tio.c_cc[VMIN] = 1;
    tio.c_cc[VTIME] = 0;
    tio.c_cc[VINTR] = 0x03;
    tio.c_cc[VERASE] = 0x7F;
    tio.c_cc[VKILL] = 0x15;
    tio.c_cc[VEOF] = 0x04;
    tio.c_cc[VSUSP] = 0x1A;

    return tcsetattr(STDIN_FILENO, action, &tio);
}

void shell_restore_tty_mode(void)
{
    (void)shell_set_tty_mode(TCSAFLUSH);
}

int shell_repair_tty_mode(void)
{
    /*
     * A foreground shell can recover after a misbehaving/background client
     * left the shared PTY in polling mode. Do not flush here: a key may arrive
     * between the zero-length read and this repair.
     */
    return shell_set_tty_mode(TCSANOW);
}

static int shell_prepare_child_pgid(int child_pid)
{
    if (child_pid <= 0)
        return -1;

    errno = 0;
    if (setpgid(child_pid, child_pid) == 0)
        return 0;

    /*
     * With a preemptive kernel, a very short-lived child can exit before the
     * parent gets scheduled again. That is not fatal: waitpid will reap it and
     * there is no foreground process group left to hand the terminal to.
     */
    if (errno == ESRCH)
        return 1;

    return -1;
}

static void shell_child_wait_for_parent(int sync_fd)
{
    char byte;

    if (sync_fd < 0)
        return;

    /*
     * Foreground children must not reach their first terminal read before the
     * parent has moved their process group to the foreground. The parent closes
     * the write end after setpgid()+tcsetpgrp(); EOF is the release signal.
     */
    while (read(sync_fd, &byte, 1) < 0 &&
           errno == EINTR &&
           !shell_termination_requested()) {
    }

    close(sync_fd);
}

static int shell_wait_foreground_child(int child_pid, int* status)
{
    int waited;

    do {
        waited = waitpid(child_pid, status, WUNTRACED);
    } while (waited < 0 && errno == EINTR && !shell_termination_requested());

    return waited;
}

static int shell_status_from_wait_status(int status)
{
    if (WIFEXITED(status))
        return WEXITSTATUS(status);
    if (WIFSIGNALED(status))
        return 128 + WTERMSIG(status);
    if (WIFSTOPPED(status) && WSTOPSIG(status) != 0)
        return 128 + WSTOPSIG(status);

    if (status < 0)
        return SHELL_ERROR;

    return status & 0xff;
}

static void shell_restore_foreground(void) {
    shell_set_foreground_pgid(shell_pgid);
    shell_restore_tty_mode();
}

typedef struct shell_redirs {
    const char* input;
    const char* output;
    const char* error;
    int output_append;
    int error_append;
    int stderr_to_stdout;
} shell_redirs_t;

#define SHELL_MAX_PIPELINE 8

typedef struct shell_command {
    int argc;
    char** argv;
    shell_redirs_t redirs;
} shell_command_t;

static int token_is_special(char c) {
    return c == '<' || c == '>' || c == '&' || c == '|' || c == ';';
}

static int shell_backslash_escapes(char quote, char next) {
    if (!next)
        return 0;
    if (quote == '\'')
        return 0;
    if (quote == '"')
        return next == '"' || next == '\\' || next == '$';
    return 1;
}

static int token_has_slash(const char* s) {
    while (*s) {
        if (*s == '/')
            return 1;
        s++;
    }
    return 0;
}

static const char* shell_last_char(const char* s, char c);

static int shell_var_start(char c) {
    return (c >= 'A' && c <= 'Z') ||
           (c >= 'a' && c <= 'z') ||
           c == '_';
}

static int shell_var_char(char c) {
    return shell_var_start(c) || (c >= '0' && c <= '9');
}

static char* shell_strdup(const char* s)
{
    size_t len;
    char* copy;

    if (!s)
        s = "";

    len = strlen(s) + 1;
    copy = malloc(len);
    if (!copy)
        return NULL;

    memcpy(copy, s, len);
    return copy;
}

static int shell_reserve_env(int needed)
{
    shell_env_t* new_env;
    int new_capacity;

    if (needed <= shell_env_capacity)
        return 0;

    new_capacity = shell_env_capacity ? shell_env_capacity * 2 : 16;
    while (new_capacity < needed)
        new_capacity *= 2;

    new_env = realloc(shell_env, new_capacity * sizeof(shell_env[0]));
    if (!new_env)
        return -1;

    for (int i = shell_env_capacity; i < new_capacity; i++) {
        new_env[i].name = NULL;
        new_env[i].value = NULL;
    }

    shell_env = new_env;
    shell_env_capacity = new_capacity;
    return 0;
}

const char* shell_getenv(const char* name) {
    int i;

    for (i = 0; i < shell_env_count; i++) {
        if (strcmp(shell_env[i].name, name) == 0)
            return shell_env[i].value;
    }

    return NULL;
}

int shell_setenv(const char* name, const char* value) {
    int i;
    char* value_copy;

    if (!name || !value || !*name)
        return -1;

    for (i = 0; i < shell_env_count; i++) {
        if (strcmp(shell_env[i].name, name) == 0) {
            value_copy = shell_strdup(value);
            if (!value_copy)
                return -1;
            free(shell_env[i].value);
            shell_env[i].value = value_copy;
            return 0;
        }
    }

    if (shell_reserve_env(shell_env_count + 1) < 0)
        return -1;

    shell_env[shell_env_count].name = shell_strdup(name);
    shell_env[shell_env_count].value = shell_strdup(value);
    if (!shell_env[shell_env_count].name || !shell_env[shell_env_count].value) {
        free(shell_env[shell_env_count].name);
        free(shell_env[shell_env_count].value);
        shell_env[shell_env_count].name = NULL;
        shell_env[shell_env_count].value = NULL;
        return -1;
    }

    shell_env_count++;
    return 0;
}

int shell_unsetenv(const char* name) {
    int i;

    if (!name || !*name)
        return -1;

    for (i = 0; i < shell_env_count; i++) {
        if (strcmp(shell_env[i].name, name) == 0) {
            int j;

            free(shell_env[i].name);
            free(shell_env[i].value);
            for (j = i; j < shell_env_count - 1; j++)
                shell_env[j] = shell_env[j + 1];
            shell_env_count--;
            shell_env[shell_env_count].name = NULL;
            shell_env[shell_env_count].value = NULL;
            return 0;
        }
    }

    return 0;
}

int shell_env_count_registered(void) {
    return shell_env_count;
}

const char* shell_env_name_at(int index) {
    if (index < 0 || index >= shell_env_count)
        return NULL;
    return shell_env[index].name;
}

const char* shell_env_value_at(int index) {
    if (index < 0 || index >= shell_env_count)
        return NULL;
    return shell_env[index].value;
}

int shell_last_status(void) {
    return shell_status;
}

int shell_push_script_args(const char* name, int argc, char* argv[]) {
    int i;
    shell_script_frame_t* frame;

    if (script_frame_depth >= SHELL_SCRIPT_STACK_MAX)
        return -1;

    frame = &script_frames[script_frame_depth++];
    frame->name = name ? name : "";
    frame->argc = argc;
    if (frame->argc > SHELL_SCRIPT_ARG_MAX - 1)
        frame->argc = SHELL_SCRIPT_ARG_MAX - 1;

    for (i = 0; i < frame->argc; i++)
        frame->argv[i] = argv[i];
    for (; i < SHELL_SCRIPT_ARG_MAX; i++)
        frame->argv[i] = NULL;

    return 0;
}

void shell_pop_script_args(void) {
    if (script_frame_depth > 1)
        script_frame_depth--;
}

static const char* shell_script_arg_value(int index) {
    shell_script_frame_t* frame;

    if (script_frame_depth <= 0)
        return "";

    frame = &script_frames[script_frame_depth - 1];
    if (index == 0)
        return frame->name ? frame->name : "";
    if (index < 0 || index > frame->argc)
        return "";

    return frame->argv[index - 1] ? frame->argv[index - 1] : "";
}

static char** shell_build_envp(void) {
    static char* empty_envp[] = { NULL };
    char** envp;
    int i;

    envp = malloc((shell_env_count + 1) * sizeof(envp[0]));
    if (!envp)
        return empty_envp;

    for (i = 0; i < shell_env_count; i++) {
        size_t name_len = strlen(shell_env[i].name);
        size_t value_len = strlen(shell_env[i].value);
        char* entry = malloc(name_len + value_len + 2);

        if (!entry) {
            for (int j = 0; j < i; j++)
                free(envp[j]);
            free(envp);
            return empty_envp;
        }

        memcpy(entry, shell_env[i].name, name_len);
        entry[name_len] = '=';
        memcpy(entry + name_len + 1, shell_env[i].value, value_len + 1);
        envp[i] = entry;
    }
    envp[shell_env_count] = NULL;
    return envp;
}

static void shell_import_environ(char **envp)
{
    if (!envp)
        return;

    for (char **p = envp; *p; p++) {
        char *eq = strchr(*p, '=');
        char name[SHELL_ENV_NAME_LEN];
        size_t name_len;

        if (!eq || eq == *p)
            continue;

        name_len = (size_t)(eq - *p);
        if (name_len >= sizeof(name))
            name_len = sizeof(name) - 1;

        memcpy(name, *p, name_len);
        name[name_len] = '\0';
        shell_setenv(name, eq + 1);
    }
}

static void shell_set_default_env(const char *name, const char *value)
{
    if (!shell_getenv(name))
        shell_setenv(name, value);
}

static void shell_init_env(char **envp) {
    script_frames[0].name = "mash";
    script_frames[0].argc = 0;
    shell_import_environ(envp);
    shell_set_default_env("PATH", "/bin:/usr/bin:/opt/kilo/bin");
    shell_set_default_env("HOME", "/home/user");
    shell_set_default_env("USER", "user");
    shell_set_default_env("PWD", "/");
    shell_set_default_env("PS1", "mash$> ");
}

static char* trim_spaces(char* s) {
    char* end;

    while (*s == ' ' || *s == '\t')
        s++;

    end = s + strlen(s);
    while (end > s && (end[-1] == ' ' || end[-1] == '\t' ||
                       end[-1] == '\r' || end[-1] == '\n')) {
        *--end = '\0';
    }

    return s;
}

static int starts_with(const char* s, const char* prefix) {
    while (*prefix) {
        if (*s++ != *prefix++)
            return 0;
    }
    return 1;
}

static int shell_ifs_space(char c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

static int shell_is_valid_name(const char* name) {
    if (!name || !shell_var_start(*name))
        return 0;

    name++;
    while (*name) {
        if (!shell_var_char(*name))
            return 0;
        name++;
    }

    return 1;
}

static int shell_apply_assignment(const char* token) {
    char name[SHELL_ENV_NAME_LEN];
    const char* eq;
    size_t name_len;

    eq = strchr(token, '=');
    if (!eq)
        return -1;

    name_len = (size_t)(eq - token);
    if (name_len == 0 || name_len >= sizeof(name))
        return -1;

    memcpy(name, token, name_len);
    name[name_len] = '\0';

    if (!shell_is_valid_name(name))
        return -1;

    return shell_setenv(name, eq + 1);
}

static void shell_apply_rc_line(char* line) {
    char* name;
    char* value;
    char* eq;

    line = trim_spaces(line);
    if (!*line || *line == '#')
        return;

    if (starts_with(line, "export "))
        line = trim_spaces(line + 7);

    eq = strchr(line, '=');
    if (!eq)
        return;

    *eq = '\0';
    name = trim_spaces(line);
    value = trim_spaces(eq + 1);

    if (*value && (*value == '"' || *value == '\'') && value[strlen(value) - 1] == *value) {
        value[strlen(value) - 1] = '\0';
        value++;
    }

    if (!shell_is_valid_name(name)) {
        printf("mash: rc: invalid variable name '%s'\n", name);
        return;
    }

    if (shell_setenv(name, value) < 0)
        printf("mash: rc: cannot set '%s'\n", name);
}

static void shell_load_rc_file(const char* path) {
    char buffer[128];
    char line[1024];
    int fd;
    int n;
    int line_len = 0;
    int line_no = 1;
    int skipping_long_line = 0;

    fd = open(path, O_RDONLY, 0);
    if (fd < 0)
        return;

    while ((n = read(fd, buffer, sizeof(buffer))) > 0) {
        for (int i = 0; i < n; i++) {
            char c = buffer[i];

            if (skipping_long_line) {
                if (c == '\n') {
                    skipping_long_line = 0;
                    line_len = 0;
                    line_no++;
                }
                continue;
            }

            if (c == '\n') {
                line[line_len] = '\0';
                shell_apply_rc_line(line);
                line_len = 0;
                line_no++;
                continue;
            }

            if (line_len >= (int)sizeof(line) - 1) {
                printf("mash: rc: %s:%d: line too long, ignored\n",
                       path, line_no);
                skipping_long_line = 1;
                line_len = 0;
                continue;
            }

            line[line_len++] = c;
        }
    }
    close(fd);

    if (!skipping_long_line && line_len > 0) {
        line[line_len] = '\0';
        shell_apply_rc_line(line);
    }
}

static void shell_load_startup_files(void) {
    const char *home = shell_getenv("HOME");
    char path[192];

    if (!home || !*home)
        home = "/home/user";

    snprintf(path, sizeof(path), "%s/.mashrc", home);
    shell_load_rc_file(path);
}

static int build_exec_path_from_dir(const char* dir, int dir_len,
                                    const char* name, char* out, size_t out_size) {
    size_t pos = 0;

    if (token_has_slash(name)) {
        strncpy(out, name, out_size - 1);
        out[out_size - 1] = '\0';
        return 0;
    }

    if (dir_len <= 0) {
        if (out_size < 3)
            return -1;
        out[pos++] = '.';
    } else {
        if ((size_t)dir_len >= out_size)
            return -1;
        memcpy(out, dir, dir_len);
        pos = dir_len;
    }

    if (pos > 0 && out[pos - 1] != '/') {
        if (pos + 1 >= out_size)
            return -1;
        out[pos++] = '/';
    }

    if (pos + strlen(name) >= out_size)
        return -1;

    strcpy(out + pos, name);
    return 0;
}

static int append_path_component(char* out, size_t out_size,
                                 const char* component, size_t len) {
    size_t pos;

    if (len == 0 || (len == 1 && component[0] == '.'))
        return 0;

    if (len == 2 && component[0] == '.' && component[1] == '.') {
        char* slash;

        pos = strlen(out);
        if (pos <= 1) {
            out[0] = '/';
            out[1] = '\0';
            return 0;
        }

        slash = strrchr(out, '/');
        if (!slash || slash == out)
            out[1] = '\0';
        else
            *slash = '\0';
        return 0;
    }

    pos = strlen(out);
    if (pos > 1) {
        if (pos + 1 >= out_size)
            return -1;
        out[pos++] = '/';
        out[pos] = '\0';
    }

    if (pos + len >= out_size)
        return -1;

    memcpy(out + pos, component, len);
    out[pos + len] = '\0';
    return 0;
}

static int normalize_absolute_path(const char* path, char* out, size_t out_size) {
    const char* p = path;

    if (!path || path[0] != '/' || out_size < 2)
        return -1;

    out[0] = '/';
    out[1] = '\0';

    while (*p) {
        const char* start;
        size_t len;

        while (*p == '/')
            p++;
        start = p;
        while (*p && *p != '/')
            p++;

        len = (size_t)(p - start);
        if (append_path_component(out, out_size, start, len) < 0)
            return -1;
    }

    return 0;
}

static int resolve_explicit_exec_path(const char* name, char* out, size_t out_size) {
    char combined[256];
    char cwd[256];

    if (!name || !out || out_size == 0)
        return -1;

    if (name[0] == '/')
        return normalize_absolute_path(name, out, out_size);

    if (!getcwd(cwd, sizeof(cwd)))
        return -1;

    if (strcmp(cwd, "/") == 0) {
        if (snprintf(combined, sizeof(combined), "/%s", name) >= (int)sizeof(combined))
            return -1;
    } else {
        if (snprintf(combined, sizeof(combined), "%s/%s", cwd, name) >= (int)sizeof(combined))
            return -1;
    }

    return normalize_absolute_path(combined, out, out_size);
}

static int shell_file_looks_text(const char* path) {
    unsigned char buf[64];
    int fd;
    int n;
    int i;

    fd = open(path, O_RDONLY, 0);
    if (fd < 0)
        return 0;

    n = read(fd, buf, sizeof(buf));
    close(fd);

    if (n < 0)
        return 0;
    if (n == 0)
        return 1;
    if (n >= 4 && buf[0] == 0x7f && buf[1] == 'E' &&
        buf[2] == 'L' && buf[3] == 'F')
        return 0;

    for (i = 0; i < n; i++) {
        if (buf[i] == 0)
            return 0;
    }

    return 1;
}

static void exec_script_or_die_if_text(const char* path, int argc, char* argv[]) {
    if (access(path, 0) == 0 && access(path, X_OK) < 0) {
        printf("mash: permission denied: %s\n", path);
        exit(126);
    }

    if (errno == ENOEXEC || shell_file_looks_text(path)) {
        int status = shell_source_file(path, argc - 1, &argv[1]);

        if (status == SHELL_EXIT)
            status = 0;
        exit(status);
    }
}

static int parse_redirections(int* argc, char* argv[], shell_redirs_t* redirs) {
    int src = 0;
    int dst = 0;

    redirs->input = NULL;
    redirs->output = NULL;
    redirs->error = NULL;
    redirs->output_append = 0;
    redirs->error_append = 0;
    redirs->stderr_to_stdout = 0;

    while (src < *argc) {
        int target_fd = -1;
        int op_index = src;
        const char* op = argv[src];

        if (strcmp(argv[src], "2>&1") == 0) {
            redirs->error = NULL;
            redirs->error_append = 0;
            redirs->stderr_to_stdout = 1;
            src++;
            continue;
        }

        if ((strcmp(argv[src], "1") == 0 || strcmp(argv[src], "2") == 0) &&
            src + 1 < *argc &&
            (strcmp(argv[src + 1], ">") == 0 || strcmp(argv[src + 1], ">>") == 0)) {
            target_fd = argv[src][0] - '0';
            op_index = src + 1;
            op = argv[op_index];
        } else if ((strcmp(argv[src], "1>") == 0 || strcmp(argv[src], "1>>") == 0 ||
                    strcmp(argv[src], "2>") == 0 || strcmp(argv[src], "2>>") == 0)) {
            target_fd = argv[src][0] - '0';
            op = argv[src] + 1;
        }

        if (strcmp(argv[src], "<") == 0 ||
            strcmp(argv[src], ">") == 0 ||
            strcmp(argv[src], ">>") == 0 ||
            target_fd >= 0) {
            int is_input = strcmp(op, "<") == 0;
            int is_append = strcmp(op, ">>") == 0;
            int target_index = op_index + 1;

            if (!is_input && target_fd == STDERR_FILENO &&
                target_index + 1 < *argc &&
                strcmp(argv[target_index], "&") == 0 &&
                strcmp(argv[target_index + 1], "1") == 0) {
                redirs->error = NULL;
                redirs->error_append = 0;
                redirs->stderr_to_stdout = 1;
                src = target_index + 2;
                continue;
            }

            if (target_index >= *argc) {
                printf("mash: missing redirection target after %s\n", argv[op_index]);
                return -1;
            }

            if (is_input) {
                redirs->input = argv[target_index];
            } else if (target_fd == 2) {
                redirs->error = argv[target_index];
                redirs->error_append = is_append;
                redirs->stderr_to_stdout = 0;
            } else {
                redirs->output = argv[target_index];
                redirs->output_append = is_append;
            }
            src = target_index + 1;
            continue;
        }

        argv[dst++] = argv[src++];
    }

    argv[dst] = NULL;
    *argc = dst;
    return 0;
}

static int open_redirect_output(const char* path, int append) {
    int flags = O_CREAT | O_WRONLY;

    flags |= append ? O_APPEND : O_TRUNC;
    return open(path, flags, 0644);
}

static int apply_redirections(const shell_redirs_t* redirs) {
    int fd;

    if (redirs->input) {
        fd = open(redirs->input, O_RDONLY, 0);
        if (fd < 0) {
            printf("mash: cannot open input %s\n", redirs->input);
            return -1;
        }
        if (dup2(fd, STDIN_FILENO) < 0) {
            close(fd);
            printf("mash: cannot redirect stdin\n");
            return -1;
        }
        close(fd);
    }

    if (redirs->output) {
        fd = open_redirect_output(redirs->output, redirs->output_append);
        if (fd < 0) {
            printf("mash: cannot open output %s\n", redirs->output);
            return -1;
        }
        if (dup2(fd, STDOUT_FILENO) < 0) {
            close(fd);
            printf("mash: cannot redirect stdout\n");
            return -1;
        }
        close(fd);
    }

    if (redirs->error) {
        fd = open_redirect_output(redirs->error, redirs->error_append);
        if (fd < 0) {
            printf("mash: cannot open stderr %s\n", redirs->error);
            return -1;
        }
        if (dup2(fd, STDERR_FILENO) < 0) {
            close(fd);
            printf("mash: cannot redirect stderr\n");
            return -1;
        }
        close(fd);
    }

    if (redirs->stderr_to_stdout) {
        if (dup2(STDOUT_FILENO, STDERR_FILENO) < 0) {
            printf("mash: cannot redirect stderr to stdout\n");
            return -1;
        }
    }

    return 0;
}

static int argv_has_pipeline(int argc, char* argv[]) {
    int i;

    for (i = 0; i < argc; i++) {
        if (strcmp(argv[i], "|") == 0)
            return 1;
    }

    return 0;
}

static void exec_external_or_die(int argc, char* argv[]) {
    char* exec_argv[SHELL_MAX_ARGS];
    char** envp = shell_build_envp();
    char cmd[256];
    const char* path;
    const char* entry;
    int i;

    for (i = 1; i < argc && i < SHELL_MAX_ARGS - 1; i++)
        exec_argv[i] = argv[i];
    exec_argv[i] = NULL;

    if (token_has_slash(argv[0])) {
        if (resolve_explicit_exec_path(argv[0], cmd, sizeof(cmd)) < 0) {
            printf("mash: command path too long: %s\n", argv[0]);
            exit(126);
        }
        exec_argv[0] = cmd;
        execve(cmd, exec_argv, envp);
        exec_script_or_die_if_text(cmd, argc, argv);
    } else {
        path = shell_getenv("PATH");
        if (!path || !*path)
            path = "/bin:/usr/bin:/opt/kilo/bin";

        entry = path;
        while (*entry) {
            const char* next = strchr(entry, ':');
            int len = next ? (int)(next - entry) : (int)strlen(entry);

            if (build_exec_path_from_dir(entry, len, argv[0], cmd, sizeof(cmd)) == 0) {
                exec_argv[0] = cmd;
                execve(cmd, exec_argv, envp);
                exec_script_or_die_if_text(cmd, argc, argv);
            }

            if (!next)
                break;
            entry = next + 1;
        }
    }

    printf("mash: command not found: %s\n", argv[0]);
    exit(127);
}

static int external_command_exists(const char* name) {
    char cmd[256];
    const char* path;
    const char* entry;

    if (token_has_slash(name)) {
        if (resolve_explicit_exec_path(name, cmd, sizeof(cmd)) < 0)
            return 0;
        return access(cmd, 0) == 0;
    }

    path = shell_getenv("PATH");
    if (!path || !*path)
        path = "/bin:/usr/bin:/opt/kilo/bin";

    entry = path;
    while (*entry) {
        const char* next = strchr(entry, ':');
        int len = next ? (int)(next - entry) : (int)strlen(entry);

        if (build_exec_path_from_dir(entry, len, name, cmd, sizeof(cmd)) == 0 &&
            access(cmd, 0) == 0)
            return 1;

        if (!next)
            break;
        entry = next + 1;
    }

    return 0;
}

static int split_pipeline(int argc, char* argv[], shell_command_t commands[], int* command_count) {
    int start = 0;
    int count = 0;
    int i;

    for (i = 0; i <= argc; i++) {
        if (i == argc || strcmp(argv[i], "|") == 0) {
            int segment_argc = i - start;

            if (segment_argc == 0) {
                printf("mash: empty command in pipeline\n");
                return -1;
            }

            if (count >= SHELL_MAX_PIPELINE) {
                printf("mash: pipeline too long\n");
                return -1;
            }

            argv[i] = NULL;
            commands[count].argc = segment_argc;
            commands[count].argv = &argv[start];

            if (parse_redirections(&commands[count].argc,
                                   commands[count].argv,
                                   &commands[count].redirs) < 0) {
                return -1;
            }

            if (commands[count].argc == 0) {
                printf("mash: empty command in pipeline\n");
                return -1;
            }

            count++;
            start = i + 1;
        }
    }

    for (i = 0; i < count; i++) {
        if (i > 0 && commands[i].redirs.input) {
            printf("mash: input redirection is only supported on the first pipeline command\n");
            return -1;
        }
        if (i < count - 1 && commands[i].redirs.output) {
            printf("mash: output redirection is only supported on the last pipeline command\n");
            return -1;
        }
    }

    *command_count = count;
    return 0;
}

static int validate_pipeline_commands(shell_command_t commands[], int command_count) {
    int i;

    for (i = 0; i < command_count; i++) {
        const char* name = commands[i].argv[0];

        if (strcmp(name, "exit") == 0 || find_command(name))
            continue;

        if (external_command_exists(name))
            continue;

        printf("mash: command not found: %s\n", name);
        return 127;
    }

    return 0;
}

static void close_pipeline_fds(int pipes[][2], int pipe_count) {
    int i;

    for (i = 0; i < pipe_count; i++) {
        if (pipes[i][0] >= 0)
            close(pipes[i][0]);
        if (pipes[i][1] >= 0)
            close(pipes[i][1]);
    }
}

static void remember_stopped_job(int pid, int pgid, const char* command, int status)
{
    jobs_add(pid, pgid, command);
    jobs_note_status(pid, status);
    jobs_print_stopped(pgid);
}

static int pipeline_pid_index(const int pids[], int count, int pid)
{
    for (int i = 0; i < count; i++) {
        if (pids[i] == pid)
            return i;
    }
    return -1;
}

static int pipeline_has_stopped_member(const int seen[], const int statuses[], int count)
{
    for (int i = 0; i < count; i++) {
        if (seen[i] && WIFSTOPPED(statuses[i]) && WSTOPSIG(statuses[i]) != 0)
            return 1;
    }
    return 0;
}

static int run_pipeline(int argc, char* argv[], int background) {
    shell_command_t commands[SHELL_MAX_PIPELINE];
    int pipes[SHELL_MAX_PIPELINE - 1][2];
    int pids[SHELL_MAX_PIPELINE];
    int wait_statuses[SHELL_MAX_PIPELINE];
    int wait_seen[SHELL_MAX_PIPELINE];
    int command_count = 0;
    int pipe_count;
    int i;
    int status = 0;
    int last_status = 0;
    int launched = 0;
    int pgid = 0;
    int reported = 0;
    int sync_pipe[2] = { -1, -1 };
    char command[JOBS_COMMAND_LEN];

    for (i = 0; i < SHELL_MAX_PIPELINE - 1; i++) {
        pipes[i][0] = -1;
        pipes[i][1] = -1;
    }
    for (i = 0; i < SHELL_MAX_PIPELINE; i++) {
        wait_statuses[i] = 0;
        wait_seen[i] = 0;
    }

    jobs_build_command(argc, argv, command, sizeof(command));

    if (split_pipeline(argc, argv, commands, &command_count) < 0)
        return SHELL_ERROR;

    {
        int validate_status = validate_pipeline_commands(commands, command_count);
        if (validate_status != 0)
            return validate_status;
    }

    pipe_count = command_count - 1;
    for (i = 0; i < pipe_count; i++) {
        if (pipe(pipes[i]) < 0) {
            printf("mash: pipe failed\n");
            close_pipeline_fds(pipes, pipe_count);
            return SHELL_ERROR;
        }
    }

    if (!background && pipe(sync_pipe) < 0) {
        printf("mash: foreground sync pipe failed\n");
        close_pipeline_fds(pipes, pipe_count);
        return SHELL_ERROR;
    }

    for (i = 0; i < command_count; i++) {
        int pid = fork();

        if (pid < 0) {
            printf("mash: fork failed\n");
            close_pipeline_fds(pipes, pipe_count);
            if (sync_pipe[0] >= 0)
                close(sync_pipe[0]);
            if (sync_pipe[1] >= 0)
                close(sync_pipe[1]);
            for (i = 0; i < launched; i++)
                waitpid(pids[i], &status, 0);
            return SHELL_ERROR;
        }

        if (pid == 0) {
            command_entry_t *entry;

            if (i == 0)
                setpgid(0, 0);
            else if (pgid > 0)
                setpgid(0, pgid);

            shell_restore_child_signal_handlers();
            if (!background) {
                close(sync_pipe[1]);
                shell_child_wait_for_parent(sync_pipe[0]);
            }

            if (i > 0 && dup2(pipes[i - 1][0], STDIN_FILENO) < 0) {
                printf("mash: cannot connect pipeline input\n");
                exit(-1);
            }

            if (i < command_count - 1 && dup2(pipes[i][1], STDOUT_FILENO) < 0) {
                printf("mash: cannot connect pipeline output\n");
                exit(-1);
            }

            close_pipeline_fds(pipes, pipe_count);

            if (apply_redirections(&commands[i].redirs) < 0)
                exit(-1);

            if (strcmp(commands[i].argv[0], "exit") == 0)
                exit(0);

            entry = find_command(commands[i].argv[0]);
            if (entry) {
                int result = entry->function(commands[i].argc, commands[i].argv);
                pflush();
                exit(result);
            }

            exec_external_or_die(commands[i].argc, commands[i].argv);
        }

        pids[i] = pid;
        if (i == 0) {
            pgid = pid;
            shell_prepare_child_pgid(pid);
        } else {
            errno = 0;
            if (setpgid(pid, pgid) < 0 && errno != ESRCH)
                printf("mash: setpgid failed for pid %d\n", pid);
        }
        launched++;
    }

    close_pipeline_fds(pipes, pipe_count);
    if (background) {
        printf("[bg]");
        for (i = 0; i < command_count; i++) {
            jobs_add(pids[i], pgid, command);
            printf(" pid %d", pids[i]);
        }
        printf("\n");
        return SHELL_OK;
    }

    if (sync_pipe[0] >= 0)
        close(sync_pipe[0]);
    if (pgid > 0)
        shell_set_foreground_pgid(pgid);
    if (sync_pipe[1] >= 0)
        close(sync_pipe[1]);
    while (reported < command_count) {
        int waited = waitpid(-pgid, &status, WUNTRACED);
        int idx;

        if (waited < 0 && errno == EINTR && !shell_termination_requested())
            continue;
        if (waited <= 0)
            break;

        idx = pipeline_pid_index(pids, command_count, waited);
        if (idx < 0)
            continue;

        wait_statuses[idx] = status;
        if (!wait_seen[idx]) {
            wait_seen[idx] = 1;
            reported++;
        }
        last_status = shell_status_from_wait_status(status);
    }
    shell_restore_foreground();

    if (pipeline_has_stopped_member(wait_seen, wait_statuses, command_count)) {
        for (i = 0; i < command_count; i++)
            jobs_add(pids[i], pgid, command);
        for (i = 0; i < command_count; i++) {
            if (wait_seen[i])
                jobs_note_status(pids[i], wait_statuses[i]);
        }
        jobs_print_stopped(pgid);
    }

    return last_status;
}

static int run_builtin_with_redirs(command_entry_t* entry, int argc, char* argv[],
                                  const shell_redirs_t* redirs) {
    int saved_stdin = -1;
    int saved_stdout = -1;
    int saved_stderr = -1;
    int result;

    if (redirs->input) {
        saved_stdin = dup(STDIN_FILENO);
        if (saved_stdin < 0)
            return SHELL_ERROR;
    }

    if (redirs->output) {
        saved_stdout = dup(STDOUT_FILENO);
        if (saved_stdout < 0) {
            if (saved_stdin >= 0)
                close(saved_stdin);
            return SHELL_ERROR;
        }
    }

    if (redirs->error || redirs->stderr_to_stdout) {
        saved_stderr = dup(STDERR_FILENO);
        if (saved_stderr < 0) {
            if (saved_stdin >= 0)
                close(saved_stdin);
            if (saved_stdout >= 0)
                close(saved_stdout);
            return SHELL_ERROR;
        }
    }

    if (apply_redirections(redirs) < 0) {
        if (saved_stdin >= 0) {
            dup2(saved_stdin, STDIN_FILENO);
            close(saved_stdin);
        }
        if (saved_stdout >= 0) {
            dup2(saved_stdout, STDOUT_FILENO);
            close(saved_stdout);
        }
        if (saved_stderr >= 0) {
            dup2(saved_stderr, STDERR_FILENO);
            close(saved_stderr);
        }
        return SHELL_ERROR;
    }

    result = entry->function(argc, argv);
    pflush();

    if (saved_stdin >= 0) {
        dup2(saved_stdin, STDIN_FILENO);
        close(saved_stdin);
    }
    if (saved_stdout >= 0) {
        dup2(saved_stdout, STDOUT_FILENO);
        close(saved_stdout);
    }
    if (saved_stderr >= 0) {
        dup2(saved_stderr, STDERR_FILENO);
        close(saved_stderr);
    }

    return result;
}

// Display the shell banner
void shell_print_banner(void) {
    const char* path = shell_getenv("PATH");

    printf("\n");
    printf("\033[1;36m");
    printf("  __  __    _    ____  _   _\n");
    printf(" |  \\/  |  / \\  / ___|| | | |\n");
    printf(" | |\\/| | / _ \\ \\___ \\| |_| |\n");
    printf(" | |  | |/ ___ \\ ___) |  _  |\n");
    printf(" |_|  |_/_/   \\_\\____/|_| |_|\n");
    printf("\033[0m");
    printf(" arm-os shell on newlib\n");
    printf(" type 'help' for builtins");
    if (path && *path)
        printf(", PATH=%s", path);
    printf("\n");
    printf("\n");
}

// Display the prompt
void shell_print_prompt(void) {
    const char* ps1 = shell_getenv("PS1");
    char status_buf[16];

    if (!ps1)
        ps1 = "mash$> ";

    while (*ps1) {
        if (*ps1 == '\\' && ps1[1] == 'w') {
            char cwd[256];
            const char* pwd = shell_getenv("PWD");

            if (getcwd(cwd, sizeof(cwd)))
                printf("%s", cwd);
            else
                printf("%s", pwd && *pwd ? pwd : "/");
            ps1 += 2;
            continue;
        }

        if (*ps1 == '$') {
            char name[SHELL_ENV_NAME_LEN];
            int len = 0;
            const char* value;

            ps1++;
            if (*ps1 == '?') {
                snprintf(status_buf, sizeof(status_buf), "%d", shell_status);
                printf("%s", status_buf);
                ps1++;
                continue;
            }

            if (!shell_var_start(*ps1)) {
                putchar('$');
                continue;
            }

            while (shell_var_char(*ps1) && len < SHELL_ENV_NAME_LEN - 1)
                name[len++] = *ps1++;
            while (shell_var_char(*ps1))
                ps1++;
            name[len] = '\0';

            value = shell_getenv(name);
            if (value)
                printf("%s", value);
            continue;
        }

        putchar(*ps1++);
    }
    pflush();
}

static int shell_extract_command_substitution(char** readp,
                                              char* command,
                                              size_t command_size)
{
    char* p = *readp + 2;
    char quote = 0;
    int depth = 1;
    size_t used = 0;

    while (*p) {
        if (quote) {
            if (*p == quote) {
                quote = 0;
            } else if (*p == '\\' && shell_backslash_escapes(quote, p[1])) {
                if (used + 2 >= command_size)
                    return -1;
                command[used++] = *p++;
                command[used++] = *p++;
                continue;
            }
        } else {
            if (*p == '\'' || *p == '"') {
                quote = *p;
            } else if (*p == '$' && p[1] == '(') {
                depth++;
                if (used + 2 >= command_size)
                    return -1;
                command[used++] = *p++;
                command[used++] = *p++;
                continue;
            } else if (*p == ')') {
                depth--;
                if (depth == 0) {
                    command[used] = '\0';
                    *readp = p + 1;
                    return 0;
                }
            }
        }

        if (used + 1 >= command_size)
            return -1;
        command[used++] = *p++;
    }

    printf("mash: unmatched command substitution\n");
    return -1;
}

static int shell_capture_command(const char* command, char* out, size_t out_size)
{
    int pipefd[2];
    int pid;
    int status = 0;
    int overflow = 0;
    size_t used = 0;

    if (!out || out_size == 0)
        return -1;
    out[0] = '\0';

    if (pipe(pipefd) < 0) {
        printf("mash: command substitution: pipe failed\n");
        return -1;
    }

    pid = fork();
    if (pid < 0) {
        close(pipefd[0]);
        close(pipefd[1]);
        printf("mash: command substitution: fork failed\n");
        return -1;
    }

    if (pid == 0) {
        char line[SHELL_BUFFER_SIZE];
        int result;

        close(pipefd[0]);
        dup2(pipefd[1], STDOUT_FILENO);
        close(pipefd[1]);
        shell_restore_child_signal_handlers();

        strncpy(line, command, sizeof(line) - 1);
        line[sizeof(line) - 1] = '\0';
        result = shell_execute_line(line);
        if (result == SHELL_EXIT)
            result = 0;
        exit(result);
    }

    close(pipefd[1]);
    for (;;) {
        char buf[128];
        int n = read(pipefd[0], buf, sizeof(buf));

        if (n < 0) {
            if (errno == EINTR && !shell_termination_requested())
                continue;
            break;
        }
        if (n == 0)
            break;

        for (int i = 0; i < n; i++) {
            if (used + 1 < out_size) {
                out[used++] = buf[i];
            } else {
                overflow = 1;
            }
        }
    }
    close(pipefd[0]);

    while (waitpid(pid, &status, 0) < 0 &&
           errno == EINTR &&
           !shell_termination_requested()) {
    }

    if (overflow) {
        printf("mash: command substitution output too long\n");
        return -1;
    }

    while (used > 0 && (out[used - 1] == '\n' || out[used - 1] == '\r'))
        used--;
    out[used] = '\0';
    return shell_status_from_wait_status(status);
}

static int shell_parse_line_into(char* line, char* argv[],
                                 char* tokens, size_t tokens_size) {
    int argc = 0;
    char* readp = line;
    char* writep = tokens;
    char* endp = tokens + tokens_size - 1;

    if (tokens_size < 2) {
        printf("mash: tokenizer buffer too small\n");
        return -1;
    }

    while (*readp && argc < SHELL_MAX_ARGS - 1) {
        while (*readp == ' ' || *readp == '\t') {
            readp++;
        }
        
        if (*readp == '\0') {
            break;
        }

        argv[argc++] = writep;

        if (readp[0] == '2' && readp[1] == '>' &&
            readp[2] == '&' && readp[3] == '1') {
            if (writep + 4 >= endp) {
                printf("mash: token buffer full\n");
                return -1;
            }
            *writep++ = *readp++;
            *writep++ = *readp++;
            *writep++ = *readp++;
            *writep++ = *readp++;
        } else if ((*readp == '&' && readp[1] == '&') ||
            (*readp == '|' && readp[1] == '|') ||
            (*readp == '>' && readp[1] == '>')) {
            if (writep + 2 >= endp) {
                printf("mash: token buffer full\n");
                return -1;
            }
            *writep++ = *readp++;
            *writep++ = *readp++;
        } else if (token_is_special(*readp)) {
            if (writep + 1 >= endp) {
                printf("mash: token buffer full\n");
                return -1;
            }
            *writep++ = *readp++;
        } else {
            char quote = 0;
            char* token_start = writep;

            while (*readp) {
                if (quote) {
                    if (*readp == quote) {
                        quote = 0;
                        readp++;
                        continue;
                    }
                } else {
                    if (*readp == '\'' || *readp == '"') {
                        quote = *readp++;
                        continue;
                    }
                    if (*readp == ' ' || *readp == '\t' || token_is_special(*readp))
                        break;
                }

                if (*readp == '\\' && shell_backslash_escapes(quote, readp[1])) {
                    readp++;
                }
                if (!token_start) {
                    if (argc >= SHELL_MAX_ARGS - 1) {
                        printf("mash: too many arguments\n");
                        return -SHELL_MAX_ARGS;
                    }
                    argv[argc++] = writep;
                    token_start = writep;
                }
                if (!quote && writep == token_start && *readp == '~' &&
                    (readp[1] == '\0' || readp[1] == '/' ||
                     readp[1] == ' ' || readp[1] == '\t' ||
                     token_is_special(readp[1]))) {
                    const char* home = shell_getenv("HOME");

                    if (!home || !*home)
                        home = "/";

                    while (*home && writep < endp)
                        *writep++ = *home++;
                    if (*home) {
                        printf("mash: expansion too long\n");
                        return -1;
                    }
                    readp++;
                    continue;
                }
                if (*readp == '$' && quote != '\'') {
                    char var_name[SHELL_ENV_NAME_LEN];
                    int var_len = 0;
                    const char* value;
                    char status_buf[16];

                    readp++;
                    if (*readp == '(') {
                        char command[SHELL_BUFFER_SIZE];
                        char output[SHELL_BUFFER_SIZE];

                        readp--;
                        if (shell_extract_command_substitution(&readp,
                                                               command,
                                                               sizeof(command)) < 0)
                            return -1;
                        if (shell_capture_command(command, output,
                                                  sizeof(output)) < 0)
                            return -1;
                        value = output;
                        while (*value) {
                            if (!quote && shell_ifs_space(*value)) {
                                if (writep != token_start) {
                                    if (writep >= endp) {
                                        printf("mash: token buffer full\n");
                                        return -1;
                                    }
                                    *writep++ = '\0';
                                    token_start = NULL;
                                }
                                value++;
                                continue;
                            }

                            if (!token_start) {
                                if (argc >= SHELL_MAX_ARGS - 1) {
                                    printf("mash: too many arguments\n");
                                    return -SHELL_MAX_ARGS;
                                }
                                argv[argc++] = writep;
                                token_start = writep;
                            }
                            if (writep >= endp) {
                                printf("mash: expansion too long\n");
                                return -1;
                            }
                            *writep++ = *value++;
                        }
                        continue;
                    }
                    if (*readp == '{') {
                        int basename = 0;

                        readp++;
                        while (shell_var_char(*readp) && var_len < SHELL_ENV_NAME_LEN - 1)
                            var_name[var_len++] = *readp++;
                        while (shell_var_char(*readp))
                            readp++;
                        var_name[var_len] = '\0';

                        if (readp[0] == '#' && readp[1] == '#' &&
                            readp[2] == '*' && readp[3] == '/') {
                            basename = 1;
                            readp += 4;
                        }

                        if (*readp == '}') {
                            readp++;
                            value = shell_getenv(var_name);
                            if (!value)
                                value = "";
                            if (basename) {
                                const char* slash = shell_last_char(value, '/');
                                if (slash)
                                    value = slash + 1;
                            }
                            while (*value && writep < endp)
                                *writep++ = *value++;
                            if (*value) {
                                printf("mash: expansion too long\n");
                                return -1;
                            }
                            continue;
                        }

                        if (writep + 2 >= endp) {
                            printf("mash: token buffer full\n");
                            return -1;
                        }
                        *writep++ = '$';
                        *writep++ = '{';
                        continue;
                    }
                    if (*readp == '?') {
                        snprintf(status_buf, sizeof(status_buf), "%d", shell_status);
                        value = status_buf;
                        readp++;
                        while (*value && writep < endp)
                            *writep++ = *value++;
                        if (*value) {
                            printf("mash: expansion too long\n");
                            return -1;
                        }
                        continue;
                    }
                    if (*readp == '#') {
                        shell_script_frame_t* frame = &script_frames[script_frame_depth - 1];

                        snprintf(status_buf, sizeof(status_buf), "%d", frame->argc);
                        value = status_buf;
                        readp++;
                        while (*value && writep < endp)
                            *writep++ = *value++;
                        if (*value) {
                            printf("mash: expansion too long\n");
                            return -1;
                        }
                        continue;
                    }
                    if (*readp >= '0' && *readp <= '9') {
                        value = shell_script_arg_value(*readp - '0');
                        readp++;
                        while (*value && writep < endp)
                            *writep++ = *value++;
                        if (*value) {
                            printf("mash: expansion too long\n");
                            return -1;
                        }
                        continue;
                    }
                    if (!shell_var_start(*readp)) {
                        if (writep >= endp) {
                            printf("mash: token buffer full\n");
                            return -1;
                        }
                        *writep++ = '$';
                        continue;
                    }

                    while (shell_var_char(*readp) && var_len < SHELL_ENV_NAME_LEN - 1)
                        var_name[var_len++] = *readp++;
                    while (shell_var_char(*readp))
                        readp++;
                    var_name[var_len] = '\0';

                    value = shell_getenv(var_name);
                    if (!value)
                        value = "";

                    while (*value && writep < endp)
                        *writep++ = *value++;
                    if (*value) {
                        printf("mash: expansion too long\n");
                        return -1;
                    }
                    continue;
                }
                if (writep >= endp) {
                    printf("mash: token too long\n");
                    return -1;
                }
                *writep++ = *readp++;
            }
        }

        if (writep >= endp) {
            printf("mash: token buffer full\n");
            return -1;
        }
        *writep++ = '\0';

        if (*readp == ' ' || *readp == '\t') {
            readp++;
        }
    }
    
    while (*readp == ' ' || *readp == '\t')
        readp++;
    if (*readp) {
        argv[0] = NULL;
        if (argc >= SHELL_MAX_ARGS - 1)
            printf("mash: too many arguments\n");
        else
            printf("mash: parse error near '%c'\n", *readp);
        return -SHELL_MAX_ARGS;
    }

    argv[argc] = NULL;
    return argc;
}

// Parse a line into arguments
int shell_parse_line(char* line, char* argv[]) {
    return shell_parse_line_into(line, argv, token_buffer, sizeof(token_buffer));
}

static int shell_execute_command(int argc, char* argv[], int background) {
    shell_redirs_t redirs;

    if (argc == 0) {
        return SHELL_OK;
    }

    SHELL_TRACE("execute argc=%d cmd=%s bg=%d", argc, argv[0], background);

    if (argv_has_pipeline(argc, argv)) {
        SHELL_TRACE("pipeline start cmd=%s", argv[0]);
        return run_pipeline(argc, argv, background);
    }

    if (parse_redirections(&argc, argv, &redirs) < 0)
        return SHELL_ERROR;
    if (argc == 0)
        return SHELL_OK;

    if (argc == 1 && strchr(argv[0], '=')) {
        if (shell_apply_assignment(argv[0]) == 0)
            return SHELL_OK;
        printf("mash: invalid assignment: %s\n", argv[0]);
        return SHELL_ERROR;
    }

    // Command exit built-in
    if (strcmp(argv[0], "exit") == 0 ||
        strcmp(argv[0], "quit") == 0 ||
        strcmp(argv[0], "logout") == 0) {
        if (shell_is_login_shell()) {
            printf("%s: refusing to terminate the login shell; use shutdown instead\n", argv[0]);
            return SHELL_ERROR;
        }
        printf("Au revoir!\n");
        shell_running = 0;
        return SHELL_EXIT;
    }

    command_entry_t *entry = find_command(argv[0]);

    if(entry)
    {
        if (background) {
            printf("mash: background builtins are not supported\n");
            return SHELL_ERROR;
        }
        SHELL_TRACE("builtin start cmd=%s", argv[0]);
        int result = run_builtin_with_redirs(entry, argc, argv, &redirs);
        SHELL_TRACE("builtin done cmd=%s status=%d", argv[0], result);
        return result;
    }

    SHELL_TRACE("fork start cmd=%s", argv[0]);
    int sync_pipe[2] = { -1, -1 };
    if (!background && pipe(sync_pipe) < 0) {
        printf("mash: foreground sync pipe failed\n");
        return SHELL_ERROR;
    }

    int child_pid = fork();
    if (child_pid < 0) {
        if (sync_pipe[0] >= 0)
            close(sync_pipe[0]);
        if (sync_pipe[1] >= 0)
            close(sync_pipe[1]);
        printf("mash: fork failed: errno=%d\n", errno);
        return SHELL_ERROR;
    }

    if (child_pid == 0) {
        SHELL_TRACE("child exec cmd=%s", argv[0]);
        setpgid(0, 0);
        shell_restore_child_signal_handlers();
        if (!background) {
            close(sync_pipe[1]);
            shell_child_wait_for_parent(sync_pipe[0]);
        }
        if (apply_redirections(&redirs) < 0)
            exit(-1);

        exec_external_or_die(argc, argv);
        
    } else {
        SHELL_TRACE("fork done cmd=%s child=%d", argv[0], child_pid);
        shell_prepare_child_pgid(child_pid);
        if (sync_pipe[0] >= 0)
            close(sync_pipe[0]);

        if (background) {
            char command[JOBS_COMMAND_LEN];

            jobs_build_command(argc, argv, command, sizeof(command));
            jobs_add(child_pid, child_pid, command);
            printf("[bg] pid %d\n", child_pid);
            return SHELL_OK;
        }

        int status = 0;
        int stopped_status = 0;
        int was_stopped = 0;
        char command[JOBS_COMMAND_LEN];

        jobs_build_command(argc, argv, command, sizeof(command));
        shell_set_foreground_pgid(child_pid);
        if (sync_pipe[1] >= 0)
            close(sync_pipe[1]);

        SHELL_TRACE("wait start child=%d cmd=%s", child_pid, argv[0]);
        int waited_pid = shell_wait_foreground_child(child_pid, &status);
        SHELL_TRACE("wait done child=%d waited=%d status=%d", child_pid, waited_pid, status);
        if (waited_pid == child_pid &&
            WIFSTOPPED(status) &&
            WSTOPSIG(status) != 0) {
            stopped_status = status;
            was_stopped = 1;
        }
        shell_restore_foreground();
        if (was_stopped)
            remember_stopped_job(child_pid, child_pid, command, stopped_status);
        //printf("SHELL waked up waited_pid %d, son status = %d\n", waited_pid, status);

        return shell_status_from_wait_status(status);
    } 


    // Command unknown
    //printf("Commande inconnue: ");
    //printf(argv[0]);
    //printf("\n");
    return SHELL_ERROR;
}

// Execute a command
int shell_execute(int argc, char* argv[]) {
    int background = 0;

    if (argc == 0)
        return SHELL_OK;

    if (strcmp(argv[argc - 1], "&") == 0) {
        background = 1;
        argv[--argc] = NULL;
        if (argc == 0)
            return SHELL_OK;
    }

    return shell_execute_command(argc, argv, background);
}

static int shell_is_sequence_operator(const char* token) {
    return strcmp(token, ";") == 0 ||
           strcmp(token, "&") == 0 ||
           strcmp(token, "&&") == 0 ||
           strcmp(token, "||") == 0;
}

static int shell_should_execute_segment(const char* previous_op, int last_status) {
    if (!previous_op || strcmp(previous_op, ";") == 0 ||
        strcmp(previous_op, "&") == 0)
        return 1;
    if (strcmp(previous_op, "&&") == 0)
        return last_status == 0;
    if (strcmp(previous_op, "||") == 0)
        return last_status != 0;
    return 1;
}

static int shell_execute_argv_line(int argc, char* argv[]);

static int shell_token_is_word(const char* token, const char* word) {
    return token && strcmp(token, word) == 0;
}

static int shell_execute_arg_range(char* argv[], int start, int end) {
    char* local_argv[SHELL_MAX_ARGS];
    int argc = 0;

    while (start < end && shell_is_sequence_operator(argv[start]))
        start++;
    while (end > start && shell_is_sequence_operator(argv[end - 1]))
        end--;

    if (start >= end)
        return SHELL_OK;

    while (start < end && argc < SHELL_MAX_ARGS - 1)
        local_argv[argc++] = argv[start++];
    local_argv[argc] = NULL;

    if (start < end) {
        printf("mash: command too long in if block\n");
        return SHELL_ERROR;
    }

    return shell_execute_argv_line(argc, local_argv);
}

static int shell_find_then(int argc, char* argv[]) {
    int depth = 0;
    int i;

    for (i = 1; i < argc; i++) {
        if (shell_token_is_word(argv[i], "if")) {
            depth++;
        } else if (shell_token_is_word(argv[i], "fi")) {
            if (depth > 0)
                depth--;
        } else if (depth == 0 && shell_token_is_word(argv[i], "then")) {
            return i;
        }
    }

    return -1;
}

static int shell_find_else_or_fi(int argc, char* argv[], int start,
                                 int* else_index, int* fi_index) {
    int depth = 0;
    int i;

    *else_index = -1;
    *fi_index = -1;

    for (i = start; i < argc; i++) {
        if (shell_token_is_word(argv[i], "if")) {
            depth++;
        } else if (shell_token_is_word(argv[i], "fi")) {
            if (depth == 0) {
                *fi_index = i;
                return 0;
            }
            depth--;
        } else if (depth == 0 && shell_token_is_word(argv[i], "else")) {
            *else_index = i;
        }
    }

    return -1;
}

static int shell_execute_if(int argc, char* argv[]) {
    int then_index;
    int else_index;
    int fi_index;
    int cond_status;
    int result;

    then_index = shell_find_then(argc, argv);
    if (then_index < 0) {
        printf("mash: if: missing then\n");
        return SHELL_ERROR;
    }

    if (shell_find_else_or_fi(argc, argv, then_index + 1,
                              &else_index, &fi_index) < 0) {
        printf("mash: if: missing fi\n");
        return SHELL_ERROR;
    }

    cond_status = shell_execute_arg_range(argv, 1, then_index);
    if (cond_status == SHELL_EXIT)
        return SHELL_EXIT;

    if (cond_status == 0) {
        result = shell_execute_arg_range(argv, then_index + 1,
                                        else_index >= 0 ? else_index : fi_index);
    } else if (else_index >= 0) {
        result = shell_execute_arg_range(argv, else_index + 1, fi_index);
    } else {
        result = SHELL_OK;
    }

    return result;
}

#define SHELL_FOR_MAX_ITEMS 32
#define SHELL_FOR_ITEM_LEN  160

static int shell_token_has_glob(const char* token) {
    while (token && *token) {
        if (*token == '*')
            return 1;
        token++;
    }
    return 0;
}

static int shell_glob_match(const char* pattern, const char* text) {
    if (!*pattern)
        return !*text;

    if (*pattern == '*') {
        while (pattern[1] == '*')
            pattern++;
        pattern++;
        if (!*pattern)
            return 1;
        while (*text) {
            if (shell_glob_match(pattern, text))
                return 1;
            text++;
        }
        return shell_glob_match(pattern, text);
    }

    if (*text && *pattern == *text)
        return shell_glob_match(pattern + 1, text + 1);

    return 0;
}

static int shell_add_for_item(char items[][SHELL_FOR_ITEM_LEN],
                              int* count, const char* value) {
    if (*count >= SHELL_FOR_MAX_ITEMS)
        return -1;
    strncpy(items[*count], value, SHELL_FOR_ITEM_LEN - 1);
    items[*count][SHELL_FOR_ITEM_LEN - 1] = '\0';
    (*count)++;
    return 0;
}

static const char* shell_last_char(const char* s, char c) {
    const char* last = NULL;

    while (*s) {
        if (*s == c)
            last = s;
        s++;
    }

    return last;
}

static int shell_split_glob_token(const char* token, char* dir,
                                  char* prefix, char* pattern) {
    const char* slash = shell_last_char(token, '/');

    if (!slash) {
        strcpy(dir, ".");
        prefix[0] = '\0';
        strncpy(pattern, token, SHELL_FOR_ITEM_LEN - 1);
        pattern[SHELL_FOR_ITEM_LEN - 1] = '\0';
        return 0;
    }

    if (slash == token) {
        strcpy(dir, "/");
        strcpy(prefix, "/");
    } else {
        int dir_len = slash - token;

        if (dir_len >= SHELL_FOR_ITEM_LEN - 1)
            return -1;
        memcpy(dir, token, dir_len);
        dir[dir_len] = '\0';
        memcpy(prefix, token, dir_len);
        prefix[dir_len] = '/';
        prefix[dir_len + 1] = '\0';
    }

    strncpy(pattern, slash + 1, SHELL_FOR_ITEM_LEN - 1);
    pattern[SHELL_FOR_ITEM_LEN - 1] = '\0';
    return 0;
}

static int shell_expand_glob_for_item(const char* token,
                                      char items[][SHELL_FOR_ITEM_LEN],
                                      int* count) {
    char dir[SHELL_FOR_ITEM_LEN];
    char prefix[SHELL_FOR_ITEM_LEN];
    char pattern[SHELL_FOR_ITEM_LEN];
    char buf[1024];
    int matched = 0;
    int fd;
    int n;

    if (!shell_token_has_glob(token))
        return shell_add_for_item(items, count, token);

    if (shell_split_glob_token(token, dir, prefix, pattern) < 0)
        return shell_add_for_item(items, count, token);

    fd = open(dir, O_RDONLY | O_DIRECTORY, 0);
    if (fd < 0)
        return shell_add_for_item(items, count, token);

    while ((n = getdents(fd, buf, sizeof(buf))) > 0) {
        int pos = 0;

        while (pos < n) {
            struct linux_dirent* entry = (struct linux_dirent*)(buf + pos);
            char full[SHELL_FOR_ITEM_LEN];

            if (entry->d_reclen == 0)
                break;

            if (strcmp(entry->d_name, ".") != 0 &&
                strcmp(entry->d_name, "..") != 0 &&
                (pattern[0] == '.' || entry->d_name[0] != '.') &&
                shell_glob_match(pattern, entry->d_name)) {
                snprintf(full, sizeof(full), "%s%s", prefix, entry->d_name);
                if (shell_add_for_item(items, count, full) < 0) {
                    close(fd);
                    return -1;
                }
                matched = 1;
            }

            pos += entry->d_reclen;
        }
    }

    close(fd);

    if (!matched)
        return shell_add_for_item(items, count, token);

    return 0;
}

static int shell_collect_for_items(char* argv[], int start, int end,
                                   char items[][SHELL_FOR_ITEM_LEN],
                                   int* count) {
    int i;

    *count = 0;
    for (i = start; i < end; i++) {
        if (shell_is_sequence_operator(argv[i]))
            continue;
        if (shell_expand_glob_for_item(argv[i], items, count) < 0) {
            printf("mash: for: too many items\n");
            return -1;
        }
    }

    return 0;
}

static int shell_find_for_in_do_done(int argc, char* argv[],
                                     int* in_index, int* do_index,
                                     int* done_index) {
    int depth = 0;
    int i;

    *in_index = -1;
    *do_index = -1;
    *done_index = -1;

    for (i = 2; i < argc; i++) {
        if (shell_token_is_word(argv[i], "for")) {
            depth++;
        } else if (shell_token_is_word(argv[i], "done")) {
            if (depth == 0) {
                *done_index = i;
                return *in_index >= 0 && *do_index >= 0 ? 0 : -1;
            }
            depth--;
        } else if (depth == 0 && shell_token_is_word(argv[i], "in")) {
            if (*in_index < 0)
                *in_index = i;
        } else if (depth == 0 && shell_token_is_word(argv[i], "do")) {
            *do_index = i;
        }
    }

    return -1;
}

static int shell_execute_for(int argc, char* argv[]) {
    char items[SHELL_FOR_MAX_ITEMS][SHELL_FOR_ITEM_LEN];
    int item_count = 0;
    int in_index;
    int do_index;
    int done_index;
    int i;
    int result = SHELL_OK;

    if (argc < 6 || !shell_is_valid_name(argv[1])) {
        printf("mash: for: expected 'for NAME in ...; do ...; done'\n");
        return SHELL_ERROR;
    }

    if (shell_find_for_in_do_done(argc, argv, &in_index,
                                  &do_index, &done_index) < 0) {
        printf("mash: for: missing in/do/done\n");
        return SHELL_ERROR;
    }

    if (shell_collect_for_items(argv, in_index + 1, do_index,
                                items, &item_count) < 0)
        return SHELL_ERROR;

    for (i = 0; i < item_count; i++) {
        if (shell_setenv(argv[1], items[i]) < 0) {
            printf("mash: for: cannot set %s\n", argv[1]);
            return SHELL_ERROR;
        }

        result = shell_execute_arg_range(argv, do_index + 1, done_index);
        if (result == SHELL_EXIT)
            return result;
    }

    return result;
}

static int shell_execute_argv_line(int argc, char* argv[]) {
    int start = 0;
    int last_status = SHELL_OK;
    const char* previous_op = NULL;

    while (start < argc) {
        int end = start;
        const char* next_op = NULL;
        int segment_argc;
        int depth = 0;

        while (end < argc) {
            if (shell_token_is_word(argv[end], "if")) {
                depth++;
            } else if (shell_token_is_word(argv[end], "for")) {
                depth++;
            } else if (shell_token_is_word(argv[end], "fi") && depth > 0) {
                depth--;
                end++;
                if (depth == 0)
                    break;
                continue;
            } else if (shell_token_is_word(argv[end], "done") && depth > 0) {
                depth--;
                end++;
                if (depth == 0)
                    break;
                continue;
            }
            if (depth == 0 && shell_is_sequence_operator(argv[end]))
                break;
            end++;
        }

        if (end < argc)
            next_op = argv[end];

        segment_argc = end - start;
        if (segment_argc == 0) {
            printf("mash: syntax error near '%s'\n", next_op ? next_op : "newline");
            return SHELL_ERROR;
        }

        if (shell_should_execute_segment(previous_op, last_status)) {
            int background = next_op && strcmp(next_op, "&") == 0;

            argv[end] = NULL;
            if (background &&
                (shell_token_is_word(argv[start], "if") ||
                 shell_token_is_word(argv[start], "for") ||
                 shell_token_is_word(argv[start], "while") ||
                 shell_token_is_word(argv[start], "until"))) {
                printf("mash: background compound commands are not supported\n");
                last_status = SHELL_ERROR;
            } else if (shell_token_is_word(argv[start], "if")) {
                last_status = shell_execute_if(segment_argc, &argv[start]);
            } else if (shell_token_is_word(argv[start], "for")) {
                last_status = shell_execute_for(segment_argc, &argv[start]);
            } else if (shell_token_is_word(argv[start], "while") ||
                       shell_token_is_word(argv[start], "until")) {
                /* Line-oriented while/until is handled before token execution. */
                printf("mash: loop syntax error\n");
                last_status = SHELL_ERROR;
            } else {
                last_status = shell_execute_command(segment_argc,
                                                    &argv[start],
                                                    background);
            }
            if (last_status != SHELL_EXIT)
                shell_status = last_status;
            if (last_status == SHELL_EXIT)
                return SHELL_EXIT;
        }

        if (!next_op)
            break;

        previous_op = next_op;
        start = end + 1;

        if (start >= argc) {
            if (strcmp(previous_op, ";") == 0 || strcmp(previous_op, "&") == 0)
                break;
            printf("mash: expected command after '%s'\n", previous_op);
            return SHELL_ERROR;
        }
    }

    return last_status;
}

static int shell_is_word_boundary_char(char c) {
    return !shell_var_char(c);
}

static int shell_word_equals_at(const char* p, const char* word, int word_len) {
    int i;

    for (i = 0; i < word_len; i++) {
        if (p[i] != word[i])
            return 0;
    }

    return 1;
}

static char* shell_find_control_word(char* s, const char* word) {
    int word_len = strlen(word);
    char quote = 0;
    char* p = s;

    while (*p) {
        if (quote) {
            if (*p == quote)
                quote = 0;
            p++;
            continue;
        }

        if (*p == '\'' || *p == '"') {
            quote = *p++;
            continue;
        }

        if ((p == s || shell_is_word_boundary_char(p[-1])) &&
            shell_word_equals_at(p, word, word_len) &&
            shell_is_word_boundary_char(p[word_len])) {
            return p;
        }

        p++;
    }

    return NULL;
}

static char* shell_skip_separators(char* s) {
    while (*s == ' ' || *s == '\t' || *s == ';')
        s++;
    return s;
}

static void shell_trim_trailing_separators(char* s) {
    char* end = s + strlen(s);

    while (end > s && (end[-1] == ' ' || end[-1] == '\t' ||
                       end[-1] == ';')) {
        *--end = '\0';
    }
}

static int shell_execute_for_line(char* line) {
    char list_copy[SHELL_BUFFER_SIZE];
    char body_copy[SHELL_BUFFER_SIZE];
    char list_tokens[SHELL_BUFFER_SIZE];
    char* list_argv[SHELL_MAX_ARGS];
    char items[SHELL_FOR_MAX_ITEMS][SHELL_FOR_ITEM_LEN];
    char var_name[SHELL_ENV_NAME_LEN];
    char* p;
    char* in_word;
    char* do_word;
    char* done_word;
    char* list_start;
    char* body_start;
    int var_len = 0;
    int list_argc;
    int item_count = 0;
    int i;
    int result = SHELL_OK;

    p = trim_spaces(line + 3);
    while (shell_var_char(*p) && var_len < SHELL_ENV_NAME_LEN - 1) {
        var_name[var_len++] = *p++;
    }
    var_name[var_len] = '\0';

    if (!shell_is_valid_name(var_name)) {
        printf("mash: for: invalid variable name\n");
        return SHELL_ERROR;
    }

    in_word = shell_find_control_word(p, "in");
    if (!in_word) {
        printf("mash: for: missing in\n");
        return SHELL_ERROR;
    }

    list_start = in_word + 2;
    do_word = shell_find_control_word(list_start, "do");
    if (!do_word) {
        printf("mash: for: missing do\n");
        return SHELL_ERROR;
    }

    body_start = do_word + 2;
    done_word = shell_find_control_word(body_start, "done");
    if (!done_word) {
        printf("mash: for: missing done\n");
        return SHELL_ERROR;
    }

    *do_word = '\0';
    *done_word = '\0';
    list_start = shell_skip_separators(trim_spaces(list_start));
    body_start = shell_skip_separators(trim_spaces(body_start));
    shell_trim_trailing_separators(list_start);
    shell_trim_trailing_separators(body_start);

    strncpy(list_copy, list_start, sizeof(list_copy) - 1);
    list_copy[sizeof(list_copy) - 1] = '\0';

    list_argc = shell_parse_line_into(list_copy, list_argv,
                                      list_tokens, sizeof(list_tokens));
    if (list_argc < 0)
        return SHELL_ERROR;
    if (list_argc == 0)
        return SHELL_OK;

    if (shell_collect_for_items(list_argv, 0, list_argc,
                                items, &item_count) < 0)
        return SHELL_ERROR;

    for (i = 0; i < item_count; i++) {
        if (shell_setenv(var_name, items[i]) < 0) {
            printf("mash: for: cannot set %s\n", var_name);
            return SHELL_ERROR;
        }

        strncpy(body_copy, body_start, sizeof(body_copy) - 1);
        body_copy[sizeof(body_copy) - 1] = '\0';
        result = shell_execute_line(body_copy);
        if (result == SHELL_EXIT)
            return result;
    }

    return result;
}

static int shell_execute_loop_line(char* line, int until_loop)
{
    char cond_copy[SHELL_BUFFER_SIZE];
    char body_copy[SHELL_BUFFER_SIZE];
    char* do_word;
    char* done_word;
    char* cond_start;
    char* body_start;
    int result = SHELL_OK;

    cond_start = trim_spaces(line + (until_loop ? 5 : 5));
    do_word = shell_find_control_word(cond_start, "do");
    if (!do_word) {
        printf("mash: %s: missing do\n", until_loop ? "until" : "while");
        return SHELL_ERROR;
    }

    body_start = do_word + 2;
    done_word = shell_find_control_word(body_start, "done");
    if (!done_word) {
        printf("mash: %s: missing done\n", until_loop ? "until" : "while");
        return SHELL_ERROR;
    }

    *do_word = '\0';
    *done_word = '\0';
    cond_start = shell_skip_separators(trim_spaces(cond_start));
    body_start = shell_skip_separators(trim_spaces(body_start));
    shell_trim_trailing_separators(cond_start);
    shell_trim_trailing_separators(body_start);

    if (!*cond_start) {
        printf("mash: %s: missing condition\n", until_loop ? "until" : "while");
        return SHELL_ERROR;
    }

    while (!shell_termination_requested()) {
        int cond_status;
        int should_run;

        strncpy(cond_copy, cond_start, sizeof(cond_copy) - 1);
        cond_copy[sizeof(cond_copy) - 1] = '\0';
        cond_status = shell_execute_line(cond_copy);
        if (cond_status == SHELL_EXIT)
            return cond_status;

        should_run = until_loop ? (cond_status != 0) : (cond_status == 0);
        if (!should_run)
            break;

        strncpy(body_copy, body_start, sizeof(body_copy) - 1);
        body_copy[sizeof(body_copy) - 1] = '\0';
        result = shell_execute_line(body_copy);
        if (result == SHELL_EXIT)
            return result;
    }

    return result;
}

static void shell_strip_comment(char* line) {
    char quote = 0;
    char* p = line;
    char previous = '\0';

    while (*p) {
        if (quote) {
            if (*p == quote)
                quote = 0;
        } else {
            if (*p == '\'' || *p == '"') {
                quote = *p;
            } else if (*p == '#' &&
                       (p == line || previous == ' ' || previous == '\t')) {
                *p = '\0';
                return;
            }
        }

        previous = *p;
        p++;
    }
}

static char shell_unmatched_quote(const char* line) {
    char quote = 0;
    const char* p = line;

    while (*p) {
        if (quote) {
            if (*p == quote) {
                quote = 0;
                p++;
                continue;
            }
            if (*p == '\\' && shell_backslash_escapes(quote, p[1])) {
                p += 2;
                continue;
            }
        } else {
            if (*p == '\'' || *p == '"') {
                quote = *p++;
                continue;
            }
            if (*p == '\\' && shell_backslash_escapes(quote, p[1])) {
                p += 2;
                continue;
            }
        }
        p++;
    }

    return quote;
}

int shell_execute_line(char* line) {
    char local_tokens[SHELL_BUFFER_SIZE];
    char* local_argv[SHELL_MAX_ARGS];
    char* trimmed;
    char quote;
    int argc;
    int result;

    if (!line)
        return SHELL_OK;

    shell_strip_comment(line);
    trimmed = trim_spaces(line);
    if (!*trimmed)
        return SHELL_OK;

    quote = shell_unmatched_quote(trimmed);
    if (quote) {
        printf("mash: unmatched %c quote\n", quote);
        shell_status = 2;
        return 2;
    }

    if (starts_with(trimmed, "for ")) {
        result = shell_execute_for_line(trimmed);
        if (result != SHELL_EXIT)
            shell_status = result;
        return result;
    }

    if (starts_with(trimmed, "while ") || starts_with(trimmed, "until ")) {
        result = shell_execute_loop_line(trimmed, starts_with(trimmed, "until "));
        if (result != SHELL_EXIT)
            shell_status = result;
        return result;
    }

    argc = shell_parse_line_into(trimmed, local_argv,
                                 local_tokens, sizeof(local_tokens));
    if (argc < 0) {
        shell_status = SHELL_ERROR;
        return SHELL_ERROR;
    }
    if (argc == 0)
        return SHELL_OK;

    SHELL_TRACE("parsed argc=%d cmd=%s", argc, local_argv[0]);
    result = shell_execute_argv_line(argc, local_argv);
    SHELL_TRACE("line done status=%d", result);
    if (result != SHELL_EXIT)
        shell_status = result;
    return result;
}


// Shell main loop
void shell_run(void) {
    if (shell_should_print_banner())
        shell_print_banner();
    shell_running = 1;
    
    while (shell_running) {
        if (shell_termination_requested())
            break;

        jobs_reap_background();
        shell_restore_foreground();
        shell_print_prompt();

        SHELL_TRACE("read start");
        char* line = shell_read_line();
        SHELL_TRACE("read done line='%s'", line ? line : "(null)");
        if (shell_termination_requested())
            break;
        if (!line) {
            if (shell_line_was_eof()) {
                if (shell_is_login_shell()) {
                    printf("mash: refusing EOF on login shell; use shutdown instead\n");
                    continue;
                }
                printf("logout\n");
                break;
            }
            continue;
        }
        if (strlen(line) == 0) {
            continue;
        }

        SHELL_TRACE("exec line start");
        int result = shell_execute_line(line);
        SHELL_TRACE("exec line done status=%d", result);
        if (result == SHELL_EXIT) {
            break;
        }
    }

    shell_line_edit_shutdown();
    if (shell_terminate_signal)
        printf("Shell closed (signal %d)\n", (int)shell_terminate_signal);
    else
        printf("Shell closed\n");
}


int main(int argc, char **argv, char **envp) {
    int version = 11 ;
    int result;

    shell_init_env(envp);

    if (argc >= 3 && strcmp(argv[1], "-c") == 0) {
        char command[SHELL_BUFFER_SIZE];
        size_t len = strlen(argv[2]);

        command_init();
        if (len >= sizeof(command)) {
            printf("mash: -c command too long\n");
            exit(2);
        }
        memcpy(command, argv[2], len + 1);

        result = shell_execute_line(command);
        if (result == SHELL_EXIT)
            result = 0;
        exit(result);
    }

    shell_load_startup_files();
    shell_line_edit_init();
    setpgid(0, 0);
    shell_pgid = getpgrp();
    jobs_set_shell_pgid(shell_pgid);
    shell_install_signal_handlers();
    shell_restore_foreground();
    shell_restore_tty_mode();
    command_init();
    shell_run();

    exit(version);

}
