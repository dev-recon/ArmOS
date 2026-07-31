/*
 * ArmOS
 * Copyright (c) 2026 Mohamed Ennassiri
 *
 * Licensed under the Apache License, Version 2.0.
 * See LICENSE for details.
 *
 * File: userland/programs/ttytest/ttytest.c
 * Layer: Userland / test or sample program
 * Description: Userland test, diagnostic, or sample application.
 */

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

static struct termios saved_termios;
static int have_saved_termios;
static int failures;

static void restore_terminal(void)
{
    if (have_saved_termios)
        tcsetattr(STDIN_FILENO, TCSANOW, &saved_termios);
}

static void ok(const char *name)
{
    printf("\033[1;32m[OK]\033[0m %s\n", name);
}

static void fail(const char *name, int detail)
{
    failures++;
    printf("\033[1;31m[FAIL]\033[0m %s (%d)\n", name, detail);
}

static void expect_true(int cond, const char *name, int detail)
{
    if (cond)
        ok(name);
    else
        fail(name, detail);
}

static int line_equals(const char *line, const char *expected)
{
    return strcmp(line, expected) == 0;
}

static int read_interactive_line(const char *prompt, char *buf, size_t size)
{
    ssize_t n;

    printf("%s", prompt);
    fflush(stdout);

    memset(buf, 0, size);
    errno = 0;
    n = read(STDIN_FILENO, buf, size - 1);
    if (n < 0) {
        fail(prompt, errno);
        return -1;
    }

    buf[n] = '\0';
    return (int)n;
}

static int run_interactive_canon_test(void)
{
    struct termios canon;
    char line[128];
    int n;

    printf("ttytest: interactive canonical test\n");
    printf("This test temporarily enables kernel canonical echo/editing.\n");

    if (tcgetattr(STDIN_FILENO, &saved_termios) < 0) {
        fail("tcgetattr stdin", errno);
        return 1;
    }
    have_saved_termios = 1;
    atexit(restore_terminal);

    canon = saved_termios;
    canon.c_lflag |= ICANON | ECHO | ECHOE | ECHOK | ECHOCTL;
    canon.c_iflag |= ICRNL;
    canon.c_oflag |= OPOST | ONLCR;
    canon.c_cc[VERASE] = 0x7F;
    canon.c_cc[VKILL] = 0x15;
    canon.c_cc[VEOF] = 0x04;
    canon.c_cc[VWERASE] = 0x17;
    canon.c_cc[VMIN] = 1;
    canon.c_cc[VTIME] = 0;

    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &canon) < 0) {
        fail("tcsetattr canonical interactive mode", errno);
        return 1;
    }
    ok("tcsetattr canonical interactive mode");

    printf("1) Type: abc, Backspace, d, Enter. Expected line: abd\n");
    n = read_interactive_line("> ", line, sizeof(line));
    if (n >= 0)
        expect_true(line_equals(line, "abd\n"), "canonical erase edits line", n);

    printf("2) Type: garbage, Ctrl-U, ok, Enter. Expected line: ok\n");
    n = read_interactive_line("> ", line, sizeof(line));
    if (n >= 0)
        expect_true(line_equals(line, "ok\n"), "canonical kill-line edits line", n);

    printf("3) Type: one two, Ctrl-W, three, Enter. Expected line: one three\n");
    n = read_interactive_line("> ", line, sizeof(line));
    if (n >= 0)
        expect_true(line_equals(line, "one three\n"), "canonical word erase edits line", n);

    printf("4) Press Ctrl-D on an empty line. Expected EOF read length 0.\n");
    n = read_interactive_line("> ", line, sizeof(line));
    if (n >= 0)
        expect_true(n == 0, "canonical Ctrl-D returns EOF", n);

    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &saved_termios) == 0)
        ok("tcsetattr restores terminal");
    else
        fail("tcsetattr restores terminal", errno);

    have_saved_termios = 0;
    printf("ttytest interactive: %s\n", failures ? "failed" : "all tests passed");
    return failures ? 1 : 0;
}

