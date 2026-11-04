#include <stdint.h>
/* SuperFX register and RAM-bank handlers (chips/sfxproc.asm), register ABI of the originals (al = value / result, ecx = offset).
 * Generated shapes: R0-R15 byte accessors and the four 64K RAM windows; the irregular ones are written out below. */
#include "zl.h"
#include <stdint.h>
#define Z8(s, o)  (*(volatile u8 *)(uintptr_t)((u32)ZO_##s + (u32)(o)))
#define Z16(s, o) (*(volatile u16 *)(uintptr_t)((u32)ZO_##s + (u32)(o)))
#define Z32(s, o) (*(volatile u32 *)(uintptr_t)((u32)ZO_##s + (u32)(o)))
#define M8(a)  (*(volatile u8 *)(uintptr_t)(u32)(a))
#define M16(a) (*(volatile u16 *)(uintptr_t)(u32)(a))
#define AL (LG.eax & 0xFF)
static inline void set_al(u32 v) { LG.eax = (LG.eax & ~0xFFu) | (v & 0xFF); }
void zl_reg3000r(void) { set_al(Z8(SfxR0, 0)); }
void zl_reg3000w(void) { Z8(SfxR0, 0) = (u8)AL; }
void zl_reg3001r(void) { set_al(Z8(SfxR0, 1)); }
void zl_reg3001w(void) { Z8(SfxR0, 1) = (u8)AL; }
void zl_reg3002r(void) { set_al(Z8(SfxR1, 0)); }
void zl_reg3002w(void) { Z8(SfxR1, 0) = (u8)AL; }
void zl_reg3003r(void) { set_al(Z8(SfxR1, 1)); }
void zl_reg3003w(void) { Z8(SfxR1, 1) = (u8)AL; }
void zl_reg3004r(void) { set_al(Z8(SfxR2, 0)); }
void zl_reg3004w(void) { Z8(SfxR2, 0) = (u8)AL; }
void zl_reg3005r(void) { set_al(Z8(SfxR2, 1)); }
void zl_reg3005w(void) { Z8(SfxR2, 1) = (u8)AL; }
void zl_reg3006r(void) { set_al(Z8(SfxR3, 0)); }
void zl_reg3006w(void) { Z8(SfxR3, 0) = (u8)AL; }
void zl_reg3007r(void) { set_al(Z8(SfxR3, 1)); }
void zl_reg3007w(void) { Z8(SfxR3, 1) = (u8)AL; }
void zl_reg3008r(void) { set_al(Z8(SfxR4, 0)); }
void zl_reg3008w(void) { Z8(SfxR4, 0) = (u8)AL; }
void zl_reg3009r(void) { set_al(Z8(SfxR4, 1)); }
void zl_reg3009w(void) { Z8(SfxR4, 1) = (u8)AL; }
void zl_reg300Ar(void) { set_al(Z8(SfxR5, 0)); }
void zl_reg300Aw(void) { Z8(SfxR5, 0) = (u8)AL; }
void zl_reg300Br(void) { set_al(Z8(SfxR5, 1)); }
void zl_reg300Bw(void) { Z8(SfxR5, 1) = (u8)AL; }
void zl_reg300Cr(void) { set_al(Z8(SfxR6, 0)); }
void zl_reg300Cw(void) { Z8(SfxR6, 0) = (u8)AL; }
void zl_reg300Dr(void) { set_al(Z8(SfxR6, 1)); }
void zl_reg300Dw(void) { Z8(SfxR6, 1) = (u8)AL; }
void zl_reg300Er(void) { set_al(Z8(SfxR7, 0)); }
void zl_reg300Ew(void) { Z8(SfxR7, 0) = (u8)AL; }
void zl_reg300Fr(void) { set_al(Z8(SfxR7, 1)); }
void zl_reg300Fw(void) { Z8(SfxR7, 1) = (u8)AL; }
void zl_reg3010r(void) { set_al(Z8(SfxR8, 0)); }
void zl_reg3010w(void) { Z8(SfxR8, 0) = (u8)AL; }
void zl_reg3011r(void) { set_al(Z8(SfxR8, 1)); }
void zl_reg3011w(void) { Z8(SfxR8, 1) = (u8)AL; }
void zl_reg3012r(void) { set_al(Z8(SfxR9, 0)); }
void zl_reg3012w(void) { Z8(SfxR9, 0) = (u8)AL; }
void zl_reg3013r(void) { set_al(Z8(SfxR9, 1)); }
void zl_reg3013w(void) { Z8(SfxR9, 1) = (u8)AL; }
void zl_reg3014r(void) { set_al(Z8(SfxR10, 0)); }
void zl_reg3014w(void) { Z8(SfxR10, 0) = (u8)AL; }
void zl_reg3015r(void) { set_al(Z8(SfxR10, 1)); }
void zl_reg3015w(void) { Z8(SfxR10, 1) = (u8)AL; }
void zl_reg3016r(void) { set_al(Z8(SfxR11, 0)); }
void zl_reg3016w(void) { Z8(SfxR11, 0) = (u8)AL; }
void zl_reg3017r(void) { set_al(Z8(SfxR11, 1)); }
void zl_reg3017w(void) { Z8(SfxR11, 1) = (u8)AL; }
void zl_reg3018r(void) { set_al(Z8(SfxR12, 0)); }
void zl_reg3018w(void) { Z8(SfxR12, 0) = (u8)AL; }
void zl_reg3019r(void) { set_al(Z8(SfxR12, 1)); }
void zl_reg3019w(void) { Z8(SfxR12, 1) = (u8)AL; }
void zl_reg301Ar(void) { set_al(Z8(SfxR13, 0)); }
void zl_reg301Aw(void) { Z8(SfxR13, 0) = (u8)AL; }
void zl_reg301Br(void) { set_al(Z8(SfxR13, 1)); }
void zl_reg301Bw(void) { Z8(SfxR13, 1) = (u8)AL; }
void zl_reg301Cr(void) { set_al(Z8(SfxR14, 0)); }
void zl_reg301Cw(void) { Z8(SfxR14, 0) = (u8)AL; }
void zl_reg301Dr(void) { set_al(Z8(SfxR14, 1)); }
void zl_reg301Dw(void) { Z8(SfxR14, 1) = (u8)AL; }
void zl_reg301Er(void) { set_al(Z8(SfxR15, 0)); }
void zl_reg301Ew(void) { Z8(SfxR15, 0) = (u8)AL; }
void zl_reg301Fr(void) { set_al(Z8(SfxR15, 1)); }

