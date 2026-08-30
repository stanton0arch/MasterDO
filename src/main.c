#include "sys.h"
#include "log.h"
#include "z80.h"
#include "cart.h"
#include "vdp.h"
#include "dynarec_j0.h"
#include "dynarec_j1.h"
#include "dynarec_j2.h"

/*
 * Glyph width of the graphics folio's 8x8 font, used to centre text:
 * src_exemple/file_api/main.c:34, :102-103.
 */
#define MAIN_GLYPH_WIDTH 8

#define MAIN_TEXT_COLOR MakeRGB15(31,31,31)
#define MAIN_BACK_COLOR MakeRGB15(0,0,12)

static const char main_boot_text[] = SMS3DO_NAME " v" SMS3DO_VERSION " boot ok";

/*
 * The frame number belongs to main.c and to main.c alone. Every other module
 * only reads it; log.c receives its address and never writes through it, and
 * the frame loop below is the sole place that advances it.
 */
static uint32 main_frame = 0;

/*
 * Frames between two aggregate lines (cart_io_report, vdp_report): sixty
 * turns of the loop, one second at the field rate this loop paces to and a
 * fifth more on a 50 Hz host. The counter is what the frame loop
 * decrements.
 */
#define MAIN_IO_REPORT_FRAMES 60UL
static uint32 main_io_report_in = MAIN_IO_REPORT_FRAMES;

/*
 * One field per frame. The loop holds the console's own field rate rather than
 * a rate of its own choosing: whether that rate is 60 or 50 is the host's
 * business, it is reported at boot, and no timing here is written around
 * either figure.
 */
#define MAIN_VBL_STEP 1

/*
 * ---------------------------------------------------------------------------
 * How the emulated second is cut up, and why the two figures live here rather
 * than inside the processor.
 *
 * The processor owns its clock; it does not own the calendar. What a scanline
 * is worth and how many of them make a frame are properties of the video part
 * and of the region, so they belong to the loop that calls the modules in
 * order -- which is also the only place that could reconcile them if they ever
 * disagreed.
 *
 * Both are read off TotalSMS/src/core/sms.c:44, which gives a frame as
 * 228 * 262 T-states for the sixty hertz region. The product corroborates the
 * only figure a hardware document states: 228 * 262 * 60 is 3 584 160, which is
 * the 3.58 MHz of docs/sms_gg/SMSOfficialDocs.md:128.
 *
 * The line count is the sixty hertz one. The fifty hertz region runs 313 lines
 * (sms.c:45) and adapting to it is out of scope for this project, so no path
 * here tests for it: a branch never taken is a branch never checked. The
 * constant is named after the region it describes so that whoever reopens the
 * question knows exactly which figure has to change.
 * ---------------------------------------------------------------------------
 */
#define MAIN_TSTATES_PER_LINE 228
#define MAIN_LINES_PER_FRAME  262

/*
 * The video part wraps its line count on a figure of its own; the two
 * describe one raster and must agree, and a comment saying so is not a
 * check.
 */
#if MAIN_LINES_PER_FRAME != VDP_LINES_PER_FRAME
#error "frame loop and video part disagree on the line count"
#endif

/*
 * The measurement exists exactly when the line it feeds exists. Tying both to
 * the same condition is what keeps an accumulator from being kept alive for an
 * output that was compiled out -- which would leave its cost in the very build
 * whose figures are being read.
 */
#if LOG_ENABLE && SMS_TELEMETRY
#define MAIN_MEASURE 1
#else
#define MAIN_MEASURE 0
#endif

/*
 * The frame loop below is the one executor: it runs the cartridge by
 * scanline quotas, and MAIN_MEASURE alone is what the rest of this file
 * tests.
 */

#if MAIN_MEASURE
/*
 * The PC window: one sample of the program counter
 * per frame into this ring, counted for distinct values once per second.
 * What the count separates is the one confusion a bare progressing PC line
 * invites -- a game waiting for a VBlank it cannot see yet alternates
 * between one or two addresses, and read casually that looks like a run.
 * The per-second line names it instead: distinct<=2 is a wait loop, said
 * as such, never counted as progress.
 *
 * A power of two, so the write index folds by a mask, and about a second
 * of paced frames, so the window the count describes is the window of the
 * measurement line beside it. Ring, index and both lines exist only under
 * the telemetry switch, the rule of log.h: a counter kept to feed a line
 * that was compiled out would leave its cost in the measured build.
 */
#define MAIN_PC_RING 64
#endif

#if MAIN_MEASURE

/* One aggregate per second of wall time, and never more often than that. */
#define MAIN_PERF_PERIOD_USEC 1000000UL

/*
 * Above this many frames in one window, the scaling below is done the other
 * way round. See main_perf_emit.
 */
