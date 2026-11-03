#define _GNU_SOURCE
/*
 * General-purpose DMA transfers ($420B), ported from cpu/dma.asm (transdma / transdmappu2cpu).
 * Register conventions of the asm bank handlers are preserved: they are invoked through zl_mcall
 * with eax = data, ecx = offset, ebx = bank, esi = host bank base.
 */
/*
 * General-purpose DMA ($420B) and HDMA, ported from cpu/dma.asm.  Guest handler tables hold 32-bit guest code ids;
 * the bank handlers use the asm register ABI (eax = data, ecx = offset, ebx = bank, esi = host bank base ...) and
 * run through zl_mcall.  Entry points are overrides of the asm labels.
 */
#include "zl.h"
#define R LG
extern u32 regptw, regptr;                                  /* guest pointer slots -> tables of handler ids */
#define REGPTW ((u32 *)(uintptr_t)regptw)
#define REGPTR ((u32 *)(uintptr_t)regptr)
extern u32 memtabler8[256], memtablew8[256], memtabler16[256];
extern u32 snesmmap[256], snesmap2[256];
extern u8 AddrNoIncr;
#define G(p) ((u32)(uintptr_t)(p))
static const u16 addrwrite[8][4] = {
    {0,0,0,0},{0,1,0,1},{0,0,0,0},{0,0,1,1},{0,1,2,3},{0,1,2,3},{0,1,2,3},{0,1,2,3}};

/* d = pointer to the 16-byte DMA channel record: [0]=ctrl [1]=ppu reg [2..3]=addr [4]=bank [5..6]=count */
static void transdma_c(u8 *d)
{
    u8 ctl = d[0];
    int ppu2cpu = (ctl & 0x80) != 0;
    int incr;
    if (ctl & 0x08) incr = 0; else if (ctl & 0x10) incr = -1; else incr = 1;
    if (!ppu2cpu) AddrNoIncr = (incr == 0);
    unsigned mode = ctl & 7;
    if (!ppu2cpu && mode == 5) mode = 1;           /* asm: al==5 -> al-4 */
    u32 *rt = ppu2cpu ? REGPTR : REGPTW;
    u32 rp[4];
    for (int i = 0; i < 4; i++)
        rp[i] = rt[(u16)((0x2100 | d[1]) + addrwrite[mode][i])];
    u32 count = *(u16 *)(d + 5);
    u32 bank = d[4];
    u16 cx = *(u16 *)(d + 2);
    *(u16 *)(d + 5) = 0;
    u32 base = (cx & 0x8000) ? snesmmap[bank] : snesmap2[bank];
    if (count == 0) count = 65536;
   

    u32 mem = ppu2cpu ? memtablew8[bank] : memtabler8[bank];
    for (u32 n = 0; n < count; n++) {
        u32 r = rp[n & 3];
        R.esi = base; R.ebx = bank; R.edx = count - n;
        if (ppu2cpu) {
            R.ecx = cx; zl_mcall(r);              /* PPU/IO read -> al */
            R.ecx = cx; R.ebx = bank; zl_mcall(mem);
            cx += incr;
        } else {
            R.ecx = cx; zl_mcall(mem);            /* memory read -> al */
            cx += incr;
            R.esi = base; R.ecx = cx; zl_mcall(r);
        }
    }
    *(u16 *)(d + 2) = cx;
    if (!ppu2cpu) AddrNoIncr = 0;
}

/* ---------------------------------------------------------------- HDMA ---- */
extern u8 hdmatype, nexthdma, curhdma, hdmadelay, hdmaearlstart, disablehdma, hdmarestart, ewj2hack, dmadata[], hdmadata[], spcRam[];
extern u16 curypos, resolutn;

/* hdmadata record (19 bytes): 4 write-handler ids, bytes per line, u16 address at +17 */
#define H16(h) (*(u16 *)((h) + 17))

static const u8 addrnumt[8] = {1,2,2,4,4,4,4,4};

static u8 rd8(u8 *d, u8 *h, u32 bank, u32 cx)
{
    R.ebx = bank; R.ecx = cx; R.esi = G(d); R.edx = G(h);
    zl_mcall(memtabler8[bank]);
    return (u8)R.eax;
}
static void wr_ppu(u8 *d, u8 *h, int i)
{
    R.esi = G(d); R.edx = G(h);
    zl_mcall(((u32 *)h)[i]);
}

static void hdma_pointers(u8 *d, u8 *h, unsigned mode)
{
    for (int i = 0; i < 4; i++) {
        u16 bx = (u16)((0x2100 | d[1]) + addrwrite[mode][i]);
        if (bx == 0x2118 || bx == 0x2119) bx = 0x2200;       /* "bad hack _Demo_" */
        ((u32 *)h)[i] = REGPTW[bx];
    }
}
static unsigned hdma_mode(u8 *d, u8 *h)
{
    unsigned al = d[0] & 7;
    if (al >= 5) al -= 4;
    h[16] = addrnumt[al];
    return al;
}
static void setuphdma(u8 *d, u8 *h, u8 bit)
{
    u16 a = *(u16 *)(d + 2);
    *(u16 *)(d + 8) = a; H16(h) = a;
    hdma_pointers(d, h, hdma_mode(d, h));
    d[10] = 0;
    hdmatype |= bit;
}
static void setuphdmars(u8 *d, u8 *h)
{
    hdma_pointers(d, h, hdma_mode(d, h));
}

