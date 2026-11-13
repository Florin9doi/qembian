/*
 * Host unit test of the P1i's fake CCPU (src/qemu-syborg/hw/arm/syborg_ccpu.c, part of the modem core in
 * QEMU and on the hosted board): the CAIF link layer and the BridgeServer protocol, fed the packets
 * caifmux.csy and BServer.dll wrote on R6E30 boots (serial log "SYBCAIF:" lines).
 * docs/devices/se-p1i/ccpu-link.md.
 *   cc -std=gnu11 -Wall -DSYBCCPU_TEST -Isrc/qemu-syborg/hw/arm -o build/caif_peer_test \
 *      tests/caif_peer_test.c src/qemu-syborg/hw/arm/syborg_ccpu.c && build/caif_peer_test
 */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "syborg_ccpu.h"

/* what the CCPU sent: packets (unit, bytes) and log lines */
static struct { uint32_t unit, len; uint8_t data[CCPU_MAX_PKT]; } sent[8];
static int nsent;
static char logs[8][200];
static int nlogs;

static void t_send(void *ctx, uint32_t unit, const uint8_t *pkt, uint32_t len)
{
    (void)ctx;
    assert(nsent < 8 && len <= CCPU_MAX_PKT);
    sent[nsent].unit = unit;
    sent[nsent].len = len;
    memcpy(sent[nsent].data, pkt, len);
    nsent++;
}

static void t_log(void *ctx, uint32_t unit, const char *text)
{
    (void)ctx;
    (void)unit;
    if (nlogs < 8) {
        snprintf(logs[nlogs++], sizeof(logs[0]), "%s", text);
    }
}

static const SybCcpuOut out = { t_send, t_log, NULL };

static int hex(const char *h, uint8_t *o)
{
    int n = 0;
    unsigned v;
    while (h[0] && h[1] && sscanf(h, "%2x", &v) == 1) {
        o[n++] = (uint8_t)v;
        h += 2;
    }
    return n;
}

static char *tohex(const uint8_t *d, int n)
{
    static char buf[2 * CCPU_MAX_PKT + 1];
    for (int i = 0; i < n; i++) {
        sprintf(buf + 2 * i, "%02x", d[i]);
    }
    buf[2 * n] = 0;
    return buf;
}

/* feed one packet (hex) on `unit`; expect exactly the packets in want[] (hex, NULL-terminated) */
static void feed(SybCcpu *c, uint32_t unit, const char *in, const char *const *want)
{
    uint8_t p[CCPU_MAX_PKT], w[CCPU_MAX_PKT];
    int n = hex(in, p), k = 0;
    nsent = nlogs = 0;
    sybccpu_packet(c, &out, unit, p, n);
    for (; want[k]; k++) {
        int wn = hex(want[k], w);
        if (k >= nsent || sent[k].len != (uint32_t)wn || memcmp(sent[k].data, w, wn) || sent[k].unit != unit) {
            fprintf(stderr, "FAIL: %s\n  answer %d: got %s\n  want %s\n", in, k,
                    k < nsent ? tohex(sent[k].data, sent[k].len) : "(none)", want[k]);
            assert(0);
        }
    }
    if (nsent != k) {
        fprintf(stderr, "FAIL: %s: %d answers, want %d (extra %s)\n", in, nsent, k, tohex(sent[k].data, sent[k].len));
        assert(0);
    }
}

#define NONE ((const char *const[]){ NULL })
#define ONE(x) ((const char *const[]){ x, NULL })

static void test_link_layer(void)
{
    SybCcpu c;
    sybccpu_reset(&c);
    feed(&c, 2, "000301", ONE("002301"));                      /* ENUM */
    feed(&c, 2, "0000008100", ONE("002000810001"));            /* LINK_SETUP -> channel 1 */
    feed(&c, 2, "0000108100", ONE("002010810002"));            /* LINK_SETUP -> channel 2 */
    feed(&c, 1, "0000008100", ONE("002000810001"));            /* channel ids count per unit */
    feed(&c, 2, "0009aabb", ONE("0029aabb"));                  /* MODEM_SET */
    feed(&c, 2, "000101", ONE("002101"));                      /* LINK_DESTROY */
    /* responses, errors, other commands: never answered (caifmux panics 104..110 on a CCPU request) */
    feed(&c, 2, "002301", NONE);
    feed(&c, 2, "001301", NONE);
    feed(&c, 2, "000201", NONE);
    feed(&c, 2, "00", NONE);
}

static void test_secaif(void)
{
    SybCcpu c;
    sybccpu_reset(&c);
    feed(&c, 1, "0300" "41542a5345434149463d310d", ONE("0300" "0d0a4f4b0d0a"));   /* AT*SECAIF=1\r */
    feed(&c, 1, "0380" "41542a5345434149463d310d", NONE);                         /* flow command */
}

/* Regression (2026-11-13): word 1 of the type-14 reply was a "thread id" 1. BServer takes it as the
   CCPU's error, flags the bridge failed, every CreateSubSession completes -29 and APS panics APS-9
   0x8000FFE3. Word 1 must be 0. */
static void test_thread_id_reply_status_zero(void)
{
    SybCcpu c;
    sybccpu_reset(&c);
    feed(&c, 2, "01000d000000c814706dc814706d", ONE("01000e00000000000000c814706d000000000000000000000000"));
}