#define MAIN_PERF_FRAMES_MAX 400000UL

/*
 * The clock is not trusted blindly. Read on the console, the sampling call
 * (sys.h, sys_usec) now and then returns a reading off by whole seconds --
 * 1.4 to 12 of them in the runs that showed it -- and one such reading
 * poisons both windows it bounds: the one it closes reads high by that
 * much, the one it opens wraps below zero. Each measured stretch is
 * therefore compared to what it can physically be before it is added: a
 * frame's emulated stretch or its draw call above one second, or one
 * sampled line above a tenth of one, is a bad reading and is counted as
 * such (clk= on the periodic line) instead of averaged in. A line is a
 * fifth of a millisecond, a frame's stretch a few tens of them; nothing
 * legitimate reaches the bounds.
 */
#define MAIN_CLK_MAX_FRAME_USEC 1000000UL
#define MAIN_CLK_MAX_LINE_USEC   100000UL

/* Pairs of back-to-back readings taken once to price the reading itself. */
#define MAIN_CLK_COST_PAIRS 16UL

/*
 * One line in this many is timed around its video call, the phase walking
 * one step per frame so that every line of the frame is seen in as many
 * frames: eight or nine samples a frame spread over its whole height --
 * picture lines and blank ones in their true proportion, which one sample
 * a frame could not keep at the paces a slow render falls to -- for
 * sixteen or eighteen readings, half a percent of a frame held at pace.
 */
#define MAIN_PERF_SAMPLE_STRIDE 32UL

/*
 * Emits the periodic measurement.
 *
 * Every figure is computed in integers, and that is a hardware constraint and
 * not a preference: the ARM60 has no floating point unit and the build passes
 * -fpu none (Makefile:97), so a conversion of a real number would pull
 * software emulation of floating point into the diagnostic layer, on a path
 * that already blocks. Each value is therefore carried as tenths and split
 * into its whole and fractional parts only at the moment of printing.
 *
 * The window is passed in rather than sampled here so that the same reading of
 * the clock bounds the window and starts the next one, leaving no gap between
 * two of them for frames to fall into.
 */
static void
main_perf_emit(uint32 usec,
               uint32 frames,
               uint32 emul_usec,
               uint32 emul_frames,
               uint32 vdp_usec,
               uint32 vdp_samples,
               uint32 draw_usec,
               uint32 over,
               uint32 clk)
{
  uint32 fps10;
  uint32 frame10;
  uint32 emul_pf;
  uint32 vdp_pf;
  uint32 z8010;
  uint32 vdp10;
  uint32 draw10;

  /*
   * Guard, and the divisors below rest on it: a window of at least a second
   * makes usec / 1000 and usec / 1000000 both non-zero, and a window with no
   * frame in it has no average to report.
   */
  if((frames == 0UL) || (usec < MAIN_PERF_PERIOD_USEC))
    return;

  /*
   * fps * 10 is frames * 10^7 / usec, which no 32 bit intermediate can hold
   * directly. Dividing the denominator first keeps the numerator in range at
   * the paces this loop can reach; past that many frames per window -- which
   * only an unpaced run produces -- the ratio is taken the other way round,
   * coarser but unable to overflow.
   */
  if(frames < MAIN_PERF_FRAMES_MAX)
    fps10 = (frames * 10000UL) / (usec / 1000UL);
  else
    fps10 = (frames / (usec / 1000000UL)) * 10UL;

  /*
   * Microseconds per frame. The video part's figure is one sampled line
   * scaled to the lines of a frame -- the sample is bounded by
   * MAIN_CLK_MAX_LINE_USEC, so the product holds in 32 bits -- and the
   * processor's figure is the emulated stretch of a frame with that
   * estimate taken out: the stretch was read around the whole line loop,
   * lines included. Frames whose stretch was a bad reading are not in
   * emul_frames; a window where every reading was bad reports zero.
   */
  vdp_pf = (vdp_samples != 0UL)
           ? (vdp_usec / vdp_samples) * MAIN_LINES_PER_FRAME : 0UL;
  emul_pf = (emul_frames != 0UL) ? (emul_usec / emul_frames) : 0UL;

  /* Tenths of a millisecond per frame, averaged over the window. */
  frame10 = (usec / 100UL) / frames;
  z8010 = ((emul_pf > vdp_pf) ? (emul_pf - vdp_pf) : 0UL) / 100UL;
  vdp10 = vdp_pf / 100UL;
  draw10 = (draw_usec / 100UL) / frames;

  LOG_HOT(LOG_CAT_PERF,LOG_LVL_INFO,
          ("fps=%lu.%lu frame=%lu.%lums z80=%lu.%lums vdp=%lu.%lums draw=%lu.%lums over=%lu clk=%lu",
           (unsigned long)(fps10 / 10UL),(unsigned long)(fps10 % 10UL),
           (unsigned long)(frame10 / 10UL),(unsigned long)(frame10 % 10UL),
           (unsigned long)(z8010 / 10UL),(unsigned long)(z8010 % 10UL),
           (unsigned long)(vdp10 / 10UL),(unsigned long)(vdp10 % 10UL),
           (unsigned long)(draw10 / 10UL),(unsigned long)(draw10 % 10UL),
           (unsigned long)over,(unsigned long)clk));
}

