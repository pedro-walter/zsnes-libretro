#!/bin/bash
# Differential run of generated register-fuzz ROMs: frozen asm reference vs a candidate core.
# usage: tools/fuzzrom_check.sh <cand.so> <chip> <seed-from> <seed-to> [frames]
D="$(cd "$(dirname "$0")/.." && pwd)"; CAND="$1"; CHIP="$2"; S0="$3"; S1="$4"; N="${5:-200}"
W="$(mktemp -d)"; trap 'rm -rf "$W"' EXIT
H() { if head -c 5 "$1" | od -An -tx1 | grep -q "02"; then echo "$D/build/harness64"; else echo "$D/build/harness"; fi; }
bad=0
for s in $(seq "$S0" "$S1"); do
  python3 "$D/tools/mkrom2.py" "$W/r.sfc" "$s" "$CHIP"
  SH=$(seq -s, 25 25 "$N")
  HARNESS_DIR="$W" timeout ${FZ_TIMEOUT:-25} "$D/build/harness" "$D/refs/zsnes_asm_ref.so" "$W/r.sfc" -f "$N" -s "$SH" -o "$W/a" 2>&1 | grep -a "^frame\|^done" | sed 's/ -> .*//' > "$W/ref.txt"
  HARNESS_DIR="$W" timeout ${FZ_TIMEOUT:-25} "$(H "$CAND")" "$CAND" "$W/r.sfc" -f "$N" -s "$SH" -o "$W/b" 2>&1 | grep -a "^frame\|^done" | sed 's/ -> .*//' > "$W/cand.txt"
  if ! grep -q "^done" "$W/ref.txt"; then echo "seed $s: reference did not finish (skipped)"; continue
  elif ! grep -q "^done" "$W/cand.txt"; then echo "seed $s: candidate did not finish (timeout/fatal)"; continue
  elif cmp -s "$W/ref.txt" "$W/cand.txt"; then echo "seed $s: identical ($(tail -1 "$W/ref.txt" | cut -c1-60))"; else echo "seed $s: MISMATCH"; diff "$W/ref.txt" "$W/cand.txt" | head -4; bad=$((bad+1)); fi
done
exit $bad
