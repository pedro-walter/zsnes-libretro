#include <stdint.h>
/*
 * BuildWindow / BuildWindow2 (video/newgfx.asm): builds the per-scanline window run-length table (ngwintable, mirrored at +64)
 * for one layer from the two window boundaries and the combine logic.  Literal transcription; registers: eax = scanline,
 * ebx = layer*256+scanline on entry; only ebx is changed on return (as in the asm).
 */
#include "zl.h"
#include <stdint.h>
#define Z8(s, o)  (*(volatile u8 *)(uintptr_t)((u32)ZO_##s + (u32)(o)))
#define Z32(s, o) (*(volatile u32 *)(uintptr_t)((u32)ZO_##s + (u32)(o)))
#define M8(a)  (*(volatile u8 *)(uintptr_t)(u32)(a))
#define M32(a) (*(volatile u32 *)(uintptr_t)(u32)(a))
#define TBL ((u32)ZO_ngwintable)
#define TBLC ((u32)ZO_ngwintablec)

/* Process1DualWindow: ch bit0 selects outside; returns new esi, updates *pebx */
static u32 dual1(u32 esi, u32 *pebx, u32 edx, u32 ch)
{
    u32 ebx = *pebx;
    if (!(ch & 1)) {
        ebx++;
        if (edx < ebx) { M32(esi) = 0xEE00; esi += 4; }
        else { M32(esi) = ebx; edx -= ebx; edx += 2; M32(esi + 4) = edx; M32(esi + 8) = 0xEE00; esi += 12; }
    } else {
        if (edx < ebx) { M32(esi) = 0; M32(esi + 4) = 0xEE00; esi += 8; }
        else {
            if (ebx != 0) { ebx++; M32(esi) = 0; M32(esi + 4) = ebx; esi += 8; }
            edx -= ebx; edx += 2; M32(esi) = edx; M32(esi + 4) = 0xEE00; esi += 8;
        }
    }
    *pebx = ebx;
    return esi;
}

static void finish(void) { Z32(pngwinen, 0) = Z32(ngwinen, 0); }

