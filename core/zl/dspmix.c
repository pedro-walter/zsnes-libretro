#define _GNU_SOURCE
/*
 * S-DSP sample generator, ported from cpu/dspproc.asm "ProcessSoundBuffer"
 * (ZSNES 1.42): BRR decode, envelope state machine, Gaussian interpolation,
 * per-voice stereo mix, echo/FIR and the final buffer. A faithful translation,
 * quirks included (16-bit imul/shr volume trick, the envelope "Time" counters,
 * the read-ahead decode used by the interpolators, ...).
 *
 * Scope: the configuration libretro runs with -- stereo, Gaussian (MMX)
 * interpolation, no low-pass/surround/reverse-stereo, and no noise / pitch
 * modulation. For anything else zs_dsp_mix() returns 0 and the original asm
 * (ProcessSoundBuffer_asm) runs, so behaviour never changes. Register writes,
 * key-on and the voice (re)start code are still asm and share the same
 * variables.
 *
 * ZS_DSP_DIFF=1 runs both implementations on identical state for every call
 * and reports any byte of the DSP state, output or SPC sample buffer that
 * differs (asm result is kept, so the game continues on the reference path).
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

/* ---- asm state ---- */
extern u8 DSPMem[256];
extern u8 spcRam[0x10000];
extern u32 spcBuffera;                    /* guest pointer slot */
#define SPCBUF ((u8 *)(uintptr_t)spcBuffera)
extern u16 VolumeConvTable[32768];
extern u8 Voice0Disable[8], Voice0Status[8], echoon0[8], Voice0End[8], Voice0Loop[8], Voice0State[8];
extern u8 Voice0Volume[8], Voice0VolumeR[8], Voice0VolumeL[8], Voice0Volumee[8], Voice0VolumeRe[8], Voice0VolumeLe[8];
extern u8 GainDecBendDataPos[8], GainDecBendDataDat[8], AdsrBlocksLeft[8], SoundLooped0[8];
extern u32 GainDecBendDataTime[8], AdsrNextTimeDepth[8];
extern u32 Voice0Freq[8], Voice0Ptr[8], Voice0LoopPtr[8], Voice0BufPtr[8], Voice0Prev0[8], Voice0Prev1[8];
extern u32 Voice0Time[8], Voice0IncNumber[8], Voice0EnvInc[8];
extern u16 Voice0Pitch[8];
extern u32 BRRPlace0[16];
extern s32 PSampleBuf[24 * 8];
extern u32 DLPFsamples[8 * 24];
extern u16 BRRreadahead[4];
extern s16 DSPInterP[1024];
extern s32 DSPBuffer[], EchoBuffer[], echobuf[];
extern s32 FiltLoop[16], FiltLoopR[16];
extern s32 FIRTAPVal0[8];
extern u32 GlobalVL, GlobalVR, EchoVL, EchoVR, EchoT, EchoFB, MaxEcho, CEchoPtr, CurFiltPtr;
extern u32 BufferSizeB, dspPAdj, curvoice;
extern u8 SoundInterpType, MMXSupport, StereoSound, LowPassFilterType, cfgecho, Surround, RevStereo, ENVDisable;
extern u8 UniqueSoundv, echowrittento;
extern u32 prev0, prev1, filter0, filter1, powhack, paramhack[4];
extern u8 bshift, lastbl, loopbl, sampleleft;
extern u32 DecayRate[8], SustainRate[32], Increase[32];
extern u8 SustainValue[8], AdsrSustLevLoc[8], AdsrBendData[], GainDecBendData[], VolumeTableD[];

static unsigned long cov[48];
#define COV(n) (cov[n]++)
static const s32 Filter[8] = { 0, 0, 240, 0, 488, -240, 460, -208 };

#define VCT8(idx) (((u8 *)VolumeConvTable)[(idx) * 2])

static inline u32 udiv(u32 a, u32 b) { return b ? a / b : 0; }

