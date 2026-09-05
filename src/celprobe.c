#include "celprobe.h"

#if SMS_CEL_PROBE

#include "sys.h"
#include "log.h"
#include "vdp.h"
#include "celutils.h"

/*
 * The guard common.h could not carry, for the reason memprobe.c gives:
 * every figure this probe takes leaves through one LOG_INFO, and the level
 * names belong to log.h.
 */
#if (LOG_LVL_INFO) > (LOG_LEVEL)
#error "SMS_CEL_PROBE needs LOG_LEVEL at LOG_LVL_INFO or above: its figures are INFO lines"
#endif

/*
 * ---------------------------------------------------------------------------
 * The picture, and the two pages.
 *
 * The test pattern is the whole name table of the emulated machine, 32 by
 * 28 tiles of 8 pixels, one byte a pixel: 256 by 224, of which the screen
 * shows 192 lines (docs/sms_gg/SMSOfficialDocs.md, the grid the epic reads).
 * It is drawn by the same coded cel of eight bits the render uses today
 * (src/vdp.c, the preamble words), and every cel of every list below reads
 * from it -- sprites, priority tiles and the 896 tiles of the second
 * variant are windows of eight pixels into this one picture, with the
 * picture's own row pitch. The engine fetches two words a row whatever the
 * source (docs/3do/3DO_Development_Notes.md:87-89), and its cost follows the
 * size of the data (:77), so a window costs what a separate pattern would
 * and no second pattern is needed.
 *
 * Memory is taken in pages of 64 kilobytes because that is the grain the
 * allocator was measured to spend in (a 12 kilobyte request took 64). The
 * first page holds the picture (57 344 bytes) and, behind it, the identity
 * palette of the cels. The second holds the cel lists, and is REUSED by
 * phase: first the 130 cels of the list cases and the 32 window cels of the
 * band cases, then the 896 tile cels of the second variant, then the
 * display list. The three never draw together, and 896 cels alone are
 * 60 928 bytes of a 65 536 byte page.
 * ---------------------------------------------------------------------------
 */
/*
 * Overridable for one reader only: the host check includes this file, and
 * its cel structure carries wider pointers than the console's, so 896 of
 * them outgrow a console page there and nowhere else.
 */
#ifndef CELPROBE_PAGE_BYTES
#define CELPROBE_PAGE_BYTES    65536UL
#endif

#define CELPROBE_PIC_W         VDP_PIX_WIDTH
#define CELPROBE_PIC_H         VDP_NT_LINES
#define CELPROBE_VIS_H         VDP_ACTIVE_LINES
#define CELPROBE_PIC_BYTES     (CELPROBE_PIC_W * CELPROBE_PIC_H)
#define CELPROBE_STRIDE_WORDS  (CELPROBE_PIC_W / 4UL)
#define CELPROBE_PLUT_ENTRIES  32UL
#define CELPROBE_PLUT_OFFSET   CELPROBE_PIC_BYTES

#if (CELPROBE_PLUT_OFFSET + (CELPROBE_PLUT_ENTRIES * 2UL)) > CELPROBE_PAGE_BYTES
#error "the picture and its palette no longer fit one page"
#endif

#if (CELPROBE_PIC_W % 4UL) != 0UL
#error "a row of the picture must be a whole number of words"
#endif

/*
 * The test pattern: 8 columns by 4 rows of cells 32 by 48, numbered 0 to 31
 * row-major, each cell solid in its own number but for its last eight lines,
 * a one-pixel checkerboard of the number and the number sixteen away -- the
 * averaging of the display, if it is on, blends those two into a colour that
 * is neither, which is what makes its state readable from the screen. The
 * 32 lines below the visible picture are a checkerboard of 0 and 31.
 */
#define CELPROBE_CELL_W        32UL
#define CELPROBE_CELL_H        48UL
#define CELPROBE_CELL_COLS     8UL
#define CELPROBE_CHECKER_ROWS  8UL

#if (CELPROBE_CELL_W * CELPROBE_CELL_COLS) != CELPROBE_PIC_W
#error "the cells no longer span the picture"
#endif
#if (CELPROBE_CELL_H * 4UL) != CELPROBE_VIS_H
#error "four rows of cells no longer fill the visible picture"
#endif

/*
 * The scroll the two decor windows meet at, in pixels: a multiple of four,
 * because a source pointer is a word address. The first window reads from
 * that column to the right edge and lands at the left of the screen; the
 * second reads the columns before it and lands after the first. Together
 * they read each visible pixel once, which is the whole point of windows
 * over a second copy.
 */
#define CELPROBE_SCROLL        64UL
#define CELPROBE_WIN_A_W       (CELPROBE_PIC_W - CELPROBE_SCROLL)

#if (CELPROBE_SCROLL % 4UL) != 0UL
#error "the scroll must be a whole number of words"
#endif

/* The small cels: 64 sprites of 8 by 16, 64 priority tiles of 8 by 8. */
#define CELPROBE_SPRITES       VDP_SPR_COUNT
#define CELPROBE_SPR_W         8UL
#define CELPROBE_SPR_H         16UL
#define CELPROBE_PRIO          64UL
#define CELPROBE_TILE          8UL
#define CELPROBE_SMALL         (CELPROBE_SPRITES + CELPROBE_PRIO)

/* The rows the small cels are laid on: eight of them over the 192 lines. */
#define CELPROBE_SMALL_PITCH   (CELPROBE_VIS_H / 8UL)

#if ((CELPROBE_SPRITES / CELPROBE_CELL_COLS) * CELPROBE_SMALL_PITCH) > CELPROBE_VIS_H
#error "the sprite rows no longer fit the visible picture"
#endif
#if (4UL + CELPROBE_SPR_H) > CELPROBE_SMALL_PITCH
#error "a sprite no longer fits its row of the screen"
#endif
#if (4UL + CELPROBE_SPR_H) > (CELPROBE_CELL_H - CELPROBE_CHECKER_ROWS)
#error "a sprite window no longer fits the solid part of a cell"
#endif
#if (12UL + CELPROBE_TILE) > (CELPROBE_CELL_H - CELPROBE_CHECKER_ROWS)
#error "a priority tile window no longer fits the solid part of a cell"
#endif

/* The second variant: one cel a tile, all 28 rows of the table, or the 24
   visible ones for a figure read against the same pixels as the first. */
