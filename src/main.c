#include "sys.h"
#include "log.h"
#include "z80.h"
#include "z80c.h"
#include "cart.h"
#include "vdp.h"
#include "sms.h"
#include "dynarec_j0.h"
#include "dynarec_j1.h"
#include "dynarec_j2.h"
#include "memprobe.h"
#include "celprobe.h"

/*
 * The segments of a picture's palette go one each into an entry of the
 * screen's display list: the video part's cap must fit the list's.
 */
#if VDP_PAL_SEGMENTS > SYS_VDL_ENTRIES
#error "more palette segments than the display list holds entries"
#endif

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
 * The measurement reads the video part's counters, so the two switches
 * have to be the one switch. They are written from the same condition in
 * two files, and outside both of them here: a check placed inside the
 * measurement would be compiled out by the very build it exists to
 * refuse.
 */
#if MAIN_MEASURE != VDP_COUNTERS
#error "the measurement and the counters it reads must be compiled together"
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
 * A window total said per frame to a tenth, as the two arguments a %lu.%lu
 * pair wants. Integers throughout, for the reason main_perf_emit gives:
 * this processor has no floating point unit and the build passes none in.
 *
 * Quotient and remainder, never the total scaled by ten first: a count of
 * line slots over a long window reaches hundreds of millions, and ten
 * times that is past what 32 bits hold -- the figure would not be coarse,
 * it would be wrong, and wrong in silence. The remainder is bounded by
 * the divisor, so its scaling cannot overflow at any window this loop can
 * hold.
 *
 * Both arguments are read more than once, so neither may carry a side
 * effect -- the uses below pass a field and a count.
 */
#define MAIN_PERF_TENTHS(n,f)                            \
  (unsigned long)((n) / (f)),                            \
  (unsigned long)((((n) % (f)) * 10UL) / (f))

/*
 * A price and its age, as the two arguments a value/age pair wants.
 */
#define MAIN_PERF_PRICE(p,i)                             \
  (unsigned long)(p)[(i)].value,(unsigned long)(p)[(i)].age

/*
 * The four prices, and the order they are held in.
 */
#define MAIN_PRICE_MARK  0
#define MAIN_PRICE_SCAN  1
#define MAIN_PRICE_RECT  2
#define MAIN_PRICE_PIX   3
#define MAIN_PRICE_COUNT 4

/*
 * A mark priced above this is a bad reading, not a slow mark, and the
 * bound is derived and not guessed. The mark is one byte stored, two
 * bytes read and a compare; on this processor a register operation costs
 * 1.05 cycles, a read 5.25 and a write 6.30, and the part runs at
 * 12.5 MHz -- about eighteen cycles, some 1.4 microseconds. The lot is
 * VDP_MARK_PRICE_LOT of them, so the difference the probe reads is around
 * seven tenths of a millisecond: twenty-five times what one reading of
 * the clock costs, which is what makes the probe able to resolve anything
 * at all.
 *
 * Ten times the expected price is past any plausible slow path and well
 * short of a clock that jumped by seconds. It is also what keeps the
 * product of a frame's writes by this price inside 32 bits: twenty
 * thousand writes at this bound is four hundred million nanoseconds.
 */
#define MAIN_MARK_NSEC_MAX 20000UL

/*
 * The same idea for the two probes priced in microseconds: a rebuild of
 * the per-line sprite table walks sixty-four entries and fills up to as
 * many line slots each, and a collision replay walks at most a line of
 * pixels. Neither can honestly reach a tenth of a second; past this they
 * are a clock that jumped, and the bound keeps their products inside 32
 * bits at any count a window can hold.
 */
#define MAIN_PRICE_USEC_MAX 10000UL

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
 * as three spans, the quota, the sprites' share of the video call and the
 * end of the line, and each is compared to the bound on its own, so a
 * whole sampled line is refused only past three tenths of a second. That
 * is deliberate -- the bound exists to catch a clock that jumped by
 * seconds, not to cap a slow render -- and it is what makes the sides
 * refusable one without the other, which the per-side counting of clk=
 * then reports.
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
 * Two spans and not three, and that is a decision taken with a figure in
 * hand. A third reading, cutting the video call between the sprites and
 * the end of the line, was tried: the end of the line is a handful of
 * compares, some hundreds of nanoseconds, against a reading that costs
 * twenty-seven microseconds. The span it would have measured is a hundred
 * times under the resolution of the instrument, so it published nothing
 * and the reading it cost inflated the very stretch every share is taken
 * from. What the sprites cost is answered by counts times measured prices
 * instead (vdp.h, the three probes), which no clock reading can be too
 * coarse for. The readings that remain are themselves published as a
 * post: probe= on the decomposition line.
 *
 * A power of two, and it has to be: the line that carries the sample is
 * picked with a mask and the phase is wrapped with the same mask, both
 * cheaper than a remainder on a path that runs 262 times a frame. A value
 * that is not a power of two would not select one line in that many, it
 * would select the wrong lines silently.
 */
#define MAIN_PERF_SAMPLE_STRIDE 32UL

/*
 * What one reading of the clock costs, priced once at the head of the
 * loop as the mean of a few back-to-back pairs, and taken out of every
 * span: a span read between two calls into the operating system
 * otherwise carries one call's worth, tens of microseconds on this
 * console -- more than an empty line, a few percent of a rendered one.
 * Declared here, above its first reader: the periodic line takes it off
 * the two means it forms and off the stretch those means are shares of.
 */
static uint32 main_perf_clock_cost = 0;

/*
 * The refusals, running and never cleared, each said on the line that
 * owns it -- and apart from clk=, which counts readings of the clock that
 * were impossible and nothing else. A reader has to be able to tell which
 * of them moved.
 *
 * main_perf_clamp: a post that came out larger than the whole it is taken
 * out of, floored at zero. Every such floor is a fact -- a sum that fell
 * just short reads as a zero remainder, and without this count it would
 * be indistinguishable from a decomposition that closed exactly.
 *
 * main_perf_price_bad: a price the probe could not take -- a reading
 * refused as impossible, a lot that came out no dearer than the empty
 * lot, or a lot that moved a counter it had no business moving. The
 * window then republishes the price it already had, which is why the
 * published prices carry their age.
 */
static uint32 main_perf_clamp = 0;
static uint32 main_perf_price_bad = 0;

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
/*
 * What one price is worth and how old it is: the figure the probes below
 * measured, and how many windows have closed since. Age zero is a price
 * taken in the window being published; anything else is a price carried
 * forward because that window's probe was refused, and the reader has to
 * be able to see that rather than take a stale figure for a fresh one.
 */
