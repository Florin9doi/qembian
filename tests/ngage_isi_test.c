/*
 * Host unit test of src/qemu-ngage/hw/arm/ngage_isi.c: the reply table and the frames the board
 * queues, for the N-Gage's NOSDRIVER channels and the Nokia 6600's ISA_IF path (isa_chan).
 *   cc -std=gnu11 -Wall -Itests/ngage_shim $(pkg-config --cflags glib-2.0) -o build/ngage_isi_test \
 *      tests/ngage_isi_test.c src/qemu-ngage/hw/arm/ngage_isi.c $(pkg-config --libs glib-2.0) \
 *      && build/ngage_isi_test
 */
#include "qemu/osdep.h"
#include "qemu/log.h"
#include "hw/core/cpu.h"
#include <assert.h>
#include <unistd.h>
#include "../src/qemu-ngage/hw/arm/ngage_isi.h"

GString *shim_log;
static CPUState cpu0;
CPUState *first_cpu = &cpu0;
uint8_t shim_mem[65536];
uint32_t shim_mem_base = 0x80000000;

static uint8_t got[512];
static uint32_t gotlen;

/* the stub's DFC: fetch one delivery, copy its bytes */
static bool fetch(void)
{
    uint32_t dlc = 0, a1 = 0, req = 0;

    if (!ngage_isi_fetch(&dlc, &a1, &req, &gotlen)) {
        return false;
    }
    for (uint32_t i = 0; i < gotlen; i += 4) {
        uint32_t w = ngage_isi_fetched_word(i);
        memcpy(got + i, &w, 4);
    }
    ngage_isi_done(0);
    return true;
}

static void load(const char *text)
{
    char path[] = "/tmp/ngage_isi_testXXXXXX";
    int fd = mkstemp(path);

    assert(fd >= 0);
    assert(write(fd, text, strlen(text)) == (ssize_t)strlen(text));
    close(fd);
    ngage_isi_load(path);
    unlink(path);
}

/* 6600 CRC handshake (isaapi CheckCrcMsgL): request to object 0x10 from 0x37, reply data[1] = 0x82 */
static void test_isa_crc_reply(void)
{
    static const uint8_t req[] = { 0x01, 0x00, 0x00, 0x00, 0x06, 0x00, 0x10, 0x37, 0x05, 0x01, 0x00, 0x00 };

    ngage_isi_isa_send(req, sizeof(req));
    assert(ngage_isi_ready());
    assert(fetch());
    assert(gotlen == 12);
    assert(got[0] == 0x01);                     /* media kept */
    assert(got[3] == 0x00);                     /* resource */
    assert(got[4] == 6 && got[5] == 0);         /* length: objects + tid + msg + 2 data bytes */
    assert(got[6] == 0x37);                     /* receiver = the request's sender object */
    assert(got[8] == 0x05);                     /* transaction id */
    assert(got[9] == 0x02);
    assert(got[10] == 0x00 && got[11] == 0x82);
    assert(!fetch());
}

/*
 * Regression "isa_reply_sender_object": the first ISA version set the reply's sender object and
 * then the shared N-Gage code cleared byte 7 again, so replies came from object 0.
 */
static void test_isa_reply_sender_object(void)
{
    static const uint8_t req[] = { 0x01, 0x00, 0x00, 0x00, 0x06, 0x00, 0x10, 0x37, 0x00, 0x01, 0x00, 0x00 };

    ngage_isi_isa_send(req, sizeof(req));
    assert(fetch());
    assert(got[7] == 0x10);
}

/* devices are swapped: the reply goes back to the request's sender device */
static void test_isa_devices_swapped(void)
{
    static const uint8_t req[] = { 0x01, 0x12, 0x34, 0x00, 0x06, 0x00, 0x10, 0x37, 0x00, 0x01, 0x00, 0x00 };

    ngage_isi_isa_send(req, sizeof(req));
    assert(fetch());
    assert(got[1] == 0x34 && got[2] == 0x12);
}

