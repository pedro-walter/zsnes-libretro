#include <stdint.h>
/*
 * SA-1 / S-DD1 register handlers (chips/sa1regs.asm): $2200-$2239 writes, $2300-$230E reads, the S-DD1 bank registers, the IRAM
 * accessors and the arithmetic unit.  Register ABI of the originals: al = value (writes) / result (reads); ebx is scratch.
 * The two DMA engines (executesa1dma, sa1chconv) are ported below.
 */
#include "zl.h"
#include <stdint.h>
#define Z8(s, o)  (*(volatile u8 *)(uintptr_t)((u32)ZO_##s + (u32)(o)))
#define Z16(s, o) (*(volatile u16 *)(uintptr_t)((u32)ZO_##s + (u32)(o)))
#define Z32(s, o) (*(volatile u32 *)(uintptr_t)((u32)ZO_##s + (u32)(o)))
#define M8(a)  (*(volatile u8 *)(uintptr_t)(u32)(a))
#define M16(a) (*(volatile u16 *)(uintptr_t)(u32)(a))
#define M32(a) (*(volatile u32 *)(uintptr_t)(u32)(a))
#define AL (LG.eax & 0xFF)
static inline void set_al(u32 v) { LG.eax = (LG.eax & ~0xFFu) | (v & 0xFF); }
static inline void set_bl(u32 v) { LG.ebx = (LG.ebx & ~0xFFu) | (v & 0xFF); }

/* ---- ROM bank switching ($2220-$2223) ---- */
static void bank_switch(int n, u32 mm_first, u32 m2_first, u32 upper_base)
{
    u32 al = AL, eax = LG.eax, i;
    Z8(SA1BankVal, n) = (u8)al;
    u32 *snesmmap = (u32 *)(uintptr_t)(u32)ZO_snesmmap, *snesmap2 = (u32 *)(uintptr_t)(u32)ZO_snesmap2;
    u32 romdata = Z32(romdata, 0);
    if (Z8(NumofBanks, 0) == 64) eax &= 1;
    eax &= 7; eax <<= 20;
    u32 start = (al & 0x80) ? eax : upper_base;
    start += romdata; start -= 0x8000;
    for (i = 0; i < 0x20; i++, start += 0x8000) snesmmap[mm_first + i] = start;
    u32 p = eax + romdata;
    for (i = 0; i < 0x10; i++, p += 0x10000) { snesmap2[m2_first + i] = p; snesmmap[m2_first + i] = p; }
    LG.ebx = (u32)(uintptr_t)&snesmap2[m2_first + 0x10];        /* the asm leaves ebx behind the last entry */
}
void zl_sa12220w(void) { bank_switch(0, 0x00, 0xC0, 0); }
void zl_sa12221w(void) { bank_switch(1, 0x20, 0xD0, 0x100000); }
void zl_sa12222w(void) { bank_switch(2, 0x80, 0xE0, 0x200000); }
void zl_sa12223w(void) { bank_switch(3, 0xA0, 0xF0, 0x300000); }

static void sdd1_bank(int n, u32 first)
{
    u32 eax = AL, i;
    u32 *snesmmap = (u32 *)(uintptr_t)(u32)ZO_snesmmap, *snesmap2 = (u32 *)(uintptr_t)(u32)ZO_snesmap2;
    Z8(SDD1BankA, n) = (u8)eax;
    eax = ((eax & 7) << 20) + Z32(romdata, 0);
    for (i = 0; i < 0x10; i++, eax += 0x10000) { snesmap2[first + i] = eax; snesmmap[first + i] = eax; }
    LG.ebx = (u32)(uintptr_t)&snesmap2[first + 0x10];
}
void zl_sdd14804w(void) { sdd1_bank(0, 0xC0); }
void zl_sdd14805w(void) { sdd1_bank(1, 0xD0); }
void zl_sdd14806w(void) { sdd1_bank(2, 0xE0); }
void zl_sdd14807w(void) { sdd1_bank(3, 0xF0); }

