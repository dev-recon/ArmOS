/*
 * ArmOS
 * Copyright (c) 2026 Mohamed Ennassiri
 *
 * Licensed under the Apache License, Version 2.0.
 * See LICENSE for details.
 *
 * File: userland/include/xkbcommon/xkbcommon-compose.h
 * Layer: Userland / Keyboard compatibility
 *
 * Responsibilities:
 * - Expose compose-table lifecycle and compose-state interfaces.
 * - Allow Wayland applications to operate when no locale rules are present.
 */

#ifndef ARMOS_XKBCOMMON_COMPOSE_H
#define ARMOS_XKBCOMMON_COMPOSE_H

#include <xkbcommon/xkbcommon.h>

#ifdef __cplusplus
extern "C" {
#endif

struct xkb_compose_table;
struct xkb_compose_state;

enum xkb_compose_compile_flags {
    XKB_COMPOSE_COMPILE_NO_FLAGS = 0
};

enum xkb_compose_state_flags {
    XKB_COMPOSE_STATE_NO_FLAGS = 0
};

enum xkb_compose_feed_result {
    XKB_COMPOSE_FEED_IGNORED = 0,
    XKB_COMPOSE_FEED_ACCEPTED = 1
};

enum xkb_compose_status {
    XKB_COMPOSE_NOTHING = 0,
    XKB_COMPOSE_COMPOSING = 1,
    XKB_COMPOSE_COMPOSED = 2,
    XKB_COMPOSE_CANCELLED = 3
};

struct xkb_compose_table *xkb_compose_table_new_from_locale(
    struct xkb_context *context, const char *locale,
    enum xkb_compose_compile_flags flags);
struct xkb_compose_table *xkb_compose_table_ref(
    struct xkb_compose_table *table);
void xkb_compose_table_unref(struct xkb_compose_table *table);
struct xkb_compose_state *xkb_compose_state_new(
    struct xkb_compose_table *table, enum xkb_compose_state_flags flags);
struct xkb_compose_state *xkb_compose_state_ref(
    struct xkb_compose_state *state);
void xkb_compose_state_unref(struct xkb_compose_state *state);
enum xkb_compose_feed_result xkb_compose_state_feed(
    struct xkb_compose_state *state, xkb_keysym_t keysym);
void xkb_compose_state_reset(struct xkb_compose_state *state);
enum xkb_compose_status xkb_compose_state_get_status(
    struct xkb_compose_state *state);
int xkb_compose_state_get_utf8(struct xkb_compose_state *state,
                               char *buffer, size_t size);

#ifdef __cplusplus
}
#endif

#endif /* ARMOS_XKBCOMMON_COMPOSE_H */
