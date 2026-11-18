# The 808's per-frame full-screen VG upload: measured (2026-11-13)

Question from the owner: the 808 (RM-807, Belle FP2 113.010.1508) eats the whole CPU on animations, and
something in the window server "sends many bitmaps" per frame, where the UI is expected to be mostly
GPU rendered. Is the per-frame software bitmap + upload our own implementation's fault (a GPU surface
that we fail to reuse), at the apps or WSERV layer? Measurements below, then what is ruled out and
what is still open. Device: `models/nokia-808-rm807`, session `build/sydb-glproof`
(`SYBORG_DEVICE=nokia-808-rm807 sydb --session ... start --seed build/seeds/nokia-808-rm807`), host
GL ready (`sydb gl`: `ready=1 … GLES 3 on radeonsi`).

## Numbers (all [verified] from this session)

`sydb gl` before/after, plus the GL device's 5 s lines in `qemu.err` (`SYBORG_GL_STATS=1`); VG/EGL
function numbers decoded from `src/syvg/vg2_fn.h` and `src/syvg/egl2_fn.h`.

Idle, app grid up, **screen content pixel-identical** (frame.seq seq advanced 180 in 5 s = 36 presents/s
while only 1 distinct frame hash appeared), per 5 s:

| call | count | per frame |
|---|---|---|
| `vgImageSubData` (61) | 52-58 | **1 × 360×640 = 0.23 Mpx** |
| `vgDrawImage` (66) | 52-58 | 1 |
| `vgClear` (35) | 52-58 | 1 |
| `eglSwapBuffers` (30) | 58-66 | 1 (11-13/s) |
| `vgSeti/Setiv/SetPaint/LoadMatrix/Translate/Scale` (4,6,53,22,25,26) | ~2000 | ~35 state calls |
| `eglClientWaitSyncKHR` (38) | ~152 | constant ~32/s, **also with nothing rendering** (the sync poll, known) |

So: **the compositor uploads exactly one full-screen image and draws it once per frame, ~11.6
frames/s with the UI up — while nothing on the screen changes.** With the power key (screen blanked)
the same pattern continues at 2.2 frames/s, and with the screen off it stops (0 VG calls, only the
32/s GOOM sync poll).

Checked around it:

- Composition itself is **not** copying: GCE layers are read **in place** from guest memory
  (`in_direct` 2.7 MB, `in_copied` 0, and `in_direct` does not grow at idle — the layers are not even
  re-read).
- The host GPU is not the cost: 1000 composes took 57 ms in total (`compose_ms`), 6800 kicks 3.7 s.
  A compose's read-back is asynchronous (`async_readbacks` 1 per compose, `waited=0` nearly always).
- GPU memory accounting is nominal: 2.6 of 128 MB, by **one** process, pid 0x51 = `EwSrv.exe`.
- So the 808's compositor is **EwSrv**, not `alfredserver.exe`: that binary is not in this ROM at all,
  and `huirenderpluginvg.dll` is an ECOM plugin (nothing imports it by name). The N8 findings about
  "alf's vendor test", GOOM and its software fallback must not be carried over to the 808 without
  re-checking, even though both are Belle.

`huirenderpluginvg.dll` (rofs1, uid3 0x200184b7) analysed with `tools/e32load.py`: it links
`libOpenVG` (vgImageSubData, vgCreateImage, vgDrawImage, vgCopyImage, vgSetPixels, vgGaussianBlur,
vgMask…), `libEGL` (window surface, **eglCreatePbufferSurface**, **eglCreatePbufferFromClientBuffer**,
SwapBuffers, QuerySurface, SurfaceAttrib…) and `nvgdecoder`; it does **not** link any software
rasteriser (no bitgdi/scdv): the window content it composites is rasterised by the app and fbserv.
That is the Symbian/Belle design — the GPU is a compositor, not a widget renderer — so "the UI is
mostly GPU rendered" is true of compositing only.

## What is ruled out

