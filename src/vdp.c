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

#if SMS_CEL_BPP8
/*
 * The free memory call the depth probe's boot line reads its figure from.
 * celutils.h above brings this header in already, so naming it changes
 * nothing today; it is named because the probe uses it directly, and a file
 * that uses a header should say so rather than inherit it. It buys no
 * protection: sitting under this switch, it is only ever preprocessed by a
 * probe build, so a day when celutils.h stopped including it would be a day
 * the DEFAULT build kept compiling and only the next probe build broke.
 */
#include "mem.h"

/*
 * The three guards common.h could not carry, on the pattern memprobe.c had
 * to use for the same reason (memprobe.c:18-19): the level and category
 * names belong to log.h, and common.h is a leaf that does not include it --
 * an #if written there would compare against names the preprocessor has
 * never seen, pass whatever it is given, and be a guard that cannot fire.
 *
 * What they protect is not a number that would come out wrong but a run that
 * would come out MUTE. This build exists to publish exactly two things: the
 * boot line that says whether it is measuring at all, an INFO line of the
 * video category, and the draw= field of the periodic line, an INFO line of
 * the performance category. Silence either of them and what is left is a
 * picture and no figure, which is the one outcome a probe must not be able
 * to reach quietly.
 */
#if (LOG_LVL_INFO) > (LOG_LEVEL)
#error "SMS_CEL_BPP8 needs LOG_LEVEL at LOG_LVL_INFO or above: its boot line and its draw= field are INFO lines"
#endif

#if ((LOG_CAT_MASK) & (1UL << (LOG_CAT_VDP))) == 0UL
#error "SMS_CEL_BPP8 needs LOG_CAT_VDP in LOG_CAT_MASK: without it the probe cannot say whether it is measuring"
#endif

#if ((LOG_CAT_MASK) & (1UL << (LOG_CAT_PERF))) == 0UL
#error "SMS_CEL_BPP8 needs LOG_CAT_PERF in LOG_CAT_MASK: the draw= field it exists for is a performance line"
#endif
#endif

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

#if SMS_CEL_BPP8
/*
 * The depth probe (common.h, SMS_CEL_BPP8): a second picture at one byte a
 * pixel, a second cel of eight bits over it, and the flag that says both
 * were had. Kept across a second init for the reason every block above is.
 *
 * The flag is read in two places and by nobody else: the render, which
 * composes eight bits only when there is somewhere to compose them, and the
 * init, which hands the drawing side the eight bit cel only when there is
 * one. Off, the program composes and draws exactly as the shipped build
 * does -- the picture stays right, and the boot line says the run measures
 * nothing rather than letting a figure be read off it.
 */
static uint8 *vdp_cel8_pixels = NULL;
static CCB *vdp_cel8_block = NULL;
static int32 vdp_cel8_on = 0;

/*
 * Whether the render writes that buffer, which is NOT the same question as
 * whether the eight bit cel is drawn. The ruler build (common.h,
 * SMS_CEL_BPP8_TESTPAT) draws it and does not write it: the pattern laid down
 * at init has to survive the frame to be read off the screen. Every write site
 * asks this one, the cel choice asks the other.
 */
static int32 vdp_cel8_compose = 0;

/*
 * The free DRAM the console reported the moment after the buffer above was
 * taken. Sampled there and printed later because the boot line belongs with
 * the rest of the cel trace, while the figure it carries only means anything
 * at the instant of the allocation.
 */
static uint32 vdp_cel8_dram_free = 0;

/*
 * The eight bit cel's preamble words as the library left them, read out the
 * moment CreateCel returns. The six bit block keeps its own pair for the same
 * reason (above): one debug line then puts the library's answer beside this
 * file's calculation, and a disagreement on the row offset is the one defect
 * that draws a garbled picture with no error anywhere.
 */
static uint32 vdp_cel8_pre0_lib = 0;
static uint32 vdp_cel8_pre1_lib = 0;
#endif

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
 * bands where the same order written a byte at a time had put them.
 *
 * Reading BYTES is what makes it the written definition of the format: the
 * word it produces carries the same value whatever the byte order of the
 * machine, so it belongs to neither order and can judge both. That is what
 * the bench uses it for -- it drives the two lane forms of vdp.h, the one
 * this build calls and the one it does not, and holds each against this
 * function. Holding only the called one would leave the other compiled by
 * nothing and tested by nothing, and a mistake in it would reach the
 * console with every bench green.
 *
 * It is no longer on the render's path, for that reason and no other; the
 * header says the rest.
 *
 * The indexes are taken as they come, unmasked: every value the render
 * puts in a row is below 32 by construction (a four bit pattern index,
 * plus 16 for the second bank, or a border colour of the same form), and a
 * mask per pixel would be sixteen operations per stroke paid for nothing.
 */
