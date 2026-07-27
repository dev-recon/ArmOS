/*
 * ArmOS
 * Copyright (c) 2026 Mohamed Ennassiri
 *
 * Licensed under the Apache License, Version 2.0.
 * See LICENSE for details.
 *
 * File: userland/include/xkbcommon/xkbcommon.h
 * Layer: Userland / Keyboard compatibility
 *
 * Responsibilities:
 * - Expose the xkbcommon API subset required by native Wayland clients.
 * - Define stable keyboard, modifier, layout, and keysym types.
 *
 * Notes:
 * - The ArmOS implementation parses the compositor's XKB text keymap.
 */

#ifndef ARMOS_XKBCOMMON_H
#define ARMOS_XKBCOMMON_H

#include <stddef.h>
#include <stdint.h>
#include <xkbcommon/xkbcommon-keysyms.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef uint32_t xkb_keycode_t;
typedef uint32_t xkb_keysym_t;
typedef uint32_t xkb_layout_index_t;
typedef uint32_t xkb_level_index_t;
typedef uint32_t xkb_mod_index_t;
typedef uint32_t xkb_mod_mask_t;

#define XKB_KEYCODE_INVALID ((xkb_keycode_t)0xffffffffu)
#define XKB_LAYOUT_INVALID  ((xkb_layout_index_t)0xffffffffu)
#define XKB_LEVEL_INVALID   ((xkb_level_index_t)0xffffffffu)
#define XKB_MOD_INVALID     ((xkb_mod_index_t)0xffffffffu)
#define XKB_KEYSYM_MAX      0x1fffffffu

#define XKB_MOD_NAME_SHIFT "Shift"
#define XKB_MOD_NAME_CAPS  "Lock"
#define XKB_MOD_NAME_CTRL  "Control"
#define XKB_MOD_NAME_ALT   "Mod1"
#define XKB_MOD_NAME_NUM   "Mod2"
#define XKB_MOD_NAME_LOGO  "Mod4"

struct xkb_context;
struct xkb_keymap;
struct xkb_state;

struct xkb_rule_names {
    const char *rules;
    const char *model;
    const char *layout;
    const char *variant;
    const char *options;
};

enum xkb_context_flags {
    XKB_CONTEXT_NO_FLAGS = 0
};

enum xkb_keymap_compile_flags {
    XKB_KEYMAP_COMPILE_NO_FLAGS = 0
};

enum xkb_keymap_format {
    XKB_KEYMAP_FORMAT_TEXT_V1 = 1
};

enum xkb_keysym_flags {
    XKB_KEYSYM_NO_FLAGS = 0,
    XKB_KEYSYM_CASE_INSENSITIVE = 1 << 0
};

enum xkb_key_direction {
    XKB_KEY_UP = 0,
    XKB_KEY_DOWN = 1
};

enum xkb_state_component {
    XKB_STATE_MODS_DEPRESSED = 1 << 0,
    XKB_STATE_MODS_LATCHED = 1 << 1,
    XKB_STATE_MODS_LOCKED = 1 << 2,
    XKB_STATE_MODS_EFFECTIVE = 1 << 3,
    XKB_STATE_LAYOUT_DEPRESSED = 1 << 4,
    XKB_STATE_LAYOUT_LATCHED = 1 << 5,
    XKB_STATE_LAYOUT_LOCKED = 1 << 6,
    XKB_STATE_LAYOUT_EFFECTIVE = 1 << 7
};

enum xkb_consumed_mode {
    XKB_CONSUMED_MODE_XKB = 0,
    XKB_CONSUMED_MODE_GTK = 1
};

struct xkb_context *xkb_context_new(enum xkb_context_flags flags);
struct xkb_context *xkb_context_ref(struct xkb_context *context);
void xkb_context_unref(struct xkb_context *context);

struct xkb_keymap *xkb_keymap_new_from_buffer(
    struct xkb_context *context, const char *buffer, size_t length,
    enum xkb_keymap_format format, enum xkb_keymap_compile_flags flags);
