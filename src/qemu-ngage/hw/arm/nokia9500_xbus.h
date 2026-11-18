/*
 * Nokia 9500 (RA-2): the modem's side of the XBUS link (nokia9500_xbus.c). Byte level: the board's
 * UART2 model feeds every byte the phone transmits in and takes the bytes the modem sends out.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef NOKIA9500_XBUS_H
#define NOKIA9500_XBUS_H

#define RA2_XBUS_MAX 2048           /* the phone's receive buffer [I: XbusDma restarts RX with 2000] */
#define RA2_XBUS_FRAMES 64          /* frames waiting for the phone */
#define RA2_XBUS_ACKS 16            /* ACKs waiting for the phone */
#define RA2_XBUS_HAS_PAYLOAD 0x8000 /* flen[]: a DATA header whose payload burst follows */

typedef struct {
    uint8_t win[4];                 /* the last header-candidate bytes from the phone */
    unsigned nwin;
    uint32_t want, got;             /* payload of a phone DATA frame being collected */
    uint8_t frame_b0;
    uint8_t frame[RA2_XBUS_MAX];
    bool synced;
    uint8_t seq;                    /* the modem's DATA sequence bit (0/1) */
    int wait_ack;                   /* -1, or the header byte 0 of the modem DATA frame not acked yet */
    int phone_seq;                  /* -1, or the sequence bit (0/8) of the phone's last DATA frame */
    uint8_t last[RA2_XBUS_MAX];     /* the modem DATA frame waiting for its ACK (for a resend) */
    uint32_t last_len;
    unsigned resends;
    uint8_t out[16384];             /* bytes for the phone, in order */
    uint32_t out_head, out_len;
    uint16_t flen[RA2_XBUS_FRAMES];            /* bursts in out[] (a header; a payload), oldest first */
    unsigned fhead, nframes;
    uint32_t fleft;                 /* bytes left of the frame being popped */
    bool payload_next;              /* a DATA header is going out; its payload follows (no ACK between) */
    uint8_t acks[RA2_XBUS_ACKS][4]; /* ACKs: sent ahead of queued frames, at frame boundaries */
    unsigned ack_head, nack, ack_left;
    unsigned frames_in, frames_out, dups;
} Ra2Xbus;

uint8_t ra2_xbus_crc8(const uint8_t *b, unsigned n);
#define RA2_XBUS_TXDONE_FALLBACK_NS 70000000LL   /* XbusDma's own TxDone timer [V: research s. 1] */
/*
 * May the modem's next byte go out? CTS high, its answer time reached, and the phone has seen CTS
 * come back (read MSR: its TxDone) since CTS last rose, or 70 ms have passed.
 */
bool ra2_xbus_may_answer(bool cts, bool msr_pending, int64_t now, int64_t cts_rose, int64_t answer_at);
/* UART2 LSR (THR/shifter empty; data ready only outside DMA mode) */
uint8_t ra2_xbus_lsr(bool dma, bool ready);
void ra2_xbus_reset(Ra2Xbus *x);
/* one byte the phone transmitted */
void ra2_xbus_from_phone(Ra2Xbus *x, uint8_t b);
/* not synced: queue a SYNC (the caller paces it); synced: queue the next ISI message, if any. */
void ra2_xbus_poll(Ra2Xbus *x);
/* the phone has not ACKed the last modem DATA frame in time: send it again */
void ra2_xbus_ack_timeout(Ra2Xbus *x);
/* a byte may go to the phone now (no new frame starts while a phone DATA frame is arriving) */
bool ra2_xbus_ready(const Ra2Xbus *x);
/* bytes waiting for the phone (frames and ACKs) */
uint32_t ra2_xbus_pending(const Ra2Xbus *x);
/* the next byte for the phone; *end = it was the last byte of its burst (header or payload) */
bool ra2_xbus_pop(Ra2Xbus *x, uint8_t *b, bool *end);

#endif
