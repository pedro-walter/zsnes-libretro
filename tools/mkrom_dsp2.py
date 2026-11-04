#!/usr/bin/env python3
# DSP-2 test ROM: a model of the DSP-2 command queue drives long writes/reads to $3F:8000+ following the protocol
# (Dungeon Master style), with a few rogue writes at the end.   mkrom_dsp2.py <out.sfc> <seed> [steps]
import sys, struct, random
out, seed = sys.argv[1], int(sys.argv[2]); steps = int(sys.argv[3]) if len(sys.argv) > 3 else 120
r = random.Random(seed)
c = bytearray()
def emit(*b): c.extend(b)
emit(0x78, 0x18, 0xFB, 0xE2, 0x20, 0xA9, 0x0F, 0x8D, 0x00, 0x21)
def wr(addr, v): emit(0xA9, v & 255, 0x8F, addr & 255, (addr >> 8) & 255, 0x3F)
def rd(addr): emit(0xAF, addr & 255, (addr >> 8) & 255, 0x3F)
q = [(0, 0)]; nochk = False; org = 0
def add(cmd, lo=0): q.append((cmd, lo))
for _ in range(steps):
    if not q: q.append((0, 0))
    cmd, lo = q.pop(0)
    addr = 0x8000 | lo
    if nochk: addr = 0x8000 | r.randrange(0x40)
    if cmd == 0:
        nochk = False
        v = r.choice([1, 3, 5, 6, 9, 0xD, 0xF, 1, 3, 9, 6])
        wr(addr, v)
        if v == 1:
            for i in range(32): add(1, i)
            add(0)
        elif v == 3: add(2); add(0)
        elif v == 5: nochk = True; add(3)
        elif v == 6: add(6)
        elif v == 9:
            for i in range(4): add(8)
            add(0)
        elif v == 0xD: add(9)
        elif v == 0xF: add(0)
    elif cmd == 1: wr(addr, r.randrange(256))
    elif cmd == 2: wr(addr, r.randrange(256))
    elif cmd == 3:
        n = r.randrange(1, 9); wr(addr, n)
        for i in range(n): add(4, i)
        for i in range(n): add(5, i)
        add(0)
    elif cmd in (4, 5, 7, 8, 0xB): wr(addr, r.randrange(256))
    elif cmd == 6:
        n = r.randrange(1, 9); wr(addr, n)
        for i in range(n): add(7)
        add(0)
    elif cmd == 9:
        k = r.randrange(1, 17); wr(addr, k * 2 | r.randrange(2)); org = k; add(0xA)
    elif cmd == 0xA:
        k = r.randrange(1, 17); wr(addr, k * 2 | r.randrange(2))
        for i in range(org): add(0xB, i)
        add(0)
    if r.random() < 0.3: rd(0x8000 + r.randrange(0x100) | (0 if r.random() < .9 else 0x7000))
for _ in range(r.randrange(0, 4)): wr(0x8000 + r.randrange(0x40), r.randrange(256)); rd(0x8000 + r.randrange(16))
loop = 0x8000 + len(c); emit(0x4C, loop & 255, loop >> 8)
rom = bytearray(1 << 17); assert len(c) < 0x7E00
rom[0:len(c)] = c; rom[0x7F00] = 0x40
hdr = 0x7FC0
rom[hdr:hdr + 21] = b'ZSNES DSP2 TEST      '[:21]
rom[hdr + 0x15] = 0x20; rom[hdr + 0x16] = 0x05; rom[hdr + 0x17] = 0x07; rom[hdr + 0x19] = 0x01; rom[hdr + 0x1A] = 0x33
for a, v in {0x7FEA: 0xFF00, 0x7FFC: 0x8000, 0x7FFE: 0xFF00}.items(): rom[a:a + 2] = struct.pack('<H', v)
cs = sum(rom) & 0xFFFF
rom[hdr + 0x1C:hdr + 0x20] = struct.pack('<HH', (~cs) & 0xFFFF, cs)
open(out, 'wb').write(rom)