- Not a full-screen **copy through our device**: composition is in-place, `in_copied` = 0.
- Not the **host GPU** being slow: per-compose and per-kick host time is microseconds.
- Not **GPU memory pressure** (the N8's software-fallback trigger): 2.6/128 MB, one client.
- Not the **EGL extension list** alone (already tried 2026-11-13: same calls, same upload).

## The `eglGetProcAddress` check (corrected 2026-11-13, later)

First reading of this was wrong: an earlier run reported "0 lookups", but that run's QEMU binary
predated the logging hunk (`build/qemu-modern/install/bin/qemu-system-arm` 16:16, the printf added
17:12, commit 726b40e) and simply could not print the lines — the 5 s stats lines it *did* print come
from an older hunk of the same `getenv`. Checked with `strings ... | grep "eglGetProcAddress(%s)"`:
absent in the old binary, 2 hits after `scripts/build-qemu-modern.sh`.

Re-run with the current binary, same session recipe, `SYBORG_GL_STATS=1 SYBORG_EGL_TRACE=1`, one boot
to the home screen:

| lookup | times | our answer |
|---|---|---|
| `eglQueryProfilingDataNOK` | 4 | 3 (EGL extension) |
| `eglCreateSyncKHR` | 2 | 3 |
| `eglDestroySyncKHR` | 2 | 3 |
| `eglClientWaitSyncKHR` | 2 | 3 |

All found; nothing image-related is looked up (the owner's longer factory-fresh run sees 16 lookups,
all answered). So: **the 808 is not steered by a missing function** — it asks only for the sync and
profiling extensions and gets them.

## What our glue answers on the eglwindow path

Guest-side log of the same boot (RDebug::Printf, serial.log):

```
 3.204 SYBCODE: Z:\sys\bin\eglwindow.dll code=7ea3c000+b40 data=0 bss=0 ep=7ea3c000
 3.662 KHR: host GPU on (owner 81, kick user)                       <- EwSrv
 3.769 KHR: window 0c002ea0 surface 3d74abfc 360x640, background 0  <- SetBackgroundSurface = KErrNone
 3.828 KHR: swap 1 window 0c002ea0 360x640 (host EGL)               (prints only the first 3 swaps)
```

and on the host side, in the same second: `eglCreateWindowSurface(...) = 1 err 3000` and
`eglSurfaceAttrib(…, 0x3093 EGL_SWAP_BEHAVIOR, 0x3094 EGL_BUFFER_PRESERVED)` = 1 — i.e. the renderer
asks for a preserved swap and gets it.

So the hypothesis "we answer 'no background surface' through eglwindow, and that pushes the renderer
into uploads" is **ruled out**: the window is 360×640 (full screen), the surface exists, and
`SetBackgroundSurface` returns KErrNone. One window surface is created, no pbuffer (matching the
"0 offscreen surfaces" in the two-ROM table).

## `eglwindow.dll`'s interface, for the next probe

`eglwindow.dll` (rofs1, uid3 0x20031ca8, 2880 code bytes, 29 exports, 40 imports) is the ROM's own
code, running in the guest (it is not replaced): the render plugin imports 7 of its exports —
ordinals 4, 11, 12, 13, 15, 16, 17. Addresses (e32load's `export_N` symbols, base 0x8000; runtime =
load address 0x7ea3c000 + (addr - 0x8000), all Thumb, odd):

| ord | link | runtime | what the first instructions show |
|---|---|---|---|
| 4 | 0x0856d | 0x7ea3c56d | indirect calls + `ws32 ord652` |
| 11 | 0x08257 | 0x7ea3c257 | (short; dumped next round) |
| 12 | 0x082e9 | 0x7ea3c2e9 | (short; dumped next round) |
| 13/14 | 0x08247 | 0x7ea3c247 | `RArrayBase::At/Count/Close`, `operator delete` — an RArray accessor |
| 15 | 0x085b7 | 0x7ea3c5b7 | literal load + call |
| 16 | 0x08671 | 0x7ea3c671 | `RWindow::SetBackgroundColor/BeginRedraw/EndRedraw` — the plain-RWindow background helper |
| 17 | 0x08593 | 0x7ea3c593 | `ws32 ord647`, `CWsScreenDevice(RWsSession&)` ctor, `ws32 ord605` — screen-device setup |

Next: `sydb dbg attach`, break on those seven runtime addresses (the DLL's load address is in the
SYBCODE line), log `$r0` at entry and exit plus a backtrace — that names what the 808's renderer
actually asks eglwindow for at start-up, which is the one channel its build has and the N8's does not.

## The two renderers, compared (808 vs N8, same uid3 0x200184b7)

`huirenderpluginvg.dll` from both ROMs, decompressed with `tools/e32load.py`: **the same offscreen
EGL surface is linked into both.** Both import 22 of the same 23 `libEGL` ordinals — including
`eglCreatePbufferFromClientBuffer` (@7, the call the N8's drawer used 40×) and
`eglCreatePbufferSurface` (@8) — and the same 48 `libOpenVG` functions. The only difference in that
set: the 808 also imports `eglWaitClient` (@33), the N8 `eglCreatePixmapSurface` (@9). The 808 never
calls the pbuffer pair at all.

The builds differ (55086 vs 54946 bytes) in everything *around* that:

| imports | 808 | N8 (011.012) |
|---|---|---|
| `eglwindow` (REglWindowBase glue) | 7 | 0 |
| `ws32` (window server) | 1 | 0 |
| `alfdecoderserverclient` | 2 | 0 |
| `nvgdecoder` / `NVGDecoder` | 5 / 0 | 0 / 5 |
| `hitchcock` (UI framework) | 341 | 309 |

and in strings: the N8's build carries `CHuiVg10RenderPlugin::ReadAllowsVisualPBufferSurfaces()` and
`ReadAllowsSwapBufferPreserved()` (with ETrue/EFalse variants) plus the effect transform parameter
names (rotation_*, scale_*, skew_*, translation_*); the 808's build has none of them [I: either
different gates or logging compiled out — the same binary-class naming is present].

So the 808's renderer is a **different build that links deeper into the window system** (EGL-window
glue + WindowServer) than the N8's. Our guest libEGL already speaks that interface — src/khr/eglos.cpp
drives `REglWindowBase`'s vtable (taken from *this* eglwindow.dll, 29 exports) via
`SetBackgroundSurface`/`SizeInPixels`/`IsValid`, which is why the UI comes up at all.

