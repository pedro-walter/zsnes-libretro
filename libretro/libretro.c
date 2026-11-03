/* libretro API entry points for the ZSNES 1.42 core. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "libretro.h"
#ifdef _WIN32
#include <direct.h>
#define mkdir(p, m) _mkdir(p)
#define setenv(k, v, o) _putenv_s(k, v)
#endif
#include "platform.h"

static retro_environment_t env_cb;
static retro_video_refresh_t video_cb;
static retro_audio_sample_batch_t audio_batch_cb;
static retro_input_poll_t input_poll_cb;
static retro_input_state_t input_state_cb;
static retro_log_printf_t log_cb;

static int is_pal;
#ifdef LIFTED
static bool cheat_dirty;                 /* applied at the start of retro_run, once the guest has finished its init */
static unsigned run_count;
static void cheat_rebuild(void);
#endif
extern unsigned char romispal;
static int16_t audio_buf[256 * 8];
static int audio_frames_pending;   /* fractional accumulator, in 1/1000 frames */

void retro_set_environment(retro_environment_t cb)
{
    struct retro_log_callback log;
    env_cb = cb;
    if (cb(RETRO_ENVIRONMENT_GET_LOG_INTERFACE, &log)) log_cb = log.log;
    bool yes = true;
    cb(RETRO_ENVIRONMENT_SET_SUPPORT_NO_GAME, &(bool){ false });
    (void)yes;
}
void retro_set_video_refresh(retro_video_refresh_t cb) { video_cb = cb; }
void retro_set_audio_sample(retro_audio_sample_t cb) { (void)cb; }
void retro_set_audio_sample_batch(retro_audio_sample_batch_t cb) { audio_batch_cb = cb; }
void retro_set_input_poll(retro_input_poll_t cb) { input_poll_cb = cb; }
void retro_set_input_state(retro_input_state_t cb) { input_state_cb = cb; }

unsigned retro_api_version(void) { return RETRO_API_VERSION; }

void retro_get_system_info(struct retro_system_info *info)
{
    memset(info, 0, sizeof(*info));
    info->library_name = "ZSNES 1.42";
    info->library_version = "1.42";
    info->valid_extensions = "smc|sfc|swc|fig|bs";
    info->need_fullpath = true;   /* ZSNES loads the ROM itself from disk */
    info->block_extract = false;
}

void retro_get_system_av_info(struct retro_system_av_info *info)
{
    memset(info, 0, sizeof(*info));
    info->geometry.base_width = 256;
    info->geometry.base_height = 224;
    info->geometry.max_width = ZS_MAX_W;
    info->geometry.max_height = ZS_MAX_H;
    info->geometry.aspect_ratio = 4.0f / 3.0f;
    info->timing.fps = is_pal ? 50.0070 : 60.0988;
    info->timing.sample_rate = 32000.0;
}

void retro_set_controller_port_device(unsigned port, unsigned device) { (void)port; (void)device; }

void retro_init(void)
{
    enum retro_pixel_format fmt = RETRO_PIXEL_FORMAT_RGB565;
    env_cb(RETRO_ENVIRONMENT_SET_PIXEL_FORMAT, &fmt);
}

void retro_deinit(void) { zs_stop(); }

/* Same rule as init.asm: country code 2..0Ch at header+0x19 means PAL (header found via checksum complement). */
static int rom_is_pal(const char *path)
{
    static const long offs[2] = { 0x7FC0, 0xFFC0 };
    FILE *f = fopen(path, "rb");
    long sz, base = 0;
    int i, pal = 0;
    unsigned char h[32];
    if (!f) return 0;
    fseek(f, 0, SEEK_END); sz = ftell(f);
    if (sz % 1024 == 512) base = 512;
    for (i = 0; i < 2; i++) {
        if (base + offs[i] + 32 > sz) continue;
        fseek(f, base + offs[i], SEEK_SET);
        if (fread(h, 1, 32, f) != 32) continue;
        if ((unsigned)(h[0x1C] | h[0x1D] << 8) + (unsigned)(h[0x1E] | h[0x1F] << 8) == 0xFFFF) {
            pal = h[0x19] >= 2 && h[0x19] <= 0x0C;
            break;
        }
    }
    fclose(f);
    return pal;
}

bool retro_load_game(const struct retro_game_info *game)
{
    const char *dir = NULL;
    char path[1024];
    if (!game || !game->path) return false;

    /* ZSNES keeps its config/SRAM under $HOME/.zsnes; point HOME at the frontend's save dir. */
    if (env_cb(RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY, &dir) && dir) {
        snprintf(path, sizeof(path), "%s/zsnes_home", dir);
        mkdir(path, 0755);
        setenv("HOME", path, 1);
    }
    if (!zs_start(game->path)) return false;
#ifdef LIFTED
    run_count = 0; cheat_dirty = true;
#endif
    is_pal = rom_is_pal(game->path);
    return true;
}

bool retro_load_game_special(unsigned t, const struct retro_game_info *i, size_t n) { return false; }
extern void zs_save_sram(void);
void retro_unload_game(void) { zs_save_sram(); zs_stop(); }
unsigned retro_get_region(void) { return is_pal ? RETRO_REGION_PAL : RETRO_REGION_NTSC; }

extern int zs_reset_req;
void retro_reset(void) { zs_reset_req = 1; }

