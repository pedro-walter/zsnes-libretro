#include <stdint.h>
#define _GNU_SOURCE
/*
 * S-DSP register writes + key-on/voice start, ported from cpu/dsp.asm
 * (WDSPReg00..FF, VoiceAdsr/VoiceAdsr2/VoiceGain, SwitchSustain, ProcessGain,
 * ProcessGain2, key-on/off) and cpu/dspproc.asm (VoiceNStart, VoiceStarter).
 * Faithful translation, quirks included (envelope "Time"/"IncNumber" setup,
 * 16-bit address wrap of the sample directory, divide-by-zero guarded).
 *
 * The asm dspWptr[] table is overwritten by per-register stubs that call
 * zs_dspw_c(); the original handlers are kept in zs_dspw_orig[] for the
 * differential checks:
 *   ZS_DSPW_ASM=1   use the original asm handlers
 *   ZS_DSPW_DIFF=1  run asm + C on identical state per write, compare, keep asm
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "zl.h"

typedef uint8_t u8;
typedef int8_t s8;
typedef uint16_t u16;
typedef int16_t s16;
typedef uint32_t u32;
typedef int32_t s32;
typedef uint64_t u64;

#define HID __attribute__((visibility("hidden")))

#define DSPMem ((u8 *)0x34015520u)
#define spcRam ((u8 *)0x34037e80u)
#define VolumeConvTable ((u16 *)0x3414a971u)
#define VolumeTableb ((u8 *)0x3402e8ecu)
#define Voice0Status ((u8 *)0x34033958u)
#define echoon0 ((u8 *)0x34033cf4u)
#define Voice0End ((u8 *)0x34033a10u)
#define Voice0Loop ((u8 *)0x34033a08u)
#define Voice0State ((u8 *)0x34033a48u)
#define Voice0Noise ((u8 *)0x34033a18u)
#define GainDecBendDataPos ((u8 *)0x34033c08u)
#define GainDecBendDataDat ((u8 *)0x34033c30u)
#define AdsrBlocksLeft ((u8 *)0x34033c38u)
#define SoundLooped0 ((u8 *)0x34033e34u)
#define Voice0FirstBlock ((u8 *)0x34033e40u)
#define GainDecBendDataTime ((u32 *)0x34033c10u)
#define AdsrNextTimeDepth ((u32 *)0x34033c40u)
#define Voice0Freq ((u32 *)0x34033928u)
#define Voice0Ptr ((u32 *)0x34033960u)
#define Voice0LoopPtr ((u32 *)0x34033980u)
#define Voice0Prev0 ((u32 *)0x340339c8u)
#define Voice0Prev1 ((u32 *)0x340339e8u)
#define Voice0Time ((u32 *)0x34033a50u)
#define Voice0IncNumber ((u32 *)0x34033b00u)
#define Voice0EnvInc ((u32 *)0x34033ba0u)
#define Voice0Pitch ((u16 *)0x34033948u)
#define BRRPlace0 ((u32 *)0x340338e8u)
extern s32 PSampleBuf[24 * 8];
extern s32 FIRTAPVal0[8];
#define GlobalVL (*(u32 *)0x34033cfcu)
#define GlobalVR (*(u32 *)0x34033d00u)
#define EchoVL (*(u32 *)0x34033d04u)
#define EchoVR (*(u32 *)0x34033d08u)
#define EchoFB (*(u32 *)0x34033d4cu)
#define MaxEcho (*(u32 *)0x3402e9ecu)
#define NoiseInc (*(u32 *)0x34033e50u)
#define dspPAdj (*(u32 *)0x3402e2aeu)
#define DecayRate ((u32 *)0x3402ea70u)
#define SustainRate ((u32 *)0x3402ea90u)
#define Increase ((u32 *)0x3402eb18u)
#define Decrease ((u32 *)0x3402ec18u)
#define DecreaseRateExp ((u32 *)0x3402ec98u)
#define AttackRate ((u32 *)0x3402ea30u)
#define EchoRate ((u32 *)0x3402e9f0u)
#define NoiseSpeeds ((u32 *)0x3402e320u)
#define AdsrSustLevLoc ((u8 *)0x3402edd6u)
extern u8 MusicVol, KeyOnStA, KeyOnStB, SoundTest, keyonsn, MuteVoiceF, TempValueSnd, spcres;
extern u32 TimeTemp[8], IncNTemp[8], EnvITemp[8], spc700temp[2];
extern u8 StatTemp[8];
static unsigned long cov[16];
#define COV(n) (cov[n]++)

#define VCT8(idx) (((u8 *)VolumeConvTable)[(idx) * 2])
static inline u32 udiv(u32 a, u32 b) { return b ? a / b : 0; }
#define ENV2(v) (((u8 *)&Voice0EnvInc[v])[2])

/* ---------- envelope setup helpers ---------- */
static void load_ptrs(int v)
{
    u32 off = (u32)DSPMem[4 + v * 16] << 2;
    u32 idx = (u16)(((u32)DSPMem[0x5D] << 8) + off);
    Voice0Ptr[v] = *(u16 *)((u8 *)spcRam + idx);
    Voice0LoopPtr[v] = *(u16 *)((u8 *)spcRam + idx + 2);
}

