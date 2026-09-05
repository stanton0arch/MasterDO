/*
 * The two headers below are quoted like every other include of this directory,
 * and that is a correctness rule rather than a habit: armcc serves its own
 * include path before the one the Makefile gives it, so in angle brackets the
 * armlib headers are read instead of the ones describing the linked libraries
 * (Makefile:96, :103).
 */
#include "audio.h"
#include "timerutils.h"
#include "time.h"

#include "sys.h"
#include "log.h"

/*
 * The module carries its own screen state: no global state structure exists
 * yet, and nothing justifies creating one for a single module.
 */
static ScreenContext *sys_ctx = NULL;
static int32 sys_bm_width = 0;
static int32 sys_bm_height = 0;

/*
 * Display type the console was actually built with, kept so that the pacer can
 * name it when it comes up. Which raster the host presents cannot be told by
 * eye and changes what a frame rate means.
 */
static int32 sys_display_used = DI_TYPE_DEFAULT;

/*
 * The field pacer. Zero until it is opened, and it may legitimately stay so:
 * the console is not required to give one, and the program runs unpaced rather
 * than not at all.
 */
static Item sys_vbl_ior = 0;

/*
 * Whether the audio folio is open. It gates the release path as much as it
 * makes the open idempotent: closing a folio that was never opened, or
 * unloading instrument Items left over from a failed open, would be a call
 * made on nothing.
 */
static int32 sys_audio_open_done = 0;

/*
 * The memory ledger. Both live here and nowhere else: the running total only
 * means something if a single counter sees every allocation, and the flag only
 * proves something if a single gate refuses them.
 *
 * The total is never decremented on a release, and that is not an oversight.
 * What it answers is what the init phase spent, so it is meant to match the
 * sum of the traced alloc lines exactly; subtracting a release that traces
 * nothing would break that correspondence and leave no way to check the figure
 * against anything.
 *
 * Two things it does not count, and both matter when the figure is read
 * against a memory budget: the code and static data of the binary, and the
 * task stack (Makefile:6). Neither goes through an allocator. This is the
 * dynamic half of the footprint, and the size of takeme/LaunchMe is the other.
 */
static uint32 sys_mem_total = 0;
static int32 sys_mem_sealed = 0;

/*
 * How many screens the display is built with, and how deep the rotation
 * of sys_display_show runs. Two: one being drawn while the other is being
 * scanned, which is the buffering that keeps a picture from being
 * composed under the beam. Every loop over the screens and the countdown
 * a caller arms to repaint all of them read this one figure.
 */
#define SYS_NUM_SCREENS 2

/*
 * The instruments of the sound path, loaded in this order and released in the
 * reverse one. square.dsp is the square wave generator the emulated sound chip
 * needs, one instance per voice once voices are driven; the mixer sums them to
 * a stereo pair and directout.dsp hands that pair to the DAC
 * (docs/3do/3do_portfolio_2.5.md:35755). sampler.dsp (:35613) belongs to the
 * same path.
 *
 * Two of the four are guesses that this walk is meant to inform rather than
 * settle. mixer4x2.dsp is taken for its input count -- three square voices
 * plus a noise voice -- where mixer8x2.dsp would do as well with room to
 * spare; loading it commits to nothing, since nothing is wired through it.
 */
#define SYS_AUDIO_COUNT 4

static const char *const sys_audio_name[SYS_AUDIO_COUNT] =
  {
    "square.dsp",
    "sampler.dsp",
    "mixer4x2.dsp",
    "directout.dsp"
  };

/*
 * The loaded Items, or zero for a slot that did not load. Zero rather than a
 * negative code, so that one test tells a usable Item from anything else, the
 * way the pacer request above is tested.
 */
static Item sys_audio_item[SYS_AUDIO_COUNT];

/*
 * Execution order in the DSP, and the priority under which voices are stolen.
 * The documented range is 0 to 200 and the documented typical value is 100
 * (docs/3do/3do_portfolio_2.5.md:32139), which is what the reference player
 * uses (src_exemple_video_player/audio_play.c:40). Named rather than written
 * at each call: the four instruments must share one rank, or their order of
 * execution would rest on the order they happen to be loaded in.
 */
