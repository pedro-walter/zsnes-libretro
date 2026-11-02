/*
 * Platform layer for the ZSNES libretro core.  Replaces linux/sdllink.c,
 * sw_draw.c, zloaderw.c: no SDL, no windows, no threads.
 *
 * ZSNES's main loop (zstart -> execloop) never returns, so the whole emulator
 * runs as a coroutine on its own stack.  Every time the original code would
 * wait for the 60Hz timer (Check60hz -> sem_sleep) we switch back to the
 * frontend, which gives us exactly one emulated frame per retro_run().
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <ucontext.h>
#include <zlib.h>
#include "platform.h"

/* ---- symbols living in the asm/C core ---- */
extern void zstart(void);
extern void Game60hzcall(void);
extern void makeextension(void);
extern void ProcessSoundBuffer(void);
extern void SoundProcess(void);
extern void DrawWin256x224x16(void);
extern void init_hq_tables_dummy(void);

extern unsigned char fname[];
extern unsigned KeyQuickRst;
extern unsigned char pressed[256 + 128 + 64];
extern unsigned char cvidmode, soundon, SoundQuality, StereoSound, romispal;
extern unsigned int per2exec;
extern void Setper2exec(void);
extern unsigned char curblank, newengen, NGNoTransp, GUIOn, pl1contrl, pl2contrl;
extern unsigned short resolutn;
extern unsigned int vidbuffer;
extern int DSPBuffer[];
extern unsigned int BufferSizeB, BufferSizeW;
extern unsigned int AddEndBytes, NumBytesPerLine;
extern unsigned char *WinVidMemStart;
extern unsigned int converta;
extern unsigned int pl1upk, pl1downk, pl1leftk, pl1rightk, pl1startk, pl1selk,
    pl1Ak, pl1Bk, pl1Xk, pl1Yk, pl1Lk, pl1Rk;
extern unsigned int pl2upk, pl2downk, pl2leftk, pl2rightk, pl2startk, pl2selk,
    pl2Ak, pl2Bk, pl2Xk, pl2Yk, pl2Lk, pl2Rk;


/* The original asm does not preserve ebx/esi/edi/ebp (ebx is the GOT pointer in PIC code),
 * so every C->asm call goes through this register-saving trampoline. */
__asm__(".text\n.globl zs_call_asm\n.type zs_call_asm,@function\nzs_call_asm:\n"
        "  pushal\n  movl 36(%esp),%eax\n  cld\n  call *%eax\n  popal\n  ret\n");
extern void zs_call_asm(void (*fn)(void));

/* ---- variables the asm expects the platform layer to provide ---- */
int numlockptr, MouseButton, MouseX, MouseY, MouseMove2X, MouseMove2Y;
int SystemTimewHour, SystemTimewMinute, SystemTimewSecond;
int T36HZEnabled, T60HZEnabled;

/* used by the video blit routines in copyvwin.asm */
unsigned int ScreenPtr, SurfBufD, pitch;
int SurfaceX = 256, SurfaceY = 224;

/* ---- shared state with libretro.c ---- */
uint16_t zs_framebuf[ZS_MAX_W * ZS_MAX_H];
unsigned zs_width = 256, zs_height = 224;
int zs_frame_ready;
uint16_t zs_pad[2];  /* RETRO_DEVICE_ID_JOYPAD_* bitmask per port */

static ucontext_t main_ctx, emu_ctx;
static char *emu_stack;
static int emu_started, emu_dead;

#define EMU_STACK_SIZE (8 * 1024 * 1024)

/* Scancode slots we assign to pad buttons. */
static const int padkey_base[2] = { 0x10, 0x20 };
/* order matches the RETRO_DEVICE_ID_JOYPAD_* ids: B Y SELECT START UP DOWN LEFT RIGHT A X L R */

static void set_keymap(void)
{
    unsigned *p1[12] = { &pl1Bk, &pl1Yk, &pl1selk, &pl1startk, &pl1upk, &pl1downk,
                         &pl1leftk, &pl1rightk, &pl1Ak, &pl1Xk, &pl1Lk, &pl1Rk };
    unsigned *p2[12] = { &pl2Bk, &pl2Yk, &pl2selk, &pl2startk, &pl2upk, &pl2downk,
                         &pl2leftk, &pl2rightk, &pl2Ak, &pl2Xk, &pl2Lk, &pl2Rk };
    int i;
    for (i = 0; i < 12; i++) {
        *p1[i] = padkey_base[0] + i;
        *p2[i] = padkey_base[1] + i;
    }
    KeyQuickRst = 0x30;   /* quick-reset scancode, driven by retro_reset() */
    pl1contrl = 1;  /* keyboard-type input; our pad state is injected into pressed[] */
    pl2contrl = 1;
}

