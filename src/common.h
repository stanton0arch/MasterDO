#ifndef SMS3DO_COMMON_H
#define SMS3DO_COMMON_H

/*
 * Definitions shared by every module of the port.
 *
 * This header is a leaf of the dependency tree: it includes no project header
 * and calls nothing, so it can be included anywhere without ever creating a
 * cycle.
 */

#include "types.h"
#include "graphics.h"
#include "displayutils.h"
#include "mem.h"

#define SMS3DO_NAME    "sms3do"
#define SMS3DO_VERSION "0.1"

/*
 * The emulated system profile. Preprocessor integers rather than an
 * enumeration so that SMS_FORCE_SYSTEM below can be compared by #if.
 *
 * Written once by the cartridge module when a load succeeds
 * (sms.cart.system), read-only afterwards; cart.h carries the rule and the
 * precedence that fixes the value. It lives here rather than in cart.h
 * because it is the one fact every subsystem reads at its own init.
 */
#define SYS_NONE 0
#define SYS_SMS  1
#define SYS_GG   2

/*
 * SMS_FORCE_SYSTEM -- build-time override of the detected system.
 *
 * At 0 the profile comes from the ROM itself: header first, extension as the
 * fallback (cart.c). At 1 (SYS_SMS) or 2 (SYS_GG) the build imposes the
 * system: the header is still parsed and its lines still traced, but the
 * verdict is the constant's and the trace says so. The precedence -- an
 * imposed system wins over the header -- follows TotalSMS, where a system
 * handed in from outside short-circuits the header branch
 * (TotalSMS/src/core/sms.c:560-563).
 *
 * Cuts: at 0, the forced branch and its trace text -- the default binary
 * carries no trace of the constant, not even the string. At 1 or 2, the
 * unforced system= lines, and with them the only lines that name the system
 * and region the header claims.
 * Leaves: the header parse in every build, and in a forced build the header
 * found line -- a forced run still says whether a header exists and at which
 * offset, no more; the region it may carry stays extracted into
 * sms.cart.region, unlogged.
 */
#ifndef SMS_FORCE_SYSTEM
#define SMS_FORCE_SYSTEM 0
#endif

#if (SMS_FORCE_SYSTEM < 0) || (SMS_FORCE_SYSTEM > 2)
#error "SMS_FORCE_SYSTEM must be 0 (off), 1 (SMS) or 2 (GG)"
#endif

/*
 * The cartridge mapper the build serves. Preprocessor integers for the
 * reason the SYS_* values are: SMS_MAPPER below is compared by #if.
 */
#define MAPPER_SEGA        0
#define MAPPER_CODEMASTERS 1
#define MAPPER_NONE        2

/*
 * SMS_MAPPER -- build-time choice of the cartridge mapper.
 *
 * No source this repository trusts gives a criterion telling a Sega
 * cartridge from a Codemasters one or from none (docs/pseudocodes/
 * 30_mappers.md section 9, note D), so the mapper is not detected: it is
 * chosen here, at compile time, and the boot trace says so. At 0
 * (MAPPER_SEGA, the default) the Sega mapper is assumed, as it has been
 * since the first bus. At 1 (MAPPER_CODEMASTERS) the Codemasters mapper is
 * served on a best-effort basis -- not a release commitment. At 2
 * (MAPPER_NONE) no mapper is served: the bus stays on the linear map of
 * banks 0/1/2, which is the documented fallback for an unknown mapper
 * (docs/pseudocodes/30_mappers.md section 9), and one warning says so at
 * install.
 *
 * A compile-time constant and not a run-time field, on purpose: the mapper
 * trigger sits on the Z80's one write path, and a choice made at run time
 * would be a test paid on every emulated write, on the path whose cycle
 * budget is the tightest of the program. cart.h reads the
 * constant at exactly two places -- the address test of the trigger and
 * the body of the mapper write -- so the day a criterion exists, those two
 * become a switch at install and the write path is not reopened.
 *
 * Cuts: at 0, every Codemasters and fallback line and body -- the default
 * binary carries no trace of the constant, not even a string, and its
 * write path expands exactly as it did before the constant existed. At 1,
 * the Sega bodies, the cartridge RAM and its allocation. At 2, every
 * mapper body, the trigger itself -- the write path is then a plain
 * store through the table -- and the cartridge RAM.
 * Leaves: the bus installation and the linear map, in every build; the
 * bank count and mask, which the load fixes whatever the mapper.
 */
