#!/bin/sh
# Regenerate the recompiled C and build pwr + tests.
# usage: recomp/build.sh [extracted dir] [build dir]     (defaults: <project>/extracted, <project>/build)
set -e
R=$(cd "$(dirname "$0")" && pwd); P=$(dirname "$R")
EX=${1:-$P/extracted}; B=${2:-$P/build}
python3 "$R/arm2c.py" "$EX/code.bin" "$EX/exheader.bin" "$B/gen"
cmake -S "$R" -B "$B" -DGEN_DIR="$B/gen" -DCMAKE_BUILD_TYPE=RelWithDebInfo >/dev/null
cmake --build "$B" -j "$(nproc)"
