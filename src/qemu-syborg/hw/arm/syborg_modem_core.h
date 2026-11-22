/*
 * Syborg fake modem (qembian): the modem core, shared by the QEMU device (syborg_modem.c) and the hosted
 * EKA2 board (src/hosted/variant/hmodem.cpp). Everything the modem knows (the ISI protocol, the reply
 * table, SIM, network, packet data, SMS, power) lives in the core; a host gives it interrupts, timers,
 * a log, power control, a network and the sound device's ring state (SybModemHost). Register window per
 * port and protocol: syborg_modem.c header and docs/devices/nokia-n95-8gb-rm320/n95-isc-isi-protocol*.md.
 *
 * Built without QEMU when SYBMODEM_HOSTED is defined (the hosted build provides syborg_modem_env.h:
 * fixed-width types, string functions, vsnprintf, an allocator).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later OR EPL-1.0
 * (dual licensed by its author, 2026-11-12: usable in QEMU and in the EPL hosted kernel)
 */
#ifndef SYBORG_MODEM_CORE_H
#define SYBORG_MODEM_CORE_H

#ifdef SYBMODEM_HOSTED
#include "syborg_modem_env.h"
typedef struct SybModemTimer SybModemTimer;     /* the hosted board's timer */
#else
#include "qemu/osdep.h"
#include "qemu/timer.h"
typedef QEMUTimer SybModemTimer;
#define SYBMODEM_ALLOC_ZERO(n) g_malloc0(n)
#endif

#ifndef MIN
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#endif

#include "syborg_ccpu.h"

#define MODEM_PORTS     2
#define MODEM_CHANNELS  64          /* ISC channel numbers 0..63 */
#define MODEM_LINK_RES  8           /* link mode: resources remembered per channel object */
#define MODEM_HANDLES   64          /* guest channel objects per port (handle 0 unused) */
#define MODEM_MAX_MSG   2048        /* ISI messages (the guest driver's are <= 256) and packet data frames */
#define MODEM_FIFO      64
#define MODEM_SUBS      128
#define MODEM_LATE      16
#define MODEM_SMS_HELD  8
#define MODEM_BUF       0x800
#define MODEM_WINDOW    0x2000      /* register window per port */
#define SYBMODEM_ID     0xc51d0020  /* SYBORG_ID_MODEM (syborg.h) */

enum { TX_SEND = 1, TX_SUBSCRIBE = 2, TX_OPEN = 3, TX_CLOSE = 4, TX_DATA = 5,   /* 5: ISA DataSend on a data channel */
       TX_CAIF = 6 };              /* 6: a CAIF packet from the P1i's bridgedrv (channel number = its unit), syborg_ccpu.c */
enum { RX_DELIVER = 1, RX_LOG = 2, RX_DATA = 3,            /* 3: for the channel's DataReceive */
       RX_CAIF = 4 };              /* 4: a CAIF packet from the fake CCPU for bridgedrv */

typedef struct ModemFrame {
    uint32_t kind, channel, len;
    uint8_t data[MODEM_MAX_MSG];
} ModemFrame;

/* A channel is one guest channel object, not an ISC channel number: several clients open the same number
 * (the 701's audio server, DSP controller and audio engine all on 36), each with its own replies,
 * subscriptions and open/close. The guest numbers its objects per port and writes handle << 8 | number;
 * the key used here is port << 16 | that, and frames carry handle << 8 | number back. */
#define CH_NUM(k)    ((k) & 0xff)
#define CH_HANDLE(k) (((k) >> 8) & 0xff)
#define CH_PORT(k)   ((k) >> 16)

typedef struct ModemChannel {
    uint32_t subs_len;
    uint8_t subs[MODEM_SUBS];
    uint32_t mtc_ind_sent;
    uint32_t ind_sent;              /* [modem].indications already sent (bit per entry) */
    /* link mode only (not migrated: the linked modem is another process): the channel's key while open (0 =
       closed), and the resources it sent requests to most recently (sequence numbers), so that a modem reply,
       which names only the channel number, goes back to the object that asked */
    uint32_t link_key;
    uint8_t link_res[MODEM_LINK_RES];
    uint32_t link_seq[MODEM_LINK_RES];
} ModemChannel;

typedef struct ModemLate {
    SybModemTimer *timer;
    struct SyborgModemState *s;
    uint32_t busy, channel, res, tid, id, len;
    uint8_t data[16];
    int snd_slot;                   /* >= 0: reply once syborg-snd slot snd_slot has played up to snd_target */
    uint32_t snd_lead;              /* ... minus this many bytes still queued */
    uint64_t snd_target;
} ModemLate;

typedef struct ModemPort {
    struct SyborgModemState *s;
    uint32_t tx_channel, tx_len, int_enable;
    uint8_t tx[MODEM_BUF];
    uint32_t head, count;
    uint32_t hold;                  /* the last `hold` frames came out of the guest's own send: no interrupt */
    uint32_t in_tx;                 /* a send is running (not migrated: never true between instructions) */
    ModemFrame fifo[MODEM_FIFO];
} ModemPort;

#define MODEM_REPLY_MAX 255         /* data bytes of one table reply (1B 2E -> 2F is 138: n95-isi-encoders.md 6) */

typedef struct ModemReply {
    uint8_t res, req, resp, len;
    uint8_t data[MODEM_REPLY_MAX];
} ModemReply;