#define SYS_AUDIO_PRIORITY 100

/*
 * Rate reported when the folio cannot be asked. Never used as the rate: it is
 * what the trace falls back on, and it says so when it does.
 */
#define SYS_AUDIO_RATE_FALLBACK 44100UL

/*
 * Fallback used only if the SDK reports a bitmap with degenerate dimensions:
 * without this guard a fill or a centring computation would run on zero and
 * the screen would stay blank without the slightest sign
 * (src_exemple_video_player/system_init.c:106-118).
 */
#define SYS_FALLBACK_WIDTH  320
#define SYS_FALLBACK_HEIGHT 240

void *
sys_alloc(const char *name,
          int32       size,
          uint32      memtype)
{
  void *block;

  /*
   * A block with no name would cost as much as any other and teach nothing
   * about what spent it, so it gets one rather than being refused: the caller
   * loses the diagnosis, not the memory.
   */
  if(name == NULL)
    name = "unnamed";

  /*
   * Refused, not served, and said out loud. This is the only place able to
   * catch memory being taken once the program is supposed to be running on
   * what it already has, and catching it is the entire reason the gate exists.
   */
  if(sys_mem_sealed)
    {
      LOG_ERR(LOG_CAT_SYS,("alloc after seal refused: %s size=%ld bytes",
                           name,(long)size));
      return NULL;
    }

  if(size <= 0)
    {
      LOG_ERR(LOG_CAT_SYS,("alloc %s refused: size=%ld bytes",name,(long)size));
      return NULL;
    }

  block = AllocMem(size,memtype);
  if(block == NULL)
    {
      /*
       * Traced and handed back as a failure, never swallowed: what fails to
       * allocate during init is something the program was about to rely on,
       * and the caller is the only one who knows what stopping means.
       */
      LOG_ERR(LOG_CAT_SYS,("alloc %s failed size=%ld bytes",name,(long)size));
      return NULL;
    }

  sys_mem_total += (uint32)size;

  /*
   * The line per allocation is detail and stays out of the default build; the
   * summary it adds up to is not, and comes out whatever the verbosity.
   */
  LOG_DBG(LOG_CAT_SYS,("alloc %s size=%ld bytes",name,(long)size));

  return block;
}

void
sys_mem_report(void)
{
  /*
   * The whole body produces trace lines and nothing else, so it exists only
   * where tracing does. Left in a silent build it would walk the memory pools
   * to throw the answer away.
   *
   * The condition is on the summary's own level rather than on tracing at
   * large: below it the two locals holding the free figures would have no
   * reader, which is one more warning in a build that must produce none.
   */
#if LOG_ENABLE && ((LOG_LVL_INFO) <= (LOG_LEVEL))
  MemInfo mi;
  uint32 dram_free;
  uint32 vram_free;

  /*
   * Two calls, one memory type each, and not a single call asking for both:
   * asking about more than one type at a time may produce unexpected results
   * (docs/3do/3do_portfolio_2.5.md:21320).
   */
  AvailMem(&mi,MEMTYPE_DRAM);
  dram_free = mi.minfo_SysFree;
  LOG_DBG(LOG_CAT_SYS,
          ("mem dram sysfree=%lu syslargest=%lu taskfree=%lu tasklargest=%lu",
           (unsigned long)mi.minfo_SysFree,
           (unsigned long)mi.minfo_SysLargest,
           (unsigned long)mi.minfo_TaskFree,
           (unsigned long)mi.minfo_TaskLargest));

  AvailMem(&mi,MEMTYPE_VRAM);
  vram_free = mi.minfo_SysFree;
  LOG_DBG(LOG_CAT_SYS,
          ("mem vram sysfree=%lu syslargest=%lu taskfree=%lu tasklargest=%lu",
           (unsigned long)mi.minfo_SysFree,
           (unsigned long)mi.minfo_SysLargest,
           (unsigned long)mi.minfo_TaskFree,
           (unsigned long)mi.minfo_TaskLargest));

  /*
   * Of the four fields each pool reports, the system-wide free figure is the
   * one that answers "how much room is left" (mem.h:30). The other three are
   * traced above for whoever needs to know how that room is broken up --
   * fragmentation is what tells a free byte from a usable one.
   *
   * All four are unsigned 32 bit quantities and are printed through unsigned
   * long. The arithmetic stays integer throughout: the ARM60 has no floating
   * point unit and the build passes -fpu none (Makefile:97).
   */
  LOG_INFO(LOG_CAT_SYS,("mem base total=%lu dram_free=%lu vram_free=%lu",
                        (unsigned long)sys_mem_total,
                        (unsigned long)dram_free,
                        (unsigned long)vram_free));
#endif /* LOG_ENABLE && INFO */
}

