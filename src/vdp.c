#include "vdp.h"
#include "sms.h"
#include "sys.h"
#include "log.h"
#include "cart.h"

/*
 * The library side of the cel, quoted like every include of this
 * directory (the compiler serves its own path before the Makefile's, and
 * in angle brackets another library's headers would be read; see the note
 * at the head of sys.c). It brings CreateCel and the cel structure the
 * fallback below fills by hand.
 */
#include "celutils.h"

/*
 * The video display processor at the stage where it answers, keeps,
 * renders the background plane and owns the way out for pixels. What the
 * ports do is written in vdp.h beside each declaration; this file holds
 * the bodies, the two clocks -- the line, which renders one row of the
 * picture before it counts, and the reporting second -- and the init-time
 * construction of the drawing side: colour table, plane table, coded cel.
 * The draw call belongs to the frame loop.
 */

/*
 * The video memory block, kept across a second init: the allocator refuses
 * everything past the seal, and the one block taken at boot is the block
 * for the whole run. Held in a static and not only in the structure so
 * that a re-init after the seal finds it rather than asks again (precedent:
 * the work RAM of cart.c).
 */
static uint8 *vdp_vram_block = NULL;

/*
 * The index buffers, kept across a second init for the reason the video
 * memory is. A static array of pointers, all NULL until the first init
 * takes the blocks.
 */
static uint8 *vdp_pixels_block[SMS_VDP_BUFFERS];

/*
 * The bit plane table, kept across a second init for the same reason.
 */
static uint8 *vdp_planes_block = NULL;

/*
 * The decoded row cache, kept across a second init for the same reason.
 * One block holds both parts: the words of the entries first, so that the
 * block's own alignment carries them, then the validity byte per row.
 */
static uint32 *vdp_tc_block = NULL;

/*
 * The cel, built once at the first successful init and kept: CreateCel
 * allocates from the system, which the seal forbids later, and one block
 * for the whole run is the rule of every allocation here. When the
 * library refuses, the static below is filled by hand instead -- same
 * fields, same final flags -- and the flag says which way it went so the
 * init line can name it.
 */
static CCB *vdp_cel_block = NULL;
static CCB vdp_ccb_manual;
static int32 vdp_cel_manual = 0;

/*
 * The preamble words as the library left them, read out of the block the
 * moment CreateCel returns and before anything here touches it. Kept in
 * statics because the cel is built once while the init line that prints
 * them beside this file's own calculation is emitted on every init: one
 * debug line then says whether the library and the calculation agree,
 * which is the question a garbled first picture cannot answer by itself.
 * Zero on the manual way, where the calculated pair is the only pair.
 */
static uint32 vdp_pre0_lib = 0;
static uint32 vdp_pre1_lib = 0;

#if VDP_COUNTERS
/*
 * The processor's acceptance count as vdp_report last read it. Kept at
 * file level rather than inside the function so that vdp_init can clear
 * it: the processor's count restarts at zero on a reset, and a previous
 * reading left standing would make the first difference after that reset
 * wrap instead of counting.
 */
static uint32 vdp_irq_seen = 0;

/*
 * Whether the backdrop line of the current report window has been said.
 * File level for the same reason as the count above: the window is opened
 * and closed by vdp_report, and the flag has to outlive the call that
 * raises it.
 */
static uint32 vdp_backdrop_said = 0;
#endif

/*
 * The power-on register file: SMSOfficialDocs.md:948-957, the table this
 * port takes over TotalSMS's after-BIOS values (see vdp_init in vdp.h).
 * Registers 11 to 15 have no value in any source and are never written by
 * a program either; zero.
 */
static const uint8 vdp_reg_power_on[VDP_REG_COUNT] =
{
  0x36, 0xA0, 0xFF, 0xFF, 0xFF, 0xFF, 0xFB, 0x00,
  0x00, 0x00, 0xFF, 0x00, 0x00, 0x00, 0x00, 0x00
};

/*
 * ---------------------------------------------------------------------------
 * The colour table: six bit colour to one entry of the screen's colour
 * table, 64 entries, the index byte left at zero. A colour byte carries
 * two bits per component, red in bits 0-1, green in 2-3, blue in 4-5
 * (SMSOfficialDocs.md:288-306; TotalSMS/src/core/sms_vdp.c:1236-1238),
 * each at one of four levels -- off, one third, two thirds, full
 * (SMSOfficialDocs.md:483-495) -- which on the eight bits of a component
 * of a screen table entry (include/3do/graphics.h:264) are 0, 85, 170 and
 * 255. Filled by a loop at init rather than written as 64 literals:
 * nothing to mistype. Read by the rebuild below and by nothing else.
 * ---------------------------------------------------------------------------
 */
static uint32 vdp_clut_rgb[64];

static const uint8 vdp_level[4] = { 0, 85, 170, 255 };

/*
 * The rebuild of a screen table from a colour memory: one conversion
 * per entry, the index put in the high byte here since the table above
 * has none. Then the display's background entry, the colour it shows for
 * a zero word: a pixel of index 0 leaves the identity palette as 0x0000,
 * so that entry is given the same three components as colour 0 and the
 * pixel shows as colour 0 whichever way the display reads it. Called at
 * init and by vdp_clut_take -- for the table of the picture, and on the
 * list path for the table of each palette segment from its own copy of
 * the colour memory -- and it is the one place the entries are written.
 */
static void
vdp_clut_build(uint32      *dst,
               const uint8 *src)
{
  int32 i;
  uint32 c0;

  for(i = 0; i < VDP_CRAM_SIZE; i++)
    dst[i] = ((uint32)i << 24) | vdp_clut_rgb[src[i] & 0x3FU];

  c0 = vdp_clut_rgb[src[0] & 0x3FU];
  dst[VDP_CRAM_SIZE] = MakeCLUTBackgroundEntry((c0 >> 16) & 0xFFU,
                                               (c0 >> 8) & 0xFFU,
                                               c0 & 0xFFU);
}

#if VDP_PROFILE
/*
 * ---------------------------------------------------------------------------
 * The render broken down into posts, by repetition (vdp.h, VDP_PROFILE).
 *
 * The armed variant, and the whole of this module's part in the
 * breakdown: one word, written by the frame loop through the accessor
 * below and read once per post per line. No clock is read here, no
 * duration is held here, and nothing here decides when a variant changes
 * -- the loop that cuts up a turn owns all three.
 *
 * The control is the value a static starts at, so a run that never arms
 * anything renders exactly as the delivered build does.
 * ---------------------------------------------------------------------------
 */
static uint32 vdp_profile_variant = VDP_PROFILE_CONTROL;

void
vdp_profile_select(uint32 variant)
{
  vdp_profile_variant = (variant < VDP_PROFILE_VARIANTS)
                        ? variant : VDP_PROFILE_CONTROL;
}

uint32
vdp_profile_reps(uint32 post)
{
  if(vdp_profile_variant == VDP_PROFILE_ALL)
    return 2UL;
  if(vdp_profile_variant == post)
    return 2UL;
  return 1UL;
}
#endif


#if !SMS_DECOR_CEL
/*
 * ---------------------------------------------------------------------------
 * The sprites of one line, chosen once. The sixty-four entries of the
 * attribute table are walked in order and the walk stops on a vertical
 * position of 0xD0, the value the hardware reads as "no more sprites"
 * on the 192 line raster (docs/sms_gg/SMSOfficialDocs.md:443-446). The
 * position on screen is the byte plus one, and a position above the
 * height of the name table belongs to a sprite entering from the top, so
 * it is taken as negative (TotalSMS/src/core/sms_vdp.c:1074, :1090-1093).
 * The
 * height covered is eight lines, sixteen when register 1 bit 1 asks for
 * tall sprites, doubled again when bit 0 asks for magnification
 * (sms_vdp.c:302-316; in mode 4 the width is always eight, magnification
 * apart). Eight sprites at most are kept; the ninth to fall on the line
 * raises the overflow bit and ends the walk (SMSOfficialDocs.md:393-397;
 * sms_vdp.c:1105-1117). The X position plays no part here, so a ninth
 * sprite wholly off the picture still raises the bit -- which is the
 * hardware's order, selection then display.
 *
 * Called once per line and never per pixel; what it leaves behind is a
 * count and two small arrays the composition below walks.
 * ---------------------------------------------------------------------------
 */
static void
vdp_select_sprites(uint32 y)
{
  const uint8 *sat;
  const uint8 *reg;
  uint32 i;
  uint32 count;
  uint32 height;
  int32 top;
  int32 line;

  reg = sms.vdp.reg;
  sat = sms.vdp.vram + (((uint32)reg[5] & 0x7EUL) << 7);
  height = (((uint32)reg[1] & 0x02UL) != 0UL) ? 16UL : 8UL;
  height <<= ((uint32)reg[1] & 0x01UL);
  line = (int32)y;
  count = 0;

  for(i = 0; i < VDP_SPR_COUNT; i++)
    {
      if((uint32)sat[i] == VDP_SPR_TERMINATOR)
        break;

      top = (int32)sat[i] + 1;
      if(top > (int32)VDP_SPR_Y_WRAP)
        top -= 256;

      if((line >= top) && (line < (top + (int32)height)))
        {
          if(count == VDP_SPR_MAX_ON_LINE)
            {
              /*
               * The bit is raised whether or not it already stood: a
               * program that never reads the status would otherwise make
               * every overflow after the first invisible. The count is
               * one per line by construction -- this branch ends the
               * walk.
               */
              sms.vdp.spr_overflow = 1;
              VDP_COUNT(spr_ovf);
              break;
            }

          sms.vdp.spr_sel[count] = (uint8)i;
          sms.vdp.spr_top[count] = top;
          count++;
        }
    }

  sms.vdp.spr_count = count;

#if VDP_COUNTERS
  if(count > sms.vdp.cnt_spr_max)
    sms.vdp.cnt_spr_max = count;
  if(((uint32)reg[1] & 0x01UL) != 0UL)
    sms.vdp.cnt_spr_zoom = 1;
#endif
}

/*
 * ---------------------------------------------------------------------------
 * The sprites of one line, laid over the background already composed in
 * the row of the picture. The kept entries are walked in table order,
 * which is what makes the first of them win a shared pixel
 * (TotalSMS/src/core/sms_vdp.c:1206-1213). The two authorised sources
 * disagree here and the disagreement is recorded rather than smoothed
 * over: the document says the higher numbered sprite shows on top
 * (SMSOfficialDocs.md:445-447), the working reference draws the lower one
 * and lets the higher one only collide with it. The reference is the one
 * that runs, so it is the one followed -- and this comment is what stops
 * the next reader from quietly flipping it back to the document.
 *
 * The horizontal position is the byte at base + 128 + 2i, eight to the
 * left when register 0 bit 3 asks for it, and the pattern number the byte
 * after, taken from the second eight kilobytes when register 6 bit 2 is
 * set and forced even when the sprites are tall (sms_vdp.c:1148-1165;
 * SMSOfficialDocs.md:846-852). The address is masked on fourteen bits
 * like every other one this module forms -- and, as it happens, the mask
 * never has anything to trim: the two extreme rows both land on the last
 * four bytes of the block, 511 * 32 + 7 * 4 + 3 without tall sprites and
 * 510 * 32 + 15 * 4 + 3 with them (a tall pattern number is forced even,
 * which is what keeps 511 out of the second case), both 16383. The mask
 * is kept as the module's rule and as the guard on the day one of those
 * two figures moves; the reference does not mask (sms_vdp.c:1165). Sprite
 * colours are the second bank of the palette, so the index written is
 * sixteen plus the pattern's (SMSOfficialDocs.md:383-385).
 *
 * The order of the tests inside the pixel loop is the whole of the
 * semantics and is the reference's (sms_vdp.c:1179-1224): outside the
 * picture or inside a masked left column, nothing at all -- not even a
 * collision; index zero is transparent; a pixel a sprite already took is
 * a collision whether or not anything is drawn there, which is how two
 * sprites hidden behind the background still collide; and only then does
 * the background's priority mask decide whether the pixel is written.
 * The left edge the first test uses is the reference's own
 * (sms_vdp.c:359-360): eight when register 0 bit 5 blanks the left
 * column, zero otherwise. Suppressing a collision there, and for a sprite
 * pushed off an edge, is a limit the reference marks as unfinished
 * (sms_vdp.c:1099) and this port inherits knowingly rather than by
 * accident.
 *
 * Two differences from the reference are deliberate. It reads the
 * horizontal and pattern bytes from the base itself when register 5 bit 0
 * is clear (sms_vdp.c:1064, marked as unfinished there); the document
 * puts that half of the table at base + 128 with no condition, and the
 * two agree at the 0xFF every program writes. And it doubles the height a
 * magnified sprite covers without dividing the pattern row it reads
 * (sms_vdp.c:1174), which walks off the end of the pattern; the row is
 * divided here, to match the doubling it does apply across
 * (sms_vdp.c:1197). No document line describes magnification beyond
 * naming the bit (SMSOfficialDocs.md:1451), so this is the one rule here
 * with a single authority and it is a best effort.
 *
 * One shift per pixel pays for magnification rather than two loops
 * written out, as the background's two flip directions are: a sprite is
 * sixteen pixels at most against the two hundred and fifty-six of a
 * background line.
 * ---------------------------------------------------------------------------
 */
static void
vdp_draw_sprites(uint32 y)
{
  const uint8 *vram;
  const uint8 *reg;
  const uint8 *sat;
  const uint8 *planes;
  const uint8 *tile;
  const uint8 *t0;
  const uint8 *t1;
  const uint8 *t2;
  const uint8 *t3;
  uint8 *line;
  uint8 *prio;
  uint8 *taken;
  uint32 *clear;
  uint32 n;
  uint32 entry;
  uint32 pattern;
  uint32 base;
  uint32 zoom;
  uint32 width;
  uint32 row;
  uint32 idx;
  uint32 x;
  int32 x0;
  int32 xi;
  int32 startx;
#if VDP_COUNTERS
  uint32 col_seen;
#endif

  reg = sms.vdp.reg;
  vram = sms.vdp.vram;
  planes = sms.vdp.planes;
  /*
   * Two origins, and they differ. The pixels are written into the row of
   * the picture, where pixel x is byte x. The mask is read out of the
   * scratch at the origin the background left behind: a fine scroll moves
   * where pixel 0 of the picture sits inside the scratch, and a sprite
   * does not scroll with the background. Every write below is bounded to
   * the picture -- a pixel left of the first drawn column or past the
   * right edge is skipped before anything is touched -- which is what lets
   * the row, with no lead and no tail, take the pixels directly.
   */
  line = sms.vdp.pixels[0] + (y * VDP_PIX_ROW_BYTES);
  prio = VDP_PRIO_BYTES + sms.vdp.line_org;
  taken = (uint8 *)sms.vdp.spr_taken;
  clear = sms.vdp.spr_taken;
  sat = vram + (((uint32)reg[5] & 0x7EUL) << 7);
  zoom = (uint32)reg[1] & 0x01UL;
  width = 8UL << zoom;
  base = (((uint32)reg[6] & 0x04UL) != 0UL) ? 256UL : 0UL;
  startx = (((uint32)reg[0] & 0x20UL) != 0UL) ? 8 : 0;

#if VDP_COUNTERS
  col_seen = 0;
#endif

  for(x = 0; x < (VDP_PIX_WIDTH / 4UL); x++)
    clear[x] = 0;

  for(n = 0; n < sms.vdp.spr_count; n++)
    {
      entry = VDP_SPR_XN_OFFSET + ((uint32)sms.vdp.spr_sel[n] << 1);

      /*
       * A sprite wholly left of the first drawn column is skipped whole
       * (sms_vdp.c:1150-1153). There is no matching test on the right:
       * the position is a byte and the shift only subtracts, so it can
       * never reach the width of the picture, and the pixel loop below
       * leaves on the first pixel past it.
       */
      x0 = (int32)sat[entry];
      if(((uint32)reg[0] & 0x08UL) != 0UL)
        x0 -= 8;
      if((x0 + (int32)width) < startx)
        continue;

      pattern = (uint32)sat[entry + 1UL] + base;
      if(((uint32)reg[1] & 0x02UL) != 0UL)
        pattern &= ~1UL;

      row = ((uint32)((int32)y - sms.vdp.spr_top[n])) >> zoom;
      tile = vram + ((((pattern << 5) + (row << 2))) & VDP_VRAM_MASK);
      t0 = planes + ((uint32)tile[0] << 3);
      t1 = planes + VDP_PLANES_PLANE + ((uint32)tile[1] << 3);
      t2 = planes + (2UL * VDP_PLANES_PLANE) + ((uint32)tile[2] << 3);
      t3 = planes + (3UL * VDP_PLANES_PLANE) + ((uint32)tile[3] << 3);

      for(x = 0; x < width; x++)
        {
          xi = x0 + (int32)x;
          if(xi < startx)
            continue;
          if(xi >= (int32)VDP_PIX_WIDTH)
            break;

          idx = (uint32)t0[x >> zoom] | (uint32)t1[x >> zoom]
              | (uint32)t2[x >> zoom] | (uint32)t3[x >> zoom];
          if(idx == 0UL)
            continue;

          if(taken[xi] != 0)
            {
              /*
               * Raised whether or not it already stood, for the reason
               * the overflow bit is; counted once per line, or a crowded
               * line would report a collision per pixel and the figure
               * would stop meaning anything.
               */
              sms.vdp.spr_collision = 1;
#if VDP_COUNTERS
              if(col_seen == 0UL)
                {
                  col_seen = 1;
                  VDP_COUNT(spr_col);
                }
#endif
              continue;
            }
          taken[xi] = 1;

          /*
           * The background wins where its column carries priority AND
           * its pixel is opaque. The mask holds the first; the second is
           * read off the row: a background index is four bits plus
           * sixteen for the second bank, so its low four bits are zero
           * exactly when it is transparent (vdp.h, VDP_TC_KEY). The row
           * still holds the background here -- a pixel a sprite already
           * wrote is taken, and left above.
           */
          if((prio[xi] != 0) && (((uint32)line[xi] & 15UL) != 0UL))
            continue;

          line[xi] = (uint8)(16UL + idx);
        }
    }
}
#endif /* !SMS_DECOR_CEL */

#if SMS_DECOR_CEL
/*
 * ---------------------------------------------------------------------------
 * The background picture (vdp.h, VDP_DECOR_*): kept in step tile by tile,
 * drawn by windows. Everything from here to the line render is that path;
 * with the switch off none of it is compiled and the processor composes
 * the background line by line, as below.
 * ---------------------------------------------------------------------------
 */

/*
 * The page of the picture and the window blocks, and the page of the
 * sprite sheet and the small cel blocks, kept across a second init for
 * the reason every block above is.
 */
static uint8 *vdp_decor_block = NULL;
static uint8 *vdp_sprite_block = NULL;

/*
 * The chain of the band being built: its first block and its last, so
 * that a new block is chained after the last one of THIS band only. The
 * two arenas are shared by every band of a presentation and a chain must
 * not run from one band into the next.
 */
static CCB *vdp_chain_head = NULL;
static CCB *vdp_chain_tail = NULL;

/* Defined with the sprites below, called by the register setter above them. */
static void vdp_column_fill(void);

/*
 * The four bytes of a lane word tested for opacity, one result byte per
 * lane: an index is opaque when its low four bits are not zero (vdp.h,
 * the guard on VDP_PLANES_COUNT), so fifteen is added to each low nibble
 * and bit 4 of the sum is the answer; a sum of at most thirty carries
 * into no neighbouring lane.
 */
#define VDP_LANE_OPAQUE(e) \
  (((((e) & 0x0F0F0F0FUL) + 0x0F0F0F0FUL) >> 4) & VDP_TC_LANE_ONE)

/* The flags of every small cel of the list; a sprite adds nothing to them. */
#define VDP_SPRITE_BGND 0UL

/*
 * One row of the decoded row cache, decoded first if it does not stand:
 * the same eight bytes the two stroke loops of the older path write, laid
 * as bytes for the reason given there. Its own name for the decode -- the
 * profile bench counts the decodes of each site by name.
 */
