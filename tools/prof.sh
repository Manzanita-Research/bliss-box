#!/bin/sh
# Where the FM-1's cycles go: the firmware in fm1-emu (FM1_STRICT), the Grove started, a profile from FROM to TO ms.
#   tools/prof.sh [FROM TO [KEYS]]
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)
EMU=${FM1_EMU:-$TMPDIR/fm1-emu}
FROM=${1:-8000}; TO=${2:-11000}; KEYS=${3:-1500:PLAY,1600:PLAY^}
mkdir -p "$ROOT/build/emu/work" && cd "$ROOT/build/emu"
FM1_STRICT=1 FM1_TPROF=$PWD/tprof.txt,$FROM,$TO FM1_BENCH_KEYS="$KEYS" "$EMU/target/release/fm1-live" \
  --fwsc "$ROOT/app/build/blissbox.fwsc" --shot $((TO + 1000)) 2>&1 | grep "ISRs"
python3 "$ROOT/tools/tprof.py" tprof.txt "$ROOT/app/build/blissbox.dis"
