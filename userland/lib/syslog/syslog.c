/*
 * ArmOS
 * Copyright (c) 2026 Mohamed Ennassiri
 *
 * Licensed under the Apache License, Version 2.0.
 * See LICENSE for details.
 *
 * File: userland/lib/syslog/syslog.c
 * Layer: Userland / POSIX compatibility
 *
 * Responsibilities:
 * - Format BSD/POSIX syslog records.
 * - Send records to the local `/dev/log` AF_UNIX datagram endpoint.
 * - Honor the priority mask and stderr/console fallback options.
 *
 * Notes:
 * - This library does not introduce a kernel logging policy.
 * - Delivery gracefully degrades while no userland syslog daemon is running.
 */

#include <syslog.h>

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <sys/socket.h>
#include <sys/un.h>

#define SYSLOG_MESSAGE_MAX 1024

static const char *log_ident;
static int log_option;
static int log_facility = LOG_USER;
static int log_mask = LOG_UPTO(LOG_DEBUG);

void openlog(const char *ident, int option, int facility)
{
    log_ident = ident;
    log_option = option;
    if ((facility & LOG_FACMASK) != 0)
        log_facility = facility & LOG_FACMASK;
}

void closelog(void)
{
    log_ident = NULL;
    log_option = 0;
    log_facility = LOG_USER;
}

int setlogmask(int mask)
{
    int previous = log_mask;

    if (mask != 0)
        log_mask = mask;
    return previous;
}

static void write_fallback(const char *message)
{
    size_t length;

    if ((log_option & (LOG_PERROR | LOG_CONS)) == 0)
        return;
    length = strlen(message);
    (void)write(STDERR_FILENO, message, length);
    (void)write(STDERR_FILENO, "\n", 1);
}

static int send_record(const char *message)
{
    struct sockaddr_un address;
    int descriptor;
    size_t length = strlen(message);
    int result;

    descriptor = socket(AF_UNIX, SOCK_DGRAM, 0);
    if (descriptor < 0)
        return -1;

    memset(&address, 0, sizeof(address));
    address.sun_family = AF_UNIX;
    (void)snprintf(address.sun_path, sizeof(address.sun_path), "/dev/log");
    result = connect(descriptor, (const struct sockaddr *)&address,
                     sizeof(address));
    if (result == 0)
        result = send(descriptor, message, length, 0) < 0 ? -1 : 0;
    (void)close(descriptor);
    return result;
}

void vsyslog(int priority, const char *format, va_list arguments)
{
    char body[SYSLOG_MESSAGE_MAX];
    char message[SYSLOG_MESSAGE_MAX];
    int effective_priority;
    int offset;

    effective_priority = priority;
    if ((effective_priority & LOG_FACMASK) == 0)
        effective_priority |= log_facility;
    if ((log_mask & LOG_MASK(LOG_PRI(effective_priority))) == 0)
        return;

    (void)vsnprintf(body, sizeof(body), format, arguments);
    offset = snprintf(message, sizeof(message), "<%d>",
                      effective_priority & (LOG_FACMASK | LOG_PRIMASK));
    if (log_ident != NULL && log_ident[0] != '\0')
        offset += snprintf(message + offset, sizeof(message) - (size_t)offset,
                           "%s%s", log_ident,
                           (log_option & LOG_PID) != 0 ? "" : ": ");
    if ((log_option & LOG_PID) != 0)
        offset += snprintf(message + offset, sizeof(message) - (size_t)offset,
                           "[%ld]: ", (long)getpid());
    (void)snprintf(message + offset, sizeof(message) - (size_t)offset,
                   "%s", body);

    if (send_record(message) != 0)
        write_fallback(message);
}

void syslog(int priority, const char *format, ...)
{
    va_list arguments;

    va_start(arguments, format);
    vsyslog(priority, format, arguments);
    va_end(arguments);
}
