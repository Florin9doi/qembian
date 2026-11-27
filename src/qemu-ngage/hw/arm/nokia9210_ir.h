/*
 * The Nokia 9210's IrDA UART (SIR) as Euart2.pdd ("Comm.Linda2", COMM::1 of irda.esk: irPhysicalComPort= 1)
 * drives it. No QEMU types: tests/nokia9210_ir_test.c; the board glue (link, pacing, interrupt sources) is in
 * nokia9210.c. Evidence: docs/research/rae3-irda.md ([V] read in the firmware, [I] inferred).
 *
 * The PDD reaches the hardware only through EKern exports (the 9210's kernel has a helper per register):
 * 0x0500C000 + [V accessors 0x500029B4..0x50002A4C]
 *   +0x04 W  divisor: 13 MHz / baud (the PDD's table at 0x50097AC8: 9600 -> 0x54A, 115200 -> 0x71)
 *   +0x08 W  line control: data bits | stop bits | parity | 1
 *   +0x0C R  receive error bits (2 parity, 4 overrun, 8 framing [I]: the PDD's 0x40/0x20/0x10 flags to EComm);
 *            the kernel's reader clears it; the PDD writes 0 before a burst and after it
 *   +0x10 W  transmit a byte
 *   +0x14 R  the byte just written is still being taken (the PDD polls it to 0 after each byte)
 *   +0x18 R  the transmitter is busy (polled to 0 before the PDD turns the line round, and before a reconfigure)
 *   +0x1C R  the received byte (the PDD keeps bits 7-0)
 *   +0x20 R  a received byte is there (the RX ISR loops while it is non-zero)
 * 0x05003000 (the GPIO/status word, catch-all) bit 21 = "an error came with this byte" (then +0x0C is read), bit
 * 22 = TX FIFO full (the TX ISR stops and waits for the next interrupt) [V Euart2 0x500976AC / 0x50097784, kernel
 * 0x50002870]: both stay 0 in this model.
 * Interrupts (bound by name; FIQ name table order at 0x5002750C): "IrqIrRx" / "IrqIrTx" = sources 13 / 14 of the
 * IRQ controller (0x05005000) [V]. RX = data in the FIFO (level). TX = the transmitter can take bytes (level,
 * always: the PDD enables it only while it has bytes to send and disables it when EComm has no more) [I].
 * On / off: 0x05008010 (set mask) / 0x0500800C (clear mask) bit 7: Configure sets it, Stop clears it [V 0x50097548,
 * 0x500973CC; Euart1 uses other bits of the same pair]. While off, bytes from the line are dropped (light that
 * nobody receives), so a peer's discovery frames do not pile up for when the port opens.
 * Not modelled: the transceiver's mode pins (0x05002024 set / 0x05002028 clear, bits 14-19: catch-all), errors,
 * pulse timing. TX is instant (the board paces RX at the baud rate).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef NOKIA9210_IR_H
#define NOKIA9210_IR_H

#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#define RAE3_IR_BASE      0x0500c000u
#define RAE3_IR_SIZE      0x24u
#define RAE3_IR_DIVISOR   0x04u
#define RAE3_IR_LINE      0x08u
#define RAE3_IR_ERROR     0x0cu
#define RAE3_IR_TXDATA    0x10u
#define RAE3_IR_TXTAKE    0x14u
#define RAE3_IR_TXBUSY    0x18u
#define RAE3_IR_RXDATA    0x1cu
#define RAE3_IR_RXREADY   0x20u
#define RAE3_IR_SRC_RX    13            /* "IrqIrRx" (FiqIrRx = 13 on the other controller) */
#define RAE3_IR_SRC_TX    14            /* "IrqIrTx" */
#define RAE3_UARTON_SET   0x05008010u   /* W 1 << n: UART n on */
#define RAE3_UARTON_CLR   0x0500800cu   /* W 1 << n: UART n off */
#define RAE3_UARTON_IR    0x80u
#define RAE3_IR_CLOCK     13000000u
#define RAE3_IR_FIFO      64            /* [I]: the ISR drains it in one loop, the depth only bounds a burst */

