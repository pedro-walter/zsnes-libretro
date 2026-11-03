#!/bin/bash
# parallel driver: tools/fuzzrom_par.sh <cand.so> <chip> <seed-from> <seed-to> [frames]
D="$(cd "$(dirname "$0")/.." && pwd)"
seq "$3" "$4" | xargs -P 4 -I{} "$D/tools/fuzzrom_check.sh" "$1" "$2" {} {} "${5:-150}" 2>&1 | sort -V
