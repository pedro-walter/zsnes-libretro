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
#include "zl.h"
#define R LG
#define ENTRY
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


enum { C_ENT, C_DEL, C_LINE, C_NMI, C_OVY, C_VIRQ, C_HDMA, C_DRAW, C_IRQ, C_EXIT, C_PEX };
static unsigned long cnt[16];

#define DL ((u8)R.edx)
#define DH ((u8)(R.edx >> 8))
#define SETDH(v) (R.edx = (R.edx & ~0xFF00u) | ((u32)(u8)(v) << 8))

#define CALL(f) zl_mcall(ZID_##f)

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
    SETDH(DH >> 1);
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
                u8 op = *(u8 *)(uintptr_t)R.esi;
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

static u32 sched_c(void)
{
    cnt[C_ENT]++;
    if (HIRQNextExe || SA1Enable || (exiter & 1) || (SfxSFR & 0x20) || FlipWait ||
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


/* tail-style override of the asm label "cpuover": the old cpuover_sh shim, expressed as a jump to the continuation */
extern u8 spcRam[];
static FILE *strace, *strace_txt; static int strace_init;
static void sched_trace(void)
{
    u32 h = 2166136261u;
    if (!strace_init) {
        strace_init = 1;
        if (getenv("ZL_SCHED_TRACE")) strace = fopen(getenv("ZL_SCHED_TRACE"), "wb");
        if (getenv("ZL_SCHED_TRACE_TXT")) strace_txt = fopen(getenv("ZL_SCHED_TRACE_TXT"), "w");
    }
    if (!strace && !strace_txt) return;
#define MIX(v) do { h ^= (u32)(v); h *= 16777619u; } while (0)
    MIX(R.edx); MIX(R.esi - initaddrl); MIX(curypos); MIX(INTEnab); MIX(intrset); MIX(irqon);
    MIX(doirqnext); MIX(curexecstate); MIX(nmirept); MIX(nmistatus); MIX(nmiprevline);
    MIX(hdmadelay); MIX(cycpbl); MIX(KeyOnStA); MIX(KeyOnStB); MIX(numspcvblleft);
    MIX(NMIEnab); MIX(curnmi); MIX(ppustatus); MIX(cfield); MIX(spc700idle);
    MIX(R.ebp - (u32)(uintptr_t)spcRam);
    if (strace) fwrite(&h, 4, 1, strace);
    if (strace_txt) fprintf(strace_txt, "y=%u edx=%08x pc=%x ebp=%x ie=%x ir=%x irqon=%x dirq=%x ces=%x nr=%x ns=%x npl=%x hd=%x cpb=%x nsv=%x\n", curypos, R.edx, R.esi - initaddrl, R.ebp - (u32)(uintptr_t)spcRam,
                                  INTEnab, intrset, irqon, doirqnext, curexecstate, nmirept, nmistatus, nmiprevline, hdmadelay, cycpbl, numspcvblleft);
}

u32 zl_cpuover(void)
{
    u32 code;
    sched_trace();
    code = sched_c();
    if (code == S_ASM) { LG.eax = code; lift_once = 1; return ZID_cpuover; }
    LG.ebx = 0;
    switch (code) {
    case S_EXEC: LG.ecx = 0; LG.ebx = *(u8 *)(uintptr_t)LG.esi; LG.esi++; return ZID_execloop_startagain;
    case S_VIRQ: return ZID_switchtovirq;
    case S_NMI: return ZID_switchtonmi;
    case S_PEXECS: return ZID_pexecs;
    case S_EXIT: return ZID_exitloop;
    case S_EXIT2: return ZID_exitloop2;
    default: return ZID_endprog;
    }
}
