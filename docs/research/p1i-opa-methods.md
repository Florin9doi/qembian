# P1i OPA call sizes: (IID, method) -> input and output bytes

Sony Ericsson P1i R6E30, `OpaClient.dll` (core XIP 0xf80b5e60..0xf811b400). Purpose: let the host CCPU
(`src/qemu-syborg/hw/arm/syborg_ccpu.c`) answer a type-8 call `{8, P, req, obj, n, w0, args}` with
`{9, 0, req, outlen}` + `outlen` bytes, where `outlen` is what APS's proxy reserved. Static reading only.
[V] = verified in the disassembly or against the boot log, [I] = inferred.

Full table: `docs/research/p1i-opa-methods.inc` (C, 135 interfaces, 834 methods; generated, see "Method").

## How a proxy call is built

- Every CCPU-side interface has one proxy class in OpaClient (`N5OPAIF<n>C<IName>E`, e.g.
  `CIParameterStorageManager`). Its constructor calls the common base constructor f80b6500 with the
  interface IID (copied to proxy+0x34) [V]. One proxy class per IID, no IID shared [V].
- A method marshals into two buffers of the request object: input stream at +0x2c, output stream at
  +0x30 [V]. Calls: f80bad4a begin, f80badc4 header (4-byte word), writers, f80bb094 end [V].
- **The header word is the method number, the COM vtable index** (0..2 = IUnknown, first own method = 3):
  `w0` in the type-8 packet = the method [V: ISystemControl slot 13 writes header 7, matching the log's
  `07000000 ...` 14-byte call on object 4096].
- The output buffer gets the same header word plus fixed reserves (f80bad20 n). Output length = 4 +
  reserves [V: IParameterStorageManager m3 reserves 4+4 -> 12, the `{0x1F7, 4, 12}` in the log].
- Input writers: f80bad76 (n-byte int), f8103dfc/e22/e2c/e36 (4), f8103e40/e4a (2), f8103e0e/e18 (1),
  f80bae20 (TDesC8), f80bae50/f80baf96/f80bb084 (byte buffers), f80baeca/f80bb028/f8103e06
  (UTF-16 strings): variable-length ones make `in` = -1 in the table [V]. The CCPU gets the input length in
  the packet anyway; `in` is only a check.
- The IPC to BServer is `{op, in len, out len}`: op 0x1F7 (503) is the normal call (in = +0x2c, out =
  +0x30, f80b6836) [V]. So 0x1F7 is not "method 3": it is the call op for every method [V].
- f80b6e20 sends op 0x1F9-family one-way message: header + TDesC8 of 8 bytes `{callback key, value}`,
  no output [V code; I that it is the `{0x1F9, 8, 0}` in bring-up item 13]. It runs after a method's
  reply is parsed and hands word 1 of the output to the app's local callback object. The garbage
  `13 00 14 00 6e 00 69 00` there is an uninitialised stack value: the empty output gave the reader
  nothing to read [I]. With the right 12 bytes it carries word 1 of our output.

## Output content

Reply parsing (IParameterStorageManager, f80e4032) [V]: begin read, read word 0 = **status** (negative =
error, skips the rest), read word 1 = value handed to the callback (f80b6e20). Word 2 is reserved but not
read there. So the output is `{status, outputs...}`: word 0 replaces the header word. For start-up:
**all zeros** (status 0 = success, value 0) is the sensible answer [I for value meaning: probably a
result code passed to the ICB* callback, so 0 = OK].

## Start-up objects

| object | CLSID | IID | interface | w0 seen | in (log) | out |
|---|---|---|---|---|---|---|
| 4096 | {65bd320f-...} | {ac0b8359-d1ab-d24e-98fd-0412ea9dae43} | ISystemControl | 5, 7 | 4, 14 [V both] | m5: 8, m7: 12 |
| 4097 | {20b6a200-...} | {f7b509b5-f9e2-a544-973b-2edb78f5bbb3} | IAudioDeviceManager | 3 | 4 [V] | 12 |
| 4098 | {e8658abf-...} | {336dbcce-23a7-d748-8a40-b6206fb47f8e} | IBatteryDeviceManager | 3 | 4 [V] | 12 |
| 4099 | {c1e7720b-...} | {0303fc15-1275-7c47-b519-587c03c29a51} | ISimManager | 5 | 4 [V] | 12 |
| 4100 | {2b688f65-...} | {be50d762-086a-2849-b72f-cde24013af71} | ISystemControlManager | 4 | 4 [V] | 12 |
| 4101 | {fa5b516a-...} | {68d3d445-a0d6-b046-88eb-a795abdcfe5c} | IParameterStorageManager | 3 | 4 [V] | 12 [V log] |

