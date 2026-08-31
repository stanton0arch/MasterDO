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
 * The colour table: six bit colour to RGB555, 64 entries. A colour byte
 * carries two bits per component, red in bits 0-1, green in 2-3, blue in
 * 4-5 (SMSOfficialDocs.md:288-306; TotalSMS/src/core/sms_vdp.c:1236-1238),
 * each at one of four levels -- off, one third, two thirds, full
 * (SMSOfficialDocs.md:494-500) -- which on the five bits of a component
 * of include/3do/graphics.h:261 are 0, 10, 21 and 31. Filled by a loop at
 * init rather than written as 64 literals: nothing to mistype. Read by the
 * data write macro of vdp.h, which is why it is not static.
 * ---------------------------------------------------------------------------
 */
uint16 vdp_cram_rgb[64];

static const uint8 vdp_level[4] = { 0, 10, 21, 31 };

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


/*
 * Packs one composed row into the 6 bit form the engine reads: the
 * leftmost pixel in the most significant bits of the first word, each
 * index in the next 6 bits down, sixteen pixels to three words. Word
 * stores, on purpose: on the big endian target a stored word puts its most
 * significant byte at the lowest address, which is exactly the byte order
 * a console capture proved for this cel -- the picture came out with flat
 * bands where the same order written a byte at a time had put them. On a
 * little endian host the same store reverses the bytes; a bench compiled
 * there compares words, not bytes.
 *
 * The indexes are taken as they come, unmasked: every value the render
 * puts in the scratch is below 32 by construction (a four bit pattern
 * index, plus 16 for the second bank, or a border colour of the same
 * form), and a mask per pixel would be sixteen operations per stroke paid
 * for nothing. The row pointer is a word pointer: the buffers come from
 * the allocator word aligned and the row length is a multiple of twelve
 * bytes (vdp.h refuses a width that is not).
 */
static void
vdp_pack_row(const uint8 *row,
             uint32      *dst)
{
  uint32 n;

  for(n = 0; n < (VDP_PIX_WIDTH / 16UL); n++)
    {
      dst[0] = ((uint32)row[0] << 26) | ((uint32)row[1] << 20)
             | ((uint32)row[2] << 14) | ((uint32)row[3] << 8)
             | ((uint32)row[4] << 2)  | ((uint32)row[5] >> 4);
      dst[1] = (((uint32)row[5] & 15UL) << 28) | ((uint32)row[6] << 22)
             | ((uint32)row[7] << 16) | ((uint32)row[8] << 10)
             | ((uint32)row[9] << 4)  | ((uint32)row[10] >> 2);
      dst[2] = (((uint32)row[10] & 3UL) << 30) | ((uint32)row[11] << 24)
             | ((uint32)row[12] << 18) | ((uint32)row[13] << 12)
             | ((uint32)row[14] << 6)  | (uint32)row[15];
      row += 16;
      dst += 3;
    }
}

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
 * the scratch. The kept entries are walked in table order, which is what
 * makes the first of them win a shared pixel
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
  line = sms.vdp.line + VDP_LINE_LEAD;
  prio = sms.vdp.prio + VDP_LINE_LEAD;
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

          if(prio[xi] != 0)
            continue;

          line[xi] = (uint8)(16UL + idx);
        }
    }
}

