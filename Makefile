# ZSNES 1.42 libretro core (32-bit x86; original asm core)
#   make          -> build/zsnes_libretro.so
#   make harness  -> build/harness (headless libretro frontend, no window)
#   make test     -> run headless smoke test
CORE   := core
CC     := gcc -m32
CXX    := g++ -m32
NASM   := nasm
CFLAGS := -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=0 -fno-stack-protector -O2 -g -pipe -fPIC -fno-strict-aliasing -w -I$(CORE) -Ilibretro -D__LINUX__ -DNO_PNG -D__LIBRETRO__
NFLAGS := -w-orphan-labels -D__LINUX__ -DELF -DNO_PNG -D__LIBRETRO__ -f elf32 -O1 -I$(CORE)/
B      := build

ASM := $(addprefix chips/,sfxproc fxemu2 dsp1proc fxemu2b fxemu2c fxtable sa1proc sa1regs st10proc dsp2proc) \
       $(addprefix cpu/,addrni cpustub dma dsp dspproc execute irq memory spc700 stable table tableb tablec) \
       $(addprefix gui/,gui menu) \
       $(addprefix video/,makev16b makev16t makevid mode716 mode716b mode716d mode716e mode716t mode7 mode7ext mv16tms newg162 newgfx16 newgfx2 newgfx m716text 2xsaiw procvid sw_draw hq2x16 hq2x32 hq3x16 hq3x32 hq4x16 hq4x32) \
       $(addprefix linux/,copyvwin sdlintrf) \
       $(addprefix dos/,debug joy modemrtn vesa2 initvid sw gppro vesa12) \
       endmem init ui vcache
CSRC := linux/zfilew linux/ztcp linux/zipxw chips/dsp1emu chips/seta10 chips/sdd1emu cpu/executec cpu/spc700c cpu/dspc cpu/dspw cpu/cpuc cpu/dmac cfgload initc uic patch version \
        effects/burn effects/water effects/smoke
CXXSRC :=

OBJ := $(addprefix $(B)/,$(addsuffix .ao,$(ASM)) $(addsuffix .co,$(CSRC))) \
       $(B)/libretro.co $(B)/platform.co

all: $(B)/zsnes_libretro.so

$(B)/%.ao: $(CORE)/%.asm
	@mkdir -p $(dir $@)
	$(NASM) $(NFLAGS) -o $@ $<

$(B)/%.co: $(CORE)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c -o $@ $<

$(B)/%.co: libretro/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c -o $@ $<

$(B)/zsnes_libretro.so: $(OBJ)
	$(CC) -shared -o $@ $(OBJ) -lm -Wl,-z,notext

clean:
	rm -rf $(B)

.PHONY: all clean

# ---- headless test frontend (needs i386 libz at runtime only) ----
harness: $(B)/harness
$(B)/harness: tools/harness.c libretro/libretro.h
	$(CC) -O1 -g -o $@ $< -ldl /usr/lib/i386-linux-gnu/libz.so.1 -I/usr/include
.PHONY: harness

# Smoke test: ROM=path/to/rom.sfc make test   (headless, no window; checks boot + audio)
test: all harness
	@test -n "$(ROM)" || { echo "set ROM=..."; exit 2; }
	HARNESS_DIR=$(B) $(B)/harness $(B)/zsnes_libretro.so "$(ROM)" -f 600 -s 600 -o $(B)/test
	tools/run_retroarch32.sh "$(ROM)" 120 >/dev/null 2>&1 && echo "retroarch32: OK"
.PHONY: test

# Freeze the current build as the reference ("pure asm") core for timing_check.sh
ref: all
	mkdir -p refs && cp $(B)/zsnes_libretro.so refs/zsnes_asm_ref.so
timing: all harness
	tools/timing_check.sh "$(ROM)"
.PHONY: ref timing

# ---- Reference ZSNES executable: original asm + ORIGINAL (unmodified) linux platform layer on a headless SDL shim ----
RCFLAGS := -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=0 -fno-stack-protector -O2 -g -pipe -fno-strict-aliasing -w \
           -Irefexe -I$(CORE) -D__LINUX__ -DNO_PNG