int zs_reset_req;
static void poll_input(void)
{
    int port, i;
    pressed[0x30] = zs_reset_req ? 1 : 0;
    zs_reset_req = 0;
    for (port = 0; port < 2; port++)
        for (i = 0; i < 12; i++)
            pressed[padkey_base[port] + i] = (zs_pad[port] >> i) & 1;
}

/* ---------------- coroutine plumbing ---------------- */

static void emu_entry(void)
{
    zs_call_asm(zstart);
    emu_dead = 1;
    for (;;) swapcontext(&emu_ctx, &main_ctx);
}

int zs_start(const char *rom_path)
{
    unsigned char *f = fname;
    size_t n = strlen(rom_path);
    if (n > 126) n = 126;
    f[0] = (unsigned char)n;
    memcpy(f + 1, rom_path, n);
    f[n + 1] = 0;
    zs_call_asm(makeextension);

    emu_stack = malloc(EMU_STACK_SIZE);
    if (!emu_stack) return 0;
    getcontext(&emu_ctx);
    emu_ctx.uc_stack.ss_sp = emu_stack;
    emu_ctx.uc_stack.ss_size = EMU_STACK_SIZE;
    emu_ctx.uc_link = &main_ctx;
    makecontext(&emu_ctx, emu_entry, 0);
    emu_started = 1;
    emu_dead = 0;
    return 1;
}

/* Run the emulator until it asks to wait for the next 60Hz tick. */
int zs_run_frame(void)
{
    if (!emu_started || emu_dead) return 0;
    poll_input();
    swapcontext(&main_ctx, &emu_ctx);
    return !emu_dead;
}

void zs_stop(void)
{
    free(emu_stack);
    emu_stack = NULL;
    emu_started = 0;
}

/* Called by the asm (Check60hz) when it wants to sleep until the next tick. */
void sem_sleep(void)
{
    swapcontext(&emu_ctx, &main_ctx);
    zs_call_asm((void (*)(void))Game60hzcall);   /* one emulated frame's worth of timer ticks */
}

void CheckTimers(void) { /* pacing is handled by the frontend */ }

void LinuxExit(void)
{
    emu_dead = 1;
    for (;;) swapcontext(&emu_ctx, &main_ctx);
}

/* ---------------- timers (no-ops) ---------------- */
void Start60HZ(void) { T36HZEnabled = 0; T60HZEnabled = 1; }
void Stop60HZ(void)  { T60HZEnabled = 0; }
void Start36HZ(void) { T60HZEnabled = 0; T36HZEnabled = 1; }
void Stop36HZ(void)  { T36HZEnabled = 0; }

/* ---------------- test hook ----------------
 * ZS_WRAM_AT_DRAW=1: snapshot WRAM at the moment the frame is drawn (the point where the original
 * SDL build calls SDL_Flip), so traces can be compared sample-point for sample-point with the
 * reference executable.  retro_get_memory_data() then returns the snapshot. */
extern unsigned char wramdataa[];
unsigned char zs_wram_snap[131072];
int zs_snap_enabled = -1;
static void snap_wram(void)
{
    if (zs_snap_enabled < 0) zs_snap_enabled = getenv("ZS_WRAM_AT_DRAW") != NULL;
    if (zs_snap_enabled) memcpy(zs_wram_snap, wramdataa, sizeof(zs_wram_snap));
}

/* ---------------- video ---------------- */
void initwinvideo(void)
{
    static int first = 1;
    cvidmode = 1;          /* 256x224 16-bit windowed */
    SurfaceX = 256;
    SurfaceY = 224;
    converta = 0;          /* RGB565 */
    soundon = 1;
    SoundQuality = 5;      /* 32000 Hz */
    StereoSound = 1;
    set_keymap();
    if (first && getenv("ZS_TEST_PER2EXEC")) { /* negative control for timing tests */
        per2exec = (unsigned)atoi(getenv("ZS_TEST_PER2EXEC"));
        Setper2exec();
    }
    if (first) {
        first = 0;
        /* hq2x/other filter tables are only needed for the unsupported modes */
    }
}

void clearwin(void)
{
    memset(zs_framebuf, 0, sizeof(zs_framebuf));
}

