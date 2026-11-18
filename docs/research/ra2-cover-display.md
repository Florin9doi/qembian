# Nokia 9500 (RA-2): cover display and cover switch, for mocking

Static reading of `eka1/ra2/core/ROM.bin` (VA = 0x50000000 + offset). Literal 0x580xxxxx in kernel
code = physical 0xFFFxxxxx. [V] = read in the code at the given address, [I] = inferred.
Scope: just enough to model the panel and the switch on our `nokia9500` board. Not covered: the inner
640x200 panel (VideoDriver.dll) and the XBUS link.

## 1. Cover panel (Cuilcd.ldd + Cuilcdpdd.pdd "CuiLcd.Lilly2")

### Microwire path

- The uWire registers are at **phys 0xFFFB3000** (virt 0x580B3000). EKern exports the accessors
  (ord 418 modify, 419/420 SR3 clock, 421 write, 422 read at 0x5000fe60..0x5000ff1c). Their base is
  built as `0x58000000 + 0xB3000`. The CSR busy bit (bit 14) is polled before each write. [V]
  QEMU's existing `omap_uwire` model covers this, so no new MMIO is needed. [I]
- `EuWire.dll` (0x50040320, `THelenuWire`) is the controller. It has no exports. It stores itself in
  a kernel global (0x80000b48), and the PDD fetches it through EKern ord 1 (0x5000ff70). The PDD
  panics "Microwire controller has not been loaded/initialised" if the pointer is NULL. [V]
- Channel object vtable at 0x50041ee4: +28 `Reserve(ch)` (0x500407d0), +32 `Release(ch)`, +36
  `SetConfig(ch, cfg*)` (0x50040860, copies 36 bytes), +8 transfer (0x500409f0). [V]
- The PDD reserves **channel 0** and configures it at 0x50b40e34. If the reserve fails it panics
  "Microwire channel not available". The config is {1,0,0,1,1,0,9,1,0}. [V]
- Channel to chip select: channels 0-1 program SR1 (0x08), channels 2-3 program SR2 (0x0C). An even
  channel uses the low 6-bit field and an odd channel the high one (0x500408b0..0x5000097c). So
  **channel 0 = CS0**. [V for the register selection; I for the "ch == CS" reading]
  The "CS:3 TX 0x0004" seen at boot is therefore another uWire client, not the cover. [I]
- Word size is **9 bits**: config word 6 = 9. [V that it is 9; I that it is the bit count]
  Every queued halfword is `((dc << 8) | byte) << 7`, which is the TDR left-aligned form. Bit 8 is
  D/C: 0 = command, 1 = parameter or pixel data (e.g. `(x|0x100)<<23>>16` at 0x50b3fee8). [V]
  QEMU's uWire hands the slave `txbuf >> (16 - nbits)`, so the slave sees `0x0cc` for a command and
  `0x1dd` for data. [I: check omap_uwire in omap1.c]
- EuWire reserves a DMA channel ("Uwire DLL failed to reserve a DMA channel"). The bulk pixel
  stream may therefore go through OMAP DMA and the uWire DRQ. If our uWire model has no DRQ, expect
  a stall there. [I]

### Command set (MIPI-DCS / Philips PCF8833 style)

The word stored in memory is `[b0, b1]`, the halfword is `b0 | b1<<8`, and the 9-bit word is the
halfword shifted right by 7. All decoded tables:

| Use | 9-bit words sent | Where |
|---|---|---|
| reset pin | GPIO12 low, wait 40 ms; GPIO12 high, wait 200 ms (queue ops 1/2) | 0x50b40534, 0x50b410a4 [V] |
| init 1 | NOP 0x00, SWRESET 0x01, wait 120 ms | obj+0x48c [V] |
| init 2 | NOP, SLPOUT 0x11, wait 120 ms | obj+0x490 [V] |
| init 3 | NORON 0x13, INVOFF 0x20, MADCTL 0x36 + 0x00, GAMSET 0x26 + 0x04 | obj+0x494 [V] |
| colour mode | COLMOD 0x3A + {0x02 EColor256, 0x03 EColor4K, 0x05 EColor64K}, then for 256/4K RGBSET 0x2D + 48 data bytes | 0x50b4081c [V] |
| on/off | DISPON 0x29 / DISPOFF 0x28 (halfwords 0x1480/0x1400) | 0x50b40670 [V] |
| idle/partial on | IDMON 0x39, PTLAR 0x30 + (y0+1, y0+h), PTLON 0x12 | 0x50b406f0, 0x50b40b9c [V] |
| idle/partial off | NORON 0x13, IDMOFF 0x38, DISPON/DISPOFF | 0x50b40764 [V] |
| sleep | NOP, SLPIN 0x10, wait 50 ms, GPIO12 low, wait 200 ms | 0x50b40610 [V] |
| wake (re-init) | colour mode, SLPOUT, wait 5 ms, 0xC3 + COLMOD param (vendor cmd 0xC3, 1 data), SETCON 0x25 + 0x3F, INVOFF, NORON, IDMOFF, GAMSET 4, MADCTL 0, DISPON/OFF | 0x50b40c58 [V for the bytes; I for the meaning of 0xC3] |
| contrast | SETCON 0x25 + 1 byte (initial 0x3F) | 0x50b40d48 [V for the bytes; I for the name] |
| blit | PASET 0x2B + (y+1, y+h); CASET 0x2A + (x+1, x+w); RAMWR 0x2C + pixel data | 0x50b3fe78 [V] |