typedef struct main_price_s
{
  uint32 value;
  uint32 age;
} main_price_t;

static void
main_perf_emit(uint32 usec,
               uint32 frames,
               uint32 emul_usec,
               uint32 emul_frames,
               uint32 z80_usec,
               uint32 z80_samples,
               uint32 vdp_usec,
               uint32 vdp_samples,
               uint32 line_samples,
               uint32 present_usec,
               uint32 draw_usec,
               uint32 clut_usec,
               uint32 tiles_usec,
               uint32 list_usec,
               uint32 over,
               uint32 clk,
               const main_price_t *price,
               const vdp_perf_t *cnt)
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
  uint32 clut_per_frame;
  uint32 tiles_per_frame;
  uint32 list_per_frame;
  uint32 probe_pf;
  uint32 emul_pf;
  uint32 vdp_pf;
  uint32 scan_pf;
  uint32 col_pf;
  uint32 tail_pf;
  uint32 mark_pf;
  uint32 pres_pf;
  uint32 pres_rest_pf;
  uint32 spans_pf;
  uint32 frame_pf;
  uint32 sum_pf;
  uint32 other_pf;

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
   * The cost of the instrument inside the stretch it measures, taken out
   * of that stretch and published as a post of its own. Three readings of
   * the clock on each sampled line and one closing the frame, at the
   * price one reading was measured to cost at boot: some two hundred
   * microseconds a frame, which is a fifth of what this story went
   * looking for and would otherwise have been shared out silently between
   * the processor and the video call. Two of the three readings of a
   * sampled line are already taken off the two means below, but those
   * corrections touch the RATIO, not the stretch: the stretch carries all
   * of them and is corrected here, once.
   */
  probe_pf = (((line_samples * 3UL) + frames) * main_perf_clock_cost) / frames;

  emul_pf = (emul_frames != 0UL) ? (emul_usec / emul_frames) : 0UL;
  if(probe_pf > emul_pf)
    {
      probe_pf = emul_pf;
      main_perf_clamp++;
    }
  emul_pf -= probe_pf;

  /*
   * The emulated stretch of one frame, in tenths of a millisecond: the
   * measured quantity, read around the whole line loop with the lines
   * included, less the instrument above. Frames whose stretch was a bad
   * reading are not in emul_frames; a window where every reading was bad
   * reports zero.
   */
  emul10 = emul_pf / 100UL;

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
   * The price of the reading that closes each span is taken off ONCE, at
   * the mean, and not off each sample: a span shorter than one reading --
   * which the end of a blank line is -- would be floored to zero sample
   * after sample, and a post that is really small would be published as a
   * post that is really zero. Subtracted from the total, the same span
   * lands where it belongs: small, and measured. A total that does not
   * even cover the readings it contains is not a small post, it is a
   * reading that cannot be trusted, and that side is then refused
   * outright.
   *
   * The means and not the raw totals, and that is the overflow guard the
   * scaling above uses in its own way: a total grows with the window and
   * would carry the product past 32 bits, where a mean is bounded by
   * MAIN_CLK_MAX_LINE_USEC. Tenths of a millisecond per frame times a
   * bounded mean holds with room to spare.
   *
   * A share needs both of its sides, and a side is had only when it was
   * sampled at all, when its total stands clear of the readings inside
   * it, and when what is left is not zero. One side alone would divide
   * the whole stretch by itself and hand it all to that side, which is
   * exactly the shape this arrangement exists to make impossible -- a
   * processor figure of zero beside a video figure carrying the frame.
   * Both sides are therefore refused together, and the two figures
   * published as zero together. A window that could not weigh both sides
   * says so by weighing neither; it must never read as a window that
   * measured everything on one side.
   */
  z80_mean = 0;
  if((z80_samples != 0UL)
     && (z80_usec > (z80_samples * main_perf_clock_cost)))
    z80_mean = (z80_usec - (z80_samples * main_perf_clock_cost)) / z80_samples;

  vdp_mean = 0;
  if((vdp_samples != 0UL)
     && (vdp_usec > (vdp_samples * main_perf_clock_cost)))
    vdp_mean = (vdp_usec - (vdp_samples * main_perf_clock_cost)) / vdp_samples;

  share = z80_mean + vdp_mean;

  if((z80_mean != 0UL) && (vdp_mean != 0UL))
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

  /*
   * The colour table in whole microseconds per frame, over all the frames
   * of the window and not only those that set one: the figure is what a
   * frame pays on average, which is zero for a still palette and the
   * price of one set for a palette rewritten every frame. Microseconds
   * because the set is a fraction of a millisecond and tenths would
   * publish 0.2 for every value it can take.
   */
  clut_per_frame = clut_usec / frames;

  /*
   * The background picture's two posts on the same terms, microseconds
   * per frame over every frame of the window: the tiles converted at the
   * head of the presentation, and the windows built band by band. Zero
   * on a frame where nothing was written and nothing moved; the draw of
   * the windows is in draw= with the sprite cel's.
   */
  tiles_per_frame = tiles_usec / frames;
  list_per_frame = list_usec / frames;

  /*
   * ---------------------------------------------------------------------
   * The decomposition, in microseconds per frame throughout: the posts of
   * this line are fractions of a millisecond and tenths of a millisecond
   * would publish 0.0 for most of what they can take.
   * ---------------------------------------------------------------------
   *
   * The video call broken up by counts times prices. The rebuilds of the
   * per-line sprite table and the collision replays are the two things it
   * does; what is left over is the end of the line and whatever the two
   * prices do not describe, and it is published rather than assumed away.
   * The window's own counts and the prices measured on this console in
   * this scene: no constant, no estimate from elsewhere.
   *
   * The multiplication is done over the window and divided by the frames
   * afterwards, never the other way round: a count of one rebuild per
   * frame and a half would otherwise round to one before it was priced.
   */
  scan_pf = (cnt->scans * price[MAIN_PRICE_SCAN].value) / frames;
  col_pf = (((cnt->col_lines - cnt->col_pix) * price[MAIN_PRICE_RECT].value)
            + (cnt->col_pix * price[MAIN_PRICE_PIX].value)) / frames;

  vdp_pf = vdp10 * 100UL;
  if((scan_pf + col_pf) > vdp_pf)
    {
      /*
       * The parts outgrew the whole: a price taken in a scene the window
       * did not spend its frames in, or a count over a window the price
       * was not taken in. Floored and counted, never published as a
       * negative remainder dressed up as zero work.
       */
      tail_pf = 0UL;
      main_perf_clamp++;
    }
  else
    tail_pf = vdp_pf - (scan_pf + col_pf);

  /*
   * What the picture costs inside the processor's quota, and so inside
   * z80=: every write to the video memory pays the mark that lets the
   * picture be brought up to date later, and the mark was priced on this
   * window's own cold side. A count times a measured unit price, which is
   * the only way to weigh work spread over thousands of writes a frame.
   *
   * A FLOOR and named as one: the price is that of the cheap path, the
   * mark of a write the watch refuses. The writes the watch hands on pay
   * that and then a journal entry and a pattern mark on top, work that is
   * counted (note=, hotw=) and not priced -- pricing it would mean
   * journaling writes that never happened, which would change the
   * picture. markmin= is therefore a lower bound on the picture's share
   * of z80=, and the true figure is above it.
   *
   * The writes of one frame first, so that the product cannot outgrow
   * what a 32 bit multiply holds: a window's writes times a price in
   * nanoseconds would, a frame's writes times the same price cannot.
   */
  mark_pf = ((cnt->vram_w / frames) * price[MAIN_PRICE_MARK].value) / 1000UL;

  /*
   * A part that outgrew its whole is not a measurement, it is a fault:
   * the mark is paid inside the processor's quota, so a figure above z80=
   * can only mean a price taken while something else was running. Floored
   * and counted.
   *
   * Only when there IS a whole to hold it against. z8010 is zero whenever
   * the share was refused, which is a window that weighed neither side --
   * and a count times a price is a measurement of its own, made in no
   * part by the share. It is then published unheld, and the reader sees
   * z80=0.0 on the line above saying exactly that.
   */
  if((z8010 != 0UL) && (mark_pf > (z8010 * 100UL)))
    {
      mark_pf = 0UL;
      main_perf_clamp++;
    }

  /*
   * The presentation, whole, and the part of it no span describes. The
   * four spans -- the table set, the tiles converted, the windows built,
   * the cels drawn -- are closed one before the next opens, but the
   * presentation also paints the ground, ends the list and binds the
   * screen, and that work fell into no post until now: it was landing in
   * the residual, where it could be mistaken for something unexplained.
   */
  pres_pf = (emul_frames != 0UL) ? (present_usec / emul_frames) : 0UL;
  spans_pf = (draw_usec + clut_usec + tiles_usec + list_usec) / frames;
  if(spans_pf > pres_pf)
    {
      pres_rest_pf = 0UL;
      main_perf_clamp++;
    }
  else
    pres_rest_pf = pres_pf - spans_pf;

  /*
   * The whole every post is held against, and the part of it that is in
   * no post at all. frame= is wall time per frame over the window and
   * takes everything in: the emulated stretch, the presentation, the
   * pacing wait. The posts are the emulated stretch (z80= and vdp=, which
   * sum to it), the instrument inside it, and the presentation (its four
   * spans and pres=). What is left is named rather than left to be
   * guessed at -- an undecomposed remainder is what this line exists to
   * abolish.
   *
   * markmin=, scan=, col= and tail= are NOT added into the sum: the first
   * is a part of z80= and the other three are the parts of vdp=. Adding
   * any of them would count the same microseconds twice.
   *
   * Floored at zero and counted when it floors: a zero here without the
   * count beside it moving would read as a decomposition that closed
   * exactly, which is the one thing it must never be mistaken for.
   */
  frame_pf = usec / frames;
  sum_pf = emul_pf + probe_pf + pres_pf;
  if(sum_pf > frame_pf)
    {
      other_pf = 0UL;
      main_perf_clamp++;
    }
  else
    other_pf = frame_pf - sum_pf;

  /*
   * The arguments are packed on few lines on purpose: the compiler warns
   * on a macro call whose arguments run ten lines or more.
   */
  LOG_HOT(LOG_CAT_PERF,LOG_LVL_INFO,
          ("fps=%lu.%lu frame=%lu.%lums z80=%lu.%lums vdp=%lu.%lums draw=%lu.%lums clut=%luus tiles=%luus list=%luus over=%lu clk=%lu",
           (unsigned long)(fps10 / 10UL),(unsigned long)(fps10 % 10UL),
           (unsigned long)(frame10 / 10UL),(unsigned long)(frame10 % 10UL),
           (unsigned long)(z8010 / 10UL),(unsigned long)(z8010 % 10UL),
           (unsigned long)(vdp10 / 10UL),(unsigned long)(vdp10 % 10UL),
           (unsigned long)(draw10 / 10UL),(unsigned long)(draw10 % 10UL),
           (unsigned long)clut_per_frame,(unsigned long)tiles_per_frame,
           (unsigned long)list_per_frame,(unsigned long)over,(unsigned long)clk));

  /*
   * The decomposition, beside the line above and never inside it: the
   * figures of that line are what earlier measurements were written
   * against, and a field moved or renamed there would silently rewrite
   * them. The three parts of vdp=, the floor of the picture's work inside
   * z80=, the instrument, the rest of the presentation, the sum of every
   * post and what frame= has over it.
   */
  LOG_HOT(LOG_CAT_PERF,LOG_LVL_INFO,
          ("split scan=%luus col=%luus tail=%luus markmin=%luus probe=%luus pres=%luus sum=%lu.%lums other=%lu.%lums clamp=%lu",
           (unsigned long)scan_pf,(unsigned long)col_pf,(unsigned long)tail_pf,
           (unsigned long)mark_pf,(unsigned long)probe_pf,(unsigned long)pres_rest_pf,
           (unsigned long)(sum_pf / 1000UL),(unsigned long)((sum_pf / 100UL) % 10UL),
           (unsigned long)(other_pf / 1000UL),(unsigned long)((other_pf / 100UL) % 10UL),
           (unsigned long)main_perf_clamp));

  /*
   * The prices the line above multiplied counts by, each with the number
   * of windows since it was last measured: a price of age zero was taken
   * in this window, and any other age is a price carried forward because
   * this window's probe was refused. Published so that no figure above
   * can be read without knowing what it rests on.
   */
  LOG_HOT(LOG_CAT_PERF,LOG_LVL_INFO,
          ("price mark=%luns/%lu scan=%luus/%lu rect=%luus/%lu pix=%luus/%lu bad=%lu",
           MAIN_PERF_PRICE(price,MAIN_PRICE_MARK),MAIN_PERF_PRICE(price,MAIN_PRICE_SCAN),
           MAIN_PERF_PRICE(price,MAIN_PRICE_RECT),MAIN_PERF_PRICE(price,MAIN_PRICE_PIX),
           (unsigned long)main_perf_price_bad));

  /*
   * What those microseconds were spent on, in the unit the work is done
   * in: per frame, over the same window. The rebuilds of the sprite table
   * and the lines that replay a collision are counted to a tenth -- a
   * couple of them a frame is a figure a tenth changes the meaning of --
   * and everything else in whole units, being thousands a frame where a
   * tenth would say nothing.
   *
   * ent= is the attribute entries the rebuilds walked and span= the line
   * slots those entries filled: three stores each, and the largest thing
   * a rebuild does. col= against colpix= splits the collision replay into
   * the lines the rectangles settled and the lines that walked pixels --
   * the two prices above. w=, note= and hotw= are the picture's work
   * inside the quota: every write pays the mark, some are handed on to
   * the note, and some of those mark a pattern. hots= is the same mark
   * raised by the table rebuild, which is the video call and not the
   * quota. stale= counts the writes that found the table clean and
   * dirtied it, which is what scan= is the consequence of.
   */
  LOG_HOT(LOG_CAT_PERF,LOG_LVL_INFO,
          ("count scan=%lu.%lu ent=%lu span=%lu col=%lu.%lu colpix=%lu.%lu w=%lu note=%lu hotw=%lu hots=%lu stale=%lu",
           MAIN_PERF_TENTHS(cnt->scans,frames),(unsigned long)(cnt->scan_ent / frames),
           (unsigned long)(cnt->scan_span / frames),MAIN_PERF_TENTHS(cnt->col_lines,frames),
           MAIN_PERF_TENTHS(cnt->col_pix,frames),(unsigned long)(cnt->vram_w / frames),
           (unsigned long)(cnt->notes / frames),(unsigned long)(cnt->hot_write / frames),
           (unsigned long)(cnt->hot_scan / frames),(unsigned long)(cnt->spr_stale / frames)));

  /*
   * The translated code's own lines beside this one, at the same pace,
   * out of the processor share just published: the ARM cycles a T-state
   * cost, the part of the instructions that ran translated, and, while
   * a table is armed, its four counters over the window.
   */
  z80c_report(z8010);
}