/* ---- control / IRQ ---- */
void zl_sa12200w(void)
{
    u32 al = AL;
    set_bl(al & 0x0F); Z8(SA1Message, 0) = (u8)(al & 0x0F);
    if (al & 0x80) Z8(SA1DoIRQ, 0) |= 1;
    if (al & 0x10) Z8(SA1DoIRQ, 0) |= 2;
    if ((Z8(SA1Control, 0) & 0x20) && !(al & 0x20)) {
        Z8(SA1Control, 0) = (u8)al;
        u32 romdata = Z32(romdata, 0);
        Z32(SA1BankPtr, 0) = romdata;
        u32 ebx = (u16)Z16(SA1ResetV, 0) + romdata - 0x8000;
        Z32(SA1Ptr, 0) = ebx;
        Z8(SA1xpb, 0) = 0;
        Z16(SA1xs, 0) = 0x1FF;
        Z32(SA1RegPCS, 0) = romdata - 0x8000;
        LG.ebx = 0;
        return;
    }
    Z8(SA1Control, 0) = (u8)al;
}
void zl_sa12201w(void) { Z8(SA1IRQEnable, 0) = (u8)AL; }
void zl_sa12202w(void)
{
    u32 al = AL;
    if (al & 0x80) { Z8(SA1IRQExec, 0) &= 0xFE; Z8(SA1DoIRQ, 0) &= 0xFB; }
    if (al & 0x20) { Z8(SA1IRQExec, 0) &= 0xFD; Z8(SA1DoIRQ, 0) &= 0xF7; }
}
void zl_sa12203w(void) { Z8(SA1ResetV, 0) = (u8)AL; }
void zl_sa12204w(void) { Z8(SA1ResetV, 1) = (u8)AL; }
void zl_sa12205w(void) { Z8(SA1NMIV, 0) = (u8)AL; }
void zl_sa12206w(void) { Z8(SA1NMIV, 1) = (u8)AL; }
void zl_sa12207w(void) { Z8(SA1IRQV, 0) = (u8)AL; }
void zl_sa12208w(void) { Z8(SA1IRQV, 1) = (u8)AL; }
void zl_sa12209w(void)
{
    u32 al = AL, bx;
    Z8(SA1IRQData, 1) = (u8)al;
    if (al & 0x80) Z8(SA1DoIRQ, 0) |= 4;
    Z8(SA1Message, 1) = (u8)(al & 0x0F);
    bx = (al & 0x40) ? Z16(SNSIRQV, 0) : Z16(irqv2, 0);
    Z16(irqv, 0) = (u16)bx;
    bx = (al & 0x10) ? Z16(SNSNMIV, 0) : Z16(nmiv2, 0);
    Z16(nmiv, 0) = (u16)bx;
    LG.ebx = (LG.ebx & ~0xFFFFu) | bx;
}
void zl_sa1220Aw(void) { Z8(SA1IRQEn, 0) = (u8)AL; }
void zl_sa1220Bw(void)
{
    u32 al = AL;
    if (al & 0x80) { Z8(SA1IRQExec, 1) = 0; Z8(SA1DoIRQ, 0) &= 0xFE; }
    if (al & 0x10) { Z8(SA1IRQExec, 2) = 0; Z8(SA1DoIRQ, 0) &= 0xFD; }
}
void zl_sa1220Cw(void) { Z8(SNSNMIV, 0) = (u8)AL; }
void zl_sa1220Dw(void) { Z8(SNSNMIV, 1) = (u8)AL; }
void zl_sa1220Ew(void) { Z8(SNSIRQV, 0) = (u8)AL; }
void zl_sa1220Fw(void) { Z8(SNSIRQV, 1) = (u8)AL; }
void zl_sa12210w(void) { Z8(SA1TimerSet, 0) = (u8)AL; }
void zl_sa12211w(void) { Z32(SA1TimerVal, 0) = 0; }
void zl_sa12212w(void) { Z8(SA1TimerCount, 0) = (u8)AL; }
void zl_sa12213w(void) { Z8(SA1TimerCount, 1) = (u8)AL; }
void zl_sa12214w(void) { Z8(SA1TimerCount, 2) = (u8)AL; }
void zl_sa12215w(void) { Z8(SA1TimerCount, 3) = (u8)AL; }

