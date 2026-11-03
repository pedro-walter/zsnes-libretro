/* Runtime for lifted x86 code: guest arena, dispatch loop, C<->guest calls. */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include "lift.h"
#include "lift_syms.h"

lift_state_t LG;
double lift_fp[8]; int lift_fptop;
extern const unsigned char lift_data_bin[];
extern const unsigned int lift_data_bin_len;
extern void *const lift_ext_fns[];
extern const char *const lift_ext_names[];
extern const char *const lift_missing_names[];
extern const int lift_ext_count;

#define SENT_ID (0x7Eu << 21)
#define STACK_SIZE (4u << 20)
static u32 heap_ptr, heap_end;
static int inited;

#ifndef MAP_FIXED_NOREPLACE
#define MAP_FIXED_NOREPLACE 0x100000
#endif

int lift_init(void)
{
    void *p;
    if (inited) return 1;
    p = mmap((void *)(uintptr_t)LIFT_ARENA_BASE, LIFT_ARENA_SIZE, PROT_READ | PROT_WRITE,
             MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE | MAP_NORESERVE, -1, 0);
    if (p != (void *)(uintptr_t)LIFT_ARENA_BASE) {
        fprintf(stderr, "lift: cannot map guest arena at 0x%x\n", LIFT_ARENA_BASE);
        return 0;
    }
    memcpy(p, lift_data_bin, lift_data_bin_len);
    heap_ptr = (LIFT_DATA_END + 0xFFFFu) & ~0xFFFFu;
    heap_end = LIFT_ARENA_BASE + LIFT_ARENA_SIZE - STACK_SIZE;
    memset(&LG, 0, sizeof LG);
    LG.esp = LIFT_ARENA_BASE + LIFT_ARENA_SIZE - 256;
    inited = 1;
    return 1;
}

void lift_shutdown(void)
{
    if (inited) munmap((void *)(uintptr_t)LIFT_ARENA_BASE, LIFT_ARENA_SIZE);
    inited = 0;
}

/* guest heap */
void *zl_malloc(unsigned n)
{
    u32 a = (heap_ptr + 31u) & ~31u;
    if (!n) n = 1;
    if (a + n > heap_end || a + n < a) return 0;
    heap_ptr = a + n;
    return (void *)(uintptr_t)a;
}
void zl_free(void *p) { (void)p; }
void *zl_calloc(unsigned n, unsigned m) { return zl_malloc(n * m); }
void *zl_realloc(void *p, unsigned n) { void *q = zl_malloc(n); if (q && p) memcpy(q, p, n); return q; }   /* old size unknown: callers only grow */
char *zl_strdup(const char *s) { size_t n = strlen(s) + 1; char *d = zl_malloc((unsigned)n); if (d) memcpy(d, s, n); return d; }
int zl_putchar(int c) { return putchar(c); }
int zl_getchar(void) { return -1; }

__attribute__((weak)) void zs_fatal(void) {}   /* platform layer: leave the guest and keep the frontend alive */
static u32 ring[64]; static unsigned ringi;
static const char *oname(u32 pc) { u32 o = pc >> 21; return o < 128 && lift_objname[o] ? lift_objname[o] : "?"; }
static void die(const char *fmt, ...) __attribute__((noreturn));
#include <stdarg.h>
static void die(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "lift: fatal: ");
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
    { unsigned i; fprintf(stderr, "recent entries (old->new):\n"); for (i = 0; i < 64; i++) { u32 p = ring[(ringi + i) & 63]; fprintf(stderr, "  %08x %s+0x%x\n", p, oname(p), p & 0x1FFFFF); } }
    fprintf(stderr, "esp=%08x eax=%08x ebx=%08x ecx=%08x edx=%08x esi=%08x edi=%08x ebp=%08x\n", LG.esp, LG.eax, LG.ebx, LG.ecx, LG.edx, LG.esi, LG.edi, LG.ebp);
    zs_fatal();
    abort();
}

