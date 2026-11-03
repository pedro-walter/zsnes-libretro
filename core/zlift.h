/* Helpers for C code that shares pointer variables with the lifted guest (32-bit slots in the guest arena). */
#ifndef ZLIFT_H
#define ZLIFT_H
#include <stdint.h>
#ifdef LIFTED
#define ZADDR(name)         ((void *)&name##_SLOT)
#define ZSETP(name, v)      (name##_SLOT = (unsigned int)(uintptr_t)(v))
#define ZADDP(name, T, n)   (name##_SLOT += (unsigned int)((n) * (int)sizeof(T)))
#else
#define ZADDR(name)         ((void *)&name)
#define ZSETP(name, v)      (name = (v))
#define ZADDP(name, T, n)   (name += (n))
#endif
#endif
