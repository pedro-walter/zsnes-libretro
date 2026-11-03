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
#include <string.h>
#include <stddef.h>
__attribute__((visibility("hidden"))) u32 zs_dma_use_asm;
__attribute__((constructor)) static void dma_init(void){ zs_dma_use_asm = getenv("ZS_DMA_ASM") != 0; }

static int brk(void){ static int b=-1; if (b<0) b = getenv("ZS_DMA_BREAK") ? atoi(getenv("ZS_DMA_BREAK")) : 0; return b; }
static unsigned long hdma_ops, hdma_ind;
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
    if (brk() == 1 && !ppu2cpu && incr) cx += incr;   /* negative control: skew source address */
   

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
            zs_R.esi = base; zs_R.ecx = cx; zs_mcall(r);
        }
    }
    *(u16 *)(d + 2) = cx;
    if (!ppu2cpu) AddrNoIncr = 0;
}

/* ---------------------------------------------------------------- HDMA ---- */
extern u8 hdmatype, nexthdma, curhdma, hdmadelay, hdmaearlstart, disablehdma, hdmarestart, ewj2hack, dmadata[], hdmadata[], spcRam[];
extern u16 curypos, resolutn;
extern void *memtabler16[256];

typedef struct { void *ptr[4]; u8 count; } __attribute__((packed)) hd_t;   /* 4 write handlers, bytes per line, then u16 address at +17 */
#define H16(h) (*(u16 *)((h) + 17))

static const u8 addrnumt[8] = {1,2,2,4,4,4,4,4};

static u8 rd8(u8 *d, u8 *h, u32 bank, u32 cx)
{
    zs_R.ebx = bank; zs_R.ecx = cx; zs_R.esi = (u32)d; zs_R.edx = (u32)h;
    zs_mcall(memtabler8[bank]);
    return (u8)zs_R.eax;
}
static void wr_ppu(u8 *d, u8 *h, int i)
{
    zs_R.esi = (u32)d; zs_R.edx = (u32)h;
    zs_mcall(((void **)h)[i]);
}

