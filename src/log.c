/*
 * The single diagnostic output point of the code base.
 *
 * The primitive used is printf(), declared in include/3do/stdio.h:51
 * (int32 printf(const char *fmt, ...)) and linked from lib/community/libc.lib
 * (Makefile:134). No other output API is used: the kprintf() fallback
 * (include/3do/debug.h:21, SWI 0x1000E in
 * docs/3do/software_interrupts.md:23) is documented but not retained.
 *
 * The serial printf is blocking -- src_exemple_video_player/
 * cinepak_decoder.c:659 states it and acts on it. A line is therefore composed
 * entirely in memory, then emitted by a single call: alignment obtained
 * through extra calls would cost dearly.
 *
 * The includes below MUST stay quoted, and this is not a style matter.
 * Quoted, they resolve through -I (Makefile:96) to include/3do/, the headers
 * that describe the libraries actually linked. In angle brackets the compiler
 * serves its own copies from include/rwstl/ instead -- headers written for
 * armlib, which -noscanlib (Makefile:103) keeps out of the link. Those two
 * families disagree on va_list: include/rwstl/stdarg.h:23 makes it an array
 * (char *[1]), so it reaches a callee as the address of the argument pointer,
 * while include/3do/varargs.h:15 makes it the argument pointer itself. Mixing
 * them compiles and links without a single diagnostic, then hands vsprintf an
 * argument pointer one indirection off: every conversion reads stack debris
 * rather than the caller's values.
 *
 * The formatting is done by the kernel: vsprintf (include/3do/stdio.h:54)
 * reaches it through __vfprintf, which is a plain thunk into the folio vector
 * table -- so the convention that counts is the one of include/3do/, and it is
 * the one src_exemple/display.cpp:8,130 already follows.
 */

#include "stdio.h"
#include "stdarg.h"
#include "item.h"

#include "log.h"

/*
 * Width of the [<CAT>][<LVL>] field, space-padded so that f= always starts at
 * the same column. Twelve is the width of the longest combination the default
 * build can produce, [BOOT][INFO].
 */
#define LOG_FIELD_WIDTH 12

/*
 * A whole line fits in this buffer, prefix and end of line included. The SDK
 * has no bounded variant of vsprintf (include/3do/stdio.h:54): the margin is
 * taken here and messages are kept short.
 */
#define LOG_BUF_MAX 256

/*
 * Format string of the single printing call.
 *
 * The composed line is passed as an argument and never as the format: it holds
 * text that has already been through a format pass, and a % surviving in it
 * would be read as a conversion a second time -- against arguments that do not
 * exist.
 */
static const char log_out_fmt[] = "%s";

static const char *const log_cat_name[LOG_CAT_COUNT] =
  {
    "BOOT", "SYS", "CART", "BUS", "Z80", "VDP",
    "PSG", "PAD", "SAVE", "PERF", "GG"
  };

static const char *const log_lvl_name[LOG_LVL_COUNT] =
  {
    "ERR", "WARN", "INFO", "DBG", "TRACE"
  };

static char log_buf[LOG_BUF_MAX];
static int32 log_len = 0;

/*
 * Read-only pointer to the frame counter owned by main.c. log.c never writes
 * through it and does not own it.
 */
static const uint32 *log_frame = NULL;

static void
log_put_char(char c)
{
  if(log_len >= (int32)(LOG_BUF_MAX - 1))
    return;

  log_buf[log_len] = c;
  log_len++;
}

static void
log_put(const char *text)
{
  int32 i;

  if(text == NULL)
    return;

  for(i = 0; text[i] != '\0'; i++)
    log_put_char(text[i]);
}

static void
log_put_u32(uint32 value)
{
  char digits[10];
  int32 n;

  n = 0;
  do
    {
      digits[n] = (char)('0' + (int32)(value % 10UL));
      n++;
      value /= 10UL;
    }
  while((value != 0UL) && (n < (int32)sizeof(digits)));

  while(n > 0)
    {
      n--;
      log_put_char(digits[n]);
    }
}

void
log_set_frame(const uint32 *frame)
{
  log_frame = frame;
}

void
log_begin(int32 cat,
          int32 lvl)
{
  log_len = 0;

  /*
   * An out-of-range index must not index the table: the line would be lost at
   * the exact moment it is most useful.
   */
  log_put_char('[');
  if((cat >= 0) && (cat < LOG_CAT_COUNT))
    log_put(log_cat_name[cat]);
  else
    log_put("????");
  log_put("][");
  if((lvl >= 0) && (lvl < LOG_LVL_COUNT))
    log_put(log_lvl_name[lvl]);
  else
    log_put("????");
  log_put_char(']');

  while(log_len < LOG_FIELD_WIDTH)
    log_put_char(' ');

  log_put_char(' ');
  log_put("f=");
  if(log_frame == NULL)
    log_put_char('-');
  else
    log_put_u32(*log_frame);
  log_put_char(' ');
}

