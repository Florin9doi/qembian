/*
 * Nokia 9500 (RA-2): the modem's side of the XBUS link, for the phone's own XbusDma.pdd /
 * XbusProtocol.ldd (protocol: docs/research/ra2-xbus-modem.md).
 *
 * Frames start with a 4-byte header b0 b1 b2 crc (CRC-8, polynomial 0x07, init 0, over b0..b2);
 * b0 & ~8 is the type, bit 3 a sequence bit:
 *   0x10 SYNC; 0x20 DATA (b1:b2 = payload length, big-endian; the payload is one ISI message as
 *   the ISA_IF driver hands it: media byte first); 0x30 ACK (b1 = the acked header's byte 0, or
 *   0x10 for a SYNC); 0xF0 a request answered with ACK 0xF0.
 * Start-up: the phone only listens; the modem sends SYNC, the phone answers with its own SYNC
 * bytes (cycling 01 10 00 00 A2, one byte per transmit), the modem ACKs (30 10 00 B6) and the
 * link is up (IsaApi's sync check passes).
 * The modem pauses after every header and every payload (the board's UART leaves a gap), so each
 * burst is one receive on the phone's side.
 * A resent phone frame (same sequence bit as the last) is ACKed again but not handed on; a modem
 * DATA frame the phone does not ACK in time is sent again (the board's UART calls
 * ra2_xbus_ack_timeout).
 * After that each DATA frame is ACKed by the other side (the modem's first DATA frame has sequence
 * bit 1: a frame with the bit the phone does not expect is ACKed as a duplicate and dropped); the modem sends its next DATA frame only
 * after the phone's ACK of the previous one. ISI requests go to the reply table (ngage_isi.c,
 * the 6600's ISA path), replies come back as DATA frames.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "qemu/log.h"
#include "ngage_isi.h"
#include "nokia9500_xbus.h"

uint8_t ra2_xbus_crc8(const uint8_t *b, unsigned n)
{
    uint8_t c = 0;

    while (n--) {
        c ^= *b++;
        for (int i = 0; i < 8; i++) {
            c = c & 0x80 ? (uint8_t)(c << 1) ^ 0x07 : (uint8_t)(c << 1);
        }
    }
    return c;
}

uint8_t ra2_xbus_lsr(bool dma, bool ready)
{
    /*
     * In DMA mode the modem's bytes wait for the phone's RX DMA: XbusDma arms each receive only after
     * checking LSR data-ready (0x500B4B68) and takes a set bit as line error 8, whose recovery
     * (0x500B419C) leaves the receiver unarmed while [pdd+0x28] is set; the link then died with
     * XBUSProtocol fault 0x1000D. A real phone arms its receive long before the modem's answer is on
     * the wire; under QEMU a slow guest did not (regression xbus_answer_before_rx_armed) [V].
     */
    return 0x60 | (ready && !dma ? 1 : 0);
}

bool ra2_xbus_may_answer(bool cts, bool msr_pending, int64_t now, int64_t cts_rose, int64_t answer_at)
{
    /* wait for the phone's TxDone (it reads MSR after CTS rose), at most the PDD's 70 ms fallback */
    if (msr_pending && now < cts_rose + RA2_XBUS_TXDONE_FALLBACK_NS) {
        return false;
    }
    return cts && now >= answer_at;
}

void ra2_xbus_reset(Ra2Xbus *x)
{
    memset(x, 0, sizeof(*x));
    x->wait_ack = -1;
    x->phone_seq = -1;
}

static void put(Ra2Xbus *x, const uint8_t *b, uint32_t n)
{
    for (uint32_t i = 0; i < n; i++) {
        x->out[(x->out_head + x->out_len++) % sizeof(x->out)] = b[i];
    }
}

/* one frame: header, then n payload bytes */
static void frame(Ra2Xbus *x, uint8_t b0, uint8_t b1, uint8_t b2, const uint8_t *p, uint32_t n)
{
    uint8_t h[4] = { b0, b1, b2, 0 };

    if (x->out_len + 4 + n > sizeof(x->out) || x->nframes + 2 > RA2_XBUS_FRAMES) {
        qemu_log_mask(LOG_UNIMP, "nokia9500-xbus: output full, frame %02x dropped\n", b0);
        return;
    }
    h[3] = ra2_xbus_crc8(h, 3);
    put(x, h, 4);
    put(x, p, n);
    /*
     * The header and the payload are separate bursts: the phone arms the payload's receive only
     * after parsing the header, and fails it (line error 8, 0x500B4B88) if the UART already holds
     * data then [V].
     */
    x->flen[(x->fhead + x->nframes++) % RA2_XBUS_FRAMES] = 4 | (n ? RA2_XBUS_HAS_PAYLOAD : 0);
    if (n) {
        x->flen[(x->fhead + x->nframes++) % RA2_XBUS_FRAMES] = n;
    }
}

static void header(Ra2Xbus *x, uint8_t b0, uint8_t b1, uint8_t b2)
{
    frame(x, b0, b1, b2, NULL, 0);
}

/*
 * An ACK (30 b1 00 crc). ACKs wait in their own queue and go out at the next frame boundary, ahead
 * of queued DATA frames: the phone takes an ACK only while it waits for one, and a modem DATA frame
 * arriving first while the phone waits for its ACK left the phone's receiver unarmed (XBUSProtocol
 * fault 0x1000D, regression xbus_data_crossing_phone_frame).
 */
