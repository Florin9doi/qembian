/*
 * Nokia 9500 (RA-2): the keyboard controller ("DErinKeyboard" in EKeyb.dll) on the OMAP I2C bus,
 * address 0x56 [V: EKeyb's request at 0x5009E720; 7-bit vs 8-bit addressing I]. It interrupts on
 * MPUIO 2 (rising edge [I]); EKeyb then reads one byte (docs/research/ra2-cover-display.md):
 *   < 0xE0  a key: index = byte >> 1 into EKeyb's scan-code table (3 variants per key, at
 *           [0x5009E8FC]); bit 0 = released [I]. Keys count only while the lid is open.
 *   0xE0    lid (cover) open  -> TRawEvent 18 (ECaseOpen)
 *   0xE1    lid closed        -> TRawEvent 19 (ECaseClose)
 * The controller keeps its interrupt line up while bytes are waiting; the model pulses MPUIO 2 every
 * 50 ms until EKeyb has read them all (QEMU's MPUIO interrupts on edges only).
 * Board controls (QOM, on the machine): cover-open (bool), kbd-byte (int: queue a raw byte).
 * Host keys (QEMU input events) are sent as key bytes: RA2_KEYS maps each qcode to its
 * index in EKeyb's table (byte = index << 1, | 1 on release: bit 0 set takes EKeyb's key-up path at
 * 0x5009E8A0 [V]). The scan codes behind each index are read from the table [V]; which physical key
 * carries the application keys 0xB4-0xBB and Device6-A is [I].
 * The phone starts with the lid closed (EKeyb's default) and the controller sends nothing at
 * power-on; setting cover-open sends 0xE0 / 0xE1.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/timer.h"
#include "qapi/error.h"
#include "qapi/visitor.h"
#include "hw/core/irq.h"
#include "hw/i2c/i2c.h"
#include "hw/arm/omap.h"
#include "qom/object.h"
#include "standard-headers/linux/input-event-codes.h"
#include "ui/input.h"
#include "nokia9500_kbd.h"

#define TYPE_RA2_KBC "nokia9500-kbc"
OBJECT_DECLARE_SIMPLE_TYPE(Ra2Kbc, RA2_KBC)

struct Ra2Kbc {
    I2CSlave parent_obj;
    qemu_irq line;
    QEMUTimer *tick;
    uint8_t fifo[64];
    unsigned head, len;
    bool cover_open;
    unsigned reads;
};

static Ra2Kbc *ra2_kbc;

static void kbc_kick(Ra2Kbc *s)
{
    if (s->len) {
        qemu_irq_lower(s->line);
        qemu_irq_raise(s->line);
        timer_mod(s->tick, qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) + 50);
    } else {
        qemu_irq_lower(s->line);
    }
}

static void kbc_tick(void *opaque)
{
    kbc_kick(opaque);
}

void ra2_kbd_push(uint8_t b)
{
    Ra2Kbc *s = ra2_kbc;

    if (!s || s->len == sizeof(s->fifo)) {
        return;
    }
    s->fifo[(s->head + s->len++) % sizeof(s->fifo)] = b;
    kbc_kick(s);
}

/* host key (Linux input code, as QEMU's input events carry them) -> index into EKeyb's scan-code table at [0x5009E8FC] (3 halfwords per index) [V table] */
static const struct { unsigned key; uint8_t index; } RA2_KEYS[] = {
    { KEY_Q, 46 }, { KEY_W, 45 }, { KEY_E, 44 }, { KEY_R, 40 },
    { KEY_T, 41 }, { KEY_Y, 42 }, { KEY_U, 43 }, { KEY_I, 87 },
    { KEY_O, 90 }, { KEY_P, 91 }, { KEY_A, 54 }, { KEY_S, 53 },
    { KEY_D, 52 }, { KEY_F, 48 }, { KEY_G, 49 }, { KEY_H, 50 },
    { KEY_J, 51 }, { KEY_K, 95 }, { KEY_L, 74 }, { KEY_Z, 62 },
    { KEY_X, 61 }, { KEY_C, 60 }, { KEY_V, 56 }, { KEY_B, 57 },
    { KEY_N, 58 }, { KEY_M, 59 },
    { KEY_1, 38 }, { KEY_2, 37 }, { KEY_3, 36 }, { KEY_4, 32 },
    { KEY_5, 33 }, { KEY_6, 34 }, { KEY_7, 35 }, { KEY_8, 86 },
    { KEY_9, 82 }, { KEY_0, 83 },
    { KEY_ESC, 39 }, { KEY_TAB, 47 }, { KEY_CAPSLOCK, 55 },
    { KEY_LEFTSHIFT, 15 }, { KEY_RIGHTSHIFT, 8 }, { KEY_LEFTCTRL, 23 },
    { KEY_LEFTALT, 70 },                                 /* LeftFunc (Chr) [I] */
    { KEY_COMPOSE, 16 }, { KEY_MENU, 16 },
    { KEY_DOWN, 65 }, { KEY_UP, 66 }, { KEY_LEFT, 67 }, { KEY_RIGHT, 68 },
    { KEY_SPACE, 72 }, { KEY_APOSTROPHE, 73 }, { KEY_SEMICOLON, 75 },
    { KEY_DOT, 76 }, { KEY_COMMA, 77 }, { KEY_BACKSPACE, 80 },
    { KEY_EQUAL, 81 }, { KEY_ENTER, 88 }, { KEY_BACKSLASH, 89 },
    { KEY_SLASH, 92 },
    /* application keys EStdKeyApplication0..7 (0xB4..0xBB) */
    { KEY_F1, 31 }, { KEY_F2, 29 }, { KEY_F3, 28 }, { KEY_F4, 25 },
    { KEY_F5, 27 }, { KEY_F6, 30 }, { KEY_F7, 26 }, { KEY_F8, 24 },
    { KEY_INSERT, 64 },                                     /* 0xAE: Enter in EKdata's keymap [V] */
};

