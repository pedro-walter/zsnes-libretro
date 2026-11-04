#include <stdint.h>
/*
 * ST010/ST011 (Seta) memory handlers (chips/st10proc.asm): the 4K shared RAM at $60-$67 / $68-$6F:xxxx and the command-enable
 * bytes.  Quirks of the asm kept: the 16-bit "a" writes and the $7FFF word write go to setaramdata / its neighbourhood
 * instead of SetaCmdEnable.
 */
#include "zl.h"
#include <stdint.h>
#define M8(a)  (*(volatile u8 *)(uintptr_t)(u32)(a))
#define M16(a) (*(volatile u16 *)(uintptr_t)(u32)(a))
#define Z32(s, o) (*(volatile u32 *)(uintptr_t)((u32)ZO_##s + (u32)(o)))
#define AL (LG.eax & 0xFF)
void ST010DoCommand(void);
static inline void set_al(u32 v) { LG.eax = (LG.eax & ~0xFFu) | (v & 0xFF); }
static inline void set_ax(u32 v) { LG.eax = (LG.eax & ~0xFFFFu) | (v & 0xFFFF); }
static inline void xchg_ah_al(void) { u32 e = LG.eax; LG.eax = (e & ~0xFFFFu) | ((e & 0xFF) << 8) | ((e >> 8) & 0xFF); }

void zl_setaaccessbankr8(void)  { LG.ecx &= 0xFFF; set_al(M8(Z32(setaramdata, 0) + LG.ecx)); LG.ebx = 0; }
void zl_setaaccessbankr16(void) { LG.ecx &= 0xFFF; set_ax(M16(Z32(setaramdata, 0) + LG.ecx)); LG.ebx = 0; }
void zl_setaaccessbankw8(void)
{
    if (!(LG.ecx & 0x8000)) {
        u32 b;
        LG.ecx &= 0xFFF;
        b = Z32(setaramdata, 0); LG.ebx = b;
        M8(b + LG.ecx) = (u8)AL;
        if (M8(b + 0x21) == 0x80) ST010DoCommand();
    }
    LG.ebx = 0;
}
void zl_setaaccessbankw16(void)
{
    if (!(LG.ecx & 0x8000)) {
        if (LG.ecx == 0x7FFF) M8((u32)ZO_setaramdata + 0xFFF) = (u8)AL;
        else {
            u32 b;
            LG.ecx &= 0xFFF;
            b = Z32(setaramdata, 0); LG.ebx = b;
            if (LG.ecx == 0xFFF) { M8(b + LG.ecx) = (u8)AL; xchg_ah_al(); M8(b) = (u8)AL; }
            else { M16(b + LG.ecx) = (u16)LG.eax; if (M8(b + 0x21) == 0x80) ST010DoCommand(); }
        }
    }
    LG.ebx = 0;
}
void zl_setaaccessbankr8a(void)
{
    set_al(0);
    if (LG.ecx < 0x4000) { LG.ecx &= 3; set_al(M8((u32)ZO_SetaCmdEnable + LG.ecx)); }
    LG.ebx = 0;
}
void zl_setaaccessbankw8a(void)
{
    if (LG.ecx < 0x4000) { LG.ecx &= 3; M8((u32)ZO_SetaCmdEnable + LG.ecx) = (u8)AL; }
    LG.ebx = 0;
}
void zl_setaaccessbankr16a(void)
{
    set_ax(0);
    if (LG.ecx < 0x4000) {
        LG.ecx &= 3; set_al(M8((u32)ZO_SetaCmdEnable + LG.ecx)); xchg_ah_al();
        LG.ecx = (LG.ecx + 1) & 3; set_al(M8((u32)ZO_SetaCmdEnable + LG.ecx));
    }
    LG.ebx = 0;
}
void zl_setaaccessbankw16a(void)
{
    if (LG.ecx < 0x4000) {
        u32 b = Z32(setaramdata, 0); LG.ebx = b;
        LG.ecx &= 3; M8(b + LG.ecx) = (u8)AL;
        xchg_ah_al();
        LG.ecx = (LG.ecx + 1) & 3; M8(b + LG.ecx) = (u8)AL;
    }
    LG.ebx = 0;
}
