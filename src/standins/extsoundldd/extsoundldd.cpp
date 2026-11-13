// Copyright (c) 2026 qembian contributors. Licensed under the Eclipse Public License v1.0.
//
// Sony Ericsson P1i ExtSoundLdd.ldd stand-in: "EXT_SOUND_DRV", the sound output channel the start-up
// animation player (system_animation_player.exe, ROFS1) and A2dp.dll open. The player panics "Could not
// load SoundLDD" when the LDD is missing, and APS (startuphandler.aps) dies after it [V R6E30 boot].
// Stand-in H-p1i-sound-silent (docs/hacks.md): every control returns KErrNone, every request completes
// at once with KErrNone, nothing is played; each call is logged ("SYBSTUB:") as the specification
// for a real sound path later. Player calls seen statically [V]: controls 0, 2, 3, 4, 5, 7, 8, 9, 10,
// 11, 46 (cancel), requests 0..5 (5 is waited for synchronously).
#include "nokiastub.h"
NO_CONTROLS
NOKIA_STUB_LDD2("EXT_SOUND_DRV", KCtl, 1, ETrue)
