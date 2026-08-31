#ifndef SMS3DO_LOG_H
#define SMS3DO_LOG_H

/*
 * Logging layer: the single interface through which every diagnostic output of
 * the emulator passes. No module ever calls an SDK printing primitive
 * directly.
 *
 * This header is included by every module. It depends on common.h only, which
 * is itself a leaf of the dependency tree -- log depends on common, and on
 * nothing else.
 */

#include "common.h"

/*
 * The eleven categories are fixed. They are preprocessor integers rather than
 * an enumeration so that the category mask can be evaluated at compile time.
 */
#define LOG_CAT_BOOT  0
#define LOG_CAT_SYS   1
#define LOG_CAT_CART  2
#define LOG_CAT_BUS   3
#define LOG_CAT_Z80   4
#define LOG_CAT_VDP   5
#define LOG_CAT_PSG   6
#define LOG_CAT_PAD   7
#define LOG_CAT_SAVE  8
#define LOG_CAT_PERF  9
#define LOG_CAT_GG    10
#define LOG_CAT_COUNT 11

/* Every category on, whatever their number grows to. */
#define LOG_CAT_ALL ((1UL << LOG_CAT_COUNT) - 1UL)

/*
 * The five levels, from most severe to most verbose. The ascending order is
 * what makes the LOG_LEVEL >= level comparison meaningful.
 */
#define LOG_LVL_ERR   0
#define LOG_LVL_WARN  1
#define LOG_LVL_INFO  2
#define LOG_LVL_DBG   3
#define LOG_LVL_TRACE 4
#define LOG_LVL_COUNT 5

/*
 * What each switch of common.h actually does to this file, stated plainly
 * because two of them do not do the same kind of thing:
 *
 *   LOG_ENABLE and LOG_LEVEL are applied by #if below. When they exclude a
 *   line, its whole argument list -- the format string included -- is never
 *   handed to the compiler, so nothing of it can remain in the binary.
 *
 *   LOG_CAT_MASK is applied by LOG_PASSES, a constant expression the compiler
 *   folds and whose dead branch it drops. That suppresses the output and the
 *   work behind it, but says nothing about the string literal, which the
 *   compiler is free to leave in its constant pool. The mask is a filter; it
 *   is not a means of keeping text out of a binary.
 *
 *   SMS_TELEMETRY is applied by #if to LOG_HOT only, and so removes hot path
 *   lines exactly the way LOG_ENABLE removes all of them.
 */

/*
 * True if the line must exist. Both operands are preprocessor constants at
 * every call site: the condition is folded at compile time and costs nothing
 * at run time.
 */
#define LOG_PASSES(cat, lvl) \
  ((((LOG_CAT_MASK) & (1UL << (cat))) != 0UL) && ((lvl) <= (LOG_LEVEL)))

/*
 * Double-parenthesis idiom, required by C89: __VA_ARGS__ is a C99 construct
 * and this code base is strict C89. The args parameter is an already
 * parenthesised argument list:
 *
 *   LOG_INFO(LOG_CAT_SYS, ("screen init ok %ldx%ld", (long)w, (long)h));
 *
 * At LOG_ENABLE 0 that list is discarded by the preprocessor: the format
 * string never reaches the compiler, so it cannot survive in the binary.
 *
 * Corollary: the arguments must be free of side effects, since they vanish
 * when instrumentation is switched off.
 */
#if LOG_ENABLE
#define LOG_EMIT(cat, lvl, args)                    \
  do                                                \
    {                                               \
      if(LOG_PASSES((cat),(lvl)))                   \
        {                                           \
          log_begin((int32)(cat),(int32)(lvl));     \
          log_printf args;                          \
        }                                           \
    }                                               \
  while(0)
#else
#define LOG_EMIT(cat, lvl, args) ((void)0)
#endif

/*
 * One macro per level. The #if is what strips the format string out of the
 * binary when a level is switched off; LOG_PASSES alone would not.
 */
#if LOG_ENABLE && ((LOG_LVL_ERR) <= (LOG_LEVEL))
#define LOG_ERR(cat, args) LOG_EMIT((cat),LOG_LVL_ERR,args)
#else
#define LOG_ERR(cat, args) ((void)0)
#endif

#if LOG_ENABLE && ((LOG_LVL_WARN) <= (LOG_LEVEL))
#define LOG_WARN(cat, args) LOG_EMIT((cat),LOG_LVL_WARN,args)
#else
#define LOG_WARN(cat, args) ((void)0)
#endif

