# Nokia 6630 (RM-1): SecPdd's call into the OMAP1610 secure ROM at 0x3C00, and how to stub it

Static reading of `eka1/rm1/core/ROM.bin` (VA 0x50000000). [V] = read in the disassembly,
[I] = inferred. No emulator runs.

## 1. Call chain inside SecPdd (0x500a8d30, 10592 bytes)

- Copy: `0x500a9768(base)` copies 0x880 bytes from ROM 0x500aad84 to the SDRAM buffer and stores
  `base+1` at `[0x80004724]` [V]. (The run saw ROM 0x500aadc4 at PA 0x100fed40.)
- `SecureCall(r0 = service, r1 = &argptr)` = `0x500a9708` (ARM) [V]:
  - `[0x80004724] == 0` (no copy) -> returns 0 without calling.
  - else compares the copy with ROM 0x500aad84 (0x880 bytes, `0x500a97ec`); mismatch -> returns 0;
    match -> calls the copy's Thumb entry (`0x500aad86` -> dispatcher `0x500aafa8`).
- 14 operation wrappers (`0x500a8d98`, `0x500a8e34`, `0x500a8eb4`, `0x500a8f24`, ... `0x500a961c`)
  all call `SecureCall`. Each builds its arguments in a shared buffer `[0x80004728]`: word 0 = 0x1f,
  then the arguments (pointers converted with the kernel import `0x500aab78`, which converts a linear
  address to a physical one [I]). The service argument is the physical address of a 16-byte service UUID at
  0x500aace4..0x500aad84: `ab0fbd96 af2ac271 d26264af` + index (0x12, 0x13, 0x0a, 0x07, 0x0f, 0x14,
  0x0b, 0x0c, 0x0d, 0x1a) [V]. **Every secure operation goes through the same 0x3C00 entry** [V]
  (not only "load PA").
- Result: the ROM's r0 is stored to the caller's out word and returned. **Every caller treats
  ROM result `1` as success**: `cmp rX,#1; moveq rX,#0; mvnne rX,#1` -> KErrNone / KErrGeneral (-2)
  (0x500a999c, 0x500aa0f0, 0x500aa508, 0x500aa8ac, 0x500aaa00, ...) [V]. Some ops (0x500aa508)
  read result bytes from the out-buffer only when the result is 1 [V].

## 2. ROM entry contract (dispatcher 0x500aafa8 + common path 0x500ab190..0x500ab238) [V]

Entry at PA 0x3C00 (`mov r1,#0x3C00; mov pc,r1` at 0x500ab234, ARM state):
| reg | value |
|---|---|
| r4 | PA of the 16-byte service UUID (first entry); on re-entries a PA cookie 0x100ff550/560/570 |
| r5 | PA of the argument pointer (whose first word was also converted to a PA) |
| r6 | flags = first word of the argument block (0x1f) |
| r8 | 0x490d (constant) [meaning I] |
| r9 | PA - VA of the copied code; r10 = PA - VA of the stack; r11 = saved SCTLR; r12 = saved value of MMIO 0xfffed480 (0x0F written there just before) |
| sp | **physical**; `[sp]` = return address = PA 0x100fefd9 (Thumb, ROM 0x500ab05c), `[sp+4]` saved cp15 c15, `[sp+8]` saved CPSR, `[sp+12]` flags |
| CPU | caller's mode with I and F set, MMU, D/I cache and write buffer off (SCTLR bits 0, 2, 12 cleared; bit 13 set = high vectors) |

Return: the ROM **pops `[sp]` and branches (bx) to it**, MMU still off, with
- `r4` = return kind: `1` = done (result in r0); `2` = turn MMU and caches off and return [I: power-down];
  `3` = serve a pending IRQ, then re-enter; `4` = callback: call `r0(r1,r2,r3)` in normal world, re-enter
  with the result; `5` = yield, re-enter [V for the code paths, I for the names].
- `r0` = result (1 = OK).
- must preserve r9, r10, r11, r12; r1-r3, r5-r8 and lr are free (`0x500ab05c..0x500ab0d8` reloads them).
The return code restores 0xfffed480 from r12, turns the MMU back on from r11, jumps to the virtual copy,
`sp -= r10`, restores c15 and CPSR, pops r8-r11 and r4-r7/lr and returns r0 to `SecureCall` [V].

## 3. Who depends on it [V unless marked]

- `EStart.exe` starts `Z:\System\programs\SecEnvInit.exe` (0x500a6fd4 -> RevlonEStart 0x500a63a8:
  RProcess Create, Resume, Close) and **does not wait for it** [V code, I names].
- `SecEnvInit` (Thumb, E32Main 0x50306484 -> TRAP 0x503064e4 -> 0x5030652c): reads
  `Z:\System\Libs\pa.bin`, then calls SecEnv.dll (`0x50306cf8`) with the data up to 3 times (stops
  retrying on -14). Non-zero -> leaves the TRAP and **exits cleanly with that error** (no panic,
  no reboot). On success it creates the Ss.dat secure-storage observers and runs an active scheduler.
- SecPdd error paths only set -2 and clean up; nothing calls Kern::Fault there [V for the wrappers'
  callers read; I for the rest].
- Later SecEnv clients (SecUi, JcfSecurity, DRM) will see KErrGeneral or "no server" [I]; they are
  not on the boot path [I].

## 4. Getting past it, ranked

**(a) Board stub at physical 0x3C00 (recommended).** Four ARM words in CS0:

```
0x3C00: e3a00000   mov  r0, #0        @ ROM result "not OK" -> SecPdd returns KErrGeneral (-2)
0x3C04: e3a04001   mov  r4, #1        @ return kind: call complete
0x3C08: e49d1004   ldr  r1, [sp], #4  @ pop the return address (PA 0x100fefd9, Thumb bit set)
0x3C0C: e12fff11   bx   r1
```
Touches only r0, r1, r4; leaves r9-r12 as required. Effect: every secure op fails with -2,
SecEnvInit exits with -2 after 3 tries, the boot carries on [I: nothing waits for SecEnvInit].
Use `mov r0, #1` (word `e3a00001`) only if something later insists on success: the ops then
report KErrNone with untouched (zero) out-buffers [I, riskier].
The entry path also writes 0x0F to PA 0xfffed480 and back (MMU off); the board must accept those
writes (it already did in the run).

**(b) One-word patch in SecPdd: never call the ROM.** In `SecureCall`:
`0x500a9720: 0a000005 (beq 0x500a973c)` -> `ea000005 (b 0x500a973c)` [V encoding]. SecureCall then
always returns 0 -> the same KErrGeneral as (a), and the MMU-off path, 0xfffed480 writes and cache
maintenance never run. Cheapest if the board can patch ROM words; no such mechanism exists in
nokia6630_soc.c today, so (a) is simpler.

**(c)** Not loading SecEnvInit (drop from EStart) also works but needs an EStart patch; no gain over (b).
