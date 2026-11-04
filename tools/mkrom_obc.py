#!/usr/bin/env python3
# OBC-1 test ROM: random writes to the OBC registers ($7FF0-$7FF7), the clear trigger ($7A1E) and the register RAM.
#   mkrom_obc.py <out.sfc> <seed> [ops]
import sys, struct, random
out, seed = sys.argv[1], int(sys.argv[2]); n = int(sys.argv[3]) if len(sys.argv) > 3 else 400
r = random.Random(seed)
c = bytearray()
def emit(*b): c.extend(b)
emit(0x78, 0x18, 0xFB, 0xE2, 0x20, 0xA9, 0x0F, 0x8D, 0x00, 0x21)
def wr(a, v): emit(0xA9, v & 255, 0x8D, a & 255, a >> 8)
def rd(a): emit(0xAD, a & 255, a >> 8)
def sprite_set():
    for k in (6, 0, 1, 2, 3, 4, 5, 7):
        wr(0x7FF0 + k, r.randrange(256) if k != 6 else r.randrange(128))
for _ in range(n):
    k = r.random()
    if k < .15: wr(0x7A1E, r.randrange(256))
    elif k < .55: sprite_set() if r.random() < .3 else wr(0x7FF0 + r.randrange(8), r.randrange(256))
    elif k < .7: wr(0x6000 + r.randrange(0x2000), r.randrange(256))
    else: rd(0x6000 + r.choice([0x1800 + r.randrange(0x220), 0x1000 + r.randrange(0x100), r.randrange(0x2000)]))
loop = 0x8000 + len(c); emit(0x4C, loop & 255, loop >> 8)
assert len(c) < 0x7E00, len(c)
rom = bytearray(1 << 17); rom[0:len(c)] = c; rom[0x7F00] = 0x40
hdr = 0x7FC0
rom[hdr:hdr + 21] = b'ZSNES OBC TEST        '[:21]
rom[hdr + 0x15] = 0x30; rom[hdr + 0x16] = 0x25; rom[hdr + 0x17] = 0x07; rom[hdr + 0x19] = 0x01; rom[hdr + 0x1A] = 0x33
for a, v in {0x7FEA: 0xFF00, 0x7FFC: 0x8000, 0x7FFE: 0xFF00}.items(): rom[a:a + 2] = struct.pack('<H', v)
cs = sum(rom) & 0xFFFF
rom[hdr + 0x1C:hdr + 0x20] = struct.pack('<HH', (~cs) & 0xFFFF, cs)
open(out, 'wb').write(rom)
