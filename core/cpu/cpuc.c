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
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <link.h>
#include <dlfcn.h>
#include <signal.h>
#include <setjmp.h>
#include <stddef.h>

typedef uint8_t u8;
typedef int8_t s8;
typedef uint16_t u16;
typedef int16_t s16;
typedef uint32_t u32;
typedef int32_t s32;
typedef uint64_t u64;

#define HID __attribute__((visibility("hidden")))

typedef struct { u32 eax, ecx, edx, ebx, esi, edi, ebp; } regs_t;
extern regs_t zs_R;
#define R zs_R

/* ---- asm state ---- */
extern u32 xa, xdb, xpb, xs, xd, xx, xy, flagnz, flago, flagc, initaddrl, wramdata;
extern u16 xpc, stackand, stackor;
extern u8 xe, xp, intrset, doirqnext, curexecstate, nmistatus, curnmi;
extern void *memtabler8[256], *memtabler16[256], *memtablew8[256], *memtablew16[256];
extern void *DPageR8, *DPageR16, *DPageW8, *DPageW16;
extern void *Bank0datr8[256], *Bank0datr16[256], *Bank0datw8[256], *Bank0datw16[256];
extern void *tableadb[256], *tableadc[256];
extern void membank0r8(void), membank0r16(void), membank0w8(void), membank0w16(void);
extern void zs_mcall(void *fn);

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
static inline void mem_tab(void **tab, u32 off, u32 bank)
{
    set_cx(off);
    set_bl(bank);
    zs_mcall(tab[R.ebx]);
}
static inline void mem_fn(void *fn) { zs_mcall(fn); }

/* addressing modes */
enum { A_I, A_a, A_al, A_d, A_A, A_BdBCy, A_LdLCy, A_BdCxB, A_dCx, A_dCy, A_aCx, A_aCy, A_alCx, A_BdB, A_LdL, A_dCs, A_BdCsBCy };

static inline u8 *PC(void) { return (u8 *)R.esi; }
static inline u16 rd16h(const u8 *p) { return *(const u16 *)p; }

/*
 * Resolve an effective address and perform the access.
 *   wide: 0/1 ; write: 0=read (result in R.eax), 1=write (data = value) ; adv: advance esi past operands
 */