int ra2_kbd_byte(unsigned key, bool down)
{
    for (unsigned i = 0; i < ARRAY_SIZE(RA2_KEYS); i++) {
        if (RA2_KEYS[i].key == key) {
            return RA2_KEYS[i].index << 1 | !down;
        }
    }
    return -1;
}

/*
 * The four command buttons beside the inner display and the cover keypad are not on the I2C
 * controller: EKeyp.dll (0x5009D8B0) scans a 5x5 matrix on the OMAP MPUIO keypad (rows = KBR_LATCH
 * bits, step n drives KBC bit n+1; scan codes table 0x5009E35C) [V, docs/research/ra2-command-keys.md].
 * Command buttons 1..4 = EStdKeyDevice0..3 (0xA4..0xA7) -> EKeyDevice0..3, the button group's commands
 * (EikCoctl 0x50405A88) [V]; they count only after EKeyb has seen the lid open [V].
 * QEMU column = step + 1.
 */
static const struct { unsigned key; uint8_t row, col; } RA2_KEYPAD[] = {
    { KEY_F9, 0, 4 }, { KEY_F10, 0, 5 }, { KEY_F11, 1, 4 }, { KEY_F12, 1, 5 },   /* buttons 1..4 */
};

/*
 * The cover keypad, same matrix; used while the lid is closed (with the lid open these host keys
 * go to the QWERTY controller). Identified by pressing each position on the cover's idle screen /
 * menu [V by run]: digits, '*', '#', softkeys, select, send, end. The four scroll keys (scan
 * codes D6, D2, D0, D4) are [I]: (0,1) stepped the menu back once; the others showed no change and
 * after (3,1) the cover UI stopped reacting in one run.
 */
static const struct { unsigned key; uint8_t row, col; } RA2_COVER_KEYS[] = {
    { KEY_1, 2, 2 }, { KEY_2, 2, 3 }, { KEY_3, 2, 4 },
    { KEY_4, 3, 2 }, { KEY_5, 3, 3 }, { KEY_6, 3, 4 },
    { KEY_7, 4, 2 }, { KEY_8, 4, 3 }, { KEY_9, 4, 4 },
    { KEY_0, 3, 5 }, { KEY_KPASTERISK, 2, 5 }, { KEY_BACKSLASH, 4, 5 },             /* '*', '#' */
    { KEY_KP1, 2, 2 }, { KEY_KP2, 2, 3 }, { KEY_KP3, 2, 4 },
    { KEY_KP4, 3, 2 }, { KEY_KP5, 3, 3 }, { KEY_KP6, 3, 4 },
    { KEY_KP7, 4, 2 }, { KEY_KP8, 4, 3 }, { KEY_KP9, 4, 4 }, { KEY_KP0, 3, 5 },
    { KEY_F1, 0, 2 },                                       /* left softkey (DA) */
    { KEY_F2, 0, 3 },                                       /* right softkey (DC) */
    { KEY_ENTER, 1, 1 }, { KEY_KPENTER, 1, 1 },             /* select / middle softkey (DB) */
    { KEY_F3, 1, 2 },                                       /* send, green (D8) */
    { KEY_F4, 1, 3 }, { KEY_ESC, 1, 3 },                    /* end, red (D9) */
    { KEY_UP, 0, 1 }, { KEY_DOWN, 2, 1 }, { KEY_LEFT, 4, 1 }, { KEY_RIGHT, 3, 1 },   /* scroll [I] */
};

