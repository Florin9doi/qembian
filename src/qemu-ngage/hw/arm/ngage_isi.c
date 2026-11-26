/*
 * N-Gage (EKA1) board: the NOSDRIVER channels, the ISI/PhoNet transport between the Symbian
 * side and the NOS (Q36, Q50). Part of hack H-nos-standin: the NOS does not run, so the board
 * sees every channel call the stand-in stub traps and answers it here.
 *
 * nosdriver.ldd [V, Q50] is a pass-through. DoCreate allocates the NOS channel object (8
 * bytes, the stub's trap object 65), stores it at channel+0x38 and calls the NOS open with
 * the unit. Then DoRequest(reqNo, a1, a2) -> object +0x10, DoControl(fn, a1, a2) -> +0x14,
 * DoCancel -> +0x0C. The kernel channel (DLogicalChannel) keeps the client thread at +0x14
 * and the request statuses at +0x18 + 4 * reqNo [V: export 64, Complete(reqNo, reason)].
 *
 * Seen so far [V]: DoControl(4, a1) with a1 an 8-bit descriptor of 10..16 bytes (a send);
 * DoRequest(0, a1, a2) with a1 a 500-byte buffer and a2 a TBuf8<2> (a receive).
 *
 * A send is a PhoNet frame [V, q57]: 00 rdev, 00 sdev? (first three bytes 00), resource,
 * length (16-bit little-endian, from the receiver object on), receiver object, sender object,
 * then the ISI message: transaction id, message id, data.
 *
 * Replies come from a table (-M ngage,isi=FILE; the BB5 fake modem's format, `reply RES REQ
 * RESP [@XX] BYTES...`; `@XX`: only when the request's byte 10, its subtype, is XX; such a line
 * wins over one without). A reply frame mirrors the request [I]: same resource, receiver = the
 * request's sender object, sender object 0, the request's transaction id, RESP, BYTES. It is
 * queued on the channel the request came from and handed to the stub when that channel has a
 * receive (request 0) outstanding: the stub's DFC fetches it (device +0x30..), writes it into
 * the client's descriptor (DThread::Write, ekern export 479) and completes the request
 * (DLogicalChannel::Complete, export 64). Requests without a reply are logged UNANSWERED.
 *
 * `reply+ RES REQ RESP ...` lines add frames after a request's reply (the Nokia 3230's DevSound wants
 * "play started" 0x21 then "play ended" 0x2B for one E8 20; tests/ngage_isi_test.c reply_plus).
 *
 * Indications the modem sends by itself: `ind UNIT OBJ RES MSG BYTES...`, queued when that
 * unit's channel opens (receiver object OBJ, transaction id 0).
 *
 * Nokia 6600 (-M nokia6600): no NOSDRIVER channels. enosdriver.ldd hands each frame to the NOS
 * (export 1, ngage_isi_isa_send) and the NOS hands frames up by calling enosdriver's export 2
 * with a raw frame pointer, which isa_if_driver routes by receiver object [V,
 * docs/devices/nokia-6600-nhl10/bring-up.md]. One pseudo channel (isa_chan) stands for that
 * path: always ready to receive. A frame: media, receiver device, sender device, resource,
 * length (LE, from the receiver object on), receiver object, sender object, transaction id,
 * message id, data [V: isaapi CheckCrcMsgL]. A reply swaps devices and objects [I]. A reply's
 * data byte written `rq10` is the request's byte 10 (the N70's ISC channel open: {channel, object}).
 * Indications (`ind UNIT OBJ RES MSG BYTES`, as for the N-Gage): UNIT is the subscribing object
 * (decimal); the indication goes to object OBJ when UNIT subscribes (resource 0, message 0, to
 * object 0x10: a list of resource/message pairs [I]), from object 0x10.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "qemu/log.h"
#include "hw/core/cpu.h"
#include "ngage_isi.h"

#define NGAGE_ISI_CHANS 32

typedef struct {
    uint32_t obj, dlc, unit;
    bool rx_pending;
    uint32_t rx_req, rx_a1, rx_a2;
} NgageIsiChan;

static NgageIsiChan chans[NGAGE_ISI_CHANS];
static NgageIsiChan isa_chan = { .dlc = 1, .unit = 0xee, .rx_pending = true };

typedef struct {
    uint8_t res, req, resp, len;
    bool echo;                  /* "echo": the request's data, first byte 0 (BB5 H-light) */
    int sub;                    /* "@XX": only for requests whose byte 10 (subtype) is XX; -1 any */
    bool more;                  /* "reply+": an extra frame sent after the request's reply (3230 audio) */
    uint8_t data[200];          /* len= up to 200 (the 9500's SIM 09 31 reply needs a 76-byte frame) */
    uint8_t rq10[4];            /* "rq10": data positions that take the request's byte 10 (N70 ISC channel) */
    uint8_t nrq10;
    int media, sobj;            /* "media=XX", "sobj=XX": override the media byte / sender object; -1 = mirror */
    int robj;                   /* "robj=XX": another receiver object (a reply+ to a third party); -1 = the requester */
} NgageIsiReply;

