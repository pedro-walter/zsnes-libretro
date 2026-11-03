#!/usr/bin/env python3
# cross-object control-flow references: xref.py [dst-prefix ...]
import sys, os, collections
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import objs, xdis
from xdis import *
import gen
P, OC, skip, t0 = gen.load_all()
names = {}
for n, (oi, off) in P.gtext.items(): names.setdefault((oi, off), n)
edges = collections.defaultdict(set)
for oi, oc in OC.items():
    for ins in oc.ins:
        if ins.mn in ('call', 'jmp') or ins.mn.startswith('j'):
            t = getattr(ins, 'tgt', None)
            if not t: continue
            kind, v = tgt_value(*t)
            if kind != 't': continue
            d = (v >> objs.CODE_SHIFT) - 1
            if d != oi:
                edges[(d, v & ((1 << objs.CODE_SHIFT) - 1))].add(oi)
pre = sys.argv[1:]
for (d, off), srcs in sorted(edges.items()):
    dn = P.objs[d].name
    if pre and not any(dn.startswith(p) for p in pre): continue
    print('%-22s %-28s <- %s' % (dn, names.get((d, off), hex(off)), ','.join(sorted(P.objs[s].name for s in srcs))))
