/* Per-instruction differential test: native x86 execution vs the lifter's emitted C (see itest.py). 32-bit only. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include "itest.h"

#define W_BASE 0x20000000u
#define W_SIZE 4096
static u8 *W;
struct nstate { u32 r[8]; u32 eflags; u32 pad; u64 mm[8]; };
struct nout { u32 r[8]; u32 flags; u32 of; u64 mm[8]; };
struct nstate nin; struct nout nout;
u32 saved_esp, code_ptr, ret_ptr;
extern void native_run(void);
extern char native_ret[];
__asm__(".text\n.globl native_run\nnative_run:\n pushal\n movl %esp, saved_esp\n pushl nin+32\n popfl\n"
        " movq nin+40,%mm0\n movq nin+48,%mm1\n movq nin+56,%mm2\n movq nin+64,%mm3\n movq nin+72,%mm4\n movq nin+80,%mm5\n movq nin+88,%mm6\n movq nin+96,%mm7\n"
        " movl nin+0,%eax\n movl nin+4,%ecx\n movl nin+8,%edx\n movl nin+12,%ebx\n movl nin+20,%ebp\n movl nin+24,%esi\n movl nin+28,%edi\n movl nin+16,%esp\n"
        " jmp *code_ptr\n"
        ".globl native_ret\nnative_ret:\n"
        " movl %eax,nout+0\n movl %ecx,nout+4\n movl %edx,nout+8\n movl %ebx,nout+12\n movl %esp,nout+16\n movl %ebp,nout+20\n movl %esi,nout+24\n movl %edi,nout+28\n"
        " lahf\n movb %ah,nout+32\n seto nout+36\n"
        " movq %mm0,nout+40\n movq %mm1,nout+48\n movq %mm2,nout+56\n movq %mm3,nout+64\n movq %mm4,nout+72\n movq %mm5,nout+80\n movq %mm6,nout+88\n movq %mm7,nout+96\n emms\n"
        " movl saved_esp,%esp\n popal\n ret\n");

lift_state_t LG; double lift_fp[8]; int lift_fptop;
void lift_trap(const char *w, u32 pc) { fprintf(stderr, "trap %s\n", w); exit(2); }
void lift_bad(u32 pc) { exit(2); }
u32 lift_missing(u32 id) { exit(2); }
u32 ext_call(u32 id) { return 0; }

static u32 rs = 0x12345678;
static u32 rnd(void) { rs ^= rs << 13; rs ^= rs >> 17; rs ^= rs << 5; return rs; }
static u64 rnd64(void) { return ((u64)rnd() << 32) | rnd(); }

int main(int argc, char **argv)
{
    int trials = argc > 1 ? atoi(argv[1]) : 24;
    int t, tr, bad_tests = 0, total = 0;
    u8 *cb = mmap(NULL, 4096, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    u8 wsave[W_SIZE];
    W = mmap((void *)W_BASE, 8192, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
    ret_ptr = (u32)(uintptr_t)native_ret;
    for (t = 0; t < nitests; t++) {
        const test_t *T = &itests[t];
        int failed = 0, shown = 0;
        if (getenv("ITEST_V")) { int b; fprintf(stderr, "T%d %s [", t, T->mn); for (b = 0; b < T->len; b++) fprintf(stderr, "%02x", T->code[b]); fprintf(stderr, "]\n"); }
        memcpy(cb, T->code, T->len);
        cb[T->len] = 0xff; cb[T->len + 1] = 0x25; *(u32 *)(cb + T->len + 2) = (u32)(uintptr_t)&ret_ptr;
        code_ptr = (u32)(uintptr_t)cb;
        for (tr = 0; tr < trials; tr++) {
            tstate ls;
            u32 r[8]; u64 mmv[8]; u32 fl, i, regs_used = 0, skip = 0;
            u8 wcmp[W_SIZE];
            for (i = 0; i < 8; i++) r[i] = rnd();
            for (i = 0; i < 8; i++) mmv[i] = rnd64();
            fl = rnd() & 0x8d5;      /* CF PF AF ZF SF OF */
            for (i = 0; i < W_SIZE; i++) W[i] = 1 + rnd() % 255;
            /* esp inside W */
            r[4] = W_BASE + 0x300 + rnd() % 32;
            {
                const memop_t *ms[2] = { &T->m0, &T->m1 };
                int k;
                for (k = 0; k < 2; k++) {
                    const memop_t *m = ms[k];
                    u32 tg, idxv;
                    int sc;
                    if (m->base == -2) continue;
                    if (m->base == -1 && m->idx == -1) continue;     /* absolute, patched in the code */
                    sc = m->scale ? m->scale : 1;
                    tg = W_BASE + 0x200 + (k ? 0x80 : 0) + rnd() % 0x40;
                    if (m->idx >= 0) {
                        idxv = rnd() & 15;
                        if (m->base < 0) { tg = (tg - m->disp) / sc * sc + m->disp; idxv = (tg - m->disp) / sc; }
                        if ((regs_used >> m->idx) & 1) skip = 1;
                        regs_used |= 1u << m->idx;
                        r[m->idx] = idxv;
                    } else idxv = 0;
                    if (m->base >= 0) {
                        if ((regs_used >> m->base) & 1) skip = 1;
                        regs_used |= 1u << m->base;
                        r[m->base] = tg - m->disp - idxv * sc;
                    }
                }
            }
            if (skip) continue;
            if (T->rep) r[1] = rnd() % 6;
            if (!strcmp(T->mn, "div") || !strcmp(T->mn, "idiv")) {
                int sg = T->mn[0] == 'i';
                if (T->sz == 1) r[0] = (r[0] & 0xFFFF0000u) | (sg ? (u16)(s16)((int)(rnd() % 255) - 127) : (rnd() & 0xFF));
                else if (T->sz == 2) { r[0] = (r[0] & 0xFFFF0000u) | (sg ? (u16)(s16)((int)(rnd() % 65535) - 32767) : (rnd() & 0xFFFF)); r[2] = (r[2] & 0xFFFF0000u) | (sg ? ((s16)r[0] < 0 ? 0xFFFF : 0) : 0); }
                else { r[0] = rnd() & 0x7FFFFFFFu; r[2] = 0; }
                if (T->divreg >= 0) {
                    if (T->sz == 4) r[T->divreg] |= 1; else r[T->divreg] |= 1;      /* low byte/word nonzero */
                }
            }
            memcpy(wsave, W, W_SIZE);
            memcpy(nin.r, r, 32); nin.eflags = fl | 2; memcpy(nin.mm, mmv, 64);
            native_run();
            memcpy(wcmp, W, W_SIZE);
            memcpy(W, wsave, W_SIZE);
            memset(&ls, 0, sizeof ls);
            ls.eax = r[0]; ls.ecx = r[1]; ls.edx = r[2]; ls.ebx = r[3]; ls.esp = r[4]; ls.ebp = r[5]; ls.esi = r[6]; ls.edi = r[7];
            ls.cf = fl & 1; ls.pf = (fl >> 2) & 1; ls.af = (fl >> 4) & 1; ls.zf = (fl >> 6) & 1; ls.sf = (fl >> 7) & 1; ls.of = (fl >> 11) & 1;
            for (i = 0; i < 8; i++) ls.mm[i].q = mmv[i];
            T->fn(&ls);
            total++;
            {
                u32 lr[8] = { ls.eax, ls.ecx, ls.edx, ls.ebx, ls.esp, ls.ebp, ls.esi, ls.edi };
                u32 nf = nout.flags, lf = ls.cf | (ls.pf << 2) | (ls.af << 4) | (ls.zf << 6) | (ls.sf << 7);
                u32 lof = ls.of;
                int bad = 0;
                char msg[512]; msg[0] = 0;
                for (i = 0; i < 8; i++) if (lr[i] != nout.r[i]) { bad = 1; sprintf(msg + strlen(msg), " r%u C=%08x nat=%08x(in %08x);", i, lr[i], nout.r[i], r[i]); }
                if ((T->fmask & 1) && ((lf & 1) != (nf & 1))) { bad = 1; strcat(msg, " CF"); }
                if ((T->fmask & 2) && (((lf >> 6) & 1) != ((nf >> 6) & 1))) { bad = 1; strcat(msg, " ZF"); }
                if ((T->fmask & 4) && (((lf >> 7) & 1) != ((nf >> 7) & 1))) { bad = 1; strcat(msg, " SF"); }
                if ((T->fmask & 8) && (lof != (nout.of & 1))) { bad = 1; strcat(msg, " OF"); }
                if ((T->fmask & 16) && (((lf >> 2) & 1) != ((nf >> 2) & 1))) { bad = 1; strcat(msg, " PF"); }
                if ((T->fmask & 32) && (((lf >> 4) & 1) != ((nf >> 4) & 1))) { bad = 1; strcat(msg, " AF"); }
                for (i = 0; i < 8; i++) if (ls.mm[i].q != nout.mm[i]) { bad = 1; sprintf(msg + strlen(msg), " mm%u C=%016llx nat=%016llx;", i, (unsigned long long)ls.mm[i].q, (unsigned long long)nout.mm[i]); }
                if (memcmp(W, wcmp, W_SIZE)) { bad = 1; strcat(msg, " MEM"); }
                if (bad) {
                    failed++;
                    if (shown++ < 1) {
                        int b;
                        printf("FAIL %s [", T->mn);
                        for (b = 0; b < T->len; b++) printf("%02x", T->code[b]);
                        printf("]:%s\n", msg);
                    }
                }
            }
        }
        if (failed) bad_tests++;
    }
    printf("itest: %d encodings, %d runs, %d encodings with mismatches\n", nitests, total, bad_tests);
    return bad_tests != 0;
}
