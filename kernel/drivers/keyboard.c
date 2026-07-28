/*
 * ArmOS
 * Copyright (c) 2026 Mohamed Ennassiri
 *
 * Licensed under the Apache License, Version 2.0.
 * See LICENSE for details.
 *
 * File: kernel/drivers/keyboard.c
 * Layer: Kernel / terminal and character devices
 *
 * Responsibilities:
 * - Translate legacy keyboard scan codes into ArmOS input events.
 * - Route keyboard activity through the common TTY and display contracts.
 *
 * Notes:
 * - Platform-specific input drivers select their target logical TTY.
 */

#include <kernel/keyboard.h>
#include <kernel/input.h>
#include <kernel/task.h>
#include <kernel/interrupt.h>
#include <kernel/string.h>
#include <kernel/uart.h>
#include <kernel/signal.h>
#include <kernel/process.h>
#include <kernel/kprintf.h>

#ifndef ARMOS_DEFAULT_KEYBOARD_LAYOUT
#error "ARMOS_DEFAULT_KEYBOARD_LAYOUT must be selected by the build profile"
#endif

#define KEYMAP_SIZE 128u

struct keyboard_map {
    const char *name;
    const char *normal;
    const char *shifted;
    const char *alternate;
    const char *alternate_shifted;
};

static const char keymap_us[KEYMAP_SIZE] = {
    [1] = 27, [2] = '1', [3] = '2', [4] = '3', [5] = '4',
    [6] = '5', [7] = '6', [8] = '7', [9] = '8', [10] = '9',
    [11] = '0', [12] = '-', [13] = '=', [14] = '\b', [15] = '\t',
    [16] = 'q', [17] = 'w', [18] = 'e', [19] = 'r', [20] = 't',
    [21] = 'y', [22] = 'u', [23] = 'i', [24] = 'o', [25] = 'p',
    [26] = '[', [27] = ']', [28] = '\n',
    [30] = 'a', [31] = 's', [32] = 'd', [33] = 'f', [34] = 'g',
    [35] = 'h', [36] = 'j', [37] = 'k', [38] = 'l', [39] = ';',
    [40] = '\'', [41] = '`', [43] = '\\',
    [44] = 'z', [45] = 'x', [46] = 'c', [47] = 'v', [48] = 'b',
    [49] = 'n', [50] = 'm', [51] = ',', [52] = '.', [53] = '/',
    [57] = ' ', [86] = '\\',
};

static const char keymap_us_shift[KEYMAP_SIZE] = {
    [1] = 27, [2] = '!', [3] = '@', [4] = '#', [5] = '$',
    [6] = '%', [7] = '^', [8] = '&', [9] = '*', [10] = '(',
    [11] = ')', [12] = '_', [13] = '+', [14] = '\b', [15] = '\t',
    [16] = 'Q', [17] = 'W', [18] = 'E', [19] = 'R', [20] = 'T',
    [21] = 'Y', [22] = 'U', [23] = 'I', [24] = 'O', [25] = 'P',
    [26] = '{', [27] = '}', [28] = '\n',
    [30] = 'A', [31] = 'S', [32] = 'D', [33] = 'F', [34] = 'G',
    [35] = 'H', [36] = 'J', [37] = 'K', [38] = 'L', [39] = ':',
    [40] = '"', [41] = '~', [43] = '|',
    [44] = 'Z', [45] = 'X', [46] = 'C', [47] = 'V', [48] = 'B',
    [49] = 'N', [50] = 'M', [51] = '<', [52] = '>', [53] = '?',
    [57] = ' ', [86] = '|',
};

static const char keymap_fr[KEYMAP_SIZE] = {
    [1] = 27, [2] = 'a', [3] = 'e', [4] = 'e', [5] = 'e',
    [6] = '(', [7] = ')', [8] = '\'', [9] = '\'', [10] = '<',
    [11] = '>', [12] = '\'', [13] = '^', [14] = '\b', [15] = '\t',
    [16] = 'a', [17] = 'z', [18] = 'e', [19] = 'r', [20] = 't',
    [21] = 'y', [22] = 'u', [23] = 'i', [24] = 'o', [25] = 'p',
    [26] = '-', [27] = '+', [28] = '\n',
    [30] = 'q', [31] = 's', [32] = 'd', [33] = 'f', [34] = 'g',
    [35] = 'h', [36] = 'j', [37] = 'k', [38] = 'l', [39] = 'm',
    [40] = '/', [41] = '@', [43] = '*',
    [44] = 'w', [45] = 'x', [46] = 'c', [47] = 'v', [48] = 'b',
    [49] = 'n', [50] = '.', [51] = ',', [52] = ':', [53] = ';',
    [57] = ' ', [86] = '<',
};

