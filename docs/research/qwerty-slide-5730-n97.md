# QWERTY slide on the 5730 and N97: what the UI needs (mock-level)

Static ROM reading only (5730 RM-465 101.48.128, N97 RM-505 v20.0.019). [V] = read in the ROM,
[I] = inferred. Key/scan code numbers come from `e32keys.h` and Nokia's alias header
`external/oss.FCL.sf.os.graphics/windowing/windowserverplugins/keyeventrouting/src/keyaliases.h`
(EStdKeyGripOpen = EStdKeyDevice4, EStdKeyGripClose = EStdKeyDevice5, EKeyFlipOpen = EKeyDeviceA,
EKeyFlipClose = EKeyDeviceB, EStdKeyQwertyOn/Off = EStdKeyApplication7/8,
EStdKeyForward/Rewind = EStdKeyApplication4/5) [V].

## Neither phone has the slide in the keypad matrix

- The 5730's keypad_exp.dll matrix (0x8010ba3c) has no Device4/5/A/B or Application7/8 entry, and
  neither keypad_exp.dll has these codes as immediates or table words. Same for the N97's
  keypad_exp.dll [V]. The slide is a GPIO switch (probably `switch.dll` in both kernels; not read)
  [I]. A host button therefore only has to make the events that come after it.

## Nokia 5730 (S60 3.2)

Chain on the phone:
1. Slide switch -> grip key EStdKeyDevice4 (0xA8, open) / EStdKeyDevice5 (0xA9, close). The Context
   Framework windowserver source listens for keycodes 0xF846/0xF847, which its config
   (`private/10282BC4/settings/10282C75/10282C75.xml`) calls "smart phone open/close event
   EStdKeyDevice4/5" [V for the config, I that the driver sends exactly these].
2. HWRM grip status: P&S KPSUidHWRM 0x10205041, key 0x102 (KHWRMGripStatus [I name]),
   1 = open, 2 = closed [I values, see 3]. HWRMServer and SysAp both reference 0x10205041 [V];
   HWRMServer is presumably the publisher [I].
3. SysAp (core, Thumb) has a property handler at 0x819e853c: `if key == 0x102 { value 1 ->
   f(open=1); value 2 -> f(open=0) }`; f at 0x819e6c10 builds a TKeyEvent with
   iCode = 0xF84C (EKeyDeviceA = EKeyFlipOpen) for open, 0xF84D (EKeyDeviceB) for close and
   simulates it [V code, I that it goes through WsSession().SimulateKeyEvent].
4. EKeyDeviceA/B are the wsini.ini `S60_HWSTATE_KEYCODE1/0` keys: DeviceA -> screen mode 2
   (320x240, 90 degrees, softkeys right), DeviceB -> mode 1 (240x320) [V]; akncapserver switches the
   layout (as on the N95 media slide) [V on N95].
5. AknFep: whether it needs anything else to use the hardware QWERTY was not found
   (no Avkon CenRep UID literal in SysAp) [I: it probably reads the grip state or the key events].

Recipe:
- open: `sydb prop set 0x10205041 0x102 1` (SysAp then sends EKeyDeviceA -> landscape), and tap
  EStdKeyDevice4 (0xA8) for the CF/keylock rules.
- close: `sydb prop set 0x10205041 0x102 2` (-> EKeyDeviceB -> portrait), tap EStdKeyDevice5 (0xA9).
- Fallback (what keymap.toml does today): tap EStdKeyDeviceA / EStdKeyDeviceB yourself. That gives the
  landscape/portrait switch but leaves HWRM grip state (and whatever reads it) at "closed".
- Caveat: `prop set` works only if HWRMServer has defined the property; if it is not defined, it has
  to be defined first (category is HWRMServer's own SID, so a stand-in or the HWRM side defines it) [I].

Media keys (matrix indexes in keypad_exp.dll table 0x8010ba3c) [V codes]:

| matrix | scan code | value | meaning by Nokia alias |
|---|---|---|---|
| 105 | EStdKeyApplication2 | 0xB6 | EStdKeyPlay (play/pause) |
| 110 | EStdKeyApplication4 | 0xB8 | EStdKeyForward (next) [I: by alias] |
| 104 | EStdKeyApplication5 | 0xB9 | EStdKeyRewind (prev) [I: by alias] |
| 71 / 72 | EStdKeyApplication1B / 1C | 0xE4 / 0xE5 | unknown (maybe the music/Multimedia key) [I] |

keymap.toml has prev = Application4 and next = Application5; by the aliases it is the other way round
[I]. No EStdKeyApplication3 (Stop) in the matrix [V].

## Nokia N97 (S60 5th)

Chain on the phone:
1. Slide -> EStdKeyApplication7 (0xBB, EStdKeyQwertyOn) on open, EStdKeyApplication8 (0xBC,
   EStdKeyQwertyOff) on close -> keycodes 0xF859 / 0xF85A. CF windowserver source config
   `private/10282BC4/settings/10282C75/ivalo_10282C75.xml`: `0xF859 KeyQwertyOpen
   (BSW_EStdKeyApplication7)`, `0xF85A KeyQwertyClosed (BSW_EStdKeyApplication8)` [V].
2. Rule `private/10282BC4/rules/ivalo_Qwerty.rul` [V]: on KeyQwertyOpen it publishes context
   Device.Qwerty = Open, runs `<Orientation Mode='Alternate'/>` (OrientationActionPlugIn ->
   landscape), `<QwertyOpenActions/>` and the audio route. On KeyQwertyClosed: Device.Qwerty = Closed,
   QwertyClosedActions, and `Orientation Mode='Normal'` only when auto-rotation is off (otherwise the
   sensor decides). At CF start it replays the state from Device.Qwerty (initial "Closed",
   `ivalo_10282C79.xml`) [V].
3. Keyguard: `ivalo_DevLock_qwerty_closed_exceptions.rul` locks keys when the slide closes unless an
   exception applies [V].
4. On-screen keyboard suppression: not located (AknFep/peninput are in ROFS, not read) [I: they read
   the QWERTY key events or a CenRep/P&S state set by the QwertyOpenActions plugin].
5. HWRM grip P&S: only HWRMServer references 0x10205041 in the N97 core; SysAp is in ROFS [I: the same
   0x102 key may exist].

Recipe:
- open: tap EStdKeyApplication7 (0xBB) (down/up).
- close: tap EStdKeyApplication8 (0xBC).
- Optional, harmless: also `sydb prop set 0x10205041 0x102 1/2` [I].
- Needs the CF server and its windowserver source plugin running; if the layout does not change,
  check CF first.

Note: `models/nokia-n97-rm505/keymap.toml` is a stale copy of the 5800 keymap (`device =
"nokia-5800-rm356"` in the file) [V]; the N97's own keypad_exp.dll table has not been read.

## Confidence

- 5730 landscape via P&S 0x10205041/0x102 -> SysAp -> EKeyDeviceA: medium-high (code read; P&S
  values 1/2 read from the compare, names inferred).
- 5730 grip scan codes 0xA8/0xA9: medium (CF config comment).
- N97 0xBB/0xBC -> CF rule -> landscape: high for the rule chain, medium for the driver sending exactly
  those scan codes (the CF comments say so) and for the scan code -> keycode mapping being standard.
- AknFep / virtual keyboard on N97: unknown.