void
sys_mem_seal(void)
{
  sys_mem_sealed = 1;

  LOG_INFO(LOG_CAT_SYS,("mem alloc sealed"));
}

/*
 * Readable name of the display type in use, for tracing. The values are those
 * of include/3do/graphics.h:378-384. Which raster the host actually presents
 * cannot be told by eye, so it is logged.
 *
 * The helper only exists when logging exists: at LOG_ENABLE 0 its sole callers
 * disappear and armcc would then report an unused static (-fh, Makefile:91),
 * that is, one more warning.
 */
#if LOG_ENABLE
static const char *
sys_display_type_name(int32 display_type)
{
  switch(display_type)
    {
    case DI_TYPE_NTSC:
      return "NTSC";
    case DI_TYPE_PAL1:
      return "PAL1";
    case DI_TYPE_PAL2:
      return "PAL2";
    default:
      break;
    }

  return "DEFAULT";
}
#endif /* LOG_ENABLE */

Err
sys_display_open(void)
{
  Err err;
  int32 display_type;
  int32 reported_type;
  int32 i;
  Bitmap *bm;

  if(sys_ctx != NULL)
    return 0;

  err = OpenGraphicsFolio();
  if(err < 0)
    {
      LOG_ERR(LOG_CAT_SYS,("screen init failed: OpenGraphicsFolio err=%ld",
                           (long)err));
      return err;
    }

  sys_ctx = (ScreenContext *)sys_alloc("screen_ctx",
                                       (int32)sizeof(ScreenContext),
                                       MEMTYPE_ANY | MEMTYPE_FILL);
  if(sys_ctx == NULL)
    {
      /*
       * The wrapper has already named the size and the reason; what is added
       * here is what the block was for, which is the part the caller knows.
       */
      LOG_ERR(LOG_CAT_SYS,("screen init failed: no screen context"));
      return -1;
    }

  /*
   * QueryGraphics is the only call whose failure does not propagate, and that
   * is deliberate: its sole consequence is that the default type is kept,
   * which is exactly the intended fallback. The failure is handled, not
   * ignored.
   */
  display_type = DI_TYPE_DEFAULT;
  if(QueryGraphics(QUERYGRAF_TAG_DEFAULTDISPLAYTYPE,&display_type) < 0)
    {
      display_type = DI_TYPE_DEFAULT;
      LOG_WARN(LOG_CAT_SYS,("QueryGraphics failed, display type left default"));
    }

  reported_type = display_type;

  if((display_type == DI_TYPE_PAL1) || (display_type == DI_TYPE_PAL2))
    display_type = DI_TYPE_PAL1;
  else
    display_type = DI_TYPE_NTSC;

  err = (Err)CreateBasicDisplay(sys_ctx,
                                (uint32)display_type,
                                (uint32)SYS_NUM_SCREENS);
  if(err < 0)
    {
      LOG_ERR(LOG_CAT_SYS,("screen init failed: CreateBasicDisplay type=%s err=%ld",
                           sys_display_type_name(display_type),(long)err));
      FreeMem(sys_ctx,sizeof(ScreenContext));
      sys_ctx = NULL;
      return err;
    }

  /*
   * CreateBasicDisplay does not fill in sc_BitmapItems
   * (src_exemple_video_player/system_init.c:88-95), yet DrawText8 and FillRect
   * take precisely a bitmap Item: without this loop every draw is a silent
   * no-op and the screen stays black.
   */
  for(i = 0; i < SYS_NUM_SCREENS; i++)
    {
      if(sys_ctx->sc_Bitmaps[i] == NULL)
        {
          /* The teardown result is deliberately discarded: the value returned
             is that of the root cause, never that of the cleanup, which would
             mask it. */
          LOG_ERR(LOG_CAT_SYS,("screen init failed: no bitmap for screen %ld",
                               (long)i));
          (void)DeleteBasicDisplay(sys_ctx);
          FreeMem(sys_ctx,sizeof(ScreenContext));
          sys_ctx = NULL;
          return -1;
        }
      sys_ctx->sc_BitmapItems[i] = sys_ctx->sc_Bitmaps[i]->bm.n_Item;
    }

  bm = sys_ctx->sc_Bitmaps[0];
  sys_bm_width = bm->bm_Width;
  sys_bm_height = bm->bm_Height;
  if((sys_bm_width <= 0) || (sys_bm_height <= 0))
    {
      LOG_WARN(LOG_CAT_SYS,("bitmap reports %ldx%ld, falling back to %ldx%ld",
                            (long)sys_bm_width,(long)sys_bm_height,
                            (long)SYS_FALLBACK_WIDTH,(long)SYS_FALLBACK_HEIGHT));
      sys_bm_width = SYS_FALLBACK_WIDTH;
      sys_bm_height = SYS_FALLBACK_HEIGHT;
    }

  /*
   * The rotation starts on screen 0, and every later write of the field
   * is the step in sys_display_show: the boot banner is drawn here and
   * presented once, and from that presentation on the drawing and the
   * scanning are never the same screen.
   */
  sys_ctx->sc_CurrentScreen = 0;
  sys_display_used = display_type;

  LOG_INFO(LOG_CAT_SYS,("screen init ok %ldx%ld",
                        (long)sys_bm_width,(long)sys_bm_height));

  /*
   * The display type is logged separately: 60 Hz and 50 Hz do not give the
   * frame loop the same pace, and the difference is invisible on screen.
   */
  LOG_INFO(LOG_CAT_SYS,("display reported=%s used=%s screens=%ld",
                        sys_display_type_name(reported_type),
                        sys_display_type_name(display_type),
                        (long)SYS_NUM_SCREENS));

  return 0;
}

