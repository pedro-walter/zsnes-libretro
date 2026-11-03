/* Generic asm-vs-C differential check over the whole writable image of the core plus registered
 * external (malloc'd) regions.  Used by the renderer ports (and reusable for anything else). */
#ifndef DIFFIMG_H
#define DIFFIMG_H
#include <stddef.h>
#include <stdint.h>
typedef struct { uint32_t eax, ecx, edx, ebx, esi, edi, ebp; } zregs_t;
extern zregs_t zs_R;
/* external regions are re-registered through this hook on every check (pointers can move) */
void di_ext(void *p, size_t n);
void di_exclude(void *p, size_t n);
extern void (*di_region_hook)(void);
/* Run afn (asm, via zs_mcall) and cfn (C) from identical state R/memory; compare memory + the registers in
 * regmask (bit0 eax,1 ecx,2 edx,3 ebx,4 esi,5 edi,6 ebp).  On mismatch state is left as the asm result.
 * Returns number of mismatching bytes/regs (0 = identical). */
int di_run(const char *name, void *afn, void (*cfn)(void), unsigned regmask);
void di_guard(int on);  /* catch SIGSEGV inside asm (fuzzing); such cases count as skipped */
unsigned long di_total_bad(void);
unsigned long di_total_ops(void);
void di_report(void);
#endif
