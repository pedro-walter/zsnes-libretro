#include <stdint.h>
/*
 * StartDrawNewGfx16b (video/newgfx16.asm + the Proc* macros of newgfx16.mac): the new graphics engine's per-frame drawing
 * orchestration.  For every layer (sub screen pass, then main screen pass) it walks the scanlines and decides, per line,
 * between the tile-by-tile and the line-by-line drawer, then calls the pixel level drawers (still lifted asm, called with the
 * exact register contents the macros leave).  The macros are transcribed instruction for instruction on the guest register
 * file (LG), so that registers the drawers inherit from earlier code (ecx/edx/eax upper bits ...) are identical.
 */
#include "zl.h"
#include <stdint.h>
#define A(s) ((u32)ZO_##s)
#define L8(a)  (*(volatile u8 *)(uintptr_t)(u32)(a))
#define L16(a) (*(volatile u16 *)(uintptr_t)(u32)(a))
#define L32(a) (*(volatile u32 *)(uintptr_t)(u32)(a))
#define Z8(s, o)  L8(A(s) + (u32)(o))
#define Z16(s, o) L16(A(s) + (u32)(o))
#define Z32(s, o) L32(A(s) + (u32)(o))
#define EAX LG.eax
#define EBX LG.ebx
#define ECX LG.ecx
#define EDX LG.edx
#define ESI LG.esi
#define EBP LG.ebp
static inline void set_bl(u32 v) { EBX = (EBX & ~0xFFu) | (v & 0xFF); }
static inline void set_al(u32 v) { EAX = (EAX & ~0xFFu) | (v & 0xFF); }
static inline void set_cl(u32 v) { ECX = (ECX & ~0xFFu) | (v & 0xFF); }
static inline void set_ax(u32 v) { EAX = (EAX & ~0xFFFFu) | (v & 0xFFFF); }
static inline void set_dx(u32 v) { EDX = (EDX & ~0xFFFFu) | (v & 0xFFFF); }
#define BX ((u16)EBX)
#define LINE_STRIDE (288u * 2)
static int vd_trace, vd_calls_n;
static const struct { u32 id; const char *n; } vd_names[] = {
    { ZID_drawbg1line16b, "drawbg1line16b" },
    { ZID_drawbg1tile16b, "drawbg1tile16b" },
    { ZID_drawbg1linepr116b, "drawbg1linepr116b" },
    { ZID_drawbg1tilepr116b, "drawbg1tilepr116b" },
    { ZID_drawbg2line16b, "drawbg2line16b" },
    { ZID_drawbg2tile16b, "drawbg2tile16b" },
    { ZID_drawbg2linepr116b, "drawbg2linepr116b" },
    { ZID_drawbg2tilepr116b, "drawbg2tilepr116b" },
    { ZID_drawbg3line16b, "drawbg3line16b" },
    { ZID_drawbg3tile16b, "drawbg3tile16b" },
    { ZID_drawbg3linepr116b, "drawbg3linepr116b" },
    { ZID_drawbg3tilepr116b, "drawbg3tilepr116b" },
    { ZID_drawbg4line16b, "drawbg4line16b" },
    { ZID_drawbg4tile16b, "drawbg4tile16b" },
    { ZID_drawbg4linepr116b, "drawbg4linepr116b" },
    { ZID_drawbg4tilepr116b, "drawbg4tilepr116b" },
    { ZID_drawsprng16b, "drawsprng16b" },
    { ZID_drawsprng16bhr, "drawsprng16bhr" },
    { ZID_drawmode7win16b, "drawmode7win16b" },
    { ZID_drawmode7win16bd, "drawmode7win16bd" },
    { ZID_processmode7hires16b, "processmode7hires16b" },
    { ZID_processmode7hires16bd, "processmode7hires16bd" },
    { ZID_drawmode7ngextbg16b, "drawmode7ngextbg16b" },
    { ZID_drawmode7ngextbg216b, "drawmode7ngextbg216b" },
};
static void leaf(u32 id)
{
    if (vd_trace) { const char *nm = "?"; for (unsigned i = 0; i < sizeof vd_names / sizeof *vd_names; i++) if (vd_names[i].id == id) nm = vd_names[i].n;
        fprintf(stderr, "L %s %x %x %x %x %x\n", nm, EBX, ESI - Z32(vidbuffer, 0), EAX, ECX, EDX); }
    zl_mcall(id);
}

