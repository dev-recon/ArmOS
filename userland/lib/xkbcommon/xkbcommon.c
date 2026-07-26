/*
 * ArmOS
 * Copyright (c) 2026 Mohamed Ennassiri
 *
 * Licensed under the Apache License, Version 2.0.
 * See LICENSE for details.
 *
 * File: userland/lib/xkbcommon/xkbcommon.c
 * Layer: Userland / Keyboard compatibility
 *
 * Responsibilities:
 * - Interpret the XKB keymap exported by the ArmOS compositor.
 * - Track depressed, latched, and locked modifier state.
 * - Convert key symbols to names, Unicode code points, and UTF-8.
 * - Provide inert compose-state support when locale rules are unavailable.
 *
 * Notes:
 * - This is an original, dependency-free ArmOS implementation.
 * - It deliberately implements the xkbcommon subset exercised by Foot.
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <xkbcommon/xkbcommon-compose.h>
#include <xkbcommon/xkbcommon-keysyms.h>
#include <xkbcommon/xkbcommon-names.h>

#define ARRAY_SIZE(array) (sizeof(array) / sizeof((array)[0]))
#define MOD_SHIFT   (1u << 0)
#define MOD_CAPS    (1u << 1)
#define MOD_CONTROL (1u << 2)
#define MOD_ALT     (1u << 3)
#define MOD_NUM     (1u << 4)
#define MOD_LOGO    (1u << 5)

struct xkb_context {
    unsigned int references;
};

struct key_definition {
    char name[16];
    xkb_keysym_t levels[2];
    xkb_mod_mask_t modifier;
    unsigned int repeats;
};

struct xkb_keymap {
    unsigned int references;
    struct xkb_context *context;
    struct key_definition keys[256];
};

struct xkb_state {
    unsigned int references;
    struct xkb_keymap *keymap;
    xkb_mod_mask_t depressed;
    xkb_mod_mask_t latched;
    xkb_mod_mask_t locked;
    xkb_layout_index_t layout;
};

struct xkb_compose_table {
    unsigned int references;
    struct xkb_context *context;
};

struct xkb_compose_state {
    unsigned int references;
    struct xkb_compose_table *table;
};

#define KEY(code, key_name, normal, shifted) \
    [code] = { key_name, { normal, shifted }, 0u, 1u }
#define CONTROL_KEY(code, key_name, symbol) \
    [code] = { key_name, { symbol, symbol }, 0u, 0u }
#define MODIFIER_KEY(code, key_name, symbol, mask) \
    [code] = { key_name, { symbol, symbol }, mask, 0u }

static const struct key_definition default_keys[256] = {
    CONTROL_KEY(9, "ESC", XKB_KEY_Escape),
    KEY(10, "AE01", XKB_KEY_1, XKB_KEY_exclam),
    KEY(11, "AE02", XKB_KEY_2, XKB_KEY_at),
    KEY(12, "AE03", XKB_KEY_3, XKB_KEY_numbersign),
    KEY(13, "AE04", XKB_KEY_4, XKB_KEY_dollar),
    KEY(14, "AE05", XKB_KEY_5, XKB_KEY_percent),
    KEY(15, "AE06", XKB_KEY_6, XKB_KEY_asciicircum),
    KEY(16, "AE07", XKB_KEY_7, XKB_KEY_ampersand),
    KEY(17, "AE08", XKB_KEY_8, XKB_KEY_asterisk),
    KEY(18, "AE09", XKB_KEY_9, XKB_KEY_parenleft),
    KEY(19, "AE10", XKB_KEY_0, XKB_KEY_parenright),
    KEY(20, "AE11", XKB_KEY_minus, XKB_KEY_underscore),
    KEY(21, "AE12", XKB_KEY_equal, XKB_KEY_plus),
    CONTROL_KEY(22, "BKSP", XKB_KEY_BackSpace),
    CONTROL_KEY(23, "TAB", XKB_KEY_Tab),
    KEY(24, "AD01", XKB_KEY_q, XKB_KEY_Q),
    KEY(25, "AD02", XKB_KEY_w, XKB_KEY_W),
    KEY(26, "AD03", XKB_KEY_e, XKB_KEY_E),
    KEY(27, "AD04", XKB_KEY_r, XKB_KEY_R),
    KEY(28, "AD05", XKB_KEY_t, XKB_KEY_T),
    KEY(29, "AD06", XKB_KEY_y, XKB_KEY_Y),
    KEY(30, "AD07", XKB_KEY_u, XKB_KEY_U),
    KEY(31, "AD08", XKB_KEY_i, XKB_KEY_I),
    KEY(32, "AD09", XKB_KEY_o, XKB_KEY_O),
    KEY(33, "AD10", XKB_KEY_p, XKB_KEY_P),
    KEY(34, "AD11", XKB_KEY_bracketleft, XKB_KEY_braceleft),
    KEY(35, "AD12", XKB_KEY_bracketright, XKB_KEY_braceright),
    CONTROL_KEY(36, "RTRN", XKB_KEY_Return),
    MODIFIER_KEY(37, "LCTL", XKB_KEY_Control_L, MOD_CONTROL),
    KEY(38, "AC01", XKB_KEY_a, XKB_KEY_A),
    KEY(39, "AC02", XKB_KEY_s, XKB_KEY_S),
    KEY(40, "AC03", XKB_KEY_d, XKB_KEY_D),
    KEY(41, "AC04", XKB_KEY_f, XKB_KEY_F),
    KEY(42, "AC05", XKB_KEY_g, XKB_KEY_G),
    KEY(43, "AC06", XKB_KEY_h, XKB_KEY_H),
    KEY(44, "AC07", XKB_KEY_j, XKB_KEY_J),
    KEY(45, "AC08", XKB_KEY_k, XKB_KEY_K),
    KEY(46, "AC09", XKB_KEY_l, XKB_KEY_L),
    KEY(47, "AC10", XKB_KEY_semicolon, XKB_KEY_colon),
    KEY(48, "AC11", XKB_KEY_apostrophe, XKB_KEY_quotedbl),
    KEY(49, "TLDE", XKB_KEY_grave, XKB_KEY_asciitilde),
    MODIFIER_KEY(50, "LFSH", XKB_KEY_Shift_L, MOD_SHIFT),
    KEY(51, "BKSL", XKB_KEY_backslash, XKB_KEY_bar),
    KEY(52, "AB01", XKB_KEY_z, XKB_KEY_Z),
    KEY(53, "AB02", XKB_KEY_x, XKB_KEY_X),
    KEY(54, "AB03", XKB_KEY_c, XKB_KEY_C),
    KEY(55, "AB04", XKB_KEY_v, XKB_KEY_V),
    KEY(56, "AB05", XKB_KEY_b, XKB_KEY_B),
    KEY(57, "AB06", XKB_KEY_n, XKB_KEY_N),
    KEY(58, "AB07", XKB_KEY_m, XKB_KEY_M),
    KEY(59, "AB08", XKB_KEY_comma, XKB_KEY_less),
    KEY(60, "AB09", XKB_KEY_period, XKB_KEY_greater),
    KEY(61, "AB10", XKB_KEY_slash, XKB_KEY_question),
    MODIFIER_KEY(62, "RTSH", XKB_KEY_Shift_R, MOD_SHIFT),
    MODIFIER_KEY(64, "LALT", XKB_KEY_Alt_L, MOD_ALT),
    KEY(65, "SPCE", XKB_KEY_space, XKB_KEY_space),
    MODIFIER_KEY(66, "CAPS", XKB_KEY_Caps_Lock, MOD_CAPS),
    CONTROL_KEY(67, "FK01", XKB_KEY_F1),
    CONTROL_KEY(68, "FK02", XKB_KEY_F2),
    CONTROL_KEY(69, "FK03", XKB_KEY_F3),
    CONTROL_KEY(70, "FK04", XKB_KEY_F4),
    CONTROL_KEY(71, "FK05", XKB_KEY_F5),
    CONTROL_KEY(72, "FK06", XKB_KEY_F6),
    CONTROL_KEY(73, "FK07", XKB_KEY_F7),
    CONTROL_KEY(74, "FK08", XKB_KEY_F8),
    CONTROL_KEY(75, "FK09", XKB_KEY_F9),
    CONTROL_KEY(76, "FK10", XKB_KEY_F10),
    CONTROL_KEY(95, "FK11", XKB_KEY_F11),
    CONTROL_KEY(96, "FK12", XKB_KEY_F12),
    MODIFIER_KEY(105, "RCTL", XKB_KEY_Control_R, MOD_CONTROL),
    MODIFIER_KEY(108, "RALT", XKB_KEY_Alt_R, MOD_ALT),
    CONTROL_KEY(110, "HOME", XKB_KEY_Home),
    CONTROL_KEY(111, "UP", XKB_KEY_Up),
    CONTROL_KEY(112, "PGUP", XKB_KEY_Prior),
    CONTROL_KEY(113, "LEFT", XKB_KEY_Left),
    CONTROL_KEY(114, "RGHT", XKB_KEY_Right),
    CONTROL_KEY(115, "END", XKB_KEY_End),
    CONTROL_KEY(116, "DOWN", XKB_KEY_Down),
    CONTROL_KEY(117, "PGDN", XKB_KEY_Next),
    CONTROL_KEY(118, "INS", XKB_KEY_Insert),
    CONTROL_KEY(119, "DELE", XKB_KEY_Delete)
};

struct symbol_name {
    const char *name;
    xkb_keysym_t symbol;
};

static const struct symbol_name symbol_names[] = {
    {"NoSymbol", XKB_KEY_NoSymbol},
    {"BackSpace", XKB_KEY_BackSpace},
    {"Tab", XKB_KEY_Tab},
    {"ISO_Left_Tab", XKB_KEY_ISO_Left_Tab},
    {"Return", XKB_KEY_Return},
    {"Escape", XKB_KEY_Escape},
    {"Home", XKB_KEY_Home},
    {"Left", XKB_KEY_Left},
    {"Up", XKB_KEY_Up},
    {"Right", XKB_KEY_Right},
    {"Down", XKB_KEY_Down},
    {"Prior", XKB_KEY_Prior},
    {"Page_Up", XKB_KEY_Page_Up},
    {"Next", XKB_KEY_Next},
    {"Page_Down", XKB_KEY_Page_Down},
    {"End", XKB_KEY_End},
    {"Insert", XKB_KEY_Insert},
    {"Delete", XKB_KEY_Delete},
    {"Shift_L", XKB_KEY_Shift_L},
    {"Shift_R", XKB_KEY_Shift_R},
    {"Control_L", XKB_KEY_Control_L},
    {"Control_R", XKB_KEY_Control_R},
    {"Caps_Lock", XKB_KEY_Caps_Lock},
    {"Alt_L", XKB_KEY_Alt_L},
    {"Alt_R", XKB_KEY_Alt_R},
    {"Super_L", XKB_KEY_Super_L},
    {"Super_R", XKB_KEY_Super_R},
    {"space", XKB_KEY_space},
    {"exclam", XKB_KEY_exclam},
    {"quotedbl", XKB_KEY_quotedbl},
    {"numbersign", XKB_KEY_numbersign},
    {"dollar", XKB_KEY_dollar},
    {"percent", XKB_KEY_percent},
    {"ampersand", XKB_KEY_ampersand},
    {"parenleft", XKB_KEY_parenleft},
    {"parenright", XKB_KEY_parenright},
    {"asterisk", XKB_KEY_asterisk},
    {"minus", XKB_KEY_minus},
    {"equal", XKB_KEY_equal},
    {"plus", XKB_KEY_plus},
    {"comma", XKB_KEY_comma},
    {"period", XKB_KEY_period},
    {"slash", XKB_KEY_slash},
    {"colon", XKB_KEY_colon},
    {"semicolon", XKB_KEY_semicolon},
    {"less", XKB_KEY_less},
    {"greater", XKB_KEY_greater},
    {"question", XKB_KEY_question},
    {"apostrophe", XKB_KEY_apostrophe},
    {"grave", XKB_KEY_grave},
    {"bracketleft", XKB_KEY_bracketleft},
    {"bracketright", XKB_KEY_bracketright},
    {"backslash", XKB_KEY_backslash},
    {"asciicircum", XKB_KEY_asciicircum},
    {"underscore", XKB_KEY_underscore},
    {"braceleft", XKB_KEY_braceleft},
    {"bar", XKB_KEY_bar},
    {"braceright", XKB_KEY_braceright},
    {"asciitilde", XKB_KEY_asciitilde},
    {"section", XKB_KEY_section},
    {"degree", XKB_KEY_degree},
    {"agrave", XKB_KEY_agrave},
    {"ccedilla", XKB_KEY_ccedilla},
    {"egrave", XKB_KEY_egrave},
    {"eacute", XKB_KEY_eacute},
    {"ugrave", XKB_KEY_ugrave},
    {"XF86AudioLowerVolume", XKB_KEY_XF86AudioLowerVolume},
    {"XF86AudioMute", XKB_KEY_XF86AudioMute},
    {"XF86AudioRaiseVolume", XKB_KEY_XF86AudioRaiseVolume},
    {"XF86AudioPlay", XKB_KEY_XF86AudioPlay},
    {"XF86AudioStop", XKB_KEY_XF86AudioStop},
    {"XF86AudioPrev", XKB_KEY_XF86AudioPrev},
    {"XF86AudioNext", XKB_KEY_XF86AudioNext},
    {"XF86Copy", XKB_KEY_XF86Copy},
    {"XF86Paste", XKB_KEY_XF86Paste}
};

static const char *const modifier_names[] = {
    XKB_MOD_NAME_SHIFT,
    XKB_MOD_NAME_CAPS,
    XKB_MOD_NAME_CTRL,
    XKB_MOD_NAME_ALT,
    XKB_MOD_NAME_NUM,
    XKB_MOD_NAME_LOGO
};

static int text_contains(const char *text, size_t length,
                         const char *needle)
{
    size_t needle_length = strlen(needle);

    if (!text || needle_length > length)
        return 0;
    for (size_t index = 0u; index + needle_length <= length; index++) {
        if (memcmp(text + index, needle, needle_length) == 0)
            return 1;
    }
    return 0;
}

static int name_equal(const char *left, const char *right, int insensitive)
{
    if (!left || !right)
        return 0;
    while (*left && *right) {
        unsigned char a = (unsigned char)*left++;
        unsigned char b = (unsigned char)*right++;

        if (insensitive) {
            if (a >= 'A' && a <= 'Z')
                a = (unsigned char)(a + ('a' - 'A'));
            if (b >= 'A' && b <= 'Z')
                b = (unsigned char)(b + ('a' - 'A'));
        }
        if (a != b)
            return 0;
    }
    return *left == '\0' && *right == '\0';
}

static struct xkb_keymap *allocate_keymap(struct xkb_context *context,
                                          int use_defaults)
{
    struct xkb_keymap *keymap;

    if (!context) {
        errno = EINVAL;
        return NULL;
    }
    keymap = calloc(1u, sizeof(*keymap));
    if (!keymap)
        return NULL;
    keymap->references = 1u;
    keymap->context = xkb_context_ref(context);
    if (use_defaults)
        memcpy(keymap->keys, default_keys, sizeof(default_keys));
    return keymap;
}

static const char *skip_space(const char *cursor, const char *end)
{
    while (cursor < end &&
           (*cursor == ' ' || *cursor == '\t' ||
            *cursor == '\r' || *cursor == '\n'))
        cursor++;
    return cursor;
}

static int parse_name(const char **cursor, const char *end,
                      char *name, size_t capacity)
{
    const char *start;
    size_t length;

    if (*cursor >= end || **cursor != '<')
        return -1;
    start = ++*cursor;
    while (*cursor < end && **cursor != '>')
        (*cursor)++;
    if (*cursor >= end)
        return -1;
    length = (size_t)(*cursor - start);
    (*cursor)++;
    if (length == 0u || length >= capacity)
        return -1;
    memcpy(name, start, length);
    name[length] = '\0';
    return 0;
}

static void parse_keycodes(struct xkb_keymap *keymap,
                           const char *buffer, size_t length)
{
    const char *cursor = buffer;
    const char *end = buffer + length;

    while (cursor < end) {
        char name[16];
        char *number_end;
        long code;

        while (cursor < end && *cursor != '<')
            cursor++;
        if (cursor >= end)
            break;
        if (parse_name(&cursor, end, name, sizeof(name)) < 0)
            continue;
        cursor = skip_space(cursor, end);
        if (cursor >= end || *cursor != '=')
            continue;
        cursor = skip_space(cursor + 1, end);
        code = strtol(cursor, &number_end, 10);
        if (number_end == cursor || code < 0 ||
            code >= (long)ARRAY_SIZE(keymap->keys))
            continue;
        memcpy(keymap->keys[code].name, name, strlen(name) + 1u);
        cursor = number_end;
    }
}

static int parse_symbol_token(const char **cursor, const char *end,
                              char *name, size_t capacity)
{
    const char *start;
    size_t length;

    *cursor = skip_space(*cursor, end);
    start = *cursor;
    while (*cursor < end && **cursor != ',' && **cursor != ']' &&
           **cursor != ' ' && **cursor != '\t' &&
           **cursor != '\r' && **cursor != '\n')
        (*cursor)++;
    length = (size_t)(*cursor - start);
    if (length == 0u || length >= capacity)
        return -1;
    memcpy(name, start, length);
    name[length] = '\0';
    return 0;
}

static xkb_mod_mask_t symbol_modifier(xkb_keysym_t symbol)
{
    if (symbol == XKB_KEY_Shift_L || symbol == XKB_KEY_Shift_R)
        return MOD_SHIFT;
    if (symbol == XKB_KEY_Control_L || symbol == XKB_KEY_Control_R)
        return MOD_CONTROL;
    if (symbol == XKB_KEY_Alt_L || symbol == XKB_KEY_Alt_R)
        return MOD_ALT;
    if (symbol == XKB_KEY_Caps_Lock)
        return MOD_CAPS;
    if (symbol == XKB_KEY_Num_Lock)
        return MOD_NUM;
    if (symbol == XKB_KEY_Super_L || symbol == XKB_KEY_Super_R)
        return MOD_LOGO;
    return 0u;
}

static void parse_symbols(struct xkb_keymap *keymap,
                          const char *buffer, size_t length)
{
    const char *cursor = buffer;
    const char *end = buffer + length;

    while (cursor + 4u < end) {
        char key_name[16];
        char first_name[48];
        char second_name[48];
        xkb_keycode_t key;
        xkb_keysym_t first;
        xkb_keysym_t second;

        if (memcmp(cursor, "key ", 4u) != 0) {
            cursor++;
            continue;
        }
        cursor = skip_space(cursor + 4u, end);
        if (parse_name(&cursor, end, key_name, sizeof(key_name)) < 0)
            continue;
        while (cursor < end && *cursor != '[' && *cursor != ';')
            cursor++;
        if (cursor >= end || *cursor != '[')
            continue;
        cursor++;
        if (parse_symbol_token(&cursor, end, first_name,
                               sizeof(first_name)) < 0)
            continue;
        cursor = skip_space(cursor, end);
        second_name[0] = '\0';
        if (cursor < end && *cursor == ',') {
            cursor++;
            (void)parse_symbol_token(&cursor, end, second_name,
                                     sizeof(second_name));
        }
        key = xkb_keymap_key_by_name(keymap, key_name);
        if (key == XKB_KEYCODE_INVALID)
            continue;
        first = xkb_keysym_from_name(first_name, XKB_KEYSYM_NO_FLAGS);
        second = second_name[0] ?
            xkb_keysym_from_name(second_name, XKB_KEYSYM_NO_FLAGS) : first;
        if (first == XKB_KEY_NoSymbol)
            continue;
        keymap->keys[key].levels[0] = first;
        keymap->keys[key].levels[1] =
            second == XKB_KEY_NoSymbol ? first : second;
        keymap->keys[key].modifier = symbol_modifier(first);
        keymap->keys[key].repeats =
            keymap->keys[key].modifier == 0u;
    }
}

struct xkb_context *xkb_context_new(enum xkb_context_flags flags)
{
    struct xkb_context *context;

    if (flags != XKB_CONTEXT_NO_FLAGS) {
        errno = EINVAL;
        return NULL;
    }
    context = calloc(1u, sizeof(*context));
    if (context)
        context->references = 1u;
    return context;
}

struct xkb_context *xkb_context_ref(struct xkb_context *context)
{
    if (context)
        __sync_fetch_and_add(&context->references, 1u);
    return context;
}

void xkb_context_unref(struct xkb_context *context)
{
    if (context && __sync_sub_and_fetch(&context->references, 1u) == 0u)
        free(context);
}

struct xkb_keymap *xkb_keymap_new_from_buffer(
    struct xkb_context *context, const char *buffer, size_t length,
    enum xkb_keymap_format format, enum xkb_keymap_compile_flags flags)
{
    if (!buffer || format != XKB_KEYMAP_FORMAT_TEXT_V1 ||
        flags != XKB_KEYMAP_COMPILE_NO_FLAGS ||
        !text_contains(buffer, length, "xkb_keymap")) {
        errno = EINVAL;
        return NULL;
    }
    struct xkb_keymap *keymap = allocate_keymap(context, 0);

    if (!keymap)
        return NULL;
    parse_keycodes(keymap, buffer, length);
    parse_symbols(keymap, buffer, length);
    return keymap;
}

struct xkb_keymap *xkb_keymap_new_from_names(
    struct xkb_context *context, const struct xkb_rule_names *names,
    enum xkb_keymap_compile_flags flags)
{
    (void)names;
    if (flags != XKB_KEYMAP_COMPILE_NO_FLAGS) {
        errno = EINVAL;
        return NULL;
    }
    return allocate_keymap(context, 1);
}

struct xkb_keymap *xkb_keymap_ref(struct xkb_keymap *keymap)
{
    if (keymap)
        __sync_fetch_and_add(&keymap->references, 1u);
    return keymap;
}

void xkb_keymap_unref(struct xkb_keymap *keymap)
{
    if (!keymap ||
        __sync_sub_and_fetch(&keymap->references, 1u) != 0u)
        return;
    xkb_context_unref(keymap->context);
    free(keymap);
}

xkb_keycode_t xkb_keymap_min_keycode(struct xkb_keymap *keymap)
{
    return keymap ? 8u : XKB_KEYCODE_INVALID;
}

xkb_keycode_t xkb_keymap_max_keycode(struct xkb_keymap *keymap)
{
    return keymap ? 255u : XKB_KEYCODE_INVALID;
}

xkb_keycode_t xkb_keymap_key_by_name(struct xkb_keymap *keymap,
                                     const char *name)
{
    if (!keymap || !name)
        return XKB_KEYCODE_INVALID;
    for (xkb_keycode_t key = 0u; key < ARRAY_SIZE(keymap->keys); key++) {
        if (keymap->keys[key].name[0] &&
            strcmp(keymap->keys[key].name, name) == 0)
            return key;
    }
    return XKB_KEYCODE_INVALID;
}

xkb_mod_index_t xkb_keymap_mod_get_index(struct xkb_keymap *keymap,
                                         const char *name)
{
    if (!keymap || !name)
        return XKB_MOD_INVALID;
    for (xkb_mod_index_t index = 0u;
         index < ARRAY_SIZE(modifier_names); index++) {
        if (strcmp(modifier_names[index], name) == 0)
            return index;
    }
    if (strcmp(name, XKB_VMOD_NAME_ALT) == 0)
        return 3u;
    if (strcmp(name, XKB_VMOD_NAME_META) == 0 ||
        strcmp(name, XKB_VMOD_NAME_SUPER) == 0)
        return 5u;
    if (strcmp(name, XKB_VMOD_NAME_NUM) == 0)
        return 4u;
    return XKB_MOD_INVALID;
}

const char *xkb_keymap_mod_get_name(struct xkb_keymap *keymap,
                                    xkb_mod_index_t index)
{
    if (!keymap || index >= ARRAY_SIZE(modifier_names))
        return NULL;
    return modifier_names[index];
}

xkb_layout_index_t xkb_keymap_num_layouts(struct xkb_keymap *keymap)
{
    return keymap ? 1u : 0u;
}

xkb_level_index_t xkb_keymap_num_levels_for_key(
    struct xkb_keymap *keymap, xkb_keycode_t key,
    xkb_layout_index_t layout)
{
    if (!keymap || key >= ARRAY_SIZE(keymap->keys) ||
        !keymap->keys[key].name[0] ||
        layout != 0u)
        return 0u;
    return keymap->keys[key].levels[0] ==
        keymap->keys[key].levels[1] ? 1u : 2u;
}

int xkb_keymap_key_repeats(struct xkb_keymap *keymap, xkb_keycode_t key)
{
    return keymap && key < ARRAY_SIZE(keymap->keys) &&
        keymap->keys[key].repeats;
}

int xkb_keymap_key_get_syms_by_level(
    struct xkb_keymap *keymap, xkb_keycode_t key,
    xkb_layout_index_t layout, xkb_level_index_t level,
    const xkb_keysym_t **syms_out)
{
    if (syms_out)
        *syms_out = NULL;
    if (!keymap || !syms_out || key >= ARRAY_SIZE(keymap->keys) ||
        !keymap->keys[key].name[0] || layout != 0u || level > 1u)
        return 0;
    if (level == 1u && keymap->keys[key].levels[0] ==
        keymap->keys[key].levels[1])
        return 0;
    *syms_out = &keymap->keys[key].levels[level];
    return 1;
}

size_t xkb_keymap_key_get_mods_for_level(
    struct xkb_keymap *keymap, xkb_keycode_t key,
    xkb_layout_index_t layout, xkb_level_index_t level,
    xkb_mod_mask_t *masks_out, size_t masks_size)
{
    if (!keymap || key >= ARRAY_SIZE(keymap->keys) ||
        !keymap->keys[key].name[0] ||
        layout != 0u || level > 1u)
        return 0u;
    if (masks_out && masks_size > 0u)
        masks_out[0] = level == 1u ? MOD_SHIFT : 0u;
    return 1u;
}

struct xkb_state *xkb_state_new(struct xkb_keymap *keymap)
{
    struct xkb_state *state;

    if (!keymap) {
        errno = EINVAL;
        return NULL;
    }
    state = calloc(1u, sizeof(*state));
    if (!state)
        return NULL;
    state->references = 1u;
    state->keymap = xkb_keymap_ref(keymap);
    return state;
}

struct xkb_state *xkb_state_ref(struct xkb_state *state)
{
    if (state)
        __sync_fetch_and_add(&state->references, 1u);
    return state;
}

void xkb_state_unref(struct xkb_state *state)
{
    if (!state ||
        __sync_sub_and_fetch(&state->references, 1u) != 0u)
        return;
    xkb_keymap_unref(state->keymap);
    free(state);
}

enum xkb_state_component xkb_state_update_mask(
    struct xkb_state *state, xkb_mod_mask_t depressed_mods,
    xkb_mod_mask_t latched_mods, xkb_mod_mask_t locked_mods,
    xkb_layout_index_t depressed_layout,
    xkb_layout_index_t latched_layout,
    xkb_layout_index_t locked_layout)
{
    xkb_mod_mask_t old_effective;
    enum xkb_state_component changed = 0;

    if (!state)
        return 0;
    old_effective = state->depressed | state->latched | state->locked;
    if (state->depressed != depressed_mods)
        changed |= XKB_STATE_MODS_DEPRESSED;
    if (state->latched != latched_mods)
        changed |= XKB_STATE_MODS_LATCHED;
    if (state->locked != locked_mods)
        changed |= XKB_STATE_MODS_LOCKED;
    state->depressed = depressed_mods;
    state->latched = latched_mods;
    state->locked = locked_mods;
    state->layout = locked_layout;
    if (old_effective !=
        (state->depressed | state->latched | state->locked))
        changed |= XKB_STATE_MODS_EFFECTIVE;
    if (depressed_layout || latched_layout || locked_layout)
        changed |= XKB_STATE_LAYOUT_EFFECTIVE;
    return changed;
}

enum xkb_state_component xkb_state_update_key(
    struct xkb_state *state, xkb_keycode_t key,
    enum xkb_key_direction direction)
{
    xkb_mod_mask_t modifier;

    if (!state || key >= ARRAY_SIZE(state->keymap->keys) ||
        !state->keymap->keys[key].name[0])
        return 0;
    modifier = state->keymap->keys[key].modifier;
    if (!modifier)
        return 0;
    if (modifier == MOD_CAPS) {
        if (direction == XKB_KEY_DOWN)
            state->locked ^= MOD_CAPS;
        return XKB_STATE_MODS_LOCKED | XKB_STATE_MODS_EFFECTIVE;
    }
    if (direction == XKB_KEY_DOWN)
        state->depressed |= modifier;
    else
        state->depressed &= ~modifier;
    return XKB_STATE_MODS_DEPRESSED | XKB_STATE_MODS_EFFECTIVE;
}

xkb_mod_mask_t xkb_state_serialize_mods(
    struct xkb_state *state, enum xkb_state_component components)
{
    xkb_mod_mask_t result = 0u;

    if (!state)
        return 0u;
    if (components & XKB_STATE_MODS_DEPRESSED)
        result |= state->depressed;
    if (components & XKB_STATE_MODS_LATCHED)
        result |= state->latched;
    if (components & XKB_STATE_MODS_LOCKED)
        result |= state->locked;
    if (components & XKB_STATE_MODS_EFFECTIVE)
        result |= state->depressed | state->latched | state->locked;
    return result;
}

int xkb_state_mod_index_is_active(
    struct xkb_state *state, xkb_mod_index_t index,
    enum xkb_state_component components)
{
    if (!state || index >= 32u)
        return 0;
    return (xkb_state_serialize_mods(state, components) &
            (1u << index)) != 0u;
}

xkb_layout_index_t xkb_state_key_get_layout(
    struct xkb_state *state, xkb_keycode_t key)
{
    (void)key;
    return state ? state->layout : XKB_LAYOUT_INVALID;
}

xkb_keysym_t xkb_state_key_get_one_sym(
    struct xkb_state *state, xkb_keycode_t key)
{
    xkb_mod_mask_t effective;
    xkb_keysym_t symbol;
    int shifted;

    if (!state || key >= ARRAY_SIZE(state->keymap->keys) ||
        !state->keymap->keys[key].name[0])
        return XKB_KEY_NoSymbol;
    effective = state->depressed | state->latched | state->locked;
    shifted = (effective & MOD_SHIFT) != 0u;
    symbol = state->keymap->keys[key].levels[shifted ? 1u : 0u];
    if (symbol >= XKB_KEY_a && symbol <= XKB_KEY_z &&
        (effective & MOD_CAPS))
        symbol = shifted ? symbol : symbol - ('a' - 'A');
    else if (symbol >= XKB_KEY_A && symbol <= XKB_KEY_Z &&
             (effective & MOD_CAPS) && shifted)
        symbol += 'a' - 'A';
    return symbol;
}

uint32_t xkb_keysym_to_utf32(xkb_keysym_t keysym)
{
    if ((keysym >= 0x20u && keysym <= 0x7eu) ||
        (keysym >= 0x00a0u && keysym <= 0x00ffu))
        return keysym;
    if ((keysym & 0xff000000u) == 0x01000000u)
        return keysym & 0x00ffffffu;
    if (keysym >= XKB_KEY_KP_0 && keysym <= XKB_KEY_KP_9)
        return '0' + (keysym - XKB_KEY_KP_0);
    if (keysym == XKB_KEY_KP_Space)
        return ' ';
    if (keysym == XKB_KEY_KP_Equal)
        return '=';
    if (keysym == XKB_KEY_KP_Multiply)
        return '*';
    if (keysym == XKB_KEY_KP_Add)
        return '+';
    if (keysym == XKB_KEY_KP_Subtract)
        return '-';
    if (keysym == XKB_KEY_KP_Decimal)
        return '.';
    if (keysym == XKB_KEY_KP_Divide)
        return '/';
    return 0u;
}

static int encode_utf8(uint32_t codepoint, char *buffer, size_t size)
{
    unsigned char bytes[4];
    size_t length;

    if (codepoint == 0u)
        return 0;
    if (codepoint <= 0x7fu) {
        bytes[0] = (unsigned char)codepoint;
        length = 1u;
    } else if (codepoint <= 0x7ffu) {
        bytes[0] = 0xc0u | (unsigned char)(codepoint >> 6);
        bytes[1] = 0x80u | (unsigned char)(codepoint & 0x3fu);
        length = 2u;
    } else if (codepoint <= 0xffffu) {
        bytes[0] = 0xe0u | (unsigned char)(codepoint >> 12);
        bytes[1] = 0x80u | (unsigned char)((codepoint >> 6) & 0x3fu);
        bytes[2] = 0x80u | (unsigned char)(codepoint & 0x3fu);
        length = 3u;
    } else if (codepoint <= 0x10ffffu) {
        bytes[0] = 0xf0u | (unsigned char)(codepoint >> 18);
        bytes[1] = 0x80u | (unsigned char)((codepoint >> 12) & 0x3fu);
        bytes[2] = 0x80u | (unsigned char)((codepoint >> 6) & 0x3fu);
        bytes[3] = 0x80u | (unsigned char)(codepoint & 0x3fu);
        length = 4u;
    } else {
        return 0;
    }
    if (buffer && size > 0u) {
        size_t copied = length < size - 1u ? length : size - 1u;

        memcpy(buffer, bytes, copied);
        buffer[copied] = '\0';
    }
    return (int)length;
}

uint32_t xkb_state_key_get_utf32(struct xkb_state *state,
                                 xkb_keycode_t key)
{
    return xkb_keysym_to_utf32(xkb_state_key_get_one_sym(state, key));
}

int xkb_state_key_get_utf8(struct xkb_state *state, xkb_keycode_t key,
                           char *buffer, size_t size)
{
    return encode_utf8(xkb_state_key_get_utf32(state, key), buffer, size);
}

xkb_mod_mask_t xkb_state_key_get_consumed_mods2(
    struct xkb_state *state, xkb_keycode_t key,
    enum xkb_consumed_mode mode)
{
    (void)mode;
    if (!state || key >= ARRAY_SIZE(state->keymap->keys) ||
        !state->keymap->keys[key].name[0] ||
        state->keymap->keys[key].levels[0] ==
            state->keymap->keys[key].levels[1])
        return 0u;
    return MOD_SHIFT;
}

xkb_keysym_t xkb_keysym_from_name(const char *name,
                                  enum xkb_keysym_flags flags)
{
    int insensitive = (flags & XKB_KEYSYM_CASE_INSENSITIVE) != 0;

    if (!name)
        return XKB_KEY_NoSymbol;
    if (name[0] && name[1] == '\0')
        return (unsigned char)name[0];
    for (size_t index = 0u; index < ARRAY_SIZE(symbol_names); index++) {
        if (name_equal(name, symbol_names[index].name, insensitive))
            return symbol_names[index].symbol;
    }
    if (name[0] == 'F') {
        char *end;
        long number = strtol(name + 1, &end, 10);

        if (*end == '\0' && number >= 1 && number <= 35)
            return XKB_KEY_F1 + (xkb_keysym_t)(number - 1);
    }
    return XKB_KEY_NoSymbol;
}

int xkb_keysym_get_name(xkb_keysym_t keysym, char *buffer, size_t size)
{
    const char *name = NULL;
    char generated[16];
    size_t length;

    if (!buffer || size == 0u)
        return -1;
    for (size_t index = 0u; index < ARRAY_SIZE(symbol_names); index++) {
        if (symbol_names[index].symbol == keysym) {
            name = symbol_names[index].name;
            break;
        }
    }
    if (!name && keysym >= XKB_KEY_F1 && keysym <= XKB_KEY_F35) {
        snprintf(generated, sizeof(generated), "F%u",
                 (unsigned)(keysym - XKB_KEY_F1 + 1u));
        name = generated;
    }
    if (!name && keysym >= 0x21u && keysym <= 0x7eu) {
        generated[0] = (char)keysym;
        generated[1] = '\0';
        name = generated;
    }
    if (!name)
        return -1;
    length = strlen(name);
    if (length >= size)
        return -1;
    memcpy(buffer, name, length + 1u);
    return (int)length;
}

xkb_keysym_t xkb_keysym_to_lower(xkb_keysym_t keysym)
{
    if (keysym >= XKB_KEY_A && keysym <= XKB_KEY_Z)
        return keysym + ('a' - 'A');
    return keysym;
}

struct xkb_compose_table *xkb_compose_table_new_from_locale(
    struct xkb_context *context, const char *locale,
    enum xkb_compose_compile_flags flags)
{
    struct xkb_compose_table *table;

    if (!context || !locale || flags != XKB_COMPOSE_COMPILE_NO_FLAGS) {
        errno = EINVAL;
        return NULL;
    }
    table = calloc(1u, sizeof(*table));
    if (!table)
        return NULL;
    table->references = 1u;
    table->context = xkb_context_ref(context);
    return table;
}

struct xkb_compose_table *xkb_compose_table_ref(
    struct xkb_compose_table *table)
{
    if (table)
        __sync_fetch_and_add(&table->references, 1u);
    return table;
}

void xkb_compose_table_unref(struct xkb_compose_table *table)
{
    if (!table ||
        __sync_sub_and_fetch(&table->references, 1u) != 0u)
        return;
    xkb_context_unref(table->context);
    free(table);
}

struct xkb_compose_state *xkb_compose_state_new(
    struct xkb_compose_table *table, enum xkb_compose_state_flags flags)
{
    struct xkb_compose_state *state;

    if (!table || flags != XKB_COMPOSE_STATE_NO_FLAGS) {
        errno = EINVAL;
        return NULL;
    }
    state = calloc(1u, sizeof(*state));
    if (!state)
        return NULL;
    state->references = 1u;
    state->table = xkb_compose_table_ref(table);
    return state;
}

struct xkb_compose_state *xkb_compose_state_ref(
    struct xkb_compose_state *state)
{
    if (state)
        __sync_fetch_and_add(&state->references, 1u);
    return state;
}

void xkb_compose_state_unref(struct xkb_compose_state *state)
{
    if (!state ||
        __sync_sub_and_fetch(&state->references, 1u) != 0u)
        return;
    xkb_compose_table_unref(state->table);
    free(state);
}

enum xkb_compose_feed_result xkb_compose_state_feed(
    struct xkb_compose_state *state, xkb_keysym_t keysym)
{
    (void)keysym;
    return state ? XKB_COMPOSE_FEED_ACCEPTED : XKB_COMPOSE_FEED_IGNORED;
}

void xkb_compose_state_reset(struct xkb_compose_state *state)
{
    (void)state;
}

enum xkb_compose_status xkb_compose_state_get_status(
    struct xkb_compose_state *state)
{
    return state ? XKB_COMPOSE_NOTHING : XKB_COMPOSE_CANCELLED;
}

int xkb_compose_state_get_utf8(struct xkb_compose_state *state,
                               char *buffer, size_t size)
{
    if (buffer && size > 0u)
        buffer[0] = '\0';
    return state ? 0 : -1;
}
