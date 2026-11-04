# Disassembly with relocation resolution + flag liveness.
import struct, sys
from capstone import *
from capstone.x86 import *
import objs

CF, ZF, SF, OF, PF, AF = 1, 2, 4, 8, 16, 32
ALL = 63

class Ins:
    __slots__ = ('addr', 'size', 'mn', 'ops', 'rep', 'tgt', 'relocs', 'ci', 'uses', 'defs', 'cond', 'lout', 'lin', 'succ_kind', 'raw', 'immr', 'dispr', 'c4', 'patch', 'pcell')

md = Cs(CS_ARCH_X86, CS_MODE_32)
md.detail = True

def s32(x):
    x &= 0xFFFFFFFF
    return x - (1 << 32) if x & 0x80000000 else x

JCC = {'jo': OF, 'jno': OF, 'jb': CF, 'jae': CF, 'je': ZF, 'jne': ZF, 'jbe': CF | ZF, 'ja': CF | ZF, 'js': SF, 'jns': SF,
       'jp': PF, 'jnp': PF, 'jl': SF | OF, 'jge': SF | OF, 'jle': SF | ZF | OF, 'jg': SF | ZF | OF,
       'jnae': CF, 'jc': CF, 'jnc': CF, 'jnb': CF, 'jz': ZF, 'jnz': ZF, 'jna': CF | ZF, 'jnbe': CF | ZF, 'jpe': PF, 'jpo': PF,
       'jnge': SF | OF, 'jnl': SF | OF, 'jng': SF | ZF | OF, 'jnle': SF | ZF | OF}
COND = {'o': OF, 'no': OF, 'b': CF, 'ae': CF, 'e': ZF, 'ne': ZF, 'be': CF | ZF, 'a': CF | ZF, 's': SF, 'ns': SF, 'p': PF, 'np': PF,
        'l': SF | OF, 'ge': SF | OF, 'le': SF | ZF | OF, 'g': SF | ZF | OF, 'nae': CF, 'c': CF, 'nc': CF, 'nb': CF, 'z': ZF, 'nz': ZF,
        'na': CF | ZF, 'nbe': CF | ZF, 'pe': PF, 'po': PF, 'nge': SF | OF, 'nl': SF | OF, 'ng': SF | ZF | OF, 'nle': SF | ZF | OF}

def flag_info(mn, ops, rep):
    """returns (uses, defs, conditional_defs) as bit masks"""
    m = mn
    if m in JCC: return JCC[m], 0, False
    if m.startswith('set') and m[3:] in COND: return COND[m[3:]], 0, False
    if m.startswith('cmov') and m[4:] in COND: return COND[m[4:]], 0, False
    if m in ('add', 'sub', 'cmp', 'and', 'or', 'xor', 'test', 'neg'): return 0, ALL, False
    if m in ('adc', 'sbb'): return CF, ALL, False
    if m in ('inc', 'dec'): return 0, ALL & ~CF, False
    if m in ('shl', 'sal', 'shr', 'sar', 'shld', 'shrd', 'rol', 'ror'):
        d = ALL if m in ('shl', 'sal', 'shr', 'sar', 'shld', 'shrd') else (CF | OF)
        if m in ('shld', 'shrd'):
            cnt = ops[2]
        else:
            cnt = ops[1] if len(ops) > 1 else None
        if cnt is not None and cnt.type == X86_OP_IMM:
            if (cnt.imm & 31) == 0: return 0, 0, False
            return 0, d, False
        if cnt is None: return 0, d, False  # shift by 1 form
        return 0, d, True
    if m in ('rcl', 'rcr'):
        cnt = ops[1] if len(ops) > 1 else None
        if cnt is not None and cnt.type == X86_OP_IMM and (cnt.imm & 31) == 0: return CF, 0, False
        return CF, CF | OF, cnt is not None and cnt.type != X86_OP_IMM
    if m in ('mul', 'imul'): return 0, CF | OF | (ALL & ~(CF | OF)), False
    if m == 'lahf': return SF | ZF | AF | PF | CF, 0, False
    if m == 'sahf': return 0, SF | ZF | AF | PF | CF, False
    if m in ('pushf', 'pushfd'): return ALL, 0, False
    if m in ('popf', 'popfd'): return 0, ALL, False
    if m == 'cmc': return CF, CF, False
    if m in ('clc', 'stc'): return 0, CF, False
    if m in ('bsf', 'bsr'): return 0, ZF, False
    if m in ('bt', 'bts', 'btr', 'btc'): return 0, CF, False
    if m in ('daa', 'das'): return CF | AF, ALL, False
    if m in ('adc',): return CF, ALL, False
    if m.startswith('f') and m in ('fcom', 'fcomp', 'fucom', 'fucomp', 'fcompp'): return 0, 0, False
    if rep and rep.startswith('repe'): return ZF, 0, False
    return 0, 0, False

class ObjCode:
    pass

def find_data_entries(P):
    """code entry points inside .data sections: PC32 relocs (calls/jumps) resolving into some object's .data range"""
    import bisect
    ents = {o.idx: set() for o in P.objs}
    ranges = sorted([(o.data_addr, o.data_addr + len(o.data), o) for o in P.objs if len(o.data)], key=lambda r: r[0])
    starts = [r[0] for r in ranges]
    def owner(a):
        i = bisect.bisect_right(starts, a) - 1
        if i >= 0 and ranges[i][0] <= a < ranges[i][1]: return ranges[i][2]
        return None
    for o in P.objs:
        for rels, buf in ((o.rel_text, o.text), (o.rel_data, o.data)):
            for off, symidx, typ in rels:
                if typ != objs.R_386_PC32: continue
                res = objs.resolve(P, o, symidx)
                if res[0] != 'd': continue
                addend = struct.unpack_from('<i', buf, off)[0]
                a = res[1] + addend + 4
                w = owner(a)
                if w is not None: ents[w.idx].add(a - w.data_addr)
    return ents

