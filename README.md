# Bliss Box

A firmware for the M-VAVE FM-1 that turns it into a slow, cosmic new age instrument: the 1980s cassette kind,
Steven Halpern and Iasos and the shop by the crystal counter. Pick a mood and it plays a piece by itself for as long as you like,
lets you steer it and play over it, and runs it all through a cassette deck you can wear down.

> **Status: prototype.** It runs in the host simulator and in [fm1-emu](https://github.com/joakimeriksson/fm1-emu)
> with cycle timing. It has not been on hardware yet.

## What it does

- **Moods**: DAWN, TEMPLE, REDWOODS, STARS, STILL and RIVER. A mood is a key, a scale, a few written
  progressions and the voices of four parts. Turn SELECT to choose one; the screen paints its sky.
- **Grove**: the mood played as a piece, 5 to 60 minutes or endless. It has a shape: it arrives, blooms, floats,
  blooms again and comes home to its first chord at the end. Four parts play all the way through: the **pad**
  (strings, choir or glass), the **low** part under it, the **motion** (harp ripples, the CP-80's or the Rhodes'
  broken chords, a strummed zither, sparks of bells) and a **melody** (flute phrases or bell motifs). The HOME
  knobs are their levels. Each pass through a progression strays a little (DRIFT), and **BLOOM** opens and closes
  the sounds as the piece moves: string filters, FM brightness, the choir's vowel.
- **Steering**: the bottom octave (F3 to E4) moves the Grove to a chord on that note and holds it there while you
  hold the key. It starts the Grove from silence too. Let go and the progression goes on from there.
- **Solo**: play over the Grove with the keys above it, in nine voices: Rhodes, CP-80, flute, harp, zither, bells,
  strings, choir, glass. PRESET picks the voice.
- **Sting**: a seeded cue of 3, 5, 10 or 30 s for idents and transitions. The same seed always gives the same sting.
- **Keys**: each mood has its own key. ALGO moves it to any of seven keys named for the chakras (ROOT C …
  CROWN B). There's just intonation too, and A=432 for the schtick.
- **Space**: plate-ish reverb, echo, shimmer, tone.
- **Cassette** on the master: pre/de-emphasis, saturation, head bump, wow, flutter, drift, hiss, and dropouts as
  it wears. WEAR runs from fresh chrome to the thrift-store bin. Tap SEL to bypass it.
- **TALK**: the Grove steps back and dips at 1–4 kHz to leave room for your voice. Tap GLO.
- **Four screen looks**: Redwood, Spectrum, J-card, and Painted (an airbrushed sky for each mood;
  `tools/paint.py` makes them from `renders/painted/src`). Hold HOME and turn SELECT.

## The pages

| button | K1 | K2 | K3 | K4 |
|---|---|---|---|---|
| HOME | PAD | LOW | MOTION | MELODY |
| LFO (GROVE) | LENGTH | BLOOM | ACTIVITY | SOLO |
| FX (SPACE) | SPACE | ECHO | SHIMMER | TONE |
| ENV (TAPE) | WEAR | HISS | TAPE | LEVEL |
| EDIT (TUNING) | KEY | DRIFT | JUST | A432 |

## Layout

| path | what |
|---|---|
| `app/dsp/` | the engine, portable C: voices, harmony, Grove, Sting, effects, cassette |
| `app/app.c` | the FM-1 app: panel, screens, MIDI (on [fm1-base](https://github.com/charlesvestal/fm1-base)) |
| `base/` | fm1-base (submodule) |
| `tools/render.c` | offline renders to WAV: `build/render grove 600 out.wav mood=2 wear=0.4 log=1` |
| `tools/stress.sh`, `prof.sh`, `emu.sh` | load tests and profiles in fm1-emu |
| `research/` | notes on the scene and its techniques |

## Build

    git submodule update --init
    tools/fw.sh            # app/build/blissbox.fwsc (JieLi toolchain, Docker on macOS)
    tools/fw.sh --host     # the host simulator
    clang -O2 -std=gnu11 -DBL_OFFLINE -I base/include -I app/dsp tools/render.c app/dsp/bliss.c -o build/render -lm

## Licence

MPL-2.0, like fm1-base. Copyright © 2026 Manzanita Research.
