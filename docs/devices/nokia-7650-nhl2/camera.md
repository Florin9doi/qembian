# Nokia 7650 camera (test pattern)

The phone's own camera stack runs unchanged on `-M nokia7650`. The board models the hardware it drives and
feeds a generated test pattern; there is no host-camera passthrough. Model: `src/qemu-ngage/hw/arm/ngage_cam.c`
(QEMU-free core) with glue in `ngage.c`, `ngage_mmc.c` (DMA) and `ngage_keys.c` (pins, slide). Hacks:
H-nhl2-camera, H-nhl2-slide (docs/hacks.md). Test: `tests/ngage_cam_test.c`.

Marks: **[V]** read in the firmware or seen in a run, **[I]** inferred.

## Result (2026-11-26)

- Home screen, left softkey Camera: the viewfinder shows colour bars, a red square top-left (orientation) and a
  grey ramp with a marker that moves every frame (20 frames/s) [V run].
- Select takes a picture: a 640x480 still, shown converted, saved as a JPEG under Images ("Image",
  thumbnail and full view correct) [V run].
- Slide closed (F7, the window's Slide button, `qom-set /machine slide-closed true`): SysAp locks the keypad and
  the camera shows "Camera on stand-by: Open cover"; opening it unlocks and resumes the viewfinder [V run].

```sh
python3 eka1/tools/ngage_phone.py --model nokia7650          # Camera = left softkey on the home screen
cc -std=gnu11 -Wall -o build/ngage_cam_test tests/ngage_cam_test.c \
   src/qemu-ngage/hw/arm/ngage_cam.c && build/ngage_cam_test
```

## Software stack

| Module | Role |
|---|---|
| Camera.app (Thumb) | UI; reads System Agent state 0x100052DD (1 = cover closed) before it starts the camera [V] |
| CameraServer.exe/.dll | session server; loads `cameraldd` / `camerapdd.pdd` [V strings] |
| Csu.dll | camera service utility: viewfinder/capture calls (Camera.app imports 2, 4, 5, 7); converts a still's YUV to 12-bit (preview) and 24-bit (JPEG) [V 0x50a606cc, 0x50a6097c] |
| cameraldd.ldd | logical channel (2 KB) |
| camerapdd.pdd (0x503d5fc4-0x503d8af4) | the hardware driver: everything below [V static RE] |
| SysAp.app | sets 0x100052DD from the slide's key events [V: the only writer besides Phone] |
| ekeyb.dll | reads the slide on 0x0C15020C bit 9 (0x50367370: key-state bit 24 when the line is low) [V] |

## camerapdd's hardware

All addresses are physical (the ASIC is at virtual 0x58000000 = physical 0x0C000000; the PDD goes through
ekern's accessors #234 modify16, #315 read16, #396 write16, #397 write32, #316 read32) [V].

### Sensor I2C (bit-banged)

| What | Value |
|---|---|
| lines | 0x0C15020C input: bit 7 SCL, bit 8 SDA; 0x0C150210 bits 7/8 = pull that line low (open drain) [V 0x503d6b98-0x503d6c04] |
| address | 0x78 write / 0x79 read (7-bit 0x3C) [V] |
| read | START 0x78 reg STOP, START 0x79, n bytes (ACK each, NACK the last), STOP [V 0x503d666c] |
| write | START 0x78 reg value STOP; the PDD caches all 128 registers and writes only changed ones [V 0x503d762c, 0x503d7968] |
| clock stretching | the PDD waits up to 10 x 1 us for SCL high (0x503d6b20); the model never stretches |
| identification | at power-up the PDD reads 0..0x7F. Table 0x503d8738 (2 entries) -> "type 2"; table 0x503d8740 (16 entries) -> "type 1"; entry = reg \| shift << 8 \| width << 11 \| value << 16 \| variant << 24. Type 1: reg 1 = 0x01, 0x11 .. 0xF1 -> variant 1, 1, 2, 3 .. [V] |
| model | reg 1 = 0x01 (type 1, variant 1); all else 0; writes stored. Variant 0 would drop the first 12 viewfinder frames after each start, variants 1-3 one [V 0x503d7f28] |

Sensor settings by mode (table 0x503d8780, 17 entries of 0x14 bytes: picture-mode mask, phase mask (bit 0
power-up, bit 1 still, bit 2 viewfinder: `(byte4 >> phase) & 1`, phase = PDD +0x148), variant mask, ASIC
writes, sensor patches, frame size) [V]: register 3 selects size and output (VGA 0x96000 bytes, QCIF 0xC600, viewfinder 0x9600; bit 6 set
for the viewfinder), 0x10/0x14/0x55/0x56 differ per phase. The model ignores them.

### CCP receiver (0x0C121000)

| Offset | Use |
|---|---|
| +00..+28 | set once at power-up: +00 = 0x1E0, +04 = 0x20, +0C/+14/+1C/+24 = the CCP2 sync codes FF000002 (frame start), FF000003 (frame end), FF000000 (line start), FF000001 (line end), +28 = 0x3FF [V mode entry 0] |
| +2C | receive FIFO; the DMA engine reads it as 0x1012102C [V run] |
| +30 | status: bit 3 frame end; 0x340 and 0x400 = a bad frame [V ISR 0x503d7f28]. Read once per interrupt, never written: read-to-clear [I] |
| +34 | interrupt mask, 1 = masked: 0x3FF at set-up, bit 3 cleared while running, 0x340 set during the ISR and cleared after [V usage; mask polarity I] |
| +38 | bit 0 = receiver on (1 at power-up, 0 at power-down) [V] |
| +3C | bit 0 pulsed = restart at the next frame (0x503d7ca8); bits 1-2 = 3 in the viewfinder mode, 0 for a still [V mode entries 5/6] |

Interrupt: `IrqCCPStatus`, interrupt name index 95 -> controller source 86 [V ekern table 0x5030a9f4, slots 8
bytes apart from IrqTimer0 = 0]. Also GPIO 0x0C120C0C..14 bit 10 and 0x0C14F018 bit 4 (power or clock for the
module), CBus +0x0C (0x0C15042C) 0x800 or 8 by sensor type, and a variant call `(6, 0x2E, 7, 0, clear, set)` that
must return 0 [V calls; meaning I]; the catch-all and existing models answer them.

### Frames and DMA

- Buffer: the variant's hardware chunk at SDRAM physical **0x0A000000**, 0x96000 bytes (ecust 0x5032fcd0, through
  Kern::Variant()->vtable+0xE8) [V]. On our board this RAM holds only the bootstrap's map table from boot time.
- The PDD arms a kernel DMA channel (#313: source 0x1012102C, destination the buffer's physical address, count
  = bytes / 4 - 1, control 2 = device-paced) [V run: channel 1, count 0x257F for the viewfinder].
- The ISR (0x503d7f28): read +30, stop the DMA (#37), check the DMA's remaining count (+2C) reads -1 (a whole frame),
  then account the frame and re-arm the DMA for the next buffer (viewfinder: two buffers, 0x0A000000 / 0x0A009600)
  and pulse +3C. Up to 19 bad frames in a row are retried, then KErrOverflow ("Temporary overload") [V].
- A 70 ms watchdog timer restarts the receiver when no frame ended within two ticks [V 0x503d7d7c].

### Frame formats

| Mode | Size | Format |
|---|---|---|
| viewfinder (+3C bits 1-2 = 3) | 160x120, 0x9600 bytes | one little-endian word 0x0RGB (4444) per pixel; the app draws it as it is [V run: YUV data showed white as cyan] |
| still (+3C = 0) | 640x480 (0x96000) or 176x144 (0xC600) | YUV 4:2:2, per pixel pair the bytes Y1 Cr Y0 Cb; Csu.dll: R = Y + 2(Cr - 128), B = Y + 2(Cb - 128), G = Y - (7(Cb - 128) + 15(Cr - 128)) / 16 [V 0x50a606cc] |

Which part converts the viewfinder to RGB (the sensor, reg 3 bit 6, or the receiver, +3C) is not known [I]; the
model keys on +3C. The pattern's YUV is the exact inverse of Csu.dll's converter (Y = (15R + 32G + 7B) / 54).

## Model

`ngage_cam.c`: the I2C slave (edge-driven state machine on the two lines), the CCP registers, the pattern.
`ngage.c`: the CCP region, a 50 ms frame timer while +38 bit 0 is set: if a DMA transfer from the FIFO is
armed, the pattern is written to its destination and the channel completes (`ngage_dma_finish`: count -1, idle);
then the frame-end event, interrupt 86 unless masked. `ngage_mmc.c`: the DMA engine's peer hook (a transfer that
does not touch the MMC FIFO is offered to the camera). The DMA channel's own interrupt is not raised for it [I].

## Nokia 6600 (2026-11-26)

`-M nokia6600` has the model too (`NgageModel.camera`; no slide): its campdd.pdd drives the same hardware (sensor
0x3C, 8-bit registers, reads 0..0x9F; CCP block, interrupt 86, buffer 0x0A000000, 0x97000 bytes) [V]. It is not
usable as shipped: **BTTestServer opens the camera channel (camdrv) at boot** for SelfTestServer's test session
and keeps it, so CamServerCore's open fails -11 (it reports -19 and panics "CamServerCore panic" 4) and Camera stays
blank [V run].
- Ending the session works like the N-Gage's H-ngage-selftest-stop: `reply+ 59 01 01 08 00 media=00 robj=38` in
  `isi-replies-nhl10.txt` (after DosServer's own 59 01) sends PN_EPOC_TEST msg 1 sub-id 8 to SelfTestServer (object
  0x38 = isa_if channel 1, table 0x5040f788; dispatcher 0x503586e8, table 0x50358740: 8 -> 0x50357dd0 ->
  CActiveScheduler::Stop). SelfTestServer and BTTestServer exit, the camera works: viewfinder in RGB444, VGA capture
  in YUV, JPEG saved under Images [V run].
- Not enabled: with it the Menu key does nothing until about 120 s after boot (60 s without it): SysAp waits in
  DosServer function 404 (DSClient 0x50a1aba0, argument 1); the Bluetooth stack starts after BTTestServer frees btldd
  and looks normal; cause not found. The owner chose the old behaviour (2026-11-26).

## Nokia 3650 and 3660 (2026-11-26)

`-M nokia3650` / `-M nokia3660` (`NgageModel.camera`, `cam_nhl8`; no slide). Viewfinder, capture, the saved JPEG in
Images: all [V run]; `python3 tests/ngage-boot-check.py --model nokia3650 --camera` (and 3660). Their camerapdd
drives the same receiver, interrupt 86, DMA source 0x1012102C and buffer 0x0A000000 (0x97000 bytes) [V], with
another sensor:

| What | 3650 / 3660 |
|---|---|
| I2C | address 0x08 / 0x09 (7-bit 0x04), register number 16 bits high byte first, 8-bit data (0x5040c780) [V] |
| identification | 3650: none (13 registers read and cached); 3660: register 0xC007 = 0x36 selects sensor variant 2 (0x503ea1c4), else variant 1 [V]; the model answers 0 (variant 1) |
| mode table | 3650 0x5040ee24 (20 entries), 3660 0x503ea350 (24): picture-mode mask, state mask, variant mask, receiver writes, sensor writes (16-bit register, value), frame size. Mode 2 uploads 8051 code to 0x8165-0x81A1 [V]; the model stores every write |
| frame layout | w x (h + 2) x 2: a header line, the picture, a spare line; the driver hands out buffer + 0x140 (0x5040db2c) [V] |
| viewfinder, 3650 (12-bit screen) | state 5: CCP control 6, sensor 0xC005 = 2: RGB 4444, 160x122 [V run] |
| viewfinder, 3660 (16-bit screen) | state 6: control 0, 0xC005 = 7: RGB565 little-endian, copied into the bitmap as it is (Csu.dll 0x50aa0c88, display mode 7) [V; byte order V run] |
| still | states 7-9 (quality in 0xC20C): 0xC102 = 6, the sensor's own JPEG in 640x482 x 2 bytes, two bytes of the file per 32-bit word at byte offsets 3 and 2; 0x5040e87c checks FF D8, compacts the bytes and finds FF D9 [V] |
| preview after a still | state 10: 0xC102 = 4, raw YUV 160x122, bytes Cr Y1 Cb Y0 with Csu.dll's own matrix (0x50abfee0: R = 1.128 Y + 1.569 Cr', G = 1.157 Y - 0.814 Cr' - 0.393 Cb', B = 1.108 Y + 1.988 Cb') [V] |

The model's JPEG (`ngage_cam_jpeg`): baseline, JFIF, YCbCr 4:2:2, quality 75, the standard Huffman tables (identical
to libjpeg's, `tests/ngage-cam-jpeg-check.py`); the phone's gallery shows it [V run]. Two wrong turns, kept as
regression tests: the preview read as the 7650's YUV order (white came out magenta, black green), and the 3660's
viewfinder sent as YUV (it is RGB565).

## Open

- Host camera as a source (a later step; the owner asked for a pattern only).
- Sensor type 2 (the other module) is not exercised; neither are the night mode and the QCIF still size.
- 3650/3660: the camcorder (176x144 video, mode state 6 with control 0 on the 3650 [I]) is untried; the 3660's sensor
  variant 2 is not exercised.
