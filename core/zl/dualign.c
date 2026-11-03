/* regions the original asm uses as scratch but the C overrides do not touch: ignored by the differential test */
#include "zl.h"
__attribute__((constructor)) static void dual_ignores(void)
{
    zl_dual_ignore(ZO_SetaCmdEnable + 4, 0x100);   /* dma.asm: .tempdecr/.dest of the HDMA routines */
}
__attribute__((constructor)) static void dual_ignores_vcache(void)
{
    /* function-local statics of processsprites/processspritesb/cachesprites (vcache.ao .bss +0x43..0xcc, .data +0x2fa..0x301) */
    zl_dual_ignore((u32)ZO_fskipped + 0x43, 0xcd + 4 - 0x43);
    zl_dual_ignore((u32)ZO_sprprifix + 0x2fa, 8);
}
