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
    make -f Makefile.lift distwin32            # Windows x86 (run under wine, identical)
    gcc -O1 -o build/harness64 tools/harness.c -ldl -lz        # headless test frontend (-DNO_ZLIB for cross builds)
    tools/timing_check.sh "<rom>" build/lift64/zsnes_lifted.so 4500
    tools/rom_check.sh "<rom>" <core.so> 2000 <seed>           # any ROM, scripted random input, vs the asm reference

Copy the core to RetroArch's cores directory as `zsnes_libretro.so` (`.dll`) together with `zsnes_libretro.info`.
Verified identical to the x86-64 core (frame CRCs, WRAM CRC, audio CRC): AArch64 and ARMhf under `qemu-user`, Windows x64 and x86
under `wine`.  All platforms are built with `-ffp-contract=off` (FMA contraction changes the DSP-1 results).

## How the lifter works (tools/lift)

* `objs.py`/`xdis.py`: read the ELF32 objects, lay out their data, decode `.text` (and code that lives in `.data`)
  with capstone, resolve relocations.
* `gen.py`/`emit.py`: one C function per object; registers are locals, flags are locals pruned by a whole-program
  flag-liveness analysis, `call`/`ret` use an emulated stack, code addresses are ids `(object+1)<<21 | offset`,
  jump tables and function pointers in data hold those ids. MMX/x87 are emulated in `lift_mmx.h`/`lift.h`.
* Guest memory is a fixed-address arena (`0x30000000`, 256 MB, `lift_rt.c`) so guest 32-bit addresses *are* host
  pointers (the guest heap -- ROM, caches -- sits *below* the static data at `+0x4000000`: the asm's NMI speed hack compares
  host pointers across regions, so the ordering has to match the native build; with the opposite order Yoshi's Island
  diverged from the reference after ~440 frames); the C that talks to the asm (initc.c, platform.c ...) is rewritten by `cfix.py` so its shared variables
  live at the arena addresses (asm data symbols are absolute linker symbols from `lift_syms.ld`). Heap blocks the
  guest sees are allocated from the arena (`zl_malloc`).
* `make -f Makefile.lift itest` runs every distinct instruction encoding found in the program (8288) on 24 random
  machine states natively and through the emitted C, and compares registers, flags, MMX state and memory.
* Save states: arena pages that differ from a baseline taken after the first frame (+registers). `harness -X N -Y M`
  does a save/load round trip check.

## C overrides (the port in progress)

`tools/lift/overrides.txt` lists asm functions that are replaced by hand-written C in `core/zl/` (register ABI through the
lifted `LG` state; the original lifted body stays available as the oracle).  Done so far, all bit-identical to the asm core
(18 ROMs, scripted random input, video/WRAM/audio CRCs):

| area | C file(s) |
|---|---|
| DSP mixer, DSP register writes, key-on | `dspmix.c`, `dspw.c` |
| SPC700 opcodes + timers | `spc700.c` |
| 65816 opcodes (incl. decimal ADC/SBC via a portable model of x86 DAA/DAS), memory bank handlers | `cpu.c`, `mem.c` |
| SA-1: its 65816 opcodes (the same code compiled with the SA-1 state names), registers/arith/var-length/IRAM | `cpu_sa1.c`, `sa1regs.c` |
| DMA/HDMA, PPU/IO register handlers, scanline scheduler | `dma.c`, `regs.c`, `sched.c` |
| video: per-line state capture, window tables, screen clipping, colour math (`ProcessTransparencies`), tile cache conversion, sprite caching and sprite lists | `video_line.c`, `video_win.c`, `video_clip.c`, `video_transp.c`, `video_cache.c`, `video_sprcache.c`, `video_sprites.c` |
| SuperFX registers/RAM windows, DSP-1 command interface, ST010/ST011 memory | `sfxregs.c`, `dsp1.c`, `seta.c` |

Verification tools (`tools/lift/lift_rt.c`):
* `ZL_DUAL=N [ZL_DUAL_ONLY=name] [ZL_DUAL_MAX=n]` runs every Nth call of an override twice (C, then the original lifted code, from
  the same state) and compares the whole guest memory + registers (`core/zl/dualign.c` lists scratch areas to ignore).
  `tools/dev/dualall.sh <N> <frames> <name>` does it for all ROMs in parallel.
