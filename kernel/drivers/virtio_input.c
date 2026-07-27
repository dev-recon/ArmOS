/*
 * ArmOS
 * Copyright (c) 2026 Mohamed Ennassiri
 *
 * Licensed under the Apache License, Version 2.0.
 * See LICENSE for details.
 *
 * File: kernel/drivers/virtio_input.c
 * Layer: Kernel / VirtIO drivers
 *
 * Responsibilities:
 * - Negotiate VirtIO device features and queues.
 * - Provide block, GPU, or input transport services.
 *
 * Notes:
 * - Device ordering and cache coherency matter under preemption.
 */

#include <kernel/types.h>
#include <kernel/address_space.h>
#include <kernel/fdt.h>
#include <kernel/virtio_input.h>
#include <kernel/virtio_block.h>
#include <kernel/tty.h>
#include <kernel/memory.h>
#include <kernel/string.h>
#include <kernel/kprintf.h>
#include <kernel/interrupt.h>
#include <kernel/display.h>
#include <kernel/arch_barrier.h>
#include <kernel/arch_platform.h>
#include <kernel/input.h>

#define VIRTIO_ID_INPUT        18
#define VIRTIO_INPUT_VQ_SIZE   32
#define VIRTIO_INPUT_CFG_ABS_INFO 0x12
#define VIRTIO_INPUT_CONFIG_SELECT 0x100
#define VIRTIO_INPUT_CONFIG_SUBSEL 0x101
#define VIRTIO_INPUT_CONFIG_SIZE   0x102
#define VIRTIO_INPUT_CONFIG_DATA   0x108

#define EV_KEY                 0x01

#define KEY_ESC                1
#define KEY_1                  2
#define KEY_2                  3
#define KEY_3                  4
#define KEY_4                  5
#define KEY_5                  6
#define KEY_6                  7
#define KEY_7                  8
#define KEY_8                  9
#define KEY_9                  10
#define KEY_0                  11
#define KEY_MINUS              12
#define KEY_EQUAL              13
#define KEY_BACKSPACE          14
#define KEY_TAB                15
#define KEY_Q                  16
#define KEY_W                  17
#define KEY_E                  18
#define KEY_R                  19
#define KEY_T                  20
#define KEY_Y                  21
#define KEY_U                  22
#define KEY_I                  23
#define KEY_O                  24
#define KEY_P                  25
#define KEY_LEFTBRACE          26
#define KEY_RIGHTBRACE         27
#define KEY_ENTER              28
#define KEY_LEFTCTRL           29
#define KEY_A                  30
#define KEY_S                  31
#define KEY_D                  32
#define KEY_F                  33
#define KEY_G                  34
#define KEY_H                  35
#define KEY_J                  36
#define KEY_K                  37
#define KEY_L                  38
#define KEY_SEMICOLON          39
#define KEY_APOSTROPHE         40
#define KEY_GRAVE              41
#define KEY_LEFTSHIFT          42
#define KEY_BACKSLASH          43
#define KEY_Z                  44
#define KEY_X                  45
#define KEY_C                  46
#define KEY_V                  47
#define KEY_B                  48
#define KEY_N                  49
#define KEY_M                  50
#define KEY_COMMA              51
#define KEY_DOT                52
#define KEY_SLASH              53
#define KEY_RIGHTSHIFT         54
#define KEY_LEFTALT            56
#define KEY_SPACE              57
#define KEY_CAPSLOCK           58
#define KEY_RIGHTCTRL          97
#define KEY_RIGHTALT           100
#define KEY_UP                 103
#define KEY_LEFT               105
#define KEY_RIGHT              106
#define KEY_DOWN               108

typedef struct {
    uint16_t type;
    uint16_t code;
    uint32_t value;
} __attribute__((packed)) virtio_input_event_t;

typedef struct {
    virtio_input_event_t event;
    uint8_t padding[64 - sizeof(virtio_input_event_t)];
} __attribute__((packed, aligned(64))) virtio_input_event_slot_t;