static struct omap_mpuio_s *ra2_mpuio;

int ra2_keypad_pos(unsigned key, bool lid_open)
{
    for (unsigned i = 0; i < ARRAY_SIZE(RA2_KEYPAD); i++) {
        if (RA2_KEYPAD[i].key == key) {
            return RA2_KEYPAD[i].row * 8 + RA2_KEYPAD[i].col;
        }
    }
    for (unsigned i = 0; !lid_open && i < ARRAY_SIZE(RA2_COVER_KEYS); i++) {
        if (RA2_COVER_KEYS[i].key == key) {
            return RA2_COVER_KEYS[i].row * 8 + RA2_COVER_KEYS[i].col;
        }
    }
    return -1;
}

static void kbc_key_event(DeviceState *dev, QemuConsole *src, QemuInputEvent *evt)
{
    int pos = ra2_keypad_pos(evt->key.key, ra2_kbc && ra2_kbc->cover_open);
    int b;

    if (pos >= 0) {
        if (ra2_mpuio) {
            omap_mpuio_key(ra2_mpuio, pos / 8, pos % 8, evt->key.down);
        }
        return;
    }
    b = ra2_kbd_byte(evt->key.key, evt->key.down);

    if (b >= 0) {
        ra2_kbd_push(b);
    }
}

static const QemuInputHandler kbc_key_handler = {
    .name  = "nokia9500 keyboard",
    .mask  = INPUT_EVENT_MASK_KEY,
    .event = kbc_key_event,
};

static uint8_t kbc_recv(I2CSlave *i2c)
{
    Ra2Kbc *s = RA2_KBC(i2c);
    uint8_t b = 0xff;                       /* nothing waiting [I] */

    if (s->len) {
        b = s->fifo[s->head];
        s->head = (s->head + 1) % sizeof(s->fifo);
        s->len--;
    }
    s->reads++;
    qemu_log_mask(LOG_UNIMP, "nokia9500-kbc: read %02x\n", b);
    if (!s->len) {
        qemu_irq_lower(s->line);
    }
    return b;
}

static int kbc_send(I2CSlave *i2c, uint8_t data)
{
    qemu_log_mask(LOG_UNIMP, "nokia9500-kbc: write %02x\n", data);
    return 0;
}

static void kbc_reset(DeviceState *dev)
{
    Ra2Kbc *s = RA2_KBC(dev);

    s->head = s->len = 0;
    /*
     * Nothing at power-on: EKeyb starts with the lid closed ([this+0x58] = 0 at 0x5009E4E8) [V] and
     * a lid-open byte during start-up stalls the file server's start-up (regression
     * kbc_lid_byte_at_boot). The lid is opened later through cover-open.
     */
    s->cover_open = false;
}

static void kbc_class_init(ObjectClass *oc, const void *data)
{
    I2CSlaveClass *k = I2C_SLAVE_CLASS(oc);
    DeviceClass *dc = DEVICE_CLASS(oc);

    k->recv = kbc_recv;
    k->send = kbc_send;
    device_class_set_legacy_reset(dc, kbc_reset);
}

static const TypeInfo kbc_info = {
    .name = TYPE_RA2_KBC,
    .parent = TYPE_I2C_SLAVE,
    .instance_size = sizeof(Ra2Kbc),
    .class_init = kbc_class_init,
};

static void kbc_register(void)
{
    type_register_static(&kbc_info);
}
type_init(kbc_register)

static bool ra2_get_cover(Object *obj, Error **errp)
{
    return ra2_kbc ? ra2_kbc->cover_open : false;
}