void
log_printf(const char *fmt,
           ...)
{
  va_list args;
  int32 written;

  if(fmt != NULL)
    {
      va_start(args,fmt);
      written = vsprintf(&log_buf[log_len],fmt,args);
      va_end(args);

      if(written > 0)
        log_len += written;
      if(log_len > (int32)(LOG_BUF_MAX - 2))
        log_len = (int32)(LOG_BUF_MAX - 2);
    }

  log_buf[log_len] = '\n';
  log_buf[log_len + 1] = '\0';

  /* The single printing call of the code base -- see log_out_fmt. */
  printf(log_out_fmt,log_buf);

  log_len = 0;
}

const char *
log_build_date(void)
{
  /*
   * __DATE__ is a standard C89 predefined macro of the form "Mmm DD YYYY" --
   * the day carries a leading space below 10. The boot line wants YYYY-MM-DD:
   * the conversion happens here, once, with no SDK call. Should the month go
   * unrecognised, the raw string is returned rather than a wrong date.
   */
  static const char log_months[] = "JanFebMarAprMayJunJulAugSepOctNovDec";
  static char log_date[11] = "";

  const char *src;
  int32 m;

  if(log_date[0] != '\0')
    return log_date;

  src = __DATE__;

  for(m = 0; m < 12; m++)
    {
      if((log_months[m * 3] == src[0])
         && (log_months[(m * 3) + 1] == src[1])
         && (log_months[(m * 3) + 2] == src[2]))
        break;
    }

  if(m >= 12)
    return src;

  log_date[0] = src[7];
  log_date[1] = src[8];
  log_date[2] = src[9];
  log_date[3] = src[10];
  log_date[4] = '-';
  log_date[5] = (char)('0' + ((m + 1) / 10));
  log_date[6] = (char)('0' + ((m + 1) % 10));
  log_date[7] = '-';
  log_date[8] = (src[4] == ' ') ? '0' : src[4];
  log_date[9] = src[5];
  log_date[10] = '\0';

  return log_date;
}

const char *
log_level_name(void)
{
  /*
   * The level announced by the boot line is the one the macros actually
   * enforce, never a hardwired value: a boot line advertising a level that is
   * not applied would be a lie.
   */
  if(((int32)(LOG_LEVEL) >= 0) && ((int32)(LOG_LEVEL) < LOG_LVL_COUNT))
    return log_lvl_name[LOG_LEVEL];

  return "????";
}

/*
 * ---------------------------------------------------------------------------
 * Fatal error screen -- the second output of this layer.
 *
 * A player has no serial console. Without a picture, a failed load is a black
 * screen with no recourse, so every fatal stop paints what it also traces.
 *
 * Nothing here goes through the LOG_* macros: they compile away when
 * instrumentation is switched off, and the build a player runs is exactly the
 * one where the message matters most. The composition helpers above are plain
 * functions and remain available in every build.
 * ---------------------------------------------------------------------------
 */

/*
 * Injected by log_bind_screen. Zero means no display exists yet, which is a
 * reachable state: the screen init is itself something that can fail.
 */
static Item log_fatal_bitmap = 0;
static Item log_fatal_screen = 0;

/*
 * A red ground, so that the error screen is told apart from a normal one at a
 * glance and from across a room, and an opaque one, so that whatever was on
 * screen before cannot show through and be read as part of the message.
 */
#define LOG_FATAL_BACK_COLOR MakeRGB15(14,0,0)
#define LOG_FATAL_TEXT_COLOR MakeRGB15(31,31,31)

/*
 * Left margin, first baseline and step between baselines, in pixels. The
 * message starts above the middle of the shortest raster the console can
 * present, so it stays visible whether the host runs 240 or 288 lines.
 *
 * The margin costs three of the forty columns a 320 pixel raster offers at 8
 * pixels per glyph, which is where the 37 character budget documented in
 * log.h comes from.
 */
#define LOG_FATAL_X       24
#define LOG_FATAL_Y       72
#define LOG_FATAL_LEADING 16

/*
 * Ground covered when the bitmap cannot be measured. The NTSC raster is the
 * conservative choice: too small only leaves a border of the previous picture,
 * whereas too large would have FillRect write past the bitmap.
 */
#define LOG_FATAL_FALLBACK_WIDTH  320
#define LOG_FATAL_FALLBACK_HEIGHT 240

static const char log_fatal_title[] = SMS3DO_NAME " fatal error";

/*
 * Renders the code as Ennn. Three digits by construction: the registry in
 * log.h spaces subsystems a hundred codes apart and stops well short of a
 * thousand. Out-of-range values are clamped rather than truncated, so a wrong
 * code never reads as a valid one belonging to another subsystem.
 */
