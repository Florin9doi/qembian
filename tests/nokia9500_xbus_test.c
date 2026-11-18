/*
 * Host unit test of src/qemu-ngage/hw/arm/nokia9500_xbus.c: the Nokia 9500's XBUS link, modem side
 * (sync handshake, DATA/ACK framing, ISI hand-over to ngage_isi.c).
 *   cc -std=gnu11 -Wall -Itests/ngage_shim $(pkg-config --cflags glib-2.0) -o build/nokia9500_xbus_test \
 *      tests/nokia9500_xbus_test.c src/qemu-ngage/hw/arm/nokia9500_xbus.c \
 *      src/qemu-ngage/hw/arm/ngage_isi.c $(pkg-config --libs glib-2.0) && build/nokia9500_xbus_test
 */
#include "qemu/osdep.h"
#include "qemu/log.h"
#include "hw/core/cpu.h"
#include <assert.h>
#include <unistd.h>
#include "../src/qemu-ngage/hw/arm/ngage_isi.h"
#include "../src/qemu-ngage/hw/arm/nokia9500_xbus.h"

GString *shim_log;
static CPUState cpu0;
CPUState *first_cpu = &cpu0;
uint8_t shim_mem[65536];
uint32_t shim_mem_base = 0x80000000;

static Ra2Xbus x;

static void load(const char *text)
{
    char path[] = "/tmp/nokia9500_xbus_testXXXXXX";
    int fd = mkstemp(path);

    assert(fd >= 0);
    assert(write(fd, text, strlen(text)) == (ssize_t)strlen(text));
    close(fd);
    ngage_isi_load(path);
    unlink(path);
}

static void phone(const uint8_t *b, unsigned n)
{
    for (unsigned i = 0; i < n; i++) {
        ra2_xbus_from_phone(&x, b[i]);
    }
}

/* everything the modem has queued for the phone */
static unsigned drain(uint8_t *b, unsigned cap)
{
    unsigned n = 0;
    bool end;

    while (n < cap && ra2_xbus_pop(&x, &b[n], &end)) {
        n++;
    }
    return n;
}

static void test_crc8(void)
{
    static const uint8_t sync[] = { 0x10, 0, 0 }, ack[] = { 0x30, 0x10, 0 }, f0[] = { 0x30, 0xf0, 0 };

    assert(ra2_xbus_crc8(sync, 3) == 0xa2);
    assert(ra2_xbus_crc8(ack, 3) == 0xb6);
    assert(ra2_xbus_crc8(f0, 3) == 0xf5);
}

/* the modem speaks first; the phone's cycling sync bytes are found; the modem ACKs: link up */
static void test_sync_handshake(void)
{
    static const uint8_t cycle[] = { 0x01, 0x10, 0x00, 0x00, 0xa2, 0x01, 0x10, 0x00, 0x00, 0xa2 };
    uint8_t out[64];

    ra2_xbus_reset(&x);
    ra2_xbus_poll(&x);
    assert(drain(out, sizeof(out)) == 4);
    assert(!memcmp(out, "\x10\x00\x00\xa2", 4));
    assert(!x.synced);
    phone(cycle, 5);
    assert(x.synced);
    assert(drain(out, sizeof(out)) == 4);
    assert(!memcmp(out, "\x30\x10\x00\xb6", 4));
    /* the rest of the cycle is ACKed again, nothing else */
    phone(cycle + 5, 5);
    assert(drain(out, sizeof(out)) == 4);
    assert(!memcmp(out, "\x30\x10\x00\xb6", 4));
}

/* The window over the phone's sync cycle must not lock onto a false header (it would misalign). */
static void test_sync_cycle_has_no_false_header(void)
{
    static const uint8_t cycle[] = { 0x01, 0x10, 0x00, 0x00, 0xa2 };

    for (unsigned s = 0; s < 5; s++) {
        uint8_t w[3] = { cycle[s], cycle[(s + 1) % 5], cycle[(s + 2) % 5] };

        if (s != 1) {
            assert(ra2_xbus_crc8(w, 3) != cycle[(s + 3) % 5]);
        }
    }
}

/* a phone DATA frame: ACKed with its own header byte, the ISI message reaches the reply table,
 * the reply comes back as a modem DATA frame; the next one waits for the phone's ACK */
