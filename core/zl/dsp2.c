/*
 * DSP-2 command engine (chips/dsp2proc.asm).  A queue of "inside commands" (the enforcer) predicts what the game writes
 * next; every write pops one entry and runs it.  The register results (eax/ebx/ecx on exit) are reproduced exactly, since
 * the callers are the memory-handler tables.
 */
#include "zl.h"
#include <stdint.h>
#define Z8(s, o)  (*(volatile u8 *)(uintptr_t)((u32)ZO_##s + (u32)(o)))
#define Z16(s, o) (*(volatile u16 *)(uintptr_t)((u32)ZO_##s + (u32)(o)))
#define Z32(s, o) (*(volatile u32 *)(uintptr_t)((u32)ZO_##s + (u32)(o)))

#define F_HALT 1
#define F_AUTO 2
#define F_NOCHK 4

static void add2queue(void)
{
    u32 w = Z32(dsp2enforcerWriterCursor, 0);
    Z32(dsp2enforcerWriterCursor, 0) = (w + 1) & 511;
    Z32(dsp2enforcerQueue, w * 8) = Z32(dsp2enforcer, 0);
    Z32(dsp2enforcerQueue, w * 8 + 4) = Z32(dsp2enforcer, 4);
}
static void enq(u32 cmd)                          /* mov dword[enforcer+0],cmd / [enforcer+4],8000h / call add */
{
    Z32(dsp2enforcer, 0) = cmd;
    Z32(dsp2enforcer, 4) = 0x8000;
    add2queue();
}
static void enq_b1_b4(u32 cmd, u8 b1, u8 b4, int set1, int set4)
{
    Z32(dsp2enforcer, 0) = cmd;
    Z32(dsp2enforcer, 4) = 0x8000;
    if (set1) Z8(dsp2enforcer, 1) = b1;
    if (set4) Z8(dsp2enforcer, 4) = b4;
    add2queue();
}

void zl_InitDSP2(void)
{
    Z32(dsp2state, 0) = 0;
    Z32(dsp2enforcerQueue, 0) = 0;
    Z32(dsp2enforcerQueue, 4) = 0x8000;
    Z32(dsp2enforcerReaderCursor, 0) = 0;
    Z32(dsp2enforcerWriterCursor, 0) = 1;
}

void zl_DSP2Read8b(void)
{
    u32 cx = LG.ecx;
    if ((Z8(dsp2state, 0) & F_HALT) || !(cx & 0x8000) || (cx & 0x7000)) { LG.eax = 0; return; }
    cx &= 255;
    LG.eax = (LG.eax & ~0xFFu) | Z8(dsp2buffer, cx);
    LG.ecx = 0;
    if (Z8(dsp2state, 0) & F_AUTO) Z32(dsp2buffer, 0) = (u32)((int32_t)Z32(dsp2buffer, 0) >> 8);
}
void zl_DSP2Read16b(void) { LG.eax = 0; }
void zl_DSP2Write16b(void) { LG.eax = 0; }