/* ---- BW-RAM mapping ---- */
void zl_sa12224w(void)
{
    u32 ebx = (((AL) & 0x1F) << 13) + Z32(romdata, 0) + 1024 * 4096 - 0x6000;
    LG.ebx = ebx;
    Z32(SNSBWPtr, 0) = ebx;
    if (Z8(SA1Status, 0) == 0) Z32(CurBWPtr, 0) = ebx;
}
void zl_sa12225w(void)
{
    u32 al = AL, ebx;
    Z8(BWUsed2, 0) = (u8)al;
    if (!(al & 0x80)) {
        ebx = ((al & 0x1F) << 13) + Z32(romdata, 0) + 1024 * 4096 - 0x6000;
        LG.ebx = ebx;
        Z32(SA1BWPtr, 0) = ebx;
        if (Z8(SA1Status, 0) != 0) Z32(CurBWPtr, 0) = ebx;
        Z8(BWShift, 0) = 0; Z8(BWAndAddr, 0) = 0; Z8(BWAnd, 0) = 0xFF; Z8(BWRAnd, 0) = 0;
        return;
    }
    ebx = al & 0x7F;
    if (Z8(SA1Overflow, 1) & 0x80) {
        ebx <<= 11;
        Z8(BWShift, 0) = 2; Z8(BWAndAddr, 0) = 3; Z8(BWAnd, 0) = 3; Z8(BWRAnd, 0) = 0xFC;
    } else {
        Z8(BWShift, 0) = 1; Z8(BWAndAddr, 0) = 1; Z8(BWAnd, 0) = 0x0F; Z8(BWRAnd, 0) = 0xF0;
        ebx &= 0x3F; ebx <<= 12;
    }
    ebx += Z32(romdata, 0) + 1024 * 4096;
    LG.ebx = ebx;
    Z32(SA1BWPtr, 0) = ebx;
    if (Z8(SA1Status, 0) != 0) Z32(CurBWPtr, 0) = ebx;
}
void zl_sa1223Fw(void) { Z8(SA1Overflow, 1) = (u8)AL; }

/* ---- arithmetic unit ---- */
static void update_arith(void)
{
    if (Z8(SA1ARC, 1) != 1) return;
    Z8(SA1ARC, 1) = 0;
    u8 arc = Z8(SA1ARC, 0);
    if (arc & 3) {
        if (arc & 2) return;                                    /* cumulative sum: handled on the AR2 high write */
        /* division */
        int32_t num = (int16_t)Z16(SA1AR1, 0);
        u32 den = Z16(SA1AR2, 0);
        if (den == 0) { Z16(SA1ARR1, 0) = 0; Z16(SA1ARR1, 2) = 0; return; }
        Z16(SA1ARR1, 0) = (u16)(num / (int32_t)den);
        Z16(SA1ARR1, 2) = (u16)(num % (int32_t)den);
        return;
    }
    int32_t p = (int32_t)(int16_t)Z16(SA1AR1, 0) * (int32_t)(int16_t)Z16(SA1AR2, 0);
    Z16(SA1ARR1, 0) = (u16)p;
    Z16(SA1ARR1, 2) = (u16)((u32)p >> 16);
}
void zl_sa12250w(void)
{
    u32 al = AL;
    Z8(SA1ARC, 0) = (u8)al; Z8(SA1ARC, 1) = 1;
    if (al & 2) { Z16(SA1ARR1, 0) = 0; Z16(SA1ARR1, 2) = 0; Z16(SA1ARR1, 4) = 0; }
}
void zl_sa12251w(void) { Z8(SA1AR1, 0) = (u8)AL; Z8(SA1ARC, 1) = 1; }
void zl_sa12252w(void) { Z8(SA1AR1, 1) = (u8)AL; Z8(SA1ARC, 1) = 1; }
void zl_sa12253w(void) { Z8(SA1AR2, 0) = (u8)AL; Z8(SA1ARC, 1) = 1; }
void zl_sa12254w(void)
{
    Z8(SA1AR2, 1) = (u8)AL; Z8(SA1ARC, 1) = 1;
    if (!(Z8(SA1ARC, 0) & 2)) { update_arith(); return; }
    int32_t p = (int32_t)(int16_t)Z16(SA1AR1, 0) * (int32_t)(int16_t)Z16(SA1AR2, 0);
    Z8(SA1Overflow, 0) = 0;
    u32 old = Z32(SA1ARR1, 0), sum = old + (u32)p;
    Z32(SA1ARR1, 0) = sum;
    {   /* adc byte[SA1ARR2],0 : carry out of the 32-bit add propagates into the 5th byte; carry out of that sets the flag */
        u32 b = Z8(SA1ARR2, 0) + (sum < old ? 1u : 0u);
        Z8(SA1ARR2, 0) = (u8)b;
        if (b > 0xFF) Z8(SA1Overflow, 0) = 0x80;
    }
}

