/*
 * ArmOS
 * Copyright (c) 2026 Mohamed Ennassiri
 *
 * Licensed under the Apache License, Version 2.0.
 * See LICENSE for details.
 *
 * File: userland/programs/wayland-test/wayland-test.c
 * Layer: Userland / graphical service validation
 *
 * Responsibilities:
 * - Exercise registry discovery against the ArmOS Wayland server.
 * - Pass an SHM descriptor and construct a real wl_buffer and wl_surface.
 * - Verify buffer release and frame completion events end to end.
 *
 * Notes:
 * - Run armos-wlcomp --headless first for automated testing.
 * - This deliberately uses the wire protocol without an external client library.
 */

#include <arm_os_abi.h>
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#define TEST_SOCKET_PATH "/tmp/wayland-0"
#define TEST_WIDTH       320u
#define TEST_HEIGHT      180u
#define TEST_STRIDE      (TEST_WIDTH * 4u)
#define TEST_SHM_SIZE    (TEST_STRIDE * TEST_HEIGHT)

struct test_event {
    uint32_t object_id;
    uint16_t opcode;
    uint8_t payload[1024];
    size_t payload_size;
};

static uint32_t test_align(uint32_t value)
{
    return (value + 3u) & ~3u;
}

static void test_store_u32(uint8_t *destination, uint32_t value)
{
    memcpy(destination, &value, sizeof(value));
}

static uint32_t test_load_u32(const uint8_t *source)
{
    uint32_t value;

    memcpy(&value, source, sizeof(value));
    return value;
}

static int test_write_full(int fd, const void *data, size_t size)
{
    const uint8_t *cursor = data;
    size_t done = 0;

    while (done < size) {
        ssize_t count = write(fd, cursor + done, size - done);

        if (count < 0 && errno == EINTR)
            continue;
        if (count <= 0)
            return -1;
        done += (size_t)count;
    }
    return 0;
}

static int test_read_full(int fd, void *data, size_t size)
{
    uint8_t *cursor = data;
    size_t done = 0;

    while (done < size) {
        ssize_t count = read(fd, cursor + done, size - done);

        if (count < 0 && errno == EINTR)
            continue;
        if (count <= 0)
            return -1;
        done += (size_t)count;
    }
    return 0;
}

static int test_send_words(int fd, uint32_t object_id, uint16_t opcode,
                           const uint32_t *words, size_t word_count)
{
    uint8_t message[128];
    size_t size = 8u + word_count * 4u;

    if (size > sizeof(message))
        return -1;
    test_store_u32(message, object_id);
    test_store_u32(message + 4u, ((uint32_t)size << 16) | opcode);
    if (word_count)
        memcpy(message + 8u, words, word_count * 4u);
    return test_write_full(fd, message, size);
}

static int test_send_bind(int fd, uint32_t registry_id, uint32_t name,
                          const char *interface_name, uint32_t new_id)
{
    uint8_t message[128];
    uint32_t string_size = (uint32_t)strlen(interface_name) + 1u;
    uint32_t padded = test_align(string_size);
    uint32_t size = 8u + 4u + 4u + padded + 4u + 4u;

    memset(message, 0, size);
    test_store_u32(message, registry_id);
    test_store_u32(message + 4u, (size << 16) | 0u);
    test_store_u32(message + 8u, name);
    test_store_u32(message + 12u, string_size);
    memcpy(message + 16u, interface_name, string_size);
    test_store_u32(message + 16u + padded, 1u);
    test_store_u32(message + 20u + padded, new_id);
    return test_write_full(fd, message, size);
}