static void test_data_request_reply(void)
{
    /* ISA request: media 1, devices 0/0, resource 0, length 6, objects 0x10/0x37, tid 5, msg 1 */
    static const uint8_t isi[] = { 0x01, 0x00, 0x00, 0x00, 0x06, 0x00, 0x10, 0x37, 0x05, 0x01, 0x00, 0x00 };
    uint8_t f[4 + sizeof(isi)], out[128];
    unsigned n;

    load("reply 00 01 02 00 82\n");
    ra2_xbus_reset(&x);
    x.synced = true;
    f[0] = 0x28;                                /* DATA, phone seq 1 */
    f[1] = 0;
    f[2] = sizeof(isi);
    f[3] = ra2_xbus_crc8(f, 3);
    memcpy(f + 4, isi, sizeof(isi));
    phone(f, sizeof(f));
    assert(x.frames_in == 1);
    n = drain(out, sizeof(out));
    assert(n == 4 && out[0] == 0x30 && out[1] == 0x28 && out[2] == 0 && out[3] == ra2_xbus_crc8(out, 3));

    ra2_xbus_poll(&x);                          /* the reply */
    n = drain(out, sizeof(out));
    assert(n == 4 + 12);
    assert(out[0] == 0x20 && out[1] == 0 && out[2] == 12 && out[3] == ra2_xbus_crc8(out, 3));
    assert(out[4] == 0x01 && out[4 + 6] == 0x37 && out[4 + 8] == 0x05 && out[4 + 9] == 0x02);
    assert(out[4 + 11] == 0x82);

    /* a second request: its reply waits until the phone ACKs the first (30 20 00 crc) */
    f[0] = 0x20;
    f[3] = ra2_xbus_crc8(f, 3);
    phone(f, sizeof(f));
    drain(out, sizeof(out));
    ra2_xbus_poll(&x);
    assert(drain(out, sizeof(out)) == 0);
    {
        uint8_t ack[4] = { 0x30, 0x20, 0x00, 0 };

        ack[3] = ra2_xbus_crc8(ack, 3);
        phone(ack, 4);
    }
    ra2_xbus_poll(&x);
    n = drain(out, sizeof(out));
    assert(n == 16 && out[0] == 0x28);          /* modem seq toggled */
}

static void test_f0_request_acked(void)
{
    uint8_t f[4] = { 0xf0, 0, 0, 0 }, out[8];

    ra2_xbus_reset(&x);
    x.synced = true;
    f[3] = ra2_xbus_crc8(f, 3);
    phone(f, 4);
    assert(drain(out, sizeof(out)) == 4 && !memcmp(out, "\x30\xf0\x00\xf5", 4));
}

/*
 * Frame ends are reported: the board leaves a gap after each frame (regression "xbus_back_to_back":
 * a DATA frame right behind an ACK was lost, the phone's RX reset timer fired).
 */
static void test_frame_ends(void)
{
    uint8_t b, ends[64];
    unsigned n = 0;
    bool end;

    ra2_xbus_reset(&x);
    x.synced = true;
    phone((const uint8_t *)"\xf0\x00\x00\x00", 0);
    ra2_xbus_poll(&x);                          /* no ISI queued: nothing */
    {
        uint8_t f[4] = { 0xf0, 0, 0, 0 };

        f[3] = ra2_xbus_crc8(f, 3);
        phone(f, 4);
        phone(f, 4);                            /* two ACK frames queued back to back */
    }
    while (ra2_xbus_pop(&x, &b, &end)) {
        ends[n++] = end;
    }
    assert(n == 8);
    for (unsigned i = 0; i < 8; i++) {
        assert(ends[i] == (i == 3 || i == 7));
    }
}

/*
 * Regression "xbus_payload_in_fifo": the phone arms the payload receive after parsing the header and
 * fails with line error 8 if the UART already holds bytes; the header must be a burst of its own.
 */
static void test_data_header_is_own_burst(void)
{
    static const uint8_t isi[] = { 0x01, 0x00, 0x00, 0x00, 0x06, 0x00, 0x10, 0x37, 0x05, 0x01, 0x00, 0x00 };
    uint8_t f[4 + sizeof(isi)], b;
    unsigned n = 0, ends = 0, at[4];
    bool end;

    load("reply 00 01 02 00 82\n");
    ra2_xbus_reset(&x);
    x.synced = true;
    f[0] = 0x20;
    f[1] = 0;
    f[2] = sizeof(isi);
    f[3] = ra2_xbus_crc8(f, 3);
    memcpy(f + 4, isi, sizeof(isi));
    phone(f, sizeof(f));
    ra2_xbus_poll(&x);
    while (ra2_xbus_pop(&x, &b, &end)) {
        n++;
        if (end) {
            at[ends++] = n;
        }
    }
    assert(ends == 3);
    assert(at[0] == 4 && at[1] == 8 && at[2] == 8 + 12);    /* ACK | DATA header | ISI */
}