/* ---------- BRR ---------- */
static s16 brr_sample(u8 byte, int hi)
{
    s32 eax = hi ? (byte >> 4) : (byte & 0x0F);
    s32 edx, p0 = (s32)prev0, p1 = (s32)prev1;
    eax = (eax ^ 8) - 8;
    if (bshift > 12) { COV(20); eax &= ~0x7FF; }
    else eax = (s32)((u32)eax << bshift) >> 1;
    edx = eax;
    if (filter0 == 240) { COV(17);
        edx += p0 >> 1;
        edx += (-p0) >> 5;
    }
    if (filter0 == 488) { COV(18);
        edx += p0;
        edx += (-((p0 >> 1) + p0)) >> 5;
        edx -= p1 >> 1;
        edx += p1 >> 5;
    }
    if (filter0 == 460) { COV(19);
        edx += p0;
        edx += (-(((p0 * 2 + p0) << 2) + p0)) >> 7;
        edx -= p1 >> 1;
        edx += ((p1 >> 1) + p1) >> 4;
    }
    if (edx < -32768) { edx = -32768; COV(21); }
    if (edx > 32767) { edx = 32767; COV(22); }
    prev1 = prev0;
    {
        s16 r = (s16)((u32)edx << 1);
        prev0 = (u32)(s32)r;
        return r;
    }
}

static void brr_header(u8 h)
{
    int i = (h & 0x0C) >> 1;
    filter0 = (u32)Filter[i];
    filter1 = (u32)Filter[i + 1];
    bshift = h >> 4;
}

/* decode the 9-byte block at src into dst[16]; also the 4-sample read-ahead */
static void brr_decode(const u8 *src, s16 *dst, int voice)
{
    const u8 *esi;
    int i;
    curvoice = voice;
    lastbl = 0;
    loopbl = 0;
    if (src[0] & 1) {
        lastbl = 1;
        if (src[0] & 2) loopbl = 1;
    }
    brr_header(src[0]);
    esi = src + 1;
    sampleleft = 8;
    for (i = 0; i < 8; i++) {
        dst[i * 2] = brr_sample(*esi, 1);
        dst[i * 2 + 1] = brr_sample(*esi, 0);
        esi++;
    }
    sampleleft = 0;

    /* SoundInterpType >= 1: BRR_decode_ahead */
    if (lastbl != 1) goto fill;
    if (loopbl != 1) { COV(15);
        BRRreadahead[0] = BRRreadahead[1] = BRRreadahead[2] = BRRreadahead[3] = 0;
        return;
    }
    esi = spcRam + Voice0LoopPtr[voice]; COV(16);
fill: {
        u32 sp0 = prev0, sp1 = prev1;
        brr_header(esi[0]);
        esi++;
        BRRreadahead[0] = (u16)brr_sample(*esi, 1);
        BRRreadahead[1] = (u16)brr_sample(*esi, 0);
        esi++;
        BRRreadahead[2] = (u16)brr_sample(*esi, 1);
        BRRreadahead[3] = (u16)brr_sample(*esi, 0);
        prev1 = sp1;
        prev0 = sp0;
    }
}

/* ---------- Gaussian interpolator (DSPInterpolate_4_mmx) ---------- */
static inline s32 gauss(int v)
{
    u32 place = BRRPlace0[v * 2];
    int idx = (place >> 24) & 0xFF;
    int frac = (place >> 16) & 0xFF;
    const s32 *s = &PSampleBuf[v * 24 + idx + 2];
    const s16 *c = &DSPInterP[frac * 4];
    u32 d0 = (u32)((s32)(s16)s[0] * c[0] + (s32)(s16)s[1] * c[1]);
    u32 d1 = (u32)((s32)(s16)s[2] * c[2] + (s32)(s16)s[3] * c[3]);
    s32 r = (s32)(d0 + d1) >> 11;
    if (r < -32768) r = -32768;
    if (r > 32767) r = 32767;
    return r;
}

