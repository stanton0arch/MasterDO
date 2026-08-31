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
 * The breakdown of the render into posts (common.h, SMS_VDP_PROFILE) is a
 * reading of the measurement, so it exists only where the measurement
 * does. It adds no clock reading of its own: it steps a selector the
 * video part obeys, and reads the cost of a post off the displacement of
 * the figure the periodic line already publishes.
 */
#if MAIN_MEASURE && SMS_VDP_PROFILE
#define MAIN_PROFILE 1
#else
#define MAIN_PROFILE 0
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
 * side of a sampled line above a tenth of one, is a bad reading and is
 * counted as such (clk= on the periodic line) instead of averaged in.
 *
 * The line bound is per SIDE and not per line: a sampled line is weighed
 * as two spans, the quota and the video call, and each is compared to the
 * bound on its own, so a whole sampled line is refused only past two
 * tenths of a second. That is deliberate -- the bound exists to catch a
 * clock that jumped by seconds, not to cap a slow render -- and it is
 * what makes the two sides refusable one without the other, which the
 * per-side counting of clk= then reports.
 *
 * A whole line is a fifth of a millisecond, a frame's stretch a few tens
 * of them; nothing legitimate reaches the bounds.
 */
#define MAIN_CLK_MAX_FRAME_USEC 1000000UL
#define MAIN_CLK_MAX_LINE_USEC   100000UL

/* Pairs of back-to-back readings taken once to price the reading itself. */
#define MAIN_CLK_COST_PAIRS 16UL

/*
 * One line in this many is timed on both of its sides -- around the
 * processor's quota and around the video call -- the phase walking one
 * step per frame so that every line of the frame is seen in as many
 * frames: eight or nine samples a frame spread over its whole height --
 * picture lines and blank ones in their true proportion, which one sample
 * a frame could not keep at the paces a slow render falls to. Three
 * readings a sample, the middle one closing the quota and opening the
 * video call, so twenty-four to twenty-seven a frame: one reading a
 * sample more than timing the video call alone, and what it buys is a
 * figure for the processor that is measured rather than deduced by
 * subtraction.
 *
 * A power of two, and it has to be: the line that carries the sample is
 * picked with a mask and the phase is wrapped with the same mask, both
 * cheaper than a remainder on a path that runs 262 times a frame. A value
 * that is not a power of two would not select one line in that many, it
 * would select the wrong lines silently.
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
               uint32 z80_usec,
               uint32 z80_samples,
               uint32 vdp_usec,
               uint32 vdp_samples,
               uint32 draw_usec,
               uint32 over,
               uint32 clk)
{
  uint32 fps10;
  uint32 frame10;
  uint32 emul10;
  uint32 z80_mean;
  uint32 vdp_mean;
  uint32 share;
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
   * The emulated stretch of one frame, in tenths of a millisecond: the
   * measured quantity, read around the whole line loop with the lines
   * included. Frames whose stretch was a bad reading are not in
   * emul_frames; a window where every reading was bad reports zero.
   */
  emul10 = ((emul_frames != 0UL) ? (emul_usec / emul_frames) : 0UL) / 100UL;

  /*
   * The two halves of that stretch, and they are a SHARE of it and not
   * two figures of their own. The sampled lines are timed on both sides
   * -- once around the quota, once around the video call -- and what the
   * two accumulators are used for is the ratio between them, applied to
   * the stretch that was actually measured.
   *
   * The arrangement it replaces subtracted one mean sample scaled to the
   * lines of a frame from the measured stretch, which mixed an estimate
   * with a measurement: nothing made the estimate the smaller of the two,
   * and when it was not, a floor published a processor figure of zero
   * beside an inflated video one, and the pair summed past the frame. As
   * a share, the sum is the stretch by construction, no floor can be
   * reached, and the sampling bias -- a fixed phase sees a different mix
   * of picture and blank lines each frame -- falls on both sides at once
   * and cancels in the ratio.
   *
   * The means and not the raw totals, and that is the overflow guard the
   * scaling above uses in its own way: a total grows with the window and
   * would carry the product past 32 bits, where a mean is bounded by
   * MAIN_CLK_MAX_LINE_USEC. Tenths of a millisecond per frame times a
   * bounded mean holds with room to spare.
   *
   * A share needs both of its sides, and the condition below asks for
   * both of them twice over: a sample kept on each side, and a mean that
   * is not zero on each side. One side alone would divide the whole
   * stretch by itself and hand it all to that side, which is exactly the
   * shape this arrangement exists to make impossible -- a processor
   * figure of zero beside a video figure carrying the frame. That shape
   * is reachable two ways: every reading of one side refused as
   * impossible, and a side whose mean falls to zero once the price of a
   * clock reading is taken off it. Both are refused here, and the two
   * figures are then published as zero together. A window that could not
   * weigh both sides says so by weighing neither; it must never read as a
   * window that measured everything on one side.
   */
  z80_mean = (z80_samples != 0UL) ? (z80_usec / z80_samples) : 0UL;
  vdp_mean = (vdp_samples != 0UL) ? (vdp_usec / vdp_samples) : 0UL;
  share = z80_mean + vdp_mean;

  if((z80_samples != 0UL) && (vdp_samples != 0UL) &&
     (z80_mean != 0UL) && (vdp_mean != 0UL))
    {
      z8010 = (emul10 * z80_mean) / share;
      vdp10 = emul10 - z8010;
    }
  else
    {
      z8010 = 0UL;
      vdp10 = 0UL;
    }

  /* Tenths of a millisecond per frame, averaged over the window. */
  frame10 = (usec / 100UL) / frames;
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

