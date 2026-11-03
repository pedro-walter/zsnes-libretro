#define _GNU_SOURCE
/*
 * 65816 opcode bodies, ported from cpu/65816d*.inc + e65816*.inc + address*.inc (ZSNES 1.42).
 *
 * The asm interpreter is threaded: every handler ends with "endloop" (fetch next opcode, charge its
 * cycles, jmp [edi+ebx*4]).  Here, the table entries of ported opcodes point at asm stubs
 * (cpu/cpustub.asm) that save the x86 registers into R, call zs_cpu_op(), reload them and run the
 * very same endloop.  The C code therefore sees the machine state exactly as the asm handler did:
 *   esi = host pointer to the operand bytes, dl = P (I/D/X/M bits valid; N/Z/C/V live in flagnz/flagc/flago),
 *   dh = cycles left in the scanline, edi = opcode table, ebp = SPC700 PC.
 * Memory accesses call the asm bank handlers (memtabler8 ... DPageR8 ...) with the same register
 * conventions, so side effects and cycle charges are identical.
 *
 * Handlers not ported yet keep their asm entry in the tables.
 */
#include "zl.h"
#include "x86dec.h"
typedef int8_t s8; typedef int16_t s16; typedef int32_t s32;
#define R LG

/* ---- asm state ---- */
extern u32 xa, xdb, xpb, xs, xd, xx, xy, flagnz, flago, flagc, initaddrl, wramdata;
extern u16 xpc, stackand, stackor;
extern u8 xe, xp, intrset, doirqnext, curexecstate, nmistatus, curnmi;
extern u32 memtabler8[256], memtabler16[256], memtablew8[256], memtablew16[256];
extern u32 DPageR8, DPageR16, DPageW8, DPageW16;
extern u32 Bank0datr8[256], Bank0datr16[256], Bank0datw8[256], Bank0datw16[256];
extern u32 tableadb[256], tableadc[256];
extern u32 snesmmap[256], snesmap2[256];
extern u8 regaccessbankr8[], dmadata[];
extern u16 brkv, brkv8, copv, copv8;
extern u8 xirqb;

#define XA8 (*(u8 *)&xa)
#define XA16 (*(u16 *)&xa)
#define XX8 (*(u8 *)&xx)
#define XX16 (*(u16 *)&xx)
#define XY8 (*(u8 *)&xy)
#define XY16 (*(u16 *)&xy)

#define DL (*(u8 *)&R.edx)
#define DH (((u8 *)&R.edx)[1])

/* ---- register helpers (partial register writes, like the asm) ---- */
static inline void set_cx(u32 v) { R.ecx = (R.ecx & 0xFFFF0000u) | (v & 0xFFFF); }
static inline void set_bl(u32 v) { R.ebx = (R.ebx & 0xFFFFFF00u) | (v & 0xFF); }
static inline void set_al(u32 v) { R.eax = (R.eax & 0xFFFFFF00u) | (v & 0xFF); }
static inline void set_ax(u32 v) { R.eax = (R.eax & 0xFFFF0000u) | (v & 0xFFFF); }

/* ---- flags ---- */
static inline void nz8(u8 v) { flagnz = (u32)v << 8; }
/* "mov cx,ax; mov [flagnz],ecx": upper half comes from whatever ecx holds */
static inline void nz16(u16 v) { set_cx(v); flagnz = R.ecx; }

static inline void makedl(void)
{
    DL &= 0x3C;
    if (flagnz & 0x18000) DL |= 0x80;
    if (!(flagnz & 0xFFFF)) DL |= 0x02;
    if (flagc & 0xFF) DL |= 0x01;
    if (flago & 0xFF) DL |= 0x40;
}
static inline void restoredl(void)
{
    flagnz = 0;
    if (DL & 0x80) flagnz |= 0x10000;
    if (!(DL & 0x02)) flagnz |= 1;
    flagc = (DL & 1) ? 0xFF : 0;
    flago = (DL & 0x40) ? 0xFF : 0;
}

/* ---- memory access through the asm bank handlers ---- */
static inline void mem_tab(u32 *tab, u32 off, u32 bank)
{
    set_cx(off);
    set_bl(bank);
    zl_mcall(tab[R.ebx]);
}
static inline void mem_fn(u32 fn) { zl_mcall(fn); }

/* addressing modes */
enum { A_I, A_a, A_al, A_d, A_A, A_BdBCy, A_LdLCy, A_BdCxB, A_dCx, A_dCy, A_aCx, A_aCy, A_alCx, A_BdB, A_LdL, A_dCs, A_BdCsBCy };

static inline u8 *PC(void) { return (u8 *)(uintptr_t)R.esi; }
static inline u16 rd16h(const u8 *p) { return *(const u16 *)p; }

/*
 * Resolve an effective address and perform the access.
 *   wide: 0/1 ; write: 0=read (result in R.eax), 1=write (data = value) ; adv: advance esi past operands
 */
static void am_access(int mode, int wide, int write, u32 data, int adv)
{
    u8 *p = PC();
    u16 d16 = (u16)data;
    u32 *tab = write ? (wide ? memtablew16 : memtablew8) : (wide ? memtabler16 : memtabler8);
    u32 b0 = write ? (wide ? ZID_membank0w16 : (void *)ZID_membank0w8) : (wide ? ZID_membank0r16 : (void *)ZID_membank0r8);
    u32 dpg = write ? (wide ? DPageW16 : DPageW8) : (wide ? DPageR16 : DPageR8);
    u32 cx, bl;
    u16 t;
#define SETDATA() do { if (write) set_ax(d16); } while (0)
    switch (mode) {
    case A_I:
        if (wide) R.eax = *(u32 *)p; else set_al(*p);
        if (adv) R.esi += wide ? 2 : 1;
        return;
    case A_a:
        set_cx(rd16h(p)); set_bl(xdb); if (adv) R.esi += 2;
        SETDATA(); zl_mcall(tab[R.ebx]); return;
    case A_al:
        set_cx(rd16h(p)); set_bl(p[2]); if (adv) R.esi += 3;
        SETDATA(); zl_mcall(tab[R.ebx]); return;
    case A_d:
        set_bl(p[0]); R.ecx = xd; if (adv) R.esi += 1;
        SETDATA(); zl_mcall(dpg); return;
    case A_A:
        if (write) { if (wide) XA16 = d16; else XA8 = (u8)d16; }
        else { if (wide) R.eax = xa; else set_al(XA8); }
        return;
    case A_BdBCy:
        set_bl(p[0]); R.ecx = xd; if (adv) R.esi += 1;
        zl_mcall(DPageR16);
        set_cx(R.eax); set_bl(xdb);
        t = (u16)R.ecx;
        { u32 s = t + (u16)xy; set_cx(s); if (s > 0xFFFF) set_bl(R.ebx + 1); }
        SETDATA(); zl_mcall(tab[R.ebx]); return;
    case A_LdLCy:
        R.ecx = xd; set_bl(p[0]); set_cx((u16)R.ecx + (u16)R.ebx); if (adv) R.esi += 1;
        { u16 save = (u16)R.ecx;
          zl_mcall(ZID_membank0r16);
          set_cx(save); }
        set_cx((u16)R.ecx + 2);
        { u16 axs = (u16)R.eax;
          zl_mcall(ZID_membank0r8);
          set_bl(R.eax);
          set_ax(axs); }
        set_cx(R.eax);
        { u32 s = (u16)R.ecx + (u16)xy; set_cx(s); if (s > 0xFFFF) set_bl(R.ebx + 1); }
        if (write) set_ax(d16);
        zl_mcall(tab[R.ebx]); return;
    case A_BdCxB:
        R.ecx = xd; set_bl(p[0]); set_cx((u16)R.ecx + (u16)R.ebx); if (adv) R.esi += 1;
        set_cx((u16)R.ecx + (u16)xx);
        zl_mcall(ZID_membank0r16);
        set_cx(R.eax); set_bl(xdb);
        SETDATA(); zl_mcall(tab[R.ebx]); return;
    case A_dCx:
    case A_dCy:
        R.ecx = xd; set_bl(p[0]); set_cx((u16)R.ecx + (u16)R.ebx); if (adv) R.esi += 1;
        set_cx((u16)R.ecx + (u16)(mode == A_dCx ? xx : xy));
        SETDATA(); zl_mcall(b0); return;
    case A_aCx:
    case A_aCy:
        set_cx(rd16h(p)); set_bl(xdb); if (adv) R.esi += 2;
        { u32 s = (u16)R.ecx + (u16)(mode == A_aCx ? xx : xy); set_cx(s); if (s > 0xFFFF) set_bl(R.ebx + 1); }
        SETDATA(); zl_mcall(tab[R.ebx]); return;
    case A_alCx:
        set_cx(rd16h(p)); set_bl(p[2]); if (adv) R.esi += 3;
        { u32 s = (u16)R.ecx + (u16)xx; set_cx(s); if (s > 0xFFFF) set_bl(R.ebx + 1); }
        SETDATA(); zl_mcall(tab[R.ebx]); return;
    case A_BdB:
        set_bl(p[0]); R.ecx = xd; if (adv) R.esi += 1;
        zl_mcall(DPageR16);
        set_cx(R.eax); set_bl(xdb);
        SETDATA(); zl_mcall(tab[R.ebx]); return;
    case A_LdL:
        set_bl(p[0]); R.ecx = xd; if (adv) R.esi += 1;
        set_cx((u16)R.ecx + (u16)R.ebx);
        { u16 save = (u16)R.ecx;
          zl_mcall(ZID_membank0r16);
          set_cx(save); }
        set_cx((u16)R.ecx + 2);
        { u16 axs = (u16)R.eax;
          zl_mcall(ZID_membank0r8);
          set_bl(R.eax);
          set_ax(axs); }
        set_cx(R.eax);
        SETDATA(); zl_mcall(tab[R.ebx]); return;
    case A_dCs:
        set_bl(p[0]); set_cx(xs); if (adv) R.esi += 1;
        set_cx((u16)R.ecx + (u16)R.ebx);
        SETDATA(); zl_mcall(b0); return;
    case A_BdCsBCy:
        set_bl(p[0]); set_cx(xs); if (adv) R.esi += 1;
        set_cx((u16)R.ecx + (u16)R.ebx);
        zl_mcall(ZID_membank0r16);
        set_cx(R.eax); set_bl(xdb);
        { u32 s = (u16)R.ecx + (u16)xy; set_cx(s); if (s > 0xFFFF) set_bl(R.ebx + 1); }
        SETDATA(); zl_mcall(tab[R.ebx]); return;
    }
}
#undef SETDATA