static int test_send_pool(int fd, int shm_fd)
{
    uint8_t message[16];
    uint8_t control[CMSG_SPACE(sizeof(int))];
    struct cmsghdr *header;
    struct iovec iov;
    struct msghdr packet;

    test_store_u32(message, 5u);
    test_store_u32(message + 4u, (16u << 16) | 0u);
    test_store_u32(message + 8u, 7u);
    test_store_u32(message + 12u, TEST_SHM_SIZE);
    memset(control, 0, sizeof(control));
    memset(&packet, 0, sizeof(packet));
    iov.iov_base = message;
    iov.iov_len = sizeof(message);
    packet.msg_iov = &iov;
    packet.msg_iovlen = 1u;
    packet.msg_control = control;
    packet.msg_controllen = sizeof(control);
    header = CMSG_FIRSTHDR(&packet);
    header->cmsg_len = CMSG_LEN(sizeof(int));
    header->cmsg_level = SOL_SOCKET;
    header->cmsg_type = SCM_RIGHTS;
    memcpy(CMSG_DATA(header), &shm_fd, sizeof(shm_fd));
    return sendmsg(fd, &packet, 0) == (ssize_t)sizeof(message) ? 0 : -1;
}

static int test_receive_event(int fd, struct test_event *event)
{
    uint8_t header[8];
    uint32_t word;
    uint32_t size;

    if (test_read_full(fd, header, sizeof(header)) < 0)
        return -1;
    event->object_id = test_load_u32(header);
    word = test_load_u32(header + 4u);
    event->opcode = (uint16_t)(word & 0xffffu);
    size = word >> 16;
    if (size < 8u || (size & 3u) != 0 ||
        size - 8u > sizeof(event->payload))
        return -1;
    event->payload_size = size - 8u;
    return test_read_full(fd, event->payload, event->payload_size);
}

