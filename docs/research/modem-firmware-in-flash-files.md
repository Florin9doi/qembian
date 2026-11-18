# The modem's own firmware: what the flash files hold, and whether it can run (2026-11-18)

Question from the owner: can we (a) run the real modem firmware instead of the reply tables, and (b)
reverse-engineer it to speed up the mocks (the 9500 first). [V] = read in the bytes or the code,
[I] = inferred.

## 1. Nokia 9500 (RA-2): the modem firmware is not in the archive

- The archive `RA-002 v.06.27 9500.rar` holds three files only: `RA2n6270.c01`, `.l18`, `.u18` [V].
  All three are the application-processor (APE, OMAP1510) side: the .c01's own tertiary loader names its
  images "Bootcode", "Tertiary Loader", "APE Core MCU SW" [V, strings in record `000L`]; the records are
  the EKA1 core ROM, ROFS, a second test ROM, loaders, variant data, the C: FAT (eka1/ra2/images/manifest.json).
- Phoenix flashes a 9500 with a separate cellular-side MCU file (plus PPM and CNT) [I: community flashing
  logs for RA-2 list MCU, PPM, CNT, ADSP .c01, APE variant .l20, APE user .u20]. That file is what we lack.
- The cellular side is a separate baseband behind the XBUS link (UART2 + wake GPIO + CTS; the phone only
  listens until the peer syncs: docs/research/ra2-xbus-modem.md) [V phone side], a DCT4-generation modem
  [I]. DCT4 MCU code is scrambled in flash files (docs/eka1-ngage-questions.md Q20: the N-Gage/6600 NOS is
  7.95 bits/byte; only a GPL descrambler, TriX, is at hand) [V on the N-Gage/6600 files].
- Even with the file: running it needs the baseband chip (ARM7-class MCU + undocumented peripherals), the
  LEAD3-class DSP that owns GSM layer 1 (no emulator exists; the MCU firmware waits on it), the
  permanent-memory data (IMEI, security, RF tuning: not in the archive) and a fake network. Verdict: no.
- What speeds up the 9500 mock is the phone side we have: the core ROM is imported in the owner's Ghidra
  (`/ra2/ra2_core_rom.bin`, base 0x50000000, ARM:LE:32:v4t, auto-analysed) so every ISI parser (Mmgsmwcdma
  router 0x50c07348 and its 10 resource tables, Nokia.dsy, LightServer, SpeDeServer, IsaApi) can be read in
  one pass instead of one handler per stall (docs/research/ra2-isi-replies-4.md lists the open requests).
  Trap: the ROM mixes ARM and Thumb; a function created by hand at a Thumb vtable target decodes as ARM
  garbage unless the TMode context is set (0x50c096bc).

## 2. Nokia N95 8GB (RM-320) and the BB5 family: the modem firmware is in the core file, in clear

File: `roms/RM-320/35.0.001/RM-320_35.0.001_PR.C0R` (FIASCO B2; splitter `tools/fiasco.py`, partition lister
`build/n95-cmt/fpsx_parts.py`). The container header names the APE flashing algorithm
`OMAP2420 UNISTORE-II-1.5.1 ALG`, `XSR 1.5`, and Elf2flash versions for both sides [V].

### 2.1 Two ASICs in one file [V]

Every 0x17 data chunk's descriptor byte 0, and byte 36 of each 0x27/0x28 partition record, is the ASIC key:
`00` = CMT (modem), `01` = APE (the owner's `BB5_FLASH_ORCHESTRATION.md` says the same from
ta_adl2_impl.dll). Chunk locations are addresses in that ASIC's own flash.

| ASIC | Partitions (record location) | Notes |
|---|---|---|
| CMT (00) | NOLO 0xa0000, KEYS 0xac000, PRIMAPP 0xae000, PASUBTOC 0xb0000, PAPUBKEYS, UPDAPP 0xe0000, DSP0 0x200000, MCUSW 0x3e0000 | the modem's NOR flash; the MCUSW chunks start at 0x200 with the CMT TOC (X-LOADER, KEYS, PRIMAPP, ...) and run to 0x8806c0 |
| APE (01) | X-LOADER 0x2000, KEYS, PRIMAPP, PASUBTOC, PAPUBKEYS, SOS+XZIP 0x260000, SOS+UPDAPP 0x4c0000, SOS+CORE 0xa40000, SOS+ROFS 0x3a40000 | the OMAP2420's OneNAND (XSR); SOS+CORE is the Symbian XIP ROM we boot |

Extracted copies (git-ignored): `build/n95-cmt/` (MCUSW as a NOR image from 0x200, DSP0, UPDAPP, PRIMAPP, NOLO).

### 2.2 MCUSW = the RAP3G modem firmware, plain [V]

