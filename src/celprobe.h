#ifndef SMS3DO_CELPROBE_H
#define SMS3DO_CELPROBE_H

#include "common.h"

#if SMS_CEL_PROBE

/*
 * ---------------------------------------------------------------------------
 * What the cel engine charges for a list, and what the screen's colour
 * table does to a picture of colour numbers.
 *
 * The picture is moving from the processor to the cel engine and the
 * display list. Three figures that move rests on are documented nowhere:
 * the fixed cost of one draw call, the cost of one small cel in a list, and
 * the cost -- and the look -- of the screen's colour table driven from a
 * picture whose pixels carry their colour number. This probe measures the
 * three on a numbered test pattern and prints them. It emulates nothing, it
 * draws no game, and nothing in the program may come to depend on it.
 *
 * What it does NOT decide, and must not be made to: how the picture is then
 * built. It prints figures and the verdict of a rule written before the run;
 * the reading of those figures belongs to whoever reads the trace.
 * ---------------------------------------------------------------------------
 */

/*
 * Takes the two pages the probe works in: one for the test pattern and its
 * palette, one for the cel lists and, later, the display list.
 *
 * Called before the memory seal, because that is the only time memory can
 * be had, and before the memory summary, so that the footprint printed is
 * the footprint of the build that measures. It draws nothing and times
 * nothing here: on return the pages exist, filled.
 *
 * Returns 0, or -1 having traced the reason. A probe that cannot get its
 * pages is absent, not fatal -- the program boots into the cartridge without
 * it, and says so.
 */
Err celprobe_install(void);

/*
 * Runs every measurement and publishes a figure per line, then holds the
 * test pattern on the screen for ever: it DOES NOT RETURN once installed.
 *
 * Called after the seal and in the place of the frame loop: after, so that
 * a measurement needing memory to run would be caught rather than hidden;
 * in the place of the loop, because the cartridge would draw over the
 * pattern with a colour table that is no longer its own, and the pattern is
 * what the run exists to show. Returns only when install had failed, in
 * which case it has traced that nothing was measured and the caller goes
 * on into the frame loop.
 */
void celprobe_measure(void);

#endif /* SMS_CEL_PROBE */

#endif /* SMS3DO_CELPROBE_H */