RCSRC := linux/zfilew linux/ztcp linux/zipxw linux/sdllink linux/sw_draw linux/zloaderw zip/unzip \
         chips/dsp1emu chips/seta10 chips/sdd1emu cpu/executec cfgload initc uic patch version \
         effects/burn effects/water effects/smoke
UPS  := upstream/zsnes_1_42/src
ROBJ := $(addprefix $(B)/ref/,$(addsuffix .ao,$(ASM))) $(addprefix $(B)/ref/,$(addsuffix .co,$(RCSRC))) $(B)/ref/sdl_shim.co $(B)/ref/stubs.co
# the reference exe is built from the PRISTINE upstream asm (no libretro hooks)
$(B)/ref/%.ao: $(UPS)/%.asm
	@mkdir -p $(dir $@)
	$(NASM) -w-orphan-labels -D__LINUX__ -DELF -DNO_PNG -f elf32 -O1 -I$(UPS)/ -o $@ $<
$(B)/ref/%.co: $(CORE)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(RCFLAGS) -c -o $@ $<
$(B)/ref/%.co: refexe/%.c
	@mkdir -p $(dir $@)
	$(CC) $(RCFLAGS) -c -o $@ $<
refexe-bin: $(B)/zsnes_ref
$(B)/zsnes_ref: $(ROBJ)
	$(CC) -no-pie -Wl,-z,notext -o $@ $(ROBJ) -lm -lpthread /usr/lib/i386-linux-gnu/libz.so.1
.PHONY: refexe-bin

# Regenerate the golden trace from the real ZSNES executable (real-time, ~90s). Needs ROM=Top Gear (Europe).
golden: refexe-bin
	rm -rf $(B)/refhome && mkdir -p $(B)/refhome && env -u DISPLAY HOME=$(abspath $(B))/refhome ZS_REF_TRACE=$(B)/ref_full.txt ZS_REF_FRAMES=4300 timeout 300 $(B)/zsnes_ref "$(ROM)" >/dev/null 2>&1
	python3 -I -c "L=[l.split()[1:] for l in open('$(B)/ref_full.txt')];i=next(i for i,t in enumerate(L) if t[2]=='1ffc=1');open('refs/golden_topgear_e_zsnes_exe.txt','w').write(''.join(' '.join(t)+chr(10) for t in L[i:]))"
.PHONY: golden

# Differential fuzz of C-ported SPC700 opcodes/timers vs the original asm handlers (ROM only needed to boot the core)
fuzz: all harness
	HARNESS_DIR=$(abspath $(B)) $(B)/harness $(B)/zsnes_libretro.so "$(ROM)" -f 300 -F $(or $(N),2570000) 2>&1 | grep -E "MISMATCH|spc fuzz|  op |fuzz mism"
.PHONY: fuzz

# Differential fuzz of the C DSP mixer vs the original asm ProcessSoundBuffer (random register traffic, byte-exact state compare)
dspfuzz: all harness
	HARNESS_DIR=$(abspath $(B)) $(B)/harness $(B)/zsnes_libretro.so "$(ROM)" -f 300 -D $(or $(N),300000) 2>&1 | grep -E "dsp diff|dsp fuzz|dsp cov"
.PHONY: dspfuzz

# Differential fuzz of the C DSP register-write / key-on code vs the original asm handlers
dspwfuzz: all harness
	HARNESS_DIR=$(abspath $(B)) $(B)/harness $(B)/zsnes_libretro.so "$(ROM)" -f 300 -W $(or $(N),300000) 2>&1 | grep -E "dspw|dsp write"
.PHONY: dspwfuzz

# Differential fuzz of the C 65816 opcodes vs the original asm handlers (same machine state, WRAM + registers compared)
cpufuzz: all harness
	HARNESS_DIR=$(abspath $(B)) $(B)/harness $(B)/zsnes_libretro.so "$(ROM)" -f 300 -C $(or $(N),300000) 2>&1 | grep -E "cpu fuzz"
.PHONY: cpufuzz