typedef struct {
    paddr_t phys;
    uint32_t irq;
    volatile uint32_t *mmio;
    vq_legacy_t vq;
    virtio_input_event_slot_t events[VIRTIO_INPUT_VQ_SIZE] __attribute__((aligned(64)));
    int tty_id;
    bool initialized;
    bool shift;
    bool ctrl;
    bool opt;
    bool caps_lock;
    uint32_t irq_count;
    uint32_t used_count;
    uint32_t key_events;
    uint32_t emitted_chars;
    uint32_t last_type;
    uint32_t last_code;
    uint32_t last_value;
    uint32_t last_irq_status;
    bool irq_edge_triggered;
    int32_t abs_min[2];
    int32_t abs_max[2];
} virtio_input_state_t;

static virtio_input_state_t input = {0};
static virtio_input_state_t input_secondary = {0};

static uint32_t input_divide_u64_u32(uint64_t dividend, uint32_t divisor)
{
    uint64_t quotient = 0;
    uint64_t remainder = 0;

    if (divisor == 0u)
        return 0u;
    for (int bit = 63; bit >= 0; bit--) {
        remainder = (remainder << 1) | ((dividend >> bit) & 1u);
        if (remainder >= divisor) {
            remainder -= divisor;
            quotient |= 1ULL << bit;
        }
    }
    return (uint32_t)quotient;
}

static struct vring_desc *input_desc_ptr(vq_legacy_t *vq, unsigned i)
{
    return (struct vring_desc *)((uint8_t *)(uintptr_t)vq->va_desc +
                                 i * sizeof(struct vring_desc));
}

static struct vring_avail *input_avail_ptr(vq_legacy_t *vq)
{
    return (struct vring_avail *)((uint8_t *)(uintptr_t)vq->va_avail);
}

static struct vring_used *input_used_ptr(vq_legacy_t *vq)
{
    return (struct vring_used *)((uint8_t *)(uintptr_t)vq->va_used);
}

static bool input_vq_alloc(vq_legacy_t *vq, uint16_t qsize)
{
    uint32_t desc_sz = 16u * qsize;
    uint32_t avail_sz = ALIGN_UP(6u + 2u * qsize, 2u);
    uint32_t used_sz = ALIGN_UP(6u + 8u * qsize, VQ_ALIGN);
    uint32_t total = ALIGN_UP(desc_sz, 16) +
                     ALIGN_UP(avail_sz, 2) +
                     ALIGN_UP(used_sz, VQ_ALIGN);
    size_t npages = (total + PAGE_SIZE - 1) / PAGE_SIZE;
    paddr_t pa_base = (paddr_t)allocate_pages(npages);
    uint8_t *va_base = pa_base ? (uint8_t *)phys_to_virt(pa_base) : NULL;
    uint32_t off = 0;

    if (!va_base)
        return false;

    memset(va_base, 0, npages * PAGE_SIZE);

    vq->pa_base = pa_base;
    vq->va_base = (uintptr_t)va_base;

    vq->pa_desc = pa_base + off;
    vq->va_desc = (uintptr_t)(va_base + off);
    vq->desc_size = desc_sz;
    off = ALIGN_UP(off + desc_sz, 16);

    vq->pa_avail = pa_base + off;
    vq->va_avail = (uintptr_t)(va_base + off);
    vq->avail_size = avail_sz;
    off = ALIGN_UP(off + avail_sz, 2);

    off = ALIGN_UP(off, VQ_ALIGN);
    vq->pa_used = pa_base + off;
    vq->va_used = (uintptr_t)(va_base + off);
    vq->used_size = used_sz;
    vq->qsize = qsize;
    vq->last_used_idx = 0;

    arch_clean_dcache_by_mva(va_base, npages * PAGE_SIZE);
    return true;
}