void zl_reg301Fw(void)
{
    Z8(SfxR15, 1) = (u8)AL;
    u32 pbr = Z32(SfxPBR, 0);
    u32 p = ((u32 *)(uintptr_t)(u32)ZO_SfxMemTable)[pbr] + Z32(SfxR15, 0);
    Z8(SfxPIPE, 0) = M8(p);
    Z16(SfxR15, 0) = (u16)(Z16(SfxR15, 0) + 1);
    Z8(SfxSFR, 0) |= 0x20;
    Z32(SfxSFR, 0) |= 0x8000;
    Z32(SFXProc, 0) = 1;
}
void zl_reg3030r(void)
{
    u16 sfr = (u16)(Z16(SfxSFR, 0) & 0x8F60);
    if (Z8(SfxCarry, 0) & 1) sfr |= 0x04;
    if (Z16(SfxSignZero, 0) == 0) sfr |= 0x02;
    if (Z16(SfxSignZero, 0) & 0x8000) sfr |= 0x08;
    if (Z8(SfxOverflow, 0) != 0) sfr |= 0x10;
    if (Z8(SfxB, 0) != 0) sfr |= 0x1000;
    Z16(SfxSFR, 0) = sfr;
    set_al(sfr);
}
void zl_reg3031r(void)
{
    int32_t *co = (int32_t *)(uintptr_t)(u32)ZO_ChangeOps;
    if (Z8(SfxAC, 0) == 1 || *co >= -350 * 240) Z8(SfxSFR, 1) &= 0x7F;
    else if (*co < -350 * 240 * 4) *co = -350 * 240 * 4;
    else *co += 350 * 240;
    set_al(Z8(SfxSFR, 1));
}
void zl_reg3032r(void) { set_al(0); }
void zl_reg3033r(void) { set_al(Z8(SfxBRAMR, 0)); }
void zl_reg3034r(void) { set_al(Z8(SfxPBR, 0)); }
void zl_reg3035r(void) { set_al(0); }
void zl_reg3036r(void) { set_al(Z8(SfxROMBR, 0)); }
void zl_reg3037r(void) { set_al(Z8(SfxCFGR, 0)); }
void zl_reg3038r(void) { set_al(Z8(SfxSCBR, 0)); }
void zl_reg3039r(void) { set_al(Z8(SfxCLSR, 0)); }
void zl_reg303Ar(void)
{
    u32 eax, scmr = Z8(SfxSCMR, 0);
    if (Z8(SfxPOR, 0) & 0x10) eax = Z32(sfxobjlineloc, 0);
    else {
        u32 t = scmr & 0x24;
        eax = t == 4 ? Z32(sfx160lineloc, 0) : t == 32 ? Z32(sfx192lineloc, 0) : t == 36 ? Z32(sfxobjlineloc, 0) : Z32(sfx128lineloc, 0);
    }
    LG.eax = eax;
    Z32(sfxclineloc, 0) = eax;
}
void zl_reg303Br(void) { set_al(0x20); }
void zl_reg303Cr(void) { set_al(Z8(SfxRAMBR, 0)); }
void zl_reg303Dr(void) { set_al(0); }
void zl_reg303Er(void) { set_al(Z8(SfxCBR, 0)); }
void zl_reg303Fr(void) { set_al(Z8(SfxCBR, 1)); }
void zl_reg3030w(void)
{
    Z8(SfxSFR, 0) = (u8)AL;
    if (AL & 0x20) { Z32(NumberOfOpcodes, 0) = 100; zl_mcall(ZID_StartSFX); }
}
void zl_reg3031w(void) { Z8(SfxSFR, 1) = (u8)AL; }
void zl_reg3033w(void) { Z8(SfxBRAMR, 0) = (u8)(AL & 0xFE); }
void zl_reg3034w(void)
{
    Z8(SfxPBR, 0) = (u8)AL;
    LG.ebx = ((u32 *)(uintptr_t)(u32)ZO_SfxMemTable)[AL];
    Z32(SfxCPB, 0) = LG.ebx;
}
void zl_reg3036w(void)
{
    Z8(SfxROMBR, 0) = (u8)AL;
    LG.ebx = ((u32 *)(uintptr_t)(u32)ZO_SfxMemTable)[AL];
    Z32(SfxCROM, 0) = LG.ebx;
}
void zl_reg3037w(void) { Z8(SfxCFGR, 0) = (u8)AL; }
void zl_reg3038w(void) { Z8(SfxSCBR, 0) = (u8)AL; zl_mcall(ZID_UpdateSCBRCOLR); }
void zl_reg3039w(void) { set_al(AL & 0xFE); Z8(SfxCLSR, 0) = (u8)AL; zl_mcall(ZID_UpdateCLSR); }
void zl_reg303Aw(void) { Z8(SfxSCMR, 0) = (u8)AL; zl_mcall(ZID_UpdatePORSCMR); }
void zl_reg303Cw(void)
{
    u32 mask = Z32(SfxnRamBanks, 0) - 1;
    u32 eax = LG.eax; eax = (eax & ~0xFFu) | ((eax & 0xFF) & (mask & 0xFF));
    eax &= mask;
    LG.eax = eax;
    Z32(SfxRAMBR, 0) = eax;
    LG.ebx = ((eax & 0xFF) << 16) + Z32(sfxramdata, 0);
    Z32(SfxRAMMem, 0) = LG.ebx;
}
void zl_reg303Ew(void) { Z8(SfxCBR, 0) = (u8)AL; }
void zl_reg303Fw(void) { Z8(SfxCBR, 1) = (u8)AL; }
void zl_sfxaccessbankr8(void) { LG.eax = (LG.eax & ~0xFFu) | M8(Z32(sfxramdata, 0) + LG.ecx + 0); LG.ebx = 0; }
void zl_sfxaccessbankw8(void) { M8(Z32(sfxramdata, 0) + LG.ecx + 0) = (u8)LG.eax; LG.ebx = 0; }
void zl_sfxaccessbankr16(void) { LG.eax = (LG.eax & ~0xFFFFu) | M16(Z32(sfxramdata, 0) + LG.ecx + 0); LG.ebx = 0; }
void zl_sfxaccessbankw16(void) { M16(Z32(sfxramdata, 0) + LG.ecx + 0) = (u16)LG.eax; LG.ebx = 0; }
void zl_sfxaccessbankr8b(void) { LG.eax = (LG.eax & ~0xFFu) | M8(Z32(sfxramdata, 0) + LG.ecx + 65536); LG.ebx = 0; }
void zl_sfxaccessbankw8b(void) { M8(Z32(sfxramdata, 0) + LG.ecx + 65536) = (u8)LG.eax; LG.ebx = 0; }
void zl_sfxaccessbankr16b(void) { LG.eax = (LG.eax & ~0xFFFFu) | M16(Z32(sfxramdata, 0) + LG.ecx + 65536); LG.ebx = 0; }
void zl_sfxaccessbankw16b(void) { M16(Z32(sfxramdata, 0) + LG.ecx + 65536) = (u16)LG.eax; LG.ebx = 0; }
void zl_sfxaccessbankr8c(void) { LG.eax = (LG.eax & ~0xFFu) | M8(Z32(sfxramdata, 0) + LG.ecx + 131072); LG.ebx = 0; }
void zl_sfxaccessbankw8c(void) { M8(Z32(sfxramdata, 0) + LG.ecx + 131072) = (u8)LG.eax; LG.ebx = 0; }
void zl_sfxaccessbankr16c(void) { LG.eax = (LG.eax & ~0xFFFFu) | M16(Z32(sfxramdata, 0) + LG.ecx + 131072); LG.ebx = 0; }
void zl_sfxaccessbankw16c(void) { M16(Z32(sfxramdata, 0) + LG.ecx + 131072) = (u16)LG.eax; LG.ebx = 0; }
void zl_sfxaccessbankr8d(void) { LG.eax = (LG.eax & ~0xFFu) | M8(Z32(sfxramdata, 0) + LG.ecx + 196608); LG.ebx = 0; }
void zl_sfxaccessbankw8d(void) { M8(Z32(sfxramdata, 0) + LG.ecx + 196608) = (u8)LG.eax; LG.ebx = 0; }
void zl_sfxaccessbankr16d(void) { LG.eax = (LG.eax & ~0xFFFFu) | M16(Z32(sfxramdata, 0) + LG.ecx + 196608); LG.ebx = 0; }
void zl_sfxaccessbankw16d(void) { M16(Z32(sfxramdata, 0) + LG.ecx + 196608) = (u16)LG.eax; LG.ebx = 0; }
