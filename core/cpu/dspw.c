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
#include <link.h>
#include <dlfcn.h>
#include <signal.h>
#include <setjmp.h>

typedef uint8_t u8;
typedef int8_t s8;
typedef uint16_t u16;
typedef int16_t s16;
typedef uint32_t u32;
typedef int32_t s32;
typedef uint64_t u64;

#define HID __attribute__((visibility("hidden")))

extern u8 DSPMem[256];
extern u8 spcRam[0x10000];
extern u16 VolumeConvTable[32768];
extern u8 VolumeTableb[256];
extern u8 Voice0Status[8], echoon0[8], Voice0End[8], Voice0Loop[8], Voice0State[8], Voice0Noise[8];
extern u8 GainDecBendDataPos[8], GainDecBendDataDat[8], AdsrBlocksLeft[8], SoundLooped0[8], Voice0FirstBlock[8];
extern u32 GainDecBendDataTime[8], AdsrNextTimeDepth[8];
extern u32 Voice0Freq[8], Voice0Ptr[8], Voice0LoopPtr[8], Voice0Prev0[8], Voice0Prev1[8];
extern u32 Voice0Time[8], Voice0IncNumber[8], Voice0EnvInc[8];
extern u16 Voice0Pitch[8];
extern u32 BRRPlace0[16];
extern s32 PSampleBuf[24 * 8];
extern s32 FIRTAPVal0[8];
extern u32 GlobalVL, GlobalVR, EchoVL, EchoVR, EchoFB, MaxEcho, NoiseInc, dspPAdj;
extern u32 DecayRate[8], SustainRate[32], Increase[32], Decrease[32], DecreaseRateExp[32], AttackRate[16], EchoRate[16], NoiseSpeeds[32];
extern u8 AdsrSustLevLoc[8];
extern u8 MusicVol, KeyOnStA, KeyOnStB, SoundTest, keyonsn, MuteVoiceF, TempValueSnd, spcres;
extern u32 TimeTemp[8], IncNTemp[8], EnvITemp[8], spc700temp[2];
extern u8 StatTemp[8];
extern void *dspWptr[256];
extern u8 zs_dsp_bss_start[], zs_dsp_bss_end[];
extern void *zs_wstub_tab[256];

HID void *zs_dspw_orig[256];
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

HID __attribute__((force_align_arg_pointer)) void zs_keyon_c(u32 al)
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


/* ---------- differential self-check ---------- */
__asm__(".text\n.type zs_dspw_orig_call,@function\nzs_dspw_orig_call:\n"
        "pushal\n movl 36(%esp),%ebx\n movl 40(%esp),%eax\n cld\n call *%ebx\n popal\n ret\n"
        ".type zs_keyon_asm_call,@function\nzs_keyon_asm_call:\n"
        "pushal\n movl 36(%esp),%eax\n call ProcessKeyOn_asm\n popal\n ret\n");
extern void zs_dspw_orig_call(void *fn, u32 val);
extern void zs_keyon_asm_call(u32 al);

typedef struct { u8 *p; size_t n; } region_t;
static region_t reg[16];
static int nreg;
static u8 *snap0[16], *snapA[16];
static unsigned long dw_calls, dw_bad, dw_skipped;
static int dw_guard, dw_in_asm;
static sigjmp_buf dw_jb;
static void dw_sig(int sig) { if (dw_in_asm) { dw_in_asm = 0; siglongjmp(dw_jb, 1); } signal(sig, SIG_DFL); raise(sig); }

static void dw_setup(void)
{
    int i;
    if (nreg) return;
    reg[nreg++] = (region_t){ zs_dsp_bss_start, (size_t)(zs_dsp_bss_end - zs_dsp_bss_start) };
    reg[nreg++] = (region_t){ DSPMem, 256 };
    reg[nreg++] = (region_t){ (u8 *)&MaxEcho, 4 };
    reg[nreg++] = (region_t){ &KeyOnStA, 1 };
    reg[nreg++] = (region_t){ &KeyOnStB, 1 };
    reg[nreg++] = (region_t){ &SoundTest, 1 };
    reg[nreg++] = (region_t){ &keyonsn, 1 };
    reg[nreg++] = (region_t){ &TempValueSnd, 1 };
    reg[nreg++] = (region_t){ &spcres, 1 };
    reg[nreg++] = (region_t){ &MuteVoiceF, 1 };
    for (i = 0; i < nreg; i++) { snap0[i] = malloc(reg[i].n); snapA[i] = malloc(reg[i].n); }
}

