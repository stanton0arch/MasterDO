#ifndef SMS3DO_MEMPROBE_H
#define SMS3DO_MEMPROBE_H

#include "common.h"

#if SMS_MEM_PROBE

/*
 * ---------------------------------------------------------------------------
 * What a memory access costs on this machine.
 *
 * The whole cost model of this project is one sentence -- a register operation
 * is about one cycle, a memory access about three -- and every figure the
 * render and the core have been reasoned about with rests on it. It was read
 * off the ARM60 manual and has never been measured here. The render says it is
 * wrong: the background costs 25.6 cycles a pixel against four to six counted.
 *
 * This probe answers one question and no other: how many ARM cycles does one
 * memory access take, and by how much does that change with the shape of the
 * access. It emulates nothing, it is not part of the render, and nothing in
 * the program may come to depend on it.
 *
 * What it does NOT answer, and must not be made to: what the render should do
 * about it. A probe that optimised on its way would stop being able to account
 * for what it measured.
 * ---------------------------------------------------------------------------
 */

/*
 * Takes the memory the timed loops walk over.
 *
 * Called before the memory seal, because that is the only time memory can be
 * had. It walks nothing and times nothing here: on return the buffers exist.
 *
 * Returns 0, or -1 having traced the reason. A probe that cannot get its
 * buffers is absent, not fatal -- the program boots without it. The VRAM
 * buffer is the one allowed to be missing on its own: the DRAM figures stand
 * without it, and the shape that needed it says so rather than being silently
 * dropped.
 */
Err memprobe_install(void);

/*
 * Runs every shape and publishes a figure per shape.
 *
 * Called after the seal and before the frame loop: after, so that a probe
 * needing memory to run would be caught rather than hidden; before, because it
 * holds the processor for several seconds and a frame that took several
 * seconds would be a frame destroyed rather than a frame slowed.
 *
 * Returns 0 when every shape published a figure, 1 when some were dropped and
 * the report is therefore partial -- a shape needing the VRAM buffer that was
 * never obtained, or one whose windows the clock ruined -- and -1 when nothing
 * was measured at all. A partial report is not a failure and each dropped
 * shape names itself in the trace; the distinction is here so that the return
 * value does not claim completeness the report does not have.
 */
Err memprobe_measure(void);

#endif /* SMS_MEM_PROBE */

#endif /* SMS3DO_MEMPROBE_H */
