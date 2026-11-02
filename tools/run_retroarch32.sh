#!/bin/bash
# Headless run of the core inside the side-by-side 32-bit RetroArch (null video/audio, isolated HOME).
# usage: tools/run_retroarch32.sh <rom> [frames]
D="$(cd "$(dirname "$0")/.." && pwd)"; T="$D/tools/retroarch32"
exec env -u DISPLAY -u WAYLAND_DISPLAY HOME="$T/home" timeout 120 \
  "$T/extract/RetroArch-Linux-x86/squashfs-root/usr/bin/retroarch" --config "$T/ra.cfg" \
  --max-frames="${2:-300}" -L "$D/build/zsnes_libretro.so" "$1"
