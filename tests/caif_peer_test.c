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
#include <stdlib.h>
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
    if (!want)                                       /* NULL_OK: answers not checked */
        return;
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
#define NULL_OK NULL
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
    /* ISystemControl {ac0b8359-...} method 5: 8 output bytes (OpaClient's size, docs/research/p1i-opa-methods.md);
       bug 2026-11-13/17: answered with no outputs, BServer refused the caller's next request (-6) and start-up stopped */
    feed(&c, 2, "020008000000c814706d201a706d001000000400000005000000",
         ONE("02000900000000000000201a706d080000000000000000000000"));
    assert(nlogs == 1 && strstr(logs[0], "object 4096 {65bd320f-21d4-6a4e-bc4a-b5000d0e52ba} args 4: 05000000"));
    assert(strstr(logs[0], "8 output bytes"));
    /* an unknown object: still answered, class "?" */
    feed(&c, 2, "020008000000c814706d211a706d991000000000000000000000",
         ONE("02000900000000000000211a706d000000000000000000000000"));
    assert(nlogs == 1 && strstr(logs[0], "object 4249 ?"));
}

/* IParameterStorageManager {68d3d445-...} m3 returns an interface (IParameterStorage): bug 2026-11-17, answered
   with zeros, APS's bind (BServer op 0x1F9) found no proxy for word 1 and failed -6, start-up stalled. Now the
   CCPU registers a new object (type 4 with the call's P), and after BServer's 5 {proxy L} replies 9 with the
   12 output bytes {0, L, 0} (docs/research/p1i-bserver-fn3.md) */
static void test_call_output_size(void)
{
    SybCcpu c;
    sybccpu_reset(&c);
    feed(&c, 2, "0100000000005047706d5047706d6a515bfa4601486a9353798b35b0f12b45d4d368d6a046b088eba795abdcfe5c",
         ONE("0100" "04000000" "5047706d" "01000040" "01000000" "45d4d368d6a046b088eba795abdcfe5c" "00100000"));
    feed(&c, 2, "0100050000000000000001000040084f706d", ONE("010001000000000000005047706d084f706d0000000000000000"));
    /* the call: no 9 yet, our 4 for object 0x1001 with IParameterStorage's IID */
    feed(&c, 2, "0200080000005047706d504b706d001000000400000003000000",
         ONE("0200" "04000000" "5047706d" "02000040" "01000000" "a88160534b1246eb87ed72c8a2f8930b" "01100000"));
    assert(strstr(logs[0], "returns interface {536081a8-124b-eb46-87ed-72c8a2f8930b}"));
    /* BServer's 5 { 0, req', L }: the call's 9 with {status 0, L, 0} */
    feed(&c, 2, "02000500000000000000020000407851706d",
         ONE("02000900000000000000504b706d0c000000" "00000000" "7851706d" "00000000"));
    /* a method with no interface and not in the table: no outputs, as before, logged */
    feed(&c, 2, "0200080000005047706d514b706d0010000004000000ee000000",
         ONE("02000900000000000000514b706d000000000000000000000000"));
    assert(nlogs == 1 && strstr(logs[0], "unknown"));
}

/* bug 2026-11-17: APS's 119-byte call on IAudioDevice came as two packets (102 bytes with byte 1 = 1, then 17);
   the first was taken for flow control and dropped, the second parsed as a message of type 0x0061002e */
static void test_fragments_reassembled(void)
{
    SybCcpu c;
    sybccpu_reset(&c);
    /* an unknown object: the call is answered, so the reassembled message reached bs_call */
    feed(&c, 2, "0201" "08000000" "401d706d" "4025706d" "99100000" "08000000", NONE);
    feed(&c, 2, "0200" "15000000" "01010000", ONE("020009000000000000004025706d000000000000000000000000"));
    assert(nlogs == 1 && strstr(logs[0], "object 4249 ? args 8: 15000000 01010000"));
}

