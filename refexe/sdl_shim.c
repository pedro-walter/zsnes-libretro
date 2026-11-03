/* Headless SDL shim for the reference ZSNES executable.
 * ZS_REF_TRACE=file ZS_REF_FRAMES=N : every SDL_Flip (one per displayed emulated frame) appends
 * "idx fuel demo ctr" from WRAM, then the process exits after N flips. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <semaphore.h>
#include <time.h>
#include <errno.h>
#include "SDL.h"

extern unsigned char wramdataa[];
static SDL_Surface surf; static SDL_PixelFormat fmt;
static FILE *trace; static long flips, maxflips;

int SDL_Init(Uint32 f) { return 0; }
int SDL_InitSubSystem(Uint32 f) { return 0; }
void SDL_QuitSubSystem(Uint32 f) {}
void SDL_Quit(void) { if (trace) fflush(trace); }
char *SDL_GetError(void) { return "headless"; }

SDL_Surface *SDL_SetVideoMode(int w, int h, int bpp, Uint32 flags)
{
    free(surf.pixels);
    fmt.Gmask = 0x07E0;                     /* RGB565 */
    surf.format = &fmt; surf.w = w; surf.h = h; surf.flags = flags;
    surf.pitch = (Uint16)(w * 2);
    surf.pixels = calloc(1, (size_t)w * h * 2 + 4096);
    if (getenv("ZS_REF_TRACE") && !trace) {
        trace = fopen(getenv("ZS_REF_TRACE"), "w");
        maxflips = getenv("ZS_REF_FRAMES") ? atol(getenv("ZS_REF_FRAMES")) : 1000;
    }
    fprintf(stderr, "[shim] video mode %dx%d\n", w, h);
    return &surf;
}
static unsigned crc_wram(void)
{
    static unsigned t[256]; static int init; unsigned c = 0xFFFFFFFFu; int i, k;
    if (!init) { for (i = 0; i < 256; i++) { unsigned r = i; for (k = 0; k < 8; k++) r = (r & 1) ? (r >> 1) ^ 0xEDB88320u : r >> 1; t[i] = r; } init = 1; }
    for (i = 0; i < 131072; i++) c = t[(c ^ wramdataa[i]) & 0xFF] ^ (c >> 8);
    return ~c;
}
int SDL_Flip(SDL_Surface *s)
{
    if (trace) {
        flips++;
        fprintf(trace, "%ld w=%08x %x=%x %x=%x %x=%x\n", flips, crc_wram(),
                0x1e62, wramdataa[0x1e62] | wramdataa[0x1e63] << 8,
                0x1ff6, wramdataa[0x1ff6] | wramdataa[0x1ff7] << 8,
                0x1ffc, wramdataa[0x1ffc] | wramdataa[0x1ffd] << 8);
        if (flips >= maxflips) { fflush(trace); _exit(0); }
    }
    return 0;
}
int SDL_LockSurface(SDL_Surface *s) { return 0; }
void SDL_UnlockSurface(SDL_Surface *s) {}
void SDL_WM_SetCaption(const char *a, const char *b) {}
int SDL_WM_GrabInput(int m) { return 0; }
int SDL_ShowCursor(int t) { return 0; }
void SDL_WarpMouse(Uint16 x, Uint16 y) {}
Uint8 SDL_GetRelativeMouseState(int *x, int *y) { if (x) *x = 0; if (y) *y = 0; return 0; }
int SDL_PollEvent(SDL_Event *e) { return 0; }
int SDL_NumJoysticks(void) { return 0; }
SDL_Joystick *SDL_JoystickOpen(int i) { return NULL; }
const char *SDL_JoystickName(int i) { return ""; }
int SDL_JoystickNumAxes(SDL_Joystick *j) { return 0; }
int SDL_JoystickNumButtons(SDL_Joystick *j) { return 0; }
int SDL_JoystickNumHats(SDL_Joystick *j) { return 0; }
int SDL_JoystickNumBalls(SDL_Joystick *j) { return 0; }
int SDL_JoystickEventState(int s) { return 0; }

/* ---- audio: a thread that drains the callback at the real device rate ---- */
static SDL_AudioSpec spec; static pthread_t audio_th; static int audio_run, audio_paused = 1;
static pthread_mutex_t audio_mx = PTHREAD_MUTEX_INITIALIZER;
static void *audio_thread(void *a)
{
    Uint8 *buf = malloc(spec.size);
    long ns = (long)(1e9 * (spec.size / (2.0 * spec.channels)) / spec.freq);
    while (audio_run) {
        struct timespec ts = { ns / 1000000000L, ns % 1000000000L }; nanosleep(&ts, NULL);
        if (audio_paused) continue;
        pthread_mutex_lock(&audio_mx);
        memset(buf, 0, spec.size); spec.callback(spec.userdata, buf, (int)spec.size);
        pthread_mutex_unlock(&audio_mx);
    }
    free(buf); return NULL;
}
int SDL_OpenAudio(SDL_AudioSpec *w, SDL_AudioSpec *o)
{
    w->size = w->samples * w->channels * 2; w->silence = 0; spec = *w; if (o) *o = *w;
    audio_run = 1; audio_paused = 1; pthread_create(&audio_th, NULL, audio_thread, NULL); return 0;
}
void SDL_CloseAudio(void) { if (audio_run) { audio_run = 0; pthread_join(audio_th, NULL); } }
void SDL_PauseAudio(int p) { audio_paused = p; }
void SDL_LockAudio(void) { pthread_mutex_lock(&audio_mx); }
void SDL_UnlockAudio(void) { pthread_mutex_unlock(&audio_mx); }

/* ---- semaphores / threads ---- */
SDL_sem *SDL_CreateSemaphore(Uint32 v) { sem_t *s = malloc(sizeof *s); sem_init(s, 0, v); return (SDL_sem *)s; }
void SDL_DestroySemaphore(SDL_sem *s) { sem_destroy((sem_t *)s); free(s); }
int SDL_SemPost(SDL_sem *s) { return sem_post((sem_t *)s); }
int SDL_SemWaitTimeout(SDL_sem *s, Uint32 ms)
{
    struct timespec t; clock_gettime(CLOCK_REALTIME, &t);
    t.tv_sec += ms / 1000; t.tv_nsec += (long)(ms % 1000) * 1000000L;
    if (t.tv_nsec >= 1000000000L) { t.tv_sec++; t.tv_nsec -= 1000000000L; }
    return sem_timedwait((sem_t *)s, &t) ? 1 : 0;
}
struct SDL_Thread { pthread_t t; };
SDL_Thread *SDL_CreateThread(int (*fn)(void *), void *d)
{
    SDL_Thread *t = malloc(sizeof *t);
    pthread_create(&t->t, NULL, (void *(*)(void *))fn, d); return t;
}
void SDL_WaitThread(SDL_Thread *t, int *st) { pthread_join(t->t, NULL); free(t); }
