# Nokia 6630 (RM-1): the modem link (ssi.pdd + xbus.ldd over the SSI block)

Static reading of `eka1/rm1/core/ROM.bin` (VA 0x50000000): ssi.pdd 0x5014a500, xbus.ldd 0x50149590,
kernel helpers. Goal: just enough to model the modem's end of the link in QEMU. [V] = read in the code
(address given), [I] = inferred.

## Summary

- Transport: an SSI block (OMAP-style **SST** transmitter at 0x30000000, **SSR** receiver at 0x30000800,
  its own **GDD** DMA at 0x30002000) with 32-bit frames [V]. Channel 0 carries single **control words**
  by PIO; channel 3 carries **ISI payloads** by GDD DMA [V].
- Framing is **not** the 9500's byte XBUS (no CRC-8 4-byte header, no SYNC bytes). Everything is 32-bit
  words; the top nibble is the type: 0 hello, 1 link up, 7 data header, 8 ACK, C NAK [V].
- Start-up: phone raises AP-wake (GPIO4 bit 3), then waits for the SSR "Ctrl" interrupt (or a word 0);
  answers with word 0x00000000; the modem sends 0x10000000 and the link state goes to 0 (up) [V code
  path, I meaning].
- The ISI peer is unchanged: hand complete payloads to `ngage_isi_isa_send()` and take replies with
  `ngage_isi_isa_take()` (the 9500's path), with `isi-replies-rm1.txt`.

## 1. Registers (ssi.pdd; all 32-bit via kernel read32 0x5000c56c / write32 0x5000c574 unless noted)

Base VA 0x58101000 = PA 0x30000000.

| PA | role (by use) | what ssi.pdd does | model |
|---|---|---|---|
| SST +0x04 | MODE | init writes 2; 0x5014ab54 reads it after a 999-loop delay and rewrites 2 if it is not 2 [V] | plain R/W |
| SST +0x08 | FRAMESIZE | 31 (= 32-bit words) [V] | R/W |
| SST +0x0C | TXSTATE | 0 [V] | R/W |
| SST +0x10 | BUFSTATE | PIO send polls bit 0 (ch0 full) until clear before writing +0x80 (0x5014aae0) [V] | read 0 (instantly drained) |
| SST +0x18 | DIVISOR | 1 [V] | R/W |
| SST +0x20 | (enable) | 1 after the MODE check [V] | R/W |
| SST +0x24 | CHANNELS | 4 [V] | R/W |
| SST +0x28 | ARBMODE | 0 [V] | R/W |
| SST +0x80 | BUFFER ch0 | PIO TX of one control word [V] | word -> modem |
| SST +0x8C | BUFFER ch3 | GDD TX destination (kernel 0x5000a7d0: id 28 -> SST+0x8C, sync 7) [V] | DMA data -> modem |
| SSR +0x04/08/0C | MODE 2 / FRAMESIZE 31 / RXSTATE 0 [V] | | R/W |
| SSR +0x1C | Ctrl IRQ ack | Ctrl ISR writes 0; start writes 0 before enabling [V] | write -> lower Ctrl IRQ |
| SSR +0x20 | ERROR | Err ISR reads it [V] | read 0 |
| SSR +0x24 | ERRORACK | Err ISR and start write 3 [V] | WO |
| SSR +0x28 | CHANNELS | 4 [V] | R/W |
| SSR +0x34 | TIMEOUT | 0 [V] | R/W |
| SSR +0x80 | BUFFER ch0 | FullCh0 ISR reads one word and hands it to xbus (0x5014aba8); start reads it twice to drain [V] | pop RX word |
| SSR +0x8C | BUFFER ch3 | GDD RX source (kernel 0x5000ac6c pattern: id 25..30 -> SSR+0x80+4k, sync 2,4,6,8..) [V for 25-27, I for 28 = +0x8C, sync 8] | DMA data -> phone |
| 0x30001100 | ? | touched by the kernel early (not by ssi.pdd) [I] | RAZ/WI + log |

No ID or reset-done register is read [V].

### GDD (the SSI's DMA), kernel helpers 0x5000d248 (write16), d260 (modify16), d280 (read16)

Channel register base = 0x30002800 + 0x40 * (id - 16), id = the DMA object's channel number (16..23) [V
address arithmetic 0x5014ad48, 0x5000a83c]. 16-bit registers, OMAP-DMA-like [V offsets, I names]:

| off | name | written by |
|---|---|---|
| 0x00 | CSDP | kernel: 0x1320 \| 2 (32-bit elements) for TX [V], RX value 0x9024 \| ... [I] |
| 0x02 | CCR | PDD: TX 0x1000, RX 0x4000 (address post-increment); kernel ORs the sync number (low 5 bits) and sets bit 7 (enable) to start; PDD clears 0x80 to stop [V] |
| 0x04 | CICR | TX 0x20, RX 0x21 (block done; RX also timeout) [V] |
| 0x06 | CSR | RX done ISR 0x5014afa0 reads it: bit 0 set -> SsiPdd panic 22 [V]; return 0x20 |
| 0x08/0x0A | CSSA L/U | memory buffer for TX (kernel 0x5000b258), SSR+0x8C for RX [V/I] |
| 0x0C/0x0E | CDSA L/U | SST+0x8C for TX (0x5000a858) [V], memory for RX [I] |
| 0x10 | CEN | PDD: number of 32-bit words (0x5014ad8c TX, 0x5014af0c RX) [V] |
| 0x28 | CLNK_CTRL | 0 [V] |

Payload length is always rounded up to whole words (ceil(len/4)) [V 0x5014a888]; RX max 0x2000 bytes,
bigger panics the LDD (0x5014ac08) [V].

### Interrupts (bound by name; the kernel's table at 0x5000f288.. is indexed by interrupt number)

| name | kernel number | QEMU input | raise when | cleared by |
|---|---|---|---|---|
| Lv1_SSRFullCh0 | 29 (L1 29) | `ih[0]` 29 | a word waits in SSR ch0 | reading SSR+0x80 |
| Lv2_SSRCtrl | 122 (L2 90) | `ih[1]` 90 | modem starts the link (once) | write SSR+0x1C |
| Lv2_SSRErr | 121 (L2 89) | `ih[1]` 89 | never | write SSR+0x24 |
| Gpio1_Bit12 | 0xAC | GPIO1 line 12 (both edges) | modem wake line changes | GPIO model |
| Lv2_GDDCh0..7 | 127..134 (L2 95..102) | `ih[1]` 95+n | GDD channel n finished | read CSR [I] |

Numbers: table entries are 8 bytes, Lv2_Free127 sits right before Gpio1_Bit0 (= 0xA0, so 159) and
Lv2_FAC at 0x5000f330 is 32; Lv1_GpTimer1 comes out as 17 = the 6630 board's existing L1 17 [V
arithmetic]. GDD channel n <-> interrupt 127+n [I].

## 2. GPIO handshake

| line | dir | meaning | code |
|---|---|---|---|
| GPIO4 bit 3 (0xFFFBBC00, mask 8) | out | AP wake / ready: SET_DATAOUT (+0xF0) in PDD Start (0x5014a960) and again from xbus's timer when the link is up (0x501499dc); CLEAR_DATAOUT (+0xB0) only when idle and GPIO1_12 is low (0x50149c14 -> 0x5014aa00) [V]. Made an output by PDD +0x44 (0x5014a9a4) [V] |
| GPIO1 bit 12 (0xFFFBE400, mask 0x1000) | in | modem wake / active, active high [I]: ISR 0x5014b20c reads DATAIN: high -> LDD timer DFC (raises AP wake), low -> TX kick DFC [V]. PDD +0x4c returns !bit12; the phone keeps AP wake up while it is high [V] |