**Next probe (contained):** log the `eglwindow` and `ws32` ordinals the 808's renderer calls at
start-up and what our glue answers — that is the one channel the 808 has and the N8 does not, so it is
the likeliest place where a "this window has no visual/background surface, use a plain bitmap" answer
would silently switch the compositor to uploads.

## Is the upload expensive on our side? (measured, not assumed)

The owner's recommendation: if the full-screen composite is how Belle FP2 works, make that upload cheap
ourselves — take the bitmap straight from guest memory instead of copying it through the channel.
Checked the path in the code and the numbers from the two-ROM boot:

- The guest does **not** copy the pixels. VG calls carry the *pointer*; only inputs ≤ 4096 bytes go
  inline (`KInlineMax`, src/khr/vg2call.cpp), and a 360×640 image is far above that, so
  `vgImageSubData` takes the synchronous pointer path.
- The host reads them from guest memory with `syvg_guest_read` → `user_rw`, which reads
  `probe_access_full_mmu` + `memcpy` a page at a time straight from the host mapping
  (src/qemu-syborg/hw/arm/syborg_gl.c:1026), into a per-call `malloc` in `GuestBuf` (src/syvg/vg2.cpp),
  and only then `glTexSubImage2D`s it.
- Arithmetic from the owner's 808 boot (737 uploads, 160 Mpixels ≈ 0.87 MB each ≈ 640 MB): one memcpy
  at a few GB/s is ~0.2 s of host CPU over a 150 s boot, i.e. **under 0.5%**, plus one malloc/free of
  ~0.87 MB per frame.

