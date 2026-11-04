/*
 * Memory bank handlers (cpu/memory.asm), register ABI of the originals:
 *   ecx = offset in the bank (16 bit), ebx = bank (or an offset for the membank0* family), eax = data (writes)
 *   result in al/ax, and the very same leftover register values as the asm (ebx is cleared by most handlers).
 * Rare chip paths (SA-1 BW-RAM, C4/OBC, SPC7110) are delegated to the original lifted body (zl_orig).
 */
#include "zl.h"
#define EAX LG.eax
#define ECX LG.ecx
#define EBX LG.ebx
#define A(sym) ((u32)ZO_##sym)

extern u8 SFXEnable, SA1Enable, DSP1Type, writeon, C4Enable, OBCEnable, SPC7110Enable;
extern u32 snesmmap[256], snesmap2[256], sfxramdata, sram, ramsize, ramsizeand, sramb4save;
extern u32 Bank0datr8[256], Bank0datr16[256], Bank0datw8[256], Bank0datw16[256];
extern u32 DPageR8, DPageR16, DPageW8, DPageW16;
extern u8 xd[4];

static inline void set_al(u32 v) { EAX = (EAX & ~0xFFu) | (v & 0xFF); }
static inline void set_ah(u32 v) { EAX = (EAX & ~0xFF00u) | ((v & 0xFF) << 8); }
static inline void set_ax(u32 v) { EAX = (EAX & ~0xFFFFu) | (v & 0xFFFF); }
static inline void set_bl(u32 v) { EBX = (EBX & ~0xFFu) | (v & 0xFF); }
static inline u32 get_al(void) { return EAX & 0xFF; }
static inline u32 get_ah(void) { return (EAX >> 8) & 0xFF; }
#define WRAM(i) M8(A(wramdataa) + (i))
#define WRAM16(i) M16(A(wramdataa) + (i))
#define REGPTRA(i) M32(A(regptra) + (i) * 4 - 0x8000u)
#define REGPTWA(i) M32(A(regptwa) + (i) * 4 - 0x8000u)
extern u8 wramdataa[];

/* ---------------- SRAM ---------------- */
static void sram_r8b(void)
{
    if (ramsize == 0) { set_al(0); EBX = 0; return; }
    { u32 sv = ECX; ECX &= ramsizeand; EBX = sram; set_al(M8(EBX + ECX)); ECX = sv; EBX = 0; }
}
static void sram_r16b(void)
{
    if (ramsize == 0) { set_ax(0); EBX = 0; return; }
    EBX = sram;
    { u32 sv = ECX; ECX &= ramsizeand; set_al(M8(EBX + ECX)); ECX++; ECX &= ramsizeand; set_ah(M8(EBX + ECX)); ECX = sv; EBX = 0; }
}
static void sram_w8b(void)
{
    if (ramsize != 0) {
        u32 sv; EBX = sram; sv = ECX; ECX &= ramsizeand; M8(EBX + ECX) = (u8)EAX; ECX = sv; sramb4save = 5 * 60;
    }
    EBX = 0;
}
static void sram_w16b(void)
{
    if (ramsize != 0) {
        u32 sv; EBX = sram; sv = ECX; ECX &= ramsizeand; M8(EBX + ECX) = (u8)EAX; ECX++; ECX &= ramsizeand; M8(EBX + ECX) = (u8)(EAX >> 8); ECX = sv; sramb4save = 5 * 60;
    }
    EBX = 0;
}
void zl_sramaccessbankr8b(void) { sram_r8b(); }
void zl_sramaccessbankr16b(void) { sram_r16b(); }
void zl_sramaccessbankw8b(void) { sram_w8b(); }
void zl_sramaccessbankw16b(void) { sram_w16b(); }

#define SRAMBANK(name, pre, base, fn) \
    void name(void) { u32 sv = ECX; pre; set_bl(EBX - (base)); EBX <<= 15; ECX += EBX; fn(); ECX = sv; }
