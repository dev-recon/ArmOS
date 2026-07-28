/*
 * ArmOS
 * Copyright (c) 2026 Mohamed Ennassiri
 *
 * Licensed under the Apache License, Version 2.0.
 * See LICENSE for details.
 *
 * File: userland/programs/socket-test/socket-test.c
 * Layer: Userland / local socket validation
 *
 * Responsibilities:
 * - Validate local stream socket pairs, readiness and shutdown semantics.
 * - Validate named bind, listen, connect, accept and endpoint lifetime.
 *
 * Notes:
 * - The same source is built for ARM32 and ARM64.
 * - Public POSIX names are retained only where required by the socket API.
 */

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>
#include <arm_os_abi.h>

#define COLOR_GREEN "\033[32m"
#define COLOR_RED   "\033[31m"
#define COLOR_RESET "\033[0m"

static int failures;
static int verbose = 1;
static int smp_pressure;
static unsigned char stream_payload[32768];
static pthread_mutex_t stress_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t stress_condition = PTHREAD_COND_INITIALIZER;
static int stress_started;

static int expect(int condition, const char *name, int value)
{
    if (condition) {
        if (verbose)
            printf(COLOR_GREEN "[OK]" COLOR_RESET " %s\n", name);
        return 0;
    }

    printf(COLOR_RED "[KO]" COLOR_RESET " %s (%d, errno=%d)\n",
           name, value, errno);
    failures++;
    return -1;
}

static int status_exited(int status, int code)
{
    return WIFEXITED(status) && WEXITSTATUS(status) == code;
}

static int send_descriptor(int socket_fd, int descriptor, unsigned char tag)
{
    unsigned char control[CMSG_SPACE(sizeof(int))];
    struct cmsghdr *header;
    struct iovec iov;
    struct msghdr message;

    memset(control, 0, sizeof(control));
    memset(&message, 0, sizeof(message));
    iov.iov_base = &tag;
    iov.iov_len = sizeof(tag);
    message.msg_iov = &iov;
    message.msg_iovlen = 1;
    message.msg_control = control;
    message.msg_controllen = sizeof(control);
    header = CMSG_FIRSTHDR(&message);
    header->cmsg_len = CMSG_LEN(sizeof(int));
    header->cmsg_level = SOL_SOCKET;
    header->cmsg_type = SCM_RIGHTS;
    memcpy(CMSG_DATA(header), &descriptor, sizeof(descriptor));
    return sendmsg(socket_fd, &message, 0) == (ssize_t)sizeof(tag) ? 0 : -1;
}

static int receive_descriptor(int socket_fd, int *descriptor,
                              unsigned char expected_tag)
{
    unsigned char control[CMSG_SPACE(sizeof(int))];
    unsigned char tag = 0u;
    struct cmsghdr *header;
    struct iovec iov;
    struct msghdr message;
    ssize_t count;

    memset(control, 0, sizeof(control));
    memset(&message, 0, sizeof(message));
    iov.iov_base = &tag;
    iov.iov_len = sizeof(tag);
    message.msg_iov = &iov;
    message.msg_iovlen = 1;
    message.msg_control = control;
    message.msg_controllen = sizeof(control);
    count = recvmsg(socket_fd, &message, MSG_CMSG_CLOEXEC);
    if (count != (ssize_t)sizeof(tag) || tag != expected_tag ||
        (message.msg_flags & MSG_CTRUNC) != 0)
        return -1;
    header = CMSG_FIRSTHDR(&message);
    if (!header || header->cmsg_level != SOL_SOCKET ||
        header->cmsg_type != SCM_RIGHTS ||
        header->cmsg_len != CMSG_LEN(sizeof(int)))
        return -1;
    memcpy(descriptor, CMSG_DATA(header), sizeof(*descriptor));
    return *descriptor >= 0 ? 0 : -1;
}

