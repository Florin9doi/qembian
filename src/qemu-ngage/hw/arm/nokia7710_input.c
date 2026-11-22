/*
 * Nokia 7710 (RM-12) input on -M nokia7710: the touch screen and the hardware keys.
 * docs/research/rm12-digitiser.md and docs/research/rm12-keys.md have the evidence.
 *
 * Touch: an ADS7846-class controller on the OMAP Microwire, chip select 0, driven by the phone's own
 * exyin.dll through EuWire.dll [V]. Each transfer writes a command byte and reads 12 bits:
 *   0xD1.. X (12-bit), 0x91.. Y (12-bit), 0xB9.. Z1 and 0xC9.. Z2 (8-bit conversions read as z << 4),
 *   0x94.. ends a burst. Pen up: Z1 = 0, Z2 = 0x0FF0 (exyin's start-up self-test wants exactly that).
 *   Pen down: Z1 = Z2 (pressure Rt = 0, always accepted).
 * /PENIRQ is MPUIO 4, active low; exyin flips the edge itself to see pen-down and pen-up [V].
 * The host pointer is the pen (absolute coordinates on the 640x320 console, left button). Screen ->
 * raw goes through the phone's current calibration (six words in the kernel super page, virtual
 * 0x40000994, physical 0x10565994 [V gva2gpa]: sx = (w0 x + w2 y + w4) >> 16, sy = (w3 x + w1 y + w5) >> 16,
 * exyin 0x500C8690 [V]), inverted here, so taps land where the window server expects them also after
 * the first-boot calibration.
 *
 * Keys: ekeyp.dll scans a 5-row x 6-column matrix on the MPUIO keypad (KBC bit n low = column n) [V];
 * omap_mpuio_key(row, col) presses one. Physical names of some keys are [I] (rm12-keys.md).
 *
 * Machine properties (tests, tools): touch = "X,Y,D" (D 1 down / 0 up; X Y screen pixels),
 * keyp = row * 8 + col (+64 = release), as the 9500's.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qapi/error.h"
#include "qapi/visitor.h"
#include "hw/core/irq.h"
#include "hw/arm/omap.h"
#include "hw/core/boards.h"
#include "hw/core/cpu.h"
#include "target/arm/cpu.h"
#include "system/address-spaces.h"
#include "system/reset.h"
#include "ui/input.h"
#include "qom/object.h"
#include "standard-headers/linux/input-event-codes.h"
#include "nokia7710_input.h"
#include "nokia7710_touch.h"

#define RM12_W 640
#define RM12_H 320
#define RM12_CAL_PHYS 0x10565994        /* the calibration words (see above) */

typedef struct {
    struct omap_mpuio_s *mpuio;
    qemu_irq penirq;
    uint8_t cmd;
    bool down;
    int sx, sy;             /* screen pixels */
    int abs_x, abs_y;       /* host pointer, 0..INPUT_EVENT_ABS_MAX */
    unsigned logged;
} Rm12Input;

static Rm12Input *rm12_in;

static void rm12_cal(int32_t w[6])
{
    uint8_t b[24];

    address_space_read(&address_space_memory, RM12_CAL_PHYS, MEMTXATTRS_UNSPECIFIED, b, sizeof(b));
    for (int i = 0; i < 6; i++) {
        w[i] = (int32_t)ldl_le_p(b + 4 * i);
    }
    if (!w[0] || !w[1]) {
        memcpy(w, rm12_cal_default, sizeof(rm12_cal_default));
    }
}

static void rm12ts_send(void *opaque, uint16_t data, int len)
{
    Rm12Input *s = opaque;

    s->cmd = data & 0xff;   /* the command byte (QEMU's uWire passes the top byte of TDR) */
}