void UpdateVFrame(void)
{
    /* original polled SDL events and refilled the audio ring; both handled by the frontend now */
}

void drawscreenwin(void)
{
    NGNoTransp = 0;
    if (curblank != 0) return;
    snap_wram();

    ScreenPtr = vidbuffer;
    ScreenPtr += 16 * 2 + 32 * 2 + 256 * 2;
    if (resolutn == 239) ScreenPtr += 8 * 288 * 2;

    pitch = ZS_MAX_W * 2;
    SurfBufD = (unsigned int)zs_framebuf;
    zs_call_asm(DrawWin256x224x16);
    zs_width = 256;
    zs_height = 224;
    zs_frame_ready = 1;
}

/* ---------------- audio ---------------- */
/* Produce `frames` stereo frames at 32kHz. The DSP emits 256 samples per call. */
int zs_audio_chunk(int16_t *out)
{
    int i;
    zs_call_asm(SoundProcess);  /* sets buffer sizes and runs ProcessSoundBuffer */
    for (i = 0; i < 256; i++) {
        int v = DSPBuffer[i];
        out[i] = v > 32767 ? 32767 : (v < -32767 ? -32767 : v);
    }
    return 128;      /* stereo frames per chunk */
}

/* ---------------- mouse / time / misc ---------------- */
int GetMouseX(void)     { return MouseX; }
int GetMouseY(void)     { return MouseY; }
int GetMouseMoveX(void) { return 0; }
int GetMouseMoveY(void) { return 0; }
int GetMouseButton(void){ return MouseButton; }
void SetMouseMinX(int v) {}
void SetMouseMaxX(int v) {}
void SetMouseMinY(int v) {}
void SetMouseMaxY(int v) {}
void SetMouseX(int v) { MouseX = v; }
void SetMouseY(int v) { MouseY = v; }

void GetLocalTime(void)
{
    time_t t = time(NULL);
    struct tm *tm = localtime(&t);
    SystemTimewHour = tm->tm_hour;
    SystemTimewMinute = tm->tm_min;
    SystemTimewSecond = tm->tm_sec;
}

void ZsnesPage(void) {}
void ccmdline(void) {}

/* zip / jma containers are unwrapped by the frontend (RetroArch) */
int load_jma_file(const char *n) { return 0; }
void *unzOpen(const char *n) { return NULL; }
int unzClose(void *f) { return 0; }
int unzGoToFirstFile(void *f) { return -1; }
int unzGoToNextFile(void *f) { return -1; }
int unzLocateFile(void *f, const char *n, int c) { return -1; }
int unzGetCurrentFileInfo(void *f, void *a, char *b, unsigned long c, void *d, unsigned long e, char *g, unsigned long h) { return -1; }
int unzOpenCurrentFile(void *f) { return -1; }
int unzReadCurrentFile(void *f, void *b, unsigned n) { return -1; }
int unzCloseCurrentFile(void *f) { return -1; }

/* gz* shim: plain stdio, no gzip decompression (system zlib.h is used for declarations only) */
gzFile gzopen(const char *p, const char *m) { return (gzFile)fopen(p, m); }
int gzclose(gzFile f) { return fclose((FILE *)f); }
int gzread(gzFile f, voidp b, unsigned n) { return (int)fread(b, 1, n, (FILE *)f); }
int gzwrite(gzFile f, voidpc b, unsigned n) { return (int)fwrite(b, 1, n, (FILE *)f); }
z_off_t gzseek(gzFile f, z_off_t o, int w) { return fseek((FILE *)f, o, w) ? -1 : ftell((FILE *)f); }
z_off_t gztell(gzFile f) { return ftell((FILE *)f); }

/* constants used by the 32-bit blitters in video/sw_draw.asm */
long long copymaskRB = 0x001FF800001FF800LL;
long long copymaskG = 0x0000FC000000FC00LL;
long long copymagic = 0x0008010000080100LL;
long long coef = 0x0066009a0066009aLL;

/* ---------------- SRAM ---------------- */
extern unsigned char *sram;       /* dd pointer to cartridge SRAM */
extern unsigned ramsize;
extern unsigned char sramsavedis;
void SaveSramData(void);
void *zs_sram_ptr(void) { return ramsize ? (void *)sram : NULL; }
size_t zs_sram_size(void) { return ramsize; }
void zs_save_sram(void) { if (emu_started && !emu_dead) zs_call_asm(SaveSramData); }