static inline void rd(int mode, int wide, int adv) { am_access(mode, wide, 0, 0, adv); }
static inline void wr(int mode, int wide, u32 data) { am_access(mode, wide, 1, data, 1); }

/* ---- stack ---- */
static inline void push8(u8 v)
{
    R.eax = wramdata;
    set_cx(xs);
    set_al(v);
    zl_mcall(ZID_membank0w8);
    set_cx(((u16)R.ecx - 1) | stackor);
    xs = (xs & 0xFFFF0000u) | (u16)R.ecx;
}
/* PUSHMacro16b: high byte then low byte, a single xs update at the end */
static inline void push16(u8 hi, u8 lo)
{
    set_cx(xs);
    set_al(hi);
    zl_mcall(ZID_membank0w8);
    set_cx(((u16)R.ecx - 1) | stackor);
    set_al(lo);
    zl_mcall(ZID_membank0w8);
    set_cx(((u16)R.ecx - 1) | stackor);
    xs = (xs & 0xFFFF0000u) | (u16)R.ecx;
}
static inline u8 pop8_core(void)
{
    R.eax = wramdata;
    set_cx(((u16)xs + 1) & stackand);
    zl_mcall(ZID_membank0r8);
    return (u8)R.eax;
}

static void update_dpage(void)
{
    u32 hi = (xd >> 8) & 0xFF;
    DPageR8 = Bank0datr8[hi];
    DPageR16 = Bank0datr16[hi];
    DPageW8 = Bank0datw8[hi];
    DPageW16 = Bank0datw16[hi];
}

#ifdef SA1
extern u32 SA1tablead[256];
static inline void switch_table(int var) { (void)var; set_bl(DL); R.ebx &= 0xFF; R.edi = SA1tablead[DL]; }
#else
static inline void switch_table(int var) { set_bl(DL); R.ebx &= 0xFF; R.edi = (var ? tableadc[DL] : tableadb[DL]); }
#endif


static inline void branch(int taken)
{
    if (taken) { s8 d = *(s8 *)PC(); R.esi += (u32)(s32)d; }
    R.esi += 1;
}


/* ---- PC reload helpers (esi := map[bank] + offset, initaddrl := map[bank]) ---- */
/* tail used by JMP $4C / JSR $20 / RTI: lower-half addresses >= $4300 get the DMA register check */
static int pc_jsl;      /* SA-1 JSL: low-half addresses above $2000 in bank 0 run from IRAM */
static void pc_load(int dmacheck)
{
    u32 eax = R.eax, ebx = R.ebx;
    if (eax & 0x8000) {
        R.esi = snesmmap[ebx]; initaddrl = R.esi; R.esi += eax;
        return;
    }
#ifdef SA1
    dmacheck = 0;
    if (pc_jsl && ebx == 0 && eax > 0x2000) { R.esi = (u32)ZO_IRAM - 0x3000; initaddrl = R.esi; R.esi += eax; return; }
#endif
    R.esi = snesmap2[ebx];
    if (dmacheck && eax >= 0x4300) {
        if (memtabler8[ebx] == ZID_regaccessbankr8) R.esi = (u32)(uintptr_t)dmadata - 0x4300;
    }
    initaddrl = R.esi;
    R.esi += eax;
}
static inline void setxhi(void) { ((u8 *)&xx)[1] = 0; ((u8 *)&xy)[1] = 0; }
static inline void stk_dec(u32 *cx) { *cx = ((u16)*cx - 1) | stackor; }
/* membank0w8 with al, cx = stack pointer; leaves cx decremented (|stackor) */
static inline void pushb_cx(u8 v) { set_al(v); zl_mcall(ZID_membank0w8); set_cx(((u16)R.ecx - 1) | stackor); }
static inline u8 popb_cx(void) { set_cx(((u16)R.ecx + 1) & stackand); zl_mcall(ZID_membank0r8); return (u8)R.eax; }
/* base of the mapping the CPU is currently fetching from (uses xpc/xpb like the BRK/BRL/PER macros) */
static u32 cur_base(void)
{
    set_bl(xpb);
    return ((u16)xpc & 0x8000) ? snesmmap[R.ebx] : snesmap2[R.ebx];
}
static void irq_entry(u16 vec_n, u16 vec_e, int brk_emul_d)
{
    u8 *wr_ = (u8 *)(uintptr_t)wramdata;
    u32 base, bx;
    R.esi += 1;                                          /* inc esi (signature byte) */
#ifndef SA1
    R.ebx = 0;
#endif
    base = cur_base();
    R.ebx = R.esi; R.ebx -= base; xpc = (u16)R.ebx;      /* mov ebx,esi; sub ebx,eax; mov [xpc],bx */
    if (!(xe & 1)) {
        R.eax = wramdata; R.ebx = 0; set_cx(0);
        bx = (u16)xs;
        wr_[bx] = (u8)xpb;
        bx = (((bx - 1) & 0xFFFF) & stackand) | stackor;
        wr_[bx] = (u8)(xpc >> 8);
        bx = (((bx - 1) & 0xFFFF) & stackand) | stackor;
        wr_[bx] = (u8)xpc;
        bx = (((bx - 1) & 0xFFFF) & stackand) | stackor;
        makedl(); wr_[bx] = DL;
        bx = (((bx - 1) & 0xFFFF) & stackand) | stackor;
        xs = (xs & 0xFFFF0000u) | (u16)bx;
        R.ebx = (u8)xirqb; xpb = (xpb & ~0xFFu) | (u8)xirqb;
        R.eax = vec_n; xpc = vec_n;
    } else {
        R.eax = wramdata; R.ebx = 0;
        bx = (u16)xs;
        wr_[bx] = (u8)(xpc >> 8);
        bx = (((bx - 1) & 0xFFFF) & stackand) | stackor;
        wr_[bx] = (u8)xpc;
        bx = (((bx - 1) & 0xFFFF) & stackand) | stackor;
        makedl(); wr_[bx] = DL;
        bx = (((bx - 1) & 0xFFFF) & stackand) | stackor;
        xs = (xs & 0xFFFF0000u) | (u16)bx;
        R.ebx = (u8)xpb;
        R.eax = vec_e; xpc = vec_e;
    }
#ifdef SA1
    DL = (DL & 0xF3) | 0x04;
#else
    DL = (DL & 0xF3) | ((brk_emul_d && (xe & 1)) ? 0x0C : 0x04);   /* BRK in emulation mode also sets D */
#endif
    pc_load(0);
}