static const char keymap_fr_shift[KEYMAP_SIZE] = {
    [1] = 27, [2] = '1', [3] = '2', [4] = '3', [5] = '4',
    [6] = '5', [7] = '6', [8] = '7', [9] = '8', [10] = '9',
    [11] = '0', [12] = '"', [13] = 0, [14] = '\b', [15] = '\t',
    [16] = 'A', [17] = 'Z', [18] = 'E', [19] = 'R', [20] = 'T',
    [21] = 'Y', [22] = 'U', [23] = 'I', [24] = 'O', [25] = 'P',
    [26] = '-', [27] = '+', [28] = '\n',
    [30] = 'Q', [31] = 'S', [32] = 'D', [33] = 'F', [34] = 'G',
    [35] = 'H', [36] = 'J', [37] = 'K', [38] = 'L', [39] = 'M',
    [40] = '\\', [41] = '#', [43] = 0,
    [44] = 'W', [45] = 'X', [46] = 'C', [47] = 'V', [48] = 'B',
    [49] = 'N', [50] = '?', [51] = '!', [52] = 0, [53] = '=',
    [57] = ' ', [86] = '>',
};

static const char keymap_fr_alt[KEYMAP_SIZE] = {
    [4] = '`', [5] = '&', [6] = '[', [7] = ']', [9] = '_',
    [20] = '{', [21] = '}', [25] = '%', [26] = '-',
    [32] = '$', [38] = '|', [40] = '/', [48] = '-',
    [49] = '~',
};

static const char keymap_fr_alt_shift[KEYMAP_SIZE] = {
    [2] = 'A', [3] = 'E', [4] = 'E', [5] = 'E',
    [9] = '-', [10] = '<', [11] = '>',
    [16] = 0, [20] = 0, [22] = 'U', [24] = 0,
    [43] = 0, [46] = 'C', [50] = 0, [51] = ',',
};

/*
 * Traditional French PC AZERTY (Windows KBDFR / "French Legacy").
 * The kernel console is byte-oriented, so accented keys use an ASCII
 * fallback here. Wayland publishes their full Unicode XKB symbols.
 */
static const char keymap_fr_legacy[KEYMAP_SIZE] = {
    [1] = 27, [2] = '&', [3] = 'e', [4] = '"', [5] = '\'',
    [6] = '(', [7] = '-', [8] = 'e', [9] = '_', [10] = 'c',
    [11] = 'a', [12] = ')', [13] = '=', [14] = '\b', [15] = '\t',
    [16] = 'a', [17] = 'z', [18] = 'e', [19] = 'r', [20] = 't',
    [21] = 'y', [22] = 'u', [23] = 'i', [24] = 'o', [25] = 'p',
    [26] = '^', [27] = '$', [28] = '\n',
    [30] = 'q', [31] = 's', [32] = 'd', [33] = 'f', [34] = 'g',
    [35] = 'h', [36] = 'j', [37] = 'k', [38] = 'l', [39] = 'm',
    [40] = 'u', [41] = 0, [43] = '*',
    [44] = 'w', [45] = 'x', [46] = 'c', [47] = 'v', [48] = 'b',
    [49] = 'n', [50] = ',', [51] = ';', [52] = ':', [53] = '!',
    [57] = ' ', [86] = '<',
};

