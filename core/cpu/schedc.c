/* Scanline / IRQ / NMI scheduler (execute.asm "cpuover" + ProcessIRQStuff) in C.
 *
 * The asm shim cpuover_sh (execute.asm) saves the machine registers into zs_R, calls
 * zs_sched_c() and then dispatches on the returned code.  Rare configurations (SA-1,
 * Super FX, netplay, cheats, GUI quit, HIRQ continuation) are delegated to the original
 * asm routine, which stays in the file as "cpuover".
 *
 * ZS_SCHED_ASM=1       always delegate (reference behaviour)
 * ZS_SCHED_TRACE=file  log a hash of the machine/scheduler state at every entry
 * ZS_SCHED_BREAK=n     negative controls
 * ZS_SCHED_COUNT=1     print path counters at exit
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;

#define HID __attribute__((visibility("hidden")))
#define ENTRY __attribute__((force_align_arg_pointer))

typedef struct { u32 eax, ecx, edx, ebx, esi, edi, ebp; } regs_t;
extern regs_t zs_R;
#define R zs_R
extern void zs_mcall(void *fn);

enum { S_EXEC = 0, S_VIRQ, S_NMI, S_PEXECS, S_EXIT, S_EXIT2, S_ENDPROG, S_ASM };

/* ---- asm state ---- */
extern u8 HIRQNextExe, SA1Enable, exiter, FlipWait, NextLineCache, KeyOnStA, KeyOnStB, cycpl, spcon;
extern u8 SfxSFR, INTEnab, intrset, irqon, doirqnext, virqnodisable, nmiprevline, nmirept, nmistatus;
extern u8 curexecstate, disable65816sh, SpeedHackSafeTable[], hdmadelay, curblank, yesoutofmemory;
extern u8 NextNGDisplay, chaton, CNetType, GUIQuit, TimerEnable, nextmenupopup, ExecExitOkay;
extern u8 NoInputRead, ReturnFromSPCStall, cfield, ScreenScale, scanlines, ppustatus, NMIEnab, curnmi;
extern u8 nosprincr, MultiTapStat, JoyCRead, snesmouse, MovieProcessing, C4Enable, CheatOn, pressed[];
extern u8 NetCommand, tempdh, NetQuit, NetSwap;
extern u8 smallscreenon, newengen, ForceNewGfxOff, CurRecv;
extern u16 curypos, totlines, resolutn, scrnon, VIRQLoc, IRQHack, oamaddrs;
extern u8 scaddset;
extern u32 NetSent, nmiprevaddrl, nmiprevaddrh, numspcvblleft, lowestspc, highestspc, spc700idle;
extern u32 SPC700write, SPC700read, spc700read, HIRQCycNext, oamaddr, joycontren;
extern u32 KeyQuickSnapShot, KeyQuickClock, KeyQuickSaveSPC, KeySaveState, KeyLoadState;
extern u32 KeyQuickRst, KeyQuickExit, KeyQuickLoad, SSKeyPressed, SPCKeyPressed;
extern u32 JoyAOrig, JoyBOrig, JoyCOrig, JoyDOrig, JoyEOrig, LethEnData;
extern u32 JoyANow, JoyBNow, JoyCNow, JoyDNow, JoyENow, initaddrl, TempVidInfo;
extern u32 cycpbl;
extern u8 spcRam[0x10000];

extern void exechdma(void), starthdma(void), cachevideo(void), drawline(void), Donextlinecache(void);
extern void ProcessKeyOn(void), updatetimer(void), ReadInputDevice(void), UpdateRewind(void);
extern void idledetectspc(void), outofmemfix(void), StartDrawNewGfx(void), showvideo(void);
extern void ProcessMovies(void), C4VBlank(void);

static int sched_asm, sched_brk, sched_count;
static FILE *trace;
static unsigned long cnt[16];
static const char *cnames[] = {"entries","delegated","lines","nmi","overy","virq","hdma","draw","irqstuff","exit","pexecs","","","","",""};
enum { C_ENT, C_DEL, C_LINE, C_NMI, C_OVY, C_VIRQ, C_HDMA, C_DRAW, C_IRQ, C_EXIT, C_PEX };