static NgageIsiReply *replies;
static unsigned nreplies;

typedef struct {
    uint8_t unit, obj, res, msg, len;
    uint8_t data[64];
    int media;                  /* "media=XX": the frame's media byte; -1 = the subscription's (ISA path) */
} NgageIsiInd;

static NgageIsiInd *inds;
static unsigned ninds;

typedef struct NgageIsiMsg {
    struct NgageIsiMsg *next;
    NgageIsiChan *c;
    uint32_t len;
    uint8_t b[512];
} NgageIsiMsg;

static NgageIsiMsg *queue;
static NgageIsiMsg fetched;     /* the delivery the stub is working on */

static void on_send(NgageIsiChan *c, unsigned unit, const uint8_t *b, uint32_t len);

static bool rd(uint32_t va, void *buf, uint32_t len)
{
    return first_cpu && !cpu_memory_rw_debug(first_cpu, va, buf, len, false);
}

static uint32_t rd32(uint32_t va)
{
    uint32_t v = 0;

    rd(va, &v, 4);
    return le32_to_cpu(v);
}

/*
 * An EKA1 8-bit descriptor: word 0 = type << 28 | length. Types: 0 TBufC (data at +4),
 * 1 TPtrC (pointer at +4), 2 TPtr (max +4, pointer +8), 3 TBuf (max +4, data at +8),
 * 4 TBufCPtr (max +4, pointer to a TBufC at +8: data at that +4).
 */
bool ngage_isi_desc(uint32_t va, uint8_t *buf, uint32_t cap, uint32_t *len, uint32_t *max,
                    uint32_t *data)
{
    uint32_t w0 = rd32(va), type = w0 >> 28, d;

    *len = w0 & 0x0fffffff;
    *max = (type >= 2) ? rd32(va + 4) : *len;
    switch (type) {
    case 0: d = va + 4; break;
    case 1: d = rd32(va + 4); break;
    case 2: d = rd32(va + 8); break;
    case 3: d = va + 8; break;
    case 4: d = rd32(va + 8) + 4; break;
    default: return false;
    }
    if (data) {
        *data = d;
    }
    return buf ? rd(d, buf, MIN(*len, cap)) : true;
}

static NgageIsiChan *by_obj(uint32_t obj)
{
    for (int i = 0; i < NGAGE_ISI_CHANS; i++) {
        if (chans[i].obj == obj) {
            return &chans[i];
        }
    }
    return NULL;
}

static void hex(GString *g, const uint8_t *b, uint32_t n)
{
    for (uint32_t i = 0; i < n; i++) {
        g_string_append_printf(g, "%02x%s", b[i], i + 1 < n ? " " : "");
    }
}

void ngage_isi_ctor(uint32_t obj, uint32_t unit)
{
    NgageIsiChan *c = by_obj(obj);

    for (int i = 0; !c && i < NGAGE_ISI_CHANS; i++) {
        if (!chans[i].obj) {
            c = &chans[i];
        }
    }
    if (c) {
        memset(c, 0, sizeof(*c));
        c->obj = obj;
        c->unit = unit;
    }
}

static void enqueue(NgageIsiChan *c, const uint8_t *req, uint32_t reqlen, const NgageIsiReply *r);

void ngage_isi_open(uint32_t dlc, uint32_t unit)
{
    uint32_t obj = rd32(dlc + 0x38);
    NgageIsiChan *c = by_obj(obj);

    if (c) {
        c->dlc = dlc;
        qemu_log_mask(LOG_UNIMP, "ngage-isi: unit %u open: channel %08x, object %08x, client "
                      "thread %08x\n", unit, dlc, obj, rd32(dlc + 0x14));
        for (unsigned i = 0; i < ninds; i++) {
            if (inds[i].unit == unit) {
                uint8_t hdr[10] = { 0, 0, 0, inds[i].res, 0, 0, 0, inds[i].obj, 0, 0 };
                NgageIsiReply r = { .res = inds[i].res, .resp = inds[i].msg, .len = inds[i].len,
                                    .media = inds[i].media, .sobj = -1, .robj = -1 };

                memcpy(r.data, inds[i].data, inds[i].len);
                enqueue(c, hdr, sizeof(hdr), &r);
                qemu_log_mask(LOG_UNIMP, "ngage-isi: unit %u: indication %02x %02x queued\n",
                              unit, inds[i].res, inds[i].msg);
            }
        }
    }
}

