#include <stdint.h>
/*
 * New-engine single-tile cache conversion (vcache.asm cachesingle2bng/4bng/8bng): planar SNES tile -> 8 bits/pixel
 * cache, also records the tile type (bit0 = has no transparent... see tiletypec: starts 3, "and 1" for every non-zero
 * pixel, "and 2" for every zero pixel).  ecx = tile index; all registers are preserved.
 */
#include "zl.h"
#include <stdint.h>
#define Z8(s, o)  (*(volatile u8 *)(uintptr_t)((u32)ZO_##s + (u32)(o)))
#define Z16(s, o) (*(volatile u16 *)(uintptr_t)((u32)ZO_##s + (u32)(o)))
#define Z32(s, o) (*(volatile u32 *)(uintptr_t)((u32)ZO_##s + (u32)(o)))
#define M8(a)  (*(volatile u8 *)(uintptr_t)(u32)(a))
#define M16(a) (*(volatile u16 *)(uintptr_t)(u32)(a))

/* planes[0..nb-1] are the bitplane bytes, most significant plane first */
static void cache_tile(u32 idx, int bpp, u32 vshift, u32 cache_ptr)
{
    u32 src = Z32(vram, 0) + (idx << vshift);
    u32 dst = cache_ptr + (idx << 6);
    u8 type = 3;
    for (int row = 0; row < 8; row++, src += 2, dst += 8) {
        u8 p[8];
        switch (bpp) {
        case 2: p[0] = M8(src + 1); p[1] = M8(src); break;
        case 4: p[0] = M8(src + 17); p[1] = M8(src + 16); p[2] = M8(src + 1); p[3] = M8(src); break;
        default: p[0] = M8(src + 49); p[1] = M8(src + 48); p[2] = M8(src + 33); p[3] = M8(src + 32);
                 p[4] = M8(src + 17); p[5] = M8(src + 16); p[6] = M8(src + 1); p[7] = M8(src); break;
        }
        for (int x = 0; x < 8; x++) {
            u8 v = 0;
            for (int k = 0; k < bpp; k++) { v = (u8)((v << 1) | (p[k] >> 7)); p[k] = (u8)(p[k] << 1); }
            M8(dst + x) = v;
            type &= v ? 1 : 2;
        }
    }
    Z8(tiletypec, 0) = type;
    switch (bpp) { case 2: Z8(tltype2b, idx) = type; break; case 4: Z8(tltype4b, idx) = type; break; default: Z8(tltype8b, idx) = type; }
}

void zl_cachesingle2bng(void) { u32 c = LG.ecx; Z8(vidmemch2, c) = 0; cache_tile(c, 2, 4, Z32(vcache2b, 0)); }
void zl_cachesingle4bng(void) { u32 c = LG.ecx; Z16(vidmemch4, c * 2) = 0; cache_tile(c, 4, 5, Z32(vcache4b, 0)); }
void zl_cachesingle8bng(void) { u32 c = LG.ecx; Z32(vidmemch8, c * 4) = 0; cache_tile(c, 8, 6, Z32(vcache8b, 0)); }
