/* minimal zlib declarations for builds without zlib (the gz* functions are shims in libretro/platform.c) */
#ifndef ZLIB_SHIM_H
#define ZLIB_SHIM_H
#include <stddef.h>
#define ZEXTERN extern
#define ZEXPORT
#define ZEXPORTVA
#define OF(args) args
#define local static
typedef void *voidp; typedef const void *voidpc;
typedef void *gzFile;
typedef long z_off_t;
typedef unsigned char Bytef; typedef unsigned long uLong; typedef unsigned int uInt;
gzFile gzopen(const char *path, const char *mode);
int gzclose(gzFile f);
int gzread(gzFile f, voidp buf, unsigned len);
int gzwrite(gzFile f, voidpc buf, unsigned len);
z_off_t gzseek(gzFile f, z_off_t off, int whence);
z_off_t gztell(gzFile f);
#endif
