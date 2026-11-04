#include <stdint.h>
/* SuperFX (GSU) interpreter: chips/fxemu2.asm, fxemu2b.asm, fxemu2c.asm, ported to C.
 *
 * The asm is a threaded interpreter: every handler fetches the next opcode into cl, bumps the program counter (ebp), and
 * "prefix" opcodes (TO/WITH/FROM/ALT) call the next handler through a table and undo their state change when it returns.
 * That nesting (and the quirks of the three table families) is kept as is: handlers are C functions that run the same
 * steps in the same order, on the same guest variables.
 *
 *   FxTable   (base)   nested dispatch after most opcodes            FxOpXX[Ak]
 *   FxTableb           after "WITH R15" (R15 kept in sync)             FxOpbXX
 *   FxTablec           after WITH (B flag set: TO is MOVE, FROM is MOVES)  FxOpcXX
 *   FxTabled           the top level loop; same handlers as the base ones plus "dec NumberOfOpcodes; continue"  FxOpdXX
 *
 * The tables live in guest memory (filled by InitFxTables, entry 4Ch re-pointed by CMODE / UpdatePORSCMR); the entries are
 * code ids of the asm labels, which sfx_tab.h (tools/lift/gen_sfx.py) maps to (kind, argument).  Only MainLoop is replaced.
 *
 * Registers: ecx = (ALT bits << 8) | opcode, ebp = program counter (host pointer), esi / edi = source / destination
 * register slots (guest addresses inside SfxR0..R15). */
#include "zl.h"
#include <stdint.h>
#include "sfx_tab.h"

#define V(s)   M32(ZO_##s)
#define V8(s)  M8(ZO_##s)
#define V16(s) M16(ZO_##s)
#define R32(n) M32(ZO_SfxR0 + 4u * (n))
#define R16(n) M16(ZO_SfxR0 + 4u * (n))
#define RA(n)  (ZO_SfxR0 + 4u * (n))

static u32 ecx, ebp, esi, edi;
#define FETCHPIPE() (ecx = (ecx & ~0xFFu) | M8(ebp))
#define UPDATE_R14() (V(SfxRomBuffer) = V(SfxCROM) + V(SfxR14))
#define NEST(tbl) run(M32((tbl) + ecx * 4u))
#define TBL_A ZO_FxTable
#define TBL_B ZO_FxTableb
#define TBL_C ZO_FxTablec
#define TBL_D ZO_FxTabled
static const u32 tbl_of[3] = { ZO_FxTable, ZO_FxTableb, ZO_FxTablec };

static int run(u32 id);
static int sfx_dbg;
__attribute__((constructor)) static void sfx_dbg_init(void) { sfx_dbg = getenv("ZL_SFX_TRACE") != NULL; }

/* ---------------- label lookup ---------------- */
#define HN 4096
static const struct sfxlab *htab[HN];
static int hinit;
static const struct sfxlab *lookup(u32 id)
{
    u32 h = (id * 2654435761u) >> 20;
    if (!hinit) {
        unsigned i;
        hinit = 1;
        for (i = 0; i < sizeof sfx_labs / sizeof sfx_labs[0]; i++) {
            u32 k = (sfx_labs[i].id * 2654435761u) >> 20;
            while (htab[k & (HN - 1)]) k++;
            htab[k & (HN - 1)] = &sfx_labs[i];
        }
    }
    for (;; h++) {
        const struct sfxlab *l = htab[h & (HN - 1)];
        if (!l) return NULL;
        if (l->id == id) return l;
    }
}

/* ---------------- arithmetic helpers (16-bit operations on the low word, upper bits of the first operand kept) ---------------- */
static inline u32 add16(u32 a, u32 b, u32 cin, u32 *cf, u32 *of)
{
    u32 r = (a & 0xFFFF) + (b & 0xFFFF) + cin;
    *cf = (r >> 16) & 1; *of = (~(a ^ b) & (a ^ r) & 0x8000) != 0;
    return (a & 0xFFFF0000u) | (r & 0xFFFF);
}
static inline u32 sub16(u32 a, u32 b, u32 bin, u32 *cf, u32 *of)
{
    u32 r = (a & 0xFFFF) - (b & 0xFFFF) - bin;
    *cf = (r >> 16) & 1; *of = ((a ^ b) & (a ^ r) & 0x8000) != 0;
    return (a & 0xFFFF0000u) | (r & 0xFFFF);
}
#define SET_OV(v) (V8(SfxOverflow) = (u8)(v))
#define SET_CY(v) (V8(SfxCarry) = (u8)(v))

static void do_add(u32 b, u32 cin_from_carry)
{
    u32 cf, of, eax = M32(esi), cin = 0;
    if (cin_from_carry) { cin = V8(SfxCarry) & 1; }
    FETCHPIPE();
    eax = add16(eax, b, cin, &cf, &of);
    SET_OV(of); SET_CY(cf); V(SfxSignZero) = eax; ebp++; M32(edi) = eax;
}
static void do_sub(u32 b, int mode)   /* mode 0: sub  1: sbc  2: cmp */
{
    u32 cf, of, eax = M32(esi), bin = 0;
    if (mode == 1) bin = (V8(SfxCarry) < 1);        /* cmp byte[SfxCarry],1 ; sbb */
    eax = sub16(eax, b, bin, &cf, &of);
    SET_OV(of); SET_CY(cf); V8(SfxCarry) ^= 1;
    if (mode == 2) { V(SfxSignZero) = eax; ebp++; }
    else { ebp++; M32(edi) = eax; V(SfxSignZero) = eax; }
}

