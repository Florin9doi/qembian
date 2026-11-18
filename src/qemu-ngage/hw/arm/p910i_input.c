/*
 * Sony Ericsson P910i board (-M p910i): input (docs/research/p910i-input.md; [V] read from the
 * drivers, [I] inferred).
 *
 * Two resistive panels behind identical ADC blocks [V access]:
 *   touch screen  phys 0x50000800, IrqTouchscreen 23 (Exyin)
 *   flip keypad   phys 0x50000900, IrqKeypad 26 (ekeypad): the flip's keys and, with the flip open,
 *                 the QWERTY are pads on a second panel; ekeypad finds the key by point-in-rectangle
 *   +0x08 control: bit 0 run [V], bit 3 [I: auto-scan]
 *   +0x10 sample: bits 25:16 raw X, bits 9:0 raw Y, bit 31 pen up [V]; reading it drops the interrupt
 *         [I: the ISR reads it three times and never masks the line]
 *   other registers (timing, windows) kept
 * Touch sends a sample every 10 ms while the pen is down [I: rate], then one pen-up sample; the keypad
 * panel one sample per press and one pen-up sample per release (one key at a time) [V].
 *
 * GPIO input pins in the sys page (+0x100, p910i_soc.c): bit 0 jog phase B, bit 1 jog phase A
 * (IrqIOPort1 42 [I: bit]), bit 2 side key (0 = pressed, IrqIOPort2 54), bit 18 flip (1 = closed,
 * IrqFlip 43) [V except where marked]. The drivers flip each pin interrupt's polarity after every
 * edge (IC +0x0C), so these are real level pins.
 * Jog dial: quadrature; A falls with B = 0 then rises with B = 1 = Device1 (up [I]); the reverse =
 * Device2 (down [I]) [V].
 *
 * Host side: the absolute pointer on the console = the pen; keys (Linux key numbers) = the keypad
 * panel's pads, the jog dial, the side key; machine property flip-closed (bool, qom-set /machine
 * flip-closed true). With the flip closed: 0-9, * (KP*), # (\), F1-F4 = the four keys above the
 * digits (scan codes 0xAE 0xAD 0xA7 0xB3 [I names]), Enter / F6 = the pad at (1023, 1023) (0xAC
 * Device8, Confirm [I]), F5 = (0, 1023) 0xB1, F7 = (0, 0) 0xB2. With it open: letters, Backspace,
 * Enter, Space, Shift, comma, period, Alt = RightFunc (and F5-F7 = the corner pads).
 * Up / Down = jog dial, PgDn or Enter-on-jog = (not modelled: the jog press is a pad [I]); F8 = side key.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/timer.h"
#include "qapi/error.h"
#include "system/memory.h"
#include "system/reset.h"
#include "hw/core/irq.h"
#include "hw/core/boards.h"
#include "qom/object.h"
#include "standard-headers/linux/input-event-codes.h"
#include "ui/input.h"
#include "p910i.h"

#define TOUCH_BASE 0x50000800
#define PAD_BASE   0x50000900
#define TOUCH_IRQ  23
#define PAD_IRQ    26
#define PEN_UP     (1u << 31)

#define GPIO_JOG_B   (1u << 0)
#define GPIO_JOG_A   (1u << 1)
#define GPIO_KEY_APP (1u << 2)
#define GPIO_FLIP    (1u << 18)

typedef struct {
    MemoryRegion mr;
    qemu_irq irq;
    const char *name;
    uint32_t reg[16];
    uint32_t sample;
    bool pending;
} P910Adc;

typedef struct {
    P910Adc touch, pad;
    /* pen */
    bool pen_down;
    int abs_x, abs_y;                   /* 0..0x7fff from the host */
    QEMUTimer *pen_timer;
    /* keypad panel: the host key held on it (0 = none) */
    unsigned pad_key;
    /* jog dial steps */
    QEMUTimer *jog_timer;
    uint32_t jog_next;                  /* GPIO value for the second half of a step */
    bool jog_busy;
    bool flip_closed;
} P910Input;

