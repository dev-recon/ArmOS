/*
 * ArmOS
 * Copyright (c) 2026 Mohamed Ennassiri
 *
 * Licensed under the Apache License, Version 2.0.
 * See LICENSE for details.
 *
 * File: include/kernel/keyboard.h
 * Layer: Kernel / terminal and character devices
 *
 * Responsibilities:
 * - Define the common keyboard state and input translation contract.
 * - Feed translated input into a platform-selected logical TTY.
 *
 * Notes:
 * - Keyboard drivers must not own login-session or console policy.
 */

#ifndef _KERNEL_KEYBOARD_H
#define _KERNEL_KEYBOARD_H

#include <kernel/arch_platform.h>
#include <kernel/types.h>
#include <kernel/task.h>
#include <uapi/armos/input.h>

/* Legacy PL050 keyboard fallback selected by the current platform. */
#define KBD_BASE        ARMOS_PLATFORM_PL050_KBD_BASE
#define KBD_DATA        (KBD_BASE + 0x00u)
#define KBD_STAT        (KBD_BASE + 0x04u)
#define KBD_CTRL        (KBD_BASE + 0x08u)
#define KBD_CLKDIV      (KBD_BASE + 0x0Cu)
#define KBD_IRQ         (KBD_BASE + 0x10u)

typedef struct {
    bool shift_pressed;
    bool ctrl_pressed;
    bool alt_pressed;
    bool caps_lock;
    bool fn_pressed;        /* Mac Fn key */
    bool cmd_pressed;       /* Mac Command key */
    bool opt_pressed;       /* Mac Option key */
    
    char buffer[256];
    uint32_t head;
    uint32_t tail;
    
    task_t * waiters;
} keyboard_state_t;

/* Keyboard functions */
void init_keyboard(void);
void keyboard_irq_handler(void);
void handle_scancode(uint8_t scancode);
uint32_t keyboard_layout_get(void);
const char *keyboard_layout_name(uint32_t layout);
int keyboard_layout_set(uint32_t layout);
char keyboard_translate_key(uint16_t key, bool shift, bool caps_lock,
                            bool alternate);
void add_to_keyboard_buffer(char c);
char keyboard_getchar(void);

#endif