static int test_connect(const char *path)
{
    struct sockaddr_un address;
    int fd;

    if (strlen(path) >= sizeof(address.sun_path))
        return -1;
    fd = socket(AF_LOCAL, SOCK_STREAM, 0);
    if (fd < 0)
        return -1;
    memset(&address, 0, sizeof(address));
    address.sun_family = AF_LOCAL;
    memcpy(address.sun_path, path, strlen(path) + 1u);
    if (connect(fd, (struct sockaddr *)&address, sizeof(address)) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

static int test_discover_globals(int fd, uint32_t *compositor_name,
                                 uint32_t *shm_name, uint32_t *seat_name,
                                 uint32_t *xdg_shell_name)
{
    uint32_t registry = 2u;
    uint32_t callback = 3u;
    int synchronized = 0;

    if (test_send_words(fd, 1u, 1u, &registry, 1u) < 0 ||
        test_send_words(fd, 1u, 0u, &callback, 1u) < 0)
        return -1;
    while (!synchronized) {
        struct test_event event;

        if (test_receive_event(fd, &event) < 0)
            return -1;
        if (event.object_id == 2u && event.opcode == 0u &&
            event.payload_size >= 12u) {
            uint32_t name = test_load_u32(event.payload);
            uint32_t length = test_load_u32(event.payload + 4u);
            const char *interface_name =
                (const char *)(event.payload + 8u);

            if (length == 0u ||
                8u + test_align(length) + 4u > event.payload_size)
                return -1;
            if (strcmp(interface_name, "wl_compositor") == 0)
                *compositor_name = name;
            else if (strcmp(interface_name, "wl_shm") == 0)
                *shm_name = name;
            else if (strcmp(interface_name, "wl_seat") == 0)
                *seat_name = name;
            else if (strcmp(interface_name, "xdg_wm_base") == 0)
                *xdg_shell_name = name;
        } else if (event.object_id == 3u && event.opcode == 0u) {
            synchronized = 1;
        }
    }
    return *compositor_name && *shm_name && *seat_name &&
           *xdg_shell_name ? 0 : -1;
}

static int test_bind_globals(int fd, uint32_t compositor_name,
                             uint32_t shm_name, uint32_t seat_name,
                             uint32_t xdg_shell_name)
{
    int formats = 0;
    int seat = 0;

    if (test_send_bind(fd, 2u, compositor_name, "wl_compositor", 4u) < 0 ||
        test_send_bind(fd, 2u, shm_name, "wl_shm", 5u) < 0 ||
        test_send_bind(fd, 2u, seat_name, "wl_seat", 6u) < 0 ||
        test_send_bind(fd, 2u, xdg_shell_name, "xdg_wm_base", 13u) < 0)
        return -1;
    while (formats < 2 || !seat) {
        struct test_event event;

        if (test_receive_event(fd, &event) < 0)
            return -1;
        if (event.object_id == 5u && event.opcode == 0u &&
            event.payload_size == 4u) {
            uint32_t format = test_load_u32(event.payload);

            if (format == 0u || format == 1u)
                formats++;
        } else if (event.object_id == 6u && event.opcode == 0u &&
                   event.payload_size == 4u) {
            if (test_load_u32(event.payload) != 3u)
                return -1;
            seat = 1;
        }
    }
    return 0;
}

static uint8_t test_glyph_row(char character, uint32_t row)
{
    static const uint8_t letters[26][7] = {
        {14,17,17,31,17,17,17}, {30,17,17,30,17,17,30},
        {14,17,16,16,16,17,14}, {30,17,17,17,17,17,30},
        {31,16,16,30,16,16,31}, {31,16,16,30,16,16,16},
        {14,17,16,23,17,17,15}, {17,17,17,31,17,17,17},
        {14,4,4,4,4,4,14}, {7,2,2,2,18,18,12},
        {17,18,20,24,20,18,17}, {16,16,16,16,16,16,31},
        {17,27,21,21,17,17,17}, {17,25,21,19,17,17,17},
        {14,17,17,17,17,17,14}, {30,17,17,30,16,16,16},
        {14,17,17,17,21,18,13}, {30,17,17,30,20,18,17},
        {15,16,16,14,1,1,30}, {31,4,4,4,4,4,4},
        {17,17,17,17,17,17,14}, {17,17,17,17,17,10,4},
        {17,17,17,21,21,21,10}, {17,17,10,4,10,17,17},
        {17,17,10,4,4,4,4}, {31,1,2,4,8,16,31}
    };
    static const uint8_t digits[10][7] = {
        {14,17,19,21,25,17,14}, {4,12,4,4,4,4,14},
        {14,17,1,2,4,8,31}, {30,1,1,14,1,1,30},
        {2,6,10,18,31,2,2}, {31,16,16,30,1,1,30},
        {14,16,16,30,17,17,14}, {31,1,2,4,8,8,8},
        {14,17,17,14,17,17,14}, {14,17,17,15,1,1,14}
    };

    if (row >= 7u)
        return 0u;
    if (character >= 'a' && character <= 'z')
        character = (char)(character - 'a' + 'A');
    if (character >= 'A' && character <= 'Z')
        return letters[(uint32_t)(character - 'A')][row];
    if (character >= '0' && character <= '9')
        return digits[(uint32_t)(character - '0')][row];
    if (character == '-')
        return row == 3u ? 31u : 0u;
    if (character == '.')
        return row == 6u ? 4u : 0u;
    return 0u;
}

static void test_draw_text(uint32_t *pixels, uint32_t x, uint32_t y,
                           const char *text, uint32_t color)
{
    uint32_t start_x = x;

    while (*text && y + 14u < TEST_HEIGHT) {
        if (x + 12u >= TEST_WIDTH) {
            x = start_x;
            y += 20u;
        }
        for (uint32_t row = 0; row < 7u; row++) {
            uint8_t bits = test_glyph_row(*text, row);

            for (uint32_t column = 0; column < 5u; column++) {
                if ((bits & (1u << (4u - column))) != 0u) {
                    pixels[(y + row * 2u) * TEST_WIDTH + x + column * 2u] =
                        color;
                    pixels[(y + row * 2u) * TEST_WIDTH + x + column * 2u + 1u] =
                        color;
                    pixels[(y + row * 2u + 1u) * TEST_WIDTH + x + column * 2u] =
                        color;
                    pixels[(y + row * 2u + 1u) * TEST_WIDTH + x + column * 2u + 1u] =
                        color;
                }
            }
        }
        x += 12u;
        text++;
    }
}

static void test_draw(uint32_t *pixels, const char *text)
{
    for (uint32_t y = 0; y < TEST_HEIGHT; y++)
        for (uint32_t x = 0; x < TEST_WIDTH; x++)
            pixels[y * TEST_WIDTH + x] = 0xfffbfbfdu;
    test_draw_text(pixels, 24u, 24u, "ARMOS WAYLAND DEMO", 0xff202124u);
    test_draw_text(pixels, 24u, 54u, "MOVE TITLE - CLICK AND TYPE",
                   0xff60646cu);
    for (uint32_t y = 88u; y < 160u; y++)
        for (uint32_t x = 20u; x < TEST_WIDTH - 20u; x++)
            pixels[y * TEST_WIDTH + x] = 0xffffffffu;
    test_draw_text(pixels, 34u, 110u, text, 0xff15171au);
}

static char test_key_to_char(uint32_t key, bool shift)
{
    static const char normal[128] = {
        [2]='&',[3]='e',[4]='"',[5]='\'',[6]='(',[7]='-',[8]='e',
        [9]='_',[10]='c',[11]='a',[16]='a',[17]='z',[18]='e',[19]='r',
        [20]='t',[21]='y',[22]='u',[23]='i',[24]='o',[25]='p',[30]='q',
        [31]='s',[32]='d',[33]='f',[34]='g',[35]='h',[36]='j',[37]='k',
        [38]='l',[39]='m',[44]='w',[45]='x',[46]='c',[47]='v',[48]='b',
        [49]='n',[50]=',',[51]=';',[52]=':',[53]='=',[57]=' '
    };
    char result = key < 128u ? normal[key] : 0;

    if (shift && result >= 'a' && result <= 'z')
        result = (char)(result - 'a' + 'A');
    if (shift && key >= 2u && key <= 11u)
        result = (char)('1' + (key - 2u + 9u) % 10u);
    return result;
}

static int test_surface_roundtrip(int fd, bool interactive)
{
    uint32_t create_buffer[] = {
        8u, 0u, TEST_WIDTH, TEST_HEIGHT, TEST_STRIDE, 0u
    };
    uint32_t surface_id = 9u;
    uint32_t attach[] = {8u, 0u, 0u};
    uint32_t damage[] = {0u, 0u, TEST_WIDTH, TEST_HEIGHT};
    uint32_t callback_id = 10u;
    char shm_name[40];
    uint32_t *mapping = MAP_FAILED;
    int shm_fd = -1;
    int released = 0;
    int frame_done = 0;
    int xdg_surface_configured = 0;
    int xdg_toplevel_configured = 0;
    int result = -1;

    snprintf(shm_name, sizeof(shm_name), "/wayland-test-%d", getpid());
    shm_unlink(shm_name);
    shm_fd = shm_open(shm_name, O_CREAT | O_EXCL | O_RDWR | O_CLOEXEC,
                      0600);
    if (shm_fd < 0)
        goto out;
    if (ftruncate(shm_fd, TEST_SHM_SIZE) < 0)
        goto out;
    mapping = mmap(NULL, TEST_SHM_SIZE, PROT_READ | PROT_WRITE,
                   MAP_SHARED, shm_fd, 0);
    if (mapping == MAP_FAILED)
        goto out;
    char text[96] = "";
    size_t text_length = 0u;
    bool shift = false;

    test_draw(mapping, text);

    if ((interactive &&
         (test_send_words(fd, 6u, 0u, (uint32_t[]){11u}, 1u) < 0 ||
          test_send_words(fd, 6u, 1u, (uint32_t[]){12u}, 1u) < 0)) ||
        test_send_pool(fd, shm_fd) < 0 ||
        test_send_words(fd, 7u, 0u, create_buffer, 6u) < 0 ||
        test_send_words(fd, 4u, 0u, &surface_id, 1u) < 0 ||
        test_send_words(fd, 13u, 2u, (uint32_t[]){14u, 9u}, 2u) < 0 ||
        test_send_words(fd, 14u, 1u, (uint32_t[]){15u}, 1u) < 0 ||
        test_send_words(fd, 9u, 1u, attach, 3u) < 0 ||
        test_send_words(fd, 9u, 2u, damage, 4u) < 0 ||
        test_send_words(fd, 9u, 3u, &callback_id, 1u) < 0 ||
        test_send_words(fd, 9u, 6u, NULL, 0u) < 0)
        goto out;

    while (!released || !frame_done || !xdg_surface_configured ||
           !xdg_toplevel_configured) {
        struct test_event event;

        if (test_receive_event(fd, &event) < 0)
            goto out;
        if (event.object_id == 8u && event.opcode == 0u &&
            event.payload_size == 0u)
            released = 1;
        if (event.object_id == 10u && event.opcode == 0u &&
            event.payload_size == 4u)
            frame_done = 1;
        if (event.object_id == 14u && event.opcode == 0u &&
            event.payload_size == 4u) {
            uint32_t serial = test_load_u32(event.payload);

            xdg_surface_configured = 1;
            if (test_send_words(fd, 14u, 4u, &serial, 1u) < 0)
                goto out;
        }
        if (event.object_id == 15u && event.opcode == 0u &&
            event.payload_size == 12u)
            xdg_toplevel_configured = 1;
    }
    result = 0;
    while (interactive) {
        struct test_event event;

        if (test_receive_event(fd, &event) < 0) {
            result = -1;
            break;
        }
        if (event.object_id == 15u && event.opcode == 1u &&
            event.payload_size == 0u)
            break;
        if (event.object_id == 12u && event.opcode == 3u &&
            event.payload_size == 16u) {
            uint32_t key = test_load_u32(event.payload + 8u);
            uint32_t state = test_load_u32(event.payload + 12u);
            char character;

            if (key == 42u || key == 54u) {
                shift = state != 0u;
                continue;
            }
            if (state == 0u)
                continue;
            if (key == 14u) {
                if (text_length)
                    text[--text_length] = '\0';
            } else if (key == 28u) {
                if (text_length + 1u < sizeof(text))
                    text[text_length++] = ' ';
            } else {
                character = test_key_to_char(key, shift);
                if (character && text_length + 1u < sizeof(text)) {
                    text[text_length++] = character;
                    text[text_length] = '\0';
                }
            }
            test_draw(mapping, text);
            if (test_send_words(fd, 9u, 1u, attach, 3u) < 0 ||
                test_send_words(fd, 9u, 2u, damage, 4u) < 0 ||
                test_send_words(fd, 9u, 6u, NULL, 0u) < 0) {
                result = -1;
                break;
            }
        }
    }

out:
    if (mapping != MAP_FAILED)
        munmap(mapping, TEST_SHM_SIZE);
    if (shm_fd >= 0)
        close(shm_fd);
    shm_unlink(shm_name);
    return result;
}

int main(int argc, char **argv)
{
    bool interactive = argc > 1 && strcmp(argv[1], "--demo") == 0;
    const char *socket_path = argc > (interactive ? 2 : 1) ?
        argv[interactive ? 2 : 1] : TEST_SOCKET_PATH;
    uint32_t compositor_name = 0u;
    uint32_t shm_name = 0u;
    uint32_t seat_name = 0u;
    uint32_t xdg_shell_name = 0u;
    int fd;

    fd = test_connect(socket_path);
    if (fd < 0) {
        perror("wayland-test: connect");
        return 1;
    }
    if (test_discover_globals(fd, &compositor_name, &shm_name,
                              &seat_name, &xdg_shell_name) < 0 ||
        test_bind_globals(fd, compositor_name, shm_name, seat_name,
                          xdg_shell_name) < 0 ||
        test_surface_roundtrip(fd, interactive) < 0) {
        fprintf(stderr, "WAYLAND_TEST_FAILED\n");
        close(fd);
        return 1;
    }
    close(fd);
    printf("WAYLAND_TEST_OK registry shm xdg-shell surface frame\n");
    return 0;
}