struct xkb_keymap *xkb_keymap_new_from_names(
    struct xkb_context *context, const struct xkb_rule_names *names,
    enum xkb_keymap_compile_flags flags);
struct xkb_keymap *xkb_keymap_ref(struct xkb_keymap *keymap);
void xkb_keymap_unref(struct xkb_keymap *keymap);
xkb_keycode_t xkb_keymap_min_keycode(struct xkb_keymap *keymap);
xkb_keycode_t xkb_keymap_max_keycode(struct xkb_keymap *keymap);
xkb_keycode_t xkb_keymap_key_by_name(struct xkb_keymap *keymap,
                                     const char *name);
xkb_mod_index_t xkb_keymap_mod_get_index(struct xkb_keymap *keymap,
                                         const char *name);
const char *xkb_keymap_mod_get_name(struct xkb_keymap *keymap,
                                    xkb_mod_index_t index);
xkb_layout_index_t xkb_keymap_num_layouts(struct xkb_keymap *keymap);
xkb_level_index_t xkb_keymap_num_levels_for_key(
    struct xkb_keymap *keymap, xkb_keycode_t key,
    xkb_layout_index_t layout);
int xkb_keymap_key_repeats(struct xkb_keymap *keymap, xkb_keycode_t key);
int xkb_keymap_key_get_syms_by_level(
    struct xkb_keymap *keymap, xkb_keycode_t key,
    xkb_layout_index_t layout, xkb_level_index_t level,
    const xkb_keysym_t **syms_out);
size_t xkb_keymap_key_get_mods_for_level(
    struct xkb_keymap *keymap, xkb_keycode_t key,
    xkb_layout_index_t layout, xkb_level_index_t level,
    xkb_mod_mask_t *masks_out, size_t masks_size);

struct xkb_state *xkb_state_new(struct xkb_keymap *keymap);
struct xkb_state *xkb_state_ref(struct xkb_state *state);
void xkb_state_unref(struct xkb_state *state);
enum xkb_state_component xkb_state_update_mask(
    struct xkb_state *state, xkb_mod_mask_t depressed_mods,
    xkb_mod_mask_t latched_mods, xkb_mod_mask_t locked_mods,
    xkb_layout_index_t depressed_layout,
    xkb_layout_index_t latched_layout,
    xkb_layout_index_t locked_layout);
enum xkb_state_component xkb_state_update_key(
    struct xkb_state *state, xkb_keycode_t key,
    enum xkb_key_direction direction);
xkb_mod_mask_t xkb_state_serialize_mods(
    struct xkb_state *state, enum xkb_state_component components);
int xkb_state_mod_index_is_active(
    struct xkb_state *state, xkb_mod_index_t index,
    enum xkb_state_component components);
xkb_layout_index_t xkb_state_key_get_layout(
    struct xkb_state *state, xkb_keycode_t key);
xkb_keysym_t xkb_state_key_get_one_sym(
    struct xkb_state *state, xkb_keycode_t key);
uint32_t xkb_state_key_get_utf32(struct xkb_state *state,
                                 xkb_keycode_t key);
int xkb_state_key_get_utf8(struct xkb_state *state, xkb_keycode_t key,
                           char *buffer, size_t size);
xkb_mod_mask_t xkb_state_key_get_consumed_mods(
    struct xkb_state *state, xkb_keycode_t key);
xkb_mod_mask_t xkb_state_key_get_consumed_mods2(
    struct xkb_state *state, xkb_keycode_t key,
    enum xkb_consumed_mode mode);

xkb_keysym_t xkb_keysym_from_name(const char *name,
                                  enum xkb_keysym_flags flags);
int xkb_keysym_get_name(xkb_keysym_t keysym, char *buffer, size_t size);
uint32_t xkb_keysym_to_utf32(xkb_keysym_t keysym);
xkb_keysym_t xkb_keysym_to_lower(xkb_keysym_t keysym);

#ifdef __cplusplus
}
#endif

#endif /* ARMOS_XKBCOMMON_H */