static const char keymap_fr_legacy_shift[KEYMAP_SIZE] = {
    [1] = 27, [2] = '1', [3] = '2', [4] = '3', [5] = '4',
    [6] = '5', [7] = '6', [8] = '7', [9] = '8', [10] = '9',
    [11] = '0', [12] = 0, [13] = '+', [14] = '\b', [15] = '\t',
    [16] = 'A', [17] = 'Z', [18] = 'E', [19] = 'R', [20] = 'T',
    [21] = 'Y', [22] = 'U', [23] = 'I', [24] = 'O', [25] = 'P',
    [26] = 0, [27] = 0, [28] = '\n',
    [30] = 'Q', [31] = 'S', [32] = 'D', [33] = 'F', [34] = 'G',
    [35] = 'H', [36] = 'J', [37] = 'K', [38] = 'L', [39] = 'M',
    [40] = '%', [41] = 0, [43] = 0,
    [44] = 'W', [45] = 'X', [46] = 'C', [47] = 'V', [48] = 'B',
    [49] = 'N', [50] = '?', [51] = '.', [52] = '/', [53] = 0,
    [57] = ' ', [86] = '>',
};

static const char keymap_fr_legacy_alt[KEYMAP_SIZE] = {
    [3] = '~', [4] = '#', [5] = '{', [6] = '[', [7] = '|',
    [8] = '`', [9] = '\\', [10] = '^', [11] = '@', [12] = ']',
    [13] = '}',
};

static const char keymap_fr_legacy_alt_shift[KEYMAP_SIZE] = {0};

static const char keymap_fr_mac[KEYMAP_SIZE] = {
    [1] = 27, [2] = '&', [3] = 'e', [4] = '"', [5] = '\'',
    [6] = '(', [7] = 's', [8] = 'e', [9] = '!', [10] = 'c',
    [11] = 'a', [12] = ')', [13] = '-', [14] = '\b', [15] = '\t',
    [16] = 'a', [17] = 'z', [18] = 'e', [19] = 'r', [20] = 't',
    [21] = 'y', [22] = 'u', [23] = 'i', [24] = 'o', [25] = 'p',
    [26] = '^', [27] = '$', [28] = '\n',
    [30] = 'q', [31] = 's', [32] = 'd', [33] = 'f', [34] = 'g',
    [35] = 'h', [36] = 'j', [37] = 'k', [38] = 'l', [39] = 'm',
    [40] = 'u', [41] = '<', [43] = '`',
    [44] = 'w', [45] = 'x', [46] = 'c', [47] = 'v', [48] = 'b',
    [49] = 'n', [50] = ',', [51] = ';', [52] = ':', [53] = '=',
    [57] = ' ', [86] = '<',
};

static const char keymap_fr_mac_shift[KEYMAP_SIZE] = {
    [1] = 27, [2] = '1', [3] = '2', [4] = '3', [5] = '4',
    [6] = '5', [7] = '6', [8] = '7', [9] = '8', [10] = '9',
    [11] = '0', [13] = '_', [14] = '\b', [15] = '\t',
    [16] = 'A', [17] = 'Z', [18] = 'E', [19] = 'R', [20] = 'T',
    [21] = 'Y', [22] = 'U', [23] = 'I', [24] = 'O', [25] = 'P',
    [26] = '^', [27] = '*', [28] = '\n',
    [30] = 'Q', [31] = 'S', [32] = 'D', [33] = 'F', [34] = 'G',
    [35] = 'H', [36] = 'J', [37] = 'K', [38] = 'L', [39] = 'M',
    [40] = '%', [41] = '>', [44] = 'W', [45] = 'X', [46] = 'C',
    [47] = 'V', [48] = 'B', [49] = 'N', [50] = '?', [51] = '.',
    [52] = '/', [53] = '+', [57] = ' ', [86] = '>',
};

static const char keymap_fr_mac_alt[KEYMAP_SIZE] = {
    [6] = '{', [12] = '}', [16] = '@', [32] = '#', [49] = '~',
};

static const char keymap_fr_mac_alt_shift[KEYMAP_SIZE] = {
    [6] = '[', [12] = ']', [38] = '|', [52] = '\\',
};

static const struct keyboard_map keyboard_maps[ARMOS_KEYBOARD_LAYOUT_COUNT] = {
    [ARMOS_KEYBOARD_LAYOUT_US] = {
        "us", keymap_us, keymap_us_shift, NULL, NULL
    },
    [ARMOS_KEYBOARD_LAYOUT_US_MAC] = {
        "us-mac", keymap_us, keymap_us_shift, NULL, NULL
    },
    [ARMOS_KEYBOARD_LAYOUT_FR] = {
        "fr", keymap_fr, keymap_fr_shift,
        keymap_fr_alt, keymap_fr_alt_shift
    },
    [ARMOS_KEYBOARD_LAYOUT_FR_MAC] = {
        "fr-mac", keymap_fr_mac, keymap_fr_mac_shift,
        keymap_fr_mac_alt, keymap_fr_mac_alt_shift
    },
    [ARMOS_KEYBOARD_LAYOUT_FR_LEGACY] = {
        "fr-legacy", keymap_fr_legacy, keymap_fr_legacy_shift,
        keymap_fr_legacy_alt, keymap_fr_legacy_alt_shift
    },
};

