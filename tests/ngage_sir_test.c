/*
 * Host unit test of the UPP SIR UART (src/qemu-ngage/hw/arm/ngage_sir.c), driven the way the 6600's
 * euart1.pdd ("SIR.PDD", docs/research/6600-irda.md) drives it.
 *   cc -std=gnu11 -Wall -o build/ngage_sir_test tests/ngage_sir_test.c \
 *      src/qemu-ngage/hw/arm/ngage_sir.c && build/ngage_sir_test
 */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../src/qemu-ngage/hw/arm/ngage_sir.h"

static uint8_t line[256];
static unsigned nline, bursts, last_burst, baud_set;
static bool irq;

static void tx_byte(void *o, uint8_t b) { (void)o; line[nline++] = b; }
static void burst(void *o, unsigned n) { (void)o; bursts++; last_burst = n; }
static void set_baud(void *o, unsigned b) { (void)o; baud_set = b; }
static void set_irq(void *o, bool l) { (void)o; irq = l; }

static void setup(NgageSir *s)
{
    memset(s, 0, sizeof(*s));
    ngage_sir_init(s);
    s->tx_byte = tx_byte;
    s->tx_burst_done = burst;
    s->set_baud = set_baud;
    s->irq = set_irq;
    nline = bursts = last_burst = baud_set = 0;
    irq = false;
}

/* the driver's "TX FIFO drained and transmitter empty" wait (0x503e58d4 and four copies) */
static bool tx_idle(NgageSir *s)
{
    uint16_t p = ngage_sir_read(s, SIR_TXPTR);
    return ((p & 0xf0) >> 4) == (p & 0xf) && (ngage_sir_read(s, SIR_STAT) & 4);
}

/* create (0x503e58d4) + power-up (0x503e5aa0) */
static void open_port(NgageSir *s)
{
    ngage_sir_write(s, SIR_CTRL, 3);
    ngage_sir_write(s, SIR_TXDATA, 0);          /* create writes 0 to the TX register */
    assert(tx_idle(s));
    ngage_sir_write(s, SIR_LINE, 0x410);
    ngage_sir_write(s, SIR_CTRL, 8);
    ngage_sir_write(s, SIR_CTRL2, 1);
    ngage_sir_write(s, SIR_RXCTRL, 0);
    ngage_sir_write(s, SIR_CTSI_STATUS, 0x10);
    ngage_sir_write(s, SIR_CTRL, 0x200);
    ngage_sir_write(s, SIR_START, 2);
    ngage_sir_write(s, SIR_CTSI_STATUS, 0x128);
    ngage_sir_write(s, SIR_CTSI_ENABLE, 0x128);
}

/* the ISR's receive half (0x503e61c4): returns the bytes drained */
static unsigned isr_rx(NgageSir *s, uint8_t *out)
{
    unsigned n = 0;
    uint16_t st = ngage_sir_read(s, SIR_CTSI_STATUS);

    if (!(st & 0x128)) {
        return 0;
    }
    if (st & 0x108) {
        ngage_sir_write(s, SIR_RXCTRL, 7);
    }
    ngage_sir_write(s, SIR_CTSI_STATUS, 0x128);
    for (;;) {
        uint16_t p = ngage_sir_read(s, SIR_RXPTR);
        if (((p & 0xf0) >> 4) == (p & 0xf)) {
            break;
        }
        out[n++] = ngage_sir_read(s, SIR_RXDATA) >> 8;
        assert(!(ngage_sir_read(s, SIR_STAT) & 1));     /* no receive error */
    }
    return n;
}

/* TX start (0x503e5c3c): half-duplex switch, count, bytes (in bits 15-8) */
static void send(NgageSir *s, const uint8_t *p, unsigned n)
{
    ngage_sir_write(s, SIR_CTSI_STATUS, 0x10);
    ngage_sir_write(s, SIR_CTSI_ENABLE, 0x10);
    ngage_sir_write(s, SIR_CTSI_DISABLE, 0x28);
    ngage_sir_write(s, SIR_TXCOUNT, n - 1);
    for (unsigned i = 0; i < n; i++) {
        ngage_sir_write(s, SIR_TXDATA, p[i] << 8);
    }
}

static void test_create_sends_nothing(void)
{
    NgageSir s;
    setup(&s);
    open_port(&s);
    assert(nline == 0 && bursts == 0);
    assert(!irq);
}

static void test_baud_codes(void)
{
    NgageSir s;
    setup(&s);
    assert(ngage_sir_baud(0x200e) == 115200);
    assert(ngage_sir_baud(0x40a9) == 9600);
    assert(ngage_sir_baud(0x1234) == 0);
    /* configure (0x503e5d48): mode, control, divisor, control2 */
    ngage_sir_write(&s, SIR_MODE, 0x13);
    ngage_sir_write(&s, SIR_CTRL, 4);
    ngage_sir_write(&s, SIR_DIVISOR, 0x200e);
    ngage_sir_write(&s, SIR_CTRL2, 0);
    assert(baud_set == 115200 && s.baud == 115200);
    baud_set = 0;
    ngage_sir_write(&s, SIR_DIVISOR, 0x200e);    /* unchanged: no ioctl */
    assert(baud_set == 0);
    ngage_sir_write(&s, SIR_DIVISOR, 0x7777);    /* unknown code: the rate stays */
    assert(s.baud == 115200);
}