So a zero-copy path (point the RI at guest memory, as `scan_upload` already does for GCE layers) is
correct but buys next to nothing; the only cheap win is a reusable buffer instead of the per-frame
malloc. The frame cost is the ROM's software drawing, as on the phone, with TCG's multiplier on top.

## The 808 does call `eglCreatePbufferFromClientBuffer` — in live code

The owner asked whether that call is dead in the 808 build. It is not: three call sites (0x9cac,
0x9ce8, 0x113c8 in `huirenderpluginvg.dll`, all inside the plugin-entry region):

- 0x9cac / 0x9ce8: wrap a client buffer as a pbuffer; on failure it compares `eglGetError()` with
  `0x3003` (EGL_BAD_ALLOC) and **retries the same call** with the setup repeated — a deliberate
  out-of-memory retry, not a fallback to software.
- 0x113c8: `eglCreatePbufferFromClientBuffer(dpy, buftype=0x3096 (EGL_OPENVG_IMAGE), image, config, 0)`
  — the standard VG-image→pbuffer path — and on failure calls libOpenVG ord 15 (vgDestroyImage) and
  returns. That is exactly the mechanism behind the N8's "68 offscreen surfaces from VG images".

So the 808 firmware contains and reaches-through code for the offscreen path; our runs simply never take
it. Combined with everything else that was ruled out (no missing extension, no missing function, glue
answering success, no GOOM switch, no stored state), the conclusion stands: **the 808's Belle FP2
renderer chooses the full-screen upload for its windows by itself**, and there is no "hardware path"
of ours being lost.

## The guards, read from the code (with one breakpoint to pick the live one)

`huirenderpluginvg.dll` loads at 0x7e980000 (SYBCODE line), so link address 0x8000 = runtime 0x7e980000.

**The per-frame upload** — `vgImageSubData` is called at 0x12e78 (runtime 0x7e98ae78, LR seen in gdb)
from the function at 0x12d4c, in the **`EwSrv.exe::alfredserver`** thread. Its guard is the first thing
it does:

```
12d54  bl   TRect::IsEmpty()        ; the update rectangle passed in (sp+20)
12d5c  cmp  r0, #0
12d60  bne  12cf0                   ; empty -> return, nothing uploaded
12d64  bl   eglGetCurrentContext    ; then: cache/rebind the window surface
12db4  cmp  r0, r1 / beq 12df0      ; same context -> no eglMakeCurrent
12df0  bl   fbscli ord250 / CFbsBitmap::SizeInPixels
12e78  bl   vgImageSubData(image, bitmap pixels, stride, format 0x42, x, y, w, h)
```

So the upload is gated by **"the window's update rect is not empty"**: any dirty rectangle makes the
compositor take the window's `CFbsBitmap` from fbserv and upload it into a VG image, then draw it. At
idle that rect is non-empty ~12 times a second, which is the whole per-frame cost story — it is not a
capability check at all.

**The offscreen path** — the function at 0x112a4 contains the `eglCreatePbufferFromClientBuffer(
EGL_OPENVG_IMAGE)` call (0x113c8) and guards it with:

```
112c4  ldr r0, [r0, #0x154] / cmp r0,#0 / beq return     ; a sub-object must exist
112d0  ldr r1, [vtable + 0x4c] / blx r1
112e0  cmp r0, #1 / bne skip                            ; that virtual must return exactly 1
112e8  ldr r2, [vtable + 0x50] / blx r2 (arg 0)
11300  bne return                                       ; and that one must return 0
11304  cmp sl,#0 / cmpne r7,#0 / beq return             ; image and format non-null
113c8  bl eglCreatePbufferFromClientBuffer(EGL_OPENVG_IMAGE)
```

Setting a breakpoint on that function's entry (0x7e9892a4) and running the home screen for 15 s: **it
never fires**. So this guarded path is armed only for some other feature (effects/video/overlays), not
for windows — the compositor's window path is the `IsEmpty()` one above.

(This also explains why "off-screen surfaces" and "full-image uploads" are complements in the two-ROM
table: the N8's renderer reaches the pbuffer/offscreen path for window content, the 808's reaches the
`CFbsBitmap` upload path, and both builds contain both.)