static void update_pitch(int v)
{
    u16 p = *(u16 *)&DSPMem[2 + v * 16];
    if (Voice0Pitch[v] != p) {
        Voice0Pitch[v] = p;
        Voice0Freq[v] = (u32)(((u64)(p & 0x3FFF) * dspPAdj) >> 8);
    }
}

static void reset_brr(int v)
{
    BRRPlace0[v * 2] = 0x10000000;
    Voice0Prev0[v] = 0;
    Voice0Prev1[v] = 0;
    Voice0End[v] = 0;
    Voice0Loop[v] = 0;
    PSampleBuf[v * 24 + 16] = 0;
    PSampleBuf[v * 24 + 17] = 0;
    PSampleBuf[v * 24 + 18] = 0;
}

/* sets envelope state/rates from the GAIN register assuming a fresh start (VoiceStart / ProcessGain) */
static void gain_fresh(int v)
{
    u8 g = DSPMem[7 + v * 16];
    u32 ebx;
    if (!(g & 0x80)) {                                   /* direct */
        Voice0EnvInc[v] = 0;
        ENV2(v) = g & 0x7F;
        Voice0Time[v] = 0xFFFFFFFFu;
        Voice0IncNumber[v] = 0;
        Voice0State[v] = 4;
    } else if (!(g & 0x40)) {                            /* decrease */
        if (!(g & 0x20)) {                               /* linear */
            ebx = Decrease[g & 0x1F];
            Voice0EnvInc[v] = 0x7FFFFF;
            Voice0Time[v] = ebx;
            Voice0IncNumber[v] = (u32)-(s32)udiv(127u << 16, ebx);
            Voice0State[v] = 5;
        } else {                                         /* exponential */
            ebx = DecreaseRateExp[g & 0x1F] >> 5;
            Voice0EnvInc[v] = 0x7FFFFF;
            Voice0Time[v] = ebx;
            GainDecBendDataTime[v] = ebx;
            GainDecBendDataPos[v] = 0;
            GainDecBendDataDat[v] = 127;
            Voice0IncNumber[v] = (u32)-(s32)udiv(9u << 16, ebx);
            Voice0State[v] = 7;
        }
    } else {                                             /* increase */
        ebx = Increase[g & 0x1F];
        Voice0EnvInc[v] = 0;
        Voice0Time[v] = ebx;
        Voice0IncNumber[v] = udiv(127u << 16, ebx);
        if (g & 0x20) {                                  /* bent */
            ebx = Voice0Time[v];
            ebx = ebx - (ebx >> 2) - 1;
            Voice0Time[v] = ebx;
            Voice0State[v] = 6;
        } else
            Voice0State[v] = 3;
    }
}

/* ProcessGain2: change the GAIN mode of a running voice (keeps the current envelope level) */
static void gain_running(int v)
{
    u8 g = DSPMem[7 + v * 16];
    u8 e = ENV2(v);
    u32 ebx;
    if (!(g & 0x80)) {
        Voice0EnvInc[v] = 0;
        ENV2(v) = g & 0x7F;
        Voice0Time[v] = 0xFFFFFFFFu;
        Voice0IncNumber[v] = 0;
        Voice0State[v] = 4;
    } else if (!(g & 0x40)) {
        if (!(g & 0x20)) {
            ebx = Decrease[g & 0x1F];
            Voice0Time[v] = ebx;
            Voice0IncNumber[v] = (u32)-(s32)udiv((u32)e << 16, ebx);
            Voice0State[v] = 5;
        } else {
            u32 eax; COV(10);
            ebx = DecreaseRateExp[g & 0x1F] >> 5;
            Voice0Time[v] = ebx;
            GainDecBendDataTime[v] = ebx;
            eax = (u32)VCT8((118u << 8) | e);
            eax = (u32)-(s32)(eax << 16) + ((u32)e << 16);
            GainDecBendDataPos[v] = 0;
            Voice0IncNumber[v] = (u32)-(s32)udiv(eax, ebx);
            GainDecBendDataDat[v] = e;
            Voice0State[v] = 7;
        }
    } else {
        u8 a = (u8)(e + 1);
        COV(11);
        ebx = Increase[g & 0x1F];
        Voice0Time[v] = ebx;
        if (a & 0x80) a = 127;
        a ^= 127;
        Voice0IncNumber[v] = udiv((u32)a << 16, ebx);
        if (g & 0x20) {
            ebx = ebx - (ebx >> 2) - 1;
            Voice0Time[v] = ebx;
            Voice0State[v] = 6;
        } else
            Voice0State[v] = 3;
    }
}