/* imul cx; shr ax,7; add dl,dl; or ah,dl  ==  (s16)((s16)sample * (s16)cx >> 7) */
static inline s32 vmul(s32 sample, u16 cx)
{
    return (s16)(((s32)(s16)sample * (s32)(s16)cx) >> 7);
}

/* ---------- envelope ---------- */
enum { ENV_MIX = 0, ENV_VOICE_DONE = 1, ENV_RESTART = 2 };

static void env_mute(int v)
{
    Voice0EnvInc[v] = 0;
    Voice0IncNumber[v] = 0;
    Voice0Time[v] = 0xFFFFFFFFu;
}

static inline void set_env_byte2(int v, u8 b)
{
    ((u8 *)&Voice0EnvInc[v])[2] = b;
}

static int env_next(int v)
{
    u8 bit = 1 << v;
    switch (Voice0State[v]) {
    case 10: { /* ADSR sustain */ COV(0);
        u32 ebx = GainDecBendDataPos[v];
        u8 dl = GainDecBendDataDat[v];
        u8 dh = AdsrBendData[ebx];
        u8 al;
        Voice0EnvInc[v] = 0;
        al = VCT8((dh << 8) | dl);
        set_env_byte2(v, al);
        dh = AdsrBendData[ebx + 1];
        if (dh == 255) {
            Voice0State[v] = 5;
            env_mute(v);
            return ENV_MIX;
        }
        dl = VCT8((dh << 8) | dl);
        ebx = AdsrNextTimeDepth[v];
        al = (u8)(al - dl);
        Voice0Time[v] = ebx;
        GainDecBendDataPos[v]++;
        Voice0IncNumber[v] = (u32)(-(s32)udiv((u32)al << 16, ebx));
        return ENV_MIX;
    }
    case 9: { /* ADSR decay proc */ COV(1);
        u32 ebx = GainDecBendDataPos[v];
        u8 dl = GainDecBendDataDat[v];
        u8 dh = AdsrBendData[ebx];
        u8 al;
        Voice0EnvInc[v] = 0;
        al = VCT8((dh << 8) | dl);
        set_env_byte2(v, al);
        dh = AdsrBendData[ebx + 1];
        al = (u8)(al - VCT8((dh << 8) | dl));
        ebx = GainDecBendDataTime[v];
        Voice0Time[v] = ebx;
        GainDecBendDataPos[v]++;
        Voice0IncNumber[v] = (u32)(-(s32)udiv((u32)al << 16, ebx));
        if (--AdsrBlocksLeft[v] == 0) Voice0State[v] = 10;
        return ENV_MIX;
    }
    case 7: { /* decrease bent */ COV(2);
        u32 ebx = GainDecBendDataPos[v];
        u8 dl = GainDecBendDataDat[v];
        u8 dh = GainDecBendData[ebx];
        u8 al;
        Voice0EnvInc[v] = 0;
        al = VCT8((dh << 8) | dl);
        set_env_byte2(v, al);
        dh = GainDecBendData[ebx + 1];
        if (dh == 255) {
            Voice0State[v] = 5;
            env_mute(v);
            return ENV_MIX;
        }
        ebx = GainDecBendDataTime[v];
        al = (u8)(al - VCT8((dh << 8) | dl));
        Voice0Time[v] = ebx;
        GainDecBendDataPos[v]++;
        Voice0IncNumber[v] = (u32)(-(s32)udiv((u32)al << 16, ebx));
        return ENV_MIX;
    }
    case 8: { /* ADSR decay */
        u32 edx = DecayRate[(DSPMem[0x05 + v * 16] >> 4) & 7];
        u32 ebx = SustainRate[DSPMem[0x06 + v * 16] & 0x1F];
        if (edx < ebx) { COV(3);
            u32 lvl = AdsrSustLevLoc[DSPMem[0x06 + v * 16] >> 5];
            u32 t;
            AdsrBlocksLeft[v] = (u8)lvl;
            ebx -= edx;
            t = udiv(edx, lvl);
            Voice0Time[v] = t;
            GainDecBendDataTime[v] = t;
            AdsrNextTimeDepth[v] = udiv(ebx, 64 - (lvl & 0xFF));
            Voice0EnvInc[v] = 0x007FFFFFu;
            GainDecBendDataPos[v] = 0;
            GainDecBendDataDat[v] = 127;
            Voice0IncNumber[v] = (u32)(-(s32)udiv(127 * 65536u - 122 * 65536u, Voice0Time[v]));
            Voice0State[v] = 9;
        } else { COV(4);
            u32 d = edx - ebx;
            u32 m = DSPMem[0x06 + v * 16] >> 5;
            u64 prod;
            m ^= 7;
            prod = (u64)d * m;
            d = (u32)(prod / 7);
            ebx += d;
            Voice0EnvInc[v] = 0x007FFFFFu;
            ebx >>= 5;
            Voice0Time[v] = ebx;
            GainDecBendDataTime[v] = ebx;
            GainDecBendDataPos[v] = 0;
            GainDecBendDataDat[v] = 127;
            Voice0IncNumber[v] = (u32)(-(s32)udiv(127 * 65536u - 118 * 65536u, ebx));
            Voice0State[v] = 7;
        }
        return ENV_MIX;
    }
    case 1: { /* decay */ COV(5);
        u32 eax = (DSPMem[0x05 + v * 16] >> 4) & 7;
        u32 edx = DSPMem[0x06 + v * 16] & 0x1F;
        u32 ebx = DecayRate[eax];
        u32 sv;
        Voice0EnvInc[v] = 0x007FFFFFu;
        if (edx != 0x1F && ebx > SustainRate[edx]) {
            if (eax == 0 && (DSPMem[0x06 + v * 16] & 0xE0) == 0xE0) {
                /* Sustain2 */ COV(6);
                ebx = SustainRate[edx];
                if ((s32)ebx <= 100) ebx = 100;
                Voice0Time[v] = ebx;
                Voice0IncNumber[v] = (u32)(-(s32)udiv((u32)((Voice0EnvInc[v] >> 16) & 0xFF) << 16, ebx));
                Voice0State[v] = 4;
                return ENV_MIX;
            }
            ebx = DecayRate[(DSPMem[0x05 + v * 16] >> 4) & 7] - SustainRate[edx];
            if (!(ebx >= SustainRate[edx])) ebx = SustainRate[edx];
        }
        if (ebx == 0) ebx++;
        sv = (DSPMem[0x06 + v * 16] >> 5) & 7;
        Voice0Time[v] = ebx;
        eax = (u8)(SustainValue[sv] ^ 0x7F);
        Voice0IncNumber[v] = (u32)(-(s32)udiv(eax << 16, ebx));
        Voice0State[v] = 2;
        return ENV_MIX;
    }
    case 2: { /* sustain */ COV(7);
        u32 eax = (DSPMem[0x05 + v * 16] >> 4) & 7;
        u32 edx = DSPMem[0x06 + v * 16] & 0x1F;
        u32 ebx = SustainRate[edx];
        if (!(ebx & 0x80000000u)) ebx -= DecayRate[eax];
        if ((s32)ebx <= 100) ebx = 100;
        Voice0Time[v] = ebx;
        Voice0IncNumber[v] = (u32)(-(s32)udiv((u32)((Voice0EnvInc[v] >> 16) & 0xFF) << 16, ebx));
        Voice0State[v] = 4;
        return ENV_MIX;
    }
    case 3: /* blank */ COV(8);
        Voice0EnvInc[v] = 0x007F0000u;
        Voice0IncNumber[v] = 0;
        Voice0Time[v] = 0xFFFFFFFFu;
        return ENV_MIX;
    case 5: /* mute gain */ COV(9);
        env_mute(v);
        return ENV_MIX;
    case 6: /* increase bent */ COV(10);
        Voice0Time[v] = Increase[DSPMem[0x07 + v * 16] & 0x1F];
        Voice0IncNumber[v] >>= 2;
        Voice0State[v] = 3;
        return ENV_MIX;
    case 210: COV(11);
        Voice0EnvInc[v] = 0;
        Voice0IncNumber[v] = 0;
        Voice0State[v] = 0;
        DSPMem[0x08 + v * 16] = 0;
        DSPMem[0x09 + v * 16] = 0;
        DSPMem[0x7C] |= bit;
        zs_voicestarter_c(v);
        return ENV_RESTART;
    default: /* 4, 200 and anything unknown */ COV(12);
        memset(&DLPFsamples[v * 24 + 16], 0, 16);
        Voice0EnvInc[v] = 0;
        Voice0IncNumber[v] = 0;
        Voice0Status[v] = 0;
        Voice0State[v] = 0;
        DSPMem[0x08 + v * 16] = 0;
        DSPMem[0x09 + v * 16] = 0;
        DSPMem[0x7C] |= bit;
        return ENV_VOICE_DONE;
    }
}