/* ngsub / ngmain: return 1 when the line is to be skipped */
static int ng_skip(int main, u32 ebx, u32 m)
{
    if (!main) {
        if (!(L32(A(BGMS1) + ebx * 2 + 1) & m)) return 1;
        if (L32(A(BGMS1) + ebx * 2) & m) return 1;
        if (Z8(FillSubScr, ebx) & 1) return 1;
        return 0;
    }
    if (L32(A(BGMS1) + ebx * 2) & m) return 0;
    if (!(Z8(FillSubScr, ebx) & 1)) return 1;
    if (!(L32(A(BGMS1) + ebx * 2 + 1) & m)) return 1;
    return 0;
}

/* Only esi is saved around the calls (push esi / pop esi); ebx is whatever the drawer leaves, exactly as in the asm: some drawers
 * return with a clobbered ebx and the macros then carry on with it. */
static u32 call_line(u32 fn, u32 esi, u32 ebx)    /* push esi; eax=cpalval[ebx]; CPalPtrng=eax; call fn; pop esi */
{
    EAX = L32(A(cpalval) + ebx * 4); Z32(CPalPtrng, 0) = EAX;
    leaf(fn);
    ESI = esi;
    return EBX;
}
static u32 call_plain(u32 fn, u32 esi, u32 ebx)   /* push esi; call fn; pop esi */
{
    (void)ebx;
    leaf(fn);
    ESI = esi;
    return EBX;
}

/* Procbgpr016b (bg3 = 0) / Procbg3pr016b (bg3 = 1) */
static void proc_bg_pr0(int bg, u32 linefn, u32 tilefn, int main, u32 mask, int bg3)
{
    u32 esi = Z32(vidbuffer, 0) + 16 * 2, ebx = 0;
    ESI = esi; EBX = 0;
    for (;;) {
        ebx = EBX;
        if (Z8(BGFB, ebx) != 0) goto nogo;
        if (ng_skip(main, ebx, mask)) goto nogo;
        ECX = L32(A(BG1SYl) + ebx * 2 + bg * 512) & 0xFFFF; ECX += ebx;
        if ((ECX & 7) == 0) goto tilecheck;
    proclinebyline:
        ebx = call_line(linefn, esi, ebx);
    nogo:
        ebx++; esi += LINE_STRIDE; EBX = ebx; ESI = esi;
        if (Z16(resolutn, 0) != (u16)ebx) continue;
        return;
    tilecheck:
        if (Z32(startlinet, 0) >= ebx) goto proclinebyline;
        if (Z32(endlinet, 0) <= ebx) goto proclinebyline;
        if (L32(A(bgwinchange) + ebx) & 0xFFFFFF00u) goto linebyline;
        if (L32(A(bgwinchange) + ebx + 4) == 0) goto tilebytile;
    linebyline:
        if (L32(A(winbg1enval) + ebx + bg * 256) & 0x0A0A0A0Au) goto proclinebyline;
        if (L32(A(winbg1enval) + ebx + bg * 256 + 4) & 0x0A0A0A0Au) goto proclinebyline;
    tilebytile:
        if (ebx >= Z32(reslbyl, 0)) goto proclinebyline;
        if (L32(A(bgallchange) + ebx) & 0xFFFFFF00u) goto proclinebyline;
        if (L32(A(bgallchange) + ebx + 4)) goto proclinebyline;
        if (L32(A(bg1change) + ebx + bg * 256) & 0xFFFFFF00u) goto proclinebyline;
        if (L32(A(bg1change) + ebx + bg * 256 + 4)) goto proclinebyline;
        if (!bg3) {
            if (Z8(BGMA, ebx) >= 5) goto proclinebyline;
            if (Z8(BGMA, ebx) == 2) goto proclinebyline;
        } else {
            EAX = L32(A(BG3PRI) + ebx);
            if (EAX != L32(A(BG3PRI) + ebx + 4)) goto proclinebyline;
        }
        if (Z8(mosenng, ebx) & (1u << bg)) {
            if (Z8(mosszng, ebx) == 0) goto nomosaic;
            goto proclinebyline;
        }
    nomosaic:
        set_ax(Z16(resolutn, 0)); set_ax(EAX - 8);
        if ((u16)ebx >= (u16)EAX) goto proclinebyline;
        ebx = call_plain(tilefn, esi, ebx);
        ebx += 8; esi += LINE_STRIDE * 8; EBX = ebx; ESI = esi;
        if (Z16(resolutn, 0) > (u16)ebx) continue;
        return;
    }
}

