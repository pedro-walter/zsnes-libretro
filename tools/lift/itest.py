#!/usr/bin/env python3
# Per-instruction differential test generator: every distinct instruction encoding found in the lifted objects is run
# natively (32-bit process) and through the lifter's emitted C on random machine states; results must match.
#   itest.py <outdir>   -> writes itest_gen.c  (build+run with `make -f Makefile.lift itest`)
import sys, os, struct, collections
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import gen, objs, xdis
from xdis import *
from emit import Emitter, Unsupported
from capstone.x86 import *

W_BASE = 0x20000000
EXCL = set(['jmp', 'call', 'ret', 'loop', 'jecxz', 'jcxz', 'int', 'in', 'out', 'cpuid', 'lar', 'iretd', 'hlt', 'cli', 'sti',
            'pushfd', 'popfd', 'cld', 'std', 'nop', 'emms', 'ud2', 'pushf', 'popf'] ) | set(JCC.keys())
REGN = {'eax': 0, 'ecx': 1, 'edx': 2, 'ebx': 3, 'esp': 4, 'ebp': 5, 'esi': 6, 'edi': 7}

def fmask(ins):
    mn = ins.mn
    ALLF = 63
    if mn in ('mul', 'imul'): return CF | OF
    if mn in ('div', 'idiv'): return 0
    if mn in ('and', 'or', 'xor', 'test'): return ALLF & ~AF
    if mn in ('shl', 'sal', 'shr', 'sar', 'shld', 'shrd'):
        cnt = ins.ops[2] if mn in ('shld', 'shrd') else (ins.ops[1] if len(ins.ops) > 1 else None)
        if cnt is None or (cnt.type == X86_OP_IMM and (cnt.imm & 31) == 1): return ALLF & ~AF
        if cnt.type == X86_OP_IMM and (cnt.imm & 31) == 0: return ALLF
        return ALLF & ~AF & ~OF
    if mn in ('rol', 'ror', 'rcl', 'rcr'):
        cnt = ins.ops[1] if len(ins.ops) > 1 else None
        if cnt is None or (cnt.type == X86_OP_IMM and (cnt.imm & 31) == 1): return CF | OF
        return CF
    if mn in ('daa', 'das'): return ALLF & ~OF
    return ALLF