#endif /* MAIN_MEASURE */

/*
 * ---------------------------------------------------------------------------
 * The presentation of a frame: what the loop below does once line 191 has
 * been counted, before the blanking lines of the emulated raster.
 *
 * Why there and not at the end of the frame. The picture the console
 * shows is the picture as the video memory stood line by line, and line
 * y shows the memory as it was after the processor's quota of line y.
 * The writes of lines 192 to 261 therefore belong to the NEXT picture:
 * presenting when line 191 is counted makes that true with nothing to
 * note about those writes -- they mark tiles the next presentation
 * converts. The pace on the field counter stays at the end of the frame,
 * in the loop, which alone holds a pace.
 *
 * What one presentation does, in order: the ground painted around the
 * picture on the screen that owes it; the colour table set on the screen
 * that owes it; the picture brought up to date and cut into bands, then
 * each band's list -- windows of the background, sprites, priority
 * tiles, backdrop -- built and drawn, one draw call per band; the screen
 * bound for the error path and presented.
 *
 * The state it shares with the loop -- the ground, the two countdowns,
 * the measurement accumulators -- is file scope rather than passed nine
 * ways; the loop below is the one other reader and writer.
 * ---------------------------------------------------------------------------
 */

/*
 * The ground painted around the picture, and how many screens still owe
 * it. What is painted is the NUMBER of the emulated machine's background
 * entry, laid on the three components the way the picture's own pixels
 * carry theirs, and the screen's colour table turns both into the same
 * colour; read once per frame and compared with what was last painted:
 * sixteen bits and one compare per frame in the regime where nothing
 * changes, which is the regime of every game that sets its background
 * entry once. A game that rewrites the colour of that entry and leaves
 * the register alone repaints nothing here: the table follows it.
 *
 * The countdown is armed with the number of screens on every change and
 * spent one screen a frame, each just before that screen is drawn into:
 * the change reaches every screen of the rotation, and none of them is
 * ever repainted while the scan is reading it.
 */