/* Procbgpr116b / Procbg3pr116b (kind 1) / Procbg3pr1b16b (kind 2) */
static void proc_bg_pr1(int kind, u32 linefn, u32 tilefn, u32 prarr, int main, u32 mask)
{
    u32 esi = Z32(vidbuffer, 0) + 16 * 2, ebx = 0;
    ESI = esi; EBX = 0;
    for (;;) {
        ebx = EBX;
        if (Z8(BGFB, ebx) != 0) goto nogo;
        if (ng_skip(main, ebx, mask)) goto nogo;
        if (kind == 1) { if (Z8(BGMA, ebx) == 1 && Z8(BG3PRI, ebx) != 0) goto nogo; }
        else if (kind == 2) { if (Z8(BGMA, ebx) != 1) goto nogo; if (Z8(BG3PRI, ebx) == 0) goto nogo; }
        if (L8(prarr + ebx) == 1) goto tile;
        ebx = call_line(linefn, esi, ebx);
    nogo:
        ebx++; esi += LINE_STRIDE; EBX = ebx; ESI = esi;
        if (Z16(resolutn, 0) != (u16)ebx) continue;
        return;
    tile:
        ebx = call_line(tilefn, esi, ebx);
        ebx += 8; esi += LINE_STRIDE * 8; EBX = ebx; ESI = esi;
        if (Z16(resolutn, 0) > (u16)ebx) continue;
        return;
    }
}

/* Procsprng16b (kind 0), Procsprng0116b (kind 1), Procsprng23456716b (kind 2) */
static void proc_spr(int kind, int main, u32 mask)
{
    u32 esi = Z32(vidbuffer, 0) + 16 * 2 + LINE_STRIDE, ebx = 1;
    ESI = esi; EBX = 1;
    for (;;) {
        ebx = EBX;
        if (Z8(BGFB, ebx) != 0) goto nogo;
        if (ng_skip(main, ebx, mask)) goto nogo;
        if (kind == 1 && Z8(BGMA, ebx) > 1) goto nogo;
        if (kind == 2 && Z8(BGMA, ebx) <= 1) goto nogo;
        EAX = 0; set_al(Z8(sprtlng, ebx));
        set_cl(Z8(sprlefttot, ebx));
        Z32(csprival, 0) = EAX;
        Z8(sprtlng, ebx)++;
        if (!(Z8(sprleftpr, ebx * 4 + (EAX & 0xFF)) & 1)) goto nogo;
        if ((ECX & 0xFF) == 0) goto nogo;
        if (Z8(SpecialLine, ebx) & 2) { set_cl(Z8(sprlefttot, ebx)); leaf(ZID_drawsprng16bhr); }
        else leaf(ZID_drawsprng16b);
        ESI = esi; ebx = EBX;
    nogo:
        ebx++; esi += LINE_STRIDE; EBX = ebx; ESI = esi;
        if (Z16(resolutn, 0) != (u16)ebx) continue;
        return;
    }
}

