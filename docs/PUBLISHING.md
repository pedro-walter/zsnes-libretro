# Getting the core into RetroArch's Core Downloader

The libretro buildbot builds cores listed in `libretro/libretro-super` and shows them in *Online Updater → Core Downloader*.

1. Repo: https://github.com/pedro-walter/zsnes-libretro (public, GPLv2, `COPYING`). Builds with only gcc + make:
   `make -f Makefile.libretro` (also `platform=win`, any `CC`; 32/64-bit picked from `$(CC) -dumpmachine`).
2. PR to `libretro/libretro-super`: add to `recipes/linux/cores-linux-x64-generic`, `recipes/windows/cores-windows-x64-generic`
   (and the x86 / arm variants if wanted):

       zsnes libretro-zsnes https://github.com/pedro-walter/zsnes-libretro.git master YES GENERIC Makefile.libretro .

3. PR to `libretro/libretro-core-info` adding `zsnes_libretro.info`.
4. Optionally ask on the libretro forum/Discord to move the repo under the `libretro` org.

Until it is merged: attach the built libs and `zsnes_libretro.info` to a GitHub Release.
Notes for reviewers: the lifted C in `gen/` is generated from the unmodified ZSNES 1.42 asm (see README); the core needs no firmware.
