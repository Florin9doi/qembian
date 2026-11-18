# P1i: CCPU -> app calls into registered callback objects (ICB*)

Static research (R6E30 core image, base 0xf8000000). Marks: [V] read in the code, [I] inferred.
Goal: just enough to make the fake CCPU (src/qemu-syborg/hw/arm/syborg_ccpu.c) call
`ICBSimLockStatus` after SecurityApp's `ISimLock` m5, so start-up can continue.

## 1. Wire format of a CCPU -> app call

BServer's incoming-request dispatcher f81a6e40 (switch8 at f81a6f0c) sends incoming type 8 to the
handler object at `+0x1c` of its handler set [V]. That handler is **`CACPU_RemoteInvokeSession`**
(RTTI `25CACPU_RemoteInvokeSession`, vtable thunk f81ad428, slot 0 f81a6005 -> f81a5f2c) [V]. Its
outgoing twin `CCCPU_RemoteInvokeSession` (f81a61c2, waits for type 9) is the app -> CCPU call [V].

f81a5f2c, with the received header at `this+0x34..` [V]:

| word | offset | use |
|---|---|---|
| 0 | +0x34 | type, must be **8** (else no dispatch) [V] |
| 1 | +0x38 | sender; not read on this path [V not read here; I: put our handle V] |
| 2 | +0x3c | request id, copied to `this+8` and echoed in the reply [V] |
| 3 | +0x40 | **target**: looked up through `[this+0xc]->vtbl[+0x30]`; miss -> reply status **2** [V] |
| 4 | +0x44 | argument length n [V] |
| 5.. | msg+0x14 | n argument bytes, copied verbatim and handed to the target (`target->vtbl[1](target, args)`) [V] |

The argument bytes are the stub's input stream; the CICB stub reads **word 0 = method number** first
(f80c839a: f80bb0ac begin read, f80bb134 read 4) [V], exactly like the app -> CCPU direction where
`m+20` is the method and `arg length` counts it [V in our bs_call].

Target value [I, best guess]: **the last word `n` of the app's type-4 message** (0x3e in
`04000000 a8d2726d 0825736d 01000000 <iid> 3e000000`). Reasoning: in the app -> CCPU direction the
target word is the CCPU's own object id (the last word of *our* type 4) [V boot]; BServer's outgoing
type 4 (`CACPU_MarshalInterfacePtrSession`, f81a4e18) puts in that last word `(proxy+0x1c)->vtbl[4]()`,
i.e. its own id for the app's object [V code, I meaning], and the invoke handler looks it up through a
different manager (`[this+0xc]`) than the CCPU-object key table (`[this+8]`, keyed by proxy pointers,
f81a6ae6) [V fields, I that they differ in kind]. Fallback if BServer answers status 2: use word 1 of the
type-4 (0x6d72d2a8, pointer-like) as the target.

### The reply (app/BServer -> CCPU) [V f81a5e76]

```
09000000 <status> <req (our word 2)> <out length> <outputs...>
```
status 0 = delivered (stub ran), **2** = target not found (f81a5f62), **0x13** = no memory (f81a5f86) [V].
The CICB stub answers with a 12-byte output reserve (`f80b69ba(this, 12)`) **before** it calls its
observer [V f80c8386]. We only have to accept/log it (today `case 9` from the app falls to "not answered":
harmless, but log status to see 2 vs 0).

Note: `f80c839a` returns without dispatch (and without reply) for any method other than 3, and with
error reply 12 if `f80b672e` fails (stub not ready) [V].

## 2. ICBSimLockStatus (stub CICBSimLockStatus, vtable 0xf810e64c)

- Dispatch = vtable slot 4 f80c839a [V]: only **method 3** exists [V].
- Method 3 (f80c8368): reads `a` (4 bytes, f80bb156) then `b` (4 bytes, f80bb11e), ends the read,
  replies (12), then calls `observer(this+0x84)->vtbl[+0xc](observer, a, b)` [V]. Observer is the second
  argument of the NewL f80c8428 (`(owner, observer)`, ctor f80c83ec stores it at +0x84) [V].
- So the arguments are `{03000000, a, b}`, n = 12 [V layout].
- Meaning of a/b [I]: MICBSimLockStatus callback `(status/result, lock state)`. Best guess for
  "no SIM lock, carry on": **a = 0, b = 0**. If start-up still waits, try b = 1, then a = 0 / b = 2.
