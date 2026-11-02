#!/bin/sh
# Build the FM-1 firmware (app/build/blissbox.fwsc) with the JieLi toolchain (Docker on macOS).
#   tools/fw.sh [build.py options]     e.g. tools/fw.sh --host for the simulator
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)
PY=${PY:-$ROOT/build/venv/bin/python}   # Pillow, with libraqm (brew install libraqm) for the fonts
[ -x "$PY" ] || PY=python3
JIELI_TOOLCHAIN=${JIELI_TOOLCHAIN:-$HOME/.jieli/toolchain} AC79_SDK=${AC79_SDK:-$HOME/.jieli/ac79-sdk} \
DYLD_LIBRARY_PATH=/opt/homebrew/lib "$PY" "$ROOT/base/tools/build.py" "$ROOT/app" "$@" 2>&1 | grep -v "^font "