/* 8-bit / 16-bit read-modify-write ALU kernels: value in/out through *v, flags like the asm */
static inline void shl_flags8(u8 *v, int carry_out) { nz8(*v); flagc = carry_out ? 0xFF : 0; }
static inline void shl_flags16(u16 *v, int carry_out) { nz16(*v); flagc = carry_out ? 0xFF : 0; }

/* returns 1 if handled (ADC/SBC with D set are never routed here: the D tables keep the asm handlers) */
static int zs_cpu_op_inner(u32 op, u32 var);
static int zs_cpu_op(u32 op, u32 var) { return zs_cpu_op_inner(op, var); }
static int zs_cpu_op_inner(u32 op, u32 var)
{
    int m8 = (DL & 0x20) != 0, x8 = (DL & 0x10) != 0;
    int grp = (op >> 5) & 7, lo = op & 0x1F;
    int mode = -1;
    int w = !m8;

    /* ---- the 8 regular ALU/load/store groups (rows x1,x3,x5,x7,x9,xD,xF + 11,12,13,15,17,19,1D,1F) ---- */
    if ((op & 3) == 1 || (op & 3) == 3 || lo == 0x12 || (op & 0x0F) == 0x0D || (op & 0x0F) == 0x0F || (op & 0x0F) == 0x09) {
        switch (lo) {
        case 0x01: mode = A_BdCxB; break;
        case 0x03: mode = A_dCs; break;
        case 0x05: mode = A_d; break;
        case 0x07: mode = A_LdL; break;
        case 0x09: mode = A_I; break;
        case 0x0D: mode = A_a; break;
        case 0x0F: mode = A_al; break;
        case 0x11: mode = A_BdBCy; break;
        case 0x12: mode = A_BdB; break;
        case 0x13: mode = A_BdCsBCy; break;
        case 0x15: mode = A_dCx; break;
        case 0x17: mode = A_LdLCy; break;
        case 0x19: mode = A_aCy; break;
        case 0x1D: mode = A_aCx; break;
        case 0x1F: mode = A_alCx; break;
        }
        if (mode >= 0 && !(grp == 4 && mode == A_I)) {
            if (grp == 4) { wr(mode, w, w ? (u16)xa : XA8); return 1; }       /* STA */
            rd(mode, w, 1);
            switch (grp) {
            case 0:
                if (!w) { u8 a = (u8)R.eax | XA8; nz8(a); XA8 = a; }
                else { u16 a = (u16)(R.eax | XA16); XA16 = a; nz16(a); }
                return 1;
            case 1:
                if (!w) { u8 a = (u8)R.eax & XA8; nz8(a); XA8 = a; }
                else { u16 a = (u16)(R.eax & xa); XA16 = a; nz16(a); }
                return 1;
            case 2:
                if (!w) { u8 a = (u8)R.eax ^ XA8; nz8(a); XA8 = a; }
                else { u16 a = (u16)(R.eax ^ xa); XA16 = a; nz16(a); }
                return 1;
            case 5:
                if (!w) { XA8 = (u8)R.eax; nz8((u8)R.eax); }
                else { XA16 = (u16)R.eax; nz16((u16)R.eax); }
                return 1;
            case 6:
                if (!w) { u8 c = XA8, v = (u8)R.eax; nz8((u8)(c - v)); flagc = (c < v) ? 0 : 0xFF; }
                else { u16 c = XA16, v = (u16)R.eax; nz16((u16)(c - v)); flagc = (c < v) ? 0 : 0xFF; }
                return 1;
            case 3: if (DL & 0x08) {                           /* ADC (decimal): x86 ADC+DAA, modelled portably */
                int cf = (flagc & 0xFF) != 0, of, af;
                u8 a_lo = XA8, a_hi = ((u8 *)&xa)[1], vl = (u8)R.eax, vh = (u8)(R.eax >> 8);
                { unsigned t = (unsigned)a_lo + vl + (unsigned)cf; u8 r = (u8)t; cf = t > 0xFF; af = ((a_lo ^ vl ^ r) & 0x10) != 0; a_lo = x86_daa(r, af, &cf, &of); }
                if (!w) {
                    XA8 = a_lo; flagnz = (u32)a_lo << 8;
                } else {
                    { unsigned t = (unsigned)a_hi + vh + (unsigned)cf; u8 r = (u8)t; cf = t > 0xFF; af = ((a_hi ^ vh ^ r) & 0x10) != 0; a_hi = x86_daa(r, af, &cf, &of); }
                    XA8 = a_lo; ((u8 *)&xa)[1] = a_hi; set_cx(XA16); flagnz = R.ecx;
                }
                flago = (flago & ~0xFFu) | (u32)of; flagc = cf ? 0xFF : 0;
                return 1;
            } else {                                          /* ADC (binary) */
                u32 cin = (flagc & 0x80) ? 1 : 0, o, k, sres;
                if (!w) {
                    u8 a = XA8, v = (u8)R.eax; sres = (u32)a + v + cin;
                    o = ((~(a ^ v) & (a ^ sres)) & 0x80) != 0; k = sres > 0xFF;
                    XA8 = (u8)sres; flagnz = (u32)(u8)sres << 8;
                } else {
                    u16 a = XA16, v = (u16)R.eax; sres = (u32)a + v + cin;
                    o = ((~(a ^ v) & (a ^ sres)) & 0x8000) != 0; k = sres > 0xFFFF;
                    XA16 = (u16)sres; set_cx(XA16); flagnz = R.ecx;
                }
                flago = (flago & ~0xFFu) | o; flagc = k ? 0xFF : 0;
                return 1;
            }
            case 7: if (DL & 0x08) {                           /* SBC (decimal): x86 SBB+DAS, modelled portably */
                int bin = (flagc & 0xFF) ? 0 : 1, cf, of, af;
                u8 a_lo = XA8, a_hi = ((u8 *)&xa)[1], vl = (u8)R.eax, vh = (u8)(R.eax >> 8);
                { u8 r = (u8)(a_lo - vl - bin); cf = (unsigned)a_lo < (unsigned)vl + bin; af = ((a_lo ^ vl ^ r) & 0x10) != 0; a_lo = x86_das(r, af, &cf, &of); }
                if (!w) {
                    XA8 = a_lo; flagnz = (u32)a_lo << 8; flago = (flago & ~0xFFu) | (u32)of; flagc = cf ? 0 : 0xFF;
                } else {
                    { u8 r = (u8)(a_hi - vh - cf); int c2 = (unsigned)a_hi < (unsigned)vh + cf; af = ((a_hi ^ vh ^ r) & 0x10) != 0; cf = c2; a_hi = x86_das(r, af, &cf, &of); }
                    cf = !cf;   /* the asm ends the 16-bit sequence with cmc */
                    XA8 = a_lo; ((u8 *)&xa)[1] = a_hi; set_cx(XA16); flagnz = R.ecx; flago = (flago & ~0xFFu) | (u32)of; flagc = cf ? 0xFF : 0;
                }
                return 1;
            } else {                                          /* SBC (binary) */
                u32 bin = (flagc & 0xFF) ? 0 : 1, o, k, sres;
                if (!w) {
                    u8 a = XA8, v = (u8)R.eax; sres = (u32)a - v - bin;
                    o = (((a ^ v) & (a ^ sres)) & 0x80) != 0; k = (u32)a < (u32)v + bin;
                    XA8 = (u8)sres; flagnz = (u32)(u8)sres << 8;
                } else {
                    u16 a = XA16, v = (u16)R.eax; sres = (u32)a - v - bin;
                    o = (((a ^ v) & (a ^ sres)) & 0x8000) != 0; k = (u32)a < (u32)v + bin;
                    XA16 = (u16)sres; set_cx(XA16); flagnz = R.ecx;
                }
                flago = (flago & ~0xFFu) | o; flagc = k ? 0 : 0xFF;
                return 1;
            }
            }
        }
    }

    switch (op) {
    /* ---- shifts / rotates / inc / dec on memory or A ---- */
    case 0x06: case 0x0A: case 0x0E: case 0x16: case 0x1E:       /* ASL */
    case 0x26: case 0x2A: case 0x2E: case 0x36: case 0x3E:       /* ROL */
    case 0x46: case 0x4A: case 0x4E: case 0x56: case 0x5E:       /* LSR */
    case 0x66: case 0x6A: case 0x6E: case 0x76: case 0x7E:       /* ROR */
    case 0x3A: case 0xC6: case 0xCE: case 0xD6: case 0xDE:       /* DEC */
    case 0x1A: case 0xE6: case 0xEE: case 0xF6: case 0xFE: {     /* INC */
        int kind;   /* 0 ASL 1 ROL 2 LSR 3 ROR 4 DEC 5 INC */
        switch (op & 0xE0) {
        case 0x00: kind = (op == 0x1A) ? 5 : 0; break;
        case 0x20: kind = (op == 0x3A) ? 4 : 1; break;
        case 0x40: kind = 2; break;
        case 0x60: kind = 3; break;
        case 0xC0: kind = 4; break;
        default:   kind = 5; break;
        }
        switch (op & 0x1F) {
        case 0x06: mode = A_d; break;
        case 0x0A: case 0x1A: mode = A_A; break;
        case 0x0E: mode = A_a; break;
        case 0x16: mode = A_dCx; break;
        case 0x1E: mode = A_aCx; break;
        }
#ifdef SA1
        int w16rmw = (op == 0xD6 && !w);     /* SA1COpD6m8 uses addr_dCx_16brni/16bw: reads and writes 16 bits */
#else
        const int w16rmw = 0;
#endif
        rd(mode, w || w16rmw, 0);                                /* "ni": esi not advanced */
        if (!w) {
            u8 v = (u8)R.eax; u32 cin = (flagc & 0x80) ? 1 : 0, cout = 0;
            switch (kind) {
            case 0: cout = v >> 7; v = (u8)(v << 1); break;
            case 1: cout = v >> 7; v = (u8)((v << 1) | cin); break;
            case 2: cout = v & 1; v >>= 1; break;
            case 3: cout = v & 1; v = (u8)((v >> 1) | (cin << 7)); break;
            case 4: v--; break;
            case 5: v++; break;
            }
            if (kind < 4) { flagnz = (u32)v << 8; flagc = cout ? 0xFF : 0; } else flagnz = (u32)v << 8;
            set_al(v);
        } else {
            u16 v = (u16)R.eax; u32 cin = (flagc & 0x80) ? 1 : 0, cout = 0;
            switch (kind) {
            case 0: cout = v >> 15; v = (u16)(v << 1); break;
            case 1: cout = v >> 15; v = (u16)((v << 1) | cin); break;
            case 2: cout = v & 1; v >>= 1; break;
            case 3: cout = v & 1; v = (u16)((v >> 1) | (cin << 15)); break;
            case 4: v--; break;
            case 5: v++; break;
            }
            set_ax(v);
            nz16(v);
            if (kind < 4) flagc = cout ? 0xFF : 0;
        }
        wr(mode, w || w16rmw, R.eax);
        return 1;
    }
    /* ---- TSB / TRB ---- */
    case 0x04: case 0x0C: case 0x14: case 0x1C: {
        int trb = (op & 0x10) != 0;
        mode = (op & 0x08) ? A_a : A_d;
        rd(mode, w, 0);
        if (flagnz & 0x18000) flagnz |= 0x10000;
        if (!w) {
            u8 a = XA8, v = (u8)R.eax;
            flagnz = (flagnz & 0xFFFF0000u) | ((v & a) ? 1 : 0);
            v = trb ? (u8)(v & ~a) : (u8)(v | a);
            set_al(v);
        } else {
            u16 a = XA16, v = (u16)R.eax;
            flagnz = (flagnz & 0xFFFF0000u) | ((v & a) ? 1 : 0);
            v = trb ? (u16)(v & ~a) : (u16)(v | a);
            set_ax(v);
        }
        wr(mode, w, R.eax);
        return 1;
    }
    /* ---- BIT ---- */
    case 0x24: case 0x2C: case 0x34: case 0x3C: {
        mode = (op == 0x24) ? A_d : (op == 0x2C) ? A_a : (op == 0x34) ? A_dCx : A_aCx;
        rd(mode, w, 1);
        if (!w) {
            u8 v = (u8)R.eax;
            flagnz = (v & 0x80) ? 0x10000 : 0;
            flago = (v & 0x40) ? 1 : 0;
            flagnz = (flagnz & 0xFFFF0000u) | ((XA8 & v) ? 1 : 0);
        } else {
            u16 v = (u16)R.eax;
            flagnz = (v & 0x8000) ? 0x10000 : 0;
            flago = (v & 0x4000) ? 1 : 0;
            flagnz = (flagnz & 0xFFFF0000u) | ((XA16 & v) ? 1 : 0);
        }
        return 1;
    }
    case 0x89: {
        if (!w) set_al(*PC()); else set_ax(rd16h(PC()));
        if (flagnz & 0x18000) flagnz |= 0x10000;
        R.esi += w ? 2 : 1;
        flagnz = (flagnz & 0xFFFF0000u) | ((w ? (XA16 & (u16)R.eax) : (XA8 & (u8)R.eax)) ? 1 : 0);
        return 1;
    }
    /* ---- STZ ---- */
    case 0x64: case 0x74: case 0x9C: case 0x9E:
        mode = (op == 0x64) ? A_d : (op == 0x74) ? A_dCx : (op == 0x9C) ? A_a : A_aCx;
        if (!w) set_al(0); else R.eax = 0;
        wr(mode, w, 0);
        return 1;
    /* ---- index register loads / stores / compares ---- */
    case 0xA0: case 0xA4: case 0xAC: case 0xB4: case 0xBC:       /* LDY */
    case 0xA2: case 0xA6: case 0xAE: case 0xB6: case 0xBE:       /* LDX */
    case 0xC0: case 0xC4: case 0xCC: case 0xE0: case 0xE4: case 0xEC:   /* CPY / CPX */
    case 0x84: case 0x8C: case 0x94: case 0x86: case 0x8E: case 0x96: { /* STY / STX */
        int xw = !x8;
        switch (op) {
        case 0xA0: case 0xA2: case 0xC0: case 0xE0: mode = A_I; break;
        case 0xA4: case 0xA6: case 0xC4: case 0xE4: case 0x84: case 0x86: mode = A_d; break;
        case 0xAC: case 0xAE: case 0xCC: case 0xEC: case 0x8C: case 0x8E: mode = A_a; break;
        case 0xB4: case 0x94: mode = A_dCx; break;
        case 0xB6: case 0x96: mode = A_dCy; break;
        case 0xBC: mode = A_aCx; break;
        case 0xBE: mode = A_aCy; break;
        }
        if (op == 0x84 || op == 0x8C || op == 0x94) { wr(mode, xw, xw ? XY16 : XY8); return 1; }
        if (op == 0x86 || op == 0x8E || op == 0x96) { wr(mode, xw, xw ? XX16 : XX8); return 1; }
        rd(mode, xw, 1);
        if (op == 0xA0 || op == 0xA4 || op == 0xAC || op == 0xB4 || op == 0xBC) {
            if (!xw) { XY8 = (u8)R.eax; nz8((u8)R.eax); } else { XY16 = (u16)R.eax; nz16((u16)R.eax); }
        } else if (op == 0xA2 || op == 0xA6 || op == 0xAE || op == 0xB6 || op == 0xBE) {
            if (!xw) { XX8 = (u8)R.eax; nz8((u8)R.eax); } else { XX16 = (u16)R.eax; nz16((u16)R.eax); }
        } else {
            u16 c = (op >= 0xE0) ? (xw ? XX16 : XX8) : (xw ? XY16 : XY8), v = xw ? (u16)R.eax : (u8)R.eax;
            if (!xw) { nz8((u8)(c - v)); flagc = ((u8)c < (u8)v) ? 0 : 0xFF; }
            else { nz16((u16)(c - v)); flagc = (c < v) ? 0 : 0xFF; }
        }
        return 1;
    }
    /* ---- inc/dec index registers ---- */
    case 0x88: if (x8) { XY8--; nz8(XY8); } else { XY16--; nz16(XY16); } return 1;
    case 0xC8: if (x8) { XY8++; nz8(XY8); } else { XY16++; nz16(XY16); } return 1;
    case 0xCA: if (x8) { XX8--; nz8(XX8); } else { XX16--; nz16(XX16); } return 1;
    case 0xE8: if (x8) { XX8++; nz8(XX8); } else { XX16++; nz16(XX16); } return 1;
    /* ---- transfers ---- */
    case 0x8A: if (m8) { XA8 = XX8; nz8(XA8); } else { XA16 = XX16; nz16(XA16); } return 1;
    case 0x98: if (m8) { XA8 = XY8; nz8(XA8); } else { XA16 = XY16; nz16(XA16); } return 1;
    case 0xA8: if (x8) { XY8 = XA8; nz8(XY8); } else { XY16 = XA16; nz16(XY16); } return 1;
    case 0xAA: if (x8) { XX8 = XA8; nz8(XX8); } else { XX16 = XA16; nz16(XX16); } return 1;
    case 0x9B: if (x8) { XY8 = XX8; nz8(XY8); } else { XY16 = XX16; nz16(XY16); } return 1;
    case 0xBB: if (x8) { XX8 = XY8; nz8(XX8); } else { XX16 = XY16; nz16(XX16); } return 1;
    case 0xBA: if (x8) { XX8 = (u8)xs; nz8(XX8); } else { XX16 = (u16)xs; nz16(XX16); } return 1;
#ifdef SA1
    case 0x1B: *(u16 *)&xs = XA16; return 1;
#else
    case 0x1B: if (xe & 1) *(u8 *)&xs = XA8; else *(u16 *)&xs = XA16; return 1;
#endif
    case 0x3B: *(u16 *)&xa = (u16)xs; nz16(XA16); return 1;
    case 0x5B: *(u16 *)&xd = XA16; update_dpage(); nz16(XA16); return 1;
    case 0x7B: *(u16 *)&xa = (u16)xd; nz16(XA16); return 1;
    case 0x9A: *(u16 *)&xs = XX16; if (xe & 1) ((u8 *)&xs)[1] = 1; return 1;
    case 0xEB: { u8 lo = XA8, hi = ((u8 *)&xa)[1]; XA8 = hi; ((u8 *)&xa)[1] = lo; nz8(hi); return 1; }
    /* ---- flag ops ---- */
    case 0x18: flagc = 0; return 1;
    case 0x38: flagc = 0xFF; return 1;
    case 0xB8: flago = 0; return 1;
    case 0x58: DL &= 0xFB; return 1;
    case 0x78: DL |= 0x04; return 1;
    case 0xD8: DL &= 0xF7; switch_table(var); return 1;
    case 0xF8: DL |= 0x08; switch_table(var); return 1;
    case 0xEA: return 1;
    case 0x42: R.esi += 1; return 1;
    case 0xDB: R.esi -= 1; return 1;
    /* ---- branches ---- */
    case 0x10: branch(!(flagnz & 0x18000)); return 1;
    case 0x30: branch((flagnz & 0x18000) != 0); return 1;
    case 0x50: branch(!(flago & 0xFF)); return 1;
    case 0x70: branch((flago & 0xFF) != 0); return 1;
    case 0x90: branch(!(flagc & 1)); return 1;
    case 0xB0: branch((flagc & 1) != 0); return 1;
    case 0xD0: branch((flagnz & 0xFFFF) != 0); return 1;
    case 0xF0: branch((flagnz & 0xFFFF) == 0); return 1;
    case 0x80: { s8 d = *(s8 *)PC(); R.esi += 1; R.esi += (u32)(s32)d; return 1; }

    /* ---- control flow ---- */
    case 0x4C: R.eax = rd16h(PC()); set_bl(xpb); xpc = (u16)R.eax; pc_load(1); return 1;
    case 0x5C: R.eax = rd16h(PC()); set_bl(PC()[2]); xpb = (xpb & ~0xFFu) | (u8)R.ebx; xpc = (u16)R.eax; pc_load(0); return 1;
    case 0x6C:
        set_cx(rd16h(PC())); R.eax = 0; zl_mcall(ZID_membank0r16);
        xpc = (u16)R.eax; set_bl(xpb); pc_load(0); return 1;
    case 0x7C:
        set_cx(rd16h(PC())); R.eax = 0; set_cx((u16)R.ecx + (u16)xx); set_bl(xpb);
        zl_mcall(memtabler16[R.ebx]);
        xpc = (u16)R.eax; set_bl(xpb); pc_load(0); return 1;
    case 0xDC: {
        u16 axs;
        set_cx(rd16h(PC())); R.eax = 0; zl_mcall(ZID_membank0r16);
        set_cx((u16)R.ecx + 2); axs = (u16)R.eax;
        zl_mcall(ZID_membank0r8);
        set_bl(R.eax); set_ax(axs);
        xpc = (u16)R.eax; xpb = (xpb & ~0xFFu) | (u8)R.ebx;
        pc_load(0); return 1;
    }
    case 0x20: {
        u16 off;
        R.ebx = R.esi - initaddrl; R.ebx = (R.ebx & 0xFFFF0000u) | (u16)(R.ebx + 1); xpc = (u16)R.ebx;
        set_cx(xs);
        pushb_cx((u8)(xpc >> 8));
        pushb_cx((u8)xpc);
        R.eax = 0;
        xs = (xs & 0xFFFF0000u) | (u16)R.ecx;
        R.eax = rd16h(PC()); R.ebx &= ~0xFF00u; xpc = (u16)R.eax; set_bl(xpb);
        (void)off; pc_load(1); return 1;
    }
    case 0x22: {
        R.ebx = R.esi - initaddrl; R.ebx = (R.ebx & 0xFFFF0000u) | (u16)(R.ebx + 2); xpc = (u16)R.ebx;
        set_cx(xs);
        pushb_cx((u8)xpb);
        pushb_cx((u8)(xpc >> 8));
        pushb_cx((u8)xpc);
        xs = (xs & 0xFFFF0000u) | (u16)R.ecx;
        R.eax = 0; R.ebx &= ~0xFF00u;
        R.eax = rd16h(PC()); set_bl(PC()[2]); xpc = (u16)R.eax; xpb = (xpb & ~0xFFu) | (u8)R.ebx;
        pc_jsl = 1; pc_load(0); pc_jsl = 0; return 1;
    }
    case 0xFC:
        R.ebx = R.esi - initaddrl; R.ebx = (R.ebx & 0xFFFF0000u) | (u16)(R.ebx + 1); xpc = (u16)R.ebx;
        set_cx(xs);
        pushb_cx((u8)(xpc >> 8));
        pushb_cx((u8)xpc);
        R.eax = 0; xs = (xs & 0xFFFF0000u) | (u16)R.ecx;
        R.eax = 0; R.ebx &= ~0xFF00u;
        set_cx(rd16h(PC())); set_bl(xpb); set_cx((u16)R.ecx + (u16)xx);
        zl_mcall(memtabler16[R.ebx]);
        xpc = (u16)R.eax; set_bl(xpb); pc_load(0); return 1;
    case 0x60:
        set_cx(((u16)xs + 1) & stackand); zl_mcall(ZID_membank0r8); ((u8 *)&xpc)[0] = (u8)R.eax;
        set_cx(((u16)R.ecx + 1) & stackand); zl_mcall(ZID_membank0r8); ((u8 *)&xpc)[1] = (u8)R.eax;
        xs = (xs & 0xFFFF0000u) | (u16)R.ecx;
        R.ebx &= ~0xFF00u; R.eax = (u16)(xpc + 1); xpc = (u16)R.eax; set_bl(xpb);
        pc_load(0); return 1;
    case 0x6B:
        set_cx(((u16)xs + 1) & stackand); R.eax = 0; zl_mcall(ZID_membank0r8); ((u8 *)&xpc)[0] = (u8)R.eax;
        set_cx(((u16)R.ecx + 1) & stackand); R.eax = 0; zl_mcall(ZID_membank0r8); ((u8 *)&xpc)[1] = (u8)R.eax;
        set_cx(((u16)R.ecx + 1) & stackand); R.eax = 0; zl_mcall(ZID_membank0r8); xpb = (xpb & ~0xFFu) | (u8)R.eax;
        xs = (xs & 0xFFFF0000u) | (u16)R.ecx;
        R.ebx &= ~0xFF00u; R.eax = (u16)(xpc + 1); set_bl(xpb); xpc = (u16)R.eax;
        pc_load(0); return 1;
    case 0x82: {
        u32 base;
        R.eax = (R.eax & 0xFFFF0000u) | xpc; set_bl(xpb);
        base = (R.eax & 0x8000) ? snesmmap[R.ebx] : snesmap2[R.ebx];
        R.eax = base;
        R.ebx = R.esi; R.ebx -= R.eax;
        R.ebx = (R.ebx & 0xFFFF0000u) | (u16)(R.ebx + 2);
        R.eax = 0;
        R.ebx = (R.ebx & 0xFFFF0000u) | (u16)((u16)R.ebx + rd16h(PC()));
        R.eax = (u16)R.ebx;
        R.ebx = 0; xpc = (u16)R.eax; set_bl(xpb);
        pc_load(0); return 1;
    }
    case 0x62: {
        u32 base;
        set_bl(xpb); R.eax = (R.eax & 0xFFFF0000u) | xpc;
        base = (R.eax & 0x8000) ? snesmmap[R.ebx] : snesmap2[R.ebx];
        R.eax = base;
        R.ebx = R.esi; R.ebx -= R.eax;
        R.ebx = (R.ebx & 0xFFFF0000u) | (u16)((u16)R.ebx + rd16h(PC()));
        R.eax = (R.eax & 0xFFFF0000u) | (u16)R.ebx;
        R.esi += 2;
        R.eax = (R.eax & 0xFFFF0000u) | (u16)((u16)R.eax + 2);
        set_cx(xs);
        { u16 axs = (u16)R.eax; set_al(axs >> 8); zl_mcall(ZID_membank0w8); set_cx(((u16)R.ecx - 1) | stackor); set_ax(axs); }
        zl_mcall(ZID_membank0w8); set_cx(((u16)R.ecx - 1) | stackor);
        xs = (xs & 0xFFFF0000u) | (u16)R.ecx;
        R.ebx = 0; return 1;
    }
    case 0xF4:
        set_cx(xs);
        set_al(PC()[1]); zl_mcall(ZID_membank0w8); set_cx(((u16)R.ecx - 1) | stackor);
        set_al(PC()[0]); zl_mcall(ZID_membank0w8); set_cx(((u16)R.ecx - 1) | stackor);
        xs = (xs & 0xFFFF0000u) | (u16)R.ecx; R.esi += 2; return 1;
    case 0xD4: {
        u16 axs;
        set_ax((u16)PC()[0]); R.eax &= ~0xFF00u; /* xor ah,ah; mov al,[esi] */
        R.ecx = xd; R.esi += 1;
        set_cx((u16)R.ecx + (u16)R.eax);
        zl_mcall(ZID_membank0r16);
        set_cx(xs);
        axs = (u16)R.eax;
        set_al(axs >> 8); zl_mcall(ZID_membank0w8); set_cx(((u16)R.ecx - 1) | stackor);
        set_ax(axs);
        zl_mcall(ZID_membank0w8); set_cx(((u16)R.ecx - 1) | stackor);
        xs = (xs & 0xFFFF0000u) | (u16)R.ecx; return 1;
    }
    /* ---- P register ops ---- */
    case 0x28: {
        set_cx(((u16)xs + 1) & stackand); zl_mcall(ZID_membank0r8);
        xs = (xs & 0xFFFF0000u) | (u16)R.ecx;
        R.ebx &= ~0xFF00u; DL = (u8)R.eax; restoredl();
        if (xe & 1) { DL |= 0x30; switch_table(var); return 1; }
        switch_table(var);
        if (DL & 0x10) setxhi();
        return 1;
    }
    case 0xC2: {
        u8 al = *PC(); R.esi += 1; set_al(al);
        if (al & 0xC3) { makedl(); DL &= (u8)~al; restoredl(); } else DL &= (u8)~al;
        if (xe & 1) DL |= 0x30;
        switch_table(var); return 1;
    }
    case 0xE2: {
        u8 al = *PC(); R.esi += 1; set_al(al);
        if (al & 0xC3) { makedl(); DL |= al; restoredl(); } else DL |= al;
        switch_table(var);
        if (DL & 0x10) setxhi();
        return 1;
    }
    case 0xFB: {
        u8 al = (u8)(flagc & 1);
        set_al(al); flagc = 0;
#ifndef SA1
        if (xe == al) return 1;
#endif
        if (xe) flagc = 0xFF;
        xe = al;
        if (!(xe & 1)) { DL |= 0x20; stackand = 0xFFFF; stackor = 0; return 1; }
        DL |= 0x30; switch_table(var);
        ((u8 *)&xx)[1] = 0; ((u8 *)&xy)[1] = 0; ((u8 *)&xs)[1] = 1;
        stackand = 0x01FF; stackor = 0x0100;
        return 1;
    }
#ifdef SA1
    case 0x00: { extern u8 debstop4; debstop4 = 1; R.esi -= 1; return 1; }   /* SA1COp00 is a stop (BRKMacro there is unreachable) */
#else
    case 0x00: irq_entry(brkv, brkv8, 1); return 1;
#endif
    case 0x02: irq_entry(copv, copv8, 0); return 1;
#ifdef SA1
    case 0x40: {                                  /* SA1COp40: intrset=2, then the SA-1 RTIMacro (always the native 4-byte frame) */
        intrset = 2;
        if (curexecstate == 0) DH = 0;
        curnmi = 0;
        set_cx(((u16)xs + 1) & stackand); zl_mcall(ZID_membank0r8);
        DL = (u8)R.eax; restoredl();
        set_cx(((u16)R.ecx + 1) & stackand); zl_mcall(ZID_membank0r8); ((u8 *)&xpc)[0] = (u8)R.eax;
        set_cx(((u16)R.ecx + 1) & stackand); zl_mcall(ZID_membank0r8); ((u8 *)&xpc)[1] = (u8)R.eax;
        set_cx(((u16)R.ecx + 1) & stackand); zl_mcall(ZID_membank0r8); xpb = (xpb & ~0xFFu) | (u8)R.eax;
        xs = (xs & 0xFFFF0000u) | (u16)R.ecx;
        R.ebx &= ~0xFF00u; R.eax = 0;
        set_bl(DL); R.edi = SA1tablead[R.ebx & 0xFF];
        set_bl(xpb); R.eax = (u16)xpc;
        if (R.eax & 0x8000) { R.esi = snesmmap[R.ebx]; initaddrl = R.esi; R.esi += R.eax; }
        else {
            R.esi = snesmap2[R.ebx];
            if (R.eax >= 0x4300 && memtabler8[R.ebx] == ZID_regaccessbankr8) R.esi = (u32)(uintptr_t)dmadata - 0x4300;
            initaddrl = R.esi; R.esi += R.eax;
        }
        if (DL & 0x10) setxhi();
        return 1;
    }
#else
    case 0x40: {
        if (nmistatus == 3) {
            if (curexecstate & 1) { curexecstate &= 0xFE; zl_mcall(ZID_changeexecloop); }
            if (curexecstate == 0) DH = 0;
        }
        curnmi = 0;
        if (!(xe & 1)) {
            set_cx(((u16)xs + 1) & stackand); zl_mcall(ZID_membank0r8);
            xs = (xs & 0xFFFF0000u) | (u16)R.ecx;
            DL = (u8)R.eax; restoredl();
            set_cx(((u16)xs + 1) & stackand); R.eax = 0; zl_mcall(ZID_membank0r8); ((u8 *)&xpc)[0] = (u8)R.eax;
            set_cx(((u16)R.ecx + 1) & stackand); R.eax = 0; zl_mcall(ZID_membank0r8); ((u8 *)&xpc)[1] = (u8)R.eax;
            set_cx(((u16)R.ecx + 1) & stackand); R.eax = 0; zl_mcall(ZID_membank0r8); xpb = (xpb & ~0xFFu) | (u8)R.eax;
            xs = (xs & 0xFFFF0000u) | (u16)R.ecx;
            R.ebx &= ~0xFF00u; R.eax = xpc;
            set_bl(DL); R.edi = (u32)(var ? tableadc[R.ebx & 0xFF] : tableadb[R.ebx & 0xFF]);
            set_bl(xpb); xpc = (u16)R.eax;
        } else {
            set_cx(((u16)xs + 1) & stackand); zl_mcall(ZID_membank0r8);
            xs = (xs & 0xFFFF0000u) | (u16)R.ecx;
            DL = (u8)R.eax | 0x30; restoredl();
            set_cx(((u16)xs + 1) & stackand); R.eax = 0; zl_mcall(ZID_membank0r8); ((u8 *)&xpc)[0] = (u8)R.eax;
            set_cx(((u16)R.ecx + 1) & stackand); R.eax = 0; zl_mcall(ZID_membank0r8); ((u8 *)&xpc)[1] = (u8)R.eax;
            xs = (xs & 0xFFFF0000u) | (u16)R.ecx;
            R.ebx &= ~0xFF00u; R.eax = xpc;
            set_bl(DL); R.edi = (u32)(var ? tableadc[R.ebx & 0xFF] : tableadb[R.ebx & 0xFF]);
            set_bl(0); xpc = (u16)R.eax;
            if (xpc & 0x8000) { R.esi = snesmmap[R.ebx]; initaddrl = R.esi; R.esi += R.eax; }
            else { R.esi = snesmap2[R.ebx]; initaddrl = R.esi; R.esi += R.eax; }
            return 1;
        }
        /* native-mode tail: set esi, handle WAI marker / DMA area / 16-bit index high bytes */
        R.eax = xpc;
        if (R.eax & 0x8000) {
            R.esi = snesmmap[R.ebx]; initaddrl = R.esi; R.esi += R.eax;
            if (*PC() == 0xCB) intrset = 2;
        } else {
            R.esi = snesmap2[R.ebx];
            if (R.eax >= 0x4300) {
                if (memtabler8[R.ebx] == ZID_regaccessbankr8) { R.esi = (u32)(uintptr_t)dmadata - 0x4300; initaddrl = R.esi; R.esi += R.eax; }
                else { doirqnext = 0; initaddrl = R.esi; R.esi += R.eax; }
            } else {
                initaddrl = R.esi; R.esi += R.eax;
                if (*PC() == 0xCB) intrset = 2;
            }
        }
        if (DL & 0x10) setxhi();
        return 1;
    }
#endif
    case 0xCB:
        if (intrset == 1) { R.esi -= 1; return 1; }
        if (intrset == 0) { intrset = 1; R.esi -= 1; return 1; }
        if (intrset == 2) { intrset = 0;
#ifndef SA1
            doirqnext = 0;
#endif
            return 1; }
        R.esi -= 1; return 1;
    case 0x44: case 0x54: {
        int up = (op == 0x54);
        set_ax(rd16h(PC())); xdb = (xdb & ~0xFFu) | (u8)R.eax; set_bl(R.eax >> 8);
        set_cx(xx);
        zl_mcall(memtabler8[R.ebx]);
        set_bl(xdb); set_cx(xy);
        zl_mcall(memtablew8[R.ebx]);
        if (DL & 0x10) {
            if (up) { XX8++; XY8++; } else { XX8--; XY8--; }
        } else {
            if (up) { XX16++; XY16++; } else { XX16--; XY16--; }
        }
        XA16--;
        if (XA16 == 0xFFFF) R.esi += 2; else R.esi -= 1;
        return 1;
    }
    /* ---- stack ---- */
    case 0x48: if (m8) push8(XA8); else push16(((u8 *)&xa)[1], XA8); return 1;
    case 0xDA: if (x8) push8(XX8); else push16(((u8 *)&xx)[1], XX8); return 1;
    case 0x5A: if (x8) push8(XY8); else push16(((u8 *)&xy)[1], XY8); return 1;
    case 0x0B: push16(((u8 *)&xd)[1], (u8)xd); return 1;
    case 0x4B: push8((u8)xpb); return 1;
    case 0x8B: push8((u8)xdb); return 1;
    case 0x08: makedl(); push8(DL); return 1;
    case 0x68: case 0x7A: case 0xFA: {
        u8 *lo = (op == 0x68) ? (u8 *)&xa : (op == 0x7A) ? (u8 *)&xy : (u8 *)&xx;
        int narrow = (op == 0x68) ? m8 : x8;
        u8 v = pop8_core();
        xs = (xs & 0xFFFF0000u) | (u16)R.ecx;
        lo[0] = v;
        if (narrow) { nz8(v); }
        else {
            u8 v2;
            set_cx(((u16)R.ecx + 1) & stackand);
            zl_mcall(ZID_membank0r8);
            v2 = (u8)R.eax;
            lo[1] = v2;
            xs = (xs & 0xFFFF0000u) | (u16)R.ecx;
            nz16((u16)((v2 << 8) | v));
        }
        return 1;
    }
    case 0xAB: { u8 v = pop8_core(); xs = (xs & 0xFFFF0000u) | (u16)R.ecx; *(u8 *)&xdb = v; nz8(v); return 1; }
    case 0x2B: {
        u8 v = pop8_core(), v2;
        *(u8 *)&xd = v;
        xs = (xs & 0xFFFF0000u) | (u16)R.ecx;
        set_cx(((u16)R.ecx + 1) & stackand);
        zl_mcall(ZID_membank0r8);
        v2 = (u8)R.eax;
        ((u8 *)&xd)[1] = v2;
        update_dpage();
        xs = (xs & 0xFFFF0000u) | (u16)R.ecx;
        nz16((u16)((v2 << 8) | v));
        return 1;
    }
    }
    return 0;
}



