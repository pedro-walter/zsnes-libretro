/*
 * OBC-1 (cpu/memory.asm: InitOBC, OBCRegs, OBCSprites, OBCClear, OBCReadReg, OBCWriteReg).  Register RAM shares the C4
 * handler tables (C4RamR / C4RamW / C4Ram, see mem.c).
 */
#include "zl.h"
#include <stdint.h>
#define Z8(s, o)  (*(volatile u8 *)(uintptr_t)((u32)ZO_##s + (u32)(o)))
#define Z32(s, o) (*(volatile u32 *)(uintptr_t)((u32)ZO_##s + (u32)(o)))

static inline u8 *ram(void) { return (u8 *)(uintptr_t)Z32(C4Ram, 0); }

void zl_InitOBC(void)
{
    u32 base = Z32(romdata, 0) + 4096 * 1024;
    u32 *t = (u32 *)(uintptr_t)base;
    Z32(C4RamR, 0) = base; Z32(C4RamW, 0) = base + 8192 * 4; Z32(C4Ram, 0) = base + 8192 * 8;
    for (int i = 0; i < 8192; i++) { t[i] = ZID_OBCReadReg; t[8192 + i] = ZID_OBCWriteReg; t[16384 + i] = 0; }
    t[0x3A1E] = ZID_OBCClear;
    for (int i = 0; i < 8; i++) t[0x3FF0 + i] = ZID_OBCRegs;
}

static void obc_sprites(void)
{
    u8 *esi = ram(), *edi = esi + 0x1800;
    Z8(NumSprites, 0) = 0;
    u8 r0 = (u8)((Z8(OBCRegArray, 0) + 2) & 0xFE);
    if (r0 == 0xFE || r0 == 0) r0 = Z8(OBCOldRegArray, 0); else Z8(OBCOldRegArray, 0) = r0;
    while (r0 != 0) {
        r0 -= 2;
        u32 b = (u32)esi[6] << 2;
        edi[b] = esi[3]; edi[b + 1] = esi[9]; edi[b + 2] = esi[10]; edi[b + 3] = esi[0xB];
        u32 idx = (esi[6] >> 2) + 512;
        u8 cl = (u8)((esi[6] & 3) * 2);
        u8 al = (u8)((esi[4] & 3) << cl);
        u8 ah = (u8)((0xFC << cl | 0xFC >> (8 - cl)) & 0xFF);
        if (cl == 0) ah = 0xFC;
        edi[idx] = (u8)((edi[idx] & ah) | al);
        Z8(NumSprites, 0)++;
        esi += 16;
    }
    Z8(OBCRegArray, 0) = r0;
}

void zl_OBCClear(void)
{
    obc_sprites();
    Z8(OBCRegArray, 8) = 1;
    Z32(OBClog, 0) = 0;
}

void zl_OBCRegs(void)
{
    u32 ecx = LG.ecx - 0x1FF0;
    u8 al = (u8)LG.eax;
    if (Z8(OBCRegArray, 8)) {
        if (ecx != 6) return;
        for (int i = 0; i < 8; i++) Z8(OBCRegArray, i) = 0;
        Z8(OBCRegArray, 0) = 0xFE;
        Z8(OBCRegArray, 8) = 0;
    }
    u8 *r = ram();
    r[0x1000 + Z32(OBClog, 0)] = (u8)ecx;
    Z32(OBClog, 0)++;
    if ((u8)ecx == 6) {
        Z8(OBCRegArray, 0) += 2;
        u8 bl = Z8(OBCRegArray, 0);
        Z8(OBCRegArray, 1) = bl;
        for (int i = 2; i < 8; i++) Z8(OBCRegArray, i) = bl;
    }
    u32 ebx = Z8(OBCRegArray, ecx);
    if (Z8(OBCIncArray, ecx) == 1) Z8(OBCRegArray, ecx) |= 1;
    u8 *p = r + ecx + (ebx << 3);
    p[0] = al; p[8] = 0xFF;
}

void zl_OBCReadReg(void) { LG.eax = (LG.eax & ~0xFFu) | ram()[LG.ecx]; }
void zl_OBCWriteReg(void) { ram()[LG.ecx] = (u8)LG.eax; }
