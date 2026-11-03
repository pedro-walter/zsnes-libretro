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
#include "zl.h"
#include "../cpu/regw_ids.h"
#include "../cpu/regr_ids.h"

typedef int8_t s_t;
typedef int32_t s32;
typedef int16_t s16;

#define R LG

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
extern u32 vram;                              /* guest pointer slot */
#define VRAM ((u8 *)(uintptr_t)vram)
extern u8 vrama[], vidmemch2[], vidmemch4[], vidmemch8[];
extern u32 regptw, regptr;                   /* guest pointer slots */
#define REGPTW ((u32 *)(uintptr_t)regptw)
extern u32 tableadc[256];

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


static const u32 rw_label_id[RW_COUNT] = {
    ZID_reg2100w,
    ZID_reg2101w,
    ZID_reg2102w,
    ZID_reg2103w,
    ZID_reg2104w,
    ZID_reg2105w,
    ZID_reg2106w,
    ZID_reg2107w,
    ZID_reg2108w,
    ZID_reg2109w,
    ZID_reg210Aw,
    ZID_reg210Bw,
    ZID_reg210Cw,
    ZID_reg210Dw,
    ZID_reg210Ew,
    ZID_reg210Fw,
    ZID_reg2110w,
    ZID_reg2111w,
    ZID_reg2112w,
    ZID_reg2113w,
    ZID_reg2114w,
    ZID_reg2115w,
    ZID_reg2116w,
    ZID_reg2117w,
    ZID_regINVALIDw,
    ZID_regINVALIDw,
    ZID_reg211Aw,
    ZID_reg211Bw,
    ZID_reg211Cw,
    ZID_reg211Dw,
    ZID_reg211Ew,
    ZID_reg211Fw,
    ZID_reg2120w,
    ZID_reg2121w,
    ZID_reg2122w,
    ZID_reg2123w,
    ZID_reg2124w,
    ZID_reg2125w,
    ZID_reg2126w,
    ZID_reg2127w,
    ZID_reg2128w,
    ZID_reg2129w,
    ZID_reg212Aw,
    ZID_reg212Bw,
    ZID_reg212Cw,
    ZID_reg212Dw,
    ZID_reg212Ew,
    ZID_reg212Fw,
    ZID_reg2130w,
    ZID_reg2131w,
    ZID_reg2132w,
    ZID_reg2133w,
    ZID_reg2140w,
    ZID_reg2141w,
    ZID_reg2142w,
    ZID_reg2143w,
    ZID_reg2180w,
    ZID_reg2181w,
    ZID_reg2182w,
    ZID_reg2183w,
    ZID_reg4016w,
    ZID_reg4017w,
    ZID_reg4200w,
    ZID_reg4201w,
    ZID_reg4202w,
    ZID_reg4203w,
    ZID_reg4204w,
    ZID_reg4205w,
    ZID_reg4206w,
    ZID_reg4207w,
    ZID_reg4208w,
    ZID_reg4209w,
    ZID_reg420Aw,
    ZID_reg420Dw,
    ZID_reg420Ew,
    ZID_reg4210w,
    ZID_reg4211w,
    ZID_reg4212w,
    ZID_reg4216w,
    ZID_reg43X0w,
    ZID_reg43X1w,
    ZID_reg43x2w,
    ZID_reg43x3w,
    ZID_reg43x4w,
    ZID_reg43x5w,
    ZID_reg43x6w,
    ZID_reg43x7w,
    ZID_reg43x8w,
    ZID_reg43x9w,
    ZID_reg43XAw,
    ZID_reg2118,
    ZID_reg2118inc,
    ZID_reg2118inc8,
    ZID_reg2118inc8inc,
    ZID_reg2119,
    ZID_reg2119inc,
    ZID_reg2119inc8,
    ZID_reg2119inc8inc,
};
static const u32 rr_label_id[RR_COUNT] = {
    ZID_reg2100r,
    ZID_reg2134r,
    ZID_reg2135r,
    ZID_reg2136r,
    ZID_reg2137r,
    ZID_reg2138r,
    ZID_reg2139r,
    ZID_reg213Ar,
    ZID_reg213Br,
    ZID_reg213Cr,
    ZID_reg213Dr,
    ZID_reg213Er,
    ZID_reg213Fr,
    ZID_reg2140r,
    ZID_reg2141r,
    ZID_reg2142r,
    ZID_reg2143r,
    ZID_reg2144r,
    ZID_reg2180r,
    ZID_reg4016r,
    ZID_reg4017r,
    ZID_reg4100r,
    ZID_reg420Ar,
    ZID_reg4210r,
    ZID_reg4211r,
    ZID_reg4212r,
    ZID_reg4213r,
    ZID_reg4214r,
    ZID_reg4215r,
    ZID_reg4216r,
    ZID_reg4217r,
    ZID_reg4218r,
    ZID_reg4219r,
    ZID_reg421Ar,
    ZID_reg421Br,
    ZID_reg421Cr,
    ZID_reg421Dr,
    ZID_reg421Er,
    ZID_reg421Fr,
    ZID_reg43XXr,
    ZID_regINVALID,
};

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
    memset(p, 0, 4);
    p[3] += (al >> 7) & 1;      /* BG4 */
    p[2] += (al >> 6) & 1;      /* BG3 */
    p[1] += (al >> 5) & 1;      /* BG2 */
    p[0] += (al >> 4) & 1;      /* BG1 */
}

