/*
 * Syborg fake CCPU (qembian): the P1i's modem CPU behind BRIDGEDRV.LDD (syborg_ccpu.h). Evidence marks:
 * [V] read in the R6E30 firmware (addresses in its core image) or seen on a boot, [I] inferred.
 *
 * Packet = byte 0 link id (0 = control), byte 1 command / data flags, then the payload. One packet per
 * bridgedrv read or write (no framing).
 *
 * CAIF control (link 0) [V caifmux.csy]: command low nibble 0 LINK_SETUP, 1 LINK_DESTROY, 3 ENUM,
 *   9 MODEM_SET, answered with the same bytes and bit 0x20 set; LINK_SETUP's answer ends with the new
 *   channel id (f8186458). Responses, errors and other commands are never answered: a CCPU-initiated
 *   control request or response 2, 4..8 makes caifmux panic 104..110.
 * Data link, byte 1 = 0: "AT*SECAIF=<n>\r" -> "\r\nOK\r\n" (f8186836), else a BridgeServer message
 *   { u32 type, word1 (sender object), word2 (request id), ... } little endian. Dispatcher f81a6e40:
 *   even types (and 13, 15) are requests, odd ones replies matched to a request by word 2; incoming
 *   requests 0, 2, 4/6, 8, 10 go to BServer's handlers, 12, 13, 15 from us are dropped.
 *   App -> CCPU, answered here:
 *     13 RemoteCreateThreadId  -> 14 { 14, 0, req }: word 1 is our status; nonzero makes BServer flag the
 *        bridge failed (f81a8454 -> f81a39c2) and every CreateSubSession complete -29 (APS-9 0x8000FFE3) [V]
 *     4  { 4, proxy, req, 1, IID, n } the app registers interface n -> 5 { 5, 0, req, our handle } [V layout;
 *        the handle's use is I]
 *     0  { 0, proxy, req, CLSID, IID } CreateInstance: the answer { 1, status, req, value } must name a
 *        proxy in BServer's table (f81a5644 -> lookup f81a6ae6; a miss completes the client 0x80030000) [V].
 *        So we create the object, register it with BServer by our own request 4 { 4, proxy, req', 1, IID,
 *        obj } (handler f81a514e creates the proxy, key = obj, and answers 5 { 5, status, req', local id }),
 *        then answer the CreateInstance with that local id [V handler; sequence I]. Word 1 must be the
 *        app's proxy the CreateInstance came from: the new proxy looks it up through its owner (f81a74a4,
 *        vtable +56) and BServer answers status 12 (a KErrNoMemory leave) for an unknown one [V].
 *     8  { 8, proxy, req, object, arg length, args.. } a call on a CCPU object [I: from the layout; the
 *        method is not identified yet] -> 9 { 9, status, req, w3 } (handler f81a61c2 passes the whole
 *        message on: outputs would follow) [V layout]. Answered 0 with no outputs, logged with the object's
 *        class (H-p1i-ccpu-calls).
 *   Everything else is logged and left unanswered (H-p1i-ccpu-silent).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later OR EPL-1.0
 */
#if defined(SYBMODEM_HOSTED)
#include "syborg_modem_env.h"
#elif defined(SYBCCPU_TEST)
#include <stdint.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#else
#include "qemu/osdep.h"
#endif
#include "syborg_ccpu.h"

enum { BS_CREATE = 0, BS_CREATE_R = 1, BS_REGISTER = 4, BS_REGISTER_R = 5, BS_CALL = 8, BS_CALL_R = 9,
       BS_THREAD = 13, BS_THREAD_R = 14 };
#define OBJ_BASE 0x1000u

