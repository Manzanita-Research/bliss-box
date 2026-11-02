#!/bin/sh
# Worst-case load: the grove playing, the solo voice moved PRESET steps from RHODES, and a six-note
# chord struck every 1.5 s over MIDI. The HOME readout (DSP %, xruns, voices) is on the panel at the end.
#   tools/stress.sh STEPS out.png [MS]
S=$1; OUT=$2; MS=${3:-15000}
K="1500:PLAY,1600:PLAY^,2000:e6:$S"
t=3000
while [ $t -lt $((MS - 1000)) ]; do
  for n in 30 37 3C 40 43 48; do K="$K,$t:m80${n}00"; done
  for n in 30 37 3C 40 43 48; do K="$K,$((t + 10)):m90${n}64"; done
  t=$((t + 1500))
done
K="$K,$((MS - 6000)):HOME"
exec "$(dirname "$0")/emu.sh" "$MS" "$K" "$OUT"