/* SwitchSustain: re-derive the decay/sustain schedule of a running ADSR voice */
static void switch_sustain(int v)
{
    u8 a1 = DSPMem[5 + v * 16], a2 = DSPMem[6 + v * 16];
    u32 edx, ebx, eax;
    GainDecBendDataDat[v] = ENV2(v);
    if (Voice0State[v] == 8 || Voice0State[v] < 2) GainDecBendDataDat[v] = 0x7F;
    edx = DecayRate[(a1 >> 4) & 7];
    ebx = SustainRate[a2 & 0x1F];
    if (edx >= ebx) {                                   /* decay slower than sustain */
        COV(0);
        u32 t;
        edx -= ebx;
        eax = (u32)(((u64)edx * (u32)((a2 >> 5) ^ 7)) / 7);
        ebx += eax;
        Voice0EnvInc[v] = 0x7FFFFF;
        ebx >>= 5;
        Voice0Time[v] = ebx;
        GainDecBendDataTime[v] = ebx;
        t = (u32)VCT8((118u << 8) | 0x7F);
        eax = (u32)-(s32)(t << 16) + (0x7Fu << 16);
        GainDecBendDataPos[v] = 0;
        Voice0IncNumber[v] = (u32)-(s32)udiv(eax, ebx);
        Voice0State[v] = 7;
        return;
    }
    {
        COV(1);
        u32 lev = AdsrSustLevLoc[a2 >> 5];
        u32 t, rest, q, blocks;
        AdsrBlocksLeft[v] = (u8)lev;
        rest = ebx - edx;
        q = lev ? edx / lev : edx;
        Voice0Time[v] = q;
        GainDecBendDataTime[v] = q;
        blocks = 64 - (u8)lev;
        AdsrNextTimeDepth[v] = udiv(rest, (u8)blocks);
        Voice0EnvInc[v] = 0;
        ENV2(v) = GainDecBendDataDat[v];
        ebx = Voice0Time[v];
        t = (u32)VCT8((122u << 8) | ENV2(v));
        eax = (u32)-(s32)(t << 16) + ((u32)ENV2(v) << 16);
        GainDecBendDataPos[v] = 0;
        Voice0IncNumber[v] = (u32)-(s32)udiv(eax, ebx);
        Voice0State[v] = 9;
    }
}

/* ---------- VoiceStarter: resume a voice that was parked in state 210 ---------- */
HID void zs_voicestarter_c(int v)
{
    if (v < 0 || v > 7) return;
    Voice0Time[v] = TimeTemp[v];
    Voice0IncNumber[v] = IncNTemp[v];
    Voice0EnvInc[v] = EnvITemp[v];
    Voice0State[v] = StatTemp[v];
    SoundLooped0[v] = 0;
    echoon0[v] = (DSPMem[0x4D] >> v) & 1;
    load_ptrs(v);
    update_pitch(v);
    reset_brr(v);
}