static int run_interactive_raw_test(void)
{
    struct termios raw;
    unsigned char buf[16];
    ssize_t n;

    printf("ttytest: interactive raw test\n");
    printf("This test temporarily enables non-canonical no-echo mode.\n");

    if (tcgetattr(STDIN_FILENO, &saved_termios) < 0) {
        fail("tcgetattr stdin", errno);
        return 1;
    }
    have_saved_termios = 1;
    atexit(restore_terminal);

    raw = saved_termios;
    raw.c_lflag &= ~(ICANON | ECHO);
    raw.c_iflag &= ~ICRNL;
    raw.c_oflag &= ~OPOST;
    raw.c_cc[VMIN] = 3;
    raw.c_cc[VTIME] = 0;

    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) < 0) {
        fail("tcsetattr raw VMIN=3", errno);
        return 1;
    }
    ok("tcsetattr raw VMIN=3");

    printf("1) Type exactly abc. Expected one read of 3 bytes: abc\n");
    fflush(stdout);
    memset(buf, 0, sizeof(buf));
    errno = 0;
    n = read(STDIN_FILENO, buf, 3);
    if (n == 3 && memcmp(buf, "abc", 3) == 0)
        ok("raw read waits for VMIN bytes");
    else if (n < 0)
        fail("raw read waits for VMIN bytes", errno);
    else
        fail("raw read waits for VMIN bytes", (int)n);

    raw.c_cc[VMIN] = 3;
    raw.c_cc[VTIME] = 1;
    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) < 0) {
        fail("tcsetattr raw VMIN=3 VTIME=1", errno);
        return 1;
    }
    ok("tcsetattr raw VMIN=3 VTIME=1");

    printf("2) Press Right Arrow once. Expected ESC [ C in one read.\n");
    fflush(stdout);
    memset(buf, 0, sizeof(buf));
    errno = 0;
    n = read(STDIN_FILENO, buf, sizeof(buf));
    if (n >= 3 && buf[0] == 0x1b && buf[1] == '[' && buf[2] == 'C')
        ok("raw timeout read captures arrow sequence");
    else if (n < 0)
        fail("raw timeout read captures arrow sequence", errno);
    else
        fail("raw timeout read captures arrow sequence", (int)n);

    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &saved_termios) == 0)
        ok("tcsetattr restores terminal");
    else
        fail("tcsetattr restores terminal", errno);

    have_saved_termios = 0;
    printf("ttytest raw interactive: %s\n", failures ? "failed" : "all tests passed");
    return failures ? 1 : 0;
}

static void test_tcflush_selectors(void)
{
    if (tcflush(STDIN_FILENO, TCIFLUSH) == 0)
        ok("tcflush input queue");
    else
        fail("tcflush input queue", errno);

    if (tcflush(STDOUT_FILENO, TCOFLUSH) == 0)
        ok("tcflush output queue");
    else
        fail("tcflush output queue", errno);

    if (tcflush(STDIN_FILENO, TCIOFLUSH) == 0)
        ok("tcflush input/output queues");
    else
        fail("tcflush input/output queues", errno);

    errno = 0;
    if (tcflush(STDIN_FILENO, 99) < 0 && errno == EINVAL)
        ok("tcflush rejects invalid selector");
    else
        fail("tcflush rejects invalid selector", errno);
}

static void test_tcsetattr_actions(void)
{
    errno = 0;
    if (tcsetattr(STDIN_FILENO, 99, &saved_termios) < 0 && errno == EINVAL)
        ok("tcsetattr rejects invalid action");
    else
        fail("tcsetattr rejects invalid action", errno);

    if (tcsetattr(STDIN_FILENO, TCSADRAIN, &saved_termios) == 0)
        ok("tcsetattr TCSADRAIN accepted");
    else
        fail("tcsetattr TCSADRAIN accepted", errno);
}

static void test_tcdrain(void)
{
    if (tcdrain(STDOUT_FILENO) == 0)
        ok("tcdrain stdout");
    else
        fail("tcdrain stdout", errno);
}

static int set_and_check_termios(const struct termios *tio,
                                 const char *set_name,
                                 struct termios *out)
{
    memset(out, 0, sizeof(*out));
    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, tio) < 0) {
        fail(set_name, errno);
        return -1;
    }
    ok(set_name);

    if (tcgetattr(STDIN_FILENO, out) < 0) {
        fail("tcgetattr observes mode", errno);
        return -1;
    }
    ok("tcgetattr observes mode");
    return 0;
}

