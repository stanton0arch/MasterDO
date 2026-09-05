#ifndef SMS3DO_SYS_H
#define SMS3DO_SYS_H

#include "common.h"

/*
 * Allocates a named block and traces what it cost.
 * AllocMem: include/3do/mem.h:247. MEMTYPE_ANY and MEMTYPE_FILL: :62, :68.
 *
 * Every allocation of the program goes through here, and that is what makes
 * the memory budget readable from the trace alone -- the console offers no
 * other way to see it. An allocation made behind this wrapper's back is
 * invisible to the figures below and defeats them.
 *
 * The name is carried into the trace as it is given, because a size on its own
 * says how much was spent and never on what: "alloc 1024 bytes" teaches
 * nothing, "alloc screen_ctx size=1024 bytes" teaches where to look.
 *
 * Returns NULL, having traced the reason, when the block cannot be had or when
 * the init phase has been closed by sys_mem_seal.
 */
void *sys_alloc(const char *name, int32 size, uint32 memtype);

/*
 * Emits the memory summary: what this program has allocated, and what the
 * console reports free.
 * AvailMem: include/3do/mem.h:254, filling a MemInfo (:28-33).
 *
 * DRAM and VRAM are asked for in two separate calls, because the
 * documentation is explicit that asking about more than one memory type at a
 * time "may produce unexpected results"
 * (docs/3do/3do_portfolio_2.5.md:21320). Both are reported: the screen buffers
 * live in VRAM, so a DRAM reading alone would draw a false picture.
 *
 * The total is the sum of the sizes handed to sys_alloc, a quantity this
 * program owns entirely, and the free figures sit beside it as an observation
 * of the system rather than as a measurement of this program. Consumption is
 * deliberately not derived from a before-and-after difference of AvailMem: the
 * pools move for reasons that are not ours, and the result would be wrong
 * without looking wrong.
 *
 * Call it once every steady allocation is in place and before the frame loop
 * starts (pattern: src_exemple_video_player/main.c:915-939). Called earlier,
 * the figure describes a program that does not exist yet.
 */
void sys_mem_report(void);

/*
 * Closes the init phase: every later call to sys_alloc is refused and traced.
 *
 * Steady state allocates nothing -- the serial trace and the frame pace both
 * rest on it -- and this turns that rule from a statement into something the
 * console can be caught breaking. Whatever needs memory takes it before this
 * point.
 */
void sys_mem_seal(void);

/*
 * Opens the graphics folio and builds the double-buffered display.
 * Init sequence and display type query: src_exemple_video_player/
 * system_init.c:39-95. The type is never hardwired to NTSC: the console is
 * queried, then any PAL variant is folded to DI_TYPE_PAL1
 * (include/3do/graphics.h:378-384).
 * Returns 0, or a negative SDK Err.
 */
Err sys_display_open(void);

/*
 * Destroys the display and releases the ScreenContext.
 * DeleteBasicDisplay: include/3do/displayutils.h:57.
 */
Err sys_display_close(void);

/*
 * Bitmap Item of the screen being drawn into -- exactly what FillRect and
 * DrawText8 expect (include/3do/graphics.h:800, :796). Returns 0 while the
 * display is not open.
 *
 * "Being drawn into" and not "displayed": the screens rotate on every
 * presentation (sys_display_show), so this is the hidden one, the one the
 * scan is not reading. A caller that paints what the viewer must see this
 * instant -- an error screen -- paints the one it presented, and presents
 * it again.
 */
Item sys_bitmap(void);

/*
 * Screen Item of the screen being drawn into -- what DisplayScreen expects
 * (include/3do/graphics.h:793). Returns 0 while the display is not open.
 * Rotates with sys_bitmap above.
 */
Item sys_screen(void);

#if SMS_CEL_PROBE
/*
 * The same two Items for a screen named by its index in the rotation,
 * whether or not it is the one being drawn into. A caller that must act
 * on every screen -- a colour table to load on all of them, a display
 * list bound to one of them -- names the screen it means. An index
 * outside the count is refused, traced once, and answered 0, as is a
 * display that is not open.
 *
 * Compiled with their one caller and not otherwise: the delivered
 * binary has no use for them yet, and a build with the probe off must
 * be the build that was there before it, to the byte. The day the
 * render loads a colour table on every screen, the guard goes.
 */
Item sys_screen_at(int32 index);
Item sys_bitmap_at(int32 index);
#endif /* SMS_CEL_PROBE */

/*
 * How many screens the rotation runs over, and which one is being drawn
 * into. The count is what a caller arms a per-screen countdown with when
 * it has something to repaint on all of them -- a change of background
 * colour, one screen a frame, each just before it is drawn again -- and
 * the index is what names the screen it repaints. The index is 0 while
 * the display is not open.
 */
int32 sys_screen_count(void);
int32 sys_screen_index(void);

/*
 * Dimensions of the bitmap the SDK actually built
 * (include/3do/graphics.h:393-394), rather than hardwired constants: a PAL
 * raster is not the same height as an NTSC one
 * (src_exemple_video_player/system_init.c:96-140).
 */
int32 sys_width(void);
int32 sys_height(void);

/*
 * Fills a named screen, or the one being drawn into, with a solid color.
 * SetFGPen then FillRect idiom: src_exemple/file_api/main.c:97-98.
 *
 * The named form is the primitive; sys_fill is it applied to
 * sys_screen_index, kept for the boot sequence, which paints one screen
 * and has no rotation to think about yet. The two are the same call, and
 * what separates them is what the call site is able to say: with the
 * screens rotating, a caller painting them one per frame is doing
 * something to a particular screen, and the named form makes it write
 * which one. That is worth a line of source -- the fill and the drawing
 * that follows it must land on the same screen, and a reader has to be
 * able to see that they do. An index outside the count is refused and
 * traced once.
 */