## What is still ours to fix, and what is still open

Missing on our side, each a plausible way to make the upload unnecessary — the "reuse the GPU
surface" the owner asked about:

1. `eglCreateImageKHR` / `EGL_KHR_image_base` is not in our guest `eglGetProcAddress` table
   (`src/khr/egl2.inl`), and `EGL_NOK_pixmap_type_rsgimage` is unadvertised: no zero-copy import of a
   client buffer (an RSgImage / fbserv bitmap) into a VG image or GL texture. With it, EwSrv could
   sample the window bitmap directly instead of `vgImageSubData`-ing it each frame.
2. Of the phone's 14 EGL extensions we advertise 4 (`[gl.identity] egl_extensions`).
3. `gcebe` does no off-screen composition (`model.toml` `open`: transition effects), so effect frames
   cannot be composed by us.
4. **Open: what paces the ~11.6 frames/s idle loop.** It continues (slower) with the screen blanked and
   only stops when the screen is off, which says "screen-on redraw loop", not "content changed". Two
   candidates, one experiment each:
   - ours: our present/vsync completion re-arms the compositor's redraw. Test: change the frame timer
     rate at run time (`qom-set` on `syborg-fb.refresh-rate`) and see whether the swap rate follows; or
     attribute each `eglSwapBuffers` to a guest thread and correlate with our presents.
   - the ROM's own animation scheduler (~100 ms tick). Test: same measurement on the N8/E7 (same Belle
     line, different SoC) — if they also re-draw an unchanged screen, it is ROM behaviour.
   Until that is settled, "we lose a GPU path" is not established for the 808; what is established is
   that the ROM re-uploads one full-screen VG image per frame at ~12 Hz on a static screen, and that
   our stack neither copies it nor slows the GPU work down.

## RESOLVED 2026-11-13: our EGL memory-profiling reply put ALF into software mode

The upload storm above was **ours**, not Nokia's. It is fixed in `src/syvg/syvg_profiling.h` (used by
`syvg_egl.cpp`, and therefore by QEMU and by the hosted GL server, which compile the same code).

### Chain, with the evidence for each link
1. **`EwSrv.exe::GOOM FW` → the ALF app server, IPC `0x10cc` = `EAlfEnableLowMemoryState`, arg0 non-zero,
   at 3.9 s after boot.** Caught in the kernel with a temporary print in `DSession::Send`
   (sender, all four args, and the target server name): `sender=EwSrv.exe::GOOM FW
   server=10282845_10282845_AppServer`. Printed for both `0x10cc` and `0x10cd`.
2. **The handler forces the lowest memory level**: `CAlfAppSrvSession::EnvEnableLowMemoryState` →
   `CAlfAppUi::NotifyLowMemory(arg)` (`alfappsrvsession.cpp`, `alfappui.cpp`) → because the argument is
   non-zero, `CAlfBridge::SetMemoryLevel(EHuiMemoryLevelLowest)`. Caught by breakpoint: `r1 = -40`
   (`EHuiMemoryLevelLowest`), `r3 = 0x10cc` (the opcode) in `alfappservercore.dll` (this ROM:
   `SetMemoryLevel` at `0x1198a`, `DoUpdateMemoryLevel` at `0xf006`, `ForceSwRendering` at `0x11998`,
   `EnableSwRenderingL` at `0xed6e`; Thumb-1, image link base 0x8000).
3. **Lowest memory level = software rendering.** `DoUpdateMemoryLevel()` has exactly one branch that
   calls `EnableSwRenderingL(ETrue)` (level ≤ `EHuiMemoryLevelLowest`) and one for `(EFalse)`. That
   creates the 360×640 `EColor16MAP` target and makes it `CHuiDisplay`'s foreground bitmap
   (`SetForegroundBitmapL`), so every dirty rect is `PartialBitmapUpload`ed.