#define CELPROBE_TILE_COLS     (CELPROBE_PIC_W / CELPROBE_TILE)
#define CELPROBE_TILE_ROWS     (CELPROBE_PIC_H / CELPROBE_TILE)
#define CELPROBE_TILE_ROWS_VIS (CELPROBE_VIS_H / CELPROBE_TILE)
#define CELPROBE_TILES_ALL     (CELPROBE_TILE_COLS * CELPROBE_TILE_ROWS)

/* The band cases: the decor in 1, 2, 4, 8 and 16 bands of rows. */
#define CELPROBE_BANDS_MAX     16UL
#define CELPROBE_WINDOWS       (2UL * CELPROBE_BANDS_MAX)
#define CELPROBE_LIST_CELS     (CELPROBE_WINDOWS + CELPROBE_SMALL)

#if (CELPROBE_VIS_H % CELPROBE_BANDS_MAX) != 0UL
#error "the visible picture must split into whole bands"
#endif

/* Frames timed per case, and the window past which the clock has lied. */
#define CELPROBE_FRAMES        30UL
#define CELPROBE_USEC_MAX      1000000UL

/* Fields a colour state is held on the screen for: five seconds at 60. */
#define CELPROBE_HOLD_FIELDS   300UL

/*
 * The display list. One entry a line, forty words each: a DMA control
 * word, the address of the line, the address of the line before, the link
 * to the next entry, a display control word, thirty-two colour entries, a
 * background entry and two words of padding (include/3do/form3do.h:238-250).
 * The DMA count names the words after the four of control, less the two of
 * padding, which the list is padded with but which are not counted
 * (docs/3do/3do_portfolio_2.5.md:9089). The second colour table starts at
 * the split line.
 */
#define CELPROBE_VDL_WORDS     40UL
#define CELPROBE_VDL_BYTES     (CELPROBE_VDL_WORDS * 4UL)
/*
 * Thirty-four and not thirty-six: the header's own arithmetic (words in the
 * entry less the four of control, include/3do/form3do.h:240) would give
 * thirty-six with the two words of padding, and the folio's page says the
 * padding is NOT counted (docs/3do/3do_portfolio_2.5.md:9089). The folio
 * proofs the list; a count it refuses is traced with its code.
 */
#define CELPROBE_VDL_DMA_WORDS 34UL
#define CELPROBE_VDL_CLUT_AT   5UL
#define CELPROBE_VDL_BG_AT     37UL
#define CELPROBE_VDL_SPLIT     96UL
#define CELPROBE_VDL_ROTATE    16UL

/*
 * The budget the verdict is read against, and the worst band count a game
 * was seen to need: a minute of a real cartridge on the host runner, 3 600
 * frames, showed sprite patterns rewritten while visible on 41 percent of
 * the frames, one extra band on most of those and up to seven on a few.
 */
#define CELPROBE_BUDGET_US     8000UL
#define CELPROBE_BANDS_WORST   7UL

/* The colour code of a window: a decor window paints its colour 0. */
#define CELPROBE_PIXC_OPAQUE   0x1F001F00UL

static uint8  *celprobe_pic  = NULL;
static uint16 *celprobe_plut = NULL;
static uint8  *celprobe_list = NULL;

/* Where the visible picture sits in the raster, as the render centres it. */
static int32 celprobe_px = 0;
static int32 celprobe_py = 0;

/*
 * The thirty-two colours the table is detoured to, as red, green and blue
 * of eight bits each in one word, the order the colour entry takes them
 * in. Chosen to be told apart and NOT to form a ramp: a ramp is what the
 * linear table already shows, and would prove nothing about where the
 * colours come from.
 */
static const uint32 celprobe_colour[CELPROBE_PLUT_ENTRIES] =
{
  0x000000UL, 0xFFFFFFUL, 0xFF0000UL, 0x00FF00UL,
  0x0000FFUL, 0xFFFF00UL, 0xFF00FFUL, 0x00FFFFUL,
  0xFF8000UL, 0x8000FFUL, 0xFF80C0UL, 0x80FF00UL,
  0x008080UL, 0x000080UL, 0x800000UL, 0x808000UL,
  0x808080UL, 0x80C0FFUL, 0x804000UL, 0x80FFC0UL,
  0xFFC000UL, 0xC000FFUL, 0xFF8080UL, 0x008000UL,
  0x400080UL, 0x00FFC0UL, 0xFF6040UL, 0xC0A0FFUL,
  0x404040UL, 0xC0C0C0UL, 0xC0FF40UL, 0xC00040UL
};

/* One colour entry of the table: the index, then the word's three bytes. */
#define CELPROBE_ENTRY(index, rgb) \
  MakeCLUTColorEntry((index),((rgb) >> 16) & 0xFFUL,((rgb) >> 8) & 0xFFUL,(rgb) & 0xFFUL)

/* One timed case: the least, mean and most of the frames kept. */
typedef struct
{
  uint32 min;
  uint32 avg;
  uint32 max;
  uint32 kept;
  uint32 dropped;
} celprobe_fig_t;

/*
 * ---------------------------------------------------------------------------
 * The pattern.
 * ---------------------------------------------------------------------------
 */
static uint8
celprobe_pattern_pixel(uint32 x,
                       uint32 y)
{
  uint32 cell;
  uint32 checker;

  checker = (x ^ y) & 1UL;

  if(y >= CELPROBE_VIS_H)
    return (uint8)(checker ? (CELPROBE_PLUT_ENTRIES - 1UL) : 0UL);

  cell = ((y / CELPROBE_CELL_H) * CELPROBE_CELL_COLS) + (x / CELPROBE_CELL_W);

  if((y % CELPROBE_CELL_H) >= (CELPROBE_CELL_H - CELPROBE_CHECKER_ROWS))
    return (uint8)(checker ? (cell ^ (CELPROBE_PLUT_ENTRIES / 2UL)) : cell);

  return (uint8)cell;
}