/*
 * ---------------------------------------------------------------------------
 * One line of the background plane, composed into the scratch and packed
 * into the index buffer at row y. Called once per line of the picture
 * from vdp_line and from nowhere else; nothing is called from inside it,
 * per pixel or per tile -- the plane table replaces the loop over bits,
 * the two flip directions are two loops written out, and the scratch's
 * lead and tail replace the compare a fine scroll would otherwise need on
 * every pixel.
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
 *   3. One stroke per tile column. Screen column c shows source column
 *      (c - coarse) & 31, each stroke shifted right by the fine scroll
 *      (docs/sms_gg/GGOfficialDocs.md:1394: a value of 1 moves the scene
 *      one dot to the right; sms_vdp.c:798-800, :880). Screen column -1
 *      -- the stroke whose tail becomes pixels 0 to fine-1 -- is walked
 *      when the fine scroll is not zero, into the lead of the scratch; the
 *      stroke of screen column 31 then runs into the tail. When register
 *      0 bit 7 holds the right eight columns still, screen columns 24 to
 *      31 take the unscrolled row (sms_vdp.c:838-850): a tile grain,
 *      like TotalSMS, which is at most seven pixels off at that border
 *      under a fine scroll. The name table word is read low byte first
 *      (sms_vdp.c:852): bit 12 priority, 11 palette bank, 10 vertical
 *      flip, 9 horizontal flip, 0 to 8 the pattern (sms_vdp.c:855-863).
 *      The pattern row is 4 bytes, one per plane, at pattern * 32 + row *
 *      4, the row taken as 7 - row under vertical flip (sms_vdp.c:865-872);
 *      the horizontal flip walks the eight pixels the other way. The bank
 *      adds 16 (sms_vdp.c:874-876); the mask is priority and index not
 *      zero (sms_vdp.c:893).
 *   4. Register 0 bit 5: pixels 0 to 7 take the border colour and the
 *      mask (sms_vdp.c:826-836).
 *   5. Pack.
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
  uint8 *line;
  uint8 *prio;
  uint8 *dst;
  uint8 *pd;
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
  uint32 bank;
  uint32 pr;
  uint32 idx;
  uint32 x;
  uint32 w0;
  uint32 w1;
  uint32 w2;

  reg = sms.vdp.reg;
  line = sms.vdp.line + VDP_LINE_LEAD;
  prio = sms.vdp.prio + VDP_LINE_LEAD;
  out = (uint32 *)(sms.vdp.pixels[0] + (y * VDP_PIX_ROW_BYTES));
  border = VDP_BACKDROP_INDEX();

  if(((uint32)reg[1] & 0x40UL) == 0UL)
    {
      /*
       * Step 1. The three words of a uniform row are computed once and
       * stored sixteen times; the scratch takes the same row so that it
       * always holds what the buffer holds.
       */
      for(x = 0; x < VDP_PIX_WIDTH; x++)
        {
          line[x] = (uint8)border;
          prio[x] = 0;
        }
      w0 = (border << 26) | (border << 20) | (border << 14)
         | (border << 8) | (border << 2) | (border >> 4);
      w1 = ((border & 15UL) << 28) | (border << 22) | (border << 16)
         | (border << 10) | (border << 4) | (border >> 2);
      w2 = ((border & 3UL) << 30) | (border << 24) | (border << 18)
         | (border << 12) | (border << 6) | border;
      for(x = 0; x < (VDP_PIX_WIDTH / 16UL); x++)
        {
          out[0] = w0;
          out[1] = w1;
          out[2] = w2;
          out += 3;
        }
      return;
    }

  /* Step 2. */
  vram = sms.vdp.vram;
  planes = sms.vdp.planes;
  hs = ((((uint32)reg[0] & 0x40UL) != 0UL) && (y < 16UL))
       ? 0UL : sms.vdp.hscroll;
  fine = hs & 7UL;
  coarse = hs >> 3;
  ys = y + sms.vdp.vscroll;
  if(ys >= VDP_NT_LINES)
    ys -= VDP_NT_LINES;
  nt = vram + (((uint32)reg[2] & 0x0EUL) << 10);
  /* Stroke index of the first still column: screen column 24 is stroke 25. */
  vsi_from = (((uint32)reg[0] & 0x80UL) != 0UL) ? 25UL : 33UL;

  /*
   * Step 3. Stroke c is screen column c - 1, and the strokes are counted
   * from screen column -1: that first one lands in the lead of the
   * scratch and only its last pixels are picture. With no fine scroll it
   * is all lead and is skipped -- 32 strokes, columns 0 to 31; with one,
   * the 33 strokes run from -1 to 31, the last one ending in the tail.
   *
   * This is the background post of the breakdown (vdp.h, VDP_REPEAT_*),
   * and the wrapper opens above the three assignments rather than at the
   * loop: the stroke index and the two cursors advance, so a second pass
   * that did not set them again would write past the row instead of over
   * it. Set again, the pass writes the same bytes into the same places
   * and the picture is the one the control renders. Off, the wrapper is a
   * do/while(0) the compiler folds away.
   */
  VDP_REPEAT_BEGIN(VDP_POST_BG)
  c = (fine != 0UL) ? 0UL : 1UL;
  dst = sms.vdp.line + (c * 8UL) + fine;
  pd = sms.vdp.prio + (c * 8UL) + fine;

  for(; c <= 32UL; c++)
    {
      /* screen column c - 1, source column (c - 1 - coarse) & 31 */
      yy = (c >= vsi_from) ? y : ys;
      word = read16_le(nt + ((yy >> 3) << 6) + (((c - 1UL - coarse) & 31UL) << 1));
      row = yy & 7UL;
      if((word & 0x400UL) != 0UL)
        row = 7UL - row;
      tile = vram + ((word & 0x1FFUL) << 5) + (row << 2);
      t0 = planes + ((uint32)tile[0] << 3);
      t1 = planes + VDP_PLANES_PLANE + ((uint32)tile[1] << 3);
      t2 = planes + (2UL * VDP_PLANES_PLANE) + ((uint32)tile[2] << 3);
      t3 = planes + (3UL * VDP_PLANES_PLANE) + ((uint32)tile[3] << 3);
      bank = ((word & 0x800UL) != 0UL) ? 16UL : 0UL;
      pr = (word >> 12) & 1UL;

      if((word & 0x200UL) != 0UL)
        {
          for(x = 0; x < 8UL; x++)
            {
              idx = (uint32)t0[7UL - x] | (uint32)t1[7UL - x]
                  | (uint32)t2[7UL - x] | (uint32)t3[7UL - x];
              dst[x] = (uint8)(idx | bank);
              pd[x] = (uint8)(pr & (idx != 0UL));
            }
        }
      else
        {
          for(x = 0; x < 8UL; x++)
            {
              idx = (uint32)t0[x] | (uint32)t1[x]
                  | (uint32)t2[x] | (uint32)t3[x];
              dst[x] = (uint8)(idx | bank);
              pd[x] = (uint8)(pr & (idx != 0UL));
            }
        }

      dst += 8;
      pd += 8;
    }
  VDP_REPEAT_END;

  /* Step 4. */
  if(((uint32)reg[0] & 0x20UL) != 0UL)
    {
      for(x = 0; x < 8UL; x++)
        {
          line[x] = (uint8)border;
          prio[x] = 1;
        }
    }

  /*
   * Step 5. The sprites of this line, over the background. The masked
   * left column of the step above is not covered by them and does not
   * need its priority mask for that: the sprite pass leaves those pixels
   * alone by the left edge it starts from.
   *
   * The sprite post of the breakdown, the two calls together: the
   * selection alone would leave nothing composed, and the composition
   * alone would need a selection it did not make. Both are idempotent --
   * the selection recomputes the same kept list, the composition clears
   * its taken mask on entry and lays the same pixels back down, and the
   * overflow and collision bits are raised whether or not they already
   * stood. What a second pass does move is the counters of the report:
   * the overflow and collision tallies of vdp_report read double under
   * this variant, which is why they are not read off a profiling run.
   */
  VDP_REPEAT_BEGIN(VDP_POST_SPRITES)
  vdp_select_sprites(y);
  if(sms.vdp.spr_count != 0UL)
    vdp_draw_sprites(y);
  VDP_REPEAT_END;

  /*
   * Step 6, and the packing post of the breakdown: a pure function of the
   * scratch into the row, so a second pass writes the same three words per
   * sixteen pixels back over themselves.
   */
  VDP_REPEAT_BEGIN(VDP_POST_PACK)
  vdp_pack_row(line,out);
  VDP_REPEAT_END;
}

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

  for(i = 0; i < (int32)VDP_LINE_SCRATCH; i++)
    {
      sms.vdp.line[i] = 0;
      sms.vdp.prio[i] = 0;
    }

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