/* QueryInterface (type 2) from the P1i: answered after registering the new interface's object (2026-11-17: it was
   left unanswered and APS's request on that subsession never completed) */
static void test_query_interface(void)
{
    SybCcpu c;
    sybccpu_reset(&c);
    feed(&c, 2, "010002000000c814706d202e706d2056388531e04df0977c39df4962ad1306100000",
         ONE("0100" "04000000" "c814706d" "01000040" "01000000" "2056388531e04df0977c39df4962ad13" "00100000"));
    feed(&c, 2, "01000500000000000000010000408816706d", ONE("01000300000000000000202e706d8816706d0000000000000000"));
}

/* 2026-11-17: a call on an object whose IID was guessed (returned by ICopsManager m4) used method 10 with 14 input
   bytes, unknown for the guessed interface: answered without outputs, semc.tsy panicked USER 21. The size now
   comes from the interfaces with that method and input length when they agree (10/14 -> 12 everywhere) */
static void test_size_by_shape(void)
{
    SybCcpu c;
    sybccpu_reset(&c);
    /* CreateInstance of ICopsManager {7bf1607a-...}, registered as object 0x1000 */
    feed(&c, 2, "010000000000c814706dc814706d" "a8b89261b0064f5bbc57a1c6f12c0602" "7a60f17b166145a2a80f3354be49359c",
         ONE("0100" "04000000" "c814706d" "01000040" "01000000" "7a60f17b166145a2a80f3354be49359c" "00100000"));
    nlogs = 0;
    feed(&c, 2, "020008000000c814706d201a706d00100000" "0e000000" "0a000000" "0f010000" "00000000" "0000",
         ONE("02000900000000000000201a706d0c000000" "000000000000000000000000"));
    assert(nlogs == 2 && strstr(logs[0], "not in its interface's table, 12 output bytes"));
}

/* bug 2026-11-17: IParameterStorage m5 reads were answered without data; M3SAudioPolicyMgr took byte 0 of each
   (TDesC8::AtC(0)) and mmmgmtserver and semc.tsy panicked USER 21. m11 now gives size 4, m5 {0, 4, value} */
static void test_parameter_reads(void)
{
    SybCcpu c;
    sybccpu_reset(&c);
    feed(&c, 2, "010000000000c814706dc814706d" "a8b89261b0064f5bbc57a1c6f12c0602" "a88160534b1246eb87ed72c8a2f8930b",
         ONE("0100" "04000000" "c814706d" "01000040" "01000000" "a88160534b1246eb87ed72c8a2f8930b" "00100000"));
    feed(&c, 2, "020008000000c814706d201a706d00100000" "08000000" "0b000000" "d5061000",
         ONE("02000900000000000000201a706d0c000000" "00000000" "04000000" "00000000"));
    feed(&c, 2, "020008000000c814706d211a706d00100000" "0c000000" "05000000" "d5061000" "00000000",
         ONE("02000900000000000000211a706d0c000000" "00000000" "04000000" "32000000"));
    feed(&c, 2, "020008000000c814706d221a706d00100000" "0c000000" "05000000" "9c071000" "00000000",
         ONE("02000900000000000000221a706d0c000000" "00000000" "04000000" "00000000"));
}

/* bug 2026-11-17: only 64 objects were tracked; the P1i's start-up creates more and the 65th and later lost their
   IID, so their calls (semc.tsy's ISystemControlManager m4) got no outputs: TSY_MErr SM:1 -2 */