static void test_tx_burst_then_irq(void)
{
    NgageSir s;
    const uint8_t xid[] = { 0xc0, 0xff, 0x3f, 0x01, 0x7d, 0x5e, 0xc1 };
    setup(&s);
    open_port(&s);
    send(&s, xid, sizeof(xid));
    assert(nline == sizeof(xid) && memcmp(line, xid, sizeof(xid)) == 0);
    assert(bursts == 1 && last_burst == sizeof(xid));
    assert(!irq);                               /* not before the char time is over */
    ngage_sir_tx_done(&s);
    assert(irq);
    assert(ngage_sir_read(&s, SIR_CTSI_STATUS) & 0x10);
    ngage_sir_write(&s, SIR_CTSI_STATUS, 0x10);
    assert(!irq);
    assert(tx_idle(&s));
}

static void test_rx_fifo_and_irq(void)
{
    NgageSir s;
    uint8_t out[32];
    const uint8_t in[] = { 0xc0, 0x01, 0x02, 0xc1 };
    setup(&s);
    open_port(&s);
    assert(ngage_sir_rx_room(&s) == SIR_FIFO);
    ngage_sir_rx(&s, in, sizeof(in));
    assert(irq);
    assert(isr_rx(&s, out) == sizeof(in) && memcmp(out, in, sizeof(in)) == 0);
    assert(!irq);
}

/* half duplex: RX masked during a burst; what came meanwhile is delivered when TX ends */
static void test_rx_held_during_tx(void)
{
    NgageSir s;
    uint8_t out[32];
    const uint8_t b = 0x55;
    setup(&s);
    open_port(&s);
    send(&s, &b, 1);
    ngage_sir_rx(&s, &b, 1);
    assert(!irq);                               /* 0x28 disabled */
    ngage_sir_tx_done(&s);
    assert(irq);
    /* TX done in the ISR: disable 0x10, enable 0x28 (0x503e5f4c's empty path) */
    ngage_sir_write(&s, SIR_CTSI_STATUS, 0x10);
    ngage_sir_write(&s, SIR_CTSI_DISABLE, 0x10);
    ngage_sir_write(&s, SIR_CTSI_ENABLE, 0x28);
    assert(irq);
    assert(isr_rx(&s, out) == 1 && out[0] == 0x55);
}

/* the 4-bit pointers: 16 bytes would read as empty, so the FIFO takes 15 and asks the line to wait */
static void test_rx_full_flow_control(void)
{
    NgageSir s;
    uint8_t buf[20], out[32];
    setup(&s);
    open_port(&s);
    for (unsigned i = 0; i < sizeof(buf); i++) {
        buf[i] = i;
    }
    ngage_sir_rx(&s, buf, ngage_sir_rx_room(&s));
    assert(ngage_sir_rx_room(&s) == 0);
    uint16_t p = ngage_sir_read(&s, SIR_RXPTR);
    assert(((p & 0xf0) >> 4) != (p & 0xf));     /* full must not look empty */
    assert(isr_rx(&s, out) == SIR_FIFO && out[14] == 14);
    assert(ngage_sir_rx_room(&s) == SIR_FIFO);
}

/* a closed port drops what arrives (no backlog of a peer's discovery frames) */
static void test_closed_port_drops(void)
{
    NgageSir s;
    const uint8_t b[3] = { 1, 2, 3 };
    setup(&s);
    assert(ngage_sir_rx_room(&s) > 0);
    ngage_sir_rx(&s, b, 3);
    assert(s.rx_count == 0 && s.rx_dropped == 3);
    open_port(&s);
    ngage_sir_rx(&s, b, 3);
    assert(s.rx_count == 3);
    /* stop (0x503e5b58): disable 0x38, SIR_STOP */
    ngage_sir_write(&s, SIR_CTSI_DISABLE, 0x38);
    ngage_sir_write(&s, SIR_STOP, 2);
    assert(s.rx_count == 0 && !irq);
    ngage_sir_rx(&s, b, 3);
    assert(s.rx_count == 0);
}

/*
 * bluecore_stop_closed_irda: on the 3230/6260 BC02_UART1.pdd (the BlueCore UART) writes its own bit 12 to
 * SIR_START / SIR_STOP (0x1000, seen at boot on the 6260). The model took any SIR_STOP write as "IrDA off" and any
 * nonzero SIR_START as "on": Bluetooth stopping its UART dropped IrDA's receiver, and Bluetooth starting it opened
 * IrDA's. The words are set/clear masks; IrDA's receiver is bit 1.
 */
static void test_bluecore_bits_leave_irda_alone(void)
{
    NgageSir s;
    const uint8_t b[3] = { 1, 2, 3 };
    setup(&s);
    ngage_sir_write(&s, SIR_START, 0x1000);         /* BlueCore on: IrDA stays closed */
    ngage_sir_rx(&s, b, 3);
    assert(s.rx_count == 0 && s.rx_dropped == 3);
    open_port(&s);
    ngage_sir_rx(&s, b, 3);
    assert(s.rx_count == 3);
    ngage_sir_write(&s, SIR_STOP, 0x1000);          /* BlueCore off: IrDA keeps its receiver and FIFO */
    assert(s.rx_count == 3);
    ngage_sir_rx(&s, b, 3);
    assert(s.rx_count == 6);
    ngage_sir_write(&s, SIR_STOP, 2);               /* IrDA off */
    assert(s.rx_count == 0);
    ngage_sir_rx(&s, b, 3);
    assert(s.rx_count == 0);
}

int main(void)
{
    test_create_sends_nothing();
    test_baud_codes();
    test_tx_burst_then_irq();
    test_rx_fifo_and_irq();
    test_rx_held_during_tx();
    test_rx_full_flow_control();
    test_closed_port_drops();
    test_bluecore_bits_leave_irda_alone();
    printf("ngage_sir_test: all passed\n");
    return 0;
}
