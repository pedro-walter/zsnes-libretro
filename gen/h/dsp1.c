#include <stdint.h>
/*
 * DSP-1 command interface (chips/dsp1proc.asm): the SNES-side read/write handlers of the DSP-1 data/status ports and the
 * per-command marshalling between DSP1VARS/DSP1RET and the C math routines in chips/dsp1emu.c.  The command bodies are
 * generated from the asm (all of them have the same shape: copy N input words into the Op variables, run DSPOpXX, copy
 * the results out).
 */
#include "zl.h"
#include <stdint.h>
#define Z8(s, o)  (*(volatile u8 *)(uintptr_t)((u32)ZO_##s + (u32)(o)))
#define Z16(s, o) (*(volatile u16 *)(uintptr_t)((u32)ZO_##s + (u32)(o)))
/* DSP-1 math (chips/dsp1emu.c) */
void DSPOp00(void);
void DSPOp01(void);
void DSPOp02(void);
void DSPOp03(void);
void DSPOp04(void);
void DSPOp06(void);
void DSPOp08(void);
void DSPOp0A(void);
void DSPOp0B(void);
void DSPOp0C(void);
void DSPOp0D(void);
void DSPOp0E(void);
void DSPOp0F(void);
void DSPOp10(void);
void DSPOp11(void);
void DSPOp13(void);
void DSPOp14(void);
void DSPOp18(void);
void DSPOp1B(void);
void DSPOp1C(void);
void DSPOp1D(void);
void DSPOp21(void);
void DSPOp23(void);
void DSPOp28(void);
void DSPOp2B(void);
void DSPOp2D(void);
#define Op00Multiplicand (*(short *)0x341e32c0u)
#define Op00Multiplier (*(short *)0x341e32c8u)
#define Op00Result (*(short *)0x341e32d0u)
#define Op01Xr (*(short *)0x341e34c0u)
#define Op01Yr (*(short *)0x341e34b8u)
#define Op01Zr (*(short *)0x341e34b0u)
#define Op01m (*(short *)0x341e34a8u)
#define Op02AAS (*(short *)0x341e3420u)
#define Op02AZS (*(short *)0x341e3428u)
#define Op02CX (*(short *)0x341e3440u)
#define Op02CY (*(short *)0x341e3448u)
#define Op02FX (*(short *)0x341e33f8u)
#define Op02FY (*(short *)0x341e3400u)
#define Op02FZ (*(short *)0x341e3408u)
#define Op02LES (*(short *)0x341e3418u)
#define Op02LFE (*(short *)0x341e3410u)
#define Op02VOF (*(short *)0x341e3430u)
#define Op02VVA (*(short *)0x341e3438u)
#define Op03F (*(short *)0x341e35a0u)
#define Op03L (*(short *)0x341e35a8u)
#define Op03U (*(short *)0x341e35b0u)
#define Op03X (*(short *)0x341e35b8u)
#define Op03Y (*(short *)0x341e35c0u)
#define Op03Z (*(short *)0x341e35c8u)
#define Op04Angle (*(short *)0x341e32f8u)
#define Op04Cos (*(short *)0x341e3310u)
#define Op04Radius (*(short *)0x341e3300u)
#define Op04Sin (*(short *)0x341e3308u)
#define Op06H (*(short *)0x341e3470u)
#define Op06S (*(short *)0x341e3480u)
#define Op06V (*(short *)0x341e3478u)
#define Op06X (*(short *)0x341e3458u)
#define Op06Y (*(short *)0x341e3460u)
#define Op06Z (*(short *)0x341e3468u)
#define Op08Lh (*(short *)0x341e3338u)
#define Op08Ll (*(short *)0x341e3330u)
#define Op08X (*(short *)0x341e3318u)
#define Op08Y (*(short *)0x341e3320u)
#define Op08Z (*(short *)0x341e3328u)
#define Op0AA (*(short *)0x341e32a0u)
#define Op0AB (*(short *)0x341e32a8u)
#define Op0AC (*(short *)0x341e32b0u)
#define Op0AD (*(short *)0x341e32b8u)
#define Op0AVS (*(short *)0x341e3450u)
#define Op0BS (*(short *)0x341e3648u)
#define Op0BX (*(short *)0x341e3630u)
#define Op0BY (*(short *)0x341e3638u)
#define Op0BZ (*(short *)0x341e3640u)
#define Op0CA (*(short *)0x341e3388u)
#define Op0CX1 (*(short *)0x341e3390u)
#define Op0CX2 (*(short *)0x341e33a0u)
#define Op0CY1 (*(short *)0x341e3398u)
#define Op0CY2 (*(short *)0x341e33a8u)
#define Op0DF (*(short *)0x341e3520u)
#define Op0DL (*(short *)0x341e3528u)
#define Op0DU (*(short *)0x341e3530u)
#define Op0DX (*(short *)0x341e3508u)
#define Op0DY (*(short *)0x341e3510u)
#define Op0DZ (*(short *)0x341e3518u)
#define Op0EH (*(short *)0x341e3488u)
#define Op0EV (*(short *)0x341e3490u)
#define Op0EX (*(short *)0x341e3498u)
#define Op0EY (*(short *)0x341e34a0u)
#define Op0FPass (*(short *)0x341e3538u)
#define Op10Coefficient (*(short *)0x341e32d8u)
#define Op10CoefficientR (*(short *)0x341e32e8u)
#define Op10Exponent (*(short *)0x341e32e0u)
#define Op10ExponentR (*(short *)0x341e32f0u)
#define Op11Xr (*(short *)0x341e34e0u)
#define Op11Yr (*(short *)0x341e34d8u)
#define Op11Zr (*(short *)0x341e34d0u)
#define Op11m (*(short *)0x341e34c8u)
#define Op13F (*(short *)0x341e35d0u)
#define Op13L (*(short *)0x341e35d8u)
#define Op13U (*(short *)0x341e35e0u)
#define Op13X (*(short *)0x341e35e8u)
#define Op13Y (*(short *)0x341e35f0u)
#define Op13Z (*(short *)0x341e35f8u)
#define Op14F (*(short *)0x341e36b0u)
#define Op14L (*(short *)0x341e36b8u)
#define Op14U (*(short *)0x341e36a8u)
#define Op14Xr (*(short *)0x341e3698u)
#define Op14Xrr (*(short *)0x341e36c8u)
#define Op14Yr (*(short *)0x341e36a0u)
#define Op14Yrr (*(short *)0x341e36d0u)
#define Op14Zr (*(short *)0x341e3690u)
#define Op14Zrr (*(short *)0x341e36c0u)
#define Op18D (*(short *)0x341e3360u)
#define Op18R (*(short *)0x341e3358u)
#define Op18X (*(short *)0x341e3340u)
#define Op18Y (*(short *)0x341e3348u)
#define Op18Z (*(short *)0x341e3350u)
#define Op1BS (*(short *)0x341e3668u)
#define Op1BX (*(short *)0x341e3650u)
#define Op1BY (*(short *)0x341e3658u)
#define Op1BZ (*(short *)0x341e3660u)
#define Op1CX (*(short *)0x341e33c0u)
#define Op1CXAR (*(short *)0x341e33e0u)
#define Op1CXBR (*(short *)0x341e33c8u)
#define Op1CY (*(short *)0x341e33b8u)
#define Op1CYAR (*(short *)0x341e33e8u)
#define Op1CYBR (*(short *)0x341e33d0u)
#define Op1CZ (*(short *)0x341e33b0u)
#define Op1CZAR (*(short *)0x341e33f0u)
#define Op1CZBR (*(short *)0x341e33d8u)
#define Op1DF (*(short *)0x341e3558u)
#define Op1DL (*(short *)0x341e3560u)
#define Op1DU (*(short *)0x341e3568u)
#define Op1DX (*(short *)0x341e3540u)
#define Op1DY (*(short *)0x341e3548u)
#define Op1DZ (*(short *)0x341e3550u)
#define Op21Xr (*(short *)0x341e3500u)
#define Op21Yr (*(short *)0x341e34f8u)
#define Op21Zr (*(short *)0x341e34f0u)
#define Op21m (*(short *)0x341e34e8u)
#define Op23F (*(short *)0x341e3600u)
#define Op23L (*(short *)0x341e3608u)
#define Op23U (*(short *)0x341e3610u)
#define Op23X (*(short *)0x341e3618u)
#define Op23Y (*(short *)0x341e3620u)
#define Op23Z (*(short *)0x341e3628u)
#define Op28R (*(short *)0x341e3380u)
#define Op28X (*(short *)0x341e3368u)
#define Op28Y (*(short *)0x341e3370u)
#define Op28Z (*(short *)0x341e3378u)
#define Op2BS (*(short *)0x341e3688u)
#define Op2BX (*(short *)0x341e3670u)
#define Op2BY (*(short *)0x341e3678u)
#define Op2BZ (*(short *)0x341e3680u)
#define Op2DF (*(short *)0x341e3588u)
#define Op2DL (*(short *)0x341e3590u)
#define Op2DU (*(short *)0x341e3598u)
#define Op2DX (*(short *)0x341e3570u)
#define Op2DY (*(short *)0x341e3578u)
#define Op2DZ (*(short *)0x341e3580u)