static void test_object_65_gets_outputs(void)
{
    SybCcpu *c = calloc(1, sizeof(*c));
    sybccpu_reset(c);
    char pkt[256];
    for (int i = 0; i < 70; i++) {
        feed(c, 2, "010000000000c814706dc814706d" "0f32bd65d4214e6abc4ab5000d0e52ba" "59830bacabd14ed298fd0412ea9dae43", NULL_OK);
        uint32_t r = 0x40000001u + i;            /* BServer's answer 5 to our type 4 */
        snprintf(pkt, sizeof(pkt), "010005000000" "00000000" "%02x%02x%02x%02x" "8816706d",
                 r & 0xff, (r >> 8) & 0xff, (r >> 16) & 0xff, r >> 24);
        feed(c, 2, pkt, NULL_OK);
    }
    /* object 0x1045 (the 70th): ISystemControl m5 -> 8 output bytes */
    snprintf(pkt, sizeof(pkt), "020008000000c814706d201a706d45100000" "04000000" "05000000");
    feed(c, 2, pkt, ONE("02000900000000000000201a706d080000000000000000000000"));
    free(c);
}

/* bug 2026-11-17: IPhonebookManager m4 returned a phonebook object and the client walked its entries forever;
   without a SIM it now fails "not ready" (-18 as 0x8000ffee in output word 0) */
static void test_phonebook_not_ready(void)
{
    SybCcpu c;
    sybccpu_reset(&c);
    feed(&c, 2, "010000000000c814706dc814706d" "6b723d912aab4e6f8e75dac4788ddc1e" "4e29f91a9a8d4b468f78387e11da29e0",
         ONE("0100" "04000000" "c814706d" "01000040" "01000000" "4e29f91a9a8d4b468f78387e11da29e0" "00100000"));
    feed(&c, 2, "020008000000c814706d201a706d00100000" "08000000" "04000000" "00000000",
         ONE("02000900000000000000201a706d0c000000" "eeff0080" "0000000000000000"));
}

/* bug 2026-11-17: ICopsManager m3/m4 were bound as ICopsManager itself; SecurityApp's next ISimLock m5 got
   23 output bytes instead of 12, its reply buffer overflowed and start-up stalled at a full progress bar.
   m3 must return an ISimLock object, m4 an IPhoneLock object, and ISimLock m5 answers 12 bytes */
static void test_cops_manager_returns_lock_objects(void)
{
    SybCcpu c;
    sybccpu_reset(&c);
    /* CreateInstance CLSID {6192b8a8} IID ICopsManager -> object 0x1000 */
    feed(&c, 2, "010000000000c814706dc814706d" "a8b89261b0064f5bbc57a1c6f12c0602" "7a60f17b166145a2a80f3354be49359c", NULL_OK);
    feed(&c, 2, "010005000000" "00000000" "01000040" "8816706d", NULL_OK);
    /* m3 -> a new object registered as ISimLock */
    feed(&c, 2, "020008000000c814706d201a706d00100000" "04000000" "03000000", NULL_OK);
    assert(nsent == 1 && strstr(tohex(sent[0].data, sent[0].len), "a550e2c3d19e432983e80383aea38f60"));
    feed(&c, 2, "010005000000" "00000000" "02000040" "9816706d", NULL_OK);
    /* m4 -> a new object registered as IPhoneLock */
    feed(&c, 2, "020008000000c814706d211a706d00100000" "04000000" "04000000", NULL_OK);
    assert(nsent == 1 && strstr(tohex(sent[0].data, sent[0].len), "c17749a2fc8f41ebb74d642cb0d87107"));
    feed(&c, 2, "010005000000" "00000000" "03000040" "a816706d", NULL_OK);
    /* ISimLock (object 0x1001) m5: 12 output bytes */
    feed(&c, 2, "020008000000c814706d221a706d01100000" "04000000" "05000000",
         ONE("02000900000000000000221a706d0c000000" "000000000000000000000000"));
}

/* bug 2026-11-17: SecurityApp registered an ICBSimLockStatus callback and waited for it forever after ISimLock m5
   (start-up stalled at a full progress bar); the CCPU now calls it: type 8 {8, the app's session word, req, the app's target
   word from its type 4, 12, method 3, 0, 0}, and the app's type-9 answer is accepted */
