#ifndef SMS3DO_DYNAREC_J0_H
#define SMS3DO_DYNAREC_J0_H

#include "common.h"

#if SMS_DYNAREC_J0

/*
 * ---------------------------------------------------------------------------
 * The throughput mock-up.
 *
 * One block of emulated code, translated by hand into native instructions,
 * copied into ordinary memory at run time and jumped into. It answers one
 * question: what does translated code cost per T-state on this machine, block
 * entry and exit included. Everything here exists to make that one figure
 * trustworthy, and nothing here emulates anything.
 *
 * The figure is read against an absolute threshold and not against the
 * interpreter. Roughly two cycles per T-state is the line: under it the margin
 * the translation rests on is real, over it the case for translating has to be
 * made again.
 *
 * What it does NOT answer, and must not be made to: how blocks are found, what
 * happens when the guest writes over code it is running, and what a bank
 * switch costs. Those are separate, later, and each is a danger of its own.
 * ---------------------------------------------------------------------------
 */

/*
 * The emulated registers a translated block borrows, one per word.
 *
 * A word per eight bit register rather than a packed byte: this struct is the
 * spill area between two blocks, and packing would buy a few bytes of memory
 * at the price of a mask on every single load and store across the boundary --
 * the very sequence being measured.
 *
 * CONTRACT with dynarec_j0.s, which reads and writes these by fixed offset:
 *   +0 a  +4 f  +8 d  +12 e  +16 hl  +20 sp  +24 pc  +28 mem
 * Changing the order or inserting a field means changing that file too.
 *
 * hl and sp are held here as plain sixteen bit values. The translated code
 * keeps them shifted into the top half of a register while it runs, which is
 * its business and stops at the boundary.
 *
 * pc is an output only: the address the block's final RET popped. It is what
 * says the translated block left by the door the original leaves by.
 */
typedef struct
{
  uint32 a;
  uint32 f;
  uint32 d;
  uint32 e;
  uint32 hl;
  uint32 sp;
  uint32 pc;
  uint8 *mem;
} j0_state_t;

/*
 * Takes the memory the translated block will live in and copies it there.
 *
 * Called before the memory seal, because that is the only time memory can be
 * had. Consumes nothing and executes nothing: on return the block is sitting
 * in the buffer, not yet run.
 *
 * Returns 0, or -1 having traced the reason. A mock-up that cannot get its
 * buffer is absent, not fatal -- the program boots without it.
 */
Err dynarec_j0_install(void);

/*
 * Checks the translation against the core, then measures it, then says so.
 *
 * In that order, and the order is the point: the check runs several starting
 * states through both the interpreter and the translated block and compares
 * what each left behind, registers and touched memory alike. If any of them
 * disagree, no figure is published at all -- a number taken on wrong code
 * reads exactly like a real one and is worth less than nothing.
 *
 * Returns 0 when a figure was published, -1 otherwise.
 */
Err dynarec_j0_measure(void);

#endif /* SMS_DYNAREC_J0 */

#endif /* SMS3DO_DYNAREC_J0_H */