#define F0FIX if ((EBX & 0xFF) == 0xF0) set_bl(EBX - 0x80)
void zl_sramaccessbankr8(void) { u32 sv = ECX; F0FIX; set_bl(EBX - 0x70); EBX <<= 15; ECX += EBX; sram_r8b(); ECX = sv; }
void zl_sramaccessbankr16(void) { u32 sv = ECX; set_bl(EBX - 0x70); EBX <<= 15; ECX += EBX; sram_r16b(); ECX = sv; }
void zl_sramaccessbankw8(void) { u32 sv = ECX; set_bl(EBX - 0x70); if ((EBX & 0xFF) == 0xF0) set_bl(EBX - 0x80); EBX <<= 15; ECX += EBX; sram_w8b(); ECX = sv; }
void zl_sramaccessbankw16(void) { u32 sv = ECX; F0FIX; set_bl(EBX - 0x70); EBX <<= 15; ECX += EBX; sram_w16b(); ECX = sv; }
void zl_sramaccessbankr8s(void) { u32 sv = ECX; set_bl(EBX - 0x78); EBX <<= 15; ECX += EBX; sram_r8b(); ECX = sv; }
void zl_sramaccessbankr16s(void) { u32 sv = ECX; set_bl(EBX - 0x78); EBX <<= 15; ECX += EBX; sram_r16b(); ECX = sv; }
void zl_sramaccessbankw8s(void) { u32 sv = ECX; set_bl(EBX - 0x78); EBX <<= 15; ECX += EBX; sram_w8b(); ECX = sv; }
void zl_sramaccessbankw16s(void) { u32 sv = ECX; set_bl(EBX - 0x78); EBX <<= 15; ECX += EBX; sram_w16b(); ECX = sv; }

/* ---------------- ROM / WRAM / ERAM banks ---------------- */
void zl_memaccessbankr8(void) { EBX = snesmmap[EBX]; set_al(M8(EBX + ECX)); EBX = 0; }
void zl_memaccessbankr16(void) { EBX = snesmmap[EBX]; set_ax(M16(EBX + ECX)); EBX = 0; }
void zl_memaccessbankw8(void) { if (writeon == 0) return; EBX = snesmmap[EBX]; M8(EBX + ECX) = (u8)EAX; EBX = 0; }
void zl_memaccessbankw16(void) { if (writeon == 0) return; EBX = snesmmap[EBX]; M16(EBX + ECX) = (u16)EAX; EBX = 0; }
void zl_memaccessbankr848mb(void) { EBX = (ECX & 0x8000) ? snesmmap[EBX] : snesmap2[EBX]; set_al(M8(EBX + ECX)); EBX = 0; }
void zl_memaccessbankr1648mb(void) { EBX = (ECX & 0x8000) ? snesmmap[EBX] : snesmap2[EBX]; set_ax(M16(EBX + ECX)); EBX = 0; }
void zl_wramaccessbankr8(void) { set_al(WRAM(ECX)); }
void zl_wramaccessbankr16(void) { set_ax(WRAM16(ECX)); }
void zl_wramaccessbankw8(void) { WRAM(ECX) = (u8)EAX; }
void zl_wramaccessbankw16(void) { WRAM16(ECX) = (u16)EAX; }
void zl_eramaccessbankr8(void) { set_al(M8(A(ram7fa) + ECX)); }
void zl_eramaccessbankr16(void) { set_ax(M16(A(ram7fa) + ECX)); }
void zl_eramaccessbankw8(void) { M8(A(ram7fa) + ECX) = (u8)EAX; }
void zl_eramaccessbankw16(void) { M16(A(ram7fa) + ECX) = (u16)EAX; }

