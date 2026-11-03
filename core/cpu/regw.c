/*
 * 65816-bus register write handlers ($21xx PPU, $2140 APU ports, $2180 WRAM, $4016/$42xx/$43xx)
 * ported from regsw.inc.  Each handler is selected by id (regw_ids.h); the asm table entries in
 * regptw[] are replaced by stubs (zs_rwstub_N in regsw.inc) that save the machine registers into
 * zs_R, call zs_regw_c(id) and reload them.  Input: R.eax(al)=value, R.ecx=address, R.edx(dh)=cycles.
 *
 *   ZS_REGW_ASM=1   keep the original asm handlers
 *   ZS_REGW_DIFF=1  run asm and C on identical full-image state per write (sampled), compare
 *   ZS_REGW_BREAK=1 negative control (corrupt mosaicsz in C)
 */
#define _GNU_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <link.h>
#include <dlfcn.h>
#include <signal.h>
#include <setjmp.h>
#include "regw_ids.h"
#include "regr_ids.h"

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef int8_t s_t;
typedef int32_t s32;
typedef int16_t s16;
#define HID __attribute__((visibility("hidden")))

typedef struct { u32 eax, ecx, edx, ebx, esi, edi, ebp; } regs_t;
extern regs_t zs_R;
#define R zs_R

/* ---- asm state ---- */
extern u8 vidbright, forceblnk, objsize1, objsize2, objmovs1, objmovs2, objhipr, bgmode, bg3highst, bgtilesz;
extern u8 mosaicon, mosaicsz, vramincr, vramread, vramread2, cgmod, multa, sndwrit, NextLineCache, prevoamptr;
extern u8 oamlow, nexthprior, nosprincr, vraminctype, vramincby8on, vramincby8left, vramincby8totl, debstop;
extern u8 BG116x16t, mode7set, multchange, winbg1en, winbg2en, winbg3en, winbg4en, winobjen, wincolen;
extern u8 winl1, winr1, winl2, winr2, winlogica, winlogicb, winenabm, winenabs, scaddset, scaddtype;
extern u8 coladdr, coladdg, coladdb, interlval, hdmarestart, JoyCRead, iohvlatch, ioportval, MultiTapStat;
extern u8 INTEnab, HIRQSkip, HIRQNextExe, HIRQCycNext, xirqb, cycpl, cycphb, bgscroltemp;
extern u8 opexec268, opexec268cph, opexec358, opexec358cph, cycpb268, cycpb358;
extern u8 oamram[], cgram[], dmadata[], spcRam[];
extern u16 objadds1, objadds2, oamaddrs, poamaddrs, addrincr, cgaddr, scrnon, resolutn, diva, divres, multres;
extern u16 vramincby8ptri, vramincby8var, mode7A, mode7B, mode7C, mode7D, mode7X0, mode7Y0;
extern u16 HIRQLoc, VIRQLoc, curypos, totlines;
extern u32 objptr, objptrn, oamaddr, vramaddr, wramrwadr, wramdata, nmirept, nmistatus, SPC700write;
extern u32 cycpbl, cycpblt, curexecstate;
extern u32 JoyAOrig, JoyBOrig, JoyCOrig, JoyDOrig, JoyEOrig, JoyANow, JoyBNow, JoyCNow, JoyDNow, JoyENow;
extern u8 *vram;
extern u8 vrama[], vidmemch2[], vidmemch4[], vidmemch8[];
extern u32 *regptw;
extern u32 tableadc[256];
extern void *zs_rwstub_tab[RW_COUNT];
extern void *zs_rwasm_tab[RW_COUNT];

typedef struct {
    u16 *ptr, *ptrb, *ptrc, *ptrd; u32 *ptrx, *ptry; u8 *scsize; u16 *objptr, *scrolx, *scroly;
} bg_t;
extern u16 bg1ptr, bg2ptr, bg3ptr, bg4ptr, bg1ptrb, bg2ptrb, bg3ptrb, bg4ptrb, bg1ptrc, bg2ptrc, bg3ptrc, bg4ptrc;
extern u16 bg1ptrd, bg2ptrd, bg3ptrd, bg4ptrd, bg1objptr, bg2objptr, bg3objptr, bg4objptr;
extern u16 bg1scrolx, bg2scrolx, bg3scrolx, bg4scrolx, bg1scroly, bg2scroly, bg3scroly, bg4scroly;
extern u32 bg1ptrx, bg2ptrx, bg3ptrx, bg4ptrx, bg1ptry, bg2ptry, bg3ptry, bg4ptry;
extern u8 bg1scsize, bg2scsize, bg3scsize, bg4scsize;
static const bg_t BG[4] = {
    { &bg1ptr, &bg1ptrb, &bg1ptrc, &bg1ptrd, &bg1ptrx, &bg1ptry, &bg1scsize, &bg1objptr, &bg1scrolx, &bg1scroly },
    { &bg2ptr, &bg2ptrb, &bg2ptrc, &bg2ptrd, &bg2ptrx, &bg2ptry, &bg2scsize, &bg2objptr, &bg2scrolx, &bg2scroly },
    { &bg3ptr, &bg3ptrb, &bg3ptrc, &bg3ptrd, &bg3ptrx, &bg3ptry, &bg3scsize, &bg3objptr, &bg3scrolx, &bg3scroly },
    { &bg4ptr, &bg4ptrb, &bg4ptrc, &bg4ptrd, &bg4ptrx, &bg4ptry, &bg4scsize, &bg4objptr, &bg4scrolx, &bg4scroly },
};
extern u16 mode7A;

/* mode7 / scroll registers are written as little-endian byte pairs */
#define LO(p) (((u8 *)(p))[0])
#define HI(p) (((u8 *)(p))[1])

static int brk_mode;

static void vmemch(u32 idx)
{
    idx >>= 4;
    vidmemch2[idx] = 1; vidmemch4[idx] = 1; vidmemch8[idx] = 1;
}

static u32 inc8_addr(void)
{
    u32 ebx = (vramaddr & (u32)vramincby8left) << 3;
    u32 eax = vramaddr;
    eax = (eax & 0xFFFF0000u) | ((eax & vramincby8var) & 0xFFFF);
    eax >>= (vramincby8totl & 31);
    ebx += eax;
    eax = vramaddr;
    eax = (eax & 0xFFFF0000u) | ((eax & vramincby8ptri) & 0xFFFF);
    ebx += eax;
    return ebx;
}