static void report(void)
{
    for (int i = 0; i <= C_PEX; i++) fprintf(stderr, "sched %s=%lu\n", cnames[i], cnt[i]);
}

__attribute__((constructor)) static void sched_init(void)
{
    sched_asm = getenv("ZS_SCHED_ASM") != 0;
    sched_brk = getenv("ZS_SCHED_BREAK") ? atoi(getenv("ZS_SCHED_BREAK")) : 0;
    sched_count = getenv("ZS_SCHED_COUNT") != 0;
    if (getenv("ZS_SCHED_TRACE")) trace = fopen(getenv("ZS_SCHED_TRACE"), "wb");
    if (sched_count) atexit(report);
}

#define DL ((u8)R.edx)
#define DH ((u8)(R.edx >> 8))
#define SETDH(v) (R.edx = (R.edx & ~0xFF00u) | ((u32)(u8)(v) << 8))

static void call(void *fn) { zs_mcall(fn); }
#define CALL(f) call((void *)f)

static void trace_state(void)
{
    u32 h = 2166136261u;
#define MIX(v) do { h ^= (u32)(v); h *= 16777619u; } while (0)
    MIX(R.edx); MIX(R.esi - initaddrl); MIX(curypos); MIX(INTEnab); MIX(intrset); MIX(irqon);
    MIX(doirqnext); MIX(curexecstate); MIX(nmirept); MIX(nmistatus); MIX(nmiprevline);
    MIX(hdmadelay); MIX(cycpbl); MIX(KeyOnStA); MIX(KeyOnStB); MIX(numspcvblleft);
    MIX(NMIEnab); MIX(curnmi); MIX(ppustatus); MIX(cfield); MIX(spc700idle);
    MIX(R.ebp - (u32)spcRam);
    fwrite(&h, 4, 1, trace);
    {   static FILE *tt; static int ti;
        if (!ti) { ti = 1; if (getenv("ZS_SCHED_TRACE_TXT")) tt = fopen(getenv("ZS_SCHED_TRACE_TXT"), "w"); }
        if (tt) fprintf(tt, "y=%u edx=%08x pc=%x ebp=%x ie=%x ir=%x irqon=%x dirq=%x ces=%x nr=%x ns=%x npl=%x hd=%x cpb=%x nsv=%x\n", curypos, R.edx, R.esi - initaddrl, R.ebp - (u32)spcRam,
                          INTEnab, intrset, irqon, doirqnext, curexecstate, nmirept, nmistatus, nmiprevline, hdmadelay, cycpbl, numspcvblleft);
    }
}

/* ProcessIRQStuff: returns 1 when execution must go to .virq */
static int irq_stuff(void)
{
    cnt[C_IRQ]++;
    if (virqnodisable != 1 && !(DL & 4) && doirqnext == 1) return 1;
    if (INTEnab & 0x20) {
        u16 ax = (u16)(VIRQLoc + IRQHack);
        if (ax == resolutn) ax--;
        if (ax == 0xFFFF) ax = 0;
        if (curypos != ax) return 0;
        if (INTEnab & 0x10) goto startirq; /* tryhirq */
startirq:
        if (intrset == 1) intrset = 2;
        irqon = 0x80;
        if (DL & 4) { doirqnext = 1; return 0; }
        return 1;
    }
    if (!(INTEnab & 0x10)) return 0;
    for (;;) { /* setagain */
        if (intrset > 2) {
            intrset--;
            if (intrset > 2) return 0;
        }
        if (intrset == 1) { intrset = 2; continue; }
        break;
    }
    if (DL & 4) return 0;
    goto startirq;
}