/* ---------------- plotting ---------------- */
static void drawpix(u32 eax, u32 edx, int bits, int dither)
{
    u32 ebx, p01 = V(fxbit01pcal), p23 = V(fxbit23pcal), p45 = V(fxbit45pcal), p67 = V(fxbit67pcal);
    if (bits == 0) {                   /* 4 bit */
        M32(eax) &= edx; M32(eax + 16) &= edx; edx = ~edx;
        ebx = (dither ? p45 : p01) & edx; M32(eax) |= ebx;
        edx &= dither ? p67 : p23; M32(eax + 16) |= edx;
    } else if (bits == 1) {            /* 2 bit */
        M32(eax) &= edx; edx = ~edx; edx &= dither ? p45 : p01; M32(eax) |= edx;
    } else {                           /* 8 bit */
        M32(eax) &= edx; M32(eax + 16) &= edx; M32(eax + 32) &= edx; M32(eax + 48) &= edx; edx = ~edx;
        ebx = (dither ? p45 : p01) & edx; M32(eax) |= ebx;
        ebx = (dither ? p67 : p23) & edx; M32(eax + 16) |= ebx;
        ebx = (dither ? p01 : p45) & edx; M32(eax + 32) |= ebx;
        edx &= dither ? p23 : p67; M32(eax + 48) |= edx;
    }
}
static void plot(u32 arg)
{
    int bits = arg & 3, z = (arg >> 2) & 1, d = (arg >> 3) & 1, low = (arg >> 4) & 1;
    u32 ebx = (V(SfxR2) & ~0xFF00u) | ((V8(SfxR1) & 0xFFu) << 8), eax, edx;
    u32 shift = bits == 0 ? 5 : bits == 1 ? 4 : 6, zm = bits == 0 ? 0x0F : bits == 1 ? 0x03 : low ? 0x0F : 0xFF;
    FETCHPIPE();
    eax = V(sfxclineloc);
    ebp++;
    eax = M32(eax + ebx * 4u);
    if (eax != 0xFFFFFFFFu) {
        eax <<= shift; ebx &= 7; ebx += ebx; eax += ebx; eax += V(SCBRrel);
        ebx = V8(SfxR1);
        edx = M32(ZO_fxxand + ebx * 4u);
        if (z && !(V8(SfxCOLR) & zm)) goto nodraw;
        if (d && ((V8(SfxR1) ^ V8(SfxR2)) & 1)) { drawpix(eax, edx, bits, 1); V16(SfxR1)++; return; }
        drawpix(eax, edx, bits, 0);
    }
nodraw:
    V16(SfxR1)++;
}

static u32 lineloc_for_por(void)
{
    u32 al;
    if (V8(SfxPOR) & 0x10) return V(sfxobjlineloc);
    al = V8(SfxSCMR) & 0x24;
    if (al == 4) return V(sfx160lineloc);
    if (al == 32) return V(sfx192lineloc);
    if (al == 36) return V(sfxobjlineloc);
    return V(sfx128lineloc);
}

static void rpix(void)
{
    u32 ebx = (V(SfxR2) & ~0xFF00u) | ((V8(SfxR1) & 0xFFu) << 8), eax, bh = 0, bl;
    int bits;
    FETCHPIPE();
    eax = lineloc_for_por();
    ebx = M32(eax + ebx * 4u);
    if (ebx == 0xFFFFFFFFu) { bh = 0xFF; goto done; }
    bits = V8(SfxSCMR) & 3;
    ebx <<= (bits == 0 ? 4 : bits == 3 ? 6 : 5);
    eax = ((u32)V8(SfxSCBR)) << 10; eax += ebx; eax += V(sfxramdata);
    ebx = V(SfxR2) & 7; eax += ebx << 1;
    bl = 1u << ((V8(SfxR1) & 7) ^ 7);
    bh = 0;
    if (M8(eax) & bl) bh |= 0x01;
    if (M8(eax + 1) & bl) bh |= 0x02;
    if (bits != 0) {
        if (M8(eax + 16) & bl) bh |= 0x04;
        if (M8(eax + 17) & bl) bh |= 0x08;
        if (bits == 3) {
            if (M8(eax + 32) & bl) bh |= 0x10;
            if (M8(eax + 33) & bl) bh |= 0x20;
            if (M8(eax + 48) & bl) bh |= 0x40;
            if (M8(eax + 49) & bl) bh |= 0x80;
        }
    }
done:
    ebx = bh;
    ebp++;
    M32(edi) = ebx; V(flagnz) = ebx;
}

/* ---------------- COLOR / GETC / CMODE ---------------- */
static void set_color(u32 eax)
{
    u32 ebx, bl;
    if (V8(SfxPOR) & 4) { bl = (eax & 0xFF) >> 4; eax = (eax & ~0xFFu) | ((eax & 0xF0) | bl); }
    if (V8(SfxPOR) & 8) {
        bl = V8(SfxCOLR);
        eax = (eax & ~0xFFu) | (((eax & 0x0F) | (bl & 0xF0)) & 0xFF);
    }
    if (V8(SfxCOLR) != (u8)eax) {
        V8(SfxCOLR) = (u8)eax; eax &= 0xFF;
        ebx = M32(ZO_fxbit01 + eax * 4u); V(fxbit01pcal) = ebx;
        ebx = M32(ZO_fxbit23 + eax * 4u); V(fxbit23pcal) = ebx;
        ebx = M32(ZO_fxbit45 + eax * 4u); V(fxbit45pcal) = ebx;
        ebx = M32(ZO_fxbit67 + eax * 4u); V(fxbit67pcal) = ebx;
    }
    ebp++;
}
static void cmode(int top)
{
    u32 eax = M32(esi), ebx, bl;
    ebp++;
    V(SfxPOR) = eax;
    eax = lineloc_for_por();
    V(sfxclineloc) = eax;
    eax = V8(SfxSCMR) & 3;
    bl = (u8)((V8(SfxPOR) & 0x0F) << 2);
    eax |= bl;
    ebx = M32(ZO_PLOTJmpb + eax * 4u); eax = M32(ZO_PLOTJmpa + eax * 4u);
    M32(ZO_FxTable + 0x4Cu * 4) = eax;
    M32(ZO_FxTableb + 0x4Cu * 4) = eax;
    M32(ZO_FxTablec + 0x4Cu * 4) = eax;
    if (top) M32(ZO_FxTable + 0x4Cu * 4) = ebx; else M32(ZO_FxTabled + 0x4Cu * 4) = ebx;
}