static void bgreg(int n, u32 al)
{
    const bg_t *b = &BG[n];
    u16 bx = (u16)((u8)(al >> 2) << 11);
    u8 sc = al & 3;
    *b->ptr = *b->ptrb = *b->ptrc = *b->ptrd = bx;
    *b->ptrx = 0; *b->ptry = 0;
    *b->scsize = sc;
    if (sc == 1) { *b->ptrb += 0x800; *b->ptrd += 0x800; *b->ptrx = 0x800; }
    if (sc == 2) { *b->ptrc += 0x800; *b->ptrd += 0x800; *b->ptry = 0x800; }
    if (sc == 3) { *b->ptrb += 0x800; *b->ptrc += 0x1000; *b->ptrd += 0x1800; *b->ptrx = 0x800; *b->ptry = 0x1000; }
}

static void scrx(int n, u32 al) { u16 *p = BG[n].scrolx; LO(p) = HI(p); HI(p) = (u8)al; }
static void scry(int n, u32 al) { u16 *p = BG[n].scroly; LO(p) = (u8)(HI(p) + bgscroltemp); HI(p) = (u8)al; }

static void set16x16(u32 al)
{
    u8 *p = (u8 *)&BG116x16t;
    __asm__("" : "+r"(p));
    memset(p, 0, 4);
    p[3] += (al >> 7) & 1;      /* BG4 */
    p[2] += (al >> 6) & 1;      /* BG3 */
    p[1] += (al >> 5) & 1;      /* BG2 */
    p[0] += (al >> 4) & 1;      /* BG1 */
}

static void install_vram_handlers(int idx2118, int idx2119)
{
    regptw[0x2118] = (u32)zs_rwstub_tab[idx2118];
    regptw[0x2119] = (u32)zs_rwstub_tab[idx2119];
}

static void dh_add(u32 v) { R.edx = (R.edx & 0xFFFF00FFu) | ((((R.edx >> 8) + v) & 0xFF) << 8); }
static u32 dh_get(void) { return (R.edx >> 8) & 0xFF; }
static void dh_set(u32 v) { R.edx = (R.edx & 0xFFFF00FFu) | ((v & 0xFF) << 8); }

static void reenablespc(void)
{
    if (cycpbl < 0x1000000u) return;
    cycpbl = 0;
    if (curexecstate & 2) return;
    curexecstate |= 2;
    R.edi = tableadc[R.edx & 0xFF];
}

static void determine_hirq(void)
{
    u32 cl, al, dh;
    if (HIRQSkip == 1) return;
    dh_add(HIRQCycNext);
    HIRQCycNext = 0;
    HIRQNextExe = 0;
    al = ((u32)HIRQLoc * cycpl) & 0xFFFF;          /* mul cx -> dx:ax; only ax used by div's low word */
    {
        u32 prod = (u32)HIRQLoc * cycpl;
        u32 q = prod / 340u;                       /* div cx: dx:ax / 340 */
        al = q & 0xFFFF;
    }
    cl = (u8)(cycpl - (u8)al);
    dh = dh_get();
    if (dh > cl) {
        dh_set(dh - cl);
        dh_add(30);
        HIRQCycNext = (u8)cl;
        HIRQNextExe = 1;
    }
}

static void joy_reset_now(void)
{
    JoyANow = JoyAOrig | 0xFFFF; JoyBNow = JoyBOrig | 0xFFFF; JoyCNow = JoyCOrig | 0xFFFF;
    JoyDNow = JoyDOrig | 0xFFFF; JoyENow = JoyEOrig | 0xFFFF;
}

static void virq_cancel(void)
{
    if (HIRQNextExe != 1) return;
    if (curypos == VIRQLoc) return;
    dh_add(HIRQCycNext);
    HIRQCycNext = 0;
    HIRQNextExe = 0;
}

static void reg2115(u32 al)
{
    u32 bl;
    vraminctype = (u8)al;
    switch (al & 3) {
    case 0: addrincr = 2; break;
    case 1: addrincr = 64; break;
    default: addrincr = 256; break;
    }
    vramincby8on = 0;
    bl = al & 0x0C;
    if (bl) {
        vramincby8on = 1;
        if (bl == 4) { vramincby8left = 63; vramincby8totl = 5; vramincby8ptri = 65535 - 511; vramincby8var = 256 + 128 + 64; }
        if (bl == 8) { vramincby8left = 127; vramincby8totl = 6; vramincby8ptri = 65535 - 1023; vramincby8var = 512 + 256 + 128; }
        if (bl == 12) { vramincby8left = 255; vramincby8totl = 7; vramincby8ptri = 65535 - 2047; vramincby8var = 1024 + 512 + 256; }
        if (al & 0x80) { install_vram_handlers(RW_V2118inc8, RW_V2119inc8inc); vramincr = 0; }
        else { install_vram_handlers(RW_V2118inc8inc, RW_V2119inc8); vramincr = 1; }
    } else {
        if (al & 0x80) { install_vram_handlers(RW_V2118, RW_V2119inc); vramincr = 0; }
        else { install_vram_handlers(RW_V2118inc, RW_V2119); vramincr = 1; }
    }
}

static const u8 t_objsize1[8] = { 1, 1, 1, 4, 4, 16, 1, 1 };
static const u8 t_objsize2[8] = { 4, 16, 64, 16, 64, 64, 4, 4 };
static const u8 t_objmovs1[8] = { 2, 2, 2, 2, 2, 4, 2, 2 };
static const u8 t_objmovs2[8] = { 2, 4, 8, 4, 8, 8, 2, 2 };
static const u16 t_objadds1[8] = { 14, 14, 14, 14, 14, 12, 14, 14 };
static const u16 t_objadds2[8] = { 14, 12, 8, 12, 8, 8, 14, 14 };

