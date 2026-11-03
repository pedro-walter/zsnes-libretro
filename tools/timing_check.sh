#!/bin/bash
# Compare a candidate core against the frozen asm reference core, frame by frame.
# Traces Top Gear WRAM (fuel $1E62, demo flag $1FF6, frame counter $1FFC) plus per-frame video CRC
# and total audio CRC.  usage: tools/timing_check.sh <rom> [candidate.so] [frames]
# The reference is refs/zsnes_asm_ref.so (frozen build of the pure-asm core): `make ref` to (re)freeze.
D="$(cd "$(dirname "$0")/.." && pwd)"; ROM="$1"; CAND="${2:-$D/build/zsnes_libretro.so}"; N="${3:-4500}"
REF="$D/refs/zsnes_asm_ref.so"; W="$(mktemp -d)"; trap 'rm -rf "$W"' EXIT
[ -f "$REF" ] || { echo "no reference core; run: make ref"; exit 2; }
H() { if head -c 5 "$1" | od -An -tx1 | grep -q "02"; then echo "$D/build/harness64"; else echo "$D/build/harness"; fi; }
run() { HARNESS_DIR="$W" "$(H "$1")" "$1" "$ROM" -f "$N" -t 1e62:2,1ff6:2,1ffc:2 -T "$W/$2.trace" -s "$(seq -s, 100 100 "$N")" | grep -E "^frame|^done" > "$W/$2.out"; }
run "$REF" ref; run "$CAND" cand
if cmp -s "$W/ref.trace" "$W/cand.trace"; then echo "WRAM trace (fuel/demo/frame counter): IDENTICAL over $N frames"; else
  echo "WRAM trace DIFFERS; first divergence:"; diff "$W/ref.trace" "$W/cand.trace" | head -4; fi
if cmp -s "$W/ref.out" "$W/cand.out"; then echo "video CRCs + audio CRC: IDENTICAL"; else
  echo "video/audio DIFFERS:"; diff "$W/ref.out" "$W/cand.out" | head -6; fi
# Golden trace: captured from the real ZSNES 1.42 executable (original asm + original SDL platform layer on the
# headless shim; `make golden`).  Sampled at draw time, aligned from the first game frame (frame counter = 1).
GOLD="$D/refs/golden_topgear_e_zsnes_exe.txt"; rc=0
if [ -f "$GOLD" ] && [ "$(md5sum "$ROM" | cut -d" " -f1)" = "$(cat "$D/refs/golden_rom.md5" 2>/dev/null)" ]; then   # golden is Top Gear (Europe) only; for other ROMs this check is skipped
  ZS_WRAM_AT_DRAW=1 HARNESS_DIR="$W" "$(H "$CAND")" "$CAND" "$ROM" -f 4300 -t 1e62:2,1ff6:2,1ffc:2 -T "$W/gold.trace" >/dev/null 2>&1
  python3 -I - "$W/gold.trace" "$GOLD" <<'PY' || rc=1
import sys
c=[l.split()[1:] for l in open(sys.argv[1])]; g=[l.split() for l in open(sys.argv[2])]
i=next((i for i,t in enumerate(c) if t[2]=="1ffc=1"),None)
if i is None: print("golden: candidate never reached game frame 1"); sys.exit(1)
c=c[i:]; n=min(len(c),len(g)); bad=[k for k in range(n) if c[k]!=g[k]]
print("vs real ZSNES executable (golden): %d game frames compared, %d mismatches"%(n,len(bad)))
if bad: print("  first at game frame %d: golden %s candidate %s"%(bad[0],g[bad[0]],c[bad[0]])); sys.exit(1)
PY
fi
cmp -s "$W/ref.trace" "$W/cand.trace" && cmp -s "$W/ref.out" "$W/cand.out" && [ $rc = 0 ]
