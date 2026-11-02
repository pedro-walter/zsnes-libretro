/*
 * SPC700 opcode handlers, ported from cpu/spc700.asm (ZSNES 1.42).
 *
 * This is a faithful translation of the asm handlers, quirks included (the
 * normalised spcNZ flag byte, the half-carry/overflow behaviour taken from the
 * x86 ALU flags, TCLR1 using the direct page, the SPC idle-loop "halt" hack,
 * ...). The CPU/SPC interleaving in cpu/execute.asm is unchanged: it still does
 *     call [opcjmptab + opcode*4]      with ebp = SPC pc (host pointer into spcRam)
 * and the table entries now point at the stubs below, which call zs_spc_exec().
 * The SPC state stays in the same asm variables (spcA, spcP, spcNZ, ...), so
 * everything that still lives in asm (timers, DSP, save states) keeps working.
 *
 * Memory addresses are "offsets from spcRam" (the asm used host pointers).
 */
#include <stdint.h>
#include <string.h>
#include <stdlib.h>

typedef uint8_t u8;
typedef int8_t s8;
typedef uint16_t u16;
typedef uint32_t u32;

#define HID __attribute__((visibility("hidden")))

/* ---- state shared with the asm ---- */
extern u8 spcRam[0x10000];
extern u8 spcA, spcX, spcY, spcP, spcNZ;
extern u32 spcS;
extern u8 *spcRamDP;
extern u8 reg1read; /* reg1read..reg4read are consecutive */
extern u8 timeron, timincr0, timinl0;
extern u8 spcextraram[64];
extern u8 SPCROM[64];
extern u8 DSPMem[256];
extern u32 spcnumread, spchalted, cycpbl, spc700read, curexecstate;
extern u8 SPC700sh, disablespcclr, SPCSkipXtraROM;
extern u32 tableadb[256];
extern u32 opcjmptab[256];
extern void *dspWptr[128];

#define TIMINCR(i) (&timincr0)[i]
#define TIMINL(i) (&timinl0)[i]
#define REGREAD(i) (&reg1read)[i]

/* set when haltspc() must reload the CPU's edi (exec table pointer) */
u32 zs_spc_edi_dirty, zs_spc_edi_val;
static u32 g_edx; /* cpu edx on entry (dl = 65816 P) */
static u32 dsp_res;

/* ---- asm helper: call dspWptr[reg] with al=val, return al afterwards ---- */
extern u32 zs_dspw_call(u32 reg, u32 val);
__asm__(".text\n.globl zs_dspw_call\n.hidden zs_dspw_call\n.type zs_dspw_call,@function\nzs_dspw_call:\n"
        "  pushal\n  movl 36(%esp),%ebx\n  movl 40(%esp),%eax\n  cld\n"
        "  call *dspWptr(,%ebx,4)\n"
        "  movl %eax,zs_dsp_res_tmp\n  popal\n  movl zs_dsp_res_tmp,%eax\n  ret\n"
        ".data\n.hidden zs_dsp_res_tmp\nzs_dsp_res_tmp: .long 0\n.text\n");

/* ---- SPC register space ($F0-$FF) ---- */
static void haltspc(void)
{
    if (SPC700sh != 1) {
        cycpbl = 0xFFFFFFFFu;
        if (curexecstate & 2) {
            curexecstate &= ~2u; /* and byte[curexecstate],0FDh */
            spchalted = spcnumread;
            zs_spc_edi_val = tableadb[g_edx & 0xFF];
            zs_spc_edi_dirty = 1;
        }
    }
    spcnumread = 0;
}

static void skipmacro(int i)
{
    if (!(timeron & (1 << i))) return;
    if (TIMINCR(i) == 0) return;
    ((u8 *)&spcnumread)[i]++;
    if (((u8 *)&spcnumread)[i] == 8) haltspc();
}

static u8 rd_reg(unsigned r)
{
    u8 al;
    switch (r) {
    case 0: case 1: case 2: case 3: return spcRam[0xF0 + r];
    case 4: case 5: case 6: case 7: return spcRam[0xF0 + r];
    case 8: case 9: return 0;
    case 10: case 11: case 12: return spcRam[0xF0 + r];
    default: { /* FD, FE, FF: timer counters */
        int i = r - 13;
        al = spcRam[0xF0 + r] & 0x0F;
        if (spcRam[0xF0 + r] == 0) { skipmacro(i); return al; }
        spcRam[0xF0 + r] = 0;
        ((u8 *)&spcnumread)[i] = 0;
        return al;
    }
    }
}

static void wr_reg(unsigned r, u8 al)
{
    int i;
    switch (r) {
    case 0: spcRam[0xF0] = al; break;
    case 1:
        if (disablespcclr != 1) {
            if (al & 0x10) { spcRam[0xF4] = 0; spcRam[0xF5] = 0; }
            if (al & 0x20) { spcRam[0xF6] = 0; spcRam[0xF7] = 0; }
        }
        if (SPCSkipXtraROM != 1) {
            if (al & 0x80) memcpy(&spcRam[0xFFC0], SPCROM, 0x40);
            else memcpy(&spcRam[0xFFC0], spcextraram, 0x40);
        }
        spcRam[0xF1] = al;
        timeron = al & 0x0F;
        break;
    case 2:
        spcRam[0xF2] = al;
        spcRam[0xF3] = DSPMem[spcRam[0xF2]];
        break;
    case 3:
        al = (u8)zs_dspw_call(spcRam[0xF2] & 0x7F, al);
        spcRam[0xF3] = al;
        break;
    case 4: case 5: case 6: case 7:
        REGREAD(r - 4) = al;
        spc700read++;
        break;
    case 8: case 9: spcRam[0xF0 + r] = al; break;
    case 10: case 11: case 12:
        i = r - 10;
        TIMINCR(i) = al;
        if (TIMINL(i) == 0) TIMINL(i) = al;
        spcRam[0xF0 + r] = al;
        break;
    default: break; /* FD-FF */
    }
}