u32 lift_missing(u32 id) { die("call to unresolved/skipped code 0x%x (%s)", id, oname(id)); }
void lift_trap(const char *what, u32 pc) { die("trap: %s at %s+0x%x", what, oname(pc), pc & 0x1FFFFF); }
void lift_bad(u32 pc) { die("bad dispatch target 0x%x (%s+0x%x)", pc, oname(pc), pc & 0x1FFFFF); }

typedef u32 (*ext8_t)(uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t);
static u32 ext_invoke(u32 id, const u32 *a)
{
    u32 idx = id & 0x1FFFFF;
    void *f;
    if (idx >= (u32)lift_ext_count) die("bad foreign id 0x%x", id);
    f = lift_ext_fns[idx];
    if (!f) die("foreign function %s not linked", lift_ext_names[idx]);
    static int tr = -1; if (tr < 0) tr = getenv("LIFT_TRACE_EXT") != 0;
    if (tr) { u32 r = ((ext8_t)f)(a[0], a[1], a[2], a[3], a[4], a[5], a[6], a[7]); fprintf(stderr, "ext %s(%x,%x,%x)=%x\n", lift_ext_names[idx], a[0], a[1], a[2], r); return r; }
    return ((ext8_t)f)(a[0], a[1], a[2], a[3], a[4], a[5], a[6], a[7]);
}
u32 ext_call(u32 id) { return ext_invoke(id, (const u32 *)(uintptr_t)LG.esp); }

u32 lift_run(u32 pc)
{
    for (;;) {
        u32 o = pc >> 21;
        if (o == (SENT_ID >> 21)) return pc;
        if (o == 0x7F) {      /* indirect call/jump to a foreign function: args after the return address */
            LG.eax = ext_invoke(pc, (const u32 *)(uintptr_t)(LG.esp + 4));
            pc = M32(LG.esp); LG.esp += 4;
            continue;
        }
        ring[ringi++ & 63] = pc;
        if (o >= 128 || !lift_objtab[o]) die("dispatch into non-lifted code 0x%x (%s+0x%x)", pc, oname(pc), pc & 0x1FFFFF);
        ring[ringi++ & 63] = pc;
        pc = lift_objtab[o](pc);
    }
}

/* run guest function `id` to completion; registers of the caller state are preserved */
u32 lift_call(u32 id)
{
    lift_state_t save = LG;
    u32 r;
    LG.esp -= 4; M32(LG.esp) = SENT_ID;
    lift_run(id);
    r = LG.eax;
    LG = save;
    return r;
}

/* same, but with explicit register in/out (zs_R layout: eax ecx edx ebx esi edi ebp) */
void lift_call_regs(u32 id, u32 *r)
{
    lift_state_t save = LG;
    LG.eax = r[0]; LG.ecx = r[1]; LG.edx = r[2]; LG.ebx = r[3]; LG.esi = r[4]; LG.edi = r[5]; LG.ebp = r[6];
    LG.esp -= 4; M32(LG.esp) = SENT_ID;
    lift_run(id);
    r[0] = LG.eax; r[1] = LG.ecx; r[2] = LG.edx; r[3] = LG.ebx; r[4] = LG.esi; r[5] = LG.edi; r[6] = LG.ebp;
    LG = save;
}

/* ---------------- save states ----------------
 * The guest is always suspended at the same point between frames (inside sem_sleep), so its whole machine state is the
 * arena + LG + the x87 stack.  A state stores only the 4K pages that differ from a baseline captured after the first
 * frame, plus the registers; loading restores the baseline first. */
#define PG 4096u
#define STATE_MAX (24u << 20)
static u8 *baseline;
static u32 base_used;
#define STACK_SAVE (64u << 10)
static u8 *stack_base_copy;