4. **The upload site itself**: `hitchcock.dll 0x7ec8a830` (`ldr r3,[r1,#36]; blx r3`, the string's
   texture-extension slot) → `huirenderpluginvg.dll 0x12f00` (`TextureExtension`, UID `0x2002C359`) →
   `PartialBitmapUpload 0x12c44` → `vgImageSubData(image, data, stride, VG_sRGBA_8888 = 0x42, 0,0,360,640)`
   at `0x12e78`. Confirmed live: the upload runs in `EwSrv.exe::alfredserver`, `params.iRect` is the full
   screen, `params.iBitmap` points at an `EColor16MAP` bitmap (display mode 13, checked at `0x12cf8`).

### Why GOOM decided the phone was low on graphics memory
`CMemoryMonitor::GetFreeMemory()` (`goommonitor/src/goommemorymonitor.cpp:617`) asks
`eglQueryProfilingData(EGL_PROF_QUERY_GLOBAL_BIT_NOK|EGL_PROF_QUERY_MEMORY_USAGE_BIT_NOK, ...)` and then
scans the reply for `EGL_PROF_TOTAL_MEMORY_NOK` / `EGL_PROF_USED_MEMORY_NOK`; free = total − used, and it
returns 0 when either is missing or when used > total. The verdict is `free <= iLowThreshold`
(`goomconfig.xml`, `Private/10003b20/`: global `lowRam_threshold`, per-app overrides).

Our reply only contained that pair when the caller set `EGL_PROF_QUERY_GLOBAL_BIT_NOK` (0x1). GOOM also
queries with `EGL_PROF_QUERY_MEMORY_USAGE_BIT_NOK` (0x2) alone — and on that query it got **no totals at
all**, i.e. total = used = 0, free = 0, "low graphics memory", ALF to `EHuiMemoryLevelLowest`. The second
defect was in the per-process entries: `PROCESS_ID` was sent as two words and each figure with a padding
word, while both GOOM parsers (`GetFreeMemory`, `CGOomWindowGroupList::RefreshL`) advance one word per
value on a 32-bit build.

### Fix
`src/syvg/syvg_profiling.h`: TOTAL and USED are sent on **every** query, whatever the bits; per process
entries are three token,value pairs with one word each (`0x3072 id 0x3073 private 0x3074 shared`).
`syvg_egl.cpp` uses it; the reply stream and the reported count are logged under `SYBORG_GL_STATS`.
Regression test: `tests/profiling_stream_test.py`.

### Measured effect (808, FP2 113.010.1508, factory seed, 5 s intervals)
| | before | after |
|---|---|---|
| `EAlfEnableLowMemoryState` from GOOM | 2 per boot | 0 |
| `vgImageSubData` | 4.6–8 Mpx per interval | **0.0** (0.4–2.6 when an app uploads content) |
| `eglCreatePbufferFromClientBuffer` | never | used (3+ intervals) |
| screen | correct | correct (home screen, widgets, wallpaper, icons) |

Both firmwares were affected identically (FP1 112.020.0311 and FP2), which is why the earlier
"Nokia dropped the GPU renderer" reading was wrong: FP1 and FP2 look the same because the *environment*
put both into software mode. The renderer's `CHuiVg10PBufferSurface`/`CHuiFxVg10TransformFilter`
removal in FP2 is real but was never the cause: the N8 keeps GPU composition with the same profiling
fix, and FP1 (which still has those classes) was uploading too.

### Rebuilding
Host-side only — no standin, ROM or firmware change:
- desktop QEMU: `scripts/build-qemu-modern.sh`
- hosted EKA2: `scripts/build-glserver.sh` (the hosted server includes `syborg_gl.c`, hence `src/syvg`)
- the Android app: `android/native/build.sh all`, then `(cd android && ./gradlew assembleDebug)` and
  `adb install -r android/app/build/outputs/apk/debug/app-debug.apk`. The app runs QEMU in-process
  (`libqemu-syborg.so`), so a stale library means the phone app keeps booting the 808 in software
  rendering. The clang/NDK build also needed a shadowing `extern` removed from `syvg_egl.cpp`
  (GCC accepts it, clang rejects it with "different language linkage").