static void test_raw_timeout_mode(void)
{
    struct termios raw = saved_termios;
    struct termios check;
    unsigned char c = 0;
    ssize_t n;

    raw.c_lflag &= ~(ICANON | ECHO);
    raw.c_iflag &= ~ICRNL;
    raw.c_oflag &= ~OPOST;
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 1;

    if (set_and_check_termios(&raw, "tcsetattr raw timeout mode", &check) < 0)
        return;

    expect_true((check.c_lflag & ICANON) == 0,
                "raw mode clears ICANON", (int)check.c_lflag);
    expect_true((check.c_lflag & ECHO) == 0,
                "raw mode clears ECHO", (int)check.c_lflag);
    expect_true((check.c_iflag & ICRNL) == 0,
                "raw mode clears ICRNL", (int)check.c_iflag);
    expect_true((check.c_oflag & OPOST) == 0,
                "raw mode clears OPOST", (int)check.c_oflag);
    expect_true(check.c_cc[VMIN] == 0,
                "raw mode keeps VMIN=0", check.c_cc[VMIN]);
    expect_true(check.c_cc[VTIME] == 1,
                "raw mode keeps VTIME=1", check.c_cc[VTIME]);

    errno = 0;
    n = read(STDIN_FILENO, &c, 1);
    if (n == 0)
        ok("read timeout returns 0 with VMIN=0/VTIME=1");
    else if (n == 1)
        printf("\033[1;33m[WARN]\033[0m read consumed pending byte 0x%02x\n", c);
    else
        fail("read timeout returns 0 with VMIN=0/VTIME=1", errno);
}

static void test_raw_poll_mode(void)
{
    struct termios raw = saved_termios;
    struct termios check;
    unsigned char c = 0;
    ssize_t n;

    raw.c_lflag &= ~(ICANON | ECHO);
    raw.c_iflag &= ~ICRNL;
    raw.c_oflag &= ~OPOST;
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 0;

    if (set_and_check_termios(&raw, "tcsetattr raw poll mode", &check) < 0)
        return;

    expect_true(check.c_cc[VMIN] == 0,
                "raw poll keeps VMIN=0", check.c_cc[VMIN]);
    expect_true(check.c_cc[VTIME] == 0,
                "raw poll keeps VTIME=0", check.c_cc[VTIME]);

    errno = 0;
    n = read(STDIN_FILENO, &c, 1);
    if (n == 0)
        ok("read poll returns 0 with VMIN=0/VTIME=0");
    else if (n == 1)
        printf("\033[1;33m[WARN]\033[0m poll consumed pending byte 0x%02x\n", c);
    else
        fail("read poll returns 0 with VMIN=0/VTIME=0", errno);
}

static void test_control_chars_preserved(void)
{
    struct termios tio = saved_termios;
    struct termios check;

    tio.c_cc[VERASE] = 0x08;
    tio.c_cc[VKILL] = 0x15;
    tio.c_cc[VEOF] = 0x04;
    tio.c_cc[VINTR] = 0x03;
    tio.c_cc[VSUSP] = 0x1A;
    tio.c_cc[VWERASE] = 0x17;

    if (set_and_check_termios(&tio, "tcsetattr control chars", &check) < 0)
        return;

    expect_true(check.c_cc[VERASE] == 0x08,
                "termios preserves VERASE", check.c_cc[VERASE]);
    expect_true(check.c_cc[VKILL] == 0x15,
                "termios preserves VKILL", check.c_cc[VKILL]);
    expect_true(check.c_cc[VEOF] == 0x04,
                "termios preserves VEOF", check.c_cc[VEOF]);
    expect_true(check.c_cc[VINTR] == 0x03,
                "termios preserves VINTR", check.c_cc[VINTR]);
    expect_true(check.c_cc[VSUSP] == 0x1A,
                "termios preserves VSUSP", check.c_cc[VSUSP]);
    expect_true(check.c_cc[VWERASE] == 0x17,
                "termios preserves VWERASE", check.c_cc[VWERASE]);
}

