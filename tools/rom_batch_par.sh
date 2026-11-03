#!/bin/bash
# parallel rom_check over every ROM in a dir and several seeds: tools/rom_batch_par.sh <dir> <frames> <jobs> <seeds...>   (CAND=<core.so> selects the candidate)
D="$(cd "$(dirname "$0")/.." && pwd)"; DIR="$1"; N="$2"; J="$3"; shift 3
for rom in "$DIR"/*.sfc "$DIR"/*.smc; do [ -f "$rom" ] || continue; for s in "$@"; do printf '%s\0%s\0' "$rom" "$s"; done; done |
  xargs -0 -n 2 -P "$J" bash -c 'rom="$0"; s="$1"; echo "[$(basename "$rom") seed $s] $("'"$D"'/tools/rom_check.sh" "$rom" "${CAND:-}" "'"$N"'" "$s" 2>&1 | head -3 | tr "\n" " ")"'