/* ===== which opcodes are done in C, the opcode tables, and the threaded dispatch (the asm "endloop") ===== */
extern u8 cpucycle[256];
extern u32 cycpbl, cycpblt, opcjmptab[256];
extern u32 tableAb[256], tableBb[256], tableCb[256], tableDb[256], tableEb[256], tableFb[256], tableGb[256], tableHb[256];
extern u32 tableAc[256], tableBc[256], tableCc[256], tableDc[256], tableEc[256], tableFc[256], tableGc[256], tableHc[256];
static int handles(u32 op, int *alu_dec_sensitive)
{
    int grp = (op >> 5) & 7, lo = op & 0x1F;
    *alu_dec_sensitive = 0;
    if ((op & 3) == 1 || (op & 3) == 3 || lo == 0x12 || (op & 0x0F) == 0x0D || (op & 0x0F) == 0x0F || (op & 0x0F) == 0x09) {
        switch (lo) {
        case 0x01: case 0x03: case 0x05: case 0x07: case 0x0D: case 0x0F: case 0x11: case 0x12: case 0x13:
        case 0x15: case 0x17: case 0x19: case 0x1D: case 0x1F:
            *alu_dec_sensitive = (grp == 3 || grp == 7);
            return 1;
        case 0x09:
            if (grp == 4) break;               /* $89 = BIT # */
            *alu_dec_sensitive = (grp == 3 || grp == 7);
            return 1;
        }
    }
    switch (op) {
    case 0x06: case 0x0A: case 0x0E: case 0x16: case 0x1E: case 0x26: case 0x2A: case 0x2E: case 0x36: case 0x3E:
    case 0x46: case 0x4A: case 0x4E: case 0x56: case 0x5E: case 0x66: case 0x6A: case 0x6E: case 0x76: case 0x7E:
    case 0x3A: case 0xC6: case 0xCE: case 0xD6: case 0xDE: case 0x1A: case 0xE6: case 0xEE: case 0xF6: case 0xFE:
    case 0x04: case 0x0C: case 0x14: case 0x1C: case 0x24: case 0x2C: case 0x34: case 0x3C: case 0x89:
    case 0x64: case 0x74: case 0x9C: case 0x9E:
    case 0xA0: case 0xA4: case 0xAC: case 0xB4: case 0xBC: case 0xA2: case 0xA6: case 0xAE: case 0xB6: case 0xBE:
    case 0xC0: case 0xC4: case 0xCC: case 0xE0: case 0xE4: case 0xEC:
    case 0x84: case 0x8C: case 0x94: case 0x86: case 0x8E: case 0x96:
    case 0x88: case 0xC8: case 0xCA: case 0xE8:
    case 0x8A: case 0x98: case 0xA8: case 0xAA: case 0x9B: case 0xBB: case 0xBA: case 0x1B: case 0x3B: case 0x5B: case 0x7B: case 0x9A: case 0xEB:
    case 0x18: case 0x38: case 0xB8: case 0x58: case 0x78: case 0xD8: case 0xF8: case 0xEA: case 0x42: case 0xDB:
    case 0x10: case 0x30: case 0x50: case 0x70: case 0x90: case 0xB0: case 0xD0: case 0xF0: case 0x80:
    case 0x48: case 0xDA: case 0x5A: case 0x0B: case 0x4B: case 0x8B: case 0x08:
    case 0x68: case 0x7A: case 0xFA: case 0xAB: case 0x2B:
    case 0x4C: case 0x5C: case 0x6C: case 0x7C: case 0xDC: case 0x20: case 0x22: case 0xFC: case 0x60: case 0x6B: case 0x82:
    case 0x62: case 0xF4: case 0xD4: case 0x28: case 0xC2: case 0xE2: case 0xFB: case 0x00: case 0x02: case 0x40: case 0xCB: case 0x44: case 0x54:
        return 1;
    }
    return 0;
}


