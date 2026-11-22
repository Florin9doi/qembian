/*
 * Nokia 7710 (RM-12) touch screen: screen pixel -> the raw 12-bit X/Y that exyin.dll's calibration maps
 * there. Plain C (no QEMU headers) so tests/nokia7710_touch_test.c can check it on the host.
 * w = the six calibration words in exyin's layout (super page 0x40000994, exyin 0x500C8690 [V]):
 *   sx = (w0 x + w2 y + w4) >> 16,  sy = (w3 x + w1 y + w5) >> 16.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef NOKIA7710_TOUCH_H
#define NOKIA7710_TOUCH_H

#include <stdint.h>

/* exyin's defaults (0x500C828C) [V] */
static const int32_t rm12_cal_default[6] = { 11463, 6159, -25, -2, -2310033, -2175762 };

static inline void rm12_touch_raw(const int32_t w[6], int sx, int sy, int *rx, int *ry)
{
    /* aim at the pixel centre: sx*65536 + 32768 - w4 = w0 x + w2 y; sy*65536 + 32768 - w5 = w3 x + w1 y */
    double a = w[0], b = w[2], c = w[3], d = w[1];
    double u = sx * 65536.0 + 32768 - w[4], v = sy * 65536.0 + 32768 - w[5];
    double det = a * d - b * c;
    double x = det ? (u * d - b * v) / det : 0, y = det ? (a * v - c * u) / det : 0;
    int ix = (int)(x + 0.5), iy = (int)(y + 0.5);

    *rx = ix < 0 ? 0 : ix > 4095 ? 4095 : ix;
    *ry = iy < 0 ? 0 : iy > 4095 ? 4095 : iy;
}

#endif