/* ---- variable-length bit port ---- */
void zl_sa12258w(void)
{
    u32 al = AL, bl = al & 0x0F;
    Z8(VarLenBarrel, 2) = (u8)al;
    if (bl == 0) bl = 16;
    Z8(VarLenBarrel, 3) = (u8)bl;
    if (al & 0x80) { Z8(VarLenBarrel, 0) = (u8)bl; Z8(VarLenBarrel, 1) = (u8)bl; }
    set_bl(bl);
}
static void varlen_addr(int b)                       /* sa12259w..sa1225Bw: byte b of the address */
{
    u8 al = (u8)AL;
    Z8(VarLenAddr, b) = al; Z8(VarLenAddrB, b) = al;
    Z8(VarLenBarrel, 0) = 0; Z8(VarLenBarrel, 1) = 0;
}
void zl_sa12259w(void) { varlen_addr(0); }
void zl_sa1225Aw(void) { varlen_addr(1); }
void zl_sa1225Bw(void) { varlen_addr(2); }
static void varlen_read(u32 addr_sym, u32 bar_idx, int high)   /* sa1230Cr / sa1230Dr */
{
    u32 bank = M8(addr_sym + 2), off = M16(addr_sym);
    u32 base = (off & 0x8000) ? ((u32 *)(uintptr_t)(u32)ZO_snesmmap)[bank] : ((u32 *)(uintptr_t)(u32)ZO_snesmap2)[bank];
    u32 ebx = M32(base + off);
    ebx >>= (Z8(VarLenBarrel, 1) & 31);
    set_al(high ? (ebx >> 8) : ebx);
    LG.ebx = ebx;
    if (Z8(VarLenBarrel, 2) & 0x80) {
        u32 bl = Z8(VarLenBarrel, 3);
        Z8(VarLenBarrel, bar_idx) = (u8)(Z8(VarLenBarrel, bar_idx) + bl);
        bl = Z8(VarLenBarrel, bar_idx);
        set_bl(bl);
        if (bl > 16) {
            Z8(VarLenBarrel, bar_idx) = (u8)(Z8(VarLenBarrel, bar_idx) - 16);
            M32(addr_sym) = M32(addr_sym) + 2;
            M8(addr_sym + 3) = 0;
        }
    }
}
void zl_sa1230Cr(void) { varlen_read((u32)ZO_VarLenAddr, 0, 0); }
void zl_sa1230Dr(void) { varlen_read((u32)ZO_VarLenAddrB, 1, 1); }
void zl_sa1230Er(void) { set_al(0x10); }

/* ---- status / timer / arithmetic reads ---- */
void zl_sa12300r(void)
{
    u32 al = Z8(SA1Message, 1), bl;
    if (Z8(SA1IRQExec, 0) & 1) al |= 0x80;
    if (Z8(SA1IRQExec, 0) & 2) al |= 0x20;
    bl = Z8(SA1IRQData, 1) & 0x50;
    set_bl(bl); set_al(al | bl);
}
void zl_sa12301r(void)
{
    u32 al = Z8(SA1Message, 0);
    if (Z8(SA1IRQExec, 1) & 1) al |= 0x80;
    set_al(al);
}
void zl_sa12306r(void) { set_al(Z8(SA1ARR1, 0)); }
void zl_sa12307r(void) { set_al(Z8(SA1ARR1, 1)); }
void zl_sa12308r(void) { set_al(Z8(SA1ARR1, 2)); }
void zl_sa12309r(void) { set_al(Z8(SA1ARR1, 3)); }
void zl_sa1230Ar(void) { set_al(Z8(SA1ARR2, 0)); }
void zl_sa1230Br(void) { set_al(Z8(SA1Overflow, 0)); }
void zl_sa12302r(void)
{
    if (Z8(SA1TimerSet, 0) & 0x80) { set_al(Z8(SA1TimerCount, 0)); return; }
    set_al((u8)(Z8(CurrentExecSA1, 0) << 2) + (u8)((LG.edx >> 8) & 0xFF));
}
void zl_sa12303r(void)
{
    if (Z8(SA1TimerSet, 0) & 0x80) { set_al(Z8(SA1TimerCount, 1) & 1); return; }
    set_al(Z8(CurrentExecSA1, 0) >> 3);
}
void zl_sa12304r(void)
{
    if (Z8(SA1TimerSet, 0) & 0x80) { u32 bx = (u16)(Z16(SA1TimerCount, 1) >> 1); LG.ebx = (LG.ebx & ~0xFFFFu) | bx; set_al(bx); return; }
    set_al(Z8(curypos, 0));
}
void zl_sa12305r(void)
{
    if (Z8(SA1TimerSet, 0) & 0x80) { u32 bx = (u16)(Z16(SA1TimerCount, 2) >> 1); LG.ebx = (LG.ebx & ~0xFFFFu) | bx; set_al(bx & 1); return; }
    set_al(Z8(curypos, 1));
}