void
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
 * The same three words per sixteen pixels, read out of the composition
 * scratch a WORD at a time. This is the packing post of a line that went
 * through the scratch, and the only difference with the line that never
 * touched it is where the four lane words come from: there they are still
 * in registers as the composition made them, here they are read back.
 *
 * The picture begins at scratch byte line_org, which the fine scroll moves
 * off a word boundary on six values out of eight, so each group of four
 * pixels is cut out of two consecutive words by VDP_LANE_JOIN. The shift
 * is constant down the line and computed once here; the word that follows
 * the last group is inside the tail of the scratch, which vdp.h refuses to
 * be too short for.
 *
 * Sixteen byte reads per sixteen pixels become four word reads. The rest
 * of the arithmetic is what the byte form paid too.
 *
 * The four recuts are put in variables before the emitter is called, and
 * that is a cost and not a style: the emitter copies each of its lane
 * arguments four times (vdp.h, VDP_EMIT16), so handing it four recut
 * expressions would compute sixteen recuts per sixteen pixels instead of
 * four. The other caller passes variables for the same reason.
 */
static void
vdp_pack_line(uint32 *dst)
{
  const uint32 *src;
  uint32 sl;
  uint32 sr;
  uint32 held;
  uint32 w0;
  uint32 w1;
  uint32 w2;
  uint32 w3;
  uint32 g0;
  uint32 g1;
  uint32 g2;
  uint32 g3;
  uint32 n;

  src = sms.vdp.line_w + (sms.vdp.line_org >> 2);
  sl = (sms.vdp.line_org & 3UL) << 3;
  sr = 31UL - sl;
  held = src[0];

  for(n = 0; n < (VDP_PIX_WIDTH / 16UL); n++)
    {
      w0 = src[1];
      w1 = src[2];
      w2 = src[3];
      w3 = src[4];
      g0 = VDP_LANE_JOIN(held,w0,sl,sr);
      g1 = VDP_LANE_JOIN(w0,w1,sl,sr);
      g2 = VDP_LANE_JOIN(w1,w2,sl,sr);
      g3 = VDP_LANE_JOIN(w2,w3,sl,sr);
      VDP_EMIT16(g0,g1,g2,g3,dst);
      held = w3;
      src += 4;
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
  /*
   * Picture coordinates, so the origin the background left behind: a fine
   * scroll moves where pixel 0 of the picture sits inside the scratch,
   * and a sprite does not scroll with the background.
   */
  line = VDP_LINE_BYTES + sms.vdp.line_org;
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
 *   3. The sprites of the line are chosen, before anything is composed:
 *      the choice reads the attribute table and the registers only, and
 *      what it leaves behind -- how many sprites were kept -- decides how
 *      the line is rendered. None kept and nothing will read the two
 *      scratches, so neither is written and the composition below lays its
 *      sixteen pixel groups straight into the row; one kept or more and
 *      the line goes through the scratch exactly as it always did, since
 *      the sprite pass reads both. The two ways draw the same row, and the
 *      same words: they share one emitter (vdp.h, VDP_EMIT16).
 *   4. One stroke per tile column. Screen column c shows source column
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
 *   5. Register 0 bit 5: pixels 0 to 7 take the border colour and the
 *      mask (sms_vdp.c:826-836). The line that skips the scratch does the
 *      same to the two words those eight pixels fill, and lays no mask:
 *      nothing on that line reads one.
 *   6. The sprites of the line, over the background -- only where step 3
 *      kept some.
 *   7. Pack, likewise only where step 3 kept some: the other line is
 *      already packed.
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
  uint8 *line;
  uint8 *prio;
  uint8 *eb;
  uint8 *fb;
  uint32 *ent;
  uint32 *dstw;
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
  uint32 w0;
  uint32 w1;
  uint32 w2;
  uint32 *ow;
  uint32 lag;
  uint32 jsl;
  uint32 jsr;
  uint32 borderw;
  uint32 maskcol;
  uint32 pw0;
  uint32 pw1;
  uint32 gw0;
  uint32 gw1;
#if SMS_CEL_BPP8
  /*
   * The eight bit row of this line: as words, with the word of four border
   * pixels the display-off branch fills it with, and as bytes for the copy a
   * line with sprites ends on. The pair the packing needs is gone under this
   * switch: at one byte a pixel every stroke writes its own two words, so
   * nothing is held back for the stroke after it.
   */
  uint32 *ow8;
  uint32 bg8;
  const uint8 *src8;
  uint8 *dst8;
#else
  uint32 qw0;
  uint32 qw1;
#endif

  reg = sms.vdp.reg;

  /*
   * The origin of the picture inside the scratch, before anything is
   * written there: the lead, which is where it stands on a line with no
   * fine scroll and on a line with the display off. Step 2 moves it left
   * by the fine scroll once that is known.
   */
  sms.vdp.line_org = VDP_LINE_LEAD;
  line = VDP_LINE_BYTES + VDP_LINE_LEAD;
  prio = VDP_PRIO_BYTES + VDP_LINE_LEAD;
  out = (uint32 *)(sms.vdp.pixels[0] + (y * VDP_PIX_ROW_BYTES));
  border = VDP_BACKDROP_INDEX();

  if(((uint32)reg[1] & 0x40UL) == 0UL)
    {
      /*
       * Step 1. The three words of a uniform row are computed once and
       * stored sixteen times, and the scratch takes the same row.
       *
       * The reason it once gave for doing so -- that the scratch always
       * holds what the buffer holds -- no longer stands: a line with no
       * sprite on it writes no scratch at all (vdp.h, VDP_LINE_*). Nothing
       * reads what this branch leaves there either, since a line with the
       * picture off carries no sprite pass. The write is kept because this
       * branch is left untouched, not because anything depends on it.
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
#if SMS_CEL_BPP8
      /*
       * And the same uniform row at one byte a pixel, so that a line with
       * the picture switched off is as right on the probe's cel as it is on
       * the shipped one. Four equal bytes in a word carry no byte order
       * with them.
       */
      if(vdp_cel8_compose != 0)
        {
          bg8 = border * VDP_TC_LANE_ONE;
          ow8 = (uint32 *)(vdp_cel8_pixels + (y * VDP_CEL8_ROW_BYTES));
          for(x = 0; x < (VDP_CEL8_ROW_BYTES / 4UL); x++)
            ow8[x] = bg8;
        }
#endif
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
   * The fine scroll shifts the origin of the picture and not the place
   * each stroke is written, so that a stroke always lands on a word
   * boundary (vdp.h, VDP_LINE_*). Everything that speaks in picture
   * coordinates -- the masked left column below, the sprite pass, the
   * packer -- reads the origin from here.
   */
  sms.vdp.line_org = VDP_LINE_LEAD - fine;
  line = VDP_LINE_BYTES + sms.vdp.line_org;
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

  if(sms.vdp.spr_count == 0UL
#if SMS_CEL_BPP8
     /*
      * The depth probe composes eight bit pixels here and nowhere else, so
      * this is where it falls back when it has neither buffer nor cel: the
      * line takes the scratch path below instead, which composes and packs
      * six bits exactly as the shipped build does and is drawn by the six
      * bit cel. A run that could not measure then shows a right picture and
      * says in its boot line that it measured nothing -- which is the whole
      * of the fallback, and it costs one test a line, outside the stroke
      * loop the disassembly is counted on.
      */
     && (vdp_cel8_compose != 0)
#endif
    )
    {
      /*
       * ------------------------------------------------------------------
       * A line with no sprite on it, which on a real screen is most of
       * them. Nothing will read either scratch before the next line
       * overwrites what matters, so nothing is written to either: the
       * sixteen pixel groups go straight from the composition into the
       * three words of the row, and the second walk over the line -- 256
       * byte reads to make 48 words -- does not happen.
       *
       * The picture still begins at scratch byte line_org, so this way of
       * rendering has to do the recut the scratch used to do for free by
       * moving the origin. It is the same cut, made on words instead of on
       * an address: lag says which of the four words in hand the picture
       * starts in, and VDP_LANE_JOIN takes the four bytes that begin
       * jsl / 8 bytes into it. Both are constant down the line. Nothing
       * here is approximate -- it is the same run of index bytes, read
       * from another rank -- and the bench proves it against the byte
       * packer, group for group, for each of the eight fine scrolls.
       *
       * Strokes 1 to 32 are the picture; stroke 0 is screen column -1 and
       * exists only under a fine scroll, where the picture starts inside
       * it. It is composed for its two words and yields no group of its
       * own, which is why the group work sits behind a test on c.
       * ------------------------------------------------------------------
       */
      VDP_COUNT(line_fast);

      borderw = border * VDP_TC_LANE_ONE;
      maskcol = (uint32)reg[0] & 0x20UL;
      lag = (VDP_LINE_LEAD - fine) >> 2;
      jsl = ((VDP_LINE_LEAD - fine) & 3UL) << 3;
      jsr = 31UL - jsl;
      pw0 = 0;
      pw1 = 0;
#if !SMS_CEL_BPP8
      qw0 = 0;
      qw1 = 0;
#endif

      /*
       * The background post, and the row cursor is armed inside the
       * wrapper with the stroke index because the cursor advances: a
       * second pass that did not arm it again would write past the row
       * instead of over it.
       */
      VDP_REPEAT_BEGIN(VDP_POST_BG)
#if SMS_CEL_BPP8
      ow = (uint32 *)(vdp_cel8_pixels + (y * VDP_CEL8_ROW_BYTES));
#else
      ow = (uint32 *)(sms.vdp.pixels[0] + (y * VDP_PIX_ROW_BYTES));
#endif
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

              /*
               * Step 5 folded in: register 0 bit 5 gives pixels 0 to 7 the
               * border colour, and those eight pixels are exactly the two
               * groups of the first stroke. Four equal bytes in a word
               * carry no byte order with them, so the word is the same on
               * either machine. No mask is laid down for the sprite pass:
               * there is no sprite pass on this line.
               */
              if((maskcol != 0UL) && (c == 1UL))
                {
                  gw0 = borderw;
                  gw1 = borderw;
                }

#if SMS_CEL_BPP8
              /*
               * One byte a pixel, and this is the whole of what the depth
               * probe changes in the loop: the word the composition has in
               * hand IS the word the engine reads, so the stroke stores its
               * two and moves on. No packing, and no pair held back to be
               * packed with the next stroke -- the parity that fills a
               * group of sixteen has nothing left to do.
               *
               * True only on a big-endian machine, where byte 0 of a lane
               * word is pixel 0 (vdp.h, VDP_LANE_MSB_FIRST). That is why it
               * lives behind this switch and not in a named form beside
               * VDP_EMIT16: a real port of the format would have to be
               * compiled and exercised in both orders, and this probe
               * exists to price the format, not to carry it.
               */
              ow[0] = gw0;
              ow[1] = gw1;
              ow += 2;
#else
              /*
               * Two strokes fill one group of sixteen pixels, so one
               * stroke in two writes the three words.
               */
              if((c & 1UL) != 0UL)
                {
                  qw0 = gw0;
                  qw1 = gw1;
                }
              else
                {
                  VDP_EMIT16(qw0,qw1,gw0,gw1,ow);
                  ow += 3;
                }
#endif
            }

          pw0 = e0;
          pw1 = e1;
        }
      VDP_REPEAT_END;

      return;
    }

  VDP_COUNT(line_scratch);


  /*
   * Step 4. Stroke c is screen column c - 1, and the strokes are counted
   * from screen column -1: that first one lands in the lead of the
   * scratch and only its last pixels are picture. With no fine scroll it
   * is all lead and is skipped -- 32 strokes, columns 0 to 31; with one,
   * the 33 strokes run from -1 to 31, the last one ending in the tail.
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
   * and the wrapper opens above the three assignments rather than at the
   * loop: the stroke index and the two cursors advance, so a second pass
   * that did not set them again would write past the row instead of over
   * it. Set again, the pass writes the same bytes into the same places
   * and the picture is the one the control renders. Off, the wrapper is a
   * do/while(0) the compiler folds away.
   */
  VDP_REPEAT_BEGIN(VDP_POST_BG)
  c = (fine != 0UL) ? 0UL : 1UL;
  dstw = sms.vdp.line_w + (c * 2UL);
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
       * The priority mask is the same thing the pixel loop used to write
       * one byte at a time: the bit is kept only where the index is not
       * transparent. An index is below sixteen, so adding 0x7F to each
       * lane cannot carry into the lane above, and the top bit of
       * lane | (lane + 0x7F) is exactly "not zero" -- shifted down to the
       * low bit of the lane and kept only if the column carries priority.
       */
      bankw = ((word & 0x800UL) != 0UL) ? (16UL * VDP_TC_LANE_ONE) : 0UL;
      prw = ((word & 0x1000UL) != 0UL) ? VDP_TC_LANE_ONE : 0UL;

      dstw[0] = e0 | bankw;
      dstw[1] = e1 | bankw;
      pdw[0] = ((e0 | (e0 + VDP_TC_LANE_LOW)) >> 7) & prw;
      pdw[1] = ((e1 | (e1 + VDP_TC_LANE_LOW)) >> 7) & prw;

      dstw += 2;
      pdw += 2;
    }
  VDP_REPEAT_END;

  /* Step 5. */
  if(((uint32)reg[0] & 0x20UL) != 0UL)
    {
      for(x = 0; x < 8UL; x++)
        {
          line[x] = (uint8)border;
          prio[x] = 1;
        }
    }

  /*
   * Step 6. The sprites of this line, over the background. The masked left
   * column of the step above is not covered by them and does not need its
   * priority mask for that: the sprite pass leaves those pixels alone by
   * the left edge it starts from.
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

  /*
   * Step 7, and the packing post of the breakdown: a pure function of the
   * scratch into the row, so a second pass writes the same three words per
   * sixteen pixels back over themselves. It reads the scratch as words and
   * emits through VDP_EMIT16, the same emitter the line that skips the
   * scratch uses, so one proof covers the two.
   */
  VDP_REPEAT_BEGIN(VDP_POST_PACK)
  vdp_pack_line(out);
  VDP_REPEAT_END;

