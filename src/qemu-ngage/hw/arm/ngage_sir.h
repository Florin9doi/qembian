/*
 * UPP SIR UART (IrDA): the UART at 0x0C150510 that euart1.pdd ("SIR.PDD", COMM::1) drives, and its
 * interrupt bits in the CTSI FIQ block (0x0C15000C/0E/BA, kernel source 26 "FiqCTSI_FIQ").
 * QEMU-free core (registers, FIFOs, status); the board glue (a QEMU chardev, the char-time timer,
 * the interrupt line) is in ngage.c. Host unit test: tests/ngage_sir_test.c.
 * Evidence: docs/research/6600-irda.md (the 6600's euart1.pdd, NHL-10 V 05.53).
 *
 * SIR: the IrDA framing (BOF/EOF, escapes, CRC) and IrLAP are done in software by IrDA.prt, the
 * transceiver only turns UART bits into light pulses. So the byte stream on the chardev is the
 * raw SIR stream; two emulated phones joined by one chardev link beam to each other.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef NGAGE_SIR_H
#define NGAGE_SIR_H

#include <stdint.h>
#include <stdbool.h>

/* physical addresses of the 16-bit registers [V: euart1.pdd literals; I: the names] */
#define SIR_CTSI_DISABLE  0x0c15000c    /* W 1<<n: disable FIQ bit n (TX start: 0x28, stop: 0x38) */
#define SIR_CTSI_ENABLE   0x0c15000e    /* W 1<<n: enable FIQ bit n (power-up: 0x128) */
#define SIR_CTSI_STATUS   0x0c1500ba    /* R: CTSI FIQ status; W 1<<n: clear bit n */
#define SIR_BASE          0x0c150510
#define SIR_CTRL          0x0c150510    /* W 3, 8 (create), 4 (configure), 0x200 (power-up) */
#define SIR_LINE          0x0c150512    /* W 0x410 (create) */
#define SIR_STAT          0x0c150514    /* R bit 2: transmitter empty; bit 0: error on the last RX byte */
#define SIR_CTRL2         0x0c150516    /* W 1 (create), 0 (after a baud change) */
#define SIR_DIVISOR       0x0c150518    /* W baud code (table in ngage_sir_baud) */
#define SIR_MODE          0x0c15052c    /* W 0x13 before a baud change */
#define SIR_STOP          0x0c15053a    /* W 2 on stop */
#define SIR_START         0x0c15053c    /* W 2 on power-up */
#define SIR_TXDATA        0x0c150554    /* W byte << 8: next TX byte */
#define SIR_RXDATA        0x0c150556    /* R: byte << 8, pops the RX FIFO */
#define SIR_TXCOUNT       0x0c15055a    /* W n-1 before n TX bytes */
#define SIR_TXPTR         0x0c15055c    /* R: bits 7-4 == bits 3-0 when the TX FIFO is empty */
#define SIR_RXCTRL        0x0c150560    /* W 7 on an RX interrupt, 0x10/0x20/0/8 elsewhere */
#define SIR_RXPTR         0x0c150562    /* R: bits 7-4 == bits 3-0 when the RX FIFO is empty */
#define SIR_END           0x0c150564

/* CTSI FIQ status bits [V: the ISR's tests; I: the names] */
#define SIR_IRQ_RX        0x0008        /* RX FIFO holds data (level) */
#define SIR_IRQ_TX        0x0010        /* the TX burst went out */
#define SIR_IRQ_RXIDLE    0x0020        /* RX activity [I]; never raised by the model */
#define SIR_IRQ_RXEND     0x0100        /* RX end/error [I]; never raised by the model */

#define SIR_FIFO          15            /* the pointers are 4-bit: 16 entries would read as empty */

typedef struct NgageSir {
    uint16_t enable;                    /* CTSI FIQ enables */
    uint16_t latched;                   /* status bits set until the driver clears them */
    uint8_t rx[SIR_FIFO];
    unsigned rx_head, rx_count;
    unsigned tx_expect;                 /* bytes still announced by SIR_TXCOUNT */
    unsigned tx_burst;                  /* bytes in the burst being sent */
    unsigned baud;
    uint16_t regs[(SIR_END - SIR_BASE) / 2];
    unsigned tx_total, rx_total, rx_dropped;
    /* board callbacks (any may be NULL) */
    void (*tx_byte)(void *opaque, uint8_t b);
    void (*tx_burst_done)(void *opaque, unsigned nbytes);  /* call ngage_sir_tx_done() after the char time */
    void (*set_baud)(void *opaque, unsigned baud);
    void (*irq)(void *opaque, bool level);
    void *opaque;
} NgageSir;

void ngage_sir_init(NgageSir *s);
bool ngage_sir_handles(uint32_t addr);
uint16_t ngage_sir_read(NgageSir *s, uint32_t addr);
void ngage_sir_write(NgageSir *s, uint32_t addr, uint16_t val);

/* the line side */
unsigned ngage_sir_rx_room(NgageSir *s);        /* how many bytes the chardev may hand over */
void ngage_sir_rx(NgageSir *s, const uint8_t *buf, unsigned n);
void ngage_sir_tx_done(NgageSir *s);            /* the board's timer: the burst is on the air */

unsigned ngage_sir_baud(uint16_t code);         /* 0 = unknown code */
bool ngage_sir_irq_level(const NgageSir *s);

#endif
