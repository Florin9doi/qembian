/*
 * N-Gage (EKA1) board: the keypad (Q53).
 *
 * ekeyb.dll scans a 6 x 5 key matrix itself through two UPP ports [V, static RE of ekeyb]:
 *   0x0C150400 columns: +00 input (active low, bits 0-4), +02 drive latch (low byte sets, high
 *              byte clears), +04 interrupt enable [I], +08/+0A other latches
 *   0x0C15040C rows (bits 0-5): +00 input, +02 drive latch, +04 (0x0C150410) row interrupt
 *              enable (low byte enables, high byte disables [I])
 *   0x0C150100 CTSI status: bit 2 = keypad (bit 15 = CBUS/UEM); write 1 to clear [I]
 *   0x0C150102 CTSI status 2: bit 4 = the Bluetooth module (LPRF, ngage_lprf.c) [V: cbus.dll ISR
 *              0x5037d5bc tests it and calls btldd's ISR, which writes 0x10 back to ack]
 *   interrupt source 25 (IrqCTSI_IRQ) for both [I for status 2].
 * Idle: all columns driven, row interrupts enabled; a press pulls its row. The ISR disables the
 * row interrupts, acks, and a DFC scans: drive one row, read the columns. Scan codes come from
 * ekeyb's layout 3 table (0x5041c4b0, index row * 5 + col) [V], so the board only needs to
 * know where each key sits in the matrix.
 *
 * Host keys (QEMU input layer: ngage_phone.py, QMP input-send-event) map through NGAGE_KEYS.
 * The power key is not in the matrix (UEM over CBUS, dev 1 reg 0x1F bit 1): not modelled yet.
 *
 * Nokia 6600 (-M nokia6600; research agent's static RE of its ekeyb.dll, 2026-11-18, in
 * docs/devices/nokia-6600-nhl10/bring-up.md): the same ports and scan, but [V]
 *   - two sub-matrices scanned together: rows 0-1 x sense lines 0-1 (left soft, menu, right soft,
 *     0xAA) and rows 2-5 x sense lines 2-5, where line 5 is GPIO 0x0C120C04 bit 19 (active low);
 *   - a 5-way joystick outside the matrix: 0x0C15020A bits 13 left, 1 up, 2 right, 10 down,
 *     11 select (active low); enable 0x0C150216 / disable 0x0C150214 with left 0x08, up 0x20,
 *     right 0x80, down 0x02, select 0x04, which are also its CTSI status-2 bits [I];
 *   - a switch line 0x0C15020C bit 9 (0xA8/0xA9 on change; idle high = not asserted [I]).
 * Unmapped, these read 0 = asserted (joystick held), so the 6600 board maps both pages.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "qemu/log.h"
#include "hw/core/irq.h"
#include "system/memory.h"
#include "standard-headers/linux/input-event-codes.h"
#include "ui/input.h"
#include "ngage_keys.h"

#define NGAGE_KPD_BASE   0x0c150400
#define NGAGE_CTSI_BASE  0x0c150100
#define NGAGE_CTSI_KPD   0x0004

/* host key -> matrix position (row, column) [V: ekeyb layout 3; key names inferred from the
 * standard scan codes] */
static const NgageKey ngage_keys[] = {
    { KEY_UP, "up", 0, 0 },          { KEY_LEFT, "left", 0, 1 },
    { KEY_F2, "right soft", 0, 2 },  { KEY_BACKSPACE, "clear", 0, 3 },
    { KEY_F5, "menu", 0, 4 },
    { KEY_DOWN, "down", 1, 0 },      { KEY_RIGHT, "right", 1, 1 },
    { KEY_F4, "end", 1, 2 },         { KEY_LEFTSHIFT, "edit", 1, 3 },
    { KEY_ENTER, "select", 1, 4 },
    { KEY_F1, "left soft", 2, 0 },   { KEY_1, "1", 2, 2 }, { KEY_6, "6", 2, 3 }, { KEY_8, "8", 2, 4 },
    { KEY_F3, "call", 3, 0 },        { KEY_5, "5", 3, 2 }, { KEY_7, "7", 3, 3 }, { KEY_F10, "#", 3, 4 },
    { KEY_9, "9", 4, 2 },            { KEY_0, "0", 4, 3 }, { KEY_3, "3", 4, 4 },
    { KEY_KPASTERISK, "*", 5, 2 },   { KEY_2, "2", 5, 3 }, { KEY_4, "4", 5, 4 },
    { KEY_F12, "power", -1, -1 },
};