static inline u8 rd(u32 off)
{
    if (off >= 0xF0 && off <= 0xFF) return rd_reg(off - 0xF0);
    return spcRam[off];
}

static inline void wr(u32 off, u8 v)
{
    if (off > 0xFF) {
        if (off >= 0xFFC0) {
            spcextraram[off - 0xFFC0] = v;
            if (spcRam[0xF1] & 0x80) return;
        }
        spcRam[off] = v;
    } else if (off >= 0xF0) {
        wr_reg(off - 0xF0, v);
    } else {
        spcRam[off] = v;
    }
}

static inline u16 rd16p(const u8 *p) { u16 v; memcpy(&v, p, 2); return v; }
static inline u32 dpb(void) { return (u32)(spcRamDP - spcRam); }

static inline void push8(u8 v) { spcRam[spcS] = v; ((u8 *)&spcS)[0]--; }
static inline u8 pop8(void) { ((u8 *)&spcS)[0]++; return spcRam[spcS]; }

/* ---- flag helpers (mirror the SPCSetFlag* macros) ---- */
static inline void set_nz_norm(u32 res, u32 sign, u32 mask)
{
    spcNZ = (res & sign) ? 0x80 : ((res & mask) == 0 ? 0 : 1);
}
static inline void set_c(int c) { if (c) spcP |= 1; else spcP &= 0xFE; }

/* SPCSetFlagnzc: N/Z from res, C */
static inline void fl_nzc8(u32 res, int c) { set_nz_norm(res, 0x80, 0xFF); set_c(c); }
static inline void fl_nzc16(u32 res, int c) { set_nz_norm(res, 0x8000, 0xFFFF); set_c(c); }

/* SPCSetFlagnvhzc: N/Z from res, V from x86 OF, C, H from x86 AF */
static inline void fl_nvhzc(u32 res, u32 sign, u32 mask, int c, int o, int h)
{
    set_nz_norm(res, sign, mask);
    if (o) spcP |= 0x40; else spcP &= 0xBF;
    spcP &= 0xF6;
    if (c) spcP |= 1;
    if (h) spcP |= 8;
}

static inline u8 do_adc8(u8 a, u8 b)
{
    u32 cin = spcP & 1, r = (u32)a + b + cin;
    u8 res = (u8)r;
    fl_nvhzc(res, 0x80, 0xFF, r > 0xFF, (~(a ^ b) & (a ^ res) & 0x80) != 0, ((a ^ b ^ res) & 0x10) != 0);
    return res;
}
static inline u8 do_sbc8(u8 a, u8 b)
{
    u32 bin = (spcP & 1) ^ 1;
    int borrow = (u32)a < (u32)b + bin;
    u8 res = (u8)(a - b - bin);
    fl_nvhzc(res, 0x80, 0xFF, !borrow, (((a ^ b) & (a ^ res)) & 0x80) != 0, ((a ^ b ^ res) & 0x10) != 0);
    return res;
}
static inline void do_cmp8(u8 a, u8 b)
{
    fl_nzc8((u8)(a - b), a >= b);
}

/* operand effective addresses (offsets from spcRam); advance pc */
#define EA_DP()      (dpb() + *pc++)
#define EA_DPX()     (dpb() + (u8)(*pc++ + spcX))
#define EA_DPY()     (dpb() + (u8)(*pc++ + spcY))
#define EA_ABS()     ({ u32 e_ = rd16p(pc); pc += 2; e_; })
#define EA_ABSX()    ({ u32 e_ = (u16)(spcX + rd16p(pc)); pc += 2; e_; })
#define EA_ABSY()    ({ u32 e_ = (u16)(spcY + rd16p(pc)); pc += 2; e_; })
#define EA_X()       (dpb() + spcX)
#define EA_DPXI()    ({ u32 p_ = dpb() + (u8)(*pc++ + spcX); (u32)rd16p(spcRam + p_); })
#define EA_DPIY()    ({ u32 p_ = dpb() + *pc++; (u32)(u16)(rd16p(spcRam + p_) + spcY); })

#define SETNZ(v) (spcNZ = (v))
#define BRANCH(cond) do { if (cond) { s8 r_ = (s8)*pc; pc++; pc += r_; } else pc++; return pc; } while (0)

/* mode index for the 8 regular ALU addressing modes, from the opcode */
static inline u32 alu_ea(u8 **pcp, unsigned op)
{
    u8 *pc = *pcp;
    u32 ea = 0;
    switch (((op & 0x0F) - 4) * 2 + ((op >> 4) & 1)) {
    case 0: ea = EA_DP(); break;
    case 1: ea = EA_DPX(); break;
    case 2: ea = EA_ABS(); break;
    case 3: ea = EA_ABSX(); break;
    case 4: ea = EA_X(); break;
    case 5: ea = EA_ABSY(); break;
    case 6: ea = EA_DPXI(); break;
    case 7: ea = EA_DPIY(); break;
    }
    *pcp = pc;
    return ea;
}

/* ALU op on A, 0=OR 1=AND 2=EOR 3=CMP 4=ADC 5=SBC 7=MOV */
static inline void alu_a(unsigned which, u8 v)
{
    switch (which) {
    case 0: spcA |= v; SETNZ(spcA); break;
    case 1: spcA &= v; SETNZ(spcA); break;
    case 2: spcA ^= v; SETNZ(spcA); break;
    case 3: do_cmp8(spcA, v); break;
    case 4: spcA = do_adc8(spcA, v); break;
    case 5: spcA = do_sbc8(spcA, v); break;
    case 7: spcA = v; SETNZ(v); break;
    }
}

/* read-modify-write ALU op for dp,dp / dp,#imm / (X),(Y): returns 1 if result is to be stored */
static inline int alu_rmw(unsigned which, u8 *d, u8 s)
{
    u8 a = *d;
    switch (which) {
    case 0: a |= s; SETNZ(a); break;
    case 1: a &= s; SETNZ(a); break;
    case 2: a ^= s; SETNZ(a); break;
    case 3: do_cmp8(a, s); return 0;
    case 4: a = do_adc8(a, s); break;
    case 5: a = do_sbc8(a, s); break;
    }
    *d = a;
    return 1;
}