#endif /* MAIN_MEASURE */

int
main(int    argc,
     char **argv)
{
  Err err;
  int32 x;
  int32 y;
  int32 paced;
  int32 vbl_delta;
  uint32 vbl_now;
  uint32 vbl_target;
  int32 line;
  /*
   * What the last instruction of a scanline spent past the end of its quota.
   * It is taken off the next line's quota, so no instruction is ever cut in
   * half nor charged twice.
   */
  int32 residue;
  /*
   * The cel the video part built at init, read once here and handed to
   * one draw call per frame. The concrete type is in scope in this file
   * through the graphics header; the video part hands the block over as
   * an opaque pointer and never draws or waits itself.
   */
  CCB *cel;
  /*
   * The draw call's verdict, kept so the compare on it sits after the
   * measured stretch has closed rather than inside it.
   */
  Err draw_err;
#if MAIN_MEASURE
  /* The near edge of the one measured stretch of the drawing side. */
  uint32 draw_start;
#endif
  /*
   * Whether the core has yet to be found stopped by the per-frame
   * consultation below. Cleared once, when the core has met an opcode it
   * cannot execute, and nothing ever sets it back: clearing it exactly
   * once is what makes the stop say itself exactly once.
   */
  int32 core_live = 1;
#if MAIN_MEASURE
  /*
   * The clock readings of one frame: the two around the emulated stretch,
   * the two around the one sampled line, and the reading being weighed.
   */
  uint32 emul_start;
  uint32 line_start;
  uint32 perf_delta;
  /*
   * What one reading of the clock costs, priced once at the head of the
   * loop as the least of a few back-to-back pairs, and taken out of every
   * sampled line: a line read between two calls into the operating
   * system otherwise carries one call's worth, tens of microseconds on
   * this console -- more than an empty line, a few percent of a rendered
   * one, and scaled by the lines of a frame either way.
   */
  uint32 perf_clock_cost;
  /* The phase of the sampled lines this frame; it steps once per turn. */
  uint32 perf_sample_phase = 0;
  uint32 perf_window;
  uint32 perf_now;
  uint32 perf_frames = 0;
  /*
   * Running total since the loop started, and deliberately not reset with the
   * rest: what is read off a series of these lines is whether the figure grows
   * and how fast, which a per-window count would lose the moment one line
   * scrolls past.
   */
  uint32 perf_over = 0;
  /*
   * The three counters the periodic line reports separately -- the
   * processor's quotas, the video part's lines, the draw call. They are
   * written by this function and by no other: a module told to do its
   * share of a turn does not time itself, because the timing belongs
   * where the turn is cut up and where the pace is held.
   */
  uint32 perf_emul = 0;
  uint32 perf_emul_frames = 0;
  uint32 perf_vdp = 0;
  uint32 perf_vdp_samples = 0;
  uint32 perf_draw = 0;
  /* Readings refused as impossible (MAIN_CLK_MAX_*), per window. */
  uint32 perf_clk = 0;
#endif
#if MAIN_MEASURE
  /*
   * The PC window (see MAIN_PC_RING above): the samples of the closing
   * second, and how many landed. The index grows over one window -- the
   * mask at the write picks the slot, so past MAIN_PC_RING samples the
   * ring holds the latest ones -- and the per-second reader starts it
   * over for the window that follows.
   */
  uint16 pc_ring[MAIN_PC_RING];
  uint32 pc_ring_n = 0;
#endif

  (void)argc;
  (void)argv;

  /*
   * Boot line. It is the very first output of the binary: if it is missing
   * from the serial debug output, nothing else can be diagnosed.
   */
  LOG_INFO(LOG_CAT_BOOT,("%s v%s build=%s log=%s",
                         SMS3DO_NAME,SMS3DO_VERSION,
                         log_build_date(),log_level_name()));

  /*
   * Every failure below ends the run through log_fatal, which paints before it
   * stops: no path may leave the console dark and silent. The LOG_ERR line
   * that precedes each call carries the raw SDK error number, which belongs to
   * the trace and would be noise on a screen the player reads.
   */
  err = sys_display_open();
  if(err < 0)
    {
      LOG_ERR(LOG_CAT_BOOT,("boot aborted: display open err=%ld",(long)err));
      /*
       * The only stop that cannot paint: this is the failure of the very thing
       * that would have painted it. log_fatal degrades to a trace, and the
       * black screen is the consequence rather than the cause.
       */
      log_fatal(LOG_CAT_BOOT,LOG_E_DISPLAY_OPEN,
                "cannot open the 3do display",
                "graphics folio refused a screen");
      return (int)err;
    }

  /*
   * The paint target is handed to the log layer as soon as it exists, and
   * before anything else can fail: from here on a fatal stop has a screen to
   * write on.
   */
  log_bind_screen(sys_bitmap(),sys_screen());

  err = sys_fill(MAIN_BACK_COLOR);
  if(err < 0)
    {
      LOG_ERR(LOG_CAT_BOOT,("boot aborted: background fill err=%ld",(long)err));
      log_fatal(LOG_CAT_BOOT,LOG_E_DISPLAY_FILL,
                "cannot paint the screen background",
                "FillRect refused the boot bitmap");
      return (int)err;
    }

  x = (sys_width() - (int32)(sizeof(main_boot_text) - 1) * MAIN_GLYPH_WIDTH) / 2;
  if(x < 0)
    x = 0;
  y = sys_height() / 2;

  err = sys_text(x,y,main_boot_text,MAIN_TEXT_COLOR);
  if(err < 0)
    {
      LOG_ERR(LOG_CAT_BOOT,("boot aborted: text draw err=%ld",(long)err));
      log_fatal(LOG_CAT_BOOT,LOG_E_DISPLAY_TEXT,
                "cannot draw text on the screen",
                "DrawText8 refused the boot banner");
      return (int)err;
    }

  err = sys_display_show();
  if(err < 0)
    {
      LOG_ERR(LOG_CAT_BOOT,("boot aborted: display show err=%ld",(long)err));
      log_fatal(LOG_CAT_BOOT,LOG_E_DISPLAY_SHOW,
                "cannot present the screen",
                "DisplayScreen refused the screen");
      return (int)err;
    }

#if LOG_SELFTEST_FATAL
  /*
   * Placed after the screen is up and presented, so that what it exercises is
   * the nominal path -- trace, paint, present, halt. Placed any earlier it
   * would only ever prove the degraded one.
   */
  log_fatal(LOG_CAT_BOOT,LOG_E_SELFTEST,
            "self-test: built-in fatal stop",
            "rebuild without the selftest switch");
#endif

  /*
   * Opening the pacer is allowed to fail, and the program carries on when it
   * does. sys_vbl_open says so in the trace; what is kept here is only whether
   * there is a clock to hold, because a loop that asked an unusable request to
   * wait on every turn would ask it several million times.
   */
  paced = (sys_vbl_open() >= 0);

  /*
   * The sound path is brought up last, and its failure is tolerated on the
   * same ground as the pacer's: a run without sound still shows a picture and
   * still traces, so it stays diagnosable, which a run stopped at the error
   * screen would not be.
   *
   * The result is discarded rather than kept, and that is not the same as
   * ignoring it: every step of the sequence names its own outcome in the
   * trace, and there is nothing this function would do differently either way.
   * No sound is expected out of the speakers -- what the walk brings up is
   * loaded and named, never wired and never played.
   */
  (void)sys_audio_open();

  /*
   * The emulated processor is brought up here, and the position in the
   * sequence is part of what it does. It takes the whole of the emulated
   * address space in one block, so it must come before the memory summary --
   * which would otherwise describe a program still missing its largest single
   * allocation -- and before the seal, past which it would be refused.
   *
   * Its failure is a stop and not a degraded mode, unlike the pacer and the
   * sound path above: those leave a run that still shows and still traces,
   * whereas a processor with no address space cannot execute one instruction.
   */
  err = z80_init();
  /*
   * Cannot fail: z80_init allocates nothing, the emulated memory being the
   * resident ROM plus the work RAM the cartridge boot allocates below --
   * whose refusal paints E200 with the words that name that block
   * (cart.c). The return stays in the contract for the day a core has
   * something of its own to allocate.
   */
  (void)err;

  /*
   * The cartridge comes next, and its position follows the processor's for
   * the same reason: it takes the largest block of the program, one megabyte
   * for the resident ROM, so it must come before the memory summary and
   * before the seal. The allocation and the load are two calls because they
   * are two different things -- the first is a budget, taken whole whatever
   * the disc holds; the second is a policy, the pair of names tried at boot.
   *
   * Both failures are stops. A buffer that cannot be had leaves no way to
   * ever load a program; a ROM that is missing, unreadable or of a refused
   * size leaves nothing to run, and the second call paints its own screen,
   * naming what was looked for or what was read, before it stops.
   */
  err = cart_init();
  if(err < 0)
    {
      LOG_ERR(LOG_CAT_CART,("boot aborted: cart init err=%ld",(long)err));
      log_fatal(LOG_CAT_CART,LOG_E_CART_ALLOC,
                "cannot allocate the rom buffer",
                "the console refused 1 megabyte");
      return (int)err;
    }

  err = cart_boot();
  if(err < 0)
    return (int)err;

  /*
   * The video part comes after the cartridge boot and before the reset:
   * after, because its init line names the profile that boot has just
   * fixed; before the seal, because it takes its video memory through the
   * allocator. Either failure is a stop: a program with no video memory has nowhere to put
   * its first tile, and a picture with no pixel buffer has nowhere to
   * come out.
   */
  /*
   * The screen text below states the buffer size in prose, and prose does
   * not follow a constant: the guard makes a change of buffer size refuse
   * to build until the words are brought back in line.
   */
#if VDP_PIX_BUF_BYTES != 36864UL
#error "the pixel buffer screen text says 36 kilobytes: update it with the new size"
#endif

  err = vdp_init();
  if(err < 0)
    {
      /*
       * Two allocations, two screens: the player quotes a code, and the
       * code must name the block that was refused.
       */
      LOG_ERR(LOG_CAT_VDP,("boot aborted: vdp init err=%ld",(long)err));
      if(err == VDP_ERR_NO_PIXELS)
        log_fatal(LOG_CAT_VDP,LOG_E_VDP_PIXELS,
                  "cannot allocate the pixel buffer",
                  "the console refused 36 kilobytes");
      else
        log_fatal(LOG_CAT_VDP,LOG_E_VDP_VRAM,
                  "cannot allocate the video ram",
                  "the console refused 16 kilobytes");
      return (int)err;
    }

  /*
   * The one reset of a boot, and its position is the point: after
   * cart_boot, whose installation has just put the ROM at the bottom of the
   * address space -- so the trace reads in the order the work happens,
   * work RAM allocated, map written, processor reset onto the program.
   * z80_init does not reset (z80.h).
   */
  z80_reset();

  /*
   * Every steady allocation is now in place -- the screen above, the sound
   * path just now, the emulated memory and the ROM buffer a moment ago -- so
   * this is where the footprint is worth measuring. Any
   * earlier and the figure would describe a program that is not finished
   * starting; the pattern this follows makes the same point
   * (src_exemple_video_player/main.c:915-921).
   */
  sys_mem_report();

  /*
   * Then the door is shut, and shut here rather than anywhere else. Earlier,
   * it would refuse memory to a part of the boot sequence that has every right
   * to it; later, it would let the frame loop through and prove nothing. What
   * comes after this line runs on what it already has.
   */
#if SMS_DYNAREC_J0
  /*
   * On this side of the door because it is the only side memory can be had
   * from, and it takes three blocks: the one it writes native code into, an
   * address space of its own for the translated run, and the reference
   * program as an image. It runs nothing here -- what it has done on return is
   * copy code into memory.
   */
  (void)dynarec_j0_install();
#endif

#if SMS_DYNAREC_J1
  /* Same side of the door, and for the same reason. */
  (void)dynarec_j1_install();
#endif

#if SMS_DYNAREC_J2
  /* Same side of the door, and for the same reason. */
  (void)dynarec_j2_install();
#endif

  sys_mem_seal();

#if SMS_DYNAREC_J0
  /*
   * And measured here, after the seal and before the frame loop. After,
   * because a mock-up that needed memory to run would be hiding an allocation
   * the boot footprint never sees; before, because it holds the processor for
   * a second or two and a frame that took a second would be a frame destroyed
   * rather than a frame slowed.
   *
   * Its verdict is a figure, so there is nothing here to branch on: a mock-up
   * that refuses to publish one has said why, and a boot that stopped over it
   * would be stopping over a measurement.
   */
  (void)dynarec_j0_measure();
#endif

#if SMS_DYNAREC_J1
  (void)dynarec_j1_measure();
#endif

#if SMS_DYNAREC_J2
  (void)dynarec_j2_measure();
#endif

  /*
   * Last line of the boot sequence and the last one to carry f=-, so that the
   * switch to a real frame number in the field marks the exact point where the
   * nominal regime begins.
   */
  LOG_INFO(LOG_CAT_SYS,("main loop entered"));

  /*
   * The counter is published to log.c here, between that line and the loop:
   * every line above carries f=-, every line below carries a frame number.
   */
  log_set_frame(&main_frame);

  /*
   * One read of the cel for the whole run: the block never moves after
   * init, so reading it per frame would buy nothing and cost a call on
   * every turn.
   */
  cel = (CCB *)vdp_cel();

  /*
   * The witness fill, once, before any frame: a magenta ground under
   * everything the run will draw. It serves two readings at one glance.
   * Around the picture it is the framing border -- its width on each side
   * is the centring offset, wrong framing shows as unequal borders -- and
   * under the picture it is a colour the palette does not contain: were
   * the black entry drawn transparent, the black band would read magenta
   * instead of black. It covers the boot banner, which has said its word
   * by now; the screen is never filled again, so the border stays -- and
   * that survival rests on sys.c never advancing sc_CurrentScreen: every
   * frame draws into the one bitmap this fill painted. A buffering policy
   * that starts flipping screens breaks that silence and must repaint the
   * border on both.
   */
  (void)sys_fill(MakeRGB15(31,0,31));

  vbl_target = sys_vbl_count() + MAIN_VBL_STEP;

  /*
   * Carried across frames as well as across lines. A frame boundary is a
   * scanline boundary like any other, and it settles no debt: whatever the
   * last instruction of a frame spent past the end of its quota is owed by the
   * first line of the next one.
   */
  residue = 0;

#if MAIN_MEASURE
  /*
   * The price of one reading, said once so that the figures it is taken
   * out of can be read back with it: the mean of a few back-to-back
   * pairs. Not the least of them -- the clock steps coarsely enough that
   * the least pair reads about half the typical one, and a floor of that
   * half, scaled by the lines of a frame, showed on the console as four
   * milliseconds of video work in a build that rendered nothing.
   * A pair that reads as impossible is a bad reading and is left out.
   */
  perf_clock_cost = 0;
  line_start = 0;
  for(perf_delta = 0; perf_delta < MAIN_CLK_COST_PAIRS; perf_delta++)
    {
      emul_start = sys_usec();
      emul_start = sys_usec() - emul_start;
      if(emul_start < MAIN_CLK_MAX_LINE_USEC)
        {
          perf_clock_cost += emul_start;
          line_start++;
        }
    }
  if(line_start != 0UL)
    perf_clock_cost /= line_start;
  LOG_INFO(LOG_CAT_PERF,("clock read cost=%luus (taken out of each vdp sample)",
                         (unsigned long)perf_clock_cost));

  perf_window = sys_usec();
#endif

  /*
   * The frame loop. It never returns: were main() to hand control back to the
   * 3DO shell the picture would vanish with the task.
   *
   * This is the one place in the program that waits. Pacing lives here and
   * nowhere else -- two modules each holding a pace of their own would wait
   * twice and neither would own the result.
   */
  for(;;)
    {
      /*
       * One cold consultation per frame, before the measured stretch and
       * outside it. A core that meets an opcode it cannot execute has
       * named it, once, in its own trace, and refuses every quota after
       * that on its own, at the top of the call: what this reading adds is
       * the fact said once, on the loop's schedule, so that a picture that
       * stops moving can be told from a program that stopped drawing. The
       * loop itself keeps turning: its pace, its breath and its periodic
       * line are what the rest of the run is read by.
       */
      if(core_live && z80_is_stopped())
        {
          core_live = 0;

          /*
           * A field handed to the host on the near side of the blocking
           * write below, as everywhere else a serial line is about to be
           * paid: output that only completes once something else has run
           * must be entered with the processor already given up once. A
           * one-time event, outside anything measured; without a pacer the
           * call refuses without blocking.
           */
          (void)sys_vbl_wait(1);
          LOG_INFO(LOG_CAT_Z80,("core stopped"));
        }

      /*
       * The emulated work of one turn, weighed by two of the three
       * accumulators the measurement reports: the processor's quotas and
       * the video part's lines, apart from each other. The third weighs
       * the draw call at the end of the frame. The three are kept apart
       * because an optimisation decision taken later would otherwise have
       * no way of telling which of them it was looking at.
       *
       * The clock is read twice around the whole line loop, for the
       * emulated stretch, and twice around the video call of one line in
       * MAIN_PERF_SAMPLE_STRIDE, the phase stepping once per frame so that
       * the sampled lines walk the whole frame. The video figure is the
       * mean sample scaled to a frame, the processor figure is the
       * stretch with it taken out (main_perf_emit). This
       * loop once read the clock around every quota and every line -- 525
       * readings a frame, the exact figure with no estimate in it -- and
       * the console refused it: the call is documented as very low
       * overhead (docs/3do/3do_portfolio_2.5.md:19573) and costs some
       * forty microseconds, twenty milliseconds a frame, a third of the
       * frame this loop is meant to hold. Two readings a frame, the
       * arrangement before that, could not tell the render from the
       * quotas once the line call rendered. The mask and compare that
       * pick the sampled lines are the one thing this arrangement adds
       * inside the loop, 262 times a frame, against the thousands of
       * cycles of the quota beside it.
       */
#if MAIN_MEASURE
      emul_start = sys_usec();
#endif

      /*
       * The pace is the scanline, and it is the scanline because that is the
       * grain the raster effects of this machine are written at: a frame at a
       * time would run a whole screen with one set of video registers, and an
       * event scheduler would cost a load and a comparison per emulated
       * instruction, on the path that has 3.49 ARM cycles per T-state to
       * spend.
       *
       * The order inside a line is the processor first, then the video
       * part: the quota runs, then the line ends. What the video part
       * raises at the end of a line -- the frame interrupt, the line
       * interrupt -- the processor sees at the head of its next quota,
       * where it samples the line; that is the status latched at HBlank of
       * the hardware, at this grain.
       *
       * The empty place below is named and left empty on purpose. It holds
       * no call at all, not even to a function that does nothing, because a
       * call to nothing still costs what a call costs -- and it would run
       * 262 times a frame, the one after the loop once. The video part's
       * call is real and is paid: one call per line, against the thousands
       * of cycles the quota beside it costs.
       *
       * The overrun is not cleared here. Clearing it once a frame would hand
       * the processor most of a free instruction sixty times a second, and
       * would do it in the one place the accounting has no way of noticing.
       */
      for(line = 0; line < MAIN_LINES_PER_FRAME; line++)
        {
          residue = z80_run((int32)MAIN_TSTATES_PER_LINE - residue);

#if MAIN_MEASURE
          if((line & (MAIN_PERF_SAMPLE_STRIDE - 1UL)) == perf_sample_phase)
            {
              line_start = sys_usec();
              vdp_line();
              perf_delta = sys_usec() - line_start;
              if(perf_delta < MAIN_CLK_MAX_LINE_USEC)
                {
                  perf_vdp += (perf_delta > perf_clock_cost)
                              ? (perf_delta - perf_clock_cost) : 0UL;
                  perf_vdp_samples++;
                }
              else
                perf_clk++;
            }
          else
            vdp_line();
#else
          vdp_line();
#endif

          /* place of the sound part: accumulate this line's samples */
        }
#if MAIN_MEASURE
      perf_delta = sys_usec() - emul_start;
      if(perf_delta < MAIN_CLK_MAX_FRAME_USEC)
        {
          perf_emul += perf_delta;
          perf_emul_frames++;
        }
      else
        perf_clk++;
      perf_sample_phase = (perf_sample_phase + 1UL) & (MAIN_PERF_SAMPLE_STRIDE - 1UL);
#endif

      /*
       * The end of the frame: the one draw call of the turn, then the
       * presentation. The draw is what the second accumulator weighs --
       * the emulated stretch has already closed its own clock above, so
       * the two figures never overlap -- and the presentation stays
       * outside the measurement: it is one cold call per frame, and the
       * figure being read here is the cel engine's, not the display's.
       */
#if MAIN_MEASURE
      draw_start = sys_usec();
#endif
      draw_err = DrawCels(sys_bitmap(),cel);
#if MAIN_MEASURE
      perf_delta = sys_usec() - draw_start;
      if(perf_delta < MAIN_CLK_MAX_FRAME_USEC)
        perf_draw += perf_delta;
      else
        perf_clk++;
#endif
      /*
       * A refused draw is traced once and the run goes on: the trace is
       * what separates a black picture from a dead loop. The compare sits
       * on the cold side, after the measured stretch has closed, so the
       * figure never includes it; the presentation's own failure is
       * traced by sys.c itself and is not doubled here.
       */
      if(draw_err < 0)
        LOG_ONCE(LOG_CAT_VDP,LOG_LVL_ERR,
                 ("draw failed err=%ld",(long)draw_err));
      (void)sys_display_show();

#if MAIN_MEASURE
      /*
       * One PC reading per frame, on the cold side of the measured stretch:
       * the raw per-frame line at debug level -- the reading a stuck boot
       * is diagnosed from, absent from the default build -- and the same value
       * into the ring the per-second window line counts distinct values out
       * of. Sampled once, into a local: z80_pc is a call, and the line and
       * the ring must see the same frame.
       */
      {
        uint16 pc_now = z80_pc();

        LOG_HOT(LOG_CAT_Z80,LOG_LVL_DBG,("pc=0x%04lx",(unsigned long)pc_now));

        pc_ring[pc_ring_n & (MAIN_PC_RING - 1UL)] = pc_now;
        pc_ring_n++;
      }
#endif

      if(paced)
        {
          vbl_now = sys_vbl_count();

          /*
           * The comparison is a signed difference and not a test of two
           * counters against each other. The field counter wraps, and around
           * that wrap an absolute comparison reverses its answer, while a
           * difference stays exact.
           */
          vbl_delta = (int32)(vbl_now - vbl_target);

          if(vbl_delta > 0)
            {
              /*
               * The deadline is already behind us. The lost time is dropped
               * rather than replayed: catching up would run the following
               * frames as fast as they will go, turning one late frame into a
               * burst of early ones. What must not happen is that it passes
               * unnoticed, so it is counted and reported.
               *
               * What to do about it beyond counting -- skip the drawing, let
               * it slip -- is deliberately not decided here. That choice is
               * meant to be made on the series of measurements this loop
               * produces, and making it now would be making it with the very
               * data it was supposed to rest on still unwritten.
               */
              vbl_target = vbl_now;
#if MAIN_MEASURE
              perf_over++;
#endif
            }
          else if(vbl_delta < 0)
            {
              (void)sys_vbl_wait((uint32)(-vbl_delta));
            }

          vbl_target += MAIN_VBL_STEP;
        }

      main_frame++;

      /*
       * The two aggregates, once per sixty frames: a countdown rather than
       * a remainder, this target having no divider. The bus reports what
       * reached an empty hook, the video part what reached it; each emits
       * nothing when it has nothing to say, and each emission is a
       * blocking serial write on the cold side of the frame, after the
       * pacing wait above -- the position the measurement line holds.
       */
      if(--main_io_report_in == 0)
        {
          main_io_report_in = MAIN_IO_REPORT_FRAMES;
          cart_io_report();
          vdp_report();
        }

#if MAIN_MEASURE
      perf_frames++;

      perf_now = sys_usec();
      if((perf_now - perf_window) >= MAIN_PERF_PERIOD_USEC)
        {
          /*
           * Once per second of wall time, never per frame. The serial output
           * blocks, so a line emitted on every turn would not slow the frame
           * down, it would destroy it -- and the figures then measured would
           * be those of the tracing.
           *
           * A second of wall time is what is available while nothing is being
           * emulated, and it is also what will keep working once something is:
           * it depends neither on the field rate of the host nor on any
           * supposed frame rate.
           */
          main_perf_emit(perf_now - perf_window,perf_frames,
                         perf_emul,perf_emul_frames,perf_vdp,perf_vdp_samples,
                         perf_draw,perf_over,perf_clk);

          /*
           * The PC window, at the pace of the measurement line above and on
           * its cold side. This is the reading that separates three
           * outcomes: a value that moves is a run making progress, a value
           * that does not while the lines keep coming is the emulated
           * program looping, and no lines at all is this loop itself
           * stopped -- a fault of the host, not of the emulation. The
           * count is over the frames of the
           * closing second: a range of distinct values is a program
           * advancing; a handful of values repeating is a wait loop -- a
           * game parked on a VBlank nothing raises yet reads exactly like
           * that -- and the line says so itself rather than leaving a few
           * alternating values to pass for progress. The threshold is 4
           * because a polling loop is a handful of instructions (read,
           * compare, jump back) and the per-frame sample can land on any
           * of them: the first real ROM run showed exactly three. The
           * quadratic count is at most 64 by 64 compares, once a second,
           * outside everything measured.
           */
          {
            uint32 pc_i;
            uint32 pc_j;
            uint32 pc_seen;
            uint32 pc_distinct;
            unsigned long pc_last;

            pc_seen = (pc_ring_n < MAIN_PC_RING) ? pc_ring_n : MAIN_PC_RING;
            pc_distinct = 0;

            for(pc_i = 0; pc_i < pc_seen; pc_i++)
              {
                for(pc_j = 0; pc_j < pc_i; pc_j++)
                  if(pc_ring[pc_j] == pc_ring[pc_i])
                    break;
                if(pc_j == pc_i)
                  pc_distinct++;
              }

            pc_last = (pc_seen > 0)
                      ? (unsigned long)
                        pc_ring[(pc_ring_n - 1UL) & (MAIN_PC_RING - 1UL)]
                      : 0UL;

            /*
             * Two spellings of one line, whole in each branch: the suffix
             * is the diagnosis and must not be composable away by a format
             * trick a grep would then miss.
             */
            if(pc_distinct <= 4)
              LOG_HOT(LOG_CAT_Z80,LOG_LVL_INFO,
                      ("pc window distinct=%lu last=0x%04lx wait loop?",
                       (unsigned long)pc_distinct,pc_last));
            else
              LOG_HOT(LOG_CAT_Z80,LOG_LVL_INFO,
                      ("pc window distinct=%lu last=0x%04lx",
                       (unsigned long)pc_distinct,pc_last));

            /* The next window counts its own frames, not this one's. */
            pc_ring_n = 0;
          }

          perf_window = perf_now;
          perf_frames = 0;
          perf_emul = 0;
          perf_emul_frames = 0;
          perf_vdp = 0;
          perf_vdp_samples = 0;
          perf_draw = 0;
          perf_clk = 0;
        }
#endif
    }
}