static void dsp1_00(const u16 *vars, u32 data)
{
    (void)vars; (void)data;
    Z8(DSPDet, 0) |= 0x01;
    Op00Multiplicand = (short)vars[0];
    Op00Multiplier = (short)vars[1];
    DSPOp00();
    Z16(DSP1RET, 0) = (u16)Op00Result;
    Z8(DSP1RLeft, 0) = 1;
}

static void dsp1_10(const u16 *vars, u32 data)
{
    (void)vars; (void)data;
    Op10Coefficient = (short)vars[0];
    Op10Exponent = (short)vars[1];
    DSPOp10();
    Z16(DSP1RET, 0) = (u16)Op10CoefficientR;
    Z16(DSP1RET, 2) = (u16)Op10ExponentR;
    Z8(DSP1RLeft, 0) = 2;
}

static void dsp1_04(const u16 *vars, u32 data)
{
    (void)vars; (void)data;
    Z8(DSPDet, 0) |= 0x02;
    Op04Angle = (short)vars[0];
    Op04Radius = (short)vars[1];
    DSPOp04();
    Z16(DSP1RET, 0) = (u16)Op04Sin;
    Z16(DSP1RET, 2) = (u16)Op04Cos;
    Z8(DSP1RLeft, 0) = 2;
}

static void dsp1_08(const u16 *vars, u32 data)
{
    (void)vars; (void)data;
    Op08X = (short)vars[0];
    Op08Y = (short)vars[1];
    Op08Z = (short)vars[2];
    DSPOp08();
    Z16(DSP1RET, 0) = (u16)Op08Ll;
    Z16(DSP1RET, 2) = (u16)Op08Lh;
    Z8(DSP1RLeft, 0) = 2;
}