static void test_socket_pair(void)
{
    static const char message[] = "socketpair";
    struct pollfd descriptor;
    char buffer[32];
    int pair[2] = {-1, -1};

    if (expect(socketpair(AF_LOCAL, SOCK_STREAM, 0, pair) == 0,
               "local socketpair create", errno) < 0)
        return;

    expect(read(pair[1], buffer, 0) == 0,
           "zero-length read returns immediately", errno);
    descriptor.fd = pair[1];
    descriptor.events = POLLIN;
    descriptor.revents = 0;
    expect(poll(&descriptor, 1, 0) == 0,
           "empty socketpair is not readable", descriptor.revents);
    descriptor.fd = pair[0];
    descriptor.events = POLLOUT;
    descriptor.revents = 0;
    expect(poll(&descriptor, 1, 0) == 1 &&
               (descriptor.revents & POLLOUT) != 0,
           "socketpair is writable", descriptor.revents);
    expect(send(pair[0], message, sizeof(message), 0) ==
               (ssize_t)sizeof(message),
           "socketpair send", errno);
    descriptor.fd = pair[1];
    descriptor.events = POLLIN;
    descriptor.revents = 0;
    expect(poll(&descriptor, 1, 0) == 1 &&
               (descriptor.revents & POLLIN) != 0,
           "socketpair poll readable", descriptor.revents);
    memset(buffer, 0, sizeof(buffer));
    expect(recv(pair[1], buffer, sizeof(buffer), 0) ==
               (ssize_t)sizeof(message) &&
               memcmp(buffer, message, sizeof(message)) == 0,
           "socketpair preserves bytes", errno);
    expect(write(pair[1], "ok", 2) == 2,
           "socketpair is bidirectional", errno);
    memset(buffer, 0, sizeof(buffer));
    expect(read(pair[0], buffer, sizeof(buffer)) == 2 &&
               memcmp(buffer, "ok", 2) == 0,
           "reverse stream preserves bytes", errno);
    expect(shutdown(pair[0], SHUT_WR) == 0,
           "shutdown write side", errno);
    expect(read(pair[1], buffer, sizeof(buffer)) == 0,
           "shutdown delivers EOF", errno);

    close(pair[0]);
    close(pair[1]);
}

static void test_duplicated_endpoint(void)
{
    char buffer[8] = {0};
    int pair[2] = {-1, -1};
    int duplicate = -1;

    if (expect(socketpair(AF_LOCAL, SOCK_STREAM, 0, pair) == 0,
               "dup socketpair create", errno) < 0)
        return;
    duplicate = dup(pair[0]);
    if (expect(duplicate >= 0, "duplicate endpoint", errno) < 0)
        goto out;
    close(pair[0]);
    pair[0] = -1;
    expect(write(duplicate, "dup", 3) == 3,
           "duplicate survives original close", errno);
    expect(read(pair[1], buffer, sizeof(buffer)) == 3 &&
               memcmp(buffer, "dup", 3) == 0,
           "duplicate carries stream data", errno);
    close(duplicate);
    duplicate = -1;
    expect(read(pair[1], buffer, sizeof(buffer)) == 0,
           "last peer close delivers EOF", errno);

out:
    if (duplicate >= 0)
        close(duplicate);
    if (pair[0] >= 0)
        close(pair[0]);
    if (pair[1] >= 0)
        close(pair[1]);
}

static void test_parent_child_socket_pair(void)
{
    static const char parent_message[] = "from-parent";
    static const char child_message[] = "from-child";
    char buffer[32];
    int pair[2] = {-1, -1};
    int status = 0;
    pid_t child;

    if (expect(socketpair(AF_LOCAL, SOCK_STREAM, 0, pair) == 0,
               "parent/child socketpair create", errno) < 0)
        return;
    child = fork();
    if (child == 0) {
        ssize_t count;

        close(pair[0]);
        memset(buffer, 0, sizeof(buffer));
        count = read(pair[1], buffer, sizeof(buffer));
        if (count != (ssize_t)sizeof(parent_message) ||
            memcmp(buffer, parent_message, sizeof(parent_message)) != 0 ||
            write(pair[1], child_message, sizeof(child_message)) !=
                (ssize_t)sizeof(child_message))
            _exit(1);
        close(pair[1]);
        _exit(0);
    }
    if (expect(child > 0, "fork socket peer", child) < 0)
        goto out;

    close(pair[1]);
    pair[1] = -1;
    expect(write(pair[0], parent_message, sizeof(parent_message)) ==
               (ssize_t)sizeof(parent_message),
           "parent sends to child", errno);
    memset(buffer, 0, sizeof(buffer));
    expect(read(pair[0], buffer, sizeof(buffer)) ==
               (ssize_t)sizeof(child_message) &&
               memcmp(buffer, child_message, sizeof(child_message)) == 0,
           "parent receives from child", errno);
    waitpid(child, &status, 0);
    expect(status_exited(status, 0), "child socket peer exits cleanly", status);

out:
    if (pair[0] >= 0)
        close(pair[0]);
    if (pair[1] >= 0)
        close(pair[1]);
}