static void
celprobe_fill_pattern(uint8  *pic,
                      uint16 *plut)
{
  uint32 x;
  uint32 y;

  for(y = 0UL; y < CELPROBE_PIC_H; y++)
    for(x = 0UL; x < CELPROBE_PIC_W; x++)
      pic[(y * CELPROBE_PIC_W) + x] = celprobe_pattern_pixel(x,y);

  /*
   * The identity palette: index n becomes the colour whose three components
   * are all n, so that each component of the pixel the engine writes is n,
   * and each is an index into the screen's colour table
   * (docs/3do/3do_portfolio_2.5.md:8991). Bit 15 stays clear: a pixel with
   * it set may bypass the table (include/3do/hardware.h:77), and this probe
   * exists to go through it.
   */
  for(x = 0UL; x < CELPROBE_PLUT_ENTRIES; x++)
    plut[x] = (uint16)MakeRGB15(x,x,x);
}

/*
 * ---------------------------------------------------------------------------
 * The cels.
 *
 * Every cel is filled by hand, from the flag set the library documents for
 * a cel it creates (docs/3do/3do_portfolio_2.5.md:5444-5447) and the two
 * preamble words the render computes for its own cel -- a calculation the
 * console showed equal to the library's, word for word, at eight bits
 * (src/vdp.c, the preamble arbiter). The library is not asked for 896
 * cels: each would be an allocation of its own, outside the budget the
 * boot prints.
 * ---------------------------------------------------------------------------
 */
static uint32
celprobe_pre0(uint32 height)
{
  return ((height - PRE0_VCNT_PREFETCH) << PRE0_VCNT_SHIFT) | PRE0_BPP_8;
}

static uint32
celprobe_pre1(uint32 width)
{
  return ((CELPROBE_STRIDE_WORDS - PRE1_WOFFSET_PREFETCH)
          << PRE1_WOFFSET10_SHIFT)
       | PRE1_TLLSB_PDC0
       | (width - PRE1_TLHPCNT_PREFETCH);
}

/*
 * One cel over a window of the picture. The source is the byte of the
 * window's top-left pixel, and must be a word address: the caller's
 * offsets are all multiples of four and the guard at the head of this file
 * keeps the pitch one. The extra flags are the decor's background and
 * palette-load bits, or nothing.
 */
static void
celprobe_cel(CCB         *c,
             const uint8 *src,
             uint32       width,
             uint32       height,
             int32        x,
             int32        y,
             uint32       extra)
{
  c->ccb_Flags = CCB_LAST | CCB_NPABS | CCB_SPABS | CCB_PPABS | CCB_LDSIZE
               | CCB_LDPRS | CCB_LDPPMP | CCB_CCBPRE | CCB_YOXY | CCB_USEAV
               | CCB_NOBLK | CCB_ACE | CCB_ACW | CCB_ACCW | extra;
  c->ccb_NextPtr = NULL;
  c->ccb_SourcePtr = (CelData *)src;
  c->ccb_PLUTPtr = celprobe_plut;
  c->ccb_XPos = x << 16;
  c->ccb_YPos = y << 16;
  c->ccb_HDX = 1L << 20;
  c->ccb_HDY = 0;
  c->ccb_VDX = 0;
  c->ccb_VDY = 1L << 16;
  c->ccb_HDDX = 0;
  c->ccb_HDDY = 0;
  c->ccb_PIXC = CELPROBE_PIXC_OPAQUE;
  c->ccb_PRE0 = celprobe_pre0(height);
  c->ccb_PRE1 = celprobe_pre1(width);
  c->ccb_Width = (int32)width;
  c->ccb_Height = (int32)height;
}

/* Chains one cel after another: the first is no longer the last. */
static void
celprobe_link(CCB *prev,
              CCB *next)
{
  prev->ccb_Flags &= ~CCB_LAST;
  prev->ccb_NextPtr = next;
}

/*
 * The decor of the rows r0 to r1, as two windows of the picture meeting at
 * the scroll: the first reads from the scroll column to the right edge, the
 * second the columns before it, and they land side by side. The first cel
 * paints its colour 0 and loads the palette; the second paints its colour
 * 0 too and keeps the palette loaded. Two cels are written at the arena,
 * and the head is returned.
 */
static CCB *
celprobe_build_decor(CCB    *arena,
                     uint32  r0,
                     uint32  r1)
{
  const uint8 *base;

  base = celprobe_pic + (r0 * CELPROBE_PIC_W);

  celprobe_cel(&arena[0],base + CELPROBE_SCROLL,CELPROBE_WIN_A_W,r1 - r0,
               celprobe_px,celprobe_py + (int32)r0,CCB_BGND | CCB_LDPLUT);
  celprobe_cel(&arena[1],base,CELPROBE_SCROLL,r1 - r0,
               celprobe_px + (int32)CELPROBE_WIN_A_W,celprobe_py + (int32)r0,
               CCB_BGND);
  celprobe_link(&arena[0],&arena[1]);

  return &arena[0];
}

/*
 * The 64 sprites and the 64 priority tiles, each a window of the picture,
 * laid on a grid of eight by eight over the visible area so that they
 * overlap nothing of their own kind. Neither paints its colour 0 -- that
 * is what a sprite is -- and neither reloads the palette. The count asked
 * for is written from the arena as one chain, and the head is returned, or
 * NULL for a count of zero.
 */
static CCB *
celprobe_build_small(CCB    *arena,
                     uint32  count)
{
  uint32 i;
  uint32 col;
  uint32 row;
  uint32 cell;
  const uint8 *src;

  if(count > CELPROBE_SMALL)
    count = CELPROBE_SMALL;

  for(i = 0UL; i < count; i++)
    {
      /*
       * Landing: eight rows of eight on the visible picture, 24 lines a
       * row. Source: the solid part of a numbered cell, never cell 0 -- a
       * window of zeros would be wholly transparent, and a cel that writes
       * nothing may cost less than one that writes, which would bias the
       * cost of a cel downward. Cells 1 to 31 in turn; every source column
       * is a word address.
       */
      if(i < CELPROBE_SPRITES)
        {
          col = i % CELPROBE_CELL_COLS;
          row = i / CELPROBE_CELL_COLS;
          cell = (i % (CELPROBE_PLUT_ENTRIES - 1UL)) + 1UL;
          src = celprobe_pic
              + ((((cell / CELPROBE_CELL_COLS) * CELPROBE_CELL_H) + 4UL) * CELPROBE_PIC_W)
              + ((cell % CELPROBE_CELL_COLS) * CELPROBE_CELL_W) + 8UL;
          celprobe_cel(&arena[i],src,CELPROBE_SPR_W,CELPROBE_SPR_H,
                       celprobe_px + (int32)((col * CELPROBE_CELL_W) + 4UL),
                       celprobe_py + (int32)((row * CELPROBE_SMALL_PITCH) + 4UL),
                       0UL);
        }
      else
        {
          col = (i - CELPROBE_SPRITES) % CELPROBE_CELL_COLS;
          row = (i - CELPROBE_SPRITES) / CELPROBE_CELL_COLS;
          cell = ((i - CELPROBE_SPRITES) % (CELPROBE_PLUT_ENTRIES - 1UL)) + 1UL;
          src = celprobe_pic
              + ((((cell / CELPROBE_CELL_COLS) * CELPROBE_CELL_H) + 12UL) * CELPROBE_PIC_W)
              + ((cell % CELPROBE_CELL_COLS) * CELPROBE_CELL_W) + 16UL;
          celprobe_cel(&arena[i],src,CELPROBE_TILE,CELPROBE_TILE,
                       celprobe_px + (int32)((col * CELPROBE_CELL_W) + 20UL),
                       celprobe_py + (int32)((row * CELPROBE_SMALL_PITCH) + 12UL),
                       0UL);
        }

      if(i > 0UL)
        celprobe_link(&arena[i - 1UL],&arena[i]);
    }

  return (count > 0UL) ? &arena[0] : NULL;
}