void ngage_isi_call(uint32_t obj, unsigned method, uint32_t r1, uint32_t a1, uint32_t a2)
{
    NgageIsiChan *c = by_obj(obj);
    unsigned unit = c ? c->unit : 0xff;
    uint8_t b[512];
    uint32_t len = 0, max = 0;
    GString *g = g_string_new(NULL);

    switch (method) {
    case 3:                                 /* DoCancel(mask) */
        if (c) {
            c->rx_pending = false;
        }
        qemu_log_mask(LOG_UNIMP, "ngage-isi: unit %u cancel %08x\n", unit, r1);
        break;
    case 4:                                 /* DoRequest(reqNo, a1, a2) */
        if (c) {
            c->rx_pending = true;
            c->rx_req = r1;
            c->rx_a1 = a1;
            c->rx_a2 = a2;
        }
        if (ngage_isi_desc(a1, b, sizeof(b), &len, &max, NULL)) {
            g_string_append_printf(g, "a1 len %u max %u", len, max);
            if (r1 != 0 && len) {
                g_string_append(g, ": ");
                hex(g, b, MIN(len, sizeof(b)));
            }
        }
        if (a2 && ngage_isi_desc(a2, b, 16, &len, &max, NULL)) {
            g_string_append_printf(g, "; a2 len %u max %u: ", len, max);
            hex(g, b, MIN(len, 16));
        }
        qemu_log_mask(LOG_UNIMP, "ngage-isi: unit %u request %u: %s\n", unit, r1, g->str);
        break;
    case 5:                                 /* DoControl(fn, a1, a2) */
        if (a1 && ngage_isi_desc(a1, b, sizeof(b), &len, &max, NULL) && r1 == 4) {
            on_send(c, unit, b, MIN(len, sizeof(b)));
            break;
        }
        hex(g, b, MIN(len, sizeof(b)));
        qemu_log_mask(LOG_UNIMP, "ngage-isi: unit %u control %u (%u bytes): %s\n", unit, r1, len,
                      g->str);
        break;
    }
    g_string_free(g, true);
}

void ngage_isi_load(const char *path)
{
    g_autofree char *text = NULL;
    g_auto(GStrv) lines = NULL;

    if (!path || !g_file_get_contents(path, &text, NULL, NULL)) {
        if (path) {
            qemu_log_mask(LOG_UNIMP, "ngage-isi: cannot read reply table %s\n", path);
        }
        return;
    }
    lines = g_strsplit(text, "\n", -1);
    for (char **l = lines; *l; l++) {
        char *h = strchr(*l, '#');
        g_auto(GStrv) t = NULL;
        NgageIsiReply r = { .sub = -1, .media = -1, .sobj = -1, .robj = -1 };
        unsigned k = 0, pad = 0;

        if (h) {
            *h = 0;
        }
        t = g_strsplit_set(g_strstrip(*l), " \t", -1);
        if (t[0] && !strcmp(t[0], "ind")) {
            NgageIsiInd in = { .media = -1 };
            unsigned j = 0;

            for (char **w = t + 1; *w; w++) {
                if (!**w) {
                    continue;
                }
                if (!strncmp(*w, "media=", 6)) {
                    in.media = strtoul(*w + 6, NULL, 16);
                    continue;
                }
                unsigned v = strtoul(*w, NULL, j == 0 ? 10 : 16);    /* the unit is decimal */
                if (j == 0) {
                    in.unit = v;
                } else if (j == 1) {
                    in.obj = v;
                } else if (j == 2) {
                    in.res = v;
                } else if (j == 3) {
                    in.msg = v;
                } else if (in.len < sizeof(in.data)) {
                    in.data[in.len++] = v;
                }
                j++;
            }
            if (j >= 4) {
                inds = g_renew(NgageIsiInd, inds, ninds + 1);
                inds[ninds++] = in;
            }
            continue;
        }
        if (!t[0] || (strcmp(t[0], "reply") && strcmp(t[0], "reply+"))) {
            continue;
        }
        r.more = !strcmp(t[0], "reply+");
        for (char **w = t + 1; *w; w++) {
            if (!**w) {
                continue;
            }
            if (!strcmp(*w, "echo")) {
                r.echo = true;
                continue;
            }
            if (**w == '@') {
                r.sub = strtoul(*w + 1, NULL, 16);
                continue;
            }
            if (!strncmp(*w, "len=", 4)) {
                pad = strtoul(*w + 4, NULL, 0);
                continue;
            }
            if (!strncmp(*w, "media=", 6)) {
                r.media = strtoul(*w + 6, NULL, 16);
                continue;
            }
            if (!strncmp(*w, "sobj=", 5)) {
                r.sobj = strtoul(*w + 5, NULL, 16);
                continue;
            }
            if (!strncmp(*w, "robj=", 5)) {
                r.robj = strtoul(*w + 5, NULL, 16);
                continue;
            }
            if (!strcmp(*w, "rq10") && k >= 3 && r.len < sizeof(r.data) && r.nrq10 < sizeof(r.rq10)) {
                r.rq10[r.nrq10++] = r.len;
                r.data[r.len++] = 0;
                k++;
                continue;
            }
            unsigned v = strtoul(*w, NULL, 16);
            if (k == 0) {
                r.res = v;
            } else if (k == 1) {
                r.req = v;
            } else if (k == 2) {
                r.resp = v;
            } else if (r.len < sizeof(r.data)) {
                r.data[r.len++] = v;
            }
            k++;
        }
        if (k < 3) {
            continue;
        }
        r.len = MAX(r.len, MIN(pad, sizeof(r.data)));
        replies = g_renew(NgageIsiReply, replies, nreplies + 1);
        replies[nreplies++] = r;
    }
    qemu_log_mask(LOG_UNIMP, "ngage-isi: %u replies, %u indications from %s\n", nreplies, ninds,
                  path);
}