/* ---------------- branches (05-0F) ---------------- */
static void branch(u32 op, u32 tbl)
{
    s32 eax = (s8)M8(ebp);
    u32 ebx, sz = V(SfxSignZero);
    int take;
    if (op == 5) {
        ecx = (ecx & ~0xFFu) | M8(ebp + 1); ebp++; ebp += (u32)eax; NEST(tbl); return;
    }
    switch (op) {
    case 6:  ebx = (sz >> 15); ebx = (ebx & ~0xFFu) | ((ebx ^ V8(SfxOverflow)) & 0xFF); take = !(ebx & 1); break;      /* BGE */
    case 7:  ebx = (sz >> 15); ebx = (ebx & ~0xFFu) | ((ebx ^ V8(SfxOverflow)) & 0xFF); take = (ebx & 1) != 0; break;   /* BLT */
    case 8:  take = (sz & 0xFFFF) != 0; break;      /* BNE */
    case 9:  take = (sz & 0xFFFF) == 0; break;      /* BEQ */
    case 10: take = (sz & 0x88000) == 0; break;     /* BPL */
    case 11: take = (sz & 0x88000) != 0; break;     /* BMI */
    case 12: take = !(V8(SfxCarry) & 1); break;     /* BCC */
    case 13: take = (V8(SfxCarry) & 1) != 0; break; /* BCS */
    case 14: take = !(V8(SfxOverflow) & 1); break;  /* BVC */
    default: take = (V8(SfxOverflow) & 1) != 0; break; /* BVS */
    }
    ebp++;
    ecx = (ecx & ~0xFFu) | M8(ebp);
    if (take) ebp += (u32)eax; else ebp++;
    NEST(tbl);
}