/*
 * The list case: the whole visible decor as one band, then the small cels
 * asked for, as one chain from the arena. Returns the head.
 */
static CCB *
celprobe_build_list(CCB    *arena,
                    uint32  small)
{
  CCB *head;
  CCB *tail;

  head = celprobe_build_decor(arena,0UL,CELPROBE_VIS_H);
  tail = celprobe_build_small(&arena[2],small);
  if(tail != NULL)
    celprobe_link(&arena[1],tail);

  return head;
}

/*
 * The band cases: the visible decor cut into equal bands of rows, each
 * band its own two-cel chain from the arena, one head per band written to
 * the caller's array. Every band reads its own rows and no other, so the
 * bands together read what the one-band case reads.
 */
static void
celprobe_build_bands(CCB    *arena,
                     uint32  bands,
                     CCB   **heads)
{
  uint32 k;
  uint32 rows;

  rows = CELPROBE_VIS_H / bands;

  for(k = 0UL; k < bands; k++)
    heads[k] = celprobe_build_decor(&arena[2UL * k],k * rows,(k + 1UL) * rows);
}

/*
 * The second variant: one cel a tile of the table, the rows asked for, all
 * painting their colour 0 since together they ARE the decor, the first
 * loading the palette. The picture is centred for its own height so that
 * no tile lands below the raster and is clipped for free. Returns the head.
 */
static CCB *
celprobe_build_tiles(CCB    *arena,
                     uint32  rows)
{
  uint32 r;
  uint32 c;
  uint32 i;
  int32  py;
  const uint8 *src;

  py = (sys_height() - (int32)(rows * CELPROBE_TILE)) / 2;
  if(py < 0)
    py = 0;

  i = 0UL;
  for(r = 0UL; r < rows; r++)
    for(c = 0UL; c < CELPROBE_TILE_COLS; c++)
      {
        src = celprobe_pic + (r * CELPROBE_TILE * CELPROBE_PIC_W)
            + (c * CELPROBE_TILE);
        celprobe_cel(&arena[i],src,CELPROBE_TILE,CELPROBE_TILE,
                     celprobe_px + (int32)(c * CELPROBE_TILE),
                     py + (int32)(r * CELPROBE_TILE),
                     (i == 0UL) ? (CCB_BGND | CCB_LDPLUT) : CCB_BGND);
        if(i > 0UL)
          celprobe_link(&arena[i - 1UL],&arena[i]);
        i++;
      }

  return &arena[0];
}

/*
 * ---------------------------------------------------------------------------
 * The rule, written before the run.
 *
 * The first variant costs its two windows plus one small cel per priority
 * tile it must draw after the sprites; the second costs its 896 cels and
 * needs no priority cels at all. The second is taken when it costs no more
 * than the first with the 64 priority tiles the sprites case already
 * carries; otherwise the first.
 * ---------------------------------------------------------------------------
 */
static const char *
celprobe_pick(uint32 a_us,
              uint32 b_us)
{
  return (b_us <= a_us) ? "B" : "A";
}

/*
 * The verdict against the budget of the picture: the list with everything
 * in it, the colour table and the list upkeep, drawn in one band, and the
 * same drawn in the worst band count measured on a game. Fits when both
 * fit; at the limit when only the one-band figure does; exceeds otherwise,
 * naming the heaviest of the four posts.
 */
static const char *
celprobe_verdict(uint32 draw_us,
                 uint32 bands_extra_us,
                 uint32 clut_us,
                 uint32 build_us)
{
  uint32 image1;
  uint32 image7;

  image1 = draw_us + clut_us + build_us;
  image7 = image1 + bands_extra_us;

  if(image7 <= CELPROBE_BUDGET_US)
    return "fits";
  if(image1 <= CELPROBE_BUDGET_US)
    return "limit";

  if((draw_us >= bands_extra_us) && (draw_us >= clut_us) && (draw_us >= build_us))
    return "exceeds:draw";
  if((bands_extra_us >= clut_us) && (bands_extra_us >= build_us))
    return "exceeds:bands";
  if(clut_us >= build_us)
    return "exceeds:clut";
  return "exceeds:build";
}

/*
 * ---------------------------------------------------------------------------
 * The timed draw.
 *
 * One window of the clock a frame, around the draw calls of the case and
 * nothing else: the presentation and the field wait come after the second
 * reading. The clock costs some tens of microseconds and, about once in
 * seven windows, returns a sample wrong by whole seconds; a window past a
 * second is the clock having lied and is dropped and counted, never
 * averaged in. A refused draw drops its frame too, and says so once.
 * ---------------------------------------------------------------------------
 */
