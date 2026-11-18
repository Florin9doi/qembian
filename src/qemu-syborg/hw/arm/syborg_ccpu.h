/*
 * Syborg fake CCPU (qembian): the Sony Ericsson P1i's modem CPU as its application CPU sees it through
 * BRIDGEDRV.LDD: the CAIF link layer and the BridgeServer object protocol (BServer.dll, OPA). Part of the
 * modem core (syborg_modem_core.c: TX_CAIF / RX_CAIF frames on port 0); the guest's bridgedrv stand-in only
 * forwards packets. Plain C: built into QEMU, the hosted EKA2 board and the host test tests/caif_peer_test.c.
 * Protocol and evidence: docs/devices/se-p1i/ccpu-link.md.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later OR EPL-1.0
 */
#ifndef SYBORG_CCPU_H
#define SYBORG_CCPU_H

#include <stdint.h>

#define CCPU_UNITS      8           /* bridgedrv units (caifmux uses 1 and 2) */
#define CCPU_CREATES    16          /* CreateInstance calls waiting for BServer's proxy (our type 4 -> its 5) */
#define CCPU_MAX_PKT    256         /* longest packet we send */
#define CCPU_MAX_MSG    2048        /* longest reassembled message from the app */
#define CCPU_BS_REPLY   24          /* BridgeServer reads Left(24) of every message */
#define CCPU_CALLBACKS  128         /* callback objects the apps registered with us (their type 4) */
#define CCPU_OBJECTS    1024        /* CCPU objects (ids 0x1000 + index); 64 overflowed on the P1i start-up (2026-11-17: object 4160+ lost its IID, semc.tsy got no outputs, TSY_MErr SM:1 -2) */

typedef struct SybCcpuCreate {
    uint32_t used, unit, link;      /* where the app's CreateInstance came from */
    uint32_t app_req;               /* its request id (word 2), echoed in our type-1 answer */
    uint32_t our_req;               /* the id of our type-4 request to BServer */
    uint32_t object;                /* the CCPU object the proxy stands for */
    uint32_t kind;                  /* 0: an app's CreateInstance; 1: a call whose output word 1 is an interface */
    uint32_t outlen;                /* kind 1: the call's output size */
} SybCcpuCreate;

typedef struct SybCcpu {
    uint32_t next_link[CCPU_UNITS]; /* channel id of the next LINK_SETUP answer, per unit (1, 2, ...) */
    uint32_t next_value;            /* our handle for the next interface the app registers (type 4 -> 5) */
    uint32_t next_req;              /* request ids of CCPU-initiated requests */
    uint32_t next_obj;              /* CCPU object ids */
    SybCcpuCreate create[CCPU_CREATES];
    /* an app's callback object (its type 4: ICB* interface); we call it with a type 8 addressed by `target` */
    struct { uint8_t iid[16]; uint32_t unit, link, session, target, value; } cb[CCPU_CALLBACKS];
    uint32_t ncb;
    uint16_t obj_cb[CCPU_OBJECTS];         /* 1 + the callback registration a call on the object subscribed */
    uint8_t obj_clsid[CCPU_OBJECTS][16];   /* what each object is (for the log and, later, its behaviour) */
    uint8_t obj_iid[CCPU_OBJECTS][16];
    /* a BridgeServer message split over several data packets (byte 1 = 1: more follows; the last has 0) */
    uint32_t frag_len[CCPU_UNITS];
    uint8_t frag_link[CCPU_UNITS];
    uint8_t frag[CCPU_UNITS][CCPU_MAX_MSG];
} SybCcpu;

/* how the CCPU talks back: a packet to bridgedrv unit `unit`, a log line (no newline) */
typedef struct SybCcpuOut {
    void (*send)(void *ctx, uint32_t unit, const uint8_t *pkt, uint32_t len);
    void (*log)(void *ctx, uint32_t unit, const char *text);
    void *ctx;
} SybCcpuOut;

void sybccpu_reset(SybCcpu *c);
/* one packet that caifmux wrote on bridgedrv unit `unit` */
void sybccpu_packet(SybCcpu *c, const SybCcpuOut *out, uint32_t unit, const uint8_t *p, uint32_t n);

#endif