static inline u8 spc_daa(u8 al)
{
    int cf = spcP & 1, af = (spcP & 8) != 0, ncf = 0;
    u8 old = al;
    ncf = cf;
    if ((al & 0x0F) > 9 || af) { u32 t = al + 6; al = (u8)t; ncf = cf || (t > 0xFF); }
    if (old > 0x99 || cf) { al += 0x60; ncf = 1; } /* x86: CF keeps the first clause's value otherwise */
    fl_nzc8(al, ncf);
    return al;
}
static inline u8 spc_das(u8 al)
{
    int cf = spcP & 1, af = (spcP & 8) != 0, ncf = 0;
    u8 old = al;
    ncf = cf;
    if ((al & 0x0F) > 9 || af) { ncf = cf || (al < 6); al = (u8)(al - 6); }
    if (old > 0x99 || cf) { al -= 0x60; ncf = 1; }
    fl_nzc8(al, ncf);
    return al;
}

static inline void set_dp_from_p(void) { spcRamDP = spcRam + ((spcP & 32) ? 0x100 : 0); }

__attribute__((force_align_arg_pointer)) HID
u8 *zs_spc_exec(u8 *pc, unsigned op, u32 edx)
{
    u32 ea, ea2;
    u8 al, ah, v, s;
    u32 w;
    g_edx = edx;

    switch (op) {
    /* ---- misc ---- */
    case 0x00: return pc;
    case 0xEF: return pc - 1;
    case 0xFF: spc700read++; return pc - 1;
    case 0x0F: spc700read++; return pc - 1;
    case 0x7F: return pc - 1;
    case 0x9F: spcA = (u8)((spcA >> 4) | (spcA << 4)); SETNZ(spcA); return pc;

    /* ---- branches ---- */
    case 0x10: BRANCH(!(spcNZ & 128));
    case 0x30: BRANCH(spcNZ & 128);
    case 0x50: BRANCH(!(spcP & 64));
    case 0x70: BRANCH(spcP & 64);
    case 0x90: BRANCH(!(spcP & 1));
    case 0xB0: BRANCH(spcP & 1);
    case 0xD0: BRANCH(spcNZ != 0);
    case 0xF0: BRANCH(spcNZ == 0);
    case 0x2F: { s8 r = (s8)*pc; pc++; return pc + r; }

    /* ---- flags ---- */
    case 0x20: spcP &= ~0x20; spcRamDP = spcRam; return pc;
    case 0x40: spcP |= 0x20; spcP &= ~0x04; spcRamDP = spcRam + 0x100; return pc;
    case 0x60: spcP &= ~1; return pc;
    case 0x80: spcP |= 1; return pc;
    case 0xA0: spcP |= 4; return pc;
    case 0xC0: spcP &= ~4; return pc;
    case 0xE0: spcP &= 0xB7; return pc;
    case 0xED: spcP ^= 1; return pc;

    /* ---- TCALL ---- */
    case 0x01: case 0x11: case 0x21: case 0x31: case 0x41: case 0x51: case 0x61: case 0x71:
    case 0x81: case 0x91: case 0xA1: case 0xB1: case 0xC1: case 0xD1: case 0xE1: case 0xF1: {
        unsigned n = 30 - 2 * (op >> 4);
        u32 ret = (u32)(pc - spcRam);
        push8((u8)(ret >> 8));
        push8((u8)ret);
        if (spcRam[0xF1] & 0x80) w = rd16p(&spcextraram[n]);
        else w = rd16p(&spcRam[0xFFC0 + n]);
        return spcRam + w;
    }

    /* ---- SET1/CLR1/BBS/BBC ---- */
    case 0x02: case 0x22: case 0x42: case 0x62: case 0x82: case 0xA2: case 0xC2: case 0xE2:
        ea = dpb() + *pc++; wr(ea, rd(ea) | (1u << (op >> 5))); return pc;
    case 0x12: case 0x32: case 0x52: case 0x72: case 0x92: case 0xB2: case 0xD2: case 0xF2:
        ea = dpb() + *pc++; wr(ea, rd(ea) & ~(1u << (op >> 5))); return pc;
    case 0x03: case 0x23: case 0x43: case 0x63: case 0x83: case 0xA3: case 0xC3: case 0xE3:
    case 0x13: case 0x33: case 0x53: case 0x73: case 0x93: case 0xB3: case 0xD3: case 0xF3: {
        int bbc = op & 0x10;
        ea = dpb() + *pc;
        al = rd(ea);
        int set = (al & (1u << (op >> 5))) != 0;
        if (set != (bbc != 0)) { pc += (s8)pc[1] + 2; } else pc += 2;
        return pc;
    }

    /* ---- ALU A,<mode> : OR AND EOR CMP ADC SBC MOV ---- */
    case 0x04: case 0x14: case 0x05: case 0x15: case 0x06: case 0x16: case 0x07: case 0x17:
    case 0x24: case 0x34: case 0x25: case 0x35: case 0x26: case 0x36: case 0x27: case 0x37:
    case 0x44: case 0x54: case 0x45: case 0x55: case 0x46: case 0x56: case 0x47: case 0x57:
    case 0x64: case 0x74: case 0x65: case 0x75: case 0x66: case 0x76: case 0x67: case 0x77:
    case 0x84: case 0x94: case 0x85: case 0x95: case 0x86: case 0x96: case 0x87: case 0x97:
    case 0xA4: case 0xB4: case 0xA5: case 0xB5: case 0xA6: case 0xB6: case 0xA7: case 0xB7:
    case 0xE4: case 0xF4: case 0xE5: case 0xF5: case 0xE6: case 0xF6: case 0xE7: case 0xF7:
        ea = alu_ea(&pc, op);
        alu_a(op >> 5, rd(ea));
        return pc;
    case 0x08: case 0x28: case 0x48: case 0x68: case 0x88: case 0xA8: case 0xE8:
        v = *pc++;
        alu_a(op >> 5, v);
        return pc;

    /* ---- dp,#imm : SBC ADC CMP EOR AND OR ---- */
    case 0xB8: case 0x98: case 0x78: case 0x58: case 0x38: case 0x18: {
        static const u8 which[8] = { 0, 1, 2, 3, 4, 5, 0, 0 };
        unsigned wsel = which[op >> 5];
        ea = dpb() + pc[1];
        s = pc[0];
        v = (wsel == 3) ? rd(ea) : rd(ea);
        pc += 2;
        if (alu_rmw(wsel, &v, s)) wr(ea, v);
        return pc;
    }

    /* ---- dp,dp ---- */
    case 0x09: case 0x29: case 0x49: case 0x69: case 0x89: case 0xA9: {
        ea = dpb() + pc[1];
        ea2 = dpb() + pc[0];
        pc += 2;
        v = rd(ea);
        s = rd(ea2);
        if (alu_rmw(op >> 5, &v, s)) wr(ea, v);
        return pc;
    }
    case 0xFA:
        ea = dpb() + pc[1];
        ea2 = dpb() + pc[0];
        pc += 2;
        wr(ea, rd(ea2));
        return pc;

    /* ---- (X),(Y) ---- */
    case 0x19: case 0x39: case 0x59: case 0x79: case 0x99: case 0xB9: {
        s = rd(dpb() + spcY);
        ea = dpb() + spcX;
        v = rd(ea);
        if (alu_rmw(op >> 5, &v, s)) wr(ea, v);
        return pc;
    }

    /* ---- MOV mem,A ---- */
    case 0xC4: case 0xD4: case 0xC5: case 0xD5: case 0xC6: case 0xD6: case 0xC7: case 0xD7:
        ea = alu_ea(&pc, op);
        wr(ea, spcA);
        return pc;

    /* ---- MOV X/Y ---- */
    case 0xD8: ea = EA_DP(); wr(ea, spcX); return pc;
    case 0xF8: ea = EA_DP(); spcX = rd(ea); SETNZ(spcX); return pc;
    case 0xC9: ea = EA_ABS(); wr(ea, spcX); return pc;
    case 0xE9: ea = EA_ABS(); spcX = rd(ea); SETNZ(spcX); return pc;
    case 0xD9: ea = EA_DPY(); wr(ea, spcX); return pc;
    case 0xF9: ea = EA_DPY(); spcX = rd(ea); SETNZ(spcX); return pc;
    case 0xCB: ea = EA_DP(); wr(ea, spcY); return pc;
    case 0xEB: ea = EA_DP(); spcY = rd(ea); SETNZ(spcY); return pc;
    case 0xDB: ea = EA_DPX(); wr(ea, spcY); return pc;
    case 0xFB: ea = EA_DPX(); spcY = rd(ea); SETNZ(spcY); return pc;
    case 0xCC: ea = EA_ABS(); wr(ea, spcY); return pc;
    case 0xEC: ea = EA_ABS(); spcY = rd(ea); SETNZ(spcY); return pc;
    case 0x5D: spcX = spcA; SETNZ(spcA); return pc;
    case 0x7D: spcA = spcX; SETNZ(spcA); return pc;
    case 0x8D: spcY = *pc++; SETNZ(spcY); return pc;
    case 0xCD: spcX = *pc++; SETNZ(spcX); return pc;
    case 0x8F: ea = dpb() + pc[1]; al = pc[0]; pc += 2; wr(ea, al); return pc;
    case 0x9D: spcX = (u8)spcS; SETNZ(spcX); return pc;
    case 0xBD: ((u8 *)&spcS)[0] = spcX; return pc;
    case 0xDD: spcA = spcY; SETNZ(spcA); return pc;
    case 0xFD: spcY = spcA; SETNZ(spcY); return pc;
    case 0xAF: ea = EA_X(); spcX++; wr(ea, spcA); return pc;
    case 0xBF: ea = EA_X(); al = rd(ea); spcX++; spcA = al; SETNZ(al); return pc;

    /* ---- CMP X/Y ---- */
    case 0xC8: v = *pc++; do_cmp8(spcX, v); return pc;
    case 0xAD: v = *pc++; do_cmp8(spcY, v); return pc;
    case 0x1E: ea = EA_ABS(); do_cmp8(spcX, rd(ea)); return pc;
    case 0x3E: ea = EA_DP(); do_cmp8(spcX, rd(ea)); return pc;
    case 0x5E: ea = EA_ABS(); do_cmp8(spcY, rd(ea)); return pc;
    case 0x7E: ea = EA_DP(); do_cmp8(spcY, rd(ea)); return pc;

    /* ---- 16-bit ---- */
    case 0x1A: case 0x3A: {
        ea = dpb() + *pc++;
        ah = rd(ea + 1);
        al = rd(ea);
        w = (u16)(((u32)ah << 8 | al) + ((op == 0x3A) ? 1 : -1));
        set_nz_norm(w, 0x8000, 0xFFFF);
        wr(ea, (u8)w);
        wr(ea + 1, (u8)(w >> 8));
        return pc;
    }
    case 0x5A: case 0x7A: case 0x9A: case 0xBA: {
        u32 mem, ya = (u32)spcA | ((u32)spcY << 8);
        ea = dpb() + *pc++;
        ah = rd(ea + 1);
        al = rd(ea);
        mem = (u32)ah << 8 | al;
        if (op == 0x5A) {
            fl_nzc16((u16)(ya - mem), ya >= mem);
        } else if (op == 0x7A) {
            u32 r = ya + mem;
            u16 res = (u16)r;
            spcA = (u8)res; spcY = (u8)(res >> 8);
            fl_nvhzc(res, 0x8000, 0xFFFF, r > 0xFFFF, (~(ya ^ mem) & (ya ^ res) & 0x8000) != 0, ((ya ^ mem ^ res) & 0x10) != 0);
        } else if (op == 0x9A) {
            int borrow = ya < mem;
            u16 res = (u16)(ya - mem);
            spcA = (u8)res; spcY = (u8)(res >> 8);
            fl_nvhzc(res, 0x8000, 0xFFFF, !borrow, (((ya ^ mem) & (ya ^ res)) & 0x8000) != 0, ((ya ^ mem ^ res) & 0x10) != 0);
        } else {
            spcA = al; spcY = ah;
            set_nz_norm(mem, 0x8000, 0xFFFF);
            /* the asm leaves 1 for any non-zero non-negative value; same as set_nz_norm */
        }
        return pc;
    }
    case 0xDA:
        ea = dpb() + *pc++;
        wr(ea, spcA);
        wr(ea + 1, spcY);
        return pc;

    /* ---- bit ops on mem.bit ---- */
    case 0x0A: case 0x2A: case 0x4A: case 0x6A: case 0x8A: case 0xAA: {
        unsigned cl = rd16p(pc) >> 13;
        ea = rd16p(pc) & 0x1FFF;
        pc += 2;
        al = (u8)((rd(ea) >> cl) & 1);
        switch (op) {
        case 0x0A: spcP |= al; break;
        case 0x2A: spcP |= al ^ 1; break;
        case 0x4A: spcP &= (u8)(al | 0xFE); break;
        case 0x6A: spcP &= (u8)((al | 0xFE) ^ 1); break;
        case 0x8A: spcP ^= al; break;
        case 0xAA: spcP &= 0xFE; spcP |= al; break;
        }
        return pc;
    }
    case 0xCA: {
        unsigned cl = rd16p(pc) >> 13;
        ea = rd16p(pc) & 0x1FFF;
        pc += 2;
        al = rd(ea);
        al = (u8)((al & ~(1u << cl)) | ((spcP & 1) << cl));
        wr(ea, al);
        return pc;
    }
    case 0xEA: {
        unsigned cl = rd16p(pc) >> 13;
        ea = rd16p(pc) & 0x1FFF;
        pc += 2;
        wr(ea, rd(ea) ^ (1u << cl));
        return pc;
    }

    /* ---- shifts/rotates on memory ---- */
    case 0x0B: case 0x4B: case 0x1B: case 0x5B: case 0x0C: case 0x4C:
    case 0x2B: case 0x6B: case 0x3B: case 0x7B: case 0x2C: case 0x6C: {
        switch (op) {
        case 0x0B: case 0x4B: case 0x2B: case 0x6B: ea = EA_DP(); break;
        case 0x1B: case 0x5B: case 0x3B: case 0x7B: ea = EA_DPX(); break;
        default: ea = EA_ABS(); break;
        }
        al = rd(ea);
        switch (op) {
        case 0x0B: case 0x1B: case 0x0C: /* ASL */
            fl_nzc8((u8)(al << 1), al >> 7); al <<= 1; break;
        case 0x4B: case 0x5B: case 0x4C: /* LSR */
            fl_nzc8(al >> 1, al & 1); al >>= 1; break;
        case 0x2B: case 0x3B: case 0x2C: { /* ROL */
            u8 r = (u8)((al << 1) | (spcP & 1));
            set_c(al >> 7); SETNZ(r); al = r; break; }
        default: { /* ROR */
            u8 r = (u8)((al >> 1) | ((spcP & 1) << 7));
            set_c(al & 1); SETNZ(r); al = r; break; }
        }
        wr(ea, al);
        return pc;
    }
    case 0x1C: fl_nzc8((u8)(spcA << 1), spcA >> 7); spcA <<= 1; return pc;
    case 0x5C: fl_nzc8(spcA >> 1, spcA & 1); spcA >>= 1; return pc;
    case 0x3C: { u8 r = (u8)((spcA << 1) | (spcP & 1)); set_c(spcA >> 7); spcA = r; SETNZ(r); return pc; }
    case 0x7C: { u8 r = (u8)((spcA >> 1) | ((spcP & 1) << 7)); set_c(spcA & 1); spcA = r; SETNZ(r); return pc; }

    /* ---- INC/DEC ---- */
    case 0x8B: case 0xAB: case 0x9B: case 0xBB: case 0x8C: case 0xAC: {
        switch (op) {
        case 0x8B: case 0xAB: ea = EA_DP(); break;
        case 0x9B: case 0xBB: ea = EA_DPX(); break;
        default: ea = EA_ABS(); break;
        }
        al = rd(ea);
        al += (op == 0xAB || op == 0xBB || op == 0xAC) ? 1 : -1;
        SETNZ(al);
        wr(ea, al);
        return pc;
    }
    case 0x9C: spcA--; SETNZ(spcA); return pc;
    case 0xBC: spcA++; SETNZ(spcA); return pc;
    case 0xDC: spcY--; SETNZ(spcY); return pc;
    case 0xFC: spcY++; SETNZ(spcY); return pc;
    case 0x1D: spcX--; SETNZ(spcX); return pc;
    case 0x3D: spcX++; SETNZ(spcX); return pc;

    /* ---- stack ---- */
    case 0x0D: {
        u8 bl = spcP & 0x7D;
        if (spcNZ & 0x80) bl |= 0x80;
        else if (spcNZ == 0) bl |= 0x02;
        push8(bl);
        return pc;
    }
    case 0x2D: push8(spcA); return pc;
    case 0x4D: push8(spcX); return pc;
    case 0x6D: push8(spcY); return pc;
    case 0x8E:
        spcP = pop8();
        spcNZ = 0;
        if (!(spcP & 2)) { spcNZ = 1; if (spcP & 0x80) spcNZ |= 0x80; }
        set_dp_from_p();
        return pc;
    case 0xAE: spcA = pop8(); return pc;
    case 0xCE: spcX = pop8(); return pc;
    case 0xEE: spcY = pop8(); return pc;

    /* ---- TSET1/TCLR1 ---- */
    case 0x0E:
        ea = EA_ABS();
        al = rd(ea);
        SETNZ(al & spcA);
        wr(ea, al | spcA);
        return pc;
    case 0x4E: /* NB: the original adds the direct page base here */
        ea = dpb() + rd16p(pc);
        pc += 2;
        al = rd(ea);
        SETNZ(al & spcA);
        wr(ea, al & ~spcA);
        return pc;

    /* ---- CBNE / DBNZ ---- */
    case 0x2E:
        ea = dpb() + *pc;
        al = rd(ea);
        if (spcA != al) return pc + (s8)pc[1] + 2;
        return pc + 2;
    case 0xDE:
        ea = dpb() + (u8)(*pc + spcX);
        al = rd(ea);
        if (spcA != al) return pc + (s8)pc[1] + 2;
        return pc + 2;
    case 0x6E:
        ea = dpb() + *pc;
        al = rd(ea) - 1;
        if (al != 0) pc += (s8)pc[1] + 2; else pc += 2;
        wr(ea, al);
        return pc;
    case 0xFE:
        spcY--;
        if (spcY != 0) { s8 r = (s8)*pc; pc++; return pc + r; }
        return pc + 1;

    /* ---- jumps / calls ---- */
    case 0x1F: w = (u16)(spcX + rd16p(pc)); return spcRam + rd16p(&spcRam[w]);
    case 0x3F: {
        u32 ret = (u32)(pc + 2 - spcRam);
        push8((u8)(ret >> 8));
        push8((u8)ret);
        return spcRam + rd16p(pc);
    }
    case 0x4F: {
        u32 ret = (u32)(pc + 1 - spcRam);
        push8((u8)(ret >> 8));
        push8((u8)ret);
        return spcRam + 0xFF00 + *pc;
    }
    case 0x5F: return spcRam + rd16p(pc);
    case 0x6F: {
        u32 lo = pop8();
        u32 hi = pop8();
        return spcRam + (lo | hi << 8);
    }

    /* ---- DIV / MUL / DAA / DAS ---- */
    case 0x9E: {
        u32 dividend = (u32)spcA | ((u32)spcY << 8);
        if (spcX == 0) {
            spcA = 0xFF; spcY = 0xFF;
            spcP |= 16; spcP &= 255 - 64;
            return pc;
        }
        {
            u32 q = dividend / spcX, r = dividend % spcX;
            spcA = (u8)q; spcY = (u8)r;
            if ((q >> 8) & 0xFF) { spcP |= 64; spcP &= 255 - 16; }
            else spcP &= 191 - 16;
            SETNZ((u8)q);
        }
        return pc;
    }
    case 0xCF: {
        u32 r = (u32)spcA * spcY;
        spcA = (u8)r; spcY = (u8)(r >> 8);
        set_nz_norm(r, 0x8000, 0xFFFF);
        return pc;
    }
    case 0xBE: spcA = spc_das(spcA); return pc;
    case 0xDF: spcA = spc_daa(spcA); return pc;
    }
    return pc - 1; /* Invalidopcode */
}

