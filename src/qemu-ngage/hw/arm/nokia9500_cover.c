/*
 * Nokia 9500 (RA-2): the cover display, a 128x128 colour panel ("CuiLcd.Lilly2") on Microwire chip
 * select 0, driven by the phone's own Cuilcd.ldd / Cuilcdpdd.pdd (docs/research/ra2-cover-display.md).
 *
 * 9-bit words: bit 8 = D/C (0 command, 1 parameter or pixel data). Commands (MIPI DCS / PCF8833
 * style): SWRESET 01, SLPIN 10, SLPOUT 11, NORON 13, INVOFF 20, GAMSET 26, DISPOFF 28, DISPON 29,
 * CASET 2A, PASET 2B, RAMWR 2C, RGBSET 2D, MADCTL 36, COLMOD 3A (2 = 8 bpp, 3 = 12 bpp, 5 = 16 bpp),
 * SETCON 25, 0xC3 (vendor). The controller RAM is 132x132; the visible 128x128 starts at row and
 * column 1 (the PDD's CASET/PASET add 1) [V]. 12 bpp packs two pixels in three bytes (R1G1 B1R2
 * G2B2) [V packing, I byte order]. The PDD never reads back.
 *
 * The panel is its own QEMU graphic console (the inner 640x200 is the OMAP LCD controller's), bound
 * to a device with id "cover" so QMP can name it (screendump device=cover).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "qemu/log.h"
#include "ui/console.h"
#include "hw/arm/omap.h"
#include "hw/core/sysbus.h"
#include "monitor/qdev.h"
#include "qapi/error.h"
#include "nokia9500_cover.h"

#define RAM_W 132
#define RAM_H 132
#define VIS   128

typedef struct {
    QemuConsole *con;
    uint8_t cmd;
    unsigned nparam;
    int xs, xe, ys, ye, x, y;
    uint8_t colmod;
    uint8_t acc[3];
    unsigned nacc;
    bool on, sleep, dirty;
    uint32_t ram[RAM_W * RAM_H];    /* xRGB8888 */
    unsigned words;
} Ra2Cover;

static Ra2Cover *ra2_cover;

static uint32_t c4(unsigned v)
{
    return (v & 15) * 0x11;
}

static void cover_put(Ra2Cover *s, uint32_t rgb)
{
    if (s->x < RAM_W && s->y < RAM_H) {
        s->ram[s->y * RAM_W + s->x] = rgb;
    }
    s->dirty = true;
    if (++s->x > s->xe) {
        s->x = s->xs;
        if (++s->y > s->ye) {
            s->y = s->ys;
        }
    }
}

static void cover_pixels(Ra2Cover *s, uint8_t b)
{
    s->acc[s->nacc++] = b;
    switch (s->colmod) {
    case 2:                                         /* 8 bpp RGB332 [I] */
        cover_put(s, ((b >> 5) * 255 / 7) << 16 | (((b >> 2) & 7) * 255 / 7) << 8 | (b & 3) * 85);
        s->nacc = 0;
        break;
    case 5:                                         /* 16 bpp RGB565 [I] */
        if (s->nacc == 2) {
            unsigned p = s->acc[0] << 8 | s->acc[1];

            cover_put(s, ((p >> 11) * 255 / 31) << 16 | (((p >> 5) & 63) * 255 / 63) << 8 |
                         (p & 31) * 255 / 31);
            s->nacc = 0;
        }
        break;
    default:                                        /* 12 bpp, 2 pixels in 3 bytes */
        if (s->nacc == 3) {
            cover_put(s, c4(s->acc[0] >> 4) << 16 | c4(s->acc[0]) << 8 | c4(s->acc[1] >> 4));
            cover_put(s, c4(s->acc[1]) << 16 | c4(s->acc[2] >> 4) << 8 | c4(s->acc[2]));
            s->nacc = 0;
        }
    }
}

static void cover_send(void *opaque, uint16_t w, int nbits)
{
    Ra2Cover *s = opaque;
    uint8_t b = w & 0xff;

    s->words++;
    if (nbits != 9) {
        qemu_log_mask(LOG_UNIMP, "nokia9500-cover: %d-bit word %03x ignored\n", nbits, w);
        return;
    }
    if (!(w & 0x100)) {                             /* command */
        s->cmd = b;
        s->nparam = 0;
        s->nacc = 0;
        switch (b) {
        case 0x01:                                  /* SWRESET */
            s->on = false;
            s->sleep = true;
            s->colmod = 3;
            s->xs = s->ys = 0;
            s->xe = RAM_W - 1;
            s->ye = RAM_H - 1;
            s->dirty = true;
            break;
        case 0x10:
            s->sleep = true;
            s->dirty = true;
            break;
        case 0x11:
            s->sleep = false;
            s->dirty = true;
            break;
        case 0x28:
            s->on = false;
            s->dirty = true;
            break;
        case 0x29:
            s->on = true;
            s->dirty = true;
            break;
        case 0x2c:
            s->x = s->xs;
            s->y = s->ys;
            break;
        }
        return;
    }
    switch (s->cmd) {                               /* parameters and data */
    case 0x2a:
        if (s->nparam++ == 0) {
            s->xs = b;
        } else {
            s->xe = b;
        }
        break;
    case 0x2b:
        if (s->nparam++ == 0) {
            s->ys = b;
        } else {
            s->ye = b;
        }
        break;
    case 0x3a:
        s->colmod = b & 7;
        break;
    case 0x2c:
        cover_pixels(s, b);
        break;
    default:                                        /* RGBSET, GAMSET, MADCTL, SETCON, 0xC3 */
        break;
    }
}

static uint16_t cover_receive(void *opaque, int nbits)
{
    return 0;                                       /* the PDD reads nothing back [I] */
}

static bool cover_update(void *opaque)
{
    Ra2Cover *s = opaque;
    DisplaySurface *surf = qemu_console_surface(s->con);
    uint32_t *d;

    if (!s->dirty || !surf) {
        return true;
    }
    s->dirty = false;
    d = surface_data(surf);
    for (int y = 0; y < VIS; y++) {
        for (int x = 0; x < VIS; x++) {
            d[y * VIS + x] = (s->on && !s->sleep) ? s->ram[(y + 1) * RAM_W + x + 1] : 0;
        }
    }
    qemu_console_update(s->con, 0, 0, VIS, VIS);
    return true;
}

static void cover_invalidate(void *opaque)
{
    ((Ra2Cover *)opaque)->dirty = true;
}

static const GraphicHwOps cover_ops = {
    .invalidate = cover_invalidate,
    .gfx_update = cover_update,
};

/* a device for the panel's console, only so that QMP can address it by id */
#define TYPE_RA2_COVER "nokia9500-cover"

static const TypeInfo cover_info = {
    .name = TYPE_RA2_COVER,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(SysBusDevice),
};

static void cover_register(void)
{
    type_register_static(&cover_info);
}
type_init(cover_register)

void ra2_cover_init(struct omap_mpu_state_s *mpu)
{
    Ra2Cover *s = g_new0(Ra2Cover, 1);
    DeviceState *dev = qdev_new(TYPE_RA2_COVER);

    s->colmod = 3;
    s->xe = RAM_W - 1;
    s->ye = RAM_H - 1;
    s->sleep = true;
    qdev_set_id(dev, g_strdup("cover"), &error_fatal);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(dev), &error_fatal);
    s->con = qemu_graphic_console_create(dev, 0, &cover_ops, s);
    qemu_console_resize(s->con, VIS, VIS);
    omap_uwire_attach(mpu->microwire, 0, cover_send, cover_receive, s);
    ra2_cover = s;
}