static void
celprobe_time(celprobe_fig_t *out,
              const char     *name,
              CCB           **heads,
              uint32          calls)
{
  celprobe_fig_t fig;
  uint32 f;
  uint32 k;
  uint32 t0;
  uint32 usec;
  uint32 sum;
  Err    err;
  Err    e;
  int32  said;

  fig.min = 0xFFFFFFFFUL;
  fig.avg = 0UL;
  fig.max = 0UL;
  fig.kept = 0UL;
  fig.dropped = 0UL;
  sum = 0UL;
  said = 0;

  for(f = 0UL; f < CELPROBE_FRAMES; f++)
    {
      err = 0;
      t0 = sys_usec();
      for(k = 0UL; k < calls; k++)
        {
          e = DrawCels(sys_bitmap(),heads[k]);
          if(e < 0)
            err = e;
        }
      usec = sys_usec() - t0;

      (void)sys_display_show();
      (void)sys_vbl_wait(1UL);

      if(err < 0)
        {
          if(!said)
            {
              LOG_ERR(LOG_CAT_PERF,("probe %s draw failed err=%ld",
                                    name,(long)err));
              said = 1;
            }
          fig.dropped++;
          continue;
        }

      if(usec > CELPROBE_USEC_MAX)
        {
          fig.dropped++;
          continue;
        }

      fig.kept++;
      sum += usec;
      if(usec < fig.min)
        fig.min = usec;
      if(usec > fig.max)
        fig.max = usec;
    }

  if(fig.kept > 0UL)
    fig.avg = sum / fig.kept;
  else
    fig.min = 0UL;

  /* Field by field: this compiler names a structure copy as a warning. */
  out->min = fig.min;
  out->avg = fig.avg;
  out->max = fig.max;
  out->kept = fig.kept;
  out->dropped = fig.dropped;
}

/* The four figures a case line prints, in the order it prints them. */
#define CELPROBE_FIG(f) \
  (unsigned long)(f).min,(unsigned long)(f).avg,(unsigned long)(f).max, \
  (unsigned long)(f).dropped

/*
 * One reading that closes a window opened at t0, for the figures taken once
 * rather than over thirty frames: filtered the way a frame is, since the
 * clock lies the same way whatever it times. A lie is said and becomes a
 * zero, which the verdict then reads as a figure that is missing rather
 * than as seconds of cost.
 */
static uint32
celprobe_window(const char *name,
                uint32      t0)
{
  uint32 usec;

  usec = sys_usec() - t0;
  if(usec > CELPROBE_USEC_MAX)
    {
      LOG_WARN(LOG_CAT_PERF,("probe %s window discarded: the clock read %lu us",
                             name,(unsigned long)usec));
      return 0UL;
    }

  return usec;
}

/*
 * ---------------------------------------------------------------------------
 * The colour table.
 * ---------------------------------------------------------------------------
 */

/*
 * Loads the thirty-two colours into the table of every screen, rotated by
 * the given number of entries, and returns what the first screen's call
 * cost -- one call is what a frame would pay. A refused call is traced and
 * the walk goes on: the other screen still gets its table.
 */
static uint32
celprobe_clut_set(uint32 rotate)
{
  uint32 entries[CELPROBE_PLUT_ENTRIES];
  uint32 i;
  int32  s;
  uint32 t0;
  uint32 usec;
  uint32 first;
  Err    err;

  for(i = 0UL; i < CELPROBE_PLUT_ENTRIES; i++)
    entries[i] = CELPROBE_ENTRY(i,celprobe_colour[(i + rotate) % CELPROBE_PLUT_ENTRIES]);

  first = 0UL;
  for(s = 0; s < sys_screen_count(); s++)
    {
      t0 = sys_usec();
      err = SetScreenColors(sys_screen_at(s),entries,(int32)CELPROBE_PLUT_ENTRIES);
      usec = celprobe_window("clut",t0);
      if(err < 0)
        LOG_ERR(LOG_CAT_PERF,("probe clut set refused screen=%ld err=%ld",
                              (long)s,(long)err));
      if((s == 0) && (err >= 0))
        first = usec;
    }

  return first;
}

/* Cuts or restores the display's averaging, both directions, every screen. */
static void
celprobe_avg(int32 on)
{
  int32 s;
  Err   eh;
  Err   ev;

  for(s = 0; s < sys_screen_count(); s++)
    {
      if(on)
        {
          eh = EnableHAVG(sys_screen_at(s));
          ev = EnableVAVG(sys_screen_at(s));
        }
      else
        {
          eh = DisableHAVG(sys_screen_at(s));
          ev = DisableVAVG(sys_screen_at(s));
        }
      if((eh < 0) || (ev < 0))
        LOG_ERR(LOG_CAT_PERF,("probe avg %s refused screen=%ld h=%ld v=%ld",
                              on ? "on" : "off",(long)s,(long)eh,(long)ev));
    }
}

/*
 * ---------------------------------------------------------------------------
 * The display list.
 *
 * Built in the list page, one entry a line of the raster the console
 * built, over the bitmap of the screen about to be presented: the address
 * of each line is asked of the graphics folio (GetPixelAddress,
 * docs/3do/3do_portfolio_2.5.md:9140) rather than assumed from a layout.
 * The lines before the split carry the thirty-two colours, the lines from
 * it the same colours rotated by sixteen; the display control word of the
 * first entry is the system's default with both averaging bits cleared
 * (include/3do/graphics.h:62-71), every later one a no-op. The last entry
 * links to the folio's own post-display list, which is what a full list
 * ends on (include/3do/form3do.h:244).
 *
 * The folio proofs the list on submission and refuses a bad one; a refusal
 * is traced with its code and leaves the rest of the run untouched. The
 * folio's page calls custom lists "discouraged at this time"
 * (docs/3do/3do_portfolio_2.5.md:25048): this is the one place the
 * program submits one, to learn what it costs and whether it is taken.
 * Once submitted the list is copied into system space (:9070), so the page
 * it was built in is free again on return.
 * ---------------------------------------------------------------------------
 */
