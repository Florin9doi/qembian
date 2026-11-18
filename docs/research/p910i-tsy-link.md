# P910i: how erigsm.tsy reaches the GSM side (stall 14, SIM emergency numbers)

Static reading of `eka1/p910i/core/ROM.bin` (VA = 0x50000000 + offset) plus the peer log of the
last session (`build/ngage/sess-p910/qemu.log`). No emulator runs. [V] = read from code or log,
[I] = inferred. Budget-limited (CLAUDE.md research rules): enough to fix stall 14.

## 0. Summary

- **erigsm.tsy does not speak AT and does not use the UART mux for control.** It imports nothing
  from C32.dll; its imports are ETel/etelmm/etelpckt/etelsat, Gsmadv/Gsmbas, erigsmapi (5),
  pmbcli (2) and **226 functions of `ereq.dll`** [V, import thunks]. `ereq.dll` imports
  only EUser and **423 stubs of `swbp.dll`**, the OSE client [V]. So telephony control is
  **OSE signals over the existing link on UART unit 2** (eose -> elh4drv), the same path and frame
  format as the start-up requests we already answer (`src/qemu-ngage/p910i-ose-replies.txt`).
- `ECMUX` / `MUXCOMM::0` in the TSY are only port names [V strings, I use]: the standard ETel data
  port handed out by `RCall::LoanDataPort` for CSD/fax/GPRS data. `ecmux.csy` ("Multiplexer CSY") ->
  `eesp.ldd` ("Esp") + `edldrv.pdd` ("Esp.dl", binds IrqUart1..5, UART units 0..4) is the **data
  path**, opened only when a data call / PPP starts [I]. Nothing needs it for stall 14; that is why
  no other UART has been touched, and it is not a sign the TSY is missing.
- The TSY **is** loaded and running: IPC 0x858 is answered from an OSE round trip. The last frame
  in the session log, `62 05 00 E3 3F 41 00 00 00` (signal **0x413F**, 3 data bytes `00 00 00`), is
  that request [V log; I that this particular frame came from the 0x858 call, the code path is V].
- **Stall 14 is our reply**: the table answers `413F` with `22C9` + all zeros = status 0, **count 0**.
  ereq passes that to the TSY as "success, 0 numbers" and Gsmadv leaves KErrNotFound [V].
  Fix: answer 0x413F with a non-empty list (section 3).

## 1. Chain for IPC 0x858 [V unless marked]

| step | address | what |
|---|---|---|
| Gsmadv `RAdvGsmPhone::RetrieveSimEmergencyNumbersLC` | 0x5030F8xx | two-phase list retrieval: IPC **0x858** = phase 1 (size/count), **0x859** = phase 2 (copy list) (literals 0x5030f950/54) |
| TSY ExtFunc dispatch | 0x5090eb88 | 0x858 -> 0x5090f220 -> handler **0x50916a3c**; 0x859 -> 0x5090f230 -> **0x50916ad0** |
| TSY 0x858 handler | 0x50916a3c | takes the current network info (cached at this+0x868; MCC/MNC digits, 6 nibbles), calls ereq **0x508cc038**(client [this+0x7FC], digits, this+0x8C). Result <= 1 -> registers pending TSY request **0xF1** (0x508fa43c) and waits; else completes the IPC at once with the mapped error (0x508fa5f0) |
| ereq request | 0x508cc038 | busy -> 10; digits > 15 -> 7; packs the 6 digits as 3 BCD bytes (low nibble first, 0x508d9ca4); calls swbp stub **0x508a9f80**; status 1 -> remembers the client, arms its active object (SetActive) and waits for the reply asynchronously; other status -> calls the client back at once |
| swbp stub | 0x508a9f80 | allocates signal **0x413F** with 3 data bytes = the BCD PLMN, sends it to OSE process **0x62** (pid byte at 0x508bf529); mode word `*0x508ea878 == 1` (ROM constant, so always) -> returns 1 without waiting: async |
| ereq RunL | 0x508cc0d8 | the received signal (this+28) -> swbp decoder **0x508a9ff8**: signo must be **0x22C9** (else status 9), status = data[0], copies 53 bytes data[1..53]; status mapped 1:1 (0x508d9bb0, 0..9, else 11); status 0 -> converter **0x508ccba8** to a 56-byte struct; client callback(status, struct) |
| converter | 0x508ccba8 | out[0] = raw[0] (**count**); 13 entries: out[4+4i..7+4i] = raw[1+4i..4+4i] |
| TSY callback for 0xF1 | 0x50916b84 | status -> Symbian error (0x508fa5f0: 0 -> KErrNone); for i < count: entry bytes 0..2 = **BCD digits, low nibble first, stop at a nibble > 9**; adds the number to the list at this+0xEFC (skips duplicates/empties, 0x5093198c [I]); completes request 0xF1 = the 0x858 IPC with the list count |
| TSY 0x859 handler | 0x50916ad0 | copies the stored list (this+0xEFC) into the client's buffer |