GPIO1_12 set-up (0x5014b158): pin mux 0xFFFE1048 bit 30 set, 0xFFFE10BC bit 30 clear; direction input,
edge mode 3 (both) [I], SYSCONFIG (+0x10) |= 4, +0xE8 (SET_WAKEUPENA) = 0x1000 [V]. PDD +0x48 (0x5014a9b4)
flips the two pin-mux bits back; xbus calls it when a "link up" word has bit 0 set [V].

Model: hold GPIO1_12 **high** from reset (modem always awake) [I]; the phone then never drops AP wake.

## 3. Framing on SSI vs the 9500

All control words go through SST/SSR channel 0 one at a time; the type is word >> 28 (dispatch
0x50149ef0). While the "Ctrl pending" flag (PDD Ctrl object byte +20, initially 1) is set, every word
except type 0 is ignored [V].

| word | sender | meaning | phone code |
|---|---|---|---|
| 0x00000000 | phone, modem | hello. Phone sends it from the first Ctrl ISR (0x5014b0b0). Received type 0: clears Ctrl pending, kicks TX [V] |
| 0x1000000b | modem | link up: LDD state (byte +100, initial -2) := 0 and ISA notified (0x501498c8); b = 1 also flips the pin mux (PDD +0x48) [V path, I meaning]. Send 0x10000000 |
| 0x73HHLLCC | both | data header: LLHH = payload length in 32-bit words (LE 16-bit), CC = 0x73 ^ HH ^ LL. Receiver checks CC == b1^b2^b3 (0x50149ff8) [V] |
| 0x83000000 | both | ACK of a header: the sender then DMAs the payload on channel 3 (phone: ACK -> DFC 0x104 -> 0x50149d10 sends the queued message with Send(ch 3)) [V]. Phone's own ACK is exactly 0x83000000 [V 0x5014a0a0] |
| 0xC3000000 | both | NAK (bad checksum / not ready) (phone 0x50149d9c). Received while the phone waits for an ACK: header re-sent [V path, I meaning] |

Phone -> modem message: header word on ch0 (PIO) -> wait for type 8 -> payload words on ch3 (DMA) -> TX
done ISR re-enables TX [V]. Modem -> phone: header on ch0 -> phone queues ACK 0x83000000 and **arms the
ch3 RX DMA for N words right away** (0x5014a0cc) -> DMA done -> message to ISA, ready for the next
header [V]. The ACK goes out on the next TX kick [I].