static Err
celprobe_vdl(void)
{
  uint32 *w;
  uint32 *e;
  uint32  lines;
  uint32  y;
  uint32  i;
  uint32  cur;
  uint32  prev;
  uint32  rotate;
  int32   idx;
  Item    bitmap;
  Item    screen;
  Item    vdl;
  Item    old;
  uint32  t0;
  uint32  usec;

  if(sys_height() <= 0)
    {
      LOG_WARN(LOG_CAT_PERF,("probe vdl skipped: no raster height"));
      return -1;
    }
  lines = (uint32)sys_height();

  if((lines * CELPROBE_VDL_BYTES) > CELPROBE_PAGE_BYTES)
    {
      LOG_WARN(LOG_CAT_PERF,("probe vdl skipped: %lu lines do not fit the page",
                             (unsigned long)lines));
      return -1;
    }

  idx = sys_screen_index();
  bitmap = sys_bitmap_at(idx);
  screen = sys_screen_at(idx);

  w = (uint32 *)celprobe_list;
  prev = 0UL;

  for(y = 0UL; y < lines; y++)
    {
      e = w + (y * CELPROBE_VDL_WORDS);

      cur = (uint32)GetPixelAddress(bitmap,0,(Coord)y);
      if(cur == 0UL)
        {
          LOG_WARN(LOG_CAT_PERF,("probe vdl skipped: no address for line %lu",
                                 (unsigned long)y));
          return -1;
        }

      /*
       * The line count is written as 1 for one line: the field is "scan
       * lines to wait before loading the next entry" in the header's own
       * bit layout (include/3do/form3do.h, DMAControlWord), where the
       * record's comment beside it says "lines in effect minus one". The
       * two readings of the same header disagree; the first is taken, and
       * the screen is the judge -- under the second every entry would hold
       * two lines and the split would land at raster line 192.
       *
       * The last entry carries no relative link and no further video DMA:
       * its next pointer is the folio's own post-display list, absolute
       * (form3do.h:244), which takes the display over from there.
       */
      e[0] = VDL_LDCUR | VDL_LDPREV | VDL_DISPMOD_320
           | (CELPROBE_VDL_DMA_WORDS << VDL_LEN_SHIFT)
           | (1UL << VDL_LINE_SHIFT);
      if(y < (lines - 1UL))
        e[0] |= VDL_ENVIDDMA | VDL_RELSEL;

      e[1] = cur;
      e[2] = (y == 0UL) ? cur : prev;
      prev = cur;

      e[3] = (y < (lines - 1UL))
           ? CELPROBE_VDL_BYTES
           : (uint32)GrafBase->gf_VDLPostDisplay;

      e[4] = (y == 0UL)
           ? (DEFAULT_DISPCTRL & ~(VDL_HINTEN | VDL_VINTEN))
           : VDL_NOP;

      rotate = (y < CELPROBE_VDL_SPLIT) ? 0UL : CELPROBE_VDL_ROTATE;
      for(i = 0UL; i < CELPROBE_PLUT_ENTRIES; i++)
        e[CELPROBE_VDL_CLUT_AT + i] =
          CELPROBE_ENTRY(i,celprobe_colour[(i + rotate) % CELPROBE_PLUT_ENTRIES]);

      e[CELPROBE_VDL_BG_AT] = MakeCLUTBackgroundEntry(0,0,0);
      e[CELPROBE_VDL_BG_AT + 1UL] = VDL_NOP;
      e[CELPROBE_VDL_BG_AT + 2UL] = VDL_NOP;
    }

  t0 = sys_usec();
  vdl = SubmitVDL((VDLEntry *)w,(int32)(lines * CELPROBE_VDL_WORDS),VDLTYPE_FULL);
  usec = celprobe_window("vdl",t0);

  if(vdl < 0)
    {
      LOG_WARN(LOG_CAT_PERF,("probe vdl refused err=%ld",(long)vdl));
      return (Err)vdl;
    }

  LOG_INFO(LOG_CAT_PERF,("probe vdl submit us=%lu lines=%lu split=%lu",
                         (unsigned long)usec,
                         (unsigned long)lines,
                         (unsigned long)CELPROBE_VDL_SPLIT));

  old = SetVDL(screen,vdl);
  if(old < 0)
    {
      LOG_WARN(LOG_CAT_PERF,("probe vdl set refused err=%ld",(long)old));
      return (Err)old;
    }

  LOG_INFO(LOG_CAT_PERF,("probe vdl set screen=%ld old=%ld",
                         (long)idx,(long)old));

  /* The screen the list was bound to is the one about to be presented. */
  (void)sys_display_show();

  return 0;
}

/*
 * ---------------------------------------------------------------------------
 * Install and measure.
 * ---------------------------------------------------------------------------
 */
Err
celprobe_install(void)
{
  /*
   * The arena must hold the largest list. sizeof is not a preprocessor
   * quantity, so the check is made here, once, before any page is taken.
   */
  if((CELPROBE_TILES_ALL * sizeof(CCB)) > CELPROBE_PAGE_BYTES)
    {
      LOG_ERR(LOG_CAT_PERF,("cel probe absent: %lu cels do not fit the list page",
                            (unsigned long)CELPROBE_TILES_ALL));
      return -1;
    }
  if((CELPROBE_LIST_CELS * sizeof(CCB)) > CELPROBE_PAGE_BYTES)
    {
      LOG_ERR(LOG_CAT_PERF,("cel probe absent: the list cels do not fit the page"));
      return -1;
    }

  celprobe_pic = (uint8 *)sys_alloc("probe_decor",
                                    (int32)CELPROBE_PAGE_BYTES,
                                    MEMTYPE_DRAM | MEMTYPE_FILL);
  if(celprobe_pic == NULL)
    {
      LOG_ERR(LOG_CAT_PERF,("cel probe absent: no decor page"));
      return -1;
    }

  celprobe_list = (uint8 *)sys_alloc("probe_list",
                                     (int32)CELPROBE_PAGE_BYTES,
                                     MEMTYPE_DRAM | MEMTYPE_FILL);
  if(celprobe_list == NULL)
    {
      LOG_ERR(LOG_CAT_PERF,("cel probe absent: no list page"));
      LOG_WARN(LOG_CAT_PERF,("cel probe: the decor page stays taken and counted, nothing frees it"));
      celprobe_pic = NULL;
      return -1;
    }

  celprobe_plut = (uint16 *)(celprobe_pic + CELPROBE_PLUT_OFFSET);
  celprobe_fill_pattern(celprobe_pic,celprobe_plut);

  celprobe_px = (sys_width() - (int32)CELPROBE_PIC_W) / 2;
  celprobe_py = (sys_height() - (int32)CELPROBE_VIS_H) / 2;
  if(celprobe_px < 0)
    celprobe_px = 0;
  if(celprobe_py < 0)
    celprobe_py = 0;

  LOG_INFO(LOG_CAT_SYS,("cel probe on (test pattern, no game)"));

  return 0;
}

