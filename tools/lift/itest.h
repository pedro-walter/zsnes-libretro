#ifndef ITEST_H
#define ITEST_H
#include "lift.h"
typedef struct { u32 eax, ecx, edx, ebx, esp, ebp, esi, edi; u8 cf, zf, sf, of, pf, af; mmx_t mm[8]; } tstate;
typedef struct { int base, idx, scale; int disp; } memop_t;
typedef struct {
    const char *mn; u8 code[16]; int len; int rep; int fmask; int sz; int divreg; memop_t m0, m1; void (*fn)(tstate *);
} test_t;
extern const test_t itests[]; extern const int nitests;
#endif