/* the common mosaic / matrix set-up of the three mode 7 macros; returns with eax, edx loaded and ebx = original line */
static void m7_setup(u32 *pesi, u32 ebx)
{
    u32 esi = *pesi;
    EBP = L32(A(cpalval) + ebx * 4);
    Z8(curmosaicsz, 0) = 1;
    ECX = 0;
    if (Z8(mosenng, ebx) & 1) {
        u32 cl = Z8(mosszng, ebx);
        if (cl != 0) {
            u32 q;
            cl = (cl + 1) & 0xFF; set_cl(cl);
            Z8(curmosaicsz, 0) = (u8)cl;
            EDX = 0; EAX = ebx;
            q = (EAX & 0xFFFF) / cl;   /* div cl: al = ax / cl, ah = ax % cl */
            set_ax((q & 0xFF) | (((EAX & 0xFFFF) % cl) << 8));
            EDX = 0;
            set_ax((EAX & 0xFF) * cl);   /* mul cl */
            ebx = EAX;
        }
    }
    EBX = ebx;
    EDX = L32(A(mode7ab) + ebx * 4); Z32(mode7A, 0) = EDX;
    EDX = L32(A(mode7cd) + ebx * 4); Z32(mode7C, 0) = EDX;
    EDX = L32(A(mode7xy) + ebx * 4); Z32(mode7X0, 0) = EDX;
    EDX = L32(A(mode7st) + ebx); Z8(mode7set, 0) = (u8)EDX;
    EDX = 0; EAX = ebx;
    if (Z8(mode7set, 0) & 2) { EAX = 261; EAX -= ebx; }
    set_dx(L16(A(BG1SXl) + ebx * 2));
    set_ax(EAX + L16(A(BG1SYl) + ebx * 2));
    Z32(curvidoffset, 0) = esi;
    *pesi = esi;
}
static void m7_save(void)
{
    u32 ebx;
    Z8(winon, 0) = 0;
    ebx = Z32(mode7A, 0); Z32(Mode7BackA, 0) = ebx;
    ebx = Z32(mode7C, 0); Z32(Mode7BackC, 0) = ebx;
    ebx = Z32(mode7X0, 0); Z32(Mode7BackX0, 0) = ebx;
    EBX = ebx; set_bl(Z8(mode7set, 0)); Z32(Mode7BackSet, 0) = EBX;
}
static void m7_restore(void)
{
    u32 ebx;
    ebx = Z32(Mode7BackA, 0); Z32(mode7A, 0) = ebx;
    ebx = Z32(Mode7BackC, 0); Z32(mode7C, 0) = ebx;
    ebx = Z32(Mode7BackX0, 0); Z32(mode7X0, 0) = ebx;
    EBX = ebx = Z32(Mode7BackSet, 0); Z8(mode7set, 0) = (u8)ebx;
}

/* ProcMode7ng16b */
static void proc_mode7(int main, u32 mask)
{
    u32 esi, ebx;
    m7_save();
    esi = Z32(vidbuffer, 0) + 16 * 2 + LINE_STRIDE; ESI = esi; EBX = 1;
    for (;;) {
        ebx = EBX;
        if (Z8(BGFB, ebx) != 0) goto nogo;
        if (Z8(BGMA, ebx) != 7) goto nogo;
        if (Z8(intrlng, ebx) & 0x40) goto nogo;
        if (ng_skip(main, ebx, mask)) goto nogo;
        {
            u32 line = ebx, eax, edx;
            m7_setup(&esi, ebx);
            ebx = line; EBX = ebx;
            eax = EAX; edx = EDX;
            if (Z8(scadsng, ebx) & 1) leaf(ZID_drawmode7win16bd); else leaf(ZID_drawmode7win16b);
            EBX = ebx; ESI = esi; EDX = edx; EAX = eax;
            if (Z8(Mode7HiRes16b, 0) != 0 && Z8(scanlines, 0) == 0) {
                if (Z8(scadsng, ebx) & 1) leaf(ZID_processmode7hires16bd); else leaf(ZID_processmode7hires16b);
                ebx = EBX;
            }
        }
        ESI = esi;
    nogo:
        ebx++; esi += LINE_STRIDE; EBX = ebx; ESI = esi;
        if (Z16(resolutn, 0) != (u16)ebx) continue;
        break;
    }
    m7_restore();
}

/* ProcMode7ngextbg16b (variant 1) / ProcMode7ngextbg216b (variant 2) */
static void proc_mode7_ext(int variant, int main, u32 mask)
{
    u32 esi, ebx;
    m7_save();
    esi = Z32(vidbuffer, 0) + 16 * 2 + LINE_STRIDE; ESI = esi; EBX = 1;
    for (;;) {
        ebx = EBX;
        if (variant == 1) L8(esi - 1) = 0;
        if (Z8(BGFB, ebx) != 0) goto nogo;
        if (Z8(BGMA, ebx) != 7) goto nogo;
        if (ng_skip(main, ebx, mask)) goto nogo;
        if (!(Z8(intrlng, ebx) & 0x40)) goto nogo;
        if (variant == 1) L8(esi - 1) = 1;
        {
            u32 line = ebx;
            m7_setup(&esi, ebx);                 /* ebx stays the mosaic-adjusted line here (no pop before the call) */
            ESI = esi;
            leaf(variant == 1 ? ZID_drawmode7ngextbg16b : ZID_drawmode7ngextbg216b);
            esi = ESI = esi; ebx = EBX = line;
        }
    nogo:
        ebx++; esi += LINE_STRIDE; EBX = ebx; ESI = esi;
        if (Z16(resolutn, 0) != (u16)ebx) continue;
        break;
    }
    m7_restore();
}