/*
 * The figures derived from the case means, by the rule written before the
 * run. Each difference is clamped at zero: a list that timed under a
 * shorter one has measured noise, and a zero says so where a wrapped
 * unsigned would read as an enormous cost.
 *
 *   cel_us     one small cel, sprites and tiles alike: (list130 - list2) / 128
 *   sprite_us  one sprite of 8 by 16: (list66 - list2) / 64
 *   tile_us    one tile of 8 by 8: (list130 - list66) / 64 -- for the reader;
 *              the rule below is written with cel_us
 *   call_us    one more draw call: (bands16 - bands1) / 15. Each band also
 *              brings two more window cels, whose own overhead is in this
 *              figure; it is the cost of one more band as a band is built
 *   a_us       the first variant with 64 priority tiles: list2 + 64 * cel_us
 *   b_us       the second variant: tiles896
 *   image1     the whole list in one band, plus the table and the upkeep
 *   image7     the same in the worst band count seen
 */
typedef struct
{
  uint32 cel_us;
  uint32 sprite_us;
  uint32 tile_us;
  uint32 call_us;
  uint32 a_us;
  uint32 b_us;
  uint32 image1;
  uint32 image7;
} celprobe_derived_t;

static uint32
celprobe_over(uint32 a,
              uint32 b,
              uint32 by)
{
  return (a > b) ? ((a - b) / by) : 0UL;
}

static void
celprobe_derive(celprobe_derived_t *d,
                uint32              list0,
                uint32              list64,
                uint32              list128,
                uint32              bands1,
                uint32              bands16,
                uint32              tiles896,
                uint32              clut_us,
                uint32              build_us)
{
  d->cel_us = celprobe_over(list128,list0,CELPROBE_SMALL);
  d->sprite_us = celprobe_over(list64,list0,CELPROBE_SPRITES);
  d->tile_us = celprobe_over(list128,list64,CELPROBE_PRIO);
  d->call_us = celprobe_over(bands16,bands1,CELPROBE_BANDS_MAX - 1UL);
  d->a_us = list0 + (CELPROBE_PRIO * d->cel_us);
  d->b_us = tiles896;
  d->image1 = list128 + clut_us + build_us;
  d->image7 = d->image1 + ((CELPROBE_BANDS_WORST - 1UL) * d->call_us);
}

/*
 * Draws a list on every screen of the rotation, untimed: what the screens
 * hold when the drawing stops is what the eye and the capture will read.
 */
static void
celprobe_show(CCB *head)
{
  int32 s;

  for(s = 0; s < sys_screen_count(); s++)
    {
      (void)DrawCels(sys_bitmap(),head);
      (void)sys_display_show();
      (void)sys_vbl_wait(1UL);
    }
}