void zl_DSP2Write8b(void)
{
    u32 ecx = LG.ecx, ebx = LG.ebx;
    if (Z8(dsp2state, 0) & F_HALT) { LG.eax = 0; return; }
    u8 in = (u8)LG.eax;
    Z8(dsp2input, 0) = in;
    u32 rc = Z32(dsp2enforcerReaderCursor, 0);
    Z32(dsp2enforcer, 0) = Z32(dsp2enforcerQueue, rc * 8);
    Z32(dsp2enforcer, 4) = Z32(dsp2enforcerQueue, rc * 8 + 4);
    ebx = 0;
    int halt = 0, done_ecx_set = 0;
    if (!(Z8(dsp2state, 0) & F_NOCHK) && Z16(dsp2enforcer, 4) != (u16)ecx) { halt = 1; goto fin; }
    switch (Z8(dsp2enforcer, 0)) {
    case 0x0B:
        ecx = Z8(dsp2enforcer, 1);
        Z8(dsp2buffer, ecx) = in;
        break;
    case 0x0A: {
        u8 al = (u8)((int8_t)in >> 1);
        Z8(dsp2f0dSizeNew, 0) = al;
        if (al == 0) { halt = 1; goto fin; }
        u8 cl = 0, bl = al, bh = Z8(dsp2f0dSizeOrg, 0);
        do {
            u32 ax = (u32)cl * bl;
            u8 q = bh ? (u8)((ax & 0xFFFF) / bh) : 0;
            enq_b1_b4(0x0B, q, cl, 1, 1);
            cl++;
        } while (cl != Z8(dsp2f0dSizeOrg, 0));
        ebx = 0; ecx = cl;
        enq(0);                                    /* .queueincoming */
        break;
    }
    case 0x09: {
        u8 al = (u8)((int8_t)in >> 1);
        Z8(dsp2f0dSizeOrg, 0) = al;
        if (al == 0) { halt = 1; goto fin; }
        enq(0x0A);
        break;
    }
    case 0x08: {
        u32 idx = Z8(dsp2enforcer, 1);
        ecx = (ecx & ~0xFFu) | in;
        Z8(dsp2buffer, idx) = in;
        if (idx == 3) {
            u32 ax = (u32)Z8(dsp2buffer, 0) * Z8(dsp2buffer, 2);
            Z32(dsp2buffer, 0) = ax & 0xFFFF;
            Z8(dsp2state, 0) |= F_AUTO;
        }
        break;
    }
    case 0x07: {
        u8 cl = (u8)((in << 4) | (in >> 4));
        ecx = (ecx & ~0xFFu) | cl;
        Z8(dsp2buffer, Z8(dsp2enforcer, 1)) = cl;
        break;
    }
    case 0x06: {
        if (in == 0) { halt = 1; goto fin; }
        u8 cl = in, al = 0;
        do {
            cl--;
            enq_b1_b4(7, cl, al, 1, 1);
            al++;
        } while (cl != 0);
        ecx = 0;
        enq(0);
        break;
    }
    case 0x05: {
        u32 idx = Z8(dsp2enforcer, 4);
        u8 cl = Z8(dsp2buffer, idx), ch = in & 0xF0;
        if (ch != Z8(dsp2f03KeyHi, 0)) cl = (cl & 0x0F) | ch;
        ch = in & 0x0F;
        if (ch != Z8(dsp2f03KeyLo, 0)) cl = (cl & 0xF0) | ch;
        Z8(dsp2buffer, idx) = cl;
        ecx = (ecx & 0xFFFF0000u) | ((u32)ch << 8) | cl;
        break;
    }
    case 0x04: {
        Z8(dsp2buffer, Z8(dsp2enforcer, 4)) = in;
        ecx = 0;
        break;
    }
    case 0x03: {
        if (in == 0) { halt = 1; goto fin; }
        u8 al = 0;
        do { enq_b1_b4(4, 0, al, 0, 1); al++; } while (al != in);
        al = 0;
        do { enq_b1_b4(5, 0, al, 0, 1); al++; } while (al != in);
        ecx = 0;
        enq(0);
        break;
    }
    case 0x02: {
        u8 al = in & 0x0F;
        Z8(dsp2f03KeyLo, 0) = al;
        Z8(dsp2f03KeyHi, 0) = (u8)(al << 4);
        break;
    }
    case 0x01: {
        u32 c = (u32)Z8(dsp2enforcer, 4) << 3;
        u8 t = in;
        Z8(dsp2inputTemp, 0) = in;
        do {
            u32 b = Z8(dsp2f01TblByte, c);
            u8 m = Z8(dsp2f01TblBitMask, c);
            if (t & 1) Z8(dsp2buffer, b) |= m; else Z8(dsp2buffer, b) &= (u8)~m;
            t = (u8)((int8_t)t >> 1);
            c++;
        } while (c & 7);
        Z8(dsp2inputTemp, 0) = t;
        ebx = 0; ecx = 0;
        break;
    }
    case 0x00: {
        Z8(dsp2state, 0) &= (u8)~(F_AUTO | F_NOCHK);
        switch (in) {
        case 0x0D: enq(9); break;
        case 0x09: {
            u8 al = 4, ah = 0;
            do { enq_b1_b4(8, ah, 0, 1, 0); ah++; al--; } while (al);
            enq(0);
            break;
        }
        case 0x06: enq(6); break;
        case 0x05: Z8(dsp2state, 0) |= F_NOCHK; enq(3); break;
        case 0x03: enq(2); enq(0); break;
        case 0x01: {
            u8 al = 0;
            do { enq_b1_b4(1, 0, al, 0, 1); al++; } while (al != 32);
            ecx = 0;
            enq(0);
            break;
        }
        case 0x0F: enq(0); break;
        default: halt = 1; goto fin;
        }
        break;
    }
    default: halt = 1; goto fin;
    }
    (void)done_ecx_set;
    Z32(dsp2enforcerReaderCursor, 0) = (Z32(dsp2enforcerReaderCursor, 0) + 1) & 511;
fin:
    if (halt) Z8(dsp2state, 0) |= F_HALT;
    LG.eax = 0; LG.ebx = ebx; LG.ecx = ecx;
}
