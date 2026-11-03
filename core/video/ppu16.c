/*
 * Renderer port, part 1: per-scanline state capture (newengine16b from video/newgfx16.asm), palette conversion
 * (c_setpalette16bng/c_setpalallng) and BackAreaFill.  Written as a literal transcription: all state lives in the asm
 * data (addressed by name through Z8/Z16/Z32, including the odd overlapping stores and index -1 reads), so the
 * asm and C versions can be diffed over the whole image.
 *
 *   ZS_PPU_ASM=1   keep the asm newengine16b
 *   ZS_PPU_DIFF=1  run asm and C on identical state each (sampled) scanline and compare image + video buffers
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "zsyms.h"
#include "diffimg.h"

#define HID __attribute__((visibility("hidden")))
#define R zs_R
#define FORCE __attribute__((force_align_arg_pointer))
extern void zs_mcall(void *fn);
extern u8 newengine16b_asm[];
extern u8 doveg[], dovegrest[], BuildWindow2[];
extern u8 SA1Enable[];
#define PTRV(s) (*(u8 **)(s))
#define P32(s) ((u32)(uintptr_t)(s))

/* machine registers as the asm sees them */
static struct { u32 eax, ebx, ecx, edx, esi, edi, ebp; } G;
static void sync_out(void) { R.eax = G.eax; R.ebx = G.ebx; R.ecx = G.ecx; R.edx = G.edx; R.esi = G.esi; R.edi = G.edi; R.ebp = G.ebp; }
static void sync_in(void) { G.eax = R.eax; G.ebx = R.ebx; G.ecx = R.ecx; G.edx = R.edx; G.esi = R.esi; G.edi = R.edi; G.ebp = R.ebp; }
#define eax G.eax
#define ebx G.ebx
#define ecx G.ecx
#define edx G.edx
#define esi G.esi
#define edi G.edi
#define ebp G.ebp
static void acall(void *fn) { sync_out(); zs_mcall(fn); sync_in(); }

static u32 scalec(u32 c, u32 vb, u32 sh)
{
    u32 ax = (c & 0xFF) * (vb & 0xFF);
    u32 q = (ax / 15) & 0xFF;
    return (q << (sh & 31)) & 0xFFFF;
}

static u16 convcol(u16 dx)
{
    u32 bx = 0;
    u8 vb = Z8(vidbright, 0);
    bx = (bx + scalec(dx & 0x1F, vb, Z32(ngrposng, 0))) & 0xFFFF;
    bx = (bx + scalec((dx >> 5) & 0x1F, vb, Z32(nggposng, 0))) & 0xFFFF;
    bx = (bx + scalec((dx >> 10) & 0x1F, vb, Z32(ngbposng, 0))) & 0xFFFF;
    return (u16)bx;
}

static void c_setpalallng(void)
{
    u8 *dst = PTRV(vbufdptr); u32 ebp_ = 0, cnt;
    Z32(palchanged, 0) = 1; Z8(cgmod, 0) = 0;
    Z8(colleft16b, 0) = 0;
    Z32(cpalptrng, 0) = (Z32(cpalptrng, 0) + 1024) & (255 * 1024);
    dst += Z32(cpalptrng, 0);
    for (cnt = 0; cnt < 256; cnt++, ebp_ += 2, dst += 2) {
        u16 dx = Z16(cgram, ebp_), bx;
        Z16(prevpal2, ebp_) = dx;
        bx = convcol(dx);
        *(u16 *)dst = bx;
        *(u16 *)(dst + 512) = bx | (u16)Z32(UnusedBit, 0);
    }
    Z8(colleft16b, 0) = 0;  /* wrapped 256 times -> 0 */
    Z8(prevbright, 0) = Z8(vidbright, 0);
    ecx = 0;
    if (Z8(V8Mode, 0) == 1) acall(dovegrest);
}