static uint32 *
vdp_tc_row(uint32 key)
{
  uint32 *ent;
  const uint8 *tile;
  const uint8 *planes;
  const uint8 *t0;
  const uint8 *t1;
  const uint8 *t2;
  const uint8 *t3;
  uint8 *db;
  uint32 x;

  ent = sms.vdp.tc + (key << 1);
  if(sms.vdp.tc_valid[key] != 0)
    {
      VDP_COUNT(tc_hit);
      return ent;
    }

  planes = sms.vdp.planes;
  tile = sms.vdp.vram + (key << 2);
  t0 = planes + ((uint32)tile[0] << 3);
  t1 = planes + VDP_PLANES_PLANE + ((uint32)tile[1] << 3);
  t2 = planes + (2UL * VDP_PLANES_PLANE) + ((uint32)tile[2] << 3);
  t3 = planes + (3UL * VDP_PLANES_PLANE) + ((uint32)tile[3] << 3);
  db = (uint8 *)ent;
  for(x = 0; x < 8UL; x++)
    db[x] = (uint8)((uint32)t0[x] | (uint32)t1[x]
                  | (uint32)t2[x] | (uint32)t3[x]);
  sms.vdp.tc_valid[key] = 1;
  VDP_COUNT(tc_miss);

  return ent;
}

/*
 * Whether chunk c of the video memory is inside the name table, or
 * inside the sprite attribute table: one subtraction against the table's
 * first chunk, the difference read unsigned so that a chunk below the
 * table falls past its length. The sprite table is 256 bytes, eight
 * chunks (docs/sms_gg/SMSOfficialDocs.md:411-461).
 */
#define VDP_CHUNK_IN_NT(chunk)  (((chunk) - sms.vdp.nt_chunk0) < VDP_NT_CHUNKS)
#define VDP_CHUNK_IN_SAT(chunk) (((chunk) - sms.vdp.sat_chunk0) < (256UL / 32UL))

/*
 * The watch byte of one chunk from the five facts it stands for: a
 * pattern the picture shows, a pattern marked hot, a pattern the sprite
 * table names, the name table, the sprite table. The one spelling of the
 * union, so that the places that drop a fact cannot drop the others.
 */
static uint8
vdp_watch_of(uint32 chunk)
{
  return (uint8)(((sms.vdp.refs[chunk] != 0U)
                  || (sms.vdp.hot[chunk] != 0U)
                  || (sms.vdp.spr_named[chunk] != 0U)
                  || VDP_CHUNK_IN_NT(chunk)
                  || VDP_CHUNK_IN_SAT(chunk)) ? 1U : 0U);
}

/*
 * The watch byte of every chunk brought back in line. Cold: init, a move
 * of the name table by register 2, a move of the sprite table by
 * register 5.
 */
static void
vdp_decor_watch_rebuild(void)
{
  uint32 chunk;

  for(chunk = 0; chunk < VDP_CHUNKS; chunk++)
    sms.vdp.watch[chunk] = vdp_watch_of(chunk);
}

/*
 * A pattern marked hot: watched from now to the end of the presentation,
 * whether or not the picture or the sheet shows it yet, and listed so
 * that the mark falls without a walk. Each pattern is listed once, which
 * bounds the list by the chunk count.
 */
static void
vdp_decor_hot(uint32 p)
{
  if(sms.vdp.hot[p] == 0U)
    {
      sms.vdp.hot[p] = 1;
      sms.vdp.watch[p] = 1;
      sms.vdp.hot_list[sms.vdp.hot_count++] = (uint16)p;
    }
}

/*
 * One tile of the picture converted again from the name table word it
 * now holds: the eight rows read out of the decoded row cache -- decoded
 * on a miss, like a stroke of the older path -- flipped as the word asks,
 * the bank laid on, two words a row written straight into the picture at
 * the tile's place. The same recut as a stroke of the older path, with
 * nothing to shift: the picture is the table, unscrolled; the scroll is
 * the windows' business.
 *
 * The reference counts follow the word: the pattern the tile showed
 * before loses one, the one it shows now gains one, and the watch byte of
 * the first falls when nothing else keeps it up.
 */
static void
vdp_decor_tile(uint32 t)
{
  const uint8 *nt;
  uint32 *dst;
  uint32 *ent;
  uint32 word;
  uint32 pattern;
  uint32 old;
  uint32 bankw;
  uint32 r;
  uint32 e0;
  uint32 e1;
  uint32 rv;
  uint32 sw;

  nt = sms.vdp.vram + (((uint32)sms.vdp.reg[2] & 0x0EUL) << 10);
  /* The thirteen bits that draw; 0xFFFF stays the never-converted mark. */
  word = read16_le(nt + (t << 1)) & 0x1FFFUL;
  pattern = word & 0x1FFUL;

  old = sms.vdp.decor_word[t];
  if(old != 0xFFFFUL)
    {
      old &= 0x1FFUL;
      sms.vdp.refs[old]--;
      if(sms.vdp.refs[old] == 0U)
        sms.vdp.watch[old] = vdp_watch_of(old);
    }
  sms.vdp.refs[pattern]++;
  sms.vdp.watch[pattern] = 1;
  sms.vdp.decor_word[t] = (uint16)word;

  /* Tile column t & 31 at byte column times 8, tile row t >> 5 at line times 8. */
  dst = (uint32 *)(sms.vdp.decor + ((t >> 5) * 8UL * VDP_PIX_ROW_BYTES)
                   + ((t & 31UL) << 3));
  bankw = ((word & 0x800UL) != 0UL) ? (16UL * VDP_TC_LANE_ONE) : 0UL;

  for(r = 0; r < 8UL; r++)
    {
      ent = vdp_tc_row(VDP_TC_KEY_TILE(pattern,
                                       ((word & 0x400UL) != 0UL) ? (7UL - r) : r));
      e0 = ent[0];
      e1 = ent[1];

      if((word & 0x200UL) != 0UL)
        {
          /* The eight indexes the other way round: see the stroke loops. */
          rv = (e0 ^ ((e0 >> 16) | (e0 << 16))) & 0xFF00FFFFUL;
          sw = ((e0 >> 8) | (e0 << 24)) ^ (rv >> 8);
          rv = (e1 ^ ((e1 >> 16) | (e1 << 16))) & 0xFF00FFFFUL;
          e0 = ((e1 >> 8) | (e1 << 24)) ^ (rv >> 8);
          e1 = sw;
        }

      dst[0] = e0 | bankw;
      dst[1] = e1 | bankw;
      dst += VDP_PIX_ROW_BYTES / 4UL;
    }

  VDP_COUNT(decor_tiles);
}

/*
 * The picture brought up to date with the video memory as it stands.
 *
 * The dirty marks are swept a word at a time and the dirty chunks listed;
 * a dirty chunk that is a pattern the picture shows is marked for the tile
 * sweep, a dirty chunk of the name table is noted. Then one of two walks:
 * every tile, when a shown pattern moved or when everything must be looked
 * at (init, a moved table) -- a tile is converted when its word differs
 * from the table's or names a marked pattern; or only the tiles of the
 * dirty name table chunks, sixteen a chunk, when the writes touched the
 * table alone. A dirty chunk that is neither -- a sprite pattern, an
 * unused pattern -- costs its mark and nothing else. Idempotent: called
 * again with nothing dirty it walks the marks and returns.
 */
static void
vdp_decor_apply(void)
{
  const uint8 *nt;
  uint32 *dw;
  uint8 *db;
  uint32 i;
  uint32 k;
  uint32 chunk;
  uint32 n;
  uint32 t;
  uint32 t_end;
  uint32 word;
  uint32 pat_hit;
  uint32 nt_hit;

  dw = sms.vdp.decor_dirty_w;
  db = VDP_DECOR_DIRTY;
  n = 0;
  pat_hit = 0;
  nt_hit = 0;

  for(i = 0; i < (VDP_CHUNKS / 4UL); i++)
    {
      if(dw[i] == 0UL)
        continue;

      for(k = 0; k < 4UL; k++)
        {
          chunk = (i << 2) + k;
          if(db[chunk] == 0U)
            continue;
          sms.vdp.dirty_list[n++] = (uint16)chunk;
          /* The sheet's place of the pattern no longer holds it. */
          sms.vdp.sheet_valid[chunk] = 0;
          if(sms.vdp.refs[chunk] != 0U)
            {
              sms.vdp.pat_dirty[chunk] = 1;
              pat_hit = 1;
            }
          if(VDP_CHUNK_IN_NT(chunk))
            nt_hit = 1;
        }
      dw[i] = 0;
    }

  if((n == 0UL) && (sms.vdp.decor_sweep_all == 0UL))
    return;

  nt = sms.vdp.vram + (((uint32)sms.vdp.reg[2] & 0x0EUL) << 10);

  if((sms.vdp.decor_sweep_all != 0UL) || (pat_hit != 0UL))
    {
      sms.vdp.decor_sweep_all = 0;
      for(t = 0; t < VDP_NT_TILES; t++)
        {
          word = read16_le(nt + (t << 1)) & 0x1FFFUL;
          if(((uint32)sms.vdp.decor_word[t] != word)
             || (sms.vdp.pat_dirty[word & 0x1FFUL] != 0U))
            vdp_decor_tile(t);
        }
    }
  else if(nt_hit != 0UL)
    {
      for(i = 0; i < n; i++)
        {
          chunk = sms.vdp.dirty_list[i];
          if(!VDP_CHUNK_IN_NT(chunk))
            continue;
          t = (chunk - sms.vdp.nt_chunk0) << 4;
          t_end = t + 16UL;
          for(; t < t_end; t++)
            {
              word = read16_le(nt + (t << 1)) & 0x1FFFUL;
              if((uint32)sms.vdp.decor_word[t] != word)
                vdp_decor_tile(t);
            }
        }
    }

  for(i = 0; i < n; i++)
    sms.vdp.pat_dirty[sms.vdp.dirty_list[i]] = 0;
}

/*
 * One entry into the journal, kept sorted by line. The writes land in
 * line order but for one: a register 8 entry is dated one line later
 * than the write (the hardware latches the scroll at the end of the
 * line, docs/sms_gg/GGOfficialDocs.md:1438), so a write of the same
 * line that follows it must be put before it. The walk back is at most
 * over the entries of one line, and nothing at all on the common case.
 * When the journal is full the first lost write says so once and raises
 * the overflow: the picture is then presented in one band from its
 * final state, memory and registers alike (vdp.h, journal_overflow).
 */
static void
vdp_journal_add(uint32 line,
                uint32 addr,
                uint32 old,
                uint32 val)
{
  vdp_journal_t *j;
  uint32 i;

  if(sms.vdp.journal_count >= VDP_JOURNAL_ENTRIES)
    {
      if(sms.vdp.journal_overflow == 0UL)
        {
          sms.vdp.journal_overflow = 1;
          sms.vdp.pic_degraded = 1;
          VDP_COUNT(journal_full);
          LOG_ONCE(LOG_CAT_VDP,LOG_LVL_WARN,
                   ("decor journal full: the picture shows its final state in one band"));
        }
      return;
    }

  i = sms.vdp.journal_count;
  while((i > 0UL) && ((uint32)sms.vdp.journal[i - 1UL].line > line))
    {
      j = &sms.vdp.journal[i - 1UL];
      sms.vdp.journal[i].line = j->line;
      sms.vdp.journal[i].addr = j->addr;
      sms.vdp.journal[i].old = j->old;
      sms.vdp.journal[i].val = j->val;
      i--;
    }
  j = &sms.vdp.journal[i];
  j->line = (uint16)line;
  j->addr = (uint16)addr;
  j->old = (uint16)old;
  j->val = (uint16)val;
  sms.vdp.journal_count++;
}

void
vdp_decor_note(uint32 addr,
               uint32 value)
{
  uint32 chunk;
  uint32 word;
  uint32 p;

  if(sms.vdp.vcount < VDP_ACTIVE_LINES)
    vdp_journal_add(sms.vdp.vcount,addr,(uint32)sms.vdp.vram[addr],value);

  chunk = addr >> 5;
  if(VDP_CHUNK_IN_NT(chunk))
    {
      /* The word as it will read once the byte lands, low byte first. */
      if((addr & 1UL) != 0UL)
        word = (uint32)sms.vdp.vram[addr - 1UL] | (value << 8);
      else
        word = value | ((uint32)sms.vdp.vram[addr + 1UL] << 8);
      vdp_decor_hot(word & 0x1FFUL);
    }
  /*
   * Tested on its own and not as the other branch of the name table:
   * registers 2 and 5 may lay the two tables over each other.
   */
  if(VDP_CHUNK_IN_SAT(chunk))
    {
      /*
       * A byte of the sprite table: the per-line table is stale from
       * the next line on, and a pattern number just written names its
       * pattern before the rebuild can count it -- both patterns of the
       * pair when the sprites are tall, since they draw the pair
       * whichever the byte names (SMSOfficialDocs.md:846-852; the
       * pattern byte is the odd byte of the second half of the table,
       * :448-461).
       */
      sms.vdp.spr_dirty = 1;
      if(((addr & 255UL) >= VDP_SPR_XN_OFFSET) && ((addr & 1UL) != 0UL))
        {
          p = value + ((((uint32)sms.vdp.reg[6] & 0x04UL) != 0UL) ? 256UL : 0UL);
          if(((uint32)sms.vdp.reg[1] & 0x02UL) != 0UL)
            {
              vdp_decor_hot(p & ~1UL);
              vdp_decor_hot(p | 1UL);
            }
          else
            vdp_decor_hot(p);
        }
    }
}

/*
 * The bits of a register the list is built from, and so the bits a
 * write mid-picture is journaled on and the bits an undo or a replay
 * puts back -- the other bits keep the live value throughout, so that a
 * display bit of register 1 written mid-picture and not journaled is
 * not clobbered by the replay of a size bit written earlier. Register
 * 0: the two locks, the masked column, the sprite shift
 * (SMSOfficialDocs.md:764-772); register 1: size and magnification
 * (:779-783; the display bit is per line already, row_off); registers
 * 2 its name table bits, 5 its sprite table bits, 6 its pattern base
 * bit (:801-860) -- the bits the list reads and the live write compares,
 * so that a program rewriting the unused bits, as many do, cuts no
 * band; register 8 whole (:872); register 7 its low four bits
 * (:861-864). Registers 3, 4, 9 and 10 are never journaled: 9 is latched
 * in the blanking (:895), 10 and the interrupt bits are per line, 3 and
 * 4 draw nothing in mode 4.
 */
static uint32
vdp_reg_journal_mask(uint32 number)
{
  switch(number)
    {
    case 0UL:
      return 0xE8UL;
    case 1UL:
      return 0x03UL;
    case 2UL:
      return 0x0EUL;
    case 5UL:
      return 0x7EUL;
    case 6UL:
      return 0x04UL;
    case 8UL:
      return 0xFFUL;
    case 7UL:
      return 0x0FUL;
    default:
      return 0UL;
    }
}

/*
 * A register put to a value, on its journaled bits, with the effect the
 * list reads off it: register 8 moves the horizontal latch with it (the
 * presentation has no line between two bands, so the latch is the
 * register); register 5 moves the sprite table; register 7 refills the
 * backdrop column; register 2 moves the name table and marks every tile
 * of the picture for conversion, as the live write does. What it does
 * NOT do is the live write's own business: the per-line sprite table
 * is not owed again and the watch bytes are not rebuilt -- both follow
 * the final state, which the live write already gave them. Called by
 * the live write for its shared part, and by the undo and the replay of
 * the presentation for the whole of theirs.
 */
static void
vdp_reg_apply(uint32 number,
              uint32 value)
{
  uint32 mask;
  uint32 v;

  mask = vdp_reg_journal_mask(number);
  v = ((uint32)sms.vdp.reg[number] & ~mask) | (value & mask);

  if(number == 2UL)
    {
      if((((uint32)sms.vdp.reg[2] ^ v) & 0x0EUL) != 0UL)
        {
          uint32 i;

          sms.vdp.nt_chunk0 = ((v & 0x0EUL) << 10) >> 5;
          for(i = 0; i < VDP_NT_TILES; i++)
            sms.vdp.decor_word[i] = 0xFFFFU;
          for(i = 0; i < VDP_CHUNKS; i++)
            sms.vdp.refs[i] = 0;
          sms.vdp.decor_sweep_all = 1;
        }
    }
  else if(number == 5UL)
    {
      sms.vdp.sat_base = (v & 0x7EUL) << 7;
      sms.vdp.sat_chunk0 = sms.vdp.sat_base >> 5;
    }
  else if(number == 7UL)
    {
      if((((uint32)sms.vdp.reg[7] ^ v) & 0x0FUL) != 0UL)
        {
          sms.vdp.reg[7] = (uint8)v;
          vdp_column_fill();
        }
    }
  else if(number == 8UL)
    {
      sms.vdp.hscroll = v;
    }

  sms.vdp.reg[number] = (uint8)v;
}

/*
 * A register the list is built from, written mid-picture to another
 * value on its journaled bits: one entry of the journal, dated the line
 * of the write, or the line after for register 8 (the latch at the end
 * of the line, GGOfficialDocs.md:1438: the line being scanned still
 * shows the old scroll, the next one the new). An entry on line 192
 * opens no band and is put back by the last band. Counted apart as a
 * scroll split or another register.
 */
static void
vdp_reg_note(uint32 number,
             uint32 old,
             uint32 value)
{
  uint32 line;

  line = (number == 8UL) ? (sms.vdp.vcount + 1UL) : sms.vdp.vcount;
  vdp_journal_add(line,VDP_JOURNAL_REG | number,old,value);
  if(number == 8UL)
    VDP_COUNT(scroll_mid);
  else
    VDP_COUNT(reg_journal);
}

void
vdp_cram_note(void)
{
  uint32 line;
  uint32 k;
  uint32 i;

  line = sms.vdp.vcount;
  if(line == 0UL)
    return;

  k = sms.vdp.pal_count;
  if(k == 0UL)
    {
      sms.vdp.pal_line[0] = 0;
      k = 1;
    }
  if((uint32)sms.vdp.pal_line[k - 1UL] == line)
    {
      sms.vdp.pal_count = k;
      return;
    }
  if(k >= VDP_PAL_SEGMENTS)
    {
      /*
       * The cap: the lines from here on show the final table, counted
       * once per picture and said once for the run.
       */
      sms.vdp.pal_count = k;
      if(sms.vdp.pal_capped == 0UL)
        {
          sms.vdp.pal_capped = 1;
          sms.vdp.pic_degraded = 1;
          VDP_COUNT(pal_capped);
          LOG_ONCE(LOG_CAT_VDP,LOG_LVL_WARN,
                   ("palette segments capped at %lu: later lines take the last table",
                    (unsigned long)VDP_PAL_SEGMENTS));
        }
      return;
    }

  for(i = 0; i < VDP_CRAM_SIZE; i++)
    sms.vdp.pal_cram[k - 1UL][i] = sms.vdp.cram[i];
  sms.vdp.pal_line[k] = (uint8)line;
  sms.vdp.pal_count = k + 1UL;
  VDP_COUNT(pal_seg);
}

/*
 * ---------------------------------------------------------------------------
 * The sprites: the per-line table, the sheet, the flags.
 * ---------------------------------------------------------------------------
 */