/* Nokia 6600 [V positions; names from the scan codes]; row -2 = joystick bit in 0x0C15020A */
static const NgageKey nhl10_keys[] = {
    { KEY_F1, "left soft", 0, 0 },   { KEY_F5, "menu", 0, 1 },
    { KEY_F2, "right soft", 1, 0 },
    { KEY_LEFTSHIFT, "edit", 2, 2 }, { KEY_KPASTERISK, "*", 2, 3 }, { KEY_F10, "#", 2, 4 },
    { KEY_BACKSPACE, "clear", 2, 5 },
    { KEY_1, "1", 3, 2 }, { KEY_2, "2", 3, 3 }, { KEY_3, "3", 3, 4 }, { KEY_0, "0", 3, 5 },
    { KEY_4, "4", 4, 2 }, { KEY_5, "5", 4, 3 }, { KEY_6, "6", 4, 4 }, { KEY_F3, "call", 4, 5 },
    { KEY_7, "7", 5, 2 }, { KEY_8, "8", 5, 3 }, { KEY_9, "9", 5, 4 }, { KEY_F4, "end", 5, 5 },
    { KEY_LEFT, "left", -2, 13 },    { KEY_UP, "up", -2, 1 },   { KEY_RIGHT, "right", -2, 2 },
    { KEY_DOWN, "down", -2, 10 },    { KEY_ENTER, "select", -2, 11 },
    { KEY_F12, "power", -1, -1 },
};

/*
 * Nokia 3660 (NHL-8X V 05.08): the 6600's ports, scan and joystick, but ekeyb picks its second key grid
 * (ekeyb 0x2360 + 0x90 * index, index from a virtual call at init; 6x6 entries of {scan code, key id}, row * 6 + line):
 *     row 0: .     .     call  1     2     3         row 3: menu  .     .     .     .     .
 *     row 1: .     .     0     4     5     6         row 4: rsk   lsk   .     .     .     .
 *     row 2: .     .     edit  *     #     C         row 5: .     .     7     8     9     end
 * Rows 2 and 5 equal the 6600's, so only those keys worked on the 6600 grid [V static; the index 1 is inferred from which keys
 * worked: only rows 2 and 5, no soft keys].
 */
static const NgageKey nhl8x_keys[] = {
    { KEY_F3, "call", 0, 2 },        { KEY_1, "1", 0, 3 }, { KEY_2, "2", 0, 4 }, { KEY_3, "3", 0, 5 },
    { KEY_0, "0", 1, 2 },            { KEY_4, "4", 1, 3 }, { KEY_5, "5", 1, 4 }, { KEY_6, "6", 1, 5 },
    { KEY_LEFTSHIFT, "edit", 2, 2 }, { KEY_KPASTERISK, "*", 2, 3 }, { KEY_F10, "#", 2, 4 },
    { KEY_BACKSPACE, "clear", 2, 5 },
    { KEY_F5, "menu", 3, 0 },
    { KEY_F2, "right soft", 4, 0 },  { KEY_F1, "left soft", 4, 1 },
    { KEY_7, "7", 5, 2 }, { KEY_8, "8", 5, 3 }, { KEY_9, "9", 5, 4 }, { KEY_F4, "end", 5, 5 },
    { KEY_LEFT, "left", -2, 13 },    { KEY_UP, "up", -2, 1 },   { KEY_RIGHT, "right", -2, 2 },
    { KEY_DOWN, "down", -2, 10 },    { KEY_ENTER, "select", -2, 11 },
    { KEY_F12, "power", -1, -1 },
};