* `ZL_DUALT=N [ZL_DUAL_ONLY=name]` does the same for the tail-style overrides (opcode handlers that end in "jump to the next
  handler"): the cycle counter is forced to 0 so that exactly one opcode runs, then the real emulation continues with the C code.
* `LIFT_NOOVR=name,...|all` disables overrides (bisecting); `LIFT_PROF=2` prints the hottest guest entry points.
* `tools/rom_check.sh`, `tools/rom_batch.sh <dir> <frames> <seeds>`, `tools/rom_batch_par.sh <dir> <frames> <jobs> <seeds>`: ref-vs-candidate
  with scripted input (`CAND=<so>` selects the candidate); 20000-60000 frame soak runs are cheap and found a rare timing divergence.
* `tools/fuzzrom_par.sh <so> rom|dsp1|sfx|sa1|sdd1 <from> <to>`: generated register-fuzz ROMs (random PPU/DMA/chip traffic); a mismatch that
  also exists with `LIFT_NOOVR=all` is a lifter-vs-native effect, one that disappears with an override disabled is a bug in that override
  (this is how the StartDrawNewGfx16b pass order and the SA-1 BRK were found).
  The remaining lifter-vs-native cases (e.g. rom 41, DMA setups with random A/B-bus registers) end in `bad dispatch target` inside
  `regaccessbankr8`: the `regptra` entry it calls (a fixed 0x3000-entry table, all `regINVALID` after init) holds a non-code value in the lifted
  core, so something wrote into that table earlier. The reference does not die there. Root cause not found; real games do not hit it.
* Debug aids: harness `-Z <frame> <file>` dumps WRAM, `ZL_SCHED_TRACE=file` / `ZL_SCHED_TRACE_TXT` (lifted) and `ZS_SCHED_TRACE[_TXT]` (the hybrid
  `make all` core) log the scheduler state per scanline, `ZL_VD_TRACE=N` traces the drawer calls of the Nth frame.

Still lifted asm (no hand-written C yet): the SuperFX interpreter (`fxemu2*.asm`, `fxtable.asm`: 16 handler tables, runtime
patched plot routines), SPC7110 (needs data files / ROM hacks), C4 and OBC1 (OBC1 is never enabled by ZSNES for the Metal Combat dump
used here), the SA-1 DMA/character-conversion engines, DSP-2..4, the new-engine renderer proper (`StartDrawNewGfx16b`, the tile/line/sprite
drawers: ~13k lines of macros) and the old per-line engine (only Stunt Race FX uses it), the remainder of `execute.asm`, the opcode-table
builders, init and UI glue.

## Status

* All 18 test ROMs (LoROM/HiROM, SA-1, SuperFX incl. Stunt Race FX, DSP-1, C4, OBC1 header, ST010/011, S-DD1, SPC7110) are identical
  to the asm reference core over thousands of frames with random input; Top Gear (E) also matches the real ZSNES executable's trace.
* PAL/NTSC, SRAM, reset, save states, Pro Action Replay RAM cheats work. No zip/JMA (the frontend unwraps archives), no netplay/GUI,
  SNES mouse (port 1/2) and Super Scope (port 2, via the libretro lightgun device) are mapped (untested against real hardware behaviour; multitap is not). Game Genie codes patch the ROM image after the first frame (unverified against the original).
* Not translated (will abort with a message if reached): the DOS-only files and the hq2x/3x/4x filters. The old engine's mode-7 EXTBG
  routines (`mode7ext.asm`) patch the immediates of their own inner loop; the lifter turns such stores into per-instruction cells
  (`pcN` in the generated C), and the asm now uses `strict dword` so that the patched fields really are 32-bit (with `nasm -O1` the
  pristine asm shrank them to imm8 and its own patches corrupted the code). `ZS_OLDGFX=1` forces the old engine, `tools/mkrom_m7ext.py` makes a test ROM.
* Two bugs of the original asm were fixed because they made the reference itself misbehave: Stunt Race FX's old video engine read
  `fulladdtab` out of range (`pal16bxcl` high word, also patched in `upstream/.../makev16b.asm` which is what the lifter reads) and
  `clearback16t` lived in a non-executable `.data` section in the asm reference build.

## Notes for development

* `build/ref/**` are objects of the *pristine* upstream asm (`upstream/zsnes_1_42/src`); the lifter only reads those.
* After changing `objs.ARENA_BASE` or anything in `tools/lift`, rerun `gen`.
* Debug aids for the lifted core: `LIFT_TRACE_EXT=1` traces calls into C; fatal errors print the last 64 code entries.
* The hybrid build's tools (`make fuzz|dspfuzz|cpufuzz|regwfuzz|hdmafuzz|schedfuzz`, `ZS_*_DIFF`, `core/diffimg.c`)
  are described in the commit messages and `core/cpu/*.c` headers.