/* ---------------- bank 0 family: ebx = offset, called through Bank0dat* ---------------- */
static void sfxram_r8(void)  { u32 sv = ECX; ECX = (ECX - 0x6000) & 0x1FFF; EBX = sfxramdata; set_al(M8(EBX + ECX)); EBX = 0; ECX = sv; }
static void sfxram_r16(void) { u32 sv = ECX; ECX = (ECX - 0x6000) & 0x1FFF; EBX = sfxramdata; set_ax(M16(EBX + ECX)); EBX = 0; ECX = sv; }
static void sfxram_w8(void)  { u32 sv = ECX; ECX = (ECX - 0x6000) & 0x1FFF; EBX = sfxramdata; M8(EBX + ECX) = (u8)EAX; EBX = 0; ECX = sv; }
static void sfxram_w16(void) { u32 sv = ECX; ECX = (ECX - 0x6000) & 0x1FFF; EBX = sfxramdata; M16(EBX + ECX) = (u16)EAX; EBX = 0; ECX = sv; }

void zl_membank0r8ram(void) { set_al(WRAM(EBX + ECX)); }
void zl_membank0r8reg(void) { ECX += EBX; zl_mcall(REGPTRA(ECX)); EBX = 0; }
void zl_membank0r8inv(void) { ECX += EBX; set_al(ECX >> 8); }
void zl_membank0r8chip(void)
{
    if (SA1Enable == 1) { zl_orig(ZID_membank0r8chip); return; }
    ECX += EBX;
    if (SFXEnable == 1) { sfxram_r8(); return; }
    set_al(0);
    if (DSP1Type == 2) zl_mcall(ZID_DSP1Read8b);
}
void zl_membank0r8rom(void) { EBX += snesmmap[0]; set_al(M8(EBX + ECX)); EBX = 0; }
void zl_membank0r8romram(void)
{
    ECX = (ECX & ~0xFFFFu) | ((ECX + EBX) & 0xFFFF);
    if (!(ECX & 0x8000)) { set_al(WRAM(ECX)); return; }
    EBX = snesmmap[0]; set_al(M8(EBX + ECX)); EBX = 0;
}
void zl_membank0r16ram(void) { set_ax(WRAM16(EBX + ECX)); }
void zl_membank0r16ramh(void)
{
    ECX += EBX;
    if (ECX == 0x1FFF) { set_al(WRAM(ECX)); set_ah(0); return; }
    set_ax(WRAM16(ECX));
}
void zl_membank0r16reg(void)
{
    ECX += EBX; zl_mcall(REGPTRA(ECX)); ECX++; set_ah(get_al()); zl_mcall(REGPTRA(ECX));
    set_bl(get_al()); ECX--; set_al(get_ah()); set_ah(EBX); EBX = 0;
}
void zl_membank0r16inv(void) { ECX += EBX; set_ax(0x8080); }
void zl_membank0r16chip(void)
{
    if (SA1Enable == 1) { zl_orig(ZID_membank0r16chip); return; }
    ECX += EBX;
    if (SFXEnable == 1) { sfxram_r16(); return; }
    set_ax(0);
    if (DSP1Type == 2) zl_mcall(ZID_DSP1Read16b);
}
void zl_membank0r16rom(void) { EBX += snesmmap[0]; set_ax(M16(EBX + ECX)); EBX = 0; }
void zl_membank0r16romram(void)
{
    ECX = (ECX & ~0xFFFFu) | ((ECX + EBX) & 0xFFFF);
    if (!(ECX & 0x8000)) { set_ax(WRAM16(ECX)); return; }
    EBX = snesmmap[0]; set_ax(M16(EBX + ECX)); EBX = 0;
}
void zl_membank0w8ram(void) { WRAM(EBX + ECX) = (u8)EAX; }
void zl_membank0w8reg(void) { ECX += EBX; zl_mcall(REGPTWA(ECX)); EBX = 0; }
void zl_membank0w8inv(void) { }
void zl_membank0w8chip(void)
{
    if (SA1Enable == 1) { zl_orig(ZID_membank0w8chip); return; }
    ECX += EBX;
    if (SFXEnable == 1) { sfxram_w8(); return; }
    if (DSP1Type == 2) zl_mcall(ZID_DSP1Write8b);
}
void zl_membank0w8rom(void) { }
void zl_membank0w8romram(void)
{
    ECX = (ECX & ~0xFFFFu) | ((ECX + EBX) & 0xFFFF);
    if (!(ECX & 0x8000)) WRAM(ECX) = (u8)EAX;
}
void zl_membank0w16ram(void) { WRAM16(EBX + ECX) = (u16)EAX; }
void zl_membank0w16ramh(void)
{
    ECX += EBX;
    if (ECX == 0x1FFF) { WRAM(ECX) = (u8)EAX; return; }
    WRAM16(ECX) = (u16)EAX;
}
void zl_membank0w16reg(void)
{
    ECX += EBX; zl_mcall(REGPTWA(ECX)); ECX++; set_al(get_ah()); zl_mcall(REGPTWA(ECX)); ECX--; EBX = 0;
}
void zl_membank0w16inv(void) { }
static void w16rom_body(void)
{
    if (SA1Enable == 1) { zl_orig(ZID_membank0w16rom); return; }
    if (SFXEnable == 1) { sfxram_w16(); return; }
    if (DSP1Type == 2) zl_mcall(ZID_DSP1Write16b);
}
void zl_membank0w16chip(void)
{
    if (SA1Enable == 1) { zl_orig(ZID_membank0w16chip); return; }
    ECX += EBX; w16rom_body();
}
void zl_membank0w16rom(void) { w16rom_body(); }
void zl_membank0w16romram(void)
{
    ECX = (ECX & ~0xFFFFu) | ((ECX + EBX) & 0xFFFF);
    if (!(ECX & 0x8000)) WRAM16(ECX) = (u16)EAX;
}