void
vdp_sprite_scan(uint32 from)
{
  const uint8 *sat;
  const uint8 *reg;
  uint32 i;
  uint32 y;
  uint32 n;
  uint32 p;
  uint32 height;
  uint32 base;
  uint32 tall;
  int32 top;
  int32 ya;
  int32 yb;

  reg = sms.vdp.reg;
  sat = sms.vdp.vram + sms.vdp.sat_base;
  tall = (uint32)reg[1] & 0x02UL;
  height = (tall != 0UL) ? 16UL : 8UL;
  height <<= ((uint32)reg[1] & 0x01UL);
  base = (((uint32)reg[6] & 0x04UL) != 0UL) ? 256UL : 0UL;

  /*
   * The lines before the one being counted keep what they had: their
   * flags have been read and the bands of the presentation draw them
   * with the memory as it stood then, admission included. Only the
   * lines from here on take the table as it now stands.
   */
  for(y = from; y < VDP_ACTIVE_LINES; y++)
    {
      sms.vdp.spr_n[y] = 0;
      sms.vdp.spr_ovf_line[y] = 0;
      sms.vdp.spr_adm[y][0] = 0;
      sms.vdp.spr_adm[y][1] = 0;
    }

  /*
   * The named marks of the previous table fall; the ones of this table
   * are raised below as the walk meets them. Inside the picture the
   * lines before this one drew the previous table, and the bands of the
   * presentation replay it for them: a write to one of its patterns
   * later in the picture must still be journaled, so its mark turns hot
   * -- kept to the end of the presentation, like a pattern the name
   * table stopped naming. At line 0 no band to come drew it, and the
   * watch byte falls at once where nothing else holds it up.
   */
  for(i = 0; i < sms.vdp.spr_named_count; i++)
    {
      p = sms.vdp.spr_named_list[i];
      sms.vdp.spr_named[p] = 0;
      if(from != 0UL)
        vdp_decor_hot(p);
      else
        sms.vdp.watch[p] = vdp_watch_of(p);
    }
  sms.vdp.spr_named_count = 0;

  /*
   * The walk of the older path's vdp_select_sprites, once for every line
   * instead of once per line: the terminator ends it, the position on
   * screen is the byte plus one and turns negative past VDP_SPR_Y_WRAP
   * (SMSOfficialDocs.md:443-446; TotalSMS/src/core/sms_vdp.c:1074,
   * :1090-1093), an entry touches the lines from its top to its height,
   * and on each of them the first eight of the table are admitted, the
   * ninth and every one after it raise the line's overflow mark
   * (SMSOfficialDocs.md:393-397; sms_vdp.c:1105-1117). The pattern the
   * entry names is marked for the watch, the pair of it when the sprites
   * are tall.
   */
  for(i = 0; i < VDP_SPR_COUNT; i++)
    {
      if(sat[i] == (uint8)VDP_SPR_TERMINATOR)
        break;

      top = (int32)sat[i] + 1;
      if(top > (int32)VDP_SPR_Y_WRAP)
        top -= 256;
      sms.vdp.sat_top[i] = (int16)top;

      p = (uint32)sat[VDP_SPR_XN_OFFSET + (i << 1) + 1UL] + base;
      if(tall != 0UL)
        p &= ~1UL;
      if(sms.vdp.spr_named[p] == 0U)
        {
          sms.vdp.spr_named[p] = 1;
          sms.vdp.watch[p] = 1;
          sms.vdp.spr_named_list[sms.vdp.spr_named_count++] = (uint16)p;
        }
      if((tall != 0UL) && (sms.vdp.spr_named[p + 1UL] == 0U))
        {
          sms.vdp.spr_named[p + 1UL] = 1;
          sms.vdp.watch[p + 1UL] = 1;
          sms.vdp.spr_named_list[sms.vdp.spr_named_count++] = (uint16)(p + 1UL);
        }

      ya = (top < (int32)from) ? (int32)from : top;
      yb = top + (int32)height;
      if(yb > (int32)VDP_ACTIVE_LINES)
        yb = (int32)VDP_ACTIVE_LINES;

      for(y = (uint32)ya; (int32)y < yb; y++)
        {
          n = sms.vdp.spr_n[y];
          if(n < VDP_SPR_MAX_ON_LINE)
            {
              sms.vdp.spr_idx[y][n] = (uint8)i;
              sms.vdp.spr_n[y] = (uint8)(n + 1UL);
              sms.vdp.spr_adm[y][i >> 5] |= 1UL << (i & 31UL);
            }
          else
            sms.vdp.spr_ovf_line[y] = 1;
        }
    }

  sms.vdp.spr_dirty = 0;
  sms.vdp.spr_partial = (from != 0UL) ? 1UL : 0UL;
}

/*
 * The place of pattern p in the sheet converted from the video memory as
 * it stands: the eight rows read out of the decoded row cache, decoded
 * on a miss like a tile of the picture, each index moved to the second
 * bank where it is opaque and left at zero where it is not -- an OR of
 * sixteen on every lane would make colour 0 opaque, which is the one
 * thing a sprite pixel must never be. Two words a row, at the pitch of
 * the picture.
 */
static void
vdp_sheet_tile(uint32 p)
{
  uint32 *dst;
  uint32 *ent;
  uint32 r;
  uint32 e0;
  uint32 e1;

  dst = (uint32 *)(sms.vdp.sheet + (VDP_SHEET_Y(p) * VDP_SHEET_W) + VDP_SHEET_X(p));
  for(r = 0; r < 8UL; r++)
    {
      ent = vdp_tc_row(VDP_TC_KEY_TILE(p,r));
      e0 = ent[0];
      e1 = ent[1];
      dst[0] = e0 | (VDP_LANE_OPAQUE(e0) << 4);
      dst[1] = e1 | (VDP_LANE_OPAQUE(e1) << 4);
      dst += VDP_SHEET_W / 4UL;
    }
  sms.vdp.sheet_valid[p] = 1;
}

/*
 * The backdrop column filled with the backdrop index, four pixels a
 * word: what the masked left column and the rows with the display off
 * are drawn from. Init, and a move of register 7.
 */
static void
vdp_column_fill(void)
{
  uint32 *w;
  uint32 v;
  uint32 i;

  w = (uint32 *)sms.vdp.column;
  v = VDP_BACKDROP_INDEX() * VDP_TC_LANE_ONE;
  for(i = 0; i < (VDP_COLUMN_BYTES / 4UL); i++)
    w[i] = v;
}

/*
 * The collision of one line replayed without a pixel written, on the
 * admitted sprites of the line and only when two of them share a column
 * -- a lone sprite can take a pixel from no one. Then the rules of the
 * older path's vdp_draw_sprites, exactly: a sprite wholly left of the
 * first drawn column takes nothing; a pixel left of that column or past
 * the right edge takes nothing; index zero takes nothing; a pixel a
 * sprite already took is the collision, raised whether or not it stood,
 * counted once per line, and the walk stops there. The priority of the
 * background plays no part in it (TotalSMS/src/core/sms_vdp.c:1179-1224;
 * the disagreement between the two sources on who shows on top is
 * recorded beside the older path and changes nothing here: the taken
 * mask is written in table order either way). The rows are the decoded
 * row cache's, the same the picture and the sheet read.
 */
static void
vdp_sprite_collide(uint32 y,
                   uint32 n)
{
  const uint8 *reg;
  const uint8 *sat;
  const uint8 *row;
  uint8 *taken;
  uint32 *clear;
  int32 xl[VDP_SPR_MAX_ON_LINE];
  int32 xr[VDP_SPR_MAX_ON_LINE];
  uint32 need[VDP_SPR_MAX_ON_LINE];
  uint32 r;
  uint32 s;
  uint32 i;
  uint32 x;
  uint32 any;
  uint32 zoom;
  uint32 width;
  uint32 base;
  uint32 tall;
  uint32 p;
  uint32 srow;
  int32 x0;
  int32 xi;
  int32 shift;
  int32 startx;

  reg = sms.vdp.reg;
  sat = sms.vdp.vram + sms.vdp.sat_base;
  zoom = (uint32)reg[1] & 0x01UL;
  tall = (uint32)reg[1] & 0x02UL;
  width = 8UL << zoom;
  base = (((uint32)reg[6] & 0x04UL) != 0UL) ? 256UL : 0UL;
  shift = (((uint32)reg[0] & 0x08UL) != 0UL) ? 8 : 0;
  startx = (((uint32)reg[0] & 0x20UL) != 0UL) ? 8 : 0;

  for(r = 0; r < n; r++)
    {
      i = sms.vdp.spr_idx[y][r];
      x0 = (int32)sat[VDP_SPR_XN_OFFSET + (i << 1)] - shift;
      xl[r] = (x0 < startx) ? startx : x0;
      xr[r] = ((x0 + (int32)width) > (int32)VDP_PIX_WIDTH)
              ? (int32)VDP_PIX_WIDTH : (x0 + (int32)width);
      need[r] = 0;
    }

  any = 0;
  for(r = 0; r < n; r++)
    {
      for(s = r + 1UL; s < n; s++)
        {
          if((xl[r] < xr[s]) && (xl[s] < xr[r]) && (xl[r] < xr[r]) && (xl[s] < xr[s]))
            {
              need[r] = 1;
              need[s] = 1;
              any = 1;
            }
        }
    }
  if(any == 0UL)
    return;

  taken = (uint8 *)sms.vdp.spr_taken;
  clear = sms.vdp.spr_taken;
  for(x = 0; x < (VDP_PIX_WIDTH / 4UL); x++)
    clear[x] = 0;

  for(r = 0; r < n; r++)
    {
      if(need[r] == 0UL)
        continue;
      i = sms.vdp.spr_idx[y][r];
      x0 = (int32)sat[VDP_SPR_XN_OFFSET + (i << 1)] - shift;
      p = (uint32)sat[VDP_SPR_XN_OFFSET + (i << 1) + 1UL] + base;
      if(tall != 0UL)
        p &= ~1UL;
      srow = ((uint32)((int32)y - (int32)sms.vdp.sat_top[i])) >> zoom;
      row = (const uint8 *)vdp_tc_row(VDP_TC_KEY_TILE(p,srow));

      for(x = 0; x < width; x++)
        {
          xi = x0 + (int32)x;
          if(xi < startx)
            continue;
          if(xi >= (int32)VDP_PIX_WIDTH)
            break;
          if(row[x >> zoom] == 0U)
            continue;
          if(taken[xi] != 0U)
            {
              sms.vdp.spr_collision = 1;
              VDP_COUNT(spr_col);
              return;
            }
          taken[xi] = 1;
        }
    }
}

/*
 * The one cel factory of the list. Every block of every band -- a window
 * of the picture, a sprite, a run of priority tiles, a run of the
 * backdrop column -- is filled here: a coded cel of eight bits, its
 * source a word address at a row pitch given in words (at least two,
 * docs/3do/3DO_Development_Notes.md:92-95), w pixels by h rows, at
 * (x, y) of the clip rectangle, the horizontal step in 12.20 and the
 * vertical in 16.16 (src_exemple_video_player/renderer.c:56-86), the
 * palette pointer given, and the background flag given or not: with it
 * a pixel whose palette entry is 000 is painted, without it that pixel
 * is transparent (docs/3do/3do_portfolio_2.5.md:3783-3784). The
 * position is shifted by the profile's offset (pic_x, pic_y: zero for
 * the Master System, the window's negative origin for the Game Gear),
 * the one place the crop touches the list. The preamble words are the
 * same arithmetic as the whole-picture cel's, the row offset in the ten
 * bit field an eight bit depth reads.
 */
static void
vdp_cel_fill(CCB         *c,
             const uint8 *src,
             uint32       pitch_words,
             uint32       w,
             uint32       h,
             int32        x,
             int32        y,
             int32        hdx,
             int32        vdy,
             void        *plut,
             uint32       bgnd)
{
  c->ccb_Flags = CCB_NPABS | CCB_SPABS | CCB_PPABS | CCB_LDSIZE | CCB_LDPRS
               | CCB_LDPPMP | CCB_CCBPRE | CCB_YOXY | CCB_USEAV | CCB_NOBLK
               | CCB_ACE | CCB_ACW | CCB_ACCW | bgnd;
  c->ccb_NextPtr = NULL;
  c->ccb_SourcePtr = (CelData *)src;
  c->ccb_PLUTPtr = plut;
  c->ccb_XPos = (Coord)((x + sms.vdp.pic_x) * 65536L);
  c->ccb_YPos = (Coord)((y + sms.vdp.pic_y) * 65536L);
  c->ccb_HDX = hdx;
  c->ccb_HDY = 0;
  c->ccb_VDX = 0;
  c->ccb_VDY = vdy;
  c->ccb_HDDX = 0;
  c->ccb_HDDY = 0;
  c->ccb_PIXC = 0x1F001F00UL;
  c->ccb_PRE0 = ((h - PRE0_VCNT_PREFETCH) << PRE0_VCNT_SHIFT) | PRE0_BPP_8;
  c->ccb_PRE1 = ((pitch_words - PRE1_WOFFSET_PREFETCH) << PRE1_WOFFSET10_SHIFT)
              | PRE1_TLLSB_PDC0
              | (w - PRE1_TLHPCNT_PREFETCH);
  c->ccb_Width = (int32)w;
  c->ccb_Height = (int32)h;
}

/* A block chained after the last one of the band, or opening the band. */
static void
vdp_chain(CCB *c)
{
  if(vdp_chain_head == NULL)
    vdp_chain_head = c;
  else
    vdp_chain_tail->ccb_NextPtr = c;
  vdp_chain_tail = c;
}

/*
 * A small cel from the reserve of the sprite page, filled as above and
 * chained; NULL, counted and said once when the reserve is spent -- the
 * band is drawn without it rather than the page overrun.
 */
static CCB *
vdp_small(const uint8 *src,
          uint32       pitch_words,
          uint32       w,
          uint32       h,
          int32        x,
          int32        y,
          int32        hdx,
          int32        vdy,
          void        *plut,
          uint32       bgnd)
{
  CCB *blk;

  if(sms.vdp.cels_used >= VDP_LIST_CELS)
    {
      VDP_COUNT(cels_refused);
      LOG_ONCE(LOG_CAT_VDP,LOG_LVL_WARN,
               ("cel refused: the reserve of %lu blocks is full",
                (unsigned long)VDP_LIST_CELS));
      return NULL;
    }

  blk = (CCB *)sms.vdp.cels + sms.vdp.cels_used;
  sms.vdp.cels_used++;
  vdp_cel_fill(blk,src,pitch_words,w,h,x,y,hdx,vdy,plut,bgnd);
  vdp_chain(blk);
  VDP_COUNT(cels);

  return blk;
}

/*
 * One window of the picture on the screen: the rows ya to yb of the
 * picture's area at columns xa to xb, showing the picture from column px
 * and row py. The source pointer is moved back to the word before px and
 * the window widened by as much, so that its first f pixels are the
 * columns before px, standing left of xa where the clip rectangle erases
 * them. The block is chained after the previous one of the band; the
 * band builder closes the chain. Positions are relative to the clip
 * rectangle, since the folio takes (0,0) as its top-left corner
 * (docs/3do/3do_portfolio_2.5.md:10984); the x of a widened window may
 * be below zero, and it is formed by a multiplication because a shift of
 * a negative value is undefined in this language.
 */
static void
vdp_window(uint32 xa,
           uint32 xb,
           uint32 ya,
           uint32 yb,
           uint32 px,
           uint32 py)
{
  CCB *blk;
  uint32 f;
  uint32 w;
  uint32 h;
  int32 x;

  if(sms.vdp.list_used >= VDP_LIST_WINDOWS)
    {
      VDP_COUNT(list_refused);
      LOG_ONCE(LOG_CAT_VDP,LOG_LVL_WARN,
               ("decor window refused: the arena of %lu blocks is full",
                (unsigned long)VDP_LIST_WINDOWS));
      return;
    }

  f = px & 3UL;
  w = (xb - xa) + f;
  h = yb - ya;
  x = (int32)xa - (int32)f;

  blk = (CCB *)sms.vdp.windows + sms.vdp.list_used;
  sms.vdp.list_used++;
  vdp_cel_fill(blk,
               sms.vdp.decor + (py * VDP_PIX_ROW_BYTES) + (px - f),
               VDP_PIX_ROW_BYTES / 4UL,
               w,h,x,(int32)ya,1L << 20,1L << 16,
               sms.vdp.plut,CCB_BGND);
  vdp_chain(blk);
  VDP_COUNT(list_windows);

  /* Kept for the priority pass, which cuts its runs as the windows are cut. */
  if(sms.vdp.win_count < VDP_BAND_WINDOWS)
    {
      vdp_win_t *r = &sms.vdp.win[sms.vdp.win_count++];

      r->xa = (uint16)xa;
      r->xb = (uint16)xb;
      r->ya = (uint16)ya;
      r->yb = (uint16)yb;
      r->px = (uint16)px;
      r->py = (uint16)py;
    }
}

/*
 * The columns xa to xb of the rows ya to yb, the rows read from picture
 * row py on, under the horizontal scroll hs: screen column x shows
 * picture column (x - hs) & 255 (SMSOfficialDocs.md:870-882), so the
 * picture wraps at screen column hs, and the region is one window or two
 * meeting there.
 */
static void
vdp_list_cols(uint32 ya,
              uint32 yb,
              uint32 py,
              uint32 xa,
              uint32 xb,
              uint32 hs)
{
  if((hs != 0UL) && (xa < hs) && (hs < xb))
    {
      vdp_window(xa,hs,ya,yb,(xa + VDP_PIX_WIDTH) - hs,py);
      vdp_window(hs,xb,ya,yb,0UL,py);
      return;
    }

  vdp_window(xa,xb,ya,yb,((xa + VDP_PIX_WIDTH) - hs) & (VDP_PIX_WIDTH - 1UL),py);
}

/*
 * The columns xa to xb of the rows ya to yb, scrolled vertically by vs or
 * not at all: row y shows picture row y + vs, wrapped once on the table
 * height (SMSOfficialDocs.md:884-895; the single subtraction of the
 * older path, whose guard in vdp.h still holds), so the rows wrap at
 * screen row 224 - vs and the region is one piece or two meeting there.
 */
static void
vdp_list_region(uint32 ya,
                uint32 yb,
                uint32 xa,
                uint32 xb,
                uint32 hs,
                uint32 scrolled,
                uint32 vs)
{
  int32 s;

  if(scrolled == 0UL)
    {
      vdp_list_cols(ya,yb,ya,xa,xb,hs);
      return;
    }

  s = (int32)VDP_NT_LINES - (int32)vs;
  if(s <= (int32)ya)
    {
      vdp_list_cols(ya,yb,(ya + vs) - VDP_NT_LINES,xa,xb,hs);
    }
  else if(s >= (int32)yb)
    {
      vdp_list_cols(ya,yb,ya + vs,xa,xb,hs);
    }
  else
    {
      vdp_list_cols(ya,(uint32)s,ya + vs,xa,xb,hs);
      vdp_list_cols((uint32)s,yb,0UL,xa,xb,hs);
    }
}

/*
 * The rows ya to yb under one horizontal scroll: the whole width, or,
 * when register 0 bit 7 holds the right eight columns still
 * (SMSOfficialDocs.md:768), two column regions. The split is at screen
 * column 192 + fine and not at 192, because that is where the older path
 * cuts it: its strokes are tile columns picked by the coarse scroll, and
 * the stroke of screen column 24 -- the first still one -- starts fine
 * pixels in, its first fine pixels coming from the scrolled stroke
 * before it (vdp_render_line of the older path, the recut). The still
 * columns then start on a picture column that is a multiple of eight,
 * so their window needs no alignment columns. They are built before the
 * region beside them all the same, so that if a widened window ever
 * spilled over the join, the scrolled region would cover it.
 */
static void
vdp_list_rows(uint32 ya,
              uint32 yb,
              uint32 hs)
{
  uint32 split;

  if(((uint32)sms.vdp.reg[0] & 0x80UL) != 0UL)
    {
      split = (VDP_PIX_WIDTH - 64UL) + (hs & 7UL);
      vdp_list_region(ya,yb,split,VDP_PIX_WIDTH,hs,0UL,0UL);
      vdp_list_region(ya,yb,0UL,split,hs,1UL,sms.vdp.vscroll);
      return;
    }

  vdp_list_region(ya,yb,0UL,VDP_PIX_WIDTH,hs,1UL,sms.vdp.vscroll);
}

/*
 * One admitted run of a sprite, the lines la to lb of the band, as cels
 * of the sheet from the pattern's place at src: without magnification
 * one cel of the rows la - top to lb - top, at (x, la). Magnified, a
 * source row covers two lines, the first line of the run may be the
 * second line of its row and the last line the first line of its row
 * (a band boundary, a refused range), and a cel of doubled rows placed
 * there would draw a line outside the run: those two lines are one cel
 * of one row and one line each, VDY at one, and the rows between are one
 * cel of doubled rows. The horizontal step is the caller's, doubled
 * with the rows. Returns how many cels the run cost, the refused ones
 * not counted.
 */
