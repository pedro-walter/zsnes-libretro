#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <link.h>
#include <dlfcn.h>
#include <signal.h>
#include <setjmp.h>
#include "diffimg.h"

extern void zs_mcall(void *fn);
extern uint32_t zs_mcall_fn;
#define R zs_R
#define MAXR 64
typedef struct { uint8_t *p; size_t n; uint8_t *s0, *sA; size_t cap; } reg_t;
/* all mutable harness state lives on the heap so restoring the core image never touches it */
typedef struct {
    reg_t img[8]; int nimg;
    reg_t ext[MAXR]; int next_;
    uint8_t *xlo[64], *xhi[64]; int nx;
    unsigned long ops, bad_total, skipped;
    sigjmp_buf jb; volatile int in_asm; int guard;
    int pr[256]; const char *names[256]; int nn; int ready;
} st_t;
static st_t *S;
#define img S->img
#define nimg S->nimg
#define ext S->ext
#define next_ S->next_
#define xlo S->xlo
#define xhi S->xhi
#define nx S->nx
#define ops S->ops
#define bad_total S->bad_total
#define skipped S->skipped
#define jb S->jb
#define in_asm S->in_asm
#define guard S->guard
void (*di_region_hook)(void);
static int phdr_cb(struct dl_phdr_info *info, size_t sz, void *data)
{
    Dl_info di; int i;
    (void)sz; (void)data;
    if (!dladdr((void *)di_run, &di) || (void *)info->dlpi_addr != di.dli_fbase) return 0;
    for (i = 0; i < info->dlpi_phnum; i++) {
        const ElfW(Phdr) *ph = &info->dlpi_phdr[i];
        if (ph->p_type == PT_LOAD && (ph->p_flags & PF_W)) {
            uintptr_t lo = info->dlpi_addr + ph->p_vaddr, hi = lo + ph->p_memsz;
            FILE *f = fopen("/proc/self/maps", "r"); char line[512];
            while (f && fgets(line, sizeof line, f)) {
                unsigned long a, b; char perm[8];
                if (sscanf(line, "%lx-%lx %7s", &a, &b, perm) == 3 && perm[0] == 'r' && perm[1] == 'w' && b > lo && a < hi && nimg < 8) {
                    uintptr_t s0 = a > lo ? a : lo, s1 = b < hi ? b : hi;
                    img[nimg].p = (uint8_t *)s0; img[nimg].n = s1 - s0; nimg++;
                }
            }
            if (f) fclose(f);
        }
    }
    return 1;
}
void di_exclude(void *p, size_t n) { if (!S) S = calloc(1, sizeof *S); xlo[nx] = p; xhi[nx] = (uint8_t *)p + n; nx++; }
static int excluded(uint8_t *a) { int i; for (i = 0; i < nx; i++) if (a >= xlo[i] && a < xhi[i]) return 1; return 0; }
void di_ext(void *p, size_t n)
{
    int i;
    if (!S) S = calloc(1, sizeof *S);
    for (i = 0; i < next_; i++) if (ext[i].p == (uint8_t *)p && ext[i].n == n) return;
    if (next_ < MAXR) { memset(&ext[next_], 0, sizeof ext[0]); ext[next_].p = p; ext[next_].n = n; next_++; }
}
static void sizeup(reg_t *r)
{
    if (r->cap < r->n) { free(r->s0); free(r->sA); r->s0 = malloc(r->n); r->sA = malloc(r->n); r->cap = r->n; }
}
static int maxbad = 8, maxpr = 6;
static void rep(void) { di_report(); }
static void setup(void)
{
    int i;
    if (!S) S = calloc(1, sizeof *S);
    if (S->ready) return;
    S->ready = 1;
    if (getenv("DI_BAD")) maxbad = atoi(getenv("DI_BAD"));
    if (getenv("DI_PR")) maxpr = atoi(getenv("DI_PR"));
    dl_iterate_phdr(phdr_cb, NULL);
    if (getenv("DI_DBG")) { int q; for (q = 0; q < nimg; q++) fprintf(stderr, "img seg %d: %p +%zx\n", q, (void *)img[q].p, img[q].n); }
    for (i = 0; i < nimg; i++) sizeup(&img[i]);
    di_exclude(&zs_mcall_fn, 4); di_exclude(&S, sizeof S);
    atexit(rep);
}
static void sigh(int s) { if (in_asm) { in_asm = 0; siglongjmp(jb, 1); } signal(s, SIG_DFL); raise(s); }
void di_guard(int on)
{
    struct sigaction sa;
    setup();
    guard = on;
    if (on) { memset(&sa, 0, sizeof sa); sa.sa_handler = sigh; sigaction(SIGSEGV, &sa, NULL); }
}
static void save(int which) /* 0 = s0, 1 = sA */
{
    int i;
    for (i = 0; i < nimg; i++) memcpy(which ? img[i].sA : img[i].s0, img[i].p, img[i].n);
    for (i = 0; i < next_; i++) { sizeup(&ext[i]); memcpy(which ? ext[i].sA : ext[i].s0, ext[i].p, ext[i].n); }
}
static void load(int which)
{
    int i;
    for (i = 0; i < nimg; i++) memcpy(img[i].p, which ? img[i].sA : img[i].s0, img[i].n);
    for (i = 0; i < next_; i++) memcpy(ext[i].p, which ? ext[i].sA : ext[i].s0, ext[i].n);
}
unsigned long di_total_bad(void) { return S ? bad_total : 0; }
unsigned long di_total_ops(void) { return S ? ops : 0; }
void di_report(void) { if (!S) return; fprintf(stderr, "imgdiff: %lu ops, %lu mismatching, %lu skipped\n", ops, bad_total, skipped); }