typedef struct Rae3Ir {
    uint32_t divisor, line, on;
    uint8_t rx[RAE3_IR_FIFO];
    unsigned rx_head, rx_count;
    unsigned baud;
    unsigned tx_total, rx_total, rx_dropped;
    void (*tx_byte)(void *opaque, uint8_t b);
    void (*set_baud)(void *opaque, unsigned baud);
    void (*rx_level)(void *opaque, bool level);     /* source 13 */
    void *opaque;
} Rae3Ir;

/* the standard rate the divisor stands for (13 MHz / divisor, within 3 %); 0 = none */
static inline unsigned rae3_ir_baud(uint32_t divisor)
{
    static const unsigned std[] = { 50, 75, 110, 134, 150, 300, 600, 1200, 1800, 2000, 2400, 3600, 4800, 7200,
                                    9600, 19200, 38400, 57600, 115200, 230400, 460800, 576000, 921600 };
    if (!divisor) {
        return 0;
    }
    unsigned b = RAE3_IR_CLOCK / divisor;
    for (unsigned i = 0; i < sizeof(std) / sizeof(std[0]); i++) {
        if (b * 100u >= std[i] * 97u && b * 100u <= std[i] * 103u) {
            return std[i];
        }
    }
    return 0;
}

static inline bool rae3_ir_open(const Rae3Ir *s)
{
    return (s->on & RAE3_UARTON_IR) != 0;
}

static inline void rae3_ir_update(Rae3Ir *s)
{
    if (s->rx_level) {
        s->rx_level(s->opaque, s->rx_count != 0);
    }
}

static inline void rae3_ir_init(Rae3Ir *s)
{
    s->divisor = s->line = s->on = 0;
    s->rx_head = s->rx_count = 0;
    s->baud = 9600;
    s->tx_total = s->rx_total = s->rx_dropped = 0;
}

/* how many bytes the line may hand over now (0 while the port is off: they are dropped, never refused) */
static inline unsigned rae3_ir_rx_room(const Rae3Ir *s)
{
    return rae3_ir_open(s) ? RAE3_IR_FIFO - s->rx_count : RAE3_IR_FIFO;
}

static inline void rae3_ir_rx(Rae3Ir *s, const uint8_t *buf, unsigned n)
{
    for (unsigned i = 0; i < n; i++) {
        if (!rae3_ir_open(s) || s->rx_count == RAE3_IR_FIFO) {
            s->rx_dropped++;
            continue;
        }
        s->rx[(s->rx_head + s->rx_count) % RAE3_IR_FIFO] = buf[i];
        s->rx_count++;
        s->rx_total++;
    }
    rae3_ir_update(s);
}

static inline uint32_t rae3_ir_read(Rae3Ir *s, uint32_t off)
{
    uint32_t v = 0;

    switch (off) {
    case RAE3_IR_DIVISOR: return s->divisor;
    case RAE3_IR_LINE:    return s->line;
    case RAE3_IR_RXREADY: return s->rx_count != 0;
    case RAE3_IR_RXDATA:
        if (s->rx_count) {
            v = s->rx[s->rx_head];
            s->rx_head = (s->rx_head + 1) % RAE3_IR_FIFO;
            s->rx_count--;
            rae3_ir_update(s);
        }
        return v;
    }
    return 0;                   /* +0x0C no errors, +0x14 / +0x18 never busy */
}

static inline void rae3_ir_write(Rae3Ir *s, uint32_t off, uint32_t v)
{
    switch (off) {
    case RAE3_IR_DIVISOR: {
        unsigned b = rae3_ir_baud(v);

        s->divisor = v;
        if (b && b != s->baud) {
            s->baud = b;
            if (s->set_baud) {
                s->set_baud(s->opaque, b);
            }
        }
        break;
    }
    case RAE3_IR_LINE:
        s->line = v;
        break;
    case RAE3_IR_TXDATA:
        s->tx_total++;
        if (s->tx_byte) {
            s->tx_byte(s->opaque, v & 0xff);
        }
        break;
    }
}

/* the UART on/off pair at 0x0500800C / 0x05008010 (the other bits belong to the other UARTs) */
static inline void rae3_ir_power(Rae3Ir *s, uint32_t set, uint32_t clr)
{
    s->on = (s->on | set) & ~clr;
    if (!rae3_ir_open(s)) {
        s->rx_head = s->rx_count = 0;
    }
    rae3_ir_update(s);
}

#endif
