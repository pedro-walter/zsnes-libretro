#!/usr/bin/env python3
# C4 test ROM: random register setups + commands through $6000-$7FFF.   mkrom_c4.py <out.sfc> <seed> [iterations] [sprites]
import sys, struct, random
out, seed = sys.argv[1], int(sys.argv[2]); n = int(sys.argv[3]) if len(sys.argv) > 3 else 60
sprites = len(sys.argv) > 4
r = random.Random(seed)
c = bytearray()
def emit(*b): c.extend(b)
emit(0x78, 0x18, 0xFB, 0xE2, 0x20, 0xA9, 0x0F, 0x8D, 0x00, 0x21)
def wr(off, v): a = 0x6000 + off; emit(0xA9, v & 255, 0x8D, a & 255, a >> 8)
def rd(off):
    a = 0x6000 + off
    if r.random() < .3: emit(0xC2, 0x20, 0xAD, a & 255, a >> 8, 0xE2, 0x20)
    else: emit(0xAD, a & 255, a >> 8)
def w16(off, v):
    if r.random() < .4: a = 0x6000 + off; emit(0xC2, 0x20, 0xA9, v & 255, (v >> 8) & 255, 0x8D, a & 255, a >> 8, 0xE2, 0x20)
    else: wr(off, v); wr(off + 1, v >> 8)
for _ in range(n):
    op = r.choice([5, 0x0D, 0x10, 0x13, 0x15, 0x1F, 0x22, 0x25, 0x2D, 0x40, 0x54, 0x5C, 0x89] + ([0] if sprites else []))
    for i in range(r.randrange(0, 12)): wr(r.randrange(0, 0x800), r.randrange(256))
    for i in range(0x1F80, 0x1F93): wr(i, r.randrange(256) if r.random() < .7 else r.choice([0, 0xFF, 0x80, 0x7F]))
    if op == 5: w16(0x1F83, r.choice([r.randrange(3, 0x7000), -r.randrange(3, 0x7000) & 0xFFFF]))
    if op == 0x0D: w16(0x1F86, r.randrange(1, 0x300))
    if op == 0x22: w16(0x1F83, r.randrange(0, 40)); w16(0x1F89, r.randrange(0, 20)); wr(0x1F8C, r.randrange(256)); wr(0x1F8F, r.randrange(256))
    if op == 0:
        wr(0x1F4D, r.choice([0, 0x0E])); 
    else: wr(0x1F4D, r.choice([0, 0, 0x0E, 3]))
    if r.random() < .15:
        wr(0x1F4D, 0x0E); op2 = r.choice([0, 4, 8, 0x3C, 0x44]); wr(0x1F4F, op2)
    wr(0x1F4F, op)
    for i in range(r.randrange(2, 8)): rd(r.choice([0x1F80, 0x1F83, 0x1F86, 0x1F89, 0x1F8C, 0x1F8F, r.randrange(0x1F80, 0x1F94), 0x0000, 0x0800]))
    if r.random() < .2:                       # memcpy
        w16(0x1F40, 0x8000 + r.randrange(0x4000)); wr(0x1F42, 0); w16(0x1F43, r.randrange(1, 80)); w16(0x1F45, r.randrange(0x1000))
        wr(0x1F47, 0)
loop = 0x8000 + len(c); emit(0x4C, loop & 255, loop >> 8)
assert len(c) < 0x7E00, len(c)
rom = bytearray(1 << 17); rom[0:len(c)] = c; rom[0x7F00] = 0x40
for i in range(0x100, 0x7E00, 7): rom[i] = (i * 31) & 255 if i > len(c) + 16 else rom[i]
hdr = 0x7FC0
rom[hdr:hdr + 21] = b'ZSNES C4 TEST         '[:21]
rom[hdr + 0x15] = 0x20; rom[hdr + 0x16] = 0xF3; rom[hdr + 0x17] = 0x07; rom[hdr + 0x19] = 0x01; rom[hdr + 0x1A] = 0x33
for a, v in {0x7FEA: 0xFF00, 0x7FFC: 0x8000, 0x7FFE: 0xFF00}.items(): rom[a:a + 2] = struct.pack('<H', v)
cs = sum(rom) & 0xFFFF
rom[hdr + 0x1C:hdr + 0x20] = struct.pack('<HH', (~cs) & 0xFFFF, cs)
open(out, 'wb').write(rom)