/*
 * Nokia 3230 (RM-51): one 6x6 table (ekeyb 0x503905F0, row * 6 + line) on the same ports as the N-Gage;
 * the joystick is in the matrix (rows 0-1) and 0x0C15020A is never read [V: docs/research/rm51-keys.md].
 * The N-Gage's layout with edit and C swapped, plus 0xAC (EStdKeyDevice8, a side key [I]) at (4, 1).
 */
static const NgageKey rm51_keys[] = {
    { KEY_UP, "up", 0, 0 },          { KEY_LEFT, "left", 0, 1 },
    { KEY_F2, "right soft", 0, 2 },  { KEY_LEFTSHIFT, "edit", 0, 3 },
    { KEY_F5, "menu", 0, 4 },
    { KEY_DOWN, "down", 1, 0 },      { KEY_RIGHT, "right", 1, 1 },
    { KEY_F4, "end", 1, 2 },         { KEY_BACKSPACE, "clear", 1, 3 },
    { KEY_ENTER, "select", 1, 4 },
    { KEY_F1, "left soft", 2, 0 },   { KEY_1, "1", 2, 2 }, { KEY_6, "6", 2, 3 }, { KEY_8, "8", 2, 4 },
    { KEY_F3, "call", 3, 0 },        { KEY_5, "5", 3, 2 }, { KEY_7, "7", 3, 3 }, { KEY_F10, "#", 3, 4 },
    { KEY_F6, "side 0xAC", 4, 1 },   { KEY_9, "9", 4, 2 }, { KEY_0, "0", 4, 3 }, { KEY_3, "3", 4, 4 },
    { KEY_KPASTERISK, "*", 5, 2 },   { KEY_2, "2", 5, 3 }, { KEY_4, "4", 5, 4 },
    { KEY_F12, "power", -1, -1 },
};

/* joystick line (bit in 0x0C15020A) -> its enable / CTSI status-2 bit [I] */
static uint16_t joy_irq_bit(int line)
{
    switch (line) {
    case 13: return 0x08;
    case 1: return 0x20;
    case 2: return 0x80;
    case 10: return 0x02;
    case 11: return 0x04;
    }
    return 0;
}

typedef struct {
    MemoryRegion mr, ctsi_mr, gpio_mr, joy_mr;
    const NgageKey *keys;
    unsigned nkeys;
    bool nhl8x;
    bool nhl10;
    bool rm51;                      /* 3230: GPIO port (line 5, switch bit 20), no joystick port */
    uint16_t joy;                   /* pressed joystick lines (bits of 0x0C15020A) */
    uint16_t joy_ier;               /* joystick interrupt enables (CTSI status-2 bits) */
    uint16_t joy_regs[16];
    uint32_t gpio_regs[8];
    qemu_irq irq;
    uint16_t ctsi, ctsi2;
    uint16_t regs[16];
    uint8_t xc, xr, ier;            /* driven columns, driven rows, row interrupt enable */
    uint8_t pressed[6];             /* pressed[row] = column bits */
} NgageKpd;

static NgageKpd kpd;

static uint8_t kpd_rows(NgageKpd *k)            /* rows pulled by a pressed key on a driven column */
{
    uint8_t v = 0;

    for (int r = 0; r < 6; r++) {
        /* 6600 / 3660: sense line 5 (GPIO bit 19) counts as always driven [I] */
        if (k->pressed[r] & (k->xc | ((k->nhl10 || k->nhl8x) ? 0x20 : 0))) {
            v |= 1 << r;
        }
    }
    return v;
}

static uint8_t kpd_cols(NgageKpd *k)            /* columns pulled through a driven row */
{
    uint8_t v = 0;

    for (int r = 0; r < 6; r++) {
        if (k->xr & (1 << r)) {
            v |= k->pressed[r];
        }
    }
    return v;
}

static void kpd_update(NgageKpd *k)
{
    if (kpd_rows(k) & k->ier) {
        k->ctsi |= NGAGE_CTSI_KPD;
    }
    qemu_set_irq(k->irq, (k->ctsi & NGAGE_CTSI_KPD) != 0 || k->ctsi2 != 0);
}

static void kpd_latch(uint8_t *l, uint16_t v, uint8_t mask)
{
    *l = (*l | (v & mask)) & ~((v >> 8) & mask);
}

