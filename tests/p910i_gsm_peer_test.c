/*
 * Host unit test of src/qemu-ngage/hw/arm/p910i_gsm_peer.c: the P910i's OSE link, GSM side (sync,
 * frame parsing, reply table, unasked frames) and of src/qemu-ngage/p910i-ose-replies.txt.
 *   cc -std=gnu11 -Wall -o build/p910i_gsm_peer_test tests/p910i_gsm_peer_test.c \
 *      src/qemu-ngage/hw/arm/p910i_gsm_peer.c && build/p910i_gsm_peer_test
 */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../src/qemu-ngage/hw/arm/p910i_gsm_peer.h"

static P910Peer p;
static uint8_t out[512];
static char done[256];

static size_t feed(const uint8_t *b, size_t n)
{
    size_t got = 0;

    for (size_t i = 0; i < n; i++) {
        got += p910_peer_byte(&p, b[i], out + got, sizeof(out) - got, done, sizeof(done));
    }
    return got;
}

static void sync_link(void)
{
    static const uint8_t l1[] = { 0x4c, 0x31 };

    p910_peer_reset(&p);
    assert(feed(l1, 2) == 2 && out[0] == 0x4c && out[1] == 0x41);
    assert(p.synced);
}

static void test_sync_needs_l1(void)
{
    static const uint8_t junk[] = { 0x31, 0x4c, 0x00, 0x31 };   /* "1L" then "\0" "1": no sync */

    p910_peer_reset(&p);
    assert(feed(junk, sizeof(junk)) == 0 && !p.synced);
}

/* startuphandler.lsu's request 0x4398 from pid E3 to 62, seen on the link [V] */
static void test_startuphandler_request_answered(void)
{
    static const uint8_t req[] = { 0x62, 0x02, 0x00, 0xe3, 0x98, 0x43 };
    static const uint8_t want[] = { 0xe3, 0x04, 0x00, 0x62, 0x4e, 0x4f, 0x00, 0x00, 0x00 };

    assert(p910_peer_load(&p, "reply 4398 4F4E 00 00\n", NULL) == 1);
    sync_link();
    assert(feed(req, 5) == 0);                  /* not before the frame is complete */
    assert(feed(req + 5, 1) == sizeof(want));
    assert(!memcmp(out, want, sizeof(want)));
    assert(strstr(done, "4398") && strstr(done, "-> 4F4E"));
}

static void test_unanswered_logged(void)
{
    static const uint8_t req[] = { 0x62, 0x03, 0x00, 0xe4, 0x11, 0x22, 0x7f };

    assert(p910_peer_load(&p, "reply 4398 4F4E 00 00\n", NULL) == 1);
    sync_link();
    assert(feed(req, sizeof(req)) == 0);
    assert(strstr(done, "signal 2211 from pid E4") && strstr(done, " 7f") && strstr(done, "UNANSWERED"));
    /* the next frame starts cleanly */
    static const uint8_t req2[] = { 0x62, 0x02, 0x00, 0xe3, 0x98, 0x43 };
    assert(feed(req2, sizeof(req2)) == 9 && out[0] == 0xe3);
}

/* the StartSignal frame the phone accepted [V by run]: 00 03 00 00 84 C3 00 00 */
static void test_unasked_startsignal(void)
{
    static const uint8_t want[] = { 0x00, 0x03, 0x00, 0x00, 0x84, 0xc3, 0x00, 0x00 };
    unsigned ms, period;

    assert(p910_peer_load(&p, "# c\nunasked 1000 C384 00\nreply 1 2\nunasked 3000 C384 00 # again\n",
                          NULL) == 3);
    assert(p910_peer_unasked(&p, 0, &ms, &period, out, sizeof(out)) == sizeof(want) && ms == 1000 && !period);
    assert(!memcmp(out, want, sizeof(want)));
    assert(p910_peer_unasked(&p, 1, &ms, &period, out, sizeof(out)) == sizeof(want) && ms == 3000);
    assert(p910_peer_unasked(&p, 2, &ms, &period, out, sizeof(out)) == 0);
}

static void test_bad_lines(void)
{
    int bad = 0;

    assert(p910_peer_load(&p, "reply 4398\n", &bad) < 0 && bad == 1);
    assert(p910_peer_load(&p, "\nreply 4398 4F4E 100\n", &bad) < 0 && bad == 2);
    assert(p910_peer_load(&p, "unasked x C384\n", &bad) < 0 && bad == 1);
    assert(p910_peer_load(&p, "send 1 2\n", &bad) < 0 && bad == 1);
}