static uint32
vdp_list_sprite_run(const uint8 *src,
                    int32        x,
                    int32        top,
                    uint32       la,
                    uint32       lb,
                    uint32       zoom,
                    int32        hdx)
{
  uint32 d0;
  uint32 d1;
  uint32 n;
  uint32 tail;

  d0 = (uint32)((int32)la - top);
  d1 = (uint32)((int32)lb - top);

  if(zoom == 0UL)
    {
      return (vdp_small(src + (d0 * VDP_SHEET_W),VDP_SHEET_W / 4UL,8UL,lb - la,
                        x,(int32)la,hdx,1L << 16,sms.vdp.plut,VDP_SPRITE_BGND)
              != NULL) ? 1UL : 0UL;
    }

  n = 0;
  if((d0 & 1UL) != 0UL)
    {
      if(vdp_small(src + ((d0 >> 1) * VDP_SHEET_W),VDP_SHEET_W / 4UL,8UL,1UL,
                   x,(int32)la,hdx,1L << 16,sms.vdp.plut,VDP_SPRITE_BGND) != NULL)
        n++;
      la++;
      d0++;
    }

  tail = 0;
  if(((d1 & 1UL) != 0UL) && (la < lb))
    {
      tail = 1;
      lb--;
      d1--;
    }

  if(la < lb)
    {
      if(vdp_small(src + ((d0 >> 1) * VDP_SHEET_W),VDP_SHEET_W / 4UL,8UL,(d1 - d0) >> 1,
                   x,(int32)la,hdx,2L << 16,sms.vdp.plut,VDP_SPRITE_BGND) != NULL)
        n++;
    }

  if(tail != 0UL)
    {
      if(vdp_small(src + ((d1 >> 1) * VDP_SHEET_W),VDP_SHEET_W / 4UL,8UL,1UL,
                   x,(int32)lb,hdx,1L << 16,sms.vdp.plut,VDP_SPRITE_BGND) != NULL)
        n++;
    }

  return n;
}

/*
 * The sprites of the lines a to b, from the highest numbered entry to
 * the lowest so that the lowest is drawn last and shows on top -- the
 * older path's "first of the table wins" (its vdp_draw_sprites says why
 * the reference outranks the document there). The terminator and each
 * entry before it are read from the attribute table AS THE VIDEO MEMORY
 * STANDS, replayed to the band's first line -- the per-line table holds
 * the admission of each line as it was counted, nothing else -- so a
 * band before a write of the table draws the table it saw: the entry's
 * screen line, its horizontal position,
 * eight to the left when register 0 bit 3 asks (SMSOfficialDocs.md:
 * 379-381), its pattern from the second eight kilobytes when register 6
 * bit 2 is set and forced even when the sprites are tall
 * (sms_vdp.c:1148-1165; SMSOfficialDocs.md:846-852), converted into the
 * sheet if its place is stale. The lines it is drawn on are the lines
 * of the band it touches AND the hardware admits, read off the per-line
 * table as a bit per line; every maximal run of them is one run above.
 * An entry wholly left of the picture costs nothing; one that runs off
 * the right edge is cut by the clip rectangle.
 */
static void
vdp_list_sprites(uint32 a,
                 uint32 b)
{
  const uint8 *reg;
  const uint8 *sat;
  const uint8 *src;
  uint32 alive;
  uint32 k;
  uint32 i;
  uint32 zoom;
  uint32 tall;
  uint32 height;
  uint32 width;
  uint32 base;
  uint32 p;
  uint32 word;
  uint32 bit;
  uint32 y;
  uint32 ya;
  uint32 yb;
  uint32 la;
  uint32 ncel;
  int32 top;
  int32 x;
  int32 shift;
  int32 hdx;

  reg = sms.vdp.reg;
  sat = sms.vdp.vram + sms.vdp.sat_base;
  zoom = (uint32)reg[1] & 0x01UL;
  tall = (uint32)reg[1] & 0x02UL;
  height = ((tall != 0UL) ? 16UL : 8UL) << zoom;
  width = 8UL << zoom;
  base = (((uint32)reg[6] & 0x04UL) != 0UL) ? 256UL : 0UL;
  shift = (((uint32)reg[0] & 0x08UL) != 0UL) ? 8 : 0;
  hdx = (int32)((1UL << zoom) << 20);

  for(alive = 0; alive < VDP_SPR_COUNT; alive++)
    {
      if((uint32)sat[alive] == VDP_SPR_TERMINATOR)
        break;
    }

  for(k = 0; k < alive; k++)
    {
      i = (alive - 1UL) - k;
      top = (int32)sat[i] + 1;
      if(top > (int32)VDP_SPR_Y_WRAP)
        top -= 256;

      /* The lines of the band the entry touches, in signed arithmetic:
         an entry wholly above the picture ends below zero. */
      {
        int32 la_s = ((int32)a > top) ? (int32)a : top;
        int32 lb_s = ((int32)b < (top + (int32)height)) ? (int32)b : (top + (int32)height);

        if(la_s >= lb_s)
          continue;
        ya = (uint32)la_s;
        yb = (uint32)lb_s;
      }

      x = (int32)sat[VDP_SPR_XN_OFFSET + (i << 1)] - shift;
      if((x + (int32)width) <= 0)
        continue;

      p = (uint32)sat[VDP_SPR_XN_OFFSET + (i << 1) + 1UL] + base;
      if(tall != 0UL)
        p &= ~1UL;
      if(sms.vdp.sheet_valid[p] == 0U)
        vdp_sheet_tile(p);
      if((tall != 0UL) && (sms.vdp.sheet_valid[p + 1UL] == 0U))
        vdp_sheet_tile(p + 1UL);
      src = sms.vdp.sheet + (VDP_SHEET_Y(p) * VDP_SHEET_W) + VDP_SHEET_X(p);

      word = i >> 5;
      bit = 1UL << (i & 31UL);
      ncel = 0;
      y = ya;
      while(y < yb)
        {
          while((y < yb) && ((sms.vdp.spr_adm[y][word] & bit) == 0UL))
            y++;
          if(y >= yb)
            break;
          la = y;
          while((y < yb) && ((sms.vdp.spr_adm[y][word] & bit) != 0UL))
            y++;
          ncel += vdp_list_sprite_run(src,x,top,la,y,zoom,hdx);
        }

      if(ncel != 0UL)
        {
          VDP_COUNT(sprites);
#if VDP_COUNTERS
          sms.vdp.cnt_split += ncel - 1UL;
#endif
        }
    }
}

/*
 * The priority tiles of the band: for every window the band built, the
 * tiles of the picture it shows whose name table word carries bit 12
 * (SMSOfficialDocs.md:474-481), consecutive ones of a tile row as one
 * window of the picture cut to the window's rectangle, drawn under the
 * second palette without the background flag: colour 0 of either bank
 * passes, the fifteen others cover the sprites. The cut is the window's,
 * so a run stops where the window stops -- the scroll join, a locked
 * region, the fold of the table -- and the source is aligned as a window
 * is, the alignment columns standing left of the picture's area. The
 * first run of the band loads the second palette; the next band's first
 * window loads the identity back.
 */
static void
vdp_list_prio(void)
{
  const uint8 *nt;
  const vdp_win_t *r;
  CCB *c;
  uint32 k;
  uint32 w;
  uint32 h;
  uint32 tr;
  uint32 tr0;
  uint32 tr1;
  uint32 tc;
  uint32 tc0;
  uint32 tc1;
  uint32 r0;
  uint32 r1;
  uint32 c0;
  uint32 c1;
  uint32 f;
  uint32 first;

  nt = sms.vdp.vram + (((uint32)sms.vdp.reg[2] & 0x0EUL) << 10);
  first = 1;

  for(k = 0; k < sms.vdp.win_count; k++)
    {
      r = &sms.vdp.win[k];
      w = (uint32)r->xb - (uint32)r->xa;
      h = (uint32)r->yb - (uint32)r->ya;
      tr0 = (uint32)r->py >> 3;
      tr1 = ((uint32)r->py + h - 1UL) >> 3;
      tc0 = (uint32)r->px >> 3;
      tc1 = ((uint32)r->px + w - 1UL) >> 3;

      for(tr = tr0; tr <= tr1; tr++)
        {
          r0 = (tr << 3);
          if(r0 < (uint32)r->py)
            r0 = r->py;
          r1 = (tr << 3) + 8UL;
          if(r1 > ((uint32)r->py + h))
            r1 = (uint32)r->py + h;

          tc = tc0;
          while(tc <= tc1)
            {
              if((read16_le(nt + (((tr << 5) + tc) << 1)) & 0x1000UL) == 0UL)
                {
                  tc++;
                  continue;
                }
              c0 = tc << 3;
              while((tc <= tc1)
                    && ((read16_le(nt + (((tr << 5) + tc) << 1)) & 0x1000UL) != 0UL))
                tc++;
              c1 = tc << 3;
              if(c0 < (uint32)r->px)
                c0 = r->px;
              if(c1 > ((uint32)r->px + w))
                c1 = (uint32)r->px + w;

              f = c0 & 3UL;
              c = vdp_small(sms.vdp.decor + (r0 * VDP_PIX_ROW_BYTES) + (c0 - f),
                            VDP_PIX_ROW_BYTES / 4UL,
                            (c1 - c0) + f,r1 - r0,
                            ((int32)r->xa + (int32)(c0 - (uint32)r->px)) - (int32)f,
                            (int32)r->ya + (int32)(r0 - (uint32)r->py),
                            1L << 20,1L << 16,
                            sms.vdp.plut_prio,0UL);
              if(c != NULL)
                {
                  if(first != 0UL)
                    {
                      c->ccb_Flags |= CCB_LDPLUT;
                      first = 0;
                    }
                  VDP_COUNT(prio);
                }
            }
        }
    }
}

/*
 * The backdrop of the band, drawn last: every run of lines the display
 * was off on, as the column stretched thirty-two times wide, and, when
 * register 0 bit 5 masks the left column (SMSOfficialDocs.md:787), the
 * column itself over the band's lines, 1:1. Both painted whole, with
 * the background flag, and the first loads the identity palette back
 * after the priority runs.
 */
static void
vdp_list_backdrop(uint32 a,
                  uint32 b)
{
  CCB *c;
  uint32 y;
  uint32 ya;
  uint32 first;

  first = 1;
  y = a;
  while(y < b)
    {
      if(sms.vdp.row_off[y] == 0U)
        {
          y++;
          continue;
        }
      ya = y;
      while((y < b) && (sms.vdp.row_off[y] != 0U))
        y++;
      c = vdp_small(sms.vdp.column + (ya * VDP_COLUMN_W),VDP_COLUMN_W / 4UL,
                    VDP_COLUMN_W,y - ya,0,(int32)ya,
                    (int32)((VDP_PIX_WIDTH / VDP_COLUMN_W) << 20),1L << 16,
                    sms.vdp.plut,CCB_BGND);
      if((c != NULL) && (first != 0UL))
        {
          c->ccb_Flags |= CCB_LDPLUT;
          first = 0;
        }
    }

  if(((uint32)sms.vdp.reg[0] & 0x20UL) != 0UL)
    {
      c = vdp_small(sms.vdp.column + (a * VDP_COLUMN_W),VDP_COLUMN_W / 4UL,
                    VDP_COLUMN_W,b - a,0,(int32)a,1L << 20,1L << 16,
                    sms.vdp.plut,CCB_BGND);
      if((c != NULL) && (first != 0UL))
        c->ccb_Flags |= CCB_LDPLUT;
    }
}

/*
 * The list of the lines a to b, in the order the engine must draw it:
 * the windows of the background -- one row region, or two when register
 * 0 bit 6 holds the top two rows still (SMSOfficialDocs.md:769) and the
 * band crosses line 16, those rows taking no horizontal scroll, the rest
 * the latch -- then the sprites, then the priority tiles, then the
 * backdrop. One chain, closed on its last block, its first block loading
 * its palette; NULL when no block at all could be had for the band.
 */
static CCB *
vdp_list_build(uint32 a,
               uint32 b)
{
  uint32 ra;
  uint32 rb;
  uint32 hs;

  vdp_chain_head = NULL;
  vdp_chain_tail = NULL;
  sms.vdp.win_count = 0;

  ra = a;
  while(ra < b)
    {
      if(((((uint32)sms.vdp.reg[0] & 0x40UL) != 0UL)) && (ra < 16UL))
        {
          rb = (b < 16UL) ? b : 16UL;
          hs = 0;
        }
      else
        {
          rb = b;
          hs = sms.vdp.hscroll;
        }
      vdp_list_rows(ra,rb,hs);
      ra = rb;
    }

  vdp_list_sprites(a,b);
  vdp_list_prio();
  vdp_list_backdrop(a,b);

  if(vdp_chain_head == NULL)
    return NULL;

  vdp_chain_tail->ccb_Flags |= CCB_LAST;
  vdp_chain_head->ccb_Flags |= CCB_LDPLUT;

  return vdp_chain_head;
}

int32
vdp_list_begin(void)
{
  vdp_journal_t *j;
  uint32 i;
  uint32 n;
  uint32 last;
  uint32 line;

  /*
   * The journal undone, last write first, so that the video memory and
   * the picture stand as they stood when the frame began. Each undone
   * byte throws its decoded row away and marks its chunk, exactly as the
   * write did.
   *
   * Unless the journal overflowed: then it holds a part of the picture's
   * writes, and undoing that part would put back old bytes that a lost
   * write since overwrote -- the memory would never recover them. The
   * journal is dropped instead, nothing is undone or replayed, and the
   * picture is drawn in one band from the memory as it stands, its final
   * state (vdp.h, journal_overflow).
   */
  if(sms.vdp.journal_overflow != 0UL)
    sms.vdp.journal_count = 0;

  for(i = sms.vdp.journal_count; i > 0UL; i--)
    {
      j = &sms.vdp.journal[i - 1UL];
      if(((uint32)j->addr & VDP_JOURNAL_REG) != 0UL)
        {
          vdp_reg_apply((uint32)j->addr & 0x0FUL,(uint32)j->old);
          continue;
        }
      sms.vdp.vram[j->addr] = (uint8)j->old;
      sms.vdp.tc_valid[VDP_TC_KEY(j->addr)] = 0;
      VDP_DECOR_DIRTY[(uint32)j->addr >> 5] = 1;
    }

  vdp_decor_apply();

  /*
   * The bands: one from line 0, then one from each distinct journal line
   * after it, in line order, which the journal keeps. A write on line 0
   * opens no band -- band 0 already starts there -- an entry past the
   * picture (register 8 on line 191) opens none either, and lines past
   * the capacity are folded into the last band, counted, the picture
   * counted degraded.
   */
  sms.vdp.band_line[0] = 0;
  n = 1;
  last = 0;
  for(i = 0; i < sms.vdp.journal_count; i++)
    {
      line = sms.vdp.journal[i].line;
      if(line == last)
        continue;
      if(line >= VDP_ACTIVE_LINES)
        break;
      if(n >= VDP_LIST_BANDS)
        {
          VDP_COUNT(bands_capped);
          sms.vdp.pic_degraded = 1;
          LOG_ONCE(LOG_CAT_VDP,LOG_LVL_WARN,
                   ("decor bands capped at %lu: later lines fold into the last band",
                    (unsigned long)VDP_LIST_BANDS));
          break;
        }
      sms.vdp.band_line[n++] = line;
      last = line;
    }

  sms.vdp.band_count = n;
  sms.vdp.journal_replayed = 0;
  sms.vdp.list_used = 0;
  sms.vdp.cels_used = 0;
#if VDP_COUNTERS
  sms.vdp.cnt_list_bands += n;
#endif

  /*
   * The picture counted degraded once, whatever raised it: the flag
   * stands from the first -- a full journal or a palette past its cap
   * while the picture played, the display's refusal or the fold above
   * at the presentation -- and falls here.
   */
  if(sms.vdp.pic_degraded != 0UL)
    {
      VDP_COUNT(degraded);
      sms.vdp.pic_degraded = 0;
    }

  return (int32)n;
}

void *
vdp_list_band(int32 k)
{
  vdp_journal_t *j;
  uint32 a;
  uint32 b;
  uint32 i;
  uint32 all;

  if((k < 0) || ((uint32)k >= sms.vdp.band_count))
    {
      LOG_ONCE(LOG_CAT_VDP,LOG_LVL_ERR,
               ("decor band %ld asked of %lu",
                (long)k,(unsigned long)sms.vdp.band_count));
      return NULL;
    }

  a = sms.vdp.band_line[k];
  all = ((uint32)k + 1UL == sms.vdp.band_count) ? 1UL : 0UL;
  b = (all != 0UL) ? VDP_ACTIVE_LINES : sms.vdp.band_line[k + 1];

  /*
   * The writes up to this band's first line put back, in order; the
   * last band takes everything left inside the picture, which is what
   * the fold of the lines past the capacity means. Then the picture
   * follows them. An entry past the picture -- register 8 written on
   * line 191, dated 192 -- is put back by the last band too, but after
   * its list is built: no line of this picture shows it, the next one
   * starts from it.
   */
  i = sms.vdp.journal_replayed;
  while((i < sms.vdp.journal_count)
        && (((all != 0UL) && ((uint32)sms.vdp.journal[i].line < VDP_ACTIVE_LINES))
            || ((uint32)sms.vdp.journal[i].line <= a)))
    {
      j = &sms.vdp.journal[i];
      i++;
      if(((uint32)j->addr & VDP_JOURNAL_REG) != 0UL)
        {
          vdp_reg_apply((uint32)j->addr & 0x0FUL,(uint32)j->val);
          continue;
        }
      sms.vdp.vram[j->addr] = (uint8)j->val;
      sms.vdp.tc_valid[VDP_TC_KEY(j->addr)] = 0;
      VDP_DECOR_DIRTY[(uint32)j->addr >> 5] = 1;
    }
  sms.vdp.journal_replayed = i;

  vdp_decor_apply();

  {
    CCB *head = vdp_list_build(a,b);

    if(all != 0UL)
      {
        while(i < sms.vdp.journal_count)
          {
            j = &sms.vdp.journal[i];
            i++;
            if(((uint32)j->addr & VDP_JOURNAL_REG) != 0UL)
              vdp_reg_apply((uint32)j->addr & 0x0FUL,(uint32)j->val);
          }
        sms.vdp.journal_replayed = i;
      }

    return (void *)head;
  }
}

void
vdp_list_end(void)
{
  uint32 i;
  uint32 p;

  sms.vdp.journal_count = 0;
  sms.vdp.journal_replayed = 0;
  sms.vdp.journal_overflow = 0;

  for(i = 0; i < sms.vdp.hot_count; i++)
    {
      p = sms.vdp.hot_list[i];
      sms.vdp.hot[p] = 0;
      sms.vdp.watch[p] = vdp_watch_of(p);
    }
  sms.vdp.hot_count = 0;
}
#endif /* SMS_DECOR_CEL */

#if SMS_DECOR_CEL
/*
 * ---------------------------------------------------------------------------
 * One line of the picture with the whole of it drawn by the cel engine:
 * no pixel is written here, and nothing of the line is chosen here. What
 * a line still owes is the two sprite bits the program reads -- the
 * overflow and the collision -- which the engine does not raise, and
 * which must stand at the line they stand at on the hardware because a
 * program reads the status at lines of its choosing and a bit raised at
 * the end of the picture would change what it sees, and so the picture.
 *
 * The order of one line, against the older path's steps:
 *
 *   0. The per-line table rebuilt if a sprite table byte, register 1 or
 *      register 5 moved since it was built: the table of this line and
 *      of every line after it.
 *   1. Display off: the line is marked, and the presentation draws it in
 *      the backdrop colour over everything else. Nothing else is read,
 *      so the overflow bit falls on exactly the lines it fell on before.
 *   2. The overflow of the line, read off the table.
 *   3. The collision of the line, replayed on the admitted sprites only
 *      where two of them share a column.
 * ---------------------------------------------------------------------------
 */
static void
vdp_render_line(uint32 y)
{
  uint32 n;

  /*
   * The per-line table owed: a table byte or a register moved since it
   * was built, or the last build started past line 0 and the lines
   * before it still hold the previous picture's admission (vdp.h,
   * spr_partial) -- on line 0, the whole table is rebuilt then.
   */
  if((sms.vdp.spr_dirty != 0UL) || ((y == 0UL) && (sms.vdp.spr_partial != 0UL)))
    vdp_sprite_scan(y);

  if(((uint32)sms.vdp.reg[1] & 0x40UL) == 0UL)
    {
      sms.vdp.row_off[y] = 1;
      return;
    }
  sms.vdp.row_off[y] = 0;

  if(sms.vdp.spr_ovf_line[y] != 0U)
    {
      /*
       * Raised whether or not it already stood, as the older path does:
       * a program that never reads the status would otherwise make every
       * overflow after the first invisible. Counted once per line by
       * construction.
       */
      sms.vdp.spr_overflow = 1;
      VDP_COUNT(spr_ovf);
    }

  n = sms.vdp.spr_n[y];
#if VDP_COUNTERS
  if(n > sms.vdp.cnt_spr_max)
    sms.vdp.cnt_spr_max = n;
  if(((uint32)sms.vdp.reg[1] & 0x01UL) != 0UL)
    sms.vdp.cnt_spr_zoom = 1;
#endif

  /*
   * The two tallies of the older path kept with their meaning: a line
   * with no sprite on it, and a line that carries some and so may cost
   * a collision replay.
   */
  if(n == 0UL)
    {
      VDP_COUNT(line_fast);
      return;
    }
  VDP_COUNT(line_scratch);

  if(n >= 2UL)
    vdp_sprite_collide(y,n);
}

