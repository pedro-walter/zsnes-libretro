#include <stdint.h>
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
#define HIRQNextExe (*(u8 *)0x340343e0u)
#define SA1Enable (*(u8 *)0x341e3b40u)
#define exiter (*(u8 *)0x3403437au)
#define FlipWait (*(u8 *)0x341e2cb3u)
#define NextLineCache (*(u8 *)0x34034471u)
#define KeyOnStA (*(u8 *)0x340489f3u)
#define KeyOnStB (*(u8 *)0x340489f4u)
#define cycpl (*(u8 *)0x340a444du)
#define spcon (*(u8 *)0x340a444fu)
#define SfxSFR (*(u8 *)0x340024b4u)
#define INTEnab (*(u8 *)0x34048014u)
#define intrset (*(u8 *)0x340a444cu)
#define irqon (*(u8 *)0x34048138u)
#define doirqnext (*(u8 *)0x340489eau)
#define virqnodisable (*(u8 *)0x340a4648u)
#define nmiprevline (*(u8 *)0x34034465u)
#define nmirept (*(u8 *)0x34034461u)
#define nmistatus (*(u8 *)0x34034469u)
extern u8 curexecstate, disable65816sh, SpeedHackSafeTable[], hdmadelay, curblank, yesoutofmemory;
#define NextNGDisplay (*(u8 *)0x34034148u)
#define chaton (*(u8 *)0x341e2f0eu)
#define CNetType (*(u8 *)0x3409d851u)
#define GUIQuit (*(u8 *)0x340a0abau)
#define TimerEnable (*(u8 *)0x34096f01u)
#define nextmenupopup (*(u8 *)0x340a34c8u)
#define ExecExitOkay (*(u8 *)0x34034574u)
#define NoInputRead (*(u8 *)0x340a34c9u)
#define ReturnFromSPCStall (*(u8 *)0x340a7001u)
#define cfield (*(u8 *)0x340489efu)
#define ScreenScale (*(u8 *)0x341e2c00u)
#define scanlines (*(u8 *)0x340a8ec0u)
#define ppustatus (*(u8 *)0x340489ffu)
#define NMIEnab (*(u8 *)0x34048015u)
#define curnmi (*(u8 *)0x340a446au)
#define nosprincr (*(u8 *)0x340489fau)
#define MultiTapStat (*(u8 *)0x34048e24u)
#define JoyCRead (*(u8 *)0x34048c28u)
#define snesmouse (*(u8 *)0x340a8ec8u)
#define MovieProcessing (*(u8 *)0x3409d739u)
#define C4Enable (*(u8 *)0x341e3b28u)
#define CheatOn (*(u8 *)0x340a0d57u)
#define pressed ((u8 *)0x340341bau)
#define NetCommand (*(u8 *)0x34034589u)
#define tempdh (*(u8 *)0x3403414du)
#define NetQuit (*(u8 *)0x34033fe4u)
#define NetSwap (*(u8 *)0x3409d852u)
#define smallscreenon (*(u8 *)0x341e26bdu)
#define newengen (*(u8 *)0x341e1555u)
#define ForceNewGfxOff (*(u8 *)0x340a41c4u)
#define CurRecv (*(u8 *)0x34033e88u)
#define curypos (*(u16 *)0x340a4448u)
#define totlines (*(u16 *)0x340a4428u)
#define resolutn (*(u16 *)0x34048081u)
#define scrnon (*(u16 *)0x3404807eu)
#define VIRQLoc (*(u16 *)0x34048016u)
#define IRQHack (*(u16 *)0x340a4651u)
#define oamaddrs (*(u16 *)0x3404802du)
#define scaddset (*(u8 *)0x340481d8u)
#define NetSent (*(u32 *)0x3403439bu)
#define nmiprevaddrl (*(u32 *)0x34034459u)
#define nmiprevaddrh (*(u32 *)0x3403445du)
#define numspcvblleft (*(u32 *)0x340a4649u)
#define lowestspc (*(u32 *)0x34034591u)
#define highestspc (*(u32 *)0x34034595u)
#define spc700idle (*(u32 *)0x340a464du)
#define SPC700write (*(u32 *)0x34048c24u)
#define SPC700read (*(u32 *)0x34048c20u)
#define spc700read (*(u32 *)0x3403458du)
#define HIRQCycNext (*(u32 *)0x340343dcu)
#define oamaddr (*(u32 *)0x34048bf0u)
#define joycontren (*(u32 *)0x3403446du)
#define KeyQuickSnapShot (*(u32 *)0x34096f07u)
#define KeyQuickClock (*(u32 *)0x3409999cu)
#define KeyQuickSaveSPC (*(u32 *)0x340999a0u)
#define KeySaveState (*(u32 *)0x340966fcu)
#define KeyLoadState (*(u32 *)0x34096704u)
#define KeyQuickRst (*(u32 *)0x34096714u)
#define KeyQuickExit (*(u32 *)0x3409670cu)
#define KeyQuickLoad (*(u32 *)0x34096710u)
#define SSKeyPressed (*(u32 *)0x3403413cu)
#define SPCKeyPressed (*(u32 *)0x34034140u)
#define JoyAOrig (*(u32 *)0x340a4bfdu)
#define JoyBOrig (*(u32 *)0x340a4c01u)
#define JoyCOrig (*(u32 *)0x340a4c05u)
#define JoyDOrig (*(u32 *)0x340a4c09u)
#define JoyEOrig (*(u32 *)0x340a4c0du)
#define LethEnData (*(u32 *)0x340a4c39u)
#define JoyANow (*(u32 *)0x340a4c11u)
#define JoyBNow (*(u32 *)0x340a4c15u)
#define JoyCNow (*(u32 *)0x340a4c19u)
#define JoyDNow (*(u32 *)0x340a4c1du)
#define JoyENow (*(u32 *)0x340a4c21u)
#define initaddrl (*(u32 *)0x34034397u)
#define TempVidInfo (*(u32 *)0x34034149u)
#define cycpbl (*(u32 *)0x340a4470u)


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
#define spcRam ((u8 *)0x34037e80u)
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
