# Instruction -> C emission for the x86 lifter.
import sys
from capstone.x86 import *
import objs
from xdis import *

REGS32 = ['eax', 'ecx', 'edx', 'ebx', 'esp', 'ebp', 'esi', 'edi']
R16 = {'ax': 'eax', 'cx': 'ecx', 'dx': 'edx', 'bx': 'ebx', 'sp': 'esp', 'bp': 'ebp', 'si': 'esi', 'di': 'edi'}
R8L = {'al': 'eax', 'cl': 'ecx', 'dl': 'edx', 'bl': 'ebx'}
R8H = {'ah': 'eax', 'ch': 'ecx', 'dh': 'edx', 'bh': 'ebx'}
TY = {8: 'u8', 16: 'u16', 32: 'u32', 64: 'u64'}
STY = {8: 's8', 16: 's16', 32: 's32', 64: 's64'}
MASK = {8: 0xFF, 16: 0xFFFF, 32: 0xFFFFFFFF}

def hx(v):
    return '0x%xu' % (v & 0xFFFFFFFF)

class Unsupported(Exception):
    pass

MMX_BIN = {
    'paddb', 'paddw', 'paddd', 'paddsb', 'paddsw', 'paddusb', 'paddusw', 'psubb', 'psubw', 'psubd', 'psubsb', 'psubsw', 'psubusb', 'psubusw',
    'pmullw', 'pmulhw', 'pmaddwd', 'pand', 'pandn', 'por', 'pxor', 'pcmpeqb', 'pcmpeqw', 'pcmpeqd', 'pcmpgtb', 'pcmpgtw', 'pcmpgtd',
    'packuswb', 'packsswb', 'packssdw', 'punpcklbw', 'punpcklwd', 'punpckldq', 'punpckhbw', 'punpckhwd', 'punpckhdq',
}
MMX_SHIFT = {'psllw', 'pslld', 'psllq', 'psrlw', 'psrld', 'psrlq', 'psraw', 'psrad'}

