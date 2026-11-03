#!/bin/bash
# usage: dualall.sh rate frames only [inputseed]  -- dual-run all ROMs (random input) in parallel; prints per-ROM summary
S=/home/pedro/Software/SNES; D=/home/pedro/development/zsnes-libretro; mkdir -p $D/scratch/da; cd $D/scratch/da
IN=$(python3 - "$2" "${4:-1}" <<'PY'
import sys,random
n,s=int(sys.argv[1]),int(sys.argv[2]); r=random.Random(s); out=[]; f=60
while f<n:
    k=r.random(); m=(1<<3) if k<0.25 else (1<<0) if k<0.5 else (1<<8) if k<0.6 else r.getrandbits(12)&~(1<<2)
    out.append("%d:%d"%(f,m)); f+=r.choice([3,5,10,20,40]); out.append("%d:0"%f); f+=r.choice([5,10,30])
print(",".join(out))
PY
)
i=0; for f in "$S"/*.sfc "$S"/*.smc; do i=$((i+1)); ( ZL_DUAL=$1 ZL_DUAL_ONLY=$3 ZL_DUAL_MAX=12 HARNESS_DIR=$PWD/h$i timeout ${TMO:-1500} $D/build/harness64 $D/build/dev64/zsnes_lifted.so "$f" -f $2 -i "$IN" > out$i.txt 2>&1; echo "[$(basename "$f")] $(grep 'DUAL.*checked' out$i.txt | awk '{c+=$(NF-2); m+=$NF} END{print "checked",c,"mismatching",m}')" > sum$i.txt ) & done
wait; cat sum*.txt; grep -h 'DUAL.*MISMATCH' out*.txt | head -5