/* ---------- voice ---------- */
static void calc_volumes(int v)
{
    u8 al, bl, ah, bh;

    al = VolumeTableD[DSPMem[0x00 + v * 16]];
    bl = VolumeTableD[DSPMem[0x01 + v * 16]];
    Voice0VolumeRe[v] = al;
    Voice0VolumeLe[v] = bl;
    ah = al;
    bh = bl;
    if (ah & 0x80) ah = -ah;
    if (bh & 0x80) bh = -bh;
    ah = (u8)(ah + bh) >> 1;
    if ((al & 0x80) || (bl & 0x80)) ah = -ah;
    Voice0Volumee[v] = ah;

    al = VolumeTableD[DSPMem[0x00 + v * 16]];
    bl = VolumeTableD[DSPMem[0x01 + v * 16]];
    al = ((u8 *)VolumeConvTable)[((((u32)GlobalVL & 0xFF) << 8) | al) * 2];
    bl = ((u8 *)VolumeConvTable)[((((u32)GlobalVR & 0xFF) << 8) | bl) * 2];
    Voice0VolumeR[v] = al;
    Voice0VolumeL[v] = bl;
    ah = al;
    bh = bl;
    if (ah & 0x80) ah = -ah;
    if (bh & 0x80) bh = -bh;
    ah = (u8)(ah + bh) >> 1;
    if ((al & 0x80) || (bl & 0x80)) ah = -ah;
    Voice0Volume[v] = ah;
}