#define NGSUB 0
#define NGMAIN 1
static void layers(int main)
{
    u32 scrndis_, cm;
#define SD(b) (Z8(scrndis, 0) & (b))
#define CM(m) (Z32(bgcmsung, 0) & (m))
    const u32 m_bg4 = main ? 0x808 : 0x800, m_bg3 = main ? 0x404 : 0x400, m_bg2 = main ? 0x202 : 0x200, m_bg1 = main ? 0x101 : 0x100;
    const u32 m_spr = main ? 0x1010 : 0x1000, m_m7 = main ? 0x101 : 0x300, m_ext = main ? 0x303 : 0x300;
    (void)scrndis_; (void)cm;
    /* BG4 pr0 */
    if (!SD(8) && CM(m_bg4)) { Z32(mode0ads, 0) = 0x60606060; proc_bg_pr0(3, ZID_drawbg4line16b, ZID_drawbg4tile16b, main, 8, 0); }
    /* BG3 pr0 */
    if (!SD(4) && CM(m_bg3)) { Z32(mode0ads, 0) = 0x40404040; proc_bg_pr0(2, ZID_drawbg3line16b, ZID_drawbg3tile16b, main, 4, 1); }
    /* sprites mode 0-1 */
    if (!SD(0x10) && (Z32(modeused, 0) & 0x101) && CM(m_spr)) proc_spr(1, main, 0x10);
    /* BG4 pr1 */
    if (!SD(8) && CM(m_bg4)) {
        EAX = Z32(bg4totng, 0);
        if (EAX != Z32(bg4drwng, 0)) { Z32(mode0ads, 0) = 0x60606060; proc_bg_pr1(0, ZID_drawbg4linepr116b, ZID_drawbg4tilepr116b, A(prdata), main, 8); }
    }
    /* BG3 pr1 (the main pass tests only bit 2 of bgcmsung here) */
    if (!SD(4) && CM(main ? 4 : 0x400)) {
        EAX = Z32(bg3totng, 0);
        if (EAX != Z32(bg3drwng, 0)) { Z32(mode0ads, 0) = 0x40404040; proc_bg_pr1(1, ZID_drawbg3linepr116b, ZID_drawbg3tilepr116b, A(prdatc), main, 4); }
    }
    /* sprites mode 0-1 again */
    if (!SD(0x10) && (Z32(modeused, 0) & 0x101) && CM(m_spr)) proc_spr(1, main, 0x10);
    /* BG2 pr0 */
    if (!SD(2) && CM(m_bg2)) { Z32(mode0ads, 0) = 0x20202020; proc_bg_pr0(1, ZID_drawbg2line16b, ZID_drawbg2tile16b, main, 2, 0); }
    /* EXTBG */
    if (Z8(ngextbg, 0) != 0 && CM(m_ext)) proc_mode7_ext(1, main, 3);
    /* sprites mode 2-7 */
    if (!SD(0x10) && ((Z32(modeused, 0) & 0x01010000) || (Z32(modeused, 4) & 0x01010101)) && CM(m_spr)) proc_spr(2, main, 0x10);
    /* BG1 pr0 */
    if (!SD(1) && CM(m_bg1)) { Z32(mode0ads, 0) = 0; proc_bg_pr0(0, ZID_drawbg1line16b, ZID_drawbg1tile16b, main, 1, 0); }
    /* mode 7 */
    if (Z8(modeused, 7) != 0 && CM(m_m7)) proc_mode7(main, 1);
    /* sprites (all) */
    if (!SD(0x10) && CM(m_spr)) proc_spr(0, main, 0x10);
    /* BG2 pr1 and EXTBG 2: the sub screen pass runs BG2 first, the main screen pass EXTBG 2 first */
    if (!main) {
        if (!SD(2) && CM(m_bg2)) {
            EAX = Z32(bg2totng, 0);
            if (EAX != Z32(bg2drwng, 0)) { Z32(mode0ads, 0) = 0x20202020; proc_bg_pr1(0, ZID_drawbg2linepr116b, ZID_drawbg2tilepr116b, A(prdata), main, 2); }
        }
        if (Z8(ngextbg, 0) != 0 && CM(m_ext)) proc_mode7_ext(2, main, 2);
    } else {
        if (Z8(ngextbg, 0) != 0 && CM(m_ext)) proc_mode7_ext(2, main, 2);
        if (!SD(2) && CM(m_bg2)) {
            EAX = Z32(bg2totng, 0);
            if (EAX != Z32(bg2drwng, 0)) { Z32(mode0ads, 0) = 0x20202020; proc_bg_pr1(0, ZID_drawbg2linepr116b, ZID_drawbg2tilepr116b, A(prdata), main, 2); }
        }
    }
    /* sprites mode 2-7 */
    if (!SD(0x10) && ((Z32(modeused, 0) & 0x01010000) || (Z32(modeused, 4) & 0x01010101)) && CM(m_spr)) proc_spr(2, main, 0x10);
    /* BG1 pr1 */
    if (!SD(1) && CM(m_bg1)) {
        EAX = Z32(bg1totng, 0);
        if (EAX != Z32(bg1drwng, 0)) { Z32(mode0ads, 0) = 0; proc_bg_pr1(0, ZID_drawbg1linepr116b, ZID_drawbg1tilepr116b, A(prdatb), main, 1); }
    }
    /* sprites (all) */
    if (!SD(0x10) && CM(m_spr)) proc_spr(0, main, 0x10);
    /* BG3 pr1b */
    if (!SD(4) && Z8(modeused, 1) != 0 && CM(m_bg3)) {
        EAX = Z32(bg3totng, 0);
        if (EAX != Z32(bg3drwng, 0)) { Z32(mode0ads, 0) = 0x40404040; proc_bg_pr1(2, ZID_drawbg3linepr116b, ZID_drawbg3tilepr116b, A(prdatc), main, 4); }
    }
}

