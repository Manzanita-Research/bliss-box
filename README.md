# Bliss Box

A firmware for the M-VAVE FM-1 that turns it into a slow, cosmic new age instrument: the 1980s cassette kind,
Steven Halpern and Iasos and the shop by the crystal counter. It plays a bed by itself for as long as you like,
lets you play over it, and runs it all through a cassette deck you can wear down.

> **Status: prototype.** It runs in the host simulator and in [fm1-emu](https://github.com/joakimeriksson/fm1-emu)
> with cycle timing. It has not been on hardware yet.

## What it does

- **Grove**: a self-playing piece, 5 to 30 minutes or endless, that drifts through seasons (DAWN, TEMPLE,
  GARDEN, STARS, STILL, RIVER) over pads, pedal tones, keys, harp, bells and flute. It's made to sit under a
  10–20 minute video.
- **Sting**: a seeded cue of 3, 5, 10 or 30 s for idents and transitions. The same seed always gives the same sting.
- **Solo**: play over the bed with nine voices: Rhodes, CP-80, flute, harp, zither, bells, strings, choir, glass.
- **Rooms**: seven keys named for the chakras (ROOT C … CROWN B), with modes (pentatonic, Lydian, major, Dorian),
  just intonation, and A=432 for the schtick.
- **Space**: plate-ish reverb, echo, shimmer, tone.
- **Cassette** on the master: pre/de-emphasis, saturation, head bump, wow, flutter, drift, hiss, and dropouts as
  it wears. WEAR runs from fresh chrome to the thrift-store bin. Tap SELECT to bypass it.
- **TALK**: a gentle 1–4 kHz dip that leaves room for your voice. Tap GLO.
- **Four screen looks**: Redwood, Spectrum, J-card, and Painted (an airbrushed sky for each season of the Grove, dissolving as it turns; `tools/paint.py` makes them from `renders/painted/src`). Hold HOME and turn SELECT.

## Layout

| path | what |
|---|---|
| `app/dsp/` | the engine, portable C: voices, harmony, Grove, Sting, effects, cassette |
| `app/app.c` | the FM-1 app: panel, screens, MIDI (on [fm1-base](https://github.com/charlesvestal/fm1-base)) |
| `base/` | fm1-base (submodule) |
| `tools/render.c` | offline renders to WAV: `build/render grove 600 out.wav room=3 wear=0.4` |
| `tools/stress.sh`, `prof.sh`, `emu.sh` | load tests and profiles in fm1-emu |
| `research/` | notes on the scene and its techniques |

## Build

    git submodule update --init
    tools/fw.sh            # app/build/blissbox.fwsc (JieLi toolchain, Docker on macOS)
    tools/fw.sh --host     # the host simulator
    clang -O2 -std=gnu11 -DBL_OFFLINE -I base/include -I app/dsp tools/render.c app/dsp/bliss.c -o build/render -lm

## Licence

MPL-2.0, like fm1-base. Copyright © 2026 Manzanita Research.