#else /* !SMS_DECOR_CEL */
/*
 * ---------------------------------------------------------------------------
 * One line of the background plane, composed into the index buffer at row
 * y, one byte a pixel. Called once per line of the picture from vdp_line
 * and from nowhere else; nothing is called from inside it, per pixel or
 * per tile -- the plane table replaces the loop over bits, the two flip
 * directions are two loops written out, and the recut of each stroke out
 * of the words in hand replaces the compare a fine scroll would otherwise
 * need on every pixel.
 *
 * The order of one line (TotalSMS/src/core/sms_vdp.c:787-898 for every
 * step; the document where it is named):
 *
 *   1. Display off (register 1 bit 6 clear): the row is the border colour
 *      -- entry 16 + register 7 low four bits (sms_vdp.c:327, :829) --
 *      the mask is clear, nothing else is read (sms_vdp.c:1414-1425).
 *   2. The horizontal scroll is the latch, or zero on lines 0 to 15 when
 *      register 0 bit 6 holds the top two rows still (SMSOfficialDocs.md:
 *      346-362; sms_vdp.c:796). The vertical position is the line plus
 *      the vertical latch, wrapped on the 28 rows of the table by one
 *      subtraction: the sum is below 448, so one is enough and no modulo
 *      is paid (SMSOfficialDocs.md:895; sms_vdp.c:803-824). The name
 *      table base is register 2 bits 3 to 1 (sms_vdp.c:258-271).
 *   3. The sprites of the line are chosen, before anything is composed:
 *      the choice reads the attribute table and the registers only, and
 *      what it leaves behind -- how many sprites were kept -- decides how
 *      the line is rendered. Both ways lay the strokes straight into the
 *      row through one emitter (vdp.h, VDP_EMIT8). None kept and nothing
 *      will read a priority mask, so none is written; one kept or more and
 *      the mask is laid into the scratch as the strokes are composed,
 *      since the sprite pass reads it. Two loops written out rather than
 *      one loop with a test: the stroke loop already carries more live
 *      values than the processor has registers, and a mask cursor held
 *      live across the line that does not need it would be paid in stack
 *      traffic on every stroke of the lines that are most of a screen.
 *   4. One stroke per tile column. Screen column c shows source column
 *      (c - coarse) & 31, each stroke shifted right by the fine scroll
 *      (docs/sms_gg/GGOfficialDocs.md:1394: a value of 1 moves the scene
 *      one dot to the right; sms_vdp.c:798-800, :880). Screen column -1
 *      -- the stroke whose tail becomes pixels 0 to fine-1 -- is walked
 *      when the fine scroll is not zero; its eight pixels are composed for
 *      the recut of the stroke after it and yield no pixels of their own.
 *      The mask of that stroke lands in the lead of the scratch and the
 *      mask of screen column 31 runs into its tail. When register 0 bit 7
 *      holds the right eight columns still, screen columns 24 to 31 take
 *      the unscrolled row (sms_vdp.c:838-850): a tile grain, like
 *      TotalSMS, which is at most seven pixels off at that border under a
 *      fine scroll. The name table word is read low byte first
 *      (sms_vdp.c:852): bit 12 priority, 11 palette bank, 10 vertical
 *      flip, 9 horizontal flip, 0 to 8 the pattern (sms_vdp.c:855-863).
 *      The pattern row is 4 bytes, one per plane, at pattern * 32 + row *
 *      4, the row taken as 7 - row under vertical flip (sms_vdp.c:865-872);
 *      the horizontal flip walks the eight pixels the other way. The bank
 *      adds 16 (sms_vdp.c:874-876); the mask is priority and index not
 *      zero (sms_vdp.c:893).
 *   5. Register 0 bit 5: pixels 0 to 7 take the border colour and the
 *      mask (sms_vdp.c:826-836). The colour is laid by the first stroke
 *      on both ways; the mask only where a sprite pass will read it.
 *   6. The sprites of the line, over the background in the row -- only
 *      where step 3 kept some.
 *
 * Every table is read through a pointer loaded once at the head: on a
 * processor without a cache each sms.vdp.field is a load, and a line
 * touches these hundreds of times.
 * ---------------------------------------------------------------------------
 */
static void
vdp_render_line(uint32 y)
{
  const uint8 *vram;
  const uint8 *reg;
  const uint8 *planes;
  const uint8 *nt;
  const uint8 *tile;
  const uint8 *t0;
  const uint8 *t1;
  const uint8 *t2;
  const uint8 *t3;
  uint32 *tc;
  uint8 *tcv;
  uint8 *prio;
  uint8 *eb;
  uint8 *fb;
  uint32 *ent;
  uint32 *pdw;
  uint32 *out;
  uint32 border;
  uint32 hs;
  uint32 fine;
  uint32 coarse;
  uint32 ys;
  uint32 yy;
  uint32 c;
  uint32 vsi_from;
  uint32 word;
  uint32 row;
  uint32 key;
  uint32 bankw;
  uint32 prw;
  uint32 e0;
  uint32 e1;
  uint32 rv;
  uint32 sw;
  uint32 x;
  uint32 *ow;
  uint32 *rw;
  uint32 lag;
  uint32 jsl;
  uint32 jsr;
  uint32 borderw;
  uint32 maskcol;
  uint32 pw0;
  uint32 pw1;
  uint32 gw0;
  uint32 gw1;

  reg = sms.vdp.reg;

  /*
   * The origin of the picture inside the scratch, before anything is
   * written there: the lead, which is where it stands on a line with no
   * fine scroll and on a line with the display off. Step 2 moves it left
   * by the fine scroll once that is known.
   */
  sms.vdp.line_org = VDP_LINE_LEAD;
  out = (uint32 *)(sms.vdp.pixels[0] + (y * VDP_PIX_ROW_BYTES));
  border = VDP_BACKDROP_INDEX();
  /* Four equal bytes in a word carry no byte order with them. */
  borderw = border * VDP_TC_LANE_ONE;

  if(((uint32)reg[1] & 0x40UL) == 0UL)
    {
      /*
       * Step 1. The word of four border pixels, stored down the row.
       * Nothing reads the scratch after this branch -- a line with the
       * picture off carries no sprite pass -- so nothing is written there.
       */
      for(x = 0; x < (VDP_PIX_ROW_BYTES / 4UL); x++)
        out[x] = borderw;
      return;
    }

  /* Step 2. */
  vram = sms.vdp.vram;
  planes = sms.vdp.planes;
  tc = sms.vdp.tc;
  tcv = sms.vdp.tc_valid;
  hs = ((((uint32)reg[0] & 0x40UL) != 0UL) && (y < 16UL))
       ? 0UL : sms.vdp.hscroll;
  fine = hs & 7UL;
  coarse = hs >> 3;

  /*
   * The fine scroll shifts the origin of the picture inside the scratch
   * and not the place each stroke's mask is written, so that a stroke
   * always lands on a word boundary (vdp.h, VDP_LINE_*). Everything that
   * reads the mask in picture coordinates -- the masked left column
   * below, the sprite pass -- reads the origin from here. The row of the
   * picture has no origin to move: the strokes are recut into it.
   */
  sms.vdp.line_org = VDP_LINE_LEAD - fine;
  prio = VDP_PRIO_BYTES + sms.vdp.line_org;
  ys = y + sms.vdp.vscroll;
  if(ys >= VDP_NT_LINES)
    ys -= VDP_NT_LINES;
  nt = vram + (((uint32)reg[2] & 0x0EUL) << 10);
  /* Stroke index of the first still column: screen column 24 is stroke 25. */
  vsi_from = (((uint32)reg[0] & 0x80UL) != 0UL) ? 25UL : 33UL;

  /*
   * Step 3, the sprites of this line CHOSEN, and chosen here rather than
   * after the background because it is the answer to "does this line need
   * the scratch at all". The choice reads the attribute table and the
   * registers and nothing else -- no pixel, no scratch, no name table --
   * so moving it earlier moves no dependency with it. What it must not
   * move past is the branch above: the hardware's order is selection then
   * display, and a line with the picture switched off never gets as far as
   * either, so the overflow bit falls on exactly the lines it fell on
   * before.
   *
   * The sprite post of the breakdown opens here and closes again below
   * over the composition, two wrappers carrying the same name: the
   * displacement the variant measures is the two halves together, which is
   * what it measured when they were one call. Both halves are idempotent,
   * the choice rebuilding the same kept list and the composition laying
   * the same pixels back down.
   */
  VDP_REPEAT_BEGIN(VDP_POST_SPRITES)
  vdp_select_sprites(y);
  VDP_REPEAT_END;

  /*
   * The recut every stroke of either way goes through: the picture begins
   * fine pixels into the stroke of screen column -1, so the eight picture
   * pixels of stroke c are cut out of the four words in hand -- the two of
   * stroke c - 1 and the two of stroke c. lag says which of the four the
   * picture starts in and VDP_LANE_JOIN takes the four bytes that begin
   * jsl / 8 bytes into it. Both are constant down the line. Nothing here
   * is approximate -- it is the same run of index bytes, read from another
   * rank -- and the bench proves it against a composition written the old
   * way, byte by byte, for each of the eight fine scrolls.
   *
   * Strokes 1 to 32 are the picture; stroke 0 is screen column -1 and
   * exists only under a fine scroll, where the picture starts inside it.
   * It is composed for its two words and yields no pixels of its own,
   * which is why the emit sits behind a test on c.
   */
  maskcol = (uint32)reg[0] & 0x20UL;
  lag = (VDP_LINE_LEAD - fine) >> 2;
  jsl = ((VDP_LINE_LEAD - fine) & 3UL) << 3;
  jsr = 31UL - jsl;
  pw0 = 0;
  pw1 = 0;

  if(sms.vdp.spr_count == 0UL)
    {
      /*
       * ------------------------------------------------------------------
       * A line with no sprite on it, which on a real screen is most of
       * them. Nothing will read the mask before the next line overwrites
       * what matters, so none is written: the strokes go straight from
       * the composition into the row, two words each, and that is the
       * whole of the line.
       * ------------------------------------------------------------------
       */
      VDP_COUNT(line_fast);

      /*
       * The background post, and the row cursor is armed inside the
       * wrapper with the stroke index because the cursor advances: a
       * second pass that did not arm it again would write past the row
       * instead of over it.
       */
      VDP_REPEAT_BEGIN(VDP_POST_BG)
      ow = out;
      for(c = (fine != 0UL) ? 0UL : 1UL; c <= 32UL; c++)
        {
          yy = (c >= vsi_from) ? y : ys;
          word = read16_le(nt + ((yy >> 3) << 6) + (((c - 1UL - coarse) & 31UL) << 1));
          row = yy & 7UL;
          if((word & 0x400UL) != 0UL)
            row = 7UL - row;

          key = VDP_TC_KEY_TILE(word & 0x1FFUL,row);
          ent = tc + (key << 1);

          if(tcv[key] == 0)
            {
              tile = vram + (key << 2);
              t0 = planes + ((uint32)tile[0] << 3);
              t1 = planes + VDP_PLANES_PLANE + ((uint32)tile[1] << 3);
              t2 = planes + (2UL * VDP_PLANES_PLANE) + ((uint32)tile[2] << 3);
              t3 = planes + (3UL * VDP_PLANES_PLANE) + ((uint32)tile[3] << 3);
              /*
               * Its own name for the same eight bytes the other way of
               * composing a line writes: the two decode sites are weighed
               * apart by the coverage control, and two spellings of one
               * name would make it add them up instead -- a doubling on
               * one side hidden by a fall on the other.
               */
              fb = (uint8 *)ent;
              for(x = 0; x < 8UL; x++)
                fb[x] = (uint8)((uint32)t0[x] | (uint32)t1[x]
                              | (uint32)t2[x] | (uint32)t3[x]);
              tcv[key] = 1;
              VDP_COUNT(tc_miss);
            }
          else
            {
              VDP_COUNT(tc_hit);
            }

          e0 = ent[0];
          e1 = ent[1];

          if((word & 0x200UL) != 0UL)
            {
              rv = (e0 ^ ((e0 >> 16) | (e0 << 16))) & 0xFF00FFFFUL;
              sw = ((e0 >> 8) | (e0 << 24)) ^ (rv >> 8);
              rv = (e1 ^ ((e1 >> 16) | (e1 << 16))) & 0xFF00FFFFUL;
              e0 = ((e1 >> 8) | (e1 << 24)) ^ (rv >> 8);
              e1 = sw;
            }

          bankw = ((word & 0x800UL) != 0UL) ? (16UL * VDP_TC_LANE_ONE) : 0UL;
          e0 |= bankw;
          e1 |= bankw;

          if(c != 0UL)
            {
              /*
               * The eight picture pixels this stroke carries, cut out of
               * the four words in hand: the two of the stroke before and
               * the two of this one. With no fine scroll the cut is free
               * and the two words are the two groups.
               */
              if(lag == 2UL)
                {
                  gw0 = e0;
                  gw1 = e1;
                }
              else if(lag == 1UL)
                {
                  gw0 = VDP_LANE_JOIN(pw1,e0,jsl,jsr);
                  gw1 = VDP_LANE_JOIN(e0,e1,jsl,jsr);
                }
              else
                {
                  gw0 = VDP_LANE_JOIN(pw0,pw1,jsl,jsr);
                  gw1 = VDP_LANE_JOIN(pw1,e0,jsl,jsr);
                }

              VDP_EMIT8(gw0,gw1,ow);
              ow += 2;
            }

          pw0 = e0;
          pw1 = e1;
        }

      /*
       * Step 5, after the loop and not inside it: register 0 bit 5 gives
       * pixels 0 to 7 the border colour, and those eight pixels are
       * exactly the two words of the first stroke, written over. A test
       * on the stroke index inside the loop would cost every stroke a
       * compare and, worse, keep two more values live across a loop that
       * already spills to the stack for want of registers. No mask is
       * laid down for the sprite pass: there is no sprite pass on this
       * line.
       */
      if(maskcol != 0UL)
        {
          out[0] = borderw;
          out[1] = borderw;
        }
      VDP_REPEAT_END;

      return;
    }

  VDP_COUNT(line_scratch);

  /*
   * Step 4, the line that carries sprites. The same strokes, recut into
   * the row the same way, plus the priority mask of each stroke laid into
   * the scratch as it goes: stroke c's mask stands at scratch word c * 2,
   * always aligned, and the picture's mask begins at byte line_org
   * (vdp.h, VDP_LINE_*). With no fine scroll stroke 0 is all lead and is
   * skipped -- 32 strokes, columns 0 to 31; with one, the 33 strokes run
   * from -1 to 31, the last mask ending in the tail.
   *
   * What a second pass of this post does NOT do is decode: the first pass
   * leaves every row of the line valid, so the repeat is a pass of hits
   * only. Two consequences, both to be carried with the figures rather
   * than corrected here -- correcting would mean throwing the rows away
   * between the two passes, which would measure a line that never
   * happens. First, the displacement this variant shows is the cost of
   * composing from a warm cache, so the decoding of the frame's new rows
   * is left in the residual: on an established screen that share is
   * nothing, on a change of scenery it is not, and the hit and miss counts
   * of the report are what says which. Second, those counts read high
   * under this variant, the way the overflow and collision tallies do
   * under the sprite one.
   *
   * This is the background post of the breakdown (vdp.h, VDP_REPEAT_*),
   * and the wrapper opens above the three assignments rather than at
   * the loop: the stroke index, the row cursor and the mask cursor
   * advance, so a second pass that did not set them again would write
   * past the row instead of over it. Set again, the pass writes the same bytes into the same places
   * and the picture is the one the control renders. Off, the wrapper is a
   * do/while(0) the compiler folds away.
   *
   * The row cursor carries its own name here, as the decode below does:
   * the two stroke loops are weighed apart by the coverage control, and
   * two spellings of one name would make it add them up.
   */
  VDP_REPEAT_BEGIN(VDP_POST_BG)
  c = (fine != 0UL) ? 0UL : 1UL;
  rw = out;
  pdw = sms.vdp.prio_w + (c * 2UL);

  for(; c <= 32UL; c++)
    {
      /* screen column c - 1, source column (c - 1 - coarse) & 31 */
      yy = (c >= vsi_from) ? y : ys;
      word = read16_le(nt + ((yy >> 3) << 6) + (((c - 1UL - coarse) & 31UL) << 1));
      row = yy & 7UL;
      if((word & 0x400UL) != 0UL)
        row = 7UL - row;

      /*
       * The row this column shows, by its own video address: pattern
       * times thirty-two plus row times four, divided by four. Vertical
       * flip has already chosen another row above, so it needs nothing
       * here.
       */
      key = VDP_TC_KEY_TILE(word & 0x1FFUL,row);
      ent = tc + (key << 1);

      if(tcv[key] == 0)
        {
          /*
           * First use of this row since it was written. The four planes
           * are read once and the eight indexes are written as bytes,
           * not as two assembled words: written as bytes they land in
           * the order the composition lays them back down whatever the
           * byte order of the machine, and the two words below are then
           * a copy that cannot reorder anything.
           */
          tile = vram + (key << 2);
          t0 = planes + ((uint32)tile[0] << 3);
          t1 = planes + VDP_PLANES_PLANE + ((uint32)tile[1] << 3);
          t2 = planes + (2UL * VDP_PLANES_PLANE) + ((uint32)tile[2] << 3);
          t3 = planes + (3UL * VDP_PLANES_PLANE) + ((uint32)tile[3] << 3);
          eb = (uint8 *)ent;
          for(x = 0; x < 8UL; x++)
            eb[x] = (uint8)((uint32)t0[x] | (uint32)t1[x]
                          | (uint32)t2[x] | (uint32)t3[x]);
          tcv[key] = 1;
          VDP_COUNT(tc_miss);
        }
      else
        {
          VDP_COUNT(tc_hit);
        }

      e0 = ent[0];
      e1 = ent[1];

      if((word & 0x200UL) != 0UL)
        {
          /*
           * Horizontal flip: the eight indexes the other way round, which
           * is the two words exchanged and the four bytes of each one
           * reversed. Four operations a word, against the thirty-two
           * kilobytes a second table of mirrored rows would cost on a
           * machine whose free memory is already spoken for.
           */
          rv = (e0 ^ ((e0 >> 16) | (e0 << 16))) & 0xFF00FFFFUL;
          sw = ((e0 >> 8) | (e0 << 24)) ^ (rv >> 8);
          rv = (e1 ^ ((e1 >> 16) | (e1 << 16))) & 0xFF00FFFFUL;
          e0 = ((e1 >> 8) | (e1 << 24)) ^ (rv >> 8);
          e1 = sw;
        }

      /*
       * The palette bank and the priority bit belong to the name table
       * entry, not to the row, so two columns of different banks share
       * one cached row and the bank is laid on here. Both are spread over
       * the four lanes of a word and applied to four pixels at once.
       *
       * The mask carries the priority bit ALONE, eight pixels at a time:
       * one word of ones or of zeroes, twice. Whether the pixel under it
       * is opaque is not folded in here -- the sprite pass reads that off
       * the row itself, where the index it needs already lies (vdp.h, the
       * guard on VDP_PLANES_COUNT says why its low four bits suffice).
       * Folding it in used to cost this loop a lane trick per word, some
       * fourteen instructions a stroke, on a loop that already spills for
       * want of registers. What the sprite pass pays instead is one byte
       * read per candidate pixel under a priority column, and it shows in
       * the sprite post of the breakdown, not here.
       */
      prw = ((word & 0x1000UL) != 0UL) ? VDP_TC_LANE_ONE : 0UL;
      pdw[0] = prw;
      pdw[1] = prw;
      pdw += 2;

      bankw = ((word & 0x800UL) != 0UL) ? (16UL * VDP_TC_LANE_ONE) : 0UL;
      e0 |= bankw;
      e1 |= bankw;

      if(c != 0UL)
        {
          if(lag == 2UL)
            {
              gw0 = e0;
              gw1 = e1;
            }
          else if(lag == 1UL)
            {
              gw0 = VDP_LANE_JOIN(pw1,e0,jsl,jsr);
              gw1 = VDP_LANE_JOIN(e0,e1,jsl,jsr);
            }
          else
            {
              gw0 = VDP_LANE_JOIN(pw0,pw1,jsl,jsr);
              gw1 = VDP_LANE_JOIN(pw1,e0,jsl,jsr);
            }

          VDP_EMIT8(gw0,gw1,rw);
          rw += 2;
        }

      pw0 = e0;
      pw1 = e1;
    }

  /*
   * Step 5, after the loop for the reason the short way gives: the first
   * stroke's two words take the border colour, and the mask of those
   * eight pixels is raised in picture coordinates, where the sprite pass
   * will read it.
   */
  if(maskcol != 0UL)
    {
      out[0] = borderw;
      out[1] = borderw;
      for(x = 0; x < 8UL; x++)
        prio[x] = 1;
    }
  VDP_REPEAT_END;

  /*
   * Step 6. The sprites of this line, over the background in the row. The
   * masked left column of the step above is not covered by them and does
   * not need its priority mask for that: the sprite pass leaves those
   * pixels alone by the left edge it starts from.
   *
   * The second half of the sprite post, the choice above being the first:
   * the two carry the same name and the displacement the variant measures
   * is their sum. This half is reached only with a kept list that is not
   * empty, which is what the line above the background tested. It is
   * idempotent -- it clears its taken mask on entry and lays the same
   * pixels back down, and the collision bit is raised whether or not it
   * already stood. What a second pass does move is the counters of the
   * report: the overflow and collision tallies of vdp_report read double
   * under this variant, which is why they are not read off a profiling
   * run.
   */
  VDP_REPEAT_BEGIN(VDP_POST_SPRITES)
  vdp_draw_sprites(y);
  VDP_REPEAT_END;
}
#endif /* !SMS_DECOR_CEL */

