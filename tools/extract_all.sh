#!/bin/sh
# Rebuild every extracted/converted asset from the CIA.
# usage: tools/extract_all.sh <game.cia> <out dir>
set -e
T=$(dirname "$0"); CIA=$1; OUT=$2
python3 "$T/cia.py" "$CIA" "$OUT"
python3 "$T/rst.py" "$OUT/romfs" "$OUT/res"
python3 "$T/tex.py" "$OUT/res" "$OUT/png"
python3 "$T/sfx.py" "$OUT/res" "$OUT/wav"
python3 "$T/zab.py" "$OUT/res" --code "$OUT/code.bin" > "$OUT/values.txt"
mkdir -p "$OUT/text" "$OUT/obj"
for L in usenglish usfrench usspanish; do
  python3 "$T/ui.py" lng "$OUT/res/$L.res/lang.lng" "$OUT/res/$L.res/uifnt_o.gtx" "$OUT/text/$L.json"
done
find "$OUT/res" \( -name '*.mod' -o -name '*.shd' \) | while read -r m; do
  o="$OUT/obj/${m#"$OUT"/res/}.obj"; mkdir -p "$(dirname "$o")"
  python3 "$T/mod.py" "$m" "$o" --tex-root "$OUT/res" --png-root "$OUT/png" 2>/dev/null
done
find "$OUT/res" -name '*.msh' | while read -r m; do
  o="$OUT/obj/${m#"$OUT"/res/}.obj"; mkdir -p "$(dirname "$o")"
  python3 "$T/msh.py" "$m" "$o" 2>/dev/null
done
echo "done: $OUT"
