/* Runtime support for lifted x86 code (see tools/lift). */
#ifndef LIFT_H
#define LIFT_H
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
typedef uint8_t u8; typedef uint16_t u16; typedef uint32_t u32; typedef uint64_t u64;
typedef int8_t s8; typedef int16_t s16; typedef int32_t s32; typedef int64_t s64;
typedef uint16_t __attribute__((aligned(1), may_alias)) u16u;
typedef uint32_t __attribute__((aligned(1), may_alias)) u32u;
typedef uint64_t __attribute__((aligned(1), may_alias)) u64u;
typedef union { u64 q; u32 d[2]; u16 w[4]; s16 sw[4]; u8 b[8]; s8 sb[8]; s32 sd[2]; } mmx_t;

typedef struct {
    u32 eax, ecx, edx, ebx, esp, ebp, esi, edi;
    u8 cf, zf, sf, of, pf, af;
    mmx_t mm[8];
} lift_state_t;
extern lift_state_t LG;
typedef u32 (*lift_fn_t)(u32);
extern lift_fn_t lift_objtab[128];
extern const char *const lift_objname[128];

#define M8(a)  (*(u8 *)(uintptr_t)(u32)(a))
#define M16(a) (*(u16u *)(uintptr_t)(u32)(a))
#define M32(a) (*(u32u *)(uintptr_t)(u32)(a))
#define M64(a) (*(u64u *)(uintptr_t)(u32)(a))
#define PAR(r) ((u8)(!__builtin_parity((u8)(r))))

#ifdef LIFT_NOMM
#define LIFT_MMDECL
#define LIFT_MMFLUSH() ((void)0)
#define LIFT_MMRELOAD() ((void)0)
#else
#define LIFT_MMDECL mmx_t mm[8];
#define LIFT_MMFLUSH() memcpy(LG.mm, mm, sizeof mm)
#define LIFT_MMRELOAD() memcpy(mm, LG.mm, sizeof mm)
#endif
#define FLUSH() do { LG.eax=eax; LG.ecx=ecx; LG.edx=edx; LG.ebx=ebx; LG.esp=esp; LG.ebp=ebp; LG.esi=esi; LG.edi=edi; \
    LG.cf=cf; LG.zf=zf; LG.sf=sf; LG.of=of; LG.pf=pf; LG.af=af; LIFT_MMFLUSH(); } while (0)
#define RELOAD() do { eax=LG.eax; ecx=LG.ecx; edx=LG.edx; ebx=LG.ebx; esp=LG.esp; ebp=LG.ebp; esi=LG.esi; edi=LG.edi; \
    cf=LG.cf; zf=LG.zf; sf=LG.sf; of=LG.of; pf=LG.pf; af=LG.af; LIFT_MMRELOAD(); } while (0)

u32 ext_call(u32 id);                       /* call foreign C function; args at guest [LG.esp] */
u32 lift_missing(u32 id);
void lift_trap(const char *what, u32 pc);
void lift_bad(u32 pc);
#define LIFT_MISSING(id) lift_missing(id)
#define LIFT_TRAP(w, pc) lift_trap(w, pc)
#define LIFT_BAD(pc) do { lift_bad(pc); __builtin_unreachable(); } while (0)
#define LIFT_DIVCHK(d) do { if (!(d)) lift_trap("divide by zero", 0); } while (0)

#include "lift_mmx.h"
/* x87 subset */
extern double lift_fp[8]; extern int lift_fptop;
#define FST(i) (lift_fp[(lift_fptop + (i)) & 7])
#define FPUSH(v) do { double v_ = (v); lift_fptop = (lift_fptop - 1) & 7; lift_fp[lift_fptop] = v_; } while (0)
#define FPOP() (lift_fptop = (lift_fptop + 1) & 7)
#define LIFT_F32(a) (*(float *)(uintptr_t)(u32)(a))
#define LIFT_F64(a) (*(double *)(uintptr_t)(u32)(a))
#define LIFT_I16(a) (*(s16 *)(uintptr_t)(u32)(a))
#define LIFT_I32(a) (*(s32 *)(uintptr_t)(u32)(a))
#define LIFT_I64(a) (*(s64 *)(uintptr_t)(u32)(a))
#include <math.h>
static inline s16 lift_f2i16(double d) { return (s16)llrint(d); }
static inline s32 lift_f2i32(double d) { return (s32)llrint(d); }
static inline s64 lift_f2i64(double d) { return (s64)llrint(d); }
#endif
#ifdef __cplusplus
extern "C" {
#endif
int lift_init(void);
void lift_shutdown(void);
u32 lift_run(u32 pc);
u32 lift_call(u32 id);
void lift_call_regs(u32 id, u32 *r);
void *zl_malloc(unsigned n);
void zl_free(void *p);
#ifdef __cplusplus
}
#endif
void lift_baseline(void);
size_t lift_state_size(void);
int lift_state_save(void *out, size_t cap);
int lift_state_load(const void *in, size_t n);