static void write_c(u32 id)
{
    u32 al = R.eax & 0xFF;
    u32 ecx = R.ecx;
    u32 i, w;
    switch (id) {
    case RW_R2100: vidbright = al & 0x0F; forceblnk = al & 0x80; break;
    case RW_R2101:
        if (prevoamptr == 0xFF || prevoamptr != al) {
            u16 bx;
            prevoamptr = al;
            bx = (u16)((al & 3) << 14);
            *(u16 *)&objptr = bx; *(u16 *)&objptrn = bx;
            bx = (u16)((((al & 0x18) >> 3)) << 13);
            *(u16 *)&objptrn = (u16)(*(u16 *)&objptrn + bx);
            i = al >> 5;
            NextLineCache = 1;
            objsize1 = t_objsize1[i]; objsize2 = t_objsize2[i];
            objmovs1 = t_objmovs1[i]; objmovs2 = t_objmovs2[i];
            objadds1 = t_objadds1[i]; objadds2 = t_objadds2[i];
        }
        break;
    case RW_R2102:
        oamlow = 1;
        w = (u16)(al << 1);
        *(u16 *)&oamaddr = (u16)w;
        poamaddrs = oamaddrs;
        oamaddrs = (u16)oamaddr;
        if (nexthprior == 1) objhipr = (u8)(((u16)oamaddr >> 2) & 0x7F);
        else objhipr = 0;
        break;
    case RW_R2103:
        if (oamlow == 1) {
            oamlow = 0;
            w = (u16)oamaddr >> 1;
            w = (w & 0xFF) | ((al & 1) << 8);
            *(u16 *)&oamaddr = (u16)(w << 1);
        }
        if (poamaddrs > 0x200 && (u16)oamaddr == 0x200) {
            *(u16 *)&oamaddr = poamaddrs;
            nosprincr = 1;
        }
        oamaddrs = (u16)oamaddr;
        nexthprior = (al & 0x80) ? 1 : 0;
        NextLineCache = 1;
        break;
    case RW_R2104: {
        u32 ebx = oamaddr;
        NextLineCache = 1;
        if (nosprincr != 1) {
            oamaddr++;
            if (ebx >= 544) { oamaddr = 1; oamram[0] = (u8)al; break; }
        }
        oamram[ebx] = (u8)al;
        break;
    }
    case RW_R2105:
        bgmode = al & 7; bg3highst = (al >> 3) & 1; bgtilesz = (u8)(al >> 4);
        set16x16(al);
        break;
    case RW_R2106: mosaicon = al & 0x0F; mosaicsz = (u8)(al >> 4); if (brk_mode == 1) mosaicsz ^= 1; break;
    case RW_R2107: bgreg(0, al); break;
    case RW_R2108: bgreg(1, al); break;
    case RW_R2109: bgreg(2, al); break;
    case RW_R210A: bgreg(3, al); break;
    case RW_R210B: bg1objptr = (u16)((al & 0x0F) << 13); bg2objptr = (u16)((al >> 4) << 13); break;
    case RW_R210C: bg3objptr = (u16)((al & 0x0F) << 13); bg4objptr = (u16)((al >> 4) << 13); break;
    case RW_R210D: scrx(0, al); break;
    case RW_R210E: scry(0, al); break;
    case RW_R210F: scrx(1, al); break;
    case RW_R2110: scry(1, al); break;
    case RW_R2111: scrx(2, al); break;
    case RW_R2112: scry(2, al); break;
    case RW_R2113: scrx(3, al); break;
    case RW_R2114: scry(3, al); break;
    case RW_R2115: reg2115(al); break;
    case RW_R2116:
        w = (u16)vramaddr >> 1;
        w = (w & 0xFF00) | al;
        *(u16 *)&vramaddr = (u16)(w << 1);
        vramread = 0;
        break;
    case RW_R2117:
        w = (u16)vramaddr >> 1;
        w = (w & 0x00FF) | (al << 8);
        *(u16 *)&vramaddr = (u16)(w << 1);
        vramread = 0;
        vramread = vram[(u16)vramaddr];
        vramread2 = vram[(u16)vramaddr + 1u];
        break;
    case RW_V2118: vrama[vramaddr] = (u8)al; vmemch(vramaddr); break;
    case RW_V2118inc: vrama[vramaddr] = (u8)al; vmemch(vramaddr); *(u16 *)&vramaddr = (u16)(vramaddr + addrincr); break;
    case RW_V2118inc8: { u32 e = inc8_addr(); vram[e] = (u8)al; vmemch(e); break; }
    case RW_V2118inc8inc: { u32 e = inc8_addr(); vram[e] = (u8)al; vmemch(e); *(u16 *)&vramaddr = (u16)(vramaddr + addrincr); break; }
    case RW_V2119:
        if (vramaddr >= 0xE000u) debstop = 1;
        vrama[vramaddr + 1] = (u8)al; vmemch(vramaddr);
        break;
    case RW_V2119inc: vrama[vramaddr + 1] = (u8)al; vmemch(vramaddr); *(u16 *)&vramaddr = (u16)(vramaddr + addrincr); break;
    case RW_V2119inc8: { u32 e = inc8_addr(); vrama[e + 1] = (u8)al; vmemch(e); break; }
    case RW_V2119inc8inc: { u32 e = inc8_addr(); vrama[e + 1] = (u8)al; vmemch(e); *(u16 *)&vramaddr = (u16)(vramaddr + addrincr); break; }
    case RW_R211A: mode7set = (u8)al; break;
    case RW_R211B: LO(&mode7A) = HI(&mode7A); HI(&mode7A) = (u8)al; multchange = 1; break;
    case RW_R211C: LO(&mode7B) = HI(&mode7B); HI(&mode7B) = (u8)al; multchange = 1; break;
    case RW_R211D: LO(&mode7C) = HI(&mode7C); HI(&mode7C) = (u8)al; break;
    case RW_R211E: LO(&mode7D) = HI(&mode7D); HI(&mode7D) = (u8)al; break;
    case RW_R211F: LO(&mode7X0) = HI(&mode7X0); HI(&mode7X0) = (u8)al; break;
    case RW_R2120: LO(&mode7Y0) = HI(&mode7Y0); HI(&mode7Y0) = (u8)al; break;
    case RW_R2121: cgaddr = (u16)((al << 1) & 0x1FF); break;
    case RW_R2122:
        w = cgaddr;
        if (cgram[w] != al) { cgram[w] = (u8)al; cgmod = 1; }
        cgaddr = (u16)((cgaddr + 1) & 0x1FF);
        break;
    case RW_R2123: winbg1en = al & 0x0F; winbg2en = (u8)(al >> 4); break;
    case RW_R2124: winbg3en = al & 0x0F; winbg4en = (u8)(al >> 4); break;
    case RW_R2125: winobjen = al & 0x0F; wincolen = (u8)(al >> 4); break;
    case RW_R2126: winl1 = (u8)al; break;
    case RW_R2127: winr1 = (u8)al; break;
    case RW_R2128: winl2 = (u8)al; break;
    case RW_R2129: winr2 = (u8)al; break;
    case RW_R212A: winlogica = (u8)al; break;
    case RW_R212B: winlogicb = (u8)al; break;
    case RW_R212C: LO(&scrnon) = (u8)al; break;
    case RW_R212D: HI(&scrnon) = (u8)al; break;
    case RW_R212E: winenabm = (u8)al; break;
    case RW_R212F: winenabs = (u8)al; break;
    case RW_R2130: scaddset = (u8)al; break;
    case RW_R2131: scaddtype = (u8)al; break;
    case RW_R2132: {
        u8 bl = al & 0x1F;
        if (al & 0x20) coladdr = bl;
        if (al & 0x40) coladdg = bl;
        if (al & 0x80) coladdb = bl;
        break;
    }
    case RW_R2133: interlval = al & 0x41; resolutn = (al & 4) ? 239 : 224; break;
    case RW_R2140: case RW_R2141: case RW_R2142: case RW_R2143:
        sndwrit = 1;
        if (id == RW_R2140 && (u8)nmistatus == 2) *(u8 *)&nmirept = 0;
        spcRam[0xF4 + (id - RW_R2140)] = (u8)al;
        SPC700write++;
        reenablespc();
        break;
    case RW_R2180: *(u8 *)(wramrwadr + wramdata) = (u8)al; wramrwadr = (wramrwadr + 1) & 0x1FFFF; break;
    case RW_R2181: LO(&wramrwadr) = (u8)al; break;
    case RW_R2182: ((u8 *)&wramrwadr)[1] = (u8)al; break;
    case RW_R2183: ((u8 *)&wramrwadr)[2] = al & 1; break;
    case RW_R4016:
        if (!(INTEnab & 1)) {
            joy_reset_now();
            if (al == 1) MultiTapStat |= 1; else MultiTapStat &= 0xFE;
        } else if (al == 1) {
            MultiTapStat |= 1; JoyCRead |= 2;
        } else {
            MultiTapStat &= 0xFE;
            if (al == 0) {
                JoyCRead |= 1;
                if (JoyCRead == 3) joy_reset_now();
            }
        }
        break;
    case RW_R4017: break;
    case RW_R4200: INTEnab = (u8)al; break;
    case RW_R4201:
        if (iohvlatch == 1 && !(al & 0x80)) iohvlatch = 0;
        if (!(ioportval & 0x80) && (al & 0x80)) iohvlatch = 1;
        ioportval = (u8)al;
        MultiTapStat = (MultiTapStat & 0x7F) | (al & 0x80);
        break;
    case RW_R4202: multa = (u8)al; break;
    case RW_R4203: multres = (u16)(multa * al); break;
    case RW_R4204: LO(&diva) = (u8)al; break;
    case RW_R4205: HI(&diva) = (u8)al; break;
    case RW_R4206:
        if (al == 0) { divres = 0xFFFF; multres = diva; }
        else { divres = (u16)(diva / al); multres = (u16)(diva % al); }
        break;
    case RW_R4207:
        if (LO(&HIRQLoc) != al) { LO(&HIRQLoc) = (u8)al; if (curypos == VIRQLoc) determine_hirq(); }
        break;
    case RW_R4208:
        if (HI(&HIRQLoc) != al) { HI(&HIRQLoc) = (u8)al; if (curypos == VIRQLoc) determine_hirq(); }
        break;
    case RW_R4209: LO(&VIRQLoc) = (u8)al; virq_cancel(); break;
    case RW_R420A:
        HI(&VIRQLoc) = al & 1; R.eax = (R.eax & ~0xFFu) | (al & 1);
        if (VIRQLoc >= (u16)(totlines - 1)) VIRQLoc = 0x7FFF;
        virq_cancel();
        break;
    case RW_R420D:
        if (!(al & 1)) { cycpl = opexec268; cycphb = opexec268cph; xirqb = 0; *(u8 *)&cycpblt = cycpb268; R.eax = (R.eax & ~0xFFu) | opexec268cph; }
        else { cycpl = opexec358; cycphb = opexec358cph; xirqb |= 0x80; *(u8 *)&cycpblt = cycpb358; R.eax = (R.eax & ~0xFFu) | opexec358cph; }
        break;
    case RW_R420E: case RW_R4210: case RW_R4211: case RW_R4212: case RW_R4216: break;
    case RW_R43X0: case RW_R43X1:
        dmadata[(u16)(ecx - 0x4300)] = (u8)al; hdmarestart = 1; break;
    case RW_R43X2: case RW_R43X3: case RW_R43X4: case RW_R43X5: case RW_R43X6: case RW_R43X7:
    case RW_R43X8: case RW_R43X9: case RW_R43XA:
        dmadata[(u16)(ecx - 0x4300)] = (u8)al; break;
    default: break;
    }
}