static uint16 main_border_color = 0;
static int32 main_border_repaint = 0;

/*
 * How many screens still owe the current colour table. Each screen of
 * the rotation has a table of its own, and the emulated palette is set
 * on them the way the ground is painted: armed with the screen count
 * at every change of the colour memory, spent one screen a frame just
 * before that screen is drawn into. A table set on the screen the scan
 * is reading would show the old picture in the new colours for one
 * frame; the countdown lets the change travel in two frames instead,
 * which nothing sees. Rearmed if the palette moves again while it is
 * running down, so the next screen always takes the latest table.
 */
static int32 main_clut_repaint = 0;

#if MAIN_MEASURE
/*
 * The accumulators of the presentation's side of the periodic line: the
 * draw calls, the colour table sets, the tile conversions, the window
 * builds -- four spans, disjoint by construction, each closed before the
 * next opens -- and the readings refused as impossible. Written by the
 * presentation and read and cleared by the loop at each window.
 */
static uint32 main_perf_draw = 0;
static uint32 main_perf_clut = 0;
static uint32 main_perf_tiles = 0;
static uint32 main_perf_list = 0;
static uint32 main_perf_clk = 0;

/*
 * How long the last presentation took, wall clock, from its first
 * reading to its last. The presentation sits inside the emulated stretch
 * of the frame -- it is made at line 191 -- and the stretch is what the
 * processor and video figures are shares of, so it is taken out of the
 * stretch before the share is computed: the four spans above and the
 * stretch never overlap, and frame= keeps meaning the whole turn.
 */
static uint32 main_present_usec = 0;

/*
 * One span closed: the reading since its start, less the clock's own
 * cost, added to the accumulator -- or, past what the span can
 * physically be, counted as a bad reading and left out (MAIN_CLK_MAX_*).
 */
static void
main_perf_span(uint32 *acc,
               uint32  start)
{
  uint32 delta;

  delta = sys_usec() - start;
  if(delta < MAIN_CLK_MAX_FRAME_USEC)
    *acc += (delta > main_perf_clock_cost)
            ? (delta - main_perf_clock_cost) : 0UL;
  else
    main_perf_clk++;
}
#endif /* MAIN_MEASURE */