#if SMS_CEL_BPP8
  /*
   * The depth probe's row of this line, last of all. The line carries a
   * sprite, so it was composed in the scratch at six bits and packed into a
   * buffer nothing draws from under this switch; the eight bit row would
   * otherwise keep what the LAST frame left there, and a row of last frame's
   * picture is the one kind of wrong that looks right.
   *
   * An earlier form stamped the row flat instead, one uniform index across
   * the line, so that a stale row could not pass for a plausible band. On a
   * screen where nearly half the lines carry a sprite the stamp was read as
   * half a picture missing, twice. So the row is filled with the picture:
   * the scratch holds the 256 final indexes of the line, sprites drawn over
   * the background, from byte line_org on -- the same rule the packer above
   * reads by (vdp.h, line_org). A byte at a time because line_org sits off
   * a word boundary on six fine scrolls out of eight: a word copy would need
   * the recut VDP_LANE_JOIN does for the packer, and this copy exists to be
   * obviously right, not fast.
   *
   * What it costs, and to whom. About 256 loads and 256 stores a line, some
   * 3 000 cycles, on the lines that carry a sprite -- of the order of 25 ms
   * a frame on a screen where 88 lines do. That is paid by this build only,
   * and it lands in vdp=, frame= and fps= of the periodic line, which this
   * build's figures never quote. It moves neither loop the dossier counted
   * nor what draw= weighs, which is the size of the buffer and not its
   * contents.
   */
  if(vdp_cel8_compose != 0)
    {
      src8 = VDP_LINE_BYTES + sms.vdp.line_org;
      dst8 = vdp_cel8_pixels + (y * VDP_CEL8_ROW_BYTES);
      for(x = 0; x < VDP_PIX_WIDTH; x++)
        dst8[x] = src8[x];
    }