static void voice_run(int v)
{
    u32 esi;
    u8 bit = 1 << v;
    int echo;
    u16 pitch;

    if (Voice0Disable[v] != 1 || Voice0Status[v] != 1) return;
    powhack = bit;
    echo = (echoon0[v] == 1);

    COV(29);
    pitch = *(u16 *)&DSPMem[2 + v * 16];
    if (Voice0Pitch[v] != pitch) {
        Voice0Pitch[v] = pitch;
        Voice0Freq[v] = (u32)(((u64)(pitch & 0x3FFF) * dspPAdj) >> 8);
    }
    esi = 0;

restart:
    calc_volumes(v);
    lastbl = 0;
    loopbl = 0;
    UniqueSoundv = 0;

    for (;;) {
        if (BRRPlace0[v * 2] >= 0x10000000u) {
            /* ---- ProcessBRR ---- */
            if (Voice0End[v] == 1) {
                if (Voice0Loop[v] != 1) { COV(14);
                    DSPMem[0x7C] |= bit;
                    DSPMem[0x08 + v * 16] = 0;
                    memset(&DLPFsamples[v * 24 + 16], 0, 16);
                    Voice0EnvInc[v] = 0;
                    Voice0IncNumber[v] = 0;
                    Voice0Status[v] = 0;
                    return;
                }
                SoundLooped0[v] = 1; COV(13);
                DSPMem[0x7C] |= bit;
                Voice0Ptr[v] = Voice0LoopPtr[v];
            }
            {
                u32 ptr = Voice0Ptr[v];
                s16 *dst = (s16 *)(SPCBUF + (u32)((ptr + 1) << 2));
                s32 *ps = &PSampleBuf[v * 24];
                int i;
                BRRPlace0[v * 2] -= 0x10000000u;
                ps[0] = ps[16];
                ps[1] = ps[17];
                ps[2] = ps[18];
                Voice0BufPtr[v] = (u32)(uintptr_t)dst;
                prev0 = Voice0Prev0[v];
                prev1 = Voice0Prev1[v];
                brr_decode(spcRam + ptr, dst, v);
                for (i = 0; i < 16; i++) ps[3 + i] = dst[i];
                for (i = 0; i < 4; i++) ps[19 + i] = (s16)BRRreadahead[i];
                Voice0Prev0[v] = prev0;
                Voice0Prev1[v] = prev1;
                Voice0Loop[v] = loopbl;
                Voice0End[v] = lastbl;
                Voice0Ptr[v] += 9;
            }
            continue;
        }
        Voice0EnvInc[v] += Voice0IncNumber[v];
        if (--Voice0Time[v] == 0) {
            int r = env_next(v);
            if (r == ENV_VOICE_DONE) return;
            if (r == ENV_RESTART) { COV(30); goto restart; }
        }

        /* ---- EchoStereoInterpolated / NonEchoStereoInterpolated ---- */
        {
            s32 sample = (s16)gauss(v);
            u8 envb = (Voice0EnvInc[v] >> 16) & 0xFF;
            u16 cx;
            cx = VolumeConvTable[(envb << 8) | Voice0VolumeR[v]];
            DSPBuffer[esi] += vmul(sample, cx);
            if (echo) { COV(23);
                cx = VolumeConvTable[(envb << 8) | Voice0VolumeRe[v]];
                EchoBuffer[esi] += vmul(sample, cx);
            }
            cx = VolumeConvTable[(envb << 8) | Voice0VolumeL[v]];
            DSPBuffer[esi + 1] += vmul(sample, cx);
            if (echo) {
                cx = VolumeConvTable[(envb << 8) | Voice0VolumeLe[v]];
                EchoBuffer[esi + 1] += vmul(sample, cx);
            }
            esi += 2;
            BRRPlace0[v * 2] += Voice0Freq[v];
        }
        if (esi == BufferSizeB) break;
    }
    DSPMem[0x08 + v * 16] = (Voice0EnvInc[v] >> 16) & 0xFF;
    if (ENVDisable == 1) DSPMem[0x08 + v * 16] = 0;
}

