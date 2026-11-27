/*
 * The IR link of an emulated phone's IrDA port: see ngage_irlink.h.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/error-report.h"
#include "qemu/main-loop.h"
#include "qemu/sockets.h"
#include "qapi/error.h"
#include "chardev/char-fe.h"
#include "chardev/char-serial.h"
#include "ngage_irlink.h"

static void irlink_readable(void *opaque);

/* feed what the board takes; read the socket again only once the datagram is all in */
static void irlink_feed(NgageIrLink *l)
{
    while (l->rx_pos < l->rx_len) {
        unsigned n = MIN(l->rx_room(l->opaque), l->rx_len - l->rx_pos);

        if (!n) {
            break;
        }
        l->rx_bytes(l->opaque, l->rx + l->rx_pos, n);
        l->rx_pos += n;
    }
    qemu_set_fd_handler(l->fd, l->rx_pos < l->rx_len ? NULL : irlink_readable, NULL, l);
}

static void irlink_readable(void *opaque)
{
    NgageIrLink *l = opaque;

    while (l->rx_pos >= l->rx_len) {
        ssize_t n = recv(l->fd, l->rx, sizeof(l->rx), 0);

        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n <= 0) {
            break;                  /* EAGAIN, or an empty datagram */
        }
        l->rx_len = n;
        l->rx_pos = 0;
        irlink_feed(l);
    }
}

static struct addrinfo *irlink_resolve(const char *hp, bool passive)
{
    struct addrinfo hints = { .ai_family = AF_UNSPEC, .ai_socktype = SOCK_DGRAM,
                              .ai_flags = passive ? AI_PASSIVE : 0 };
    struct addrinfo *res = NULL;
    const char *colon = strrchr(hp, ':');
    g_autofree char *host = colon ? g_strndup(hp, colon - hp) : NULL;

    if (!colon || !colon[1] || getaddrinfo(*host ? host : (passive ? NULL : "127.0.0.1"), colon + 1, &hints,
                                           &res) != 0) {
        return NULL;
    }
    return res;
}

static void irlink_open_udp(NgageIrLink *l, const char *spec)
{
    g_autofree char *remote = NULL;
    const char *at;
    struct addrinfo *r, *a;

    if (!g_str_has_prefix(spec, "udp:") || !(at = strchr(spec, '@'))) {
        error_report("irda=%s: give udp:HOST:PORT@[LHOST]:LPORT", spec);
        exit(1);
    }
    remote = g_strndup(spec + 4, at - (spec + 4));
    r = irlink_resolve(remote, false);
    a = irlink_resolve(at + 1, true);
    if (!r || !a) {
        error_report("irda=%s: cannot resolve %s", spec, r ? at + 1 : remote);
        exit(1);
    }
    l->fd = qemu_socket(a->ai_family, SOCK_DGRAM, 0);
    if (l->fd < 0 || bind(l->fd, a->ai_addr, a->ai_addrlen) < 0) {
        error_report("irda=%s: cannot bind %s: %s (another phone on this end?)", spec, at + 1, strerror(errno));
        exit(1);
    }
    if (!qemu_set_blocking(l->fd, false, &error_fatal)) {
        exit(1);
    }
    memcpy(&l->peer, r->ai_addr, r->ai_addrlen);
    l->peer_len = r->ai_addrlen;
    freeaddrinfo(r);
    freeaddrinfo(a);
    qemu_set_fd_handler(l->fd, irlink_readable, NULL, l);
}

static void irlink_flush(void *opaque)
{
    NgageIrLink *l = opaque;

    if (!l->tx_len) {
        return;
    }
    /* never blocks the CPU: a peer that does not read loses the bytes, as light nobody sees */
    if (l->fd >= 0) {
        if (sendto(l->fd, l->tx, l->tx_len, 0, (struct sockaddr *)&l->peer, l->peer_len) < 0) {
            /* nobody there, or the queue is full: the burst is lost */
        }
    } else if (l->chr_on) {
        qemu_chr_fe_write(&l->chr, l->tx, l->tx_len);
    }
    l->tx_len = 0;
}

void ngage_irlink_tx(NgageIrLink *l, uint8_t b)
{
    if (l->tx_len == sizeof(l->tx)) {
        irlink_flush(l);
    }
    l->tx[l->tx_len++] = b;
    qemu_bh_schedule(l->tx_bh);
}

static int irlink_can_receive(void *opaque)
{
    NgageIrLink *l = opaque;

    return l->rx_room(l->opaque);
}

static void irlink_receive(void *opaque, const uint8_t *buf, int size)
{
    NgageIrLink *l = opaque;

    l->rx_bytes(l->opaque, buf, size);
}

void ngage_irlink_kick(NgageIrLink *l)
{
    if (l->fd >= 0) {
        if (l->rx_pos < l->rx_len) {
            irlink_feed(l);
        }
    } else if (l->chr_on) {
        qemu_chr_fe_accept_input(&l->chr);
    }
}

bool ngage_irlink_rx_pending(const NgageIrLink *l)
{
    return l->fd >= 0 && l->rx_pos < l->rx_len;
}

void ngage_irlink_set_baud(NgageIrLink *l, unsigned baud)
{
    QEMUSerialSetParams p = { .speed = baud, .parity = 'N', .data_bits = 8, .stop_bits = 1 };

    qemu_log_mask(LOG_UNIMP, "%s: %u baud\n", l->name, baud);
    if (l->chr_on) {
        qemu_chr_fe_ioctl(&l->chr, CHR_IOCTL_SERIAL_SET_PARAMS, &p);   /* only ttys take it */
    }
}

void ngage_irlink_init(NgageIrLink *l, const char *name, const char *udp_spec, Chardev *chr,
                       unsigned (*rx_room)(void *), void (*rx_bytes)(void *, const uint8_t *, unsigned),
                       void *opaque)
{
    memset(l, 0, sizeof(*l));
    l->name = name;
    l->fd = -1;
    l->rx_room = rx_room;
    l->rx_bytes = rx_bytes;
    l->opaque = opaque;
    l->tx_bh = qemu_bh_new(irlink_flush, l);
    if (udp_spec) {
        irlink_open_udp(l, udp_spec);
    } else if (chr) {
        qemu_chr_fe_init(&l->chr, chr, &error_abort);
        qemu_chr_fe_set_handlers(&l->chr, irlink_can_receive, irlink_receive, NULL, NULL, l, NULL, true);
        l->chr_on = true;
    }
}