static uint64_t kpd_read(void *opaque, hwaddr a, unsigned size)
{
    NgageKpd *k = opaque;

    if (a == 0x00) {
        return ~kpd_cols(k) & 0x1f;             /* active low; 6600 line 5 is on the GPIO */
    }
    if (a == 0x0c) {
        return ~kpd_rows(k) & 0x3f;
    }
    return k->regs[(a >> 1) & 15];
}

static void kpd_write(void *opaque, hwaddr a, uint64_t v, unsigned size)
{
    NgageKpd *k = opaque;

    k->regs[(a >> 1) & 15] = v;
    if (a == 0x02) {
        kpd_latch(&k->xc, v, 0x1f);
    } else if (a == 0x0e) {
        kpd_latch(&k->xr, v, 0x3f);
    } else if (a == 0x10) {
        kpd_latch(&k->ier, v, 0x3f);
    }
    kpd_update(k);
}

static uint64_t ctsi_read(void *opaque, hwaddr a, unsigned size)
{
    NgageKpd *k = opaque;

    return a == 0 ? k->ctsi : a == 2 ? k->ctsi2 : 0;
}

static void ctsi_write(void *opaque, hwaddr a, uint64_t v, unsigned size)
{
    NgageKpd *k = opaque;

    if (a == 0) {
        k->ctsi &= ~v;
    } else if (a == 2) {
        k->ctsi2 &= ~v;
    }
    kpd_update(k);
}

void ngage_ctsi2_raise(uint16_t bits)
{
    kpd.ctsi2 |= bits;
    kpd_update(&kpd);
}

/* 6600: GPIO block 0x0C120C00; +04 bit 19 = sense line 5 (active low), the rest read back */
static uint64_t gpio_read(void *opaque, hwaddr a, unsigned size)
{
    NgageKpd *k = opaque;
    uint32_t v = k->gpio_regs[(a >> 2) & 7];

    if ((a & ~3) == 0x04) {
        v |= 1u << 19;
        if (k->rm51) {
            v |= 1u << 20;              /* 3230 switch line (0xAB/0xAD): not asserted [I] */
        }
        if (kpd_cols(k) & 0x20) {
            v &= ~(1u << 19);
        }
    }
    return v >> ((a & 3) * 8);
}

static void gpio_write(void *opaque, hwaddr a, uint64_t v, unsigned size)
{
    ((NgageKpd *)opaque)->gpio_regs[(a >> 2) & 7] = v;
}

/* 6600: joystick and switch port 0x0C150200 (+0A lines, +0C bit 9 switch; active low) */
static uint64_t joy_read(void *opaque, hwaddr a, unsigned size)
{
    NgageKpd *k = opaque;

    if (a == 0x0a) {
        return ~k->joy & 0xffff;
    }
    if (a == 0x0c) {
        return k->joy_regs[6] | 0x0200;         /* switch not asserted [I] */
    }
    return k->joy_regs[(a >> 1) & 15];
}

static void joy_write(void *opaque, hwaddr a, uint64_t v, unsigned size)
{
    NgageKpd *k = opaque;

    k->joy_regs[(a >> 1) & 15] = v;
    if (a == 0x16) {
        k->joy_ier |= v & 0xee;
    } else if (a == 0x14) {
        k->joy_ier &= ~v;
    }
}

static const MemoryRegionOps gpio_ops = {
    .read = gpio_read, .write = gpio_write, .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 4 },
    .impl = { .min_access_size = 4, .max_access_size = 4 },
};

static const MemoryRegionOps joy_ops = {
    .read = joy_read, .write = joy_write, .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 2, .max_access_size = 4 },
    .impl = { .min_access_size = 2, .max_access_size = 2 },
};

static const MemoryRegionOps kpd_ops = {
    .read = kpd_read, .write = kpd_write, .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 2, .max_access_size = 4 },
    .impl = { .min_access_size = 2, .max_access_size = 2 },
};