Err
sys_display_close(void)
{
  Err err;

  if(sys_ctx == NULL)
    return 0;

  err = DeleteBasicDisplay(sys_ctx);
  if(err < 0)
    LOG_ERR(LOG_CAT_SYS,("DeleteBasicDisplay err=%ld",(long)err));
  else
    LOG_INFO(LOG_CAT_SYS,("screen closed"));

  FreeMem(sys_ctx,sizeof(ScreenContext));
  sys_ctx = NULL;
  sys_bm_width = 0;
  sys_bm_height = 0;

  return err;
}

Item
sys_bitmap(void)
{
  if(sys_ctx == NULL)
    return 0;

  return sys_ctx->sc_BitmapItems[sys_ctx->sc_CurrentScreen];
}

Item
sys_screen(void)
{
  if(sys_ctx == NULL)
    return 0;

  return sys_ctx->sc_ScreenItems[sys_ctx->sc_CurrentScreen];
}

#if SMS_CEL_PROBE
Item
sys_screen_at(int32 index)
{
  if(sys_ctx == NULL)
    return 0;

  if((index < 0) || (index >= (int32)SYS_NUM_SCREENS))
    {
      LOG_ONCE(LOG_CAT_SYS,LOG_LVL_ERR,("sys_screen_at: no screen %ld",
                                        (long)index));
      return 0;
    }

  return sys_ctx->sc_ScreenItems[index];
}

Item
sys_bitmap_at(int32 index)
{
  if(sys_ctx == NULL)
    return 0;

  if((index < 0) || (index >= (int32)SYS_NUM_SCREENS))
    {
      LOG_ONCE(LOG_CAT_SYS,LOG_LVL_ERR,("sys_bitmap_at: no screen %ld",
                                        (long)index));
      return 0;
    }

  return sys_ctx->sc_BitmapItems[index];
}
#endif /* SMS_CEL_PROBE */

