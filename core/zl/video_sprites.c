/*
 * processsprites / processspritesb (vcache.asm): build the per-scanline sprite lists (sprtbng entries, sprlefttot,
 * sprleftpr) for the new graphics engine from OAM.  A sprite is N x N tiles of 8x8 (N = 1, 2, 4, 8); the asm walks the
 * tiles in raster order through a small state machine (x = cx, y = dl, tile data pointer = esi) whose per-flip
 * adjustments are reproduced literally in sprite_tiles().  Quirks kept: variant b's flip-Y line loop falls into the
 * non-flipped loop when a line is above curypos (asm jumps to the wrong overflow label).
 */
#include "zl.h"
#include <stdint.h>
#define Z8(s, o)  (*(volatile u8 *)(uintptr_t)((u32)ZO_##s + (u32)(o)))
#define Z16(s, o) (*(volatile u16 *)(uintptr_t)((u32)ZO_##s + (u32)(o)))
#define Z32(s, o) (*(volatile u32 *)(uintptr_t)((u32)ZO_##s + (u32)(o)))
#define M8(a)  (*(volatile u8 *)(uintptr_t)(u32)(a))
#define M16(a) (*(volatile u16 *)(uintptr_t)(u32)(a))
#define M32(a) (*(volatile u32 *)(uintptr_t)(u32)(a))

typedef struct { u16 cx; u8 dl, dh; u32 esi; u8 status; u32 ebp; u8 cpri; int variant_b; } S;

static void reproc(S *s, int flip)
{
    int16_t cx = (int16_t)s->cx;
    if (flip ? (cx <= -8 || cx >= 256) : (cx <= -8 || cx >= 256)) { s->dl += 8; s->esi += flip ? -64u : 64u; return; }
    cx += 8;
    for (int n = 8; n; n--) {
        if (s->dl < Z8(resolutn, 0)) {
            u32 bx = s->dl;
            if (bx < Z16(curypos, 0)) {
                if (s->variant_b && flip) flip = 0;        /* jb .overflow (non-flip label) in processspritesb's flip-Y loop */
            } else {
                u32 al = Z8(sprlefttot, bx);
                if (al <= 37) {
                    Z8(sprlefttot, bx) = (u8)(al + 1);
                    if (!s->variant_b) Z8(sprleftpr, bx + Z32(addr2add, 0))++;
                    else Z8(sprleftpr, bx * 4 + s->cpri) = 1;
                    u32 ent = s->ebp + (bx << 9) + (al << 3);
                    M16(ent) = (u16)cx;
                    M32(ent + 2) = s->esi;
                    M8(ent + 6) = s->dh;
                    M8(ent + 7) = s->variant_b ? (u8)((s->status & 0xF8) | s->cpri) : s->status;
                }
            }
        }
        s->dl++;
        s->esi += flip ? -8u : 8u;
    }
    /* sub cx,8 : s->cx is unchanged */
}

static void sprite_tiles(S *s, int n, u8 attr)
{
    int fx = (attr & 0x20) != 0, fy = (attr & 0x40) != 0;
    s->status = attr;
    s->dh = (u8)(((attr & 0x07) << 4) + 128);      /* attr = OAM attribute byte >> 1 */
    int w = 8 * (n - 1);
    if (n == 1) { if (fy) s->esi += 56; reproc(s, fy); return; }
    if (fx) s->cx += w;
    if (fy) { s->dl += w; s->esi += 56; }
    reproc(s, fy);
    for (int r = 0; r < n; r++) {
        if (r) {                                   /* nextline */
            s->cx = (u16)(s->cx + (fx ? w : -w));
            s->esi += 64u * (16 - n) + (fy ? 128 : 0);
            if (fy) s->dl -= 16;
            reproc(s, fy);
        }
        for (int c = 1; c < n; c++) {              /* nextsprite2right */
            if (fy) s->esi += 128;
            s->dl -= 8;
            s->cx = (u16)(s->cx + (fx ? -8 : 8));
            reproc(s, fy);
        }
    }
}

static int size_n(u8 v) { return v == 1 ? 1 : v == 4 ? 2 : v == 16 ? 4 : 8; }

static void process(int b)
{
    S s = { 0 };
    s.variant_b = b;
    int n1 = size_n(Z8(objsize1, 0)), n2 = size_n(Z8(objsize2, 0));
    u32 bx = (u16)(Z8(objhipr, 0) << 2);
    if (!b) bx = (bx - 4) & 0x1FC; else bx &= 0x1FC;
    Z32(addr2add, 0) = 0;
    u32 loc = Z32(objptr, 0) << 1, loc2 = (Z32(objptrn, 0) - Z32(objptr, 0)) << 1;
    s.ebp = Z32(spritetablea, 0);
    u32 oam = (u32)ZO_oamram;
    for (int pri = 0; pri < (b ? 1 : 4); pri++) {
        for (int objleft = 128; objleft; objleft--) {
            u16 cxw = M16(oam + bx + 2);
            u8 ch = (u8)(cxw >> 8);
            u8 p = (u8)((ch >> 4) & 3);
            if (b) s.cpri = p;
            else if (p != pri) goto next;
            {
                s.dl = (u8)(M8(oam + bx + 1) + 1);
                s.dh = (u8)(ch >> 1);
                u32 t = (u32)(((ch & 1) << 8) | (cxw & 0xFF)) << 6;
                t += loc;
                if (M8(oam + bx + 3) & 1) t += loc2;
                t &= 0x1FFFF;
                s.esi = t + Z32(vcache4b, 0);
                u8 x = M8(oam + bx);
                u8 sh = (u8)((bx & 0xFF) >> 1) & 6;
                u8 ah = (u8)((M8(oam + (bx >> 4) + 512) >> sh) & 3);
                u16 cx = (u16)(((ah & 1) << 8) | x);
                if (cx >= 384) cx = (u16)(cx + 65535 - 511);
                if ((int16_t)cx >= 256 || (int16_t)cx <= -64) goto next;
                s.cx = cx;
                sprite_tiles(&s, (ah & 2) ? n2 : n1, (u8)(ch >> 1));
            }
        next:
            bx = ((b ? bx + 4 : bx - 4)) & 0x1FC;
        }
        if (!b) Z32(addr2add, 0) += 256;
    }
}

void zl_processspritesb(void) { process(1); }
/* sprprifix is initialised to 1 and never written, so processsprites always tail-jumps to processspritesb; the asm's
 * own variant (process(0), kept for reference) is dead code and untested. */
void zl_processsprites(void) { if (Z8(sprprifix, 0)) process(1); else process(0); }
