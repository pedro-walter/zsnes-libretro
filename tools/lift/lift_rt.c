/* Runtime for lifted x86 code: guest arena, dispatch loop, C<->guest calls. */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <sys/mman.h>
#endif
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

#if !defined(_WIN32) && !defined(MAP_FIXED_NOREPLACE)
#define MAP_FIXED_NOREPLACE 0x100000
#endif

int lift_init(void)
{
    void *p;
    if (inited) return 1;
#ifdef _WIN32
    p = VirtualAlloc((void *)(uintptr_t)LIFT_ARENA_BASE, LIFT_ARENA_SIZE, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
#else
    p = mmap((void *)(uintptr_t)LIFT_ARENA_BASE, LIFT_ARENA_SIZE, PROT_READ | PROT_WRITE,
             MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE | MAP_NORESERVE, -1, 0);
#endif
    if (p != (void *)(uintptr_t)LIFT_ARENA_BASE) {
        fprintf(stderr, "lift: cannot map guest arena at 0x%x\n", LIFT_ARENA_BASE);
        return 0;
    }
    memcpy((void *)(uintptr_t)LIFT_DATA_BASE, lift_data_bin, lift_data_bin_len);
    heap_ptr = LIFT_ARENA_BASE + 0x10000u;                 /* guest malloc: below the static data (ROM, caches < WRAM, as in the original's native layout) */
    heap_end = LIFT_DATA_BASE;
    memset(&LG, 0, sizeof LG);
    LG.esp = LIFT_ARENA_BASE + LIFT_ARENA_SIZE - 256;
    inited = 1;
    return 1;
}

void lift_shutdown(void)
{
#ifdef _WIN32
    if (inited) VirtualFree((void *)(uintptr_t)LIFT_ARENA_BASE, 0, MEM_RELEASE);
#else
    if (inited) munmap((void *)(uintptr_t)LIFT_ARENA_BASE, LIFT_ARENA_SIZE);
#endif
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


/* ---------------- differential testing of C overrides ----------------
 * ZL_DUAL=N  : every Nth call of an overridden function runs the C version, snapshots the machine, restores the
 *              pre-call state, runs the original lifted body and compares the whole arena (+registers).  The original's
 *              result is kept.  ZL_DUAL_ONLY=substr restricts it to matching functions, ZL_DUAL_MAX=n caps the reports. */
static struct { u32 lo, hi; } dual_ign[256]; static int ndual_ign;
void zl_dual_ignore(u32 addr, u32 len) { if (ndual_ign < 256) { dual_ign[ndual_ign].lo = addr; dual_ign[ndual_ign].hi = addr + len; ndual_ign++; } }
void zl_dual_ignore_sym(const char *name)
{
    int i;
    for (i = 0; i < lift_sym_count; i++) if (!strcmp(lift_sym_name[i], name)) {
        u32 lo = lift_sym_addr[i], hi = i + 1 < lift_sym_count ? lift_sym_addr[i + 1] : LIFT_DATA_END;
        zl_dual_ignore(lo, hi - lo); return;
    }
    fprintf(stderr, "zl_dual_ignore_sym: no symbol %s\n", name);
}
static const char *symname(u32 a, u32 *off)
{
    int lo = 0, hi = lift_sym_count - 1, r = -1;
    while (lo <= hi) { int m = (lo + hi) / 2; if (lift_sym_addr[m] <= a) { r = m; lo = m + 1; } else hi = m - 1; }
    if (r < 0) { *off = a; return "?"; }
    *off = a - lift_sym_addr[r]; return lift_sym_name[r];
}
static unsigned long dual_calls[4096], dual_diffs[4096];
static int dual_rate = -1, dual_max = 40, dual_reports;
static const char *dual_only;
static u8 *dsnapA, *dsnap0; static size_t dsnapcap;
#define DSTK (64u << 10)
static void dual_summary(void)
{
    int k;
    if (dual_rate <= 0) return;
    for (k = 0; k < lift_ovr_count; k++) if (dual_calls[k]) fprintf(stderr, "DUAL %-28s checked %lu  mismatching %lu\n", lift_ovr_names[k], dual_calls[k], dual_diffs[k]);
}
/* The guest arena has two live regions: the malloc heap [ARENA_BASE, heap_ptr) and the static data [LIFT_DATA_BASE, LIFT_DATA_END)
 * (data above heap, like the native layout the pointer-ordering dependent asm expects).  A "snapshot" is heap | data | top of stack. */
static u32 sn_hu, sn_dl, sn_tot;
static void sn_size(void) { sn_hu = (heap_ptr - LIFT_ARENA_BASE + 15u) & ~15u; sn_dl = (LIFT_DATA_END - LIFT_DATA_BASE + 15u) & ~15u; sn_tot = sn_hu + sn_dl; }
static inline u32 sn_addr(u32 i) { return i < sn_hu ? LIFT_ARENA_BASE + i : LIFT_DATA_BASE + (i - sn_hu); }
static inline u8 sn_cur(u32 i) { return *(volatile u8 *)(uintptr_t)sn_addr(i); }
static u32 prot_lo, prot_hi;     /* the emulator's own C stack lives in the arena: never restore or compare it */
void zl_dual_protect(void *p, unsigned long n) { prot_lo = (u32)(uintptr_t)p; prot_hi = prot_lo + (u32)n; }
static void dual_restore(u8 *arena, const u8 *src)
{
    u32 lo = prot_lo > LIFT_ARENA_BASE ? prot_lo - LIFT_ARENA_BASE : 0, hi = prot_hi > LIFT_ARENA_BASE ? prot_hi - LIFT_ARENA_BASE : 0;
    if (hi > sn_hu) hi = sn_hu;
    if (lo >= hi) memcpy(arena, src, sn_hu);
    else { memcpy(arena, src, lo); memcpy(arena + hi, src + hi, sn_hu - hi); }
    memcpy((void *)(uintptr_t)LIFT_DATA_BASE, src + sn_hu, sn_dl);
}
static void snap_get(u8 *dst, u8 *arena, u8 *stk)
{
    memcpy(dst, arena, sn_hu); memcpy(dst + sn_hu, (void *)(uintptr_t)LIFT_DATA_BASE, sn_dl); memcpy(dst + sn_tot, stk, DSTK);
}
static int ignored(u32 a) { if (a >= prot_lo && a < prot_hi) return 1; int i; for (i = 0; i < ndual_ign; i++) if (a >= dual_ign[i].lo && a < dual_ign[i].hi) return 1; return 0; }
static int dual_busy;
static void dual_run(u32 k)
{
    dual_busy = 1;
    u32 used, i, ndiff = 0;
    size_t need;
    u8 *arena = (u8 *)(uintptr_t)LIFT_ARENA_BASE, *stk = arena + LIFT_ARENA_SIZE - DSTK;
    lift_state_t lg0 = LG, lgA; double fp0[8]; int fptop0 = lift_fptop;
    u32 real = M32(LG.esp), slot = LG.esp;
    sn_size(); used = sn_tot; need = (size_t)used + DSTK;
    if (need > dsnapcap) { free(dsnapA); free(dsnap0); dsnapcap = need + (4u << 20); dsnapA = malloc(dsnapcap); dsnap0 = malloc(dsnapcap); if (!dsnapA || !dsnap0) die("dual: out of memory (%zu)", dsnapcap); }
    memcpy(fp0, lift_fp, sizeof fp0);
    if (getenv("ZL_DUAL_DBG")) fprintf(stderr, "dual %s used=%u cap=%zu d0=%p dA=%p\n", lift_ovr_names[k], used, dsnapcap, (void*)dsnap0, (void*)dsnapA);
    snap_get(dsnap0, arena, stk);
    lift_ovr_fns[k](); LG.esp += 4;          /* the override acts as the function's ret */
    lgA = LG;
    snap_get(dsnapA, arena, stk);
    dual_restore(arena, dsnap0); memcpy(stk, dsnap0 + used, DSTK);
    LG = lg0; memcpy(lift_fp, fp0, sizeof fp0); lift_fptop = fptop0;
    M32(slot) = SENT_ID;
    lift_orig++; lift_run(lift_ovr_ids[k]); lift_orig--;
    M32(slot) = real;
    dual_calls[k]++;
    {   /* compare */
        const char *why[16]; int nw = 0;
        #define CR(r) if (lgA.r != LG.r && nw < 16) { static char b[16][64]; snprintf(b[nw], 64, #r " C=%08x orig=%08x", lgA.r, LG.r); why[nw] = b[nw], nw++; }
        { unsigned f = lift_ovr_flags[k], m = (f & 0x8000) ? (f >> 8) & 0x7F : (f & 2) ? 0 : 0x7F;
          if (m & 1) { CR(eax) } if (m & 2) { CR(ecx) } if (m & 4) { CR(edx) } if (m & 8) { CR(ebx) }
          if (m & 16) { CR(esi) } if (m & 32) { CR(edi) } if (m & 64) { CR(ebp) } }
        CR(esp)
        for (i = 0; i < used; i++) {
            if (dsnapA[i] != sn_cur(i) && !ignored(sn_addr(i))) {
                if (ndiff < 8 && dual_reports < dual_max) {
                    u32 off; const char *n = symname(sn_addr(i), &off);
                    fprintf(stderr, "DUAL %s: %08x (%s+0x%x) C=%02x orig=%02x\n", lift_ovr_names[k], sn_addr(i), n, off, dsnapA[i], sn_cur(i));
                }
                ndiff++;
            }
        }
        for (i = 0; i < DSTK; i++) if (dsnapA[used + i] != stk[i] && LIFT_ARENA_BASE + LIFT_ARENA_SIZE - DSTK + i >= lg0.esp && ndiff++ < 3 && dual_reports < dual_max)
            fprintf(stderr, "DUAL %s: stack +%x C=%02x orig=%02x\n", lift_ovr_names[k], i, dsnapA[used + i], stk[i]);
        if (ndiff || nw) {
            int w;
            dual_diffs[k]++; dual_reports++;
            if (dual_reports <= dual_max) { fprintf(stderr, "DUAL %s: MISMATCH (%u bytes)", lift_ovr_names[k], ndiff); for (w = 0; w < nw; w++) fprintf(stderr, " [%s]", why[w]); fputc('\n', stderr); }
        }
    }
    LG.esp -= 4;     /* give lift_run its return address back */
    dual_busy = 0;
}
/* Tail-style overrides (opcode handlers that end in "jmp next handler"): ZL_DUALT=N [ZL_DUAL_ONLY=substr] checks every Nth call
 * by single-stepping: the cycle counter dh is forced to 0 so that the handler executes exactly one opcode and then returns
 * (the asm endloop "sub dh,cycles; jc ret").  The C version and the original lifted body run from the same state; the
 * real emulation then continues with the C version from the untouched state. */
static unsigned long dualt_calls[8192], dualt_diffs[8192];
static int dualt_rate = -1, dualt_reports;
static void dualt_summary(void)
{
    int k; unsigned long c = 0, d = 0;
    for (k = 0; k < lift_ovr_count && k < 8192; k++) { c += dualt_calls[k]; d += dualt_diffs[k]; if (dualt_diffs[k]) fprintf(stderr, "DUALT %-28s checked %lu mismatching %lu\n", lift_ovr_cnames[k], dualt_calls[k], dualt_diffs[k]); }
    fprintf(stderr, "DUALT total checked %lu mismatching %lu\n", c, d);
}
static int dualt_wanted(u32 k)
{
    if (dualt_rate < 0) {
        const char *e = getenv("ZL_DUALT"); dualt_rate = e ? atoi(e) : 0;
        dual_only = getenv("ZL_DUAL_ONLY");
        if (dualt_rate > 0) atexit(dualt_summary);
    }
    if (dualt_rate <= 0 || lift_orig || dual_busy || !(lift_ovr_flags[k] & 1) || k >= 8192) return 0;
    if (dual_only && !strstr(lift_ovr_names[k], dual_only)) return 0;
    { static unsigned long n[8192]; return (n[k]++ % (unsigned)dualt_rate) == 0; }
}
extern u32 zl_tail_orig(u32 k) __attribute__((weak));
extern u32 zl_sa1_orig(u32 k) __attribute__((weak));
static void dual_tail(u32 k)
{
    u32 oid = lift_ovr_ids[k] ? lift_ovr_ids[k] : ((zl_tail_orig ? zl_tail_orig(k) : 0) ? zl_tail_orig(k) : (zl_sa1_orig ? zl_sa1_orig(k) : 0));
    if (!oid) return;
    dual_busy = 1;
    u32 used, i, ndiff = 0;
    size_t need;
    u8 *arena = (u8 *)(uintptr_t)LIFT_ARENA_BASE, *stk = arena + LIFT_ARENA_SIZE - DSTK;
    lift_state_t lg0 = LG, lgA; double fp0[8]; int fptop0 = lift_fptop;
    u32 real = M32(LG.esp), slot = LG.esp;
    sn_size(); used = sn_tot; need = (size_t)used + DSTK;
    if (need > dsnapcap) { free(dsnapA); free(dsnap0); dsnapcap = need + (4u << 20); dsnapA = malloc(dsnapcap); dsnap0 = malloc(dsnapcap); if (!dsnapA || !dsnap0) die("dual: out of memory (%zu)", dsnapcap); }
    memcpy(fp0, lift_fp, sizeof fp0);
    snap_get(dsnap0, arena, stk);
    LG.edx &= 0xFFFF00FFu;                       /* dh = 0: one opcode, then the endloop returns */
    { u32 nx = lift_ovr_fns[k](); if (nx) { LG.esp = lg0.esp; goto skip; } }
    LG.esp += 4; lgA = LG;
    snap_get(dsnapA, arena, stk);
    dual_restore(arena, dsnap0); memcpy(stk, dsnap0 + used, DSTK);
    LG = lg0; LG.edx &= 0xFFFF00FFu; memcpy(lift_fp, fp0, sizeof fp0); lift_fptop = fptop0;
    M32(slot) = SENT_ID;
    lift_orig++; lift_run(oid); lift_orig--;
    dualt_calls[k]++;
    {
        const char *why[16]; int nw = 0;
        #define CRT(r) if (lgA.r != LG.r && nw < 16) { static char b[16][64]; snprintf(b[nw], 64, #r " C=%08x orig=%08x", lgA.r, LG.r); why[nw] = b[nw], nw++; }
        { static int all = -1; if (all < 0) all = getenv("ZL_DUALT_ALL") != NULL;   /* eax/ecx/ebx high bits are scratch between opcodes */
          if (all) { CRT(eax) CRT(ecx) } lgA.ebx &= 0xFF; LG.ebx &= 0xFF; CRT(edx) CRT(ebx) CRT(esi) CRT(edi) CRT(ebp) CRT(esp) }
        for (i = 0; i < used; i++)
            if (dsnapA[i] != sn_cur(i) && !ignored(sn_addr(i))) {
                if (ndiff < 8 && dualt_reports < dual_max) { u32 off; const char *n = symname(sn_addr(i), &off); fprintf(stderr, "DUALT %s: %08x (%s+0x%x) C=%02x orig=%02x\n", lift_ovr_cnames[k], sn_addr(i), n, off, dsnapA[i], sn_cur(i)); }
                ndiff++;
            }
        if (ndiff || nw) {
            int w; dualt_diffs[k]++; dualt_reports++;
            if (dualt_reports <= dual_max) { fprintf(stderr, "DUALT %s: MISMATCH (%u bytes)", lift_ovr_cnames[k], ndiff); for (w = 0; w < nw; w++) fprintf(stderr, " [%s]", why[w]); fputc('\n', stderr); }
        }
    }
skip:
    dual_restore(arena, dsnap0); memcpy(stk, dsnap0 + used, DSTK);
    LG = lg0; memcpy(lift_fp, fp0, sizeof fp0); lift_fptop = fptop0;
    M32(slot) = real;
    dual_busy = 0;
}
static int dual_wanted(u32 k)
{
    if (dual_rate < 0) {
        const char *e = getenv("ZL_DUAL"); dual_rate = e ? atoi(e) : 0;
        dual_only = getenv("ZL_DUAL_ONLY"); if (getenv("ZL_DUAL_MAX")) dual_max = atoi(getenv("ZL_DUAL_MAX"));
        if (dual_rate > 0) atexit(dual_summary);
    }
    if (dual_rate <= 0 || lift_orig || dual_busy || (lift_ovr_flags[k] & 1)) return 0;
    if (dual_only && !strstr(lift_ovr_names[k], dual_only)) return 0;
    { static unsigned long n[4096]; return (n[k]++ % (unsigned)dual_rate) == 0; }
}

static unsigned long long prof[128];
#define PROFN 8192
static struct { u32 pc; unsigned long long n; } prof2[PROFN];
static void prof_pc(u32 pc) { u32 h = (pc * 2654435761u) >> 19; for (;;) { if (prof2[h].pc == pc || !prof2[h].n) { prof2[h].pc = pc; prof2[h].n++; return; } h = (h + 1) & (PROFN - 1); } }
static const char *text_name(u32 pc, u32 *off)
{
    int lo = 0, hi = lift_text_count - 1, r = -1;
    while (lo <= hi) { int m = (lo + hi) / 2; if (lift_text_id[m] <= pc) { r = m; lo = m + 1; } else hi = m - 1; }
    if (r < 0) { *off = 0; return "?"; }
    *off = pc - lift_text_id[r]; return lift_text_name[r];
}
static int prof_cmp(const void *a, const void *b) { unsigned long long x = ((const typeof(prof2[0]) *)a)->n, y = ((const typeof(prof2[0]) *)b)->n; return x < y ? 1 : x > y ? -1 : 0; }
__attribute__((destructor)) static void prof_dump(void)
{
    const char *e = getenv("LIFT_PROF"); if (!e) return;
    for (int i = 0; i < 128; i++) if (prof[i]) fprintf(stderr, "PROF %s %llu\n", oname((u32)i << 21), prof[i]);
    if (atoi(e) >= 2) { int i; qsort(prof2, PROFN, sizeof prof2[0], prof_cmp); for (i = 0; i < (getenv("LIFT_PROF_TOP") ? atoi(getenv("LIFT_PROF_TOP")) : 60) && i < PROFN && prof2[i].n; i++) { u32 off; const char *n = text_name(prof2[i].pc, &off); fprintf(stderr, "PROFPC %s+0x%x %llu\n", n, off, prof2[i].n); } }
}
int lift_orig, lift_once;
int zl_ovr_off(const char *cname)
{
    static const char *e; static int init;
    char buf[512], *t, *sv;
    if (!init) { e = getenv("LIFT_NOOVR"); init = 1; }
    if (!e) return 0;
    if (!strcmp(e, "all")) return 1;
    strncpy(buf, e, sizeof buf - 1); buf[sizeof buf - 1] = 0;
    for (t = strtok_r(buf, ",", &sv); t; t = strtok_r(NULL, ",", &sv)) if (strstr(cname, t)) return 1;
    return 0;
}
static unsigned char ovr_dis[8192];
/* code id of an overridden asm label -> override index (so dispatch can skip the lifted guard) */
#define OVH 4096
static short ovr_tab[OVH];
static int ovr_built;
static void ovr_build(void)
{
    int k;
    memset(ovr_tab, 0xFF, sizeof ovr_tab);
    for (k = 0; k < lift_ovr_count; k++) {
        u32 id = lift_ovr_ids[k], h;
        if (!id) continue;
        h = (id * 2654435761u) >> 20;
        while (ovr_tab[h] >= 0) h = (h + 1) & (OVH - 1);
        ovr_tab[h] = (short)k;
    }
    for (k = 0; k < lift_ovr_count && k < 8192; k++) ovr_dis[k] = (unsigned char)zl_ovr_off(lift_ovr_cnames[k]);
    ovr_built = 1;
}
static inline int ovr_find(u32 id)
{
    u32 h = (id * 2654435761u) >> 20;
    for (;;) { short k = ovr_tab[h]; if (k < 0) return -1; if (lift_ovr_ids[k] == id) return k; h = (h + 1) & (OVH - 1); }
}
static int prof_on;
__attribute__((constructor)) static void prof_init(void) { const char *e = getenv("LIFT_PROF"); prof_on = e && atoi(e) >= 2; }
/* call a guest function with the registers in LG (results stay in LG) */
void zl_mcall(u32 id) { LG.esp -= 4; M32(LG.esp) = SENT_ID; lift_run(id); }
void zl_orig(u32 id) { LG.esp -= 4; M32(LG.esp) = SENT_ID; lift_orig++; lift_run(id); lift_orig--; }
u32 lift_run(u32 pc)
{
    for (;;) {
        u32 o = pc >> 21;
        if (o == (SENT_ID >> 21)) return pc;
        if (o == 0x7C) {      /* C override of an asm function (register ABI through LG); acts as the function's ret */
            { u32 k = pc & 0x1FFFFF, nx;
              if (!ovr_built) ovr_build();
              if (ovr_dis[k]) { if (!lift_ovr_ids[k]) die("cannot disable table-installed override %s", lift_ovr_cnames[k]); lift_once = 1; pc = lift_ovr_ids[k]; continue; }
              if (dualt_wanted(k)) dual_tail(k);
              if (dual_wanted(k)) { dual_run(k); nx = 0; } else nx = lift_ovr_fns[k]();
              if (nx) { pc = nx; continue; } }
            pc = M32(LG.esp); LG.esp += 4;
            continue;
        }
        if (o == 0x7F) {      /* indirect call/jump to a foreign function: args after the return address */
            LG.eax = ext_invoke(pc, (const u32 *)(uintptr_t)(LG.esp + 4));
            pc = M32(LG.esp); LG.esp += 4;
            continue;
        }
        if (!lift_orig && !lift_once && lift_ovr_count) {
            int k;
            if (!ovr_built) ovr_build();
            k = ovr_find(pc);
            if (k >= 0 && !ovr_dis[k]) {
                u32 nx;
                if (dualt_wanted((u32)k)) dual_tail((u32)k);
                if (dual_wanted((u32)k)) { dual_run((u32)k); nx = 0; } else nx = lift_ovr_fns[k]();
                if (nx) { pc = nx; continue; }
                pc = M32(LG.esp); LG.esp += 4;
                continue;
            }
        }
        ring[ringi++ & 63] = pc;
        if (o >= 128 || !lift_objtab[o]) die("dispatch into non-lifted code 0x%x (%s+0x%x)", pc, oname(pc), pc & 0x1FFFFF);
        ring[ringi++ & 63] = pc;
        prof[o]++; if (prof_on) prof_pc(pc);
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
static u8 *baseline, *baseline_data;
static u32 base_used, data_len;
#define STACK_SAVE (64u << 10)
#define DATA_PAGE (1u << 31)
static u8 *stack_base_copy;

void lift_baseline(void)
{
    if (baseline || !inited) return;
    base_used = (heap_ptr - LIFT_ARENA_BASE + PG - 1) & ~(PG - 1);
    data_len = (LIFT_DATA_END - LIFT_DATA_BASE + PG - 1) & ~(PG - 1);
    baseline = malloc(base_used); baseline_data = malloc(data_len);
    stack_base_copy = malloc(STACK_SAVE);
    memcpy(baseline, (void *)(uintptr_t)LIFT_ARENA_BASE, base_used);
    memcpy(baseline_data, (void *)(uintptr_t)LIFT_DATA_BASE, data_len);
    memcpy(stack_base_copy, (void *)(uintptr_t)(LIFT_ARENA_BASE + LIFT_ARENA_SIZE - STACK_SAVE), STACK_SAVE);
}

size_t lift_state_size(void) { return STATE_MAX; }

typedef struct { u32 magic, version, heap_ptr, used_pages, stack_pages, base_used; lift_state_t lg; double fp[8]; int fptop; } state_hdr_t;

int lift_state_save(void *out, size_t cap)
{
    u8 *o = out, *p;
    state_hdr_t *h = (state_hdr_t *)o;
    u32 i, np = 0, sp = 0, cur_used = (heap_ptr - LIFT_ARENA_BASE + PG - 1) & ~(PG - 1);
    u8 *arena = (u8 *)(uintptr_t)LIFT_ARENA_BASE, *data = (u8 *)(uintptr_t)LIFT_DATA_BASE;
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
    for (i = 0; i < data_len / PG; i++) {
        if (memcmp(data + i * PG, baseline_data + i * PG, PG)) {
            if ((size_t)(p - o) + 4 + PG > cap) return 0;
            *(u32 *)p = DATA_PAGE | i; memcpy(p + 4, data + i * PG, PG); p += 4 + PG; np++;
        }
    }
    for (i = 0; i < STACK_SAVE / PG; i++) {
        if (memcmp(stk + i * PG, stack_base_copy + i * PG, PG)) {
            if ((size_t)(p - o) + 4 + PG > cap) return 0;
            *(u32 *)p = i; memcpy(p + 4, stk + i * PG, PG); p += 4 + PG; sp++;
        }
    }
    h->magic = 0x4C46535Au; h->version = 2; h->heap_ptr = heap_ptr; h->used_pages = np; h->stack_pages = sp; h->base_used = base_used;
    h->lg = LG; memcpy(h->fp, lift_fp, sizeof lift_fp); h->fptop = lift_fptop;
    return (int)(p - o);
}

int lift_state_load(const void *in, size_t n)
{
    const u8 *p = in;
    const state_hdr_t *h = (const state_hdr_t *)p;
    u8 *arena = (u8 *)(uintptr_t)LIFT_ARENA_BASE, *data = (u8 *)(uintptr_t)LIFT_DATA_BASE;
    u8 *stk = arena + LIFT_ARENA_SIZE - STACK_SAVE;
    u32 i;
    if (!baseline || n < sizeof *h || h->magic != 0x4C46535Au || h->version != 2 || h->base_used != base_used) return 0;
    {   /* validate */
        const u8 *q = p + sizeof *h;
        u32 cnt = h->used_pages + h->stack_pages;
        if ((size_t)(q - p) + (size_t)cnt * (4 + PG) > n) return 0;
    }
    memcpy(arena, baseline, base_used);
    memcpy(data, baseline_data, data_len);
    if (h->heap_ptr > heap_ptr) memset(arena + base_used, 0, ((h->heap_ptr - LIFT_ARENA_BASE + PG - 1) & ~(PG - 1)) - base_used);
    memcpy(stk, stack_base_copy, STACK_SAVE);
    p += sizeof *h;
    for (i = 0; i < h->used_pages; i++, p += 4 + PG) {
        u32 idx = *(const u32 *)p;
        if (idx & DATA_PAGE) memcpy(data + (size_t)(idx & ~DATA_PAGE) * PG, p + 4, PG);
        else memcpy(arena + (size_t)idx * PG, p + 4, PG);
    }
    for (i = 0; i < h->stack_pages; i++, p += 4 + PG) memcpy(stk + (size_t)*(const u32 *)p * PG, p + 4, PG);
    heap_ptr = h->heap_ptr;
    LG = h->lg; memcpy(lift_fp, h->fp, sizeof lift_fp); lift_fptop = h->fptop;
    return 1;
}