static void c_setpalette16bng(void)
{
    if (Z8(V8Mode, 0) == 1) acall(doveg);
    ebx = (ebx & ~0xFFu) | Z8(vidbright, 0);
    if (getenv("PPUDBG")) fprintf(stderr, "setpal vb=%d pb=%d cgmod=%d ptr=%x\n", Z8(vidbright,0), Z8(prevbright,0), Z8(cgmod,0), Z32(cpalptrng,0));
    if (Z8(vidbright, 0) != Z8(prevbright, 0)) {  /* jne near c_setpalallng (its ret returns to our caller) */
        c_setpalallng();
        return;
    }
    if (Z8(cgmod, 0) != 0) {
        u32 old, ebp_ = 0, cnt;
        u8 *src, *dst;
        Z8(cgmod, 0) = 0;
        old = Z32(cpalptrng, 0);
        Z32(cpalptrng, 0) = (old + 1024) & (255 * 1024);
        dst = PTRV(vbufdptr) + Z32(cpalptrng, 0);
        src = PTRV(vbufdptr) + old;
        Z8(colleft16b, 0) = 0;
        for (cnt = 0; cnt < 256; cnt++, ebp_ += 2, src += 2, dst += 2) {
            u16 dx = Z16(cgram, ebp_);
            if (Z16(prevpal2, ebp_) == dx) {
                *(u16 *)dst = *(u16 *)src;
                *(u16 *)(dst + 512) = *(u16 *)(src + 512);
            } else {
                u16 bx;
                Z16(prevpal2, ebp_) = dx;
                if (Z8(colleft16b, 0) != 0) Z32(palchanged, 0) = 1;
                bx = convcol(dx);
                *(u16 *)dst = bx;
                *(u16 *)(dst + 512) = bx | (u16)Z32(UnusedBit, 0);
            }
            Z8(colleft16b, 0)++;
        }
        ecx = 0;
    }
    if (Z8(V8Mode, 0) == 1) acall(dovegrest);
}

/* BackAreaFill: eax = scanline */
static void BackAreaFill(u32 y)
{
    u8 *p;
    u32 *tab;
    u32 reg_eax, reg_edx, fillcol, unfill;
    if (Z8(winbg1enval, y + 5 * 256) != 0 && Z32(BackAreaFillCol, 0) != Z32(BackAreaUnFillCol, 0)) {
        p = PTRV(vidbuffer) + 16 * 2 + (y << 9) + (y << 6) + Z32(BackAreaAdd, 0);
        tab = (u32 *)ngwintable;
        p -= 2;
        reg_eax = 256;
        unfill = Z32(BackAreaUnFillCol, 0); fillcol = Z32(BackAreaFillCol, 0);
        for (;;) {
            /* .procnotemptyb */
            reg_edx = *tab++;
            if (reg_edx != 0) {
                int c1, c2;
                reg_edx--;
                for (;;) {
                    *(u32 *)p = unfill; *(u32 *)(p + 4) = unfill; p += 8;
                    c1 = reg_eax < 4; reg_eax -= 4; if (c1) return;
                    c2 = reg_edx < 4; reg_edx -= 4; if (!c2) continue;
                    reg_eax -= reg_edx; p += (s32)reg_edx; p += (s32)reg_edx; reg_eax--; p += 2;
                    break;
                }
            }
            /* .procemptyb */
            reg_edx = *tab; reg_edx--; tab++;
            for (;;) {
                int c1, c2;
                *(u32 *)p = fillcol; *(u32 *)(p + 4) = fillcol; p += 8;
                c1 = reg_eax < 4; reg_eax -= 4; if (c1) return;
                c2 = reg_edx < 4; reg_edx -= 4; if (!c2) continue;
                reg_eax -= reg_edx; p += (s32)reg_edx; p += (s32)reg_edx; reg_eax--; p += 2;
                break;
            }
        }
    } else {
        int i;
        p = PTRV(vidbuffer) + 16 * 2 + (y << 9) + (y << 6) + Z32(BackAreaAdd, 0);
        for (i = 0; i < 128; i++, p += 4) *(u32 *)p = Z32(BackAreaUnFillCol, 0);
    }
}

#define BL(v) ((u8)(v))
static void winbgcheck(u32 y, int n)
{
    u8 bl = Z8(winbg1en, n), bh = bl;
    u8 on = Z8(scrnon, 0), os = Z8(scrnon, 1), bit = (u8)(1 << n);
    if (bl & 0x0A) {
        int tobg = 1;
        if (on & bit) { if (!(os & bit)) bh = 0; }
        else { bl = 0; if (!(os & bit)) { bh = 0; tobg = 0; } }
        if (tobg) {
            if (!(Z8(winenabs, 0) & bit)) bh = 0;
            if (!(Z8(winenabm, 0) & bit)) bl = 0;
        }
    } else { bl = 0; bh = 0; }
    Z8(winbg1envalm, y + n * 256) = bl;
    Z8(winbg1envals, y + n * 256) = bh;
    bl |= bh;
    Z8(winbg1enval, y + n * 256) = bl;
    ebx = (ebx & ~0xFFFFu) | (bh << 8) | bl;
}

