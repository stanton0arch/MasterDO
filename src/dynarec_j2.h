#ifndef SMS3DO_DYNAREC_J2_H
#define SMS3DO_DYNAREC_J2_H

#include "common.h"

#if SMS_DYNAREC_J2

/*
 * ---------------------------------------------------------------------------
 * Which of two things costs.
 *
 * J1 chained the blocks and the boundary's share fell from 0.865 to 0.207
 * cycles per T-state. But the total stopped at 1.52 where that amortisation
 * alone predicted 1.26, and the 0.261 left over sat between two suspects one
 * measurement could not separate: the link, seven instructions with a load in
 * the middle, and the flag-heavy forms -- DEC costing thirteen native
 * instructions for four T-states, AND n ten for seven, INC (HL) fourteen for
 * eleven.
 *
 * So both are attacked and both are measured, separately. Four runs of the
 * same region over the same code, one lever apart:
 *
 *   variant 0  neither      the in-file baseline, J1's forms
 *   variant 1  link only
 *   variant 2  flags only
 *   variant 3  both
 *
 * Reporting a gain over the two together would have bought speed and kept the
 * ignorance. Four figures close the question.
 * ---------------------------------------------------------------------------
 */

#define J2_TABLE_ENTRIES 65536UL

/*
 * Three flag tables, one byte per value, laid out end to end so that one base
 * register reaches all three.
 *
 * F depends on a single byte for each of these forms -- the value before the
 * decrement, the value after the increment, the result of the logical
 * operation -- with the carry, which some of them preserve, left out and put
 * back by the two instructions that read the table.
 */
#define J2_TAB_DEC    0UL     /* what DEC writes, from the value BEFORE  */
#define J2_TAB_SZ53P  256UL   /* sign, zero, bits 5 and 3, parity        */
#define J2_TAB_INC    512UL   /* what INC writes, from the value AFTER   */
#define J2_TAB_SIZE   768UL

/*
 * CONTRACT with dynarec_j2.s, which reads and writes these by fixed offset:
 *   +0 a +4 f +8 d +12 e +16 hl +20 sp +24 pc +28 quota +32 mem +36 table
 *   +40 flags
 *
 * The link counter J1 carried is gone: it was instrumentation, it cost an
 * instruction on every link, and the number of links a pass makes is known
 * without counting them.
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
  uint8 *mem;
  uint32 *table;
  uint8 *flags;
} j2_state_t;

/*
 * Takes the memory, copies the four variants into it, and builds the flag
 * tables. Called before the memory seal; executes nothing.
 */
Err dynarec_j2_install(void);

/*
 * Checks the tables against the core, checks each variant against the core at
 * every boundary, then measures the four, then says so. Nothing is published
 * if anything disagrees.
 */
Err dynarec_j2_measure(void);

#endif /* SMS_DYNAREC_J2 */

#endif /* SMS3DO_DYNAREC_J2_H */
