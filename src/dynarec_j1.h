#ifndef SMS3DO_DYNAREC_J1_H
#define SMS3DO_DYNAREC_J1_H

#include "common.h"

#if SMS_DYNAREC_J1

/*
 * ---------------------------------------------------------------------------
 * Chaining translated blocks.
 *
 * J0 answered whether translation is worth it -- 1.05 cycles per T-state for
 * the translated body against 3.49 of budget -- and in answering it found what
 * costs: the block boundary, 141.9 cycles, forty-five per cent of the block.
 * It is paid on every block, and the hot loop of the test suite crosses
 * twenty-five of them per turn.
 *
 * This is the execution model that removes it. The emulated registers come
 * into native registers once; a block ends by branching straight into the next
 * one; the state goes back out only when the next address has no translation
 * or the time is spent. What remains between two blocks is a table read, a
 * countdown and a branch.
 *
 * It answers one question -- does chaining remove the boundary -- and builds
 * the machinery every later milestone needs. It does NOT discover blocks,
 * translate anything by itself, notice a guest writing over its own code, or
 * know what a bank is.
 * ---------------------------------------------------------------------------
 */

/*
 * One entry per emulated address, holding where that address's translation
 * lives, or zero for an address that has none.
 *
 * A flat table and not a dictionary: the emulated space is sixty-four
 * kilobytes, so a word each is two hundred and fifty-six kilobytes out of the
 * one and a third megabytes free, and the lookup becomes a shift and a load
 * instead of a hash. It is also the shape that survives what comes later --
 * the day a mapper pages code in and out, what changes is what the entries
 * hold, not what an entry is.
 */
#define J1_TABLE_ENTRIES 65536UL

/*
 * What the chain borrows and gives back.
 *
 * CONTRACT with dynarec_j1.s, which reads and writes these by fixed offset:
 *   +0 a +4 f +8 d +12 e +16 hl +20 sp +24 pc +28 quota +32 links
 *   +36 mem +40 table
 *
 * pc, quota and links are the three that make it a chain rather than a call.
 * pc is where to start and, on return, where it stopped -- a caller resumes
 * from exactly there. quota is the T-states it may spend, counted the way the
 * core counts them so that the two costs are in one unit. links is how many
 * block boundaries it crossed without leaving, which is the number this
 * milestone exists to make large.
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
  uint32 quota;
  uint32 links;
  uint8 *mem;
  uint32 *table;
} j1_state_t;

/*
 * Takes the memory, copies the translated code into it, and fills the table.
 *
 * Called before the memory seal. Executes nothing.
 *
 * Returns 0, or -1 having traced the reason. A milestone that cannot get its
 * memory is absent, not fatal.
 */
Err dynarec_j1_install(void);

/*
 * Checks the chain against the core, then measures it, then says so -- in that
 * order, for the reason J0 gives: a figure taken on wrong code reads exactly
 * like a real one.
 *
 * Returns 0 when a figure was published, -1 otherwise.
 */
Err dynarec_j1_measure(void);

#endif /* SMS_DYNAREC_J1 */

#endif /* SMS3DO_DYNAREC_J1_H */