static void dsp1_18(const u16 *vars, u32 data)
{
    (void)vars; (void)data;
    Op18X = (short)vars[0];
    Op18Y = (short)vars[1];
    Op18Z = (short)vars[2];
    Op18R = (short)vars[3];
    DSPOp18();
    Z16(DSP1RET, 0) = (u16)Op18D;
    Z8(DSP1RLeft, 0) = 1;
}

static void dsp1_28(const u16 *vars, u32 data)
{
    (void)vars; (void)data;
    Z8(DSPDet, 0) |= 0x04;
    Op28X = (short)vars[0];
    Op28Y = (short)vars[1];
    Op28Z = (short)vars[2];
    DSPOp28();
    Z16(DSP1RET, 0) = (u16)Op28R;
    Z8(DSP1RLeft, 0) = 1;
}

static void dsp1_0C(const u16 *vars, u32 data)
{
    (void)vars; (void)data;
    Z8(DSPDet, 0) |= 0x08;
    Op0CA = (short)vars[0];
    Op0CX1 = (short)vars[1];
    Op0CY1 = (short)vars[2];
    DSPOp0C();
    Z16(DSP1RET, 0) = (u16)Op0CX2;
    Z16(DSP1RET, 2) = (u16)Op0CY2;
    Z8(DSP1RLeft, 0) = 2;
}