#ifndef SMS_MAPPER
#define SMS_MAPPER MAPPER_SEGA
#endif

#if (SMS_MAPPER < 0) || (SMS_MAPPER > 2)
#error "SMS_MAPPER must be 0 (Sega), 1 (Codemasters) or 2 (none)"
#endif

/*
 * ---------------------------------------------------------------------------
 * Instrumentation switches. This is the one place where the amount of
 * diagnostic built into the binary is decided.
 *
 * All four are read by the preprocessor alone, never by running code: whatever
 * is switched off here is absent from the binary rather than skipped at run
 * time, which is what makes an instrumentation-free build both smaller and
 * free of any residual cost. They are set by editing this file and rebuilding.
 *
 * Each is defined under #ifndef so that a definition coming from the command
 * line overrides it without the source being touched.
 *
 * Pattern for the master telemetry switch, and for the discipline of leaving
 * init and error messages outside its reach:
 * src_exemple_video_player/common.h:31-35, cinepak_decoder.c:14-24.
 * ---------------------------------------------------------------------------
 */

/*
 * LOG_ENABLE -- the whole logging layer.
 *
 * Cuts: every LOG_* line, call and message text alike. At 0 the format strings
 * never reach the compiler, so none of them survives in the binary.
 * Leaves: the fatal error screen. log_fatal is a function and not a macro; it
 * paints and traces in every build, because the build a player runs is exactly
 * the one where a failure most needs to say something.
 */
#ifndef LOG_ENABLE
#define LOG_ENABLE 1
#endif

/*
 * LOG_LEVEL -- most verbose level kept in the binary, named by one of the
 * LOG_LVL_* constants of log.h.
 *
 * Cuts: every level past it, removed the same way LOG_ENABLE removes
 * everything. A trace left in the source at a level that is not built costs
 * nothing, which is what makes it safe to instrument generously.
 * Leaves: every level up to and including it.
 */
#ifndef LOG_LEVEL
#define LOG_LEVEL LOG_LVL_INFO
#endif

/*
 * LOG_CAT_MASK -- one bit per category, bit n being the LOG_CAT_* whose value
 * is n. LOG_CAT_ALL turns them all on.
 *
 * Cuts: nothing, and the distinction matters. This one is a filter, not a
 * removal: a masked-out category emits nothing, but its message text may still
 * sit in the binary. Use it to quieten a subsystem while working on another,
 * and use LOG_ENABLE or LOG_LEVEL when the requirement is that nothing be left
 * behind.
 * Leaves: every category whose bit is set.
 */
#ifndef LOG_CAT_MASK
#define LOG_CAT_MASK LOG_CAT_ALL
#endif

/*
 * SMS_TELEMETRY -- master switch of the hot path, independent of LOG_ENABLE.
 *
 * Cuts: what runs per instruction, per scanline or per frame -- the LOG_HOT
 * lines of log.h and any counter or timer kept only to feed them, which the
 * same #if must enclose. The serial output is blocking, so a trace on that
 * path does not slow a frame down, it destroys it, and any measurement taken
 * with it left in measures the trace.
 * Leaves: the boot banner, the init lines and every error. That is the whole
 * point of a switch separate from LOG_ENABLE: a measurement run has to stay
 * diagnosable, and a run that says nothing at all cannot be.
 */
#ifndef SMS_TELEMETRY
#define SMS_TELEMETRY 1
#endif