int di_run(const char *name, void *afn, void (*cfn)(void), unsigned regmask)
{
    zregs_t Rin, Rasm;
    int bad = 0, i, id;
    size_t k;
    unsigned long sv_ops, sv_bad, sv_skip;
    setup();
    if (di_region_hook) di_region_hook();
    for (id = 0; id < S->nn; id++) if (S->names[id] == name) break;
    if (id == S->nn && S->nn < 255) S->names[S->nn++] = name;
    Rin = R;
    save(0);
    if (guard) {
        if (sigsetjmp(jb, 1)) { sv_skip = skipped + 1; load(0); skipped = sv_skip; R = Rin; return 0; }
        in_asm = 1;
    }
    zs_mcall(afn);
    in_asm = 0;
    Rasm = R;
    save(1);
    load(0);
    R = Rin;
    cfn();
    ops++;
    for (i = 0; i < nimg && bad < maxbad; i++)
        for (k = 0; k < img[i].n && bad < maxbad; k++) {
            uint8_t *a = img[i].p + k;
            if (*a == img[i].sA[k] || excluded(a)) continue;
            { Dl_info di; const char *nm = "?"; long off = 0; unsigned long lo = 0;
              if (dladdr((void *)di_run, &di)) lo = (unsigned long)(a - (uint8_t *)di.dli_fbase);
              if (dladdr(a, &di) && di.dli_sname) { nm = di.dli_sname; off = a - (uint8_t *)di.dli_saddr; }
              if (S->pr[id]++ < maxpr) fprintf(stderr, "imgdiff[%s]: %s+%ld (so+0x%lx)  C=%02x asm=%02x\n", name, nm, off, lo, *a, img[i].sA[k]); }
            bad++;
        }
    for (i = 0; i < next_ && bad < maxbad; i++)
        for (k = 0; k < ext[i].n && bad < maxbad; k++) {
            uint8_t *a = ext[i].p + k;
            if (*a == ext[i].sA[k]) continue;
            if (S->pr[id]++ < maxpr) fprintf(stderr, "imgdiff[%s]: ext region %d (%p) +%zu  C=%02x asm=%02x pre=%02x\n", name, i, (void *)ext[i].p, k, *a, ext[i].sA[k], ext[i].s0[k]);
            bad++;
        }
    { const uint32_t *c = &R.eax, *a = &Rasm.eax;
      for (i = 0; i < 7; i++) if ((regmask >> i) & 1) if (c[i] != a[i]) {
          static const char *rn[] = { "eax", "ecx", "edx", "ebx", "esi", "edi", "ebp" };
          if (S->pr[id]++ < maxpr) fprintf(stderr, "imgdiff[%s]: reg %s C=%08x asm=%08x (in=%08x)\n", name, rn[i], c[i], a[i], (&Rin.eax)[i]);
          bad++;
      } }
    if (bad) {
        bad_total++;
        sv_ops = ops; sv_bad = bad_total; sv_skip = skipped;
        load(1); R = Rasm;
        ops = sv_ops; bad_total = sv_bad; skipped = sv_skip;
    }
    return bad;
}