int32
sys_screen_count(void)
{
  return (int32)SYS_NUM_SCREENS;
}

int32
sys_screen_index(void)
{
  if(sys_ctx == NULL)
    return 0;

  return (int32)sys_ctx->sc_CurrentScreen;
}

int32
sys_width(void)
{
  return sys_bm_width;
}

int32
sys_height(void)
{
  return sys_bm_height;
}

Err
sys_fill_screen(int32 index,
                Color color)
{
  GrafCon gc;
  Rect rect;
  Item bitmap;
  Err err;

  /*
   * Every diagnostic below is one-shot, and all three for the same
   * reason: a fill may be asked for on every frame, and a blocking printf
   * repeated at that rate would not slow the pace down, it would destroy
   * it -- and what a measurement then read would be the tracing (pattern
   * from src_exemple_video_player/cinepak_decoder.c:660-666).
   */
  if((sys_ctx == NULL) || (index < 0) || (index >= (int32)SYS_NUM_SCREENS))
    {
      LOG_ONCE(LOG_CAT_SYS,LOG_LVL_ERR,
               ("sys_fill_screen: no such screen, or display not open"));
      return -1;
    }

  bitmap = sys_ctx->sc_BitmapItems[index];
  if(bitmap == 0)
    {
      LOG_ONCE(LOG_CAT_SYS,LOG_LVL_ERR,
               ("sys_fill_screen: no bitmap, display not open"));
      return -1;
    }

  rect.rect_XLeft = 0;
  rect.rect_YTop = 0;
  rect.rect_XRight = (Coord)sys_bm_width;
  rect.rect_YBottom = (Coord)sys_bm_height;

  SetFGPen(&gc,color);

  err = FillRect(bitmap,&gc,&rect);
  if(err < 0)
    LOG_ONCE(LOG_CAT_SYS,LOG_LVL_ERR,
             ("sys_fill_screen: FillRect err=%ld",(long)err));

  return err;
}

Err
sys_fill(Color color)
{
  return sys_fill_screen(sys_screen_index(),color);
}

Err
sys_text(int32       x,
         int32       y,
         const char *text,
         Color       color)
{
  GrafCon gc;
  Item bitmap;
  Err err;

  if(text == NULL)
    {
      LOG_ONCE(LOG_CAT_SYS,LOG_LVL_ERR,("sys_text: null text"));
      return -1;
    }

  bitmap = sys_bitmap();
  if(bitmap == 0)
    {
      LOG_ONCE(LOG_CAT_SYS,LOG_LVL_ERR,("sys_text: no bitmap, display not open"));
      return -1;
    }

  SetFGPen(&gc,color);
  MoveTo(&gc,(Coord)x,(Coord)y);

  err = DrawText8(&gc,bitmap,(const uint8 *)text);
  if(err < 0)
    LOG_ONCE(LOG_CAT_SYS,LOG_LVL_ERR,("sys_text: DrawText8 err=%ld",(long)err));

  return err;
}

Err
sys_display_show(void)
{
  Err err;

  if(sys_ctx == NULL)
    {
      LOG_ONCE(LOG_CAT_SYS,LOG_LVL_ERR,("sys_display_show: display not open"));
      return -1;
    }

  err = DisplayScreen(sys_ctx->sc_ScreenItems[sys_ctx->sc_CurrentScreen],0);
  if(err < 0)
    LOG_ONCE(LOG_CAT_SYS,LOG_LVL_ERR,("sys_display_show: DisplayScreen err=%ld",
                                      (long)err));

  /*
   * The rotation, and it is the whole point of asking the graphics folio
   * for more than one screen: from here on the drawing goes to another
   * screen than the one the scan has just been handed, so no picture is
   * ever composed under the beam that is reading it.
   *
   * It steps whatever the call above answered, and that is a choice.
   * After a refused presentation the console is still showing the screen
   * it was showing, so stepping does hand the next frame the one being
   * scanned; not stepping would be the safe move for that one frame. What
   * is bought by stepping anyway is that the position of the screens
   * never depends on a history of failures -- a caller that repaints one
   * screen per frame can count frames and be right -- and what is given
   * up is a frame of tearing on a console whose display path has just
   * failed and said so in the trace.
   *
   * The step is a remainder and not a toggle so that the count is the one
   * place the number of screens is written: raising it changes the depth
   * of the rotation and nothing else.
   */
  sys_ctx->sc_CurrentScreen =
    (sys_ctx->sc_CurrentScreen + 1UL) % (uint32)SYS_NUM_SCREENS;

  return err;
}

