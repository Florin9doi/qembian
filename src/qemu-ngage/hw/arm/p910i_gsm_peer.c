/*
 * Sony Ericsson P910i: the GSM side's end of the OSE link (p910i_gsm_peer.h).
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include "p910i_gsm_peer.h"

static const char *skip(const char *s)
{
    while (*s == ' ' || *s == '\t') {
        s++;
    }
    return s;
}

static bool word(const char **s, char *w, size_t n)
{
    size_t i = 0;

    *s = skip(*s);
    while (**s && !isspace((unsigned char)**s) && **s != '#') {
        if (i + 1 < n) {
            w[i++] = **s;
        }
        (*s)++;
    }
    w[i] = 0;
    return i > 0;
}

static bool hexnum(const char *w, unsigned long max, unsigned long *v)
{
    char *end;

    *v = strtoul(w, &end, 16);
    return *w && !*end && *v <= max;
}

int p910_peer_load(P910Peer *p, const char *text, int *bad)
{
    int line = 0;

    p->n = 0;
    while (*text) {
        const char *s = text, *eol = strchr(text, '\n');
        char w[32];
        unsigned long v;
        P910PeerEntry *e;

        line++;
        text = eol ? eol + 1 : text + strlen(text);
        if (!word(&s, w, sizeof(w))) {
            continue;
        }
        if (p->n == P910_PEER_MAX_ENTRIES) {
            goto err;
        }
        e = &p->e[p->n];
        memset(e, 0, sizeof(*e));
        e->sub = -1;
        if (!strcmp(w, "reply")) {
            char *slash;

            if (!word(&s, w, sizeof(w))) {
                goto err;
            }
            slash = strchr(w, '/');
            if (slash) {
                *slash = 0;
                if (!hexnum(slash + 1, 0xff, &v)) {
                    goto err;
                }
                e->sub = v;
            }
            if (!hexnum(w, 0xffff, &v)) {
                goto err;
            }
            e->signo = v;
        } else if (!strcmp(w, "unasked") || !strcmp(w, "every")) {
            bool every = w[0] == 'e';
            char *end;
            if (!word(&s, w, sizeof(w))) {
                goto err;
            }
            e->ms = strtoul(w, &end, 10);
            if (*end || (every && !e->ms)) {
                goto err;
            }
            e->period = every ? e->ms : 0;
            e->unasked = true;
        } else {
            goto err;
        }
        if (!word(&s, w, sizeof(w)) || !hexnum(w, 0xffff, &v)) {
            goto err;
        }
        e->rsigno = v;
        while (s < text && word(&s, w, sizeof(w))) {
            if (e->len == P910_PEER_MAX_DATA || !hexnum(w, 0xff, &v)) {
                goto err;
            }
            e->data[e->len++] = v;
        }
        p->n++;
    }
    return p->n;
err:
    if (bad) {
        *bad = line;
    }
    return -1;
}

void p910_peer_reset(P910Peer *p)
{
    p->synced = false;
    p->last = 0;
    p->got = 0;
}

static size_t put_frame(uint8_t *out, size_t max, uint8_t dst, uint8_t src, uint16_t signo,
                        const uint8_t *data, unsigned len)
{
    size_t n = 4 + 2 + len + 1;
    unsigned sz = 2 + len;

    if (n > max) {
        return 0;
    }
    out[0] = dst;
    out[1] = sz;
    out[2] = sz >> 8;
    out[3] = src;
    out[4] = signo;
    out[5] = signo >> 8;
    memcpy(out + 6, data, len);
    out[6 + len] = 0;                       /* trailer [I: not checked] */
    return n;
}

size_t p910_peer_byte(P910Peer *p, uint8_t b, uint8_t *out, size_t max, char *done, size_t donelen)
{
    unsigned len;

    if (donelen) {
        done[0] = 0;
    }
    if (!p->synced) {
        if (p->last == 0x4c && b == 0x31) {             /* "L1" -> "LA" */
            p->synced = true;
            p->got = 0;
            snprintf(done, donelen, "L1 -> LA (synced)");
            if (max < 2) {
                return 0;
            }
            out[0] = 0x4c;
            out[1] = 0x41;
            return 2;
        }
        p->last = b;
        return 0;
    }
    if (p->got < sizeof(p->frame)) {
        p->frame[p->got] = b;
    }
    p->got++;
    if (p->got < 6) {
        return 0;
    }
    len = p->frame[1] | p->frame[2] << 8;
    if (len < 2 || len + 4 > sizeof(p->frame)) {        /* not a frame: resynchronise [I] */
        snprintf(done, donelen, "bad length %u, frame dropped", len);
        p->got = 0;
        return 0;
    }
    if (p->got < len + 4) {
        return 0;
    }
    p->got = 0;
    {
        uint8_t dst = p->frame[0], src = p->frame[3];
        uint16_t signo = p->frame[4] | p->frame[5] << 8;
        int at = snprintf(done, donelen, "signal %04X from pid %02X to %02X, %u data bytes:",
                          signo, src, dst, len - 2);
        for (unsigned i = 0; i < len - 2 && i < 24 && at > 0 && (size_t)at < donelen; i++) {
            at += snprintf(done + at, donelen - at, " %02x", p->frame[6 + i]);
        }
        P910PeerEntry *hit = NULL;
        for (unsigned i = 0; i < p->n; i++) {
            P910PeerEntry *e = &p->e[i];
            if (e->unasked || e->signo != signo) {
                continue;
            }
            if (e->sub >= 0 && len > 2 && p->frame[6] == e->sub) {
                hit = e;
                break;
            }
            if (e->sub < 0 && !hit) {
                hit = e;
            }
        }
        if (hit) {
            if (at > 0 && (size_t)at < donelen) {
                snprintf(done + at, donelen - at, " -> %04X", hit->rsigno);
            }
            return put_frame(out, max, src, dst, hit->rsigno, hit->data, hit->len);
        }
        if (at > 0 && (size_t)at < donelen) {
            snprintf(done + at, donelen - at, " UNANSWERED");
        }
    }
    return 0;
}

size_t p910_peer_unasked(P910Peer *p, unsigned i, unsigned *ms, unsigned *period, uint8_t *out, size_t max)
{
    for (unsigned k = 0; k < p->n; k++) {
        P910PeerEntry *e = &p->e[k];
        if (e->unasked && i-- == 0) {
            *ms = e->ms;
            *period = e->period;
            return put_frame(out, max, 0, 0, e->rsigno, e->data, e->len);
        }
    }
    return 0;
}