/* ---- timers (updatetimer, called once per scanline from execute.asm) ---- */
extern u32 timer2upd;
extern u8 timrcall;
extern u32 tableadc[256];

static void reenablespc(void)
{
    if (cycpbl >= 0x1000000u) {
        cycpbl = 0;
        if (!(curexecstate & 2)) {
            curexecstate |= 2;
            zs_spc_edi_val = tableadc[g_edx & 0xFF];
            zs_spc_edi_dirty = 1;
        }
    }
}

static inline void tick_timer(int i)
{
    /* dec byte[timinl]; jnz skip; inc byte[spcRam+FD+i]; reload; wake the SPC if it was halted */
    if (--TIMINL(i) != 0) return;
    spcRam[0xFD + i]++;
    TIMINL(i) = TIMINCR(i);
    if (spcRam[0xFD + i] != 1) return;
    if (((u8 *)&spchalted)[i] == 0) return;
    reenablespc();
    cycpbl = 0;
}

__attribute__((force_align_arg_pointer)) HID
void zs_updatetimer(u32 edx)
{
    g_edx = edx;
    for (;;) {
        timrcall ^= 1;
        if (timrcall & 1) {
            if (timeron & 1) tick_timer(0);
            if (timeron & 2) tick_timer(1);
        }
        if (timeron & 4) {
            tick_timer(2); tick_timer(2); tick_timer(2); tick_timer(2);
        }
        timer2upd++;
        if (timer2upd == 60) { timer2upd = 0; continue; }
        break;
    }
}

