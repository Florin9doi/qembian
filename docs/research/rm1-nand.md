# Nokia 6630 (RM-1) NAND: what a QEMU model must do for mednand.pdd

Static reading of `mednand.pdd` (ROM VA 0x50060580, 59904 bytes, ARM code, a Samsung XSR/SSR
stack with a TI-controller low-level driver). `nandftl.fxt` (0x5006f0f0, 18192 bytes) is the file
system extension calling mednand through ControlIO ("[MD::FSEXT]" strings); it does not touch
hardware [I]. Marks: [V] read in the code, [I] inferred.

Peripheral VA = phys - 0xFFF00000 + 0x58000000, so the block is phys **0xFFFBCC00** (VA 0x580BCC00).

## Register access helpers (ekern exports) [V]

Import stubs in mednand -> ekern: `0x5006b828` read8, `0x5006b834` write8, `0x5006b840` modify8
(bic/orr), `0x5006b84c` modify32, `0x5006b858` write32, `0x5006b864` read16, `0x5006b870` read32,
`0x5006b87c` write16 [I by pattern], `0x5006b8dc`/`0x5006b8e8` read32/write32 (used on 0xFFFECE24).
So the block must accept 8-, 16- and 32-bit accesses; the data registers stream bytes little-endian.

## Register table (offsets from 0xFFFBCC00)

