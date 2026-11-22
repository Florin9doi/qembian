# Where is the modem firmware of the EKA1 phones? (2026-11-21)

Owner's questions: (1) did the N-Gage, 7650, 6600, 3230, 9210, 9500, 7710, 6630 and N70 have a separate modem
CPU, where does its firmware come from, is it in our files and are we reading them wrong; (2) the modem protocol
varies per phone and firmware, so a stand-in tuned to one phone does not carry over, while running each phone's
own modem firmware would. Can that be done for these phones?

[V] = checked in the bytes, our docs or a cited source. [I] = inferred. No QEMU was run. Scans: entropy per
256 KB block and `strings` over the extracted images (script kept out of the repo; the commands are in the
evidence column).

## 1. Short answer

We are **not missing** the modem firmware for 8 of the 9 phones. It is in the files we have, under names we did
not read as "modem":

- **N-Gage, 7650, 6600, 3230 (DCT4, one ARM):** the cellular software (Nokia's "NOS", source tree
  `ENOS/CMT_SW/...`) is **inside the Symbian ROM image** of the `.C1`/`.0C1` file (Phoenix calls this file "MCU"),
  on the same ARM as Symbian. Its code is **scrambled** (8.0 bits/byte); its strings and data are plain.
- **9210 (Symbian 6.0):** the GSM stack is **`cmt.ext`**, a 2.3 MB EKA1 kernel extension in the Symbian ROM, plain,
  **and it already runs** in our `-M nokia9210` (against a DSP stand-in).
- **6630, N70 (BB5, OMAP1610 + RAP3G):** a separate modem CPU; its firmware is the **MCUSW and DSP0 partitions of
  the same FIASCO `.C0R`**, plain, already split out by `tools/fiasco.py` in `eka1/rm1|rm84/fiasco/core/`. Same
  lineage as the N95's modem (193 of the 6630's 262 source-file names also appear in the N95 MCUSW).
- **7710 (S90):** a separate DCT4 cellular engine behind XBUS; its firmware is the **`.300` file** in our package
  (`McuSwId=rm12pr410002.300`), scrambled (DCT4 MCU file).
- **9500:** same split as the 7710; its `.300`-type MCU file is **really absent** from `RA-002 v.06.27 9500.rar`.

## 2. Per-phone table

