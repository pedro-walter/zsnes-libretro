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
#include "zl.h"

typedef int8_t s8;


/* ---- state shared with the asm ---- */
extern u8 spcRam[0x10000];
extern u8 spcA, spcX, spcY, spcP, spcNZ;
extern u32 spcS;
extern u32 spcRamDP;                      /* guest pointer slot */
#define DPP ((u8 *)(uintptr_t)spcRamDP)
#define SETDP(p) (spcRamDP = (u32)(uintptr_t)(p))
extern u8 reg1read; /* reg1read..reg4read are consecutive */
extern u8 timeron, timincr0, timinl0;
extern u8 spcextraram[64];
extern u8 SPCROM[64];
extern u8 DSPMem[256];
extern u32 spcnumread, spchalted, cycpbl, spc700read, curexecstate;
extern u8 SPC700sh, disablespcclr, SPCSkipXtraROM;
extern u32 tableadb[256];
extern u32 opcjmptab[256];
extern u32 dspWptr[256];

#define TIMINCR(i) (&timincr0)[i]
#define TIMINL(i) (&timinl0)[i]
#define REGREAD(i) (&reg1read)[i]

/* set when haltspc() must reload the CPU's edi (exec table pointer) */
static u32 zs_spc_edi_dirty, zs_spc_edi_val;
static u32 g_edx; /* cpu edx on entry (dl = 65816 P) */
static u32 dsp_res;

/* call the DSP register-write handler dspWptr[reg] (guest code id) with al=val; returns al afterwards */
static u32 zs_dspw_call(u32 reg, u32 val)
{
    lift_state_t sv = LG;
    u32 r;
    LG.eax = val; LG.ebx = reg;
    lift_call(dspWptr[reg & 0xFF]);
    r = LG.eax;
    LG = sv;
    return r;
}

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
static inline u32 dpb(void) { return (u32)(DPP - spcRam); }

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

static inline void set_dp_from_p(void) { SETDP(spcRam + ((spcP & 32) ? 0x100 : 0)); }

HID u8 *zs_spc_exec(u8 *pc, unsigned op, u32 edx)
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
    case 0x20: spcP &= ~0x20; SETDP(spcRam); return pc;
    case 0x40: spcP |= 0x20; spcP &= ~0x04; SETDP(spcRam + 0x100); return pc;
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

HID void zs_updatetimer(u32 edx)
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