static P910Input *p910_in;

/* ---- ADC blocks ------------------------------------------------------------------------------- */

static void adc_update(P910Adc *a)
{
    qemu_set_irq(a->irq, a->pending);
}

static void adc_sample(P910Adc *a, uint32_t sample)
{
    if (!(a->reg[0x08 >> 2] & 1)) {
        return;                         /* not running */
    }
    a->sample = sample;
    a->pending = true;
    adc_update(a);
}

static uint64_t adc_read(void *opaque, hwaddr off, unsigned size)
{
    P910Adc *a = opaque;

    if (off == 0x10) {
        a->pending = false;
        adc_update(a);
        return a->sample;
    }
    return a->reg[(off >> 2) & 15];
}

static void adc_write(void *opaque, hwaddr off, uint64_t val, unsigned size)
{
    P910Adc *a = opaque;

    a->reg[(off >> 2) & 15] = val;
}

static const MemoryRegionOps adc_ops = {
    .read = adc_read,
    .write = adc_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 4 },
};

static void adc_init(P910Adc *a, MemoryRegion *sys, hwaddr base, int irq, const char *name)
{
    a->name = name;
    a->irq = p910_irq(irq);
    a->sample = PEN_UP;
    memory_region_init_io(&a->mr, NULL, &adc_ops, a, name, 0x100);
    memory_region_add_subregion_overlap(sys, base, &a->mr, 20);
}

/* ---- the pen ---------------------------------------------------------------------------------- */

/*
 * Screen pixel -> raw ADC value, the inverse of the phone's default calibration (Exyin, superpage
 * 0x40000994) [V]: x = (17451 * rawX - 2176426) >> 16, y = (22867567 - 24899 * rawY) >> 16.
 */
uint32_t p910_touch_raw(int px, int py)
{
    int64_t rx = ((int64_t)(2 * px + 1) * 32768 + 2176426 + 17451 / 2) / 17451;
    int64_t ry = (22867567 - (int64_t)(2 * py + 1) * 32768 + 24899 / 2) / 24899;

    rx = MAX(0, MIN(1023, rx));
    ry = MAX(0, MIN(1023, ry));
    return (uint32_t)rx << 16 | (uint32_t)ry;
}

static uint32_t pen_sample(P910Input *in)
{
    int px = (int)((int64_t)in->abs_x * 208 / (INPUT_EVENT_ABS_MAX + 1));
    int py = (int)((int64_t)in->abs_y * 320 / (INPUT_EVENT_ABS_MAX + 1));

    return p910_touch_raw(px, py);
}

static void pen_tick(void *opaque)
{
    P910Input *in = opaque;

    if (in->pen_down) {
        adc_sample(&in->touch, pen_sample(in));
        timer_mod(in->pen_timer, qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) + 10);
    }
}

static void pen(P910Input *in, bool down)
{
    if (down == in->pen_down) {
        return;
    }
    in->pen_down = down;
    if (down) {
        pen_tick(in);
    } else {
        timer_del(in->pen_timer);
        adc_sample(&in->touch, PEN_UP | pen_sample(in));
    }
}

/* ---- the keypad panel ---------------------------------------------------------------------------- */

typedef struct {
    unsigned key;                       /* Linux key number */
    bool closed;                        /* table A (flip closed) or B (open) */
    uint16_t x, y;                      /* pad centre, raw */
} P910Pad;