#ifndef SA1
#include "cpu_stubs.h"      /* zl_cpuop_ids[2][256]: override ids of the per-opcode C handlers */

/* original (asm) handler of every table entry, kept for the tail-style differential test (ZL_DUALT) */
static u32 cpu_orig[2][8][256];
u32 zl_tail_orig(u32 k)
{
    u32 id = 0x7Cu << 21 | k, dl = DL;
    int t = (dl & 8) ? ((dl & 0x10) ? ((dl & 0x20) ? 7 : 6) : ((dl & 0x20) ? 5 : 4)) : ((dl & 0x10) ? ((dl & 0x20) ? 3 : 2) : ((dl & 0x20) ? 1 : 0));
    for (int v = 0; v < 2; v++)
        for (int op = 0; op < 256; op++)
            if (zl_cpuop_ids[v][op] == id) return cpu_orig[v][t][op];
    return 0;
}

void zl_cpu_install(void)
{
    u32 *sets[2][8] = {
        { tableAb, tableBb, tableCb, tableDb, tableEb, tableFb, tableGb, tableHb },
        { tableAc, tableBc, tableCc, tableDc, tableEc, tableFc, tableGc, tableHc } };
    int v, t, op, dec;
    for (op = 0; op < 256; op++) {
        if (!handles((u32)op, &dec)) continue;
        for (v = 0; v < 2; v++)
            for (t = 0; t < 8; t++) { cpu_orig[v][t][op] = sets[v][t][op]; sets[v][t][op] = zl_cpuop_ids[v][op]; }
    }
}