static void hdma_type2(u8 *d, u8 *h)
{
    int n = h[16];
    for (int i = 0; i < n; i++) {
        u16 cx = H16(h); H16(h)++;
        rd8(d, h, d[4], cx);
        wr_ppu(d, h, i);
    }
    *(u16 *)(d + 8) = H16(h);
    d[10]--;
}

static void hdma_type2_indirect(u8 *d, u8 *h)
{
    if (ewj2hack == 1 && d[1] == 0x40 && d[10] == 0xDA) {
        u8 *dest = spcRam + *(u16 *)(spcRam + 0x21);
        for (int n = d[10] - 0x80; n > 0; n--) {
            for (int k = 0; k < 2; k++) {
                u16 cx = *(u16 *)(d + 5); (*(u16 *)(d + 5))++;
                R.edx = n; R.ebx = d[7]; R.ecx = cx; R.esi = G(d);   /* asm uses edx as the loop counter here */
                zl_mcall(memtabler8[d[7]]);
                *dest++ = (u8)R.eax;
            }
        }
        *(u16 *)(d + 5) -= 0xB4;
    }
    int n = h[16];
    for (int i = 0; i < n; i++) {
        u16 cx = *(u16 *)(d + 5); (*(u16 *)(d + 5))++;
        rd8(d, h, d[7], cx);
        wr_ppu(d, h, i);
    }
    d[10]--;
}

static void dohdma(u8 *d, u8 *h, u8 bit)
{
    if (d[0] & 0x40) {
        /* indirect */
        if ((d[10] & 0x7F) == 0) {
            if (!(hdmatype & bit)) H16(h) += 2;
            hdmatype &= ~bit;
            u8 al = rd8(d, h, d[4], H16(h));
            H16(h)++;
            d[10] = al;
            R.ebx = d[4]; R.ecx = H16(h); R.esi = G(d); R.edx = G(h);
            zl_mcall(memtabler16[d[4]]);
            *(u16 *)(d + 5) = (u16)R.eax;
            R.eax = (R.eax & ~0xFFu) | al;
            if (al == 0) { nexthdma ^= bit; *(u16 *)(d + 8) = H16(h); return; }
            if (d[10] > 0x80) { hdma_type2_indirect(d, h); return; }
            int n = h[16]; 
            for (int i = 0; i < n; i++) {
                rd8(d, h, d[7], (u16)(*(u16 *)(d + 5) + i));
                wr_ppu(d, h, i);
            }
        } else if (d[10] & 0x80) { hdma_type2_indirect(d, h); return; }
        *(u16 *)(d + 8) = H16(h);
        d[10]--;
        return;
    }
    if ((d[10] & 0x7F) == 0) {
        if (!(d[10] & 0x80) && !(hdmatype & bit)) H16(h) += h[16];
        hdmatype &= ~bit;
        u8 al = rd8(d, h, d[4], H16(h));
        H16(h)++;
        d[10] = al;
        if (al == 0) { nexthdma ^= bit; *(u16 *)(d + 8) = H16(h); return; }
        if (d[10] > 0x80) { hdma_type2(d, h); return; }
        int n = h[16];
        for (int i = 0; i < n; i++) {
            rd8(d, h, d[4], (u16)(H16(h) + i));
            wr_ppu(d, h, i);
        }
    } else if (d[10] & 0x80) { hdma_type2(d, h); return; }
    *(u16 *)(d + 8) = H16(h);
    d[10]--;
}

#define FOR_CH(al, body) \
    for (int ch = 0; ch < 8; ch++) if ((al) & (1u << ch)) { \
        u8 *d = dmadata + 16 * ch, *h = hdmadata + 19 * ch; u8 bit = 1u << ch; (void)d; (void)h; (void)bit; body }

static void startnexthdma(u8 al)
{
    nexthdma = al;
    if (al == 0) return;
    FOR_CH(al, setuphdma(d, h, bit);)
}

static void reg420Cw_c(u8 al)
{
    curhdma = al;
    if (curypos < resolutn && !disablehdma) {
        nexthdma = al;
        if (al != 0) {
            FOR_CH(al, setuphdma(d, h, bit);)
            hdmadelay = hdmaearlstart;
        }
    }
    hdmarestart = 0;
}
static void starthdma_c(void) { startnexthdma(curhdma); }
static void startnexthdma_c(u32 eax) { startnexthdma((u8)eax); }

static void exechdma_c(void)
{
    if (hdmarestart == 1) {
        u8 al = nexthdma;
        if (al) { FOR_CH(al, setuphdmars(d, h); dohdma(d, h, bit);) }
        hdmarestart = 0;
        return;
    }
    u8 al = nexthdma;
    if (al == 0) return;
    FOR_CH(al, dohdma(d, h, bit);)
}
static void exechdmars_c(void)
{
    u8 al = nexthdma;
    if (al) { FOR_CH(al, setuphdmars(d, h); dohdma(d, h, bit);) }
    hdmarestart = 0;
}


/* ---- overrides: the asm callers expect every register preserved ---- */
#define KEEP(body) do { lift_state_t sv_ = LG; body; LG = sv_; } while (0)
void zl_transdma(void)      { KEEP(transdma_c((u8 *)(uintptr_t)LG.esi)); }
void zl_reg420Cw(void)      { KEEP(reg420Cw_c((u8)LG.eax)); }
void zl_starthdma(void)     { KEEP(starthdma_c()); }
void zl_startnexthdma(void) { KEEP(startnexthdma_c(LG.eax)); }
void zl_exechdma(void)      { KEEP(exechdma_c()); }
void zl_exechdmars(void)    { KEEP(exechdmars_c()); }
