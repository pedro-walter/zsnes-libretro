#!/usr/bin/env python3
# SuperFX test ROM: a 65816 stub starts the GSU on a block of random GSU code (valid-ish opcode mix incl. prefixes, branches, plots,
# RAM/ROM accesses) with random register contents, again and again.   mkrom_gsu.py <out.sfc> <seed>
import sys, random, struct
out, seed = sys.argv[1], int(sys.argv[2])
mode = sys.argv[3] if len(sys.argv) > 3 else 'rand'
rnd = random.Random(seed)
rom = bytearray(rnd.getrandbits(8) for _ in range(1 << 20))
# GSU code at file offset 0x2000 (SNES 00:A000)
g = bytearray()
BAN = {0x00, 0x1F, 0x2F, 0xAF, 0xFF, 0x3C, 0x98, 0x99, 0x9A, 0x9B, 0x9C, 0x9D}   # jumps that never check the opcode budget (the asm would spin forever)
ops_nonbranch = [o for o in range(0x10, 0x100) if o not in BAN]
def pick():
    r = rnd.random()
    if r < 0.12: return rnd.randrange(0x05, 0x10)               # branches
    if r < 0.20: return rnd.choice([0x3D, 0x3E, 0x3F])          # ALT prefixes
    if r < 0.30: return rnd.choice([o for o in range(0x10, 0x30) if o not in BAN])   # TO / WITH
    if r < 0.38: return rnd.randrange(0xB0, 0xC0)               # FROM
    if r < 0.43: return rnd.choice([0x4C, 0x4C, 0x4D, 0x4E, 0xDF])   # PLOT/COLOR
    if r < 0.50: return rnd.choice([o for o in range(0xA0, 0xB0) if o not in BAN])   # IBT / LMS / SMS
    if r < 0.56: return rnd.choice([o for o in range(0xF0, 0x100) if o not in BAN])  # IWT / LM / SM
    if r < 0.60: return rnd.choice([0x02, 0x01, 0x4F, 0x70])
    return rnd.choice(ops_nonbranch)
MAIN = [o for o in list(range(0x30, 0xAF)) + list(range(0xC0, 0xFF)) if o not in BAN and o not in (0x3D, 0x3E, 0x3F)]
PRE = [o for o in list(range(0x10, 0x20)) + list(range(0x20, 0x30)) + list(range(0xB0, 0xC0))]
PRE_SAFE = [o for o in PRE if o not in (0x1F, 0x2F)]
for i in range(900):
    r = rnd.random()
    alt = 0
    if r < 0.12:                                                 # branch (+ offset); the delay slot is whatever follows
        g.append(rnd.randrange(0x05, 0x10)); g.append(rnd.choice([rnd.randrange(0, 12), rnd.randrange(0xF0, 0x100)])); continue
    if rnd.random() < 0.5:
        alt = rnd.choice([1, 2, 3]); g.append(0x3C + alt)
    for _ in range(rnd.choice([0, 0, 1, 1, 2])): g.append(rnd.choice(PRE if rnd.random() < 0.2 else PRE_SAFE))
    o = rnd.choice(MAIN) if rnd.random() < 0.85 else rnd.choice([0x4C, 0x4C, 0x4D, 0x4E, 0xDF, 0x02, 0x01, 0x4F, 0x70])
    if o == 0xDF and alt == 3: o = 0x4C
    g.append(o)
    if 0xA0 <= o < 0xB0: g.append(rnd.getrandbits(8))
    elif o >= 0xF0: g.extend(rnd.getrandbits(8) for _ in range(2))
    if rnd.random() < 0.008: g.append(0x00)                      # STOP now and then
if mode in ('ops', 'ops15'):                                              # every opcode x ALT x prefix once, in random order, straight-line
    g = bytearray()
    combos = [(o, a, pre) for o in range(0x30, 0xFF) if o not in BAN and o not in (0x3D, 0x3E, 0x3F) for a in range(4) for pre in (None, 'TO', 'FROM', 'WITH')]
    if mode == 'ops15':       # R15 as source/destination (jumps, the FxTableb/FxTablec families), branches in the delay slot
        combos = [(o, a, pre) for o in list(range(0x05, 0x10)) + list(range(0x30, 0xFF)) if o not in BAN and o not in (0x3D, 0x3E, 0x3F) for a in range(4) for pre in ('TO15', 'FROM15', 'WITH15', 'WITH15', 'WITH', 'TO')]
    rnd.shuffle(combos)
    for o, a, pre in combos[:2600]:
        if o == 0xDF and a == 3: continue
        if a: g.append(0x3C + a)
        if pre: g.append({'TO': rnd.randrange(0x10, 0x1E), 'FROM': rnd.randrange(0xB0, 0xBE), 'WITH': rnd.randrange(0x20, 0x2E), 'TO15': 0x1F, 'FROM15': 0xBF, 'WITH15': 0x2F}[pre])
        g.append(o)
        if 0x05 <= o < 0x10: g.append(rnd.randrange(0, 6))
        elif 0xA0 <= o < 0xB0: g.append(rnd.getrandbits(8))
        elif o >= 0xF0: g.extend(rnd.getrandbits(8) for _ in range(2))
