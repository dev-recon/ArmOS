/*
 * ArmOS
 * Copyright (c) 2026 Mohamed Ennassiri
 *
 * Licensed under the Apache License, Version 2.0.
 * See LICENSE for details.
 *
 * File: userland/opt/pixman/test/pixman-test.c
 * Layer: Userland / graphics regression tests
 *
 * Responsibilities:
 * - Validate Pixman image creation and source-over composition.
 * - Validate clipping through a region containing two rectangles.
 * - Exercise independent Pixman thread-local state concurrently.
 */

#include <pixman.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <threads.h>

#define WIDTH 64
#define HEIGHT 48
#define THREAD_COUNT 4

struct worker {
    unsigned int index;
    int passed;
    struct worker_sync *sync;
};

struct worker_sync {
    mtx_t lock;
    cnd_t condition;
    unsigned int ready;
};

static int run_worker(void *opaque)
{
    struct worker *worker = opaque;
    uint32_t pixels[16 * 16];
    pixman_image_t *image;
    pixman_color_t color;

    memset(pixels, 0, sizeof(pixels));
    color.alpha = 0xffff;
    color.red = (uint16_t)(0x1000u * (worker->index + 1u));
    color.green = 0x4000;
    color.blue = 0x8000;

    image = pixman_image_create_bits(PIXMAN_a8r8g8b8, 16, 16,
                                     pixels, 16 * (int)sizeof(uint32_t));
    if (image == NULL)
        return 1;

    pixman_image_fill_rectangles(PIXMAN_OP_SRC, image, &color, 1,
                                 &(pixman_rectangle16_t){ 0, 0, 16, 16 });
    worker->passed = (pixels[0] != 0 && pixels[255] == pixels[0]);
    pixman_image_unref(image);

    if (mtx_lock(&worker->sync->lock) != thrd_success)
        return 1;
    ++worker->sync->ready;
    (void)cnd_signal(&worker->sync->condition);
    (void)mtx_unlock(&worker->sync->lock);
    return worker->passed ? 0 : 1;
}

int main(void)
{
    uint32_t destination[WIDTH * HEIGHT];
    uint32_t source[16 * 16];
    pixman_image_t *dst;
    pixman_image_t *src;
    pixman_region32_t clip;
    pixman_box32_t boxes[2] = {
        { 4, 4, 12, 12 },
        { 12, 8, 16, 16 },
    };
    thrd_t threads[THREAD_COUNT];
    struct worker workers[THREAD_COUNT];
    struct worker_sync sync;
    unsigned int i;

    memset(destination, 0, sizeof(destination));
    for (i = 0; i < 16u * 16u; ++i)
        source[i] = 0xffff8040u;

    dst = pixman_image_create_bits(PIXMAN_a8r8g8b8, WIDTH, HEIGHT,
                                   destination,
                                   WIDTH * (int)sizeof(uint32_t));
    src = pixman_image_create_bits(PIXMAN_a8r8g8b8, 16, 16, source,
                                   16 * (int)sizeof(uint32_t));
    if (dst == NULL || src == NULL) {
        puts("pixman-test: image creation failed");
        return 1;
    }

    pixman_region32_init_rects(&clip, boxes, 2);
    pixman_image_set_clip_region32(dst, &clip);
    pixman_image_composite32(PIXMAN_OP_OVER, src, NULL, dst,
                             0, 0, 0, 0, 0, 0, WIDTH, HEIGHT);

    if (destination[4 * WIDTH + 4] == 0 ||
        destination[8 * WIDTH + 12] == 0 ||
        destination[0] != 0 ||
        destination[18 * WIDTH + 18] != 0) {
        puts("pixman-test: clipping or composition failed");
        return 1;
    }

    pixman_region32_fini(&clip);
    pixman_image_unref(src);
    pixman_image_unref(dst);

    memset(workers, 0, sizeof(workers));
    memset(&sync, 0, sizeof(sync));
    if (mtx_init(&sync.lock, mtx_plain) != thrd_success ||
        cnd_init(&sync.condition) != thrd_success) {
        puts("pixman-test: C11 synchronization init failed");
        return 1;
    }
    for (i = 0; i < THREAD_COUNT; ++i) {
        workers[i].index = i;
        workers[i].sync = &sync;
        if (thrd_create(&threads[i], run_worker, &workers[i]) != thrd_success) {
            puts("pixman-test: thrd_create failed");
            return 1;
        }
    }
    if (mtx_lock(&sync.lock) != thrd_success) {
        puts("pixman-test: C11 mutex lock failed");
        return 1;
    }
    while (sync.ready != THREAD_COUNT) {
        if (cnd_wait(&sync.condition, &sync.lock) != thrd_success) {
            puts("pixman-test: C11 condition wait failed");
            return 1;
        }
    }
    (void)mtx_unlock(&sync.lock);
    for (i = 0; i < THREAD_COUNT; ++i) {
        int result;

        if (thrd_join(threads[i], &result) != thrd_success ||
            result != 0 || !workers[i].passed) {
            puts("pixman-test: threaded rendering failed");
            return 1;
        }
    }
    cnd_destroy(&sync.condition);
    mtx_destroy(&sync.lock);

    printf("pixman-test: ok version=%s threads=%u\n",
           pixman_version_string(), THREAD_COUNT);
    return 0;
}
