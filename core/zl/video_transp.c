/*
 * ProcessTransparencies (video/newgfx16.asm, the MMX variant used when MMXSupport == 1): colour math between the main and the
 * sub screen of every scanline.  A literal transcription of the asm control flow, including its group-of-four-pixels
 * behaviour and the quirks of the fast paths (e.g. the last group of a line is not stored by the full-add fast path),
 * because the output depends on them.  Pixels are 16 bit; the MMX registers are modelled as four 16-bit lanes in a u64.
 */
#include "zl.h"

typedef uint64_t v64;
typedef uint64_t __attribute__((aligned(1), may_alias)) v64u;
typedef uint32_t __attribute__((aligned(1), may_alias)) u32a;
typedef uint16_t u16;

#define REP(x) ((v64)(x) * 0x0001000100010001ULL)
#define UB   REP(0x0020)
#define UBX  REP(0xFFDF)
#define FBA  REP(0xF800)
#define HT   REP(0xF7DE)
#define UB32 0x00200020u
#define SUB  75036

static inline v64 lane(v64 a, int i) { return (a >> (16 * i)) & 0xFFFF; }
static inline v64 psllw(v64 a, int n) { v64 r = 0; int i; for (i = 0; i < 4; i++) r |= ((n > 15) ? 0 : ((lane(a, i) << n) & 0xFFFF)) << (16 * i); return r; }
static inline v64 psrlw(v64 a, int n) { v64 r = 0; int i; for (i = 0; i < 4; i++) r |= ((n > 15) ? 0 : (lane(a, i) >> n)) << (16 * i); return r; }
static inline v64 paddusw(v64 a, v64 b) { v64 r = 0; int i; for (i = 0; i < 4; i++) { v64 s = lane(a, i) + lane(b, i); if (s > 0xFFFF) s = 0xFFFF; r |= s << (16 * i); } return r; }
static inline v64 paddw(v64 a, v64 b) { v64 r = 0; int i; for (i = 0; i < 4; i++) r |= ((lane(a, i) + lane(b, i)) & 0xFFFF) << (16 * i); return r; }
static inline v64 pcmpeqw(v64 a, v64 b) { v64 r = 0; int i; for (i = 0; i < 4; i++) if (lane(a, i) == lane(b, i)) r |= (v64)0xFFFF << (16 * i); return r; }

#define LD(i)  (*(v64u *)(e + (i)))
#define ST(i, v) (*(v64u *)(e + (i)) = (v))
#define D(i)   (*(u32a *)(e + (i)))

extern u8 NGNoTransp, MMXSupport, ngrposng;
extern u8 FillSubScr[], scadtng[], SpecialLine[];
extern u16 resolutn;
extern u32 HiResDone, vidbuffer;

