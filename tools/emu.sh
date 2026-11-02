#!/bin/sh
# Run the firmware in fm1-emu as an FM-1 runs (FM1_STRICT: measured cycles, timed LCD) and keep the panel.
#   tools/emu.sh MS 'KEYS' out.png      e.g. tools/emu.sh 12000 '1500:PLAY,10000-12500:HOME' renders/emu/grove.png
# KEYS is fm1-live's FM1_BENCH_KEYS. FM1_EMU: the fm1-emu checkout (default $TMPDIR/fm1-emu).
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)
EMU=${FM1_EMU:-$TMPDIR/fm1-emu}
W=$ROOT/build/emu
case "$3" in /*) OUT=$3 ;; *) OUT=$PWD/$3 ;; esac
mkdir -p "$(dirname "$OUT")"
mkdir -p "$W/work"
cd "$W"
FM1_STRICT=1 FM1_BENCH_KEYS="$2" "$EMU/target/release/fm1-live" --fwsc "$ROOT/app/build/blissbox.fwsc" --shot "$1" 2>&1 | tail -6
ffmpeg -loglevel error -y -i work/panel.ppm "$OUT"