static void test_background_termios_job_control(void)
{
    pid_t child;
    pid_t waited;
    int status = 0;

    if (tcgetpgrp(STDIN_FILENO) != getpgrp()) {
        printf("\033[1;33m[SKIP]\033[0m background tcsetattr job control "
               "(ttytest is not foreground)\n");
        return;
    }

    child = fork();
    if (child == 0) {
        signal(SIGTTOU, SIG_DFL);
        if (setpgid(0, 0) < 0)
            _exit(2);
        (void)tcsetattr(STDIN_FILENO, TCSANOW, &saved_termios);
        _exit(3);
    }
    if (child < 0) {
        fail("fork background tcsetattr child", errno);
        return;
    }

    waited = waitpid(child, &status, WUNTRACED);
    expect_true(waited == child && WIFSTOPPED(status) &&
                WSTOPSIG(status) == SIGTTOU,
                "background tcsetattr stops group with SIGTTOU", status);
    kill(child, SIGKILL);
    (void)waitpid(child, &status, 0);

    child = fork();
    if (child == 0) {
        signal(SIGTTOU, SIG_IGN);
        if (setpgid(0, 0) < 0)
            _exit(2);
        _exit(tcsetattr(STDIN_FILENO, TCSANOW, &saved_termios) == 0 ? 0 : 3);
    }
    if (child < 0) {
        fail("fork ignored SIGTTOU child", errno);
        return;
    }

    waited = waitpid(child, &status, 0);
    expect_true(waited == child && WIFEXITED(status) && WEXITSTATUS(status) == 0,
                "ignored SIGTTOU permits background tcsetattr", status);

    child = fork();
    if (child == 0) {
        sigset_t blocked;

        sigemptyset(&blocked);
        sigaddset(&blocked, SIGTTOU);
        if (sigprocmask(SIG_BLOCK, &blocked, NULL) < 0 || setpgid(0, 0) < 0)
            _exit(2);
        _exit(tcsetattr(STDIN_FILENO, TCSANOW, &saved_termios) == 0 ? 0 : 3);
    }
    if (child < 0) {
        fail("fork blocked SIGTTOU child", errno);
        return;
    }

    waited = waitpid(child, &status, 0);
    expect_true(waited == child && WIFEXITED(status) && WEXITSTATUS(status) == 0,
                "blocked SIGTTOU permits background tcsetattr", status);
}

int main(int argc, char **argv)
{
    struct termios tio;

    if (argc > 1) {
        if (strcmp(argv[1], "--interactive-canon") == 0)
            return run_interactive_canon_test();
        if (strcmp(argv[1], "--interactive-raw") == 0)
            return run_interactive_raw_test();
        printf("usage: ttytest [--interactive-canon|--interactive-raw]\n");
        return 1;
    }

    printf("ttytest: termios smoke test\n");

    if (tcgetattr(STDIN_FILENO, &saved_termios) < 0) {
        fail("tcgetattr stdin", errno);
        return 1;
    }
    have_saved_termios = 1;
    atexit(restore_terminal);

    tio = saved_termios;
    ok("tcgetattr stdin");
    expect_true((tio.c_lflag & ICANON) != 0, "default ICANON set", (int)tio.c_lflag);
    expect_true((tio.c_lflag & ISIG) != 0, "default ISIG set", (int)tio.c_lflag);
    expect_true((tio.c_iflag & ICRNL) != 0, "default ICRNL set", (int)tio.c_iflag);
    expect_true((tio.c_oflag & OPOST) != 0, "default OPOST set", (int)tio.c_oflag);
    expect_true((tio.c_oflag & ONLCR) != 0, "default ONLCR set", (int)tio.c_oflag);
    expect_true(tio.c_cc[VMIN] == 1, "default VMIN is 1", tio.c_cc[VMIN]);
    expect_true(tio.c_cc[VINTR] == 3, "default VINTR is Ctrl-C", tio.c_cc[VINTR]);

    test_tcflush_selectors();
    test_tcsetattr_actions();
    test_tcdrain();
    test_control_chars_preserved();
    test_background_termios_job_control();
    test_raw_timeout_mode();
    test_raw_poll_mode();

    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &saved_termios) == 0)
        ok("tcsetattr restores terminal");
    else
        fail("tcsetattr restores terminal", errno);

    have_saved_termios = 0;
    printf("ttytest: %s\n", failures ? "failed" : "all tests passed");
    return failures ? 1 : 0;
}