static keyboard_state_t kbd_state = {0};
static spinlock_t kbd_lock = SPINLOCK_INIT("keyboard");
static uint32_t active_layout = ARMOS_DEFAULT_KEYBOARD_LAYOUT;

void init_keyboard(void)
{
    if (!arch_platform_has_pl050_keyboard()) {
        KINFO("Keyboard: PL050 not present on this platform\n");
        return;
    }

    volatile uint32_t* kbd = (volatile uint32_t*)KBD_BASE;
    
    /* Reset keyboard */
    kbd[KBD_CTRL/4] = 0;
    
    /* Configure clock divisor */
    kbd[KBD_CLKDIV/4] = 8;
    
    /* Enable keyboard with interrupts */
    kbd[KBD_CTRL/4] = (1 << 2) | (1 << 4);
    
    /* Enable keyboard IRQ */
    irq_enable(IRQ_KEYBOARD);
    
    kbd_state.head = 0;
    kbd_state.tail = 0;
    kbd_state.waiters = NULL;
    
    KINFO("Keyboard initialized (layout %s)\n",
          keyboard_layout_name(keyboard_layout_get()));
}

void keyboard_irq_handler(void)
{
    if (!arch_platform_has_pl050_keyboard())
        return;

    volatile uint32_t* kbd = (volatile uint32_t*)KBD_BASE;
    uint8_t scancode;
    
    /* Check if data available */
    if (!(kbd[KBD_STAT/4] & (1 << 4))) {
        return;
    }
    
    scancode = kbd[KBD_DATA/4] & 0xFF;
    handle_scancode(scancode);
}

void handle_scancode(uint8_t scancode)
{
    bool key_released = (scancode & 0x80) != 0;
    uint8_t key = scancode & 0x7F;
    char ascii;
    
    if (key_released) {
        /* Key released */
        switch (key) {
            case 0x2A: case 0x36: /* Shift */
                kbd_state.shift_pressed = false;
                break;
            case 0x1D: /* Ctrl */
                kbd_state.ctrl_pressed = false;
                break;
            case 0x38: /* Alt/Option */
                kbd_state.opt_pressed = false;
                break;
            case 0x5B: case 0x5C: /* Cmd (Windows keys on PC) */
                kbd_state.cmd_pressed = false;
                break;
            case 0x3A: /* Fn (some layouts) */
                kbd_state.fn_pressed = false;
                break;
        }
        return;
    }
    
    /* Key pressed */
    switch (key) {
        case 0x2A: case 0x36: /* Shift */
            kbd_state.shift_pressed = true;
            break;
        case 0x1D: /* Ctrl */
            kbd_state.ctrl_pressed = true;
            break;
        case 0x38: /* Alt/Option */
            kbd_state.opt_pressed = true;
            break;
        case 0x5B: case 0x5C: /* Cmd */
            kbd_state.cmd_pressed = true;
            break;
        case 0x3A: /* Caps Lock */
            kbd_state.caps_lock = !kbd_state.caps_lock;
            break;
        case 0x57: case 0x58: /* Fn */
            kbd_state.fn_pressed = true;
            break;
        default:
            /* Convert to ASCII */
            ascii = keyboard_translate_key(key, kbd_state.shift_pressed,
                                           kbd_state.caps_lock,
                                           kbd_state.opt_pressed);
            if (kbd_state.ctrl_pressed && ascii >= 'a' && ascii <= 'z')
                ascii = (char)(ascii - 'a' + 1);
            else if (kbd_state.ctrl_pressed &&
                     ascii >= 'A' && ascii <= 'Z')
                ascii = (char)(ascii - 'A' + 1);
            if (ascii) {
                add_to_keyboard_buffer(ascii);
            }
            break;
    }
}