- Entropy 4.2 bits/byte over the NOR image (54 % erased 0xFF gap fill), 23 872 Thumb push / 28 209 pop
  halfwords vs 224/265 ARM push/pop words: mostly Thumb, ARMv6 (string
  `*** WARNING: check ARM1136 errata item 399234 ***`, ARMv6 MMU attribute strings "L2/L1: WB, NoWA, Shared").
- 295 source file names in clear (ssi_hw.c, ssi_drv.c, sys_info_router.c, time_alarm.c, test_server.c, ...),
  ISI/GSM primitive names (MM_RR_PLMN_SEARCH_REQ, MAC_RR_PAGE_IND, DL_DATA_IND, COMMGR_GROUP_ACTIVATE_RESP,
  ST_SELFTEST_LIST_GET_REQ ...), WCDMA RAN code (wran_*), USIM T=0/T=1, "Nokia N95 8GB", SSI_TASK with
  stuck/resend timers, a kernel with core dumps (core.sram, core.sdram, core.norflash0, ETB dump),
  "HW_OSMO_MSG_PHONET_MESSAGE_DATA". So layer 2/3 and all ISI servers run on this ARM; layer 1 is in DSP0.
- DSP0: 1.4 MB, no identifying strings; the modem DSP image [I].
- Not scrambled, unlike DCT4. Nothing stops a static read of every ISI response encoder.

### 2.3 Flashing: who does what [V from the owner's research, re-checked]

Owner's material: `~/work/symbian-clang/n958gb-flash/` (STATUS.md, BB5_FLASH_ORCHESTRATION.md,
bb5-security-analysis.md, pcap captures, `RM-320/ups_cmt.bin`, `ups_ape.bin`), `~/work/nokia-phoenix/`
(bb5-ups-internals.md, bb5-adl-wire-format.md, fg-file-format.md, decompiled Phoenix DLLs), the Zig flasher
`~/work/symbian-clang/nokia_flash/` (fiasco/fbus/phonet/usb, `flash`, `nand-read` commands).

- The PC first uploads two Update Servers (the "flash-time receivers"). Verified byte for byte today:
  `ups_cmt.bin` = 1024-byte header + the C0R's **UPDAPP** partition (213 888 bytes); `ups_ape.bin` =
  1024-byte header + **SOS+UPDAPP** (1 572 864 bytes). UPDAPP's strings: "UpdateAgent", "DEADPHONE USB
  FLASHING DETECTED", PASUBTOC/PAPUBKEYS handling.
- The APE Update Server is a small Symbian nanokernel build (strings ekern.exe, NandThread, MUXONENAND,
  XSRPART5, PBUS-MMC-OMAP) that writes the APE's OneNAND itself over USB (ADL loader protocol, ISI msg 0x21).
- The CMT Update Server is bare-metal on the modem, writes the modem's NOR through a flash-driver vtable,
  and receives its ADL messages (resource 0x6F) proxied by the APE over SSI. The PC never talks to the
  modem directly. So on the N95 each processor flashes its own storage; the modem does not flash the APE's
  NAND.
- Signatures: each ASIC's boot ROM validates its own chain (KEYS, PRIMAPP, PASUBTOC, PAPUBKEYS per ASIC)
  against a root-key hash in fuses; the PC does no firmware crypto (bb5-security-analysis.md).

### 2.4 Can the RAP3G firmware run?

Better position than DCT4 (plain code, debug strings, the owner's SSI/ADL protocol notes), but still a new
SoC model: ARM1136 (QEMU has it), interrupt controller, timers, NOR, the SSI link to the APE, SIM
interface, DSP boot and mailbox, RF control. The peripheral map would have to be read out of MCUSW
(doable, large). Layer 1 is in the DSP, so there would be no real network; a start-up to "no service" with
a SIM might be reachable. Per-phone permanent memory is partly available (`roms/RM-320/35.0.001/package/*.pm`,
"WITHOUT120,308" = without the IMEI/security and lock fields). Estimate: months; not before the static route.

### 2.5 The static route (recommended)

MCUSW holds the encoders of every ISI response and indication the phone side parses. One Ghidra pass over it
gives authoritative layouts (message ids, status bytes, sub-block ids and sizes) for the whole BB5 family
(N95, N95 8GB, E71, E72, N8 ... share the RAP3G modem lineage; later ones RAPUYAMA [I]). Compare against
`platform/isi/replies-bb5.txt` and fix what was guessed. Research deliverable: docs/research/n95-isi-encoders.md
(an agent's job, chunked per ISI resource).

## 3. Ghidra MCP notes (owner's server, localhost:8089)

`GET /mcp/schema` lists 239 tools. POST tools take JSON bodies (form encoding fails). Always pass
`?program=NAME`. Raw-binary flow: `POST /import_file {file_path, project_folder, language, auto_analyze:false}`,
`POST /set_image_base {address}` (starts analysis), `POST /run_analysis`, `GET /analysis_status`,
`GET /decompile_function?address=`, `GET /save_program` (refused while analysis holds a transaction).