/* ===================== read handlers (regs.inc) ===================== */
extern u8 latchxr, latchyr, extlatch, ppustatus, cfield, romispal, debuggeron, spcon, reg1read, reg2read, reg3read, reg4read;
extern u8 snesmouse, MultiTap, NMIEnab, curnmi, irqon, sndrot, sndrot2, vramincby8rowl, nssdip1, nssdip2, nssdip3, nssdip4, nssdip5, nssdip6;
extern u8 SPC7110Enable, hblank;
extern u16 latchx, latchy;
extern u32 pdh, spcnumread, SPC700read, joycontren, compmult, xa;
extern void *zs_rrstub_tab[RR_COUNT];
extern void *zs_rrasm_tab[RR_COUNT];
extern u32 *regptr;

static void set_al(u32 v) { R.eax = (R.eax & ~0xFFu) | (v & 0xFF); }

static void checkmult(void)
{
    if (multchange == 0) return;
    {
        s_t bl = (s_t)HI(&mode7B);
        int bx = (bl & 0x80) ? (int)(int16_t)(0xFF00 | (u8)bl) : (int)(u8)bl;
        int32_t prod = (int32_t)(int16_t)mode7A * (int16_t)bx;
        *(u16 *)&compmult = (u16)prod;
        ((u8 *)&compmult)[2] = (u8)((u32)prod >> 16);
    }
    multchange = 0;
}

