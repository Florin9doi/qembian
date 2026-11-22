# Recording: screen + sound to MP4 (avrec)

One recorder for every QEMU phone of two projects: qembian (`src/qemu-syborg/ui/avrec.c`) and k750i
(`src/qemu-db2010/ui/avrec.c`). The two copies are **identical by design** (duplicated, not shared): change
one, copy it over. The QEMU-side patch (QAPI type, GTK menu item, meson line) is identical too:
qembian `patches/qemu-modern/0010-avrec-recorder-ui.patch`, k750i `src/qemu-patches/0002-avrec-recorder-ui.patch`.

## Use

| Where | How |
|---|---|
| sydb phone window (qembian) | **Record** button or Ctrl+R; press again to stop. File: `~/Videos/<instance>-<date>-<time>.mp4` |
| sydb CLI (qembian) | `sydb record` (toggle), `sydb record start [OUT.mp4]`, `sydb record stop`, `sydb record status` |
| QEMU GTK window (k750i `qkeys.py --gtk`, qembian `run-modern.sh --gui`) | Machine > Start/stop recording, or Ctrl+Alt+R. File: `$AVREC_DIR` or `~/Videos` (else the current directory), `<-name>-<date>-<time>.mp4` |
| QMP (any front end) | `object-add qom-type=avrec id=rec path=/abs/x.mp4 [fps=30] [scale=1] [audiodev=ID] [console=N]`, then `object-del id=rec`; `qom-get /objects/rec recording` |
| Command line | `-object avrec,id=rec,path=x.mp4` records from start-up (stops when QEMU exits) |

Stop returns at once; ffmpeg finishes the file in the background (QEMU prints `avrec: saved PATH (N s)`).

## Output

MP4, H.264 (`libx264 -preset veryfast -crf 18`, yuv420p, keyframe every 2 s) + AAC 192k 48 kHz stereo,
`-movflags +faststart` (the index is at the front: plays while downloading). That is what Reddit, YouTube,
Telegram, Discord etc. take as uploaded. Not lossless. 30 frames/s by default (`fps=`).

- **Picture**: the QEMU console (default: the first graphic one) exactly as the console shows it, sampled on
  the host clock: what you see, including pauses (the last picture repeats). Odd sizes get one black
  column/row (yuv420p needs even sizes). If the console changes size while recording (E90 lid, a resize),
  the new picture is fitted into the first size, centred, with black borders.
- **Sound**: the final mix of the machine's audiodev (`-machine audiodev=`, or `audiodev=`), taken from QEMU's
  audio capture hook (the one `wavcapture` uses): every sound the phone plays through it, as you hear it.
  qembian: syborg-snd (PCM, MP3/AAC, the modem's key tones). k750i: k750i_snd (MIDI synth, tones). Any new
  sound source must play through that one audiodev to be recorded. Time without sound (idle, machine
  paused) is filled with silence on the host clock, so sound stays in step with the picture.
- **Upscaling** (not on by default): `scale=N` (QMP/`-object`) has ffmpeg enlarge N times with sharp
  pixels (`scale=iw*N:ih*N:flags=neighbor`). Sites shrink and blur small videos, and chroma subsampling
  smears coloured text on a 176x220 picture; for uploads `scale=4` (or 5-6 to reach 1080 lines) looks much
  better. Not exposed in the buttons yet.

## Cost

Nothing exists while not recording (no listener, no capture, no thread). While recording:
- one extra display listener: per refresh, one copy of the console picture (pixman, < 1 MB), on the main
  loop. It also asks the board for a display update each refresh, like a window would; with a window open
  that is a second update per refresh (a frame buffer copy). With `-display none` (sydb sessions) it is
  what makes syborg-fb compose at all.
- the GPU path is untouched: syborg-gl already reads its composed frame back for syborg-fb; the recorder
  copies what the console already holds.
- the encoder is a separate process (`ffmpeg`, its own process group) fed through two non-blocking pipes by
  a detached writer thread. The emulator never waits for it: the picture is "latest frame", sound waits in
  a 2 s ring. If ffmpeg is slower than real time the file is late, never out of sync (frames and samples are
  counted against the host clock when queued).

## Failure modes

- No ffmpeg on the host: `object-add` fails with `avrec: cannot run ffmpeg: No such file or directory` (stderr,
  QMP error, the phone window's status line); nothing else changes. `AVREC_FFMPEG=/path/to/ffmpeg` picks a
  binary; `AVREC_VCODEC=` another encoder name (libx264 missing in a minimal ffmpeg build: `libopenh264`).
- ffmpeg dies while recording: `avrec: recording PATH failed (reason; ffmpeg exit N; ...)`; the next Start
  clears the dead recorder and starts a new file.
- QEMU exits or is killed while recording: the pipes close, ffmpeg (not in QEMU's process group, so Ctrl-C in
  the terminal does not reach it) finishes the file with what it got.
- Only one recording at a time per QEMU.

## Not covered

- sydb phone's **Turn view** is a window-side rotation: the recording shows the phone's own picture.
- EKA1 boards (`src/qemu-ngage`, own QEMU tree) and hosted EKA2 (`tools/hosted/phone.py`) have no avrec:
  the ngage tree needs the file + patch in `scripts/build-qemu-ngage.sh` (picture only; those boards have
  no sound device).
- A zero-copy GL display (a texture straight to the window, syborg-gl `present=window`) bypasses the
  console: the recorder would then need `dpy_gl_scanout_texture` + an asynchronous read-back.

## How it was checked (2026-11-20)

No automated tests (owner's call: needs a host ffmpeg, a display and a phone). Checked by hand: QMP start/stop
on a stock machine (3.0 s file, `moov` before `mdat`, second recorder refused, missing ffmpeg reported);
K750i `sound-play=tone:1000:1500` 1 s after start -> tone at 1.003-2.502 s in the file; N95 session: key
clicks in the file at the press times, peak level equal to the audiodev's own output (-9.8 vs -10.1 dB),
frames identical to `screendump` (lit idle, Menu note, screensaver); three recordings in a row; GTK
Ctrl+Alt+R start/stop under Xvfb.