static bool input_probe_from_dtb(uint32_t index, paddr_t *out_phys,
                                 uint32_t *out_irq, bool *out_edge)
{
    paddr_t phys = 0;

    if (!fdt_find_virtio_mmio_device_at(VIRTIO_ID_INPUT, index, &phys,
                                        out_irq, out_edge))
        return false;

    *out_phys = phys;
    return true;
}

static void input_emit_char(virtio_input_state_t *state, char c)
{
    if (armos_input_tty_routing_enabled())
        tty_input_char_to_id(state->tty_id, c);
    armos_input_emit(ARMOS_INPUT_EVENT_TEXT, 0u, (uint8_t)c);
    state->emitted_chars++;
}

static void input_emit_string(virtio_input_state_t *state, const char *s)
{
    while (*s)
        input_emit_char(state, *s++);
}

static char input_translate_key(virtio_input_state_t *state, uint16_t code)
{
    /*
     * Mac French / AZERTY layout, ASCII kernel subset.
     *
     * The real macOS layout emits non-ASCII characters for several keys
     * (eacute, egrave, ccedilla, agrave, section, dead accents, etc.).
     * TTY currently transports bytes, not UTF-8 composed characters, so these
     * entries use readable ASCII fallbacks while keeping the physical layout
     * correct for shell/programming use.
     *
     * QEMU/Cocoa does not currently forward the physical Mac @/# key to the
     * guest virtio-keyboard device ("unmapped key: 0" on the host), so ArmOS
     * exposes developer fallbacks on Option+A (@) and Option+D (#).
     */
    static const char normal[128] = {
        [KEY_1] = '&', [KEY_2] = 'e', [KEY_3] = '"', [KEY_4] = '\'',
        [KEY_5] = '(', [KEY_6] = 's', [KEY_7] = 'e', [KEY_8] = '!',
        [KEY_9] = 'c', [KEY_0] = 'a', [KEY_MINUS] = ')', [KEY_EQUAL] = '-',
        [KEY_Q] = 'a', [KEY_W] = 'z', [KEY_E] = 'e', [KEY_R] = 'r',
        [KEY_T] = 't', [KEY_Y] = 'y', [KEY_U] = 'u', [KEY_I] = 'i',
        [KEY_O] = 'o', [KEY_P] = 'p', [KEY_LEFTBRACE] = '^',
        [KEY_RIGHTBRACE] = '$', [KEY_A] = 'q', [KEY_S] = 's',
        [KEY_D] = 'd', [KEY_F] = 'f', [KEY_G] = 'g', [KEY_H] = 'h',
        [KEY_J] = 'j', [KEY_K] = 'k', [KEY_L] = 'l',
        [KEY_SEMICOLON] = 'm', [KEY_APOSTROPHE] = 'u',
        [KEY_GRAVE] = '<', [KEY_BACKSLASH] = '`', [KEY_Z] = 'w',
        [KEY_X] = 'x', [KEY_C] = 'c', [KEY_V] = 'v', [KEY_B] = 'b',
        [KEY_N] = 'n', [KEY_M] = ',', [KEY_COMMA] = ';',
        [KEY_DOT] = ':', [KEY_SLASH] = '=', [KEY_SPACE] = ' ',
    };
    static const char shifted[128] = {
        [KEY_1] = '1', [KEY_2] = '2', [KEY_3] = '3', [KEY_4] = '4',
        [KEY_5] = '5', [KEY_6] = '6', [KEY_7] = '7', [KEY_8] = '8',
        [KEY_9] = '9', [KEY_0] = '0', [KEY_MINUS] = 0, [KEY_EQUAL] = '_',
        [KEY_Q] = 'A', [KEY_W] = 'Z', [KEY_E] = 'E', [KEY_R] = 'R',
        [KEY_T] = 'T', [KEY_Y] = 'Y', [KEY_U] = 'U', [KEY_I] = 'I',
        [KEY_O] = 'O', [KEY_P] = 'P', [KEY_LEFTBRACE] = '^',
        [KEY_RIGHTBRACE] = '*', [KEY_A] = 'Q', [KEY_S] = 'S',
        [KEY_D] = 'D', [KEY_F] = 'F', [KEY_G] = 'G', [KEY_H] = 'H',
        [KEY_J] = 'J', [KEY_K] = 'K', [KEY_L] = 'L',
        [KEY_SEMICOLON] = 'M', [KEY_APOSTROPHE] = '%',
        [KEY_GRAVE] = '>', [KEY_BACKSLASH] = 0, [KEY_Z] = 'W',
        [KEY_X] = 'X', [KEY_C] = 'C', [KEY_V] = 'V', [KEY_B] = 'B',
        [KEY_N] = 'N', [KEY_M] = '?', [KEY_COMMA] = '.',
        [KEY_DOT] = '/', [KEY_SLASH] = '+', [KEY_SPACE] = ' ',
    };
    static const char option[128] = {
        [KEY_5] = '{', [KEY_MINUS] = '}',
        [KEY_Q] = '@',
        [KEY_D] = '#',
        [KEY_N] = '~',
    };
    static const char option_shift[128] = {
        [KEY_5] = '[', [KEY_MINUS] = ']',
        [KEY_L] = '|', [KEY_DOT] = '\\',
    };
    bool use_shift = state->shift;
    char ascii;

    if (code >= 128)
        return 0;

    if (state->opt) {
        ascii = state->shift ? option_shift[code] : option[code];
        if (ascii)
            return ascii;
    }

    if (state->caps_lock && normal[code] >= 'a' && normal[code] <= 'z')
        use_shift = !use_shift;

    return use_shift ? shifted[code] : normal[code];
}