static void test_parent_child_descriptor_passing(void)
{
    int pair[2] = {-1, -1};
    int data_pipe[2] = {-1, -1};
    int status = 0;
    pid_t child;

    if (expect(socketpair(AF_LOCAL, SOCK_STREAM, 0, pair) == 0,
               "SCM_RIGHTS parent/child socket", errno) < 0)
        return;
    if (expect(pipe(data_pipe) == 0, "SCM_RIGHTS source pipe", errno) < 0)
        goto out;

    child = fork();
    if (child == 0) {
        unsigned char value = 0u;
        int received = -1;
        int descriptor_flags;

        close(pair[0]);
        close(data_pipe[0]);
        close(data_pipe[1]);
        if (receive_descriptor(pair[1], &received, 0x41u) < 0)
            _exit(1);
        descriptor_flags = fcntl(received, F_GETFD);
        if (descriptor_flags < 0 ||
            (descriptor_flags & FD_CLOEXEC) == 0 ||
            read(received, &value, sizeof(value)) != (ssize_t)sizeof(value) ||
            value != 0x7bu) {
            close(received);
            _exit(2);
        }
        close(received);
        close(pair[1]);
        _exit(0);
    }
    if (expect(child > 0, "fork SCM_RIGHTS receiver", child) < 0)
        goto out;

    close(pair[1]);
    pair[1] = -1;
    expect(send_descriptor(pair[0], data_pipe[0], 0x41u) == 0,
           "send descriptor to child", errno);
    close(data_pipe[0]);
    data_pipe[0] = -1;
    expect(write(data_pipe[1], "\x7b", 1) == 1,
           "transferred descriptor remains usable", errno);
    close(data_pipe[1]);
    data_pipe[1] = -1;
    waitpid(child, &status, 0);
    expect(status_exited(status, 0),
           "child receives descriptor with CLOEXEC", status);

out:
    if (data_pipe[0] >= 0)
        close(data_pipe[0]);
    if (data_pipe[1] >= 0)
        close(data_pipe[1]);
    if (pair[0] >= 0)
        close(pair[0]);
    if (pair[1] >= 0)
        close(pair[1]);
}

static void test_shared_memory_descriptor_passing(void)
{
    char name[32];
    int pair[2] = {-1, -1};
    int shared_fd = -1;
    int status = 0;
    volatile int *mapping = MAP_FAILED;
    pid_t child;

    snprintf(name, sizeof(name), "/socket-shm-%d", getpid());
    shm_unlink(name);
    if (expect(socketpair(AF_LOCAL, SOCK_STREAM, 0, pair) == 0,
               "shared buffer transfer socket", errno) < 0)
        return;
    child = fork();
    if (child == 0) {
        volatile int *child_mapping;
        int received = -1;

        close(pair[0]);
        if (receive_descriptor(pair[1], &received, 0x62u) < 0)
            _exit(1);
        child_mapping = mmap(NULL, 4096, PROT_READ | PROT_WRITE,
                             MAP_SHARED, received, 0);
        close(received);
        if (child_mapping == MAP_FAILED || child_mapping[0] != 0x1234)
            _exit(2);
        child_mapping[1] = 0x5678;
        munmap((void *)child_mapping, 4096);
        close(pair[1]);
        _exit(0);
    }
    if (expect(child > 0, "fork shared buffer receiver", child) < 0)
        goto out;
    close(pair[1]);
    pair[1] = -1;

    shared_fd = shm_open(name, O_CREAT | O_EXCL | O_RDWR, 0600);
    if (expect(shared_fd >= 0, "create descriptor-backed shared buffer",
               shared_fd) < 0)
        goto wait_child;
    if (expect(ftruncate(shared_fd, 4096) == 0,
               "size descriptor-backed shared buffer", errno) < 0)
        goto wait_child;
    mapping = mmap(NULL, 4096, PROT_READ | PROT_WRITE,
                   MAP_SHARED, shared_fd, 0);
    if (expect(mapping != MAP_FAILED, "mmap shared buffer", errno) < 0)
        goto wait_child;
    mapping[0] = 0x1234;
    mapping[1] = 0;
    expect(shm_unlink(name) == 0, "unlink keeps open shared descriptor", errno);
    expect(send_descriptor(pair[0], shared_fd, 0x62u) == 0,
           "SCM_RIGHTS transfers shared buffer", errno);
    close(shared_fd);
    shared_fd = -1;

wait_child:
    close(pair[0]);
    pair[0] = -1;
    waitpid(child, &status, 0);
    expect(status_exited(status, 0), "shared buffer receiver exits cleanly",
           status);
    if (mapping != MAP_FAILED)
        expect(mapping[1] == 0x5678, "shared buffer changes are visible",
               mapping[1]);

out:
    if (mapping != MAP_FAILED)
        munmap((void *)mapping, 4096);
    if (shared_fd >= 0)
        close(shared_fd);
    shm_unlink(name);
    if (pair[0] >= 0)
        close(pair[0]);
    if (pair[1] >= 0)
        close(pair[1]);
}