static void winbgcheck2(u32 y, int n)
{
    u8 bl = Z8(winbg1en, n), bh = (u8)(ebx >> 8), cl, ch;
    u32 e;
    if (!(bl & 0x0A)) bl = 0;
    else {
        cl = (bl | bh) & 0x0A;
        if (cl != 0x0A) {
            ch = bl | bh;
            e = Z32(winl1, 0);
            if (cl != 2) { ch >>= 2; e >>= 16; }
            if (!(ch & 1)) { if (!((u8)e <= (u8)(e >> 8))) bl = 0; }
            else if (!((u8)e > 0) && (u8)(e >> 8) >= 255) bl = 0;
        }
    }
    Z8(winbg1enval, y + n * 256) = bl;
    ebx = (ebx & ~0xFFu) | bl;
}

static void ne16b_core(void)
{
    u32 y;
    u32 in_ecx;
    int disable_spr = 0;
    sync_in();
    in_ecx = ecx;
    y = Z32(curypos, 0) & 0xFF; eax = y;
    if (y == 1) {
        u32 *p = (u32 *)vidmemch2s; u32 i;
        for (i = 0; i < 1024 + 512 + 256; i++) p[i] = 0xFFFFFFFFu;
        Z32(startlinet, 0) = 0; Z32(endlinet, 0) = 255; ebx = 0;
    }
    Z8(bgallchange, y) = 0; Z8(bgallchange, y + 1) = 0xFF;
    Z8(bg1change, y) = 0; Z8(bg2change, y) = 0; Z8(bg3change, y) = 0; Z8(bg4change, y) = 0;
    Z32(palchanged, 0) = 0;
    if (Z8(scaddset, 0) & 2) Z32(bgcmsung, 0) |= ebx;
    ebx = (ebx & ~0xFFu) | Z8(bg3highst, 0);
    Z8(BG3PRI, y) = (u8)ebx;
    if (Z8(BG3PRI, y - 1) != (u8)ebx) Z8(bgallchange, y) = 1;
    if (Z8(SA1Enable, 0) != 0 && Z16(scrnon, 0) == 0x1317 && Z8(scaddtype, 0) == 0) {
        Z16(scrnon, 0) = 0x1317; Z8(scaddtype, 0) = 0x44; Z8(scaddset, 0) = 0x02;
    }
    ebx = Z32(scrnon, 0);
    {
        u8 cl = Z8(scrnon, 0);
        cl &= 0x1F; cl |= 0x20; cl &= Z8(scaddtype, 0); cl &= 0x3F;
        Z8(FillSubScr, y) = 1;
        if (cl == 0) { ebx &= ~0xFF00u; Z8(FillSubScr, y) = 0; }
    }
    if (!(Z8(scaddset, 0) & 2)) ebx &= ~0xFF00u;
    Z32(bgcmsung, 0) |= ebx;
    Z32(BGMS1, y * 2) = ebx;
    if (Z16(BGMS1, y * 2 - 2) != (u16)ebx) Z8(bgallchange, y) = 1;
    Z8(clinemainsub, 0) = 0;
    if (Z16(cgram, 0) == 0) {
        u8 bl = Z8(scrnon, 0);
        bl &= 0x1F; bl |= 0xE0; bl &= Z8(scaddtype, 0);
        ebx = (ebx & ~0xFFu) | bl;
        if (bl == 0x20) { Z32(ngmsdraw, 0) = 1; Z8(FillSubScr, y) = 0; Z8(clinemainsub, 0) = 1; }
    }
    /* scroll values */
    {
        static const struct { u8 *sx, *sy, *sxl, *syl, *chg; } *dummy;
        (void)dummy;
    }
#define SCROLL(sym, arr, chg, idx) do { u16 v = Z16(sym, 0); Z16(arr, y * 2) = v; ebx = (ebx & ~0xFFFFu) | v; \
        if (Z16(arr, y * 2 - 2) != v) Z8(chg, y) = 1; (void)(idx); } while (0)
    SCROLL(bg1scrolx, BG1SXl, bg1change, 1); SCROLL(bg2scrolx, BG2SXl, bg2change, 2);
    SCROLL(bg3scrolx, BG3SXl, bg3change, 3); SCROLL(bg4scrolx, BG4SXl, bg4change, 4);
    SCROLL(bg1scroly, BG1SYl, bg1change, 1); SCROLL(bg2scroly, BG2SYl, bg2change, 2);
    SCROLL(bg3scroly, BG3SYl, bg3change, 3); SCROLL(bg4scroly, BG4SYl, bg1change, 4); /* sic: asm sets bg1change */
    {
        u8 bl = Z8(bgmode, 0) & 7;
        ebx = (ebx & ~0xFFu) | bl;
        Z8(BGMA, y) = bl;
        if (bl == 4 || Z8(BGMA, y - 1) != bl) Z8(bgallchange, y) = 1;
        ebx &= 7;
        Z8(modeused, ebx) = 1;
    }