/* the ISA pseudo channel stays ready after a delivery (the NOS can always hand frames up) */
static void test_isa_always_ready(void)
{
    static const uint8_t req[] = { 0x01, 0x00, 0x00, 0x00, 0x06, 0x00, 0x10, 0x37, 0x00, 0x01, 0x00, 0x00 };

    ngage_isi_isa_send(req, sizeof(req));
    ngage_isi_isa_send(req, sizeof(req));
    assert(fetch());
    assert(ngage_isi_ready());
    assert(fetch());
    assert(!ngage_isi_ready());
}

/* unanswered requests queue nothing and are logged UNANSWERED */
static void test_isa_unanswered(void)
{
    static const uint8_t req[] = { 0x01, 0x00, 0x00, 0x7f, 0x06, 0x00, 0x10, 0x37, 0x00, 0x42, 0x00, 0x00 };

    g_string_truncate(shim_log, 0);
    ngage_isi_isa_send(req, sizeof(req));
    assert(!ngage_isi_ready());
    assert(strstr(shim_log->str, "UNANSWERED"));
}

/* short frames (under 10 bytes) are logged, never answered */
static void test_isa_short_frame(void)
{
    static const uint8_t req[] = { 0x01, 0x00, 0x00 };

    ngage_isi_isa_send(req, sizeof(req));
    assert(!ngage_isi_ready());
}

/* the N-Gage path keeps its frame: first bytes 0, sender object 0 */
static void test_ngage_frame_unchanged(void)
{
    /* a NOSDRIVER channel: ctor, open (channel +0x38 = object), a receive, a send by DoControl 4 */
    uint32_t obj = 0x80001000, dlc = 0x80002000, desc = 0x80003000, rxdesc = 0x80004000;
    static const uint8_t req[] = { 0x00, 0x00, 0x00, 0x00, 0x06, 0x00, 0x10, 0x4f, 0x07, 0x01, 0x00, 0x00 };
    uint32_t w;

    w = obj; memcpy(shim_mem + (dlc + 0x38 - shim_mem_base), &w, 4);
    w = 0x00000000 | sizeof(req);              /* TBufC: length, data at +4 */
    memcpy(shim_mem + (desc - shim_mem_base), &w, 4);
    memcpy(shim_mem + (desc + 4 - shim_mem_base), req, sizeof(req));
    ngage_isi_ctor(obj, 3);
    ngage_isi_open(dlc, 3);
    ngage_isi_call(obj, 4, 0, rxdesc, 0);       /* DoRequest(0): a receive */
    ngage_isi_call(obj, 5, 4, desc, 0);         /* DoControl(4): the send */
    assert(fetch());
    assert(got[0] == 0 && got[1] == 0 && got[2] == 0);
    assert(got[6] == 0x4f);
    assert(got[7] == 0x00);
    assert(got[9] == 0x02);
}

/*
 * 6600: a subscription (resource 0, msg 0) from object 0x54 brings the indications for unit 84.
 * Regression "isa_ind_format": `ind` lines keep the N-Gage layout UNIT OBJ RES MSG; the first ISA
 * version read them as OBJ RES MSG and sent resource C0 message 01.
 */
static void test_isa_subscription_indications(void)
{
    static const uint8_t sub[] = { 0x01, 0x00, 0x00, 0x00, 0x09, 0x00, 0x10, 0x54, 0x00, 0x00,
                                   0x00, 0x01, 0x15, 0xc0 };
    bool ind = false, reply = false;

    ngage_isi_isa_send(sub, sizeof(sub));
    while (fetch()) {
        if (got[3] == 0x15 && got[9] == 0xc0) {
            ind = true;
            assert(got[6] == 0x54);             /* to the subscriber */
            assert(got[7] == 0x10);             /* from the ISA service object */
            assert(got[10] == 0x01 && got[11] == 0x03);
        } else if (got[3] == 0x00 && got[9] == 0x01) {
            reply = true;
        }
    }
    if (!(ind && reply)) {
        fprintf(stderr, "%s", shim_log->str);
    }
    assert(ind && reply);
}

/*
 * Regression "isi_reply_subtype": the 6600's Nokia.dsy (0B 11 {05 08}) and phonetsy (0B 11 {06 01})
 * share resource and message id; the N-Gage line for subtype 06 answered the DSY too, and its
 * parser (0x504159cc) took the wrong branch. "@XX" picks the reply by request byte 10.
 */
