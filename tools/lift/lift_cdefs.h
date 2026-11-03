/* force-included into the C sources of the lifted build: heap blocks that the guest can see must live in the
 * low guest arena (32-bit addresses), so the C side allocates from the same arena. */
#include <stdlib.h>
#include <string.h>
#ifdef __cplusplus
extern "C" {
#endif
void *zl_malloc(unsigned n);
void zl_free(void *p);
void *zl_calloc(unsigned n, unsigned m);
void *zl_realloc(void *p, unsigned n);
char *zl_strdup(const char *s);
#ifdef __cplusplus
}
#endif
#define malloc(n)      zl_malloc(n)
#define free(p)        zl_free(p)
#define calloc(n, m)   zl_calloc(n, m)
#define realloc(p, n)  zl_realloc(p, n)
#define strdup(s)      zl_strdup(s)

#define outofmemory zs_outofmemory
void zs_outofmemory(void);