#endif
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
#if SMS_CEL_BPP8
  uint32 pre0_calc8;
  uint32 pre1_calc8;
#endif
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

#if SMS_CEL_BPP8
  /*
   * The depth probe's picture, one byte a pixel: forty-eight kilobytes of
   * the same DRAM the render competes for.
   *
   * LAST OF ALL THE BLOCKS, and that order is the whole of its promise not to
   * get in the way. Every allocation above is fatal when it is refused -- the
   * program has no picture, no plane table, no row cache without them -- so a
   * probe served before them on a console short of forty-eight kilobytes
   * would take the memory and bring the boot down in place of switching
   * itself off. Served last, it competes with nothing: what it gets is what
   * was left over, and what it does not get costs the run its figure and
   * nothing else.
   *
   * A refusal is therefore not fatal and not a failure code: the probe has
   * nowhere to compose, says so, and the run draws the six bit picture and
   * measures nothing.
   */
  if(vdp_cel8_pixels == NULL)
    {
      vdp_cel8_pixels = (uint8 *)sys_alloc("vdp_cel8",
                                           (int32)VDP_CEL8_BUF_BYTES,
                                           MEMTYPE_DRAM | MEMTYPE_FILL);
      if(vdp_cel8_pixels == NULL)
        LOG_WARN(LOG_CAT_VDP,
                 ("cel8 probe off: no memory for the eight bit picture, "
                  "the six bit cel keeps the screen"));
    }

  /*
   * And the free figure, read only when there is an allocation for it to
   * describe, and read on EVERY init rather than only on the one that took
   * the block: the block is taken once and kept, but what the console has
   * left is a fact about the moment it is asked, and a second init reprinting
   * the first one's answer would be publishing a stale figure as a fresh one.
   */
  if(vdp_cel8_pixels != NULL)
    {
      MemInfo cel8_mi;

      AvailMem(&cel8_mi,MEMTYPE_DRAM);
      vdp_cel8_dram_free = cel8_mi.minfo_SysFree;
    }
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