static void input_handle_key(virtio_input_state_t *state, uint16_t code,
                             uint32_t value)
{
    bool down = value != 0;
    char c;

    if (code == KEY_LEFTSHIFT || code == KEY_RIGHTSHIFT) {
        state->shift = down;
        return;
    }
    if (code == KEY_LEFTCTRL || code == KEY_RIGHTCTRL) {
        state->ctrl = down;
        return;
    }
    if (code == KEY_LEFTALT || code == KEY_RIGHTALT) {
        state->opt = down;
        return;
    }
    if (code == KEY_CAPSLOCK && down) {
        state->caps_lock = !state->caps_lock;
        return;
    }
    if (!down)
        return;

    switch (code) {
    case KEY_ENTER:
        input_emit_char(state, '\r');
        return;
    case KEY_BACKSPACE:
        input_emit_char(state, 0x7F);
        return;
    case KEY_TAB:
        input_emit_char(state, '\t');
        return;
    case KEY_ESC:
        input_emit_char(state, 0x1B);
        return;
    case KEY_UP:
        if (state->opt) {
            display_scrollback_up(24);
            return;
        }
        if (state->shift) {
            display_scrollback_up(1);
            return;
        }
        input_emit_string(state, "\033[A");
        return;
    case KEY_DOWN:
        if (state->opt) {
            display_scrollback_down(24);
            return;
        }
        if (state->shift) {
            display_scrollback_down(1);
            return;
        }
        input_emit_string(state, "\033[B");
        return;
    case KEY_RIGHT:
        input_emit_string(state, "\033[C");
        return;
    case KEY_LEFT:
        input_emit_string(state, "\033[D");
        return;
    default:
        break;
    }

    c = input_translate_key(state, code);
    if (!c)
        return;

    if (state->ctrl && c >= 'a' && c <= 'z')
        c = (char)(c - 'a' + 1);
    else if (state->ctrl && c >= 'A' && c <= 'Z')
        c = (char)(c - 'A' + 1);

    input_emit_char(state, c);
}