void retro_run(void)
{
    int p, i;
#ifdef LIFTED
    if (run_count > 0 && cheat_dirty) { cheat_rebuild(); cheat_dirty = false; }
    run_count++;
#endif
    input_poll_cb();
    for (p = 0; p < 2; p++) {
        uint16_t m = 0;
        for (i = 0; i < 12; i++)
            if (input_state_cb(p, RETRO_DEVICE_JOYPAD, 0, i)) m |= 1u << i;
        zs_pad[p] = m;
    }

    zs_frame_ready = 0;
    zs_run_frame();

    video_cb(zs_frame_ready ? zs_framebuf : NULL, zs_width, zs_height, ZS_MAX_W * 2);

    /* 32000 Hz / 60.0988 fps = 532.4 frames; emit whole 128-frame DSP chunks, carrying the remainder */
    audio_frames_pending += is_pal ? 640000 : 532400;
    {
        int n = 0;
        while (audio_frames_pending >= 128000 && n < 8) {
            n++;
            audio_frames_pending -= 128000;
        }
        for (i = 0; i < n; i++) {
            int frames = zs_audio_chunk(audio_buf + i * 256);
            (void)frames;
        }
        if (n) audio_batch_cb(audio_buf, n * 128);
    }
}

extern size_t zs_state_size(void);
extern int zs_state_save(void *, size_t);
extern int zs_state_load(const void *, size_t);
size_t retro_serialize_size(void) { return zs_state_size(); }
bool retro_serialize(void *d, size_t s)
{
    int n;
    if (s < 8) return false;
    *(int32_t *)d = audio_frames_pending;
    n = zs_state_save((char *)d + 8, s - 8);
    if (n > 0 && (size_t)n + 8 < s) memset((char *)d + 8 + n, 0, s - 8 - n);
    return n > 0;
}
bool retro_unserialize(const void *d, size_t s)
{
    if (s < 8 || !zs_state_load((const char *)d + 8, s - 8)) return false;
    audio_frames_pending = *(const int32_t *)d;
    return true;
}
extern unsigned char wramdataa[];   /* 7E:0000-FFFF, immediately followed by 7F (ram7fa) */
extern void *zs_sram_ptr(void); extern size_t zs_sram_size(void);
void *retro_get_memory_data(unsigned id)
{
    extern unsigned char zs_wram_snap[]; extern int zs_snap_enabled;
    if (id == RETRO_MEMORY_SAVE_RAM) return zs_sram_ptr();
    if (id != RETRO_MEMORY_SYSTEM_RAM) return NULL;
    return zs_snap_enabled > 0 ? (void *)zs_wram_snap : (void *)wramdataa;
}
size_t retro_get_memory_size(unsigned id)
{
    return id == RETRO_MEMORY_SYSTEM_RAM ? 131072 : id == RETRO_MEMORY_SAVE_RAM ? zs_sram_size() : 0;
}
#ifdef LIFTED
/* Cheats: Pro Action Replay style RAM codes ("7E1234AB", several lines separated by '+' or spaces).  They go into ZSNES's
 * cheat table (28-byte entries: flags, value, address16, bank), which the scheduler applies once per frame through the
 * bank write handlers.  Game Genie (ROM patch) codes are not supported. */
extern unsigned char cheatdata[];
extern unsigned int NumCheats, CheatOn;
#define CHEAT_SLOTS 128
#define CHEAT_LINES 16
static struct { unsigned char val, bank; unsigned short addr; } cheat_line[CHEAT_SLOTS][CHEAT_LINES];
static unsigned char cheat_nlines[CHEAT_SLOTS];
static bool cheat_on[CHEAT_SLOTS];

static int hexv(int c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1; }

static void cheat_rebuild(void)
{
    unsigned n = 0, i, k;
    memset(cheatdata, 0, 28 * 255);
    for (i = 0; i < CHEAT_SLOTS; i++) {
        if (!cheat_on[i]) continue;
        for (k = 0; k < cheat_nlines[i] && n < 255; k++, n++) {
            unsigned char *e = cheatdata + 28 * n;
            e[0] = 0; e[1] = cheat_line[i][k].val;
            e[2] = (unsigned char)cheat_line[i][k].addr; e[3] = (unsigned char)(cheat_line[i][k].addr >> 8);
            e[4] = cheat_line[i][k].bank;
        }
    }
    NumCheats = n;
    CheatOn = n ? 1 : 0;
}

void retro_cheat_reset(void)
{
    memset(cheat_nlines, 0, sizeof cheat_nlines); memset(cheat_on, 0, sizeof cheat_on);
    cheat_dirty = true;
}
void retro_cheat_set(unsigned idx, bool enabled, const char *code)
{
    unsigned digits[8], nd = 0, nl = 0;
    if (idx >= CHEAT_SLOTS || !code) return;
    for (; *code; code++) {
        int h = hexv(*code);
        if (h < 0) { if (nd) nd = 0; continue; }   /* a separator inside a group drops the group */
        digits[nd++] = (unsigned)h;
        if (nd == 8) {
            if (nl < CHEAT_LINES) {
                cheat_line[idx][nl].bank = (unsigned char)((digits[0] << 4) | digits[1]);
                cheat_line[idx][nl].addr = (unsigned short)((digits[2] << 12) | (digits[3] << 8) | (digits[4] << 4) | digits[5]);
                cheat_line[idx][nl].val = (unsigned char)((digits[6] << 4) | digits[7]);
                nl++;
            }
            nd = 0;
        }
    }
    cheat_nlines[idx] = (unsigned char)nl; cheat_on[idx] = enabled && nl;
    cheat_dirty = true;
}
#else
void retro_cheat_reset(void) {}
void retro_cheat_set(unsigned i, bool e, const char *c) {}
#endif