class Emitter:
    def __init__(self, P, oc):
        self.P = P; self.oc = oc; self.o = oc.obj
        self.need_label = set()
        self.dispatch = set()
        self.unsup = {}

    # ---------- operands ----------
    def rn(self, ins, op):
        return ins.raw.reg_name(op.reg)

    def rd_reg(self, n):
        if n in REGS32: return n
        if n in R16: return '(u16)%s' % R16[n]
        if n in R8L: return '(u8)%s' % R8L[n]
        if n in R8H: return '(u8)(%s>>8)' % R8H[n]
        raise Unsupported('reg ' + n)

    def wr_reg(self, n, v):
        if n in REGS32: return '%s=%s;' % (n, v)
        if n in R16: r = R16[n]; return '%s=(%s&0xFFFF0000u)|(u16)(%s);' % (r, r, v)
        if n in R8L: r = R8L[n]; return '%s=(%s&0xFFFFFF00u)|(u8)(%s);' % (r, r, v)
        if n in R8H: r = R8H[n]; return '%s=(%s&0xFFFF00FFu)|((u32)(u8)(%s)<<8);' % (r, r, v)
        raise Unsupported('reg ' + n)

    def ea(self, ins, op):
        m = op.mem
        if m.segment not in (0, X86_REG_DS, X86_REG_ES, X86_REG_SS):
            raise Unsupported('segment %d' % m.segment)
        parts = []
        if m.base: parts.append(ins.raw.reg_name(m.base))
        if m.index:
            nm = ins.raw.reg_name(m.index)
            parts.append('%s*%d' % (nm, m.scale) if m.scale != 1 else nm)
        if ins.dispr is not None:
            res, add = ins.dispr
            if res[0] == 't': raise Unsupported('self-modifying/code-memory access')
            _, c = tgt_value(res, add)
        else:
            c = m.disp
        if c or not parts: parts.append(hx(c))
        return '(u32)(%s)' % ' + '.join(parts)

    def mem(self, size, addr):
        return {1: 'M8', 2: 'M16', 4: 'M32', 8: 'M64'}[size] + '(' + addr + ')'

    def size_bits(self, op):
        return op.size * 8

    def rd(self, ins, op, bits=None):
        if op.type == X86_OP_REG:
            return self.rd_reg(self.rn(ins, op))
        if op.type == X86_OP_IMM:
            if ins.immr is not None:
                res, add = ins.immr
                _, v = tgt_value(res, add)
                return hx(v)
            b = bits or op.size * 8
            return hx(op.imm & MASK[b])
        if op.type == X86_OP_MEM:
            return self.mem(op.size, self.ea(ins, op))
        raise Unsupported('operand type')

    def wr(self, ins, op, v):
        if op.type == X86_OP_REG:
            return self.wr_reg(self.rn(ins, op), v)
        if op.type == X86_OP_MEM:
            return '%s=%s;' % (self.mem(op.size, self.ea(ins, op)), v)
        raise Unsupported('wr operand')

    # ---------- flags ----------
    @staticmethod
    def fl(live, **d):
        s = []
        for name, bit in (('cf', CF), ('zf', ZF), ('sf', SF), ('of', OF), ('pf', PF), ('af', AF)):
            if (live & bit) and name in d: s.append('%s=%s;' % (name, d[name]))
        return ' '.join(s)

    @staticmethod
    def zsp(bits):
        return dict(zf='!r', sf='(r>>%d)&1' % (bits - 1), pf='PAR(r)')

    @staticmethod
    def cond(cc):
        c = {'o': 'of', 'no': '!of', 'b': 'cf', 'c': 'cf', 'nae': 'cf', 'ae': '!cf', 'nb': '!cf', 'nc': '!cf', 'e': 'zf', 'z': 'zf', 'ne': '!zf', 'nz': '!zf',
             'be': '(cf|zf)', 'na': '(cf|zf)', 'a': '!(cf|zf)', 'nbe': '!(cf|zf)', 's': 'sf', 'ns': '!sf', 'p': 'pf', 'pe': 'pf', 'np': '!pf', 'po': '!pf',
             'l': '(sf!=of)', 'nge': '(sf!=of)', 'ge': '(sf==of)', 'nl': '(sf==of)', 'le': '(zf|(sf!=of))', 'ng': '(zf|(sf!=of))', 'g': '!(zf|(sf!=of))', 'nle': '!(zf|(sf!=of))'}
        return c[cc]

    # ---------- control transfer ----------
    def retid(self, ins):
        return hx(objs.code_id(self.o.idx, ins.addr + ins.size))

    def dtarget(self, ins, is_call):
        res, add = ins.tgt
        kind, v = tgt_value(res, add)
        pre = ''
        if is_call and kind in ('t',):
            pre = 'esp-=4; M32(esp)=%s; ' % self.retid(ins)
        if kind == 't':
            oi = (v >> objs.CODE_SHIFT) - 1
            off = v & ((1 << objs.CODE_SHIFT) - 1)
            if oi == self.o.idx and not self.P.skip_text_obj(oi):
                self.need_label.add(off)
                return pre + 'goto L%x;' % off
            if self.P.skip_text_obj(oi):
                return pre + 'FLUSH(); return LIFT_MISSING(%s);' % hx(v)
            return pre + 'FLUSH(); return %s;' % hx(v)
        if kind == 'x':
            return 'FLUSH(); {u32 t_=ext_call(%s); RELOAD(); eax=t_;}%s' % (hx(v), '' if is_call else ' goto do_ret;')
        if kind == 'm':
            return 'FLUSH(); return LIFT_MISSING(%s);' % hx(v)
        raise Unsupported('direct branch to data')

    # ---------- main per-instruction emission ----------
    def emit(self, ins):
        mn = ins.mn
        ops = ins.ops
        live = ins.lout
        f = getattr(self, 'i_' + mn, None)
        if f is not None:
            return f(ins, ops, live)
        if mn in JCC:
            return 'if (%s) {%s}' % (self.cond(mn[1:]), self.dtarget(ins, False))
        if mn.startswith('set') and mn[3:] in COND:
            return self.wr(ins, ops[0], '(%s)?1:0' % self.cond(mn[3:]))
        if mn.startswith('cmov') and mn[4:] in COND:
            return 'if (%s) {%s}' % (self.cond(mn[4:]), self.wr(ins, ops[0], self.rd(ins, ops[1])))
        if mn in MMX_BIN:
            return 'mm[%d]=mm_%s(mm[%d],%s);' % (self.mmi(ins, ops[0]), mn, self.mmi(ins, ops[0]), self.mmsrc(ins, ops[1]))
        if mn in MMX_SHIFT:
            if ops[1].type == X86_OP_IMM:
                cnt = '%du' % (ops[1].imm & 0xFF)
            else:
                cnt = '(%s).q' % self.mmsrc(ins, ops[1]) if ops[1].type == X86_OP_REG else '%s' % self.mem(8, self.ea(ins, ops[1]))
                cnt = '(u32)(%s>0xFF?0xFF:%s)' % (cnt, cnt)
            return 'mm[%d]=mm_%s(mm[%d],%s);' % (self.mmi(ins, ops[0]), mn, self.mmi(ins, ops[0]), cnt)
        raise Unsupported(mn)

    def mmi(self, ins, op):
        n = self.rn(ins, op)
        return int(n[2:])

    def mmsrc(self, ins, op):
        if op.type == X86_OP_REG:
            n = self.rn(ins, op)
            if n.startswith('mm'): return 'mm[%d]' % int(n[2:])
            raise Unsupported('mmx src reg')
        return 'mm_ld(%s)' % self.ea(ins, op)

    # ---- data movement ----
    def i_mov(self, ins, ops, live):
        if ops[0].type == X86_OP_REG and self.rn(ins, ops[0]) in ('es', 'ds', 'cs', 'ss', 'fs', 'gs'): return ''
        if ops[1].type == X86_OP_REG and self.rn(ins, ops[1]) in ('es', 'ds', 'cs', 'ss', 'fs', 'gs'): return self.wr(ins, ops[0], '0')
        return self.wr(ins, ops[0], self.rd(ins, ops[1], ops[0].size * 8))

    def i_movzx(self, ins, ops, live):
        return self.wr(ins, ops[0], '(u32)' + self.rd(ins, ops[1]))

    def i_movsx(self, ins, ops, live):
        sb = ops[1].size * 8
        db = ops[0].size * 8
        return self.wr(ins, ops[0], '(%s)(%s)%s' % (TY[db], STY[sb], self.rd(ins, ops[1])))

    def i_lea(self, ins, ops, live):
        return self.wr(ins, ops[0], self.ea(ins, ops[1]))

    def i_xchg(self, ins, ops, live):
        b = ops[0].size * 8
        return '{%s a_=%s, b_=%s; %s %s}' % (TY[b], self.rd(ins, ops[0]), self.rd(ins, ops[1]), self.wr(ins, ops[0], 'b_'), self.wr(ins, ops[1], 'a_'))

    def i_bswap(self, ins, ops, live):
        return self.wr(ins, ops[0], '__builtin_bswap32(%s)' % self.rd(ins, ops[0]))

    def i_nop(self, ins, ops, live):
        return ''

    def i_cwde(self, ins, ops, live):
        return 'eax=(u32)(s32)(s16)eax;'

    def i_cbw(self, ins, ops, live):
        return 'eax=(eax&0xFFFF0000u)|(u16)(s16)(s8)eax;'

    def i_cdq(self, ins, ops, live):
        return 'edx=(u32)((s32)eax>>31);'

    def i_cwd(self, ins, ops, live):
        return 'edx=(edx&0xFFFF0000u)|(u16)((s16)eax>>15);'

    def i_push(self, ins, ops, live):
        sz = ops[0].size
        if ops[0].type == X86_OP_REG and self.rn(ins, ops[0]) in ('es', 'ds', 'cs', 'ss', 'fs', 'gs'):
            return 'esp-=4; M32(esp)=0;'
        if sz == 2:
            return '{u16 v_=%s; esp-=2; M16(esp)=v_;}' % self.rd(ins, ops[0], 16)
        return '{u32 v_=%s; esp-=4; M32(esp)=v_;}' % self.rd(ins, ops[0], 32)

    def i_pop(self, ins, ops, live):
        sz = ops[0].size
        if ops[0].type == X86_OP_REG and self.rn(ins, ops[0]) in ('es', 'ds', 'cs', 'ss', 'fs', 'gs'):
            return 'esp+=4;'
        if sz == 2:
            return '{u16 v_=M16(esp); esp+=2; %s}' % self.wr(ins, ops[0], 'v_')
        return '{u32 v_=M32(esp); esp+=4; %s}' % self.wr(ins, ops[0], 'v_')

    def i_pushal(self, ins, ops, live):
        return '{u32 s_=esp; esp-=32; M32(esp+28)=eax; M32(esp+24)=ecx; M32(esp+20)=edx; M32(esp+16)=ebx; M32(esp+12)=s_; M32(esp+8)=ebp; M32(esp+4)=esi; M32(esp)=edi;}'

    def i_popal(self, ins, ops, live):
        return 'edi=M32(esp); esi=M32(esp+4); ebp=M32(esp+8); ebx=M32(esp+16); edx=M32(esp+20); ecx=M32(esp+24); eax=M32(esp+28); esp+=32;'

    def i_pushfd(self, ins, ops, live):
        return '{u32 v_=0x202u|cf|(pf<<2)|(af<<4)|(zf<<6)|(sf<<7)|((u32)of<<11); esp-=4; M32(esp)=v_;}'

    def i_popfd(self, ins, ops, live):
        return '{u32 v_=M32(esp); esp+=4; cf=v_&1; pf=(v_>>2)&1; af=(v_>>4)&1; zf=(v_>>6)&1; sf=(v_>>7)&1; of=(v_>>11)&1;}'

    def i_lahf(self, ins, ops, live):
        return 'eax=(eax&0xFFFF00FFu)|((u32)(cf|2|(pf<<2)|(af<<4)|(zf<<6)|(sf<<7))<<8);'

    def i_sahf(self, ins, ops, live):
        return '{u8 v_=(u8)(eax>>8); cf=v_&1; pf=(v_>>2)&1; af=(v_>>4)&1; zf=(v_>>6)&1; sf=(v_>>7)&1;}'

    def i_daa(self, ins, ops, live):
        return '{u8 al_=(u8)eax; u8 oc_=cf; u8 old_=al_; cf=0; if ((al_&0xF)>9||af) { u16 t_=al_+6; al_=(u8)t_; cf=oc_||(t_>>8); af=1; } else af=0; if (old_>0x99||oc_) { al_+=0x60; cf=1; } eax=(eax&0xFFFFFF00u)|al_; zf=!al_; sf=al_>>7; pf=PAR(al_); }'

    def i_das(self, ins, ops, live):
        return '{u8 al_=(u8)eax; u8 oc_=cf; u8 old_=al_; cf=0; if ((al_&0xF)>9||af) { u16 t_=(u16)al_-6; al_=(u8)t_; cf=oc_||((t_>>8)&1); af=1; } else af=0; if (old_>0x99||oc_) { al_-=0x60; cf=1; } eax=(eax&0xFFFFFF00u)|al_; zf=!al_; sf=al_>>7; pf=PAR(al_); }'

    def i_clc(self, ins, ops, live): return 'cf=0;'
    def i_stc(self, ins, ops, live): return 'cf=1;'
    def i_cmc(self, ins, ops, live): return 'cf=!cf;'
    def i_cld(self, ins, ops, live): return ''
    def i_cli(self, ins, ops, live): return ''
    def i_sti(self, ins, ops, live): return ''
    def i_emms(self, ins, ops, live): return ''

    def trap(self, ins):
        return 'FLUSH(); LIFT_TRAP("%s", %s);' % (ins.mn, hx(objs.code_id(self.o.idx, ins.addr)))

    # ---- x87 (only the handful of forms used by the asm) ----
    def fpst(self, ins, op):
        n = ins.raw.reg_name(op.reg)      # st, st(1) ...
        return 0 if n == 'st' else int(n[3:-1])

    def i_fldz(self, ins, ops, live): return 'FPUSH(0.0);'
    def i_fld1(self, ins, ops, live): return 'FPUSH(1.0);'

    def i_fld(self, ins, ops, live):
        o = ops[0]
        if o.type == X86_OP_MEM:
            return 'FPUSH(%s);' % {4: 'LIFT_F32(%s)', 8: 'LIFT_F64(%s)'}[o.size] % self.ea(ins, o)
        return 'FPUSH(FST(%d));' % self.fpst(ins, o)

    def i_fst(self, ins, ops, live):
        o = ops[0]
        if o.type == X86_OP_MEM:
            return {4: 'LIFT_F32(%s)=(float)FST(0);', 8: 'LIFT_F64(%s)=FST(0);'}[o.size] % self.ea(ins, o)
        return 'FST(%d)=FST(0);' % self.fpst(ins, o)

    def i_fstp(self, ins, ops, live):
        return self.i_fst(ins, ops, live) + ' FPOP();'

    def i_fistp(self, ins, ops, live):
        o = ops[0]
        t = {2: 'LIFT_I16', 4: 'LIFT_I32', 8: 'LIFT_I64'}[o.size]
        return '%s(%s)=%s(FST(0)); FPOP();' % (t, self.ea(ins, o), {2: 'lift_f2i16', 4: 'lift_f2i32', 8: 'lift_f2i64'}[o.size])

    def i_fsub(self, ins, ops, live):
        if len(ops) == 2 and ops[0].type == X86_OP_REG:
            return 'FST(%d)-=FST(%d);' % (self.fpst(ins, ops[0]), self.fpst(ins, ops[1]))
        raise Unsupported('fsub form')

    def i_cpuid(self, ins, ops, live): return self.trap(ins)
    def i_in(self, ins, ops, live): return self.trap(ins)
    def i_out(self, ins, ops, live): return self.trap(ins)
    def i_int(self, ins, ops, live): return self.trap(ins)
    def i_iretd(self, ins, ops, live): return self.trap(ins)
    def i_lar(self, ins, ops, live): return self.trap(ins)
    def i_hlt(self, ins, ops, live): return self.trap(ins)

    # ---- control flow ----
    def i_jmp(self, ins, ops, live):
        if ins.tgt is not None:
            return self.dtarget(ins, False)
        return '{u32 t_=%s; pc=t_; goto dispatch;}' % self.rd(ins, ops[0], 32)

    def i_call(self, ins, ops, live):
        if ins.tgt is not None:
            return self.dtarget(ins, True)
        return '{u32 t_=%s; esp-=4; M32(esp)=%s; pc=t_; goto dispatch;}' % (self.rd(ins, ops[0], 32), self.retid(ins))

    def i_ret(self, ins, ops, live):
        extra = ' esp+=%d;' % (ops[0].imm & 0xFFFF) if ops else ''
        return 'pc=M32(esp); esp+=4;%s goto dispatch;' % extra

    def i_loop(self, ins, ops, live):
        return 'ecx--; if (ecx) {%s}' % self.dtarget(ins, False)

    def i_jecxz(self, ins, ops, live):
        return 'if (!ecx) {%s}' % self.dtarget(ins, False)

    # ---- ALU ----
    def alu(self, ins, ops, live, kind):
        b = ops[0].size * 8
        T = TY[b]
        a = self.rd(ins, ops[0]); s = self.rd(ins, ops[1], b)
        sb = b - 1
        head = '{%s a_=%s, b_=%s, r=' % (T, a, s)
        wr = '' if kind in ('cmp', 'test') else self.wr(ins, ops[0], 'r')
        z = self.zsp(b)
        if kind == 'add':
            return head + '(%s)(a_+b_); %s %s}' % (T, wr, self.fl(live, cf='r<a_', of='(((a_^r)&(b_^r))>>%d)&1' % sb, af='((a_^b_^r)>>4)&1', **z))
        if kind in ('sub', 'cmp'):
            return head + '(%s)(a_-b_); %s %s}' % (T, wr, self.fl(live, cf='a_<b_', of='(((a_^b_)&(a_^r))>>%d)&1' % sb, af='((a_^b_^r)>>4)&1', **z))
        if kind == 'adc':
            return '{%s a_=%s, b_=%s; u32 c_=cf; u64 t_=(u64)a_+b_+c_; %s r=(%s)t_; %s %s}' % (T, a, s, T, T, wr, self.fl(live, cf='(t_>>%d)&1' % b, of='(((a_^r)&(b_^r))>>%d)&1' % sb, af='((a_^b_^r)>>4)&1', **z))
        if kind == 'sbb':
            return '{%s a_=%s, b_=%s; u32 c_=cf; %s r=(%s)(a_-b_-c_); %s %s}' % (T, a, s, T, T, wr, self.fl(live, cf='((u64)a_<(u64)b_+c_)', of='(((a_^b_)&(a_^r))>>%d)&1' % sb, af='((a_^b_^r)>>4)&1', **z))
        op = {'and': '&', 'or': '|', 'xor': '^', 'test': '&'}[kind]
        return head + '(%s)(a_%sb_); %s %s}' % (T, op, wr, self.fl(live, cf='0', of='0', af='0', **z))

    def i_add(self, i, o, l): return self.alu(i, o, l, 'add')
    def i_sub(self, i, o, l): return self.alu(i, o, l, 'sub')
    def i_cmp(self, i, o, l): return self.alu(i, o, l, 'cmp')
    def i_adc(self, i, o, l): return self.alu(i, o, l, 'adc')
    def i_sbb(self, i, o, l): return self.alu(i, o, l, 'sbb')
    def i_and(self, i, o, l): return self.alu(i, o, l, 'and')
    def i_or(self, i, o, l): return self.alu(i, o, l, 'or')
    def i_xor(self, i, o, l): return self.alu(i, o, l, 'xor')
    def i_test(self, i, o, l): return self.alu(i, o, l, 'test')

    def i_inc(self, ins, ops, live):
        b = ops[0].size * 8; T = TY[b]
        return '{%s a_=%s, r=(%s)(a_+1); %s %s}' % (T, self.rd(ins, ops[0]), T, self.wr(ins, ops[0], 'r'),
                                                   self.fl(live, of='r==%s' % hx(1 << (b - 1)), af='((a_^r)>>4)&1', **self.zsp(b)))

    def i_dec(self, ins, ops, live):
        b = ops[0].size * 8; T = TY[b]
        return '{%s a_=%s, r=(%s)(a_-1); %s %s}' % (T, self.rd(ins, ops[0]), T, self.wr(ins, ops[0], 'r'),
                                                   self.fl(live, of='a_==%s' % hx(1 << (b - 1)), af='((a_^r)>>4)&1', **self.zsp(b)))

    def i_neg(self, ins, ops, live):
        b = ops[0].size * 8; T = TY[b]
        return '{%s a_=%s, r=(%s)(0-a_); %s %s}' % (T, self.rd(ins, ops[0]), T, self.wr(ins, ops[0], 'r'),
                                                   self.fl(live, cf='a_!=0', of='a_==%s' % hx(1 << (b - 1)), af='((a_^r)>>4)&1', **self.zsp(b)))

    def i_not(self, ins, ops, live):
        b = ops[0].size * 8; T = TY[b]
        return self.wr(ins, ops[0], '(%s)~%s' % (T, self.rd(ins, ops[0])))

    # ---- shifts / rotates ----
    def shiftcount(self, ins, ops):
        """returns (cexpr, static?) for shift count operand"""
        if len(ops) < 2:
            return '1', 1
        c = ops[1]
        if c.type == X86_OP_IMM: return str(c.imm & 31), c.imm & 31
        return '((u8)ecx&31)', None

    def shift(self, ins, ops, live, kind):
        b = ops[0].size * 8; T = TY[b]; S = STY[b]
        cexp, cst = self.shiftcount(ins, ops)
        if cst == 0: return ''
        a = self.rd(ins, ops[0])
        z = self.zsp(b)
        sb = b - 1
        if kind in ('shl', 'sal'):
            body = '{u64 t_=(u64)a_<<c_; %s r=(%s)t_; %s %s}' % (T, T, self.wr(ins, ops[0], 'r'), self.fl(live, cf='(t_>>%d)&1' % b, of='((r>>%d)^((t_>>%d)&1))&1' % (sb, b), af='0', **z))
        elif kind == 'shr':
            body = '{%s r=(%s)(a_>>c_); %s %s}' % (T, T, self.wr(ins, ops[0], 'r'), self.fl(live, cf='(a_>>(c_-1))&1', of='(a_>>%d)&1' % sb, af='0', **z))
        elif kind == 'sar':
            body = '{%s r=(%s)((%s)a_>>c_); %s %s}' % (T, T, S, self.wr(ins, ops[0], 'r'), self.fl(live, cf='(((%s)a_)>>(c_-1))&1' % S, of='0', af='0', **z))
        elif kind == 'rol':
            body = '{u32 k_=c_%%%d; %s r=k_?(%s)((a_<<k_)|(a_>>(%d-k_))):a_; %s %s}' % (b, T, T, b, self.wr(ins, ops[0], 'r'), self.fl(live, cf='r&1', of='((r>>%d)^r)&1' % sb))
        elif kind == 'ror':
            body = '{u32 k_=c_%%%d; %s r=k_?(%s)((a_>>k_)|(a_<<(%d-k_))):a_; %s %s}' % (b, T, T, b, self.wr(ins, ops[0], 'r'), self.fl(live, cf='(r>>%d)&1' % sb, of='((r>>%d)^(r>>%d))&1' % (sb, sb - 1)))
        elif kind == 'rcl':
            body = '{u32 k_=c_%%%d; u64 v_=((u64)cf<<%d)|a_; u64 w_=k_?(((v_<<k_)|(v_>>(%d-k_)))&%s):v_; %s r=(%s)w_; %s %s}' % (b + 1, b, b + 1, '0x%xull' % ((1 << (b + 1)) - 1), T, T, self.wr(ins, ops[0], 'r'), self.fl(live, cf='(w_>>%d)&1' % b, of='((r>>%d)^((w_>>%d)&1))&1' % (sb, b)))
        elif kind == 'rcr':
            body = '{u32 k_=c_%%%d; u64 v_=((u64)cf<<%d)|a_; u64 w_=k_?(((v_>>k_)|(v_<<(%d-k_)))&%s):v_; %s r=(%s)w_; %s %s}' % (b + 1, b, b + 1, '0x%xull' % ((1 << (b + 1)) - 1), T, T, self.wr(ins, ops[0], 'r'), self.fl(live, cf='(w_>>%d)&1' % b, of='((r>>%d)^(r>>%d))&1' % (sb, sb - 1)))
        else:
            raise Unsupported(kind)
        if cst is None:
            return '{%s a_=%s; u32 c_=%s; if (c_) %s}' % (T, a, cexp, body)
        return '{%s a_=%s; const u32 c_=%s; %s}' % (T, a, cexp, body)

    def i_shl(self, i, o, l): return self.shift(i, o, l, 'shl')
    def i_sal(self, i, o, l): return self.shift(i, o, l, 'shl')
    def i_shr(self, i, o, l): return self.shift(i, o, l, 'shr')
    def i_sar(self, i, o, l): return self.shift(i, o, l, 'sar')
    def i_rol(self, i, o, l): return self.shift(i, o, l, 'rol')
    def i_ror(self, i, o, l): return self.shift(i, o, l, 'ror')
    def i_rcl(self, i, o, l): return self.shift(i, o, l, 'rcl')
    def i_rcr(self, i, o, l): return self.shift(i, o, l, 'rcr')

    def dshift(self, ins, ops, live, left):
        b = ops[0].size * 8
        if b != 32: raise Unsupported('shld16')
        c = ops[2]
        if c.type == X86_OP_IMM:
            cst = c.imm & 31
            if cst == 0: return ''
            cexp = str(cst)
        else:
            cst = None; cexp = '((u8)ecx&31)'
        a = self.rd(ins, ops[0]); s = self.rd(ins, ops[1])
        z = self.zsp(32)
        if left:
            body = '{u32 r=(a_<<c_)|(b_>>(32-c_)); %s %s}' % (self.wr(ins, ops[0], 'r'), self.fl(live, cf='(a_>>(32-c_))&1', of='((r>>31)^(a_>>31))&1', af='0', **z))
        else:
            body = '{u32 r=(a_>>c_)|(b_<<(32-c_)); %s %s}' % (self.wr(ins, ops[0], 'r'), self.fl(live, cf='(a_>>(c_-1))&1', of='((r>>31)^(a_>>31))&1', af='0', **z))
        if cst is None:
            return '{u32 a_=%s, b_=%s, c_=%s; if (c_) %s}' % (a, s, cexp, body)
        return '{u32 a_=%s, b_=%s; const u32 c_=%s; %s}' % (a, s, cexp, body)

    def i_shld(self, i, o, l): return self.dshift(i, o, l, True)
    def i_shrd(self, i, o, l): return self.dshift(i, o, l, False)

    # ---- mul / div ----
    def i_mul(self, ins, ops, live):
        b = ops[0].size * 8
        s = self.rd(ins, ops[0])
        if b == 8: return '{u16 t_=(u16)((u8)eax*(u8)%s); eax=(eax&0xFFFF0000u)|t_; %s}' % (s, self.fl(live, cf='(t_>>8)!=0', of='(t_>>8)!=0'))
        if b == 16: return '{u32 t_=(u32)(u16)eax*(u16)%s; eax=(eax&0xFFFF0000u)|(u16)t_; edx=(edx&0xFFFF0000u)|(t_>>16); %s}' % (s, self.fl(live, cf='(t_>>16)!=0', of='(t_>>16)!=0'))
        return '{u64 t_=(u64)eax*(u32)%s; eax=(u32)t_; edx=(u32)(t_>>32); %s}' % (s, self.fl(live, cf='edx!=0', of='edx!=0'))

    def i_imul(self, ins, ops, live):
        b = ops[0].size * 8
        if len(ops) == 1:
            s = self.rd(ins, ops[0])
            if b == 8: return '{s16 t_=(s16)((s8)eax*(s8)%s); eax=(eax&0xFFFF0000u)|(u16)t_; %s}' % (s, self.fl(live, cf='t_!=(s8)t_', of='t_!=(s8)t_'))
            if b == 16: return '{s32 t_=(s32)(s16)eax*(s16)%s; eax=(eax&0xFFFF0000u)|(u16)t_; edx=(edx&0xFFFF0000u)|(u16)((u32)t_>>16); %s}' % (s, self.fl(live, cf='t_!=(s16)t_', of='t_!=(s16)t_'))
            return '{s64 t_=(s64)(s32)eax*(s32)%s; eax=(u32)t_; edx=(u32)((u64)t_>>32); %s}' % (s, self.fl(live, cf='t_!=(s32)t_', of='t_!=(s32)t_'))
        if len(ops) == 2: a, s = ops[0], ops[1]
        else: a, s = ops[1], ops[2]
        if b == 16:
            x = '(s32)(s16)%s' % self.rd(ins, a); y = '(s32)(s16)%s' % self.rd(ins, s, 16)
            return '{s32 t_=(%s)*(%s); %s %s}' % (x, y, self.wr(ins, ops[0], '(u16)t_'), self.fl(live, cf='t_!=(s16)t_', of='t_!=(s16)t_'))
        x = '(s64)(s32)%s' % self.rd(ins, a); y = '(s64)(s32)%s' % self.rd(ins, s, 32)
        return '{s64 t_=(%s)*(%s); %s %s}' % (x, y, self.wr(ins, ops[0], '(u32)t_'), self.fl(live, cf='t_!=(s32)t_', of='t_!=(s32)t_'))

    def i_div(self, ins, ops, live):
        b = ops[0].size * 8
        s = self.rd(ins, ops[0])
        if b == 8: return '{u16 n_=(u16)eax; u8 d_=%s; LIFT_DIVCHK(d_); eax=(eax&0xFFFF0000u)|((u16)(n_%%d_)<<8)|(u8)(n_/d_);}' % s
        if b == 16: return '{u32 n_=((edx&0xFFFFu)<<16)|(u16)eax; u16 d_=%s; LIFT_DIVCHK(d_); eax=(eax&0xFFFF0000u)|(u16)(n_/d_); edx=(edx&0xFFFF0000u)|(u16)(n_%%d_);}' % s
        return '{u64 n_=((u64)edx<<32)|eax; u32 d_=%s; LIFT_DIVCHK(d_); eax=(u32)(n_/d_); edx=(u32)(n_%%d_);}' % s

    def i_idiv(self, ins, ops, live):
        b = ops[0].size * 8
        s = self.rd(ins, ops[0])
        if b == 8: return '{s16 n_=(s16)eax; s8 d_=(s8)%s; LIFT_DIVCHK(d_); eax=(eax&0xFFFF0000u)|((u16)(u8)(n_%%d_)<<8)|(u8)(n_/d_);}' % s
        if b == 16: return '{s32 n_=(s32)(((edx&0xFFFFu)<<16)|(u16)eax); s16 d_=(s16)%s; LIFT_DIVCHK(d_); eax=(eax&0xFFFF0000u)|(u16)(n_/d_); edx=(edx&0xFFFF0000u)|(u16)(n_%%d_);}' % s
        return '{s64 n_=(s64)(((u64)edx<<32)|eax); s32 d_=(s32)%s; LIFT_DIVCHK(d_); eax=(u32)(n_/d_); edx=(u32)(n_%%d_);}' % s

    # ---- string ops ----
    def strop(self, ins, body, step):
        s = '{ %s }' % body
        if ins.rep:
            return 'while (ecx) { %s ecx--; }' % body
        return s

    def i_stosb(self, ins, ops, live): return self.strop(ins, 'M8(edi)=(u8)eax; edi+=1;', 1)
    def i_stosw(self, ins, ops, live): return self.strop(ins, 'M16(edi)=(u16)eax; edi+=2;', 2)
    def i_stosd(self, ins, ops, live): return self.strop(ins, 'M32(edi)=eax; edi+=4;', 4)
    def i_lodsb(self, ins, ops, live): return self.strop(ins, 'eax=(eax&0xFFFFFF00u)|M8(esi); esi+=1;', 1)
    def i_lodsw(self, ins, ops, live): return self.strop(ins, 'eax=(eax&0xFFFF0000u)|M16(esi); esi+=2;', 2)
    def i_lodsd(self, ins, ops, live): return self.strop(ins, 'eax=M32(esi); esi+=4;', 4)
    def i_movsb(self, ins, ops, live): return self.strop(ins, 'M8(edi)=M8(esi); esi+=1; edi+=1;', 1)
    def i_movsw(self, ins, ops, live): return self.strop(ins, 'M16(edi)=M16(esi); esi+=2; edi+=2;', 2)
    def i_movsd(self, ins, ops, live):
        if ops and ops[0].type != 0 and ops[0].type == X86_OP_REG and self.rn(ins, ops[0]).startswith('xmm'): raise Unsupported('movsd sse')
        return self.strop(ins, 'M32(edi)=M32(esi); esi+=4; edi+=4;', 4)

    # ---- MMX moves ----
    def i_movd(self, ins, ops, live):
        d, s = ops
        if d.type == X86_OP_REG and self.rn(ins, d).startswith('mm'):
            src = self.rd(ins, s, 32)
            return 'mm[%d].q=(u64)(u32)%s;' % (self.mmi(ins, d), src)
        # to r/m32
        return self.wr(ins, d, 'mm[%d].d[0]' % self.mmi(ins, s))

    def i_movq(self, ins, ops, live):
        d, s = ops
        if d.type == X86_OP_REG and self.rn(ins, d).startswith('mm'):
            return 'mm[%d]=%s;' % (self.mmi(ins, d), self.mmsrc(ins, s))
        return 'M64(%s)=mm[%d].q;' % (self.ea(ins, d), self.mmi(ins, s))
