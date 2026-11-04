#include <stdint.h>
/* reg420Bw (cpu/dma.asm): $420B DMA enable -- runs transdma for every set bit, channel 0 first.  eax, ecx, edx, esi, edi are preserved. */
#include "zl.h"
#include <stdint.h>

void zl_reg420Bw(void)
{
    u32 eax = LG.eax, ecx = LG.ecx, edx = LG.edx, esi = LG.esi, edi = LG.edi;
    u8 mask = (u8)eax;
    for (int i = 0; i < 8; i++) {
        LG.esi = (u32)ZO_dmadata + 16u * (u32)i;
        if (mask & (1u << i)) zl_mcall(ZID_transdma);
    }
    LG.eax = eax; LG.ecx = ecx; LG.edx = edx; LG.esi = esi; LG.edi = edi;
}