struct thread_exchange {
    int fd;
    int result;
};

static void *thread_exchange_main(void *opaque)
{
    static const char request[] = "thread-request";
    static const char response[] = "thread-response";
    struct thread_exchange *exchange = opaque;
    struct pollfd descriptor;
    char buffer[32];
    ssize_t count;

    descriptor.fd = exchange->fd;
    descriptor.events = POLLIN;
    descriptor.revents = 0;
    if (poll(&descriptor, 1, 1000) != 1 ||
        (descriptor.revents & POLLIN) == 0) {
        exchange->result = 1;
        return NULL;
    }
    memset(buffer, 0, sizeof(buffer));
    count = read(exchange->fd, buffer, sizeof(buffer));
    if (count != (ssize_t)sizeof(request) ||
        memcmp(buffer, request, sizeof(request)) != 0) {
        exchange->result = 2;
        return NULL;
    }
    if (write(exchange->fd, response, sizeof(response)) !=
        (ssize_t)sizeof(response)) {
        exchange->result = 3;
        return NULL;
    }
    exchange->result = 0;
    return NULL;
}

static void test_thread_socket_pair(void)
{
    static const char request[] = "thread-request";
    static const char response[] = "thread-response";
    struct thread_exchange exchange;
    char buffer[32];
    pthread_t thread;
    int pair[2] = {-1, -1};
    int result;

    if (expect(socketpair(AF_LOCAL, SOCK_STREAM, 0, pair) == 0,
               "thread socketpair create", errno) < 0)
        return;
    exchange.fd = pair[1];
    exchange.result = -1;
    result = pthread_create(&thread, NULL, thread_exchange_main, &exchange);
    if (expect(result == 0, "socket peer thread create", result) < 0)
        goto out;
    expect(write(pair[0], request, sizeof(request)) ==
               (ssize_t)sizeof(request),
           "main thread sends request", errno);
    memset(buffer, 0, sizeof(buffer));
    expect(read(pair[0], buffer, sizeof(buffer)) ==
               (ssize_t)sizeof(response) &&
               memcmp(buffer, response, sizeof(response)) == 0,
           "main thread receives response", errno);
    result = pthread_join(thread, NULL);
    expect(result == 0 && exchange.result == 0,
           "socket peer thread joins cleanly", exchange.result);

out:
    close(pair[0]);
    close(pair[1]);
}

struct descriptor_receiver {
    int socket_fd;
    int result;
};

static void *descriptor_receiver_main(void *opaque)
{
    struct descriptor_receiver *receiver = opaque;
    unsigned char value = 0u;
    int descriptor = -1;

    if (receive_descriptor(receiver->socket_fd, &descriptor, 0x52u) < 0) {
        receiver->result = 1;
        return NULL;
    }
    if (read(descriptor, &value, sizeof(value)) != (ssize_t)sizeof(value) ||
        value != 0x63u) {
        receiver->result = 2;
        close(descriptor);
        return NULL;
    }
    close(descriptor);
    receiver->result = 0;
    return NULL;
}