static void
main_present(void)
{
  Err draw_err;
#if MAIN_MEASURE
  uint32 present_start;
  uint32 span_start;
#endif
  int32 bands;
  int32 k;
  CCB *band;

#if MAIN_MEASURE
  present_start = sys_usec();
#endif

  /*
   * The ground, on the cold side of the lines: one load of sixteen bits
   * and one compare while nothing changes, and a fill only while the
   * countdown says a screen still owes the number. A program that moves
   * register 7 on every frame arms the countdown again on every frame
   * and so pays one fill a frame -- there is no cheaper honest answer to
   * a background that really does change that often -- and it still
   * pays only one line of trace per report window, the count of the
   * window going with the other aggregates. A program that rewrites the
   * colour of the entry instead pays nothing here: the table below
   * carries that.
   *
   * Before the drawing and not after: the fill covers the whole screen,
   * so the picture has to land on top of it.
   */
  {
    uint16 backdrop_now = vdp_backdrop();

    if(backdrop_now != main_border_color)
      {
        main_border_color = backdrop_now;
        main_border_repaint = sys_screen_count();
      }

    if(main_border_repaint > 0)
      {
        /*
         * Spent on the paint, not on the attempt. A refused fill leaves
         * that screen showing the ground it had, and the countdown has
         * to come back to it on the next turn -- decrementing here
         * regardless would leave one screen of the rotation with the old
         * colour for the rest of the run, half the frames of a game
         * showing the wrong ground and nothing saying why. The count
         * that goes with the aggregates is a count of paints for the
         * same reason: it must not report a paint that did not happen.
         */
        if(sys_fill_screen(sys_screen_index(),(Color)main_border_color) >= 0)
          {
            main_border_repaint--;
            vdp_backdrop_repainted();
          }
      }
  }

  /*
   * The palette, on the same cold side: one call into the video part,
   * which rebuilds the 32 entries only if a colour byte moved this
   * frame, and a set on the screen about to be drawn only while the
   * countdown says it still owes the table. A program that rewrites its
   * palette every frame pays one set a frame -- the video part folded
   * its 32 writes into one rebuild -- and a program that leaves it alone
   * pays a load and a compare.
   *
   * Spent on success only, as the fill above is: a refused set leaves
   * that screen with the table it had, and the countdown comes back to
   * it on its next turn rather than leaving half the frames of the run
   * in the wrong colours. The set is timed on its own, apart from the
   * draw: it is the display's cost, not the engine's, and the line
   * publishes it beside the draw so the two never blur.
   */
  if(vdp_clut_take() != 0)
    main_clut_repaint = sys_screen_count();

  /*
   * A picture whose palette changed on some line: the screen about to
   * be drawn into takes a list of one table per segment, on every such
   * picture (the system copies a list, so a new one is the only way to
   * change it), inside the table's span. When the display refused the
   * form at boot, or refuses this list, the screen takes the last table
   * the ordinary way and the picture is counted degraded. A picture
   * with no segment after one with is a change the video part reports,
   * and the countdown below then gives every screen its system list
   * back with the table.
   */
  {
    const uint32 *seg_tables;
    const uint8 *seg_lines;
    uint32 seg_count;

    seg_count = vdp_clut_segments(&seg_tables,&seg_lines);
    if((seg_count > 1UL) && (sys_vdl_ok() == 0))
      vdp_clut_refused();
    if((seg_count > 1UL) && (sys_vdl_ok() != 0))
      {
#if MAIN_MEASURE
        span_start = sys_usec();
#endif
        if(sys_set_colors_lines(sys_screen_index(),seg_tables,seg_lines,
                                (int32)seg_count) < 0)
          {
            vdp_clut_refused();
            (void)sys_set_colors(sys_screen_index(),vdp_clut(),
                                 (int32)VDP_CLUT_ENTRIES);
          }
#if MAIN_MEASURE
        main_perf_span(&main_perf_clut,span_start);
#endif
        main_clut_repaint = sys_screen_count();
      }
    else if(main_clut_repaint > 0)
      {
#if MAIN_MEASURE
        span_start = sys_usec();
#endif
        if(sys_set_colors(sys_screen_index(),vdp_clut(),(int32)VDP_CLUT_ENTRIES) >= 0)
          main_clut_repaint--;
#if MAIN_MEASURE
        main_perf_span(&main_perf_clut,span_start);
#endif
      }
  }

  /*
   * The background: the picture brought up to date and cut into bands
   * -- the tiles' span -- then each band's windows built -- the list's
   * span -- and drawn, one draw call per band, inside the draw's span
   * with the sprite cel below. The build of band k + 1 waits for the
   * draw of band k: the blocks come from one arena and the video memory
   * is replayed in place, and the draw is synchronous
   * (docs/3do/3DO_Development_Notes.md:73), so the engine is done with
   * band k when the call returns. A band with no block is skipped, said
   * once; the video part has counted it.
   */
#if MAIN_MEASURE
  span_start = sys_usec();
#endif
  bands = vdp_list_begin();
#if MAIN_MEASURE
  main_perf_span(&main_perf_tiles,span_start);
#endif

  for(k = 0; k < bands; k++)
    {
#if MAIN_MEASURE
      span_start = sys_usec();
#endif
      band = (CCB *)vdp_list_band(k);
#if MAIN_MEASURE
      main_perf_span(&main_perf_list,span_start);
#endif
      if(band == NULL)
        {
          LOG_ONCE(LOG_CAT_VDP,LOG_LVL_ERR,
                   ("decor band %ld has no window to draw",(long)k));
          continue;
        }
#if MAIN_MEASURE
      span_start = sys_usec();
#endif
      draw_err = DrawCels(sys_bitmap(),band);
#if MAIN_MEASURE
      main_perf_span(&main_perf_draw,span_start);
#endif
      if(draw_err < 0)
        LOG_ONCE(LOG_CAT_VDP,LOG_LVL_ERR,
                 ("decor draw failed err=%ld",(long)draw_err));
    }

  vdp_list_end();

  /*
   * The screen the error path would paint on, renewed on every turn so
   * that it follows the rotation. It names the screen about to be
   * presented, which is the one a viewer is looking at for the whole of
   * the next frame: a stop anywhere in that frame writes its message
   * over the picture that is on the console, and presents it again.
   * Bound after the presentation instead, it would name the screen the
   * scan is not reading, and the message would appear over a picture
   * two frames old.
   *
   * Renewed and not bound once, because a binding taken at boot names
   * one screen for good: with the screens rotating, every other frame
   * would paint its message where nothing can see it.
   */
  log_bind_screen(sys_bitmap(),sys_screen());
  (void)sys_display_show();

#if MAIN_MEASURE
  main_present_usec = sys_usec() - present_start;
#endif
}

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
   * whole frame and the two sampled sides of a line inside it. They are
   * written by this function and by no other, and the presentation's
   * four spans by main_present above and by no other: a module told to
   * do its share of a turn does not time itself, because the timing
   * belongs where the turn is cut up and where the pace is held.
   */
  uint32 perf_emul = 0;
  uint32 perf_emul_frames = 0;
  uint32 perf_present = 0;
  uint32 perf_z80 = 0;
  uint32 perf_z80_samples = 0;
  uint32 perf_vdp = 0;
  uint32 perf_vdp_samples = 0;
  /*
   * How many lines of the window were sampled at all, refused readings
   * included: three readings of the clock each, and what they cost is a
   * post of its own on the decomposition line.
   */
  uint32 perf_line_samples = 0;
  /*
   * What one of each priced piece of work costs, and how old each figure
   * is. Priced once per window on the cold side of the frame; a window
   * whose probe was refused keeps the price it knew and publishes its age
   * rather than passing a stale figure off as a fresh one.
   */
  main_price_t perf_price[MAIN_PRICE_COUNT];
  uint32 perf_price_i;
  int32 perf_price_line;
  uint32 perf_price_case;
  /*
   * The video part's counters as the previous window closed, and the
   * window's own share of them. Running totals on that side, differences
   * here: the two windows are not the same window, and the difference is
   * exact across a wrap.
   */
  vdp_perf_t perf_cnt_prev;
  vdp_perf_t perf_cnt_now;
  vdp_perf_t perf_cnt;
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
   * allocator. Every refusal it returns is a stop, each with its screen
   * below: a program with no video memory has nowhere to put its first
   * tile, and a picture with no page has nowhere to come out.
   */
  /*
   * The screen text below states the cache size in prose, and prose does
   * not follow a constant: the guard makes a change of size refuse to
   * build until the words are brought back in line.
   */
