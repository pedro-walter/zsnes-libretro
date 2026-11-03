# ZSNES 1.42 as a libretro core

The original ZSNES 1.42 core (about 100k lines of 32-bit x86 NASM) behind the libretro API, with its timing preserved
bit-exactly. Two builds exist:

| build | what it is | output |
|---|---|---|
| **lifted** (`Makefile.lift`) | the pristine asm statically translated to C by `tools/lift`; runs on **x86-64** (and i386) | `build/lift64/zsnes_lifted.so` |
| hybrid (`Makefile`) | the asm assembled as-is (i386 only) with modules replaced by hand-written C (SPC700, DSP, 65816 opcodes, DMA/HDMA, scheduler, register I/O, start of the PPU) | `build/zsnes_libretro.so` |

Both are accepted only if `tools/timing_check.sh <rom> <core.so>` is identical to the frozen asm reference
(`refs/zsnes_asm_ref.so`): per-frame WRAM trace, per-100-frame video CRC, audio CRC, and (for Top Gear (E)) the trace of
the real, unmodified ZSNES executable (`refs/golden_topgear_e_zsnes_exe.txt`).

## Building

Needs `nasm`, `gcc` (with `-m32` support for the reference/asm objects: `gcc-multilib`), python3 with `capstone` and
`pyelftools`.  Cross builds need `gcc-mingw-w64`, `gcc-aarch64-linux-gnu`, `gcc-arm-linux-gnueabihf` (+ `qemu-user`, `wine` to test).

    make -f Makefile.lift gen                  # build pristine asm objects (build/ref), lift them to C (build/lift)
    make -f Makefile.lift dist64               # Linux x86-64   -> dist/zsnes_libretro.so (+ .info)
    make -f Makefile.lift distwin64            # Windows x64    -> dist/zsnes_libretro_win64.dll
    make -f Makefile.lift distarm64 distarm32  # Linux AArch64 / ARMhf -> dist/zsnes_libretro_aarch64.so / _armhf.so
    make -f Makefile.lift distwin32            # Windows x86 (builds; not run-tested)
    gcc -O1 -o build/harness64 tools/harness.c -ldl -lz        # headless test frontend (-DNO_ZLIB for cross builds)
    tools/timing_check.sh "<rom>" build/lift64/zsnes_lifted.so 4500
    tools/rom_check.sh "<rom>" <core.so> 2000 <seed>           # any ROM, scripted random input, vs the asm reference

Copy the core to RetroArch's cores directory as `zsnes_libretro.so` (`.dll`) together with `zsnes_libretro.info`.
Verified identical to the x86-64 core (frame CRCs, WRAM CRC, audio CRC): AArch64 and ARMhf under `qemu-user`, Windows x64
under `wine`.  All platforms are built with `-ffp-contract=off` (FMA contraction changes the DSP-1 results).

## How the lifter works (tools/lift)

* `objs.py`/`xdis.py`: read the ELF32 objects, lay out their data, decode `.text` (and code that lives in `.data`)
  with capstone, resolve relocations.
* `gen.py`/`emit.py`: one C function per object; registers are locals, flags are locals pruned by a whole-program
  flag-liveness analysis, `call`/`ret` use an emulated stack, code addresses are ids `(object+1)<<21 | offset`,
  jump tables and function pointers in data hold those ids. MMX/x87 are emulated in `lift_mmx.h`/`lift.h`.
* Guest memory is a fixed-address arena (`0x30000000`, 256 MB, `lift_rt.c`) so guest 32-bit addresses *are* host
  pointers; the C that talks to the asm (initc.c, platform.c ...) is rewritten by `cfix.py` so its shared variables
  live at the arena addresses (asm data symbols are absolute linker symbols from `lift_syms.ld`). Heap blocks the
  guest sees are allocated from the arena (`zl_malloc`).
* `make -f Makefile.lift itest` runs every distinct instruction encoding found in the program (8288) on 24 random
  machine states natively and through the emitted C, and compares registers, flags, MMX state and memory.
* Save states: arena pages that differ from a baseline taken after the first frame (+registers). `harness -X N -Y M`
  does a save/load round trip check.

## C overrides (the port in progress)

`tools/lift/overrides.txt` lists asm functions that are replaced by hand-written C in `core/zl/` (register ABI through the
lifted `LG` state; the original lifted body stays available).  Done so far, all bit-identical to the asm core: DSP mixer
and register writes, SPC700 opcodes/timers, DMA/HDMA, PPU/IO register handlers, the scanline scheduler, all 65816
opcodes (incl. decimal ADC/SBC with a portable model of x86 DAA/DAS) and the memory bank handlers.
`ZL_DUAL=N [ZL_DUAL_ONLY=name] [ZL_DUAL_MAX=n]` runs every Nth call of an override twice (C, then the original lifted
code, from the same state) and compares the whole guest memory + registers; `LIFT_NOOVR=name,...|all` disables overrides
(bisecting); `LIFT_PROF=2` prints the hottest guest entry points.  Still lifted asm: SuperFX, SA-1, the video engine,
init/UI glue, the remaining parts of execute.asm.

## Status

* Top Gear (E), TGC, the USA/randomizer ROMs: identical to the asm core; random-input 6000 frame run identical.
* PAL/NTSC, SRAM, reset, save states work. No cheats, no zip/JMA (the frontend unwraps archives), no netplay/GUI.
* Not translated (will abort with a message if reached): the mode-7 EXTBG routines that patch their own code
  (`mode7ext.asm`), the DOS-only files, the hq2x/3x/4x filters. Coprocessor code (SuperFX, SA-1, DSP-1..4, S-DD1,
  SPC7110, ...) is lifted like everything else but there are no ROMs here to exercise it end to end.

## Notes for development

* `build/ref/**` are objects of the *pristine* upstream asm (`upstream/zsnes_1_42/src`); the lifter only reads those.
* After changing `objs.ARENA_BASE` or anything in `tools/lift`, rerun `gen`.
* Debug aids for the lifted core: `LIFT_TRACE_EXT=1` traces calls into C; fatal errors print the last 64 code entries.
* The hybrid build's tools (`make fuzz|dspfuzz|cpufuzz|regwfuzz|hdmafuzz|schedfuzz`, `ZS_*_DIFF`, `core/diffimg.c`)
  are described in the commit messages and `core/cpu/*.c` headers.