static void test_thread_descriptor_passing(void)
{
    struct descriptor_receiver receiver;
    pthread_t thread;
    int pair[2] = {-1, -1};
    int data_pipe[2] = {-1, -1};
    int result;

    if (expect(socketpair(AF_LOCAL, SOCK_STREAM, 0, pair) == 0,
               "SCM_RIGHTS thread socket", errno) < 0)
        return;
    if (expect(pipe(data_pipe) == 0, "SCM_RIGHTS thread pipe", errno) < 0)
        goto out;
    receiver.socket_fd = pair[1];
    receiver.result = -1;
    result = pthread_create(&thread, NULL, descriptor_receiver_main,
                            &receiver);
    if (expect(result == 0, "descriptor receiver thread", result) < 0)
        goto out;
    expect(send_descriptor(pair[0], data_pipe[0], 0x52u) == 0,
           "send descriptor between threads", errno);
    close(data_pipe[0]);
    data_pipe[0] = -1;
    expect(write(data_pipe[1], "\x63", 1) == 1,
           "thread descriptor data", errno);
    close(data_pipe[1]);
    data_pipe[1] = -1;
    result = pthread_join(thread, NULL);
    expect(result == 0 && receiver.result == 0,
           "thread receives transferred descriptor", receiver.result);

out:
    if (data_pipe[0] >= 0)
        close(data_pipe[0]);
    if (data_pipe[1] >= 0)
        close(data_pipe[1]);
    close(pair[0]);
    close(pair[1]);
}

struct stream_reader {
    int fd;
    size_t received;
    unsigned long checksum;
    int result;
};

static void *stream_reader_main(void *opaque)
{
    struct stream_reader *reader = opaque;
    unsigned char buffer[1024];

    while (reader->received < sizeof(stream_payload)) {
        ssize_t count = read(reader->fd, buffer, sizeof(buffer));

        if (count <= 0) {
            reader->result = 1;
            return NULL;
        }
        for (ssize_t index = 0; index < count; index++)
            reader->checksum += buffer[index];
        reader->received += (size_t)count;
    }
    reader->result = 0;
    return NULL;
}

static void test_thread_backpressure(void)
{
    struct stream_reader reader;
    pthread_t thread;
    unsigned long expected_checksum = 0;
    int pair[2] = {-1, -1};
    int result;

    for (size_t index = 0; index < sizeof(stream_payload); index++) {
        stream_payload[index] = (unsigned char)(index * 17u + 3u);
        expected_checksum += stream_payload[index];
    }
    if (expect(socketpair(AF_LOCAL, SOCK_STREAM, 0, pair) == 0,
               "backpressure socketpair create", errno) < 0)
        return;
    memset(&reader, 0, sizeof(reader));
    reader.fd = pair[1];
    result = pthread_create(&thread, NULL, stream_reader_main, &reader);
    if (expect(result == 0, "stream reader thread create", result) < 0)
        goto out;
    expect(write(pair[0], stream_payload, sizeof(stream_payload)) ==
               (ssize_t)sizeof(stream_payload),
           "stream crosses buffer capacity", errno);
    result = pthread_join(thread, NULL);
    expect(result == 0 && reader.result == 0 &&
               reader.received == sizeof(stream_payload) &&
               reader.checksum == expected_checksum,
           "threaded stream preserves all bytes", reader.result);

out:
    close(pair[0]);
    close(pair[1]);
}

struct stress_worker {
    unsigned int index;
    unsigned int iterations;
    int result;
};

struct stress_message {
    uint32_t worker;
    uint32_t iteration;
};