/* ---------------- the macro-generated handler families ---------------- */
static void do_macro(unsigned kind, u32 n)
{
    u32 eax, ebx, edx;
    switch (kind) {
    case SK_M_TORN:
        FETCHPIPE(); edi = RA(n); ebp++; NEST(TBL_A); edi = ZO_SfxR0; break;
    case SK_M_WITH:
        FETCHPIPE(); esi = edi = RA(n); V(SfxB) = 1; ebp++; NEST(TBL_C); esi = ZO_SfxR0; edi = ZO_SfxR0; V(SfxB) = 0; break;
    case SK_M_STWRN:
        eax = R32(n); ebx = V(SfxRAMMem); V(SfxLastRamAdr) = eax; V(SfxLastRamAdr) += ebx; edx = M32(esi);
        FETCHPIPE(); M8(ebx + eax) = (u8)edx; eax ^= 1; ebp++; M8(ebx + eax) = (u8)(edx >> 8); break;
    case SK_M_STBRN:
        eax = R32(n); FETCHPIPE(); eax += V(SfxRAMMem); V(SfxLastRamAdr) = eax; ebx = M32(esi); M8(eax) = (u8)ebx; ebp++; break;
    case SK_M_LDWRN:
        eax = R32(n); ebx = V(SfxRAMMem); V(SfxLastRamAdr) = eax; FETCHPIPE();
        edx = M8(ebx + eax); V(SfxLastRamAdr) += ebx; eax ^= 1; edx &= 0xFFFF; ebp++;
        edx = (edx & 0xFF) | ((u32)M8(ebx + eax) << 8); M32(edi) = edx; break;
    case SK_M_LDBRN:
        eax = R32(n); FETCHPIPE(); eax += V(SfxRAMMem); V(SfxLastRamAdr) = eax; ebx = M8(eax); ebp++; M32(edi) = ebx; break;
    case SK_M_ADDRN:  do_add(R32(n), 0); break;
    case SK_M_ADCRN:  FETCHPIPE(); { u32 cf, of; eax = add16(M32(esi), R32(n), V8(SfxCarry) & 1, &cf, &of); SET_OV(of); SET_CY(cf);
                      V(SfxSignZero) = eax; ebp++; M32(edi) = eax; } break;
    case SK_M_ADIRN:  do_add(n, 0); break;
    case SK_M_ADCIRN: FETCHPIPE(); { u32 cf, of; eax = add16(M32(esi), n, V8(SfxCarry) & 1, &cf, &of); SET_OV(of); SET_CY(cf);
                      V(SfxSignZero) = eax; ebp++; M32(edi) = eax; } break;
    case SK_M_SUBRN:  { u32 b = R32(n); FETCHPIPE(); do_sub(b, 0); } break;
    case SK_M_SBCRN:  FETCHPIPE(); do_sub(R32(n), 1); break;
    case SK_M_SUBIRN: FETCHPIPE(); do_sub(n, 0); break;
    case SK_M_CMPRN:  { u32 b = R32(n); FETCHPIPE(); do_sub(b, 2); } break;
    case SK_M_ANDRN:  eax = M32(esi); ebx = R32(n); FETCHPIPE(); eax &= ebx; ebp++; V(SfxSignZero) = eax; M32(edi) = eax; break;
    case SK_M_BICRN:  ebx = R32(n); eax = M32(esi); ebx ^= 0xFFFF; FETCHPIPE(); eax &= ebx; ebp++; V(SfxSignZero) = eax; M32(edi) = eax; break;
    case SK_M_ANDIRN: case SK_M_BICIRN:
        eax = M32(esi); FETCHPIPE(); eax &= n; ebp++; V(SfxSignZero) = eax; M32(edi) = eax; break;
    case SK_M_MULTRN:
        eax = (u32)(s32)(s16)((s8)M8(esi) * (s8)M8(RA(n))); FETCHPIPE(); ebp++; eax &= 0xFFFF; V(SfxSignZero) = eax; M32(edi) = eax; break;
    case SK_M_UMULTRN:
        eax = (u32)((u8)M8(esi) * (u8)M8(RA(n))); FETCHPIPE(); ebp++; eax &= 0xFFFF; V(SfxSignZero) = eax; M32(edi) = eax; break;
    case SK_M_MULTIRN:
        eax = (u32)(s32)(s16)((s8)M8(esi) * (s8)(u8)n); FETCHPIPE(); ebp++; eax &= 0xFFFF; V(SfxSignZero) = eax; M32(edi) = eax; break;
    case SK_M_UMULTIRN:
        eax = (u32)((u8)M8(esi) * (u8)n); FETCHPIPE(); ebp++; eax &= 0xFFFF; V(SfxSignZero) = eax; M32(edi) = eax; break;
    case SK_M_LINK:
        eax = ebp - V(SfxCPB); eax += n; FETCHPIPE(); V16(SfxR11) = (u16)eax; ebp++; break;
    case SK_M_JMPRN:
        FETCHPIPE(); eax = R32(n); ebp = V(SfxCPB) + eax; break;
    case SK_M_LJMPRN: {
        u32 sv;
        FETCHPIPE(); eax = R32(n) & 0x7F; V8(SfxPBR) = (u8)eax; eax = M32(ZO_SfxMemTable + eax * 4u); V(SfxCPB) = eax; ebp = eax;
        ebp += M32(esi); V(SfxCacheActive) = 0;
        sv = ecx;
        {   /* call FxOp02 (CACHE) */
            u32 e = ebp; FETCHPIPE(); e -= V(SfxCPB); e &= 0xFFF0;
            if (V(SfxCBR) != e && V8(SfxCacheActive) != 1) { V(SfxCBR) = e; V(SfxCacheActive) = 1; }
            ebp++;
        }
        ecx = sv; ebp--; break; }
    case SK_M_IBTRN:
        eax = (u32)(s32)(s8)M8(ebp); ecx = (ecx & ~0xFFu) | M8(ebp + 1); ebp += 2; R16(n) = (u16)eax; break;
    case SK_M_LMSRN:
        eax = (u32)M8(ebp) * 2; ebp++; eax += V(SfxRAMMem); ecx = (ecx & ~0xFFu) | M8(ebp); V(SfxLastRamAdr) = eax;
        ebx = M32(eax); ebp++; R16(n) = (u16)ebx; break;
    case SK_M_SMSRN:
        eax = M8(ebp); ebp++; eax += eax; ecx = (ecx & ~0xFFu) | M8(ebp); eax += V(SfxRAMMem); ebx = R32(n);
        V(SfxLastRamAdr) = eax; ebp++; M16(eax) = (u16)ebx; break;
    case SK_M_FROMRN:
        FETCHPIPE(); esi = RA(n); ebp++; NEST(TBL_A); esi = ZO_SfxR0; break;
    case SK_M_ORRN:   eax = M32(esi); ebx = R32(n); FETCHPIPE(); eax |= ebx; ebp++; M32(edi) = eax; V(SfxSignZero) = eax; break;
    case SK_M_XORRN:  eax = M32(esi); ebx = R32(n); FETCHPIPE(); eax ^= ebx; ebp++; M32(edi) = eax; V(SfxSignZero) = eax; break;
    case SK_M_ORI:    eax = M32(esi); FETCHPIPE(); eax |= n; ebp++; M32(edi) = eax; V(SfxSignZero) = eax; break;
    case SK_M_XORI:   eax = M32(esi); FETCHPIPE(); eax ^= n; ebp++; M32(edi) = eax; V(SfxSignZero) = eax; break;
    case SK_M_INCRN:  R16(n) = (u16)(R16(n) + 1); FETCHPIPE(); eax = R32(n); V(SfxSignZero) = eax; ebp++; break;
    case SK_M_DECRN:  R16(n) = (u16)(R16(n) - 1); FETCHPIPE(); eax = R32(n); R32(n) = eax; V(SfxSignZero) = eax; ebp++; break;
    case SK_M_IWTRN:  eax = M32(ebp); ecx = (ecx & ~0xFFu) | M8(ebp + 2); eax &= 0xFFFF; ebp += 3; R32(n) = eax; break;
    case SK_M_LMRN:
        eax = 0; ecx = (ecx & ~0xFFu) | M8(ebp + 2); eax = M16(ebp); ebx = V(SfxRAMMem); V(SfxLastRamAdr) = eax; V(SfxLastRamAdr) += ebx;
        edx = M8(eax + ebx); eax ^= 1; ebp += 3; edx = (edx & ~0xFF00u) | ((u32)M8(eax + ebx) << 8); R16(n) = (u16)edx; break;
    case SK_M_SMRN:
        ebx = R32(n); eax = M32(ebp); ecx = (ecx & ~0xFFu) | M8(ebp + 2); eax &= 0xFFFF; edx = ebx & 0xFFFF; ebx = V(SfxRAMMem);
        V(SfxLastRamAdr) = eax; V(SfxLastRamAdr) += ebx; M8(eax + ebx) = (u8)edx; eax ^= 1; ebp += 3; M8(eax + ebx) = (u8)(edx >> 8); break;
    /* after WITH R15 (FxTableb) */
    case SK_M_TORNb:
        FETCHPIPE();
        if (V(SfxB) & 1) { eax = M32(esi); V(withr15sk) = 1; ebp++; R32(n) = eax; break; }
        edi = RA(n); ebp++; eax = ebp - V(SfxCPB); V(withr15sk) = 1; V(SfxR15) = eax; NEST(TBL_B); edi = ZO_SfxR0; break;
    case SK_M_FROMRNb:
        FETCHPIPE();
        if (V(SfxB) & 1) { eax = R32(n); ebp++; M32(edi) = eax; V(SfxSignZero) = eax; SET_OV((eax & 0xFF) >> 7); break; }
        esi = RA(n); ebp++; NEST(TBL_A); esi = ZO_SfxR0; break;
    /* after WITH (FxTablec): MOVE / MOVES */
    case SK_M_TORNc:  FETCHPIPE(); eax = M32(esi); ebp++; R32(n) = eax; break;
    case SK_M_FROMRNc: FETCHPIPE(); eax = R32(n); ebp++; M32(edi) = eax; V(SfxSignZero) = eax; SET_OV((eax & 0xFF) >> 7); break;
    default: lift_bad(0xBAD0u + kind);
    }
}

