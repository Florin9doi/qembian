/*
 * OMAP1610 GP timer counting (nokia6630_soc.c), without QEMU types so tests/nokia6630_gpt_test.c can
 * run it on the host. TCLR bits: 0 ST, 1 AR, 2-4 PTV, 5 PRE, 6 CE. TISR bits: 0 match, 1 overflow.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef NOKIA6630_GPT_H
#define NOKIA6630_GPT_H

#include <stdint.h>

typedef struct {
    uint32_t tclr, tldr, tmar, tisr, cnt;
} GptCore;

/* Advance the counter by `ticks` timer clocks, setting match/overflow status on the way. */
static inline void gpt_core_step(GptCore *g, uint64_t ticks)
{
    while (ticks && (g->tclr & 1)) {
        uint64_t to_ovf = 0x100000000ull - g->cnt;      /* clocks until the wrap to TLDR */
        uint64_t to_match = (uint32_t)(g->tmar - g->cnt);

        if ((g->tclr & 0x40) && to_match && to_match < to_ovf && to_match <= ticks) {
            g->tisr |= 1;
        }
        if (ticks < to_ovf) {
            g->cnt += (uint32_t)ticks;
            return;
        }
        ticks -= to_ovf;
        g->tisr |= 2;
        g->cnt = g->tldr;
        if ((g->tclr & 0x40) && g->tmar == g->cnt) {
            g->tisr |= 1;
        }
        if (!(g->tclr & 2)) {
            g->tclr &= ~1u;                            /* one-shot: stops at the overflow */
            return;
        }
        ticks %= 0x100000000ull - g->tldr;             /* whole reload periods change nothing more */
    }
}

/* Clocks until the next match or overflow (0: stopped). */
static inline uint64_t gpt_core_next(const GptCore *g)
{
    uint64_t next = 0x100000000ull - g->cnt;
    uint64_t to_match = (uint32_t)(g->tmar - g->cnt);

    if (!(g->tclr & 1)) {
        return 0;
    }
    if ((g->tclr & 0x40) && to_match && to_match < next) {
        next = to_match;
    }
    return next;
}

#endif
