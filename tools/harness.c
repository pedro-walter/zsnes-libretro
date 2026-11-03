/* Headless libretro frontend for testing cores: no window, no audio device.
 * usage: harness core.so rom [-f frames] [-s frame[,frame...]] [-w out.wav] [-i "frame:mask,..."] [-d savedir]
 *   -s  save PNG screenshots at those frames (out_<frame>.png, prefix with -o)
 *   -i  hold joypad-1 button mask (bit = RETRO_DEVICE_ID_JOYPAD_*) from the given frame on
 * Prints a CRC32 of every screenshot frame and of all audio. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdarg.h>
#include <dlfcn.h>
#include <zlib.h>
#include "../libretro/libretro.h"

static uint16_t cur_mask;
static uint8_t *last_rgb; static unsigned lw, lh;
static FILE *wav; static uint32_t wav_bytes; static uint32_t audio_crc;
static unsigned audio_frames_total;

static void log_cb(enum retro_log_level l, const char *fmt, ...) {
    va_list a; va_start(a, fmt); fprintf(stderr, "[core] "); vfprintf(stderr, fmt, a); va_end(a);
}
static bool env(unsigned cmd, void *d) {
    switch (cmd) {
    case RETRO_ENVIRONMENT_GET_LOG_INTERFACE: ((struct retro_log_callback*)d)->log = log_cb; return true;
    case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT: return *(enum retro_pixel_format*)d == RETRO_PIXEL_FORMAT_RGB565;
    case RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY:
    case RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY: *(const char**)d = getenv("HARNESS_DIR") ? getenv("HARNESS_DIR") : "/tmp"; return true;
    default: return false;
    }
}
static void video(const void *data, unsigned w, unsigned h, size_t pitch) {
    if (!data) return;
    lw = w; lh = h;
    free(last_rgb); last_rgb = malloc(w * h * 3);
    for (unsigned y = 0; y < h; y++) for (unsigned x = 0; x < w; x++) {
        uint16_t p = ((const uint16_t*)((const char*)data + y * pitch))[x];
        uint8_t *o = last_rgb + (y * w + x) * 3;
        o[0] = ((p >> 11) & 31) * 255 / 31; o[1] = ((p >> 5) & 63) * 255 / 63; o[2] = (p & 31) * 255 / 31;
    }
}
static size_t audio(const int16_t *d, size_t n) {
    audio_frames_total += n; audio_crc = crc32(audio_crc, (const Bytef*)d, n * 4);
    if (wav) { fwrite(d, 4, n, wav); wav_bytes += n * 4; }
    return n;
}
static void sample(int16_t l, int16_t r) { int16_t b[2] = { l, r }; audio(b, 1); }
static void poll(void) {}
static int16_t input(unsigned port, unsigned dev, unsigned idx, unsigned id) {
    return (port == 0 && dev == RETRO_DEVICE_JOYPAD && ((cur_mask >> id) & 1)) ? 1 : 0;
}

static void put32(uint8_t *p, uint32_t v) { p[0]=v>>24; p[1]=v>>16; p[2]=v>>8; p[3]=v; }
static void chunk(FILE *f, const char *t, const uint8_t *d, uint32_t n) {
    uint8_t h[8]; put32(h, n); memcpy(h + 4, t, 4); fwrite(h, 1, 8, f);
    if (n) fwrite(d, 1, n, f);
    uint32_t c = crc32(0, (const Bytef*)t, 4); if (n) c = crc32(c, d, n);
    uint8_t cb[4]; put32(cb, c); fwrite(cb, 1, 4, f);
}
static void write_png(const char *path) {
    if (!last_rgb) return;
    FILE *f = fopen(path, "wb"); if (!f) return;
    fwrite("\x89PNG\r\n\x1a\n", 1, 8, f);
    uint8_t ihdr[13]; put32(ihdr, lw); put32(ihdr + 4, lh); ihdr[8]=8; ihdr[9]=2; ihdr[10]=ihdr[11]=ihdr[12]=0;
    chunk(f, "IHDR", ihdr, 13);
    uLong rawn = (lw * 3 + 1) * lh; uint8_t *raw = malloc(rawn);
    for (unsigned y = 0; y < lh; y++) { raw[y * (lw*3+1)] = 0; memcpy(raw + y*(lw*3+1) + 1, last_rgb + y*lw*3, lw*3); }
    uLongf zn = compressBound(rawn); uint8_t *z = malloc(zn); compress(z, &zn, raw, rawn);
    chunk(f, "IDAT", z, zn); chunk(f, "IEND", NULL, 0); fclose(f); free(raw); free(z);
}
static void wav_header(FILE *f, uint32_t bytes) {
    uint32_t v; uint16_t s; fseek(f, 0, SEEK_SET); fwrite("RIFF", 1, 4, f); v = 36 + bytes; fwrite(&v, 4, 1, f);
    fwrite("WAVEfmt ", 1, 8, f); v = 16; fwrite(&v, 4, 1, f); s = 1; fwrite(&s, 2, 1, f); s = 2; fwrite(&s, 2, 1, f);
    v = 32000; fwrite(&v, 4, 1, f); v = 128000; fwrite(&v, 4, 1, f); s = 4; fwrite(&s, 2, 1, f); s = 16; fwrite(&s, 2, 1, f);
    fwrite("data", 1, 4, f); fwrite(&bytes, 4, 1, f);
}

static int resetf = -1, fuzz = 0, dspfuzz = 0, dspwfuzz = 0, cpufuzz = 0, hdmafuzz = 0;
int main(int argc, char **argv) {
    if (argc < 3) { fprintf(stderr, "usage: %s core.so rom [-f N] [-s a,b] [-o prefix] [-w wav] [-i f:mask,...]\n", argv[0]); return 2; }
    int frames = 120; const char *trace = NULL, *tracepath = NULL; const char *shots = "", *prefix = "shot", *wavp = NULL, *inputs = "";
    for (int i = 3; i + 1 < argc; i += 2) {
        if (!strcmp(argv[i], "-f")) frames = atoi(argv[i+1]);
        else if (!strcmp(argv[i], "-s")) shots = argv[i+1];
        else if (!strcmp(argv[i], "-o")) prefix = argv[i+1];
        else if (!strcmp(argv[i], "-w")) wavp = argv[i+1];
        else if (!strcmp(argv[i], "-i")) inputs = argv[i+1];
        else if (!strcmp(argv[i], "-t")) trace = argv[i+1];
        else if (!strcmp(argv[i], "-T")) tracepath = argv[i+1];
        else if (!strcmp(argv[i], "-r")) resetf = atoi(argv[i+1]);
        else if (!strcmp(argv[i], "-F")) fuzz = atoi(argv[i+1]);
        else if (!strcmp(argv[i], "-D")) dspfuzz = atoi(argv[i+1]);
        else if (!strcmp(argv[i], "-W")) dspwfuzz = atoi(argv[i+1]);
        else if (!strcmp(argv[i], "-C")) cpufuzz = atoi(argv[i+1]);
        else if (!strcmp(argv[i], "-H")) hdmafuzz = atoi(argv[i+1]);
    }
    void *h = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
    if (!h) { fprintf(stderr, "dlopen: %s\n", dlerror()); return 1; }
#define SYM(n) __typeof__(&n) n##_p = (__typeof__(&n))dlsym(h, #n); if (!n##_p) { fprintf(stderr, "missing " #n "\n"); return 1; }
    SYM(retro_set_environment) SYM(retro_set_video_refresh) SYM(retro_set_audio_sample) SYM(retro_set_audio_sample_batch)
    SYM(retro_set_input_poll) SYM(retro_set_input_state) SYM(retro_init) SYM(retro_load_game) SYM(retro_run)
    SYM(retro_reset) SYM(retro_get_memory_data) SYM(retro_get_memory_size) SYM(retro_get_system_av_info) SYM(retro_unload_game) SYM(retro_deinit)
    retro_set_environment_p(env); retro_set_video_refresh_p(video); retro_set_audio_sample_p(sample);
    retro_set_audio_sample_batch_p(audio); retro_set_input_poll_p(poll); retro_set_input_state_p(input);
    retro_init_p();
    struct retro_game_info gi = { argv[2], NULL, 0, NULL };
    if (!retro_load_game_p(&gi)) { fprintf(stderr, "load_game failed\n"); return 1; }
    struct retro_system_av_info av; retro_get_system_av_info_p(&av);
    fprintf(stderr, "av: %ux%u @ %.4f fps, %.0f Hz\n", av.geometry.base_width, av.geometry.base_height, av.timing.fps, av.timing.sample_rate);
    if (wavp) { wav = fopen(wavp, "wb"); wav_header(wav, 0); }
    FILE *tf = trace ? (tracepath ? fopen(tracepath, "w") : stdout) : NULL;
    for (int f = 1; f <= frames; f++) {
        /* input schedule "frame:mask,frame:mask" */
        const char *p = inputs; while (*p) { int fr, m, n = 0; if (sscanf(p, "%d:%i%n", &fr, &m, &n) >= 2 && fr == f) cur_mask = m; p += n ? n : 1; if (*p == ',') p++; }
        if (f == resetf) retro_reset_p();
        retro_run_p();
        if (tf) { /* "addr:bytes,..." hex addr into 128K WRAM; bytes 1 or 2 (little endian) */
            uint8_t *ram = retro_get_memory_data_p(RETRO_MEMORY_SYSTEM_RAM); fprintf(tf, "%d", f);
            const char *t = trace; while (*t) { unsigned a, n, k = 0; if (sscanf(t, "%x:%u%n", &a, &n, &k) >= 2) { unsigned v = ram[a]; if (n == 2) v |= ram[a+1] << 8; fprintf(tf, " %x=%x", a, v); } t += k ? k : 1; if (*t == ',') t++; }
            fprintf(tf, "\n"); }
        const char *s = shots; while (*s) { int fr, n = 0; if (sscanf(s, "%d%n", &fr, &n) >= 1) { if (fr == f) {
            char path[512]; snprintf(path, sizeof path, "%s_%04d.png", prefix, f); write_png(path);
            uint32_t c = last_rgb ? crc32(0, last_rgb, lw * lh * 3) : 0; printf("frame %d: %ux%u crc %08x -> %s\n", f, lw, lh, c, path); } }
            s += n ? n : 1; if (*s == ',') s++; }
    }
    if (hdmafuzz) { unsigned (*fz)(unsigned, unsigned) = (unsigned (*)(unsigned, unsigned))dlsym(h, "zs_hdma_fuzz"); if (fz) { unsigned m = fz(hdmafuzz, 99); printf("hdma fuzz mismatches: %u\n", m); } }
    if (cpufuzz) { unsigned (*fz)(unsigned, unsigned) = (unsigned (*)(unsigned, unsigned))dlsym(h, "zs_cpu_fuzz"); if (fz) { unsigned m = fz(cpufuzz, 31337); printf("cpu fuzz mismatches: %u\n", m); } }
    if (dspwfuzz) { unsigned (*fz)(unsigned, unsigned) = (unsigned (*)(unsigned, unsigned))dlsym(h, "zs_dspw_fuzz"); if (fz) { unsigned m = fz(dspwfuzz, 4242); printf("dspw fuzz mismatches: %u\n", m); } }
    if (dspfuzz) { unsigned (*fz)(unsigned, unsigned) = (unsigned (*)(unsigned, unsigned))dlsym(h, "zs_dsp_fuzz"); if (fz) { unsigned m = fz(dspfuzz, 777); printf("dsp fuzz mismatches: %u\n", m); } }
    if (fuzz) { unsigned (*fz)(unsigned, unsigned) = (unsigned (*)(unsigned, unsigned))dlsym(h, "zs_spc_fuzz"); if (fz) { unsigned m = fz(fuzz, 12345); printf("fuzz mismatches: %u\n", m); } }
    if (tf && tracepath) fclose(tf);
    if (wav) { wav_header(wav, wav_bytes); fclose(wav); }
    printf("done: %d frames, audio %u frames crc %08x\n", frames, audio_frames_total, audio_crc);
    retro_unload_game_p(); retro_deinit_p();
    return 0;
}