static void test_sim_lock_status_callback(void)
{
    SybCcpu c;
    sybccpu_reset(&c);
    /* the app registers ICBSimLockStatus {23b8d43b}, target 0x3e: we answer type 5 with handle 0x100 */
    feed(&c, 2, "010004000000a8d2726d0825736d01000000" "3bd4b823376c42b2a0a225e1c58306c3" "3e000000",
         ONE("0100" "05000000" "00000000" "0825736d" "00010000" "0000000000000000"));
    /* ISimLock m5 without a registered callback first would only log; here: CreateInstance ICopsManager, m3 */
    feed(&c, 2, "010000000000c814706dc814706d" "a8b89261b0064f5bbc57a1c6f12c0602" "7a60f17b166145a2a80f3354be49359c", NULL_OK);
    feed(&c, 2, "010005000000" "00000000" "01000040" "8816706d", NULL_OK);
    feed(&c, 2, "020008000000c814706d201a706d00100000" "04000000" "03000000", NULL_OK);
    feed(&c, 2, "010005000000" "00000000" "02000040" "9816706d", NULL_OK);
    const char *const want[] = {
        "02000900000000000000221a706d0c000000" "000000000000000000000000",
        "0200" "08000000" "a8d2726d" "03000040" "3e000000" "0c000000" "03000000" "00000000" "00000000",
        NULL };
    feed(&c, 2, "020008000000c814706d221a706d01100000" "04000000" "05000000", want);
    /* the app's answer is logged, not answered */
    feed(&c, 2, "010009000000000000000300004000000000", NONE);
    assert(nlogs == 1 && strstr(logs[0], "answered, status 0"));
}

/* bug 2026-11-17: Flight mode never completed (powerapp did not signal POWER_ALL_CLEAR_SEM, SecurityApp and the Phone app
   waited forever): IRadioControl m6 must hand out a request id (output word 1, BAPhandler passes it to m7), and m4 is
   followed by the ICBRadioControl m3 callback to the registration whose handle m6 carried */
static void test_radio_flight_mode_handshake(void)
{
    SybCcpu *c = calloc(1, sizeof(*c));
    sybccpu_reset(c);
    /* two ICBRadioControl registrations (handles 0x100 target 3, 0x101 target 5) */
    feed(c, 2, "010004000000c814706d4035706d01000000" "dbf752fb4bf64817b68e64fba973cf01" "03000000", NULL_OK);
    feed(c, 2, "010004000000c814706d8035706d01000000" "dbf752fb4bf64817b68e64fba973cf01" "05000000", NULL_OK);
    /* an IRadioControl object (as ISystemControlManager m4 returns it): CreateInstance with its IID */
    feed(c, 2, "010000000000c814706dc814706d" "65bd320f21d46a4ebc4ab5000d0e52ba" "2a66250324d9463b9dccc826f31d66c8", NULL_OK);
    feed(c, 2, "010005000000" "00000000" "01000040" "8816706d", NULL_OK);
    /* m6 subscribes handle 0x100: output word 1 = 1 */
    feed(c, 2, "020008000000c814706d201a706d00100000" "0e000000" "06000000" "00010000" "00000000" "0000",
         ONE("02000900000000000000201a706d0c000000" "00000000" "01000000" "00000000"));
    /* m4: its answer, then the callback to target 3 (the subscribed registration, not the latest one) */
    const char *const want[] = {
        "02000900000000000000211a706d08000000" "0000000000000000",
        "0200" "08000000" "c814706d" "02000040" "03000000" "0c000000" "03000000" "00000000" "00000000",
        NULL };
    feed(c, 2, "020008000000c814706d211a706d00100000" "04000000" "04000000", want);
    free(c);
}

