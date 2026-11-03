/*
 * MainScreenClip / SubScreenClip (video/newgfx16.asm, macro ScreenClip): per-scanline screen clipping by the colour window.
 * Clips the main (and sub) layer 16-bit buffers: pixels outside the window get their "unused" bit cleared/set.
 * MainScreenClip falls through into SubScreenClip in the asm, so the main entry runs both.
 */
#include "zl.h"
#include <stdint.h>
#define Z8(s, o)  (*(volatile u8 *)(uintptr_t)((u32)ZO_##s + (u32)(o)))
#define Z16(s, o) (*(volatile u16 *)(uintptr_t)((u32)ZO_##s + (u32)(o)))
#define Z32(s, o) (*(volatile u32 *)(uintptr_t)((u32)ZO_##s + (u32)(o)))
#define M16(a) (*(volatile u16 *)(uintptr_t)(u32)(a))
#define M32(a) (*(volatile u32 *)(uintptr_t)(u32)(a))
#define SUBOFS (75036u * 2)

extern void zl_build_window(u32 eax, u32 ebx, int ns2);

static void clip(int sub)
{
    u32 line = Z32(vidbuffer, 0) + 16 * 2 + 288 * 2;
    u16 y = 1;
    do {
        u32 esi = line, ecx, edx, edi, ebx;
        u8 al = Z8(scadsng, y);
        if (sub) al = (u8)(al << 2);
        if (al & 0xC0) {
            u32 mask = Z32(UnusedBit, 0);
            al &= 0xC0;
            if (al == 0xC0) {
                ebx = sub ? ~mask : mask;
                goto full;
            }
            Z32(ngwinen, 0) = 0;
            if (Z8(winbg1enval, y + 5 * 256) & 0x0A) {
                u8 l = (u8)((Z8(winlogicaval, y * 2 + 1) >> 2) & 3);
                Z8(nglogicval, 0) = l;
                zl_build_window(y, y + 5 * 256, 0);
            }
            ebx = sub ? ~mask : mask;
            edx = 256;
            if (Z32(ngwinen, 0) == 0) {
                if (al == 0x80) goto fin;
                goto full;
            }
            edi = (u32)ZO_ngwintable;
            if (al == 0x80) {                       /* outside clipping */
                ecx = M32(edi); edi += 4;
                if (ecx != 0) goto noclipping;
                ecx = M32(edi);
                if (ecx != 0) ecx--;
                edi += 4;
                goto startb;
            }
            ecx = M32(edi);
            if (ecx != 0) ecx--;
            edi += 4;
            if (ecx != 0) goto startb;
            ecx = M32(edi); edi += 4;
            goto noclipping;
        startb:
            if (ecx >= 256) goto full;
        startclip:
            for (;;) {
                M16(esi) = (u16)(M16(esi) & ebx);
                if (!sub) M16(esi + SUBOFS) = (u16)(M16(esi + SUBOFS) | ebx);
                esi += 2;
                if (--edx == 0) goto fin;
                if (--ecx != 0) continue;
                ecx = M32(edi); edi += 4;
                break;
            }
        noclipping:
            if (edx <= ecx) goto fin;               /* sub edx,ecx; jz / jc */
            edx -= ecx;
            esi += ecx * 2;
            ecx = M32(edi); edi += 4;
            goto startclip;
        full:
            for (ecx = 128; ecx; ecx--) {
                M32(esi) &= ebx;
                if (!sub) M32(esi + SUBOFS) |= ebx;
                esi += 4;
            }
        }
    fin:
        y++;
        line += 288 * 2;
    } while (Z16(resolutn, 0) != y);
}

void zl_MainScreenClip(void) { clip(0); clip(1); }
void zl_SubScreenClip(void) { clip(1); }