Err
sys_vbl_open(void)
{
  Item ioreq;
  int32 field_hz;

  if(sys_vbl_ior > 0)
    return 0;

  ioreq = GetVBLIOReq();
  if(ioreq < 0)
    {
      /*
       * Warned and survived, never fatal. The screen error path is reserved
       * for what genuinely prevents the program from continuing, and an
       * unpaced run continues -- it just runs free, which the missing pace
       * line and this warning together explain.
       */
      LOG_WARN(LOG_CAT_SYS,("vbl ioreq failed err=%ld, running unpaced",
                            (long)ioreq));
      return (Err)ioreq;
    }

  sys_vbl_ior = ioreq;

  /*
   * The field rate is asked of the console rather than assumed. NTSC gives 60
   * fields per second and PAL 50 (include/3do/graphics.h:198-200): a frame
   * rate read without knowing which one the host runs says nothing, and the
   * whole point of reporting it here is that nobody has to suppose it.
   */
  field_hz = 0;
  if(QueryGraphics(QUERYGRAF_TAG_FIELDFREQ,&field_hz) < 0)
    {
      /*
       * Zeroed again rather than left as the call found it: a query that
       * failed says nothing about what it wrote first, and a half-written
       * value reported as a field rate would be worse than an obvious zero.
       */
      field_hz = 0;
      LOG_WARN(LOG_CAT_SYS,("field rate query failed, reported below as zero"));
    }

  LOG_INFO(LOG_CAT_SYS,("vbl ioreq ok display=%s fields=%ld",
                        sys_display_type_name(sys_display_used),
                        (long)field_hz));

  return 0;
}

uint32
sys_vbl_count(void)
{
  uint32 hiorder;
  uint32 loworder;

  if(sys_vbl_ior <= 0)
    return 0;

  /*
   * Both outputs are pre-set because GetVBLTime leaves them untouched when the
   * request is not usable (src_exemple_video_player/audio_sync.c:64-69): an
   * uninitialised local would then be read as a field count.
   *
   * The high word is discarded on purpose. The caller works on differences of
   * the low word, which stay exact across its wrap, and joining the two words
   * would only invite the absolute comparison that does not.
   */
  hiorder = 0;
  loworder = 0;
  (void)GetVBLTime(sys_vbl_ior,&hiorder,&loworder);

  return loworder;
}

Err
sys_vbl_wait(uint32 fields)
{
  Err err;

  /*
   * No pacer: return the failure without a word. Its absence was already
   * reported once, when it was found out; repeating it from a call made every
   * frame would add nothing but blocking serial output to a run that is
   * already degraded.
   */
  if(sys_vbl_ior <= 0)
    return -1;

  err = WaitVBL(sys_vbl_ior,fields);
  if(err < 0)
    LOG_ONCE(LOG_CAT_SYS,LOG_LVL_ERR,("sys_vbl_wait: WaitVBL err=%ld",
                                      (long)err));

  return err;
}

/*
 * Sample rate of the DAC, in whole samples per second.
 *
 * The rate is asked of the folio and not written down anywhere, because the
 * documentation asks for exactly that: the usual rate is 44100, some
 * environments run at 48000, and every figure the audio documentation states
 * in samples per second is to be read against whatever this call returns
 * (docs/3do/3do_portfolio_2.5.md:34694). A rate assumed here would be an error
 * nothing on screen or in the sound would ever point to.
 *
 * The value arrives as a 16.16 fixed point number and is read through an
 * unsigned type on purpose. The header names that encoding frac16, a signed
 * type (include/3do/operamath.h:30), yet 44100 in 16.16 is 0xAC440000: the top
 * bit is set, the signed reading of it is negative, and the arithmetic shift
 * that would follow gives a rate no console ever ran at. Integer arithmetic
 * throughout, the ARM60 having no FPU and the build passing -fpu none
 * (Makefile:97).
 */