Err sys_fill_screen(int32 index, Color color);
Err sys_fill(Color color);

/*
 * Draws one line of text at the given position, using the graphics folio's
 * current font.
 * SetFGPen, MoveTo, DrawText8 idiom: src_exemple/file_api/main.c:111-113.
 */
Err sys_text(int32 x, int32 y, const char *text, Color color);

/*
 * Presents the screen just drawn, then rotates: the next drawing goes to
 * another screen while the scan reads the one presented here.
 * DisplayScreen: include/3do/graphics.h:793; the rotation idiom itself is
 * src_exemple_video_player/main.c:730-735.
 *
 * The rotation happens whether or not the presentation was accepted, and
 * that is a choice rather than a property. A refused DisplayScreen leaves
 * the console showing the screen it was already showing, so turning does
 * put the next frame's drawing into the one being scanned -- holding the
 * rotation back is what would avoid that. It is not done, for two
 * reasons: a refused presentation is already an error, traced, on a
 * console whose display path has just failed and whose next frame is not
 * what the run is about; and a rotation that sometimes steps and
 * sometimes does not makes the position of the screens depend on a
 * failure history, which every caller that repaints per screen would then
 * have to reason about. One step per call, always, keeps that reasoning
 * to one sentence.
 */
Err sys_display_show(void);

/*
 * Opens the field pacer -- the I/O request every call below needs.
 * GetVBLIOReq: include/3do/graphics.h:848. CreateVBLIOReq is the same call
 * under its newer name (:697), so there is one API here and not two.
 *
 * Failure is reported and tolerated rather than propagated as a stop: a run
 * that cannot hold a pace is still observable, a run that halted is not. The
 * caller learns of it from the return value and keeps going
 * (src_exemple_video_player/main.c:814-816).
 *
 * Requires the graphics folio to be open. Returns 0, or a negative SDK Err.
 */
Err sys_vbl_open(void);

/*
 * Field counter of the console, low word.
 * GetVBLTime: include/3do/timerutils.h:90.
 *
 * The SDK hands the counter back as two words because it wraps. Only
 * differences between two readings of the low word are meaningful, and an
 * unsigned subtraction stays exact across the wrap for any interval short of
 * its whole period (src_exemple_video_player/cinepak_decoder.c:18-22).
 * Comparing two absolute readings is what breaks. Returns 0 while no pacer
 * exists.
 */
uint32 sys_vbl_count(void);

/*
 * Blocks until the given number of fields has elapsed.
 * WaitVBL: include/3do/graphics.h:850.
 *
 * A primitive, not a policy: it waits for exactly what it is asked for and
 * decides nothing. Whether to wait, and for how long, belongs to the frame
 * loop -- one place holds the pace, and no other module ever waits, sleeps or
 * synchronises. Returns a negative Err when no pacer exists, which lets a
 * degraded run free-run instead of stalling.
 */
Err sys_vbl_wait(uint32 fields);

/*
 * Opens the audio folio and loads the instruments of the sound path, then
 * reports the knobs each of them exposes.
 * OpenAudioFolio: include/3do/audio.h:453. LoadInstrument: :497.
 *
 * What it proves and what it deliberately does not do. Loading shows that the
 * hardware path is there and that the instrument files ship in the image;
 * nothing is wired and no voice is started, so the console stays silent and
 * that silence is the expected result. Wiring is a separate matter and is not
 * a question of effort: ConnectInstruments (include/3do/audio.h:403) takes
 * literal port names, and no document of docs/3do/ names the ports of
 * square.dsp or of either mixer, nor does any call enumerate them the way
 * GetNumKnobs enumerates knobs.
 *
 * The knob names are what this walk is for. They exist nowhere in writing
 * either, and GetKnobName (include/3do/audio.h:469, :470) is the only way to
 * learn them, which makes a run of the program the only place they can be
 * read.
 *
 * Failure is reported and tolerated rather than propagated as a stop, on the
 * same ground as the field pacer above: a run without sound is still
 * observable, and the error screen is for what genuinely prevents the program
 * from continuing (src_exemple_video_player/main.c:879-880). Whatever had
 * been loaded when a step failed is released before returning, so a failed
 * call leaves nothing behind. Returns 0, or a negative SDK Err.
 */
Err sys_audio_open(void);

/*
 * Unloads the instruments in the reverse of their load order, then closes the
 * folio.
 * UnloadInstrument: include/3do/audio.h:460. CloseAudioFolio: :454, the
 * documented counterpart of the open (docs/3do/3do_portfolio_2.5.md:32868).
 *
 * Reverse order is the teardown of the pattern this follows
 * (src_exemple_video_player/audio_play.c:475-479), minus its disconnect and
 * stop steps, which have nothing to act on here. Returns 0, or the first
 * negative Err met on the way -- the walk goes on to the end whatever
 * happens, since a release skipped over a failed one would leak.
 */
Err sys_audio_close(void);

#if SMS_TELEMETRY
/*
 * Microsecond wall clock, as a single value that wraps naturally.
 * SampleSystemTimeTV: include/3do/time.h:76, filling a timeval of seconds and
 * microseconds (:46-50).
 *
 * The microsecond variant is the one to use and the choice is not cosmetic:
 * SampleSystemTime() (include/3do/time.h:75) proved granular to the second in
 * practice (src_exemple_video_player/cinepak_decoder.c:6-7), which cannot
 * resolve a frame at all.
 *
 * Declared under the telemetry switch together with its only consumer: a clock
 * kept alive to feed an output that was compiled out would leave its cost in
 * the very build whose figures are being read.
 */
uint32 sys_usec(void);
#endif /* SMS_TELEMETRY */

#endif /* SMS3DO_SYS_H */