#if MAIN_PROFILE
/*
 * ---------------------------------------------------------------------------
 * The render broken down into posts (common.h, SMS_VDP_PROFILE).
 *
 * The whole method in one sentence: the video part runs one named post a
 * second time per line, and what that costs is read off the vdp= the
 * periodic line already publishes. Five variants -- the control, one per
 * repeatable post, and the three together -- take one measurement window
 * each in turn, so a single run answers five questions. No clock reading
 * is added anywhere; the instrument is the figure that was already there.
 *
 * Repetition rather than removal, because the load is a game that runs:
 * two of the three posts raise bits the emulated program reads, so
 * removing one would make it diverge and the measurement would no longer
 * be of anything. Repeating writes the same bytes twice.
 *
 * What repetition costs in accuracy is one bias, and it has one sign: the
 * second pass finds its data warmer than the first, so every displacement
 * reads LOW. The variant that repeats the three together bounds it -- the
 * sum of the three separate displacements against that one -- and the
 * publication of the table is conditional on that check closing.
 * ---------------------------------------------------------------------------
 */

/*
 * How many healthy windows each variant gathers before a round is
 * published. Five variants at four windows each, one window a second,
 * is a round every TWENTY seconds at best -- longer by one second for
 * every window thrown out, and by one more for the window discarded
 * after each round. A two-minute run therefore yields five or six
 * rounds: enough that the ones taken before the regime settled can be
 * ignored by reading the last, and short enough that several land
 * inside it.
 *
 * The figure is the constant, not the prose: change it here and the
 * cadence follows, which is why the sentence above counts rather than
 * quotes.
 */
#define MAIN_PROFILE_WINDOWS 4UL

/*
 * How many windows may pass with no round published before the loop says
 * where each variant stands. Twice the nominal round, so a run that is
 * merely slow says nothing and a run whose instrument is stuck says so
 * on its own -- a variant that never gathers a healthy window would
 * otherwise leave the output silent for ever, indistinguishable from a
 * round still in progress.
 */
#define MAIN_PROFILE_STALL 40UL

/*
 * A window counts only if its parts still add up to its whole: the
 * emulated stretch plus the draw call, against the wall time of a frame.
 * The reference regime reads 0.988 of the frame (the complement is the
 * presentation and the pacing), and the band is set around that rather
 * than around 1. What it throws out is the window a host clock jump
 * poisoned -- those read a frame of seconds against a stretch of
 * milliseconds and land nowhere near.
 */
#define MAIN_PROFILE_SUM_MIN 90UL
#define MAIN_PROFILE_SUM_MAX 105UL

/*
 * The additivity gate, in percent: the three separate displacements
 * against the one the grouped variant produces. Outside this band the
 * three do not describe the same thing as the group, and no table is
 * published -- a wrong figure is worth less than no figure.
 *
 * The band is SYMMETRIC, and that is a decision rather than an
 * oversight. The bias named above has a sign and would argue for a wider
 * low side; the answer is that a bias big enough to move the sum by more
 * than a tenth is not a bias to allow for, it is a model that does not
 * hold -- the three posts would not be describing the same work as the
 * group. Widening the low side to fit such a round would publish exactly
 * the figure this gate exists to withhold. Ten percent either way, and a
 * round outside it is reported as it stands, with no table.
 */
#define MAIN_PROFILE_ADD_MIN 90UL
#define MAIN_PROFILE_ADD_MAX 110UL

/*
 * The processor's clock, in kilohertz, and the pixels of one picture --
 * calculated from the video part's own constants, never restated as a
 * figure.
 */
#define MAIN_ARM_CLOCK_KHZ 12500UL
#define MAIN_FRAME_PIXELS  (VDP_ACTIVE_LINES * VDP_PIX_WIDTH)

/*
 * Weighs one closing window the way main_perf_emit does, and returns
 * whether it is fit to be counted, with the video figure of the window in
 * tenths of a millisecond per frame.
 *
 * The arithmetic is main_perf_emit's, deliberately repeated rather than
 * shared: that function publishes the line the whole project reads, and
 * this story does not touch it. The two must move together -- a change to
 * the share up there is a change to be made here -- and this comment is
 * the only thing that says so.
 *
 * Three ways a window is refused: too short or empty, which is that
 * function's own guard; a share that could not be weighed on both sides,
 * which that function publishes as two zeroes and which would be counted
 * here as a render that cost nothing; and parts that do not add up to the
 * whole.
 */