/* ---- IRAM access (ecx = address) ---- */
void zl_IRamRead(void)   { set_al(M8((u32)ZO_IRAM + LG.ecx - 0x3000)); }
void zl_IRamWrite(void)  { M8((u32)ZO_IRAM + LG.ecx - 0x3000) = (u8)AL; }
void zl_IRamWrite2(void) { M8((u32)ZO_IRAM + LG.ecx - 0x3000) = (u8)AL; LG.edx &= 0xFFFF00FFu; }

/* ---- DMA registers ---- */
void zl_sa12230w(void) { Z8(SA1DMAInfo, 0) = (u8)AL; }
void zl_sa12231w(void) { Z8(SA1DMAChar, 0) = (u8)AL; }
void zl_sa12232w(void) { Z8(SA1DMASource, 0) = (u8)AL; }
void zl_sa12233w(void) { Z8(SA1DMASource, 1) = (u8)AL; }
void zl_sa12234w(void) { Z8(SA1DMASource, 2) = (u8)AL; }
void zl_sa12235w(void) { Z8(SA1DMADest, 0) = (u8)AL; }
/* DMA engine (sa1regs.asm executesa1dma): sa1dmaptr = destination, sa1dmaptrs = source; copies SA1DMACount bytes */
static void sa1dma_exec(u32 dst)
{
    u32 src, n, i;
    Z32(sa1dmaptr, 0) = dst;
    if (Z8(SA1DMAInfo, 0) & 1) src = (Z32(SA1DMASource, 0) & 0x3FFFF) + Z32(SA1RAMArea, 0);
    else if (Z8(SA1DMAInfo, 0) & 2) src = (Z32(SA1DMASource, 0) & 0x7FF) + (u32)ZO_IRAM;
    else src = M32((u32)ZO_snesmmap + 4u * Z8(SA1DMASource, 2)) + (u16)Z16(SA1DMASource, 0);
    Z32(sa1dmaptrs, 0) = src;
    n = Z32(SA1DMACount, 0);
    for (i = 0; i < n; i++) M8(dst + i) = M8(src + i);
}
static void sa1dmairam(void) { sa1dma_exec((Z32(SA1DMADest, 0) & 0x7FF) + (u32)ZO_IRAM); }
static void sa1dmabwram(void) { sa1dma_exec((Z32(SA1DMADest, 0) & 0x3FFFF) + Z32(SA1RAMArea, 0)); }