#if LOG_ENABLE && ((LOG_LVL_INFO) <= (LOG_LEVEL))
#define LOG_INFO(cat, args) LOG_EMIT((cat),LOG_LVL_INFO,args)
#else
#define LOG_INFO(cat, args) ((void)0)
#endif

#if LOG_ENABLE && ((LOG_LVL_DBG) <= (LOG_LEVEL))
#define LOG_DBG(cat, args) LOG_EMIT((cat),LOG_LVL_DBG,args)
#else
#define LOG_DBG(cat, args) ((void)0)
#endif

#if LOG_ENABLE && ((LOG_LVL_TRACE) <= (LOG_LEVEL))
#define LOG_TRACE(cat, args) LOG_EMIT((cat),LOG_LVL_TRACE,args)
#else
#define LOG_TRACE(cat, args) ((void)0)
#endif

/*
 * The hot path's own entry point: what runs per instruction, per scanline or
 * per frame is written with this macro rather than with the level macros
 * above, so that one switch takes all of it out at once.
 *
 * The level still applies -- LOG_HOT is subject to LOG_ENABLE, LOG_LEVEL and
 * the mask exactly like any other line. SMS_TELEMETRY only adds a condition on
 * top, and cutting it leaves the boot banner, the init lines and the errors
 * untouched: a run whose figures are being read must still be able to say what
 * it is.
 *
 * A counter or a timer kept only to feed one of these lines belongs inside the
 * same #if SMS_TELEMETRY as the line itself, not merely inside the macro call
 * -- otherwise the cost stays in the measured build even though the output is
 * gone (src_exemple_video_player/cinepak_decoder.c:14-24, main.c:922-939).
 */
#if LOG_ENABLE && SMS_TELEMETRY
#define LOG_HOT(cat, lvl, args) LOG_EMIT((cat),(lvl),args)
#else
#define LOG_HOT(cat, lvl, args) ((void)0)
#endif

/*
 * One-shot pattern, after src_exemple_video_player/cinepak_decoder.c:660-666
 * (vid_dims_warned guard) and audio_sync.h:79 (stall_printed). It lives here
 * so that it is not reinvented at every call site: the serial printf is
 * blocking, so a diagnostic that could repeat must be emitted only once.
 *
 * The do { } while(0) is essential: without it the macro breaks inside a
 * braceless if. At LOG_ENABLE 0 the static flag itself disappears.
 */
#if LOG_ENABLE
#define LOG_ONCE(cat, lvl, args)                    \
  do                                                \
    {                                               \
      static int log_once_flag = 0;                 \
      if(!log_once_flag)                            \
        {                                           \
          log_once_flag = 1;                        \
          LOG_EMIT((cat),(lvl),args);               \
        }                                           \
    }                                               \
  while(0)
#else
#define LOG_ONCE(cat, lvl, args) ((void)0)
#endif

/*
 * Publishes the address of the frame counter. The frame number belongs to
 * main.c and to main.c alone: log.c keeps a read-only pointer to it and never
 * writes through it. While no counter is published, or if frame is NULL, lines
 * carry f=-.
 */
void log_set_frame(const uint32 *frame);

/*
 * Compose one line. They are never called directly: the macros above are the
 * only entry point, otherwise instrumentation would no longer compile away to
 * nothing.
 *
 * log_begin builds the [<CAT>][<LVL>] f=<frame> prefix, log_printf appends the
 * message and emits the line through the single printf of the code base
 * (include/3do/stdio.h:51).
 */
void log_begin(int32 cat, int32 lvl);
void log_printf(const char *fmt, ...);

/*
 * Error code registry.
 *
 * A code is what a player without access to the serial console reads off the
 * screen and quotes back, so it is a published identifier: once a code has
 * shipped it is never reassigned, and a retired condition leaves its number
 * unused rather than freeing it. The ranges are spaced by subsystem so that
 * each can grow without renumbering its neighbours.
 *
 *   E0xx  boot and screen           E4xx  PSG
 *   E1xx  cartridge and ROM load    E5xx  save
 *   E2xx  memory bus                E6xx  ROM menu
 *   E3xx  VDP                       E7xx  Z80 core
 */
#define LOG_E_SELFTEST     1
#define LOG_E_DISPLAY_OPEN 2
#define LOG_E_DISPLAY_FILL 3
#define LOG_E_DISPLAY_TEXT 4
#define LOG_E_DISPLAY_SHOW 5