static uint32_t rd32(const uint8_t *p)
{
    return p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static void wr32(uint8_t *p, uint32_t v)
{
    p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24;
}

static void ccpu_log(const SybCcpuOut *out, uint32_t unit, const char *fmt, ...)
{
    char text[200];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(text, sizeof(text), fmt, ap);
    va_end(ap);
    out->log(out->ctx, unit, text);
}

/* a GUID as Symbian/COM prints it: {xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx} from its little-endian struct */
static void guid_text(const uint8_t *g, char *buf)
{
    static const char hx[] = "0123456789abcdef";
    static const int order[16] = {3, 2, 1, 0, -1, 5, 4, -1, 7, 6, -1, 8, 9, -1, 10, 11};
    char *o = buf;
    *o++ = '{';
    for (int i = 0; i < 16; i++) {
        if (order[i] < 0) {
            *o++ = '-';
            continue;
        }
        *o++ = hx[g[order[i]] >> 4];
        *o++ = hx[g[order[i]] & 15];
    }
    for (int i = 12; i < 16; i++) {
        *o++ = hx[g[i] >> 4];
        *o++ = hx[g[i] & 15];
    }
    *o++ = '}';
    *o = 0;
}

/* a BridgeServer message { type, w1, w2, w3 } padded with zeros to CCPU_BS_REPLY, on data link `link` */
static void bs_send(const SybCcpuOut *out, uint32_t unit, uint32_t link, uint32_t type, uint32_t w1,
                    uint32_t w2, uint32_t w3)
{
    uint8_t pkt[2 + CCPU_BS_REPLY];
    memset(pkt, 0, sizeof(pkt));
    pkt[0] = link;
    wr32(pkt + 2, type);
    wr32(pkt + 6, w1);
    wr32(pkt + 10, w2);
    wr32(pkt + 14, w3);
    out->send(out->ctx, unit, pkt, sizeof(pkt));
}

static void bs_create(SybCcpu *c, const SybCcpuOut *out, uint32_t unit, uint32_t link, const uint8_t *m, uint32_t n)
{
    uint32_t req = rd32(m + 8);
    char clsid[40], iid[40];
    if (n < 44) {
        ccpu_log(out, unit, "CreateInstance too short (%u bytes): no answer", (unsigned)n);
        return;
    }
    guid_text(m + 12, clsid);
    guid_text(m + 28, iid);
    SybCcpuCreate *slot = NULL;
    for (int i = 0; i < CCPU_CREATES && !slot; i++) {
        if (!c->create[i].used) {
            slot = &c->create[i];
        }
    }
    if (!slot) {
        ccpu_log(out, unit, "CreateInstance %s: too many pending, answered 0x80000001", clsid);
        bs_send(out, unit, link, BS_CREATE_R, 0x80000001u, req, 0);
        return;
    }
    slot->used = 1;
    slot->unit = unit;
    slot->link = link;
    slot->app_req = req;
    slot->our_req = c->next_req++;
    slot->object = c->next_obj++;
    if (slot->object - OBJ_BASE < CCPU_OBJECTS) {
        memcpy(c->obj_clsid[slot->object - OBJ_BASE], m + 12, 16);
        memcpy(c->obj_iid[slot->object - OBJ_BASE], m + 28, 16);
    }
    ccpu_log(out, unit, "CreateInstance %s %s -> object %u, registering it", clsid, iid, (unsigned)slot->object);
    /* our request 4: { 4, app proxy, req', 1, IID, obj } (36 bytes, as the app's own type 4) */
    uint8_t pkt[2 + 36];
    memset(pkt, 0, sizeof(pkt));
    pkt[0] = link;
    wr32(pkt + 2, BS_REGISTER);
    wr32(pkt + 6, rd32(m + 4));
    wr32(pkt + 10, slot->our_req);
    wr32(pkt + 14, 1);
    memcpy(pkt + 18, m + 28, 16);
    wr32(pkt + 34, slot->object);
    out->send(out->ctx, unit, pkt, sizeof(pkt));
}

/* BServer's answer 5 to our request 4: the proxy exists (or not); answer the CreateInstance that needed it */
static void bs_registered(SybCcpu *c, const SybCcpuOut *out, uint32_t unit, const uint8_t *m, uint32_t n)
{
    uint32_t status = rd32(m + 4), req = rd32(m + 8), local = n >= 16 ? rd32(m + 12) : 0;
    for (int i = 0; i < CCPU_CREATES; i++) {
        SybCcpuCreate *s = &c->create[i];
        if (s->used && s->our_req == req) {
            ccpu_log(out, unit, "object %u: BServer proxy %08x status %u, CreateInstance answered",
                     (unsigned)s->object, (unsigned)local, (unsigned)status);
            bs_send(out, s->unit, s->link, BS_CREATE_R, status, s->app_req, status ? 0 : local);
            s->used = 0;
            return;
        }
    }
    ccpu_log(out, unit, "answer 5 to unknown request %08x", (unsigned)req);
}

/* a call on one of our objects: answered success, no outputs; logged with the object's class and the args */
static void bs_call(SybCcpu *c, const SybCcpuOut *out, uint32_t unit, uint32_t link, const uint8_t *m, uint32_t n)
{
    uint32_t req = rd32(m + 8), obj = n >= 16 ? rd32(m + 12) : 0, alen = n >= 20 ? rd32(m + 16) : 0;
    char clsid[40] = "?", args[3 * 16 + 4];
    if (obj - OBJ_BASE < CCPU_OBJECTS && obj < c->next_obj) {
        guid_text(c->obj_clsid[obj - OBJ_BASE], clsid);
    }
    int k = 0;
    for (uint32_t i = 20; i < n && i < 20 + 16; i++) {
        static const char hx[] = "0123456789abcdef";
        args[k++] = hx[m[i] >> 4];
        args[k++] = hx[m[i] & 15];
        if ((i - 20) % 4 == 3 && i + 1 < n) {
            args[k++] = ' ';
        }
    }
    args[k] = 0;
    ccpu_log(out, unit, "call object %u %s args %u: %s -> 0", (unsigned)obj, clsid, (unsigned)alen, args);
    bs_send(out, unit, link, BS_CALL_R, 0, req, 0);
}

void sybccpu_reset(SybCcpu *c)
{
    memset(c, 0, sizeof(*c));
    for (int i = 0; i < CCPU_UNITS; i++) {
        c->next_link[i] = 1;
    }
    c->next_value = 0x100;
    c->next_req = 0x40000001;
    c->next_obj = 0x1000;
}

void sybccpu_packet(SybCcpu *c, const SybCcpuOut *out, uint32_t unit, const uint8_t *p, uint32_t n)
{
    static const char secaif[] = "AT*SECAIF=";
    static const char ok[] = "\r\nOK\r\n";
    uint8_t pkt[CCPU_MAX_PKT];
    if (n < 2) {
        return;
    }
    if (p[0] == 0) {                                     /* CAIF control */
        uint8_t cmd = p[1] & 0x0f;
        if ((p[1] & 0x30) || !(cmd == 0 || cmd == 1 || cmd == 3 || cmd == 9) || n + 1 > CCPU_MAX_PKT) {
            return;
        }
        memcpy(pkt, p, n);
        pkt[1] = p[1] | 0x20;
        if (cmd == 0) {
            pkt[n++] = c->next_link[unit % CCPU_UNITS]++;   /* LINK_SETUP: the channel id */
        }
        out->send(out->ctx, unit, pkt, n);
        return;
    }
    if (p[1] != 0) {                                     /* flow control on a data link */
        return;
    }
    if (n >= 2 + sizeof(secaif) - 1 && memcmp(p + 2, secaif, sizeof(secaif) - 1) == 0) {
        pkt[0] = p[0];
        pkt[1] = 0;
        memcpy(pkt + 2, ok, sizeof(ok) - 1);
        out->send(out->ctx, unit, pkt, 2 + sizeof(ok) - 1);
        return;
    }
    const uint8_t *m = p + 2;
    uint32_t mn = n - 2;
    if (mn < 12) {
        return;
    }
    uint32_t type = rd32(m), req = rd32(m + 8);
    switch (type) {
    case BS_THREAD:
        bs_send(out, unit, p[0], BS_THREAD_R, 0, req, 0);
        return;
    case BS_REGISTER:
        bs_send(out, unit, p[0], BS_REGISTER_R, 0, req, c->next_value++);
        return;
    case BS_CREATE:
        bs_create(c, out, unit, p[0], m, mn);
        return;
    case BS_REGISTER_R:
        bs_registered(c, out, unit, m, mn);
        return;
    case BS_CALL:
        bs_call(c, out, unit, p[0], m, mn);
        return;
    }
    ccpu_log(out, unit, "link %u BridgeServer message type %u (not answered)", (unsigned)p[0], (unsigned)type);
}