/* ---- asm stubs: call [opcjmptab+op*4] with ebp=pc, ebx=op, preserving everything but ebp/eax/edi(halt) ---- */
extern char zs_spc_stubs[];
__asm__(".text\n.p2align 4\n.hidden zs_spc_common\n"
        "zs_spc_common:\n"
        "  pushl %ecx\n  pushl %edx\n"
        "  pushl %edx\n  pushl 12(%esp)\n  pushl %ebp\n"
        "  call zs_spc_exec\n"
        "  addl $12,%esp\n  movl %eax,%ebp\n"
        "  popl %edx\n  popl %ecx\n  addl $4,%esp\n"
        "  cmpl $0,zs_spc_edi_dirty\n  je 1f\n"
        "  movl zs_spc_edi_val,%edi\n  movl $0,zs_spc_edi_dirty\n"
        "1: ret\n"
        ".p2align 4\n.globl zs_spc_stubs\n.hidden zs_spc_stubs\nzs_spc_stubs:\n"
        ".set zsi,0\n.rept 256\n.p2align 4\n  pushl $zsi\n  jmp zs_spc_common\n.set zsi,zsi+1\n.endr\n"
        ".globl updatetimer\n.type updatetimer,@function\nupdatetimer:\n"
        "  pushl %eax\n  pushl %ecx\n  pushl %edx\n  pushl %edx\n  call zs_updatetimer\n  addl $4,%esp\n"
        "  popl %edx\n  popl %ecx\n  popl %eax\n"
        "  cmpl $0,zs_spc_edi_dirty\n  je 2f\n  movl zs_spc_edi_val,%edi\n  movl $0,zs_spc_edi_dirty\n2: ret\n"
        ".globl zs_spc_install_asm\n.hidden zs_spc_install_asm\nzs_spc_install_asm:\n"
        "  pushal\n  cld\n  call zs_spc_install\n  popal\n  ret\n");