#define PTR32(sym, arr, chg) do { ebx = Z32(sym, 0); Z32(arr, y * 2) = ebx; \
        if (Z16(arr, y * 2 - 2) != (u16)ebx) Z8(chg, y) = 1; } while (0)
    PTR32(bg1objptr, BGOPT1, bg1change); PTR32(bg2objptr, BGOPT2, bg2change);
    PTR32(bg3objptr, BGOPT3, bg3change); PTR32(bg4objptr, BGOPT4, bg4change);
    PTR32(bg1ptr, BGPT1, bg1change); PTR32(bg2ptr, BGPT2, bg2change);
    PTR32(bg3ptr, BGPT3, bg3change); PTR32(bg4ptr, BGPT4, bg4change);
    PTR32(bg1ptrx, BGPT1X, bg1change); PTR32(bg2ptrx, BGPT2X, bg2change);
    PTR32(bg3ptrx, BGPT3X, bg3change); PTR32(bg4ptrx, BGPT4X, bg4change);
    PTR32(bg1ptry, BGPT1Y, bg1change); PTR32(bg2ptry, BGPT2Y, bg2change);
    PTR32(bg3ptry, BGPT3Y, bg3change); PTR32(bg4ptry, BGPT4Y, bg4change);
    ebx = Z32(forceblnk, 0);
    if (Z8(forceblnk, 0) == 0) Z32(scfbl, 0) = 0;
    Z8(BGFB, y) = (u8)ebx;
    if (Z8(BGFB, y - 1) != (u8)ebx) Z8(bgallchange, y) = 1;
    if (Z8(interlval, 0) & 0x40) Z8(bgallchange, y) = 1;
    ebx = Z32(mode7A, 0); Z32(mode7ab, y * 4) = ebx;
    ebx = Z32(mode7C, 0); Z32(mode7cd, y * 4) = ebx;
    ebx = Z32(mode7X0, 0); Z32(mode7xy, y * 4) = ebx;
    ebx = Z32(mode7set, 0); Z32(mode7st, y) = ebx;