/* ---- overrides of the asm handlers: ebp = SPC pc (arena address), ebx = opcode ---- */
static inline void spc_op(unsigned op)
{
    LG.ebp = (u32)(uintptr_t)zs_spc_exec((u8 *)(uintptr_t)LG.ebp, op, LG.edx);
    if (zs_spc_edi_dirty) { LG.edi = zs_spc_edi_val; zs_spc_edi_dirty = 0; }
}
void zl_updatetimer(void)
{
    zs_updatetimer(LG.edx);
    if (zs_spc_edi_dirty) { LG.edi = zs_spc_edi_val; zs_spc_edi_dirty = 0; }
}
void zl_spc00(void) { spc_op(0x00); }
void zl_spc01(void) { spc_op(0x01); }
void zl_spc02(void) { spc_op(0x02); }
void zl_spc03(void) { spc_op(0x03); }
void zl_spc04(void) { spc_op(0x04); }
void zl_spc05(void) { spc_op(0x05); }
void zl_spc06(void) { spc_op(0x06); }
void zl_spc07(void) { spc_op(0x07); }
void zl_spc08(void) { spc_op(0x08); }
void zl_spc09(void) { spc_op(0x09); }
void zl_spc0A(void) { spc_op(0x0A); }
void zl_spc0B(void) { spc_op(0x0B); }
void zl_spc0C(void) { spc_op(0x0C); }
void zl_spc0D(void) { spc_op(0x0D); }
void zl_spc0E(void) { spc_op(0x0E); }
void zl_spc0F(void) { spc_op(0x0F); }
void zl_spc10(void) { spc_op(0x10); }
void zl_spc11(void) { spc_op(0x11); }
void zl_spc12(void) { spc_op(0x12); }
void zl_spc13(void) { spc_op(0x13); }
void zl_spc14(void) { spc_op(0x14); }
void zl_spc15(void) { spc_op(0x15); }
void zl_spc16(void) { spc_op(0x16); }
void zl_spc17(void) { spc_op(0x17); }
void zl_spc18(void) { spc_op(0x18); }
void zl_spc19(void) { spc_op(0x19); }
void zl_spc1A(void) { spc_op(0x1A); }
void zl_spc1B(void) { spc_op(0x1B); }
void zl_spc1C(void) { spc_op(0x1C); }
void zl_spc1D(void) { spc_op(0x1D); }
void zl_spc1E(void) { spc_op(0x1E); }
void zl_spc1F(void) { spc_op(0x1F); }
void zl_spc20(void) { spc_op(0x20); }
void zl_spc21(void) { spc_op(0x21); }
void zl_spc22(void) { spc_op(0x22); }
void zl_spc23(void) { spc_op(0x23); }
void zl_spc24(void) { spc_op(0x24); }
void zl_spc25(void) { spc_op(0x25); }
void zl_spc26(void) { spc_op(0x26); }
void zl_spc27(void) { spc_op(0x27); }
void zl_spc28(void) { spc_op(0x28); }
void zl_spc29(void) { spc_op(0x29); }
void zl_spc2A(void) { spc_op(0x2A); }
void zl_spc2B(void) { spc_op(0x2B); }
void zl_spc2C(void) { spc_op(0x2C); }
void zl_spc2D(void) { spc_op(0x2D); }
void zl_spc2E(void) { spc_op(0x2E); }
void zl_spc2F(void) { spc_op(0x2F); }
void zl_spc30(void) { spc_op(0x30); }
void zl_spc31(void) { spc_op(0x31); }
void zl_spc32(void) { spc_op(0x32); }
void zl_spc33(void) { spc_op(0x33); }
void zl_spc34(void) { spc_op(0x34); }
void zl_spc35(void) { spc_op(0x35); }
void zl_spc36(void) { spc_op(0x36); }
void zl_spc37(void) { spc_op(0x37); }
void zl_spc38(void) { spc_op(0x38); }
void zl_spc39(void) { spc_op(0x39); }
void zl_spc3A(void) { spc_op(0x3A); }
void zl_spc3B(void) { spc_op(0x3B); }
void zl_spc3C(void) { spc_op(0x3C); }
void zl_spc3D(void) { spc_op(0x3D); }
void zl_spc3E(void) { spc_op(0x3E); }
void zl_spc3F(void) { spc_op(0x3F); }
void zl_spc40(void) { spc_op(0x40); }
void zl_spc41(void) { spc_op(0x41); }
void zl_spc42(void) { spc_op(0x42); }
void zl_spc43(void) { spc_op(0x43); }
void zl_spc44(void) { spc_op(0x44); }
void zl_spc45(void) { spc_op(0x45); }
void zl_spc46(void) { spc_op(0x46); }
void zl_spc47(void) { spc_op(0x47); }
void zl_spc48(void) { spc_op(0x48); }
void zl_spc49(void) { spc_op(0x49); }
void zl_spc4A(void) { spc_op(0x4A); }
void zl_spc4B(void) { spc_op(0x4B); }
void zl_spc4C(void) { spc_op(0x4C); }
void zl_spc4D(void) { spc_op(0x4D); }
void zl_spc4E(void) { spc_op(0x4E); }
void zl_spc4F(void) { spc_op(0x4F); }
void zl_spc50(void) { spc_op(0x50); }
void zl_spc51(void) { spc_op(0x51); }
void zl_spc52(void) { spc_op(0x52); }
void zl_spc53(void) { spc_op(0x53); }
void zl_spc54(void) { spc_op(0x54); }
void zl_spc55(void) { spc_op(0x55); }
void zl_spc56(void) { spc_op(0x56); }
void zl_spc57(void) { spc_op(0x57); }
void zl_spc58(void) { spc_op(0x58); }
void zl_spc59(void) { spc_op(0x59); }
void zl_spc5A(void) { spc_op(0x5A); }
void zl_spc5B(void) { spc_op(0x5B); }
void zl_spc5C(void) { spc_op(0x5C); }
void zl_spc5D(void) { spc_op(0x5D); }
void zl_spc5E(void) { spc_op(0x5E); }
void zl_spc5F(void) { spc_op(0x5F); }
void zl_spc60(void) { spc_op(0x60); }
void zl_spc61(void) { spc_op(0x61); }
void zl_spc62(void) { spc_op(0x62); }
void zl_spc63(void) { spc_op(0x63); }
void zl_spc64(void) { spc_op(0x64); }
void zl_spc65(void) { spc_op(0x65); }
void zl_spc66(void) { spc_op(0x66); }
void zl_spc67(void) { spc_op(0x67); }
void zl_spc68(void) { spc_op(0x68); }
void zl_spc69(void) { spc_op(0x69); }
void zl_spc6A(void) { spc_op(0x6A); }
void zl_spc6B(void) { spc_op(0x6B); }
void zl_spc6C(void) { spc_op(0x6C); }
void zl_spc6D(void) { spc_op(0x6D); }
void zl_spc6E(void) { spc_op(0x6E); }
void zl_spc6F(void) { spc_op(0x6F); }
void zl_spc70(void) { spc_op(0x70); }
void zl_spc71(void) { spc_op(0x71); }
void zl_spc72(void) { spc_op(0x72); }
void zl_spc73(void) { spc_op(0x73); }
void zl_spc74(void) { spc_op(0x74); }
void zl_spc75(void) { spc_op(0x75); }
void zl_spc76(void) { spc_op(0x76); }
void zl_spc77(void) { spc_op(0x77); }
void zl_spc78(void) { spc_op(0x78); }
void zl_spc79(void) { spc_op(0x79); }
void zl_spc7A(void) { spc_op(0x7A); }
void zl_spc7B(void) { spc_op(0x7B); }
void zl_spc7C(void) { spc_op(0x7C); }
void zl_spc7D(void) { spc_op(0x7D); }
void zl_spc7E(void) { spc_op(0x7E); }
void zl_spc7F(void) { spc_op(0x7F); }
void zl_spc80(void) { spc_op(0x80); }
void zl_spc81(void) { spc_op(0x81); }
void zl_spc82(void) { spc_op(0x82); }
void zl_spc83(void) { spc_op(0x83); }
void zl_spc84(void) { spc_op(0x84); }
void zl_spc85(void) { spc_op(0x85); }
void zl_spc86(void) { spc_op(0x86); }
void zl_spc87(void) { spc_op(0x87); }
void zl_spc88(void) { spc_op(0x88); }
void zl_spc89(void) { spc_op(0x89); }
void zl_spc8A(void) { spc_op(0x8A); }
void zl_spc8B(void) { spc_op(0x8B); }
void zl_spc8C(void) { spc_op(0x8C); }
void zl_spc8D(void) { spc_op(0x8D); }
void zl_spc8E(void) { spc_op(0x8E); }
void zl_spc8F(void) { spc_op(0x8F); }
void zl_spc90(void) { spc_op(0x90); }
void zl_spc91(void) { spc_op(0x91); }
void zl_spc92(void) { spc_op(0x92); }
void zl_spc93(void) { spc_op(0x93); }
void zl_spc94(void) { spc_op(0x94); }
void zl_spc95(void) { spc_op(0x95); }
void zl_spc96(void) { spc_op(0x96); }
void zl_spc97(void) { spc_op(0x97); }
void zl_spc98(void) { spc_op(0x98); }
void zl_spc99(void) { spc_op(0x99); }
void zl_spc9A(void) { spc_op(0x9A); }
void zl_spc9B(void) { spc_op(0x9B); }
void zl_spc9C(void) { spc_op(0x9C); }
void zl_spc9D(void) { spc_op(0x9D); }
void zl_spc9E(void) { spc_op(0x9E); }
void zl_spc9F(void) { spc_op(0x9F); }
void zl_spcA0(void) { spc_op(0xA0); }
void zl_spcA1(void) { spc_op(0xA1); }
void zl_spcA2(void) { spc_op(0xA2); }
void zl_spcA3(void) { spc_op(0xA3); }
void zl_spcA4(void) { spc_op(0xA4); }
void zl_spcA5(void) { spc_op(0xA5); }
void zl_spcA6(void) { spc_op(0xA6); }
void zl_spcA7(void) { spc_op(0xA7); }
void zl_spcA8(void) { spc_op(0xA8); }
void zl_spcA9(void) { spc_op(0xA9); }
void zl_spcAA(void) { spc_op(0xAA); }
void zl_spcAB(void) { spc_op(0xAB); }
void zl_spcAC(void) { spc_op(0xAC); }
void zl_spcAD(void) { spc_op(0xAD); }
void zl_spcAE(void) { spc_op(0xAE); }
void zl_spcAF(void) { spc_op(0xAF); }
void zl_spcB0(void) { spc_op(0xB0); }
void zl_spcB1(void) { spc_op(0xB1); }
void zl_spcB2(void) { spc_op(0xB2); }
void zl_spcB3(void) { spc_op(0xB3); }
void zl_spcB4(void) { spc_op(0xB4); }
void zl_spcB5(void) { spc_op(0xB5); }
void zl_spcB6(void) { spc_op(0xB6); }
void zl_spcB7(void) { spc_op(0xB7); }
void zl_spcB8(void) { spc_op(0xB8); }
void zl_spcB9(void) { spc_op(0xB9); }
void zl_spcBA(void) { spc_op(0xBA); }
void zl_spcBB(void) { spc_op(0xBB); }
void zl_spcBC(void) { spc_op(0xBC); }
void zl_spcBD(void) { spc_op(0xBD); }
void zl_spcBE(void) { spc_op(0xBE); }
void zl_spcBF(void) { spc_op(0xBF); }
void zl_spcC0(void) { spc_op(0xC0); }
void zl_spcC1(void) { spc_op(0xC1); }
void zl_spcC2(void) { spc_op(0xC2); }
void zl_spcC3(void) { spc_op(0xC3); }
void zl_spcC4(void) { spc_op(0xC4); }
void zl_spcC5(void) { spc_op(0xC5); }
void zl_spcC6(void) { spc_op(0xC6); }
void zl_spcC7(void) { spc_op(0xC7); }
void zl_spcC8(void) { spc_op(0xC8); }
void zl_spcC9(void) { spc_op(0xC9); }
void zl_spcCA(void) { spc_op(0xCA); }
void zl_spcCB(void) { spc_op(0xCB); }
void zl_spcCC(void) { spc_op(0xCC); }
void zl_spcCD(void) { spc_op(0xCD); }
void zl_spcCE(void) { spc_op(0xCE); }
void zl_spcCF(void) { spc_op(0xCF); }
void zl_spcD0(void) { spc_op(0xD0); }
void zl_spcD1(void) { spc_op(0xD1); }
void zl_spcD2(void) { spc_op(0xD2); }
void zl_spcD3(void) { spc_op(0xD3); }
void zl_spcD4(void) { spc_op(0xD4); }
void zl_spcD5(void) { spc_op(0xD5); }
void zl_spcD6(void) { spc_op(0xD6); }
void zl_spcD7(void) { spc_op(0xD7); }
void zl_spcD8(void) { spc_op(0xD8); }
void zl_spcD9(void) { spc_op(0xD9); }
void zl_spcDA(void) { spc_op(0xDA); }
void zl_spcDB(void) { spc_op(0xDB); }
void zl_spcDC(void) { spc_op(0xDC); }
void zl_spcDD(void) { spc_op(0xDD); }
void zl_spcDE(void) { spc_op(0xDE); }
void zl_spcDF(void) { spc_op(0xDF); }
void zl_spcE0(void) { spc_op(0xE0); }
void zl_spcE1(void) { spc_op(0xE1); }
void zl_spcE2(void) { spc_op(0xE2); }
void zl_spcE3(void) { spc_op(0xE3); }
void zl_spcE4(void) { spc_op(0xE4); }
void zl_spcE5(void) { spc_op(0xE5); }
void zl_spcE6(void) { spc_op(0xE6); }
void zl_spcE7(void) { spc_op(0xE7); }
void zl_spcE8(void) { spc_op(0xE8); }
void zl_spcE9(void) { spc_op(0xE9); }
void zl_spcEA(void) { spc_op(0xEA); }
void zl_spcEB(void) { spc_op(0xEB); }
void zl_spcEC(void) { spc_op(0xEC); }
void zl_spcED(void) { spc_op(0xED); }
void zl_spcEE(void) { spc_op(0xEE); }
void zl_spcEF(void) { spc_op(0xEF); }
void zl_spcF0(void) { spc_op(0xF0); }
void zl_spcF1(void) { spc_op(0xF1); }
void zl_spcF2(void) { spc_op(0xF2); }
void zl_spcF3(void) { spc_op(0xF3); }
void zl_spcF4(void) { spc_op(0xF4); }
void zl_spcF5(void) { spc_op(0xF5); }
void zl_spcF6(void) { spc_op(0xF6); }
void zl_spcF7(void) { spc_op(0xF7); }
void zl_spcF8(void) { spc_op(0xF8); }
void zl_spcF9(void) { spc_op(0xF9); }
void zl_spcFA(void) { spc_op(0xFA); }
void zl_spcFB(void) { spc_op(0xFB); }
void zl_spcFC(void) { spc_op(0xFC); }
void zl_spcFD(void) { spc_op(0xFD); }
void zl_spcFE(void) { spc_op(0xFE); }
void zl_spcFF(void) { spc_op(0xFF); }