static void am_access(int mode, int wide, int write, u32 data, int adv)
{
    u8 *p = PC();
    u16 d16 = (u16)data;
    void **tab = write ? (wide ? memtablew16 : memtablew8) : (wide ? memtabler16 : memtabler8);
    void *b0 = write ? (wide ? (void *)membank0w16 : (void *)membank0w8) : (wide ? (void *)membank0r16 : (void *)membank0r8);
    void *dpg = write ? (wide ? DPageW16 : DPageW8) : (wide ? DPageR16 : DPageR8);
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
        SETDATA(); zs_mcall(tab[R.ebx]); return;
    case A_al:
        set_cx(rd16h(p)); set_bl(p[2]); if (adv) R.esi += 3;
        SETDATA(); zs_mcall(tab[R.ebx]); return;
    case A_d:
        set_bl(p[0]); R.ecx = xd; if (adv) R.esi += 1;
        SETDATA(); zs_mcall(dpg); return;
    case A_A:
        if (write) { if (wide) XA16 = d16; else XA8 = (u8)d16; }
        else { if (wide) R.eax = xa; else set_al(XA8); }
        return;
    case A_BdBCy:
        set_bl(p[0]); R.ecx = xd; if (adv) R.esi += 1;
        zs_mcall(DPageR16);
        set_cx(R.eax); set_bl(xdb);
        t = (u16)R.ecx;
        { u32 s = t + (u16)xy; set_cx(s); if (s > 0xFFFF) set_bl(R.ebx + 1); }
        SETDATA(); zs_mcall(tab[R.ebx]); return;
    case A_LdLCy:
        R.ecx = xd; set_bl(p[0]); set_cx((u16)R.ecx + (u16)R.ebx); if (adv) R.esi += 1;
        { u16 save = (u16)R.ecx;
          zs_mcall(membank0r16);
          set_cx(save); }
        set_cx((u16)R.ecx + 2);
        { u16 axs = (u16)R.eax;
          zs_mcall(membank0r8);
          set_bl(R.eax);
          set_ax(axs); }
        set_cx(R.eax);
        { u32 s = (u16)R.ecx + (u16)xy; set_cx(s); if (s > 0xFFFF) set_bl(R.ebx + 1); }
        if (write) set_ax(d16);
        zs_mcall(tab[R.ebx]); return;
    case A_BdCxB:
        R.ecx = xd; set_bl(p[0]); set_cx((u16)R.ecx + (u16)R.ebx); if (adv) R.esi += 1;
        set_cx((u16)R.ecx + (u16)xx);
        zs_mcall(membank0r16);
        set_cx(R.eax); set_bl(xdb);
        SETDATA(); zs_mcall(tab[R.ebx]); return;
    case A_dCx:
    case A_dCy:
        R.ecx = xd; set_bl(p[0]); set_cx((u16)R.ecx + (u16)R.ebx); if (adv) R.esi += 1;
        set_cx((u16)R.ecx + (u16)(mode == A_dCx ? xx : xy));
        SETDATA(); zs_mcall(b0); return;
    case A_aCx:
    case A_aCy:
        set_cx(rd16h(p)); set_bl(xdb); if (adv) R.esi += 2;
        { u32 s = (u16)R.ecx + (u16)(mode == A_aCx ? xx : xy); set_cx(s); if (s > 0xFFFF) set_bl(R.ebx + 1); }
        SETDATA(); zs_mcall(tab[R.ebx]); return;
    case A_alCx:
        set_cx(rd16h(p)); set_bl(p[2]); if (adv) R.esi += 3;
        { u32 s = (u16)R.ecx + (u16)xx; set_cx(s); if (s > 0xFFFF) set_bl(R.ebx + 1); }
        SETDATA(); zs_mcall(tab[R.ebx]); return;
    case A_BdB:
        set_bl(p[0]); R.ecx = xd; if (adv) R.esi += 1;
        zs_mcall(DPageR16);
        set_cx(R.eax); set_bl(xdb);
        SETDATA(); zs_mcall(tab[R.ebx]); return;
    case A_LdL:
        set_bl(p[0]); R.ecx = xd; if (adv) R.esi += 1;
        set_cx((u16)R.ecx + (u16)R.ebx);
        { u16 save = (u16)R.ecx;
          zs_mcall(membank0r16);
          set_cx(save); }
        set_cx((u16)R.ecx + 2);
        { u16 axs = (u16)R.eax;
          zs_mcall(membank0r8);
          set_bl(R.eax);
          set_ax(axs); }
        set_cx(R.eax);
        SETDATA(); zs_mcall(tab[R.ebx]); return;
    case A_dCs:
        set_bl(p[0]); set_cx(xs); if (adv) R.esi += 1;
        set_cx((u16)R.ecx + (u16)R.ebx);
        SETDATA(); zs_mcall(b0); return;
    case A_BdCsBCy:
        set_bl(p[0]); set_cx(xs); if (adv) R.esi += 1;
        set_cx((u16)R.ecx + (u16)R.ebx);
        zs_mcall(membank0r16);
        set_cx(R.eax); set_bl(xdb);
        { u32 s = (u16)R.ecx + (u16)xy; set_cx(s); if (s > 0xFFFF) set_bl(R.ebx + 1); }
        SETDATA(); zs_mcall(tab[R.ebx]); return;
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
    zs_mcall(membank0w8);
    set_cx(((u16)R.ecx - 1) | stackor);
    xs = (xs & 0xFFFF0000u) | (u16)R.ecx;
}
/* PUSHMacro16b: high byte then low byte, a single xs update at the end */
static inline void push16(u8 hi, u8 lo)
{
    set_cx(xs);
    set_al(hi);
    zs_mcall(membank0w8);
    set_cx(((u16)R.ecx - 1) | stackor);
    set_al(lo);
    zs_mcall(membank0w8);
    set_cx(((u16)R.ecx - 1) | stackor);
    xs = (xs & 0xFFFF0000u) | (u16)R.ecx;
}
static inline u8 pop8_core(void)
{
    R.eax = wramdata;
    set_cx(((u16)xs + 1) & stackand);
    zs_mcall(membank0r8);
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

static inline void *tabad(int var) { return var ? tableadc[DL] : tableadb[DL]; }
static inline void switch_table(int var) { set_bl(DL); R.ebx &= 0xFF; R.edi = (u32)(var ? tableadc[DL] : tableadb[DL]); }


static inline void branch(int taken)
{
    if (taken) { s8 d = *(s8 *)PC(); R.esi += (u32)(s32)d; }
    R.esi += 1;
}

/* 8-bit / 16-bit read-modify-write ALU kernels: value in/out through *v, flags like the asm */
static inline void shl_flags8(u8 *v, int carry_out) { nz8(*v); flagc = carry_out ? 0xFF : 0; }
static inline void shl_flags16(u16 *v, int carry_out) { nz16(*v); flagc = carry_out ? 0xFF : 0; }

/* returns 1 if handled (ADC/SBC with D set are never routed here: the D tables keep the asm handlers) */
unsigned long zs_cpu_opcount;
HID __attribute__((force_align_arg_pointer)) int zs_cpu_op(u32 op, u32 var)
{
    zs_cpu_opcount++;
    { static int brk = -1; if (brk < 0) brk = getenv("ZS_CPU_BREAK") != NULL; if (brk && op == 0x85 && (DL & 0x20)) XA8 ^= 1; }
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
            case 3: {                                           /* ADC (binary) */
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
            case 7: {                                           /* SBC (binary) */
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
        rd(mode, w, 0);                                          /* "ni": esi not advanced */
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
        wr(mode, w, R.eax);
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
    case 0x1B: if (xe & 1) *(u8 *)&xs = XA8; else *(u16 *)&xs = XA16; return 1;
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
            zs_mcall(membank0r8);
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
        zs_mcall(membank0r8);
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


/* opcodes handled by zs_cpu_op (kept in sync with the switch above) */
static void report(void) { fprintf(stderr, "cpu ops in C: %lu\n", zs_cpu_opcount); }
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
        return 1;
    }
    return 0;
}

extern void *tableAb[256], *tableBb[256], *tableCb[256], *tableDb[256], *tableEb[256], *tableFb[256], *tableGb[256], *tableHb[256];
extern void *tableAc[256], *tableBc[256], *tableCc[256], *tableDc[256], *tableEc[256], *tableFc[256], *tableGc[256], *tableHc[256];
extern void *zs_cstub_b_tab[256], *zs_cstub_c_tab[256];
HID void *zs_orig_tab[2][8][256];     /* [variant b/c][A..H][opcode] original asm handlers */
HID int zs_cpu_ported[256];

HID __attribute__((force_align_arg_pointer)) void zs_cpu_install(void)
{
    void **sets[2][8] = {
        { tableAb, tableBb, tableCb, tableDb, tableEb, tableFb, tableGb, tableHb },
        { tableAc, tableBc, tableCc, tableDc, tableEc, tableFc, tableGc, tableHc } };
    int v, t, op;
    static int mode = -1;
    if (mode < 0) mode = getenv("ZS_CPU_ASM") ? 0 : 1;
    if (getenv("ZS_CPU_COUNT")) atexit(report);
    for (v = 0; v < 2; v++)
        for (t = 0; t < 8; t++)
            memcpy(zs_orig_tab[v][t], sets[v][t], sizeof(void *) * 256);
    for (op = 0; op < 256; op++) {
        int dec;
        zs_cpu_ported[op] = handles(op, &dec);
        if (!zs_cpu_ported[op] || mode == 0) continue;
        for (v = 0; v < 2; v++)
            for (t = 0; t < 8; t++) {
                if (dec && t >= 4) continue;           /* decimal mode ADC/SBC stay asm */
                sets[v][t][op] = v ? zs_cstub_c_tab[op] : zs_cstub_b_tab[op];
            }
    }
}

/* =====================================================================
 * Differential fuzz: original asm handler vs C stub, same machine state
 * ===================================================================== */
extern void zs_cpu_run(void *entry, const regs_t *in, regs_t *out);
extern void *zs_capture_tab[256];
extern u8 xe, xp;
extern u32 curcyc;

typedef struct { void *p; size_t n; const char *name; } vreg_t;
static u32 frng = 1;
static u32 frnd(void) { frng ^= frng << 13; frng ^= frng >> 17; frng ^= frng << 5; return frng; }

static const vreg_t vars[] = {
    { &xa, 4, "xa" }, { &xx, 4, "xx" }, { &xy, 4, "xy" }, { &xs, 4, "xs" }, { &xd, 4, "xd" }, { &xdb, 4, "xdb" }, { &xpb, 4, "xpb" },
    { &xpc, 2, "xpc" }, { &xe, 1, "xe" }, { &xp, 1, "xp" }, { &flagnz, 4, "flagnz" }, { &flago, 4, "flago" }, { &flagc, 4, "flagc" },
    { &stackand, 2, "stackand" }, { &stackor, 2, "stackor" }, { &DPageR8, 4, "DPageR8" }, { &DPageR16, 4, "DPageR16" },
    { &DPageW8, 4, "DPageW8" }, { &DPageW16, 4, "DPageW16" }, { &intrset, 1, "intrset" }, { &doirqnext, 1, "doirqnext" },
};
#define NVARS ((int)(sizeof vars / sizeof vars[0]))

static void fuzz_setup(u32 op, u8 *code)
{
    u32 P, i;
    int emul = (frnd() % 8 == 0);
    int grp = (op >> 5) & 7, lo = op & 0x1F;
    int longptr = ((op & 3) == 3 && (lo == 0x07 || lo == 0x17)) || 0;
    int dec;
    handles(op, &dec);
    P = frnd() & 0xFF;
    P &= ~0x08u; if (!dec && (frnd() & 1)) P |= 0x08;                  /* D */
    if (emul) P |= 0x30;
    memset(code - 512, 0xEA, 1024);
    for (i = 0; i < 4; i++) code[i] = (u8)frnd();
    if (op == 0xD8 || op == 0xF8) memset(code, 0xEA, 4);
    xa = frnd() & 0xFFFF; xx = frnd() & 0xFFFF; xy = frnd() & 0xFFFF;
    if (frnd() % 3 == 0) { xx &= 0xFF; xy &= 0xFF; }
    if (P & 0x10) { xx &= 0xFF; xy &= 0xFF; }
    if (frnd() % 2) { xx %= 0x600; xy %= 0x600; }
    else { xx &= 0xFF; xy &= 0xFF; }
    xd = longptr || frnd() % 2 ? (frnd() % 0x1000) : (0xF000 + frnd() % 0x1000);
    if (frnd() % 4 == 0) xd &= ~0xFFu;
    xs = emul ? (0x100 | (frnd() & 0xFF)) : (frnd() % 0x1700);
    if (frnd() % 6 == 0) xs = (frnd() % 4) * 0xFF;                     /* stack wrap cases */
    stackand = emul ? 0x1FF : 0xFFFF; stackor = emul ? 0x100 : 0;
    xe = emul;
    { static const u8 banks[] = { 0x7E, 0x7F, 0xC0, 0xC1, 0xFF, 0xF0, 0xFE }; xdb = banks[frnd() % sizeof banks]; xpb = 0; }
    flagnz = (frnd() & 1 ? 0x10000 : 0) | (frnd() & 1 ? 0 : (1 + frnd() % 0xFFFE));
    flagc = frnd() & 1 ? 0xFF : 0; flago = frnd() & 1 ? 0xFF : 0;
    if (frnd() % 3 == 0) flagnz = ((frnd() & 0xFF) << 8) | 0;           /* 8-bit style value */
    P = (P & 0x3C) | (frnd() & 0xC3);                                   /* stale N V Z C bits in dl */
    xpc = 0; xp = (u8)P;
    R.edx = (200u << 8) | P;
    /* long operand banks */
    { static const u8 lb[] = { 0x7E, 0x7F, 0xC0, 0xFF }; code[2] = lb[frnd() % sizeof lb]; }
    if (op == 0x5C || op == 0x22) code[2] = 0x7E;
    update_dpage();
    if (longptr) {
        u8 *w = (u8 *)wramdata;
        u32 a = ((u32)xd + code[0]) & 0xFFFF;                           /* [d] pointer at D+dp (+index applied later) */
        static const u8 lb[] = { 0x7E, 0x7F, 0xC0, 0xFF };
        if (a + 3 <= 0x2000) { w[a] = (u8)frnd(); w[a + 1] = (u8)frnd(); w[a + 2] = lb[frnd() % sizeof lb]; }
    }
}

unsigned zs_cpu_fuzz(unsigned iters, unsigned seed)
{
    u8 *w = (u8 *)wramdata;
    u8 *snapw = malloc(0x20000), *asmw = malloc(0x20000);
    u8 *snapv[NVARS], *asmv[NVARS];
    static u8 codebuf[1024];
    u8 *code = codebuf + 512;
    unsigned it, bad = 0, ran = 0;
    unsigned long perop[256] = { 0 };
    int v;
    struct sigaction sa;
    frng = seed ? seed : 1;
    for (v = 0; v < NVARS; v++) { snapv[v] = malloc(vars[v].n); asmv[v] = malloc(vars[v].n); }
    for (it = 0; it < iters; it++) {
        u32 op;
        regs_t in, outA, outC;
        int tidx, k;
        do { op = frnd() & 0xFF; } while (!zs_cpu_ported[op]);
        if (it < 20000) op = (u32)(it % 256) , (void)0;
        if (!zs_cpu_ported[op]) continue;
        if ((it & 63) == 0) for (k = 0; k < 0x20000; k++) w[k] = (u8)frnd();
        fuzz_setup(op, code);
        in.eax = frnd(); in.ecx = frnd() & 0xFFFF; in.edx = R.edx; in.ebx = op;
        in.esi = (u32)(code);     /* operand bytes follow the opcode: esi points after the opcode */
        in.edi = (u32)zs_capture_tab; in.ebp = 0;
        tidx = ((in.edx & 0x08) ? 4 : 0) + ((in.edx & 0x10) ? 2 : 0) + ((in.edx & 0x20) ? 1 : 0);
        memcpy(snapw, w, 0x20000);
        for (v = 0; v < NVARS; v++) memcpy(snapv[v], vars[v].p, vars[v].n);
        zs_cpu_run(zs_orig_tab[0][tidx][op], &in, &outA);
        memcpy(asmw, w, 0x20000);
        for (v = 0; v < NVARS; v++) memcpy(asmv[v], vars[v].p, vars[v].n);
        memcpy(w, snapw, 0x20000);
        for (v = 0; v < NVARS; v++) memcpy(vars[v].p, snapv[v], vars[v].n);
        zs_cpu_run(zs_cstub_b_tab[op], &in, &outC);
        ran++; perop[op]++;
        {
            int diff = 0;
            if (outA.esi != outC.esi || outA.edx != outC.edx || outA.edi != outC.edi || outA.ebx != outC.ebx || outA.ebp != outC.ebp) diff = 1;
            for (v = 0; v < NVARS; v++) if (memcmp(asmv[v], vars[v].p, vars[v].n)) { diff = 1; if (bad < 20) fprintf(stderr, "cpu fuzz: op %02X (P=%02X xe=%d) var %s: C=%x asm=%x\n", op, (u8)in.edx, xe, vars[v].name, *(u32 *)vars[v].p, *(u32 *)asmv[v]); }
            if (memcmp(asmw, w, 0x20000)) { diff = 1; if (bad < 20) { int j; for (j = 0; j < 0x20000; j++) if (asmw[j] != w[j]) break; fprintf(stderr, "cpu fuzz: op %02X (P=%02X) WRAM differs at %05X: C=%02x asm=%02x\n", op, (u8)in.edx, j, w[j], asmw[j]); } }
            if (diff) {
                bad++;
                if (bad <= 20) fprintf(stderr, "cpu fuzz: op %02X P=%02X esi: C=%d asm=%d edx: C=%08x asm=%08x edi C=%x asm=%x ebx C=%x asm=%x\n", op, (u8)in.edx, (int)(outC.esi - in.esi), (int)(outA.esi - in.esi), outC.edx, outA.edx, outC.edi, outA.edi, outC.ebx, outA.ebx);
                /* continue from the asm result */
                memcpy(w, asmw, 0x20000);
                for (v = 0; v < NVARS; v++) memcpy(vars[v].p, asmv[v], vars[v].n);
            }
        }
    }
    {
        int n = 0, mn = 1 << 30, op;
        for (op = 0; op < 256; op++) if (zs_cpu_ported[op]) { n++; if (perop[op] < (unsigned long)mn) mn = (int)perop[op]; }
        fprintf(stderr, "cpu fuzz: %u cases over %d ported opcodes (min %d per opcode), %u mismatching\n", ran, n, mn, bad);
    }
    return bad;
}