static int32
main_profile_window(uint32  usec,
                    uint32  frames,
                    uint32  emul_usec,
                    uint32  emul_frames,
                    uint32  z80_usec,
                    uint32  z80_samples,
                    uint32  vdp_usec,
                    uint32  vdp_samples,
                    uint32  draw_usec,
                    uint32 *vdp10)
{
  uint32 emul10;
  uint32 z80_mean;
  uint32 vdp_mean;
  uint32 share;
  uint32 z8010;
  uint32 frame10;
  uint32 draw10;
  uint32 sum10;
  uint32 closes;

  *vdp10 = 0;

  if((frames == 0UL) || (usec < MAIN_PERF_PERIOD_USEC))
    return 0;

  if((z80_samples == 0UL) || (vdp_samples == 0UL))
    return 0;

  emul10 = ((emul_frames != 0UL) ? (emul_usec / emul_frames) : 0UL) / 100UL;

  z80_mean = z80_usec / z80_samples;
  vdp_mean = vdp_usec / vdp_samples;
  share = z80_mean + vdp_mean;

  if((z80_mean == 0UL) || (vdp_mean == 0UL))
    return 0;

  z8010 = (emul10 * z80_mean) / share;

  frame10 = (usec / 100UL) / frames;
  draw10 = (draw_usec / 100UL) / frames;
  sum10 = emul10 + draw10;

  if(frame10 == 0UL)
    return 0;

  closes = (sum10 * 100UL) / frame10;
  if((closes < MAIN_PROFILE_SUM_MIN) || (closes > MAIN_PROFILE_SUM_MAX))
    return 0;

  *vdp10 = emul10 - z8010;
  return 1;
}

/*
 * Tenths of a millisecond per frame into tenths of an ARM cycle per
 * pixel, in integers throughout: the build passes -fpu none, so a real
 * number here would pull software floating point into the diagnostic
 * layer.
 *
 * The chain is cost10 tenths of a millisecond -> times 100, microseconds
 * -> times the clock in kilohertz, divided by a thousand, cycles -> times
 * ten, tenths of a cycle -> divided by the pixels. The three factors 100,
 * 1/1000 and 10 cancel exactly, which leaves the clock in kilohertz as
 * the one multiplier; the division by the pixels comes last, where it
 * costs the least precision. The product holds in a word with room to
 * spare: a post of a whole second would reach 125 million.
 */
static uint32
main_profile_cpp10(uint32 cost10)
{
  return (cost10 * MAIN_ARM_CLOCK_KHZ) / MAIN_FRAME_PIXELS;
}

/*
 * The armed variant, by name, for every line that has to say which of the
 * five produced it.
 */
static const char *
main_profile_name(uint32 variant)
{
  if(variant == VDP_PROFILE_BG)      return "bg";
  if(variant == VDP_PROFILE_SPRITES) return "sprites";
  if(variant == VDP_PROFILE_PACK)    return "pack";
  if(variant == VDP_PROFILE_ALL)     return "all";
  return "control";
}

/*
 * Where each variant stands: healthy windows kept, and windows thrown
 * out, one figure per variant. Said at the head of every round, and on
 * its own when too many windows have gone by with no round -- which is
 * what makes a stuck instrument diagnose itself instead of falling
 * silent. The tag names the occasion so the two cannot be confused.
 */
static void
main_profile_state(const uint32 *win,
                   const uint32 *dropped,
                   uint32        frames,
                   const char   *tag)
{
  LOG_HOT(LOG_CAT_PERF,LOG_LVL_INFO,
          ("profile %s win ctrl=%lu bg=%lu spr=%lu pack=%lu all=%lu frames=%lu",
           tag,(unsigned long)win[VDP_PROFILE_CONTROL],
           (unsigned long)win[VDP_PROFILE_BG],(unsigned long)win[VDP_PROFILE_SPRITES],
           (unsigned long)win[VDP_PROFILE_PACK],(unsigned long)win[VDP_PROFILE_ALL],
           (unsigned long)frames));

  LOG_HOT(LOG_CAT_PERF,LOG_LVL_INFO,
          ("profile %s dropped ctrl=%lu bg=%lu spr=%lu pack=%lu all=%lu",
           tag,(unsigned long)dropped[VDP_PROFILE_CONTROL],
           (unsigned long)dropped[VDP_PROFILE_BG],(unsigned long)dropped[VDP_PROFILE_SPRITES],
           (unsigned long)dropped[VDP_PROFILE_PACK],(unsigned long)dropped[VDP_PROFILE_ALL]));
}

/*
 * One line of the table.
 *
 * A post whose repeated pass read FASTER than the control has not cost a
 * negative amount of time: it has not been measured. It publishes no
 * cost, and says which it is -- the alternative, a floor at zero, would
 * put a fabricated figure in the one table the stories after this one
 * are meant to quote.
 */
static void
main_profile_post(const char *name,
                  uint32      cost10,
                  uint32      below,
                  uint32      ctrl)
{
  uint32 cpp10;

  if(below != 0UL)
    {
      LOG_HOT(LOG_CAT_PERF,LOG_LVL_INFO,
              ("post %s not measurable this round: the repeated pass read %lu.%lums faster than the control",
               name,(unsigned long)(cost10 / 10UL),(unsigned long)(cost10 % 10UL)));
      return;
    }

  cpp10 = main_profile_cpp10(cost10);
  LOG_HOT(LOG_CAT_PERF,LOG_LVL_INFO,
          ("post %s cost=%lu.%lums/frame %lu.%lu cycles/pixel share=%lupct of vdp",
           name,(unsigned long)(cost10 / 10UL),(unsigned long)(cost10 % 10UL),
           (unsigned long)(cpp10 / 10UL),(unsigned long)(cpp10 % 10UL),
           (unsigned long)((ctrl != 0UL) ? ((cost10 * 100UL) / ctrl) : 0UL)));
}