static unsigned long st_lines[4], st_flagged;   /* coverage counters: ZL_STATS=1 prints them at exit */
static void st_print(void) { fprintf(stderr, "transp lines: halfadd=%lu halfsub=%lu fulladd=%lu fullsub=%lu\n", st_lines[0], st_lines[1], st_lines[2], st_lines[3]); }
static void line_transp(u16 *e0, u32 ebx, int S1, int S2, int S3)
{
    u16 *e;
    v64 mm0, mm1, mm2, mm3, mm4, mm5, mm6, mm7;
    u32 ecx, eax, t;
    e = e0;
againtransp:
    e = e0;
    { static int reg; if (!reg && getenv("ZL_STATS")) { reg = 1; atexit(st_print); } st_lines[(scadtng[ebx] & 0x40) ? ((scadtng[ebx] & 0x80) ? 1 : 0) : ((scadtng[ebx] & 0x80) ? 3 : 2)]++; }
    if (!(scadtng[ebx] & 0x40)) goto fulltransp;
    if (scadtng[ebx] & 0x80) goto subtract;
    /* ---- half add ---- */
    ecx = 64; eax = UB32;
faddl2h:
    if (D(0) & eax) goto faddloopbh;
    if (D(2) & eax) goto faddloopbh;
    e += 4; if (--ecx) goto faddl2h;
    goto faddloopdoneh;
prochalfadd:
    if (D(SUB) & eax) goto faddloopbh;
    if (D(SUB + 2) & eax) goto faddloopbh;
    t = D(0) & eax; if (t != eax) goto faddlooph;
    t = D(2) & eax; if (t != eax) goto faddlooph;
prochalfadddo:
    mm0 = LD(0); mm1 = LD(SUB);
    mm0 &= HT; mm1 &= HT; mm0 = psrlw(mm0, 1); mm1 = psrlw(mm1, 1); mm0 = paddw(mm0, mm1);
    ST(0, mm0); e += 4; if (--ecx) goto prochalfadd;
    goto faddloopdoneh;
procfulladdnext:
    ST(-4, mm0);
procfulladd:
    t = D(0) & eax; if (t != eax) goto faddlooph;
    t = D(2) & eax; if (t != eax) goto faddlooph;
    t = D(SUB) & eax; if (t != eax) goto faddlooph;
    t = D(SUB + 2) & eax; if (t != eax) goto faddlooph;
procfulladddo:
    mm0 = LD(0); mm1 = LD(SUB);
    mm0 &= UBX; mm4 = mm1; mm2 = mm0;
    if (S1 > 0) { mm0 = psllw(mm0, S1); mm1 = psllw(mm1, S1); mm3 = mm2; } else mm3 = mm0;
    mm2 = psllw(mm2, S2); mm1 &= FBA; mm0 = paddusw(mm0, mm1); mm0 &= FBA; mm1 = mm4; mm4 = psllw(mm4, S2);
    e += 4;
    if (S1 > 0) mm0 = psrlw(mm0, S1);
    mm2 = paddusw(mm2, mm4); mm3 = psllw(mm3, S3); mm2 &= FBA; mm1 = psllw(mm1, S3); mm2 = psrlw(mm2, S2); mm1 &= FBA;
    mm3 = paddusw(mm3, mm1);
    mm0 |= mm2; mm3 &= FBA; mm3 = psrlw(mm3, S3); mm0 |= mm3;
    if (--ecx) goto procfulladdnext;
    ST(0, mm0);                      /* sic: e has already advanced */
    goto faddloopdoneh;
faddlooph:
    t = D(0); if (!(t & eax)) goto faddl2h;
    t &= eax; if (t != eax) goto faddloopbh;
    t = D(2) & eax; if (t != eax) goto faddloopbh;
    t = D(SUB) & eax; if (t == 0) goto faddla; if (t != eax) goto faddloopbh;
    t = D(SUB + 2) & eax; if (t != eax) goto faddloopbh;
    goto procfulladddo;
faddla:
    if (!(D(SUB + 2) & eax)) goto prochalfadddo;
faddloopbh:
    mm0 = LD(0); mm5 = mm0; mm6 = mm0; mm5 &= UBX; mm1 = LD(SUB); mm7 = mm1; mm2 = mm0; mm1 &= UBX; mm3 = mm0; mm4 = mm1;
    if (S1 > 0) { mm0 = psllw(mm0, S1); mm1 = psllw(mm1, S1); }
    mm1 &= FBA; mm0 = paddusw(mm0, mm1); mm0 &= FBA; mm1 = mm4;
    if (S1 > 0) mm0 = psrlw(mm0, S1);
    mm2 = psllw(mm2, S2); mm1 = psllw(mm1, S2); mm1 &= FBA; mm2 = paddusw(mm2, mm1); mm2 &= FBA; mm1 = mm4;
    mm2 = psrlw(mm2, S2); mm3 = psllw(mm3, S3); mm1 = psllw(mm1, S3); mm3 = paddusw(mm3, mm1); mm3 &= FBA; mm3 = psrlw(mm3, S3);
    mm0 |= mm3; mm0 |= mm2;
    mm6 &= UB; mm6 = pcmpeqw(mm6, UB); mm0 &= mm6; mm1 = mm5; mm1 &= mm6; mm4 &= mm6; mm6 ^= UBX; mm5 &= mm6;
    mm7 &= UB; mm4 &= HT; mm1 &= HT; mm1 = psrlw(mm1, 1); mm4 = psrlw(mm4, 1); mm1 = paddw(mm1, mm4);
    mm7 = pcmpeqw(mm7, UB); mm0 &= mm7; mm7 ^= UBX; mm1 &= mm7; mm0 |= mm1; mm0 |= mm5;
    ST(0, mm0); e += 4;
    if (--ecx) goto faddlooph;
faddloopdoneh:
    goto donetransp;

    /* ---- half subtract ---- */
subtract:
    ecx = 64; eax = UB32;
fsubl2h:
    if (D(0) & eax) goto fsubloopbh;
    if (D(2) & eax) goto fsubloopbh;
    e += 4; if (--ecx) goto fsubl2h;
    goto fsubloopdoneh;
fsublooph:
    if (!(D(0) & eax)) goto fsubl2h;
fsubloopbh:
    mm0 = LD(0); mm5 = mm0; mm6 = mm0; mm0 ^= UBX; mm5 &= UBX; mm1 = LD(SUB); mm7 = mm1; mm2 = mm0; mm1 &= UBX; mm3 = mm0; mm4 = mm1;
    if (S1 > 0) { mm0 = psllw(mm0, S1); mm1 = psllw(mm1, S1); }
    mm1 &= FBA; mm0 = paddusw(mm0, mm1); mm0 &= FBA; mm1 = mm4;
    if (S1 > 0) mm0 = psrlw(mm0, S1);
    mm2 = psllw(mm2, S2); mm1 = psllw(mm1, S2); mm2 = paddusw(mm2, mm1); mm2 &= FBA; mm2 = psrlw(mm2, S2);
    mm3 = psllw(mm3, S3); mm4 = psllw(mm4, S3); mm4 &= FBA; mm3 = paddusw(mm3, mm4); mm3 &= FBA; mm3 = psrlw(mm3, S3);
    mm0 |= mm3; mm0 |= mm2;
    mm6 &= UB; mm0 ^= UBX; mm6 = pcmpeqw(mm6, UB); mm0 &= mm6; mm6 ^= UBX; mm5 &= mm6;
    mm7 &= UB; mm1 = mm0; mm1 &= HT; mm1 = psrlw(mm1, 1); mm7 = pcmpeqw(mm7, UB); mm0 &= mm7; mm7 ^= UBX; mm1 &= mm7; mm0 |= mm1; mm0 |= mm5;
    ST(0, mm0); e += 4;
    if (--ecx) goto fsublooph;
fsubloopdoneh:
    goto donetransp;

    /* ---- full add / subtract ---- */
fulltransp:
    if (scadtng[ebx] & 0x80) goto fullsubtract;
    ecx = 64; eax = UB32;
faddl2:
    if (D(0) & eax) goto faddloopb;
faddl2_2:
    if (D(2) & eax) goto faddloopb;
    e += 4; if (--ecx) goto faddl2;
    goto faddloopdone;
faddloopnext:
    ST(-4, mm0);
faddloop:
    if (!(D(0) & eax)) goto faddl2_2;
faddloopb:
    mm0 = LD(0); mm1 = LD(SUB); mm6 = mm0; mm0 &= UBX; mm4 = mm1; mm2 = mm0;
    if (S1 > 0) { mm0 = psllw(mm0, S1); mm1 = psllw(mm1, S1); mm3 = mm2; mm5 = mm2; } else { mm3 = mm0; mm5 = mm0; }
    mm1 &= FBA; mm0 = paddusw(mm0, mm1); mm0 &= FBA; mm2 = psllw(mm2, S2); mm1 = mm4; mm4 = psllw(mm4, S2);
    mm2 = paddusw(mm2, mm4); mm3 = psllw(mm3, S3); mm2 &= FBA; mm1 = psllw(mm1, S3); mm2 = psrlw(mm2, S2); mm1 &= FBA;
    mm3 = paddusw(mm3, mm1); mm3 &= FBA;
    if (S1 > 0) mm0 = psrlw(mm0, S1);
    mm3 = psrlw(mm3, S3); mm6 &= UB; mm0 |= mm2; mm6 = pcmpeqw(mm6, UB); mm0 |= mm3; mm0 &= mm6; mm6 ^= UBX; mm5 &= mm6;
    e += 4; mm0 |= mm5;
    if (--ecx) goto faddloopnext;
    ST(-4, mm0);
faddloopdone:
    goto donetransp;
fullsubtract:
    ecx = 64; eax = UB32;
fsubl2:
    if (D(0) & eax) goto fsubloopb;
    if (D(2) & eax) goto fsubloopb;
    e += 4; if (--ecx) goto fsubl2;
    goto fsubloopdone;
fsubloop:
    if (!(D(0) & eax)) goto fsubl2;
fsubloopb:
    mm0 = LD(0); mm5 = mm0; mm6 = mm0; mm0 ^= UBX; mm5 &= UBX; mm1 = LD(SUB); mm2 = mm0; mm1 &= UBX; mm3 = mm0; mm4 = mm1;
    if (S1 > 0) { mm0 = psllw(mm0, S1); mm1 = psllw(mm1, S1); }
    mm1 &= FBA; mm0 = paddusw(mm0, mm1); mm0 &= FBA; mm1 = mm4;
    if (S1 > 0) mm0 = psrlw(mm0, S1);
    mm2 = psllw(mm2, S2); mm1 = psllw(mm1, S2); mm2 = paddusw(mm2, mm1); mm2 &= FBA; mm2 = psrlw(mm2, S2);
    mm3 = psllw(mm3, S3); mm4 = psllw(mm4, S3); mm4 &= FBA; mm3 = paddusw(mm3, mm4); mm3 &= FBA; mm3 = psrlw(mm3, S3);
    mm0 |= mm3; mm0 |= mm2; mm6 &= UB; mm0 ^= UBX; mm6 = pcmpeqw(mm6, UB); mm0 &= mm6; mm6 ^= UBX; mm5 &= mm6; mm0 |= mm5;
    ST(0, mm0); e += 4;
    if (--ecx) goto fsubloop;
fsubloopdone:
donetransp:
    if (SpecialLine[ebx] & 3) {
        HiResDone ^= 1;
        if (HiResDone != 0) { e0 += SUB * 2; goto againtransp; }
    }
    return;
}

void zl_ProcessTransparencies(void)
{
    u16 *p;
    u32 ebx;
    int S1, S2, S3;
    if (NGNoTransp != 0) return;
    if (MMXSupport != 1) { zl_orig(ZID_ProcessTransparencies); return; }
    if (ngrposng == 10) { S1 = 1; S2 = 6; S3 = 11; } else { S1 = 0; S2 = 5; S3 = 11; }
    p = (u16 *)(uintptr_t)(vidbuffer + 16 * 2 + 288 * 2);
    ebx = 1;
    do {
        if (FillSubScr[ebx] & 1) {
            HiResDone = 0;
            line_transp(p, ebx, S1, S2, S3);
        }
        ebx++; p += 288;
    } while (resolutn != (u16)ebx);
}