static void dsp1_1C(const u16 *vars, u32 data)
{
    (void)vars; (void)data;
    Op1CZ = (short)vars[0];
    Op1CY = (short)vars[1];
    Op1CX = (short)vars[2];
    Op1CXBR = (short)vars[3];
    Op1CYBR = (short)vars[4];
    Op1CZBR = (short)vars[5];
    DSPOp1C();
    Z16(DSP1RET, 0) = (u16)Op1CXAR;
    Z16(DSP1RET, 2) = (u16)Op1CYAR;
    Z16(DSP1RET, 4) = (u16)Op1CZAR;
    Z8(DSP1RLeft, 0) = 3;
}

static void dsp1_02(const u16 *vars, u32 data)
{
    (void)vars; (void)data;
    Z8(DSPDet, 0) |= 0x10;
    Op02FX = (short)vars[0];
    Op02FY = (short)vars[1];
    Op02FZ = (short)vars[2];
    Op02LFE = (short)vars[3];
    Op02LES = (short)vars[4];
    Op02AAS = (short)vars[5];
    Op02AZS = (short)vars[6];
    DSPOp02();
    Z16(DSP1RET, 0) = (u16)Op02VOF;
    Z16(DSP1RET, 2) = (u16)Op02VVA;
    Z16(DSP1RET, 4) = (u16)Op02CX;
    Z16(DSP1RET, 6) = (u16)Op02CY;
    Z8(DSP1RLeft, 0) = 4;
}

static void dsp1_0A(const u16 *vars, u32 data)
{
    (void)vars; (void)data;
    Z8(DSP1COp, 0) = 0x0A;
    Z8(DSPDet, 0) |= 0x20;
    Op0AVS = (short)vars[0];
    DSPOp0A();
    Z16(DSP1RET, 0) = (u16)Op0AA;
    Z16(DSP1RET, 2) = (u16)Op0AB;
    Z16(DSP1RET, 4) = (u16)Op0AC;
    Z16(DSP1RET, 6) = (u16)Op0AD;
    Z8(DSP1RLeft, 0) = 4;
}

static void dsp1_06(const u16 *vars, u32 data)
{
    (void)vars; (void)data;
    Z8(DSPDet, 0) |= 0x40;
    Op06X = (short)vars[0];
    Op06Y = (short)vars[1];
    Op06Z = (short)vars[2];
    DSPOp06();
    Z16(DSP1RET, 0) = (u16)Op06H;
    Z16(DSP1RET, 2) = (u16)Op06V;
    Z16(DSP1RET, 4) = (u16)Op06S;
    Z8(DSP1RLeft, 0) = 3;
}

static void dsp1_0E(const u16 *vars, u32 data)
{
    (void)vars; (void)data;
    Op0EH = (short)vars[0];
    Op0EV = (short)vars[1];
    DSPOp0E();
    Z16(DSP1RET, 0) = (u16)Op0EX;
    Z16(DSP1RET, 2) = (u16)Op0EY;
    Z8(DSP1RLeft, 0) = 2;
}

static void dsp1_01(const u16 *vars, u32 data)
{
    (void)vars; (void)data;
    Op01m = (short)vars[0];
    Op01Zr = (short)vars[1];
    Op01Yr = (short)vars[2];
    Op01Xr = (short)vars[3];
    DSPOp01();
}