- RAM offset: the visible area starts at controller row/column **1**, not 0. Model a 130x130 RAM
  (or 132x132, like the PCF8833) and show rows and columns 1..128. [V for the +1; I for the RAM size]
- Pixel data:
  - EColor4K (default, mode 10 at obj+136): source XRGB4444 halfwords packed to **12 bpp, 2 pixels
    in 3 bytes** (R1G1, B1R2, G2B2). An odd pixel count is padded with one more byte. (0x50b3f8f8)
    [V for the packing; I for the byte order]
  - EColor64K: 16 bpp, 2 bytes per pixel (0x50b3fa7c). [I]
  - EColor256: 8 bpp through a 256-entry LUT at obj+0x8c (0x50b3f7e8). [I]
- RGBSET tables (48 bytes):
  - 256 colours (0x50b42408): R 8 entries, G 8, B 4, zero padded. [V]
  - 4K (0x50b42438): R 16 entries 0..0x1F, G 16 entries 0..0x3F, B 16 entries 0..0x1F. [V]
  - The model can ignore RGBSET and expand 4 bits to 8 directly. [I]
- Panel size: **128x128**. Evidence: PDD vtable fn 0x50b40514 returns 0x4000 = 128*128 [V];
  CoverUi has a `Qgn_indi_wug_128x128.gif` [V]. Treat the exact size as [I].
- Read-back: the PDD never reads anything back. There are no ID or status commands and the RX bit
  count is 0 in its config. A slave that returns 0 is fine. [I: no RDDID 0x04 / RDDST 0x09 found]

## 2. Cover switch

- The switch is **not a GPIO read by a display driver**. It comes from the keyboard controller
  ("DErinKeyboard", `EKeyb.dll` 0x5009e430, strings "KEYBOARD I2C/INT/POWER"). [V]
- On each keyboard interrupt, EKeyb issues an **I2C 1-byte read from address 0x56**: request at
  0x5009e720, `strh #0x56` and len 1. [V] Whether 0x56 is a 7-bit or an 8-bit address is [I].
- Byte value meaning (0x5009e7b4, jump table at 0x5009ec2c):
  - `< 0xE0`: key code, handled at 0x5009e7f8. [V]
  - **0xE0**: lid open. Sets `this+88 = 1`, then `Kern::AddEvent(TRawEvent(18 = ECaseOpen))`. [V]
  - **0xE1**: lid closed. Sets `this+88 = 0`, then `TRawEvent 19 = ECaseClose`. [V]
  - 0xF0-0xF3: other controller events. [I]
  - The type numbers 18/19 = ECaseOpen/ECaseClose follow the 7.0s e32 TRawEvent::TType enum. [I]
  After either event, EKeyb also calls a registered observer (`0x5009fa88` object, vtable call with
  the state). [V for the call; I for who the observer is, probably the power/variant side that
  switches the UIs]
- An I2C error of -33 (timed out) makes EKeyb reset the controller: **GPIO14** (phys 0xFFFCE000
  bit 14) is pulsed via DATA_OUTPUT (+4) and DIRECTION (+8) at 0x5009e620. [V]
- Keyboard interrupt line: EKeyb sets **MPUIO bit 2** in IO_CNTL (0xFFFB5008, input) and
  GPIO_INT_EDGE (0xFFFB501C) at 0x5009f634, then binds interrupt 68. [V for the registers; I that
  MPUIO2 is the keyboard IRQ, active edge rising]
