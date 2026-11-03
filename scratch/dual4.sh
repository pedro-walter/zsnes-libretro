#!/bin/bash
# usage: dual4.sh rate frames [only]   -- dual-run on Top Gear, Star Fox, Pilotwings, SMRPG in parallel
S=/home/pedro/Software/SNES; D=/home/pedro/development/zsnes-libretro; cd $D/scratch/d1
i=0
for r in "Top Gear (E) [!].smc" "Star Fox (USA) (Rev 2).sfc" "Pilotwings (USA).sfc" "Super Mario RPG - Legend of the Seven Stars (USA).sfc"; do
  i=$((i+1)); ZL_DUAL=$1 ZL_DUAL_ONLY=$3 ZL_DUAL_MAX=30 HARNESS_DIR=$PWD/m$i timeout 900 $D/build/harness64 $D/build/dev64/zsnes_lifted.so "$S/$r" -f $2 > mdual$i.txt 2>&1 &
done
wait
for i in 1 2 3 4; do echo "== $i"; grep 'DUAL.*checked' mdual$i.txt | awk '$6>0'; grep 'DUAL' mdual$i.txt | grep -v 'checked\|inittablec\|transdma' | head -6; grep 'DUAL.*checked' mdual$i.txt | awk '{s+=$4} END{print "total checked",s}'; done