- Who creates it [V]: only through OpaClient **ordinal 75** (f80ff7ba -> generic factory f80ff7e6 with
  NewL f80c8429). Neither Security.dll (imports OpaClient 26, 46, 64, 115, 271, 283, 294) nor SecurityApp
  (121) imports #75, so the observer lives in another DLL loaded in SecurityApp [I]; its `+0xc` slot was
  not followed to `MSecurityPhoneObserver` (SecurityApp 0xac92/0xad3c/0xad5e). On a boot: `sydb dbg` hw
  breakpoint at f80c8368 shows the observer (`[r0+0x84]`) and its vtable; break at its `+0xc` target to
  see which value of a/b reaches 0xac92 [I procedure].

## 3. ICBPhoneLockStatus

Not done (budget). Same mechanism: find its stub vtable via RTTI `N5OPAIF19CICBPhoneLockStatusE`, slot 4
= dispatch (method numbers + `f80bb1xx` read sizes), observer at +0x84 [I by analogy].

## 4. Bytes the fake CCPU sends

After answering `ISimLock` m5 (on the same link), send:

```
<link> 00 | 08000000 <V> <R> <N> 0c000000 03000000 <a=00000000> <b=00000000>
```
V = our handle from the type-5 answer to the ICBSimLockStatus type 4 (0x13d on the log) [I unused],
R = a fresh request id of ours, N = the type-4's last word (0x3e) [I], fallback the type-4's word 1.
Expect `09000000 00000000 <R> ...` back; `status 2` = wrong target.

## 5. C sketch (syborg_ccpu.c)

```c
#define CCPU_CBS 16
typedef struct { uint8_t used, link; uint32_t unit, app_obj, app_w1, our_handle; uint8_t iid[16]; } SybCcpuCb;
/* in SybCcpu: SybCcpuCb cb[CCPU_CBS]; uint32_t cb_req; */

static const uint8_t iid_icb_simlock_status[16] =   /* {23b8d43b-6c37-42b2-a0a2-25e1c58306c3} wire */
    {0x3b,0xd4,0xb8,0x23,0x37,0x6c,0x42,0xb2,0xa0,0xa2,0x25,0xe1,0xc5,0x83,0x06,0xc3};

/* case BS_REGISTER: remember { 4, w1, req, 1, IID, n } before answering 5 */
static uint32_t bs_app_register(SybCcpu *c, uint32_t unit, uint32_t link, const uint8_t *m, uint32_t n)
{
    uint32_t v = c->next_value++;
    if (n >= 36) {
        for (int i = 0; i < CCPU_CBS; i++) {
            SybCcpuCb *s = &c->cb[i];
            if (!s->used) {
                s->used = 1; s->unit = unit; s->link = link;
                s->app_w1 = rd32(m + 4); s->app_obj = rd32(m + 32); s->our_handle = v;
                memcpy(s->iid, m + 16, 16);
                break;
            }
        }
    }
    return v;
}

/* CCPU -> app: { 8, V, R, N, 4 + 4*argc, method, args } */
static void bs_callback(SybCcpu *c, const SybCcpuOut *out, const uint8_t *iid, uint32_t method,
                        const uint32_t *args, uint32_t argc)
{
    for (int i = 0; i < CCPU_CBS; i++) {
        SybCcpuCb *s = &c->cb[i];
        if (!s->used || memcmp(s->iid, iid, 16)) continue;
        uint8_t pkt[2 + 24 + 4 * 8];
        memset(pkt, 0, sizeof(pkt));
        pkt[0] = s->link;
        wr32(pkt + 2, BS_CALL);
        wr32(pkt + 6, s->our_handle);
        wr32(pkt + 10, 0x5a000000u | c->cb_req++);
        wr32(pkt + 14, s->app_obj);                   /* fallback: s->app_w1 if the reply says status 2 */
        wr32(pkt + 18, 4 + 4 * argc);
        wr32(pkt + 22, method);
        for (uint32_t k = 0; k < argc && k < 8; k++) wr32(pkt + 26 + 4 * k, args[k]);
        ccpu_log(out, s->unit, "callback object %u method %u (%u args)", (unsigned)s->app_obj, (unsigned)method, (unsigned)argc);
        out->send(out->ctx, s->unit, pkt, 2 + 24 + 4 * argc);
        return;
    }
}

/* in bs_call, after the reply to ISimLock {c3e250a5-...} m5 has been sent: */
/*     static const uint32_t ok[2] = {0, 0};   a = status 0, b = lock state 0 [I]          */
/*     bs_callback(c, out, iid_icb_simlock_status, 3, ok, 2);                              */

/* case BS_CALL_R (9) from the app: log { 9, status, req, outlen } (status 2 = unknown target) */
```

Test (tests/, host-side SYBCCPU_TEST): type 4 with the ICBSimLockStatus IID and n = 0x3e, then an
ISimLock m5 call -> expect the 9 reply followed by `08000000 <V> <R> 3e000000 0c000000 03000000 0 0`.
