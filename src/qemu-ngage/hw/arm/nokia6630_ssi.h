/*
 * The Nokia 6630's modem link, the modem's end (H-rm1-modem): xbus.ldd's word protocol on SSI channel 0
 * and the payloads on channel 3 (docs/research/rm1-ssi-xbus.md). No QEMU types (tests/nokia6630_ssi_test.c).
 *
 * Words (type = word >> 28): 0x00000000 hello, 0x10000000 link up, 0x73HHLLCC data header (LLHH =
 * payload length in 32-bit words, little-endian; CC = 0x73 ^ HH ^ LL), 0x83000000 ACK, 0xC3000000 NAK [V].
 * Start: the phone raises AP wake; the modem raises SSRCtrl once; the phone sends hello; the modem answers
 * hello and link up. Phone -> modem: header, ACK, the payload by DMA into SST channel 3. Modem -> phone: header (the
 * payload ready first: the phone arms its receive DMA as soon as it ACKs), the phone's ACK, the DMA from
 * SSR channel 3. Payloads are one ISI message (media byte first, length 6 + bytes 4..5 [I]) padded to words.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef NOKIA6630_SSI_H
#define NOKIA6630_SSI_H

#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#define RM1_SSI_HELLO 0x00000000u
#define RM1_SSI_UP    0x10000000u
#define RM1_SSI_ACK   0x83000000u
#define RM1_SSI_NAK   0xC3000000u
#define RM1_SSI_MAX   0x2000u          /* the phone's receive limit (bytes) [V] */
#define RM1_SSI_QLEN  64

typedef struct Rm1SsiLink {
    uint32_t rxq[RM1_SSI_QLEN];       /* words waiting in SSR channel 0 for the phone */
    unsigned rxh, rxn;
    bool up;
    bool greet;                       /* the phone said hello: answer on the next tick */
    uint32_t want_words;              /* the phone's announced payload (words), 0 = none */
    uint8_t out[RM1_SSI_MAX];         /* the modem's payload, ready before its header */
    uint32_t out_len;                 /* bytes (whole words), 0 = none */
    bool out_wait_ack;
    uint32_t out_header;              /* to send again after a NAK */
} Rm1SsiLink;

static inline uint32_t rm1_ssi_header(uint32_t words)
{
    uint32_t hh = (words >> 8) & 0xff, ll = words & 0xff;

    return 0x73000000u | hh << 16 | ll << 8 | (0x73 ^ hh ^ ll);
}

static inline bool rm1_ssi_header_ok(uint32_t w)
{
    return (w & 0xff) == (((w >> 8) ^ (w >> 16) ^ (w >> 24)) & 0xff);
}

/* the header's length in words (its LLHH field) */
static inline uint32_t rm1_ssi_header_words(uint32_t w)
{
    return ((w >> 8) & 0xff) | ((w >> 16) & 0xff) << 8;
}

static inline void rm1_ssi_push(Rm1SsiLink *s, uint32_t w)
{
    if (s->rxn < RM1_SSI_QLEN) {
        s->rxq[(s->rxh + s->rxn++) % RM1_SSI_QLEN] = w;
    }
}

static inline bool rm1_ssi_pop(Rm1SsiLink *s, uint32_t *w)
{
    if (!s->rxn) {
        return false;
    }
    *w = s->rxq[s->rxh];
    s->rxh = (s->rxh + 1) % RM1_SSI_QLEN;
    s->rxn--;
    return true;
}

/* a word the phone wrote to SST channel 0 */
static inline void rm1_ssi_from_phone(Rm1SsiLink *s, uint32_t w)
{
    switch (w >> 28) {
    case 0x0:                                   /* hello: answered by rm1_ssi_tick */
        if (!s->up) {
            s->greet = true;
        }
        break;
    case 0x7:                                   /* a message follows */
        if (!rm1_ssi_header_ok(w) || rm1_ssi_header_words(w) * 4 > RM1_SSI_MAX) {
            rm1_ssi_push(s, RM1_SSI_NAK);
            break;
        }
        s->want_words = rm1_ssi_header_words(w);
        rm1_ssi_push(s, RM1_SSI_ACK);
        break;
    case 0x8:                                   /* our header accepted: the phone's DMA is armed */
        s->out_wait_ack = false;
        break;
    case 0xc:                                   /* our header refused: send it again */
        if (s->out_len) {
            rm1_ssi_push(s, s->out_header);
        }
        break;
    }
}

/*
 * The modem's periodic step (the board calls it every 1 ms): answer a hello with hello (it clears the
 * phone's "Ctrl pending" flag, 0x50149f5c [V]; until then every other word is ignored) and link up. Not
 * at once: the phone says hello from its Ctrl interrupt while ssi.pdd's start-up still drains SSR
 * channel 0, and that read ate an immediate answer [V run].
 */
static inline void rm1_ssi_tick(Rm1SsiLink *s)
{
    if (s->greet) {
        s->greet = false;
        s->up = true;
        rm1_ssi_push(s, RM1_SSI_HELLO);
        rm1_ssi_push(s, RM1_SSI_UP);
    }
}

/* the modem's next message (ISI bytes) to send: false when busy with the previous one */
static inline bool rm1_ssi_modem_send(Rm1SsiLink *s, const uint8_t *m, uint32_t n)
{
    uint32_t words = (n + 3) / 4;

    if (!s->up || s->out_len || s->out_wait_ack || !n || words * 4 > RM1_SSI_MAX) {
        return false;
    }
    memset(s->out, 0, words * 4);
    memcpy(s->out, m, n);
    s->out_len = words * 4;
    s->out_wait_ack = true;
    s->out_header = rm1_ssi_header(words);
    rm1_ssi_push(s, s->out_header);
    return true;
}

/* the ISI message length inside a phone payload of `bytes` (6-byte header + its length field) */
static inline uint32_t rm1_ssi_isi_len(const uint8_t *b, uint32_t bytes)
{
    uint32_t len = bytes >= 6 ? 6u + ((uint32_t)b[4] << 8 | b[5]) : bytes;

    return len < bytes ? len : bytes;
}

#endif
