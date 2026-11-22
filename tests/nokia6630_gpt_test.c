/*
 * Host test of the Nokia 6630 board's OMAP1610 GP timer counting (src/qemu-ngage/hw/arm/nokia6630_gpt.h).
 *   cc -O2 -Wall -o build/nokia6630_gpt_test tests/nokia6630_gpt_test.c && build/nokia6630_gpt_test
 *
 * Regression rm1_no_tick (stall 3, docs/devices/nokia-6630-rm1/bring-up.md): the kernel's tick is GP
 * timer 2 in compare mode (TLDR 0, TMAR 0x200, TCLR |= 0x43 = ST|AR|CE, TIER = match|overflow); with
 * no GP timer modelled it waited for the first tick for ever. Each tick it moves TMAR on by 0x200.
 */
#include <assert.h>
#include <stdio.h>
#include "../src/qemu-ngage/hw/arm/nokia6630_gpt.h"

static void kernel_tick_setup(void)
{
    GptCore g = { .tclr = 0x43, .tldr = 0, .tmar = 0x200, .cnt = 0 };

    assert(gpt_core_next(&g) == 0x200);             /* the first event is the match */
    gpt_core_step(&g, 0x1ff);
    assert(g.tisr == 0 && g.cnt == 0x1ff);
    gpt_core_step(&g, 1);
    assert(g.tisr == 1 && g.cnt == 0x200);          /* match exactly at TMAR */
    g.tisr = 0;                                      /* the kernel acknowledges ... */
    g.tmar += 0x200;                                 /* ... and moves the compare on */
    assert(gpt_core_next(&g) == 0x200);
    gpt_core_step(&g, 0x100);
    assert(g.tisr == 0);
    gpt_core_step(&g, 0x100);
    assert(g.tisr == 1 && g.cnt == 0x400);
}

static void no_match_without_ce(void)
{
    GptCore g = { .tclr = 0x03, .tmar = 0x10 };

    gpt_core_step(&g, 0x20);
    assert(g.tisr == 0 && g.cnt == 0x20);
    assert(gpt_core_next(&g) == 0x100000000ull - 0x20);   /* only the overflow is ahead */
}

static void overflow_reloads(void)
{
    GptCore g = { .tclr = 0x03, .tldr = 0xfffffff0, .cnt = 0xfffffff0 };

    gpt_core_step(&g, 0x10);                          /* reaches the wrap */
    assert(g.tisr == 2 && g.cnt == 0xfffffff0 && (g.tclr & 1));
    g.tisr = 0;
    gpt_core_step(&g, 0x10 * 1000 + 3);               /* many periods at once: no endless loop */
    assert(g.tisr == 2 && g.cnt == 0xfffffff3);
}

static void one_shot_stops(void)
{
    GptCore g = { .tclr = 0x01, .tldr = 5, .cnt = 0xfffffffe };

    gpt_core_step(&g, 10);
    assert(g.tisr == 2 && !(g.tclr & 1) && g.cnt == 5);
    assert(gpt_core_next(&g) == 0);
    gpt_core_step(&g, 10);                            /* stopped: no change */
    assert(g.cnt == 5);
}

static void match_at_reload_value(void)
{
    GptCore g = { .tclr = 0x43, .tldr = 0x100, .tmar = 0x100, .cnt = 0xffffffff };

    gpt_core_step(&g, 1);
    assert(g.tisr == 3 && g.cnt == 0x100);
}

static void match_behind_counter_waits_for_wrap(void)
{
    GptCore g = { .tclr = 0x43, .tldr = 0, .tmar = 0x10, .cnt = 0x20 };

    gpt_core_step(&g, 0x1000);
    assert(g.tisr == 0);
    assert(gpt_core_next(&g) == 0x100000000ull - 0x1020);
}

int main(void)
{
    kernel_tick_setup();
    no_match_without_ce();
    overflow_reloads();
    one_shot_stops();
    match_at_reload_value();
    match_behind_counter_waits_for_wrap();
    printf("nokia6630_gpt_test: ok\n");
    return 0;
}
