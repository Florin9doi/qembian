/*
 * Host test of the Nokia 7710 touch mapping (src/qemu-ngage/hw/arm/nokia7710_touch.h): the raw value the
 * board returns for a screen pixel must map back to that pixel through exyin's own formula
 * (0x500C8690), under the default calibration and under a changed one.
 *   cc -O2 -o build/nokia7710_touch_test tests/nokia7710_touch_test.c && build/nokia7710_touch_test
 */
#include <assert.h>
#include <stdio.h>
#include "../src/qemu-ngage/hw/arm/nokia7710_touch.h"

static void exyin(const int32_t w[6], int x, int y, int *sx, int *sy)
{
    *sx = (int32_t)((int64_t)w[0] * x + (int64_t)w[2] * y + w[4]) >> 16;
    *sy = (int32_t)((int64_t)w[3] * x + (int64_t)w[1] * y + w[5]) >> 16;
}

static void round_trip(const int32_t w[6])
{
    for (int sy = 0; sy < 320; sy += 7) {
        for (int sx = 0; sx < 640; sx += 5) {
            int rx, ry, bx, by;

            rm12_touch_raw(w, sx, sy, &rx, &ry);
            assert(rx >= 0 && rx <= 4095 && ry >= 0 && ry <= 4095);
            exyin(w, rx, ry, &bx, &by);
            if (bx != sx || by != sy) {
                fprintf(stderr, "(%d,%d) -> raw (%d,%d) -> (%d,%d)\n", sx, sy, rx, ry, bx, by);
                assert(0);
            }
        }
    }
}

int main(void)
{
    /* the first-boot calibration screen: the defaults [V: super page before calibration] */
    round_trip(rm12_cal_default);
    /* after a calibration the words change: still exact (taps land where the window server expects) */
    static const int32_t user[6] = { 11000, 6400, 40, -15, -2100000, -2300000 };
    round_trip(user);
    /* corners of the defaults: raw ~206..3860 x ~354..3748 (docs/research/rm12-digitiser.md) */
    int rx, ry;
    rm12_touch_raw(rm12_cal_default, 0, 0, &rx, &ry);
    assert(rx > 190 && rx < 230 && ry > 340 && ry < 370);
    rm12_touch_raw(rm12_cal_default, 639, 319, &rx, &ry);
    assert(rx > 3840 && rx < 3880 && ry > 3730 && ry < 3770);
    puts("nokia7710_touch_test: ok");
    return 0;
}