/* run one opcode, then the asm "endloop" (b: plain, c: with the SPC700 step); returns the next handler (tail jump) or 0 (ret) */
u32 zl_cpu_step(u32 op, u32 var)
{
    u32 cyc;
    zs_cpu_op_inner(op, var);
    if (var) {
        u32 old = cycpbl;
        cycpbl = old - 55;
        if (old < 55) {
            LG.eax = cycpblt;
            LG.ebx = (LG.ebx & ~0xFFu) | *(u8 *)(uintptr_t)LG.ebp;
            cycpbl += LG.eax;
            LG.ebp++;
            zl_mcall(opcjmptab[LG.ebx]);
            LG.ebx = 0;
        }
    }
    LG.ebx = (LG.ebx & ~0xFFu) | *(u8 *)(uintptr_t)LG.esi;
    LG.esi++;
    cyc = cpucycle[LG.ebx];
    { u32 dh = (LG.edx >> 8) & 0xFF; LG.edx = (LG.edx & 0xFFFF00FFu) | ((dh - cyc) & 0xFF) << 8; if (dh < cyc) return 0; }
    return *(u32 *)(uintptr_t)(LG.edi + LG.ebx * 4);
}

/* post-hook of inittablec: patch the tables once the asm built them */
void zl_inittablec(void)
{
    zl_orig(ZID_inittablec);
    zl_cpu_install();
}

#endif /* !SA1 */
