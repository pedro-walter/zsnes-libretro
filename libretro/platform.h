#ifndef ZS_PLATFORM_H
#define ZS_PLATFORM_H
#include <stdint.h>
#define ZS_MAX_W 512
#define ZS_MAX_H 478
extern uint16_t zs_framebuf[];
extern unsigned zs_width, zs_height;
extern int zs_frame_ready;
extern uint16_t zs_pad[2];
int zs_start(const char *rom_path);
int zs_run_frame(void);
void zs_stop(void);
int zs_audio_chunk(int16_t *out);
#endif