static void dsp1_11(const u16 *vars, u32 data)
{
    (void)vars; (void)data;
    Op11m = (short)vars[0];
    Op11Zr = (short)vars[1];
    Op11Yr = (short)vars[2];
    Op11Xr = (short)vars[3];
    DSPOp11();
}

static void dsp1_21(const u16 *vars, u32 data)
{
    (void)vars; (void)data;
    Op21m = (short)vars[0];
    Op21Zr = (short)vars[1];
    Op21Yr = (short)vars[2];
    Op21Xr = (short)vars[3];
    DSPOp21();
}

static void dsp1_0D(const u16 *vars, u32 data)
{
    (void)vars; (void)data;
    Op0DX = (short)vars[0];
    Op0DY = (short)vars[1];
    Op0DZ = (short)vars[2];
    DSPOp0D();
    Z16(DSP1RET, 0) = (u16)Op0DF;
    Z16(DSP1RET, 2) = (u16)Op0DL;
    Z16(DSP1RET, 4) = (u16)Op0DU;
    Z8(DSP1RLeft, 0) = 3;
}

static void dsp1_0F(const u16 *vars, u32 data)
{
    (void)vars; (void)data;
    DSPOp0F();
    Z16(DSP1RET, 0) = (u16)Op0FPass;
    Z8(DSP1RLeft, 0) = 1;
}

static void dsp1_1D(const u16 *vars, u32 data)
{
    (void)vars; (void)data;
    Op1DX = (short)vars[0];
    Op1DY = (short)vars[1];
    Op1DZ = (short)vars[2];
    DSPOp1D();
    Z16(DSP1RET, 0) = (u16)Op1DF;
    Z16(DSP1RET, 2) = (u16)Op1DL;
    Z16(DSP1RET, 4) = (u16)Op1DU;
    Z8(DSP1RLeft, 0) = 3;
}

static void dsp1_2D(const u16 *vars, u32 data)
{
    (void)vars; (void)data;
    Op2DX = (short)vars[0];
    Op2DY = (short)vars[1];
    Op2DZ = (short)vars[2];
    DSPOp2D();
    Z16(DSP1RET, 0) = (u16)Op2DF;
    Z16(DSP1RET, 2) = (u16)Op2DL;
    Z16(DSP1RET, 4) = (u16)Op2DU;
    Z8(DSP1RLeft, 0) = 3;
}

static void dsp1_03(const u16 *vars, u32 data)
{
    (void)vars; (void)data;
    Op03F = (short)vars[0];
    Op03L = (short)vars[1];
    Op03U = (short)vars[2];
    DSPOp03();
    Z16(DSP1RET, 0) = (u16)Op03X;
    Z16(DSP1RET, 2) = (u16)Op03Y;
    Z16(DSP1RET, 4) = (u16)Op03Z;
    Z8(DSP1RLeft, 0) = 3;
}

static void dsp1_13(const u16 *vars, u32 data)
{
    (void)vars; (void)data;
    Op13F = (short)vars[0];
    Op13L = (short)vars[1];
    Op13U = (short)vars[2];
    DSPOp13();
    Z16(DSP1RET, 0) = (u16)Op13X;
    Z16(DSP1RET, 2) = (u16)Op13Y;
    Z16(DSP1RET, 4) = (u16)Op13Z;
    Z8(DSP1RLeft, 0) = 3;
}

static void dsp1_23(const u16 *vars, u32 data)
{
    (void)vars; (void)data;
    Op23F = (short)vars[0];
    Op23L = (short)vars[1];
    Op23U = (short)vars[2];
    DSPOp23();
    Z16(DSP1RET, 0) = (u16)Op23X;
    Z16(DSP1RET, 2) = (u16)Op23Y;
    Z16(DSP1RET, 4) = (u16)Op23Z;
    Z8(DSP1RLeft, 0) = 3;
}