/*
 * Regression "xbus_first_data_seq": the modem's first DATA frame after sync had sequence bit 0; the
 * phone expects 1 (0x28) and dropped it as a duplicate (ACKed, never delivered: IsaApi panic 0).
 */
static void test_first_data_after_sync_has_seq1(void)
{
    static const uint8_t cycle[] = { 0x01, 0x10, 0x00, 0x00, 0xa2 };
    static const uint8_t isi[] = { 0x01, 0x00, 0x00, 0x00, 0x06, 0x00, 0x10, 0x37, 0x05, 0x01, 0x00, 0x00 };
    uint8_t f[4 + sizeof(isi)], out[64];

    load("reply 00 01 02 00 82\n");
    ra2_xbus_reset(&x);
    phone(cycle, 5);
    assert(x.synced);
    drain(out, sizeof(out));
    f[0] = 0x20;
    f[1] = 0;
    f[2] = sizeof(isi);
    f[3] = ra2_xbus_crc8(f, 3);
    memcpy(f + 4, isi, sizeof(isi));
    phone(f, sizeof(f));
    drain(out, sizeof(out));
    ra2_xbus_poll(&x);
    assert(drain(out, sizeof(out)) == 16 && out[0] == 0x28);
}

/*
 * Regression "xbus_resend_delivered_twice": the phone resends a DATA frame when our ACK is late;
 * the modem must ACK it again but not hand the request on again (duplicate replies).
 */
static void test_resent_phone_frame_not_delivered_twice(void)
{
    static const uint8_t isi[] = { 0x01, 0x00, 0x00, 0x00, 0x06, 0x00, 0x10, 0x37, 0x05, 0x01, 0x00, 0x00 };
    uint8_t f[4 + sizeof(isi)], out[64];

    load("reply 00 01 02 00 82\n");
    ra2_xbus_reset(&x);
    x.synced = true;
    f[0] = 0x20;
    f[1] = 0;
    f[2] = sizeof(isi);
    f[3] = ra2_xbus_crc8(f, 3);
    memcpy(f + 4, isi, sizeof(isi));
    phone(f, sizeof(f));
    phone(f, sizeof(f));                        /* the same frame again */
    assert(x.frames_in == 1 && x.dups == 1);
    assert(drain(out, sizeof(out)) == 8);       /* two ACKs */
    ra2_xbus_poll(&x);
    assert(drain(out, sizeof(out)) == 16);      /* one reply */
    ra2_xbus_poll(&x);
    assert(drain(out, sizeof(out)) == 0);       /* no second reply queued */
    f[0] = 0x28;                                /* the next frame (bit 1) is new */
    f[3] = ra2_xbus_crc8(f, 3);
    phone(f, sizeof(f));
    assert(x.frames_in == 2);
}

/* Regression "xbus_data_never_acked": a modem DATA frame the phone did not ACK is sent again. */
static void test_unacked_modem_frame_resent(void)
{
    static const uint8_t isi[] = { 0x01, 0x00, 0x00, 0x00, 0x06, 0x00, 0x10, 0x37, 0x05, 0x01, 0x00, 0x00 };
    uint8_t f[4 + sizeof(isi)], a[16], b[16];

    load("reply 00 01 02 00 82\n");
    ra2_xbus_reset(&x);
    x.synced = true;
    f[0] = 0x20;
    f[1] = 0;
    f[2] = sizeof(isi);
    f[3] = ra2_xbus_crc8(f, 3);
    memcpy(f + 4, isi, sizeof(isi));
    phone(f, sizeof(f));
    drain(a, 4);
    ra2_xbus_poll(&x);
    assert(drain(a, sizeof(a)) == 16);
    ra2_xbus_ack_timeout(&x);
    assert(drain(b, sizeof(b)) == 16 && !memcmp(a, b, 16));
    for (int i = 0; i < 5; i++) {
        ra2_xbus_ack_timeout(&x);
        drain(b, sizeof(b));
    }
    assert(x.wait_ack == -1);                   /* given up after 5 resends */
}

/*
 * Regression xbus_answer_before_rx_armed: in DMA mode LSR showed data-ready as soon as the modem's
 * answer was due; XbusDma took it as line error 8 when arming its next receive, never re-armed, and
 * the phone resent its DATA until XBUSProtocol faulted (0x1000D).
 */
static void test_lsr_no_data_ready_in_dma_mode(void)
{
    assert(ra2_xbus_lsr(true, true) == 0x60);
    assert(ra2_xbus_lsr(true, false) == 0x60);
    assert(ra2_xbus_lsr(false, true) == 0x61);
    assert(ra2_xbus_lsr(false, false) == 0x60);
}

