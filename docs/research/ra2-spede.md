# Nokia 9500 (RA-2): SpeDeServer request that blocks Startup.app

Static reading only (objdump of eka1/ra2/core/ROM.bin). [V] = read in code, [I] = inferred.
Frame: [3] res, [8] tid, [9] msg, data[0] = frame byte 10.

## The blocking call

- SysLangUtil.dll 0x5009d0ac (language-list loader; file `C:\System\Bootdata\SIMLanguage.dat`
  string in the same DLL) calls 0x5009ce74, which does RSpeDeClient Connect, then the import
  stub 0x5009d6ec -> SpeDeClient export 0x500b3481 [V].
- 0x500b3481 = `SendReceive(11, {TDes8* buf})` [V]; return address 0x500b348f = the reported
  frame [V]. Only SysUtil.dll and SysLangUtil.dll import it [V]. Meaning: read the
  phone's language list (supported/SIM language codes) as text [I].
- Server: session dispatch 0x500b0cc8 -> 0x500ae944, jump table at 0x500ae970 (index func-2):
  func 11 -> 0x500aeab4: message saved at this+0x34, then 0x500b0574 sends
  **`1B 1F tid 06 {00 00}`** [V]. So Startup waits on **1B 1F**, not on 09 E0.
  (func 10 -> 0x500b04d0 sends `1B 0B tid 05 {00 02}`; nobody imports func 10 [V].)

## Reply 1B 20 (parser 0x500afd0e)

ISI dispatch 0x500af87e: table 0x500af890 indexed by res-9; res 0x1B -> 0x500afb2c, msg table
0x500afb50 indexed by msg-3; msg 0x20 -> 0x500afd0e [V].

| data | meaning |
|---|---|
| [0] | status, must be **00**; else func 11 completes with -2 [V] |
| [1] | not read [V] |
| [2..3] | u16 BE length n, capped at 512 [V] |
| [4..4+n) | n bytes copied (raw pointer copy, 0x500b2338) into a TBuf8<512>, then written to the client and func 11 completed with 0 (0x500aeb3c(this, 11, 0, buf)) [V call, I write/complete details] |

tid not checked [V]. Minimum: 5 data bytes (index 4 is taken with operator[]), and n real
bytes after it, because the copy is not bounds-checked [V].

## What SysLangUtil wants in the text (8-bit; 520-byte stack buffer = TBuf8<512>) [V]

0x5009ce74: length must be > 4; Delete(0,5) (5-char header); Left(4) parsed with TLex Val
must be **> 9**; then 10 times Locate('\n') + Delete through it: **at least 10 '\n'**, else -1.
0x5009d0ac then loops: Locate('$'), Delete through it, Left(3) -> TLex Val -> appended to a
CArrayFix of language codes; ends when no '$' is left (empty list returns 0 too) [V].
Codes are Symbian TLanguage: **001 = ELangEnglish (UK English)** [I, standard enum].
The meaning of the header and the 10 skipped lines is unknown [I]; any filler works.

Payload `"00000" "0010" "\n"x10 "$001"` = 23 bytes:

```
reply 1B 1F 20 00 00 00 17 30 30 30 30 30 30 30 31 30 0A 0A 0A 0A 0A 0A 0A 0A 0A 0A 24 30 30 31   # SpeDe func 11 language list (SysLangUtil): [0] 00 OK, [2..3] len, text: 5-char header, number >9, 10 lines, "$001" = UK English [V parser 0x500afd0e / SysLangUtil 0x5009ce74, 0x5009d0ac; I meaning]
```

## The other one: 09 E0 tid 07 {04 00} (not on Startup's path)

Sent by 0x500b06b4 from 0x500aef98(arg 0) (state flag this+0x20 := 1, called from the
SysState/P&S watcher code 0x500b1790.. ) [V]; data[0] = 1/2/4 by mode [V]. Reply **09 E1**,
parser 0x500afe02: data[0], data[1] -> 0x500b1efc(this[20], d0, d1) [V]; meaning not read [I].
No client request waits on it [I]. If wanted, best guess:

```
# reply 09 E0 E1 00 04    # SpeDe 09 E0 mode 04 ack: [0] status 00 [I], [1] mode echo [I]; parser 0x500afe02 [V]
```