/* the .virq tail: draw/hdma for the current line then switch to the IRQ vector */
static int do_virq(void)
{
    cnt[C_VIRQ]++;
    curexecstate |= 1;
    doirqnext = 0;
    if (curypos < resolutn) {
        if (hdmadelay == 0) CALL(exechdma); else hdmadelay--;
        if (curypos == 1) CALL(cachevideo);
        if (curblank == 0) CALL(drawline);
    }
    if (intrset == 1) intrset = 2;
    return S_VIRQ;
}

static int key_exit_check(void)
{
    u32 k;
    if (GUIQuit == 1) return S_ENDPROG;
    k = KeyQuickSnapShot;
    if (k && (pressed[k] & 1)) { SSKeyPressed = 1; pressed[k] = 2; return S_EXIT; }
    k = KeyQuickClock;
    if (k && (pressed[k] & 1)) { TimerEnable ^= 1; pressed[k] = 2; }
    k = KeyQuickSaveSPC;
    if (k && (pressed[k] & 1)) { SPCKeyPressed = 1; pressed[k] = 2; return S_EXIT; }
    if (pressed[1] & 1) return S_EXIT;
    if (pressed[59] & 1) return S_EXIT;
    if (nextmenupopup == 1) return S_EXIT;
    if (nextmenupopup >= 2) nextmenupopup--;
    if (pressed[KeySaveState & 0xFFFF] & 1) return S_EXIT;
    if (pressed[KeyLoadState & 0xFFFF] & 1) return S_EXIT;
    if (pressed[KeyQuickRst & 0xFFFF] & 1) return S_EXIT;
    if (pressed[KeyQuickExit & 0xFFFF] & 1) return S_EXIT;
    if (pressed[KeyQuickLoad & 0xFFFF] & 1) return S_EXIT;
    if (ExecExitOkay != 0) ExecExitOkay--;
    return -1;
}

static int nmi_path(void)
{
    cnt[C_NMI]++;
    irqon = 0x80;
    doirqnext = 0;
    NetSent++;
    if (yesoutofmemory == 1) CALL(outofmemfix);
    curypos--;
    tempdh = DH;
    SETDH(0);
    doirqnext = 0;
    CALL(exechdma);
    CALL(exechdma);
    NetCommand = 0;
    NextNGDisplay = 1;
    if (newengen != 0 && curblank == 0 && ForceNewGfxOff == 0) CALL(StartDrawNewGfx);
    if (chaton != 1) {
        int r = key_exit_check();
        if (r >= 0) return r;
    }
    SETDH(tempdh);
    curypos++;
    if (CurRecv != 1 && NoInputRead != 1) CALL(ReadInputDevice);
    CALL(UpdateRewind);
    NetQuit = 0;
    /* .nozerons (non-netplay) */
    if (snesmouse == 4) JoyBNow = LethEnData;
    NetSwap ^= 1;
    if (MovieProcessing != 0) CALL(ProcessMovies);
    if (INTEnab & 1) {
        JoyANow = (JoyAOrig << 16) | (JoyAOrig >> 16);
        JoyBNow = (JoyBOrig << 16) | (JoyBOrig >> 16);
        JoyCNow = (JoyCOrig << 16) | (JoyCOrig >> 16);
        JoyDNow = JoyDOrig;
        JoyENow = JoyEOrig;
        JoyCRead = 0;
    }
    MultiTapStat = 0x80;
    if (C4Enable != 0) CALL(C4VBlank);
    joycontren = 0;
    curexecstate |= 1;
    if (curypos == VIRQLoc) doirqnext = 1;
    oamaddr = oamaddrs;
    nosprincr = 0;
    CALL(showvideo);
    R.ebx = 0;
    NMIEnab = 0x81;
    if (!(INTEnab & 0x80)) {
        if (intrset == 1) intrset = 2;
        return S_EXEC;
    }
    curnmi = 1;
    if (intrset == 1) intrset = 2;
    if (nmistatus == 1) nmirept = 0;
    nmistatus = 0;
    if (nmirept == 0) {
        nmiprevline = (u8)(resolutn - 2);
        nmiprevaddrl = 0xFFFFFFFFu; nmiprevaddrh = 0;
        nmirept = 1; doirqnext = 0;
        return S_NMI;
    }
    if (nmirept != 10) {
        if (R.esi < nmiprevaddrl) nmiprevaddrl = R.esi;
        if (R.esi > nmiprevaddrh) nmiprevaddrh = R.esi;
        nmirept++;
        return S_NMI;
    }
    if (nmiprevaddrh - nmiprevaddrl > 10 || R.esi < nmiprevaddrl || R.esi > nmiprevaddrh) {
        nmirept = 0; nmiprevaddrl = 0xFFFFFFFFu; nmiprevaddrh = 0;
    }
    doirqnext = 0;
    return S_NMI;
}

