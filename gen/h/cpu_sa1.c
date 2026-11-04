#include <stdint.h>
/*
 * SA-1 65816 core: the very same opcode bodies as cpu.c (the SA-1 asm is a copy of the main 65816 code with its own
 * register set, the S-flag variables and the SA1 direct-page/opcode tables), compiled with the SA-1 names and the
 * handful of behavioural differences guarded by SA1 in cpu.c (no DMA check on JMP/JSR, JSL IRAM upper bank, native-only
 * RTI, no emulation special case in TCS, 16-bit DEC d,x, XCE without early-out, no doirqnext handling).
 */
#define SA1 1
#define xa SA1xa
#define xd SA1xd
#define xdb SA1xdb
#define xpb SA1xpb
#define xs SA1xs
#define xx SA1xx
#define xy SA1xy
#define flagnz Sflagnz
#define flago  Sflago
#define flagc  Sflagc
#define DPageR8  SA1DPageR8
#define DPageR16 SA1DPageR16
#define DPageW8  SA1DPageW8
#define DPageW16 SA1DPageW16
#include <cpu.c>   /* angle brackets: the raw file, not the cfix-rewritten copy next to this one */

/* ---- table installation + the threaded dispatch (SA-1 endloop: plain, no SPC700 step) ---- */
#define SA1tableA ((u32 *)0x34075240u)
#define SA1tableB ((u32 *)0x34075640u)
#define SA1tableC ((u32 *)0x34075a40u)
#define SA1tableD ((u32 *)0x34075e40u)
#define SA1tableE ((u32 *)0x34076240u)
#define SA1tableF ((u32 *)0x34076640u)
#define SA1tableG ((u32 *)0x34076a40u)
#define SA1tableH ((u32 *)0x34076e40u)
#include "sa1_stubs.h"
static u32 sa1_orig[8][256];

u32 zl_sa1_orig(u32 k)
{
    u32 id = 0x7Cu << 21 | k, dl = DL;
    int t = (dl & 8) ? ((dl & 0x10) ? ((dl & 0x20) ? 7 : 6) : ((dl & 0x20) ? 5 : 4)) : ((dl & 0x10) ? ((dl & 0x20) ? 3 : 2) : ((dl & 0x20) ? 1 : 0));
    for (int op = 0; op < 256; op++) if (zl_sa1op_ids[op] == id) return sa1_orig[t][op];
    return 0;
}

static void sa1_install(void)
{
    u32 *sets[8] = { SA1tableA, SA1tableB, SA1tableC, SA1tableD, SA1tableE, SA1tableF, SA1tableG, SA1tableH };
    int t, op, dec;
    for (op = 0; op < 256; op++) {
        if (!handles((u32)op, &dec)) continue;
        for (t = 0; t < 8; t++) { sa1_orig[t][op] = sets[t][op]; sets[t][op] = zl_sa1op_ids[op]; }
    }
}

u32 zl_sa1_step(u32 op)
{
    u32 cyc;
    zs_cpu_op_inner(op, 0);
    LG.ebx = (LG.ebx & ~0xFFu) | *(u8 *)(uintptr_t)LG.esi;
    LG.esi++;
    cyc = cpucycle[LG.ebx];
    { u32 dh = (LG.edx >> 8) & 0xFF; LG.edx = (LG.edx & 0xFFFF00FFu) | ((dh - cyc) & 0xFF) << 8; if (dh < cyc) return 0; }
    return *(u32 *)(uintptr_t)(LG.edi + LG.ebx * 4);
}

/* post-hook of SA1inittable */
void zl_SA1inittable(void)
{
    zl_orig(ZID_SA1inittable);
    sa1_install();
}