static void *stress_worker_main(void *opaque)
{
    struct stress_worker *worker = opaque;

    pthread_mutex_lock(&stress_lock);
    while (!stress_started)
        pthread_cond_wait(&stress_condition, &stress_lock);
    pthread_mutex_unlock(&stress_lock);

    for (unsigned int iteration = 0;
         iteration < worker->iterations;
         iteration++) {
        struct stress_message sent;
        struct stress_message received;
        struct pollfd descriptor;
        int pair[2] = {-1, -1};

        if (socketpair(AF_LOCAL, SOCK_STREAM, 0, pair) < 0) {
            worker->result = 1;
            return NULL;
        }
        sent.worker = worker->index;
        sent.iteration = iteration;
        if (write(pair[0], &sent, sizeof(sent)) != (ssize_t)sizeof(sent)) {
            worker->result = 2;
            goto iteration_failed;
        }
        descriptor.fd = pair[1];
        descriptor.events = POLLIN;
        descriptor.revents = 0;
        if (poll(&descriptor, 1, 1000) != 1 ||
            (descriptor.revents & POLLIN) == 0) {
            worker->result = 3;
            goto iteration_failed;
        }
        memset(&received, 0, sizeof(received));
        if (read(pair[1], &received, sizeof(received)) !=
                (ssize_t)sizeof(received) ||
            received.worker != sent.worker ||
            received.iteration != sent.iteration) {
            worker->result = 4;
            goto iteration_failed;
        }
        if (write(pair[1], &received, sizeof(received)) !=
                (ssize_t)sizeof(received) ||
            read(pair[0], &sent, sizeof(sent)) != (ssize_t)sizeof(sent) ||
            sent.worker != worker->index ||
            sent.iteration != iteration) {
            worker->result = 5;
            goto iteration_failed;
        }
        if ((iteration & 7u) == 0u) {
            unsigned char value = (unsigned char)(iteration + worker->index);
            unsigned char copied = 0u;
            int data_pipe[2] = {-1, -1};
            int transferred = -1;

            if (pipe(data_pipe) < 0 ||
                send_descriptor(pair[0], data_pipe[0], 0x73u) < 0 ||
                receive_descriptor(pair[1], &transferred, 0x73u) < 0 ||
                write(data_pipe[1], &value, sizeof(value)) !=
                    (ssize_t)sizeof(value) ||
                read(transferred, &copied, sizeof(copied)) !=
                    (ssize_t)sizeof(copied) ||
                copied != value) {
                if (data_pipe[0] >= 0)
                    close(data_pipe[0]);
                if (data_pipe[1] >= 0)
                    close(data_pipe[1]);
                if (transferred >= 0)
                    close(transferred);
                worker->result = 6;
                goto iteration_failed;
            }
            close(data_pipe[0]);
            close(data_pipe[1]);
            close(transferred);
        }
        if ((iteration & 15u) == 0u) {
            struct sockaddr_un address;
            int named;

            memset(&address, 0, sizeof(address));
            address.sun_family = AF_LOCAL;
            snprintf(address.sun_path, sizeof(address.sun_path),
                     "/tmp/socket-stress-%d-%u-%u", getpid(),
                     worker->index, iteration);
            named = socket(AF_LOCAL, SOCK_STREAM, 0);
            if (named < 0 ||
                bind(named, (struct sockaddr *)&address,
                     sizeof(address)) < 0) {
                if (named >= 0)
                    close(named);
                worker->result = 7;
                goto iteration_failed;
            }
            close(named);
        }
        close(pair[0]);
        close(pair[1]);
        continue;

iteration_failed:
        close(pair[0]);
        close(pair[1]);
        return NULL;
    }

    worker->result = 0;
    return NULL;
}

static void test_smp_pressure(void)
{
    enum { MAX_STRESS_THREADS = 12 };
    struct stress_worker workers[MAX_STRESS_THREADS];
    pthread_t threads[MAX_STRESS_THREADS];
    unsigned int thread_count = smp_pressure ? MAX_STRESS_THREADS : 4u;
    unsigned int iterations = smp_pressure ? 256u : 32u;
    unsigned int created = 0u;
    int result = 0;

    stress_started = 0;
    memset(workers, 0, sizeof(workers));
    for (unsigned int index = 0; index < thread_count; index++) {
        workers[index].index = index;
        workers[index].iterations = iterations;
        result = pthread_create(&threads[index], NULL,
                                stress_worker_main, &workers[index]);
        if (result != 0)
            break;
        created++;
    }

    pthread_mutex_lock(&stress_lock);
    stress_started = 1;
    pthread_cond_broadcast(&stress_condition);
    pthread_mutex_unlock(&stress_lock);

    expect(created == thread_count, "pressure threads created", result);
    for (unsigned int index = 0; index < created; index++) {
        int join_result = pthread_join(threads[index], NULL);

        if (join_result != 0 || workers[index].result != 0)
            result = workers[index].result != 0 ?
                     workers[index].result : join_result;
    }
    expect(created == thread_count && result == 0,
           smp_pressure ? "SMP socket pressure" : "socket concurrency",
           result);
}

