#!/usr/bin/env python3
# resolve so+0xOFF offsets (from imgdiff messages) to nearest symbol: tools/so_sym.py build/zsnes_libretro.so 0x1234 ...
import subprocess,sys,bisect
so=sys.argv[1]
syms=[]
for l in subprocess.run(['nm','-n','-S',so],capture_output=True,text=True).stdout.splitlines():
    t=l.split()
    if len(t)>=3 and all(c in '0123456789abcdef' for c in t[0]):
        syms.append((int(t[0],16),t[-1]))
addrs=[a for a,_ in syms]
for x in sys.argv[2:]:
    v=int(x,16); i=bisect.bisect_right(addrs,v)-1
    print(x, syms[i][1] if i>=0 else '?', '+%d'%(v-syms[i][0]) if i>=0 else '')