All other methods of these interfaces (in/out, header included):

- ISystemControl: m3..6 4/8, m7 14/12, m8 8/8.
- ISimManager: m3 8/114, m4 8/var, m5 4/12, m6 8/12, m7 9/12, m8 14/12, m9 8/8, m10 8/16, m11 8/18.
- ISystemControlManager: m3, m4, m5 4/12. IAudioDeviceManager, IBatteryDeviceManager,
  IParameterStorageManager: only m3, 4/12.

The app's own type-4 registrations are callback interfaces [V names from RTTI]:
{a02c3e11-...} ICBSystemControl, {27717ee3-...} ICBAudioDevice, {fb52f7db-...} ICBRadioControl,
{30650cd7-...} ICBBatteryDevice. The 44 `ICB*` proxy classes marshal nothing through this path (no
methods in the table): the CCPU calls them, their inputs are not covered here.

## C table (start-up subset; the full one is p1i-opa-methods.inc)

IID bytes are the wire order, as in CreateInstance (`m + 28`), i.e. `obj_iid[]` in syborg_ccpu.c.

```c
typedef struct { uint8_t iid[16]; uint16_t method; int16_t in, out; } CcpuOpaMethod;
/* in/out include the 4-byte header word; in -1 = variable; out -1 = variable reserve */
static const CcpuOpaMethod ccpu_opa_methods[] = {
    /* ISystemControl {ac0b8359-d1ab-d24e-98fd-0412ea9dae43} */
    {{0x59,0x83,0x0b,0xac,0xab,0xd1,0x4e,0xd2,0x98,0xfd,0x04,0x12,0xea,0x9d,0xae,0x43}, 3, 4, 8},
    {{0x59,0x83,0x0b,0xac,0xab,0xd1,0x4e,0xd2,0x98,0xfd,0x04,0x12,0xea,0x9d,0xae,0x43}, 4, 4, 8},
    {{0x59,0x83,0x0b,0xac,0xab,0xd1,0x4e,0xd2,0x98,0xfd,0x04,0x12,0xea,0x9d,0xae,0x43}, 5, 4, 8},
    {{0x59,0x83,0x0b,0xac,0xab,0xd1,0x4e,0xd2,0x98,0xfd,0x04,0x12,0xea,0x9d,0xae,0x43}, 6, 4, 8},
    {{0x59,0x83,0x0b,0xac,0xab,0xd1,0x4e,0xd2,0x98,0xfd,0x04,0x12,0xea,0x9d,0xae,0x43}, 7, 14, 12},
    {{0x59,0x83,0x0b,0xac,0xab,0xd1,0x4e,0xd2,0x98,0xfd,0x04,0x12,0xea,0x9d,0xae,0x43}, 8, 8, 8},
    /* IAudioDeviceManager {f7b509b5-f9e2-a544-973b-2edb78f5bbb3} */
    {{0xb5,0x09,0xb5,0xf7,0xe2,0xf9,0x44,0xa5,0x97,0x3b,0x2e,0xdb,0x78,0xf5,0xbb,0xb3}, 3, 4, 12},
    /* IBatteryDeviceManager {336dbcce-23a7-d748-8a40-b6206fb47f8e} */
    {{0xce,0xbc,0x6d,0x33,0xa7,0x23,0x48,0xd7,0x8a,0x40,0xb6,0x20,0x6f,0xb4,0x7f,0x8e}, 3, 4, 12},
    /* ISimManager {0303fc15-1275-7c47-b519-587c03c29a51} */
    {{0x15,0xfc,0x03,0x03,0x75,0x12,0x47,0x7c,0xb5,0x19,0x58,0x7c,0x03,0xc2,0x9a,0x51}, 3, 8, 114},
    {{0x15,0xfc,0x03,0x03,0x75,0x12,0x47,0x7c,0xb5,0x19,0x58,0x7c,0x03,0xc2,0x9a,0x51}, 4, 8, -1},
    {{0x15,0xfc,0x03,0x03,0x75,0x12,0x47,0x7c,0xb5,0x19,0x58,0x7c,0x03,0xc2,0x9a,0x51}, 5, 4, 12},
    {{0x15,0xfc,0x03,0x03,0x75,0x12,0x47,0x7c,0xb5,0x19,0x58,0x7c,0x03,0xc2,0x9a,0x51}, 6, 8, 12},
    {{0x15,0xfc,0x03,0x03,0x75,0x12,0x47,0x7c,0xb5,0x19,0x58,0x7c,0x03,0xc2,0x9a,0x51}, 7, 9, 12},
    {{0x15,0xfc,0x03,0x03,0x75,0x12,0x47,0x7c,0xb5,0x19,0x58,0x7c,0x03,0xc2,0x9a,0x51}, 8, 14, 12},
    {{0x15,0xfc,0x03,0x03,0x75,0x12,0x47,0x7c,0xb5,0x19,0x58,0x7c,0x03,0xc2,0x9a,0x51}, 9, 8, 8},
    {{0x15,0xfc,0x03,0x03,0x75,0x12,0x47,0x7c,0xb5,0x19,0x58,0x7c,0x03,0xc2,0x9a,0x51}, 10, 8, 16},
    {{0x15,0xfc,0x03,0x03,0x75,0x12,0x47,0x7c,0xb5,0x19,0x58,0x7c,0x03,0xc2,0x9a,0x51}, 11, 8, 18},
    /* ISystemControlManager {be50d762-086a-2849-b72f-cde24013af71} */
    {{0x62,0xd7,0x50,0xbe,0x6a,0x08,0x49,0x28,0xb7,0x2f,0xcd,0xe2,0x40,0x13,0xaf,0x71}, 3, 4, 12},
    {{0x62,0xd7,0x50,0xbe,0x6a,0x08,0x49,0x28,0xb7,0x2f,0xcd,0xe2,0x40,0x13,0xaf,0x71}, 4, 4, 12},
    {{0x62,0xd7,0x50,0xbe,0x6a,0x08,0x49,0x28,0xb7,0x2f,0xcd,0xe2,0x40,0x13,0xaf,0x71}, 5, 4, 12},
    /* IParameterStorageManager {68d3d445-a0d6-b046-88eb-a795abdcfe5c} */
    {{0x45,0xd4,0xd3,0x68,0xd6,0xa0,0x46,0xb0,0x88,0xeb,0xa7,0x95,0xab,0xdc,0xfe,0x5c}, 3, 4, 12},
};
```

Use: look up `(obj_iid[obj], w0)`; answer `{9, 0, req, out}` + `out` zero bytes (word 0 = status 0).
Unknown method or `out` < 0: keep answering `{9, 0, req, 0}` and log it.

## Method (to regenerate or extend)

Scratch scripts in the job dir (`tmp/opa/an.py`, `run3.py`, `gen.py`): disassemble OpaClient with
`tools/xipdis.py`; find every `bl f80b6500` (proxy constructor) with its IID literal (`ldr r1, [pc]`)
and vtable literal; for each vtable slot walk the function (and callees two levels deep), tracking which
register holds stream +0x2c / +0x30 and immediate r1/r2; collect begin/header/writer/reserve/end
sequences. A slot's header gives the method; in/out = 4 + fixed sizes.

Limits [I]: 30 methods have a variable output reserve (`out` -1, e.g. ISimManager m4) and 3 have no
output group found; sizes inside an `if` are summed on all paths (overloads all agreed, 0 conflicts).
Interface-pointer outputs (methods returning a new object) are not decoded: those methods will need
a new CCPU object id in the output, not zero.