int32
vdp_init(void)
{
  int32 i;
  int32 b;
  int32 px;
  int32 py;
  uint32 x;
  uint32 pre0_calc;
  uint32 pre1_calc;
  CCB *cel;
  uint32 probe;

  /*
   * The lane order the preprocessor picked (vdp.h, VDP_LANE_MSB_FIRST),
   * held against the machine actually running. Both ways of rendering a
   * line read pixel indexes out of a word, and a wrong answer here does
   * not fail: it draws every group of four pixels backwards, over the
   * whole picture, which is a slow and confusing thing to diagnose. One
   * store and one load, once at boot, buys the refusal instead.
   */
  probe = 0x01020304UL;
  if((uint32)(((const uint8 *)&probe)[0])
     != (VDP_LANE_MSB_FIRST ? 0x01UL : 0x04UL))
    {
      LOG_ERR(LOG_CAT_VDP,
              ("init failed: the byte order built for is not the machine's"));
      return VDP_ERR_LANE_ORDER;
    }

  if(vdp_vram_block == NULL)
    {
      vdp_vram_block = (uint8 *)sys_alloc("vdp_vram",
                                          (int32)VDP_VRAM_SIZE,
                                          MEMTYPE_ANY | MEMTYPE_FILL);
      if(vdp_vram_block == NULL)
        {
          /*
           * sys_alloc has said how much was asked for and why it failed;
           * what is added is what the block was for.
           */
          LOG_ERR(LOG_CAT_VDP,("init failed: no memory for the vram"));
          return VDP_ERR_NO_VRAM;
        }
    }

  sms.vdp.vram = vdp_vram_block;

  /*
   * The index buffers, in DRAM because DRAM is the preferred source for
   * cel data (docs/3do/3DO_Development_Notes.md:38), sized by the
   * calculated constants of vdp.h and taken once each, like the video
   * memory above. A refusal is its own failure code: the caller paints a
   * screen naming the pixel buffer, not the video ram.
   */
  for(b = 0; b < (int32)SMS_VDP_BUFFERS; b++)
    {
      if(vdp_pixels_block[b] == NULL)
        {
          vdp_pixels_block[b] = (uint8 *)sys_alloc("vdp_pixels",
                                                   (int32)VDP_PIX_BUF_BYTES,
                                                   MEMTYPE_DRAM | MEMTYPE_FILL);
          if(vdp_pixels_block[b] == NULL)
            {
              LOG_ERR(LOG_CAT_VDP,
                      ("init failed: no memory for the pixel buffer"));
              return VDP_ERR_NO_PIXELS;
            }
        }
      sms.vdp.pixels[b] = vdp_pixels_block[b];
    }

  /*
   * The bit plane table, taken once like the blocks above and shown in
   * the boot footprint like them: a static table would be eight kilobytes
   * the memory total could not account for.
   */
  if(vdp_planes_block == NULL)
    {
      vdp_planes_block = (uint8 *)sys_alloc("vdp_planes",
                                            (int32)VDP_PLANES_BYTES,
                                            MEMTYPE_ANY | MEMTYPE_FILL);
      if(vdp_planes_block == NULL)
        {
          LOG_ERR(LOG_CAT_VDP,("init failed: no memory for the plane table"));
          return VDP_ERR_NO_PLANES;
        }
    }

  sms.vdp.planes = vdp_planes_block;

  /*
   * The decoded row cache, in DRAM beside the index buffers and for the
   * same reason. Its size is calculated from the size of the video memory
   * (vdp.h, VDP_TC_*) and refused at compile time if it ever grew past
   * what it was granted, so what is asked for here cannot drift from what
   * was budgeted.
   */
  if(vdp_tc_block == NULL)
    {
      vdp_tc_block = (uint32 *)sys_alloc("vdp_tilecache",
                                         (int32)VDP_TC_TOTAL_BYTES,
                                         MEMTYPE_DRAM | MEMTYPE_FILL);
      if(vdp_tc_block == NULL)
        {
          LOG_ERR(LOG_CAT_VDP,
                  ("init failed: no memory for the decoded row cache"));
          return VDP_ERR_NO_TILECACHE;
        }
    }

  sms.vdp.tc = vdp_tc_block;
  sms.vdp.tc_valid = (uint8 *)vdp_tc_block + VDP_TC_BYTES;

#if SMS_DECOR_CEL
  /*
   * The page of the background picture and its window blocks, in DRAM
   * like the buffers the engine reads, asked for whole: the allocator
   * spends in pages of this size whatever is asked, so the page is what
   * the boot footprint carries either way. The block size the page was
   * sized for is the console's (vdp.h, VDP_CCB_BYTES); the size this
   * build really has is held against the page here, so that a host with
   * wider pointers is refused with a reason rather than overrun.
   */
  if(vdp_decor_block == NULL)
    {
      if((VDP_DECOR_BYTES + (VDP_LIST_WINDOWS * sizeof(CCB)) + VDP_COLUMN_BYTES)
         > VDP_DECOR_PAGE)
        {
          LOG_ERR(LOG_CAT_VDP,
                  ("init failed: %lu window blocks of %lu bytes and the backdrop column do not follow the picture in one page",
                   (unsigned long)VDP_LIST_WINDOWS,(unsigned long)sizeof(CCB)));
          return VDP_ERR_NO_DECOR;
        }
      vdp_decor_block = (uint8 *)sys_alloc("vdp_decor",
                                           (int32)VDP_DECOR_PAGE,
                                           MEMTYPE_DRAM | MEMTYPE_FILL);
      if(vdp_decor_block == NULL)
        {
          LOG_ERR(LOG_CAT_VDP,
                  ("init failed: no memory for the background picture"));
          return VDP_ERR_NO_DECOR;
        }
    }

  sms.vdp.decor = vdp_decor_block;
  sms.vdp.windows = (void *)(vdp_decor_block + VDP_DECOR_BYTES);
  /* The column after the blocks, on a word: a cel source is a word address. */
  sms.vdp.column = vdp_decor_block
                 + (((VDP_DECOR_BYTES + (VDP_LIST_WINDOWS * sizeof(CCB))) + 3UL) & ~3UL);

  /*
   * The page of the sprite sheet and the small cel blocks, on the same
   * terms. The page was sized for the console's block (vdp.h, the guard
   * on VDP_SPRITE_PAGE); a host with wider pointers has wider blocks,
   * and there the block is asked for as large as the reserve needs
   * rather than refused, since the reserve's count is the contract and
   * the page is the console's grain.
   */
  if(vdp_sprite_block == NULL)
    {
      uint32 need = VDP_SHEET_BYTES + (VDP_LIST_CELS * sizeof(CCB));

      if(need < VDP_SPRITE_PAGE)
        need = VDP_SPRITE_PAGE;
      vdp_sprite_block = (uint8 *)sys_alloc("vdp_sprites",
                                            (int32)need,
                                            MEMTYPE_DRAM | MEMTYPE_FILL);
      if(vdp_sprite_block == NULL)
        {
          LOG_ERR(LOG_CAT_VDP,
                  ("init failed: no memory for the sprite sheet"));
          return VDP_ERR_NO_SPRITES;
        }
    }

  sms.vdp.sheet = vdp_sprite_block;
  sms.vdp.cels = (void *)(vdp_sprite_block + VDP_SHEET_BYTES);
#endif

  /*
   * Cleared here on every init, not only on the first: the allocator's
   * fill flag zeroes the block the one time it is taken, and a second init
   * over a block already used would otherwise leave the previous run's
   * tiles in place while the registers and the colours start over.
   */
  for(i = 0; i < (int32)VDP_VRAM_SIZE; i++)
    sms.vdp.vram[i] = 0;

  for(b = 0; b < (int32)SMS_VDP_BUFFERS; b++)
    {
      for(x = 0; x < VDP_PIX_BUF_BYTES; x++)
        sms.vdp.pixels[b][x] = 0;
    }

  for(i = 0; i < VDP_CRAM_SIZE; i++)
    sms.vdp.cram[i] = 0;

  for(i = 0; i < (int32)VDP_LINE_WORDS; i++)
    sms.vdp.prio_w[i] = 0;

  sms.vdp.line_org = VDP_LINE_LEAD;

  /*
   * Every row invalid, on every init and not only on the first: the video
   * memory above has just been cleared, so a row left standing from a
   * previous run would describe tiles that no longer exist. The words
   * themselves are not cleared -- a row is written before it is read.
   */
  for(i = 0; i < (int32)VDP_TC_ROWS; i++)
    sms.vdp.tc_valid[i] = 0;

  for(i = 0; i < VDP_REG_COUNT; i++)
    sms.vdp.reg[i] = vdp_reg_power_on[i];

  sms.vdp.addr = 0;
  sms.vdp.code = 0;
  sms.vdp.ctrl_word = 0;
  sms.vdp.latch = 0;
  sms.vdp.read_buf = 0;
  sms.vdp.vcount = 0;
  sms.vdp.line_ctr = 0xFF;
  sms.vdp.frame_pending = 0;
  sms.vdp.line_pending = 0;
  sms.vdp.spr_overflow = 0;
  sms.vdp.spr_collision = 0;
  sms.vdp.spr_count = 0;

  for(i = 0; i < (int32)VDP_SPR_MAX_ON_LINE; i++)
    {
      sms.vdp.spr_sel[i] = 0;
      sms.vdp.spr_top[i] = 0;
    }

  for(i = 0; i < (int32)(VDP_PIX_WIDTH / 4UL); i++)
    sms.vdp.spr_taken[i] = 0;

  /* Both scroll latches from the power-on table: 0 and 0. */
  sms.vdp.vscroll = sms.vdp.reg[9];
  sms.vdp.hscroll = sms.vdp.reg[8];

#if SMS_DECOR_CEL
  /*
   * The picture cleared and every table of its upkeep at its start, on
   * every init like the video memory above: no tile converted, no
   * pattern shown, nothing dirty, nothing journaled, and one full sweep
   * owed to the first presentation, which converts the 896 tiles of the
   * zeroed table over the zeroed memory. The name table's first chunk
   * follows the power-on register 2.
   */
  {
    uint32 *dc = (uint32 *)sms.vdp.decor;

    for(x = 0; x < (VDP_DECOR_BYTES / 4UL); x++)
      dc[x] = 0;
  }
  for(x = 0; x < VDP_NT_TILES; x++)
    sms.vdp.decor_word[x] = 0xFFFFU;
  for(x = 0; x < VDP_CHUNKS; x++)
    {
      sms.vdp.refs[x] = 0;
      sms.vdp.hot[x] = 0;
      sms.vdp.pat_dirty[x] = 0;
      VDP_DECOR_DIRTY[x] = 0;
    }
  sms.vdp.hot_count = 0;
  sms.vdp.decor_sweep_all = 1;
  sms.vdp.nt_chunk0 = (((uint32)sms.vdp.reg[2] & 0x0EUL) << 10) >> 5;
  sms.vdp.journal_count = 0;
  sms.vdp.journal_replayed = 0;
  sms.vdp.journal_overflow = 0;
  sms.vdp.band_count = 0;
  sms.vdp.list_used = 0;
  sms.vdp.pal_count = 0;
  sms.vdp.pal_capped = 0;
  sms.vdp.clut_seg_count = 0;
  sms.vdp.clut_seg_prev = 0;
  for(x = 0; x < VDP_PAL_SEGMENTS; x++)
    {
      sms.vdp.pal_line[x] = 0;
      sms.vdp.clut_seg_line[x] = 0;
    }

  /*
   * The sprites: the sheet cleared and every place stale, no pattern
   * named, the per-line table owed to the first line -- the attribute
   * table follows the power-on register 5 -- no line off, no cel taken,
   * the second palette the identity with entry 16 at 000, and the
   * backdrop column filled from the power-on register 7. The watch bytes
   * are rebuilt once both tables have their first chunk.
   */
  {
    uint32 *sh = (uint32 *)sms.vdp.sheet;

    for(x = 0; x < (VDP_SHEET_BYTES / 4UL); x++)
      sh[x] = 0;
  }
  for(x = 0; x < VDP_CHUNKS; x++)
    {
      sms.vdp.sheet_valid[x] = 0;
      sms.vdp.spr_named[x] = 0;
    }
  sms.vdp.spr_named_count = 0;
  sms.vdp.sat_base = ((uint32)sms.vdp.reg[5] & 0x7EUL) << 7;
  sms.vdp.sat_chunk0 = sms.vdp.sat_base >> 5;
  sms.vdp.spr_dirty = 1;
  sms.vdp.spr_partial = 0;
  sms.vdp.pic_degraded = 0;
  for(x = 0; x < VDP_SPR_COUNT; x++)
    sms.vdp.sat_top[x] = 0;
  for(x = 0; x < VDP_ACTIVE_LINES; x++)
    {
      sms.vdp.spr_adm[x][0] = 0;
      sms.vdp.spr_adm[x][1] = 0;
      for(i = 0; i < (int32)VDP_SPR_MAX_ON_LINE; i++)
        sms.vdp.spr_idx[x][i] = 0;
      sms.vdp.spr_n[x] = 0;
      sms.vdp.spr_ovf_line[x] = 0;
      sms.vdp.row_off[x] = 0;
    }
  sms.vdp.cels_used = 0;
  sms.vdp.win_count = 0;
  for(i = 0; i < VDP_PLUT_ENTRIES; i++)
    sms.vdp.plut_prio[i] = (uint16)MakeRGB15(i,i,i);
  sms.vdp.plut_prio[16] = 0;
  vdp_column_fill();
  vdp_decor_watch_rebuild();
#endif

#if VDP_COUNTERS
  sms.vdp.cnt_reg_w = 0;
  sms.vdp.cnt_vram_w = 0;
  sms.vdp.cnt_cram_w = 0;
  sms.vdp.cnt_cram_mid = 0;
  sms.vdp.cnt_clut_upd = 0;
  sms.vdp.cnt_status_r = 0;
  sms.vdp.cnt_data_r = 0;
  sms.vdp.cnt_vcnt_r = 0;
  sms.vdp.cnt_hcnt_r = 0;
  sms.vdp.cnt_reg_oob = 0;
  sms.vdp.cnt_mode = 0;
  sms.vdp.mode_last = 0;
  sms.vdp.cnt_height = 0;
  sms.vdp.height_last = 0;
  sms.vdp.cnt_spr_max = 0;
  sms.vdp.cnt_spr_ovf = 0;
  sms.vdp.cnt_spr_col = 0;
  sms.vdp.cnt_spr_zoom = 0;
  sms.vdp.cnt_backdrop = 0;
  sms.vdp.cnt_tc_hit = 0;
  sms.vdp.cnt_tc_miss = 0;
  sms.vdp.cnt_tc_inval = 0;
  sms.vdp.cnt_line_fast = 0;
  sms.vdp.cnt_line_scratch = 0;
#if SMS_DECOR_CEL
  sms.vdp.cnt_decor_tiles = 0;
  sms.vdp.cnt_list_windows = 0;
  sms.vdp.cnt_list_bands = 0;
  sms.vdp.cnt_bands_capped = 0;
  sms.vdp.cnt_journal_full = 0;
  sms.vdp.cnt_list_refused = 0;
  sms.vdp.cnt_reg_mid = 0;
  sms.vdp.cnt_scroll_mid = 0;
  sms.vdp.cnt_reg_journal = 0;
  sms.vdp.cnt_pal_seg = 0;
  sms.vdp.cnt_pal_capped = 0;
  sms.vdp.cnt_degraded = 0;
  sms.vdp.cnt_cels = 0;
  sms.vdp.cnt_sprites = 0;
  sms.vdp.cnt_split = 0;
  sms.vdp.cnt_prio = 0;
  sms.vdp.cnt_cels_refused = 0;
#endif
  vdp_irq_seen = 0;
  vdp_backdrop_said = 0;
#endif

  /*
   * The colour table, then the screen table as the conversion of the
   * colour memory just zeroed -- through the same rebuild the frame loop
   * triggers, so there is one conversion in this file and not two -- with
   * the flag down, since what the table holds is what the memory holds.
   * Then the cel's palette as the identity, entry n at (n, n, n), the one
   * and only time it is written. Then the plane table: for plane p and
   * byte value v, pixel x takes bit 7 - x of v at weight p
   * (SMSOfficialDocs.md:505-578, bit 7 is the left pixel). Refilled on
   * every init: cheap, and a table that is rebuilt cannot be stale.
   */
  for(i = 0; i < 64; i++)
    vdp_clut_rgb[i] = MakeCLUTColorEntry(0,
                                         vdp_level[i & 3],
                                         vdp_level[(i >> 2) & 3],
                                         vdp_level[(i >> 4) & 3]);

  vdp_clut_build(sms.vdp.clut,sms.vdp.cram);
  sms.vdp.cram_dirty = 0;

  for(i = 0; i < VDP_PLUT_ENTRIES; i++)
    sms.vdp.plut[i] = (uint16)MakeRGB15(i,i,i);

  for(i = 0; i < (int32)VDP_PLANES_COUNT; i++)
    {
      for(b = 0; b < 256; b++)
        {
          for(x = 0; x < 8UL; x++)
            sms.vdp.planes[((uint32)i * VDP_PLANES_PLANE) + ((uint32)b << 3) + x]
              = (uint8)((((uint32)b >> (7UL - x)) & 1UL) << (uint32)i);
        }
    }

  /*
   * This file's own calculation of the preamble words. Each of the three
   * counts carries a prefetch correction the hardware header states --
   * one line off the vertical count, two words off the row offset, one
   * pixel off the pixel count (include/3do/hardware.h:210, :232, :234).
   *
   * Two details this pair had wrong, and a capture paid for each. The row
   * offset has TWO fields in the second word, at two different bit
   * positions: the eight bit one at 24 (include/3do/hardware.h:214, :221)
   * and the ten bit one at 16 (:215, :222). A depth BELOW eight bits is
   * read through the eight bit one; the wide field belongs to eight bits
   * and to sixteen alike -- "bits 25-16 for the word offset for an 8 or
   * 16 bpp cel" (src_exemple/lrex/main.c:290-291, and the same pairing
   * spelled as code in src_exemple/3d_3do_logo/main.c:79-82, where a row
   * offset counted for eight bit pixels is written through WOFFSET10).
   * The boundary sits between six and eight, not after eight: this file
   * learned the eight bit field at six bits, then read it as "eight or
   * less" and was caught by the arbiter at the end of this function when
   * the depth became eight. Written into the wrong field, the field the
   * engine reads stays at zero -- a stride of two words where the rows
   * are sixty-four apart, which is a picture sheared into thin diagonals
   * rather than a picture with wrong colours. And the linear bit (:193)
   * is not a coded cel's: the library sets neither of these on this very
   * block, and the debug line at the end of this function shows the two
   * spellings landing on the same two words.
   */
  pre0_calc = ((VDP_ACTIVE_LINES - PRE0_VCNT_PREFETCH) << PRE0_VCNT_SHIFT)
            | PRE0_BPP_8;
  pre1_calc = (((VDP_PIX_ROW_BYTES / 4UL) - PRE1_WOFFSET_PREFETCH)
               << PRE1_WOFFSET10_SHIFT)
            | PRE1_TLLSB_PDC0
            | (VDP_PIX_WIDTH - PRE1_TLHPCNT_PREFETCH);

  /*
   * Where the picture sits, computed on every init and not only on the
   * one that builds the cel: the offset is read through vdp_view after
   * either. See the cel's position below for what it is.
   */
  px = (sys_width() - (int32)VDP_PIX_WIDTH) / 2;
  py = (sys_height() - (int32)VDP_ACTIVE_LINES) / 2;
  if(px < 0)
    px = 0;
  if(py < 0)
    py = 0;
  sms.vdp.view_x = px;
  sms.vdp.view_y = py;
  vdp_view_fix();

  if(vdp_cel_block == NULL)
    {
      /*
       * The library first. A coded cel takes any depth up to eight, and
       * at eight CreateCel allocates a zeroed palette of its own only
       * because the coded option is set; it sets neither the background
       * nor the load-palette flag (docs/3do/3do_portfolio_2.5.md:5425-5455).
       * Its palette is left where it lies -- a small init-time allocation
       * of the library, outside the boot total -- and the cel's palette
       * pointer is repointed on this module's own palette below.
       *
       * The palette is the same thirty-two entries the picture had at six
       * bits: an index the render emits is five bits wide at either depth
       * -- the loop never writes above 31 -- so a byte changes how an
       * index is wrapped and not what it means. A console run of the
       * probe that priced this format showed the thirty-two colours, and
       * the ruler it drew, at this depth.
       */
      vdp_cel_block = CreateCel((int32)VDP_PIX_WIDTH,
                                (int32)VDP_ACTIVE_LINES,
                                (int32)VDP_PIX_BPP,
                                CREATECEL_CODED,
                                (void *)sms.vdp.pixels[0]);
      if(vdp_cel_block != NULL)
        {
          /*
           * The library's preamble words, read before anything here
           * touches the block, and then LEFT IN PLACE: the library knows
           * what the hardware reads, and a hand calculation pasted over
           * its answer is a fault waiting for a capture to show it. The
           * calculated pair below exists to be printed beside these two,
           * and to serve the manual way, which has no library to ask.
           */
          vdp_pre0_lib = vdp_cel_block->ccb_PRE0;
          vdp_pre1_lib = vdp_cel_block->ccb_PRE1;
        }
      else
        {
          /*
           * The fallback, never fatal: the same block filled by hand,
           * starting from the flag set CreateCel documents
           * (3do_portfolio_2.5.md:5444-5447), so both ways end on the
           * same final word and the init line can prove it.
           */
          vdp_cel_manual = 1;
          vdp_cel_block = &vdp_ccb_manual;
          vdp_cel_block->ccb_Flags = CCB_LAST | CCB_NPABS | CCB_SPABS
                                   | CCB_PPABS | CCB_LDSIZE | CCB_LDPRS
                                   | CCB_LDPPMP | CCB_CCBPRE | CCB_YOXY
                                   | CCB_USEAV | CCB_NOBLK | CCB_ACE
                                   | CCB_ACW | CCB_ACCW;
        }

      cel = vdp_cel_block;

      /*
       * Both ways from here, and these flags are the reason a library
       * cel cannot be used as it comes. Without the load-palette flag the
       * coded cel reads whatever palette the hardware last loaded, not
       * this module's. The background flag depends on what the buffer
       * carries: the whole picture on the older path, where a 15 bit
       * colour of 000 would otherwise be transparent
       * (3do_portfolio_2.5.md:3784) and the black band would show
       * whatever lay under it; the sprite layer alone when the engine
       * draws the background, where a zero pixel MUST be transparent so
       * that the windows drawn before it show through -- the flag is
       * left off, and the picture check reads it off the block.
       */
#if SMS_DECOR_CEL
      cel->ccb_Flags |= CCB_LDPLUT;
#else
      cel->ccb_Flags |= CCB_BGND | CCB_LDPLUT;
#endif

      /*
       * These two the library documents as set (3do_portfolio_2.5.md:
       * 5444-5447), and they are forced anyway rather than assumed: the
       * preambles below are only read if the block says it carries them,
       * and a single cel must say it is the last of its list. The next
       * pointer is nulled with them, on both ways, for the same reason.
       */
      cel->ccb_Flags |= CCB_CCBPRE | CCB_LAST;
      cel->ccb_NextPtr = NULL;

      cel->ccb_SourcePtr = (CelData *)sms.vdp.pixels[0];
      cel->ccb_PLUTPtr = sms.vdp.plut;
      cel->ccb_PIXC = 0x1F001F00UL;

      /*
       * The preambles. On the library way they are the library's and stay
       * so -- see the capture above. Only the manual way, which has no
       * library answer to keep, takes the calculated pair.
       */
      if(vdp_cel_manual)
        {
          cel->ccb_PRE0 = pre0_calc;
          cel->ccb_PRE1 = pre1_calc;
        }

      /*
       * Centred in whatever raster the console built, never in a constant
       * one: a taller raster centres lower on its own. Clamped at zero
       * because a negative offset is off-raster on this machine. The
       * offset is NOT the cel's position: it is the origin of the clip
       * rectangle the frame loop sets on every screen (vdp_view), and the
       * folio takes (0,0) as the top-left corner of that rectangle
       * (docs/3do/3do_portfolio_2.5.md:10984), so the cel stands at
       * (0,0) and lands at the offset. Positions are 16.16, the
       * horizontal step 12.20, the vertical 16.16 -- a 1:1 mapping at no
       * processor cost (src_exemple_video_player/renderer.c:56-86).
       */
      cel->ccb_XPos = 0;
      cel->ccb_YPos = 0;
      cel->ccb_HDX = 1L << 20;
      cel->ccb_HDY = 0;
      cel->ccb_VDX = 0;
      cel->ccb_VDY = 1L << 16;
      cel->ccb_HDDX = 0;
      cel->ccb_HDDY = 0;
      cel->ccb_Width = (int32)VDP_PIX_WIDTH;
      cel->ccb_Height = (int32)VDP_ACTIVE_LINES;
    }

  sms.vdp.cel = (void *)vdp_cel_block;

  /*
   * Two lines. The first names what was built -- one mode, one picture
   * size -- and the profile the cartridge boot fixed, through the one
   * spelling of it. The second says who owns the maskable line from here
   * on, because the processor's own trace used to name a test source for
   * it, and a reader of an old trace beside a new one needs the change
   * said in the trace itself.
   */
  LOG_INFO(LOG_CAT_VDP,("init ok mode=4 view=%ldx%ld profile=%s",
                        (long)sms.vdp.view_w,(long)sms.vdp.view_h,
                        cart_system_name(sms.cart.system)));
  LOG_INFO(LOG_CAT_VDP,("irq line owner=vdp (test source keeps nmi only)"));
  LOG_INFO(LOG_CAT_VDP,("palette via screen clut (%lu entries + background), cel plut identity",
                        (unsigned long)VDP_CRAM_SIZE));

  /*
   * The row cache as built: how many rows the video memory can hold, what
   * the block costs in all, and the two facts that make it correct --
   * a row is decoded at its first use and thrown away when the video
   * memory under it is written.
   */
  LOG_INFO(LOG_CAT_VDP,
           ("tile cache rows=%lu bytes=%lu (decoded once, invalidated on vram write)",
            (unsigned long)VDP_TC_ROWS,
            (unsigned long)VDP_TC_TOTAL_BYTES));

#if SMS_DECOR_CEL
  /*
   * The background as the engine draws it: the picture's size, the most
   * bands a presentation cuts it into and the writes the journal holds.
   */
  LOG_INFO(LOG_CAT_VDP,
           ("decor via cel windows picture=%lux%lu bands=%lu journal=%lu",
            (unsigned long)VDP_PIX_WIDTH,
            (unsigned long)VDP_DECOR_LINES,
            (unsigned long)VDP_LIST_BANDS,
            (unsigned long)VDP_JOURNAL_ENTRIES));

  /*
   * The sprites and the priority tiles as the engine draws them: the
   * sheet's size and the reserve of small cels a presentation may take.
   */
  LOG_INFO(LOG_CAT_VDP,
           ("sprites and priority tiles via cel list sheet=%lux%lu cels=%lu",
            (unsigned long)VDP_SHEET_W,
            (unsigned long)VDP_SHEET_LINES,
            (unsigned long)VDP_LIST_CELS));
#endif

  /*
   * Two more, for the drawing side. The first carries the whole cel as
   * built: size, depth, palette length, buffer count, the final flag word
   * with the background bit read back out of it, and which way the block
   * was made. The second carries the frame: position and the two scale
   * steps, in their raw fixed-point form.
   */
  LOG_INFO(LOG_CAT_VDP,
           ("cel ok %lux%lu bpp=%lu coded plut=%lu bufs=%lu flags=0x%lx bgnd=%lu via=%s",
            (unsigned long)VDP_PIX_WIDTH,
            (unsigned long)VDP_ACTIVE_LINES,
            (unsigned long)VDP_PIX_BPP,
            (unsigned long)VDP_PLUT_ENTRIES,
            (unsigned long)SMS_VDP_BUFFERS,
            (unsigned long)vdp_cel_block->ccb_Flags,
            (unsigned long)((vdp_cel_block->ccb_Flags & CCB_BGND) != 0),
            vdp_cel_manual ? "manual" : "createcel"));
  LOG_INFO(LOG_CAT_VDP,
           ("cel pos=%ld,%ld in view at %ld,%ld hdx=0x%lx vdy=0x%lx",
            (long)(vdp_cel_block->ccb_XPos >> 16),
            (long)(vdp_cel_block->ccb_YPos >> 16),
            (long)sms.vdp.view_x,
            (long)sms.vdp.view_y,
            (unsigned long)vdp_cel_block->ccb_HDX,
            (unsigned long)vdp_cel_block->ccb_VDY));

  /*
   * The preamble arbiter, at debug level: what the library computed for
   * this block beside what this file computes for it. Equal pairs retire
   * the calculation as a suspect; different pairs name, bit by bit, what
   * the hand had wrong. Two whole spellings, so the manual way -- which
   * has no library pair to show -- cannot be mistaken for agreement.
   *
   * And a disagreement is raised, not left to be noticed: it is the one
   * defect that draws a garbled picture with no error anywhere. The
   * composition writes 256 bytes a row and nothing but this pair tells
   * the engine to step by 256. On the library way the block carries the
   * library's words, which are the ones drawn, so the warning names a
   * wrong calculation rather than a wrong picture -- but the manual way
   * would draw the calculation, and the warning is what says it was
   * checked against the library while there was one to ask.
   */
  if(vdp_cel_manual)
    LOG_DBG(LOG_CAT_VDP,
            ("cel pre manual calc=0x%08lx 0x%08lx",
             (unsigned long)pre0_calc,
             (unsigned long)pre1_calc));
  else
    {
      LOG_DBG(LOG_CAT_VDP,
              ("cel pre lib=0x%08lx 0x%08lx calc=0x%08lx 0x%08lx",
               (unsigned long)vdp_pre0_lib,
               (unsigned long)vdp_pre1_lib,
               (unsigned long)pre0_calc,
               (unsigned long)pre1_calc));

      if((vdp_pre0_lib != pre0_calc) || (vdp_pre1_lib != pre1_calc))
        LOG_WARN(LOG_CAT_VDP,
                 ("cel preamble disagrees: lib=0x%08lx 0x%08lx "
                  "calc=0x%08lx 0x%08lx -- the row offset or the depth "
                  "this file computes is not what the engine reads",
                  (unsigned long)vdp_pre0_lib,
                  (unsigned long)vdp_pre1_lib,
                  (unsigned long)pre0_calc,
                  (unsigned long)pre1_calc));
    }

  return 0;
}