/*
 * SMS_VDP_BUFFERS -- how many index buffers the video part allocates.
 *
 * Each buffer holds one frame of packed 6 bit colour indexes, sized by the
 * calculated constants of vdp.h and taken in DRAM at init.
 *
 * The number is one, and that is a decision taken on measurement rather
 * than a value waiting for it. Two things carry it.
 *
 * The cel engine is synchronous: the draw call returns when the picture
 * has been drawn, it does not run alongside the processor
 * (docs/3do/3DO_Development_Notes.md:73). So there is no window during
 * which the engine is still reading a buffer the render could be
 * composing into, and a second buffer would sit idle for its 36 kilobytes.
 *
 * The measurement agrees, and it is the draw= field of the periodic line
 * that carries it (main.c, the third of the accumulators that line
 * reports; it weighs the draw call and nothing else). Read on the console
 * over two commercial cartridges, three hundred and fifteen windows of
 * one second: draw= sat at 2.8 milliseconds a frame in half of them and
 * inside 2.1 to 4.1 in every one, against frames of 434 and 514
 * milliseconds -- under one percent of the turn, where composing the
 * picture is what fills the rest. A second buffer would buy back nothing
 * that is being spent, and the figure is regenerated by every run, so the
 * claim can be checked rather than believed.
 *
 * The double buffering that does earn its place is the screen's, and the
 * system part turns those on every presentation, so nothing is ever
 * composed under the beam.
 *
 * What would reopen it: a tear seen on the console with the eye, a picture
 * split across two frames along a moving edge. Not a suspicion, not an
 * intuition about what a faster render might do -- a sighting. The array
 * in the video state keeps the shape such a change would need.
 *
 * A compile-time constant and not a run-time field, like the mapper choice
 * above: the number of buffers shapes an allocation and an array, both
 * fixed at build time, and every configuration of the table below ships
 * with the default of 1.
 */
#ifndef SMS_VDP_BUFFERS
#define SMS_VDP_BUFFERS 1
#endif

#if (SMS_VDP_BUFFERS < 1)
#error "SMS_VDP_BUFFERS must be at least 1: the cel needs a picture to read"
#endif

/*
 * The three configurations worth naming:
 *
 *   development  the default this file ships with: LOG_ENABLE 1, LOG_LEVEL
 *                LOG_LVL_INFO, SMS_TELEMETRY 1. A cartridge read off the
 *                disc, the periodic line, the boot trace. LOG_LVL_DBG for
 *                as long as a subsystem is being worked on.
 *   measurement  LOG_ENABLE 1, LOG_LEVEL LOG_LVL_INFO, SMS_TELEMETRY 0.
 *                The instrumentation priced out: the hot-path counters
 *                go, and the periodic line goes with them, so this build
 *                publishes no figure of its own. What it gives is a size
 *                and a boot trace for a run that measures nothing -- the
 *                cost of the instrumentation, by difference. A figure is
 *                read from the development build, the one where the
 *                periodic line exists.
 *   silent       LOG_ENABLE 0. No diagnostic output whatsoever. Proves
 *                that instrumentation compiles down to nothing, and gives
 *                the binary size the others are compared against.
 *
 * SMS_VDP_BUFFERS (above) stays at its default of 1 in all three. In every
 * one of them the frame loop executes the cartridge by scanline quotas,
 * the video part renders one line per quota, and the drawing side -- the
 * one draw call and the presentation -- closes the frame: there is one
 * executor and one path, and the switches above decide only what is
 * traced and measured along it.
 *
 * Two switch names are refused below so that an old build command fails
 * loudly rather than silently building the default. The built-in test
 * suite once had a second way to run, from the boot sequence one
 * instruction at a time, behind a switch of its own; then the suite
 * itself left the build -- its image weighed 64 kilobytes of DRAM, and a
 * make with no flag ran the test rather than the cartridge. The package
 * stays in docs/sms_gg/ZEXALL-SMS-0.21/ as the source of the processor's
 * semantics, and docs/pseudocodes/20_test_harness.md says how it was
 * wired, should it ever need to be again.
 */