static void input_process_event(virtio_input_state_t *state,
                                const virtio_input_event_t *event)
{
    int32_t value = (int32_t)event->value;

    state->last_type = event->type;
    state->last_code = event->code;
    state->last_value = event->value;

    if (event->type == EV_KEY) {
        armos_input_emit(ARMOS_INPUT_EVENT_KEY, event->code,
                         (int32_t)event->value);
        state->key_events++;
        input_handle_key(state, event->code, event->value);
    } else if (event->type == ARMOS_INPUT_EVENT_RELATIVE ||
               event->type == ARMOS_INPUT_EVENT_ABSOLUTE) {
        if (event->type == ARMOS_INPUT_EVENT_ABSOLUTE &&
            event->code <= ARMOS_INPUT_AXIS_Y) {
            int32_t minimum = state->abs_min[event->code];
            int32_t maximum = state->abs_max[event->code];

            if (maximum > minimum) {
                if (value <= minimum) {
                    value = 0;
                } else if (value >= maximum) {
                    value = ARMOS_INPUT_ABSOLUTE_MAX;
                } else {
                    uint64_t scaled =
                        (uint64_t)(uint32_t)(value - minimum) *
                        ARMOS_INPUT_ABSOLUTE_MAX;

                    value = (int32_t)input_divide_u64_u32(
                        scaled, (uint32_t)(maximum - minimum));
                }
            }
        }
        armos_input_emit(event->type, event->code, value);
    } else if (event->type == ARMOS_INPUT_EVENT_SYNC) {
        armos_input_emit(ARMOS_INPUT_EVENT_SYNC, event->code,
                         (int32_t)event->value);
    }
}

static void input_config_write8(virtio_input_state_t *state, uint32_t offset,
                                uint8_t value)
{
    volatile uint8_t *register_ptr =
        (volatile uint8_t *)((uintptr_t)state->mmio + offset);

    arch_data_memory_barrier_inner_shareable();
    *register_ptr = value;
    arch_data_sync_barrier_inner_shareable_write();
}

static int32_t input_config_read32(virtio_input_state_t *state,
                                   uint32_t offset)
{
    return (int32_t)mmio_read32(state->mmio, offset);
}

static void input_read_absolute_range(virtio_input_state_t *state,
                                      uint16_t axis)
{
    input_config_write8(state, VIRTIO_INPUT_CONFIG_SELECT,
                        VIRTIO_INPUT_CFG_ABS_INFO);
    input_config_write8(state, VIRTIO_INPUT_CONFIG_SUBSEL, (uint8_t)axis);
    if (mmio_read8(state->mmio, VIRTIO_INPUT_CONFIG_SIZE) < 8u)
        return;

    state->abs_min[axis] =
        input_config_read32(state, VIRTIO_INPUT_CONFIG_DATA);
    state->abs_max[axis] =
        input_config_read32(state, VIRTIO_INPUT_CONFIG_DATA + 4u);
}

static void input_post_desc(virtio_input_state_t *state, uint16_t id)
{
    struct vring_avail *avail = input_avail_ptr(&state->vq);
    uint16_t idx = avail->idx;

    arch_clean_invalidate_dcache_by_mva(&state->events[id], sizeof(state->events[id]));
    avail->ring[idx % state->vq.qsize] = id;
    avail->idx = idx + 1;

    /*
     * Publish the descriptor id and the new producer index as one DMA-visible
     * update.  Cleaning the ring before changing idx left the new idx in a
     * dirty cache line on ARM, so the device kept observing an empty queue.
     */
    arch_clean_dcache_by_mva((void *)state->vq.va_avail,
                             state->vq.avail_size);
    arch_data_sync_barrier_inner_shareable_write();
}

static void input_post_all(virtio_input_state_t *state)
{
    for (uint16_t i = 0; i < state->vq.qsize; i++)
        input_post_desc(state, i);
    mmio_write32(state->mmio, VIRTIO_MMIO_QUEUE_NOTIFY, 0);
}