/*
 * Publishes one round.
 *
 * Order of the lines, and it is the order of the reasoning: where the
 * round stands, what the five variants read, what the repetition
 * displaced, what the instrumentation already there costs, whether the
 * displacements add up -- and only then, if they do, the cost of each
 * post. A round that cannot be weighed prints its figures and stops
 * there, saying why.
 *
 * Three ways it stops. A post that read below the control has not been
 * measured, so the sum of the three is missing a term and no percentage
 * of it would mean anything. The sum against the grouped variant may
 * fall outside the gate. And the three may total PAST the published
 * figure, which leaves no residual to speak of and shares that add to
 * more than the whole; the round says so rather than printing a residual
 * of zero and letting the shares pass.
 *
 * The post named "rest" is not measured and is printed as what it is:
 * the published figure less the three that were. Whatever the render
 * does that none of the three posts covers is inside it, the repetition
 * bias included.
 */
static void
main_profile_emit(const uint32 *sum10,
                  const uint32 *win,
                  const uint32 *dropped,
                  uint32        frames,
                  uint32        samples,
                  uint32        clock_cost)
{
  uint32 mean[VDP_PROFILE_VARIANTS];
  uint32 gap[VDP_PROFILE_VARIANTS];
  uint32 low[VDP_PROFILE_VARIANTS];
  /*
   * The four variants held one per name for the lines below: the compiler
   * counts the SOURCE lines a macro's argument list spans and warns past
   * ten, and an indexed argument spelled out is three times as wide as a
   * name. The aliases are what keep those lists short enough to be built
   * without a diagnostic.
   */
  uint32 m_bg;
  uint32 m_spr;
  uint32 m_pack;
  uint32 m_all;
  uint32 g_bg;
  uint32 g_spr;
  uint32 g_pack;
  uint32 g_all;
  uint32 v;
  uint32 ctrl;
  uint32 sum3;
  uint32 closes;
  uint32 rest;
  uint32 reads10;
  uint32 cost10;
  uint32 unweighed;

  for(v = 0; v < VDP_PROFILE_VARIANTS; v++)
    {
      mean[v] = (win[v] != 0UL) ? (sum10[v] / win[v]) : 0UL;
      gap[v] = 0;
      low[v] = 0;
    }

  ctrl = mean[VDP_PROFILE_CONTROL];

  /*
   * A displacement below zero is noise, not a negative cost: it is kept
   * as a magnitude with a flag so every line that touches it can show it
   * rather than a floor of zero hiding it.
   */
  for(v = 0; v < VDP_PROFILE_VARIANTS; v++)
    {
      if(mean[v] >= ctrl)
        gap[v] = mean[v] - ctrl;
      else
        {
          gap[v] = ctrl - mean[v];
          low[v] = 1;
        }
    }

  sum3 = gap[VDP_PROFILE_BG] + gap[VDP_PROFILE_SPRITES] + gap[VDP_PROFILE_PACK];
  unweighed = low[VDP_PROFILE_BG] + low[VDP_PROFILE_SPRITES]
            + low[VDP_PROFILE_PACK] + low[VDP_PROFILE_ALL];

  main_profile_state(win,dropped,frames,"round");

  m_bg = mean[VDP_PROFILE_BG];
  m_spr = mean[VDP_PROFILE_SPRITES];
  m_pack = mean[VDP_PROFILE_PACK];
  m_all = mean[VDP_PROFILE_ALL];
  g_bg = gap[VDP_PROFILE_BG];
  g_spr = gap[VDP_PROFILE_SPRITES];
  g_pack = gap[VDP_PROFILE_PACK];
  g_all = gap[VDP_PROFILE_ALL];

  LOG_HOT(LOG_CAT_PERF,LOG_LVL_INFO,
          ("profile vdp ctrl=%lu.%lums bg=%lu.%lums spr=%lu.%lums pack=%lu.%lums all=%lu.%lums",
           (unsigned long)(ctrl / 10UL),(unsigned long)(ctrl % 10UL),
           (unsigned long)(m_bg / 10UL),(unsigned long)(m_bg % 10UL),
           (unsigned long)(m_spr / 10UL),(unsigned long)(m_spr % 10UL),
           (unsigned long)(m_pack / 10UL),(unsigned long)(m_pack % 10UL),
           (unsigned long)(m_all / 10UL),(unsigned long)(m_all % 10UL)));

  /*
   * Magnitudes only, eight arguments, no string among them.
   *
   * What the console showed: this line once carried a sign marker before
   * each of its four figures -- twelve arguments, four of them strings --
   * and the fourth figure came out as the third one's whole part followed
   * by six junk digits, the same six on every round. What still printed
   * whole in the same run: the periodic line, twelve arguments and every
   * one of them a number, and the per-post lines, six arguments with a
   * single leading string. So the fault sits somewhere in the count and
   * the interleaving together; which of the two carries it was not
   * established, and this comment does not claim it was. The shape kept
   * here is one both surviving lines share, and it is kept for that reason
   * rather than for a diagnosis.
   *
   * Nothing is lost with the markers: a variant reading below the control
   * ends the round above, named, before this line is reached.
   */
  LOG_HOT(LOG_CAT_PERF,LOG_LVL_INFO,
          ("profile gap bg=%lu.%lums spr=%lu.%lums pack=%lu.%lums grouped=%lu.%lums",
           (unsigned long)(g_bg / 10UL),(unsigned long)(g_bg % 10UL),
           (unsigned long)(g_spr / 10UL),(unsigned long)(g_spr % 10UL),
           (unsigned long)(g_pack / 10UL),(unsigned long)(g_pack % 10UL),
           (unsigned long)(g_all / 10UL),(unsigned long)(g_all % 10UL)));

  /*
   * The price of the instrumentation that was already there, said on
   * every round so the figures above can be read with it: three readings
   * per sampled line, taken inside the measured stretch, at the price
   * this run measured for one reading at boot. This is what closes the
   * standing question of what the clock readings of a frame cost -- as a
   * figure, on the run itself, rather than as a supposition.
   */
  if(frames != 0UL)
    {
      reads10 = (samples * 30UL) / frames;
      cost10 = ((clock_cost * 3UL * samples) / frames) / 100UL;
      LOG_HOT(LOG_CAT_PERF,LOG_LVL_INFO,
              ("profile clock reads=%lu.%lu/frame at %luus each -> %lu.%lums/frame inside the stretch",
               (unsigned long)(reads10 / 10UL),(unsigned long)(reads10 % 10UL),
               (unsigned long)clock_cost,
               (unsigned long)(cost10 / 10UL),(unsigned long)(cost10 % 10UL)));
    }

  /*
   * A post that read below the control is named, and it ends the round:
   * the sum of the three is short of a term, so the gate below would be
   * a percentage of an incomplete sum.
   */
  if(unweighed != 0UL)
    {
      main_profile_post("bg",g_bg,low[VDP_PROFILE_BG],ctrl);
      main_profile_post("sprites",g_spr,low[VDP_PROFILE_SPRITES],ctrl);
      main_profile_post("pack",g_pack,low[VDP_PROFILE_PACK],ctrl);
      main_profile_post("all",g_all,low[VDP_PROFILE_ALL],ctrl);
      LOG_HOT(LOG_CAT_PERF,LOG_LVL_INFO,
              ("profile add cannot be weighed: %lu variant(s) read below the control, no table published",
               (unsigned long)unweighed));
      return;
    }

  /*
   * The additivity control, and the gate on everything below it.
   */
  closes = (g_all != 0UL) ? ((sum3 * 100UL) / g_all) : 0UL;

  LOG_HOT(LOG_CAT_PERF,LOG_LVL_INFO,
          ("profile add sum3=%lu.%lums grouped=%lu.%lums close=%lupct",
           (unsigned long)(sum3 / 10UL),(unsigned long)(sum3 % 10UL),
           (unsigned long)(g_all / 10UL),(unsigned long)(g_all % 10UL),
           (unsigned long)closes));

  if((closes < MAIN_PROFILE_ADD_MIN) || (closes > MAIN_PROFILE_ADD_MAX))
    {
      LOG_HOT(LOG_CAT_PERF,LOG_LVL_INFO,
              ("profile add does not close: no table published, the figures above stand as read"));
      return;
    }

  /*
   * The share the published figure claims, put against a direct
   * measurement for the first time: the grouped displacement is what the
   * three posts cost, and it is compared to the whole of vdp=. What is
   * left is the rest, and the rest is a residual.
   */
  main_profile_post("bg",g_bg,0UL,ctrl);
  main_profile_post("sprites",g_spr,0UL,ctrl);
  main_profile_post("pack",g_pack,0UL,ctrl);

  if(sum3 > ctrl)
    {
      /*
       * The three measured posts total more than the figure they were
       * measured against. There is no residual to report -- and the
       * three shares above already add to more than the whole, which is
       * the reader's warning that the round describes nothing.
       */
      LOG_HOT(LOG_CAT_PERF,LOG_LVL_INFO,
              ("profile posts overrun the published figure by %lu.%lums: the shares total past 100pct",
               (unsigned long)((sum3 - ctrl) / 10UL),
               (unsigned long)((sum3 - ctrl) % 10UL)));
      LOG_HOT(LOG_CAT_PERF,LOG_LVL_INFO,
              ("post rest not interpretable this round: the three posts already overrun vdp"));
    }
  else
    {
      rest = ctrl - sum3;
      LOG_HOT(LOG_CAT_PERF,LOG_LVL_INFO,
              ("post rest cost=%lu.%lums/frame %lu.%lu cycles/pixel share=%lupct of vdp (residual, not measured)",
               (unsigned long)(rest / 10UL),(unsigned long)(rest % 10UL),
               (unsigned long)(main_profile_cpp10(rest) / 10UL),
               (unsigned long)(main_profile_cpp10(rest) % 10UL),
               (unsigned long)((ctrl != 0UL) ? ((rest * 100UL) / ctrl) : 0UL)));
    }

  LOG_HOT(LOG_CAT_PERF,LOG_LVL_INFO,
          ("post total vdp=%lu.%lums/frame %lu.%lu cycles/pixel over %lu pixels",
           (unsigned long)(ctrl / 10UL),(unsigned long)(ctrl % 10UL),
           (unsigned long)(main_profile_cpp10(ctrl) / 10UL),
           (unsigned long)(main_profile_cpp10(ctrl) % 10UL),
           (unsigned long)MAIN_FRAME_PIXELS));
}

