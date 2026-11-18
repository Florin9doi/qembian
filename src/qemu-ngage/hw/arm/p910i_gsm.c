/*
 * Sony Ericsson P910i board (-M p910i): the link to the GSM side (H-p910-gsm).
 *
 * The phone's eose.dll talks to the OSE baseband through elh4drv.ldd on UART unit 2 (physical
 * 0x58000040, IrqUart3 = interrupt 7), a 16550 with byte registers on a 4-byte stride
 * [V: docs/research/p910i-gsm-link.md]. No baseband runs here: this device is the UART with a peer
 * behind it.
 *   - The link protocol and the reply table: p910i_gsm_peer.c (-M p910i,isi=src/qemu-ngage/
 *     p910i-ose-replies.txt). Every frame and every unanswered signal is logged ("p910i-gsm:").
 *   - Frames peer -> phone: [b0][len LE16][payload][b_end][trailer]; eose takes the OSE signal number
 *     from the LE16 at frame offset 4 (payload[1..2]) [V: docs/research/p910i-gsm-link.md s. 6].
 *   - After sync the GSM side sends, unasked, the StartSignal: OSE signal 0xC384, which ereq.dll
 *     hands to EStart's observer (slot 48) -> EStart's StartUp [V static]; sent at +1 s and +3 s
 *     (EStart ignores repeats [V]). b0, payload[0], b_end and the trailer are not checked [I: 00].
 * UART details the driver depends on [V]: IIR 0x0C (receive timeout) while data waits, 0x04 only with
 * at least 60 bytes queued (its handler then reads exactly 60 without checking LSR), 0x02 transmitter
 * empty; LSR bit 0 data ready, bits 5/6 transmitter empty.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/fifo8.h"
#include "qemu/timer.h"
#include "system/memory.h"
#include "system/reset.h"
#include "hw/core/irq.h"
#include "p910i.h"
#include "p910i_gsm_peer.h"
#include "qemu/error-report.h"

#define GSM_BASE 0x58000040
#define GSM_IRQ  7

typedef struct {
    MemoryRegion mr;
    qemu_irq irq;
    Fifo8 rx;                       /* peer -> phone */
    uint8_t ier, lcr, mcr, scr, dll, dlm;
    bool thr_int;
    P910Peer peer;
    uint8_t line[64];               /* phone bytes not logged yet */
    unsigned nline;
    unsigned logged;
    QEMUTimer *flush;               /* logs the phone's bytes 5 ms after the last one */
    QEMUTimer *after_sync;          /* unasked frames after sync */
    int64_t due[P910_PEER_MAX_ENTRIES];     /* per unasked frame: next send (virtual ms), -1 = done */
    int64_t synced_at;
} P910Gsm;

static uint8_t gsm_iir(P910Gsm *g)
{
    unsigned n = fifo8_num_used(&g->rx);

    if ((g->ier & 1) && n >= 60) {
        return 0x04;
    }
    if ((g->ier & 1) && n) {
        return 0x0c;
    }
    if ((g->ier & 2) && g->thr_int) {
        return 0x02;
    }
    return 0x01;
}

static void gsm_update(P910Gsm *g)
{
    qemu_set_irq(g->irq, gsm_iir(g) != 0x01);
}

static void gsm_flush_log(P910Gsm *g)
{
    if (g->nline && g->logged < 2000) {
        char hex[64 * 3 + 1];
        for (unsigned i = 0; i < g->nline; i++) {
            snprintf(hex + i * 3, 4, " %02x", g->line[i]);
        }
        g->logged++;
        qemu_log_mask(LOG_UNIMP, "p910i-gsm: phone ->%s\n", hex);
    }
    g->nline = 0;
}

static void gsm_send(P910Gsm *g, const uint8_t *b, unsigned n)
{
    for (unsigned i = 0; i < n && !fifo8_is_full(&g->rx); i++) {
        fifo8_push(&g->rx, b[i]);
    }
    gsm_update(g);
}

/* arm the timer for the earliest unasked frame still due */
static void gsm_arm_unasked(P910Gsm *g)
{
    int64_t next = -1;

    for (unsigned i = 0; i < P910_PEER_MAX_ENTRIES; i++) {
        if (g->due[i] >= 0 && (next < 0 || g->due[i] < next)) {
            next = g->due[i];
        }
    }
    if (next >= 0) {
        timer_mod(g->after_sync, next);
    } else {
        timer_del(g->after_sync);
    }
}

static void gsm_start_unasked(P910Gsm *g)
{
    uint8_t f[256];
    unsigned ms, period, i;

    for (i = 0; i < P910_PEER_MAX_ENTRIES; i++) {
        g->due[i] = p910_peer_unasked(&g->peer, i, &ms, &period, f, sizeof(f)) ? g->synced_at + ms : -1;
    }
    gsm_arm_unasked(g);
}

