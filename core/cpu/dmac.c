#define _GNU_SOURCE
/*
 * General-purpose DMA transfers ($420B), ported from cpu/dma.asm (transdma / transdmappu2cpu).
 * Register conventions of the asm bank handlers are preserved: they are invoked through zs_mcall
 * with eax = data, ecx = offset, ebx = bank, esi = host bank base.
 */
#include <stdint.h>
typedef uint8_t u8; typedef uint16_t u16; typedef uint32_t u32;
typedef struct { u32 eax, ecx, edx, ebx, esi, edi, ebp; } regs_t;
extern regs_t zs_R;
extern void zs_mcall(void *fn);
extern void **regptw, **regptr;
extern void *memtabler8[256], *memtablew8[256];
extern u32 snesmmap[256], snesmap2[256];
extern u8 AddrNoIncr;

#include <stdlib.h>
#include <stdio.h>
__attribute__((visibility("hidden"))) u32 zs_dma_use_asm;
__attribute__((constructor)) static void dma_init(void){ zs_dma_use_asm = getenv("ZS_DMA_ASM") != 0; }

static const u16 addrwrite[8][4] = {
    {0,0,0,0},{0,1,0,1},{0,0,0,0},{0,0,1,1},{0,1,2,3},{0,1,2,3},{0,1,2,3},{0,1,2,3}};

/* d = pointer to the 16-byte DMA channel record: [0]=ctrl [1]=ppu reg [2..3]=addr [4]=bank [5..6]=count */
__attribute__((force_align_arg_pointer, visibility("hidden")))
void zs_transdma_c(u8 *d)
{
    u8 ctl = d[0];
    int ppu2cpu = (ctl & 0x80) != 0;
    int incr;
    if (ctl & 0x08) incr = 0; else if (ctl & 0x10) incr = -1; else incr = 1;
    if (!ppu2cpu) AddrNoIncr = (incr == 0);
    unsigned mode = ctl & 7;
    if (!ppu2cpu && mode == 5) mode = 1;           /* asm: al==5 -> al-4 */
    void **rt = ppu2cpu ? regptr : regptw;
    void *rp[4];
    for (int i = 0; i < 4; i++)
        rp[i] = rt[(u16)((0x2100 | d[1]) + addrwrite[mode][i])];
    u32 count = *(u16 *)(d + 5);
    u32 bank = d[4];
    u16 cx = *(u16 *)(d + 2);
    *(u16 *)(d + 5) = 0;
    u32 base = (cx & 0x8000) ? snesmmap[bank] : snesmap2[bank];
    if (count == 0) count = 65536;
    if (getenv("ZS_DMA_COUNT")) { static int k; if (k++ < 3) fprintf(stderr,"dma c: ctl=%02x reg=%02x cnt=%u\n", ctl, d[1], count); }
    static int brk = -1; if (brk < 0) brk = getenv("ZS_DMA_BREAK") != 0;
    if (brk && !ppu2cpu && incr) cx += incr;   /* negative control: skew source address */
   

    void *mem = ppu2cpu ? memtablew8[bank] : memtabler8[bank];
    for (u32 n = 0; n < count; n++) {
        void *r = rp[n & 3];
        zs_R.esi = base; zs_R.ebx = bank; zs_R.edx = count - n;
        if (ppu2cpu) {
            zs_R.ecx = cx; zs_mcall(r);              /* PPU/IO read -> al */
            zs_R.ecx = cx; zs_R.ebx = bank; zs_mcall(mem);
            cx += incr;
        } else {
            zs_R.ecx = cx; zs_mcall(mem);            /* memory read -> al */
            cx += incr;
            zs_R.esi = base; zs_R.ebx = bank; zs_R.ecx = cx; zs_mcall(r);
        }
    }
    *(u16 *)(d + 2) = cx;
    if (!ppu2cpu) AddrNoIncr = 0;
}