static uint32
sys_audio_dac_rate(void)
{
  TagArg tags[2];
  Err err;
  uint32 rate;

  tags[0].ta_Tag = AF_TAG_SAMPLE_RATE;
  tags[0].ta_Arg = NULL;
  tags[1].ta_Tag = TAG_END;
  tags[1].ta_Arg = NULL;

  err = GetAudioFolioInfo(tags);
  if(err < 0)
    {
      LOG_ERR(LOG_CAT_SYS,("dac rate query failed err=%ld, assuming %lu",
                           (long)err,(unsigned long)SYS_AUDIO_RATE_FALLBACK));
      return SYS_AUDIO_RATE_FALLBACK;
    }

  rate = ((uint32)tags[0].ta_Arg) >> 16;
  if(rate == 0UL)
    {
      /*
       * A rate of zero is not a rate. It is reported rather than passed on,
       * since every duration computed from it later would divide by it.
       */
      LOG_ERR(LOG_CAT_SYS,("dac rate reported as zero, assuming %lu",
                           (unsigned long)SYS_AUDIO_RATE_FALLBACK));
      return SYS_AUDIO_RATE_FALLBACK;
    }

  return rate;
}

#if LOG_ENABLE
/*
 * Names the knobs of one instrument.
 * GetNumKnobs: include/3do/audio.h:470. GetKnobName: :469.
 *
 * The whole product of this function is a handful of trace lines, so it exists
 * only where tracing does: built into a binary that says nothing, it would
 * walk every knob of every instrument to throw the answer away.
 *
 * The count comes first and at a level that stays visible when the names are
 * compiled out, because the count alone already answers a question -- an
 * instrument reporting no knob at all is either knobless or not really loaded,
 * and that is worth seeing without turning the verbosity up.
 *
 * The name is printed immediately and never kept. It points into system memory
 * rather than to an allocation: it must not be written to or freed, and it
 * stops being valid once the instrument's template is gone
 * (docs/3do/3do_portfolio_2.5.md:31763).
 */
static void
sys_audio_knobs(Item        instrument,
                const char *name)
{
  int32 count;
  int32 i;
  const char *knob;

  count = GetNumKnobs(instrument);
  if(count < 0)
    {
      LOG_ERR(LOG_CAT_SYS,("%s knob count failed 0x%lx",name,(long)count));
      return;
    }

  LOG_INFO(LOG_CAT_SYS,("%s knobs=%ld",name,(long)count));

  /*
   * The index stays inside 0 .. count-1, the range the call is defined over
   * (docs/3do/3do_portfolio_2.5.md:31757), and the pointer it returns is
   * tested before it is printed: NULL is how the call reports a failure, and
   * handing it to the format string would fault on the one path that was
   * supposed to diagnose the instrument.
   */
  for(i = 0; i < count; i++)
    {
      knob = (const char *)GetKnobName(instrument,i);
      if(knob == NULL)
        LOG_ERR(LOG_CAT_SYS,("%s knob[%ld] has no name",name,(long)i));
      else
        LOG_DBG(LOG_CAT_SYS,("%s knob[%ld]=%s",name,(long)i,knob));
    }
}
#endif /* LOG_ENABLE */

