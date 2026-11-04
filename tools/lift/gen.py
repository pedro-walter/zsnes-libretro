#!/usr/bin/env python3
# x86 -> C lifter driver:  gen.py <outdir>
import sys, os, glob, struct, time, collections
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import objs, xdis
from xdis import *
from emit import Emitter, Unsupported

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..'))
REFDIR = os.path.join(ROOT, 'build', 'ref')
SKIP_TEXT = ['video/hq', 'dos/sw', 'dos/debug', 'dos/modemrtn', 'dos/gppro', 'dos/vesa12', 'dos/joy']
KEEP_TEXT = ['dos/initvid']
FORCE_ARENA = ['zcfgdir', '_cchar', 'CMDLineStr']   # C buffers whose address is handed to the guest   # text needed even though dos/*

def regflags(toks):
    f = (1 if 'tail' in toks else 0) | (2 if 'noregs' in toks else 0)
    for t in toks:
        if t.startswith('cmp='):
            f |= 0x8000
            for r in t[4:].split(','): f |= 1 << (8 + ['eax', 'ecx', 'edx', 'ebx', 'esi', 'edi', 'ebp'].index(r))
    return f

def load_all():
    notsub = lambda p: '/ref/' not in p and '/lift' not in p
    ao = sorted(glob.glob(REFDIR + '/**/*.ao', recursive=True))
    co = sorted(glob.glob(REFDIR + '/**/*.co', recursive=True)) + [os.path.join(ROOT, 'build', 'platform.co')]
    t0 = time.time()
    P = objs.build_prog(ao, REFDIR, set(), co, [])
    skip = set()
    for o in P.objs:
        nm = o.name
        if any(nm.startswith(p) for p in SKIP_TEXT) and not any(nm.startswith(k) for k in KEEP_TEXT):
            skip.add(o.idx)
    P.skip_idx = skip
    P.skip_text_obj = lambda i: i in skip
    OC = {}
    P.codestarts = {}
    dent = xdis.find_data_entries(P)
    for o in P.objs:
        if o.idx in skip: continue
        OC[o.idx] = xdis.disasm_obj(P, o, dent[o.idx])
        for d in dent[o.idx] or ():
            pass
        for ins in OC[o.idx].ins:
            if ins.addr >= o.tpad:
                P.codestarts[o.data_addr + ins.addr - o.tpad] = (o.idx, ins.addr)
    # resolve relocations (needs full symbol tables)
    for oc in OC.values():
        xdis.resolve_ins(P, oc)
    print('disasm', time.time() - t0, file=sys.stderr)
    return P, OC, skip, t0

