/*
 * Sony Ericsson P910i: the GSM side's end of the OSE link (no QEMU types: host-testable,
 * tests/p910i_gsm_peer_test.c). docs/research/p910i-gsm-link.md.
 *
 * Frames (both directions) [V]: [dst pid][len LE16][src pid][signo LE16][len-2 data bytes]; frames
 * to the phone carry one more trailer byte (not checked). Before the link is synced the phone sends
 * "L1" and the peer answers "LA".
 *
 * Reply table (text, one entry per line, '#' comments, hex numbers):
 *   reply SIGNO[/SUB] RSIGNO [DATA...]   answer the phone's signal SIGNO (and, with /SUB, only when
 *                                  its first data byte is SUB: generic requests carry a sub-command
 *                                  there) with RSIGNO + DATA, addressed to the request's src pid, from
 *                                  its dst pid; a /SUB entry wins over a plain one
 *   unasked MS SIGNO [DATA...]     send SIGNO + DATA, dst/src 0, MS ms after the sync (decimal MS)
 *   every MS SIGNO [DATA...]       the same, again every MS ms (periodic reports such as the battery)
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef HW_ARM_P910I_GSM_PEER_H
#define HW_ARM_P910I_GSM_PEER_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#define P910_PEER_MAX_DATA 200
#define P910_PEER_MAX_ENTRIES 128

typedef struct {
    bool unasked;
    unsigned ms;                    /* unasked: delay after sync */
    unsigned period;                /* every: repeat interval in ms, 0 = once */
    uint16_t signo, rsigno;         /* reply: phone signal -> answer; unasked: rsigno sent */
    int sub;                        /* reply: first data byte to match, -1 = any */
    uint8_t len;
    uint8_t data[P910_PEER_MAX_DATA];
} P910PeerEntry;

typedef struct {
    P910PeerEntry e[P910_PEER_MAX_ENTRIES];
    unsigned n;
    /* link state */
    bool synced;
    uint8_t last;
    uint8_t frame[4 + 2 + 1024];    /* the phone's frame being received */
    unsigned got;
} P910Peer;

/* load a reply table; returns the number of entries, -1 on a bad line (line number in *bad) */
int p910_peer_load(P910Peer *p, const char *text, int *bad);
void p910_peer_reset(P910Peer *p);
/*
 * one byte from the phone; bytes to send back go to out (at most max); returns their count.
 * *done gets a description of a completed frame ("" if none), for the log.
 */
size_t p910_peer_byte(P910Peer *p, uint8_t b, uint8_t *out, size_t max, char *done, size_t donelen);
/*
 * the unasked frame i (in table order) -> out; its first time (ms after sync) and repeat period (0 =
 * once); returns its length, 0 if i is past the last one
 */
size_t p910_peer_unasked(P910Peer *p, unsigned i, unsigned *ms, unsigned *period, uint8_t *out, size_t max);

#endif