uint32_t keyboard_layout_get(void)
{
    return __atomic_load_n(&active_layout, __ATOMIC_ACQUIRE);
}

const char *keyboard_layout_name(uint32_t layout)
{
    if (layout >= ARMOS_KEYBOARD_LAYOUT_COUNT)
        return "unknown";
    return keyboard_maps[layout].name;
}

int keyboard_layout_set(uint32_t layout)
{
    uint32_t previous;

    if (layout >= ARMOS_KEYBOARD_LAYOUT_COUNT)
        return -EINVAL;
    previous = __atomic_load_n(&active_layout, __ATOMIC_ACQUIRE);
    __atomic_store_n(&active_layout, layout, __ATOMIC_RELEASE);
    if (previous != layout)
        armos_input_emit(ARMOS_INPUT_EVENT_CONFIG,
                         ARMOS_INPUT_CONFIG_KEYBOARD_LAYOUT, (int32_t)layout);
    return 0;
}

char keyboard_translate_key(uint16_t key, bool shift, bool caps_lock,
                            bool alternate)
{
    const struct keyboard_map *map;
    const char *table;
    uint32_t layout = keyboard_layout_get();
    char normal;

    if (key >= KEYMAP_SIZE || layout >= ARMOS_KEYBOARD_LAYOUT_COUNT)
        return 0;
    map = &keyboard_maps[layout];
    normal = map->normal[key];
    if (caps_lock && normal >= 'a' && normal <= 'z')
        shift = !shift;
    if (alternate) {
        table = shift ? map->alternate_shifted : map->alternate;
        if (table && table[key])
            return table[key];
    }
    return shift ? map->shifted[key] : normal;
}

void add_to_keyboard_buffer(char c)
{
    uint32_t next_head;
    task_t *waiter = NULL;
    unsigned long flags;

    spin_lock_irqsave(&kbd_lock, &flags);
    next_head = (kbd_state.head + 1) % 256;
    
    if (next_head != kbd_state.tail) {
        kbd_state.buffer[kbd_state.head] = c;
        kbd_state.head = next_head;
        
        if (kbd_state.waiters && kbd_state.waiters->state == TASK_INTERRUPTIBLE) {
            waiter = kbd_state.waiters;
            kbd_state.waiters = NULL;
        } else if (kbd_state.waiters &&
                   (kbd_state.waiters->state == TASK_READY ||
                    kbd_state.waiters->state == TASK_RUNNING ||
                    kbd_state.waiters->state == TASK_ZOMBIE ||
                    kbd_state.waiters->state == TASK_TERMINATED)) {
            kbd_state.waiters = NULL;
        }
    }
    spin_unlock_irqrestore(&kbd_lock, flags);

    task_wake(waiter);
}

char keyboard_getchar(void)
{
    char c;
    
    /* Wait for character */
    while (1) {
        task_t *task = task_current_local();
        unsigned long flags;

        spin_lock_irqsave(&kbd_lock, &flags);
        if (kbd_state.head != kbd_state.tail) {
            c = kbd_state.buffer[kbd_state.tail];
            kbd_state.tail = (kbd_state.tail + 1) % 256;
            spin_unlock_irqrestore(&kbd_lock, flags);
            return c;
        }
        if (!task) {
            spin_unlock_irqrestore(&kbd_lock, flags);
            return -1;
        }

        kbd_state.waiters = task;
        spin_unlock_irqrestore(&kbd_lock, flags);

        task_set_interruptible(task);

        spin_lock_irqsave(&kbd_lock, &flags);
        if (kbd_state.head != kbd_state.tail) {
            if (kbd_state.waiters == task)
                kbd_state.waiters = NULL;
            c = kbd_state.buffer[kbd_state.tail];
            kbd_state.tail = (kbd_state.tail + 1) % 256;
            spin_unlock_irqrestore(&kbd_lock, flags);
            task_set_state(task, TASK_RUNNING);
            return c;
        }
        spin_unlock_irqrestore(&kbd_lock, flags);

        schedule();
        
        /* Check for signals */
        if (has_pending_signals(task)) {
            spin_lock_irqsave(&kbd_lock, &flags);
            if (kbd_state.waiters == task)
                kbd_state.waiters = NULL;
            spin_unlock_irqrestore(&kbd_lock, flags);
            return -1;
        }
    }
}
