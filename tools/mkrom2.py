#!/usr/bin/env python3
# Register-fuzz test ROM: an unrolled 65816 loop of random register writes/reads (PPU, DMA, APU ports, chips, RAM).
#   mkrom2.py <out.sfc> <seed> <chip>   chip: rom|dsp1|sfx|sa1|sdd1
import sys, random, struct
out, seed, chip = sys.argv[1], int(sys.argv[2]), sys.argv[3]
rnd = random.Random(seed)
rom = bytearray(rnd.getrandbits(8) for _ in range(1 << 20))
code = bytearray()
def emit(*b): code.extend(b)
emit(0x78, 0x18, 0xFB, 0xC2, 0x30, 0xA2, 0xFF, 0x1F, 0x9A, 0xE2, 0x20)       # sei clc xce rep#30 ldx#1fff txs sep#20
emit(0xA9, 0x0F, 0x8D, 0x00, 0x21, 0xA9, 0x81, 0x8D, 0x00, 0x42, 0x58)       # brightness 15, NMI on, cli
loop = 0x8000 + len(code)
ppu_w = list(range(0x2101, 0x2134))
regs = []
chipaddrs = []
if chip == 'sfx': chipaddrs = [(0x00, a) for a in range(0x3000, 0x3040)] + [(0x00, a) for a in range(0x3030, 0x3040)] + [(0x70, a) for a in range(0, 0x80)]
elif chip == 'sa1': chipaddrs = [(0x00, a) for a in range(0x2200, 0x2240)] + [(0x00, a) for a in range(0x2300, 0x2310)] + [(0x40, a) for a in range(0, 0x80)]
elif chip == 'dsp1': chipaddrs = [(0x30, 0x8000), (0x30, 0xC000), (0x30, 0x8000), (0x30, 0xC000), (0x60, 0x0000), (0x60, 0x4000)]
elif chip == 'sdd1': chipaddrs = [(0x00, a) for a in range(0x4800, 0x4808)] + [(0x00, a) for a in range(0x4300, 0x4310)]
N = 5200
for i in range(N):
    r = rnd.random()
    if r < 0.30:
        a = rnd.choice(ppu_w); v = rnd.getrandbits(8)
        if a == 0x2100: v &= 0x0F
        bank, addr = 0, a
    elif r < 0.40:
        bank, addr, v = 0, 0x2118 if rnd.random() < 0.5 else 0x2119, rnd.getrandbits(8)       # VRAM data
    elif r < 0.45:
        bank, addr, v = 0, 0x2122, rnd.getrandbits(8)                                          # CGRAM data
    elif r < 0.50:
        bank, addr, v = 0, 0x2104, rnd.getrandbits(8)                                          # OAM data
    elif r < 0.58:
        bank, addr, v = 0, rnd.choice([0x4202, 0x4203, 0x4204, 0x4205, 0x4206, 0x4207, 0x4208, 0x4209, 0x420A, 0x4016, 0x4017]), rnd.getrandbits(8)
    elif r < 0.68:
        ch = rnd.randrange(8)
        reg = rnd.choice([0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 0xA])
        bank, addr, v = 0, 0x4300 + ch * 16 + reg, rnd.getrandbits(8)
        if reg == 5: v = rnd.randrange(1, 48)
        if reg == 3: v = rnd.choice([rnd.randrange(0x00, 0x20), rnd.randrange(0x80, 0x100)])
        if reg == 4: v = rnd.choice([0x00, 0x7E, 0x7F, 0x80, 0x00])
        if reg == 6 or reg == 8 or reg == 9: v = 0 if reg == 6 else v
    elif r < 0.72:
        bank, addr, v = 0, rnd.choice([0x420B, 0x420C, 0x420D, 0x420C]), rnd.getrandbits(8) & (0x07 if rnd.random() < 0.5 else 0xFF)
        if addr == 0x420B and (rnd.random() < 0.7 or __import__('os').environ.get('NODMA')): addr = 0x420C
        if addr == 0x420B:
            for ch in range(8):
                if (v >> ch) & 1:
                    emit(0xA9, rnd.randrange(1, 48), 0x8D, (0x05 + ch * 16) & 255, 0x43)
                    emit(0xA9, 0, 0x8D, (0x06 + ch * 16) & 255, 0x43)
    elif r < 0.78 and chipaddrs:
        bank, addr = rnd.choice(chipaddrs); v = rnd.getrandbits(8)
    elif r < 0.84:
        bank, addr, v = 0, 0x2140 + rnd.randrange(4), rnd.getrandbits(8)                      # APU ports
    elif r < 0.88:
        bank, addr, v = 0, 0x2180 + rnd.randrange(4), rnd.getrandbits(8)                      # WRAM port
    else:
        # read something and fold it into WRAM $1F00+
        rbank, raddr = 0, rnd.choice(list(range(0x2134, 0x2140)) + [0x4210, 0x4211, 0x4212, 0x4214, 0x4215, 0x4216, 0x4217, 0x2137, 0x213E, 0x213F, 0x2140, 0x2141, 0x2142, 0x2143, 0x2180, 0x4016, 0x4017, 0x4218, 0x4219])
        if chipaddrs and rnd.random() < 0.4: rbank, raddr = rnd.choice(chipaddrs)
        if rbank == 0: emit(0xAD, raddr & 255, raddr >> 8)
        else: emit(0xAF, raddr & 255, raddr >> 8, rbank)
        w = 0x1F00 + rnd.randrange(256)
        emit(0x4D, w & 255, w >> 8)           # eor abs
        emit(0x8D, w & 255, w >> 8)           # sta abs
        continue
    emit(0xA9, v)
    if bank == 0: emit(0x8D, addr & 255, addr >> 8)
    else: emit(0x8F, addr & 255, addr >> 8, bank)
emit(0x4C, loop & 255, loop >> 8)
assert len(code) < 0x7E00, len(code)
rom[0:len(code)] = code
rom[0x7F00] = 0x40
hdr = 0x7FC0
rom[hdr:hdr + 21] = b'ZSNES REGFUZZ TEST   '[:21]
mapmode, ctype = {'rom': (0x20, 0x00), 'dsp1': (0x20, 0x03), 'sfx': (0x20, 0x13), 'sa1': (0x23, 0x35), 'sdd1': (0x22, 0x43)}[chip]
rom[hdr + 0x15] = mapmode; rom[hdr + 0x16] = ctype; rom[hdr + 0x17] = 0x0A; rom[hdr + 0x18] = 0x03; rom[hdr + 0x19] = 0x01; rom[hdr + 0x1A] = 0x33; rom[hdr + 0x1B] = 0
vec = {0x7FE4: 0xFF00, 0x7FE6: 0xFF00, 0x7FE8: 0xFF00, 0x7FEA: 0xFF00, 0x7FEE: 0xFF00,
       0x7FF4: 0xFF00, 0x7FF8: 0xFF00, 0x7FFA: 0xFF00, 0x7FFC: 0x8000, 0x7FFE: 0xFF00}
for a, v in vec.items(): rom[a:a + 2] = struct.pack('<H', v)
rom[hdr + 0x1C:hdr + 0x20] = b'\0\0\0\0'
cs = sum(rom) & 0xFFFF
rom[hdr + 0x1C:hdr + 0x20] = struct.pack('<HH', (~cs) & 0xFFFF, cs)
open(out, 'wb').write(rom)