#define T16(sym, arr, chg) do { ebx = Z32(sym, 0); Z32(arr, y) = ebx; if (Z8(arr, y - 1) != (u8)ebx) Z8(chg, y) = 1; } while (0)
    T16(BG116x16t, t16x161, bg1change); T16(BG216x16t, t16x162, bg2change);
    T16(BG316x16t, t16x163, bg3change); T16(BG416x16t, t16x164, bg4change);
    if (Z8(bgmode, 0) == 7 && (Z8(interlval, 0) & 0x40)) Z32(ngextbg, 0) = 1;
    ebx = Z32(mosaicon, 0); Z32(mosenng, y) = ebx;
    ebx = Z32(mosaicsz, 0); Z32(mosszng, y) = ebx;
    ebx = Z32(interlval, 0); Z32(intrlng, y) = ebx;
    ecx = in_ecx;
    c_setpalette16bng();
    if (Z32(palchanged, 0) == 1) {
        if (y < 112) Z32(startlinet, 0) = y;
        else if (Z32(endlinet, 0) >= 255) Z32(endlinet, 0) = y;
    }
    ebx = Z32(cpalptrng, 0) + P32(PTRV(vbufdptr));
    Z32(cpalval, y * 4) = ebx;
    ebx = (ebx & ~0xFFu) | Z8(scaddtype, 0);
    Z8(scadtng, y) = Z8(scaddtype, 0);
    if (Z8(scadtng, y - 1) != Z8(scaddtype, 0)) Z8(bgallchange, y) = 1;
    ebx = (ebx & ~0xFFu) | Z8(scaddset, 0);
    Z8(scadsng, y) = Z8(scaddset, 0);
    if (Z8(scadsng, y - 1) != Z8(scaddset, 0)) Z8(bgallchange, y) = 1;

    /* windowing */
    Z8(bgwinchange, y) = 0;
    if (Z8(disableeffects, 0) == 1) {
        Z8(winbg1enval, y) = 0; Z8(winbg2enval, y) = 0; Z8(winbg3enval, y) = 0; Z8(winbg4enval, y) = 0; Z8(winbgobjenval, y) = 0;
    } else {
        u32 sv_ecx = ecx, sv_edx = edx;
        winbgcheck(y, 0); winbgcheck(y, 1); winbgcheck(y, 2); winbgcheck(y, 3); winbgcheck(y, 4);
        winbgcheck2(y, 5);
        ebx = Z32(winlogica, 0); Z32(winlogicaval, y * 2) = ebx;
        if (Z16(winlogicaval, y * 2 - 2) != (u16)ebx) Z8(bgwinchange, y) = 1;
        ebx = Z32(winl1, 0); Z32(winboundary, y * 4) = ebx;
        if (Z32(winboundary, y * 4 - 4) != ebx) Z8(bgwinchange, y) = 1;
        if (Z8(winbg1enval, y + 4 * 256) != 0) {
            u16 dxv = (u16)(Z8(winobjen, 0) | ((Z8(winlogicb, 0) & 3) << 8));
            int sw;  /* 0 changed, 1 usecurrent, 2 notchanged */
            if (Z32(objwlrpos, y * 4 - 4) == 0xFFFFFFFFu) sw = 0;
            else {
                int changedb = 1;
                if (Z32(objwlrpos, y * 4 - 4) == ebx && Z16(objwen, y * 2 - 2) == dxv) { changedb = 0; sw = 2; }
                if (changedb) {
                    if (Z32(objwlrpos, y * 4) != ebx || Z16(objwen, y * 2) != dxv) sw = 0;
                    else if (Z32(objclineptr, y * 4) > Z32(CSprWinPtr, 0)) sw = 1;
                    else sw = 0;
                }
            }
            edx = (edx & ~0xFFFFu) | dxv;
            if (sw == 0) {
                u32 c, ax, ed, *tb; u8 *pp;
                Z32(objwlrpos, y * 4) = ebx; Z16(objwen, y * 2) = dxv;
                Z32(nglogicval, 0) = (Z32(nglogicval, 0) & ~0xFFu) | (Z8(winlogicb, 0) & 3);
                ebx = 4 * 256 + y; Z32(ngwinen, 0) = 0;
                ecx = sv_ecx; /* BuildWindow2 preserves ecx/edx/esi/edi/ebp */
                eax = y; acall(BuildWindow2);
                if (Z32(ngwinen, 0) == 0) disable_spr = 1;
                else {
                    c = Z32(CSprWinPtr, 0) + 260; Z32(CSprWinPtr, 0) = c; Z32(objclineptr, y * 4) = c;
                    pp = PTRV(ngwinptr) + c; pp--;
                    tb = (u32 *)ngwintable; ax = 256;
                    for (;;) {
                        int c1, c2;
                        ed = *tb++;
                        if (ed != 0) {
                            ed--;
                            for (;;) {
                                *(u32 *)pp = 0; pp += 4; c1 = ax < 4; ax -= 4; if (c1) goto swdone;
                                c2 = ed < 4; ed -= 4; if (!c2) continue;
                                ax -= ed; pp += (s32)ed; ax--; pp++; break;
                            }
                        }
                        ed = *tb; ed--; tb++;
                        for (;;) {
                            *(u32 *)pp = 0x01010101u; pp += 4; c1 = ax < 4; ax -= 4; if (c1) goto swdone;
                            c2 = ed < 4; ed -= 4; if (!c2) continue;
                            ax -= ed; pp += (s32)ed; ax--; pp++; break;
                        }
                    }
                swdone:
                    eax = y;
                }
            } else if (sw == 1) {
                Z32(CSprWinPtr, 0) = Z32(objclineptr, y * 4);
                if (Z32(CSprWinPtr, 0) == 0xFFFFFFFFu) disable_spr = 1;
            } else {
                Z32(objwlrpos, y * 4) = ebx; Z16(objwen, y * 2) = dxv;
                ebx = Z32(objclineptr, y * 4 - 4); Z32(objclineptr, y * 4) = ebx;
                if (ebx == 0xFFFFFFFFu) disable_spr = 1;
            }
            if (disable_spr) {
                Z32(objclineptr, y * 4) = 0xFFFFFFFFu;
                Z8(winbg1enval, y + 4 * 256) = 0; Z8(winbg1envals, y + 4 * 256) = 0; Z8(winbg1envalm, y + 4 * 256) = 0;
            }
        }
        ecx = sv_ecx; edx = sv_edx;
    }
    /* colour math fixed colour */
    ebx = Z32(coladdr, -1);
    ebx = (ebx & ~0xFFu) | Z8(vidbright, 0);
    if (Z32(Prevcoladdr, 0) != ebx) {
        u32 bx = 0;
        Z32(Prevcoladdr, 0) = ebx;
        bx = (bx + scalec(Z8(coladdr, 0), Z8(vidbright, 0), Z32(ngrposng, 0))) & 0xFFFF;
        bx = (bx + scalec(Z8(coladdg, 0), Z8(vidbright, 0), Z32(nggposng, 0))) & 0xFFFF;
        bx = (bx + scalec(Z8(coladdb, 0), Z8(vidbright, 0), Z32(ngbposng, 0))) & 0xFFFF;
        ebx = bx;
        Z16(ColResult, 0) = (u16)bx; Z16(ColResult, 2) = (u16)bx;
    }
    if (Z16(ColResult, 0) != 0) { if (Z8(FillSubScr, y) != 0) Z8(FillSubScr, y) |= 2; }
    /* back area */
    Z32(ngwinen, 0) = 0;
    {
        u32 sv_ecx = ecx, sv_edx = edx;
        u32 un, fl, cl8, e_bx, e_dx;
        if (Z8(winbg1enval, y + 5 * 256) != 0) {
            Z32(nglogicval, 0) = (Z32(nglogicval, 0) & ~0xFFu) | ((Z8(winlogicb, 0) >> 2) & 3);
            ebx = 5 * 256 + y; eax = y; ecx = sv_ecx; acall(BuildWindow2);
        }
        Z32(BackAreaAdd, 0) = 0;
        if (Z8(clinemainsub, 0) == 1) {
            e_bx = Z32(ColResult, 0) | Z32(UnusedBit, 0); e_dx = Z32(UnusedBit, 0); cl8 = Z8(scaddset, 0);
        } else {
            u8 *b = PTRV(vbufdptr) + Z32(cpalptrng, 0);
            u32 w = *(u16 *)b;
            e_bx = (w << 16) | w; e_dx = 0; cl8 = Z8(scaddset, 0) >> 2;
            if (Z8(scaddtype, 0) & 0x20) { e_bx |= Z32(UnusedBit, 0); e_dx = Z32(UnusedBit, 0); }
        }
#define PICK(cl8, e_bx, e_dx, UN, FL) do { \
        if (!((cl8) & 0x10) && !((cl8) & 0x20)) { UN = e_bx; FL = e_bx; } \
        else if ((cl8) & 0x10) { if ((cl8) & 0x20) { e_bx = 0; UN = e_dx; FL = e_dx; } else { UN = e_dx; FL = e_bx; } } \
        else { UN = e_bx; FL = e_dx; } } while (0)
        PICK(cl8, e_bx, e_dx, un, fl);
        Z32(BackAreaUnFillCol, 0) = un; Z32(BackAreaFillCol, 0) = fl;
        if (Z32(ngwinen, 0) == 0) Z32(BackAreaFillCol, 0) = Z32(BackAreaUnFillCol, 0);
        if (Z8(forceblnk, 0) != 0) { Z32(BackAreaUnFillCol, 0) = 0; Z32(BackAreaFillCol, 0) = 0; }
        BackAreaFill(y);
        if (Z8(FillSubScr, y) & 1) {
            Z32(BackAreaAdd, 0) = 75036 * 2;
            e_bx = Z32(ColResult, 0); e_dx = Z32(UnusedBit, 0);
            if (Z8(scaddset, 0) & 2) e_bx |= Z32(UnusedBit, 0);
            cl8 = Z8(scaddset, 0);
            PICK(cl8, e_bx, e_dx, un, fl);
            Z32(BackAreaUnFillCol, 0) = un; Z32(BackAreaFillCol, 0) = fl;
            if (Z32(ngwinen, 0) == 0) Z32(BackAreaFillCol, 0) = Z32(BackAreaUnFillCol, 0);
            BackAreaFill(y);
        }
        ecx = sv_ecx; edx = sv_edx;
        (void)e_bx;
    }
    Z8(SpecialLine, y) = 0;
    {
        int hr = 0, no512 = 1;
        if (Z8(smallscreenon, 0) == 1 || Z8(ScreenScale, 0) == 1 || Z8(scanlines, 0) != 0) no512 = 1;
        else if (Z8(bgmode, 0) < 7) hr = 1;
        else if ((Z8(interlval, 0) & 0x40) == 0 && Z8(Mode7HiRes16b, 0) == 1) hr = 1;
        if (hr && Z8(res640, 0) != 0 && Z8(bgmode, 0) >= 5) {
            u8 *src;
            no512 = 0;
            Z8(SpecialLine, y) = 3;
            if (Z8(bgmode, 0) != 7) Z8(SpecialLine, y) = 2;
            Z32(hiresstuff, 0) = 1;
            src = PTRV(vidbuffer) + 16 * 2 + (y << 9) + (y << 6);
            memcpy(src + 75036 * 4, src, 512);
            if (Z8(FillSubScr, y) & 1) memcpy(src + 75036 * 2 + 75036 * 4, src + 75036 * 2, 512);
        }
        (void)no512;
    }
    if (Z8(scanlines, 0) == 0 && (Z8(interlval, 0) & 1)) Z8(SpecialLine, y) |= 4;
    ebx = Z32(sprleftpr, y * 4);
    if (ebx == 0x1 || ebx == 0x100 || ebx == 0x10000 || ebx == 0x1000000) {
        Z32(sprprdrn, 0) |= ebx; Z32(sprleftpr, y * 4) |= 0x80000000u;
    } else Z32(sprprdrn, 0) |= ebx;
    ebx = 0;
    eax = y;
    sync_out();
}