void zl_ProcessTransparencies(void);
void zl_MainScreenClip(void);

/* NEWSYM StartDrawNewGfx16b: edx, esi, edi, ebp are preserved; eax, ebx, ecx are cleared */
void zl_StartDrawNewGfx16b(void)
{
    u32 saved_edx = EDX, saved_esi = ESI, saved_edi = LG.edi, saved_ebp = EBP;
    int i;
    { const char *e = getenv("ZL_VD_TRACE"); vd_trace = e && ++vd_calls_n == atoi(e); }
    Z8(WindowRedraw, 0) = 1;
    Z32(endlinet, 0) -= 8;
    if (Z32(scfbl, 0) != 0) goto dontdraw;
    {   /* copy data to the sprite table */
        u32 eax = Z32(spritetablea, 0), edi = A(sprtbng);
        for (i = 0; i < 256; i++, eax += 512, edi += 4) L32(edi) = eax;
        for (i = 0; i < 64; i++) L32(A(sprtlng) + i * 4) = 0;
        EAX = 0; ECX = 0; LG.edi = A(sprtlng) + 256; EBX = A(sprlefttot) + 256;
    }
    Z8(firstdrawn, 0) = 1;
    Z32(bg1totng, 0) = 0; Z32(bg2totng, 0) = 0; Z32(bg3totng, 0) = 0; Z32(bg4totng, 0) = 0;
    Z32(bg1drwng, 0) = 0; Z32(bg2drwng, 0) = 0; Z32(bg3drwng, 0) = 0; Z32(bg4drwng, 0) = 0;
    if (Z8(ngmsdraw, 0) != 0) {
        Z32(CMainWinScr, 0) = A(winbg1envals); Z32(CSubWinScr, 0) = A(winbg1envals); Z32(subscrng, 0) = 1;
        layers(0);
        Z32(bg1totng, 0) = 0; Z32(bg2totng, 0) = 0; Z32(bg3totng, 0) = 0; Z32(bg4totng, 0) = 0;
        Z32(bg1drwng, 0) = 0; Z32(bg2drwng, 0) = 0; Z32(bg3drwng, 0) = 0; Z32(bg4drwng, 0) = 0;
    }
    Z32(CMainWinScr, 0) = A(winbg1envalm); Z32(CSubWinScr, 0) = A(winbg1envals); Z32(subscrng, 0) = 0;
    layers(1);
    zl_MainScreenClip();
    zl_ProcessTransparencies();
dontdraw:
    EBX = 0; ECX = 0; EAX = 0;
    EDX = saved_edx; ESI = saved_esi; LG.edi = saved_edi; EBP = saved_ebp;
}