#endif /* MAIN_PROFILE */

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
  /*
   * The ground painted around the picture, and how many screens still owe
   * that colour. The colour is the emulated machine's own background, read
   * once per frame and compared with what was last painted: sixteen bits
   * and one compare per frame in the regime where nothing changes, which
   * is the regime of every game that sets its background colour once.
   *
   * The colour and not the register, because the entry the register names
   * can be rewritten without the register moving; comparing the resolved
   * colour catches that too, for the same one load.
   *
   * The countdown is armed with the number of screens on every change and
   * spent one screen a frame, each just before that screen is drawn into:
   * the change reaches every screen of the rotation, and none of them is
   * ever repainted while the scan is reading it.
   */
  uint16 border_color;
  int32 border_repaint;
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
   * the three around the one sampled line -- its near edge, the boundary
   * between the quota and the video call, and its far edge -- and the
   * reading being weighed.
   */
  uint32 emul_start;
  uint32 line_start;
  uint32 line_mid;
  uint32 line_end;
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
   * The counters the periodic line reports -- the emulated stretch of a
   * whole frame, the two sampled sides of a line inside it, and the draw
   * call. They are written by this function and by no other: a module
   * told to do its share of a turn does not time itself, because the
   * timing belongs where the turn is cut up and where the pace is held.
   */
  uint32 perf_emul = 0;
  uint32 perf_emul_frames = 0;
  uint32 perf_z80 = 0;
  uint32 perf_z80_samples = 0;
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
#if MAIN_PROFILE
  /*
   * The round of the breakdown: which variant the window now closing ran
   * under, what each variant's kept windows totalled in tenths of a
   * millisecond of render per frame, how many windows each of them kept,
   * how many the round threw out, and the frames and line samples those
   * kept windows covered. The last two price the clock readings and are
   * read off counters that already existed -- nothing is counted per line
   * or per frame for the breakdown's sake.
   */
  uint32 prof_variant;
  uint32 prof_sum10[VDP_PROFILE_VARIANTS];
  uint32 prof_win[VDP_PROFILE_VARIANTS];
  uint32 prof_drop[VDP_PROFILE_VARIANTS];
  uint32 prof_frames;
  uint32 prof_samples;
  uint32 prof_since;
  uint32 prof_skip;
  uint32 prof_i;
  uint32 prof_vdp10;
  uint32 prof_lines;
  uint32 prof_full;
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
   * write on. This binding covers the whole of the boot sequence, which
   * draws and presents one screen and no other; the frame loop renews it
   * on every turn, because from its first presentation on the screens
   * rotate and a binding taken once would name the wrong one half the
   * time.
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
   * The ground the picture sits in: the emulated machine's own background
   * colour, the one it shows outside its picture, read from the video
   * part and painted on every screen of the rotation.
   *
   * It is armed here rather than painted here. The countdown is set to
   * the number of screens and one screen is painted per frame, each just
   * before the frame's drawing lands on it, so the very first frame draws
   * on ground of the right colour and the second one does the same on the
   * other screen. Painting both at the instant of a change would wipe the
   * screen the scan is reading -- a flash of one frame -- where the
   * countdown lets the change take two frames to travel, thirty-odd
   * milliseconds, which nothing can see.
   *
   * This replaces a fill in a flat colour the palette could not contain,
   * which proved two things while the picture was being brought up -- the
   * framing offsets, read off the width of the bands, and that the black
   * entry was drawn opaque rather than transparent -- and which said
   * nothing about the machine being emulated. Those two readings are
   * settled; a game asked for a background colour and now gets it.
   */
  border_color = vdp_backdrop();
  border_repaint = sys_screen_count();

  vbl_target = sys_vbl_count() + MAIN_VBL_STEP;

  /*
   * Carried across frames as well as across lines. A frame boundary is a
   * scanline boundary like any other, and it settles no debt: whatever the
   * last instruction of a frame spent past the end of its quota is owed by the
   * first line of the next one.
   */
  residue = 0;