static uint16_t rm12ts_receive(void *opaque, int len)
{
    Rm12Input *s = opaque;
    int32_t w[6];
    int rx, ry;
    uint16_t v = 0;

    if (s->down) {
        rm12_cal(w);
        rm12_touch_raw(w, s->sx, s->sy, &rx, &ry);
    } else {
        rx = ry = 0;
    }
    switch ((s->cmd >> 4) & 7) {
    case 5:                                 /* X */
        v = rx;
        break;
    case 1:                                 /* Y (and the 0x94 end of burst) */
        v = ry;
        break;
    case 3:                                 /* Z1 */
        v = s->down ? 0x800 : 0x000;
        break;
    case 4:                                 /* Z2 */
        v = s->down ? 0x800 : 0xff0;
        break;
    }
    if (s->logged < 64 && s->down) {
        s->logged++;
        qemu_log_mask(LOG_UNIMP, "nokia7710-touch: cmd %02x -> %03x (pen %d,%d)\n", s->cmd, v, s->sx, s->sy);
    }
    return v;
}

static void rm12_pen(Rm12Input *s, int sx, int sy, bool down)
{
    s->sx = MIN(MAX(sx, 0), RM12_W - 1);
    s->sy = MIN(MAX(sy, 0), RM12_H - 1);
    if (down != s->down) {
        s->down = down;
        qemu_set_irq(s->penirq, !down);     /* /PENIRQ: low while touched */
    }
}

/* ---- keys ------------------------------------------------------------------------------------------ */

/* host key -> (row, col) of ekeyp's matrix (scan-code table 0x500C77F0, table[row * 6 + col]) [V codes] */
static const struct { unsigned key; uint8_t row, col; } RM12_KEYS[] = {
    { KEY_UP, 0, 4 },                                   /* 0x10 5-way up */
    { KEY_DOWN, 1, 5 },                                 /* 0x11 5-way down */
    { KEY_LEFT, 1, 4 },                                 /* 0x0E 5-way left */
    { KEY_RIGHT, 0, 5 },                                /* 0x0F 5-way right */
    { KEY_ENTER, 2, 3 }, { KEY_KPENTER, 2, 3 },         /* 0xAB EKeyDevice7: 5-way select [I] */
    { KEY_F1, 0, 1 },                                   /* 0x94 Menu */
    { KEY_F2, 1, 0 }, { KEY_HOME, 1, 0 },               /* 0xB4 EKeyApplication0: Desk [I] */
    { KEY_F3, 2, 0 },                                   /* 0xC4 EKeyYes: call [I] */
    { KEY_F4, 0, 3 },                                   /* 0xC5 EKeyNo: end [I] */
    { KEY_ESC, 4, 2 }, { KEY_BACKSPACE, 4, 2 },         /* 0x04 EKeyEscape: back / cancel */
    { KEY_F5, 1, 2 },                                   /* 0xAA EKeyDevice6 [I] */
    { KEY_F6, 4, 0 },                                   /* 0xA8 EKeyDevice4 [I: volume / zoom] */
    { KEY_F7, 4, 1 },                                   /* 0xA9 EKeyDevice5 [I] */
};

int rm12_key_pos(unsigned key)
{
    for (unsigned i = 0; i < ARRAY_SIZE(RM12_KEYS); i++) {
        if (RM12_KEYS[i].key == key) {
            return RM12_KEYS[i].row * 8 + RM12_KEYS[i].col;
        }
    }
    return -1;
}

/* ---- host events ----------------------------------------------------------------------------------- */

static void rm12_input_event(DeviceState *dev, QemuConsole *src, QemuInputEvent *evt)
{
    Rm12Input *s = rm12_in;

    switch (evt->type) {
    case INPUT_EVENT_KIND_ABS:
        if (evt->abs.axis == INPUT_AXIS_X) {
            s->abs_x = evt->abs.value;
        } else if (evt->abs.axis == INPUT_AXIS_Y) {
            s->abs_y = evt->abs.value;
        }
        if (s->down) {
            rm12_pen(s, (int64_t)s->abs_x * RM12_W / (INPUT_EVENT_ABS_MAX + 1),
                     (int64_t)s->abs_y * RM12_H / (INPUT_EVENT_ABS_MAX + 1), true);
        }
        break;
    case INPUT_EVENT_KIND_BTN:
        if (evt->btn.button == INPUT_BUTTON_LEFT) {
            rm12_pen(s, (int64_t)s->abs_x * RM12_W / (INPUT_EVENT_ABS_MAX + 1),
                     (int64_t)s->abs_y * RM12_H / (INPUT_EVENT_ABS_MAX + 1), evt->btn.down);
        }
        break;
    case INPUT_EVENT_KIND_KEY: {
        int pos = rm12_key_pos(evt->key.key);

        if (pos >= 0) {
            omap_mpuio_key(s->mpuio, pos / 8, pos % 8, evt->key.down);
        }
        break;
    }
    default:
        break;
    }
}