/* docs/research/p910i-input.md s. 2 [V positions; key names of the non-digit pads I] */
static const P910Pad p910_pads[] = {
    /* table A: flip closed, x in rows, y in columns */
    { KEY_F1, true, 238, 238 },  { KEY_1, true, 238, 416 }, { KEY_2, true, 238, 594 },
    { KEY_3, true, 238, 772 },
    { KEY_F2, true, 416, 238 },  { KEY_7, true, 416, 416 }, { KEY_5, true, 416, 594 },
    { KEY_9, true, 416, 772 },
    { KEY_F3, true, 594, 238 },  { KEY_KPASTERISK, true, 594, 416 }, { KEY_8, true, 594, 594 },
    { KEY_6, true, 594, 772 },
    { KEY_F4, true, 772, 238 },  { KEY_4, true, 772, 416 }, { KEY_0, true, 772, 594 },
    { KEY_BACKSLASH, true, 772, 772 },
    { KEY_ENTER, true, 1023, 1023 },
    /* table B: flip open, QWERTY (x, y) */
    { KEY_O, false, 839, 839 }, { KEY_P, false, 707, 839 }, { KEY_L, false, 574, 839 },
    { KEY_BACKSPACE, false, 441, 839 }, { KEY_DOT, false, 309, 839 }, { KEY_ENTER, false, 177, 839 },
    { KEY_I, false, 839, 707 }, { KEY_K, false, 707, 707 }, { KEY_M, false, 574, 707 },
    { KEY_LEFTALT, false, 441, 707 }, { KEY_SPACE, false, 309, 707 }, { KEY_N, false, 177, 707 },
    { KEY_J, false, 839, 574 }, { KEY_U, false, 707, 574 }, { KEY_B, false, 574, 574 },
    { KEY_H, false, 441, 574 }, { KEY_Y, false, 309, 574 }, { KEY_T, false, 177, 574 },
    { KEY_G, false, 839, 441 }, { KEY_V, false, 707, 441 }, { KEY_C, false, 574, 441 },
    { KEY_F, false, 441, 441 }, { KEY_R, false, 309, 441 }, { KEY_E, false, 177, 441 },
    { KEY_D, false, 839, 309 }, { KEY_X, false, 707, 309 }, { KEY_LEFTSHIFT, false, 574, 309 },
    { KEY_Z, false, 441, 309 }, { KEY_S, false, 309, 309 }, { KEY_W, false, 177, 309 },
    { KEY_Q, false, 839, 177 }, { KEY_A, false, 707, 177 }, { KEY_COMMA, false, 574, 177 },
};

/* the corner pads, in both tables */
static const P910Pad p910_corners[] = {
    { KEY_F5, true, 0, 1023 }, { KEY_F6, true, 1023, 1023 }, { KEY_F7, true, 0, 0 },
};

static const P910Pad *pad_find(P910Input *in, unsigned key)
{
    for (unsigned i = 0; i < ARRAY_SIZE(p910_pads); i++) {
        if (p910_pads[i].key == key && p910_pads[i].closed == in->flip_closed) {
            return &p910_pads[i];
        }
    }
    for (unsigned i = 0; i < ARRAY_SIZE(p910_corners); i++) {
        if (p910_corners[i].key == key) {
            return &p910_corners[i];
        }
    }
    return NULL;
}

static bool pad_key(P910Input *in, unsigned key, bool down)
{
    const P910Pad *p = pad_find(in, key);

    if (!p) {
        return false;
    }
    if (down && !in->pad_key) {
        in->pad_key = key;
        adc_sample(&in->pad, (uint32_t)p->x << 16 | p->y);
        qemu_log_mask(LOG_UNIMP, "p910i-input: pad (%u, %u) down\n", p->x, p->y);
    } else if (!down && in->pad_key == key) {
        in->pad_key = 0;
        adc_sample(&in->pad, PEN_UP | (uint32_t)p->x << 16 | p->y);
    }
    return true;
}

/* ---- jog dial, side key, flip ---------------------------------------------------------------- */

static void jog_tick(void *opaque)
{
    P910Input *in = opaque;

    p910_gpio_set(GPIO_JOG_A | GPIO_JOG_B, in->jog_next);
    in->jog_busy = false;
}

/* up: A falls with B = 0, then rises with B = 1; down: the reverse [V] */
static void jog_step(P910Input *in, bool up)
{
    if (in->jog_busy) {
        return;
    }
    in->jog_busy = true;
    p910_gpio_set(GPIO_JOG_B, up ? 0 : GPIO_JOG_B);
    p910_gpio_set(GPIO_JOG_A, 0);
    in->jog_next = GPIO_JOG_A | (up ? GPIO_JOG_B : 0);
    timer_mod(in->jog_timer, qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) + 5);
}