u32 zs_spc_asm_tab[256]; /* original asm handlers (kept for differential testing) */
int zs_spc_use_c = 1;

HID void zs_spc_install(void)
{
    int i;
    for (i = 0; i < 256; i++) zs_spc_asm_tab[i] = opcjmptab[i];
    if (getenv("ZS_SPC_ASM")) { zs_spc_use_c = 0; return; }
    for (i = 0; i < 256; i++) opcjmptab[i] = (u32)(uintptr_t)(zs_spc_stubs + 16 * i);
}

/* =====================================================================
 * Differential fuzzer: runs every opcode through the ORIGINAL asm handler
 * and through the C port from identical random states, and compares all
 * SPC state (RAM, registers, timers, DSP memory/BRR state, edi).
 * Entry point: zs_spc_fuzz(iterations, seed); returns number of mismatches.
 * ===================================================================== */
#include <stdio.h>
#include <setjmp.h>
#include <signal.h>

extern u8 BRRBuffer[];
extern char updatetimer_asm[], updatetimer[];
extern u32 PHspcsave, PHdspsave;

extern void zs_spc_callop(u32 fn, u32 ebp_in, u32 op, u32 edx_in, u32 edi_in, u32 *out);
__asm__(".text\n.globl zs_spc_callop\n.hidden zs_spc_callop\nzs_spc_callop:\n"
        "  pushal\n  movl 56(%esp),%eax\n  pushl %eax\n"
        "  movl 40(%esp),%eax\n  movl 44(%esp),%ebp\n  movl 48(%esp),%ebx\n"
        "  movl 52(%esp),%edx\n  movl 56(%esp),%edi\n  cld\n  call *%eax\n"
        "  movl (%esp),%eax\n  movl %ebp,(%eax)\n  movl %edi,4(%eax)\n  movl %edx,8(%eax)\n"
        "  addl $4,%esp\n  popal\n  ret\n");