/* ---- mode / diff plumbing ---- */
static int ppu_mode(void)
{
    static int m = -1;
    if (m < 0) { m = getenv("ZS_PPU_ASM") ? 0 : getenv("ZS_PPU_DIFF") ? 2 : 1; }
    return m;
}
static void regions(void)
{
    di_ext(PTRV(vbufaptr), 512 * 296 * 4 + 512 * 296 + 64);
    di_ext(PTRV(vbufeptr), 288 * 2 * 256 + 64);
    di_ext(PTRV(vbufdptr), 1024 * 296);
    di_ext(PTRV(ngwinptrb), 256 * 224 + 64);
    di_ext(PTRV(spritetablea), 256 * 512 + 64);
    di_ext(PTRV(vcache2bs), 65536 * 4 * 4 + 64);
    di_ext(PTRV(vcache4bs), 65536 * 4 * 2 + 64);
    di_ext(PTRV(vcache8bs), 65536 * 4 + 64);
}
static void diff_setup(void) { static int d; if (d) return; d = 1; di_region_hook = regions; di_exclude(&G, sizeof G); }
HID FORCE void zs_ne16b_c(void)
{
    static unsigned long n;
    int m = ppu_mode();
    if (m == 0) { zs_mcall(newengine16b_asm); return; }
    if (m == 2) {
        diff_setup();
        if (n++ % (getenv("DI_EVERY") ? atoi(getenv("DI_EVERY")) : 7) == 0) { di_run("newengine16b", newengine16b_asm, ne16b_core, 0x7F); return; }
    }
    ne16b_core();
}