static void test_reply_subtype(void)
{
    static const uint8_t dsy[] = { 0x00, 0x00, 0x00, 0x0b, 0x06, 0x00, 0x00, 0x54, 0x09, 0x11, 0x05, 0x08 };
    static const uint8_t tsy[] = { 0x00, 0x00, 0x00, 0x0b, 0x06, 0x00, 0x00, 0x45, 0x03, 0x11, 0x06, 0x01 };
    static const uint8_t other[] = { 0x00, 0x00, 0x00, 0x0b, 0x06, 0x00, 0x00, 0x45, 0x03, 0x11, 0x07, 0x01 };

    ngage_isi_isa_send(dsy, sizeof(dsy));
    assert(fetch());
    assert(got[8] == 0x09);                     /* transaction id echoed */
    assert(got[10] == 0x05 && got[12] == 0x02);
    ngage_isi_isa_send(tsy, sizeof(tsy));
    assert(fetch());
    assert(got[10] == 0x06 && got[12] == 0x06);
    ngage_isi_isa_send(other, sizeof(other));   /* no subtype line: the generic one */
    assert(fetch());
    assert(got[10] == 0xee);
}

/*
 * Regression "locked_after_restart" (owner, 2026-11-18): on a restart the 6600's Phone.app called
 * RMobilePhone::GetALSLine synchronously (SIM 09 29 {35 00}); unanswered, the view server killed Phone
 * (ViewSrv 11) and the idle screen stayed unpainted. The phone's own table must answer it (and the
 * custom TSY's SIM list count 09 23 {25}) with the layouts in docs/research/6600-tsy-sim-replies.md.
 */
static void test_nhl10_als_line(void)
{
    static const uint8_t als[] = { 0x00, 0x00, 0x00, 0x09, 0x06, 0x00, 0x00, 0x45, 0x06, 0x29, 0x35, 0x00 };
    static const uint8_t cnt[] = { 0x00, 0x00, 0x00, 0x09, 0x05, 0x00, 0x00, 0x45, 0x07, 0x23, 0x25 };

    ngage_isi_load("src/qemu-ngage/isi-replies-nhl10.txt");
    ngage_isi_isa_send(als, sizeof(als));
    assert(fetch());
    assert(got[8] == 0x06 && got[9] == 0x2a);   /* the request's transaction id, response 2A */
    assert(got[10] == 0x35 && got[11] == 0xff); /* service 35, status FF: no ALS, no error */
    ngage_isi_isa_send(cnt, sizeof(cnt));
    assert(fetch());
    assert(got[8] == 0x07 && got[9] == 0x24);
    assert(got[10] == 0x25 && got[11] == 0x01 && got[12] == 0x00);    /* OK, count 0 */
}

/*
 * Regression isi_reply_len_capped_at_64: "len=70" gave 64 data bytes (the reply buffer size), so
 * the Nokia 9500's SIM 09 31 reply stayed short and PhoneTsy panicked USER 22.
 */
static void test_reply_len_above_64(void)
{
    uint8_t req[] = { 0, 0, 0, 0x09, 0x07, 0x00, 0x00, 0x4b, 0x04, 0x31, 0x3b, 0x00, 0x00 };
    uint8_t out[512];
    uint32_t n;

    load("reply 09 31 32 len=70 3B 01\n");
    ngage_isi_isa_send(req, sizeof(req));
    n = ngage_isi_isa_take(out, sizeof(out));
    assert(n == 10 + 70);
}

int main(void)
{
    shim_log = g_string_new(NULL);
    load("reply 00 01 02 00 82   # comment\n"
         "reply 00 00 01 00 00 00\n"
         "ind 84 54 15 C0 01 03\n"
         "reply 0B 11 12 @05 05 01 02 08 08 00 00 00\n"
         "reply 0B 11 12 EE\n"
         "reply 0B 11 12 @06 06 01 06 08 00 00\n");
    test_isa_crc_reply();
    test_isa_reply_sender_object();
    test_isa_devices_swapped();
    test_isa_always_ready();
    test_isa_unanswered();
    test_isa_short_frame();
    test_isa_subscription_indications();
    test_reply_subtype();
    test_ngage_frame_unchanged();
    test_nhl10_als_line();
    test_reply_len_above_64();
    printf("ngage_isi_test: all passed\n");
    return 0;
}