/* Character conversion DMA (sa1chconv): converts through a scratch area behind the ROM, then copies the result back over the source */
static void setbits(u32 b, u8 al, const u16 *tab)
{
    /* tab: eight (mask-bit, value) pairs: mask in tab[2k], value in tab[2k+1]; values >= 0x8000|... are for [b+16] when tab[2k] has bit 8 set */
    int k;
    for (k = 0; k < 8; k++) {
        u16 m = tab[2 * k], v = tab[2 * k + 1];
        if (al & (m & 0xFF)) M16(b + ((m & 0x100) ? 16 : 0)) |= v;
    }
}
static const u16 t4[4][16] = {
    { 0x10, 0x0001, 0x20, 0x0100, 0x140, 0x0001, 0x180, 0x0100, 0x01, 0x0002, 0x02, 0x0200, 0x104, 0x0002, 0x108, 0x0200 },
    { 0x10, 0x0004, 0x20, 0x0400, 0x140, 0x0004, 0x180, 0x0400, 0x01, 0x0008, 0x02, 0x0800, 0x104, 0x0008, 0x108, 0x0800 },
    { 0x10, 0x0010, 0x20, 0x1000, 0x140, 0x0010, 0x180, 0x1000, 0x01, 0x0020, 0x02, 0x2000, 0x104, 0x0020, 0x108, 0x2000 },
    { 0x10, 0x0040, 0x20, 0x4000, 0x140, 0x0040, 0x180, 0x4000, 0x01, 0x0080, 0x02, 0x8000, 0x104, 0x0080, 0x108, 0x8000 },
};
static const u16 t2[2][16] = {
    { 0x40, 0x0001, 0x80, 0x0100, 0x10, 0x0002, 0x20, 0x0200, 0x04, 0x0004, 0x08, 0x0400, 0x01, 0x0008, 0x02, 0x0800 },
    { 0x40, 0x0010, 0x80, 0x1000, 0x10, 0x0020, 0x20, 0x2000, 0x04, 0x0040, 0x08, 0x4000, 0x01, 0x0080, 0x02, 0x8000 },
};
static void sa1chconv(void)
{
    u32 ebx, edx, dstp, srcp, scratch, i, j, k;
    Z8(SA1DoIRQ, 0) |= 8;
    dstp = (Z32(SA1DMADest, 0) & 0x3FFFF) + Z32(SA1RAMArea, 0);
    Z8(debstop4, 0) = 1;
    Z32(sa1dmaptr, 0) = dstp;
    srcp = (Z32(SA1DMASource, 0) & 0x3FFFF) + Z32(SA1RAMArea, 0);
    Z32(sa1dmaptrs, 0) = srcp;
    LG.ebx = srcp;
    scratch = Z32(romdata, 0) + 4096 * 1024 + 1024 * 1024;
    if (Z8(SA1DMAChar, 0) & 1) {            /* 4 bit */
        ebx = scratch; edx = srcp;
        for (i = 0; i < 16; i++) {
            u32 bx0 = ebx, dx0 = edx;
            for (j = 0; j < 32; j++) {
                u32 bx1 = ebx, dx1 = edx;
                for (k = 0; k < 8; k++) {
                    M16(ebx) = 0;
                    setbits(ebx, M8(edx + 3), t4[0]); setbits(ebx, M8(edx + 2), t4[1]);
                    setbits(ebx, M8(edx + 1), t4[2]); setbits(ebx, M8(edx), t4[3]);
                    ebx += 2; edx += 128;
                }
                ebx = bx1; edx = dx1; edx += 4; ebx += 32;
            }
            ebx = bx0; edx = dx0; edx += 128 * 8; ebx += 128 * 8;
        }
        for (i = 0; i < 10 * 128 * 8; i++) M8(srcp + i) = M8(scratch + i);
    } else if (Z8(SA1DMAChar, 0) & 2) {     /* 2 bit */
        ebx = scratch; edx = srcp;
        for (i = 0; i < 16; i++) {
            u32 bx0 = ebx, dx0 = edx;
            for (j = 0; j < 32; j++) {
                u32 bx1 = ebx, dx1 = edx;
                for (k = 0; k < 8; k++) {
                    M16(ebx) = 0;
                    setbits(ebx, M8(edx + 1), t2[0]); setbits(ebx, M8(edx), t2[1]);
                    ebx += 2; edx += 64;
                }
                ebx = bx1; edx = dx1; edx += 2; ebx += 16;
            }
            ebx = bx0; edx = dx0; edx += 64 * 8; ebx += 64 * 8;
        }
        for (i = 0; i < 10 * 64 * 8; i++) M8(srcp + i) = M8(scratch + i);
    } else {
        LG.ebx = dstp;                      /* 8 bit: nothing implemented */
    }
}
void zl_sa12236w(void)
{
    Z8(SA1DMADest, 1) = (u8)AL;
    if (Z8(SA1DMAInfo, 0) & 0x10) { sa1chconv(); return; }
    if (!(Z8(SA1DMAInfo, 0) & 4)) sa1dmairam();
}
void zl_sa12237w(void)
{
    Z8(SA1DMADest, 2) = (u8)AL;
    if (Z8(SA1DMAInfo, 0) & 0x10) return;
    if (Z8(SA1DMAInfo, 0) & 4) sa1dmabwram();
}
void zl_sa12238w(void) { Z8(SA1DMACount, 0) = (u8)AL; }
void zl_sa12239w(void) { Z8(SA1DMACount, 1) = (u8)AL; }