def disasm_obj(P, o, data_entries=()):
    oc = ObjCode()
    oc.obj = o
    oc.ins = []
    oc.byaddr = {}
    text = o.text
    o.tpad = (len(text) + 15) // 16 * 16
    o.vtext = bytes(text) + bytes(o.tpad - len(text)) + bytes(o.data)
    rel = {}
    for off, symidx, typ in o.rel_text:
        rel[off] = (symidx, typ)
    for off, symidx, typ in o.rel_data:
        rel[off + o.tpad] = (symidx, typ)
    def mk(i):
        ins = Ins()
        ins.addr = i.address; ins.size = i.size; ins.mn = i.mnemonic
        ins.raw = i
        pre = i.mnemonic.split(' ')
        ins.rep = None
        if pre[0] in ('rep', 'repe', 'repne', 'repz', 'repnz', 'lock'):
            ins.rep = pre[0]; ins.mn = ' '.join(pre[1:])
        ins.ops = list(i.operands)
        ins.relocs = {}
        for r in range(i.address, i.address + i.size):
            if r in rel: ins.relocs[r - i.address] = rel[r]
        ins.immr = None; ins.dispr = None; ins.tgt = None; ins.patch = None; ins.pcell = None
        return ins
    off = 0
    n = len(text)
    while off < n:
        got = False
        for i in md.disasm(text[off:], off):
            got = True
            ins = mk(i)
            ins.ci = len(oc.ins)
            oc.byaddr[ins.addr] = ins.ci
            oc.ins.append(ins)
            off = i.address + i.size
        if off < n and not got:
            raise Exception('%s: undecodable at %x' % (o.name, off))
        if off < n:
            raise Exception('%s: disasm stopped at %x of %x' % (o.name, off, n))
    if data_entries:
        seen = {}
        work = sorted(data_entries)
        dstart = o.tpad
        while work:
            d = work.pop()
            while True:
                if d in seen or d >= len(o.data): break
                got = list(md.disasm(o.vtext[dstart + d: dstart + d + 16], dstart + d, 1))
                if not got:
                    raise Exception('%s: undecodable data code at data+%x' % (o.name, d))
                i = got[0]
                ins = mk(i)
                seen[d] = ins
                mn = ins.mn
                if (mn in JCC or mn in ('jmp', 'call', 'loop', 'jecxz', 'jcxz')) and i.operands and i.operands[0].type == X86_OP_IMM and not ins.relocs:
                    work.append(i.operands[0].imm - dstart)
                for roff, (symidx, typ) in ins.relocs.items():
                    if typ == objs.R_386_PC32:
                        res = objs.resolve(P, o, symidx)
                        if res[0] == 'd':
                            addend = struct.unpack_from('<i', o.vtext, ins.addr + roff)[0]
                            a = res[1] + addend + 4
                            if o.data_addr <= a < o.data_addr + len(o.data): work.append(a - o.data_addr)
                if mn in ('ret', 'jmp', 'hlt', 'iretd'):
                    break
                d += i.size
        for d in sorted(seen):
            ins = seen[d]
            ins.ci = len(oc.ins)
            oc.byaddr[ins.addr] = ins.ci
            oc.ins.append(ins)
    return oc

def sval(v):
    return v & 0xFFFFFFFF

def resolve_ins(P, oc):
    """attach relocation targets: ins.immr/dispr = (kind, value[, name]); ins.tgt for branches"""
    o = oc.obj
    for ins in oc.ins:
        i = ins.raw
        for roff, (symidx, typ) in ins.relocs.items():
            res = objs.resolve(P, o, symidx)
            addend = struct.unpack_from('<i', o.vtext, ins.addr + roff)[0]
            if typ == objs.R_386_PC32:
                # target = S + A + 4 when the field is the last 4 bytes (rel32 branches)
                a2 = addend + 4 + (ins.size - roff - 4)
                if res[0] == 'd' and ((res[1] + a2) & 0xFFFFFFFF) in P.codestarts:
                    oi, vo = P.codestarts[(res[1] + a2) & 0xFFFFFFFF]
                    res = ('t', objs.code_id(oi, vo), oi, vo); a2 = 0
                ins.tgt = (res, a2)
            elif typ == objs.R_386_32:
                if roff == i.disp_offset and i.disp_size:
                    ins.dispr = (res, addend)
                elif roff == i.imm_offset and i.imm_size:
                    if res[0] == 'd' and ((res[1] + addend) & 0xFFFFFFFF) in P.codestarts:
                        oi, vo = P.codestarts[(res[1] + addend) & 0xFFFFFFFF]
                        res = ('t', objs.code_id(oi, vo), oi, vo); addend = 0
                    ins.immr = (res, addend)
                else:
                    raise Exception('%s+%x: reloc at %d not disp(%s)/imm(%s)' % (o.name, ins.addr, roff, i.disp_offset, i.imm_offset))
            else:
                raise Exception('reloc type %d' % typ)
        # direct branch target without reloc (same section)
        if ins.mn in JCC or ins.mn in ('jmp', 'call', 'loop', 'loope', 'loopne', 'jecxz', 'jcxz') :
            if ins.ops and ins.ops[0].type == X86_OP_IMM and ins.tgt is None:
                ins.tgt = (('t', objs.code_id(o.idx, ins.ops[0].imm), o.idx, ins.ops[0].imm), 0)

def tgt_value(res, add):
    # returns (kind, id/addr+add)
    if res[0] in ('t', 'x', 'm'): return res[0], (res[1] + add) & 0xFFFFFFFF
    return 'd', (res[1] + add) & 0xFFFFFFFF