/* a byte from the phone */
static void gsm_peer_byte(P910Gsm *g, uint8_t b)
{
    uint8_t out[256];
    char done[256];
    bool was_synced = g->peer.synced;
    size_t n;

    if (g->nline == sizeof(g->line)) {
        gsm_flush_log(g);
    }
    g->line[g->nline++] = b;
    timer_mod(g->flush, qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) + 5);
    n = p910_peer_byte(&g->peer, b, out, sizeof(out), done, sizeof(done));
    if (done[0]) {
        gsm_flush_log(g);
        qemu_log_mask(LOG_UNIMP, "p910i-gsm: %s\n", done);
    }
    if (n) {
        gsm_send(g, out, n);
    }
    if (!was_synced && g->peer.synced) {
        g->synced_at = qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL);
        gsm_start_unasked(g);
    }
}

static void gsm_flush_tick(void *opaque)
{
    gsm_flush_log(opaque);
}

static void gsm_unasked_tick(void *opaque)
{
    P910Gsm *g = opaque;
    int64_t now = qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL);
    uint8_t f[256];
    unsigned ms, period;

    for (unsigned i = 0; i < P910_PEER_MAX_ENTRIES; i++) {
        size_t n;

        if (g->due[i] < 0 || g->due[i] > now) {
            continue;
        }
        n = p910_peer_unasked(&g->peer, i, &ms, &period, f, sizeof(f));
        if (g->logged < 2000) {
            g->logged++;
            qemu_log_mask(LOG_UNIMP, "p910i-gsm: peer -> unasked signal %02X%02X (%zu bytes)\n", f[5], f[4], n);
        }
        gsm_send(g, f, n);
        g->due[i] = period ? g->due[i] + period : -1;
    }
    gsm_arm_unasked(g);
}

static uint64_t gsm_read(void *opaque, hwaddr a, unsigned size)
{
    P910Gsm *g = opaque;
    uint8_t v = 0;

    switch (a >> 2) {
    case 0:
        if (g->lcr & 0x80) {
            return g->dll;
        }
        if (!fifo8_is_empty(&g->rx)) {
            v = fifo8_pop(&g->rx);
        }
        gsm_update(g);
        return v;
    case 1:
        return g->lcr & 0x80 ? g->dlm : g->ier;
    case 2:
        v = gsm_iir(g);
        if (v == 0x02) {
            g->thr_int = false;
            gsm_update(g);
        }
        return v | 0xc0;
    case 3: return g->lcr;
    case 4: return g->mcr;
    case 5:
        gsm_flush_log(g);
        return 0x60 | !fifo8_is_empty(&g->rx);
    case 6: return 0xb0;            /* CTS, DSR, DCD */
    case 7: return g->scr;
    }
    return 0;
}

static void gsm_write(void *opaque, hwaddr a, uint64_t val, unsigned size)
{
    P910Gsm *g = opaque;

    switch (a >> 2) {
    case 0:
        if (g->lcr & 0x80) {
            g->dll = val;
            return;
        }
        gsm_peer_byte(g, val);
        g->thr_int = true;
        break;
    case 1:
        if (g->lcr & 0x80) {
            g->dlm = val;
            return;
        }
        if ((val & 2) && !(g->ier & 2)) {
            g->thr_int = true;
        }
        g->ier = val;
        break;
    case 2:
        if (val & 2) {
            fifo8_reset(&g->rx);
        }
        break;
    case 3: g->lcr = val; break;
    case 4: g->mcr = val; break;
    case 7: g->scr = val; break;
    }
    gsm_update(g);
}

static const MemoryRegionOps gsm_ops = {
    .read = gsm_read,
    .write = gsm_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 4 },
};

static void gsm_reset(void *opaque)
{
    P910Gsm *g = opaque;

    fifo8_reset(&g->rx);
    g->ier = g->lcr = g->mcr = g->scr = 0;
    g->thr_int = false;
    p910_peer_reset(&g->peer);
    g->nline = 0;
    for (unsigned i = 0; i < P910_PEER_MAX_ENTRIES; i++) {
        g->due[i] = -1;
    }
    timer_del(g->after_sync);
    gsm_update(g);
}

void p910_gsm_init(MemoryRegion *sys)
{
    P910Gsm *g = g_new0(P910Gsm, 1);

    fifo8_create(&g->rx, 2048);
    g->irq = p910_irq(GSM_IRQ);
    if (p910_isi_file()) {
        g_autofree gchar *text = NULL;
        int bad = 0;

        if (!g_file_get_contents(p910_isi_file(), &text, NULL, NULL) ||
            p910_peer_load(&g->peer, text, &bad) < 0) {
            error_report("p910i: OSE reply table %s: line %d", p910_isi_file(), bad);
            exit(1);
        }
    }
    g->flush = timer_new_ms(QEMU_CLOCK_VIRTUAL, gsm_flush_tick, g);
    g->after_sync = timer_new_ms(QEMU_CLOCK_VIRTUAL, gsm_unasked_tick, g);
    memory_region_init_io(&g->mr, NULL, &gsm_ops, g, "p910i.gsm-uart", 0x20);
    memory_region_add_subregion_overlap(sys, GSM_BASE, &g->mr, 10);
    qemu_register_reset(gsm_reset, g);
}