Differences from the 9500 XBUS (nokia9500_xbus.c): no 4-byte byte header, no CRC-8 (XOR checksum in the
word's low byte), no sequence bit, no SYNC exchange; length counts words, not bytes; payload padded to 4
bytes; payload is a separate DMA channel, not the same byte stream. Payload content: one ISI message as
isa_if_driver hands it, media byte first, as on the 9500 (same ISA_IF_PROTOCOL_BASE family) [I]. Byte
order inside words: memory order (the DMA copies bytes as stored) [V by construction].

## 4. Start-up exchange

1. ssi.pdd DoCreate (0x5014a6c8): binds the four interrupts, GPIO4_3 output [V].
2. Start (0x5014a8fc): SSR+0x24 = 3, enable Err; SSR+0x1C = 0, enable Ctrl; enable GPIO1_12; read SSR
   ch0; SSR init; **GPIO4_3 high**; SST init (MODE check); read SSR ch0; enable FullCh0 [V].
3. Phone waits. Modem: raise Lv2_SSRCtrl (or push word 0). [I: a real modem answers AP wake]
4. Phone Ctrl ISR: writes 0x00000000 to SST ch0, ack SSR+0x1C, Ctrl pending := 0 [V]. A second Ctrl IRQ
   sets link state -2 (down) [V]: raise it only once.
5. Modem sends 0x10000000 -> state 0 (up) [V path]. Then the ISA stack sends its ISI requests as
   header/ACK/DMA exchanges; replies come back the same way.

## 5. Proposed model (`hw/arm/nokia6630_ssi.c`, sketch)

Hooks needed on the board: `rm1_gpio_set_input(0, 12, true)` at reset; an output hook from the GPIO
model for module 4 bit 3 (`rm1_ssi_ap_wake(level)`); an address space for the GDD copies.

```c
/* Nokia 6630: SSI (SST 0x000, SSR 0x800, GDD 0x2000) + the modem's end of the xbus word protocol.
 * docs/research/rm1-ssi-xbus.md. SPDX-License-Identifier: GPL-2.0-or-later */
#include "qemu/osdep.h"
#include "qemu/log.h"
#include "exec/address-spaces.h"
#include "hw/irq.h"
#include "ngage_isi.h"

#define W_HELLO 0x00000000u
#define W_UP    0x10000000u
#define W_ACK   0x83000000u
#define W_NAK   0xC3000000u
#define SSR_CH3 0x3000088Cu
#define SST_CH3 0x3000008Cu

typedef struct {
    MemoryRegion mr;
    qemu_irq full0, ctrl, gdd[8];
    uint32_t sst[0x40], ssr[0x40];           /* plain register files */
    uint16_t gddr[8][0x20];
    uint32_t rxq[64]; unsigned rxh, rxn;     /* words for SSR ch0 */
    bool ctrl_sent, up;
    /* phone -> modem payload */
    uint32_t want_words;
    /* modem -> phone payload */
    uint8_t out[0x2000]; uint32_t out_len; bool out_wait_ack;
    QEMUTimer *poll;
} Rm1Ssi;

static void push(Rm1Ssi *s, uint32_t w)
{
    if (s->rxn < 64) s->rxq[(s->rxh + s->rxn++) % 64] = w;
    qemu_set_irq(s->full0, s->rxn > 0);
}

static void modem_send_next(Rm1Ssi *s)        /* one ISI reply as header + DMA payload */
{
    uint8_t m[0x2000];
    uint32_t n, words;
    if (!s->up || s->out_wait_ack || s->out_len || !(n = ngage_isi_isa_take(m, sizeof(m)))) return;
    words = (n + 3) / 4;
    memset(s->out, 0, words * 4);
    memcpy(s->out, m, n);
    s->out_len = words * 4;
    s->out_wait_ack = true;
    push(s, 0x73000000u | (words & 0xffff) << 8 | (0x73 ^ (words >> 8 & 0xff) ^ (words & 0xff)));
}

static void from_phone(Rm1Ssi *s, uint32_t w)  /* SST ch0 word */
{
    switch (w >> 28) {
    case 0x0: if (!s->up) { s->up = true; push(s, W_UP); } break;   /* hello */
    case 0x7:
        if ((w & 0xff) != ((w >> 8 ^ w >> 16 ^ w >> 24) & 0xff)) { push(s, W_NAK); break; }
        s->want_words = w >> 8 & 0xffff;
        push(s, W_ACK);                       /* phone now DMAs want_words to SST ch3 */
        break;
    case 0x8: s->out_wait_ack = false; break; /* phone armed its RX DMA already */
    case 0xC: /* NAK of our header: send it again */
        if (s->out_len) { s->out_len = 0; /* re-queue: keep a copy in real code */ }
        break;
    }
}

static void gdd_run(Rm1Ssi *s, int ch)        /* CCR bit 7 set */
{
    uint16_t *r = s->gddr[ch];
    uint32_t src = r[4] | r[5] << 16, dst = r[6] | r[7] << 16, bytes = r[8] * 4u;
    uint8_t buf[0x2000];
    if (bytes > sizeof(buf)) bytes = sizeof(buf);
    if (dst == SST_CH3) {                     /* phone -> modem payload */
        address_space_read(&address_space_memory, src, MEMTXATTRS_UNSPECIFIED, buf, bytes);
        uint32_t len = bytes >= 6 ? 6 + (buf[4] << 8 | buf[5]) : bytes;   /* ISI length [I] */
        ngage_isi_isa_send(buf, len < bytes ? len : bytes);
    } else if (src == SSR_CH3) {              /* modem -> phone payload */
        if (bytes > s->out_len) bytes = s->out_len;   /* armed before our data exists: wait [I] */
        address_space_write(&address_space_memory, dst, MEMTXATTRS_UNSPECIFIED, s->out, bytes);
        s->out_len = 0;
    }
    r[1] &= ~0x80;
    r[3] = 0x20;                              /* CSR: block done (bit 0 = timeout panics the PDD) */
    qemu_irq_raise(s->gdd[ch]);
}

static uint64_t ssi_read(void *o, hwaddr a, unsigned sz)
{
    Rm1Ssi *s = o;
    if (a >= 0x2800 && a < 0x2a00) {          /* GDD channel regs */
        int ch = (a - 0x2800) / 0x40, r = (a & 0x3f) / 2;
        uint16_t v = s->gddr[ch][r];
        if (r == 3) { s->gddr[ch][3] = 0; qemu_irq_lower(s->gdd[ch]); }
        return v;
    }
    if (a == 0x010) return 0;                 /* SST BUFSTATE: never full */
    if (a == 0x820) return 0;                 /* SSR ERROR */
    if (a == 0x880) {                         /* SSR ch0 */
        uint32_t w = 0;
        if (s->rxn) { w = s->rxq[s->rxh]; s->rxh = (s->rxh + 1) % 64; s->rxn--; }
        qemu_set_irq(s->full0, s->rxn > 0);
        return w;
    }
    if (a < 0x100) return s->sst[a / 4];
    if (a >= 0x800 && a < 0x900) return s->ssr[(a - 0x800) / 4];
    qemu_log_mask(LOG_UNIMP, "rm1-ssi: R %05x\n", (unsigned)a);
    return 0;
}

static void ssi_write(void *o, hwaddr a, uint64_t v, unsigned sz)
{
    Rm1Ssi *s = o;
    if (a >= 0x2800 && a < 0x2a00) {
        int ch = (a - 0x2800) / 0x40, r = (a & 0x3f) / 2;
        s->gddr[ch][r] = v;
        if (r == 1 && (v & 0x80)) gdd_run(s, ch);
        return;
    }
    if (a == 0x080) { from_phone(s, v); return; }
    if (a == 0x81c) { qemu_irq_lower(s->ctrl); return; }
    if (a < 0x100) { s->sst[a / 4] = v; return; }
    if (a >= 0x800 && a < 0x900) { s->ssr[(a - 0x800) / 4] = v; return; }
    qemu_log_mask(LOG_UNIMP, "rm1-ssi: W %05x %08x\n", (unsigned)a, (uint32_t)v);
}

void rm1_ssi_ap_wake(void *o, bool level)     /* GPIO4 bit 3 output hook */
{
    Rm1Ssi *s = o;
    if (level && !s->ctrl_sent) { s->ctrl_sent = true; qemu_irq_raise(s->ctrl); }
}

static void poll_cb(void *o)                  /* every ~1 ms: next modem message */
{
    Rm1Ssi *s = o;
    modem_send_next(s);
    timer_mod(s->poll, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + 1000000);
}
/* init: memory_region_init_io(.., 0x4000) at 0x30000000; full0 = ih[0] in 29; ctrl = ih[1] in 90;
 * gdd[n] = ih[1] in 95+n; rm1_gpio_set_input(0, 12, true); start poll timer. */
```

Points to check on the first run (catches/logs, not more reading):
- which GDD channel ids the two DMA objects get (log CCR enables) and that CSSA/CDSA hold SST/SSR+0x8C;
- the RX DMA is armed before the modem payload exists only if the model sends the header first: keep
  `out[]` filled before pushing the header (the sketch does);
- whether ISA wants the exact ISI length on RX (the phone hands it words*4 bytes) [I];
- the LDD state values (-2 down, 0 up) via a strace of the ISA client once 0x10000000 is sent [I].