| Phone (our board) | Symbian CPU | Separate cellular CPU? | Where the modem firmware lives | Reflashed how | In our files? | Evidence |
|---|---|---|---|---|---|---|
| N-Gage QD RH-29 (`-M ngage`) | ARM9 in a DCT4 UPP-class ASIC ("WD2 ALGORITHM") [I chip name] | **No**: NOS and Symbian share the ARM; NOS started by `nosstarter.ext`; layer 1 on the ASIC's DSP [I] | inside `RH2941.0C1`: ROM.bin 0x00C000-0x2AC000 = NOS code (scrambled), 0x2AC000-0x31B000 NOS strings/data (plain) | one Phoenix MCU file (+ `.026` PPM: language/variant data); the flasher descrambles while writing [I] | **Yes, scrambled** | [V] entropy 8.0 over 0x40000-0x280000 of `eka1/output/rom/ROM.bin`; strings `ENOS/CMT_SW/ATI/...`, `ENOS/ECS/NOS_DRIVER/SRC/Decsimpphonet.cpp`, `SIM_SERVER_TASK`, `SIM_L2_TASK`; docs/eka1-ngage-questions.md Q20 |
| 7650 NHL-2 (`-M nokia7650`) | same family as N-Gage | **No** (same as N-Gage) | inside `NHL23250.6C1`, same layout | MCU + PPM (`.677`) | **Yes, scrambled** | [V] same entropy profile and `ENOS/CMT_SW` strings in `eka1/nhl2/output/rom/ROM.bin` |
| 6600 NHL-10 (`-M nokia6600`) | same family | **No**; Symbian reaches the NOS through `isa_if_driver.ldd` + `enosdriver.ldd` (ISA) instead of `nosdriver`+`phonet` | inside `NHL105530.C1` | MCU + PPM (`.26`) | **Yes, scrambled** | [V] same profile; `..\CMT_SW\...`, `ENOS_SW\SRC\Nos.cpp`, "PMM memory mapping fails at nos.cpp" in `eka1/nhl10/output/rom/ROM.bin`; docs/devices/nokia-6600-nhl10/bring-up.md |
| 3230 RM-51 (`-M nokia3230`) | same family | **No** | inside `RM51PE507170.C1` | MCU + PPM (`.26`) | **Yes, scrambled** | [V] same profile; `CMT_SW\ISA_PROTOCOLS\DALTON\ATI\...` in `eka1/rm51/output/rom/ROM.bin` |
| 9210 RAE-3 (`-M nokia9210`) | ARM (the only main CPU) | **No** CPU; a DSP (layer 1) with API RAM at 0x05011000 | **`cmt.ext`** in the core ROM (VA 0x509A1B30, 2 312 412 bytes), plain; it downloads the DSP code itself (127 blocks of 512 halfwords) | one Phoenix MCU file (`RAE33004.130`, "LINDA ALGORITHM") + language files | **Yes, plain, running** | [V] entropy 6.7; strings "Nokia Communicator GSM900/1800", AT `+CGSM`, task names RM/SMS/MM/SIM L2/CC, "DSP Version", "PPM Version"; docs/devices/nokia-9210-rae3/bring-up.md stalls 8-14. Whether the DSP program is all downloaded or partly in DSP mask ROM: [I] downloaded |
| 9500 RA-2 (`-M nokia9500`) | TI OMAP1510 (APE) | **Yes**: DCT4-generation cellular engine (CMT) behind XBUS (UART2) | the CMT's own flash; Phoenix file "MCU" (+ PPM, CNT) | Phoenix; the APE images are sent through the CMT (ADL) [I from the 7710 manual] | **No**: the archive holds only `.c01` (APE core, ROFS, loaders), `.l18`, `.u18` | [V] rar listing; docs/research/modem-firmware-in-flash-files.md section 1 |
| 7710 RM-12 (`-M nokia7710`) | TI OMAP1510 (APE) | **Yes**: DCT4 CMT behind XBUS (same drivers as the 9500) | **`rm12pr410002.300`** (2.4 MB), Phoenix A2 container, header "DCT4 ... MX/ORCA ALGORITHM", fully scrambled | Phoenix: `McuSwId=rm12pr410002.300` (CMT), `AdspSwId=RM12mPR4.10.0.c1u` (the APE side, Nokia's "ADSP" name), `AdspCpId=...V04` (variant); `Fps8AfterMcuSwFlashDelay` | **Yes, scrambled** | [V] `eka1/rm12_rom/*.ini`, header bytes, entropy 8.0 in every block; RM-12 service manual troubleshooting page: separate "APE flashing" and "CMT" paths, "OMAP1510 troubleshooting" ([manualslib RM-12 p.47](https://www.manualslib.com/manual/2161512/Nokia-Rm-12.html?page=47)) |
| 6630 RM-1 (`-M nokia6630`) | TI OMAP1610 (APE) | **Yes**: RAP3G modem (`hw_mdi_rap3g.c`), ARM, mostly Thumb (18 629 Thumb push vs 105 ARM stmfd); link SSI (`ssi.pdd` + `xbus.ldd`) | **MCUSW** (5 047 104 bytes) + **DSP0** (1 377 088 bytes) partitions of `RM1_6.050340_B4.C0R` | BB5 FIASCO, one file for both ASICs (as the N95) | **Yes, plain** | [V] `eka1/rm1/fiasco/core/MCUSW@00200400.bin`: entropy 7.0, strings `rm_plmn.c`, `net_sim.c`, `csdc_isi.c`, WRAN/WCDMA, "ISA GSM SMS Server", "Nokia 6630"/"Nokia 6660", `ssi_drv.c`; 193/262 `*.c` names shared with `build/n95-cmt/MCUSW@00000200.bin` |
| N70 RM-84 (`-M nokian70`) | TI OMAP1710 (APE) | **Yes**: same RAP3G lineage; link SSI, ISC stack on the APE | **MCUSW** + **DSP0** of the N70 FIASCO (same sizes, **different bytes** from the 6630's) | BB5 FIASCO | **Yes, plain** | [V] `eka1/rm84/fiasco/core/`; SHA-256 of MCUSW and DSP0 differ from the 6630's; same strings plus `ssi_t.c`, `SSI_TASK` |

Notes:
- "PPM" on DCT4 = the language/variant pack (texts, fonts, NOS strings, Symbian variant overlay); "CNT" = content
  (user files, C: image). Neither holds modem code [V for our PPM files: docs/devices/*/bring-up.md "PPM/variant";
  CNT: I].
- DCT4 DSP code: no separate DSP file in any DCT4 package we have; it is inside the MCU image and loaded by the MCU
  [I] (the 9210 does exactly that in `cmt.ext`, [V]).

## 3. Answers

**Q1. Separate modem CPU, where its firmware is, are we reading the ROMs wrong?**
- N-Gage/7650/6600/3230: no separate CPU. The GSM stack is part of the "MCU" flash file we have and sits in the
  first ~3 MB of the ROM image, next to Symbian. Not missing; scrambled. We read it right but cannot execute
  it (H-nos-none skips its entry; H-nos-standin answers the ISI it would answer).
- 9210: no separate CPU; the stack is `cmt.ext`, plain, and runs today.
- 6630/N70: separate RAP3G CPU; firmware = MCUSW + DSP0, plain, in the `.C0R` we have and already split out.
  The bring-up docs list them but nothing uses them.
- 7710: separate DCT4 CMT; firmware = the `.300` file we have (scrambled).
- 9500: separate DCT4 CMT; firmware file genuinely absent from our package. A full Phoenix/NaviFirm RA-2 package
  should carry an MCU file like the 7710's `.300` [I by analogy; not confirmed on the web].
- Correction to docs/research/modem-firmware-in-flash-files.md section 1: the 9500's cellular engine being DCT4 is
  now backed by its twin, the 7710, whose CMT file says "DCT4" in its header [V for the 7710].

**Q2. The owner's thesis and running the modem firmware.**
- The thesis holds on EKA1 too [V in our docs]: the ISA interface checksum reply byte the APE checks differs per
  phone (6600 0x82, 9500 0xA7, 7710 0x6B); the N70 replaced ISA with ISC (new control messages); the 6630 needed
  the 9500 form of NET_MODEM_REG_STATUS plus a sub-block the N-Gage form lacks; the 6630 and N70 MCUSW differ
  byte-wise. One reply table does not carry across phones.
- How the two sides talk:
  - DCT4 one-chip (N-Gage/7650): PhoNet/ISI frames through `nosdriver.ldd` channels into the NOS in the same
    address space ("EKIF" EPOC-NOS channels); 6600/3230: the same ISI through `isa_if_driver` + `enosdriver`.
    No wire, a function-call boundary [V Q50, 6600 bring-up].
  - 9210: `enos.ldd`/PHONET/isimsg to `cmt.ext` in the same kernel [V module list].
  - 9500/7710: ISI over XBUS (byte protocol on UART2 + wake GPIO + CTS) [V docs/research/ra2-xbus-modem.md].
  - 6630/N70: ISI over OMAP SSI (32-bit control words, DMA payloads), the N95's link generation [V].
- Running each one's own firmware:
  - 6630/N70: **feasible, and the same work as the N95**: plain ARM code of the same RAP3G family, same SSI link.
    The N95 CMT emulation (docs/research/n95-cmt-emulation.md: NOLO -> MCUSW boots, interrupts, timers, SSI up)
    is the template; peripheral addresses may differ (older RAP3G) [I]. Layer 1 (DSP0) stays unemulated: start-up
    to "no service" at best, as on the N95.
  - 9210: **already the case** for MCU code; what remains is the DSP per-frame protocol, the SIM interface and
    CTSI frame timing (bring-up "open" item 1). No network.
  - N-Gage/7650/6600/3230: needs (a) descrambling (only TriX, GPL, at hand: owner's decision in
    docs/eka1-ngage-questions.md "Decisions"), (b) the UPP's cellular peripherals (DSP mailbox, SIM, RF,
    timers) modelled in `ngage*.c`, (c) a DSP stand-in. The CPU is already emulated, so this is one board, not
    two. Hard but bounded; months [I].
  - 7710: descrambling + a whole second machine (DCT4 MCU + peripherals) joined over XBUS, + DSP stand-in. The
    hardest. 9500: impossible until an MCU file is found (it could then share the 7710's model [I]).

## 4. Cheapest path per family

| Family | Cheapest path |
|---|---|
| BB5 EKA1 (6630, N70) | Static first: the N95 "ISI encoders" pass (modem-firmware-in-flash-files.md 2.5) on each phone's own MCUSW (plain Thumb, load at its CMT base) to fix the reply tables authoritatively; later, reuse the N95 CMT machine with this MCUSW once the N95 answers ISI. |
| 9210 | Keep running `cmt.ext`; extend the DSP stand-in and add a SIM-interface model only if a feature needs it. |
| DCT4 one-chip (N-Gage, 7650, 6600, 3230) | Stay with the ISI stand-in. If the owner runs TriX locally (tool, not copied code), read the descrambled NOS statically in Ghidra for message layouts per phone (strings/data are already plain and name the tasks); execution only after that. |
| DCT4 CMT over XBUS (7710, 9500) | Stay with the reply tables, built from the APE-side parsers in Ghidra (ra2-isi-replies-4.md method). The 7710 `.300` becomes a static source after descrambling. For the 9500, look for a full RA-2 package with its MCU file before any modem work. |

## Sources

- Bytes: `eka1/output/rom/ROM.bin`, `eka1/nhl2|nhl10|rm51/output/rom/ROM.bin`, `eka1/rae3/core/ROM.bin` (cmt.ext via
  `files.json`), `eka1/rm1|rm84/fiasco/core/{MCUSW,DSP0}@*.bin`, `eka1/rm12_rom/rm12pr410002.300` and `*.ini`,
  `build/n95-cmt/MCUSW@00000200.bin`; archive listings of the firmware packages and `eka1/ngage_rom/`.
- Our docs: docs/eka1-ngage-questions.md (Q20, Q50, Decisions), docs/devices/*/bring-up.md, docs/research/
  modem-firmware-in-flash-files.md, docs/research/n95-cmt-emulation.md.
- Web: [Nokia RM-12 service manual, APE flashing troubleshooting](https://www.manualslib.com/manual/2161512/Nokia-Rm-12.html?page=47)
  (separate CMT and OMAP1510 paths). A search for the 9500 package's file list found nothing usable
  ([search results incl. the 9500 service manual, 403](https://manualmachine.com/nokia/9500/201922-service-manual-04-ra2-servtool)).
