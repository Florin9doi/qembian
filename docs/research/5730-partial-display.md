# Nokia 5730 (RM-465 101.48.128): why the screensaver never calls ctl 3

Static reading only (2026-11-17), no run. [V] = read in the 5730 ROM code, [I] = inferred.

## Chain on the 5730

* `power_save_display_mode.dll` (532 bytes) wraps a `CDisplayDriver` from `r_display.dll`:
  NewL (ord3) = `r_display` ord4 `New(screen 0)` + vtable +8 (`r_display` ord12 = Open: load LDD, create
  channel, vt+0x78 select screen, vt+0x7c read caps into +0x18); leaves on error except -11 [V].
  Its exports go through the CDisplayDriver vtable: ord1 -> +0x10 = r_display ord42 = **ctl 3**
  {start,end,fullColour}; ord2 -> +0x1c = ord47 = **ctl 4**; ord4 -> +0x14 = ord55 = **ctl 5**;
  ord5 -> +0x18 = ord62 = caps word 8 (+32, "max partial lines") [V].
* `r_display` caps block = 0x6C bytes = 27 words, max lines read at +32 (word 8), same offset as the
  N95 [V]. Our KCaps word 8 = PanelRows = 320 on the 5730 (Install) [V].
* **The 5730 `screensaver.exe` does not import psdm ord5 at all** (imports only #1-#4) [V]. It never
  looks at the caps block, so no KCaps value can switch partial mode on or off for it.
* The `ctl 5` calls in the log come from `0x811deff7` = the screensaver *stop* path (`0x811defd0`:
  if partial active (ord4 / ctl 5) then cancel (ord2 / ctl 4)) [V; log of instance 5730 at 99.8 s and
  186.4 s, return address in the logged args].

## The gate before ctl 3: `screensaver.exe` 0x811dfde4 (SetPartialMode(this, fullColour))

    model = this[0x44]
    if (!model[+8])                      -> return -18   (screensaver not displaying) [I meaning]
    if (TRect(this+0x2bc).IsEmpty())     -> return -18   (power-save band rect, from AknLayout) [V]
    if (!0x811e16e8())                   -> return -3    [V]
    psdm ord1(model[+0x1c], rect.iTl.iY, rect.iBr.iY, fullColour)   -> ctl 3 [V]

`0x811e16e8`: `RProperty::Get(0x101F8771, 7, v)`; returns 0 (blocked) only when Get == KErrNone and
v == 1 [V]. 0x101F8771 = screensaver P&S category; key 7 is a "no power save mode" style flag [I].
The N95 screensaver (0x82dd6030) has the same model/rect gates but **no** P&S check, and chooses
rows or columns by orientation; the 5730 always passes the rect's Y range (portrait panel) [V].

Callers of 0x811dfde4: 0x811ddf3e (container, fullColour 0, when the view state != 3 or flag bit 1)
and 0x811de888 (plugin host `SetActiveDisplayArea(rect, mode)`, the rect stored at this+0x2bc by
0x811df948, fullColour = mode == 1) [V]. The default (clock/indicator) saver builds the rect in
0x811dfaba (AknLayout line 0x55, via 0x811dfdac when state != 4) [V].

## Conclusion

* No KCaps / `d_display.caps` change is needed or useful for the 5730 [V for the screensaver path].
* Which gate fails needs one live look (sydb session, not the owner's): breakpoint at `0x811dfde4`
  (and at `0x811dfe22` / `0x811dfe28` for the -3 / -18 exits); read `[[r0+0x44]+8]`,
  `r0+0x2bc` (4 words) and the P&S value 0x101F8771/7 (`sydb ps`) [I: next step].
* Secondary [V code, I effect]: the 5730 passes `iBr.iY` (exclusive bottom). A band touching the
  bottom of the screen gives end = 320 and our ctl 3 check `end < PanelRows` returns -6. Accepting
  `end <= PanelRows` (clamp to PanelRows-1) on portrait panels is safe.
