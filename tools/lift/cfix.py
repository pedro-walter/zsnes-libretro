#!/usr/bin/env python3
# Convert C sources so that variables shared with the lifted guest live at their fixed arena addresses.
#   cfix.py <addr-table: name hex size> <out-dir> files...
import sys, re, os
tab = {}
for sec in sys.argv[1:3]:
    pass
syms = {}
for l in open(sys.argv[1]):
    p = l.split()
    if len(p) >= 2: syms[p[0]] = int(p[1], 16)
outdir = sys.argv[2]
os.makedirs(outdir, exist_ok=True)
DECL = re.compile(r'^(\s*)(extern\s+)?(static\s+)?((?:(?:unsigned|signed|const|volatile|long|short|char|int|bool|BYTE|DWORD|WORD|u8|u16|u32|float|double|void|struct\s+\w+)\s+)+)((?:\*\s*)*)([A-Za-z_]\w*)\s*((?:\[[^\]]*\])*)\s*(=\s*[^;]*)?;\s*(//.*|/\*.*\*/)?\s*$')
MULTI = re.compile(r'^(\s*)(extern\s+)?(static\s+)?((?:(?:unsigned|signed|const|volatile|long|short|char|int|bool|BYTE|DWORD|WORD|u8|u16|u32|float|double)\s+)+)([^;=()]+,[^;=()]+);\s*$')
def macro(name, base, stars, dims, tval=None):
    a = syms[name]
    t = base.strip()
    if stars.strip():
        # guest pointer slot (32-bit): rvalue only
        return '#define %s ((%s %s)(uintptr_t)(*(volatile unsigned int *)0x%xu))\n#define %s_SLOT (*(volatile unsigned int *)0x%xu)' % (name, t, stars.replace(' ', ''), a, name, a)
    if dims:
        return '#define %s ((%s *)0x%xu)' % (name, t, a)
    return '#define %s (*(%s *)0x%xu)' % (name, t, a)
for f in sys.argv[3:]:
    out = ['#include <stdint.h>\n']
    depth = 0
    for l in open(f, errors='replace'):
        s = l.rstrip('\n')
        done = False
        if depth == 0 and not s.lstrip().startswith(('#', '//', '/*', 'typedef', 'return')):
            m = DECL.match(s)
            if m and m.group(6) in syms and not (m.group(8) and '(' in m.group(8) and False):
                if not m.group(3) or True:
                    out.append(macro(m.group(6), m.group(4), m.group(5), m.group(7)) + '\n'); done = True
            else:
                m = MULTI.match(s)
                if m:
                    names = [x.strip() for x in m.group(5).split(',')]
                    ok = []
                    for x in names:
                        mm = re.match(r'^((?:\*\s*)*)([A-Za-z_]\w*)\s*((?:\[[^\]]*\])*)$', x)
                        if not mm: ok = None; break
                        ok.append(mm)
                    if ok and all(mm.group(2) in syms for mm in ok):
                        for mm in ok: out.append(macro(mm.group(2), m.group(4), mm.group(1), mm.group(3)) + '\n')
                        done = True
                    elif ok and any(mm.group(2) in syms for mm in ok):
                        print('WARN mixed multi-decl', f, s, file=sys.stderr)
        if not done: out.append(l)
        depth += l.count('{') - l.count('}')
    open(os.path.join(outdir, os.path.basename(f)), 'w').write(''.join(out))
