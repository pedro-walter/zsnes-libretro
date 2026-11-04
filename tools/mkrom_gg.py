#!/usr/bin/env python3
# Game Genie test ROM: the main loop keeps writing the backdrop colour; prints the Game Genie code that patches the high byte.
#   mkrom_gg.py <out.sfc>   -> prints "GGCODE addr"
import sys, struct
out = sys.argv[1]
rom = bytearray(1 << 17)
c = bytearray()
def emit(*b): c.extend(b)
emit(0x78, 0x18, 0xFB, 0xE2, 0x20)                  # sei clc xce sep #$20
emit(0xA9, 0x0F, 0x8D, 0x00, 0x21)                  # brightness
loop = 0x8000 + len(c)
emit(0xA9, 0x00, 0x8D, 0x21, 0x21)                  # CGADD = 0
emit(0xA9, 0x00, 0x8D, 0x22, 0x21)                  # low byte
imm = len(c) + 1
emit(0xA9, 0x7C, 0x8D, 0x22, 0x21)                  # high byte (blue)
emit(0x4C, loop & 255, loop >> 8)
rom[0:len(c)] = c
rom[0x7F00] = 0x40
hdr = 0x7FC0
rom[hdr:hdr + 21] = b'ZSNES GG TEST        '[:21]
rom[hdr + 0x15] = 0x20; rom[hdr + 0x17] = 0x07; rom[hdr + 0x19] = 0x01; rom[hdr + 0x1A] = 0x33
for a, v in {0x7FEA: 0xFF00, 0x7FFC: 0x8000, 0x7FFE: 0xFF00}.items(): rom[a:a + 2] = struct.pack('<H', v)
cs = sum(rom) & 0xFFFF
rom[hdr + 0x1C:hdr + 0x20] = struct.pack('<HH', (~cs) & 0xFFFF, cs)
open(out, 'wb').write(rom)
alpha = "DF4709156BC8A23E"
def enc(addr, val):
    b = format(addr, '024b')
    ijkl, qrst, opab, cduv, wxef, ghmn = [b[i * 4:(i + 1) * 4] for i in range(6)]
    i_, j_, k_, l_ = ijkl; q, r, s_, t = qrst; o, p, a_, b_ = opab; c_, d, u, v = cduv; w, x, e, f = wxef; g, h, m, n = ghmn
    ns = [val >> 4, val & 15] + [int(z, 2) for z in (a_ + b_ + c_ + d, e + f + g + h, i_ + j_ + k_ + l_, m + n + o + p, q + r + s_ + t, u + v + w + x)]
    s = ''.join(alpha[n] for n in ns); return s[:4] + '-' + s[4:]
print(enc(0x808000 + imm, 0x03))
