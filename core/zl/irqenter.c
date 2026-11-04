/*
 * switchtonmi / switchtovirq / switchtovirqret / NMIemulmode / IRQemulmode (cpu/irq.asm): push the return state on the 65816 stack and
 * load the interrupt vector.  Tail-style overrides: return value = code id to continue at (execloop), 0 = ret.
 * The stack pushes go through membank0w8 (register ABI) exactly like the asm does, with the same leftover register values.
 */
#include "zl.h"
#include <stdint.h>
#include <string.h>
#define A(s) ((u32)ZO_##s)
#define L8(a)  (*(volatile u8 *)(uintptr_t)(u32)(a))
static inline u32 L16(u32 a) { u16 v; memcpy(&v, (const void *)(uintptr_t)a, 2); return v; }
static inline u32 L32(u32 a) { u32 v; memcpy(&v, (const void *)(uintptr_t)a, 4); return v; }
static inline void S16(u32 a, u32 v) { u16 x = (u16)v; memcpy((void *)(uintptr_t)a, &x, 2); }
static inline void S32(u32 a, u32 v) { memcpy((void *)(uintptr_t)a, &v, 4); }
#define EAX LG.eax
#define EBX LG.ebx
#define ECX LG.ecx
#define EDX LG.edx
#define ESI LG.esi

static void makedl(void)
{
    u8 dl = (u8)EDX & 0x3C;
    if (L32(A(flagnz)) & 0x18000) dl |= 0x80;
    if ((L32(A(flagnz)) & 0xFFFF) == 0) dl |= 0x02;
    if (L32(A(flagc)) & 0xFF) dl |= 0x01;
    if (L32(A(flago)) & 0xFF) dl |= 0x40;
    EDX = (EDX & ~0xFFu) | dl;
}
static void push8(u8 v)
{
    EAX = (EAX & ~0xFFu) | v;
    zl_mcall(ZID_membank0w8);
    u32 cx = ((ECX & 0xFFFF) - 1) & 0xFFFF;
    cx &= L16(A(stackand));
    cx |= L16(A(stackor));
    ECX = (ECX & 0xFFFF0000u) | (cx & 0xFFFF);
}

/* emu: 8-bit emulation mode (no PB push); vec: guest address of the vector word; ret: switchtovirqret in native mode */
u32 zl_NMIemulmode(void);
u32 zl_IRQemulmode(void);
static u32 enter(int emu, u32 vec, int ret_native)
{
    EBX = ESI - L32(A(initaddrl));
    S16(A(xpc), EBX);
    if (!emu && vec == A(nmiv)) EBX = 0;                 /* switchtonmi clears ebx here */
    ECX = (ECX & 0xFFFF0000u) | L16(A(xs));
    if (!emu) push8(L8(A(xpb)));
    push8(L8(A(xpc) + 1));
    push8(L8(A(xpc)));
    makedl();
    push8((u8)EDX);
    S16(A(xs), ECX);
    EBX &= ~0xFF00u;
    if (emu) { L8(A(xpb)) = 0; EBX = (EBX & ~0xFFu) | 0; }
    else { EBX = (EBX & ~0xFFu) | L8(A(xirqb)); L8(A(xpb)) = (u8)EBX; }
    EAX = L16(vec);
    S16(A(xpc), EAX);
    EDX = (EDX & ~0xFFu) | (((u8)EDX & 0xF3) | 0x04);
    if (EAX & 0x8000) ESI = L32(A(snesmmap) + EBX * 4); else ESI = L32(A(snesmap2) + EBX * 4);
    S32(A(initaddrl), ESI);
    ESI += EAX;
    return ret_native ? 0 : ZID_execloop;
}

u32 zl_switchtonmi(void)
{
    L8(A(curnmi)) = 1;
    if (L8(A(xe)) & 1) return zl_NMIemulmode();
    return enter(0, A(nmiv), 0);
}
u32 zl_NMIemulmode(void) { return enter(1, A(nmiv8), 0); }
u32 zl_switchtovirq(void)
{
    L8(A(irqon)) = 0x80;
    if (L8(A(xe)) & 1) return zl_IRQemulmode();
    return enter(0, A(irqv), 0);
}
u32 zl_switchtovirqret(void)
{
    L8(A(irqon)) = 0x80;
    if (L8(A(xe)) & 1) return zl_IRQemulmode();
    return enter(0, A(irqv), 1);
}
u32 zl_IRQemulmode(void) { return enter(1, A(irqv8), 0); }