#if MAIN_PROFILE
  /*
   * Armed before the first measurement window opens, and said before it
   * too: the line below blocks on the serial port, and a window that
   * carried it would read the tracing rather than the frame. The first
   * window runs the control, which is also what the video part renders
   * when nothing arms it -- the arming here only ever confirms it.
   */
  for(prof_i = 0; prof_i < VDP_PROFILE_VARIANTS; prof_i++)
    {
      prof_sum10[prof_i] = 0;
      prof_win[prof_i] = 0;
      prof_drop[prof_i] = 0;
    }
  prof_frames = 0;
  prof_samples = 0;
  prof_since = 0;
  prof_skip = 0;
  prof_variant = VDP_PROFILE_CONTROL;
  vdp_profile_select(prof_variant);
  LOG_INFO(LOG_CAT_PERF,("profile on: %lu variants, %lu healthy windows each, one window a variant",
                         (unsigned long)VDP_PROFILE_VARIANTS,
                         (unsigned long)MAIN_PROFILE_WINDOWS));
  LOG_INFO(LOG_CAT_PERF,("profile run: four windows in five are deliberately slowed, no [PERF] line of this run is a figure to quote"));
#endif

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
  LOG_INFO(LOG_CAT_PERF,("clock read cost=%luus (taken out of each line sample)",
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
       * emulated stretch, and three times on one line in
       * MAIN_PERF_SAMPLE_STRIDE -- around its quota and around its video
       * call, sharing the boundary reading -- the phase stepping once per
       * frame so that the sampled lines walk the whole frame. The two
       * sampled sides are not published as figures of their own: they are
       * the ratio the measured stretch is split by (main_perf_emit), so
       * the pair sums to the frame that was measured and neither can be
       * an estimate that outgrows it. This loop once read the clock
       * around every quota and every line -- 525 readings a frame, the
       * exact figure with no estimate in it -- and the console refused
       * it: the call is documented as very low overhead
       * (docs/3do/3do_portfolio_2.5.md:19573) and costs some forty
       * microseconds, twenty milliseconds a frame, a third of the frame
       * this loop is meant to hold. Two readings a frame, the arrangement
       * before that, could not tell the render from the quotas once the
       * line call rendered. The mask and compare that pick the sampled
       * lines are the one thing this arrangement adds inside the loop,
       * 262 times a frame, against the thousands of cycles of the quota
       * beside it.
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
#if MAIN_MEASURE
          if((line & (MAIN_PERF_SAMPLE_STRIDE - 1UL)) == perf_sample_phase)
            {
              /*
               * The sampled line, timed on both sides by three readings:
               * the middle one closes the quota and opens the video call,
               * so the two spans are adjacent and share their boundary --
               * nothing of the line falls between them and nothing is
               * counted twice. The weighing is done after the far edge is
               * read, outside both spans.
               *
               * Each span carries the cost of the reading that closes it,
               * which is why the price of one reading comes off each of
               * them: a call into the operating system is tens of
               * microseconds here, a few percent of a rendered line and
               * more than an empty one.
               */
              line_start = sys_usec();
              residue = z80_run((int32)MAIN_TSTATES_PER_LINE - residue);
              line_mid = sys_usec();
              vdp_line();
              line_end = sys_usec();

              perf_delta = line_mid - line_start;
              if(perf_delta < MAIN_CLK_MAX_LINE_USEC)
                {
                  perf_z80 += (perf_delta > perf_clock_cost)
                              ? (perf_delta - perf_clock_cost) : 0UL;
                  perf_z80_samples++;
                }
              else
                perf_clk++;

              perf_delta = line_end - line_mid;
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
            {
              residue = z80_run((int32)MAIN_TSTATES_PER_LINE - residue);
              vdp_line();
            }
#else
          residue = z80_run((int32)MAIN_TSTATES_PER_LINE - residue);
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
       * The ground, once per turn and on the cold side of the emulated
       * stretch: one load of sixteen bits and one compare while nothing
       * changes, and a fill only while the countdown says a screen still
       * owes the colour. A program that writes the register on every
       * frame arms the countdown again on every frame and so pays one
       * fill a frame -- there is no cheaper honest answer to a background
       * that really does change that often -- and it still pays only one
       * line of trace per report window, the count of the window going
       * with the other aggregates.
       *
       * Before the drawing and not after: the fill covers the whole
       * screen, so the picture has to land on top of it.
       */
      {
        uint16 backdrop_now = vdp_backdrop();

        if(backdrop_now != border_color)
          {
            border_color = backdrop_now;
            border_repaint = sys_screen_count();
          }

        if(border_repaint > 0)
          {
            /*
             * Spent on the paint, not on the attempt. A refused fill
             * leaves that screen showing the ground it had, and the
             * countdown has to come back to it on the next turn --
             * decrementing here regardless would leave one screen of the
             * rotation with the old colour for the rest of the run, half
             * the frames of a game showing the wrong ground and nothing
             * saying why. The count that goes with the aggregates is a
             * count of paints for the same reason: it must not report a
             * paint that did not happen.
             */
            if(sys_fill_screen(sys_screen_index(),(Color)border_color) >= 0)
              {
                border_repaint--;
                vdp_backdrop_repainted();
              }
          }
      }

      /*
       * The end of the frame: the one draw call of the turn, then the
       * presentation. The draw is what the third accumulator weighs --
       * the emulated stretch has already closed its own clock above, so
       * the two figures never overlap -- and the presentation stays
       * outside the measurement: it is one cold call per frame, and the
       * figure being read here is the cel engine's, not the display's.
       * The fill above is outside it too, and for the same reason: it is
       * the display's business, not the engine's, and it is paid on the
       * frames of a change only.
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

      /*
       * The screen the error path would paint on, renewed on every turn
       * so that it follows the rotation. It names the screen about to be
       * presented, which is the one a viewer is looking at for the whole
       * of the next frame: a stop anywhere in that frame writes its
       * message over the picture that is on the console, and presents it
       * again. Bound after the presentation instead, it would name the
       * screen the scan is not reading, and the message would appear over
       * a picture two frames old.
       *
       * Renewed and not bound once, because a binding taken at boot names
       * one screen for good: with the screens rotating, every other frame
       * would paint its message where nothing can see it.
       */
      log_bind_screen(sys_bitmap(),sys_screen());
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
               * That is the whole policy, and it is now a decision and no
               * longer an open question: a late frame slips, it is counted
               * in over=, and nothing else happens to it. No frame is ever
               * replayed, no emulated frame is ever skipped, and no frame
               * is ever drawn less than whole -- an emulator that dropped
               * frames of emulation would run the machine at a pace the
               * machine does not have, and one that dropped renders would
               * hide, behind a steadier picture, the very cost the
               * measurements exist to expose.
               *
               * The measurements it was waiting for have been taken, and
               * they say the render is what costs: the honest answer to a
               * frame that overruns is therefore to make the render
               * cheaper, not to skip it. Dropping the render of a late
               * frame stays available as a lever, to be pulled -- if ever
               * -- by the work that optimises the render, and only once
               * that work has a figure to show for itself.
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
                         perf_emul,perf_emul_frames,
                         perf_z80,perf_z80_samples,
                         perf_vdp,perf_vdp_samples,
                         perf_draw,perf_over,perf_clk);

#if MAIN_PROFILE
          /*
           * The window that has just closed ran under the variant armed
           * at the previous close, so it is whole: no window ever spans
           * two variants. It is kept only if its parts still add up to
           * its whole, and the next variant is armed after that, in this
           * one place -- the cutting up of a turn and its cadence belong
           * to this loop and to nothing else.
           *
           * The stamp first, and on every window. The [PERF] line above
           * it is indistinguishable from a development build's, and four
           * windows in five are deliberately slowed: an excerpt of this
           * log lifted without its stamp is quotable and wrong. One line
           * per window, the same for all five, so it weighs on every
           * variant alike.
           */
          LOG_HOT(LOG_CAT_PERF,LOG_LVL_INFO,
                  ("profile window variant=%s (profiling run: the line above is not a figure to quote)",
                   main_profile_name(prof_variant)));

          if(prof_skip != 0UL)
            {
              /*
               * The window right after a round, or after a sign of life.
               * Those are ten blocking serial lines and two, and they
               * land inside THIS window's wall time -- while the
               * emulated stretch and the draw call, which are measured
               * spans, know nothing of them. That lowers the ratio of
               * the parts to the whole, and it does it to the same
               * variant every time, the round being a whole number of
               * turns of the rotation. Whether it lowers it enough to
               * fail the band or merely enough to bias one variant, the
               * cure is the same and it is one window: this one is
               * thrown out, counted, and never weighed.
               */
              prof_skip = 0;
              prof_drop[prof_variant]++;
            }
          else if(main_profile_window(perf_now - perf_window,perf_frames,
                                      perf_emul,perf_emul_frames,
                                      perf_z80,perf_z80_samples,
                                      perf_vdp,perf_vdp_samples,
                                      perf_draw,&prof_vdp10) != 0)
            {
              prof_sum10[prof_variant] += prof_vdp10;
              prof_win[prof_variant]++;
              prof_frames += perf_frames;
              /*
               * The count of SAMPLED LINES of the window, and the
               * smaller of the two sides is the honest one: a line is
               * weighed as two spans and either may be refused on its
               * own, so the two counters can differ. The line that
               * prices the clock readings charges three readings per
               * sampled line; taking the larger count would charge
               * readings for lines only one half of which was kept.
               */
              prof_lines = (perf_z80_samples < perf_vdp_samples)
                           ? perf_z80_samples : perf_vdp_samples;
              prof_samples += prof_lines;
            }
          else
            prof_drop[prof_variant]++;

          prof_since++;

          prof_full = 1;
          for(prof_i = 0; prof_i < VDP_PROFILE_VARIANTS; prof_i++)
            {
              if(prof_win[prof_i] < MAIN_PROFILE_WINDOWS)
                prof_full = 0;
            }

          if(prof_full != 0UL)
            {
              main_profile_emit(prof_sum10,prof_win,prof_drop,
                                prof_frames,prof_samples,perf_clock_cost);
              for(prof_i = 0; prof_i < VDP_PROFILE_VARIANTS; prof_i++)
                {
                  prof_sum10[prof_i] = 0;
                  prof_win[prof_i] = 0;
                  prof_drop[prof_i] = 0;
                }
              prof_frames = 0;
              prof_samples = 0;
              prof_since = 0;
              prof_skip = 1;
            }
          else if(prof_since >= MAIN_PROFILE_STALL)
            {
              /*
               * Too many windows with no round. The totals are NOT
               * cleared -- the round is still gathering -- only the
               * count that triggers this, so the same line comes back
               * every stall's worth of windows and a variant stuck at
               * zero healthy windows names itself.
               */
              main_profile_state(prof_win,prof_drop,prof_frames,"waiting");
              prof_since = 0;
              prof_skip = 1;
            }

          prof_variant++;
          if(prof_variant >= VDP_PROFILE_VARIANTS)
            prof_variant = VDP_PROFILE_CONTROL;
          vdp_profile_select(prof_variant);
#endif

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
          perf_z80 = 0;
          perf_z80_samples = 0;
          perf_vdp = 0;
          perf_vdp_samples = 0;
          perf_draw = 0;
          perf_clk = 0;
        }
#endif
    }
}