static void flip_set(P910Input *in, bool closed)
{
    in->flip_closed = closed;
    p910_gpio_set(GPIO_FLIP, closed ? GPIO_FLIP : 0);
    qemu_log_mask(LOG_UNIMP, "p910i-input: flip %s\n", closed ? "closed" : "open");
}

static bool flip_get_prop(Object *obj, Error **errp)
{
    return p910_in->flip_closed;
}

static void flip_set_prop(Object *obj, bool value, Error **errp)
{
    if (value != p910_in->flip_closed) {
        flip_set(p910_in, value);
    }
}

/* ---- host events -------------------------------------------------------------------------------- */

static void input_event(DeviceState *dev, QemuConsole *src, QemuInputEvent *evt)
{
    P910Input *in = p910_in;

    switch (evt->type) {
    case INPUT_EVENT_KIND_ABS:
        if (evt->abs.axis == INPUT_AXIS_X) {
            in->abs_x = evt->abs.value;
        } else if (evt->abs.axis == INPUT_AXIS_Y) {
            in->abs_y = evt->abs.value;
        }
        break;
    case INPUT_EVENT_KIND_BTN:
        if (evt->btn.button == INPUT_BUTTON_LEFT) {
            pen(in, evt->btn.down);
        }
        break;
    case INPUT_EVENT_KIND_KEY: {
        unsigned key = evt->key.key;
        bool down = evt->key.down;

        if (key == KEY_UP || key == KEY_DOWN) {
            if (down) {
                jog_step(in, key == KEY_UP);
            }
        } else if (key == KEY_F8) {
            p910_gpio_set(GPIO_KEY_APP, down ? 0 : GPIO_KEY_APP);
        } else if (key == KEY_F9) {
            if (down) {
                flip_set(in, !in->flip_closed);
            }
        } else {
            pad_key(in, key, down);
        }
        break;
    }
    default:
        break;
    }
}

static const QemuInputHandler input_handler = {
    .name  = "p910i input",
    .mask  = INPUT_EVENT_MASK_KEY | INPUT_EVENT_MASK_BTN | INPUT_EVENT_MASK_ABS,
    .event = input_event,
};

static void input_reset(void *opaque)
{
    P910Input *in = opaque;

    in->pen_down = false;
    in->pad_key = 0;
    in->jog_busy = false;
    timer_del(in->pen_timer);
    timer_del(in->jog_timer);
    in->touch.pending = in->pad.pending = false;
    in->touch.sample = in->pad.sample = PEN_UP;
    memset(in->touch.reg, 0, sizeof(in->touch.reg));
    memset(in->pad.reg, 0, sizeof(in->pad.reg));
    adc_update(&in->touch);
    adc_update(&in->pad);
    /* idle pins: jog A and B high, side key released (1), the flip as set [V: side key, I: jog] */
    p910_gpio_set(GPIO_JOG_A | GPIO_JOG_B | GPIO_KEY_APP | GPIO_FLIP,
                  GPIO_JOG_A | GPIO_JOG_B | GPIO_KEY_APP | (in->flip_closed ? GPIO_FLIP : 0));
}

void p910_input_init(MachineState *machine, MemoryRegion *sys)
{
    P910Input *in = g_new0(P910Input, 1);

    p910_in = in;
    adc_init(&in->touch, sys, TOUCH_BASE, TOUCH_IRQ, "p910i.touch");
    adc_init(&in->pad, sys, PAD_BASE, PAD_IRQ, "p910i.keypad-panel");
    in->pen_timer = timer_new_ms(QEMU_CLOCK_VIRTUAL, pen_tick, in);
    in->jog_timer = timer_new_ms(QEMU_CLOCK_VIRTUAL, jog_tick, in);
    object_property_add_bool(OBJECT(machine), "flip-closed", flip_get_prop, flip_set_prop);
    qemu_input_handler_activate(qemu_input_handler_register(NULL, &input_handler));
    qemu_register_reset(input_reset, in);
}