/* what the core needs from its host */
typedef struct SybModemHost {
    void (*set_irq)(void *ctx, int port, int level);
    void (*timer_mod)(void *ctx, SybModemTimer *t, int ms);    /* fire ms milliseconds from now (guest time) */
    void (*timer_del)(void *ctx, SybModemTimer *t);
    void (*guest_error)(void *ctx, const char *text);          /* a malformed guest access (host log) */
    void (*power)(void *ctx, int what);                        /* 1 switch off, 2 reset */
    bool (*net_present)(void *ctx);                            /* packet data has a host network */
    void (*net_send)(void *ctx, const uint8_t *frame, size_t len);
    void (*net_flush)(void *ctx);                              /* room again: resend held packets */
    bool (*snd_ring_written)(void *ctx, int slot, uint32_t pos, uint64_t *target);
    uint32_t (*snd_pending)(void *ctx, int slot, uint64_t target);
    uint32_t (*snd_ring_size)(void *ctx, int slot);
    void (*snd_beep)(void *ctx, uint32_t hz, uint32_t ms, uint32_t gain);
    int64_t (*wall_seconds)(void *ctx);                        /* host UTC, seconds since 1970 (RTC 19 0A); may be NULL */
    /* a real modem behind a link (the n95-cmt machine running the phone's own modem firmware): every ISI
       message the guest sends goes here, complete with its PhoNet header; may be NULL (no link) */
    void (*link_send)(void *ctx, const uint8_t *msg, size_t len);
} SybModemHost;

typedef struct SyborgModemState {
    const SybModemHost *host;
    void *host_ctx;
    const char *config;             /* the board config string ("key=value;...") */
    ModemPort port[MODEM_PORTS];
    ModemChannel ch[MODEM_PORTS * MODEM_HANDLES];
    ModemLate late[MODEM_LATE];
    SybModemTimer *power_timer;
    uint32_t power_what;            /* 1 switch off, 2 reset */
    uint16_t aud_src;               /* audio parameter 5 of device 5 (0B 14): what plays next (0x80 = key tone) */
    uint16_t aud_on;                /* parameter 8 of device 5: the output is on */
    uint8_t mtc_state;              /* MTC state the phone last asked for (MTC_STATE_REQ), 0x01 NORMAL at reset */
    uint8_t gpds_cid, pipe_next;    /* packet data (with a SIM): last context id, last pipe handle given out */
    uint8_t gpds_attached, gpds_detached;
    uint8_t gpds_pdp_type;          /* the last CONTEXT_CONFIGURE's PDP type and APN sub-block (for ACTIVATING_IND) */
    uint8_t gpds_apn[68];
    uint8_t gpds_apn_len;
    uint32_t chan_key[MODEM_PORTS][MODEM_CHANNELS];   /* the open channel's key per (port, channel number), 0 = none */
    uint8_t pipe_pep_ch[256];       /* pipe handle -> the channel of its pep1 (DATAPORT), 0 = none */
    uint32_t sms_key;               /* the channel that activated SMS reception (02 41), 0 = none yet */
    uint8_t sms_hold[MODEM_SMS_HELD][200];   /* incoming SMS kept until reception is on (not migrated) */
    uint8_t sms_hold_len[MODEM_SMS_HELD];
    uint32_t sms_held;
    SybModemTimer *sms_timer;
    uint8_t data_ch, data_port;     /* the active context's data channel (DATAPORT) and port, 0 = none */
    uint32_t data_tx, data_rx;      /* packets each way (log the first few) */
    uint32_t df_drops;
    SybCcpu ccpu;                   /* the P1i's CCPU (syborg_ccpu.c), on port 0 */
    ModemReply *replies;
    unsigned nreplies;
    bool link;                      /* ISI goes to a real modem (host->link_send), not to the reply tables */
    uint32_t link_sent, link_received, link_dropped;
    uint8_t link_subs[32];          /* resources last subscribed at the modem (PN_COMMGR 10/10), sorted */
    uint32_t link_subs_n;
    uint32_t link_seq;              /* link mode: requests sent (orders ModemChannel.link_seq) */
} SyborgModemState;

/* set-up: host and config first, then the host creates the timers (late[i].timer with sybmodem_late_fire
 * and &late[i], power_timer with sybmodem_power_fire, sms_timer with sybmodem_sms_fire, both with s) */
void sybmodem_init(SyborgModemState *s, const SybModemHost *host, void *ctx, const char *config);
/* the reply table text ("RR QQ PP LEN DATA..", hex; '#' comments), modified in place; returns the count */
int sybmodem_load_replies(SyborgModemState *s, char *text);
void sybmodem_reset(SyborgModemState *s);
int sybmodem_post_load(SyborgModemState *s);                    /* 0, or -1 for an impossible state */
/* a port's register window (offset within MODEM_WINDOW, little-endian accesses of 1..4 bytes) */
uint32_t sybmodem_read(SyborgModemState *s, unsigned port, uint32_t off, unsigned size);
void sybmodem_write(SyborgModemState *s, unsigned port, uint32_t off, uint32_t value, unsigned size);
/* timer callbacks */
void sybmodem_late_fire(void *late);
void sybmodem_power_fire(void *s);
void sybmodem_sms_fire(void *s);
/* packet data from the host network (Ethernet frames); receive returns 0 to have the frame kept and retried */
bool sybmodem_net_can_receive(SyborgModemState *s);
size_t sybmodem_net_receive(SyborgModemState *s, const uint8_t *buf, size_t size);
/* link mode (a real modem): turn it on before the guest runs; a message from the modem (PhoNet header + ISI) */
void sybmodem_link_enable(SyborgModemState *s);
void sybmodem_link_receive(SyborgModemState *s, const uint8_t *msg, size_t len);
/* link mode: the open channel object a modem message for channel number obj, resource res, goes to (0 = none) */
uint32_t sybmodem_link_route(SyborgModemState *s, uint8_t obj, uint8_t res);
/* an incoming SMS (SMS-DELIVER TPDU in hex): NULL, or why it was refused */
const char *sybmodem_sms(SyborgModemState *s, const char *hex);

#endif