static void
log_fatal_code_text(int32 code,
                    char *out)
{
  int32 i;

  if(code < 0)
    code = 0;
  if(code > 999)
    code = 999;

  out[0] = 'E';
  for(i = 3; i >= 1; i--)
    {
      out[i] = (char)('0' + (int32)(code % 10));
      code /= 10;
    }
  out[4] = '\0';
}

static void
log_fatal_paint(const char *code_text,
                const char *line1,
                const char *line2)
{
  GrafCon gc;
  Rect rect;
  Bitmap *bm;
  Err err;
  Err first_err;
  int32 y;

  first_err = 0;

  /*
   * The bitmap is measured rather than assumed: a PAL raster is not as tall as
   * an NTSC one, and a ground that stops short would leave the previous
   * picture framing the message.
   */
  rect.rect_XLeft = 0;
  rect.rect_YTop = 0;
  rect.rect_XRight = (Coord)LOG_FATAL_FALLBACK_WIDTH;
  rect.rect_YBottom = (Coord)LOG_FATAL_FALLBACK_HEIGHT;

  bm = (Bitmap *)LookupItem(log_fatal_bitmap);
  if((bm != NULL) && (bm->bm_Width > 0) && (bm->bm_Height > 0))
    {
      rect.rect_XRight = (Coord)bm->bm_Width;
      rect.rect_YBottom = (Coord)bm->bm_Height;
    }

  SetFGPen(&gc,LOG_FATAL_BACK_COLOR);
  err = FillRect(log_fatal_bitmap,&gc,&rect);
  if((err < 0) && (first_err == 0))
    first_err = err;

  SetFGPen(&gc,LOG_FATAL_TEXT_COLOR);

  y = LOG_FATAL_Y;

  MoveTo(&gc,(Coord)LOG_FATAL_X,(Coord)y);
  err = DrawText8(&gc,log_fatal_bitmap,(const uint8 *)log_fatal_title);
  if((err < 0) && (first_err == 0))
    first_err = err;

  y += LOG_FATAL_LEADING * 2;
  MoveTo(&gc,(Coord)LOG_FATAL_X,(Coord)y);
  err = DrawText8(&gc,log_fatal_bitmap,(const uint8 *)code_text);
  if((err < 0) && (first_err == 0))
    first_err = err;

  if(line1 != NULL)
    {
      y += LOG_FATAL_LEADING;
      MoveTo(&gc,(Coord)LOG_FATAL_X,(Coord)y);
      err = DrawText8(&gc,log_fatal_bitmap,(const uint8 *)line1);
      if((err < 0) && (first_err == 0))
        first_err = err;
    }

  if(line2 != NULL)
    {
      y += LOG_FATAL_LEADING;
      MoveTo(&gc,(Coord)LOG_FATAL_X,(Coord)y);
      err = DrawText8(&gc,log_fatal_bitmap,(const uint8 *)line2);
      if((err < 0) && (first_err == 0))
        first_err = err;
    }

  /*
   * Presenting the screen is what makes the paint visible: without it the work
   * above stays in an unpresented buffer and the console shows the previous
   * picture, which reads as the message never having been produced at all.
   */
  if(log_fatal_screen != 0)
    {
      err = DisplayScreen(log_fatal_screen,0);
      if((err < 0) && (first_err == 0))
        first_err = err;
    }

  /*
   * A drawing call that failed cannot report itself on screen, so it reports
   * itself in the trace. Staying silent here would leave a blank console with
   * no explanation anywhere.
   */
  if(first_err < 0)
    {
      log_begin(LOG_CAT_BOOT,LOG_LVL_ERR);
      log_printf("fatal screen paint failed err=%ld",(long)first_err);
    }
}

void
log_bind_screen(Item bitmap,
                Item screen)
{
  log_fatal_bitmap = bitmap;
  log_fatal_screen = screen;
}

void
log_fatal(int32       cat,
          int32       code,
          const char *line1,
          const char *line2)
{
  char code_text[5];

  log_fatal_code_text(code,code_text);

  log_begin(cat,LOG_LVL_ERR);
  log_printf("fatal %s %s%s%s",
             code_text,
             (line1 != NULL) ? line1 : "",
             (line2 != NULL) ? " -- " : "",
             (line2 != NULL) ? line2 : "");

  if(log_fatal_bitmap == 0)
    {
      /*
       * Saying plainly that no screen existed matters: the missing picture is
       * then read as a consequence of an earlier failure rather than as the
       * failure itself, which is where a diagnosis would otherwise go wrong.
       */
      log_begin(cat,LOG_LVL_ERR);
      log_printf("fatal screen unavailable, message traced but not painted");
    }
  else
    {
      log_fatal_paint(code_text,line1,line2);
    }

  /*
   * Never return, and never exit either. A task that hands control back to the
   * shell takes the picture off the screen with it, which would leave the
   * player exactly where this whole path exists to avoid.
   */
  for(;;)
    {
    }
}
