/* Shim for hand-written C that replaces lifted asm functions ("overrides", tools/lift/overrides.txt).
 * Guest data symbols are absolute linker symbols (lift_syms.ld) and can be declared extern; a guest pointer variable
 * is a 32-bit slot holding an arena address.  Register-ABI entry points read/write LG. */
#ifndef ZL_H
#define ZL_H
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "lift.h"
#include "lift_syms.h"
#define HID __attribute__((visibility("hidden")))
void zl_orig(u32 id);   /* run the original lifted body of an overridden function with the current LG state */
#endif
