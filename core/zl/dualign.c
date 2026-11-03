/* regions the original asm uses as scratch but the C overrides do not touch: ignored by the differential test */
#include "zl.h"
__attribute__((constructor)) static void dual_ignores(void)
{
    zl_dual_ignore(ZO_SetaCmdEnable + 4, 0x100);   /* dma.asm: .tempdecr/.dest of the HDMA routines */
}