#ifdef SMS_HARNESS_IN_FRAME
#error "SMS_HARNESS_IN_FRAME no longer exists: the frame loop is the one executor, make with no flag runs the cartridge"
#endif

#ifdef SMS_HARNESS
#error "SMS_HARNESS no longer exists: the test suite left the build, make with no flag runs the cartridge"
#endif

/*
 * Forces a fatal stop right after the screen is up, so that the error screen
 * can be exercised while no condition able to trigger it exists yet. Must read
 * 0 in any build that is handed to anyone: at 1 the emulator does nothing else
 * than show the test message.
 */
#ifndef LOG_SELFTEST_FATAL
#define LOG_SELFTEST_FATAL 0
#endif

/*
 * SMS_IRQ_TEST_SOURCE -- the scaffold source of the non-maskable line.
 *
 * It belongs here because this file is the one place where what the binary
 * contains is decided, and a switch kept anywhere else would break that.
 *
 * The maskable line has its owner, the video part, which holds it from the
 * VBlank or the line event until the program reads the status register
 * (vdp.h). The non-maskable line has none yet: it belongs to the Pause
 * button, whose input module does not exist. This switch builds a stand-in
 * for that one line: a source of its own state, read through the accessor
 * declared in z80.h, pulsing the line every two emulated seconds. It is
 * scaffolding written as a stub of that interface and nothing more -- the
 * input module, when it arrives, replaces it by taking over the accessor,
 * without touching the sampling in z80.c, exactly as the video part took
 * over the other line.
 *
 * Off by default: a game does not want one -- a pulse in a cartridge build
 * is a Pause button pressed every two seconds, which a run of a real
 * program showed as an accepted non-maskable interrupt with no one at the
 * console. A definition from the command line turns it on, to see the
 * acceptance path work before the input module exists.
 *
 * Cuts: the source's state, its accessor, its cadence arithmetic, and --
 * through the line folding to a constant low -- the non-maskable acceptance
 * branch of z80.c together with its trace text, which is what the size of
 * the binary shows.
 * Leaves: the sampling structure itself, the maskable acceptance and its
 * owner, the resolution of the EI delay, and the HALT semantics -- those
 * are the processor and the device, not the scaffold.
 */
#ifndef SMS_IRQ_TEST_SOURCE
#define SMS_IRQ_TEST_SOURCE 0
#endif

/*
 * SMS_DYNAREC_J0 -- the throughput mock-up.
 *
 * One block of the emulated processor's code, translated by hand into native
 * instructions, written into ordinary memory at run time and jumped into. It
 * answers one question and no other: what does translated code cost per
 * T-state on this machine, block entry and exit included. That figure decides
 * whether translation is worth months of work, and it has only ever been a
 * paper figure until now.
 *
 * Off by default, and it is scaffolding: it emulates nothing, it is not a
 * second core, and nothing else in the program may come to depend on it.
 *
 * Needed the test image the block is taken from, and the flat memory the
 * built-in suite once ran in; both left the build, so this mock-up no
 * longer compiles and the guard below says so. Its figures are on record;
 * the translation work will take it up again against a real ROM. It also
 * needs telemetry, which owns the clock a duration is read from.
 *
 * Cuts: the translated block, its buffer, the reference run, the comparison
 * and every line all four print -- the whole file on both sides.
 */
#ifndef SMS_DYNAREC_J0
#define SMS_DYNAREC_J0 0
#endif

#if SMS_DYNAREC_J0
#error "SMS_DYNAREC_J0 cannot be built: the test image is no longer in the build"
#endif

#if SMS_DYNAREC_J0 && !SMS_TELEMETRY
#error "SMS_DYNAREC_J0 needs SMS_TELEMETRY: a measurement needs the clock"
#endif

