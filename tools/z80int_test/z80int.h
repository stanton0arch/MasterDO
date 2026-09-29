#ifndef Z80INT_H
#define Z80INT_H

/*
 * Z80 interpreter on the translator's context (z80jit.h): runs the code
 * that has not been translated, instruction by instruction, with the
 * registers, the memory tables, the port callbacks and the T-state
 * counter of the context, so that control can pass between translated
 * blocks and interpreted code at any instruction boundary.
 *
 * It follows the translator's conventions: T-states are subtracted from
 * regs[5] (times 256) per instruction and the stretch ends before an
 * instruction that may not fit; port and special write callbacks get the
 * T-states left after the instruction; the undocumented flag bits 3 and
 * 5 are always 0; R is approximated as in the translated code.
 */

#include "z80jit.h"

/* Results of z80i_run; regs[6] holds the Z80 address where to go on. */
#define Z80I_DONE        0  /* the stretch cannot take the next instruction */
#define Z80I_TRANSLATED  1  /* regs[6] has translated code in the lookup table */
#define Z80I_HOT         2  /* a transfer target reached a count threshold */
#define Z80I_HALT        3  /* HALT executed: halted set, the stretch is idle */
#define Z80I_IRQ_CHECK   4  /* a pending interrupt may be taken now (after */
                            /* EI + 1, RETN, or a callback asking to leave) */

typedef struct {
    uint32  miss;           /* lookup table value of untranslated addresses */
    uint8  *hot;            /* execution counts per address (0: none kept) */
    uint32  queue_at;       /* counts at which Z80I_HOT is returned */
    uint32  sync_at;
    uint32  insns;          /* instructions interpreted (added to) */
} z80i_params;

/* Interprets from regs[6] until one of the results above. */
uint32 z80i_run(z80j_ctx *ctx, z80i_params *p);

#endif /* Z80INT_H */