void
celprobe_measure(void)
{
  CCB  *arena;
  CCB  *heads[CELPROBE_BANDS_MAX];
  celprobe_fig_t list0;
  celprobe_fig_t list64;
  celprobe_fig_t list128;
  celprobe_fig_t bands[5];
  celprobe_fig_t tiles896;
  celprobe_fig_t tiles768;
  celprobe_fig_t after;
  celprobe_derived_t d;
  uint32 build130;
  uint32 build896;
  uint32 clut_us;
  uint32 us;
  uint32 t0;
  uint32 b;
  int32  valid;
  int32  bound;
  static const uint32 band_count[5] = { 1UL, 2UL, 4UL, 8UL, 16UL };

  if((celprobe_pic == NULL) || (celprobe_list == NULL))
    {
      LOG_WARN(LOG_CAT_PERF,("cel probe not installed, nothing measured"));
      return;
    }

  arena = (CCB *)celprobe_list;

  /*
   * Said before the first draw: the pattern is about to replace the boot
   * banner, and a viewer who expects a game must have read why.
   */
  LOG_INFO(LOG_CAT_PERF,("probe starting: pattern only, %lu frames a case",
                         (unsigned long)CELPROBE_FRAMES));

  /* Phase one: the list cases, the build of the largest list timed. */
  t0 = sys_usec();
  heads[0] = celprobe_build_list(arena,CELPROBE_SMALL);
  build130 = celprobe_window("build",t0);
  celprobe_time(&list128,"list",heads,1UL);

  heads[0] = celprobe_build_list(arena,CELPROBE_SPRITES);
  celprobe_time(&list64,"list",heads,1UL);

  heads[0] = celprobe_build_list(arena,0UL);
  celprobe_time(&list0,"list",heads,1UL);

  LOG_INFO(LOG_CAT_PERF,("probe list calls=1 cels=%lu us=%lu/%lu/%lu dropped=%lu",
                         2UL,CELPROBE_FIG(list0)));
  LOG_INFO(LOG_CAT_PERF,("probe list calls=1 cels=%lu us=%lu/%lu/%lu dropped=%lu",
                         2UL + CELPROBE_SPRITES,CELPROBE_FIG(list64)));
  LOG_INFO(LOG_CAT_PERF,("probe list calls=1 cels=%lu us=%lu/%lu/%lu dropped=%lu",
                         2UL + CELPROBE_SMALL,CELPROBE_FIG(list128)));

  /* The band cases, on the same arena. */
  for(b = 0UL; b < 5UL; b++)
    {
      celprobe_build_bands(arena,band_count[b],heads);
      celprobe_time(&bands[b],"bands",heads,band_count[b]);
      LOG_INFO(LOG_CAT_PERF,("probe bands=%lu calls=%lu us=%lu/%lu/%lu dropped=%lu",
                             (unsigned long)band_count[b],
                             (unsigned long)band_count[b],
                             CELPROBE_FIG(bands[b])));
    }

  /* Phase two: the second variant, the arena rewritten as tile cels. */
  t0 = sys_usec();
  heads[0] = celprobe_build_tiles(arena,CELPROBE_TILE_ROWS);
  build896 = celprobe_window("build",t0);
  celprobe_time(&tiles896,"tiles896",heads,1UL);
  LOG_INFO(LOG_CAT_PERF,("probe tiles%lu us=%lu/%lu/%lu dropped=%lu",
                         (unsigned long)CELPROBE_TILES_ALL,CELPROBE_FIG(tiles896)));

  heads[0] = celprobe_build_tiles(arena,CELPROBE_TILE_ROWS_VIS);
  celprobe_time(&tiles768,"tiles768",heads,1UL);
  LOG_INFO(LOG_CAT_PERF,("probe tiles%lu us=%lu/%lu/%lu dropped=%lu",
                         (unsigned long)(CELPROBE_TILE_COLS * CELPROBE_TILE_ROWS_VIS),
                         CELPROBE_FIG(tiles768)));

  LOG_INFO(LOG_CAT_PERF,("probe build cels=%lu us=%lu",
                         2UL + CELPROBE_SMALL,(unsigned long)build130));
  LOG_INFO(LOG_CAT_PERF,("probe build cels=%lu us=%lu",
                         (unsigned long)CELPROBE_TILES_ALL,(unsigned long)build896));

  /*
   * The screens are put back to the one-band decor: the tile cases were
   * centred for their own height and left a strip of the taller picture
   * above and below, which is not what the colour proof is read off.
   */
  heads[0] = celprobe_build_list(arena,0UL);
  celprobe_show(heads[0]);

  /*
   * A case that kept no frame has no mean, and a mean of zero fed to the
   * rule would make the verdict read "fits" over nothing measured: the
   * derived lines are still printed, so that what was measured can be
   * read, but the verdict is void and says so.
   */
  valid = (list0.kept > 0UL) && (list64.kept > 0UL) && (list128.kept > 0UL)
       && (bands[0].kept > 0UL) && (bands[4].kept > 0UL) && (tiles896.kept > 0UL);
  if(!valid)
    LOG_WARN(LOG_CAT_PERF,("probe: a case kept no frame, the verdict below is void"));

  /*
   * The pattern has been on both screens since the first case, under the
   * linear table the console booted with: a grey ramp. Held a moment so
   * that the detour below is seen as a change, then the three states of
   * the averaging, each held long enough to be looked at and captured.
   */
  LOG_INFO(LOG_CAT_PERF,("probe clut linear: grey ramp, averaging as booted"));
  (void)sys_vbl_wait(CELPROBE_HOLD_FIELDS / 2UL);

  clut_us = celprobe_clut_set(0UL);
  LOG_INFO(LOG_CAT_PERF,("probe clut set us=%lu avg=default",
                         (unsigned long)clut_us));
  (void)sys_vbl_wait(CELPROBE_HOLD_FIELDS);

  celprobe_avg(0);
  us = celprobe_clut_set(0UL);
  LOG_INFO(LOG_CAT_PERF,("probe clut set us=%lu avg=off",(unsigned long)us));
  (void)sys_vbl_wait(CELPROBE_HOLD_FIELDS);

  celprobe_avg(1);
  us = celprobe_clut_set(0UL);
  LOG_INFO(LOG_CAT_PERF,("probe clut set us=%lu avg=on",(unsigned long)us));
  (void)sys_vbl_wait(CELPROBE_HOLD_FIELDS);

  celprobe_avg(0);

  celprobe_derive(&d,list0.avg,list64.avg,list128.avg,
                  bands[0].avg,bands[4].avg,tiles896.avg,clut_us,build130);

  LOG_INFO(LOG_CAT_PERF,("probe derive cel_us=%lu sprite_us=%lu tile_us=%lu call_us=%lu",
                         (unsigned long)d.cel_us,
                         (unsigned long)d.sprite_us,
                         (unsigned long)d.tile_us,
                         (unsigned long)d.call_us));
  LOG_INFO(LOG_CAT_PERF,("probe decision A=%lu B=%lu pick=%s",
                         (unsigned long)d.a_us,
                         (unsigned long)d.b_us,
                         valid ? celprobe_pick(d.a_us,d.b_us) : "void"));

  /*
   * Phase three: the display list, where the console accepts it. Bound,
   * it charges the display on every line; the largest list is then timed
   * again, on the same arena the list was built in and no longer needs,
   * so that the charge on a draw can be read against the earlier figure.
   */
  bound = (celprobe_vdl() == 0);
  if(bound)
    {
      heads[0] = celprobe_build_list(arena,CELPROBE_SMALL);
      celprobe_time(&after,"list",heads,1UL);
      LOG_INFO(LOG_CAT_PERF,("probe list after vdl calls=1 cels=%lu us=%lu/%lu/%lu dropped=%lu",
                             2UL + CELPROBE_SMALL,CELPROBE_FIG(after)));
    }

  LOG_INFO(LOG_CAT_PERF,
           ("probe verdict image1=%lu image7=%lu bands=%lu budget=%lu %s",
            (unsigned long)d.image1,
            (unsigned long)d.image7,
            (unsigned long)CELPROBE_BANDS_WORST,
            (unsigned long)CELPROBE_BUDGET_US,
            valid ? celprobe_verdict(list128.avg,
                                     (CELPROBE_BANDS_WORST - 1UL) * d.call_us,
                                     clut_us,build130)
                  : "void"));

  /*
   * And held. The cartridge never runs on this build: it would draw over
   * the pattern with a table that is no longer its own. The averaging is
   * toggled and the table reloaded every few seconds so that their effect
   * can be watched as long as one likes -- on the screen the display list
   * was bound to, if it was, the folio edits its own list and what shows
   * is the list's, which is said once here. A pacer that refuses leaves
   * nothing to wait on: the lines would then flood the trace, so the hold
   * is a silent spin instead.
   */
  LOG_INFO(LOG_CAT_SYS,("probe holds the pattern: reboot to leave"));
  if(bound)
    LOG_INFO(LOG_CAT_PERF,("probe hold: the display list is bound to one screen, table and averaging changes may not show on it"));

  for(;;)
    {
      if(sys_vbl_wait(CELPROBE_HOLD_FIELDS) < 0)
        {
          LOG_WARN(LOG_CAT_PERF,("probe hold: no pacer, holding silently"));
          for(;;)
            ;
        }
      celprobe_avg(1);
      us = celprobe_clut_set(0UL);
      LOG_INFO(LOG_CAT_PERF,("probe clut set us=%lu avg=on",(unsigned long)us));
      (void)sys_vbl_wait(CELPROBE_HOLD_FIELDS);
      celprobe_avg(0);
      us = celprobe_clut_set(0UL);
      LOG_INFO(LOG_CAT_PERF,("probe clut set us=%lu avg=off",(unsigned long)us));
    }
}

#endif /* SMS_CEL_PROBE */