static void dsp1_0B(const u16 *vars, u32 data)
{
    (void)vars; (void)data;
    Op0BX = (short)vars[0];
    Op0BY = (short)vars[1];
    Op0BZ = (short)vars[2];
    DSPOp0B();
    Z16(DSP1RET, 0) = (u16)Op0BS;
    Z8(DSP1RLeft, 0) = 1;
}

static void dsp1_1B(const u16 *vars, u32 data)
{
    (void)vars; (void)data;
    Op1BX = (short)vars[0];
    Op1BY = (short)vars[1];
    Op1BZ = (short)vars[2];
    DSPOp1B();
    Z16(DSP1RET, 0) = (u16)Op1BS;
    Z8(DSP1RLeft, 0) = 1;
}

static void dsp1_2B(const u16 *vars, u32 data)
{
    (void)vars; (void)data;
    Op2BX = (short)vars[0];
    Op2BY = (short)vars[1];
    Op2BZ = (short)vars[2];
    DSPOp2B();
    Z16(DSP1RET, 0) = (u16)Op2BS;
    Z8(DSP1RLeft, 0) = 1;
}

static void dsp1_14(const u16 *vars, u32 data)
{
    (void)vars; (void)data;
    Op14Zr = (short)vars[0];
    Op14Xr = (short)vars[1];
    Op14Yr = (short)vars[2];
    Op14U = (short)vars[3];
    Op14F = (short)vars[4];
    Op14L = (short)vars[5];
    DSPOp14();
    Z16(DSP1RET, 0) = (u16)Op14Zrr;
    Z16(DSP1RET, 2) = (u16)Op14Xrr;
    Z16(DSP1RET, 4) = (u16)Op14Yrr;
    Z8(DSP1RLeft, 0) = 3;
}

/* result of the last DSP-1 command: write handlers dispatch here once all argument words have arrived */
static void dsp1_process(u32 data)
{
    const u16 *vars = (const u16 *)(uintptr_t)(u32)ZO_DSP1VARS;
    switch (Z8(DSP1COp, 0)) {
    case 0x00: dsp1_00(vars, data); break;
    case 0x10: dsp1_10(vars, data); break;
    case 0x04: dsp1_04(vars, data); break;
    case 0x08: dsp1_08(vars, data); break;
    case 0x18: dsp1_18(vars, data); break;
    case 0x28: dsp1_28(vars, data); break;
    case 0x0C: dsp1_0C(vars, data); break;
    case 0x1C: dsp1_1C(vars, data); break;
    case 0x02: dsp1_02(vars, data); break;
    case 0x0A: case 0x1A: dsp1_0A(vars, data); break;
    case 0x06: dsp1_06(vars, data); break;
    case 0x0E: dsp1_0E(vars, data); break;
    case 0x01: dsp1_01(vars, data); break;
    case 0x11: dsp1_11(vars, data); break;
    case 0x21: dsp1_21(vars, data); break;
    case 0x0D: dsp1_0D(vars, data); break;
    case 0x0F: dsp1_0F(vars, data); break;
    case 0x1D: dsp1_1D(vars, data); break;
    case 0x2D: dsp1_2D(vars, data); break;
    case 0x03: dsp1_03(vars, data); break;
    case 0x13: dsp1_13(vars, data); break;
    case 0x23: dsp1_23(vars, data); break;
    case 0x0B: dsp1_0B(vars, data); break;
    case 0x1B: dsp1_1B(vars, data); break;
    case 0x2B: dsp1_2B(vars, data); break;
    case 0x14: dsp1_14(vars, data); break;
    }
}

#define AL (LG.eax & 0xFF)
#define BL (LG.ebx & 0xFF)
static inline void set_ax(u32 v) { LG.eax = (LG.eax & ~0xFFFFu) | (v & 0xFFFF); }
static inline void set_al(u32 v) { LG.eax = (LG.eax & ~0xFFu) | (v & 0xFF); }