static void latch_common(u32 dhv, int debugger)
{
    u16 bx = (u16)(((u8)(cycpl - (u8)dhv)) * 2);
    latchx = bx;
    latchy = curypos;
    if ((INTEnab & 0x30) == 0x30) {
        if (debugger) latchy++;
        else if (HIRQLoc > 0xF0 || dhv < 20) latchy++;
    }
}

static void vram_inc_after_read(int skip_if_incr)
{
    if (vramincr == skip_if_incr) return;
    *(u16 *)&vramaddr = (u16)(vramaddr + addrincr);
    if (vramincby8on != 1) return;
    vramincby8left--;
    if (vramincby8left != 0) return;
    *(u16 *)&vramaddr = (u16)(vramaddr + 2);
    vramincby8left = vramincby8totl;
    vramincby8rowl--;
    if (vramincby8rowl == 0) {
        vramincby8rowl = 8;
        *(u16 *)&vramaddr = (u16)(vramaddr - 16);
    } else
        *(u16 *)&vramaddr = (u16)(vramaddr - vramincby8ptri);
}

static void spc_patch(int tries, int first)
{
    u8 *p = (u8 *)R.esi;
    int al;
    if (first && *(u16 *)p == 0xFB10) *(u16 *)p = 0xEAEA;
    for (al = tries; al; al--, p++)
        if (*p == 0xD0) { p[0] = 0xEA; p[1] = 0xEA; break; }
}

static u32 joy_bit(u32 *j) { u32 r = (*j >> 31) & 1; *j = (*j << 1) | r; return r; }

static void read_c(u32 id)
{
    u32 al = 0;
    u32 cx = R.ecx & 0xFFFF;
    switch (id) {
    case RR_R2100: al = vidbright | forceblnk; break;
    case RR_R2134: checkmult(); al = compmult & 0xFF; break;
    case RR_R2135: checkmult(); al = (compmult >> 8) & 0xFF; break;
    case RR_R2136: checkmult(); al = (compmult >> 16) & 0xFF; break;
    case RR_R2137:
        if (debuggeron == 1) {
            if (iohvlatch == 1 || (ioportval & 0x80)) latch_common(pdh & 0xFF, 1);
        } else {
            if (iohvlatch == 1 || (ioportval & 0x80)) latch_common(dh_get(), 0);
        }
        al = 0; extlatch = 0;
        break;
    case RR_R2138: {
        u16 w = (u16)oamaddr;
        al = oamram[w];
        *(u16 *)&oamaddr = (u16)(w + 1);
        if ((u16)oamaddr > 543) *(u16 *)&oamaddr = 0;
        break;
    }
    case RR_R2139:
        al = vramread;
        vramread = vram[(u16)vramaddr];
        vram_inc_after_read(0);
        break;
    case RR_R213A:
        al = vramread2;
        vramread2 = vram[(u16)vramaddr + 1u];
        vram_inc_after_read(1);
        break;
    case RR_R213B:
        al = cgram[cgaddr];
        cgaddr = (u16)((cgaddr + 1) & 0x1FF);
        break;
    case RR_R213C:
        if (latchxr == 1) { al = (LO(&latchx) & 0xFE) | HI(&latchx); latchxr = 0; }
        else { al = LO(&latchx); latchxr = 1; }
        break;
    case RR_R213D:
        if (latchyr == 2) al = (LO(&latchy) & 0xFE) | HI(&latchy);
        else if (latchyr == 1) { al = (LO(&latchy) & 0xFE) | HI(&latchy); latchyr = 0; }
        else { al = LO(&latchy); latchyr = 1; }
        break;
    case RR_R213E: al = 1; break;
    case RR_R213F:
        al = (u8)(romispal << 4) | ppustatus | cfield;
        latchxr = 0; latchyr = 0;
        al |= extlatch;
        break;
    case RR_R2140:
        if (spcon) { SPC700read++; al = reg1read; spcnumread = 0; }
        else {
            spc_patch(5, 1);
            if (++sndrot2 == 3) sndrot2 = 0;
            al = (sndrot2 & 1) ? (xa & 0xFF) : 0;
        }
        break;
    case RR_R2141:
        if (spcon) { SPC700read++; al = reg2read; spcnumread = 0; }
        else {
            spc_patch(3, 0);
            sndrot ^= 1;
            al = (sndrot & 1) ? (xa & 0xFF) : ((xa >> 8) & 0xFF);
        }
        break;
    case RR_R2142:
        if (spcon) { SPC700read++; al = reg3read; spcnumread = 0; }
        else {
            spc_patch(3, 0);
            al = (sndrot & 1) ? ((xa >> 8) & 0xFF) : (xa & 0xFF);
        }
        break;
    case RR_R2143:
        if (spcon) { SPC700read++; al = reg4read; spcnumread = 0; }
        else { spc_patch(3, 0); al = (xa >> 8) & 0xFF; }
        break;
    case RR_R2144: al = 0; break;
    case RR_R2180:
        al = *(u8 *)(wramrwadr + wramdata);
        wramrwadr = (wramrwadr + 1) & 0x1FFFF;
        break;
    case RR_R4016: al = joy_bit(&JoyANow); break;
    case RR_R4017:
        if (snesmouse != 0 || MultiTap != 1) al = joy_bit(&JoyBNow);
        else if (MultiTapStat & 1) al = 0xFF;
        else if (MultiTapStat & 0x80) { al = joy_bit(&JoyBNow); al |= joy_bit(&JoyCNow) << 1; }
        else { al = joy_bit(&JoyDNow); al |= joy_bit(&JoyENow) << 1; }
        break;
    case RR_R4100:
        al = (nssdip1 == 1) | ((nssdip2 == 1) << 1) | ((nssdip3 == 1) << 2) | ((nssdip4 == 1) << 3) | ((nssdip5 == 1) << 4) | ((nssdip6 == 1) << 5);
        break;
    case RR_RZERO: al = 0; break;
    case RR_R4210:
        al = NMIEnab;
        if (curnmi == 0) NMIEnab = 1;
        curnmi = 0;
        break;
    case RR_R4211:
        al = irqon; irqon = 0;
        if (dh_get() < cycphb) al |= 0x40;
        break;
    case RR_R4212: {
        u32 dhv = debuggeron == 1 ? (pdh & 0xFF) : dh_get();
        u16 bx = resolutn;
        int vbl;
        al = (u8)joycontren >= 25;
        joycontren = (joycontren & ~0xFFu) | (u8)(joycontren + 1);
        if ((u8)joycontren == 50) joycontren = (joycontren & ~0xFFu);
        if (curypos == bx && (u8)nmistatus == 2) vbl = 1;
        else {
            bx = (u16)(bx + 1);
            if (curypos < bx) vbl = 0;
            else vbl = !(curypos >= (u16)(totlines - 1));
        }
        if (vbl) al |= 0x80;
        if (debuggeron != 1) hblank = 0;
        if (dhv < cycphb) { if (debuggeron != 1) hblank = 1; al |= 0x40; }
        if (brk_mode == 2) al ^= 0x40;
        break;
    }
    case RR_R4213: al = ioportval; break;
    case RR_R4214: al = LO(&divres); break;
    case RR_R4215: al = HI(&divres); break;
    case RR_R4216: al = LO(&multres); break;
    case RR_R4217: al = HI(&multres); break;
    case RR_R4218: al = ((u8 *)&JoyAOrig)[2]; break;
    case RR_R4219: al = ((u8 *)&JoyAOrig)[3]; break;
    case RR_R421A: al = ((u8 *)&JoyBOrig)[2]; break;
    case RR_R421B: al = ((u8 *)&JoyBOrig)[3]; break;
    case RR_R421C: al = ((u8 *)&JoyDOrig)[2]; break;
    case RR_R421D: al = ((u8 *)&JoyDOrig)[3]; break;
    case RR_R421E: al = ((u8 *)&JoyCOrig)[2]; break;
    case RR_R421F: al = ((u8 *)&JoyCOrig)[3]; break;
    case RR_R43XX: al = dmadata[(u16)(cx - 0x4300)]; break;
    case RR_RINVALID:
        if (cx < 0x2100) al = 0;
        else { al = cx >> 8; if (SPC7110Enable != 0) al = 0; }
        break;
    default: break;
    }
    set_al(al);
}