static void ra2_set_cover(Object *obj, bool open, Error **errp)
{
    if (ra2_kbc && ra2_kbc->cover_open != open) {
        ra2_kbc->cover_open = open;
        ra2_kbd_push(open ? 0xe0 : 0xe1);
    }
}

static void ra2_set_byte(Object *obj, Visitor *v, const char *name, void *opaque, Error **errp)
{
    uint8_t b;

    if (visit_type_uint8(v, name, &b, errp)) {
        ra2_kbd_push(b);
    }
}

/* "keyp": a keypad matrix key, row * 8 + col; + 64 = release (scripted presses, e.g. qom-set) */
static void ra2_set_keyp(Object *obj, Visitor *v, const char *name, void *opaque, Error **errp)
{
    uint8_t k;

    if (visit_type_uint8(v, name, &k, errp) && ra2_mpuio) {
        omap_mpuio_key(ra2_mpuio, (k & 63) / 8, k % 8, !(k & 64));
    }
}

void ra2_kbd_init(Object *machine, struct omap_mpu_state_s *mpu)
{
    I2CSlave *i2c = i2c_slave_create_simple(omap_i2c_bus(mpu->i2c[0]), TYPE_RA2_KBC, 0x56);

    ra2_kbc = RA2_KBC(i2c);
    ra2_kbc->line = omap_mpuio_in_get(mpu->mpuio)[2];
    ra2_kbc->tick = timer_new_ms(QEMU_CLOCK_VIRTUAL, kbc_tick, ra2_kbc);
    object_property_add_bool(machine, "cover-open", ra2_get_cover, ra2_set_cover);
    object_property_add(machine, "kbd-byte", "uint8", NULL, ra2_set_byte, NULL, NULL);
    object_property_add(machine, "keyp", "uint8", NULL, ra2_set_keyp, NULL, NULL);
    ra2_mpuio = mpu->mpuio;
    qemu_input_handler_activate(qemu_input_handler_register(NULL, &kbc_key_handler));
}

/*
 * The TLV320AIC23 audio codec on I2C 0x1A (Aic23.ldd: address at 0x5005BE04 [V]). It is write-only:
 * each write is two bytes, (reg << 1 | data bit 8), data bits 7..0. The model keeps the registers and
 * acknowledges; no audio. Without a device at 0x1A every write failed and DeviceManagementServer's
 * ASC1510 panicked "set input divider" (-23) after 10 tries.
 */
#define TYPE_RA2_AIC23 "nokia9500-aic23"
OBJECT_DECLARE_SIMPLE_TYPE(Ra2Aic23, RA2_AIC23)

struct Ra2Aic23 {
    I2CSlave parent_obj;
    uint8_t first;
    unsigned n;
    uint16_t reg[16];
};

static int aic23_send(I2CSlave *i2c, uint8_t data)
{
    Ra2Aic23 *s = RA2_AIC23(i2c);

    if (s->n++ % 2 == 0) {
        s->first = data;
    } else {
        s->reg[(s->first >> 1) & 15] = (s->first & 1) << 8 | data;
        qemu_log_mask(LOG_UNIMP, "nokia9500-aic23: reg %u = %03x\n", (s->first >> 1) & 15,
                      s->reg[(s->first >> 1) & 15]);
    }
    return 0;
}

static uint8_t aic23_recv(I2CSlave *i2c)
{
    return 0;                       /* write-only part [I: the driver never reads] */
}

static int aic23_event(I2CSlave *i2c, enum i2c_event e)
{
    RA2_AIC23(i2c)->n = 0;
    return 0;
}

static void aic23_class_init(ObjectClass *oc, const void *data)
{
    I2CSlaveClass *k = I2C_SLAVE_CLASS(oc);

    k->send = aic23_send;
    k->recv = aic23_recv;
    k->event = aic23_event;
}

static const TypeInfo aic23_info = {
    .name = TYPE_RA2_AIC23,
    .parent = TYPE_I2C_SLAVE,
    .instance_size = sizeof(Ra2Aic23),
    .class_init = aic23_class_init,
};

static void aic23_register(void)
{
    type_register_static(&aic23_info);
}
type_init(aic23_register)

void ra2_codec_init(struct omap_mpu_state_s *mpu)
{
    i2c_slave_create_simple(omap_i2c_bus(mpu->i2c[0]), TYPE_RA2_AIC23, 0x1a);
}