| off | width | role | model behaviour |
|-----|-------|------|-----------------|
| 0x04 | 8/16/32 | DATA: chip data port. Reads return next 1/2/4 bytes of the current page+spare stream (LE); writes push bytes to the page buffer. ReadID and status are read here too [V] | stream; feeds ECC accumulator |
| 0x08 | 32 | ADDRESS: `page<<9 | column(0..255)` for read/program [V]; for erase `(row & 0xff) | ((row >> 8) << 9)` with row = block<<5 [V] (i.e. the controller sends bytes cc08[7:0], cc08[16:9], cc08[24:17], ... as address cycles, bit 8 is the A8 half selected by cmd 00/01) [I] | latch |
| 0x10 | 32 | CONTROL: bits 8,10,12,14 = CE# of chip 0..3, active low (assert = clear bit, deassert = set) [V]; init sets bits 0 and 9 (0x201) [V; meaning I: enable / WP# high]; bit16 = write-posting FIFO mode, bit17 = read-prefetch FIFO mode [V] | plain RW register |
| 0x18 | 8/32 | FIFO status: bit2 = read FIFO holds 16 bytes (poll until 1, then write 4 = ack) [V]; bit3 = write FIFO has room for 16 bytes / is empty (poll until 1, write 8 = ack) [V]. Driver also does modify32(set 4 / set 8) before starting [V] | read: bit2|bit3 always 1; writes ignored |
| 0x1C | 8 | READY: bit0 = chip ready (R/B#); driver loops while bit0 == 0 [V] | always 1 |
| 0x20 | 8/32 | COMMAND + ADDRESS: writing a command byte here issues the command followed by the address cycles from cc08 [V/I]. Used with 0x00/0x01/0x50 (read), 0x80 (program setup), 0x90 (READ ID), 0x60 (erase, 32-bit write) [V] | execute command with address |
| 0x24 | 8/32 | COMMAND only (no address): 0x00/0x01/0x50 pointer before 0x80, 0x10 (program confirm), 0x70 (status), 0xFF (reset), 0xD0 (erase confirm, 32-bit write) [V] | execute command |
| 0x2C | 32 | ECC result of all data bytes moved through cc04/cc54 since the last ECC reset [V]; format see ECC [V for packing] | computed |
| 0x50 | 8 | ECC control: modify8(set bit0) = clear/restart the ECC accumulator [V] | bit0 write -> reset; reads 0 |
| 0x54 | 32 | FIFO data port (prefetch read / posted write), 4 words per 16-byte burst [V] | same stream as cc04 |
| 0x58 | 32 | FIFO config; written 0x10000020 (32 bursts of 16 bytes = 512, bit28 = enable [I]) [V] | plain RW |
| 0x5C, 0x8C, 0x90 | 8 | timing/config; saved before controller reset and restored after [V] | plain RW |
| 0x64 | 8 | controller control: bit1 = soft reset (then poll cc68) [V]; bit0 set after init [V] | plain RW, bit1 self-clears |
| 0x68 | 8 | controller status: bit0 = reset done [V] | always 1 |

No interrupt and no system DMA is used: everything is polled (cc1c, cc18, cc68) [V for all paths read].

## Command sequences [V]

- Init (0x500636c4): save cc5c/cc8c/cc90; cc64 |= 2, poll cc68 bit0; chip reset (CE, cc24=0xFF, wait cc1c, CE off); restore; cc64 |= 1; cc10 |= 0x201.
- READ ID (0x50063770): CE; cc08 = 0; cc20 = 0x90; maker = read8(cc04); device = read8(cc04); read16(cc04) (2 more bytes, ignored); CE off.
- Read, raw (0x500637dc): CE; cc08 = page<<9 | col; cc20 = 0x00 / 0x01 / 0x50 (area A / B / spare); wait cc1c; read N bytes via cc04 (32-bit words, then bytes); CE off. The bad-block check uses it with area 0x50, col 5, 1 byte.
- Read with ECC, prefetch (0x50063f44, the one used): CE; cc50 |= 1; cc08 = page<<9; cc20 = 0x00 (or 0x50 when only the spare is wanted); cc58 = 0x10000020; cc18 |= 4; cc10 |= 0x20000; wait cc1c; 32x { poll cc18 bit2; 4x read32(cc54); write8(cc18, 4) }; cc10 &= ~0x20000; ecc_main = cc2c; then spare via cc04: cc50 |= 1, read spare[0..2], ecc_spare = cc2c, read spare[3..15]. Compare ecc_main with spare[6..8], ecc_spare (2 bytes) with spare[9..10].
- Program with ECC, posting (0x50064274, the one used): CE; cc50 |= 1; cc08 = page<<9; cc24 = 0x00 (0x50 if spare only); cc20 = 0x80; cc58 = 0x10000020; cc18 |= 8; cc10 |= 0x10000; 32x { poll cc18 bit3; 4x write32(cc54); write8(cc18, 8) } + final poll; ecc = cc2c; cc10 &= ~0x10000; spare via cc04: cc50 |= 1, spare[0..2], ecc_spare = cc2c, spare[3..4] (16-bit), spare[5], ecc_main[0..2], ecc_spare[0..1] (16-bit), spare[11], spare[12..15] (32-bit). Without a spare buffer: 6x 0xFF + ecc_main[0..2] only. Then cc24 = 0x10; wait cc1c; status: cc24 = 0x70, read8(cc04), bit0 = fail -> -1.
- Raw program (0x500638f8): same without ECC/FIFO, data via cc04.
- Erase (0x5006454c): CE; cc08 = erase-format row; write32(cc20, 0x60); write32(cc24, 0xD0); wait cc1c; status bit0 -> -2.
- Status (0x50064784): cc24 = 0x70, read8(cc04) & 0xff. Return 0xC0 (ready, not protected, pass).
- Reset: cc24 = 0xFF.

## Chip ID and geometry

ID table in mednand's initialised data (ROM 0x5006ee24 -> RAM 0x800029f0), 32-byte records
`{maker, device, nBlocks, pagesPerBlock, pageSize, spareSize, reservedBlocks, ...}`; only the
device code is compared (0x50062110) [V]. All are Samsung (0xEC) small-page chips, 512+16 bytes,
32 pages per 16 KB block [V]:

| device | blocks | size | reserved |
|--------|--------|------|----------|
| 0x39 | 1024 (16 ppb) | 8 MB | 0x1a |
| 0x73 / 0x33 | 1024 | 16 MB | 0x1a |
| 0x35 / 0x45 / 0x55 / 0x75 | 2048 | 32 MB | 0x29 |
| 0x76 / 0x36 | 4096 | 64 MB | 0x4c |
| 0x79 | 8192 | 128 MB | 0x7e |

Pick **maker 0xEC, device 0x76** (K9F1208-like): 4096 blocks x 32 pages x (512+16) =
69,206,016-byte backing file. Address cycles on the real bus: 4 (col, A9-16, A17-24, A25) [I];
the model needs none of that, it decodes cc08 directly. ID bytes 3/4 can be 0xC0 0xFF [I].

## Hardware ECC [V for use, I for bit meaning]

The driver uses the hardware result (cc2c) for both write and read and never computes main-area
ECC in software (software code exists only for correction). Packing (0x50064820) [V]:
`ecc[0] = r & 0xff; ecc[1] = (r >> 16) & 0xff; ecc[2] = ((r >> 8) & 0x0f) | ((r >> 20) & 0xf0)`,
i.e. a 12-bit "odd" half in r[11:0] and a 12-bit "even" half in r[27:16] (TI GPMC/OMAP format).
Check (0x50064a24) [V]: x = stored ^ computed over 24 bits; popcount 0 -> OK, 1 -> ECC-area error
(-5), 12 -> single-bit correction (-4, corrected), else: if stored == FFFFFF and the data is all
0xFF -> OK (erased page), otherwise uncorrectable.

Model (self-consistent Hamming, gives 12 differing bits for one flipped bit): over the bytes since
the last reset, bit address a = byteIndex*8 + bit; for k = 0..11: if bit set, toggle
`odd[k]` when (a >> k) & 1 else `even[k]`; `r = odd | (even << 16)`. All-0xFF 512 bytes gives
r = 0 -> stored FF FF FF vs 00 00 00, popcount 24 -> erased-page branch -> OK. Same for the
3-byte spare ECC [I]. Which half real silicon calls "odd" only matters for correcting a real bit
flip, which the model never produces [I]. Anything pre-written into the image (ROFS) must have
spare[6..8] (and [9..10]) computed with this same function.

## Spare layout (16 bytes) [V for positions, I for names]

`[0..3] aRsv3 / FTL data, [4] iStatus, [5] iBadMark, [6..8] ECC main, [9..10] ECC of spare[0..2],
[11..15] aRsv4`. Bad block = spare byte 5 of page 0 or page 1 of the block != 0xFF (0x500645dc) [V].
A blank all-0xFF chip has no bad blocks.

## Things outside the NAND block that mednand needs

- **TOC in OCP SRAM** [V]: mednand maps phys **0x20000000** (16 KB, DPlatChunkHw) and reads a
  partition table at +0x100: up to 32 records of 32 bytes `{u32 start (byte offset in NAND),
  u32 size, ..., char name[12] at +20}`, looked up by name "SOS-USER" (C: disk) and "SOS-ROFS"
  (ROFS read straight from NAND through "DoNandFlashRead(ROFS read)") [V]. On the phone the
  bootloader leaves it there. If "SOS-USER" is missing it tries CopyTocToRam (NAND pages 0x100
  and 0x101 -> SRAM+0x100/+0x300) [I], then fails "User disk start address still not found -
  CRITICAL FAILURE". So **the board must pre-load SRAM 0x20000100 with a TOC**; the user disk runs
  from start/(512*32) to nBlocks - reservedBlocks (0x50060bc0) [V]. A blank chip itself is then
  formatted by SSR/FTL ("Repartition", fl_format/ssr_format) [I]; no factory signature on NAND
  besides the TOC was seen ("GBBM002" is compared with a parameter from xstore_get_dparameter,
  which is a local stub returning 1) [I].
- **0xFFFECE24** = OMAP1610 CLKM ARM_IDLECT3 [I]: read, `|= 0x10` while the TOC chunk is open,
  restored afterwards (clock enable for OCP/TC) [V for the access]. A plain RW register is enough.
- If ROFS is to come from NAND: put the ROFS image at the SOS-ROFS byte offset with correct ECC.

## Proposed QEMU device (sketch)

```c
#define NAND_BLOCKS 4096
#define NAND_PPB    32
#define NAND_PAGE   512
#define NAND_OOB    16
#define NAND_RAW    (NAND_PAGE + NAND_OOB)

typedef struct RM1NandState {
    SysBusDevice parent;
    MemoryRegion iomem;           /* 0xFFFBCC00, size 0x100 */
    BlockBackend *blk;            /* backing file, NAND_BLOCKS*NAND_PPB*NAND_RAW, 0xFF = blank */
    uint8_t *store;               /* or read/write pages through blk */
    uint32_t addr, ctrl, fifo_cfg, cfg5c, cfg8c, cfg90, ctl64;
    uint32_t row;                 /* current page */
    uint32_t col;                 /* 0..527 inside page+spare */
    enum { M_IDLE, M_READ, M_PROG, M_ID, M_STATUS } mode;
    uint8_t ptr_area;             /* 0x00 / 0x01 / 0x50 from last pointer command */
    uint8_t pbuf[NAND_RAW];       /* program buffer */
    uint8_t id[4]; int id_pos;
    uint16_t ecc_odd, ecc_even; uint32_t ecc_idx;
} RM1NandState;

static void ecc_reset(RM1NandState *s) { s->ecc_odd = s->ecc_even = 0; s->ecc_idx = 0; }
static void ecc_byte(RM1NandState *s, uint8_t b)
{
    for (int bit = 0; bit < 8; bit++) {
        uint32_t a = s->ecc_idx * 8 + bit;
        if (!((b >> bit) & 1)) continue;
        for (int k = 0; k < 12; k++)
            if ((a >> k) & 1) s->ecc_odd ^= 1u << k; else s->ecc_even ^= 1u << k;
    }
    s->ecc_idx++;
}

static uint32_t area_col(uint8_t area, uint32_t c)
{ return area == 0x50 ? NAND_PAGE + (c & 15) : area == 0x01 ? 256 + c : c; }

static uint8_t *page_ptr(RM1NandState *s, uint32_t row)
{ return s->store + (uint64_t)(row % (NAND_BLOCKS * NAND_PPB)) * NAND_RAW; }

static uint8_t data_in(RM1NandState *s)        /* one byte from chip */
{
    uint8_t v = 0xff;
    switch (s->mode) {
    case M_ID:     v = s->id_pos < 4 ? s->id[s->id_pos++] : 0xff; return v;
    case M_STATUS: return 0xC0;                  /* ready, not protected, pass */
    case M_READ:
        v = page_ptr(s, s->row)[s->col];
        if (++s->col == NAND_RAW) { s->col = 0; s->row++; }   /* sequential read */
        break;
    default: break;
    }
    ecc_byte(s, v);
    return v;
}
static void data_out(RM1NandState *s, uint8_t v)
{
    if (s->mode == M_PROG && s->col < NAND_RAW) s->pbuf[s->col++] &= v;
    ecc_byte(s, v);
}

static void command(RM1NandState *s, uint8_t cmd, bool with_addr)
{
    uint32_t a = s->addr;
    switch (cmd) {
    case 0x00: case 0x01: case 0x50:
        s->ptr_area = cmd;
        if (with_addr) {                          /* read */
            s->row = a >> 9; s->col = area_col(cmd, a & 0xff); s->mode = M_READ;
        }
        break;
    case 0x80:                                    /* program setup (pointer from cc24) */
        s->row = a >> 9; s->col = area_col(s->ptr_area, a & 0xff);
        memset(s->pbuf, 0xff, NAND_RAW); s->mode = M_PROG; s->ptr_area = 0x00;
        break;
    case 0x10: {                                  /* program confirm: NAND only clears bits */
        uint8_t *p = page_ptr(s, s->row);
        for (int i = 0; i < NAND_RAW; i++) p[i] &= s->pbuf[i];
        flush_page(s, s->row); s->mode = M_IDLE; break; }
    case 0x60:                                    /* erase setup: row in erase format */
        s->row = (a & 0xff) | ((a >> 9) << 8); break;
    case 0xD0: {                                  /* erase confirm */
        uint32_t first = s->row & ~(NAND_PPB - 1);
        memset(page_ptr(s, first), 0xff, NAND_PPB * NAND_RAW);
        flush_block(s, first); s->mode = M_IDLE; break; }
    case 0x70: s->mode = M_STATUS; break;
    case 0x90: s->mode = M_ID; s->id_pos = 0; break;   /* id = {0xEC, 0x76, 0xC0, 0xFF} */
    case 0xFF: s->mode = M_IDLE; s->ptr_area = 0; break;
    }
}

static uint64_t rm1_nand_read(void *opaque, hwaddr off, unsigned size)
{
    RM1NandState *s = opaque; uint64_t v = 0;
    switch (off) {
    case 0x04: case 0x54:
        for (unsigned i = 0; i < size; i++) v |= (uint64_t)data_in(s) << (8 * i);
        return v;
    case 0x08: return s->addr;
    case 0x10: return s->ctrl;
    case 0x18: return 0x0c;                       /* FIFO ready for read and write */
    case 0x1c: return 1;                          /* R/B# ready */
    case 0x2c: return s->ecc_odd | ((uint32_t)s->ecc_even << 16);
    case 0x58: return s->fifo_cfg;
    case 0x5c: return s->cfg5c;
    case 0x64: return s->ctl64;
    case 0x68: return 1;                          /* reset done */
    case 0x8c: return s->cfg8c;
    case 0x90: return s->cfg90;
    }
    return 0;
}

static void rm1_nand_write(void *opaque, hwaddr off, uint64_t v, unsigned size)
{
    RM1NandState *s = opaque;
    switch (off) {
    case 0x04: case 0x54:
        for (unsigned i = 0; i < size; i++) data_out(s, v >> (8 * i));
        break;
    case 0x08: s->addr = v; break;
    case 0x10: s->ctrl = v; break;                /* CE#/mode bits: no effect needed */
    case 0x18: break;                             /* FIFO acks */
    case 0x20: command(s, v & 0xff, true); break;
    case 0x24: command(s, v & 0xff, false); break;
    case 0x50: if (v & 1) ecc_reset(s); break;
    case 0x58: s->fifo_cfg = v; break;
    case 0x5c: s->cfg5c = v; break;
    case 0x64: s->ctl64 = v & ~2u; break;         /* soft reset completes at once */
    case 0x8c: s->cfg8c = v; break;
    case 0x90: s->cfg90 = v; break;
    }
}
/* MemoryRegionOps: .valid/.impl min 1 max 4, little endian. */
```

Board side: map this at 0xFFFBCC00; make 0xFFFECE24 a RW register (or let the OMAP clock model
keep it); pre-load OCP SRAM 0x20000100 with a TOC, e.g. record 0 = {start 0x00400000, size
0x03800000, name "SOS-USER"} and, if ROFS comes from NAND, a "SOS-ROFS" record [I values].
The driver's own error strings ("[MD::Repartition] ...", "[SSR:Err] ...") over the kernel debug
port are the quickest check that the model works.