static int rr_id(u32 r)
{
    if (r == 0x2100) return RR_R2100;
    if (r >= 0x2134 && r <= 0x2143) return RR_R2134 + (int)(r - 0x2134);
    if (r == 0x2144) return RR_R2144;
    if (r == 0x2180) return RR_R2180;
    if (r == 0x4016) return RR_R4016;
    if (r == 0x4017) return RR_R4017;
    if (r == 0x4100) return RR_R4100;
    if (r >= 0x420A && r <= 0x420F) return RR_RZERO;
    if (r >= 0x4210 && r <= 0x421F) return RR_R4210 + (int)(r - 0x4210);
    if (r >= 0x4300 && r <= 0x437F) return RR_R43XX;
    return RR_RINVALID;
}

/* ---------- id <-> register address map (mirrors initregw) ---------- */
static int reg_id(u32 r)
{
    if (r == 0x2118) return RW_V2118;
    if (r == 0x2119) return RW_V2119inc;
    if (r >= 0x2100 && r <= 0x2133) return RW_R2100 + (int)(r - 0x2100);
    if (r >= 0x2140 && r <= 0x2143) return RW_R2140 + (int)(r - 0x2140);
    if (r == 0x2144) return RW_R2140;
    if (r >= 0x2180 && r <= 0x2183) return RW_R2180 + (int)(r - 0x2180);
    if (r == 0x2118) return RW_V2118;
    if (r == 0x2119) return RW_V2119inc;
    if (r == 0x4016) return RW_R4016;
    if (r == 0x4017) return RW_R4017;
    if (r >= 0x4200 && r <= 0x420A && r != 0x420B && r != 0x420C) return RW_R4200 + (int)(r - 0x4200);
    if (r == 0x420D) return RW_R420D;
    if (r == 0x420E) return RW_R420E;
    if (r == 0x4210) return RW_R4210;
    if (r == 0x4211) return RW_R4211;
    if (r == 0x4212) return RW_R4212;
    if (r == 0x4216) return RW_R4216;
    if (r >= 0x4300 && r <= 0x437F) { u32 k = r & 15; return RW_R43X0 + (int)(k > 10 ? 10 : k); }
    return -1;
}

static int regw_mode(void)
{
    static int m = -1;
    if (m < 0) { m = getenv("ZS_REGW_ASM") ? 0 : getenv("ZS_REGW_DIFF") ? 2 : 1; brk_mode = getenv("ZS_REGW_BREAK") ? atoi(getenv("ZS_REGW_BREAK")) : 0; }
    return m;
}

HID __attribute__((force_align_arg_pointer)) void zs_regw_install(void)
{
    u32 r;
    if (regw_mode() == 0) return;
    for (r = 0x2100; r <= 0x437F; r++) {
        int id = reg_id(r);
        /* 2118/2119 may be asm variants chosen by the asm side; only replace what initregw set */
        if (id >= 0 && regptw[r] == (u32)zs_rwasm_tab[id]) regptw[r] = (u32)zs_rwstub_tab[id];
    }
}

HID __attribute__((force_align_arg_pointer)) void zs_regr_install(void)
{
    u32 r;
    if (regw_mode() == 0) return;
    for (r = 0x2100; r <= 0x437F; r++) {
        int id = rr_id(r);
        if (regptr[r] == (u32)zs_rrasm_tab[id]) regptr[r] = (u32)zs_rrstub_tab[id];
    }
}

/* ---------- differential check: full writable image ---------- */
typedef struct { u8 *p; size_t n; } seg_t;
static seg_t segs[8];
static int nsegs;
static u8 *snap0[8], *snapA[8];
#define NIDS (RW_COUNT + RR_COUNT)
static unsigned long rw_ops, rw_bad, rw_cov[NIDS], rw_skip;
extern void zs_mcall(void *fn);
extern u32 zs_mcall_fn;
static sigjmp_buf rw_jb;
static int rw_in_asm;

static int phdr_cb(struct dl_phdr_info *info, size_t sz, void *data)
{
    Dl_info di;
    int i;
    (void)sz; (void)data;
    if (!dladdr((void *)zs_regw_install, &di) || (void *)info->dlpi_addr != di.dli_fbase) return 0;
    for (i = 0; i < info->dlpi_phnum; i++) {
        const ElfW(Phdr) *ph = &info->dlpi_phdr[i];
        if (ph->p_type == PT_LOAD && (ph->p_flags & PF_W)) {
            uintptr_t lo = info->dlpi_addr + ph->p_vaddr, hi = lo + ph->p_memsz;
            FILE *f = fopen("/proc/self/maps", "r");
            char line[512];
            while (f && fgets(line, sizeof line, f)) {
                unsigned long a, b; char perm[8];
                if (sscanf(line, "%lx-%lx %7s", &a, &b, perm) == 3 && perm[0] == 'r' && perm[1] == 'w' && b > lo && a < hi && nsegs < 8) {
                    uintptr_t s0 = a > lo ? a : lo, s1 = b < hi ? b : hi;
                    segs[nsegs].p = (u8 *)s0; segs[nsegs].n = s1 - s0; nsegs++;
                }
            }
            if (f) fclose(f);
        }
    }
    return 1;
}