static const MemoryRegionOps ctsi_ops = {
    .read = ctsi_read, .write = ctsi_write, .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 2, .max_access_size = 4 },
    .impl = { .min_access_size = 2, .max_access_size = 2 },
};

static void ngage_key_event(DeviceState *dev, QemuConsole *src, QemuInputEvent *evt)
{
    for (unsigned i = 0; i < kpd.nkeys; i++) {
        const NgageKey *key = &kpd.keys[i];

        if (key->host != evt->key.key) {
            continue;
        }
        qemu_log_mask(LOG_UNIMP, "ngage-key: %s %s\n", key->name, evt->key.down ? "down" : "up");
        if (key->row == -2) {                   /* 6600 joystick line */
            uint16_t b = joy_irq_bit(key->col);

            if (evt->key.down) {
                kpd.joy |= 1 << key->col;
            } else {
                kpd.joy &= ~(1 << key->col);
            }
            /*
             * Raise the line's CTSI status-2 bit on every edge. Gating it on the enables
             * (0x0C150214/16) as first modelled lost every press: the values ekeyb leaves there
             * (0x0645 / 0x0186 at idle) do not match the remap bits [V run, regression
             * "joystick_dead"; the enable layout is [I]]. ekeyb's ISR acks with 0xEE.
             */
            kpd.ctsi2 |= b;
            kpd_update(&kpd);
            return;
        }
        if (key->row < 0) {
            return;                             /* power: UEM, not modelled */
        }
        if (evt->key.down) {
            kpd.pressed[key->row] |= 1 << key->col;
        } else {
            kpd.pressed[key->row] &= ~(1 << key->col);
        }
        kpd_update(&kpd);
        return;
    }
}

static const QemuInputHandler ngage_key_handler = {
    .name  = "ngage keypad",
    .mask  = INPUT_EVENT_MASK_KEY,
    .event = ngage_key_event,
};

void ngage_keys_init(MemoryRegion *sys, qemu_irq irq, NgageKpdLayout layout)
{
    QemuInputHandlerState *s;

    kpd.irq = irq;
    kpd.nhl8x = layout == NGAGE_KPD_NHL8X;
    kpd.nhl10 = layout == NGAGE_KPD_NHL10;
    kpd.rm51 = layout == NGAGE_KPD_RM51;
    kpd.keys = kpd.nhl8x ? nhl8x_keys
             : kpd.nhl10 ? nhl10_keys
             : kpd.rm51  ? rm51_keys
             : ngage_keys;
    kpd.nkeys = kpd.nhl8x ? ARRAY_SIZE(nhl8x_keys)
              : kpd.nhl10 ? ARRAY_SIZE(nhl10_keys)
              : kpd.rm51  ? ARRAY_SIZE(rm51_keys)
              : ARRAY_SIZE(ngage_keys);
    if (kpd.rm51) {
        memory_region_init_io(&kpd.gpio_mr, NULL, &gpio_ops, &kpd, "rm51.gpio", 0x20);
        memory_region_add_subregion(sys, 0x0c120c00, &kpd.gpio_mr);
    }
    if (kpd.nhl8x || kpd.nhl10) {
        memory_region_init_io(&kpd.gpio_mr, NULL, &gpio_ops, &kpd, "nhl10.gpio", 0x20);
        memory_region_add_subregion(sys, 0x0c120c00, &kpd.gpio_mr);
        memory_region_init_io(&kpd.joy_mr, NULL, &joy_ops, &kpd, "nhl10.joystick", 0x20);
        memory_region_add_subregion(sys, 0x0c150200, &kpd.joy_mr);
    }
    memory_region_init_io(&kpd.mr, NULL, &kpd_ops, &kpd, "ngage.keypad", 0x20);
    memory_region_add_subregion(sys, NGAGE_KPD_BASE, &kpd.mr);
    memory_region_init_io(&kpd.ctsi_mr, NULL, &ctsi_ops, &kpd, "ngage.ctsi", 0x4);
    memory_region_add_subregion(sys, NGAGE_CTSI_BASE, &kpd.ctsi_mr);
    s = qemu_input_handler_register(NULL, &ngage_key_handler);
    qemu_input_handler_activate(s);
}