static int overy_path(void)
{
    cnt[C_OVY]++;
    SETDH(sched_brk == 1 ? DH : (DH >> 1));
    if (smallscreenon != 1 && ScreenScale != 1 && scanlines == 0) cfield ^= 1;
    curypos = 0;
    ppustatus ^= 0x80;
    if (numspcvblleft != 0) {
        if (lowestspc > R.ebp || highestspc < R.ebp) {
            lowestspc = R.ebp - 10;
            highestspc = lowestspc + 20;
            spc700idle = 0;
        }
        if (SPC700write == 0 && spc700read != 0 && SPC700read >= 1500) {
            spc700idle++;
            if (spc700idle == 30) {
                CALL(idledetectspc);
                if (ReturnFromSPCStall == 1) { ExecExitOkay = 0; return S_EXIT; }
            }
        } else
            spc700idle = 0;
        numspcvblleft--;
        SPC700write = 0; SPC700read = 0; spc700read = 0;
    }
    NMIEnab = 1;
    CALL(starthdma);
    if (irq_stuff()) return do_virq();
    return S_EXEC;
}

/* the part of the line loop that draws the previous line (.drawline) */
static int drawline_path(void)
{
    cnt[C_DRAW]++;
    if (!((u8)curypos < nmiprevline) && nmirept >= 10) {
        if (curexecstate == 0) SETDH(0);
        if (nmistatus < 2) {
            if (R.esi < nmiprevaddrl || R.esi > nmiprevaddrh) {
                nmiprevline += 1;
                nmistatus = 1;
            } else {
                if (nmiprevline >= 20) nmiprevline -= 10;
                u8 op = *(u8 *)R.esi;
                if (disable65816sh == 1 || SpeedHackSafeTable[op] == 1) {
                    nmirept = 0; nmiprevaddrl = 0xFFFFFFFFu; nmiprevaddrh = 0;
                } else {
                    nmistatus = 2;
                    curexecstate &= 0xFE;
                }
            }
        }
    }
    if (hdmadelay == 0) {
        if (!(curypos == 1 && (INTEnab & 0x20) && VIRQLoc == 0)) { cnt[C_HDMA]++; CALL(exechdma); }
    } else
        hdmadelay--;
    if (curypos == 1) CALL(cachevideo);
    if (curblank == 0) CALL(drawline);
    if (curexecstate == 2) { cnt[C_PEX]++; return S_PEXECS; }
    if (curexecstate == 0) SETDH(0);
    return S_EXEC;
}

ENTRY HID u32 zs_sched_c(void)
{
    cnt[C_ENT]++;
    if (sched_count && (cnt[C_ENT] & 0xFFFFF) == 0) report();
    if (trace) trace_state();
    if (sched_asm || HIRQNextExe || SA1Enable || (exiter & 1) || (SfxSFR & 0x20) || FlipWait ||
        CNetType == 20 || CheatOn || GUIQuit == 1) {
        cnt[C_DEL]++;
        return S_ASM;
    }
    cnt[C_LINE]++;
    R.esi--;
    if (NextLineCache) CALL(Donextlinecache);
    if (KeyOnStB) { R.eax = KeyOnStB; CALL(ProcessKeyOn); }
    KeyOnStB = KeyOnStA;
    KeyOnStA = 0;
    curypos++;
    SETDH(DH + cycpl);
    if (curypos >= totlines) return overy_path();
    if (spcon) CALL(updatetimer);
    if (curypos == (u16)(resolutn + 1)) return nmi_path();
    if (curypos == resolutn) { cnt[C_HDMA]++; CALL(exechdma); }
    if ((u8)curypos == 100) { TempVidInfo = (u32)scrnon | ((u32)(*(u16 *)&scaddset) << 16); }
    if (irq_stuff()) return do_virq();
    if (curypos < (u16)(resolutn - 1)) return drawline_path();
    return S_EXEC;
}