static u8 *excl_lo[16], *excl_hi[16];
static int nexcl;
static void excl(void *p, size_t n) { excl_lo[nexcl] = p; excl_hi[nexcl] = (u8 *)p + n; nexcl++; }
static int excluded(u8 *a) { int i; for (i = 0; i < nexcl; i++) if (a >= excl_lo[i] && a < excl_hi[i]) return 1; return 0; }

static void rw_report(void)
{
    unsigned k, cov = 0;
    for (k = 0; k < NIDS; k++) cov += rw_cov[k] != 0;
    fprintf(stderr, "regw diff: %lu ops, %lu mismatching, %lu skipped; handlers exercised %u/%d\n", rw_ops, rw_bad, rw_skip, cov, NIDS);
}

static void rw_sig(int s) { if (rw_in_asm) { rw_in_asm = 0; siglongjmp(rw_jb, 1); } signal(s, SIG_DFL); raise(s); }
static int rw_guard;

static void rw_setup(void)
{
    int i;
    if (nsegs) return;
    dl_iterate_phdr(phdr_cb, NULL);
    for (i = 0; i < nsegs; i++) { snap0[i] = malloc(segs[i].n); snapA[i] = malloc(segs[i].n); }
    excl(&rw_ops, sizeof rw_ops); excl(&rw_bad, sizeof rw_bad); excl(rw_cov, sizeof rw_cov); excl(&rw_skip, sizeof rw_skip);
    excl(&zs_mcall_fn, 4); excl((u8 *)&R + 12, 4); excl(&rw_jb, sizeof rw_jb); excl(&rw_in_asm, 4);
    excl(snap0, sizeof snap0); excl(snapA, sizeof snapA); excl(segs, sizeof segs);
    atexit(rw_report);
}

static void save_all(u8 **dst) { int i; for (i = 0; i < nsegs; i++) memcpy(dst[i], segs[i].p, segs[i].n); }
static void load_all(u8 **src) { int i; for (i = 0; i < nsegs; i++) memcpy(segs[i].p, src[i], segs[i].n); }

/* identify which variant id a regptw entry holds, in either namespace */
static int tab_id(u32 v)
{
    int k;
    for (k = 0; k < RW_COUNT; k++) if (v == (u32)zs_rwstub_tab[k] || v == (u32)zs_rwasm_tab[k]) return k;
    return -1;
}

static void write_c(u32 id);
static void read_c(u32 id);
static int rw_diff2(int isw, u32 id)
{
    static int pr[NIDS];
    void *afn = isw ? zs_rwasm_tab[id] : zs_rrasm_tab[id];
    u32 cid = isw ? id : RW_COUNT + id;
    int i;
    size_t k;
    int bad = 0;
    u32 save_t[2];
    int asm_t[2];
    regs_t Rin;
    rw_setup();
    Rin = R;
    save_all(snap0);
    /* asm run */
    if (rw_guard) {
        if (sigsetjmp(rw_jb, 1)) { load_all(snap0); rw_skip++; return 0; }
        rw_in_asm = 1;
    }
    zs_mcall(afn);
    rw_in_asm = 0;
    save_all(snapA);
    asm_t[0] = tab_id(regptw[0x2118]); asm_t[1] = tab_id(regptw[0x2119]);
    save_t[0] = regptw[0x2118]; save_t[1] = regptw[0x2119];
    load_all(snap0);
    R = Rin;
    if (isw) write_c(id); else read_c(id);
    rw_ops++; rw_cov[cid]++;
    if (!rw_guard && (rw_ops & 1023) == 0) rw_report();
    for (i = 0; i < nsegs && bad < 8; i++)
        for (k = 0; k < segs[i].n && bad < 8; k++) {
            u8 *a = segs[i].p + k;
            if (*a == snapA[i][k] || excluded(a)) continue;
            if (isw && a >= (u8 *)&regptw[0x2118] && a < (u8 *)&regptw[0x2118] + 4) continue;
            if (isw && a >= (u8 *)&regptw[0x2119] && a < (u8 *)&regptw[0x2119] + 4) continue;
            {
                Dl_info di, di2; const char *nm = "?"; long off = 0; dladdr((void *)zs_regw_install, &di2);
                if (dladdr(a, &di) && di.dli_sname) { nm = di.dli_sname; off = a - (u8 *)di.dli_saddr; }
                if (pr[cid]++ < 2) fprintf(stderr, "regw diff: id=%u val=%02x ecx=%04x: %s+%ld off=%lx C=%02x asm=%02x\n", id, R.eax & 0xFF, Rin.ecx, nm, off, (unsigned long)((uintptr_t)a - (uintptr_t)((Dl_info *)&di2)->dli_fbase), *a, snapA[i][k]);
                bad++;
            }
        }
    if (isw && (tab_id(regptw[0x2118]) != asm_t[0] || tab_id(regptw[0x2119]) != asm_t[1])) {
        if (rw_bad < 40) fprintf(stderr, "regw diff: id=%u 2118/2119 handler mismatch\n", id);
        bad++;
    }
    (void)save_t;
    if (bad) {
        unsigned long ob = rw_bad + 1, oo = rw_ops, oc[NIDS], os = rw_skip;
        memcpy(oc, rw_cov, sizeof oc);
        load_all(snapA);
        rw_bad = ob; rw_ops = oo; rw_skip = os; memcpy(rw_cov, oc, sizeof oc);
    }
    else {
        /* keep the C-side table (stub addresses) but asm-equivalent state */
    }
    return bad;
}

static int rw_diff(u32 id) { return rw_diff2(1, id); }

HID __attribute__((force_align_arg_pointer)) void zs_regr_c(u32 id)
{
    static unsigned long n[RR_COUNT];
    if (regw_mode() == 2) {
        if (n[id] < 64 || (n[id] & 15) == 0) { n[id]++; rw_diff2(0, id); return; }
        n[id]++;
    }
    read_c(id);
}