#if VDP_TC_TOTAL_BYTES != 36864UL
#error "the tile cache screen text says 36 kilobytes: update it with the new size"
#endif

  err = vdp_init();
  if(err < 0)
    {
      /*
       * A screen of its own for every refusal that has one: the player
       * quotes a code, and the code must name what actually went wrong.
       * The byte order stop is not an allocation at all, so it gets its
       * own screen rather than being read out as a memory failure. The
       * plane table has none yet and falls to the video memory screen,
       * which names the wrong block for it -- a defect older than the row
       * cache and recorded as such, not one to fix in passing here.
       */
      LOG_ERR(LOG_CAT_VDP,("boot aborted: vdp init err=%ld",(long)err));
      if(err == VDP_ERR_NO_TILECACHE)
        log_fatal(LOG_CAT_VDP,LOG_E_VDP_TILECACHE,
                  "cannot allocate the tile cache",
                  "36 kilobytes of decoded tile rows");
      else if(err == VDP_ERR_LANE_ORDER)
        log_fatal(LOG_CAT_VDP,LOG_E_VDP_LANEORDER,
                  "this build has the wrong byte order",
                  "the picture was built for the other one");
      else if(err == VDP_ERR_NO_DECOR)
        log_fatal(LOG_CAT_VDP,LOG_E_VDP_DECOR,
                  "no page for the background picture",
                  "64k refused, or the blocks outrun it");
      else if(err == VDP_ERR_NO_SPRITES)
        log_fatal(LOG_CAT_VDP,LOG_E_VDP_SPRITES,
                  "no page for the sprite sheet",
                  "64k refused for the sheet and the cels");
      else
        log_fatal(LOG_CAT_VDP,LOG_E_VDP_VRAM,
                  "cannot allocate the video ram",
                  "the console refused 16 kilobytes");
      return (int)err;
    }

  /*
   * The clip rectangle of every screen set on the picture's area, once:
   * everything the frame loop draws -- the background windows, the
   * sprite cel -- is cut to it, and positioned from its corner (sys.h,
   * sys_clip). What it erases is the alignment columns of a window,
   * which start left of the picture; the ground around the picture is
   * the one drawing outside it, and the fill puts the whole bitmap back
   * for its own call. A refusal is traced and the run goes on: the
   * picture then shows at most three stray columns at its left edge, and
   * the trace says why.
   */
  {
    int32 view_x;
    int32 view_y;
    int32 view_w;
    int32 view_h;
    int32 screen;

    vdp_view(&view_x,&view_y,&view_w,&view_h);
    for(screen = 0; screen < sys_screen_count(); screen++)
      {
        if(sys_clip(screen,view_x,view_y,view_w,view_h) < 0)
          LOG_ERR(LOG_CAT_VDP,("view clip refused on screen %ld",(long)screen));
      }
    LOG_INFO(LOG_CAT_VDP,("view clip %ldx%ld at %ld,%ld set on %ld screens profile=%s",
                          (long)view_w,(long)view_h,(long)view_x,(long)view_y,
                          (long)sys_screen_count(),
                          cart_system_name(sms.cart.system)));
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
   * The translated code paired with the cartridge just loaded, after the
   * reset and before the footprint: it allocates nothing, and its one
   * line says whether the core will run blocks or interpret alone.
   */
  z80c_init();

  /*
   * Every steady allocation is now in place -- the screen above, the sound
   * path just now, the emulated memory and the ROM buffer a moment ago -- so
   * this is where the footprint is worth measuring. Any
   * earlier and the figure would describe a program that is not finished
   * starting; the pattern this follows makes the same point
   * (src_exemple_video_player/main.c:915-921).
   */
#if SMS_MEM_PROBE
  /*
   * Before the footprint is taken, and that is the difference from the three
   * mock-ups below, which install after it. They measure translated code and
   * their buffers are not part of what the program runs on; this one takes
   * sixty-four kilobytes of the same DRAM the render competes for, and a
   * footprint line that did not carry it would describe a build nobody is
   * running. The figure the epic makes opposable is the figure of the build
   * that produced the measurement.
   */
  (void)memprobe_install();
#endif

#if SMS_CEL_PROBE
  /*
   * Same side of the footprint as the memory probe, and for the same
   * reason: it takes two pages of the DRAM the render competes for, and
   * the footprint line must carry them to describe the build that
   * measures.
   */
  (void)celprobe_install();
#endif

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

#if SMS_MEM_PROBE
  /*
   * Same side of the door and for the same reason as the three above: after
   * the seal, so that a probe needing memory to run would be caught instead of
   * hidden, and before the loop, because it holds the processor for several
   * seconds -- a frame that took several seconds would be a frame destroyed
   * rather than a frame slowed.
   */
  (void)memprobe_measure();
#endif

#if SMS_CEL_PROBE
  /*
   * After the seal like the probes above, but not before the loop: in its
   * place. Once installed it draws its test pattern, prints its figures
   * and holds the pattern on the screen for ever, because the cartridge
   * would otherwise draw over it with a colour table that is no longer
   * its own. Everything below this call runs only when the probe could
   * not get its pages, which it has said in the trace.
   */
  celprobe_measure();
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
  main_border_color = vdp_backdrop();
  main_border_repaint = sys_screen_count();

  /*
   * The colour table, armed the same way: the video part built it at init
   * over the zeroed colour memory, and each screen takes it just before
   * its first drawing, so no frame is ever shown through the linear table
   * the display came up with.
   */
  main_clut_repaint = sys_screen_count();

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
  main_perf_clock_cost = 0;
  line_start = 0;
  for(perf_delta = 0; perf_delta < MAIN_CLK_COST_PAIRS; perf_delta++)
    {
      emul_start = sys_usec();
      emul_start = sys_usec() - emul_start;
      if(emul_start < MAIN_CLK_MAX_LINE_USEC)
        {
          main_perf_clock_cost += emul_start;
          line_start++;
        }
    }
  if(line_start != 0UL)
    main_perf_clock_cost /= line_start;
  LOG_INFO(LOG_CAT_PERF,("clock read cost=%luus (taken out of each line sample)",
                         (unsigned long)main_perf_clock_cost));

  /*
   * The first reading of the video part's running counters, so that the
   * first window reports its own share and not everything since the init.
   */
  vdp_perf_counts(&perf_cnt_prev);

  /*
   * No price known yet, and an age that says so: the first window has
   * taken none, and a zero price publishes a zero post -- which is the
   * honest answer until a probe has run once.
   */
  for(perf_price_i = 0; perf_price_i < MAIN_PRICE_COUNT; perf_price_i++)
    {
      perf_price[perf_price_i].value = 0;
      perf_price[perf_price_i].age = 0;
    }

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
               * The raw stretch goes into the accumulator and nothing is
               * taken off it here. Each span carries the cost of the
               * reading that closes it, and that cost is taken off the
               * TOTAL when the mean is formed, once per window: taken off
               * each sample instead, a span shorter than one reading
               * would floor at zero every time and a small post would be
               * published as no post at all.
               */
              line_start = sys_usec();
              residue = z80_run((int32)MAIN_TSTATES_PER_LINE - residue);
              line_mid = sys_usec();
              vdp_line();
              line_end = sys_usec();

              perf_line_samples++;

              perf_delta = line_mid - line_start;
              if(perf_delta < MAIN_CLK_MAX_LINE_USEC)
                {
                  perf_z80 += perf_delta;
                  perf_z80_samples++;
                }
              else
                main_perf_clk++;

              perf_delta = line_end - line_mid;
              if(perf_delta < MAIN_CLK_MAX_LINE_USEC)
                {
                  perf_vdp += perf_delta;
                  perf_vdp_samples++;
                }
              else
                main_perf_clk++;
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

          /*
           * The presentation, once the last line of the picture has been
           * counted and before the first blanking line runs: the picture
           * is whole, and the writes of the lines to come belong to the
           * next one (main_present says why). Outside the sampled spans
           * above, which have closed; inside the emulated stretch, which
           * takes its duration out below.
           */
          if(line == (int32)(VDP_ACTIVE_LINES - 1UL))
            main_present();

          /* place of the sound part: accumulate this line's samples */
        }