#if SMS_CEL_BPP8
  /* Cleared on every init, for the reason the buffers above are. */
  if(vdp_cel8_pixels != NULL)
    {
      for(x = 0; x < VDP_CEL8_BUF_BYTES; x++)
        vdp_cel8_pixels[x] = 0;
    }
#endif

  for(i = 0; i < VDP_CRAM_SIZE; i++)
    sms.vdp.cram[i] = 0;

  for(i = 0; i < (int32)VDP_LINE_WORDS; i++)
    {
      sms.vdp.line_w[i] = 0;
      sms.vdp.prio_w[i] = 0;
    }

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
  sms.vdp.cnt_tc_hit = 0;
  sms.vdp.cnt_tc_miss = 0;
  sms.vdp.cnt_tc_inval = 0;
  sms.vdp.cnt_line_fast = 0;
  sms.vdp.cnt_line_scratch = 0;
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
   * and the ten bit one at 16 (:215, :222). A depth BELOW eight bits is
   * read through the eight bit one; the wide field belongs to eight bits
   * and to sixteen alike (src_exemple/lrex/main.c:290-291) -- the boundary
   * sits between six and eight, not after eight, and reading it as "eight
   * or less" is a mistake this file has since made once at the other
   * depth. Written at 16, a row offset of 46 words leaves the
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