HID __attribute__((force_align_arg_pointer)) void zs_regw_c(u32 id)
{
    static unsigned long n[RW_COUNT];
    int m = regw_mode();
    if (m == 2) {
        /* sample: first 64 per handler, then every 16th */
        if (n[id] < 64 || (n[id] & 15) == 0) { n[id]++; rw_diff(id); return; }
        n[id]++;
    }
    write_c(id);
}

/* ---------- fuzz ---------- */
static u32 rng_s;
static u32 rnd(void) { rng_s ^= rng_s << 13; rng_s ^= rng_s >> 17; rng_s ^= rng_s << 5; return rng_s; }

static void scramble(void)
{
    static const u16 incs[] = { 2, 64, 256, 256 };
    prevoamptr = rnd() % 4 ? (u8)rnd() : 0xFF;
    oamlow = rnd() & 1; nexthprior = rnd() & 1; nosprincr = rnd() % 3 == 0;
    poamaddrs = rnd() % 2 ? 0x200 + (rnd() % 0x40) : (u16)(rnd() % 0x220);
    oamaddr = rnd() % 4 ? rnd() % 0x240 : (rnd() % 2 ? 0x200 : 543);
    oamaddrs = (u16)rnd();
    vramaddr = rnd() % 0x10000; addrincr = incs[rnd() & 3]; vramincr = rnd() & 1;
    vramincby8on = rnd() & 1;
    vramincby8left = (u8)((rnd() % 3 + 1) * 64 - 1); vramincby8totl = (u8)(5 + rnd() % 3);
    vramincby8ptri = (u16)rnd(); vramincby8var = (u16)rnd();
    cgaddr = rnd() % 0x200; wramrwadr = rnd() % 0x20000;
    JoyAOrig = rnd(); JoyBOrig = rnd(); JoyCOrig = rnd(); JoyDOrig = rnd(); JoyEOrig = rnd();
    JoyANow = rnd(); JoyBNow = rnd(); JoyCNow = rnd(); JoyDNow = rnd(); JoyENow = rnd();
    JoyCRead = rnd() & 3; INTEnab = (u8)rnd(); MultiTapStat = (u8)rnd();
    ioportval = (u8)rnd(); iohvlatch = rnd() & 1;
    HIRQLoc = (u16)(rnd() % 0x200); VIRQLoc = (u16)(rnd() % 270); curypos = rnd() % 3 ? VIRQLoc : (u16)(rnd() % 270);
    totlines = rnd() % 2 ? 262 : 312;
    HIRQNextExe = rnd() & 1; HIRQCycNext = (u8)rnd(); HIRQSkip = rnd() % 4 == 0;
    cycpl = (u8)(rnd() % 2 ? 180 : 168 + rnd() % 80); nmistatus = rnd() % 4; nmirept = rnd() % 8;
    cycpbl = rnd() % 2 ? rnd() % 200 : 0x1000000u + (rnd() % 0x1000000u);
    curexecstate = rnd() & 3;
    bgscroltemp = 0;
    latchx = (u16)rnd(); latchy = (u16)rnd(); latchxr = rnd() % 3; latchyr = rnd() % 4; extlatch = (u8)rnd();
    ppustatus = (u8)rnd(); cfield = rnd() & 1; romispal = rnd() & 1; debuggeron = rnd() % 4 ? 1 : 0;
    pdh = rnd(); spcon = rnd() % 4 != 0; reg1read = (u8)rnd(); reg2read = (u8)rnd(); reg3read = (u8)rnd(); reg4read = (u8)rnd();
    snesmouse = rnd() % 4 == 0; MultiTap = rnd() & 1; NMIEnab = (u8)rnd(); curnmi = rnd() & 1; irqon = (u8)rnd();
    sndrot = (u8)rnd(); sndrot2 = rnd() % 3; vramincby8rowl = (u8)(1 + rnd() % 8); joycontren = rnd() % 60;
    nssdip1 = rnd() & 1; nssdip2 = rnd() & 1; nssdip3 = rnd() & 1; nssdip4 = rnd() & 1; nssdip5 = rnd() & 1; nssdip6 = rnd() & 1;
    compmult = rnd(); xa = rnd(); resolutn = rnd() % 2 ? 224 : 239; SPC7110Enable = rnd() % 8 == 0;
    cycphb = (u8)(40 + rnd() % 10); mode7A = (u16)rnd(); mode7B = (u16)rnd(); multchange = rnd() & 1;
    multa = (u8)rnd(); diva = (u16)rnd();
    *(u16 *)&objptr = (u16)rnd(); objptrn = rnd();
    { u8 *p = (u8 *)&BG116x16t; __asm__("" : "+r"(p)); memset(p, (int)(rnd() & 1), 4); }
}

unsigned zs_regw_fuzz(unsigned iters, unsigned seed)
{
    unsigned it;
    struct sigaction sa;
    rw_setup();
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = rw_sig;
    sigaction(SIGSEGV, &sa, NULL);
    rw_guard = 1;
    rng_s = seed ? seed : 1;
    for (it = 0; it < iters; it++) {
        int isw = rnd() % 3 != 0;
        u32 id = rnd() % (isw ? RW_COUNT : RR_COUNT), val = rnd();
        u32 ecx = 0x2100;
        if (isw && id >= RW_R43X0 && id <= RW_R43XA) ecx = 0x4300 + (rnd() % 8) * 16 + (id - RW_R43X0);
        if (!isw) ecx = rnd() % 4 ? 0x2100 + (rnd() % 0x2300) : 0x4300 + rnd() % 0x80;
        if ((it & 63) == 0) { u32 *t = (u32 *)regptw; (void)t; }
        scramble();
        R.eax = val; R.ecx = ecx; R.edx = rnd(); R.edi = rnd(); R.ebx = rnd(); R.ebp = rnd();
        { u8 *q = spcRam + 0x1000 + rnd() % 0x1000; int j; R.esi = (u32)q; for (j = 0; j < 8; j++) q[j] = rnd() % 3 ? (u8)rnd() : (rnd() % 2 ? 0xD0 : (j & 1 ? 0xFB : 0x10)); }
        if (id == RW_R2101 || id == RW_R2115) val = rnd();
        R.eax = val;
        rw_diff2(isw, id);
    }
    rw_guard = 0;
    rw_report();
    return (unsigned)rw_bad;
}
