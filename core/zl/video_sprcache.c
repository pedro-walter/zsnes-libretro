/*
 * cachesprites (vcache.asm): converts the 4bpp sprite tiles whose cache entry is dirty (vidmemch4) into the 8bpp
 * vcache4b cache and records the tile type, for every sprite that is still visible on or below the current line.
 * The asm keeps its loop state in function-local statics; here they are plain locals (the dual-run ignores them).
 */
#include "zl.h"
#include <stdint.h>
#define Z8(s, o)  (*(volatile u8 *)(uintptr_t)((u32)ZO_##s + (u32)(o)))
#define Z16(s, o) (*(volatile u16 *)(uintptr_t)((u32)ZO_##s + (u32)(o)))
#define Z32(s, o) (*(volatile u32 *)(uintptr_t)((u32)ZO_##s + (u32)(o)))
#define M8(a)  (*(volatile u8 *)(uintptr_t)(u32)(a))
#define M16(a) (*(volatile u16 *)(uintptr_t)(u32)(a))

static void cache_sprite_tile(u32 bx)      /* bx = vidmemch4 byte offset (even), already cleared */
{
    u32 b16 = (u16)(bx << 4);
    u32 src = Z32(vram, 0) + b16;
    u32 dst = Z32(vcache4b, 0) + b16 * 2;
    u8 type = 3;
    for (int row = 0; row < 8; row++, src += 2, dst += 8) {
        u8 p0 = M8(src + 17), p1 = M8(src + 16), p2 = M8(src + 1), p3 = M8(src);   /* dh dl ch cl */
        for (int x = 0; x < 8; x++) {
            u8 v = (u8)(((p0 >> 7) << 3) | ((p1 >> 7) << 2) | ((p2 >> 7) << 1) | (p3 >> 7));
            p0 = (u8)(p0 << 1); p1 = (u8)(p1 << 1); p2 = (u8)(p2 << 1); p3 = (u8)(p3 << 1);
            M8(dst + x) = v;
            type &= v ? 1 : 2;
        }
    }
    Z8(tiletypec, 0) = type;
    Z8(tltype4b, bx >> 1) = type;
}

void zl_cachesprites(void)
{
    u32 objptr = (u32)ZO_oamram + 512;
    u8 curobjtype = M8(objptr);
    int leftinbyte = 4;
    u32 oam = (u32)ZO_oamram;
    u32 esi = oam + 2;
    u16 nbg = (u16)(Z16(objptr, 0) >> 4);
    u16 nbg2 = (u16)(Z16(objptrn, 0) >> 4);
    u32 sprnum = 3;
    for (int objleft = 128; objleft; objleft--) {
        u8 num2do, move; u16 add;
        if (curobjtype & 2) { num2do = Z8(objsize2, 0); add = Z16(objadds2, 0); move = Z8(objmovs2, 0); }
        else                { num2do = Z8(objsize1, 0); add = Z16(objadds1, 0); move = Z8(objmovs1, 0); }
        u8 byteb4add = move;
        curobjtype >>= 2;
        if (--leftinbyte == 0) { leftinbyte = 4; objptr++; curobjtype = M8(objptr); }
        u16 curobj = (u16)(M16(esi) & 0x1FF);
        do {
            u8 y = M8(oam + sprnum - 2), ch = (u8)(Z8(curypos, 0) - 1);
            if (!(y < ch)) {
                u16 bx = (u16)(((M8(oam + sprnum) & 1) ? (u16)(curobj * 2 + nbg2) : (u16)(curobj * 2 + nbg)) & 4095);
                if (Z16(vidmemch4, bx) & 0x0101) {
                    Z16(vidmemch4, bx) = 0;
                    cache_sprite_tile(bx);
                }
            }
            curobj++;
            if (--byteb4add == 0) { curobj = (u16)(curobj + add); byteb4add = move; }
        } while (--num2do);
        esi += 4;
        sprnum += 4;
    }
}