void lift_baseline(void)
{
    if (baseline || !inited) return;
    base_used = (heap_ptr - LIFT_ARENA_BASE + PG - 1) & ~(PG - 1);
    baseline = malloc(base_used);
    stack_base_copy = malloc(STACK_SAVE);
    memcpy(baseline, (void *)(uintptr_t)LIFT_ARENA_BASE, base_used);
    memcpy(stack_base_copy, (void *)(uintptr_t)(LIFT_ARENA_BASE + LIFT_ARENA_SIZE - STACK_SAVE), STACK_SAVE);
}

size_t lift_state_size(void) { return STATE_MAX; }

typedef struct { u32 magic, version, heap_ptr, used_pages, stack_pages, base_used; lift_state_t lg; double fp[8]; int fptop; } state_hdr_t;

int lift_state_save(void *out, size_t cap)
{
    u8 *o = out, *p;
    state_hdr_t *h = (state_hdr_t *)o;
    u32 i, np = 0, sp = 0, cur_used = (heap_ptr - LIFT_ARENA_BASE + PG - 1) & ~(PG - 1);
    u8 *arena = (u8 *)(uintptr_t)LIFT_ARENA_BASE;
    u8 *stk = arena + LIFT_ARENA_SIZE - STACK_SAVE;
    if (!baseline || cap < sizeof *h) return 0;
    memset(h, 0, sizeof *h);
    p = o + sizeof *h;
    for (i = 0; i < cur_used / PG; i++) {
        int diff = i >= base_used / PG;
        if (!diff) diff = memcmp(arena + i * PG, baseline + i * PG, PG) != 0;
        if (diff) {
            if ((size_t)(p - o) + 4 + PG > cap) return 0;
            *(u32 *)p = i; memcpy(p + 4, arena + i * PG, PG); p += 4 + PG; np++;
        }
    }
    for (i = 0; i < STACK_SAVE / PG; i++) {
        if (memcmp(stk + i * PG, stack_base_copy + i * PG, PG)) {
            if ((size_t)(p - o) + 4 + PG > cap) return 0;
            *(u32 *)p = i; memcpy(p + 4, stk + i * PG, PG); p += 4 + PG; sp++;
        }
    }
    h->magic = 0x4C46535Au; h->version = 1; h->heap_ptr = heap_ptr; h->used_pages = np; h->stack_pages = sp; h->base_used = base_used;
    h->lg = LG; memcpy(h->fp, lift_fp, sizeof lift_fp); h->fptop = lift_fptop;
    return (int)(p - o);
}

int lift_state_load(const void *in, size_t n)
{
    const u8 *p = in;
    const state_hdr_t *h = (const state_hdr_t *)p;
    u8 *arena = (u8 *)(uintptr_t)LIFT_ARENA_BASE;
    u8 *stk = arena + LIFT_ARENA_SIZE - STACK_SAVE;
    u32 i;
    if (!baseline || n < sizeof *h || h->magic != 0x4C46535Au || h->version != 1 || h->base_used != base_used) return 0;
    {   /* validate */
        const u8 *q = p + sizeof *h;
        u32 cnt = h->used_pages + h->stack_pages;
        if ((size_t)(q - p) + (size_t)cnt * (4 + PG) > n) return 0;
    }
    memcpy(arena, baseline, base_used);
    if (h->heap_ptr > heap_ptr) memset(arena + base_used, 0, ((h->heap_ptr - LIFT_ARENA_BASE + PG - 1) & ~(PG - 1)) - base_used);
    memcpy(stk, stack_base_copy, STACK_SAVE);
    p += sizeof *h;
    for (i = 0; i < h->used_pages; i++, p += 4 + PG) memcpy(arena + (size_t)*(const u32 *)p * PG, p + 4, PG);
    for (i = 0; i < h->stack_pages; i++, p += 4 + PG) memcpy(stk + (size_t)*(const u32 *)p * PG, p + 4, PG);
    heap_ptr = h->heap_ptr;
    LG = h->lg; memcpy(lift_fp, h->fp, sizeof lift_fp); lift_fptop = h->fptop;
    return 1;
}