Receive filter used by the sync variant: `0x508bf864` = `{1, 0x22C9}` (OSE SIGSELECT) [V].

## 2. Wire format (existing peer framing, docs/research/p910i-gsm-link.md)

Request (phone -> GSM) [V log + code]:

```
62 05 00 E3 3F 41 p0 p1 p2
dst=0x62 len=5 src=E3 signo=0x413F  PLMN BCD (MCC1|MCC2<<4, MCC3|MNC1<<4?, MNC2|MNC3<<4)
```
The digit packing is plain nibble order of the 6 digits [V]; with no network the TSY sends
`00 00 00` [V log]. The peer can ignore the PLMN [I: the real modem returns the ECC list for
that network; any list will do for us].

Reply (GSM -> phone), data bytes after signo 0x22C9 [V offsets, I meaning of byte 4 of an entry]:

| data offset | size | meaning |
|---|---|---|
| 0 | 1 | status: **0 = OK** (anything else -> error to the client; 1 = "pending", don't use) |
| 1 | 1 | count of entries (0..13) |
| 2 + 4i | 3 | entry i: number in BCD, low nibble = first digit, 0xF fill |
| 5 + 4i | 1 | entry i: extra byte, not read by the TSY [V]; probably the ECC category [I] |

Total 54 bytes (1 + 1 + 13*4); the current line sends 55, the extra byte is harmless.

## 3. Proposed reply line (replace the current 413F line)

```
# 0x413F = emergency-number list for the PLMN in the 3 data bytes (TSY IPC 0x858 via ereq/swbp).
# 22C9: status 0, count 2, entries {BCD3, category}: "112", "911"; 11 empty entries.
reply 413F 22C9 00 02 11 F2 FF 00 19 F1 FF 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
```

"112" -> `11 F2 FF`, "911" -> `19 F1 FF`, "08" would be `80 FF FF` [V decode rule]. Expected
effect: IPC 0x858 returns 2, 0x859 returns the two numbers, `RetrieveSimEmergencyNumbersLC`
returns instead of leaving -1, Tele.app's start-up step 2 (0x50998010) proceeds [I until run].

Regression test: in `tests/` (peer table test), assert that a `413F` request gets a 22C9 reply with
status 0 and count > 0, and that entry 0 decodes to "112" with the TSY's nibble rule.

## 4. Answers to the brief

1. Control: OSE signals through ereq/swbp/eose over UART unit 2 (phys 0x58000040, IRQ 7), not AT
   [V imports]. `MUXCOMM::0` (ecmux -> eesp/edldrv on one of UART units 0..4) is the data-call port
   [I]; its framing (ESP) and start-up were not read: not needed until data calls (out of scope).
   No AT strings at all in erigsm.tsy or erigsmapi.dll [V `strings`].
2. The TSY is loaded and already talking (0x413F) [V code path + log]. The idle UARTs are the data
   mux, which only opens for a data connection [I]. No property/signal wait blocks this request.
3. IPC 0x858: no static list, no SIM file read on the AP side: OSE request 0x413F (PLMN) ->
   reply 0x22C9 {status, count, 13 x 4-byte BCD entries}. count > 0 with status 0 fixes it.
4. Peer: one table line (section 3). Start-up frames seen so far all come from src E3 and are
   already answered; after this fix, expect further TSY requests (network registration, SIM
   status, signal strength) to show as `UNANSWERED` in the peer log: grow the table from there,
   each request's swbp stub names its reply signal in the same way (`0x508a9f80` pattern: signo
   literal before the stub's SIGSELECT pointer) [I].

## 5. Not read / open

- ESP framing, ecmux start-up, edldrv unit number (data calls).
- Meaning of the 4th entry byte; whether the real modem merges SIM EF_ECC with network
  numbers (not needed).
- Which other TSY requests follow once Tele.app gets past step 2.