void *
vdp_cel(void)
{
  return sms.vdp.cel;
}

void
vdp_view(int32 *x,
         int32 *y,
         int32 *w,
         int32 *h)
{
  *x = sms.vdp.view_x - sms.vdp.pic_x;
  *y = sms.vdp.view_y - sms.vdp.pic_y;
  *w = sms.vdp.view_w;
  *h = sms.vdp.view_h;
}

void
vdp_view_fix(void)
{
  /*
   * The window is the list's: its cels take the shift (vdp_cel_fill).
   * The older path draws its one cel of the whole picture at the origin
   * of the rectangle and knows nothing of a window, so it keeps the
   * full view on either profile, as it did.
   */
#if SMS_DECOR_CEL
  if(sms.cart.system == SYS_GG)
    {
      sms.vdp.view_w = VDP_GG_VIEW_W;
      sms.vdp.view_h = VDP_GG_VIEW_H;
      sms.vdp.pic_x = -VDP_GG_VIEW_X;
      sms.vdp.pic_y = -VDP_GG_VIEW_Y;
    }
  else
#endif
    {
      sms.vdp.view_w = (int32)VDP_PIX_WIDTH;
      sms.vdp.view_h = (int32)VDP_ACTIVE_LINES;
      sms.vdp.pic_x = 0;
      sms.vdp.pic_y = 0;
    }

  /*
   * The profile and the rectangle it draws in, once: the window's size
   * and where it sits on the bitmap. The Game Gear's colours are the
   * Master System's table for now (vdp.h, colour memory), and a Game
   * Gear program may show wrong colours through it; the window is
   * right.
   */
  LOG_INFO(LOG_CAT_VDP,("profile=%s view=%ldx%ld clip=%ld,%ld,%ld,%ld",
                        cart_system_name(sms.cart.system),
                        (long)sms.vdp.view_w,(long)sms.vdp.view_h,
                        (long)(sms.vdp.view_x - sms.vdp.pic_x),
                        (long)(sms.vdp.view_y - sms.vdp.pic_y),
                        (long)sms.vdp.view_w,(long)sms.vdp.view_h));
}

uint16
vdp_backdrop(void)
{
  uint32 n;

  n = VDP_BACKDROP_INDEX();
  return (uint16)MakeRGB15(n,n,n);
}

int32
vdp_clut_take(void)
{
  int32 changed;

  changed = 0;
  if(sms.vdp.cram_dirty != 0UL)
    {
      sms.vdp.cram_dirty = 0;
      vdp_clut_build(sms.vdp.clut,sms.vdp.cram);
      VDP_COUNT(clut_upd);
      changed = 1;
    }

#if SMS_DECOR_CEL
  /*
   * The segments of the picture closed: one table each, the last one
   * the table just rebuilt (a segment exists only after a byte changed,
   * so it was), the bitmap line of each, and the count -- which, moving
   * from or to zero or between two values, is a change of the screens'
   * lists whether or not a byte moved this picture.
   */
  {
    uint32 k;
    uint32 n;
    uint32 i;

    n = sms.vdp.pal_count;
    for(k = 0; k < n; k++)
      {
        if(k + 1UL < n)
          vdp_clut_build(sms.vdp.clut_seg[k],sms.vdp.pal_cram[k]);
        else
          {
            for(i = 0; i < VDP_CLUT_ENTRIES; i++)
              sms.vdp.clut_seg[k][i] = sms.vdp.clut[i];
          }
        sms.vdp.clut_seg_line[k] = (uint8)(sms.vdp.view_y
                                           + (int32)sms.vdp.pal_line[k]);
      }
    if(n != sms.vdp.clut_seg_prev)
      changed = 1;
    sms.vdp.clut_seg_prev = n;
    sms.vdp.clut_seg_count = n;
    sms.vdp.pal_count = 0;
    sms.vdp.pal_capped = 0;
  }
#endif

  return changed;
}

const uint32 *
vdp_clut(void)
{
  return sms.vdp.clut;
}

#if SMS_DECOR_CEL
uint32
vdp_clut_segments(const uint32 **tables,
                  const uint8  **lines)
{
  *tables = &sms.vdp.clut_seg[0][0];
  *lines = sms.vdp.clut_seg_line;
  return sms.vdp.clut_seg_count;
}

void
vdp_clut_refused(void)
{
  sms.vdp.pic_degraded = 1;
  LOG_ONCE(LOG_CAT_VDP,LOG_LVL_WARN,
           ("palette per line refused by the display: the picture takes its last table"));
}
#endif

void
vdp_backdrop_repainted(void)
{
#if VDP_COUNTERS
  sms.vdp.cnt_backdrop++;

  /*
   * Once per report window, and the flag is what bounds it: the count
   * above carries how many repaints there really were, and the aggregate
   * line of the window says it. A program that beats register 7 pays one
   * line a second here, not one a frame.
   */
  if(vdp_backdrop_said == 0UL)
    {
      vdp_backdrop_said = 1UL;
      LOG_HOT(LOG_CAT_VDP,LOG_LVL_DBG,
              ("backdrop=%lu border filled",
               (unsigned long)VDP_BACKDROP_INDEX()));
    }
#endif
}

/*
 * A register write, reached through code 2 of the control sequence. The
 * number is taken on four bits; 0 to 10 are stored, 11 to 15 are ignored
 * and counted (TotalSMS/src/core/sms_vdp.c:585-595). Nothing is derived
 * from the value here: every register is stored as sent and read where it
 * is needed, when it is needed.
 *
 * Writes to registers 0 and 1 are where the mode is checked, since those
 * two hold it: mode 4 is bit 2 of register 0 (sms_vdp.c:233-236) and the
 * taller pictures are bit 1 of register 0 with bit 4 (224 lines) or bit
 * 3 (240 lines) of register 1 (sms_vdp.c:239-256, which refuses them as
 * this port does). Two counts, two warnings: a mode other than 4 is
 * counted with its four bits kept for the report; mode 4 at a taller
 * height is counted apart with the height kept, and rendered at 192
 * lines. Nothing stops either way: the program keeps running, its writes
 * keep landing, and the periodic line says what it asked for.
 */
static void
vdp_reg_write(uint32 number,
              uint32 value)
{
  if(number > VDP_REG_LAST)
    {
      VDP_COUNT(reg_oob);
      return;
    }

#if SMS_DECOR_CEL
  /*
   * A register the list is built from, written to another value on the
   * bits the list reads while the picture is being scanned: the scroll
   * register, the two table bases, the sprite size and magnification of
   * register 1, the sprite pattern base of register 6, the locks, the
   * masked column and the sprite shift of register 0, the backdrop of
   * register 7. Journaled with the line, so that the presentation draws
   * the lines before it with the old value and the lines from it with
   * the new, as the line render did (vdp_reg_note); counted. The
   * display bit of register 1 is per line already (row_off) and the
   * interrupt bits, register 9 and register 10 are read per line or
   * latched in the blanking: none of them is journaled.
   */
  if(sms.vdp.vcount < VDP_ACTIVE_LINES)
    {
      uint32 moved;

      moved = ((uint32)sms.vdp.reg[number] ^ value)
            & vdp_reg_journal_mask(number);
      if(moved != 0UL)
        {
          VDP_COUNT(reg_mid);
          vdp_reg_note(number,(uint32)sms.vdp.reg[number],value);
        }
    }

  /*
   * The sprite side of a register: a size or magnification of register 1
   * and a pattern base of register 6 leave the per-line table stale; a
   * move of the attribute table by register 5 moves it (the setter), the
   * watch bytes with it; a move of the backdrop by register 7 refills
   * the column (the setter).
   */
  if((number == 1UL) && ((((uint32)sms.vdp.reg[1] ^ value) & 0x03UL) != 0UL))
    sms.vdp.spr_dirty = 1;
  if((number == 6UL) && ((((uint32)sms.vdp.reg[6] ^ value) & 0x04UL) != 0UL))
    sms.vdp.spr_dirty = 1;
  if((number == 5UL) && ((((uint32)sms.vdp.reg[5] ^ value) & 0x7EUL) != 0UL))
    {
      vdp_reg_apply(5UL,value);
      sms.vdp.spr_dirty = 1;
      vdp_decor_watch_rebuild();
    }
  if(number == 7UL)
    vdp_reg_apply(7UL,value);

  /*
   * Register 2 moving the name table: every tile of the picture is then
   * a tile of another table, so all 896 are marked never converted, the
   * reference counts start over with them (the setter), the watch bytes
   * follow the new table, and the next presentation sweeps everything.
   * The hot marks and their list keep standing -- they fall at the next
   * vdp_list_end, as always -- and the pattern marks of the dirty sweep
   * clear at the next vdp_decor_apply, which the full sweep makes
   * harmless: every tile is looked at whatever they say. Only on a
   * move: a program that writes the same base every frame pays a
   * compare. Written mid-picture it is journaled above and cuts a band,
   * the picture converted whole once for the undo and once per replay.
   */
  if((number == 2UL)
     && (((uint32)sms.vdp.reg[2] & 0x0EUL) != (value & 0x0EUL)))
    {
      vdp_reg_apply(2UL,value);
      vdp_decor_watch_rebuild();
    }
#endif

  sms.vdp.reg[number] = (uint8)value;
  VDP_COUNT(reg_w);

#if VDP_COUNTERS
  if(number <= 1UL)
    {
      uint32 r0 = sms.vdp.reg[0];
      uint32 r1 = sms.vdp.reg[1];

      if((r0 & 0x04U) == 0U)
        {
          sms.vdp.cnt_mode++;
          sms.vdp.mode_last = ((r0 & 0x04U) << 1)
                            | ((r0 & 0x02U) << 1)
                            | ((r1 & 0x08U) >> 2)
                            | ((r1 & 0x10U) >> 4);
        }
      else if(((r0 & 0x02U) != 0U) && ((r1 & 0x18U) != 0U))
        {
          sms.vdp.cnt_height++;
          sms.vdp.height_last = ((r1 & 0x10U) != 0U) ? 224UL : 240UL;
        }
    }
#endif
}