static void input_irq_state(virtio_input_state_t *state)
{
    if (!state->initialized)
        return;

    uint32_t irq_status = mmio_read32(state->mmio, VIRTIO_MMIO_INTERRUPT_STATUS);
    state->irq_count++;
    state->last_irq_status = irq_status;
    if (irq_status)
        mmio_write32(state->mmio, VIRTIO_MMIO_INTERRUPT_ACK, irq_status);

    arch_invalidate_dcache_by_mva((void *)state->vq.va_used,
        sizeof(struct vring_used) + state->vq.qsize * sizeof(struct vring_used_elem));
    arch_data_memory_barrier_inner_shareable();

    struct vring_used *used = input_used_ptr(&state->vq);
    while (state->vq.last_used_idx != used->idx) {
        struct vring_used_elem *elem =
            &used->ring[state->vq.last_used_idx % state->vq.qsize];
        uint16_t id = (uint16_t)elem->id;

        if (id < state->vq.qsize && elem->len >= sizeof(virtio_input_event_t)) {
            arch_invalidate_dcache_by_mva(&state->events[id], sizeof(state->events[id]));
            state->used_count++;
            input_process_event(state, &state->events[id].event);
            input_post_desc(state, id);
        }

        state->vq.last_used_idx++;
    }

    mmio_write32(state->mmio, VIRTIO_MMIO_QUEUE_NOTIFY, 0);
}

void virtio_input_irq_handler(uint32_t irq)
{
    if (input.initialized && input.irq == irq)
        input_irq_state(&input);
    if (input_secondary.initialized && input_secondary.irq == irq)
        input_irq_state(&input_secondary);
}

uint32_t virtio_input_get_irq(void)
{
    return input.initialized ? input.irq : 0;
}

bool virtio_input_handles_irq(uint32_t irq)
{
    return (input.initialized && input.irq == irq) ||
           (input_secondary.initialized && input_secondary.irq == irq);
}

bool virtio_input_is_initialized(void)
{
    return input.initialized;
}

void virtio_input_get_stats(uint32_t *irq_count, uint32_t *used_count,
                            uint32_t *key_events, uint32_t *emitted_chars,
                            uint32_t *last_type, uint32_t *last_code,
                            uint32_t *last_value, uint32_t *last_irq_status,
                            uint32_t *queue_size, uint32_t *last_used_idx,
                            uint32_t *status)
{
    if (irq_count)
        *irq_count = input.irq_count;
    if (used_count)
        *used_count = input.used_count;
    if (key_events)
        *key_events = input.key_events;
    if (emitted_chars)
        *emitted_chars = input.emitted_chars;
    if (last_type)
        *last_type = input.last_type;
    if (last_code)
        *last_code = input.last_code;
    if (last_value)
        *last_value = input.last_value;
    if (last_irq_status)
        *last_irq_status = input.last_irq_status;
    if (queue_size)
        *queue_size = input.vq.qsize;
    if (last_used_idx)
        *last_used_idx = input.vq.last_used_idx;
    if (status)
        *status = input.mmio ? mmio_read32(input.mmio, VIRTIO_MMIO_STATUS) : 0;
}