static void install_vram_handlers(int idx2118, int idx2119)
{
    REGPTW[0x2118] = rw_label_id[idx2118];
    REGPTW[0x2119] = rw_label_id[idx2119];
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
    case RW_R2106: mosaicon = al & 0x0F; mosaicsz = (u8)(al >> 4); break;
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
        vramread = VRAM[(u16)vramaddr];
        vramread2 = VRAM[(u16)vramaddr + 1u];
        break;
    case RW_V2118: vrama[vramaddr] = (u8)al; vmemch(vramaddr); break;
    case RW_V2118inc: vrama[vramaddr] = (u8)al; vmemch(vramaddr); *(u16 *)&vramaddr = (u16)(vramaddr + addrincr); break;
    case RW_V2118inc8: { u32 e = inc8_addr(); VRAM[e] = (u8)al; vmemch(e); break; }
    case RW_V2118inc8inc: { u32 e = inc8_addr(); VRAM[e] = (u8)al; vmemch(e); *(u16 *)&vramaddr = (u16)(vramaddr + addrincr); break; }
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
        vramread = VRAM[(u16)vramaddr];
        vram_inc_after_read(0);
        break;
    case RR_R213A:
        al = vramread2;
        vramread2 = VRAM[(u16)vramaddr + 1u];
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


/* ---- overrides of the asm register handlers ---- */
void zl_rw0(void) { write_c(0); }
void zl_rw1(void) { write_c(1); }
void zl_rw2(void) { write_c(2); }
void zl_rw3(void) { write_c(3); }
void zl_rw4(void) { write_c(4); }
void zl_rw5(void) { write_c(5); }
void zl_rw6(void) { write_c(6); }
void zl_rw7(void) { write_c(7); }
void zl_rw8(void) { write_c(8); }
void zl_rw9(void) { write_c(9); }
void zl_rw10(void) { write_c(10); }
void zl_rw11(void) { write_c(11); }
void zl_rw12(void) { write_c(12); }
void zl_rw13(void) { write_c(13); }
void zl_rw14(void) { write_c(14); }
void zl_rw15(void) { write_c(15); }
void zl_rw16(void) { write_c(16); }
void zl_rw17(void) { write_c(17); }
void zl_rw18(void) { write_c(18); }
void zl_rw19(void) { write_c(19); }
void zl_rw20(void) { write_c(20); }
void zl_rw21(void) { write_c(21); }
void zl_rw22(void) { write_c(22); }
void zl_rw23(void) { write_c(23); }
void zl_rw24(void) { write_c(24); }
void zl_rw25(void) { write_c(25); }
void zl_rw26(void) { write_c(26); }
void zl_rw27(void) { write_c(27); }
void zl_rw28(void) { write_c(28); }
void zl_rw29(void) { write_c(29); }
void zl_rw30(void) { write_c(30); }
void zl_rw31(void) { write_c(31); }
void zl_rw32(void) { write_c(32); }
void zl_rw33(void) { write_c(33); }
void zl_rw34(void) { write_c(34); }
void zl_rw35(void) { write_c(35); }
void zl_rw36(void) { write_c(36); }
void zl_rw37(void) { write_c(37); }
void zl_rw38(void) { write_c(38); }
void zl_rw39(void) { write_c(39); }
void zl_rw40(void) { write_c(40); }
void zl_rw41(void) { write_c(41); }
void zl_rw42(void) { write_c(42); }
void zl_rw43(void) { write_c(43); }
void zl_rw44(void) { write_c(44); }
void zl_rw45(void) { write_c(45); }
void zl_rw46(void) { write_c(46); }
void zl_rw47(void) { write_c(47); }
void zl_rw48(void) { write_c(48); }
void zl_rw49(void) { write_c(49); }
void zl_rw50(void) { write_c(50); }
void zl_rw51(void) { write_c(51); }
void zl_rw52(void) { write_c(52); }
void zl_rw53(void) { write_c(53); }
void zl_rw54(void) { write_c(54); }
void zl_rw55(void) { write_c(55); }
void zl_rw56(void) { write_c(56); }
void zl_rw57(void) { write_c(57); }
void zl_rw58(void) { write_c(58); }
void zl_rw59(void) { write_c(59); }
void zl_rw60(void) { write_c(60); }
void zl_rw61(void) { write_c(61); }
void zl_rw62(void) { write_c(62); }
void zl_rw63(void) { write_c(63); }
void zl_rw64(void) { write_c(64); }
void zl_rw65(void) { write_c(65); }
void zl_rw66(void) { write_c(66); }
void zl_rw67(void) { write_c(67); }
void zl_rw68(void) { write_c(68); }
void zl_rw69(void) { write_c(69); }
void zl_rw70(void) { write_c(70); }
void zl_rw71(void) { write_c(71); }
void zl_rw72(void) { write_c(72); }
void zl_rw73(void) { write_c(73); }
void zl_rw74(void) { write_c(74); }
void zl_rw75(void) { write_c(75); }
void zl_rw76(void) { write_c(76); }
void zl_rw77(void) { write_c(77); }
void zl_rw78(void) { write_c(78); }
void zl_rw79(void) { write_c(79); }
void zl_rw80(void) { write_c(80); }
void zl_rw81(void) { write_c(81); }
void zl_rw82(void) { write_c(82); }
void zl_rw83(void) { write_c(83); }
void zl_rw84(void) { write_c(84); }
void zl_rw85(void) { write_c(85); }
void zl_rw86(void) { write_c(86); }
void zl_rw87(void) { write_c(87); }
void zl_rw88(void) { write_c(88); }
void zl_rw89(void) { write_c(89); }
void zl_rw90(void) { write_c(90); }
void zl_rw91(void) { write_c(91); }
void zl_rw92(void) { write_c(92); }
void zl_rw93(void) { write_c(93); }
void zl_rw94(void) { write_c(94); }
void zl_rw95(void) { write_c(95); }
void zl_rw96(void) { write_c(96); }
void zl_rw97(void) { write_c(97); }
void zl_rr0(void) { read_c(0); }
void zl_rr1(void) { read_c(1); }
void zl_rr2(void) { read_c(2); }
void zl_rr3(void) { read_c(3); }
void zl_rr4(void) { read_c(4); }
void zl_rr5(void) { read_c(5); }
void zl_rr6(void) { read_c(6); }
void zl_rr7(void) { read_c(7); }
void zl_rr8(void) { read_c(8); }
void zl_rr9(void) { read_c(9); }
void zl_rr10(void) { read_c(10); }
void zl_rr11(void) { read_c(11); }
void zl_rr12(void) { read_c(12); }
void zl_rr13(void) { read_c(13); }
void zl_rr14(void) { read_c(14); }
void zl_rr15(void) { read_c(15); }
void zl_rr16(void) { read_c(16); }
void zl_rr17(void) { read_c(17); }
void zl_rr18(void) { read_c(18); }
void zl_rr19(void) { read_c(19); }
void zl_rr20(void) { read_c(20); }
void zl_rr21(void) { read_c(21); }
void zl_rr22(void) { read_c(22); }
void zl_rr23(void) { read_c(23); }
void zl_rr24(void) { read_c(24); }
void zl_rr25(void) { read_c(25); }
void zl_rr26(void) { read_c(26); }
void zl_rr27(void) { read_c(27); }
void zl_rr28(void) { read_c(28); }
void zl_rr29(void) { read_c(29); }
void zl_rr30(void) { read_c(30); }
void zl_rr31(void) { read_c(31); }
void zl_rr32(void) { read_c(32); }
void zl_rr33(void) { read_c(33); }
void zl_rr34(void) { read_c(34); }
void zl_rr35(void) { read_c(35); }
void zl_rr36(void) { read_c(36); }
void zl_rr37(void) { read_c(37); }
void zl_rr38(void) { read_c(38); }
void zl_rr39(void) { read_c(39); }
void zl_rr40(void) { read_c(40); }
