#!/bin/bash
# run rom_check on every ROM in a dir with several seeds: tools/rom_batch.sh <dir> <frames> <seeds...>
D="$(cd "$(dirname "$0")/.." && pwd)"; DIR="$1"; N="$2"; shift 2
for rom in "$DIR"/*.sfc "$DIR"/*.smc; do [ -f "$rom" ] || continue
  for s in "$@"; do echo "[$(basename "$rom") seed $s] $("$D/tools/rom_check.sh" "$rom" "" "$N" "$s" 2>&1 | head -3 | tr '\n' ' ')"; done; done