typedef struct {
    u8 *spc; u8 *dsp; u8 dspmem[256];
    u32 numread, halted, cyc, s700read, execstate, nout[3], t2u;
} Snap;

static sigjmp_buf fz_jb;
static void fz_segv(int s) { (void)s; siglongjmp(fz_jb, 1); }

static u32 fz_rng;
static u32 rnd(void) { fz_rng ^= fz_rng << 13; fz_rng ^= fz_rng >> 17; fz_rng ^= fz_rng << 5; return fz_rng; }
static u8 biased(void)
{
    u32 r = rnd();
    if ((r & 7) == 0) return (u8)(0xEF + ((r >> 8) % 18));
    if ((r & 7) == 1) return (u8)(r >> 8) % 4 ? 0x00 : 0xFF;
    return (u8)(r >> 16);
}

static u32 fz_spcsz, fz_dspsz;
static void snap_take(Snap *s)
{
    memcpy(s->spc, spcRam, fz_spcsz);
    memcpy(s->dsp, BRRBuffer, fz_dspsz);
    memcpy(s->dspmem, DSPMem, 256);
    s->t2u = timer2upd; s->numread = spcnumread; s->halted = spchalted; s->cyc = cycpbl; s->s700read = spc700read; s->execstate = curexecstate;
}
static void snap_put(const Snap *s)
{
    memcpy(spcRam, s->spc, fz_spcsz);
    memcpy(BRRBuffer, s->dsp, fz_dspsz);
    memcpy(DSPMem, s->dspmem, 256);
    timer2upd = s->t2u; spcnumread = s->numread; spchalted = s->halted; cycpbl = s->cyc; spc700read = s->s700read; curexecstate = s->execstate;
}