/* ---------------- bank 0 dispatchers (the SA-1 versions stay asm) ---------------- */
void zl_membank0r8(void)
{
    ECX &= 0xFFFF;
    if (SA1Enable == 1) { zl_orig(ZID_membank0r8); return; }
    if (ECX < 0x2000) { set_al(WRAM(ECX)); return; }
    if (ECX & 0x8000) { EBX = snesmmap[0]; set_al(M8(EBX + ECX)); EBX = 0; return; }
    if (ECX > 0x48FF) {
        if (ECX < 0x6000) { set_al(ECX >> 8); return; }
        if (SFXEnable == 1) { sfxram_r8(); return; }
        set_al(0);
        if (DSP1Type == 2) zl_mcall(ZID_DSP1Read8b);
        return;
    }
    zl_mcall(REGPTRA(ECX)); EBX = 0;
}
void zl_membank0r16(void)
{
    ECX &= 0xFFFF;
    if (SA1Enable == 1) { zl_orig(ZID_membank0r16); return; }
    if (ECX < 0x2000) { set_ax(WRAM16(ECX)); return; }
    if (ECX & 0x8000) { EBX = snesmmap[0]; set_ax(M16(EBX + ECX)); EBX = 0; return; }
    if (ECX > 0x48FF) {
        if (ECX < 0x6000) { set_ax(0); return; }
        if (SFXEnable == 1) { sfxram_r16(); return; }
        set_ax(0);
        if (DSP1Type == 2) zl_mcall(ZID_DSP1Read16b);
        return;
    }
    zl_mcall(REGPTRA(ECX)); ECX++; set_ah(get_al()); zl_mcall(REGPTRA(ECX));
    set_bl(get_al()); ECX--; set_al(get_ah()); set_ah(EBX); EBX = 0;
}
void zl_membank0w8(void)
{
    ECX &= 0xFFFF;
    if (SA1Enable == 1) { zl_orig(ZID_membank0w8); return; }
    if (ECX < 0x2000) { WRAM(ECX) = (u8)EAX; return; }
    if (ECX & 0x8000) { if (writeon != 0) { EBX = snesmmap[0]; M8(EBX + ECX) = (u8)EAX; EBX = 0; } return; }
    if (ECX > 0x48FF) {
        if (ECX < 0x6000) return;
        if (SFXEnable == 1) { sfxram_w8(); return; }
        if (DSP1Type == 2) zl_mcall(ZID_DSP1Write8b);
        return;
    }
    zl_mcall(REGPTWA(ECX)); EBX = 0;
}
void zl_membank0w16(void)
{
    ECX &= 0xFFFF;
    if (SA1Enable == 1) { zl_orig(ZID_membank0w16); return; }
    if (ECX < 0x2000) { WRAM16(ECX) = (u16)EAX; return; }
    if (ECX & 0x8000) { if (writeon != 0) { EBX = snesmmap[0]; M16(EBX + ECX) = (u16)EAX; EBX = 0; } return; }
    if (ECX > 0x48FF) {
        if (ECX < 0x6000) return;
        if (SFXEnable == 1) { sfxram_w16(); return; }
        if (DSP1Type == 2) zl_mcall(ZID_DSP1Write16b);
        return;
    }
    zl_mcall(REGPTWA(ECX)); ECX++; set_al(get_ah()); zl_mcall(REGPTWA(ECX)); ECX--; EBX = 0;
}