/* ---------- echo (stereo) ---------- */
static inline s32 imul32(s32 a, s32 b) { return (s32)((u32)a * (u32)b); }
static inline s32 mulbyte_sar7(s32 a, u32 b) { return (s32)((u32)a * b) >> 7; }

static void mix_echo_fir(void)
{
    u32 esi = CEchoPtr, edi = 0;
    s32 ebx, ecx;
    int k;
    u32 edx;
    *(u8 *)&EchoT = ((EchoVR & 0xFF) > (EchoVL & 0xFF)) ? (EchoVR & 0xFF) : (EchoVL & 0xFF);
    do {
        ebx = echobuf[esi];
        edx = CurFiltPtr;
        FiltLoop[edx] = ebx;
        ecx = imul32(ebx, FIRTAPVal0[0]) >> 7;
        for (k = 1; k < 8; k++) {
            edx = (edx - 2) & 0x0F;
            ecx += imul32(FIRTAPVal0[k], FiltLoop[edx]) >> 7;
        }
        DSPBuffer[edi] += ecx;
        ecx = imul32((s32)EchoFB, ecx) >> 7;
        echobuf[esi] = mulbyte_sar7(EchoBuffer[edi], EchoVL & 0xFF) + ecx;
        esi++;
        edi++;

        ebx = echobuf[esi];
        edx = CurFiltPtr;
        FiltLoopR[edx] = ebx;
        ecx = imul32(ebx, FIRTAPVal0[0]) >> 7;
        for (k = 1; k < 8; k++) {
            edx = (edx - 2) & 0x0F;
            ecx += imul32(FIRTAPVal0[k], FiltLoopR[edx]) >> 7;
        }
        DSPBuffer[edi] += ecx;
        CurFiltPtr = (CurFiltPtr & ~0xFFu) | ((CurFiltPtr + 1) & 0x0F);
        ecx = imul32((s32)EchoFB, ecx) >> 7;
        echobuf[esi] = mulbyte_sar7(EchoBuffer[edi], EchoVR & 0xFF) + ecx;
        esi++;
        if (!(esi < MaxEcho * 2)) { esi = 0; COV(27); }
        edi++;
    } while (edi != BufferSizeB);
    CEchoPtr = esi;
}