/* ---------- VoiceNStart (key on) ---------- */
static void voice_start(int v)
{
    u32 d;
    Voice0FirstBlock[v] = 1;
    spc700temp[1] = 0;
    if (Voice0Status[v] != 0) {
        spc700temp[0] = Voice0EnvInc[v];
        spc700temp[1] = 1;
    }
    Voice0Status[v] = 0;
    d = *(u32 *)&DSPMem[v * 16];
    if ((d & 0xFF) < 0x40 && ((d >> 8) & 0xFF) < 0x40 && *(u32 *)&DSPMem[v * 16 + 4] == 0x0050FF07u && DSPMem[0x5D] == 6) {
        COV(9);
        DSPMem[v * 16] = 15;
        DSPMem[v * 16 + 1] = 15;
        return;
    }
    if (DSPMem[5 + v * 16] & 0x80) {                    /* ADSR */
        u32 att = DSPMem[5 + v * 16] & 0x0F;
        if (att != 0x0F) { COV(6);
            u32 ebx = AttackRate[att];
            Voice0Time[v] = ebx;
            Voice0IncNumber[v] = udiv(127u << 16, ebx);
            Voice0State[v] = 8;
            Voice0EnvInc[v] = 0;
            GainDecBendDataDat[v] = 0x7F;
            Voice0Status[v] = 1;
        } else {
            u8 a1 = DSPMem[5 + v * 16], a2 = DSPMem[6 + v * 16];
            u32 edx = DecayRate[(a1 >> 4) & 7];
            u32 ebx = SustainRate[a2 & 0x1F];
            if (edx >= ebx) { COV(7);
                u32 eax;
                edx -= ebx;
                eax = (u32)(((u64)edx * (u32)((a2 >> 5) ^ 7)) / 7);
                ebx += eax;
                Voice0EnvInc[v] = 0x7FFFFF;
                ebx >>= 5;
                Voice0Time[v] = ebx;
                GainDecBendDataTime[v] = ebx;
                GainDecBendDataPos[v] = 0;
                GainDecBendDataDat[v] = 127;
                Voice0IncNumber[v] = (u32)-(s32)udiv(9u << 16, ebx);
                Voice0State[v] = 7;
                Voice0Status[v] = 1;
            } else { COV(8);
                u32 lev = AdsrSustLevLoc[a2 >> 5];
                u32 rest = ebx - edx;
                u32 q = udiv(edx, lev);
                AdsrBlocksLeft[v] = (u8)lev;
                Voice0Time[v] = q;
                GainDecBendDataTime[v] = q;
                AdsrNextTimeDepth[v] = udiv(rest, (u8)(64 - (u8)lev));
                Voice0EnvInc[v] = 0x7FFFFF;
                GainDecBendDataPos[v] = 0;
                GainDecBendDataDat[v] = 127;
                Voice0IncNumber[v] = (u32)-(s32)udiv(5u << 16, q);
                Voice0State[v] = 9;
                Voice0Status[v] = 1;
            }
        }
    } else {                                            /* GAIN */
        gain_fresh(v);
        Voice0Status[v] = 1;
    }

    if (spc700temp[1] != 0) {  COV(5);                         /* was playing: park the new setup, fade the old voice out */
        TimeTemp[v] = Voice0Time[v];
        IncNTemp[v] = Voice0IncNumber[v];
        EnvITemp[v] = Voice0EnvInc[v];
        StatTemp[v] = Voice0State[v];
        Voice0EnvInc[v] = spc700temp[0];
        Voice0Time[v] = 127;
        Voice0IncNumber[v] = (u32)-(s32)(spc700temp[0] >> 7);
        Voice0State[v] = 210;
    } else {
        update_pitch(v);
        reset_brr(v);
        SoundLooped0[v] = 0;
        echoon0[v] = (DSPMem[0x4D] >> v) & 1;
    }
    load_ptrs(v);
}

HID void zs_keyon_c(u32 al)
{
    int v;
    al &= 0xFF;
    for (v = 0; v < 8; v++)
        if (al & (1u << v)) voice_start(v);
    if (al) keyonsn = 1;
}

/* ---------- register writes ---------- */
static void voice_adsr(int v, u8 al)
{
    u8 *r5 = &DSPMem[5 + v * 16];
    if ((MuteVoiceF & (1 << v)) || Voice0State[v] == 200 || *r5 == al) { *r5 = al; return; }
    if (al & 0x80) { *r5 = al; switch_sustain(v); return; }
    if (Voice0Status[v] != 1 || *(u16 *)&DSPMem[6 + v * 16] == 0xA0E0) { *r5 = al; return; }
    if (!(*r5 & 0x80)) goto gain2;
    if (Voice0State[v] == 8) goto gain1;
    if (Voice0State[v] >= 2) goto gain2;
gain1:
    COV(2);
    *r5 = al;
    gain_fresh(v);
    return;
gain2:
    COV(3);
    *r5 = al;
    if (Voice0State[v] == 210) {
        COV(4);
        zs_voicestarter_c(v);
        Voice0EnvInc[v] = 0x7FFFFF;
    }
    gain_running(v);
}