def main():
    outdir = sys.argv[1]
    os.makedirs(outdir, exist_ok=True)
    for old in glob.glob(os.path.join(outdir, 'lift_*.c')): os.remove(old)
    P, OC, skip, t0 = load_all()

    # self-modifying immediates: a store whose address is (code symbol + addend) pointing into the middle of an instruction
    # (mode7ext.asm patches the constants of its inner loop).  The patched instruction reads its immediate from a cell.
    import bisect
    CELLS = {}
    for oi, oc in OC.items():
        addrs = [i.addr for i in oc.ins]; starts = set(addrs); CELLS[oi] = []
        for ins in oc.ins:
            if ins.dispr is None or ins.dispr[0][0] != 't': continue
            _, v = tgt_value(ins.dispr[0], ins.dispr[1])
            if (v >> objs.CODE_SHIFT) - 1 != oi: continue
            a = v & ((1 << objs.CODE_SHIFT) - 1)
            if a in starts: continue
            tg = oc.ins[bisect.bisect_right(addrs, a) - 1]
            mop = [o for o in ins.ops if o.type == X86_OP_MEM]
            if not mop or tg.addr + tg.size <= a: raise Exception('%s+%x: store into code that is not inside an instruction' % (oc.obj.name, ins.addr))
            r = tg.raw
            if tg.pcell is None:
                if tg.immr is not None or not r.imm_size or r.imm_offset != a - tg.addr or r.imm_size != mop[0].size or r.imm_offset + r.imm_size != tg.size:
                    raise Exception('%s+%x: unsupported self-modifying store (target %x: imm off %s size %s, store at +%d size %d)' % (oc.obj.name, ins.addr, tg.addr, r.imm_offset, r.imm_size, a - tg.addr, mop[0].size))
                imm = [o for o in tg.ops if o.type == X86_OP_IMM][0]
                tg.pcell = len(CELLS[oi]); CELLS[oi].append(imm.imm & ((1 << (8 * r.imm_size)) - 1))
            ins.patch = tg
    # flag info
    taken = set()          # (objidx, off) address-taken code
    for oc in OC.values():
        for ins in oc.ins:
            ins.uses, ins.defs, ins.cond = flag_info(ins.mn, ins.ops, ins.rep)
            ins.lout = 0; ins.lin = 0
            # address-taken via imm/disp relocs
            for r in (ins.immr, ins.dispr):
                if r is ins.dispr and ins.patch is not None: continue
                if r is not None and r[0][0] == 't':
                    _, v = tgt_value(r[0], r[1])
                    taken.add(v)
    for o in P.objs:
        for off, symidx, typ in o.rel_data:
            if typ == objs.R_386_32:
                res = objs.resolve(P, o, symidx)
                addend = struct.unpack_from('<I', o.data, off)[0]
                if res[0] == 't':
                    taken.add((res[1] + addend) & 0xFFFFFFFF)
                elif res[0] == 'd' and ((res[1] + addend) & 0xFFFFFFFF) in P.codestarts:
                    oi, vo = P.codestarts[(res[1] + addend) & 0xFFFFFFFF]
                    taken.add(objs.code_id(oi, vo))

    def node_of(v):
        oi = (v >> objs.CODE_SHIFT) - 1
        if oi in OC:
            off = v & ((1 << objs.CODE_SHIFT) - 1)
            ci = OC[oi].byaddr.get(off)
            if ci is not None: return OC[oi].ins[ci]
        return None

    # successor structure
    for oc in OC.values():
        n = len(oc.ins)
        for ins in oc.ins:
            nxt = oc.ins[ins.ci + 1] if ins.ci + 1 < n else None
            mn = ins.mn
            tg = None
            if ins.tgt is not None:
                k, v = tgt_value(*ins.tgt)
                if k == 't': tg = node_of(v)
            ins.succ_kind = None
            ins.c4 = (nxt, tg)
            if mn == 'ret': ins.succ_kind = 'ret'
            elif mn == 'jmp': ins.succ_kind = 'jmp' if ins.tgt is not None else 'ijmp'
            elif mn in JCC or mn in ('loop', 'jecxz', 'jcxz'): ins.succ_kind = 'jcc'
            elif mn == 'call': ins.succ_kind = 'call' if ins.tgt is not None else 'icall'
            elif mn in ('hlt', 'iretd', 'int', 'ud2'): ins.succ_kind = 'end'
            else: ins.succ_kind = 'seq'
    allins = [i for oc in OC.values() for i in reversed(oc.ins)]
    idp = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'idexports.txt')
    if os.path.exists(idp):
        for ln in open(idp):
            ln = ln.split('#')[0].split()
            if ln:
                key = P.gtext.get(ln[0]) or P.local_text(ln[0])
                if key is not None: taken.add(objs.code_id(*key))
    ovp0 = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'overrides.txt')
    if os.path.exists(ovp0):          # overridden labels must be dispatchable (the original body is run through them)
        for ln in open(ovp0):
            ln = ln.split('#')[0].split()
            if ln and ln[0] != '-':
                key = P.gtext.get(ln[0]) or P.local_text(ln[0])
                if key is not None: taken.add(objs.code_id(*key))
    takennodes = [node_of(v) for v in taken]
    takennodes = [x for x in takennodes if x is not None]
    GRET = 0; GIND = 0
    changed = True; it = 0
    while changed:
        changed = False; it += 1
        for ins in allins:
            nxt, tg = ins.c4
            ln = nxt.lin if nxt is not None else 0
            lt = tg.lin if tg is not None else 0
            k = ins.succ_kind
            if k == 'seq': lo = ln
            elif k == 'jmp': lo = lt
            elif k == 'jcc': lo = lt | ln
            elif k == 'call': lo = lt | ln
            elif k == 'icall': lo = GIND | ln
            elif k == 'ret': lo = GRET
            elif k == 'ijmp': lo = GIND
            else: lo = 0
            if ins.cond: li = ins.uses | lo
            else: li = ins.uses | (lo & ~ins.defs)
            if lo != ins.lout or li != ins.lin:
                ins.lout = lo; ins.lin = li; changed = True
        g = 0
        for ins in allins:
            if ins.succ_kind in ('call', 'icall') and ins.c4[0] is not None:
                if ins.succ_kind == 'icall' or ins.c4[1] is not None: g |= ins.c4[0].lin
        gi = 0
        for x in takennodes: gi |= x.lin
        if g != GRET or gi != GIND:
            GRET, GIND = g, gi; changed = True
        print('live iter', it, 'GRET', GRET, 'GIND', GIND, file=sys.stderr)

    for n in FORCE_ARENA:
        if n not in P.gdata: objs.alloc_var(P, n)
    # C overrides of asm functions: tools/lift/overrides.txt  ("asm_label c_function"); the label's lifted body is
    # kept (reachable with lift_orig=1) so the original stays available for differential testing
    OVR = {}; ovr_list = []
    ovp = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'overrides.txt')
    if os.path.exists(ovp):
        for ln in open(ovp):
            ln = ln.split('#')[0].split()
            if not ln: continue
            key = None if ln[0] == '-' else (P.gtext.get(ln[0]) or P.local_text(ln[0]))
            if key is None and ln[0] != '-': raise Exception('override: unknown asm label ' + ln[0])
            if key is not None: OVR[key] = (len(ovr_list), ln[1])
            ovr_list.append((ln[0], ln[1], regflags(ln[2:]), key))
    # emit objects
    objnames = []
    unsup = collections.Counter()
    total_unsup = 0
    for oi in sorted(OC):
        oc = OC[oi]; o = oc.obj
        em = Emitter(P, oc)
        lines = {}
        # dispatch set: taken in this obj + return sites + global text syms
        disp = set()
        for v in taken:
            if (v >> objs.CODE_SHIFT) - 1 == oi: disp.add(v & ((1 << objs.CODE_SHIFT) - 1))
        for s in o.syms:
            if s['sec'] == '.text' and s['bind'] == 'STB_GLOBAL': disp.add(s['value'])
        for ins in oc.ins:
            if ins.mn == 'call':
                disp.add(ins.addr + ins.size)
        for ins in oc.ins:
            try:
                c = em.emit(ins)
            except Unsupported as e:
                unsup[(ins.mn, str(e))] += 1; total_unsup += 1
                c = 'FLUSH(); LIFT_TRAP("unsupported %s", %s);' % (ins.mn, hx_(objs.code_id(oi, ins.addr)))
            except Exception as e:
                print('ERR', o.name, hex(ins.addr), ins.mn, ins.raw.op_str, e, file=sys.stderr); raise
            lines[ins.addr] = c
        # fallthrough at end of object
        labels = em.need_label | disp
        fn = 'lift_' + o.cname
        objnames.append((oi, fn, o.name))
        with open(os.path.join(outdir, fn + '.c'), 'w') as f:
            has_mmx = any((i.mn.startswith('p') and i.mn not in ('push', 'pop', 'pushal', 'popal', 'pushfd', 'popfd')) or i.mn in ('movd', 'movq', 'emms') for i in oc.ins)
            f.write('/* generated by tools/lift/gen.py from %s -- do not edit */\n%s#include "lift.h"\n' % (o.name, '' if has_mmx else '#define LIFT_NOMM 1\n'))
            f.write('#define MYOBJ %d\n' % (oi + 1))
            for ci, cv in enumerate(CELLS[oi]): f.write('static u32 pc%d = 0x%xu;   /* runtime-patched immediate */\n' % (ci, cv))
            f.write('u32 %s(u32 pc)\n{\n    u32 eax,ecx,edx,ebx,esp,ebp,esi,edi; u8 cf,zf,sf,of,pf,af; LIFT_MMDECL\n    RELOAD();\n' % fn)
            f.write('dispatch:\n    if ((pc >> 21) != MYOBJ) { FLUSH(); return pc; }\n    switch (pc & 0x1FFFFF) {\n')
            for a in sorted(disp):
                if a in lines: f.write('    case 0x%x: goto L%x;\n' % (a, a))
                else: print('WARN: dispatch addr not insn', o.name, hex(a), file=sys.stderr)
            f.write('    default: FLUSH(); LIFT_BAD(pc);\n    }\n')
            f.write('do_ret: pc=M32(esp); esp+=4; goto dispatch;\n')
            for ins in oc.ins:
                if ins.addr in labels: f.write('L%x:\n' % ins.addr)
                if (oi, ins.addr) in OVR: f.write('    OVR_GUARD(%d)\n' % OVR[(oi, ins.addr)][0])
                f.write('    ' + lines[ins.addr] + '\n')
            f.write('    FLUSH(); LIFT_BAD(%s);\n}\n' % hx_(objs.code_id(oi, len(o.text))))
    print('emitted; unsupported:', total_unsup, file=sys.stderr)
    for k, v in unsup.most_common(40): print('   ', k, v, file=sys.stderr)

    # ---- data image ----
    img = bytearray(P.data_end - objs.DATA_BASE)
    for o in P.objs:
        d = bytearray(o.data)
        for off, symidx, typ in o.rel_data:
            res = objs.resolve(P, o, symidx)
            addend = struct.unpack_from('<I', d, off)[0]
            if typ == objs.R_386_32:
                val = (res[1] + addend) & 0xFFFFFFFF
                if res[0] == 'd' and val in P.codestarts:
                    val = objs.code_id(*P.codestarts[val])
                struct.pack_into('<I', d, off, val)
            elif typ == objs.R_386_PC32:
                pass   # branch inside data-resident code
            else:
                raise Exception('data reloc type %d' % typ)
        img[o.data_addr - objs.DATA_BASE:o.data_addr - objs.DATA_BASE + len(d)] = d
    for n, a in P.foreign_vars.items():
        if n in P.cvar_init:
            b = P.cvar_init[n]
            if a - objs.DATA_BASE + len(b) > len(img): img.extend(bytes(a - objs.DATA_BASE + len(b) - len(img)))
            img[a - objs.DATA_BASE:a - objs.DATA_BASE + len(b)] = b
    # final size may have grown by foreign vars (resolved during codegen) -> re-pad
    if len(img) < P.data_end - objs.DATA_BASE: img.extend(bytes(P.data_end - objs.DATA_BASE - len(img)))
    open(os.path.join(outdir, 'lift_data.bin'), 'wb').write(bytes(img))

    # ---- headers / tables ----
    with open(os.path.join(outdir, 'lift_syms.h'), 'w') as f:
        f.write('/* generated */\n#define LIFT_DATA_END 0x%x\n#define LIFT_DATA_BASE 0x%xu\n#define LIFT_ARENA_BASE 0x%xu\n#define LIFT_ARENA_SIZE 0x%xu\n' % (P.data_end, objs.DATA_BASE, objs.ARENA_BASE, objs.ARENA_SIZE))
        for name, a in sorted(list(P.gdata.items()) + list(P.ldata.items())): f.write('#define ZO_%s 0x%xu\n' % (name, a))
        for name, a in sorted(P.foreign_vars.items()): f.write('#define ZO_%s 0x%xu\n' % (name, a))
        for name, (oi, off) in sorted(P.gtext.items()):
            f.write('#define ZID_%s 0x%xu\n' % (name, objs.code_id(oi, off)))
        idp = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'idexports.txt')
        if os.path.exists(idp):
            for ln in open(idp):
                ln = ln.split('#')[0].split()
                if not ln: continue
                key = P.gtext.get(ln[0]) or P.local_text(ln[0])
                if key is None: raise Exception('idexports: unknown label ' + ln[0])
                f.write('#define ZID_%s 0x%xu\n' % (ln[0].split(':')[-1].replace('.', '_'), objs.code_id(*key)))
        for k_, (lab, c, tail, key) in enumerate(ovr_list):
            f.write('#define ZOVR_%s ((0x7Cu << 21) | %du)\n' % (c, k_))
            if key is None: continue
            nm = lab.split(':')[-1]
            if nm not in P.gtext: f.write('#define ZID_%s 0x%xu\n' % (nm, objs.code_id(*key)))
    with open(os.path.join(outdir, 'lift_tab.c'), 'w') as f:
        f.write('/* generated */\n#include "lift.h"\n')
        for oi, fn, nm in objnames: f.write('u32 %s(u32);\n' % fn)
        f.write('lift_fn_t lift_objtab[128] = {\n')
        tab = {}
        for oi, fn, nm in objnames: tab[oi + 1] = fn
        for i in range(128): f.write('    %s,\n' % (tab.get(i, '0')))
        f.write('};\n')
        f.write('const char *const lift_objname[128] = {\n')
        names = {o.idx + 1: o.name for o in P.objs}
        for i in range(128): f.write('    %s,\n' % ('"%s"' % names[i] if i in names else '0'))
        f.write('};\n')
        f.write('const char *const lift_ext_names[] = {\n')
        for n, i in sorted(P.foreign_funcs.items(), key=lambda x: x[1]): f.write('    "%s",\n' % n)
        f.write('    0 };\n')
        f.write('const int lift_ext_count = %d;\n' % len(P.foreign_funcs))
        f.write('const char *const lift_missing_names[] = {\n')
        for n, i in sorted(P.missing.items(), key=lambda x: x[1]): f.write('    "%s",\n' % n)
        f.write('    0 };\n')
    with open(os.path.join(outdir, 'lift_ovr.c'), 'w') as f:
        f.write('/* generated */\n#include "lift.h"\n')
        for a, c, tail, key in ovr_list:
            if tail & 1: f.write('u32 %s(void);\n' % c)
            else: f.write('void %s(void);\nstatic u32 ovw_%s(void) { %s(); return 0; }\n' % (c, c, c))
        f.write('u32 (*const lift_ovr_fns[])(void) = {\n')
        for a, c, tail, key in ovr_list: f.write('    %s,\n' % (c if tail & 1 else 'ovw_' + c))
        f.write('    0 };\nconst char *const lift_ovr_names[] = {\n')
        for a, c, tail, key in ovr_list: f.write('    "%s",\n' % a)
        f.write('    0 };\nconst int lift_ovr_count = %d;\n' % len(ovr_list))
        f.write('const char *const lift_ovr_cnames[] = {\n')
        for a, c, tail, key in ovr_list: f.write('    "%s",\n' % c)
        f.write('    0 };\n')
        f.write('const unsigned short lift_ovr_flags[] = {\n')
        for a, c, tail, key in ovr_list: f.write('    %d,\n' % tail)
        f.write('    0 };\n')
        f.write('const unsigned lift_ovr_ids[] = {\n')
        for a, c, tail, key in ovr_list: f.write('    0x%xu,\n' % (objs.code_id(*key) if key else 0))
        f.write('    0 };\n')
        syms = sorted(list(P.gdata.items()) + list(P.ldata.items()) + list(P.foreign_vars.items()), key=lambda x: x[1])
        tx = sorted((objs.code_id(oi, off), n) for n, (oi, off) in P.gtext.items())
        f.write('const unsigned lift_text_id[] = {\n')
        for i_, n_ in tx: f.write('    0x%xu,\n' % i_)
        f.write('    0 };\nconst char *const lift_text_name[] = {\n')
        for i_, n_ in tx: f.write('    "%s",\n' % n_)
        f.write('    0 };\nconst int lift_text_count = %d;\n' % len(tx))
        f.write('const unsigned lift_sym_addr[] = {\n')
        for n, a in syms: f.write('    0x%xu,\n' % a)
        f.write('    0 };\nconst char *const lift_sym_name[] = {\n')
        for n, a in syms: f.write('    "%s",\n' % n)
        f.write('    0 };\nconst int lift_sym_count = %d;\n' % len(syms))
    NOEXPORT = {'zs_mcall', 'zs_call_asm'}
    # lift_syms.ld: absolute symbols for the guest; lift_syms_u.ld: the same with a leading underscore (32-bit PE decoration)
    for fname, pre in (('lift_syms.ld', ''), ('lift_syms_u.ld', '_')):
        with open(os.path.join(outdir, fname), 'w') as f:
            for name, a in sorted(list(P.gdata.items()) + list(P.ldata.items())): f.write('%s%s = 0x%x;\n' % (pre, name, a))
            for name, a in sorted(P.foreign_vars.items()): f.write('%s%s = 0x%x;\n' % (pre, name, a))
            for name, (oi, off) in sorted(P.gtext.items()):
                if name not in NOEXPORT: f.write('%s%s = 0x%x;\n' % (pre, name, objs.code_id(oi, off)))
    OVR = {'malloc': 'zl_malloc', 'free': 'zl_free', 'putchar': 'zl_putchar', 'getchar': 'zl_getchar'}
    with open(os.path.join(outdir, 'lift_ext_tab.c'), 'w') as f:
        f.write('/* generated */\n')
        for n, i in sorted(P.foreign_funcs.items(), key=lambda x: x[1]):
            f.write('extern void %s(void);\n' % OVR.get(n, n))
        f.write('void *const lift_ext_fns[] = {\n')
        for n, i in sorted(P.foreign_funcs.items(), key=lambda x: x[1]):
            f.write('    (void *)%s,\n' % OVR.get(n, n))
        f.write('    0 };\n')
    with open(os.path.join(outdir, 'lift_cvars.txt'), 'w') as f:
        for name, a in sorted(P.gdata.items()): f.write('%s 0x%x\n' % (name, a))
        for name, a in sorted(P.foreign_vars.items()): f.write('%s 0x%x\n' % (name, a))
        for name, (oi, off) in sorted(P.gtext.items()):
            if name not in NOEXPORT: f.write('%s 0x%x\n' % (name, objs.code_id(oi, off)))
    with open(os.path.join(outdir, 'lift_ext.h'), 'w') as f:
        f.write('/* generated: foreign C functions called from lifted asm */\n')
        for n, i in sorted(P.foreign_funcs.items(), key=lambda x: x[1]): f.write('LIFT_EXT(%s)\n' % n)
    with open(os.path.join(outdir, 'lift_foreign_vars.txt'), 'w') as f:
        for n, a in sorted(P.foreign_vars.items()): f.write('%s %d 0x%x\n' % (n, P.cvar.get(n, 4), a))
    print('foreign funcs', len(P.foreign_funcs), 'foreign vars', len(P.foreign_vars), 'missing', len(P.missing), file=sys.stderr)
    print('missing:', ' '.join(sorted(P.missing)), file=sys.stderr)
    print('total', time.time() - t0, file=sys.stderr)

def hx_(v): return '0x%xu' % (v & 0xFFFFFFFF)

if __name__ == '__main__':
    main()