/* ---------------- 00-3F/80-BF banks: regaccessbank* ---------------- */
#define HIROM_RARE (SPC7110Enable == 1)

/* C4 / OBC1 register RAM: per-address handler tables (C4RamR / C4RamW hold guest code ids) */
#define C4ENT(tab, i) (*(u32 *)(uintptr_t)(EBX + (i) * 4))
extern u32 C4RamR, C4RamW;
static void c4ram_r8(void)
{
    u32 sv = ECX; ECX = (ECX - 0x6000) & 0x1FFF; EBX = C4RamR;
    zl_mcall(C4ENT(R, ECX)); EBX = 0; ECX = sv;
}
static void c4ram_r16(void)
{
    u32 sv = ECX; ECX = (ECX - 0x6000) & 0x1FFF; EBX = C4RamR;
    ECX++; zl_mcall(C4ENT(R, ECX)); ECX--; set_ah(get_al()); zl_mcall(C4ENT(R, ECX));
    EBX = 0; ECX = sv;
}
static void c4ram_w8(void)
{
    u32 sv = ECX; ECX = (ECX - 0x6000) & 0x1FFF; EBX = C4RamW;
    zl_mcall(C4ENT(W, ECX)); EBX = 0; ECX = sv;
}
static void c4ram_w16(void)
{
    u32 sv = ECX, ax = EAX; ECX = (ECX - 0x6000) & 0x1FFF; EBX = C4RamW;
    zl_mcall(C4ENT(W, ECX)); ECX++; set_al(get_ah()); zl_mcall(C4ENT(W, ECX)); EAX = ax; ECX--;
    EBX = 0; ECX = sv;
}
#define C4_PATH (SFXEnable != 1 && (C4Enable == 1 || OBCEnable == 1))
void zl_regaccessbankr8(void)
{
    if (ECX & 0x8000) { EBX = snesmmap[EBX]; set_al(M8(EBX + ECX)); EBX = 0; return; }
    if (ECX < 0x2000) { set_al(WRAM(ECX)); return; }
    if (ECX <= 0x48FF) { zl_mcall(REGPTRA(ECX)); EBX = 0; return; }
    if (ECX < 0x6000) { set_al(ECX >> 8); return; }
    if (HIROM_RARE) { zl_orig(ZID_regaccessbankr8); return; }
    if (C4_PATH) { c4ram_r8(); return; }
    if (SFXEnable == 1) { sfxram_r8(); return; }
    EBX &= 0x7F;
    if ((EBX & 0xFF) < 0x10) { set_al(0); if (DSP1Type == 2) zl_mcall(ZID_DSP1Read8b); EBX = 0; return; }
    if ((EBX & 0xFF) >= 0x30) { u32 sv = ECX; ECX = (ECX - 0x6000) & 0x1FFF; set_bl(EBX - 0x30); EBX <<= 13; ECX += EBX; ECX &= 0xFFFF; sram_r8b(); ECX = sv; return; }
    set_al(0); EBX = 0;
}
void zl_regaccessbankr16(void)
{
    if (ECX & 0x8000) { EBX = snesmmap[EBX]; set_ax(M16(EBX + ECX)); EBX = 0; return; }
    if (ECX < 0x1FFF) { set_ax(WRAM16(ECX)); return; }
    if (ECX <= 0x48FF) {
        zl_mcall(REGPTRA(ECX)); ECX++; set_ah(get_al()); zl_mcall(REGPTRA(ECX));
        set_bl(get_al()); ECX--; set_al(get_ah()); set_ah(EBX); EBX = 0; return;
    }
    if (ECX < 0x6000) { set_al(ECX >> 8); set_ah(ECX >> 8); return; }
    if (HIROM_RARE) { zl_orig(ZID_regaccessbankr16); return; }
    if (C4_PATH) { c4ram_r16(); return; }
    if (SFXEnable == 1) { sfxram_r16(); return; }
    EBX &= 0x7F;
    if ((EBX & 0xFF) < 0x10) { set_ax(0); if (DSP1Type == 2) zl_mcall(ZID_DSP1Read16b); EBX = 0; return; }
    if ((EBX & 0xFF) >= 0x30) { u32 sv = ECX; ECX = (ECX - 0x6000) & 0x1FFF; set_bl(EBX - 0x30); EBX <<= 13; ECX += EBX; ECX &= 0xFFFF; sram_r16b(); ECX = sv; return; }
    set_ax(0); EBX = 0;
}
void zl_regaccessbankw8(void)
{
    if (ECX & 0x8000) { if (writeon != 0) { EBX = snesmmap[EBX]; M8(EBX + ECX) = (u8)EAX; EBX = 0; } return; }
    if (ECX < 0x2000) { WRAM(ECX) = (u8)EAX; return; }
    if (ECX <= 0x48FF) { zl_mcall(REGPTWA(ECX)); EBX = 0; return; }
    if (ECX < 0x6000) return;
    if (HIROM_RARE) { zl_orig(ZID_regaccessbankw8); return; }
    if (C4_PATH) { c4ram_w8(); return; }
    if (SFXEnable == 1) { sfxram_w8(); return; }
    EBX &= 0x7F;
    if ((EBX & 0xFF) < 0x10) { if (DSP1Type == 2) zl_mcall(ZID_DSP1Write8b); EBX = 0; return; }
    if ((EBX & 0xFF) >= 0x30) { u32 sv = ECX; ECX = (ECX - 0x6000) & 0x1FFF; set_bl(EBX - 0x30); EBX <<= 13; ECX += EBX; ECX &= 0xFFFF; sram_w8b(); ECX = sv; return; }
    EBX = 0;
}
void zl_regaccessbankw16(void)
{
    if (ECX & 0x8000) { if (writeon != 0) { EBX = snesmmap[EBX]; M16(EBX + ECX) = (u16)EAX; EBX = 0; } return; }
    if (ECX < 0x1FFF) { WRAM16(ECX) = (u16)EAX; return; }
    if ((ECX & 0xFFFF) <= 0x48FF) { zl_mcall(REGPTWA(ECX)); ECX++; set_al(get_ah()); zl_mcall(REGPTWA(ECX)); ECX--; EBX = 0; return; }
    if (ECX < 0x6000) return;
    if (HIROM_RARE) { zl_orig(ZID_regaccessbankw16); return; }
    if (C4_PATH) { c4ram_w16(); return; }
    if (SFXEnable == 1) { sfxram_w16(); return; }
    EBX &= 0x7F;
    if ((EBX & 0xFF) < 0x10) { if (DSP1Type == 2) zl_mcall(ZID_DSP1Write16b); return; }
    if ((EBX & 0xFF) >= 0x30) { u32 sv = ECX; ECX = (ECX - 0x6000) & 0x1FFF; set_bl(EBX - 0x30); EBX <<= 13; ECX += EBX; ECX &= 0xFFFF; sram_w16b(); ECX = sv; return; }
    set_al(0); EBX = 0;
}

/* ---------------- direct page tables ---------------- */
void zl_UpdateDPage(void)
{
    u32 hi = xd[1];
    DPageR8 = Bank0datr8[hi]; DPageR16 = Bank0datr16[hi]; DPageW8 = Bank0datw8[hi]; DPageW16 = Bank0datw16[hi];
}
