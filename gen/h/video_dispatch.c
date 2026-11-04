#include <stdint.h>
/*
 * drawline / drawline16b (video/makevid.asm, makev16b.asm): per-scanline entry points.  With the new graphics engine on (the
 * default) they only set a few flags and jump to newengine16b; the old per-line engine (ZS_OLDGFX=1, Stunt Race FX) runs the
 * original body.  Tail-style overrides: the return value is the code id to continue at (0 = ret).
 */
#include "zl.h"
#include <stdint.h>
#define Z8(s, o)  (*(volatile u8 *)(uintptr_t)((u32)ZO_##s + (u32)(o)))
#define Z32(s, o) (*(volatile u32 *)(uintptr_t)((u32)ZO_##s + (u32)(o)))

u32 zl_drawline16b(void)
{
    if (Z8(ForceNewGfxOff, 0) == 0 && Z8(newengen, 0) == 1) return ZID_newengine16b;
    zl_orig(ZID_drawline16b);
    return 0;
}

u32 zl_drawline(void)
{
    u8 al = Z8(winenabs, 0);
    Z8(cwinenabm, 0) = al;
    Z8(bg3high2, 0) = 0;
    if (Z8(bgmode, 0) == 1) { al = Z8(bg3highst, 0); Z8(bg3high2, 0) = al; }
    if (Z8(cbitmode, 0) == 1) {
        LG.eax = (LG.eax & ~0xFFu) | al;
        return zl_drawline16b();
    }
    zl_orig(ZID_drawline);
    return 0;
}