/* ---------------- the hand written handlers ---------------- */
static void cache_op(void)
{
    u32 eax = ebp;
    FETCHPIPE();
    eax -= V(SfxCPB); eax &= 0xFFF0;
    if (V(SfxCBR) != eax && V8(SfxCacheActive) != 1) { V(SfxCBR) = eax; V(SfxCacheActive) = 1; }
    ebp++;
}
static int stop_op(int top)
{
    u32 eax;
    FETCHPIPE();
    V8(SfxPIPE) = (u8)ecx;
    V(SfxSFR) &= 0xFFFFu - 32;
    if (!(V(SfxCFGR) & 0x80)) V(SfxSFR) |= 0x8000;
    ebp++;
    eax = V(NumberOfOpcodes); eax += 0xF0000000u; V(ChangeOps) += eax; V(NumberOfOpcodes) = 1;
    if (top) return 2;
    V(SFXProc) = 0; ecx &= ~0xFFu;
    return 0;
}

static int exec(const struct sfxlab *l)
{
    u32 eax, ebx, edx, n = l->arg;
    if (sfx_dbg) fprintf(stderr, "sfx op kind=%u arg=%u ecx=%x pc=%x s=%d d=%d sz=%x cy=%x ov=%x R0=%x R1=%x R2=%x R3=%x R4=%x R5=%x R12=%x R13=%x R14=%x N=%d\n", l->kind, n, ecx, ebp - V(SfxCPB), (esi - ZO_SfxR0) / 4, (edi - ZO_SfxR0) / 4, V(SfxSignZero), V8(SfxCarry), V8(SfxOverflow), R32(0), R32(1), R32(2), R32(3), R32(4), R32(5), R32(12), R32(13), R32(14), (int)V(NumberOfOpcodes));
    switch (l->kind) {
    case SK_PLOT: plot(n); break;
    case SK_K_FxOp00: return stop_op(0);
    case SK_K_FxOpd00: return stop_op(1);
    case SK_K_FxOp01: FETCHPIPE(); ebp++; break;
    case SK_K_FxOp02: cache_op(); break;
    case SK_K_FxOp03:
        eax = M32(esi); FETCHPIPE(); V8(SfxCarry) = (u8)(eax & 1); eax = (eax & ~0xFFFFu) | ((eax & 0xFFFF) >> 1); ebp++;
        M32(edi) = eax; V(SfxSignZero) = eax; break;
    case SK_K_FxOp04: {
        u32 c0 = V8(SfxCarry), cf1 = c0 & 1, c1 = c0 >> 1, cf2;
        eax = M32(esi); FETCHPIPE();
        cf2 = (eax >> 15) & 1; eax = (eax & ~0xFFFFu) | (((eax << 1) | cf1) & 0xFFFF);
        V8(SfxCarry) = (u8)((c1 << 1) | cf2); ebp++; M32(edi) = eax; V(SfxSignZero) = eax; break; }
    case SK_K_FxOp05: case SK_K_FxOp06: case SK_K_FxOp07: case SK_K_FxOp08: case SK_K_FxOp09: case SK_K_FxOp0A:
    case SK_K_FxOp0B: case SK_K_FxOp0C: case SK_K_FxOp0D: case SK_K_FxOp0E: case SK_K_FxOp0F:
        branch(l->kind - SK_K_FxOp05 + 5, tbl_of[l->tbl]); return l->top ? 1 : 0;
    case SK_K_BR: branch(n, tbl_of[l->tbl]); break;
    case SK_K_FxOp1E:
        FETCHPIPE(); edi = RA(14); ebp++; NEST(TBL_A); edi = ZO_SfxR0; UPDATE_R14(); break;
    case SK_K_FxOp1F:
        FETCHPIPE(); edi = RA(15); ebp++; NEST(TBL_A); ebp = V(SfxCPB) + V(SfxR15); edi = ZO_SfxR0; break;
    case SK_K_FxOp2E:
        FETCHPIPE(); esi = edi = RA(14); V(SfxB) = 1; ebp++; NEST(TBL_C); V(SfxB) = 0; esi = ZO_SfxR0; edi = ZO_SfxR0; UPDATE_R14(); break;
    case SK_K_FxOp2F:
        FETCHPIPE(); esi = edi = RA(15); V(SfxB) = 1; ebp++; eax = ebp - V(SfxCPB); V(withr15sk) = 0; V(SfxR15) = eax;
        NEST(TBL_B);
        if (V(withr15sk) != 1) ebp = V(SfxCPB) + V(SfxR15);
        V(SfxB) = 0; esi = ZO_SfxR0; edi = ZO_SfxR0; break;
    case SK_K_FxOp3C:
        V16(SfxR12) = (u16)(V16(SfxR12) - 1); FETCHPIPE(); eax = V(SfxR12); V(SfxSignZero) = eax;
        if (eax == 0) ebp++; else ebp = V(SfxCPB) + V(SfxR13);
        break;
    case SK_K_FxOp3D: case SK_K_FxOp3E: case SK_K_FxOp3F: {
        u32 alt = l->kind - SK_K_FxOp3D + 1;
        FETCHPIPE(); V(SfxB) = 0; ecx |= alt << 8; ebp++; NEST(TBL_A); ecx &= ~0xFF00u; break; }
    case SK_K_FxOpb3D: case SK_K_FxOpb3E: case SK_K_FxOpb3F: {
        u32 alt = l->kind - SK_K_FxOpb3D + 1;
        FETCHPIPE(); V(SfxB) = 0; ecx |= alt << 8; ebp++; eax = ebp - V(SfxCPB); V(SfxR15) = eax;
        NEST(l->kind == SK_K_FxOpb3D ? TBL_B : TBL_A); ecx &= ~0xFF00u; break; }
    case SK_K_FxOpc3D: case SK_K_FxOpc3E: case SK_K_FxOpc3F: {
        u32 alt = l->kind - SK_K_FxOpc3D + 1;
        FETCHPIPE(); V(SfxB) = 0; ecx |= alt << 8; ebp++; NEST(TBL_C); ecx &= ~0xFF00u; break; }
    case SK_K_FxOp4CA1: rpix(); break;
    case SK_K_FxOp4D:
        eax = M32(esi); FETCHPIPE(); eax = (eax & ~0xFFFFu) | ((eax >> 8) & 0xFF) | ((eax & 0xFF) << 8); ebp++;
        V(SfxSignZero) = eax; M32(edi) = eax; break;
    case SK_K_FxOp4E: FETCHPIPE(); eax = M32(esi); set_color(eax); break;
    case SK_K_FxOp4EA1: FETCHPIPE(); cmode(0); break;
    case SK_K_FxOpd4EA1: FETCHPIPE(); cmode(1); break;
    case SK_K_FxOp4F: eax = M32(esi); FETCHPIPE(); eax ^= 0xFFFF; ebp++; V(SfxSignZero) = eax; M32(edi) = eax; break;
    case SK_K_FxOp5F: FETCHPIPE(); do_add(ebp - V(SfxCPB), 0); break;
    case SK_K_FxOp5FA1: {
        u32 cf, of; FETCHPIPE(); eax = M32(esi); ebx = ebp - V(SfxCPB);
        eax = add16(eax, ebx, V8(SfxCarry) & 1, &cf, &of); SET_OV(of); SET_CY(cf); V(SfxSignZero) = eax; ebp++; M32(edi) = eax; break; }
    case SK_K_FxOp6F: FETCHPIPE(); do_sub(ebp - V(SfxCPB), 0); break;
    case SK_K_FxOp6FA1: { u32 b = ebp - V(SfxCPB); FETCHPIPE(); do_sub(b, 1); break; }
    case SK_K_FxOp6FA3: FETCHPIPE(); do_sub(ebp - V(SfxCPB), 2); break;
    case SK_K_FxOp70:
        FETCHPIPE(); eax = ((V8(SfxR7 + 1) << 8) & 0xFF00) | (V8(SfxR8 + 1)); ebp++; M32(edi) = eax;
        V(SfxSignZero) = (eax & 0xF0F0) ? 0 : 1;
        if (eax & 0x8080) V(SfxSignZero) |= 0x80000;
        V(SfxOverflow) = (eax & 0xC0C0) ? 1 : 0;
        V(SfxCarry) = (eax & 0xE0E0) ? 1 : 0; break;
    case SK_K_FxOp7F: FETCHPIPE(); eax = M32(esi); ebx = ebp - V(SfxCPB); eax &= ebx; ebp++; V(SfxSignZero) = eax; M32(edi) = eax; break;
    case SK_K_FxOp7FA1: FETCHPIPE(); ebx = ebp - V(SfxCPB); eax = M32(esi); ebx ^= 0xFFFF; eax &= ebx; ebp++; V(SfxSignZero) = eax; M32(edi) = eax; break;
    case SK_K_FxOp8F:
        FETCHPIPE(); ebx = ebp; eax = M8(esi); ebx -= V(SfxCPB);
        eax = (u32)(s32)(s16)((s8)eax * (s8)ebx); ebp++; eax &= 0xFFFF; V(SfxSignZero) = eax; M32(edi) = eax; break;
    case SK_K_FxOp8FA1:
        FETCHPIPE(); ebx = ebp; eax = M8(esi); ebx -= V(SfxCPB);
        eax = (u32)((u8)eax * (u8)ebx); ebp++; eax &= 0xFFFF; V(SfxSignZero) = eax; M32(edi) = eax; break;
    case SK_K_FxOp90:
        ebx = V(SfxLastRamAdr); eax = M32(esi); FETCHPIPE(); M8(ebx) = (u8)eax; ebx -= V(SfxRAMMem); ebx ^= 1; ebx += V(SfxRAMMem);
        ebp++; M8(ebx) = (u8)(eax >> 8); break;
    case SK_K_FxOp95:
        eax = (u32)(s32)(s8)M8(esi); FETCHPIPE(); eax &= 0xFFFF; ebp++; M32(edi) = eax; V(SfxSignZero) = eax; break;
    case SK_K_FxOp96:
        eax = M32(esi); FETCHPIPE(); V8(SfxCarry) = (u8)(eax & 1);
        eax = (eax & ~0xFFFFu) | (u32)(u16)((s16)(eax & 0xFFFF) >> 1); ebp++; M32(edi) = eax; V(SfxSignZero) = eax; break;
    case SK_K_FxOp96A1:
        eax = M32(esi); FETCHPIPE();
        if ((eax & 0xFFFF) == 0xFFFF) { V8(SfxCarry) = 1; eax = 0; ebp++; M32(edi) = eax; V(SfxSignZero) = eax; break; }
        V8(SfxCarry) = (u8)(eax & 1); eax = (eax & ~0xFFFFu) | (u32)(u16)((s16)(eax & 0xFFFF) >> 1); ebp++; M32(edi) = eax; V(SfxSignZero) = eax; break;
    case SK_K_FxOp97: {
        u32 cf1, cf2;
        FETCHPIPE(); eax = M32(esi); cf1 = V8(SfxCarry) & 1; V8(SfxCarry) >>= 1;
        cf2 = eax & 1; eax = (eax & ~0xFFFFu) | ((eax & 0xFFFF) >> 1) | (cf1 << 15); V8(SfxCarry) = (u8)cf2;
        ebp++; M32(edi) = eax; V(SfxSignZero) = eax; break; }
    case SK_K_FxOp9E:
        eax = M32(esi); FETCHPIPE(); eax &= 0xFF; ebp++; M32(edi) = eax; eax <<= 8; V(SfxSignZero) = eax; break;
    case SK_K_FxOp9F: {
        s32 p; eax = M32(esi); ebx = V(SfxR6); FETCHPIPE();
        p = (s32)(s16)eax * (s32)(s16)ebx; ebp++; edx = ((u32)p >> 16) & 0xFFFF; eax = (u32)p & 0xFFFF;
        V(SfxSignZero) = edx; M32(edi) = edx; V8(SfxCarry) = (u8)((eax >> 15) & 1); break; }
    case SK_K_FxOp9FA1: {
        s32 p; eax = M32(esi); ebx = V(SfxR6); FETCHPIPE();
        p = (s32)(s16)eax * (s32)(s16)ebx; edx = ((u32)p >> 16) & 0xFFFF; eax = (u32)p & 0xFFFF; ebp++;
        M32(edi) = edx; V16(SfxR4) = (u16)eax; V(SfxSignZero) = edx; V8(SfxCarry) = (u8)((eax >> 15) & 1); break; }
    case SK_K_FxOpAE:
        eax = (u32)(s32)(s8)M8(ebp); ecx = (ecx & ~0xFFu) | M8(ebp + 1); ebp += 2; R16(14) = (u16)eax; UPDATE_R14(); break;
    case SK_K_FxOpAF:
        eax = (u32)(s32)(s8)M8(ebp); ecx = (ecx & ~0xFFu) | M8(ebp + 1); eax &= 0xFFFF; ebp = V(SfxCPB) + eax; break;
    case SK_K_FxOpAEA1:
        eax = M8(ebp) * 2u; ebp++; eax += V(SfxRAMMem); ecx = (ecx & ~0xFFu) | M8(ebp); V(SfxLastRamAdr) = eax;
        ebx = M32(eax); ebp++; R16(14) = (u16)ebx; UPDATE_R14(); break;
    case SK_K_FxOpAFA1:
        eax = M8(ebp) * 2u; ebp++; eax += V(SfxRAMMem); ecx = (ecx & ~0xFFu) | M8(ebp); V(SfxLastRamAdr) = eax;
        ebx = M32(eax); ebx &= 0xFFFF; ebp = V(SfxCPB) + ebx; break;
    case SK_K_FxOpAFA2:
        eax = M8(ebp); ebx = ebp - V(SfxCPB); ebp++; eax += eax; FETCHPIPE(); eax += V(SfxRAMMem); V(SfxLastRamAdr) = eax; ebp++; M16(eax) = (u16)ebx; break;
    case SK_K_FxOpBF:
        FETCHPIPE(); esi = RA(15); ebp++; eax = ebp - V(SfxCPB); V(SfxR15) = eax; NEST(TBL_B); esi = ZO_SfxR0; break;
    case SK_K_FxOpC0:
        eax = M32(esi); FETCHPIPE(); eax &= 0xFF00; V(SfxSignZero) = eax; eax >>= 8; ebp++; M32(edi) = eax; break;
    case SK_K_FxOpCF: eax = M32(esi); ebx = ebp; FETCHPIPE(); ebx -= V(SfxCPB); eax |= ebx; ebp++; M32(edi) = eax; V(SfxSignZero) = eax; break;
    case SK_K_FxOpCFA1: FETCHPIPE(); eax = M32(esi); ebx = ebp - V(SfxCPB); eax ^= ebx; ebp++; M32(edi) = eax; V(SfxSignZero) = eax; break;
    case SK_K_FxOpDE:
        FETCHPIPE(); eax = R32(14); eax = (eax & ~0xFFFFu) | ((eax + 1) & 0xFFFF); R32(14) = eax; V(SfxSignZero) = eax; ebp++; UPDATE_R14(); break;
    case SK_K_FxOpDF: eax = V(SfxRomBuffer); FETCHPIPE(); eax = M32(eax); set_color(eax); break;
    case SK_K_FxOpDFA2:
        eax = M32(esi); ebx = V(SfxnRamBanks); FETCHPIPE(); ebx--; eax &= ebx; V(SfxRAMBR) = eax; eax <<= 16; eax += V(sfxramdata);
        V(SfxRAMMem) = eax; ebp++; break;
    case SK_K_FxOpDFA3:
        eax = M32(esi) & 0x7F; FETCHPIPE(); V(SfxROMBR) = eax; eax = M32(ZO_SfxMemTable + eax * 4u); V(SfxCROM) = eax; ebp++; break;
    case SK_K_FxOpEE:
        R16(14) = (u16)(R16(14) - 1); FETCHPIPE(); eax = R32(14); V(SfxSignZero) = eax; UPDATE_R14(); ebp++; break;
    case SK_K_FxOpEF: FETCHPIPE(); eax = V(SfxRomBuffer); ebp++; eax = M32(eax); eax &= 0xFF; M32(edi) = eax; break;
    case SK_K_FxOpEFA1:
        eax = M32(esi); ebx = V(SfxRomBuffer); eax &= 0xFF; FETCHPIPE(); eax = (eax & ~0xFF00u) | ((u32)M8(ebx) << 8); ebp++; M32(edi) = eax; break;
    case SK_K_FxOpEFA2:
        eax = M32(esi); ebx = V(SfxRomBuffer); eax &= 0xFF00; FETCHPIPE(); eax = (eax & ~0xFFu) | M8(ebx); ebp++; M32(edi) = eax; break;
    case SK_K_FxOpEFA3:
        ebx = V(SfxRomBuffer); FETCHPIPE(); eax = (u32)(s32)(s8)M8(ebx); ebp++; M16(edi) = (u16)eax; break;
    case SK_K_FxOpFE:
        eax = M32(ebp); ecx = (ecx & ~0xFFu) | M8(ebp + 2); eax &= 0xFFFF; ebp += 3; R32(14) = eax; UPDATE_R14(); break;
    case SK_K_FxOpFF:
        eax = M32(ebp); ecx = (ecx & ~0xFFu) | M8(ebp + 2); eax &= 0xFFFF; ebp = V(SfxCPB) + eax; break;
    case SK_K_FxOpFEA1:
        eax = 0; ecx = (ecx & ~0xFFu) | M8(ebp + 2); eax = M16(ebp); ebx = V(SfxRAMMem); V(SfxLastRamAdr) = eax; V(SfxLastRamAdr) += ebx;
        edx = M8(eax + ebx); eax ^= 1; ebp += 3; edx = (edx & ~0xFF00u) | ((u32)M8(eax + ebx) << 8); R16(14) = (u16)edx; UPDATE_R14(); break;
    case SK_K_FxOpFFA1:
        FETCHPIPE(); eax = ecx; ebp++; FETCHPIPE(); ebp++; eax = (eax & ~0xFF00u) | ((ecx & 0xFF) << 8); FETCHPIPE();
        ebx = V(SfxRAMMem); V(SfxLastRamAdr) = eax; V(SfxLastRamAdr) += ebx; edx = M8(eax + ebx); eax ^= 1;
        edx = (edx & ~0xFF00u) | ((u32)M8(eax + ebx) << 8); edx &= 0xFFFF; ebp = V(SfxCPB) + edx; break;
    case SK_K_FxOpFFA2:
        FETCHPIPE(); ebx = ebp - V(SfxCPB); eax = ecx; ebp++; FETCHPIPE(); ebp++; eax = (eax & ~0xFF00u) | ((ecx & 0xFF) << 8); FETCHPIPE();
        edx = ebx & 0xFFFF; ebx = V(SfxRAMMem); V(SfxLastRamAdr) = eax; V(SfxLastRamAdr) += ebx; M8(eax + ebx) = (u8)edx; eax ^= 1; ebp++;
        M8(eax + ebx) = (u8)(edx >> 8); break;
    /* after WITH R15 / WITH (b and c families) */
    case SK_K_FxOpb1E:
        FETCHPIPE();
        if (V(SfxB) & 1) { eax = M32(esi); V(withr15sk) = 1; R32(14) = eax; UPDATE_R14(); ebp++; break; }
        edi = RA(14); ebp++; eax = ebp - V(SfxCPB); V(withr15sk) = 1; V(SfxR15) = eax; NEST(TBL_B); edi = ZO_SfxR0; UPDATE_R14(); break;
    case SK_K_FxOpb1F:
        FETCHPIPE();
        if (V(SfxB) & 1) { eax = M32(esi); ebp = V(SfxCPB); V(withr15sk) = 1; ebp += eax; break; }
        edi = RA(15); ebp++; eax = ebp - V(SfxCPB); V(SfxR15) = eax; NEST(TBL_B); ebp = V(SfxCPB); V(withr15sk) = 1; ebp += V(SfxR15); edi = ZO_SfxR0; break;
    case SK_K_FxOpbBF:
        if (V(SfxB) & 1) { FETCHPIPE(); eax = ebp - V(SfxCPB); ebp++; M32(edi) = eax; V(SfxSignZero) = eax; SET_OV((eax & 0xFF) >> 7); break; }
        esi = RA(15); ebp++; eax = ebp - V(SfxCPB); V(SfxR15) = eax; NEST(TBL_B); esi = ZO_SfxR0; break;
    case SK_K_FxOpc1E:
        FETCHPIPE(); eax = M32(esi); R32(14) = eax; UPDATE_R14(); ebp++; break;
    case SK_K_FxOpc1F:
        FETCHPIPE(); eax = M32(esi); ebp = V(SfxCPB); V(SfxR15) = eax; ebp += eax; break;
    case SK_K_FxOpcBF:
        FETCHPIPE(); eax = ebp - V(SfxCPB); ebp++; M32(edi) = eax; V(SfxSignZero) = eax; SET_OV((eax & 0xFF) >> 7); break;
    default:
        do_macro(l->kind, n); break;
    }
    return 0;
}