static void test_register_interface(void)
{
    SybCcpu c;
    sybccpu_reset(&c);
    /* app's 4 (count 1, IID, index 1) -> 5 { 5, 0, request id, our handle 0x100 } */
    feed(&c, 2, "010004000000c814706dd81c706d01000000113e2ca0047a4f11b5c147bb684ba5ab01000000",
         ONE("01000500000000000000d81c706d000100000000000000000000"));
    feed(&c, 2, "010004000000401d706d1827706d01000000e37e7127a29946248b75be3c8b25dd6c02000000",
         ONE("010005000000000000001827706d010100000000000000000000"));
}

/* Regression (2026-11-13): CreateInstance was answered with a made-up value; BServer looks it up in its
   proxy table, misses, and completes the client 0x80030000. The CCPU must register the object first
   (its own type 4 with the IID and the object id), then answer with the proxy id BServer returns.
   Word 1 of that type 4 is the app's proxy (an object id there made BServer answer status 12). */
static void test_create_instance_registers_proxy_first(void)
{
    SybCcpu c;
    sybccpu_reset(&c);
    /* app: 0 { 0, proxy 6d7014c8, req 6d7014c8, CLSID, IID } -> our 4 { 4, proxy, req 0x40000001, 1, IID, obj 0x1000 } */
    feed(&c, 2, "010000000000c814706dc814706d0f32bd65d4214e6abc4ab5000d0e52ba59830bacabd14ed298fd0412ea9dae43",
         ONE("0100" "04000000" "c814706d" "01000040" "01000000" "59830bacabd14ed298fd0412ea9dae43" "00100000"));
    assert(nlogs == 1 && strstr(logs[0], "{65bd320f-21d4-6a4e-bc4a-b5000d0e52ba}"));
    /* a second CreateInstance on another link before BServer answered the first */
    feed(&c, 2, "020000000000401d706d401d706d00a2b62058e24ace8542218c995eec60b509b5f7e2f944a5973b2edb78f5bbb3",
         ONE("0200" "04000000" "401d706d" "02000040" "01000000" "b509b5f7e2f944a5973b2edb78f5bbb3" "01100000"));
    /* BServer answers the second first: { 5, 0, req 0x40000002, proxy 6d701d40 } -> 1 to the second app request */
    feed(&c, 2, "02000500000000000000020000404d1d706d000000000000000000000000",
         ONE("02000100000000000000401d706d4d1d706d0000000000000000"));
    /* then the first, on link 1 */
    feed(&c, 2, "01000500000000000000010000408816706d000000000000000000000000",
         ONE("01000100000000000000c814706d8816706d0000000000000000"));
    /* a failed proxy (status 12): CreateInstance answered with that status and no object */
    feed(&c, 2, "010000000000c814706d11111111" "0f32bd65d4214e6abc4ab5000d0e52ba59830bacabd14ed298fd0412ea9dae43",
         ONE("0100" "04000000" "c814706d" "03000040" "01000000" "59830bacabd14ed298fd0412ea9dae43" "02100000"));
    feed(&c, 2, "010005000000" "0c000000" "03000040" "00000000",
         ONE("0100" "01000000" "0c000000" "11111111" "00000000" "0000000000000000"));
    /* an answer 5 nobody waits for: logged, no packet */
    feed(&c, 2, "010005000000000000000300004000000000", NONE);
    assert(nlogs == 1 && strstr(logs[0], "unknown request"));
}

/* a call (type 8) on a created object: answered 9 { 9, 0, req, 0 }, logged with the object's class */
static void test_call_answered(void)
{
    SybCcpu c;
    sybccpu_reset(&c);
    feed(&c, 2, "010000000000c814706dc814706d0f32bd65d4214e6abc4ab5000d0e52ba59830bacabd14ed298fd0412ea9dae43",
         ONE("0100" "04000000" "c814706d" "01000040" "01000000" "59830bacabd14ed298fd0412ea9dae43" "00100000"));
    feed(&c, 2, "020008000000c814706d201a706d001000000400000005000000",
         ONE("02000900000000000000201a706d000000000000000000000000"));
    assert(nlogs == 1 && strstr(logs[0], "object 4096 {65bd320f-21d4-6a4e-bc4a-b5000d0e52ba} args 4: 05000000"));
    /* an unknown object: still answered, class "?" */
    feed(&c, 2, "020008000000c814706d211a706d991000000000000000000000",
         ONE("02000900000000000000211a706d000000000000000000000000"));
    assert(nlogs == 1 && strstr(logs[0], "object 4249 ?"));
}

/* types nobody answers yet are reported, not answered */
static void test_unanswered(void)
{
    SybCcpu c;
    sybccpu_reset(&c);
    feed(&c, 2, "010002000000c814706dc814706d00000000", NONE);
    assert(nlogs == 1 && strstr(logs[0], "type 2 (not answered)"));
    feed(&c, 2, "0100010203", NONE);                         /* shorter than a BridgeServer header */
    assert(nlogs == 0);
}

int main(void)
{
    test_link_layer();
    test_secaif();
    test_thread_id_reply_status_zero();
    test_register_interface();
    test_create_instance_registers_proxy_first();
    test_call_answered();
    test_unanswered();
    printf("caif_peer_test: ok\n");
    return 0;
}
