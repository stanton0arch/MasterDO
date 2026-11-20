#ifndef OPTIONS_H
#define OPTIONS_H

/*
 * Build options.
 *
 * MASTERDO_DEBUG
 *   1  development build: C pauses the run on the debug view (the
 *      reference picture drawn by the CPU, the cels, the presentation
 *      mode) and writes the run's records and summary to the debug log,
 *      as does a run that stops; the clock is measured at start-up; the
 *      banner names the build by its latest changes.
 *   0  release build: none of that is compiled (the debug view's code is
 *      left out of the executable), C does nothing and the banner names
 *      the version. The emulation, the menu, the saves, the savestate and
 *      the STOP pause are those of the development build, and the run's
 *      records are still kept in memory.
 */
#ifndef MASTERDO_DEBUG
#define MASTERDO_DEBUG 0
#endif

/* The version a release build names. */
#define MASTERDO_VERSION "0.3"

#endif
