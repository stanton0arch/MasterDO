/*
 * Z80 to ARM translator: branch encoding shared by the block management
 * (z80jit.c) and the code emitter (z80jit_emt.s).
 */

#include "z80jit_int.h"

uint32 arm_branch(uint32 cond, uint32 link, uint32 from, uint32 to)
{
    return (cond << 28) | (5u << 25) | (link << 24) | (((to - from - 8) >> 2) & 0x00FFFFFFu);
}

/* Retargets an emitted branch, keeping its condition and link bit. */
void patch_b(uint32 *site, uint32 to)
{
    uint32 from = JIT_ADDR(site);

    *site = (*site & 0xFF000000u) | (((to - from - 8) >> 2) & 0x00FFFFFFu);
}