#if MAIN_MEASURE
      /*
       * The stretch less the presentation it enclosed: the two are
       * measured apart and published apart. A stretch that came out
       * shorter than the presentation is a clock jump, refused below
       * like any impossible reading.
       */
      perf_delta = (sys_usec() - emul_start) - main_present_usec;
      if((perf_delta < MAIN_CLK_MAX_FRAME_USEC)
         && (main_present_usec < MAIN_CLK_MAX_FRAME_USEC))
        {
          perf_emul += perf_delta;
          /*
           * The presentation of the same frame, kept beside the stretch
           * and over the same frames: what the four spans of the
           * presentation are held against, so that the part of it no
           * span describes can be published instead of falling into the
           * residual unnamed.
           */
          perf_present += main_present_usec;
          perf_emul_frames++;
        }
      else
        main_perf_clk++;
      perf_sample_phase = (perf_sample_phase + 1UL) & (MAIN_PERF_SAMPLE_STRIDE - 1UL);
#endif


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
          /*
           * The prices, taken here and nowhere else: the cold side of the
           * frame, once per window, where the periodic line itself is
           * written. Each is a lot run between two readings of the clock,
           * with what the lot will touch saved before and put back after
           * -- outside the timed interval, both of them -- and the
           * restore says whether the lot changed anything it should not
           * have. A price is taken only when the reading is possible, the
           * lot is dearer than nothing, the figure is inside its bound
           * and the lot left no trace; otherwise the price already known
           * is kept and its age says so.
           *
           * Every window ages every price first, then the probes that
           * succeed set theirs back to nothing.
           */
          for(perf_price_i = 0; perf_price_i < MAIN_PRICE_COUNT; perf_price_i++)
            perf_price[perf_price_i].age++;

          /*
           * The mark of one video memory write. Two runs of the same lot
           * over the whole video memory -- one without the mark, one with
           * -- and the difference is the mark, the loop and the load of
           * the byte cancelling out along with the one clock reading each
           * span carries. The dirty marks the lot raises are put back:
           * left standing they would cost the next presentation a sweep
           * of every tile, and the probe would be measuring itself.
           */
          {
            uint32 lot_idle;
            uint32 lot_full;
            uint32 lot_now;
            int32 lot_clean;

            vdp_mark_price_save();
            lot_now = sys_usec();
            (void)vdp_mark_price_run(0);
            lot_idle = sys_usec() - lot_now;
            lot_now = sys_usec();
            (void)vdp_mark_price_run(1);
            lot_full = sys_usec() - lot_now;
            lot_clean = vdp_mark_price_restore();

            perf_delta = MAIN_MARK_NSEC_MAX;
            if((lot_idle < MAIN_CLK_MAX_LINE_USEC)
               && (lot_full < MAIN_CLK_MAX_LINE_USEC)
               && (lot_full > lot_idle))
              perf_delta = ((lot_full - lot_idle) * 1000UL)
                           / VDP_MARK_PRICE_LOT;

            if((lot_clean != 0) && (perf_delta < MAIN_MARK_NSEC_MAX))
              {
                perf_price[MAIN_PRICE_MARK].value = perf_delta;
                perf_price[MAIN_PRICE_MARK].age = 0;
              }
            else
              main_perf_price_bad++;
          }

          /*
           * One rebuild of the per-line sprite table, the whole of it,
           * priced by running the real rebuild a few times over. What it
           * leaves behind is the table line 0 of the next frame would
           * have built anyway, and the two marks that say whether that
           * rebuild is still owed are put back (vdp.h).
           */
          {
            uint32 lot_now;
            uint32 lot_usec;
            int32 lot_clean;

            vdp_scan_price_save();
            lot_now = sys_usec();
            vdp_scan_price_run();
            lot_usec = sys_usec() - lot_now;
            lot_clean = vdp_scan_price_restore();

            perf_delta = MAIN_PRICE_USEC_MAX;
            if((lot_usec < MAIN_CLK_MAX_FRAME_USEC)
               && (lot_usec > main_perf_clock_cost))
              perf_delta = (lot_usec - main_perf_clock_cost)
                           / VDP_SCAN_PRICE_LOT;

            if((lot_clean != 0) && (perf_delta < MAIN_PRICE_USEC_MAX))
              {
                perf_price[MAIN_PRICE_SCAN].value = perf_delta;
                perf_price[MAIN_PRICE_SCAN].age = 0;
              }
            else
              main_perf_price_bad++;
          }

          /*
           * One line's collision replay, in each of its two cases. The
           * lines are found in the table the last frame left, the first
           * two that carry two admitted sprites; which case a line falls
           * into is not known until it has been run, so each run sets the
           * price of whichever case it turned out to be. A scene with one
           * case only prices that one, and the other keeps its age --
           * which is exactly what the age is published for.
           */
          perf_price_line = -1;
          for(perf_price_i = 0; perf_price_i < 2UL; perf_price_i++)
            {
              uint32 lot_now;
              uint32 lot_usec;
              int32 lot_clean;

              perf_price_line = vdp_collide_price_line(perf_price_line);
              if(perf_price_line < 0)
                break;

              vdp_collide_price_save();
              lot_now = sys_usec();
              perf_price_case = vdp_collide_price_run(perf_price_line);
              lot_usec = sys_usec() - lot_now;
              lot_clean = vdp_collide_price_restore();

              perf_delta = MAIN_PRICE_USEC_MAX;
              if((lot_usec < MAIN_CLK_MAX_FRAME_USEC)
                 && (lot_usec > main_perf_clock_cost))
                perf_delta = (lot_usec - main_perf_clock_cost)
                             / VDP_COLLIDE_PRICE_LOT;

              if((lot_clean != 0) && (perf_delta < MAIN_PRICE_USEC_MAX))
                {
                  uint32 slot = (perf_price_case != 0UL)
                                ? (uint32)MAIN_PRICE_PIX
                                : (uint32)MAIN_PRICE_RECT;

                  perf_price[slot].value = perf_delta;
                  perf_price[slot].age = 0;
                }
              else
                main_perf_price_bad++;
            }

          /*
           * The video part's running counters, differenced against the
           * reading the previous window left: the window's own share of
           * the walks, the replays and the writes. After the probes, so
           * that whatever they moved and put back is already back.
           */
          vdp_perf_counts(&perf_cnt_now);

          /*
           * Field by field, and the previous reading carried forward in
           * the same breath: the compiler warns on an assignment of a
           * whole structure, and a warning left standing in this file is
           * one more line for the next reader to have to dismiss.
           */
          perf_cnt.vram_w = perf_cnt_now.vram_w - perf_cnt_prev.vram_w;
          perf_cnt_prev.vram_w = perf_cnt_now.vram_w;
          perf_cnt.notes = perf_cnt_now.notes - perf_cnt_prev.notes;
          perf_cnt_prev.notes = perf_cnt_now.notes;
          perf_cnt.hot_write = perf_cnt_now.hot_write - perf_cnt_prev.hot_write;
          perf_cnt_prev.hot_write = perf_cnt_now.hot_write;
          perf_cnt.hot_scan = perf_cnt_now.hot_scan - perf_cnt_prev.hot_scan;
          perf_cnt_prev.hot_scan = perf_cnt_now.hot_scan;
          perf_cnt.spr_stale = perf_cnt_now.spr_stale - perf_cnt_prev.spr_stale;
          perf_cnt_prev.spr_stale = perf_cnt_now.spr_stale;
          perf_cnt.scans = perf_cnt_now.scans - perf_cnt_prev.scans;
          perf_cnt_prev.scans = perf_cnt_now.scans;
          perf_cnt.scan_ent = perf_cnt_now.scan_ent - perf_cnt_prev.scan_ent;
          perf_cnt_prev.scan_ent = perf_cnt_now.scan_ent;
          perf_cnt.scan_span = perf_cnt_now.scan_span - perf_cnt_prev.scan_span;
          perf_cnt_prev.scan_span = perf_cnt_now.scan_span;
          perf_cnt.col_lines = perf_cnt_now.col_lines - perf_cnt_prev.col_lines;
          perf_cnt_prev.col_lines = perf_cnt_now.col_lines;
          perf_cnt.col_pix = perf_cnt_now.col_pix - perf_cnt_prev.col_pix;
          perf_cnt_prev.col_pix = perf_cnt_now.col_pix;

          main_perf_emit(perf_now - perf_window,perf_frames,
                         perf_emul,perf_emul_frames,
                         perf_z80,perf_z80_samples,
                         perf_vdp,perf_vdp_samples,
                         perf_line_samples,perf_present,
                         main_perf_draw,main_perf_clut,
                         main_perf_tiles,main_perf_list,
                         perf_over,main_perf_clk,
                         perf_price,&perf_cnt);

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

          /*
           * The next window opens AFTER the probes and the serial writes
           * above, not at the reading that closed the last one. Those two
           * are the instrument's own time -- some milliseconds of lots
           * and four blocking lines -- and they belong to no window: left
           * inside the next one they would land in its wall time without
           * landing in any of its posts, and the residual would carry
           * them as though the emulation had spent them. The gap between
           * two windows is the price of the line that separates them, and
           * it is named here rather than measured into something else.
           */
          perf_window = sys_usec();
          perf_frames = 0;
          perf_emul = 0;
          perf_emul_frames = 0;
          perf_present = 0;
          perf_z80 = 0;
          perf_z80_samples = 0;
          perf_vdp = 0;
          perf_vdp_samples = 0;
          perf_line_samples = 0;
          main_perf_draw = 0;
          main_perf_clut = 0;
          main_perf_tiles = 0;
          main_perf_list = 0;
          main_perf_clk = 0;
        }
#endif
    }
}