static void mix_echo_plain(void)
{
    u32 esi = CEchoPtr, edi = 0;
    s32 ecx;
    *(u8 *)&EchoT = ((EchoVR & 0xFF) > (EchoVL & 0xFF)) ? (EchoVR & 0xFF) : (EchoVL & 0xFF);
    do {
        ecx = echobuf[esi];
        DSPBuffer[edi] += ecx;
        ecx = imul32((s32)EchoFB, ecx) >> 7;
        echobuf[esi] = mulbyte_sar7(EchoBuffer[edi], EchoVL & 0xFF) + ecx;
        esi++;
        edi++;

        ecx = echobuf[esi];
        DSPBuffer[edi] += ecx;
        ecx = imul32((s32)EchoFB, ecx) >> 7;
        echobuf[esi] = mulbyte_sar7(EchoBuffer[edi], EchoVR & 0xFF) + ecx;
        esi++;
        if (!(esi < MaxEcho * 2)) esi = 0;
        edi++;
    } while (edi != BufferSizeB);
    CEchoPtr = esi;
}

static int supported(void)
{
    return SoundInterpType == 1 && MMXSupport && StereoSound == 1 && LowPassFilterType == 0 &&
           Surround == 0 && RevStereo == 0 && DSPMem[0x2D] == 0 && DSPMem[0x3D] == 0 &&
           (BufferSizeB & 1) == 0 && BufferSizeB <= 1280;
}

static void mix_c(void)
{
    int v;
    memset(DSPBuffer, 0, BufferSizeB * 4);
    if (cfgecho != 1 && !(DSPMem[0x6C] & 0x20)) memset(EchoBuffer, 0, BufferSizeB * 4);

    for (v = 0; v < 8; v++) voice_run(v);

    if (cfgecho == 1) return;
    if (!(DSPMem[0x6C] & 0x20)) {
        echowrittento = 1;
        if (FIRTAPVal0[0] == 0x7F && !FIRTAPVal0[1] && !FIRTAPVal0[2] && !FIRTAPVal0[3] &&
            !FIRTAPVal0[4] && !FIRTAPVal0[5] && !FIRTAPVal0[6] && !FIRTAPVal0[7])
            { COV(24); mix_echo_plain(); }
        else
            { COV(25); mix_echo_fir(); }
    } else if (echowrittento) { COV(26);
        memset(echobuf, 0, MaxEcho * 2 * 4);
        echowrittento = 0;
    }
}


/* override of asm ProcessSoundBuffer: C mixer for the common configuration, original lifted code otherwise */
void zl_ProcessSoundBuffer(void)
{
    if (supported()) { mix_c(); return; }
    zl_orig(ZID_ProcessSoundBuffer);
}