Err
sys_audio_open(void)
{
  Err err;
  Err first_err;
  int32 i;
  uint32 rate;

  if(sys_audio_open_done)
    return 0;

  err = OpenAudioFolio();
  if(err < 0)
    {
      LOG_ERR(LOG_CAT_SYS,("audio folio open failed 0x%lx",(long)err));
      return err;
    }

  sys_audio_open_done = 1;

  for(i = 0; i < SYS_AUDIO_COUNT; i++)
    sys_audio_item[i] = 0;

  rate = sys_audio_dac_rate();
  LOG_INFO(LOG_CAT_SYS,("audio folio ok rate=%lu",(unsigned long)rate));

  first_err = 0;

  for(i = 0; i < SYS_AUDIO_COUNT; i++)
    {
      sys_audio_item[i] = LoadInstrument((char *)sys_audio_name[i],
                                         0,
                                         SYS_AUDIO_PRIORITY);
      if(sys_audio_item[i] > 0)
        {
          LOG_INFO(LOG_CAT_SYS,("dsp load %s ok",sys_audio_name[i]));
          continue;
        }

      /*
       * The raw code is reported as it comes: which of a missing file, a
       * refused instantiation or an exhausted DSP it stands for is the whole
       * information, and a code folded into a yes or no would leave the next
       * step nothing to work from.
       */
      LOG_ERR(LOG_CAT_SYS,("dsp load %s failed 0x%lx",sys_audio_name[i],
                           (long)sys_audio_item[i]));
      if(first_err == 0)
        first_err = (sys_audio_item[i] < 0) ? (Err)sys_audio_item[i] : -1;
      sys_audio_item[i] = 0;
    }

#if LOG_ENABLE
  /*
   * The walk runs before any decision is taken on the failures above, and that
   * order is the point: a run where one instrument is missing is precisely the
   * run whose knob list is worth having, and unwinding first would throw away
   * the only thing this pass produces that is written down nowhere else.
   *
   * It runs once, here, outside any loop. The serial output blocks, so these
   * few dozen lines cost a fraction of a second at boot and nothing after.
   */
  for(i = 0; i < SYS_AUDIO_COUNT; i++)
    {
      if(sys_audio_item[i] > 0)
        sys_audio_knobs(sys_audio_item[i],sys_audio_name[i]);
    }
#endif /* LOG_ENABLE */

  if(first_err < 0)
    {
      /*
       * One missing instrument makes the path incomplete, and an incomplete
       * path is not worth the DSP resources the loaded ones hold. What did
       * load is released, and the caller is told, which is as far as this goes
       * -- there is no sound to lose yet, and a console that boots and traces
       * is worth more than one stopped over an instrument.
       */
      (void)sys_audio_close();
      return first_err;
    }

  return 0;
}

Err
sys_audio_close(void)
{
  Err err;
  Err first_err;
  int32 i;

  if(!sys_audio_open_done)
    return 0;

  first_err = 0;

  /*
   * Reverse of the load order, and the reverse of the load is all there is to
   * undo. The teardown this follows disconnects and stops before it unloads
   * (src_exemple_video_player/audio_play.c:475-479); here nothing was ever
   * wired and no voice was ever started, so both steps would be calls made on
   * nothing.
   */
  for(i = SYS_AUDIO_COUNT - 1; i >= 0; i--)
    {
      if(sys_audio_item[i] <= 0)
        continue;

      err = UnloadInstrument(sys_audio_item[i]);
      if(err < 0)
        {
          LOG_ERR(LOG_CAT_SYS,("dsp unload %s failed 0x%lx",sys_audio_name[i],
                               (long)err));
          if(first_err == 0)
            first_err = err;
        }
      else
        LOG_INFO(LOG_CAT_SYS,("dsp unload %s ok",sys_audio_name[i]));

      sys_audio_item[i] = 0;
    }

  err = CloseAudioFolio();
  if(err < 0)
    {
      LOG_ERR(LOG_CAT_SYS,("audio folio close failed 0x%lx",(long)err));
      if(first_err == 0)
        first_err = err;
    }
  else
    LOG_INFO(LOG_CAT_SYS,("audio folio closed"));

  sys_audio_open_done = 0;

  return first_err;
}

#if SMS_TELEMETRY
uint32
sys_usec(void)
{
  TimeVal tv;

  SampleSystemTimeTV(&tv);

  /*
   * The seconds are folded in whole, with no mask. Masking would wrap the
   * counter on a boundary of its own and manufacture a ghost spike, whereas
   * natural unsigned overflow is congruent modulo 2^32: a difference of two
   * readings stays exact for any interval short of its period
   * (src_exemple_video_player/cinepak_decoder.c:14-22).
   */
  return ((uint32)tv.tv_sec * 1000000UL) + (uint32)tv.tv_usec;
}
#endif /* SMS_TELEMETRY */