def main():
    outdir = sys.argv[1]
    P, OC, skip, t0 = gen.load_all()
    class FakeObj: idx = 0; name = 'itest'
    class FakeOC: obj = FakeObj()
    P.skip_text_obj = lambda i: False
    em = Emitter(P, FakeOC())
    seen = {}
    for oc in OC.values():
        o = oc.obj
        for ins in oc.ins:
            if ins.mn in EXCL or ins.mn.startswith('f') and ins.mn not in ('fabs',) : continue
            b = bytearray(o.vtext[ins.addr:ins.addr + ins.size])
            ok = True
            for op in ins.ops:
                if op.type == X86_OP_MEM and op.mem.segment not in (0, X86_REG_DS, X86_REG_ES, X86_REG_SS): ok = False
                if op.type == X86_OP_REG and ins.raw.reg_name(op.reg) in ('es', 'ds', 'cs', 'ss', 'fs', 'gs'): ok = False
            if not ok: continue
            for roff in ins.relocs: b[roff:roff + 4] = b'\0\0\0\0'
            seen.setdefault((bytes(b), ins.rep), ins)
    print('unique instruction encodings:', len(seen), file=sys.stderr)
    tests = []
    unsup = collections.Counter()
    nt = 0
    for (raw, rep), ins in sorted(seen.items(), key=lambda kv: (kv[1].mn, kv[0][0])):
        b = bytearray(raw)
        i = ins.raw
        mems = [op for op in ins.ops if op.type == X86_OP_MEM]
        dispoff = -1
        if mems and not mems[0].mem.base and not mems[0].mem.index:
            if i.disp_size == 4:
                dispoff = i.disp_offset
                struct.pack_into('<I', b, dispoff, W_BASE + 0x200 + (nt % 32) * 4)
            else:
                continue
        # re-disassemble the patched bytes (no relocations) and emit C
        got = list(xdis.md.disasm(bytes(b), 0, 1))
        if not got or got[0].size != len(b): continue
        j = got[0]
        nins = Ins()
        nins.addr = 0; nins.size = j.size; nins.raw = j
        pre = j.mnemonic.split(' ')
        nins.rep = None; nins.mn = j.mnemonic
        if pre[0] in ('rep', 'repe', 'repne', 'repz', 'repnz', 'lock'):
            nins.rep = pre[0]; nins.mn = ' '.join(pre[1:])
        nins.ops = list(j.operands); nins.relocs = {}; nins.immr = None; nins.dispr = None; nins.tgt = None; nins.lout = 63
        # divisor aliasing with eax/edx -> skip
        if nins.mn in ('div', 'idiv'):
            d = nins.ops[0]
            if d.type == X86_OP_REG and j.reg_name(d.reg) in ('eax', 'edx', 'ax', 'dx', 'al', 'ah', 'dl', 'dh'): continue
            if d.type == X86_OP_MEM and (d.mem.base in (X86_REG_EAX, X86_REG_EDX) or d.mem.index in (X86_REG_EAX, X86_REG_EDX)): continue
        try:
            stmt = em.emit(nins)
        except Unsupported as e:
            unsup[(nins.mn, str(e))] += 1; continue
        memdesc = []
        bad = False
        for op in [o for o in nins.ops if o.type == X86_OP_MEM]:
            m = op.mem
            base = REGN[j.reg_name(m.base)] if m.base else -1
            idx = REGN[j.reg_name(m.index)] if m.index else -1
            if base >= 0 and base == idx: bad = True
            memdesc.append((base, idx, m.scale if m.index else 1, m.disp))
        if bad or len(memdesc) > 2: continue
        # a register used as memory base must not also be written in a way that breaks the test -> harmless (both sides see the same)
        sz = nins.ops[0].size if nins.ops else 0
        divreg = -1
        if nins.mn in ('div', 'idiv') and nins.ops[0].type == X86_OP_REG:
            n = j.reg_name(nins.ops[0].reg)
            full = {'bx': 'ebx', 'cx': 'ecx', 'si': 'esi', 'di': 'edi', 'bp': 'ebp', 'bl': 'ebx', 'bh': 'ebx', 'cl': 'ecx', 'ch': 'ecx'}.get(n, n)
            divreg = REGN.get(full, -1)
        tests.append((bytes(b), nins.mn, nins.rep is not None, stmt, memdesc, fmask(nins), dispoff, sz, divreg))
        nt += 1
    print('tests:', len(tests), 'unsupported by emitter:', sum(unsup.values()), dict(unsup.most_common(8)), file=sys.stderr)
    with open(os.path.join(outdir, 'itest_gen.c'), 'w') as f:
        f.write('#include "itest.h"\n')
        for k, t in enumerate(tests):
            f.write('static void lt%d(tstate *s) { u32 eax=s->eax,ecx=s->ecx,edx=s->edx,ebx=s->ebx,esp=s->esp,ebp=s->ebp,esi=s->esi,edi=s->edi; u8 cf=s->cf,zf=s->zf,sf=s->sf,of=s->of,pf=s->pf,af=s->af; mmx_t mm[8]; memcpy(mm,s->mm,sizeof mm);\n  %s\n  s->eax=eax;s->ecx=ecx;s->edx=edx;s->ebx=ebx;s->esp=esp;s->ebp=ebp;s->esi=esi;s->edi=edi;s->cf=cf;s->zf=zf;s->sf=sf;s->of=of;s->pf=pf;s->af=af; memcpy(s->mm,mm,sizeof mm); }\n' % (k, t[3]))
        f.write('const test_t itests[] = {\n')
        for k, t in enumerate(tests):
            raw, mn, rep, stmt, md, fm, dispoff, sz, divreg = t
            m0 = md[0] if len(md) > 0 else (-2, 0, 0, 0)
            m1 = md[1] if len(md) > 1 else (-2, 0, 0, 0)
            f.write('  { "%s", {%s}, %d, %d, %d, %d, %d, {%d,%d,%d,%d}, {%d,%d,%d,%d}, lt%d },\n' % (
                mn, ','.join(str(x) for x in raw), len(raw), 1 if rep else 0, fm, sz, divreg,
                m0[0], m0[1], m0[2], m0[3], m1[0], m1[1], m1[2], m1[3], k))
        f.write('};\nconst int nitests = %d;\n' % len(tests))

if __name__ == '__main__':
    main()