static void enqueue(NgageIsiChan *c, const uint8_t *req, uint32_t reqlen, const NgageIsiReply *r)
{
    NgageIsiMsg *m = g_new0(NgageIsiMsg, 1), **q = &queue;
    const uint8_t *data = r->data;
    uint32_t dlen = r->len, body;
    uint8_t e[64];

    if (r->echo && reqlen > 10) {
        dlen = MIN(reqlen - 10, sizeof(e));
        memcpy(e, req + 10, dlen);
        e[0] = 0;
        data = e;
    }
    body = 2 + 2 + dlen;                        /* objects, tid + message id, data */

    m->c = c;
    m->b[3] = req[3];                           /* resource */
    m->b[4] = body & 0xff;
    m->b[5] = body >> 8;
    m->b[6] = req[7];                           /* receiver = the request's sender object */
    m->b[7] = 0;
    m->b[8] = req[8];                           /* transaction id */
    m->b[9] = r->resp;
    if (c == &isa_chan) {
        m->b[0] = req[0];                       /* media */
        m->b[1] = req[2];                       /* receiver device = the request's sender */
        m->b[2] = req[1];
        m->b[7] = req[6];                       /* sender object = the request's receiver */
    }
    memcpy(m->b + 10, data, dlen);
    if (c == &isa_chan && r->media >= 0) {
        m->b[0] = r->media;
    }
    if (c == &isa_chan && r->sobj >= 0) {
        m->b[7] = r->sobj;
    }
    if (c == &isa_chan && r->robj >= 0) {
        m->b[6] = r->robj;
    }
    for (unsigned i = 0; i < r->nrq10; i++) {
        m->b[10 + r->rq10[i]] = reqlen > 10 ? req[10] : 0;
    }
    m->len = 10 + dlen;
    while (*q) {
        q = &(*q)->next;
    }
    *q = m;
}