/* ---- differential fuzz of the IRQ decision (asm macro copy vs C) ---- */
extern int zs_irq_ref(int p);
static u32 rs;
static u32 rnd(void) { rs ^= rs << 13; rs ^= rs >> 17; rs ^= rs << 5; return rs; }
static u16 pick16(void)
{
    switch (rnd() % 6) {
    case 0: return rnd() % 4;
    case 1: return 223 + rnd() % 3;
    case 2: return 0xFFFF - rnd() % 3;
    case 3: return 237 + rnd() % 3;
    default: return rnd() % 262;
    }
}
u32 zs_sched_fuzz(u32 iters, u32 seed)
{
    u32 bad = 0, virq = 0, hit_irq = 0;
    rs = seed ? seed : 1;
    for (u32 it = 0; it < iters; it++) {
        u8 v_virqnodis = rnd() & 1, v_inten = rnd() & 0xB1, v_intrset = rnd() % 5, v_irqon = rnd(), v_doirq = rnd() % 3;
        u16 v_virqloc = pick16(), v_hack = (rnd() & 3) ? 0 : pick16(), v_res = (rnd() & 1) ? 224 : 239, v_cur = pick16();
        if (rnd() & 1) v_cur = (u16)(v_virqloc + v_hack);
        if (rnd() & 3) v_inten |= 0x20 * (rnd() & 1) | 0x10 * (rnd() & 1);
        u8 dl = rnd();
        u8 s_virqnodis = virqnodisable, s_inten = INTEnab, s_intrset = intrset, s_irqon = irqon, s_doirq = doirqnext;
        u16 s_virqloc = VIRQLoc, s_hack = IRQHack, s_res = resolutn, s_cur = curypos;
        u32 sdx = R.edx;
        virqnodisable = v_virqnodis; INTEnab = v_inten; intrset = v_intrset; irqon = v_irqon; doirqnext = v_doirq;
        VIRQLoc = v_virqloc; IRQHack = v_hack; resolutn = v_res; curypos = v_cur;
        int ra = zs_irq_ref(dl);
        u8 a_intrset = intrset, a_irqon = irqon, a_doirq = doirqnext;
        intrset = v_intrset; irqon = v_irqon; doirqnext = v_doirq;
        R.edx = dl;
        int rc = irq_stuff();
        if (sched_brk == 4) rc = !rc;
        virq += ra; hit_irq += (irqon != v_irqon);
        if (ra != rc || a_intrset != intrset || a_irqon != irqon || a_doirq != doirqnext) {
            if (bad < 5) fprintf(stderr, "irq mismatch: inten=%02x nodis=%d dl=%02x intrset=%d doirq=%d virq=%d hack=%d res=%d cur=%d asm=%d/%d/%d/%d c=%d/%d/%d/%d\n",
                v_inten, v_virqnodis, dl, v_intrset, v_doirq, v_virqloc, v_hack, v_res, v_cur, ra, a_intrset, a_irqon, a_doirq, rc, intrset, irqon, doirqnext);
            bad++;
        }
        virqnodisable = s_virqnodis; INTEnab = s_inten; intrset = s_intrset; irqon = s_irqon; doirqnext = s_doirq;
        VIRQLoc = s_virqloc; IRQHack = s_hack; resolutn = s_res; curypos = s_cur; R.edx = sdx;
    }
    fprintf(stderr, "sched fuzz: %u iterations, %u virq, %u irqon-set\n", iters, virq, hit_irq);
    return bad;
}
