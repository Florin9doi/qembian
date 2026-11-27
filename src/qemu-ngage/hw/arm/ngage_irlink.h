/*
 * The IR link of an emulated phone's IrDA port: the raw SIR byte stream between the board's IR UART and the
 * outside (another emulated phone, ngage_irsend.py, a host serial IR dongle). Used by the 6600-class boards
 * (ngage.c + ngage_sir.c) and the 9210 (nokia9210.c + nokia9210_ir.h). Usage: docs/devices/nokia-6600-nhl10/infrared.md.
 *
 * Two transports:
 * - -M ...,irda=udp:HOST:PORT@[LHOST]:LPORT: the board's own UDP socket. QEMU's udp chardev connects its
 *   socket and reads it blocking: when the peer is gone (ngage_irsend.py exited), the ICMP port-unreachable for
 *   the phone's next byte woke the read watch, a TX write from the vCPU thread consumed the error first, and the
 *   main loop sat in recvmsg holding the BQL until another datagram came: the phone, its monitor and gdb stub
 *   froze. This socket is not connected (Linux reports no ICMP errors to it) and never blocks; any sender
 *   reaches the phone. No SO_REUSEADDR: a second phone on the same end fails to start instead of sharing the port.
 * - otherwise QEMU's 2nd serial port (serial_hd(1)), any chardev: tcp, a host tty (the phone's baud is set on it).
 *
 * TX bytes are collected and sent from a bottom half, one datagram per burst: the 9210's driver puts a whole
 * frame in one interrupt, and one datagram per byte overflowed the peer's socket queue (~280 one-byte
 * datagrams fit in Linux's default receive buffer).
 * RX: the link hands the board what rx_room() allows; after a refusal the board calls ngage_irlink_kick()
 * when it has room again.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef NGAGE_IRLINK_H
#define NGAGE_IRLINK_H

#include "chardev/char-fe.h"

typedef struct NgageIrLink {
    const char *name;                   /* log prefix ("ngage-sir", "nokia9210-ir") */
    int fd;                             /* the UDP socket, or -1 */
    struct sockaddr_storage peer;
    socklen_t peer_len;
    uint8_t rx[4096];
    unsigned rx_len, rx_pos;            /* the datagram being fed to the board */
    uint8_t tx[4096];
    unsigned tx_len;
    QEMUBH *tx_bh;
    CharFrontend chr;
    bool chr_on;
    unsigned (*rx_room)(void *opaque);
    void (*rx_bytes)(void *opaque, const uint8_t *buf, unsigned n);
    void *opaque;
} NgageIrLink;

/* udp_spec (irda=) wins; else the chardev (may be NULL: not linked, TX is dropped) */
void ngage_irlink_init(NgageIrLink *l, const char *name, const char *udp_spec, Chardev *chr,
                       unsigned (*rx_room)(void *), void (*rx_bytes)(void *, const uint8_t *, unsigned),
                       void *opaque);
void ngage_irlink_tx(NgageIrLink *l, uint8_t b);
void ngage_irlink_kick(NgageIrLink *l);
void ngage_irlink_set_baud(NgageIrLink *l, unsigned baud);
bool ngage_irlink_rx_pending(const NgageIrLink *l);    /* a datagram is partly fed */

#endif
