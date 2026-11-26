/*
 * UPP SIR UART (IrDA) core: see ngage_sir.h. Model of what euart1.pdd ("SIR.PDD") depends on:
 *
 * - TX: the driver writes SIR_TXCOUNT = n-1, then n bytes to SIR_TXDATA (byte in bits 15-8), and
 *   waits for SIR_IRQ_TX. Each byte goes to the line at once (the FIFO always reads as drained,
 *   SIR_STAT bit 2 always set); SIR_IRQ_TX is latched when the board's timer says the burst's
 *   char time is over (ngage_sir_tx_done), so the phone sees a real baud rate.
 * - RX: bytes from the line go to a 15-entry FIFO; SIR_IRQ_RX is a level bit while it holds data.
 *   The ISR pops SIR_RXDATA until SIR_RXPTR reads empty. A full FIFO stops the line (the chardev
 *   keeps the rest). While the port is closed (SIR_POWER_IR not set through SIR_START, or cleared
 *   through SIR_STOP; both are bit masks shared with the BlueCore UART on the DCT4 phones) bytes are
 *   dropped, as light that nobody receives: a peer's discovery frames must not pile up.
 * - Interrupt: (status & enable) != 0, on CTSI FIQ (kernel source 26).
 * Not modelled: the RX idle/end bits (0x20, 0x100: the driver's 20 ms poll timer covers them),
 * receive errors, the 0x0C14F008 bit 4 the driver sets around a burst (catch-all) [I: LED power].
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <string.h>
#include "ngage_sir.h"

#define REG(a) s->regs[((a) - SIR_BASE) / 2]

/* SIR_DIVISOR codes, from the driver's switch on the TBps value (0x503e5d48) [V codes, I names] */
static const struct { uint16_t code; unsigned baud; } sir_bauds[] = {
    { 0x254a, 1200 }, { 0x22a5, 2400 }, { 0x8152, 4800 }, { 0x40a9, 9600 },
    { 0xa054, 19200 }, { 0x602a, 38400 }, { 0x401c, 57600 }, { 0x200e, 115200 },
};

unsigned ngage_sir_baud(uint16_t code)
{
    for (unsigned i = 0; i < sizeof(sir_bauds) / sizeof(sir_bauds[0]); i++) {
        if (sir_bauds[i].code == code) {
            return sir_bauds[i].baud;
        }
    }
    return 0;
}

static bool sir_open(const NgageSir *s)
{
    return (s->power & SIR_POWER_IR) != 0;
}

static uint16_t sir_status(const NgageSir *s)
{
    return s->latched | (s->rx_count ? SIR_IRQ_RX : 0);
}

bool ngage_sir_irq_level(const NgageSir *s)
{
    return (sir_status(s) & s->enable) != 0;
}

static void sir_update(NgageSir *s)
{
    if (s->irq) {
        s->irq(s->opaque, ngage_sir_irq_level(s));
    }
}

void ngage_sir_init(NgageSir *s)
{
    s->enable = 0;
    s->latched = 0;
    s->rx_head = s->rx_count = 0;
    s->tx_expect = s->tx_burst = 0;
    s->baud = 9600;
    s->power = 0;
    memset(s->regs, 0, sizeof(s->regs));
}

bool ngage_sir_handles(uint32_t addr)
{
    addr &= ~1u;
    return addr == SIR_CTSI_DISABLE || addr == SIR_CTSI_ENABLE || addr == SIR_CTSI_STATUS ||
           (addr >= SIR_BASE && addr < SIR_END);
}

uint16_t ngage_sir_read(NgageSir *s, uint32_t addr)
{
    addr &= ~1u;
    switch (addr) {
    case SIR_CTSI_DISABLE:
    case SIR_CTSI_ENABLE:
        return s->enable;
    case SIR_CTSI_STATUS:
        return sir_status(s);
    case SIR_STAT:
        return 0x0004;                          /* transmitter empty, no RX error */
    case SIR_TXPTR:
        return 0;                               /* drained */
    case SIR_RXPTR:
        return (uint16_t)((s->rx_count & 0xf) << 4);
    case SIR_RXDATA: {
        uint16_t v = 0;
        if (s->rx_count) {
            v = (uint16_t)(s->rx[s->rx_head] << 8);
            s->rx_head = (s->rx_head + 1) % SIR_FIFO;
            s->rx_count--;
            sir_update(s);
        }
        return v;
    }
    default:
        if (addr >= SIR_BASE && addr < SIR_END) {
            return REG(addr);
        }
        return 0;
    }
}

void ngage_sir_write(NgageSir *s, uint32_t addr, uint16_t val)
{
    addr &= ~1u;
    switch (addr) {
    case SIR_CTSI_DISABLE:
        s->enable &= ~val;
        sir_update(s);
        return;
    case SIR_CTSI_ENABLE:
        s->enable |= val;
        sir_update(s);
        return;
    case SIR_CTSI_STATUS:
        s->latched &= ~val;                     /* SIR_IRQ_RX stays while the FIFO holds data */
        sir_update(s);
        return;
    case SIR_TXCOUNT:
        s->tx_expect = (unsigned)val + 1;
        s->tx_burst = 0;
        break;
    case SIR_TXDATA:
        /* create writes a 0 here before any SIR_TXCOUNT: not a byte for the line */
        if (s->tx_expect) {
            s->tx_expect--;
            s->tx_burst++;
            s->tx_total++;
            if (s->tx_byte) {
                s->tx_byte(s->opaque, (uint8_t)(val >> 8));
            }
            if (!s->tx_expect) {
                if (s->tx_burst_done) {
                    s->tx_burst_done(s->opaque, s->tx_burst);
                } else {
                    ngage_sir_tx_done(s);
                }
            }
        }
        break;
    case SIR_DIVISOR: {
        unsigned b = ngage_sir_baud(val);
        if (b && b != s->baud) {
            s->baud = b;
            if (s->set_baud) {
                s->set_baud(s->opaque, b);
            }
        }
        break;
    }
    case SIR_START:
        /* set mask: the BlueCore UART (BC02_UART1.pdd, 3230/6260) writes its own bit 12 here */
        s->power |= val;
        break;
    case SIR_STOP:
        s->power &= ~val;                       /* clear mask: another unit's bit leaves IrDA as it is */
        if (val & SIR_POWER_IR) {
            s->rx_head = s->rx_count = 0;       /* the receiver is off: what it held is gone */
            s->tx_expect = 0;
            sir_update(s);
        }
        break;
    default:
        break;
    }
    if (addr >= SIR_BASE && addr < SIR_END) {
        REG(addr) = val;
    }
}

unsigned ngage_sir_rx_room(NgageSir *s)
{
    if (!sir_open(s)) {
        return 64;                              /* accepted and dropped */
    }
    return SIR_FIFO - s->rx_count;
}

void ngage_sir_rx(NgageSir *s, const uint8_t *buf, unsigned n)
{
    if (!sir_open(s)) {
        s->rx_dropped += n;
        return;
    }
    for (unsigned i = 0; i < n; i++) {
        if (s->rx_count == SIR_FIFO) {
            s->rx_dropped++;                    /* overrun: the chardev offered more than asked */
            continue;
        }
        s->rx[(s->rx_head + s->rx_count) % SIR_FIFO] = buf[i];
        s->rx_count++;
        s->rx_total++;
    }
    sir_update(s);
}

void ngage_sir_tx_done(NgageSir *s)
{
    s->latched |= SIR_IRQ_TX;
    sir_update(s);
}