/* ---- data/status port reads ---- */
void zl_DSP1Read8b3F(void)
{
    if (!(LG.ecx & 0x8000) && BL != 0xE0) { zl_mcall(ZID_regaccessbankr8); return; }
    set_al(0x80);
}
void zl_DSP1Read8b(void) { set_al(LG.ecx >= 0x7000 ? 0x80 : 0); }

static void read16_data(void)                          /* the ".movestuff" part: next result word */
{
    u32 w = Z16(DSP1RET, Z8(DSP1CPtrR, 0) * 2);
    set_ax(w);
    Z8(DSP1CPtrR, 0)++;
    if (--Z8(DSP1RLeft, 0) != 0) return;
    if (Z8(DSP1COp, 0) != 0x0A) return;
    DSPOp0A();
    Z16(DSP1RET, 0) = (u16)Op0AA; Z16(DSP1RET, 2) = (u16)Op0AB; Z16(DSP1RET, 4) = (u16)Op0AC; Z16(DSP1RET, 6) = (u16)Op0AD;
    Z8(DSP1RLeft, 0) = 4; Z8(DSP1CPtrR, 0) = 0;
}
static void read16_status(void)
{
    set_ax(Z8(DSP1WLeft, 0) ? 0xC000 : 0x8000);
}
void zl_DSP1Read16b(void)
{
    if (LG.ecx >= 0x7000) { read16_status(); return; }
    if (Z8(DSP1RLeft, 0) != 0) read16_data(); else set_ax(0);
}
void zl_DSP1Read16b3F(void)
{
    if (!(LG.ecx & 0x8000) && BL != 0xE0) { zl_mcall(ZID_regaccessbankr16); return; }
    LG.ecx |= 0x8000;
    if (LG.ecx >= 0xC000) { read16_status(); return; }
    if (Z8(DSP1RLeft, 0) != 0) read16_data(); else set_ax(0);
}

/* ---- data port writes ---- */
void zl_DSP1Write8b(void)
{
    static const u8 init[][2] = { {0x00,2},{0x10,2},{0x04,2},{0x08,3},{0x18,4},{0x28,3},{0x0C,3},{0x1C,6},{0x02,7},{0x0A,1},{0x1A,1},{0x06,3},{0x0E,2},
        {0x01,4},{0x11,4},{0x21,4},{0x0D,3},{0x1D,3},{0x2D,3},{0x03,3},{0x13,3},{0x23,3},{0x0B,3},{0x1B,3},{0x2B,3},{0x14,6},{0x0F,1} };
    u32 al = AL;
    Z8(DSPFuncUsed, al) = 1;
    Z8(DSP1COp, 0) = (u8)al;
    Z8(DSP1CPtrW, 0) = 0;
    for (unsigned i = 0; i < sizeof init / sizeof init[0]; i++) if (init[i][0] == al) Z8(DSP1WLeft, 0) = init[i][1];
}
void zl_DSP1Write16b(void)
{
    if (Z8(DSP1WLeft, 0) == 0) return;
    Z16(DSP1VARS, Z8(DSP1CPtrW, 0) * 2) = (u16)LG.eax;
    Z8(DSP1CPtrW, 0)++;
    if (--Z8(DSP1WLeft, 0) != 0) return;
    Z8(DSP1CPtrR, 0) = 0; Z8(DSP1RLeft, 0) = 0;
    dsp1_process(LG.eax & 0xFFFF);
}
void zl_DSP1Write8b3F(void)
{
    if (!(LG.ecx & 0x8000) && BL != 0xE0) { zl_mcall(ZID_regaccessbankw8); return; }
    zl_DSP1Write8b();
}
void zl_DSP1Write16b3F(void)
{
    if (!(LG.ecx & 0x8000) && BL != 0xE0) { zl_mcall(ZID_regaccessbankw16); return; }
    zl_DSP1Write16b();
}