/*
 * SMS_DYNAREC_J1 -- chaining translated blocks.
 *
 * J0 found what a translated block costs and, in finding it, found what really
 * costs: the boundary that loads the emulated registers in and stores them
 * back out, forty-five per cent of the block, paid on every one of them. This
 * builds the execution model that removes it -- registers resident across a
 * whole chain, a block branching straight into the next through a table of
 * block addresses, and a way out only when the next address has no
 * translation or the time is spent.
 *
 * Off by default, and scaffolding like J0: it emulates nothing and nothing may
 * come to depend on it. Same two needs, for the same two reasons -- the
 * test image the blocks came from is gone, telemetry owns the clock.
 *
 * Cuts: the block table, the chain, the reference run, the comparison, and
 * every line the four print -- both files entirely.
 */
#ifndef SMS_DYNAREC_J1
#define SMS_DYNAREC_J1 0
#endif

#if SMS_DYNAREC_J1
#error "SMS_DYNAREC_J1 cannot be built: the test image is no longer in the build"
#endif

#if SMS_DYNAREC_J1 && !SMS_TELEMETRY
#error "SMS_DYNAREC_J1 needs SMS_TELEMETRY: a measurement needs the clock"
#endif

/*
 * SMS_DYNAREC_J2 -- which of two things costs.
 *
 * J1 chained the blocks and the boundary's share fell fourfold, but the total
 * stopped short of what that alone predicted, and what was left sat between
 * two suspects one measurement could not separate: the link, and the cost of
 * the forms that write flags. This builds both answers -- a link of two
 * instructions, and flags read out of a table -- and runs the region four
 * times, with the two levers on and off independently, so that the share of
 * each is a figure rather than an opinion.
 *
 * Off by default, and scaffolding like the two before it. Same two needs, for
 * the same reasons.
 *
 * Cuts: the four variants, the block table, the flag tables, the reference
 * runs, the comparison and every line they print -- both files entirely.
 */
#ifndef SMS_DYNAREC_J2
#define SMS_DYNAREC_J2 0
#endif

#if SMS_DYNAREC_J2
#error "SMS_DYNAREC_J2 cannot be built: the test image is no longer in the build"
#endif

#if SMS_DYNAREC_J2 && !SMS_TELEMETRY
#error "SMS_DYNAREC_J2 needs SMS_TELEMETRY: a measurement needs the clock"
#endif

/*
 * ---------------------------------------------------------------------------
 * Sixteen bit access to emulated data: memory of the emulated machine, ROM
 * images, colour entries. Low byte first, whatever the host.
 *
 * These are accessors of correctness, not of portability. There is one target
 * and it is big endian, while everything the emulated machine stores is little
 * endian. Deleting them compiles clean and corrupts data, silently, which is
 * the worst way for a defect to behave.
 *
 * Both are built out of single byte accesses, and that is not a matter of
 * taste. The processor supports exactly two data types, the byte and the 32
 * bit word aligned to a four byte boundary (docs/3do/arm60.md:1003-1007):
 * there is no 16 bit load or store on this machine at all. Nor does the word
 * path fail usefully on a misaligned address -- the data is rotated silently
 * so that the addressed byte lands in bits 0 to 7 (:3210-3216), to the point
 * that the manual gives a whole routine for loading a word of unknown
 * alignment (:4976-4997). An immediate operand of the emulated processor falls
 * at any address, so the word path is both unavailable and wrong. A cast to a
 * 16 bit pointer is therefore a defect here even when it appears to work.
 *
 * Arguments are evaluated more than once: pass a plain pointer and a plain
 * value, never an expression with a side effect.
 * ---------------------------------------------------------------------------
 */
#define read16_le(p)                                    \
  ((uint16)((uint16)(((const uint8 *)(p))[0]) |         \
            ((uint16)(((const uint8 *)(p))[1]) << 8)))

#define write16_le(p, v)                                            \
  do                                                                \
    {                                                               \
      uint8 *write16_le_dst = (uint8 *)(p);                         \
      uint16 write16_le_val = (uint16)(v);                          \
                                                                    \
      write16_le_dst[0] = (uint8)(write16_le_val & 0xFFU);          \
      write16_le_dst[1] = (uint8)((write16_le_val >> 8) & 0xFFU);   \
    }                                                               \
  while(0)

#endif /* SMS3DO_COMMON_H */
