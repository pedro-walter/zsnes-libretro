/*
 * SA-1 / S-DD1 register handlers (chips/sa1regs.asm): $2200-$2239 writes, $2300-$230E reads, the S-DD1 bank registers, the IRAM
 * accessors and the arithmetic unit.  Register ABI of the originals: al = value (writes) / result (reads); ebx is scratch.
 * The two DMA engines (executesa1dma, sa1chconv) stay in asm and are called through zl_mcall.
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
void zl_sa12236w(void)
{
    Z8(SA1DMADest, 1) = (u8)AL;
    if (Z8(SA1DMAInfo, 0) & 0x10) { zl_mcall(ZID_sa1chconv); return; }
    if (!(Z8(SA1DMAInfo, 0) & 4)) zl_mcall(ZID_sa1dmairam);
}
void zl_sa12237w(void)
{
    Z8(SA1DMADest, 2) = (u8)AL;
    if (Z8(SA1DMAInfo, 0) & 0x10) return;
    if (Z8(SA1DMAInfo, 0) & 4) zl_mcall(ZID_sa1dmabwram);
}
void zl_sa12238w(void) { Z8(SA1DMACount, 0) = (u8)AL; }
void zl_sa12239w(void) { Z8(SA1DMACount, 1) = (u8)AL; }