static void test_named_socket(void)
{
    static const char client_message[] = "wayland-client";
    static const char server_message[] = "wayland-server";
    struct sockaddr_un address;
    char buffer[32];
    int server = -1;
    int duplicate = -1;
    int accepted = -1;
    int rebound = -1;
    int status = 0;
    pid_t pid;

    memset(&address, 0, sizeof(address));
    address.sun_family = AF_LOCAL;
    snprintf(address.sun_path, sizeof(address.sun_path),
             "/tmp/wayland-test-%d", getpid());

    server = socket(AF_LOCAL, SOCK_STREAM, 0);
    if (expect(server >= 0, "named local socket create", errno) < 0)
        return;
    if (expect(bind(server, (struct sockaddr *)&address, sizeof(address)) == 0,
               "pathname bind", errno) < 0)
        goto out;
    expect(listen(server, 4) == 0, "listen", errno);

    duplicate = socket(AF_LOCAL, SOCK_STREAM, 0);
    if (expect(duplicate >= 0, "duplicate socket create", errno) == 0) {
        errno = 0;
        expect(bind(duplicate, (struct sockaddr *)&address, sizeof(address)) < 0 &&
                   errno == EADDRINUSE,
               "duplicate pathname is rejected", errno);
        close(duplicate);
        duplicate = -1;
    }

    pid = fork();
    if (pid == 0) {
        int client = socket(AF_LOCAL, SOCK_STREAM, 0);
        ssize_t count;

        if (client < 0 ||
            connect(client, (struct sockaddr *)&address, sizeof(address)) < 0 ||
            write(client, client_message, sizeof(client_message)) !=
                (ssize_t)sizeof(client_message))
            _exit(1);
        memset(buffer, 0, sizeof(buffer));
        count = read(client, buffer, sizeof(buffer));
        close(client);
        _exit(count == (ssize_t)sizeof(server_message) &&
              memcmp(buffer, server_message, sizeof(server_message)) == 0 ?
              0 : 2);
    }
    if (expect(pid > 0, "fork client", pid) == 0) {
        accepted = accept(server, NULL, NULL);
        if (expect(accepted >= 0, "accept", errno) == 0) {
            memset(buffer, 0, sizeof(buffer));
            expect(read(accepted, buffer, sizeof(buffer)) ==
                       (ssize_t)sizeof(client_message) &&
                       memcmp(buffer, client_message,
                              sizeof(client_message)) == 0,
                   "client to server stream", errno);
            expect(write(accepted, server_message, sizeof(server_message)) ==
                       (ssize_t)sizeof(server_message),
                   "server to client stream", errno);
            close(accepted);
            accepted = -1;
        }
        waitpid(pid, &status, 0);
        expect(status_exited(status, 0),
               "connected client exits cleanly", status);
    }

out:
    if (accepted >= 0)
        close(accepted);
    if (duplicate >= 0)
        close(duplicate);
    if (server >= 0)
        close(server);

    rebound = socket(AF_LOCAL, SOCK_STREAM, 0);
    if (expect(rebound >= 0, "pathname reuse socket", errno) == 0) {
        expect(bind(rebound, (struct sockaddr *)&address, sizeof(address)) == 0,
               "close releases pathname", errno);
        close(rebound);
    }
}

int main(int argc, char **argv)
{
    for (int index = 1; index < argc; index++) {
        if (strcmp(argv[index], "-q") == 0) {
            verbose = 0;
        } else if (strcmp(argv[index], "--smp") == 0) {
            smp_pressure = 1;
        } else {
            printf("usage: socket-test [-q] [--smp]\n");
            return 2;
        }
    }

    test_socket_pair();
    test_duplicated_endpoint();
    test_parent_child_socket_pair();
    test_parent_child_descriptor_passing();
    test_shared_memory_descriptor_passing();
    test_thread_socket_pair();
    test_thread_descriptor_passing();
    test_thread_backpressure();
    test_smp_pressure();
    test_named_socket();

    if (failures == 0) {
        printf("socket-test: all tests passed\n");
        return 0;
    }

    printf("socket-test: %d failure(s)\n", failures);
    return 1;
}