#if SMS_CEL_BPP8
  /*
   * The same pair for the depth probe's block, and the ONE field that makes
   * eight bits a different case rather than the same case one notch deeper.
   *
   * The row offset moves to the TEN bit field at 16. The boundary is not
   * "eight bits or less takes the eight bit field" -- it is BELOW eight, and
   * a depth of exactly eight goes with sixteen: "bits 25-16 for the word
   * offset for an 8 or 16 bpp cel" (src_exemple/lrex/main.c:290-291, and the
   * same pairing spelled as code in src_exemple/3d_3do_logo/main.c:79-82,
   * where a row offset counted for eight bit pixels is written through
   * WOFFSET10). Written into the eight bit field instead, the field the
   * engine reads stays at zero: a stride of two words where the rows are
   * sixty-four apart, which is the picture sheared into diagonals this file
   * already paid a capture to learn once at six bits.
   *
   * The six bit pair above is unaffected -- six is below eight, so it keeps
   * the eight bit field, and the arbiter below shows the library agreeing
   * with it word for word.
   */
  pre0_calc8 = ((VDP_ACTIVE_LINES - PRE0_VCNT_PREFETCH) << PRE0_VCNT_SHIFT)
             | PRE0_BPP_8;
  pre1_calc8 = (((VDP_CEL8_ROW_BYTES / 4UL) - PRE1_WOFFSET_PREFETCH)
                << PRE1_WOFFSET10_SHIFT)
             | PRE1_TLLSB_PDC0
             | (VDP_PIX_WIDTH - PRE1_TLHPCNT_PREFETCH);
#endif

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