static int run(u32 id)
{
    const struct sfxlab *l = lookup(id);
    if (!l) lift_bad(id);
    return exec(l);
}

/* MainLoop: set up the registers from the SFX state, run the opcodes, and store the state back (FXEndLoop) */
void zl_sfx_mainloop(void)
{
    u32 eax;
    ebp = V(SfxCPB) + V(SfxR15);
    ecx = V8(SfxPIPE);
    ecx |= (u32)(V8(SfxSFR + 1) & 3) << 8;
    esi = ZO_SfxR0 + V(SfxSREG) * 4u;
    edi = ZO_SfxR0 + V(SfxDREG) * 4u;
    V(SfxRAMMem) = (V(SfxRAMBR) << 16) + V(sfxramdata);
    for (;;) {
        const struct sfxlab *l = lookup(M32(TBL_D + ecx * 4u));
        int r;
        if (!l) lift_bad(M32(TBL_D + ecx * 4u));
        r = exec(l);
        if (r == 2) break;                    /* d00 (STOP) jumped to FXEndLoop */
        V(NumberOfOpcodes)--;
        if (r == 1 && (s32)V(NumberOfOpcodes) < 0) break;
    }
    ebp -= V(SfxCPB);
    V(SfxR15) = ebp;
    V8(SfxPIPE) = (u8)ecx;
    V8(SfxSFR + 1) &= 0xFF - 3;
    V8(SfxSFR + 1) |= (u8)(ecx >> 8);
    eax = (esi - ZO_SfxR0) >> 2; V(SfxSREG) = eax;
    eax = (edi - ZO_SfxR0) >> 2; V(SfxDREG) = eax;
    LG.eax = eax; LG.ecx = ecx; LG.ebp = ebp; LG.esi = esi; LG.edi = edi;
}