unsigned zs_spc_fuzz(unsigned iters, unsigned seed)
{
    Snap orig, base, a;
    fz_spcsz = PHspcsave + 2048; fz_dspsz = PHdspsave;
    u32 outc[3], outa[3];
    unsigned mism = 0, crashes = 0, i, t2u;
    unsigned permis[257] = {0}, percnt[257] = {0};
    struct sigaction sa, old;
    u8 saveSH = SPC700sh, saveDis = disablespcclr, saveSkip = SPCSkipXtraROM;
    orig.spc = malloc(fz_spcsz); orig.dsp = malloc(fz_dspsz);
    base.spc = malloc(fz_spcsz); base.dsp = malloc(fz_dspsz);
    a.spc = malloc(fz_spcsz); a.dsp = malloc(fz_dspsz);
    fz_rng = seed ? seed : 1;
    snap_take(&orig);

    memset(&sa, 0, sizeof sa);
    sa.sa_handler = fz_segv;
    sa.sa_flags = SA_NODEFER;
    sigaction(SIGSEGV, &sa, &old);

    for (i = 0; i < iters; i++) {
        unsigned op = i % 257, k;
        if (getenv("ZS_FUZZ_OP")) op = strtoul(getenv("ZS_FUZZ_OP"), 0, 16);
        u32 p, off;
        int crashed = 0;
        /* fresh random machine state */
        if ((i / 256) % 4 == 0)
            for (k = 0; k < 0x10000; k++) spcRam[k] = (u8)rnd();
        /* the program counter area: keep inside RAM, operands biased */
        p = rnd() % 0xFF00u;
        spcRam[p] = (u8)op; /* op 256 = updatetimer */
        t2u = rnd() % 61; timer2upd = t2u; timrcall = (u8)rnd();
        spcRam[p + 1] = biased();
        spcRam[p + 2] = biased();
        spcRam[p + 3] = (rnd() & 3) ? (u8)(rnd() & 0x1F) : biased();
        spcA = biased(); spcX = biased(); spcY = biased();
        spcP = (u8)rnd();
        switch (rnd() & 3) { case 0: spcNZ = 0; break; case 1: spcNZ = 1; break; case 2: spcNZ = 0x80; break; default: spcNZ = (u8)rnd(); }
        spcS = 0x100 | (rnd() & 0xFF);
        spcRamDP = spcRam + ((rnd() & 1) ? 0x100 : 0);
        spcRam[0xF1] = (u8)rnd();
        spcRam[0xF2] = (u8)rnd();
        for (k = 0xF4; k <= 0xFF; k++) if (rnd() & 1) spcRam[k] = (u8)(rnd() & ((rnd() & 1) ? 0xF : 0xFF));
        timeron = (u8)(rnd() & 7);
        for (k = 0; k < 3; k++) { TIMINCR(k) = (rnd() & 3) ? (u8)(rnd() & 0x3F) : 0; TIMINL(k) = (u8)(rnd() & 3); }
        for (k = 0; k < 3; k++) ((u8 *)&spcnumread)[k] = (u8)(rnd() % 9);
        spchalted = rnd(); cycpbl = rnd(); spc700read = rnd();
        curexecstate = (curexecstate & ~2u) | ((rnd() & 1) << 1);
        SPC700sh = (rnd() & 7) == 0;
        disablespcclr = (rnd() & 7) == 0;
        SPCSkipXtraROM = (rnd() & 7) == 0;
        {
            u32 edxv = rnd();
            u32 ebp_in = (u32)(uintptr_t)(spcRam + p + 1);
            snap_take(&base);
            /* asm reference */
            crashed = 0;
            if (sigsetjmp(fz_jb, 1) == 0)
                zs_spc_callop(op == 256 ? (u32)(uintptr_t)updatetimer_asm : zs_spc_asm_tab[op], ebp_in, op, edxv, 0xDEADBEEF, outa);
            else crashed = 1;
            if (crashed) { crashes++; snap_put(&base); continue; }
            snap_take(&a); a.t2u = timer2upd;
            memcpy(a.nout, outa, sizeof outa);
            snap_put(&base);
            /* C port, through the real stub */
            zs_spc_callop(op == 256 ? (u32)(uintptr_t)updatetimer : (u32)(uintptr_t)(zs_spc_stubs + 16 * op), ebp_in, op, edxv, 0xDEADBEEF, outc);
            percnt[op]++;
            {
                int bad = outc[0] != outa[0] || outc[1] != outa[1] || outc[2] != outa[2]
                       || memcmp(spcRam, a.spc, fz_spcsz) || memcmp(BRRBuffer, a.dsp, fz_dspsz)
                       || memcmp(DSPMem, a.dspmem, 256) || spcnumread != a.numread || spchalted != a.halted
                       || timer2upd != a.t2u || cycpbl != a.cyc || spc700read != a.s700read || curexecstate != a.execstate;
                if (bad) {
                    mism++; permis[op]++;
                    if (permis[op] <= 2) {
                        u32 d = 0, dd = 0; int first = -1;
                        for (d = 0; d < fz_spcsz; d++) if (spcRam[d] != a.spc[d]) { if (first < 0) first = d; dd++; }
                        fprintf(stderr, "MISMATCH i=%u op %02X: ebp %08x/%08x edi %08x/%08x edx %08x/%08x  spcdiff=%u first@%d (c=%02x asm=%02x)  numread %x/%x halted %x/%x cyc %x/%x rd %x/%x exec %x/%x dsp=%d dspmem=%d\n",
                                i, op, outc[0] - (u32)(uintptr_t)spcRam, outa[0] - (u32)(uintptr_t)spcRam, outc[1], outa[1], outc[2], outa[2], dd, first,
                                first >= 0 ? spcRam[first] : 0, first >= 0 ? a.spc[first] : 0, spcnumread, a.numread, spchalted, a.halted,
                                cycpbl, a.cyc, spc700read, a.s700read, curexecstate, a.execstate,
                                memcmp(BRRBuffer, a.dsp, fz_dspsz) != 0, memcmp(DSPMem, a.dspmem, 256) != 0);
                        fprintf(stderr, "   pre: A=%02x X=%02x Y=%02x P=%02x NZ=%02x S=%03x DP=%x pc=%04x bytes=%02x %02x %02x %02x timeron=%x ram[F1]=%02x\n",
                                base.spc[0x10014], base.spc[0x10018], base.spc[0x1001C], base.spc[0x10020], base.spc[0x10024],
                                *(u32 *)&base.spc[0x10028], *(u32 *)&base.spc[0x1002C] - (u32)(uintptr_t)spcRam, p, spcRam[p], spcRam[p+1], spcRam[p+2], spcRam[p+3], base.spc[0x10000+0] ? 0 : 0, base.spc[0xF1]);
                        if (first >= 0 && first < (int)fz_spcsz) {
                            static const char *names[] = { "ram", "pcram", "A", "X", "Y", "P", "NZ" };
                            (void)names;
                            fprintf(stderr, "   first diff offset %d (spcRam+%d); spcPCRam@%d A@%d P@%d NZ@%d S@%d\n", first, first,
                                    (int)((u8 *)&spcRam[0] - (u8 *)&spcRam[0]), 0, 0, 0, 0);
                        }
                    }
                }
            }
            snap_put(&base);
        }
    }
    sigaction(SIGSEGV, &old, NULL);
    snap_put(&orig);
    SPC700sh = saveSH; disablespcclr = saveDis; SPCSkipXtraROM = saveSkip;
    {
        unsigned bad_ops = 0;
        for (i = 0; i < 257; i++) if (permis[i]) bad_ops++;
        fprintf(stderr, "spc fuzz: %u iterations, %u asm-crash skips, %u mismatches in %u opcodes\n", iters, crashes, mism, bad_ops);
        for (i = 0; i < 257; i++) if (permis[i]) fprintf(stderr, "  op %02X: %u/%u mismatches\n", i, permis[i], percnt[i]);
    }
    free(orig.spc); free(orig.dsp); free(base.spc); free(base.dsp); free(a.spc); free(a.dsp);
    return mism;
}