static void voice_adsr2(int v, u8 al)
{
    u8 *r6 = &DSPMem[6 + v * 16];
    if ((MuteVoiceF & (1 << v)) || Voice0State[v] == 200 || *r6 == al) { *r6 = al; return; }
    *r6 = al;
    if (DSPMem[5 + v * 16] & 0x80) switch_sustain(v);
}

static void voice_gain(int v, u8 al)
{
    u8 *r7 = &DSPMem[7 + v * 16];
    if ((MuteVoiceF & (1 << v)) || Voice0State[v] == 200 || *r7 == al) { *r7 = al; return; }
    *r7 = al;
    if (Voice0Status[v] != 1) return;
    if (DSPMem[5 + v * 16] & 0x80) return;
    if (Voice0State[v] == 210) { COV(12); zs_voicestarter_c(v); }
    gain_running(v);
}

static u8 vol_conv(u8 val)
{
    u32 idx = ((u32)MusicVol << 8) | VolumeTableb[val];
    return ((u8 *)VolumeConvTable)[idx * 2];
}

static void key_off(u8 al)
{
    int v;
    KeyOnStA &= (u8)~al;
    KeyOnStB &= (u8)~al;
    for (v = 0; v < 8; v++) {
        if (!(al & (1u << v))) continue;
        if (MuteVoiceF & (1 << v)) continue;
        COV(13);
        Voice0Time[v] = 255;
        Voice0IncNumber[v] = (u32)-(s32)(Voice0EnvInc[v] >> 8);
        Voice0State[v] = 200;
        DSPMem[8 + v * 16] = 0;
        DSPMem[0x7C] |= (u8)(1 << v);
    }
    DSPMem[0x5C] = al;
}

static void write_c(u32 reg, u32 val)
{
    u8 al = (u8)val;
    int v = (reg >> 4) & 7;
    reg &= 0xFF;
    if (reg >= 0x80) { DSPMem[reg] = al; return; }
    switch (reg & 0x0F) {
    case 5:
        if (v == 0) TempValueSnd++;
        voice_adsr(v, al);
        return;
    case 6:
        if (v == 0) TempValueSnd++;
        voice_adsr2(v, al);
        return;
    case 7:
        if (v == 0) TempValueSnd++;
        voice_gain(v, al);
        return;
    case 0x0F:
        DSPMem[reg] = al;
        FIRTAPVal0[v] = (s8)al;
        return;
    case 0x0C:
        switch (v) {
        case 0: DSPMem[reg] = al; *(u8 *)&GlobalVL = vol_conv(al); return;
        case 1: DSPMem[reg] = al; *(u8 *)&GlobalVR = vol_conv(al); return;
        case 2: DSPMem[reg] = al; *(u8 *)&EchoVL = vol_conv(al); return;
        case 3: DSPMem[reg] = al; *(u8 *)&EchoVR = vol_conv(al); return;
        case 4: {                                                  /* key on */
            u8 bl = (u8)(~MuteVoiceF & al);
            if (DSPMem[0x5C] == 0xFF) bl = 0;
            KeyOnStA |= bl;
            if (al & 0x80) SoundTest++;
            DSPMem[0x4C] = al;
            DSPMem[0x7C] &= (u8)~al;
            return;
        }
        case 5: key_off(al); return;
        case 6: {                                                  /* FLG */
            DSPMem[0x6C] = al & 0x7F;
            if (al & 0x80) spcres++;
            COV(14);
            if (al & 0xC0) { int i; for (i = 0; i < 8; i++) Voice0Status[i] = 0; }
            NoiseInc = (u32)(((u64)NoiseSpeeds[al & 0x1F] * dspPAdj) >> 17);
            return;
        }
        default: DSPMem[0x7C] = 0; return;                         /* ENDX */
        }
    case 0x0D:
        if (v == 0) { DSPMem[reg] = al; EchoFB = VolumeTableb[al]; return; }
        if (v == 3) {
            int i;
            for (i = 0; i < 8; i++) Voice0Noise[i] = (al >> i) & 1;
            DSPMem[reg] = al;
            return;
        }
        if (v == 7) { DSPMem[reg] = al; MaxEcho = EchoRate[al & 0x0F]; return; }
        DSPMem[reg] = al;
        return;
    default:
        DSPMem[reg] = al;
        return;
    }
}



HID void zs_dspw_c(u32 reg, u32 val) { write_c(reg, val); }