static void hdma_pointers(u8 *d, u8 *h, unsigned mode)
{
    for (int i = 0; i < 4; i++) {
        u16 bx = (u16)((0x2100 | d[1]) + addrwrite[mode][i]);
        if (bx == 0x2118 || bx == 0x2119) bx = 0x2200;       /* "bad hack _Demo_" */
        ((void **)h)[i] = regptw[bx];
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
                zs_R.edx = n; zs_R.ebx = d[7]; zs_R.ecx = cx; zs_R.esi = (u32)d;   /* asm uses edx as the loop counter here */
                zs_mcall(memtabler8[d[7]]);
                *dest++ = (u8)zs_R.eax;
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
            zs_R.ebx = d[4]; zs_R.ecx = H16(h); zs_R.esi = (u32)d; zs_R.edx = (u32)h;
            zs_mcall(memtabler16[d[4]]);
            *(u16 *)(d + 5) = (u16)zs_R.eax;
            zs_R.eax = (zs_R.eax & ~0xFFu) | al;
            if (al == 0) { nexthdma ^= bit; *(u16 *)(d + 8) = H16(h); return; }
            if (d[10] > 0x80) { hdma_type2_indirect(d, h); return; }
            int n = h[16]; hdma_ind++; if (brk() == 3 && n > 1) n--;
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
        int n = h[16]; hdma_ops++;
        if (brk() == 2) n = 0;
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

__attribute__((force_align_arg_pointer, visibility("hidden")))
void zs_reg420Cw_c(u8 al)
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
__attribute__((force_align_arg_pointer, visibility("hidden")))
void zs_starthdma_c(void) { startnexthdma(curhdma); }
__attribute__((force_align_arg_pointer, visibility("hidden")))
void zs_startnexthdma_c(u32 eax) { startnexthdma((u8)eax); }

__attribute__((force_align_arg_pointer, visibility("hidden")))
void zs_exechdma_c(void)
{
    if (getenv("ZS_DMA_COUNT") && (hdma_ops & 1023) == 1) fprintf(stderr, "hdma direct=%lu indirect=%lu\n", hdma_ops, hdma_ind);
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
__attribute__((force_align_arg_pointer, visibility("hidden")))
void zs_exechdmars_c(void)
{
    u8 al = nexthdma;
    if (al) { FOR_CH(al, setuphdmars(d, h); dohdma(d, h, bit);) }
    hdmarestart = 0;
}

/* ------------------------------------------------ differential fuzz: asm vs C ---- */
extern u32 zs_fz_log[], zs_fz_n;
extern u8 zs_fz_mem[];
extern void zs_fz_rd8(void), zs_fz_rd16(void), zs_fz_wr0(void), zs_fz_wr1(void), zs_fz_wr2(void), zs_fz_wr3(void),
            zs_fz_wr4(void), zs_fz_wr5(void), zs_fz_wr6(void), zs_fz_wr7(void);
extern void zs_fz_callasm(void *fn, u32 eax);
extern void reg420Cw_asm(void), startnexthdma_asm(void), exechdma_asm(void), exechdmars_asm(void);

static u32 rs;
static u32 rnd(void) { rs ^= rs << 13; rs ^= rs >> 17; rs ^= rs << 5; return rs; }

typedef struct {
    u8 dma[128], hd[152], spc[0x10200];
    u8 hdmatype, nexthdma, curhdma, hdmadelay, hdmarestart, ewj;
    u16 curypos;
    u32 logn, log[8 * 4096];
} snap_t;

static void take(snap_t *s)
{
    memcpy(s->dma, dmadata, 128); memcpy(s->hd, hdmadata, 152); memcpy(s->spc, spcRam, sizeof s->spc);
    s->hdmatype = hdmatype; s->nexthdma = nexthdma; s->curhdma = curhdma; s->hdmadelay = hdmadelay;
    s->hdmarestart = hdmarestart; s->ewj = ewj2hack; s->curypos = curypos;
    s->logn = zs_fz_n; memcpy(s->log, zs_fz_log, s->logn * 32);
}
static void put(const snap_t *s)
{
    memcpy(dmadata, s->dma, 128); memcpy(hdmadata, s->hd, 152); memcpy(spcRam, s->spc, sizeof s->spc);
    hdmatype = s->hdmatype; nexthdma = s->nexthdma; curhdma = s->curhdma; hdmadelay = s->hdmadelay;
    hdmarestart = s->hdmarestart; ewj2hack = s->ewj; curypos = s->curypos;
    zs_fz_n = 0;
}

static void normlog(snap_t *s)
{
    for (u32 k = 0; k < s->logn; k++) { u32 *e = s->log + 8 * k; e[1] = (e[0] <= 2) ? 0 : (e[1] & 0xFF); e[2] &= 0xFFFF; }
}

unsigned zs_hdma_fuzz(unsigned iters, unsigned seed)
{
    rs = seed ? seed : 1;
    void *wrs[8] = { zs_fz_wr0, zs_fz_wr1, zs_fz_wr2, zs_fz_wr3, zs_fz_wr4, zs_fz_wr5, zs_fz_wr6, zs_fz_wr7 };
    static void *mytab[0x2400];
    for (int i = 0; i < 0x2400; i++) mytab[i] = wrs[i & 7];
    void **save_regptw = regptw;
    void *save8[256], *save16[256];
    memcpy(save8, memtabler8, sizeof save8); memcpy(save16, memtabler16, sizeof save16);
    for (int i = 0; i < 256; i++) { memtabler8[i] = zs_fz_rd8; memtabler16[i] = zs_fz_rd16; }
    regptw = mytab;
    u8 save_dma[128], save_hd[152]; u16 save_resolutn = resolutn;
    memcpy(save_dma, dmadata, 128); memcpy(save_hd, hdmadata, 152);
    u8 save_dis = disablehdma, save_early = hdmaearlstart;
    snap_t *a = malloc(sizeof *a), *c = malloc(sizeof *c), *init = malloc(sizeof *init);
    u8 *spc_save = malloc(sizeof a->spc); memcpy(spc_save, spcRam, sizeof a->spc);
    unsigned bad = 0; unsigned long cover[8] = {0};
    for (unsigned it = 0; it < iters; it++) {
        for (int i = 0; i < 65536; i++) {
            u32 r = rnd() & 0xFF; zs_fz_mem[i] = (r < 30) ? 0 : (r < 120 ? (r | 0x80) : (r & 0x7F));
        }
        for (int ch = 0; ch < 8; ch++) {
            u8 *d = dmadata + 16 * ch, *h = hdmadata + 19 * ch;
            for (int i = 0; i < 16; i++) d[i] = rnd();
            if (rnd() & 1) d[1] = rnd() % 0x40;
            if (rnd() % 8 == 0) d[1] = 0x40;
            switch (rnd() % 6) { case 0: d[10] = 0; break; case 1: d[10] = 0x80; break; case 2: d[10] = 0xDA; break;
                                 case 3: d[10] = 1 + rnd() % 0x7F; break; default: d[10] = rnd(); }
            for (int i = 0; i < 4; i++) ((void **)h)[i] = wrs[rnd() & 7];
            h[16] = 1 + rnd() % 4;
            *(u16 *)(h + 17) = rnd();
        }
        for (int i = 0; i < (int)sizeof a->spc; i++) spcRam[i] = rnd();
        hdmatype = rnd(); nexthdma = rnd() % 4 ? rnd() : 0; curhdma = rnd(); hdmadelay = rnd();
        hdmarestart = rnd() & 1; ewj2hack = (rnd() % 4) == 0; curypos = rnd() % 300; resolutn = (rnd() & 1) ? 225 : 240;
        disablehdma = (rnd() % 8) == 0; hdmaearlstart = rnd();
        int op = rnd() % 4; u32 arg = rnd() & 0xFF;
        take(init);
        for (int pass = 0; pass < 2; pass++) {
            put(init);
            u8 de = disablehdma, ea = hdmaearlstart; (void)de; (void)ea;
            if (pass == 0) {
                void *fn = op == 0 ? (void *)reg420Cw_asm : op == 1 ? (void *)startnexthdma_asm : op == 2 ? (void *)exechdma_asm : (void *)exechdmars_asm;
                zs_fz_callasm(fn, arg);
            } else {
                switch (op) { case 0: zs_reg420Cw_c(arg); break; case 1: zs_startnexthdma_c(arg); break;
                              case 2: zs_exechdma_c(); break; default: zs_exechdmars_c(); }
            }
            take(pass ? c : a);
        }
        cover[op]++;
        normlog(a); normlog(c);
        if (memcmp(a, c, offsetof(snap_t, log)) || a->logn != c->logn || memcmp(a->log, c->log, a->logn * 32)) {
            if (bad < 3) { for (size_t k = 0; k < offsetof(snap_t, log); k++) if (((u8*)a)[k] != ((u8*)c)[k]) { fprintf(stderr, "  byte diff at +%zu (dma 0, hd 128, spc 280, scalars %zu) asm=%02x c=%02x\n", k, offsetof(snap_t, hdmatype), ((u8*)a)[k], ((u8*)c)[k]); break; } }
            if (bad < 3 && a->logn == c->logn) for (u32 k = 0; k < a->logn; k++) if (memcmp(a->log + 8*k, c->log + 8*k, 32)) {
                fprintf(stderr, "  first log diff #%u: asm %x %x %x %x %x %x / c %x %x %x %x %x %x\n", k, a->log[8*k], a->log[8*k+1], a->log[8*k+2], a->log[8*k+3], a->log[8*k+4]-(u32)dmadata, a->log[8*k+5], c->log[8*k], c->log[8*k+1], c->log[8*k+2], c->log[8*k+3], c->log[8*k+4]-(u32)dmadata, c->log[8*k+5]); break; }
            if (bad++ < 3) fprintf(stderr, "hdma fuzz mismatch it=%u op=%d arg=%02x logn %u/%u dma:%d hd:%d spc:%d\n", it, op, arg,
                a->logn, c->logn, memcmp(a->dma, c->dma, 128), memcmp(a->hd, c->hd, 152), memcmp(a->spc, c->spc, sizeof a->spc));
        }
    }
    fprintf(stderr, "hdma fuzz: %u iterations, ops reg420C/startnext/exec/execrs = %lu/%lu/%lu/%lu\n", iters, cover[0], cover[1], cover[2], cover[3]);
    memcpy(spcRam, spc_save, sizeof a->spc);
    memcpy(memtabler8, save8, sizeof save8); memcpy(memtabler16, save16, sizeof save16);
    regptw = save_regptw; memcpy(dmadata, save_dma, 128); memcpy(hdmadata, save_hd, 152);
    resolutn = save_resolutn; disablehdma = save_dis; hdmaearlstart = save_early;
    return bad;
}
