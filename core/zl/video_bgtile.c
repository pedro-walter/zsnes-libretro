/*
 * drawbg1..4tile16b (video/newgfx16.asm, macro drawbgtileng16b of newgfx16.mac): set up the per-BG tile drawer from the BG
 * registers, then jump into the bit-depth specific tile drawer (drawtileng{2,4,8}b16b / drawtileng16x16{2,4,8}b16b).
 * Tail-style overrides: the return value is the code id to continue at (0 = "ret"); the asm's `push ebx` before the jump is
 * reproduced on the guest stack because the drawers pop it.
 */
#include "zl.h"
#include <stdint.h>
#include <string.h>
#define A(s) ((u32)ZO_##s)
#define L8(a)  (*(volatile u8 *)(uintptr_t)(u32)(a))
#define L16(a) (*(volatile u16 *)(uintptr_t)(u32)(a))
static inline u32 L32(u32 a) { u32 v; memcpy(&v, (const void *)(uintptr_t)a, 4); return v; }
static inline void S32(u32 a, u32 v) { memcpy((void *)(uintptr_t)a, &v, 4); }
#define EAX LG.eax
#define EBX LG.ebx
#define ECX LG.ecx
#define EDX LG.edx
#define ESI LG.esi
#define EDI LG.edi

static void process_build_window(int bg)
{
    S32(A(ngwinen), 0);
    if (!(L8(A(winbg1enval) + EBX + bg * 256) & 0x0A)) return;
    u32 sv_eax = EAX, sv_ebx = EBX;
    u8 al = L8(A(winlogicaval) + EBX * 2);
    L8(A(nglogicval)) = (u8)((al >> (bg * 2)) & 3);
    EAX = EBX; EBX += bg * 256;
    zl_mcall(ZID_BuildWindow);
    int32_t d = (int32_t)(EDI - ESI) >> 1;
    if (L32(A(ngwintable)) != 0) S32(A(ngwintable), L32(A(ngwintable)) - (u32)d);
    else S32(A(ngwintable) + 4, L32(A(ngwintable) + 4) - (u32)d);
    EBX = sv_ebx; EAX = sv_eax;
}

static u32 bgtile(int bg)
{
    S32(A(ng16bprval), 0);
    S32(A(ng16bbgval), (u32)bg);
    int big = L8(A(t16x161) + EBX + bg * 256) == 1;
    u32 yinc, xinc, mask_sx, mask_c, shr_c, edimask, rowmul;
    if (!big) { yinc = 0x100; xinc = 0x100; mask_sx = 0xF8; shr_c = 2; edimask = 7; rowmul = 8; mask_c = 0x7C0; }
    else {
        S32(A(taddnfy16x16), 0); S32(A(taddfy16x16), 16);
        if (ECX & 8) { S32(A(taddnfy16x16), 16); S32(A(taddfy16x16), (u32)-16); }
        yinc = 0x200; xinc = 0x200; mask_sx = 0x1F0; shr_c = 3; edimask = 0xF; rowmul = 4; mask_c = 0x7C0;
    }
    u32 b2 = EBX * 2 + bg * 512;
    u32 eax = L32(A(BGPT1) + b2);
    if (ECX & yinc) eax += L32(A(BGPT1Y) + b2);
    ECX = (ECX * rowmul) & mask_c;
    eax += ECX;
    ECX = L32(A(BG1SXl) + b2);
    u32 edx = L32(A(BGPT1X) + b2);
    if (ECX & xinc) { eax += edx; edx = (edx & 0xFFFF0000u) | (u16)(0 - (u16)edx); }
    EDI = ECX;
    edx = (edx & 0xFFFF0000u) | (u16)((u16)edx - 64);
    ECX &= mask_sx;
    eax &= 0xFFFF;
    ECX >>= shr_c;
    S32(A(bgtxadd), edx);
    L16(A(bgtxad) + EBX * 2 + bg * 512) = (u16)edx;
    eax += ECX;
    ECX = L32(A(BGOPT1) + b2);
    edx = L32(A(BGMA) + EBX);
    EDI &= edimask; edx &= 7; ECX &= 0xFFFF;
    edx = L32(A(colormodedef) + edx * 4 + bg);
    EDI = 0 - EDI; EDI += EDI;
    u32 tgt, cshift;
    u8 dl = (u8)edx;
    if (dl == 2) { cshift = 5; tgt = big ? ZID_drawtileng16x164b16b : ZID_drawtileng4b16b; }
    else if (dl == 1) { cshift = 4; tgt = big ? ZID_drawtileng16x162b16b : ZID_drawtileng2b16b; }
    else if (dl == 3) { cshift = 6; tgt = big ? ZID_drawtileng16x168b16b : ZID_drawtileng8b16b; }
    else { EAX = eax; EDX = edx; return 0; }            /* pop ebx; ret */
    ECX >>= cshift;
    EDI += ESI;
    S32(A(ngptrdat) + bg * 1024 + EBX * 4, ECX);
    S32(A(ngptrdat2), ECX);
    S32(A(ngceax) + bg * 1024 + EBX * 4, eax);
    S32(A(ngcedi) + bg * 1024 + EBX * 4, EDI);
    EAX = eax; EDX = edx;
    if (dl == 1) S32(A(mode0add), 0);
    process_build_window(bg);
    if (dl == 1 && L8(A(BGMA) + EBX) == 0) S32(A(mode0add), L32(A(mode0ads)));
    ECX = EBX + bg * 256;
    EDX = (EDX & ~0xFFu) | (1u << bg);
    LG.esp -= 4; S32(LG.esp, EBX);                      /* the asm's push ebx; the drawer pops it */
    return tgt;
}
u32 zl_drawbg1tile16b(void) { L8(A(prdatb) + EBX) = 1; return bgtile(0); }
u32 zl_drawbg2tile16b(void) { L8(A(prdata) + EBX) = 1; return bgtile(1); }
u32 zl_drawbg3tile16b(void) { L8(A(prdatc) + EBX) = 1; return bgtile(2); }
u32 zl_drawbg4tile16b(void) { L8(A(prdata) + EBX) = 1; return bgtile(3); }
