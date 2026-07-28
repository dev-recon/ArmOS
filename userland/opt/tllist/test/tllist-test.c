/*
 * ArmOS
 * Copyright (c) 2026 Mohamed Ennassiri
 *
 * Licensed under the Apache License, Version 2.0.
 * See LICENSE for details.
 *
 * File: userland/opt/tllist/test/tllist-test.c
 * Layer: Userland / container regression tests
 *
 * Responsibilities:
 * - Validate insertion, stable sorting, iteration and removal with tllist.
 */

#include <stdio.h>
#include <tllist.h>

static int ascending(int left, int right)
{
    return left - right;
}

int main(void)
{
    tll(int) values = tll_init();
    int expected[] = { 1, 2, 3, 4 };
    unsigned int index = 0;

    tll_push_back(values, 3);
    tll_push_front(values, 2);
    tll_push_back(values, 4);
    tll_push_front(values, 1);
    tll_sort(values, ascending);

    tll_foreach(values, item) {
        if (index >= sizeof(expected) / sizeof(expected[0]) ||
            item->item != expected[index++]) {
            puts("tllist-test: sort failed");
            return 1;
        }
        if ((item->item & 1) == 0)
            tll_remove(values, item);
    }

    if (index != 4 || tll_length(values) != 2 ||
        tll_front(values) != 1 || tll_back(values) != 3) {
        puts("tllist-test: list integrity failed");
        return 1;
    }

    tll_free(values);
    if (tll_length(values) != 0) {
        puts("tllist-test: cleanup failed");
        return 1;
    }

    puts("tllist-test: ok");
    return 0;
}