#if VDP_COUNTERS
  sms.vdp.cnt_reg_w = 0;
  sms.vdp.cnt_vram_w = 0;
  sms.vdp.cnt_cram_w = 0;
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
  vdp_irq_seen = 0;
  vdp_backdrop_said = 0;
#endif

  /*
   * The colour table, then the palette as the conversion of the colour
   * memory just zeroed -- through the table, the same pen as the data
   * write, so there is one conversion in this file and not two. Then the
   * plane table: for plane p and byte value v, pixel x takes bit 7 - x
   * of v at weight p (SMSOfficialDocs.md:505-578, bit 7 is the left
   * pixel). Refilled on every init: cheap, and a table that is rebuilt
   * cannot be stale.
   */
  for(i = 0; i < 64; i++)
    vdp_cram_rgb[i] = (uint16)MakeRGB15(vdp_level[i & 3],
                                        vdp_level[(i >> 2) & 3],
                                        vdp_level[(i >> 4) & 3]);

  for(i = 0; i < VDP_PLUT_ENTRIES; i++)
    sms.vdp.plut[i] = vdp_cram_rgb[sms.vdp.cram[i] & 0x3FU];

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
   * Two details this pair had wrong, and a capture paid for both. The row
   * offset has TWO fields in the second word, at two different bit
   * positions: the eight bit one at 24 (include/3do/hardware.h:214, :221)
   * and the ten bit one at 16 (:215, :222). A depth of eight bits or less
   * is read through the eight bit one; the wide field belongs to the
   * sixteen bit form. Written at 16, a row offset of 46 words leaves the
   * field the engine actually reads at zero -- a stride of two words
   * where the rows are 48 apart, which is a picture sheared into thin
   * diagonals rather than a picture with wrong colours. And the linear
   * bit (:193) is not a coded cel's: the library sets neither of these on
   * this very block, and the debug line at the end of this function now
   * shows the two spellings landing on the same two words.
   */
  pre0_calc = ((VDP_ACTIVE_LINES - PRE0_VCNT_PREFETCH) << PRE0_VCNT_SHIFT)
            | PRE0_BPP_6;
  pre1_calc = (((VDP_PIX_ROW_BYTES / 4UL) - PRE1_WOFFSET_PREFETCH)
               << PRE1_WOFFSET8_SHIFT)
            | PRE1_TLLSB_PDC0
            | (VDP_PIX_WIDTH - PRE1_TLHPCNT_PREFETCH);

  if(vdp_cel_block == NULL)
    {
      /*
       * The library first. For a depth below 8 bits CreateCel allocates a
       * zeroed palette of its own and sets neither the background nor the
       * load-palette flag (docs/3do/3do_portfolio_2.5.md:5425-5455). Its
       * palette is left where it lies -- a small init-time allocation of
       * the library, outside the boot total -- and the cel's palette
       * pointer is repointed on this module's own palette below.
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
       * Both ways from here, and these two flags are the reason a library
       * cel cannot be used as it comes. Without the background flag a 15
       * bit colour of 000 is transparent (3do_portfolio_2.5.md:3784), so
       * the black band would show whatever lay under it; without the
       * load-palette flag the coded cel reads whatever palette the
       * hardware last loaded, not this module's.
       */
      cel->ccb_Flags |= CCB_BGND | CCB_LDPLUT;

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
       * because shifting a negative offset left is undefined in this
       * language, and off-raster on this machine. Positions are 16.16,
       * the horizontal step 12.20, the vertical 16.16 -- a 1:1 mapping at
       * no processor cost (src_exemple_video_player/renderer.c:56-86).
       */
      px = (sys_width() - (int32)VDP_PIX_WIDTH) / 2;
      py = (sys_height() - (int32)VDP_ACTIVE_LINES) / 2;
      if(px < 0)
        px = 0;
      if(py < 0)
        py = 0;
      cel->ccb_XPos = px << 16;
      cel->ccb_YPos = py << 16;
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
  LOG_INFO(LOG_CAT_VDP,("init ok mode=4 view=256x192 profile=%s",
                        cart_system_name(sms.cart.system)));
  LOG_INFO(LOG_CAT_VDP,("irq line owner=vdp (test source keeps nmi only)"));

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
           ("cel pos=%ld,%ld hdx=0x%lx vdy=0x%lx",
            (long)(vdp_cel_block->ccb_XPos >> 16),
            (long)(vdp_cel_block->ccb_YPos >> 16),
            (unsigned long)vdp_cel_block->ccb_HDX,
            (unsigned long)vdp_cel_block->ccb_VDY));

  /*
   * The preamble arbiter, at debug level: what the library computed for
   * this block beside what this file computes for it. Equal pairs retire
   * the calculation as a suspect; different pairs name, bit by bit, what
   * the hand had wrong. Two whole spellings, so the manual way -- which
   * has no library pair to show -- cannot be mistaken for agreement.
   */
  if(vdp_cel_manual)
    LOG_DBG(LOG_CAT_VDP,
            ("cel pre manual calc=0x%08lx 0x%08lx",
             (unsigned long)pre0_calc,
             (unsigned long)pre1_calc));
  else
    LOG_DBG(LOG_CAT_VDP,
            ("cel pre lib=0x%08lx 0x%08lx calc=0x%08lx 0x%08lx",
             (unsigned long)vdp_pre0_lib,
             (unsigned long)vdp_pre1_lib,
             (unsigned long)pre0_calc,
             (unsigned long)pre1_calc));

  return 0;
}

void *
vdp_cel(void)
{
  return sms.vdp.cel;
}

uint16
vdp_backdrop(void)
{
  return sms.vdp.plut[VDP_BACKDROP_INDEX()];
}

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
  vdp_backdrop_said = 0;
#endif
}