if mode == 'plot':                                             # CMODE / COLOR / PLOT / RPIX soup with random coordinates
    g = bytearray()
    def iwt(r, v): g.extend([0xF0 | r, v & 255, v >> 8])
    for i in range(700):
        k = rnd.random()
        if k < 0.25: iwt(rnd.choice([1, 2, 3, 4, 5]), rnd.getrandbits(16) & rnd.choice([0x7F, 0xFF]))
        elif k < 0.40: g.extend([0x3D, 0x20 + rnd.randrange(0, 6), 0x4E])               # CMODE from a register (via WITH R: reg -> source)
        elif k < 0.50: g.extend([0xB0 + rnd.randrange(0, 6), 0x4E])                     # COLOR
        elif k < 0.80: g.append(0x4C)
        elif k < 0.90: g.extend([0x3D, 0x4C])                                           # RPIX
        elif k < 0.95: g.extend([0x3E, 0x10 + rnd.randrange(0, 12), 0x4D])
        else: g.extend([0x3C + rnd.choice([1, 2]), 0xDF])
        if rnd.random() < 0.1: g.extend([0x0B, 0x02, 0x01, 0x01])
g.append(0x00); g.append(0x01)
for i in range(1, len(g)):
    if g[i] == 0xDF and g[i - 1] in (0x3F, 0x98, 0x99, 0x9A, 0x9B, 0x9C, 0x9D): g[i] = 0x01   # no ROMB (wild ROM bank)
    if g[i] in (0x98, 0x99, 0x9A, 0x9B, 0x9C, 0x9D) and g[i - 1] == 0x3D: g[i] = 0x01             # no LJMP
rom[0x2000:0x2000 + len(g)] = g
code = bytearray()
def emit(*b): code.extend(b)
def sta(addr, v): emit(0xA9, v & 255, 0x8D, addr & 255, addr >> 8)
emit(0x78, 0x18, 0xFB, 0xC2, 0x30, 0xA2, 0xFF, 0x1F, 0x9A, 0xE2, 0x20)
sta(0x2100, 0x8F)
sta(0x3034, 0x00)                                               # PBR
sta(0x3037, rnd.choice([0x00, 0x80, 0x20]))                     # CFGR
sta(0x3038, rnd.randrange(0, 8))                                # SCBR
sta(0x3039, rnd.choice([0, 1]))                                 # CLSR
sta(0x303A, 0x18 | rnd.randrange(0, 4) | rnd.choice([0x00, 0x04, 0x20, 0x24]))   # SCMR: RON, RAN, depth, height
sta(0x303C, 0x00)                                               # RAMBR
sta(0x3036, 0x00)                                               # ROMBR
loop = 0x8000 + len(code)
for r in range(15):                                             # random register contents
    sta(0x3000 + 2 * r, rnd.getrandbits(8)); sta(0x3001 + 2 * r, rnd.getrandbits(8) if rnd.random() < 0.6 else 0)
emit(0xA9, 0x00, 0x8D, 0x1E, 0x30)                              # R15 = 0xA000 + k : low byte
sta(0x301E, rnd.randrange(0, 0x40) * 2)
emit(0xA9, 0xA0, 0x8D, 0x1F, 0x30)                              # high byte write starts the GSU
wait = 0x8000 + len(code)
emit(0xAD, 0x30, 0x30, 0x29, 0x20, 0xD0, 0)                     # lda $3030 ; and #$20 ; bne wait
code[-1] = (wait - (0x8000 + len(code))) & 255
emit(0x4C, loop & 255, loop >> 8)
rom[0:len(code)] = code
rom[0x7F00] = 0x40
hdr = 0x7FC0
rom[hdr:hdr + 21] = b'ZSNES GSU FUZZ       '[:21]
rom[hdr + 0x15] = 0x20; rom[hdr + 0x16] = 0x13; rom[hdr + 0x17] = 0x0A; rom[hdr + 0x18] = 0x03; rom[hdr + 0x19] = 0x01; rom[hdr + 0x1A] = 0x33
for a, v in {0x7FE4: 0xFF00, 0x7FE6: 0xFF00, 0x7FE8: 0xFF00, 0x7FEA: 0xFF00, 0x7FEE: 0xFF00, 0x7FF4: 0xFF00, 0x7FF8: 0xFF00, 0x7FFA: 0xFF00, 0x7FFC: 0x8000, 0x7FFE: 0xFF00}.items():
    rom[a:a + 2] = struct.pack('<H', v)
cs = sum(rom) & 0xFFFF
rom[hdr + 0x1C:hdr + 0x20] = struct.pack('<HH', (~cs) & 0xFFFF, cs)
open(out, 'wb').write(rom)