static void dw_report(void)
{
    int k;
    fprintf(stderr, "dsp write diff: %lu ops, %lu mismatching, %lu skipped (asm crash)\n", dw_calls, dw_bad, dw_skipped);
    fprintf(stderr, "dspw coverage:");
    for (k = 0; k < 16; k++) fprintf(stderr, " %d=%lu", k, cov[k]);
    fprintf(stderr, "\n");
}

/* op: reg>=0 -> register write, reg<0 -> key on with mask val */
static void dw_op(int regn, u32 val, int use_c)
{
    if (use_c) { if (regn >= 0) write_c((u32)regn, val); else zs_keyon_c(val); }
    else if (regn >= 0) zs_dspw_orig_call(zs_dspw_orig[regn & 0xFF], val);
    else zs_keyon_asm_call(val);
}

static void dw_diff(int regn, u32 val)
{
    int i;
    size_t k;
    int bad = 0;
    static int inited;
    if (!inited) { dw_setup(); atexit(dw_report); inited = 1; }
    for (i = 0; i < nreg; i++) memcpy(snap0[i], reg[i].p, reg[i].n);
    if (dw_guard) {
        if (sigsetjmp(dw_jb, 1)) {
            for (i = 0; i < nreg; i++) memcpy(reg[i].p, snap0[i], reg[i].n);
            dw_skipped++;
            return;
        }
        dw_in_asm = 1;
        dw_op(regn, val, 0);
        dw_in_asm = 0;
    } else
        dw_op(regn, val, 0);
    for (i = 0; i < nreg; i++) { memcpy(snapA[i], reg[i].p, reg[i].n); memcpy(reg[i].p, snap0[i], reg[i].n); }
    dw_op(regn, val, 1);
    { static int brk = -1; if (brk < 0) brk = getenv("ZS_DSPW_BREAK") != NULL; if (brk) Voice0IncNumber[3]++; }
    dw_calls++;
    for (i = 0; i < nreg && bad < 8; i++)
        for (k = 0; k < reg[i].n && bad < 8; k++)
            if (reg[i].p[k] != snapA[i][k]) {
                Dl_info di;
                const char *nm = "?";
                long off = 0;
                if (dladdr(reg[i].p + k, &di) && di.dli_sname) { nm = di.dli_sname; off = reg[i].p + k - (u8 *)di.dli_saddr; }
                if (dw_bad < 30)
                    fprintf(stderr, "dspw diff: op %lu reg=%d val=%02x region %d +%zu (%s+%ld): C=%02x asm=%02x\n", dw_calls, regn, val & 0xFF, i, k, nm, off, reg[i].p[k], snapA[i][k]);
                bad++;
            }
    if (bad) {
        dw_bad++;
        for (i = 0; i < nreg; i++) memcpy(reg[i].p, snapA[i], reg[i].n);
    }
}

static int dw_mode(void)
{
    static int mode = -1;
    if (mode < 0) mode = getenv("ZS_DSPW_ASM") ? 0 : getenv("ZS_DSPW_DIFF") ? 2 : 1;
    return mode;
}

HID __attribute__((force_align_arg_pointer)) void zs_dspw_c(u32 reg_, u32 val)
{
    int m = dw_mode();
    if (m == 2) { dw_diff((int)(reg_ & 0xFF), val & 0xFF); return; }
    if (m == 0) { zs_dspw_orig_call(zs_dspw_orig[reg_ & 0xFF], val); return; }
    write_c(reg_, val);
}

/* ---------- fuzz ---------- */
static u32 rng_s;
static u32 rnd(void) { rng_s ^= rng_s << 13; rng_s ^= rng_s >> 17; rng_s ^= rng_s << 5; return rng_s; }
static const u8 states[] = { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 200, 210, 4, 7, 8, 9 };

