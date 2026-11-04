#include <stdint.h>
/* SuperFX register-change hooks (cpu/execute.asm): UpdatePORSCMR, UpdateSCBRCOLR, UpdateCLSR.  All registers are preserved. */
#include "zl.h"
#include <stdint.h>
#define Z8(s, o)  (*(volatile u8 *)(uintptr_t)((u32)ZO_##s + (u32)(o)))
#define Z32(s, o) (*(volatile u32 *)(uintptr_t)((u32)ZO_##s + (u32)(o)))

void zl_UpdatePORSCMR(void)
{
    u32 eax;
    if (Z8(SfxPOR, 0) & 0x10) eax = Z32(sfxobjlineloc, 0);
    else {
        u8 al = Z8(SfxSCMR, 0) & 0x24;
        if (al == 4) eax = Z32(sfx160lineloc, 0);
        else if (al == 32) eax = Z32(sfx192lineloc, 0);
        else if (al == 36) eax = Z32(sfxobjlineloc, 0);
        else eax = Z32(sfx128lineloc, 0);
    }
    Z32(sfxclineloc, 0) = eax;
    u32 idx = (Z8(SfxSCMR, 0) & 3u) | ((u32)(Z8(SfxPOR, 0) & 0x0F) << 2);
    u32 ebx = Z32(PLOTJmpb, idx * 4);
    eax = Z32(PLOTJmpa, idx * 4);
    Z32(FxTable, 0x4C * 4) = eax;
    Z32(FxTableb, 0x4C * 4) = eax;
    Z32(FxTablec, 0x4C * 4) = eax;
    Z32(FxTabled, 0x4C * 4) = ebx;
}

void zl_UpdateSCBRCOLR(void)
{
    Z32(SCBRrel, 0) = (Z32(SfxSCBR, 0) << 10) + Z32(sfxramdata, 0);
    u32 c = Z32(SfxCOLR, 0);
    Z32(fxbit01pcal, 0) = Z32(fxbit01, c * 4);
    Z32(fxbit23pcal, 0) = Z32(fxbit23, c * 4);
    Z32(fxbit45pcal, 0) = Z32(fxbit45, c * 4);
    Z32(fxbit67pcal, 0) = Z32(fxbit67, c * 4);
}

void zl_UpdateCLSR(void)
{
    Z32(NumberOfOpcodes2, 0) = 350;
    if (Z8(SfxCLSR, 0) & 1) Z32(NumberOfOpcodes2, 0) = 700;
    if (Z8(SFXCounter, 0) != 1) Z32(NumberOfOpcodes2, 0) = 0x0FFFFFFF;
}
