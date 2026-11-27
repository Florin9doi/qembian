/*
 * Host unit test of the Nokia 9210's IrDA UART core (src/qemu-ngage/hw/arm/nokia9210_ir.h), driven the way
 * Euart2.pdd ("Comm.Linda2", docs/research/rae3-irda.md) drives it.
 *   cc -std=gnu11 -Wall -o build/nokia9210_ir_test tests/nokia9210_ir_test.c && build/nokia9210_ir_test
 */
#include <assert.h>
#include <stdio.h>
#include "../src/qemu-ngage/hw/arm/nokia9210_ir.h"

static uint8_t line[256];
static unsigned nline, baud_set;
static bool rx_irq;

static void tx_byte(void *o, uint8_t b) { (void)o; line[nline++] = b; }
static void set_baud(void *o, unsigned b) { (void)o; baud_set = b; }
static void rx_level(void *o, bool l) { (void)o; rx_irq = l; }

static void setup(Rae3Ir *s)
{
    memset(s, 0, sizeof(*s));
    rae3_ir_init(s);
    s->tx_byte = tx_byte;
    s->set_baud = set_baud;
    s->rx_level = rx_level;
    nline = baud_set = 0;
    rx_irq = false;
}

/* Configure (0x50097548): drain, divisor from the PDD's table, line control, then the port on */
static void configure(Rae3Ir *s, uint32_t divisor)
{
    while (rae3_ir_read(s, RAE3_IR_TXBUSY)) {
    }
    rae3_ir_write(s, RAE3_IR_LINE, 0x380 | 1);
    rae3_ir_write(s, RAE3_IR_DIVISOR, divisor);
    rae3_ir_power(s, RAE3_UARTON_IR, 0);
}

/* the PDD's baud table (0x50097AC8, indexed by TBps): 13 MHz / divisor is the standard rate */
static void test_divisors(void)
{
    assert(rae3_ir_baud(0x3f7a0) == 50);
    assert(rae3_ir_baud(0x54a) == 9600);
    assert(rae3_ir_baud(0x2a5) == 19200);
    assert(rae3_ir_baud(0x153) == 38400);
    assert(rae3_ir_baud(0xe2) == 57600);
    assert(rae3_ir_baud(0x71) == 115200);
    assert(rae3_ir_baud(0) == 0);
    assert(rae3_ir_baud(1000000) == 0);         /* 13 baud: no standard rate */
}

static void test_baud_change_reported_once(void)
{
    Rae3Ir s;
    setup(&s);
    configure(&s, 0x54a);
    assert(baud_set == 0);                      /* 9600 is the reset rate: nothing to tell the link */
    configure(&s, 0x71);
    assert(baud_set == 115200 && s.baud == 115200);
    baud_set = 0;
    configure(&s, 0x71);
    assert(baud_set == 0);
}

/* the TX ISR (0x50097784): write a byte, poll +0x14 to 0; after the last one poll +0x18 to 0 */
static void test_tx_is_instant(void)
{
    Rae3Ir s;
    setup(&s);
    configure(&s, 0x54a);
    const uint8_t frame[] = { 0xc0, 0xff, 0x3f, 0x01, 0xc1 };
    for (unsigned i = 0; i < sizeof(frame); i++) {
        rae3_ir_write(&s, RAE3_IR_TXDATA, 0xffffff00u | frame[i]);   /* only bits 7-0 go out */
        assert(rae3_ir_read(&s, RAE3_IR_TXTAKE) == 0);
    }
    assert(rae3_ir_read(&s, RAE3_IR_TXBUSY) == 0);
    assert(nline == sizeof(frame) && !memcmp(line, frame, sizeof(frame)));
    assert(s.tx_total == sizeof(frame));
}

/* the RX ISR (0x500976AC): while +0x20, read +0x1C; the interrupt is the FIFO's level */
static void test_rx_fifo_and_level(void)
{
    Rae3Ir s;
    setup(&s);
    configure(&s, 0x54a);
    assert(!rx_irq && rae3_ir_read(&s, RAE3_IR_RXREADY) == 0);
    const uint8_t in[] = { 0xc0, 0x01, 0x02, 0xc1 };
    rae3_ir_rx(&s, in, sizeof(in));
    assert(rx_irq);
    uint8_t got[8];
    unsigned n = 0;
    while (rae3_ir_read(&s, RAE3_IR_RXREADY)) {
        got[n++] = rae3_ir_read(&s, RAE3_IR_RXDATA) & 0xff;
    }
    assert(n == sizeof(in) && !memcmp(got, in, n));
    assert(!rx_irq);
    assert(rae3_ir_read(&s, RAE3_IR_RXDATA) == 0);  /* empty FIFO: no underflow */
    assert(rae3_ir_read(&s, RAE3_IR_ERROR) == 0);
}

static void test_rx_room_and_overflow(void)
{
    Rae3Ir s;
    uint8_t buf[RAE3_IR_FIFO + 5];
    setup(&s);
    configure(&s, 0x54a);
    memset(buf, 0x55, sizeof(buf));
    assert(rae3_ir_rx_room(&s) == RAE3_IR_FIFO);
    rae3_ir_rx(&s, buf, 10);
    assert(rae3_ir_rx_room(&s) == RAE3_IR_FIFO - 10);
    rae3_ir_rx(&s, buf, sizeof(buf));           /* more than the room: the rest is dropped, counted */
    assert(s.rx_count == RAE3_IR_FIFO && s.rx_dropped == 15);
    assert(rae3_ir_rx_room(&s) == 0);
}

/* Regression: bytes that arrive while the port is off (bit 7 of 0x0500800C / 0x05008010) are dropped, so a
 * peer's discovery frames do not pile up and the line is never stopped for them. */
static void test_port_off_drops(void)
{
    Rae3Ir s;
    const uint8_t in[] = { 1, 2, 3 };
    setup(&s);
    assert(!rae3_ir_open(&s));
    assert(rae3_ir_rx_room(&s) == RAE3_IR_FIFO);   /* the line is not stopped while off */
    rae3_ir_rx(&s, in, sizeof(in));
    assert(s.rx_count == 0 && s.rx_dropped == 3 && !rx_irq);
    configure(&s, 0x54a);
    rae3_ir_rx(&s, in, sizeof(in));
    assert(s.rx_count == 3 && rx_irq);
    rae3_ir_power(&s, 0, RAE3_UARTON_IR);        /* Stop (0x500973CC): off empties the FIFO */
    assert(s.rx_count == 0 && !rx_irq);
}

/* the other UARTs' bits of the pair (Euart1) neither open nor close the IR port */
static void test_other_uart_bits(void)
{
    Rae3Ir s;
    setup(&s);
    rae3_ir_power(&s, 0x40, 0);
    assert(!rae3_ir_open(&s));
    rae3_ir_power(&s, RAE3_UARTON_IR, 0);
    rae3_ir_power(&s, 0, 0x40);
    assert(rae3_ir_open(&s));
}

int main(void)
{
    test_divisors();
    test_baud_change_reported_once();
    test_tx_is_instant();
    test_rx_fifo_and_level();
    test_rx_room_and_overflow();
    test_port_off_drops();
    test_other_uart_bits();
    printf("nokia9210_ir_test: ok\n");
    return 0;
}