void
vdp_io_ctrl_write(uint8 value)
{
  if(sms.vdp.latch != 0UL)
    {
      sms.vdp.ctrl_word = (sms.vdp.ctrl_word & 0xFFUL)
                        | ((uint32)value << 8);
      sms.vdp.code = ((uint32)value >> 6) & 3UL;
      sms.vdp.latch = 0;

      switch(sms.vdp.code)
        {
        case VDP_CODE_VRAM_READ:
          /*
           * The read code fills the buffer at once, so that the first data
           * read returns the byte at the address just set
           * (sms_vdp.c:649-654).
           */
          sms.vdp.addr = sms.vdp.ctrl_word & VDP_VRAM_MASK;
          sms.vdp.read_buf = sms.vdp.vram[sms.vdp.addr];
          sms.vdp.addr = (sms.vdp.addr + 1UL) & VDP_VRAM_MASK;
          break;

        case VDP_CODE_REG_WRITE:
          vdp_reg_write((uint32)value & 0xFUL,sms.vdp.ctrl_word & 0xFFUL);
          break;

        default:
          /* Codes 1 and 3: the address, and nothing moves until a data access. */
          sms.vdp.addr = sms.vdp.ctrl_word & VDP_VRAM_MASK;
          break;
        }
    }
  else
    {
      sms.vdp.addr = (sms.vdp.addr & 0x3F00UL) | (uint32)value;
      sms.vdp.ctrl_word = (uint32)value;
      sms.vdp.latch = 1;
    }
}

uint8
vdp_io_data_read(void)
{
  uint32 data;

  sms.vdp.latch = 0;

  data = sms.vdp.read_buf;
  sms.vdp.read_buf = sms.vdp.vram[sms.vdp.addr & VDP_VRAM_MASK];
  sms.vdp.addr = (sms.vdp.addr + 1UL) & VDP_VRAM_MASK;

  VDP_COUNT(data_r);

  return (uint8)data;
}

uint8
vdp_io_status_read(void)
{
  uint32 status;

  sms.vdp.latch = 0;

  /*
   * Bit 7 from the frame request, bit 6 from sprite overflow, bit 5 from
   * sprite collision; bits 4 to 0 read as ones (sms_vdp.c:490-502). All
   * four fall here and nowhere else, which is what holds the line up
   * between the event and the read, as the document says it is held
   * (SMSOfficialDocs.md:216-217), and what makes a program that never
   * reads the status see the two sprite bits stay up.
   */
  status = (sms.vdp.frame_pending != 0UL) ? 0x80UL : 0x00UL;
  if(sms.vdp.spr_overflow != 0UL)
    status |= 0x40UL;
  if(sms.vdp.spr_collision != 0UL)
    status |= 0x20UL;
  status |= 0x1FUL;

  sms.vdp.frame_pending = 0;
  sms.vdp.line_pending = 0;
  sms.vdp.spr_overflow = 0;
  sms.vdp.spr_collision = 0;

  VDP_COUNT(status_r);

  return (uint8)status;
}

uint8
vdp_io_vcounter_read(void)
{
  uint32 v;

  VDP_COUNT(vcnt_r);

  v = sms.vdp.vcount;
  if(v > VDP_VCOUNT_FOLD)
    v -= 6UL;

  return (uint8)v;
}

uint8
vdp_io_hcounter_read(void)
{
  VDP_COUNT(hcnt_r);

  return (uint8)0x00;
}

void
vdp_line(void)
{
  /*
   * The line is rendered before it is counted, with the registers as the
   * program left them during its quota: the scanline grain of the frame
   * loop, where a raster effect written in line y shows in line y.
   */
  if(sms.vdp.vcount < VDP_ACTIVE_LINES)
    vdp_render_line(sms.vdp.vcount);

  sms.vdp.vcount++;

  /* The frame interrupt is raised on the line after the picture (sms_vdp.c:1472-1476). */
  if(sms.vdp.vcount == VDP_ACTIVE_LINES + 1UL)
    sms.vdp.frame_pending = 1;

  /*
   * The line counter runs on every line of the picture and on the one
   * after it (sms_vdp.c:1484-1499): at zero it reloads and raises the
   * request, otherwise it steps down. Register 10 is read at the reload,
   * which is the one interrupt of delay the document gives between
   * writing it and seeing the effect (SMSOfficialDocs.md:942).
   */
  if(sms.vdp.vcount <= VDP_ACTIVE_LINES)
    {
      if(sms.vdp.line_ctr == 0UL)
        {
          sms.vdp.line_ctr = sms.vdp.reg[10];
          sms.vdp.line_pending = 1;
        }
      else
        {
          sms.vdp.line_ctr--;
        }
    }

  /*
   * The vertical scroll is latched in vertical blanking
   * (SMSOfficialDocs.md:895), here at the wrap: the value the next frame
   * renders with. TotalSMS copies it at every line (sms_vdp.c:1466); the
   * document outranks it.
   */
  if(sms.vdp.vcount == VDP_LINES_PER_FRAME)
    {
      sms.vdp.vcount = 0;
      sms.vdp.line_ctr = sms.vdp.reg[10];
      sms.vdp.vscroll = sms.vdp.reg[9];
    }

  /*
   * The horizontal scroll becomes effective one line late
   * (docs/sms_gg/GGOfficialDocs.md:1438): taken here, after this line was
   * rendered, for the next one.
   */
  sms.vdp.hscroll = sms.vdp.reg[8];
}

void
vdp_report(void)
{
#if VDP_COUNTERS
  /*
   * The acceptances are a running count the processor keeps; what the
   * line reports is the window's share, so the previous reading (file
   * static above) is subtracted. Unsigned, so the difference stays exact
   * across a wrap.
   */
  uint32 irq_now;
  uint32 irq;

  irq_now = sms.z80.irq_accepted;
  irq = irq_now - vdp_irq_seen;
  vdp_irq_seen = irq_now;

  if((sms.vdp.cnt_reg_w != 0UL) || (sms.vdp.cnt_vram_w != 0UL) ||
     (sms.vdp.cnt_cram_w != 0UL) || (sms.vdp.cnt_status_r != 0UL) ||
     (irq != 0UL))
    {
      LOG_HOT(LOG_CAT_VDP,LOG_LVL_DBG,
              ("reg w=%lu vram w=%lu cram w=%lu status r=%lu irq=%lu",
               (unsigned long)sms.vdp.cnt_reg_w,
               (unsigned long)sms.vdp.cnt_vram_w,
               (unsigned long)sms.vdp.cnt_cram_w,
               (unsigned long)sms.vdp.cnt_status_r,
               (unsigned long)irq));
    }

  /*
   * The screen table: the colour writes of the window again, how many of
   * them landed inside the picture, and how many rebuilds they cost. The
   * ratio of the first to the third is what the end-of-frame rebuild
   * buys; the second is the journal of a palette split, which the table
   * set once per frame cannot show and which a later stage may.
   */
  if((sms.vdp.cnt_cram_w != 0UL) || (sms.vdp.cnt_cram_mid != 0UL) ||
     (sms.vdp.cnt_clut_upd != 0UL))
    {
      LOG_HOT(LOG_CAT_VDP,LOG_LVL_DBG,
              ("clut writes=%lu mid=%lu updates=%lu",
               (unsigned long)sms.vdp.cnt_cram_w,
               (unsigned long)sms.vdp.cnt_cram_mid,
               (unsigned long)sms.vdp.cnt_clut_upd));
    }

  /*
   * The rarer reads on a line of their own, so that the line above keeps
   * its shape whatever is added here. A non-zero H counter figure is the
   * one to look at when a raster effect comes out wrong (vdp.h).
   */
  if((sms.vdp.cnt_vcnt_r != 0UL) || (sms.vdp.cnt_hcnt_r != 0UL) ||
     (sms.vdp.cnt_data_r != 0UL) || (sms.vdp.cnt_reg_oob != 0UL))
    {
      LOG_HOT(LOG_CAT_VDP,LOG_LVL_DBG,
              ("vcnt r=%lu hcnt r=%lu data r=%lu reg oob=%lu",
               (unsigned long)sms.vdp.cnt_vcnt_r,
               (unsigned long)sms.vdp.cnt_hcnt_r,
               (unsigned long)sms.vdp.cnt_data_r,
               (unsigned long)sms.vdp.cnt_reg_oob));
    }

  /*
   * What the background render is asked to show, every time and even
   * when nothing moved: the name table base, both scroll latches, the
   * two inhibit bits of register 0 (bit 1 of the field is the right
   * columns, bit 0 the top rows), the left column mask, and whether the
   * display is on at all (register 1 bit 6) -- a black picture with
   * disp=1 is a palette question, with disp=0 a program that has not
   * switched its screen on yet.
   */
  LOG_HOT(LOG_CAT_VDP,LOG_LVL_DBG,
          ("bg nt=0x%04lx scroll x=%lu y=%lu inhibit=%lu mask_col=%lu disp=%lu",
           (unsigned long)(((uint32)sms.vdp.reg[2] & 0x0EUL) << 10),
           (unsigned long)sms.vdp.hscroll,
           (unsigned long)sms.vdp.vscroll,
           (unsigned long)(((uint32)sms.vdp.reg[0] >> 6) & 3UL),
           (unsigned long)(((uint32)sms.vdp.reg[0] >> 5) & 1UL),
           (unsigned long)(((uint32)sms.vdp.reg[1] >> 6) & 1UL)));

  /*
   * What the sprites of the window did: the busiest line, how many of its
   * lines overflowed, how many had a collision, and whether magnification
   * was on for any line it composed. Every figure describes the window
   * and none is a reading taken at the moment of the report. Emitted
   * every time like the line above -- a game that shows no sprite at all
   * is a fact worth reading, and this is the only witness of two bits a
   * program raises and clears within the same frame.
   */
  LOG_HOT(LOG_CAT_VDP,LOG_LVL_DBG,
          ("sprites line_max=%lu overflow=%lu collision=%lu zoom=%lu",
           (unsigned long)sms.vdp.cnt_spr_max,
           (unsigned long)sms.vdp.cnt_spr_ovf,
           (unsigned long)sms.vdp.cnt_spr_col,
           (unsigned long)sms.vdp.cnt_spr_zoom));

  /*
   * The surround of the picture, and only when there was one to report:
   * how many repaints the window paid and which palette entry they used.
   * Zero repaints is the nominal case and says nothing, so the line
   * appearing at all is already the fact worth reading. The naming line
   * of the change itself was emitted once, at the change; this is its
   * count.
   */
  if(sms.vdp.cnt_backdrop != 0UL)
    {
      LOG_HOT(LOG_CAT_VDP,LOG_LVL_DBG,
              ("border repaint=%lu backdrop=%lu",
               (unsigned long)sms.vdp.cnt_backdrop,
               (unsigned long)VDP_BACKDROP_INDEX()));
    }

  /*
   * What the decoded row cache did over the window. Emitted every time,
   * like the background line above: a hit count that stopped growing, or
   * a miss count that stays level with it, is the fact worth reading, and
   * neither can be seen from a line that only appears when something is
   * wrong. Rows thrown away is the third: a game that never rewrites a
   * tile shows zero there, and a game that redraws its scenery shows the
   * cost of doing so.
   */
  LOG_HOT(LOG_CAT_VDP,LOG_LVL_DBG,
          ("tilecache hit=%lu miss=%lu inval=%lu",
           (unsigned long)sms.vdp.cnt_tc_hit,
           (unsigned long)sms.vdp.cnt_tc_miss,
           (unsigned long)sms.vdp.cnt_tc_inval));

  /*
   * How the lines of the window were rendered: the short way, or with the
   * priority scratch and the sprite pass because they carried a sprite.
   * Emitted every time, like the two lines above.
   *
   * It is the key to the breakdown printed by the other report, and the
   * two are read together or not at all: a line counted in fast= enters
   * the background post only, so the sprite post weighs the lines counted
   * in scratch= and nothing else. Lines with the display off are in
   * neither count -- they enter no post at all.
   */
  LOG_HOT(LOG_CAT_VDP,LOG_LVL_DBG,
          ("lines fast=%lu scratch=%lu",
           (unsigned long)sms.vdp.cnt_line_fast,
           (unsigned long)sms.vdp.cnt_line_scratch));

#if SMS_DECOR_CEL
  /*
   * What the background picture cost over the window, when it cost
   * anything: tiles converted, windows built, bands drawn, and the three
   * refusals -- lines folded into the last band, writes the journal had
   * no room for, windows the arena had no block for. A window where every
   * figure is zero is a window with no frame in it, and says nothing.
   */
  if((sms.vdp.cnt_decor_tiles != 0UL) || (sms.vdp.cnt_list_windows != 0UL)
     || (sms.vdp.cnt_list_bands != 0UL) || (sms.vdp.cnt_bands_capped != 0UL)
     || (sms.vdp.cnt_journal_full != 0UL) || (sms.vdp.cnt_list_refused != 0UL)
     || (sms.vdp.cnt_reg_mid != 0UL))
    {
      LOG_HOT(LOG_CAT_VDP,LOG_LVL_DBG,
              ("decor tiles=%lu windows=%lu bands=%lu capped=%lu journal_full=%lu refused=%lu reg_mid=%lu",
               (unsigned long)sms.vdp.cnt_decor_tiles,
               (unsigned long)sms.vdp.cnt_list_windows,
               (unsigned long)sms.vdp.cnt_list_bands,
               (unsigned long)sms.vdp.cnt_bands_capped,
               (unsigned long)sms.vdp.cnt_journal_full,
               (unsigned long)sms.vdp.cnt_list_refused,
               (unsigned long)sms.vdp.cnt_reg_mid));
    }

  /*
   * What the small cels of the list cost over the window, when any was
   * built: cels in all, sprites drawn, the cels a sprite cost past its
   * first, priority runs, cels the reserve had no block for -- and,
   * beside them, the bands drawn and the two sprite bits' line counts,
   * so that one line reads the whole of the list's work.
   */
  if((sms.vdp.cnt_cels != 0UL) || (sms.vdp.cnt_cels_refused != 0UL))
    {
      LOG_HOT(LOG_CAT_VDP,LOG_LVL_DBG,
              ("list cels=%lu sprites=%lu split=%lu prio=%lu refused=%lu bands=%lu collide=%lu overflow=%lu",
               (unsigned long)sms.vdp.cnt_cels,
               (unsigned long)sms.vdp.cnt_sprites,
               (unsigned long)sms.vdp.cnt_split,
               (unsigned long)sms.vdp.cnt_prio,
               (unsigned long)sms.vdp.cnt_cels_refused,
               (unsigned long)sms.vdp.cnt_list_bands,
               (unsigned long)sms.vdp.cnt_spr_col,
               (unsigned long)sms.vdp.cnt_spr_ovf));
    }

  /*
   * What changed mid-picture over the window and what it cost: the
   * bands drawn, the scroll splits (register 8 journaled), the other
   * registers journaled, the palette segments opened, the pictures
   * whose segments passed their cap, and the pictures counted degraded
   * -- bands or segments past their cap, a full journal, a display
   * list refused, once each. Emitted whenever a picture was presented, so that a
   * window where nothing changed reads as zeros beside its bands rather
   * than as silence.
   */
  if(sms.vdp.cnt_list_bands != 0UL)
    {
      LOG_HOT(LOG_CAT_VDP,LOG_LVL_DBG,
              ("raster bands=%lu scroll_changes=%lu reg_changes=%lu clut_changes=%lu clut_capped=%lu degraded=%lu",
               (unsigned long)sms.vdp.cnt_list_bands,
               (unsigned long)sms.vdp.cnt_scroll_mid,
               (unsigned long)sms.vdp.cnt_reg_journal,
               (unsigned long)sms.vdp.cnt_pal_seg,
               (unsigned long)sms.vdp.cnt_pal_capped,
               (unsigned long)sms.vdp.cnt_degraded));
    }
#endif

  if(sms.vdp.cnt_mode != 0UL)
    {
      LOG_HOT(LOG_CAT_VDP,LOG_LVL_WARN,
              ("unsupported mode=%lu count=%lu (mode 4 only)",
               (unsigned long)sms.vdp.mode_last,
               (unsigned long)sms.vdp.cnt_mode));
    }

  if(sms.vdp.cnt_height != 0UL)
    {
      LOG_HOT(LOG_CAT_VDP,LOG_LVL_WARN,
              ("unsupported height=%lu count=%lu (rendering 192)",
               (unsigned long)sms.vdp.height_last,
               (unsigned long)sms.vdp.cnt_height));
    }

  sms.vdp.cnt_reg_w = 0;
  sms.vdp.cnt_vram_w = 0;
  sms.vdp.cnt_cram_w = 0;
  sms.vdp.cnt_cram_mid = 0;
  sms.vdp.cnt_clut_upd = 0;
  sms.vdp.cnt_status_r = 0;
  sms.vdp.cnt_data_r = 0;
  sms.vdp.cnt_vcnt_r = 0;
  sms.vdp.cnt_hcnt_r = 0;
  sms.vdp.cnt_reg_oob = 0;
  sms.vdp.cnt_mode = 0;
  sms.vdp.cnt_height = 0;
  sms.vdp.cnt_spr_max = 0;
  sms.vdp.cnt_spr_ovf = 0;
  sms.vdp.cnt_spr_col = 0;
  sms.vdp.cnt_spr_zoom = 0;
  sms.vdp.cnt_backdrop = 0;
  sms.vdp.cnt_tc_hit = 0;
  sms.vdp.cnt_tc_miss = 0;
  sms.vdp.cnt_tc_inval = 0;
  sms.vdp.cnt_line_fast = 0;
  sms.vdp.cnt_line_scratch = 0;
#if SMS_DECOR_CEL
  sms.vdp.cnt_decor_tiles = 0;
  sms.vdp.cnt_list_windows = 0;
  sms.vdp.cnt_list_bands = 0;
  sms.vdp.cnt_bands_capped = 0;
  sms.vdp.cnt_journal_full = 0;
  sms.vdp.cnt_list_refused = 0;
  sms.vdp.cnt_reg_mid = 0;
  sms.vdp.cnt_scroll_mid = 0;
  sms.vdp.cnt_reg_journal = 0;
  sms.vdp.cnt_pal_seg = 0;
  sms.vdp.cnt_pal_capped = 0;
  sms.vdp.cnt_degraded = 0;
  sms.vdp.cnt_cels = 0;
  sms.vdp.cnt_sprites = 0;
  sms.vdp.cnt_split = 0;
  sms.vdp.cnt_prio = 0;
  sms.vdp.cnt_cels_refused = 0;
#endif
  vdp_backdrop_said = 0;
#endif
}