/*
 * Regression xbus_ack_before_txdone: the modem's ACK went out a fixed 500 us after CTS rose; a slowed
 * guest had not run TxDone yet (it reads MSR first), XbusProtocol ignored the ACK and the link died
 * (fault 0x1000D). Now the answer waits for the MSR read, with the PDD's 70 ms fallback.
 */
static void test_answer_waits_for_txdone(void)
{
    const int64_t ms = 1000000;

    assert(!ra2_xbus_may_answer(true, true, 10 * ms, 0, 1 * ms));      /* MSR not read yet */
    assert(ra2_xbus_may_answer(true, false, 10 * ms, 0, 1 * ms));      /* read, answer time reached */
    assert(!ra2_xbus_may_answer(true, false, 1 * ms, 0, 2 * ms));      /* read, too early */
    assert(ra2_xbus_may_answer(true, true, 71 * ms, 0, 1 * ms));       /* fallback after 70 ms */
    assert(!ra2_xbus_may_answer(false, false, 10 * ms, 0, 1 * ms));    /* CTS low */
}

/*
 * Regression xbus_data_crossing_phone_frame: the modem started a DATA frame while the phone's DATA
 * frame was arriving / its ACK was still queued behind that DATA; the phone (waiting for the ACK)
 * never armed the payload receive and XBUSProtocol faulted (0x1000D). ACKs now go first at a frame
 * boundary and no new frame starts while a phone frame is arriving; an ACK never splits a DATA
 * header from its payload.
 */
static void test_ack_before_queued_data(void)
{
    static const uint8_t isi[] = { 0x01, 0x00, 0x00, 0x00, 0x06, 0x00, 0x10, 0x37, 0x05, 0x01, 0x00, 0x00 };
    uint8_t f[4 + sizeof(isi)], out[128], b;
    unsigned n;
    bool end;

    load("reply 00 01 02 00 82\n");
    ra2_xbus_reset(&x);
    x.synced = true;
    x.seq = 1;
    f[0] = 0x28;
    f[1] = 0;
    f[2] = sizeof(isi);
    f[3] = ra2_xbus_crc8(f, 3);
    memcpy(f + 4, isi, sizeof(isi));
    phone(f, sizeof(f));                        /* request: the reply is now queued as ISI */
    n = drain(out, sizeof(out));                /* its ACK */
    assert(n == 4 && out[0] == 0x30);
    ra2_xbus_poll(&x);                          /* the reply DATA is queued ... */
    assert(ra2_xbus_ready(&x));
    /* ... but the phone starts its next DATA frame first: nothing new may start mid-frame */
    f[0] = 0x20;
    f[3] = ra2_xbus_crc8(f, 3);
    phone(f, 8);
    assert(!ra2_xbus_ready(&x));
    phone(f + 8, sizeof(f) - 8);
    /* the ACK of the phone's frame goes before the queued DATA */
    n = drain(out, sizeof(out));
    assert(n == 4 + 16);
    assert(out[0] == 0x30 && out[1] == 0x20);
    assert(out[4] == 0x28 && out[6] == 12);

    /* an ACK arriving between a DATA header and its payload waits for the payload */
    ra2_xbus_reset(&x);
    x.synced = true;
    x.seq = 1;
    f[0] = 0x28;
    f[3] = ra2_xbus_crc8(f, 3);
    phone(f, sizeof(f));
    drain(out, sizeof(out));                    /* ACK */
    ra2_xbus_poll(&x);
    for (int i = 0; i < 4; i++) {               /* the reply's header */
        assert(ra2_xbus_pop(&x, &b, &end));
    }
    assert(end);
    f[0] = 0x20;
    f[3] = ra2_xbus_crc8(f, 3);
    phone(f, sizeof(f));                        /* an ACK is now queued */
    assert(ra2_xbus_pop(&x, &b, &end) && b == 0x01);   /* the payload's first byte, not the ACK */
}

int main(void)
{
    shim_log = g_string_new(NULL);
    test_crc8();
    test_sync_handshake();
    test_sync_cycle_has_no_false_header();
    test_data_request_reply();
    test_f0_request_acked();
    test_frame_ends();
    test_data_header_is_own_burst();
    test_first_data_after_sync_has_seq1();
    test_resent_phone_frame_not_delivered_twice();
    test_unacked_modem_frame_resent();
    test_lsr_no_data_ready_in_dma_mode();
    test_answer_waits_for_txdone();
    test_ack_before_queued_data();
    printf("nokia9500_xbus_test: ok\n");
    return 0;
}