static void on_send(NgageIsiChan *c, unsigned unit, const uint8_t *b, uint32_t len)
{
    GString *g = g_string_new(NULL);

    hex(g, b, len);
    if (len < 10) {
        qemu_log_mask(LOG_UNIMP, "ngage-isi: unit %u send (short, %u bytes): %s\n", unit, len,
                      g->str);
    } else {
        const NgageIsiReply *r = NULL;

        for (unsigned i = 0; i < nreplies; i++) {
            if (replies[i].more) {
                continue;                       /* reply+ lines only follow a reply */
            }
            if (replies[i].res == b[3] && replies[i].req == b[9] && replies[i].sub < 0 &&
                !(r && r->sub >= 0)) {
                r = &replies[i];
            }
            if (replies[i].res == b[3] && replies[i].req == b[9] && replies[i].sub >= 0 &&
                len > 10 && replies[i].sub == b[10]) {
                r = &replies[i];                /* a subtype match wins */
            }
        }
        qemu_log_mask(LOG_UNIMP, "ngage-isi: unit %u send res %02x msg %02x tid %02x obj %02x: "
                      "%s%s\n", unit, b[3], b[9], b[8], b[7], g->str,
                      r ? "" : "  UNANSWERED");
        if (r && c) {
            enqueue(c, b, len, r);
            /* reply+ RES REQ RESP ...: further frames for the same request, in table order */
            for (unsigned i = 0; i < nreplies; i++) {
                if (replies[i].more && replies[i].res == b[3] && replies[i].req == b[9] &&
                    (replies[i].sub < 0 || (len > 10 && replies[i].sub == b[10]))) {
                    enqueue(c, b, len, &replies[i]);
                }
            }
        }
        if (c == &isa_chan && b[3] == 0 && b[9] == 0) {
            /* a subscription: the modem's state indications for this object */
            for (unsigned i = 0; i < ninds; i++) {
                if (inds[i].unit == b[7]) {
                    /* as if the subscriber had asked: the reply swaps devices and objects */
                    uint8_t hdr[10] = { b[0], b[1], b[2], inds[i].res, 0, 0, b[6], inds[i].obj, 0, 0 };
                    NgageIsiReply ir = { .res = inds[i].res, .resp = inds[i].msg, .len = inds[i].len,
                                         .media = inds[i].media, .sobj = -1, .robj = -1 };

                    memcpy(ir.data, inds[i].data, inds[i].len);
                    enqueue(c, hdr, sizeof(hdr), &ir);
                    qemu_log_mask(LOG_UNIMP, "ngage-isi: object %02x subscribed: indication %02x %02x "
                                  "queued\n", b[7], inds[i].res, inds[i].msg);
                }
            }
        }
    }
    g_string_free(g, true);
}

void ngage_isi_isa_send(const uint8_t *b, uint32_t len)
{
    on_send(&isa_chan, isa_chan.unit, b, len);
}

/* Nokia 9500 (XBUS): the next frame for the phone's ISA_IF, if any; its length, 0 = none. */
uint32_t ngage_isi_isa_take(uint8_t *buf, uint32_t cap)
{
    for (NgageIsiMsg **q = &queue; *q; q = &(*q)->next) {
        NgageIsiMsg *m = *q;

        if (m->c == &isa_chan) {
            GString *g = g_string_new(NULL);
            uint32_t n = MIN(m->len, cap);

            *q = m->next;
            memcpy(buf, m->b, n);
            hex(g, m->b, m->len);
            qemu_log_mask(LOG_UNIMP, "ngage-isi: isa deliver: %s\n", g->str);
            g_string_free(g, true);
            g_free(m);
            return n;
        }
    }
    return 0;
}

bool ngage_isi_ready(void)
{
    for (NgageIsiMsg *m = queue; m; m = m->next) {
        if (m->c->rx_pending) {
            return true;
        }
    }
    return false;
}

/* The stub's DFC asks for the next delivery: false = none left. */
bool ngage_isi_fetch(uint32_t *dlc, uint32_t *a1, uint32_t *req, uint32_t *len)
{
    for (NgageIsiMsg **q = &queue; *q; q = &(*q)->next) {
        NgageIsiMsg *m = *q;

        if (m->c->rx_pending) {
            GString *g = g_string_new(NULL);

            *q = m->next;
            fetched = *m;
            g_free(m);
            if (fetched.c != &isa_chan) {
                fetched.c->rx_pending = false;
            }
            *dlc = fetched.c->dlc;
            *a1 = fetched.c->rx_a1;
            *req = fetched.c->rx_req;
            *len = fetched.len;
            hex(g, fetched.b, fetched.len);
            qemu_log_mask(LOG_UNIMP, "ngage-isi: unit %u deliver: %s\n", fetched.c->unit, g->str);
            g_string_free(g, true);
            return true;
        }
    }
    return false;
}

uint32_t ngage_isi_fetched_word(unsigned off)
{
    return off + 4 <= sizeof(fetched.b) ? ldl_le_p(fetched.b + off) : 0;
}

void ngage_isi_done(int32_t result)
{
    if (result) {
        qemu_log_mask(LOG_UNIMP, "ngage-isi: unit %u delivery: DThread::Write -> %d\n",
                      fetched.c ? fetched.c->unit : 0xff, result);
    }
}