static void test_bad_length_resyncs(void)
{
    static const uint8_t junk[] = { 0x62, 0x01, 0x00, 0xe3, 0x98, 0x43 };   /* len 1 < 2 */

    sync_link();
    assert(feed(junk, sizeof(junk)) == 0 && strstr(done, "bad length"));
    assert(p.got == 0);
}

/* the shipped table loads */
static void test_shipped_table(void)
{
    FILE *f = fopen("src/qemu-ngage/p910i-ose-replies.txt", "r");
    static char text[65536];
    size_t n;
    int bad = 0;

    assert(f);
    n = fread(text, 1, sizeof(text) - 1, f);
    fclose(f);
    text[n] = 0;
    assert(p910_peer_load(&p, text, &bad) >= 3);
}

/*
 * Regression teleapp_emergency_numbers_empty: 0x413F (the TSY's SIM emergency numbers, ETel IPC 0x858)
 * was answered with all zeros = count 0; Gsmadv left KErrNotFound, Tele.app panicked, the kernel restarted.
 * The shipped table must answer status 0, count > 0, entry 0 = "112" (BCD, low nibble first).
 */
static void test_teleapp_emergency_numbers_empty(void)
{
    static const uint8_t req[] = { 0x62, 0x05, 0x00, 0xe3, 0x3f, 0x41, 0x00, 0x00, 0x00 };
    FILE *f = fopen("src/qemu-ngage/p910i-ose-replies.txt", "r");
    static char text[65536];
    size_t n;

    assert(f);
    n = fread(text, 1, sizeof(text) - 1, f);
    fclose(f);
    text[n] = 0;
    assert(p910_peer_load(&p, text, NULL) > 0);
    sync_link();
    n = feed(req, sizeof(req));
    assert(n == 4 + 2 + 1 + 53 + 1);                    /* header, signo, status, count + 13 x 4, trailer */
    assert(out[0] == 0xe3 && out[3] == 0x62 && out[4] == 0xc9 && out[5] == 0x22);
    assert(out[6] == 0 && out[7] > 0);                  /* status OK, count */
    assert(out[8] == 0x11 && (out[9] & 0x0f) == 0x02 && (out[9] >> 4) == 0xf);   /* "112" */
}

/* generic requests: /SUB matches the first data byte and wins over a plain entry */
static void test_sub_command_match(void)
{
    static const uint8_t r18[] = { 0x49, 0x08, 0x00, 0xe3, 0x33, 0x05, 0x18, 0, 0, 0, 0, 0 };
    static const uint8_t r10[] = { 0x49, 0x08, 0x00, 0xe3, 0x33, 0x05, 0x10, 0, 0, 0, 0, 0 };

    assert(p910_peer_load(&p, "reply 0533 1111 00\nreply 0533/18 1681 18 01\n", NULL) == 2);
    sync_link();
    assert(feed(r18, sizeof(r18)) && out[4] == 0x81 && out[5] == 0x16);
    assert(feed(r10, sizeof(r10)) && out[4] == 0x11 && out[5] == 0x11);
    assert(p910_peer_load(&p, "reply 0533/1G 1681\n", NULL) < 0);
}

/*
 * Regression battery_level_zero: the status bar showed 0 % because the GSM side never sent the battery
 * indication 0xC39C {percent, charger}; it is a periodic report now (every MS), so a late subscriber gets it.
 */
static void test_battery_level_zero(void)
{
    static const uint8_t want[] = { 0x00, 0x04, 0x00, 0x00, 0x9c, 0xc3, 0x64, 0x00, 0x00 };
    FILE *f = fopen("src/qemu-ngage/p910i-ose-replies.txt", "r");
    static char text[65536];
    unsigned ms, period, i;
    size_t n;
    bool found = false;

    assert(f);
    n = fread(text, 1, sizeof(text) - 1, f);
    fclose(f);
    text[n] = 0;
    assert(p910_peer_load(&p, text, NULL) > 0);
    for (i = 0; (n = p910_peer_unasked(&p, i, &ms, &period, out, sizeof(out))); i++) {
        if (out[4] == 0x9c && out[5] == 0xc3) {
            assert(n == sizeof(want) && !memcmp(out, want, sizeof(want)));     /* 100 %, not charging */
            assert(period > 0);
            found = true;
        }
    }
    assert(found);
    assert(p910_peer_load(&p, "every 0 C39C 64 00\n", NULL) < 0);
}

int main(void)
{
    test_sync_needs_l1();
    test_startuphandler_request_answered();
    test_unanswered_logged();
    test_unasked_startsignal();
    test_bad_lines();
    test_bad_length_resyncs();
    test_shipped_table();
    test_teleapp_emergency_numbers_empty();
    test_sub_command_match();
    test_battery_level_zero();
    printf("p910i_gsm_peer_test: all passed\n");
    return 0;
}