- What switches the UIs: window server raw events ECaseOpen/ECaseClose reach the inner (S80) side.
  CuiWs.exe and Coverui.dll run the cover UI. Whether the inner panel is powered down by VideoDriver
  on ECaseClose, or by user side, was not traced (budget). [I]
- `EFlip` belongs to the camera (CamPdd.pdd), not to the lid. [V]

## 3. Board sketch (QEMU, nokia9500.c)

```c
/* Cover LCD: 9-bit DCS slave on uWire CS0. */
typedef struct {
    uint8_t cmd, nparam; int xs, xe, ys, ye, x, y;
    uint8_t colmod;            /* 2/3/5 */
    uint8_t acc[3]; int nacc;  /* pixel byte assembly */
    bool on, sleep;
    uint32_t ram[132 * 132];   /* RGB888 */
    QemuConsole *con; bool dirty;
} CoverLcd;

static void cover_put(CoverLcd *s, uint32_t rgb) {
    s->ram[s->y * 132 + s->x] = rgb; s->dirty = true;
    if (++s->x > s->xe) { s->x = s->xs; if (++s->y > s->ye) s->y = s->ys; }
}
static void cover_send(void *opaque, uint16_t w) {   /* w = 9-bit word */
    CoverLcd *s = opaque; uint8_t b = w & 0xff;
    if (!(w & 0x100)) {                               /* command */
        s->cmd = b; s->nparam = 0; s->nacc = 0;
        switch (b) {
        case 0x01: /* SWRESET */ s->on = false; s->colmod = 3; break;
        case 0x10: s->sleep = true; break;   case 0x11: s->sleep = false; break;
        case 0x28: s->on = false; s->dirty = true; break;
        case 0x29: s->on = true;  s->dirty = true; break;
        case 0x2C: s->x = s->xs; s->y = s->ys; break;
        }
        return;
    }
    switch (s->cmd) {                                  /* parameters / data */
    case 0x2A: if (s->nparam++ == 0) s->xs = b; else s->xe = b; break;
    case 0x2B: if (s->nparam++ == 0) s->ys = b; else s->ye = b; break;
    case 0x3A: s->colmod = b & 7; break;
    case 0x2C:
        s->acc[s->nacc++] = b;
        if (s->colmod == 3 && s->nacc == 3) {          /* 2 px in 3 bytes */
            cover_put(s, rgb444(s->acc[0] << 4 | s->acc[1] >> 4));
            cover_put(s, rgb444((s->acc[1] & 15) << 8 | s->acc[2])); s->nacc = 0;
        } else if (s->colmod == 5 && s->nacc == 2) {
            cover_put(s, rgb565(s->acc[0] << 8 | s->acc[1])); s->nacc = 0;
        } else if (s->colmod == 2) { cover_put(s, rgb332(b)); s->nacc = 0; }
        break;
    /* 0x25 SETCON, 0x26, 0x2D RGBSET, 0x30, 0x36, 0xC3: accept and ignore */
    }
}
static uint16_t cover_receive(void *opaque) { return 0; }

/* display: gfx console 128x128, copies ram rows/cols 1..128, black when !on || sleep */
static void cover_update(void *opaque);
static const GraphicHwOps cover_ops = { .gfx_update = cover_update };

/* in machine init */
s->cover.con = graphic_console_init(NULL, 0, &cover_ops, &s->cover);
qemu_console_resize(s->cover.con, 128, 128);
omap_uwire_attach(mpu->microwire,
    &(uWireSlave){ cover_receive, cover_send, &s->cover }, 0 /* CS0 */);
/* GPIO12 (0xFFFCE000) = LCD reset: on falling edge, reset CoverLcd (qdev_connect_gpio_out on the
   omap_gpio output 12, or the omap310 GPIO out line). */

/* Lid: I2C slave 0x56 on the OMAP I2C bus (keyboard controller "Erin"). Keep a byte FIFO. On
   cover-open set, push 0xE0 (open) or 0xE1 (closed) and pulse MPUIO input 2 (rising edge, then
   low). An I2C read returns the next byte. Expose it as a QOM bool:
       object_property_add_bool(OBJECT(machine), "cover-open", get, set);
   used as  qom-set /machine cover-open true */
```

Notes for the implementer:
- The real key path uses the same I2C slave. The board probably already has a key stand-in there.
  The lid bytes must share its FIFO; don't add a second controller. [I]
- At boot, push the current lid state once after EKeyb binds. Otherwise `this+88` stays at its
  constructor value. [I]
- If EuWire drives the uWire with DMA, `omap_uwire` needs its TX DRQ wired to the OMAP DMA (check
  that first if RAMWR data never arrives). [I]