void zl_build_window(u32 eax, u32 ebx, int ns2)
{
    u32 ecx, edx, esi, edi, ch, cl;
    if (!ns2) {
        if (Z8(WindowRedraw, 0) == 1) ns2 = 1;
        else {
            if (Z8(WindowDisables, 0) != 0) { LG.ebx = ebx; return; }
            Z32(valtemp, 0) = 0xEE00;
            edx = ((*(volatile u16 *)(uintptr_t)((u32)ZO_winlogicaval + eax * 2)) << 16) | Z8(winbg1enval, ebx);
            if (edx == Z32(pwinen, 0)) {
                edx = Z32(winboundary, eax * 4);
                if (edx == Z32(pwinbound, 0)) {
                    ecx = TBL;
                    for (;;) {
                        edx = M32(ecx + 64); M32(ecx) = edx; ecx += 4;
                        if (edx > 0xD000) break;
                        if (ecx == TBL + 64) break;
                    }
                    Z32(ngwinen, 0) = Z32(pngwinen, 0);
                    LG.ebx = ebx; return;
                }
            } else {
                Z32(pwinen, 0) = edx;
                edx = Z32(winboundary, eax * 4);
            }
            Z32(pwinbound, 0) = edx;
        }
    }
    if (ns2) Z8(WindowRedraw, 0) = 0;
    /* .ns */
    cl = Z8(winbg1enval, ebx); edx = 0; ch = cl; cl &= 0x0A; esi = TBL;
    if (cl == 0x0A) {
        u32 sebx = ebx;
        ebx = 0; edx = Z8(winboundary, eax * 4 + 1); ebx = Z8(winboundary, eax * 4);
        esi = dual1(esi, &ebx, edx, ch);
        ebx = sebx;
        Z32(WinPtrAPos, 0) = esi;
        ch = Z8(winbg1enval, ebx); ebx = 0; edx = 0;
        ebx = Z8(winboundary, eax * 4 + 2); edx = Z8(winboundary, eax * 4 + 3);
        ch >>= 2;
        esi = dual1(esi, &ebx, edx, ch);
        Z32(WinPtrBPos, 0) = esi;
        /* displacement -> cumulative */
        esi = TBL; ebx = 0;
        do { ebx += M32(esi); M32(esi) = ebx; esi += 4; } while (esi != Z32(WinPtrAPos, 0));
        ebx = 0;
        do { ebx += M32(esi); M32(esi) = ebx; esi += 4; } while (esi != Z32(WinPtrBPos, 0));
        /* combine both lists */
        cl = 1; ch = 1;
        edx = TBLC; esi = TBL; edi = Z32(WinPtrAPos, 0);
        M32(edx) = 0; M32(edx + 64) = 0;
        if (M32(esi) == 0) { esi += 4; M32(edx + 64)++; cl = (u8)-cl; }
        if (M32(edi) == 0) { edi += 4; M32(edx + 64)++; ch = (u8)-ch; }
        edx += 4;
        for (;;) {
            int goesi = 0, goedi = 1;
            ebx = M32(esi);
            if (ebx < 0xEE00) {
                if (ebx == M32(edi)) {
                    M32(edx) = ebx; M8(edx + 64) = (u8)cl; M8(edx + 64) = (u8)(M8(edx + 64) + ch);
                    cl = (u8)-cl; ch = (u8)-ch; edx += 4; esi += 4; edi += 4;
                    if (ebx < 0xEE00) continue;
                    goto finb;
                }
                if (ebx < M32(edi)) { goesi = 1; goedi = 0; }
            }
            if (goedi) {
                ebx = M32(edi);
                if (ebx < 0xEE00) { M32(edx) = ebx; M8(edx + 64) = (u8)ch; edx += 4; edi += 4; ch = (u8)-ch; continue; }
                ebx = M32(esi);
                if (ebx >= 0xEE00) { M32(edx) = 0xEE00; edx += 4; goto finb; }
            }
            (void)goesi;
            M32(edx) = ebx; M8(edx + 64) = (u8)cl; edx += 4; esi += 4; cl = (u8)-cl;
        }
    finb:
        Z32(WinPtrAPos, 0) = edx;
        /* convert to on/off */
        edi = ((Z32(nglogicval, 0) & 3) << 2) + (u32)ZO_OrLogicTable;
        edx = TBLC; ecx = 0;
        do {
            ecx = (u8)(ecx + M8(edx + 64));
            ebx = (ebx & ~0xFFu) | M8(edi + ecx);
            M8(edx + 64) = (u8)ebx;
            edx += 4;
        } while (edx != Z32(WinPtrAPos, 0));
        /* shorten & convert back to displacement */
        edx = TBLC; esi = TBL; cl = 1; ebx = 0;
        do {
            if (M8(edx + 64) == cl) {
                edi = M32(edx); edi -= ebx; ebx += edi;
                M32(esi) = edi; M32(esi + 64) = edi; esi += 4; cl ^= 1;
            }
            edx += 4;
        } while (edx != Z32(WinPtrAPos, 0));
        M32(esi) = 0xEE00; M32(esi + 64) = 0xEE00;
        Z32(ngwinen, 0) = 1; Z32(pngwinen, 0) = 1;
        LG.ebx = ebx; return;
    }
    ebx = 0;
    if (cl == 2) { ebx = Z8(winboundary, eax * 4); edx = Z8(winboundary, eax * 4 + 1); }
    else { ebx = Z8(winboundary, eax * 4 + 2); edx = Z8(winboundary, eax * 4 + 3); ch = (u8)(ch >> 2); }
    if (!(ch & 1)) {
        ebx++;
        if (edx >= ebx) {
            M32(esi) = ebx; M32(esi + 64) = ebx;
            edx -= ebx; edx += 2;
            M32(esi + 4) = edx; M32(esi + 8) = 0xEE00; M32(esi + 4 + 64) = edx; M32(esi + 8 + 64) = 0xEE00;
            Z32(ngwinen, 0) = 1;
        }
    } else if (edx < ebx) {
        M32(esi) = 0; M32(esi + 4) = 0xEE00; M32(esi + 64) = 0; M32(esi + 4 + 64) = 0xEE00;
        Z32(ngwinen, 0) = 1;
    } else {
        if (ebx != 0) { ebx++; M32(esi) = 0; M32(esi + 4) = ebx; M32(esi + 64) = 0; M32(esi + 4 + 64) = ebx; esi += 8; }
        edx -= ebx; edx += 2;
        M32(esi) = edx; M32(esi + 64) = edx; M32(esi + 4) = 0xEE00; M32(esi + 4 + 64) = 0xEE00;
        Z32(ngwinen, 0) = 1;
    }
    finish();
    LG.ebx = ebx;
}

void zl_BuildWindow(void)  { zl_build_window(LG.eax, LG.ebx, 0); }
void zl_BuildWindow2(void) { zl_build_window(LG.eax, LG.ebx, 1); }