/*
 * The cartridge range, opened with the four ways a ROM can fail to be there
 * when the console starts. None of them is a warning: a machine with no
 * program has nothing to run, so every one of them stops on the screen and
 * names, in plain words, the directory and the file names that were tried, or
 * the size that was read and the bound it broke.
 */
#define LOG_E_CART_NOT_FOUND 100 /* neither expected file is on the disc */
#define LOG_E_CART_READ      101 /* the file is there but its bytes are not */
#define LOG_E_CART_SIZE      102 /* the size read breaks one of the five bounds */
#define LOG_E_CART_ALLOC     103 /* the resident ROM buffer could not be had */

/*
 * The emulated address space is memory of the bus rather than of the console,
 * so it takes its number in the bus range and not in the one above.
 */
#define LOG_E_Z80_RAM 200

/*
 * The core's range. Its first number, E700, was the built-in test suite's
 * image-checksum stop; the suite left the build and took the condition with
 * it. The number stays spent rather than freed -- the rule above holds for
 * any number a build has carried -- so the next condition of the core takes
 * E701 and the range starts there.
 */

/*
 * The video range. The first three are allocations refused at boot, and
 * each is a stop rather than a degraded mode: a program writes its tiles
 * into the video memory before it does anything else, a picture with no
 * pixel buffer has nowhere to come out, and the background is composed out
 * of the decoded row cache with no path around it.
 *
 * The fourth is not an allocation. The render reads pixel indexes out of
 * words, so it is built for one byte order (vdp.h, VDP_LANE_MSB_FIRST);
 * the boot holds that against the machine and stops here when they
 * disagree, because the alternative is a picture scrambled four pixels at
 * a time with nothing saying why.
 */
#define LOG_E_VDP_VRAM      300
#define LOG_E_VDP_PIXELS    301
#define LOG_E_VDP_TILECACHE 302
#define LOG_E_VDP_LANEORDER 303

/*
 * Publishes where the fatal error screen must be painted: the bitmap to draw
 * into and the screen to present afterwards.
 *
 * The target is injected rather than looked up, and that is what keeps the
 * dependency graph acyclic. This file may depend on common.h and on the SDK
 * only; asking the screen module where to draw would reverse an existing edge,
 * and would leave this file unable to paint at the very moment it is the one
 * detecting the failure.
 *
 * Called once, after the display exists. Until then both Items are zero and a
 * fatal stop traces without painting.
 * Source: include/3do/graphics.h:796 (DrawText8 takes a bitmap Item), :793
 * (DisplayScreen takes a screen Item).
 */
void log_bind_screen(Item bitmap, Item screen);

/*
 * Paints the code and its description on screen, emits the same information as
 * an [<CAT>][ERR] trace, then stops the console for good. Never returns.
 *
 * Both outputs matter and neither replaces the other: the trace is the
 * developer's channel, the picture is the only one a player has. They always
 * carry the same code and the same words.
 *
 * The message comes as two short lines rather than one string because
 * DrawText8 wraps nothing (include/3do/graphics.h:796): splitting is the
 * caller's job, and two lines are enough to say what was expected and what was
 * found.
 *
 * Each line must fit 37 characters. The narrowest raster the console presents
 * is 320 pixels wide and the folio font is 8 pixels per glyph, which leaves 40
 * columns, less the three the left margin takes. Past that the text runs off
 * the edge -- it is not wrapped, and the part that carries the diagnosis is
 * usually the part that disappears.
 *
 * Two properties of that font, established by reading it off a screen rather
 * than from any document, shape what a message may say. It draws every letter
 * as a capital, whatever the case of the string. And it draws no underscore at
 * all: an identifier written LOG_SELFTEST_FATAL reaches the eye as LOG
 * SELFTEST FATAL, so a message naming a macro, a filename or a symbol that
 * carries one tells its reader something untrue. Name such things in the trace,
 * where they survive, and keep the painted line to plain words. Colon, comma
 * and hyphen do render.
 *
 * Unlike the macros above this function is unconditional. It survives an
 * instrumentation-free build, which is precisely the build a player runs.
 */
void log_fatal(int32 cat, int32 code, const char *line1, const char *line2);

/*
 * Build date as YYYY-MM-DD, for the boot line. Derived from the standard
 * predefined macro __DATE__.
 */
const char *log_build_date(void);

/*
 * Name of the level actually active in this binary, so that the log=<level>
 * field of the boot line tells the truth about what the code enforces.
 */
const char *log_level_name(void);

#endif /* SMS3DO_LOG_H */