static const QemuInputHandler rm12_input_handler = {
    .name  = "nokia7710 touch and keys",
    .mask  = INPUT_EVENT_MASK_KEY | INPUT_EVENT_MASK_BTN | INPUT_EVENT_MASK_ABS,
    .event = rm12_input_event,
};

static void rm12_set_touch(Object *obj, const char *value, Error **errp)
{
    int x, y, d;

    if (sscanf(value, "%d,%d,%d", &x, &y, &d) != 3) {
        error_setg(errp, "touch: X,Y,D");
        return;
    }
    rm12_pen(rm12_in, x, y, d != 0);
}

static char *rm12_get_touch(Object *obj, Error **errp)
{
    return g_strdup_printf("%d,%d,%d", rm12_in->sx, rm12_in->sy, rm12_in->down);
}

static void rm12_set_keyp(Object *obj, Visitor *v, const char *name, void *opaque, Error **errp)
{
    uint8_t k;

    if (visit_type_uint8(v, name, &k, errp)) {
        omap_mpuio_key(rm12_in->mpuio, (k & 63) / 8, k % 8, !(k & 64));
    }
}

static void rm12_input_reset(void *opaque)
{
    Rm12Input *s = opaque;

    s->down = false;
    qemu_irq_raise(s->penirq);              /* pen up: the line idles high */
}

/*
 * ---- the panel's controller on Microwire CS 3 ---------------------------------------------------------
 * VideoDriver.dll writes 16-bit words (register << 8 | value: 09, 34-37, 30-31, 61, 75 ...) and reads two
 * registers: 0x12 (read once at start, 0 accepted [V by run]) and 0x0A, the status its power-up state
 * machine checks: in state 4 (0x5009A834) it must read 0x40, else the driver gives up (state 8) and
 * switches the LCD controller off for good. That path runs when the phone starts from a kept C: (a
 * panel power cycle); a fresh boot never reads it [V by runs: owner's phone black before, Desk after].
 * State 3 wants 0x20 (0x5009A7E4) [V code, not seen in a run]. H-rm12-panel: 0x0A always reads 0x40 [I]:
 * the controller's real status bits are not known.
 */
static unsigned rm12_panel_logged;
static uint8_t rm12_panel_cmd;
static const uint16_t rm12_panel_status = 0x40;

static void rm12_panel_send(void *opaque, uint16_t data, int len)
{
    rm12_panel_cmd = data >> 8;
    if (rm12_panel_logged < 400) {
        rm12_panel_logged++;
        qemu_log_mask(LOG_UNIMP, "nokia7710-panel: <- %04x (%d bits) pc=%08x\n", data, len,
                      current_cpu ? ARM_CPU(current_cpu)->env.regs[15] : 0);
    }
}

static uint16_t rm12_panel_receive(void *opaque, int len)
{
    if (rm12_panel_logged < 400) {
        rm12_panel_logged++;
        qemu_log_mask(LOG_UNIMP, "nokia7710-panel: -> read %d bits after %02x\n", len, rm12_panel_cmd);
    }
    return rm12_panel_cmd == 0x0a ? rm12_panel_status : 0;
}

void rm12_input_init(Object *machine, struct omap_mpu_state_s *mpu)
{
    Rm12Input *s = g_new0(Rm12Input, 1);

    rm12_in = s;
    s->mpuio = mpu->mpuio;
    s->penirq = omap_mpuio_in_get(mpu->mpuio)[4];
    omap_uwire_attach(mpu->microwire, 0, rm12ts_send, rm12ts_receive, s);
    omap_uwire_attach(mpu->microwire, 3, rm12_panel_send, rm12_panel_receive, s);
    object_property_add_str(machine, "touch", rm12_get_touch, rm12_set_touch);
    object_property_add(machine, "keyp", "uint8", NULL, rm12_set_keyp, NULL, NULL);
    qemu_input_handler_activate(qemu_input_handler_register(NULL, &rm12_input_handler));
    qemu_register_reset(rm12_input_reset, s);
}