#if SMS_CEL_BPP8
  /*
   * The depth probe's cel: the block above again, field for field, at eight
   * bits over the eight bit picture. Built after it and never instead of
   * it, so that a refusal anywhere here leaves a program that draws its six
   * bit cel as it always did.
   *
   * Coded at eight bits is what the library documents as buildable -- a
   * coded cel takes any depth up to eight, and at eight it allocates a
   * palette only because the coded option is set
   * (docs/3do/3do_portfolio_2.5.md:5433-5439). That palette is left where
   * it lies and the pointer repointed on this module's own, as on the six
   * bit way. There is no hand-filled fallback here: on the six bit way a
   * refusal by the library still has to produce a screen, while here it
   * only has to produce an honest boot line.
   *
   * The palette is the SAME thirty-two entries, and that is the point of
   * the probe as much as the composition is: an index the loop emits is
   * five bits wide in either depth -- the loop never writes above 31 -- so
   * eight bits changes how an index is wrapped and not what it means. The
   * run says whether that holds by showing a picture with right colours.
   */
  if((vdp_cel8_block == NULL) && (vdp_cel8_pixels != NULL))
    {
      vdp_cel8_block = CreateCel((int32)VDP_PIX_WIDTH,
                                 (int32)VDP_ACTIVE_LINES,
                                 (int32)VDP_CEL8_BPP,
                                 CREATECEL_CODED,
                                 (void *)vdp_cel8_pixels);
      if(vdp_cel8_block == NULL)
        {
          /*
           * The forty-eight kilobytes taken above stay taken, deliberately.
           * Everything in this program is allocated at init and never given
           * back -- there is no counterpart to sys_alloc, and handing the
           * block to FreeMem behind its back would leave the memory total it
           * publishes describing a program that no longer exists, which is
           * worse than the block. The build is scaffolding and its own boot
           * line says the memory went and the figure did not come, so the
           * cost is visible rather than hidden.
           */
          LOG_WARN(LOG_CAT_VDP,
                   ("cel8 probe off: the library refused an eight bit cel, "
                    "the six bit cel keeps the screen (the buffer stays "
                    "taken: nothing here is given back)"));
        }
      else
        {
          cel = vdp_cel8_block;

          /* The two the library does not set, for the two reasons above. */
          cel->ccb_Flags |= CCB_BGND | CCB_LDPLUT;

          /* And the two it does, forced rather than assumed, as above. */
          cel->ccb_Flags |= CCB_CCBPRE | CCB_LAST;
          cel->ccb_NextPtr = NULL;

          cel->ccb_SourcePtr = (CelData *)vdp_cel8_pixels;
          cel->ccb_PLUTPtr = sms.vdp.plut;
          cel->ccb_PIXC = 0x1F001F00UL;

          /*
           * The preamble words stay the library's, exactly as they do on the
           * six bit way and for the same reason: the library knows what the
           * hardware reads. They are READ OUT AND KEPT here so that the
           * arbiter at the end of this function can hold them against what
           * this file computes for a row of eight bit pixels -- the same
           * arbiter the six bit block has had since a capture paid for it.
           *
           * It is not a formality on this side. The composition writes 256
           * bytes a row and nothing but this pair tells the engine to step by
           * 256: a row offset the library computed differently would shear
           * the picture into diagonals AND make draw= weigh a fetch pattern
           * nobody counted -- a wrong figure that looks like a figure.
           *
           * Same frame as the cel it replaces: same picture, same raster,
           * so the same centring and the same 1:1 mapping.
           */
          vdp_cel8_pre0_lib = cel->ccb_PRE0;
          vdp_cel8_pre1_lib = cel->ccb_PRE1;

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
    }

  vdp_cel8_on = (vdp_cel8_block != NULL) ? 1 : 0;

#if SMS_CEL_BPP8_TESTPAT
  /*
   * The ruler, laid down once and left alone: line y is filled with colour
   * number y modulo 32, so the picture is six ramps of thirty-two bands over
   * its hundred and ninety-two lines. What it measures is on the screen and
   * not in a figure -- where the bands stop says whether every line is being
   * fetched, and how many colours they show says whether a colour number
   * still means what it means at six bits.
   *
   * A byte at a time on purpose. This runs once at init, it is not on any
   * path that is counted, and a byte written is a byte the engine reads: no
   * word arithmetic stands between what is written here and what comes out.
   */
  vdp_cel8_compose = 0;
  if(vdp_cel8_on != 0)
    {
      uint32 ry;
      uint32 rx;

      for(ry = 0; ry < VDP_ACTIVE_LINES; ry++)
        {
          uint8 *rrow = vdp_cel8_pixels + (ry * VDP_CEL8_ROW_BYTES);

          for(rx = 0; rx < VDP_PIX_WIDTH; rx++)
            rrow[rx] = (uint8)(ry & 31UL);
        }
    }
#else
  vdp_cel8_compose = vdp_cel8_on;
#endif

  /*
   * And this is the only line of the drawing side the probe touches: the
   * frame loop reads the cel once through vdp_cel() and draws whatever it
   * finds. The six bit block is built either way and stands ready here.
   */
  sms.vdp.cel = (void *)(vdp_cel8_on ? vdp_cel8_block : vdp_cel_block);
#else
  sms.vdp.cel = (void *)vdp_cel_block;
#endif

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
   * The row cache as built: how many rows the video memory can hold, what
   * the block costs in all, and the two facts that make it correct --
   * a row is decoded at its first use and thrown away when the video
   * memory under it is written.
   */
  LOG_INFO(LOG_CAT_VDP,
           ("tile cache rows=%lu bytes=%lu (decoded once, invalidated on vram write)",
            (unsigned long)VDP_TC_ROWS,
            (unsigned long)VDP_TC_TOTAL_BYTES));

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

#if SMS_CEL_BPP8
  /*
   * The depth probe in one line: whether it is measuring at all, the depth
   * and row it composes at, what the extra picture cost in DRAM, and what the
   * console had left once it was taken. Every figure the run is read with has
   * to be in the run's own trace -- a memory total quoted from a document is
   * a total nobody can check against the build that produced the drawing
   * figure.
   *
   * Two whole spellings, and the second is the reason there are two: a line
   * that printed the size of a buffer it never got would be announcing a
   * spend that did not happen, next to the very word saying it did not. What
   * was not taken is reported as nothing, not as a plan.
   */
  if(vdp_cel8_pixels != NULL)
    LOG_INFO(LOG_CAT_VDP,
             ("cel8 probe=%s compose=%s bpp=%lu row=%lu bytes=%lu dram_free=%lu "
              "%s",
              vdp_cel8_on ? "on" : "off",
              vdp_cel8_compose ? "on" : "off (ruler)",
              (unsigned long)VDP_CEL8_BPP,
              (unsigned long)VDP_CEL8_ROW_BYTES,
              (unsigned long)VDP_CEL8_BUF_BYTES,
              (unsigned long)vdp_cel8_dram_free,
              vdp_cel8_compose
                ? "(sprite lines copied from the scratch after packing)"
                : "(the buffer holds the ruler laid down at init)"));
  else
    LOG_INFO(LOG_CAT_VDP,
             ("cel8 probe=off bytes=0 (no buffer taken, nothing measured, "
              "the six bit cel keeps the screen)"));
#endif

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

#if SMS_CEL_BPP8
  /*
   * The same arbiter for the probe's block, and one thing more than the six
   * bit one has: a disagreement is raised rather than left to be noticed.
   *
   * The six bit pair is arbitrated at debug level because a wrong picture is
   * its own alarm -- the screen shears and the run stops there. This build
   * draws a picture that is ALREADY part wrong on purpose, and its whole
   * output is one number; a sheared row here would pass for the mess the
   * probe was expected to make, and draw= would come back as a plausible
   * figure for a fetch pattern nobody counted. So the pair is printed, and
   * the moment it differs the line says which half and what it costs.
   */
  if(vdp_cel8_block != NULL)
    {
      LOG_DBG(LOG_CAT_VDP,
              ("cel8 pre lib=0x%08lx 0x%08lx calc=0x%08lx 0x%08lx",
               (unsigned long)vdp_cel8_pre0_lib,
               (unsigned long)vdp_cel8_pre1_lib,
               (unsigned long)pre0_calc8,
               (unsigned long)pre1_calc8));

      if((vdp_cel8_pre0_lib != pre0_calc8) || (vdp_cel8_pre1_lib != pre1_calc8))
        LOG_WARN(LOG_CAT_VDP,
                 ("cel8 preamble disagrees: lib=0x%08lx 0x%08lx "
                  "calc=0x%08lx 0x%08lx -- the engine is not reading the "
                  "row this build composes, draw= weighs another shape",
                  (unsigned long)vdp_cel8_pre0_lib,
                  (unsigned long)vdp_cel8_pre1_lib,
                  (unsigned long)pre0_calc8,
                  (unsigned long)pre1_calc8));
    }
#endif

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
   * How the lines of the window were rendered: straight into the row, or
   * through the composition scratch because they carried a sprite.
   * Emitted every time, like the two lines above.
   *
   * It is the key to the breakdown printed by the other report, and the
   * two are read together or not at all: a line counted in fast= packs
   * itself as it composes, so its packing falls INSIDE the background post
   * and it never enters the packing post. On a frame with few sprites
   * pack= therefore falls towards nothing and bg= carries what pack= used
   * to, without either post having got faster or slower. Lines with the
   * display off are in neither count -- they enter no post at all.
   */
  LOG_HOT(LOG_CAT_VDP,LOG_LVL_DBG,
          ("lines fast=%lu scratch=%lu",
           (unsigned long)sms.vdp.cnt_line_fast,
           (unsigned long)sms.vdp.cnt_line_scratch));

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
  sms.vdp.cnt_tc_hit = 0;
  sms.vdp.cnt_tc_miss = 0;
  sms.vdp.cnt_tc_inval = 0;
  sms.vdp.cnt_line_fast = 0;
  sms.vdp.cnt_line_scratch = 0;
  vdp_backdrop_said = 0;
#endif
}