static void scramble(void)
{
    int v;
    for (v = 0; v < 8; v++) {
        int k;
        for (k = 0; k < 16; k++) if (k != 0xC && k != 0xD && k != 0xF) DSPMem[v * 16 + k] = (u8)rnd();
        if (rnd() % 3) DSPMem[v * 16 + 7] = (u8)((rnd() % 2 ? 0x80 : 0) | (rnd() & 0x7F));
        if (rnd() % 3) DSPMem[v * 16 + 5] &= (u8)(rnd() % 4 ? 0xFF : 0x7F);
        Voice0State[v] = states[rnd() % sizeof states];
        Voice0Status[v] = (u8)(rnd() % 3 != 0);
        Voice0EnvInc[v] = rnd() % 4 ? ((rnd() & 0x7F) << 16) | (rnd() & 0xFFFF) : rnd();
        Voice0Time[v] = rnd();
        Voice0IncNumber[v] = rnd();
        Voice0Pitch[v] = (u16)rnd();
        Voice0Freq[v] = rnd();
        TimeTemp[v] = rnd(); IncNTemp[v] = rnd(); EnvITemp[v] = rnd(); StatTemp[v] = (u8)rnd();
        GainDecBendDataTime[v] = rnd(); AdsrNextTimeDepth[v] = rnd();
        GainDecBendDataPos[v] = (u8)rnd(); GainDecBendDataDat[v] = (u8)rnd(); AdsrBlocksLeft[v] = (u8)rnd();
        echoon0[v] = (u8)rnd(); SoundLooped0[v] = (u8)rnd(); Voice0FirstBlock[v] = (u8)rnd();
        Voice0End[v] = (u8)rnd(); Voice0Loop[v] = (u8)rnd();
        Voice0Prev0[v] = rnd(); Voice0Prev1[v] = rnd();
        BRRPlace0[v * 2] = rnd(); BRRPlace0[v * 2 + 1] = rnd();
        Voice0Ptr[v] = rnd(); Voice0LoopPtr[v] = rnd();
        PSampleBuf[v * 24 + 16] = (s32)rnd(); PSampleBuf[v * 24 + 17] = (s32)rnd(); PSampleBuf[v * 24 + 18] = (s32)rnd();
    }
    if (rnd() % 4 == 0) {          /* the volume-hack in VoiceStart: low volumes + magic pattern */
        int vv = rnd() % 8;
        DSPMem[vv * 16] = (u8)(rnd() % 0x40); DSPMem[vv * 16 + 1] = (u8)(rnd() % 0x40);
        *(u32 *)&DSPMem[vv * 16 + 4] = 0x0050FF07u;
        DSPMem[0x5D] = 6;
    } else
        DSPMem[0x5D] = (u8)(rnd() % 0x40 + 2);
    DSPMem[0x4D] = (u8)rnd(); DSPMem[0x7C] = (u8)rnd(); DSPMem[0x5C] = rnd() % 4 ? (u8)rnd() : 0xFF; DSPMem[0x4C] = (u8)rnd();
    KeyOnStA = (u8)rnd(); KeyOnStB = (u8)rnd();
    MuteVoiceF = rnd() % 3 ? 0 : (u8)rnd();
    spc700temp[0] = rnd(); spc700temp[1] = rnd();
    MusicVol = (u8)(rnd() % 2 ? 0x7F : rnd() % 0x80);
}

unsigned zs_dspw_fuzz(unsigned iters, unsigned seed)
{
    unsigned it;
    struct sigaction sa;
    u8 save_mem[256];
    dw_setup();
    {   static int inited; if (!inited) { atexit(dw_report); inited = 1; } }
    rng_s = seed ? seed : 1;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = dw_sig;
    sigaction(SIGFPE, &sa, NULL);
    sigaction(SIGSEGV, &sa, NULL);
    dw_guard = 1;
    memcpy(save_mem, DSPMem, 256);
    for (it = 0; it < iters; it++) {
        u32 c = rnd() % 32, val = rnd() & 0xFF, r;
        int v = rnd() % 8;
        if ((it & 15) == 0) {
            size_t i;
            for (i = 0x200; i < 0x1000; i++) spcRam[i] = (u8)rnd();
        }
        scramble();
        if (c < 4) r = v * 16 + 5;
        else if (c < 8) r = v * 16 + 6;
        else if (c < 14) r = v * 16 + 7;
        else if (c < 16) r = 0x4C;
        else if (c < 18) r = 0x5C;
        else if (c < 19) r = 0x6C;
        else if (c < 20) r = 0x3D;
        else if (c < 21) r = 0x7D;
        else if (c < 22) r = 0x0D;
        else if (c < 23) r = (rnd() % 4) * 16 + 0x0C;
        else if (c < 25) r = v * 16 + 0x0F;
        else if (c < 26) r = 0x7C;
        else if (c < 29) r = rnd() % 0x100;
        else r = 0xFFFFFFFFu;                         /* key on op */
        if (r == 0xFFFFFFFFu) dw_diff(-1, rnd() % 4 ? val : (rnd() % 2 ? 0xFF : (1u << v)));
        else {
            if (r == 0x5C || r == 0x4C) { if (rnd() % 2) val &= rnd(); }
            dw_diff((int)r, val);
        }
    }
    dw_guard = 0;
    memcpy(DSPMem, save_mem, 256);
    dw_report();
    return (unsigned)dw_bad;
}

/* ---------- install: replace the dspWptr[] entries with C stubs ---------- */
HID __attribute__((force_align_arg_pointer)) void zs_dspw_install(void)
{
    int i;
    for (i = 0; i < 256; i++) {
        zs_dspw_orig[i] = dspWptr[i];
        if (dw_mode() != 0) dspWptr[i] = zs_wstub_tab[i];
    }
}