/* bugs 2026-11-17 (SE P1i): the phone stopped on "Insert SIM card" (ISimManager m5 said 0 SIM applications), then on
   "SIM card blocked" (ISimApplication m6 said 0 PIN tries left). The minimal SIM: one application of type 1 with a
   label, m6 returns an ISimApplication object, PIN state 0 with 3 / 10 tries */
static void test_minimal_sim(void)
{
    SybCcpu *c = calloc(1, sizeof(*c));
    sybccpu_reset(c);
    /* CreateInstance ISimManager -> object 0x1000 */
    feed(c, 2, "010000000000c814706dc814706d" "0b72e7c10bba2e418857657aae9a15eb" "15fc03037512477cb519587c03c29a51", NULL_OK);
    feed(c, 2, "010005000000" "00000000" "01000040" "8816706d", NULL_OK);
    /* m5: one SIM application */
    feed(c, 2, "020008000000c814706d201a706d00100000" "04000000" "05000000",
         ONE("02000900000000000000201a706d0c000000" "00000000" "01000000" "00000000"));
    /* m3(0): type 1, label "S" (words 4..6 of 114 bytes) */
    feed(c, 2, "020008000000c814706d211a706d00100000" "08000000" "03000000" "00000000", NULL_OK);
    assert(nsent == 1 && sent[0].len == 2 + 16 + 114);
    assert(!memcmp(sent[0].data + 18 + 16, "\x01\0\0\0\x01\0\0\0\x53\0\0\0", 12));
    /* m6(0) -> a new object registered as ISimApplication */
    feed(c, 2, "020008000000c814706d221a706d00100000" "08000000" "06000000" "00000000", NULL_OK);
    assert(nsent == 1 && strstr(tohex(sent[0].data, sent[0].len), "0338bf10672f48a68bbe57c4de35bbd6"));
    feed(c, 2, "010005000000" "00000000" "02000040" "9816706d", NULL_OK);
    /* ISimApplication (object 0x1001) m6(1): PIN1 state 0, 3 tries, 10 unblock tries */
    feed(c, 2, "020008000000c814706d231a706d01100000" "08000000" "06000000" "01000000",
         ONE("02000900000000000000231a706d10000000" "00000000" "00000000" "03000000" "0a000000"));
    free(c);
}

/* bug 2026-11-17 (SE P1i): opening a path on the CCPU file system answered OK, the reader (asrhandler.aps) looped on
   the returned object 34000 times until c32's port handler panicked !PortHandler 4: the open now fails not found */
static void test_ccpu_file_not_found(void)
{
    SybCcpu c;
    sybccpu_reset(&c);
    feed(&c, 2, "010000000000c814706dc814706d" "746b63093a3f412688d37c16d2a9dd7b" "bf9485d81dfe4c0da25639bd516877f3", NULL_OK);
    feed(&c, 2, "010005000000" "00000000" "01000040" "8816706d", NULL_OK);
    feed(&c, 2, "020008000000c814706d201a706d00100000" "0c000000" "08000000" "02000000" "2f006100", NULL_OK);
    assert(nsent == 1 && sent[0].data[2] == 9 && !memcmp(sent[0].data + 18, "\xff\xff\x00\x80", 4));
}

/* types nobody answers yet are reported, not answered */
static void test_unanswered(void)
{
    SybCcpu c;
    sybccpu_reset(&c);
    feed(&c, 2, "010006000000c814706dc814706d00000000", NONE);
    assert(nlogs == 1 && strstr(logs[0], "type 6 (not answered)"));
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
    test_call_output_size();
    test_fragments_reassembled();
    test_query_interface();
    test_size_by_shape();
    test_parameter_reads();
    test_object_65_gets_outputs();
    test_phonebook_not_ready();
    test_cops_manager_returns_lock_objects();
    test_sim_lock_status_callback();
    test_radio_flight_mode_handshake();
    test_minimal_sim();
    test_ccpu_file_not_found();
    test_unanswered();
    printf("caif_peer_test: ok\n");
    return 0;
}