static bool input_init_state(virtio_input_state_t *state, uint32_t index,
                             int tty_id)
{
    paddr_t phys = 0;
    uint32_t irq = 0;
    bool irq_edge = true;

    memset(state, 0, sizeof(*state));
    state->tty_id = tty_id;

    if (!input_probe_from_dtb(index, &phys, &irq, &irq_edge))
        return false;

    state->phys = phys;
    state->irq = irq;
    state->irq_edge_triggered = irq_edge;
    state->mmio = arch_platform_virtio_mmio_base(phys);
    state->abs_min[ARMOS_INPUT_AXIS_X] = 0;
    state->abs_min[ARMOS_INPUT_AXIS_Y] = 0;
    state->abs_max[ARMOS_INPUT_AXIS_X] = 0;
    state->abs_max[ARMOS_INPUT_AXIS_Y] = 0;

    uint32_t magic = mmio_read32(state->mmio, VIRTIO_MMIO_MAGIC);
    uint32_t version = mmio_read32(state->mmio, VIRTIO_MMIO_VERSION);
    uint32_t devid = mmio_read32(state->mmio, VIRTIO_MMIO_DEVICE_ID);
    if (magic != 0x74726976 || version != 1 || devid != VIRTIO_ID_INPUT) {
        KERROR("virtio_input: bad device magic=0x%08X version=%u id=%u\n",
               magic, version, devid);
        return false;
    }

    mmio_write32(state->mmio, VIRTIO_MMIO_STATUS, 0);
    mmio_write32(state->mmio, VIRTIO_MMIO_STATUS, VIRTIO_STATUS_ACK);
    mmio_write32(state->mmio, VIRTIO_MMIO_STATUS,
                 mmio_read32(state->mmio, VIRTIO_MMIO_STATUS) | VIRTIO_STATUS_DRIVER);
    mmio_write32(state->mmio, VIRTIO_MMIO_DRIVER_FEATURES, 0);
    mmio_write32(state->mmio, VIRTIO_MMIO_STATUS,
                 mmio_read32(state->mmio, VIRTIO_MMIO_STATUS) | VIRTIO_STATUS_FEATURES_OK);
    if (!(mmio_read32(state->mmio, VIRTIO_MMIO_STATUS) & VIRTIO_STATUS_FEATURES_OK)) {
        KERROR("virtio_input: features rejected\n");
        return false;
    }

    input_read_absolute_range(state, ARMOS_INPUT_AXIS_X);
    input_read_absolute_range(state, ARMOS_INPUT_AXIS_Y);

    mmio_write32(state->mmio, VIRTIO_REG_GUEST_PAGE_SIZE, PAGE_SIZE);
    mmio_write32(state->mmio, VIRTIO_MMIO_QUEUE_SEL, 0);
    uint32_t qmax = mmio_read32(state->mmio, VIRTIO_MMIO_QUEUE_NUM_MAX);
    if (qmax == 0) {
        KERROR("virtio_input: event queue unavailable\n");
        return false;
    }
    uint16_t qsize = VIRTIO_INPUT_VQ_SIZE <= qmax ? VIRTIO_INPUT_VQ_SIZE : (uint16_t)qmax;
    mmio_write32(state->mmio, VIRTIO_MMIO_QUEUE_NUM, qsize);
    mmio_write32(state->mmio, VIRTIO_MMIO_QUEUE_ALIGN_OFF, VQ_ALIGN);

    if (!input_vq_alloc(&state->vq, qsize)) {
        KERROR("virtio_input: virtqueue allocation failed\n");
        return false;
    }

    for (uint16_t i = 0; i < qsize; i++) {
        struct vring_desc *desc = input_desc_ptr(&state->vq, i);
        desc->addr = (uint64_t)virt_to_phys((vaddr_t)&state->events[i]);
        desc->len = sizeof(state->events[i].event);
        desc->flags = VRING_DESC_F_WRITE;
        desc->next = 0;
    }
    arch_clean_dcache_by_mva((void *)state->vq.va_desc, sizeof(struct vring_desc) * qsize);

    mmio_write32(state->mmio, VIRTIO_MMIO_QUEUE_PFN, state->vq.pa_base >> 12);
    state->initialized = true;
    mmio_write32(state->mmio, VIRTIO_MMIO_STATUS,
                 mmio_read32(state->mmio, VIRTIO_MMIO_STATUS) | VIRTIO_STATUS_DRIVER_OK);
    if (state->irq_edge_triggered)
        irq_enable(state->irq);
    else
        irq_enable_level(state->irq);
    input_post_all(state);

    KINFO("VirtIO input%u initialized: phys=0x%lX irq=%u %s tty%d\n",
          index, (unsigned long)state->phys, state->irq,
          state->irq_edge_triggered ? "edge" : "level", tty_id);
    return true;
}

bool virtio_input_init(int tty_id)
{
    bool primary = input_init_state(&input, 0u, tty_id);

    if (primary)
        (void)input_init_state(&input_secondary, 1u, tty_id);
    return primary;
}
