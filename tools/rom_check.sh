#!/bin/bash
# Generic ref-vs-candidate comparison for any ROM with scripted random input.
# usage: tools/rom_check.sh <rom> [cand.so] [frames] [seed]
D="$(cd "$(dirname "$0")/.." && pwd)"; ROM="$1"; CAND="${2:-$D/build/lift64/zsnes_lifted.so}"; N="${3:-2000}"; SEED="${4:-1}"
REF="$D/refs/zsnes_asm_ref.so"; W="$(mktemp -d)"; trap 'rm -rf "$W"' EXIT
H() { if head -c 5 "$1" | od -An -tx1 | grep -q "02"; then echo "$D/build/harness64"; else echo "$D/build/harness"; fi; }
IN=$(python3 -I - "$N" "$SEED" <<'PY'
import sys,random
n,s=int(sys.argv[1]),int(sys.argv[2]); r=random.Random(s); out=[]
# buttons: B=0 Y=1 SEL=2 START=3 UP=4 DOWN=5 LEFT=6 RIGHT=7 A=8 X=9 L=10 R=11
f=60
while f<n:
    m=0
    k=r.random()
    if k<0.25: m=1<<3
    elif k<0.5: m=1<<0
    elif k<0.6: m=1<<8
    else: m=r.getrandbits(12)&~(1<<2)
    out.append("%d:%d"%(f,m)); f+=r.choice([3,5,10,20,40])
    out.append("%d:0"%f); f+=r.choice([5,10,30])
print(",".join(out))
PY
)
run() { HARNESS_DIR="$W/$2" ; mkdir -p "$W/$2"; HARNESS_DIR="$W/$2" timeout ${TMO:-900} "$(H "$1")" "$1" "$ROM" -f "$N" -M 1 -i "$IN" -s "$(seq -s, 50 50 "$N")" 2>"$W/$2.err" | grep -E "^frame|^done" > "$W/$2.out"; echo "$? " >/dev/null; }
run "$REF" ref & run "$CAND" cand & wait
nr=$(wc -l < "$W/ref.out"); nc=$(wc -l < "$W/cand.out")
if cmp -s "$W/ref.out" "$W/cand.out"; then echo "IDENTICAL ($nr lines) $(tail -1 "$W/cand.out")"; exit 0; fi
echo "DIFFERENT (ref $nr lines, cand $nc lines)"; diff "$W/ref.out" "$W/cand.out" | head -6; tail -2 "$W/ref.err" "$W/cand.err" | head -12; exit 1