static void ack(Ra2Xbus *x, uint8_t b1)
{
    uint8_t *h;

    if (x->nack == RA2_XBUS_ACKS) {
        qemu_log_mask(LOG_UNIMP, "nokia9500-xbus: ACK queue full, ACK %02x dropped\n", b1);
        return;
    }
    h = x->acks[(x->ack_head + x->nack++) % RA2_XBUS_ACKS];
    h[0] = 0x30;
    h[1] = b1;
    h[2] = 0;
    h[3] = ra2_xbus_crc8(h, 3);
}

bool ra2_xbus_ready(const Ra2Xbus *x)
{
    if (x->fleft || x->ack_left || x->payload_next) {
        return true;                    /* finish the burst / the DATA frame under way */
    }
    if (x->nack) {
        return true;
    }
    /* no new frame while a phone DATA frame is still arriving (its ACK must go first) */
    return x->out_len && !x->want;
}

uint32_t ra2_xbus_pending(const Ra2Xbus *x)
{
    return x->out_len + x->nack * 4 + x->ack_left;
}

bool ra2_xbus_pop(Ra2Xbus *x, uint8_t *b, bool *end)
{
    if (!ra2_xbus_ready(x)) {
        return false;
    }
    if (x->ack_left || (!x->fleft && !x->payload_next && x->nack)) {
        if (!x->ack_left) {
            x->ack_left = 4;
        }
        *b = x->acks[x->ack_head][4 - x->ack_left];
        *end = !--x->ack_left;
        if (*end) {
            x->ack_head = (x->ack_head + 1) % RA2_XBUS_ACKS;
            x->nack--;
        }
        return true;
    }
    if (!x->out_len) {
        return false;
    }
    if (!x->fleft) {
        uint16_t f = x->flen[x->fhead];

        x->fleft = f & ~RA2_XBUS_HAS_PAYLOAD;
        x->payload_next = f & RA2_XBUS_HAS_PAYLOAD;     /* a DATA header: its payload follows */
        x->fhead = (x->fhead + 1) % RA2_XBUS_FRAMES;
        x->nframes--;
    }
    *b = x->out[x->out_head];
    x->out_head = (x->out_head + 1) % sizeof(x->out);
    x->out_len--;
    *end = !--x->fleft;
    return true;
}

void ra2_xbus_poll(Ra2Xbus *x)
{
    uint8_t m[RA2_XBUS_MAX];
    uint32_t n;

    if (!x->synced) {
        header(x, 0x10, 0, 0);
        return;
    }
    if (x->wait_ack >= 0 || !(n = ngage_isi_isa_take(m, sizeof(m)))) {
        return;
    }
    x->wait_ack = 0x20 | (x->seq << 3);
    frame(x, x->wait_ack, n >> 8, n & 0xff, m, n);
    memcpy(x->last, m, n);
    x->last_len = n;
    x->resends = 0;
    x->seq ^= 1;
    x->frames_out++;
}

void ra2_xbus_ack_timeout(Ra2Xbus *x)
{
    if (x->wait_ack < 0) {
        return;
    }
    if (++x->resends > 5) {
        qemu_log_mask(LOG_UNIMP, "nokia9500-xbus: DATA %02x never ACKed, dropped\n", x->wait_ack);
        x->wait_ack = -1;
        return;
    }
    frame(x, x->wait_ack, x->last_len >> 8, x->last_len & 0xff, x->last, x->last_len);
}

static void on_header(Ra2Xbus *x, const uint8_t *h)
{
    switch (h[0] & ~8) {
    case 0x10:
        if (h[1] == 0 && h[2] == 0) {
            ack(x, 0x10);               /* ACK the phone's SYNC: the link is up */
            if (!x->synced) {
                qemu_log_mask(LOG_UNIMP, "nokia9500-xbus: synced\n");
                /* the phone expects the modem's first DATA frame with sequence bit 1 ([rx+4] = 8) [V] */
                x->seq = 1;
                x->wait_ack = -1;
                x->phone_seq = -1;
            }
            x->synced = true;
        }
        break;
    case 0x20:
        if (!x->synced) {
            break;
        }
        x->frame_b0 = h[0];
        x->want = (h[1] << 8) | h[2];
        x->got = 0;
        if (x->want > sizeof(x->frame)) {
            qemu_log_mask(LOG_UNIMP, "nokia9500-xbus: DATA of %u bytes, too long\n", x->want);
            x->want = 0;
        } else if (!x->want) {
            ack(x, x->frame_b0);
        }
        break;
    case 0x30:
        if (x->wait_ack >= 0 && h[1] == x->wait_ack) {
            x->wait_ack = -1;
        }
        break;
    case 0xf0:
        ack(x, 0xf0);
        break;
    default:
        qemu_log_mask(LOG_UNIMP, "nokia9500-xbus: header %02x %02x %02x ignored\n",
                      h[0], h[1], h[2]);
    }
}

void ra2_xbus_from_phone(Ra2Xbus *x, uint8_t b)
{
    if (x->want) {
        x->frame[x->got++] = b;
        if (x->got == x->want) {
            x->want = 0;
            ack(x, x->frame_b0);
            if ((x->frame_b0 & 8) == x->phone_seq) {
                x->dups++;                      /* a resend: our ACK was late or lost */
            } else {
                x->phone_seq = x->frame_b0 & 8;
                x->frames_in++;
                ngage_isi_isa_send(x->frame, x->got);
            }
        }
        return;
    }
    if (x->nwin == 4) {
        memmove(x->win, x->win + 1, 3);
        x->nwin = 3;
    }
    x->win[x->nwin++] = b;
    if (x->nwin == 4 && ra2_xbus_crc8(x->win, 3) == x->win[3]) {
        x->nwin = 0;
        on_header(x, x->win);
    }
}
