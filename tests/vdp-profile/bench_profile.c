/* Host bench for the repetition wrappers of src/vdp.c: the five variants of
 * the render breakdown must draw the same picture, leave the same emulated
 * state, and -- proved by the coverage pass of run_profile.sh, not here --
 * each do exactly twice the work of its own post.
 *
 * Built twice by that script. With SMS_VDP_PROFILE=1 it compares the five
 * variants against each other. With the switch OFF it renders the same six
 * scenes through the DELIVERED form of the macros -- the form that goes out
 * to a player, and which nothing else exercises -- and prints a digest the
 * script compares against the instrumented build's control.
 *
 * Stubs follow bench_cart.c's, trimmed to what vdp.c needs, with the sizes
 * checked at the allocator rather than taken on trust.
 */
#include "sms.h"
#include "vdp.h"
#include "log.h"
#include "celutils.h"
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

/*
 * The guard bands around the picture buffer, and the byte they are filled
 * with. A band of ZEROES tested for non-zero would miss the overrun that
 * writes zeroes -- a repeated pass over a blank row, a priority mask laid
 * flat -- which is exactly the overrun a repeated post produces. So the
 * bands carry a pattern and are compared against it.
 *
 * One band each side. A wrapper that starts its second pass one stroke too
 * early walks BACKWARDS off the row, and a band behind the buffer alone
 * would never see it.
 */
#define GUARD_BYTES 4096
#define GUARD_FILL  0xA5

static unsigned char vram_pool[VDP_VRAM_SIZE];
static unsigned char planes_pool[VDP_PLANES_BYTES];
/*
 * Words, not bytes: vdp.c takes this block as a uint32 * and lays the
 * entries word first, so a pool with no alignment of its own would let the
 * bench pass on a host that tolerates what the target refuses. Guarded on
 * both sides for the same reason the picture buffer is -- the composition
 * now writes words at offsets it never used before.
 */
#define TC_GUARD_WORDS (GUARD_BYTES / 4)
static uint32 tc_pool_all[TC_GUARD_WORDS + (VDP_TC_TOTAL_BYTES / 4) + TC_GUARD_WORDS];
#define TC_POOL ((unsigned char *)(tc_pool_all + TC_GUARD_WORDS))
/* Set for one init only, to see the refusal path answer with its own code
   instead of the picture buffer's or the video memory's. */
static int tc_refuse = 0;
static unsigned char pixels_pool[GUARD_BYTES + VDP_PIX_BUF_BYTES + GUARD_BYTES];
static unsigned char pool[65536];
#define PIXELS (pixels_pool + GUARD_BYTES)

static int failed = 0;
/* Digest mode prints one line per scene and nothing else: its output is
   diffed against the other build's, so a passing assertion printed on one
   side and not the other would read as a disagreement. Failures still
   count, and still decide the exit status. */
static int quiet = 0;
static void check(int cond, const char *what)
{ if(!quiet || !cond) printf("  [%s] %s\n", cond ? "OK" : "FAIL", what);
  if(!cond) failed++; }

/*
 * The allocator the video part calls. The size is CHECKED against the pool
 * rather than ignored: a buffer that outgrew its pool would otherwise be
 * handed a short block, overwrite the statics behind it, and surface as a
 * failure somewhere unrecognisable. The restriction the stub has to carry
 * is the one the real allocator has -- it hands back what was asked for or
 * it hands back nothing.
 */
static void *give(const char *name, int32 want, int32 have, unsigned char *p)
{
  if(want != have)
    { printf("  [FAIL] %s asked for %ld bytes, the bench pool holds %ld\n",
             name,(long)want,(long)have);
      failed++; exit(1); }
  memset(p,0,(size_t)have);
  return p;
}

void *sys_alloc(const char *name, int32 size, uint32 memtype)
{
  (void)memtype;
  if(strcmp(name,"vdp_vram") == 0)
    return give(name,size,(int32)VDP_VRAM_SIZE,vram_pool);
  if(strcmp(name,"vdp_pixels") == 0)
    return give(name,size,(int32)VDP_PIX_BUF_BYTES,PIXELS);
  if(strcmp(name,"vdp_planes") == 0)
    return give(name,size,(int32)VDP_PLANES_BYTES,planes_pool);
  if(strcmp(name,"vdp_tilecache") == 0)
    {
      if(tc_refuse) return NULL;
      return give(name,size,(int32)VDP_TC_TOTAL_BYTES,TC_POOL);
    }
  if(size > (int32)sizeof(pool))
    { printf("  [FAIL] %s asked for %ld bytes, past the general pool\n",
             name,(long)size);
      failed++; exit(1); }
  return pool;
}

void sys_mem_report(void) {}
void sys_mem_seal(void) {}
int32 sys_width(void) { return 320; }
int32 sys_height(void) { return 240; }
Item sys_bitmap(void) { return 0; }
const char *cart_system_name(int32 system) { (void)system; return "SMS"; }

static CCB createcel_ccb;
static uint16 createcel_plut[32];
CCB *CreateCel(int32 width, int32 height, int32 bitsPerPixel, int32 options, void *dataBuf)
{ (void)options;
  memset(&createcel_ccb,0,sizeof(createcel_ccb));
  memset(createcel_plut,0,sizeof(createcel_plut));
  createcel_ccb.ccb_Flags = CCB_SPABS|CCB_PPABS|CCB_LDSIZE|CCB_LDPRS|CCB_YOXY|
                            CCB_ACW|CCB_ACCW|CCB_ACE|CCB_LAST;
  createcel_ccb.ccb_SourcePtr = (CelData *)dataBuf;
  createcel_ccb.ccb_PLUTPtr = createcel_plut;
  createcel_ccb.ccb_Width = width; createcel_ccb.ccb_Height = height;
  createcel_ccb.ccb_PRE0 = (uint32)bitsPerPixel;
  return &createcel_ccb; }

void log_begin(int32 cat, int32 lvl) { (void)cat; (void)lvl; }
void log_printf(const char *fmt, ...) { (void)fmt; }
void log_bind_screen(Item b, Item s) { (void)b; (void)s; }
void log_fatal(int32 cat, int32 code, const char *l1, const char *l2)
{ (void)cat; (void)code; (void)l1; (void)l2; }

/* ------------------------------------------------------------------ */

#if VDP_PROFILE
#define VARIANTS VDP_PROFILE_VARIANTS
#else
#define VARIANTS 1UL
#endif

static uint32 rng = 0x1234567UL;
static unsigned rnd(void)
{ rng = rng * 1103515245UL + 12345UL; return (unsigned)((rng >> 16) & 0xFFFFU); }

static vdp_t snap;
static unsigned char pix[VARIANTS][VDP_PIX_BUF_BYTES];
static unsigned char prio[VARIANTS][VDP_LINE_SCRATCH];
/*
 * The two sprite bits, ONE BYTE PER LINE. Read as they stand at the end of
 * a frame they are sticky -- the pass only ever raises them, and every
 * scene with sprites raises both -- so a whole-frame comparison is 1 == 1
 * whatever the repeated pass does. Cleared before each line and recorded
 * after it, they can fall, and then they witness.
 */
static unsigned char bits[VARIANTS][VDP_ACTIVE_LINES];

static void guard_arm(void)
{ memset(pixels_pool,GUARD_FILL,GUARD_BYTES);
  memset(PIXELS + VDP_PIX_BUF_BYTES,GUARD_FILL,GUARD_BYTES);
  memset(tc_pool_all,GUARD_FILL,GUARD_BYTES);
  memset(TC_POOL + VDP_TC_TOTAL_BYTES,GUARD_FILL,GUARD_BYTES); }

static int guard_intact(void)
{ int i;
  for(i = 0; i < GUARD_BYTES; i++)
    { if(pixels_pool[i] != GUARD_FILL) return 0;
      if(PIXELS[VDP_PIX_BUF_BYTES + i] != GUARD_FILL) return 0;
      if(((unsigned char *)tc_pool_all)[i] != GUARD_FILL) return 0;
      if(TC_POOL[VDP_TC_TOTAL_BYTES + i] != GUARD_FILL) return 0; }
  return 1; }

/*
 * The sixteen bytes past the picture in the priority scratch. The strokes
 * stop at byte 263 and the picture ends there at the latest, so nothing
 * may write from VDP_LINE_LEAD + VDP_PIX_WIDTH upward. Filled with the
 * guard pattern rather than tested for zero: a stroke written one stroke
 * too far would lay mask bytes there, and most of them are zero.
 */
static void tail_arm(void)
{ memset(VDP_PRIO_BYTES + VDP_LINE_LEAD + VDP_PIX_WIDTH,GUARD_FILL,VDP_LINE_TAIL); }

static int tail_intact(void)
{ uint32 i;
  for(i = 0; i < VDP_LINE_TAIL; i++)
    { if(VDP_PRIO_BYTES[VDP_LINE_LEAD + VDP_PIX_WIDTH + i] != GUARD_FILL) return 0; }
  return 1; }

/* One picture rendered line by line, the raster forced so that what is
   compared is the render and not the counters around it. */
static void run_variant(uint32 v)
{
  uint32 y;
  char msg[128];

  sms.vdp = snap;
  memset(sms.vdp.pixels[0],0,VDP_PIX_BUF_BYTES);
  /* Armed before EVERY render: a band left dirty by one variant would be
     charged to every variant rendered after it. */
  guard_arm();
#if VDP_PROFILE
  vdp_profile_select(v);
#endif
  for(y = 0; y < VDP_ACTIVE_LINES; y++)
    {
      sms.vdp.vcount = y;
      sms.vdp.spr_overflow = 0;
      sms.vdp.spr_collision = 0;
      vdp_line();
      bits[v][y] = (unsigned char)((sms.vdp.spr_overflow != 0UL ? 1U : 0U)
                                 | (sms.vdp.spr_collision != 0UL ? 2U : 0U));
    }
  sprintf(msg,"variant %lu stayed inside the picture buffer",(unsigned long)v);
  check(guard_intact(),msg);
  memcpy(pix[v],sms.vdp.pixels[0],VDP_PIX_BUF_BYTES);
  memcpy(prio[v],VDP_PRIO_BYTES,VDP_LINE_SCRATCH);
}

static void scene(uint32 hs, uint32 vs, uint32 r0, uint32 r1, int sprites)
{
  uint32 i;

  for(i = 0; i < VDP_VRAM_SIZE; i++)
    sms.vdp.vram[i] = (uint8)rnd();
  /* Filled behind the port's back, so nothing invalidated the rows the
     previous scene left decoded. The port does this for a real program. */
  for(i = 0; i < VDP_TC_ROWS; i++)
    sms.vdp.tc_valid[i] = 0;
  for(i = 0; i < 32UL; i++)
    sms.vdp.cram[i] = (uint8)(rnd() & 0x3FU);

  sms.vdp.reg[0] = (uint8)r0;
  sms.vdp.reg[1] = (uint8)r1;
  sms.vdp.reg[2] = 0x0E;            /* name table at 0x3800 */
  sms.vdp.reg[5] = 0x7E;            /* sprite attribute table at 0x3F00 */
  sms.vdp.reg[6] = 0x04;            /* sprite patterns in the second bank */
  sms.vdp.reg[7] = 0x05;
  sms.vdp.hscroll = hs;
  sms.vdp.vscroll = vs;
  sms.vdp.spr_overflow = 0;
  sms.vdp.spr_collision = 0;

  if(sprites == 2)
    {
      /*
       * EXACTLY eight sprites on the same lines, and the terminator right
       * after them. Eight is the boundary: the ninth is what raises the
       * overflow bit and ends the walk, so at eight the kept list is full
       * and the bit stays down. That is the state a repeated selection
       * pass has to reproduce exactly, and neither the crowded scene nor
       * the empty one visits it.
       */
      for(i = 0; i < 8UL; i++)
        {
          sms.vdp.vram[0x3F00UL + i] = (uint8)(20UL + (i & 1UL));
          sms.vdp.vram[0x3F80UL + (i * 2UL)] = (uint8)(i * 24UL);
          sms.vdp.vram[0x3F80UL + (i * 2UL) + 1UL] = (uint8)(i + 1UL);
        }
      sms.vdp.vram[0x3F08UL] = 0xD0;
    }
  else if(sprites == 1)
    {
      /* A crowded raster: more than eight on the same lines, so overflow
         and collision both fire. */
      for(i = 0; i < 64UL; i++)
        sms.vdp.vram[0x3F00UL + i] = (uint8)((i * 3UL) & 0xBFU);
      for(i = 0; i < 64UL; i++)
        {
          sms.vdp.vram[0x3F80UL + (i * 2UL)] = (uint8)((i * 5UL) & 0xFFU);
          sms.vdp.vram[0x3F80UL + (i * 2UL) + 1UL] = (uint8)(i & 0xFFU);
        }
    }
  else
    sms.vdp.vram[0x3F00UL] = 0xD0;  /* the terminator on the first entry */
}

#if VDP_PROFILE
static void compare(const char *name)
{
  uint32 v;
  char msg[160];

  for(v = 1; v < VARIANTS; v++)
    {
      sprintf(msg,"%s: variant %lu picture identical to the control",name,(unsigned long)v);
      check(memcmp(pix[v],pix[0],VDP_PIX_BUF_BYTES) == 0,msg);
      /* The priority mask is written by the background post and READ by the
         sprite composition: a repeated background pass that corrupted it
         could leave the final picture untouched on this scene and wreck the
         next one. Compared in its own right. */
      sprintf(msg,"%s: variant %lu priority mask identical",name,(unsigned long)v);
      check(memcmp(prio[v],prio[0],VDP_LINE_SCRATCH) == 0,msg);
      sprintf(msg,"%s: variant %lu overflow/collision identical line by line",name,(unsigned long)v);
      check(memcmp(bits[v],bits[0],VDP_ACTIVE_LINES) == 0,msg);
    }
}
#endif

/* ------------------------------------------------------------------ */
/*
 * The decoded row cache, held against a reference that owes it nothing.
 *
 * reference_bg composes the background of one line the way the render
 * composed it before there was a cache: four plane tables read per pixel,
 * one byte written at a time, the stroke shifted right by the fine scroll.
 * It is deliberately the OLD shape -- if it were written from the new one
 * it would agree with a wrong render. Only the picture is compared, at
 * each side's own origin, because the two lay their lead and their tail in
 * different places by design.
 *
 * What this pass is for, and what makes it bite: a cache that never served
 * a row would still pass a comparison of pictures. So the picture is
 * checked, then the cache is caught SERVING -- a byte poked straight into
 * the video memory must NOT change the picture, and the same byte written
 * through the port MUST change it.
 */
static unsigned char ref_line[VDP_LINE_SCRATCH];
static unsigned char ref_prio[VDP_LINE_SCRATCH];
static uint32 ref_mirrored = 0;
static uint32 ref_flipped = 0;
static uint32 ref_banked = 0;
static uint32 ref_prioritised = 0;

static void reference_bg(uint32 y, uint32 hs, uint32 vs)
{
  const uint8 *vram = sms.vdp.vram;
  const uint8 *reg = sms.vdp.reg;
  const uint8 *planes = sms.vdp.planes;
  const uint8 *nt;
  uint32 fine, coarse, ys, yy, c, vsi_from, word, row, bank, pr, idx, x;
  uint8 *dst;
  uint8 *pd;

  memset(ref_line,0,sizeof(ref_line));
  memset(ref_prio,0,sizeof(ref_prio));

  if(((uint32)reg[1] & 0x40UL) == 0UL)
    {
      for(x = 0; x < VDP_PIX_WIDTH; x++)
        ref_line[VDP_LINE_LEAD + x] = (uint8)VDP_BACKDROP_INDEX();
      return;
    }

  fine = (((((uint32)reg[0] & 0x40UL) != 0UL) && (y < 16UL)) ? 0UL : hs) & 7UL;
  coarse = ((((uint32)reg[0] & 0x40UL) != 0UL) && (y < 16UL)) ? 0UL : (hs >> 3);
  ys = y + vs;
  if(ys >= VDP_NT_LINES) ys -= VDP_NT_LINES;
  nt = vram + (((uint32)reg[2] & 0x0EUL) << 10);
  vsi_from = (((uint32)reg[0] & 0x80UL) != 0UL) ? 25UL : 33UL;

  c = (fine != 0UL) ? 0UL : 1UL;
  dst = ref_line + (c * 8UL) + fine;
  pd = ref_prio + (c * 8UL) + fine;
  for(; c <= 32UL; c++)
    {
      const uint8 *tile, *t0, *t1, *t2, *t3;
      yy = (c >= vsi_from) ? y : ys;
      word = read16_le(nt + ((yy >> 3) << 6) + (((c - 1UL - coarse) & 31UL) << 1));
      row = yy & 7UL;
      if((word & 0x400UL) != 0UL) { row = 7UL - row; ref_flipped++; }
      if((word & 0x200UL) != 0UL) ref_mirrored++;
      if((word & 0x800UL) != 0UL) ref_banked++;
      if((word & 0x1000UL) != 0UL) ref_prioritised++;
      tile = vram + ((word & 0x1FFUL) << 5) + (row << 2);
      t0 = planes + ((uint32)tile[0] << 3);
      t1 = planes + VDP_PLANES_PLANE + ((uint32)tile[1] << 3);
      t2 = planes + (2UL * VDP_PLANES_PLANE) + ((uint32)tile[2] << 3);
      t3 = planes + (3UL * VDP_PLANES_PLANE) + ((uint32)tile[3] << 3);
      bank = ((word & 0x800UL) != 0UL) ? 16UL : 0UL;
      pr = (word >> 12) & 1UL;
      /* The mask is the priority bit of the column, and that alone: the
         sprite pass reads whether the pixel under it is opaque off the
         row (src/vdp.c, vdp_draw_sprites), and the scene below holds it
         to that. */
      for(x = 0; x < 8UL; x++)
        {
          uint32 k = ((word & 0x200UL) != 0UL) ? (7UL - x) : x;
          idx = (uint32)t0[k] | (uint32)t1[k] | (uint32)t2[k] | (uint32)t3[k];
          dst[x] = (uint8)(idx | bank);
          pd[x] = (uint8)pr;
        }
      dst += 8; pd += 8;
    }

  if(((uint32)reg[0] & 0x20UL) != 0UL)
    for(x = 0; x < 8UL; x++)
      { ref_line[VDP_LINE_LEAD + x] = (uint8)VDP_BACKDROP_INDEX();
        ref_prio[VDP_LINE_LEAD + x] = 1; }
}

/* One line rendered with the scroll pinned, since vdp_line latches the
   horizontal one for the next line as soon as this one is drawn. */
static void render_pinned(uint32 y, uint32 hs, uint32 vs)
{
  sms.vdp.vcount = y;
  sms.vdp.hscroll = hs;
  sms.vdp.vscroll = vs;
  vdp_line();
}

/*
 * What the line actually produced, held against the reference line.
 *
 * The row of the picture is one index per byte, pixel x at byte x, and
 * the reference composes one index per byte from its own origin: the two
 * are compared byte for byte over the width of the picture. Nothing
 * stands between the composition and the comparison -- no packer, no
 * decoder -- so a lane taken from the wrong end of a word, which is the
 * mistake the word recut can make, shows as a row that differs.
 */
static int picture_matches(uint32 y)
{
  return memcmp(ref_line + VDP_LINE_LEAD,
                sms.vdp.pixels[0] + (y * VDP_PIX_ROW_BYTES),
                VDP_PIX_WIDTH) == 0;
}

/*
 * The priority scratch, filled whole with the guard pattern. A line with
 * no sprite on it must leave it exactly as it was found -- not the tail
 * only, the whole of it -- which is the claim that a line with no sprite
 * lays no mask. Filled with a pattern rather than zeroed for the reason
 * the picture guard is: a pass that laid a flat mask down would pass a
 * test for zero.
 */
static void scratch_arm(void)
{ memset(VDP_PRIO_BYTES,GUARD_FILL,VDP_LINE_SCRATCH); }

static int scratch_untouched(void)
{ uint32 i;
  for(i = 0; i < VDP_LINE_SCRATCH; i++)
    { if(VDP_PRIO_BYTES[i] != GUARD_FILL) return 0; }
  return 1; }

static void tilecache_scene(const char *name, uint32 hs, uint32 vs,
                            uint32 r0, uint32 r1, int drawn)
{
  uint32 y;
  int same = 1;
  int tail = 1;
  int clean = 1;
  char msg[160];
#if VDP_COUNTERS
  uint32 miss_first, miss_second;
#endif

  scene(hs,vs,r0,r1,0);          /* no sprite: the scratch is background */
  ref_mirrored = ref_flipped = ref_banked = ref_prioritised = 0;
#if VDP_COUNTERS
  sms.vdp.cnt_tc_hit = 0;
  sms.vdp.cnt_tc_miss = 0;
  sms.vdp.cnt_line_fast = 0;
  sms.vdp.cnt_line_scratch = 0;
#endif

  for(y = 0; y < VDP_ACTIVE_LINES; y++)
    {
      /* No sprite anywhere in this scene, so every drawn line takes the
         short way and must not write the scratch. The blank branch is
         armed for its tail only. */
      if(drawn) scratch_arm(); else tail_arm();
      render_pinned(y,hs,vs);
      if(drawn) { if(!scratch_untouched()) clean = 0; }
      else      { if(!tail_intact()) tail = 0; }
      reference_bg(y,hs,vs);
      if(!picture_matches(y)) same = 0;
    }
  sprintf(msg,"%s: every line matches a composition that owes the cache nothing",name);
  check(same,msg);
  if(drawn)
    {
      sprintf(msg,"%s: the scratch was not written on a line with no sprite",name);
      check(clean,msg);
    }
  else
    {
      sprintf(msg,"%s: nothing written past the picture in the scratch",name);
      check(tail,msg);
    }

  /* The blank branch fills the row and returns before the background post:
     it has no row to decode, and the three counts below would be zero for
     a right reason. Its picture is checked above like every other.
     What it must NOT do is claim a way: it takes neither, so a line
     rendered with the display off belongs to neither tally -- otherwise
     the share the two counters publish would be read against a total
     that is not the number of lines they describe. */
  if(!drawn)
    {
#if VDP_COUNTERS
      sprintf(msg,"%s: a blank line claims neither way (fast=%lu scratch=%lu)",
              name,(unsigned long)sms.vdp.cnt_line_fast,
              (unsigned long)sms.vdp.cnt_line_scratch);
      check((sms.vdp.cnt_line_fast == 0UL)
            && (sms.vdp.cnt_line_scratch == 0UL),msg);
#endif
      return;
    }

#if VDP_COUNTERS
  miss_first = sms.vdp.cnt_tc_miss;
  sprintf(msg,"%s: rows were decoded on first use (miss=%lu)",name,
          (unsigned long)miss_first);
  check(miss_first > 0UL,msg);

  /* The same frame again: nothing was written, so nothing may be decoded
     twice. This is the whole claim of the story, stated as a number. */
  sms.vdp.cnt_tc_miss = 0;
  sms.vdp.cnt_tc_hit = 0;
  for(y = 0; y < VDP_ACTIVE_LINES; y++)
    render_pinned(y,hs,vs);
  miss_second = sms.vdp.cnt_tc_miss;
  sprintf(msg,"%s: the second frame decoded nothing again (miss=%lu hit=%lu)",
          name,(unsigned long)miss_second,(unsigned long)sms.vdp.cnt_tc_hit);
  check((miss_second == 0UL) && (sms.vdp.cnt_tc_hit > 0UL),msg);

#endif

  sprintf(msg,"%s: the scene exercised mirror=%lu flip=%lu bank=%lu priority=%lu",
          name,(unsigned long)ref_mirrored,(unsigned long)ref_flipped,
          (unsigned long)ref_banked,(unsigned long)ref_prioritised);
  check(ref_mirrored && ref_flipped && ref_banked && ref_prioritised,msg);
}

static void tilecache_checks(void)
{
  /* The row the line produced, not the scratch: a line with no sprite on
     it leaves the scratch alone, so the scratch would compare stale to
     stale and the two checks below would pass without proving anything. */
  static unsigned char before[VDP_PIX_ROW_BYTES];
  uint32 y = 40UL;
  uint32 hs = 5UL, vs = 7UL;
  uint32 addr;
  uint32 word;
  uint32 key;
  uint32 i;

  printf("the decoded row cache\n");

  /*
   * The read side of the table computes its key from a pattern and a row,
   * the write side from a video address. They are two spellings of one
   * number and nothing but this holds them together.
   */
  {
    uint32 pat, r;
    int agree = 1;
    int inside = 1;
    for(pat = 0; pat < 512UL; pat++)
      for(r = 0; r < 8UL; r++)
        {
          uint32 k = VDP_TC_KEY_TILE(pat,r);
          if(k != VDP_TC_KEY((pat << 5) + (r << 2))) agree = 0;
          if(k >= VDP_TC_ROWS) inside = 0;
        }
    check(agree,"the key from a pattern and a row is the key from its address");
    check(inside,"every key a name table entry can name is inside the table");
  }

  tilecache_scene("scrolled",5UL,7UL,0x06UL,0x62UL,1);
  tilecache_scene("unscrolled-masked-column",0UL,0UL,0x26UL,0x62UL,1);
  tilecache_scene("inhibits-on",4UL,120UL,0xC6UL,0x62UL,1);
  tilecache_scene("display-off",4UL,120UL,0x06UL,0x22UL,0);

  /* Back to a drawn scene, and the row stroke 1 shows on line y. */
  scene(hs,vs,0x06UL,0x62UL,0);
  render_pinned(y,hs,vs);
  memcpy(before,sms.vdp.pixels[0] + (y * VDP_PIX_ROW_BYTES),VDP_PIX_ROW_BYTES);

  word = read16_le(sms.vdp.vram + 0x3800UL
                   + (((y + vs) >> 3) << 6)
                   + ((((1UL - 1UL) - (hs >> 3)) & 31UL) << 1));
  key = VDP_TC_KEY_TILE(word & 0x1FFUL,
                        ((word & 0x400UL) != 0UL) ? (7UL - ((y + vs) & 7UL))
                                                  : ((y + vs) & 7UL));
  addr = key << 2;

  /*
   * The scene fills the whole video memory at random, so the pattern this
   * column names can be one that overlaps the name table at 0x3800 or the
   * sprite table at 0x3F00. Poking there would change what is drawn rather
   * than the row that is drawn, and the two checks below would fail for a
   * reason that has nothing to do with the cache. Refused loudly rather
   * than left to the random seed.
   */
  check(addr + 4UL <= 0x3800UL,
        "the row this column shows is pattern data, clear of the tables");

  /* Poked behind the port's back: the cache must still serve what it has,
     which is the only way to see that it served at all. */
  for(i = 0; i < 4UL; i++)
    sms.vdp.vram[addr + i] = (uint8)(~sms.vdp.vram[addr + i]);
  render_pinned(y,hs,vs);
  check(memcmp(before,sms.vdp.pixels[0] + (y * VDP_PIX_ROW_BYTES),
               VDP_PIX_ROW_BYTES) == 0,
        "a row already decoded is served without reading the video memory");

  /* The same four bytes through the port, which is the one path a program
     has. Now the picture must move. */
  for(i = 0; i < 4UL; i++)
    {
      sms.vdp.code = VDP_CODE_VRAM_WRITE;
      sms.vdp.addr = addr + i;
      VDP_IO_DATA_WRITE(sms.vdp.vram[addr + i]);
    }
  render_pinned(y,hs,vs);
  check(memcmp(before,sms.vdp.pixels[0] + (y * VDP_PIX_ROW_BYTES),
               VDP_PIX_ROW_BYTES) != 0,
        "a write through the port throws the decoded row away");

#if VDP_COUNTERS
  /*
   * The count of rows thrown away, which the report publishes and nothing
   * else pins. The render just above decoded the row again, so it stands:
   * four bytes written into it throw it away ONCE, not once a byte. Four
   * more, with no render in between, find it already gone and must move
   * nothing -- which is what tells a counter that counts writes from a
   * counter that counts rows.
   */
  { uint32 standing, gone;
    sms.vdp.cnt_tc_inval = 0;
    for(i = 0; i < 4UL; i++)
      { sms.vdp.code = VDP_CODE_VRAM_WRITE;
        sms.vdp.addr = addr + i;
        VDP_IO_DATA_WRITE(sms.vdp.vram[addr + i]); }
    standing = sms.vdp.cnt_tc_inval;
    for(i = 0; i < 4UL; i++)
      { sms.vdp.code = VDP_CODE_VRAM_WRITE;
        sms.vdp.addr = addr + i;
        VDP_IO_DATA_WRITE(sms.vdp.vram[addr + i]); }
    gone = sms.vdp.cnt_tc_inval - standing;
    check(standing == 1UL,
          "a row that stood is counted thrown away once, not once per byte");
    check(gone == 0UL,
          "writing a row already thrown away throws nothing away again");
  }
  render_pinned(y,hs,vs);
#endif

  /* And what it renders now is what the reference renders now. */
  reference_bg(y,hs,vs);
  check(picture_matches(y),
        "the row decoded again is the row the video memory now holds");

  /*
   * The fine scroll now moves the ORIGIN of the picture inside the scratch
   * instead of the place each stroke is written, so that a stroke lands on
   * a word boundary. Everything that speaks in picture coordinates has to
   * follow it, and nothing else in this bench would notice if one of them
   * did not: the five variants would all be wrong together.
   *
   * So: a background that scrolling cannot change -- every name table
   * entry the same, every plane of its pattern zero -- and one sprite at a
   * fixed place. The row and the mask must come out identical under a
   * fine scroll of zero and of five. A sprite pass or a masked left column
   * left on the old origin moves them by the fine scroll.
   */
  {
    static unsigned char row_a[VDP_PIX_ROW_BYTES];
    static unsigned char prio_a[VDP_PIX_WIDTH];
    uint32 sy = 40UL;

    for(i = 0; i < VDP_VRAM_SIZE; i++)
      sms.vdp.vram[i] = 0;
    for(i = 0; i < VDP_TC_ROWS; i++)
      sms.vdp.tc_valid[i] = 0;
    /* Pattern 2, every plane set: an opaque sprite. */
    for(i = 0; i < 32UL; i++)
      sms.vdp.vram[0x2000UL + (2UL * 32UL) + i] = 0xFF;
    sms.vdp.reg[0] = 0x26;              /* left column masked, and it moves too */
    sms.vdp.reg[1] = 0x62;
    sms.vdp.reg[2] = 0x0E;
    sms.vdp.reg[5] = 0x7E;
    sms.vdp.reg[6] = 0x04;
    sms.vdp.reg[7] = 0x05;
    sms.vdp.vram[0x3F00UL] = (uint8)(sy - 8UL);
    sms.vdp.vram[0x3F01UL] = 0xD0;
    sms.vdp.vram[0x3F80UL] = 60;        /* x */
    sms.vdp.vram[0x3F81UL] = 2;         /* pattern */

    render_pinned(sy,0UL,0UL);
    /* This scene carries a sprite, so the line laid a mask and the mask
       is a thing to read. */
    memcpy(prio_a,VDP_PRIO_BYTES + sms.vdp.line_org,VDP_PIX_WIDTH);
    memcpy(row_a,sms.vdp.pixels[0] + (sy * VDP_PIX_ROW_BYTES),VDP_PIX_ROW_BYTES);

    render_pinned(sy,5UL,0UL);
    check(memcmp(prio_a,VDP_PRIO_BYTES + sms.vdp.line_org,VDP_PIX_WIDTH) == 0,
          "a fine scroll moves the origin of the mask and not what stands in picture coordinates");
    check(memcmp(row_a,sms.vdp.pixels[0] + (sy * VDP_PIX_ROW_BYTES),
                 VDP_PIX_ROW_BYTES) == 0,
          "the sprite pass writes the row from the picture's own origin");
  }

  /*
   * The two halves of "the background wins": its column carries priority,
   * which the mask holds, AND its pixel is opaque, which the sprite pass
   * reads off the row. One tile, priority on, whose pixels alternate
   * opaque and transparent; one opaque sprite over it. The sprite must
   * show through every transparent pixel and be hidden behind every
   * opaque one -- and with the priority bit off, show everywhere. Held
   * on the second palette bank too, where a transparent pixel reads as
   * sixteen in the row and would pass for opaque if the low four bits
   * were not what the test reads.
   *
   * Then a third half under a fine scroll: the mask keeps the line's
   * origin while the row is recut to the picture's, and only a mask that
   * is not uniform tells the two origins apart. Priority on every other
   * source column, the pattern opaque throughout, a fine scroll of five:
   * screen pixels 64..68 stand on source column 7, which carries no
   * priority, and 69..71 on column 8, which does. A mask read from a
   * fixed origin reads the columns five pixels to the right and hides the
   * sprite over all eight.
   */
  {
    uint32 sy = 40UL;
    uint32 nt;
    uint32 bank;
    int ok_pri = 1;
    int ok_none = 1;
    int ok_org = 1;

    for(bank = 0; bank < 2UL; bank++)
      {
        for(i = 0; i < VDP_VRAM_SIZE; i++)
          sms.vdp.vram[i] = 0;
        for(i = 0; i < VDP_TC_ROWS; i++)
          sms.vdp.tc_valid[i] = 0;
        /* Pattern 1: plane 0 set on the even pixels of every row (bit 7 is
           pixel 0), the other planes clear -- index 1 and 0 alternating. */
        for(i = 0; i < 8UL; i++)
          sms.vdp.vram[32UL + (i * 4UL)] = 0xAA;
        /* Pattern 2 of the sprite bank, every plane set: an opaque sprite. */
        for(i = 0; i < 32UL; i++)
          sms.vdp.vram[0x2000UL + (2UL * 32UL) + i] = 0xFF;
        sms.vdp.reg[0] = 0x06;
        sms.vdp.reg[1] = 0x62;
        sms.vdp.reg[2] = 0x0E;
        sms.vdp.reg[5] = 0x7E;
        sms.vdp.reg[6] = 0x04;
        sms.vdp.reg[7] = 0x05;
        sms.vdp.vram[0x3F00UL] = (uint8)(sy - 8UL);
        sms.vdp.vram[0x3F01UL] = 0xD0;
        sms.vdp.vram[0x3F80UL] = 64;        /* x */
        sms.vdp.vram[0x3F81UL] = 2;         /* pattern */

        /* Every name table entry: pattern 1, priority on, the bank asked. */
        for(nt = 0; nt < (28UL * 32UL); nt++)
          write16_le(sms.vdp.vram + 0x3800UL + (nt * 2UL),
                     0x1001UL | (bank << 11));
        render_pinned(sy,0UL,0UL);
        for(i = 0; i < 8UL; i++)
          {
            uint32 got = sms.vdp.pixels[0][(sy * VDP_PIX_ROW_BYTES) + 64UL + i];
            uint32 want = ((i & 1UL) == 0UL) ? (1UL | (bank << 4)) : 31UL;
            if(got != want) ok_pri = 0;
          }

        /* The same, priority off: the sprite shows over every pixel. */
        for(nt = 0; nt < (28UL * 32UL); nt++)
          write16_le(sms.vdp.vram + 0x3800UL + (nt * 2UL),
                     0x0001UL | (bank << 11));
        for(i = 0; i < VDP_TC_ROWS; i++)
          sms.vdp.tc_valid[i] = 0;
        render_pinned(sy,0UL,0UL);
        for(i = 0; i < 8UL; i++)
          if(sms.vdp.pixels[0][(sy * VDP_PIX_ROW_BYTES) + 64UL + i] != 31U)
            ok_none = 0;

        /* Pattern 1 opaque on every pixel; priority on the even source
           columns only; a fine scroll of five. */
        for(i = 0; i < 8UL; i++)
          sms.vdp.vram[32UL + (i * 4UL)] = 0xFF;
        for(nt = 0; nt < (28UL * 32UL); nt++)
          write16_le(sms.vdp.vram + 0x3800UL + (nt * 2UL),
                     0x0001UL | (bank << 11)
                     | (((nt & 1UL) == 0UL) ? 0x1000UL : 0UL));
        for(i = 0; i < VDP_TC_ROWS; i++)
          sms.vdp.tc_valid[i] = 0;
        render_pinned(sy,5UL,0UL);
        for(i = 0; i < 8UL; i++)
          {
            uint32 got = sms.vdp.pixels[0][(sy * VDP_PIX_ROW_BYTES) + 64UL + i];
            uint32 want = (i < 5UL) ? 31UL : (1UL | (bank << 4));
            if(got != want) ok_org = 0;
          }
      }
    check(ok_pri,
          "a priority column hides the sprite behind its opaque pixels only, on both banks");
    check(ok_none,
          "without priority the sprite shows over opaque and transparent alike");
    check(ok_org,
          "under a fine scroll the mask is read from the line's origin, where its columns stand, on both banks");
  }

  /* A colour write moves nothing in the cache: the address it carries is a
     colour index, and a row of the same number must survive it. */
  sms.vdp.tc_valid[3] = 1;
  sms.vdp.code = VDP_CODE_CRAM_WRITE;
  sms.vdp.addr = 12UL;
  VDP_IO_DATA_WRITE(0x2A);
  check(sms.vdp.tc_valid[3] == 1,
        "a colour write leaves the decoded rows alone");
  /* Marked by hand and never decoded: left standing it would be served to
     whatever renders next. This pass does not get to poison the ones after
     it. */
  sms.vdp.tc_valid[3] = 0;
}

/* ------------------------------------------------------------------ */
/*
 * The line that never touches the scratch, held to the pixel.
 *
 * Two claims, and each is checked in its own right: one of them alone
 * would pass on a picture that is wrong the same way twice.
 *
 *   The row a sprite-free line emits is byte for byte the row a
 *   composition that owes this path nothing lays down -- under each of
 *   the eight fine scrolls, with the left column masked and without --
 *   and the scratch is not written while it happens.
 *
 *   The same background rendered the sprite way comes out the same row,
 *   that row is the reference's, and the mask holds what the reference
 *   composed.
 *
 * The second is what holds the two ways of rendering a line together: the
 * composition is written twice in the source, and nothing else here would
 * notice if one copy changed. The sprite way is forced by sprites that
 * draw nothing at all -- register 0 bit 3 moves a sprite eight pixels
 * left, so a sprite whose horizontal byte is zero covers pixels -8 to -1
 * and every pixel of it is dropped before anything is written and before
 * any collision is seen. Twenty-four of them, eight lines apart, put one
 * on every line of the picture and never nine on any.
 */
static unsigned char frame_a[VDP_PIX_BUF_BYTES];

static void fastpath_scene(const char *name, uint32 hs, uint32 vs,
                           uint32 r0, uint32 r1)
{
  uint32 y;
  int same = 1;
  int clean = 1;
  char msg[160];

  scene(hs,vs,r0,r1,0);
#if VDP_COUNTERS
  sms.vdp.cnt_line_fast = 0;
  sms.vdp.cnt_line_scratch = 0;
#endif
  /* The short way carries a row cursor of its own that walks three words
     at a time, which is the shape that runs off the end of a row. A
     comparison line by line in rising y would not see it: the overrun
     lands in the row after, and the render after that writes over it. The
     bands on either side of the picture buffer do see it. */
  guard_arm();

  for(y = 0; y < VDP_ACTIVE_LINES; y++)
    {
      scratch_arm();
      render_pinned(y,hs,vs);
      if(!scratch_untouched()) clean = 0;
      reference_bg(y,hs,vs);
      if(!picture_matches(y)) same = 0;
    }

  sprintf(msg,"%s: every row is the row the byte reference composes",name);
  check(same,msg);
  sprintf(msg,"%s: the priority scratch was not written on any line",name);
  check(clean,msg);
  sprintf(msg,"%s: nothing was written outside the picture buffer",name);
  check(guard_intact(),msg);
#if VDP_COUNTERS
  sprintf(msg,"%s: every line took the short way (fast=%lu scratch=%lu)",name,
          (unsigned long)sms.vdp.cnt_line_fast,
          (unsigned long)sms.vdp.cnt_line_scratch);
  check((sms.vdp.cnt_line_fast == VDP_ACTIVE_LINES)
        && (sms.vdp.cnt_line_scratch == 0UL),msg);
#endif
}

/*
 * Every name table entry pointed at a pattern of the first eight
 * kilobytes. The scene fills the whole video memory at random, so an entry
 * can name a pattern that overlaps the sprite attribute table -- and the
 * two renders below differ by what stands in that table. Trimmed, the
 * background cannot see the difference and the comparison means what it
 * says.
 */
static void nt_trim(void)
{
  uint32 i;
  uint8 *p;
  uint32 w;

  for(i = 0; i < (28UL * 32UL); i++)
    {
      p = sms.vdp.vram + 0x3800UL + (i * 2UL);
      w = (uint32)read16_le(p);
      write16_le(p,w & ~0x100UL);
    }
}

/*
 * The sprite patterns of the two_ways scenes, blanked. A sprite that draws
 * nothing is the whole point of that pass, and the horizontal shift alone
 * only guarantees it for a sprite eight pixels wide: magnified, the sprite
 * is sixteen and its right half lands on the picture. Every plane of the
 * patterns it can name is zeroed instead, so every one of its pixels is
 * index zero and is dropped before anything is written or any collision is
 * seen, at any width and any height.
 *
 * Where it is safe to zero: the sprite patterns come from the second eight
 * kilobytes (register 6 bit 2 is set by scene), so from 0x2000 up, and
 * nt_trim has already put every background pattern below 0x2000. Blanking
 * here therefore cannot move the background, which is what the two renders
 * are compared on.
 */
static void spr_blank(void)
{
  uint32 i;

  for(i = 0; i < 128UL; i++)
    sms.vdp.vram[0x2000UL + i] = 0;
}

static void two_ways(const char *name, uint32 hs, uint32 vs,
                     uint32 r0, uint32 r1)
{
  uint32 y;
  uint32 i;
  uint32 seed;
  int sc = 1;
  int pr = 1;
  int tail = 1;
  char msg[160];
#if VDP_COUNTERS
  int firstfast;
#endif

  /* The same seed twice, so the two renders differ by the sprite table
     and by nothing else. */
  seed = rng;
  scene(hs,vs,r0 | 0x08UL,r1,0);
  nt_trim();
  spr_blank();
#if VDP_COUNTERS
  sms.vdp.cnt_line_fast = 0;
  sms.vdp.cnt_line_scratch = 0;
#endif
  guard_arm();
  for(y = 0; y < VDP_ACTIVE_LINES; y++)
    render_pinned(y,hs,vs);
  memcpy(frame_a,sms.vdp.pixels[0],VDP_PIX_BUF_BYTES);
  sprintf(msg,"%s: nothing was written outside the picture buffer, short way",name);
  check(guard_intact(),msg);
#if VDP_COUNTERS
  /*
   * Read BEFORE the second render, and checked: if the choice of path ever
   * stopped firing, this pass would compare the scratch way against itself
   * and still print that the two agree.
   */
  firstfast = ((sms.vdp.cnt_line_fast == VDP_ACTIVE_LINES)
               && (sms.vdp.cnt_line_scratch == 0UL));
  sprintf(msg,"%s: the first render took the short way (fast=%lu scratch=%lu)",
          name,(unsigned long)sms.vdp.cnt_line_fast,
          (unsigned long)sms.vdp.cnt_line_scratch);
  check(firstfast,msg);
#endif

  rng = seed;
  scene(hs,vs,r0 | 0x08UL,r1,0);
  nt_trim();
  spr_blank();
  for(i = 0; i < 24UL; i++)
    {
      sms.vdp.vram[0x3F00UL + i] = (uint8)(((i * 8UL) - 1UL) & 0xFFUL);
      sms.vdp.vram[0x3F80UL + (i * 2UL)] = 0;
      sms.vdp.vram[0x3F80UL + (i * 2UL) + 1UL] = 0;
    }
  sms.vdp.vram[0x3F00UL + 24UL] = 0xD0;
  sms.vdp.spr_overflow = 0;
  sms.vdp.spr_collision = 0;
#if VDP_COUNTERS
  sms.vdp.cnt_line_fast = 0;
  sms.vdp.cnt_line_scratch = 0;
#endif
  guard_arm();

  for(y = 0; y < VDP_ACTIVE_LINES; y++)
    {
      /*
       * The tail of the scratch, armed around the render that lays a mask
       * into it. This is the only pass where the loop of thirty-three
       * strokes writes the scratch, so it is the only place that can hold
       * it to the promise of vdp.h: the strokes stop at byte 263 and
       * nothing writes past it. The scenes with no sprite cannot check it
       * -- they write no scratch at all.
       */
      tail_arm();
      render_pinned(y,hs,vs);
      if(!tail_intact()) tail = 0;
      reference_bg(y,hs,vs);
      if(!picture_matches(y)) sc = 0;
      if(memcmp(VDP_PRIO_BYTES + sms.vdp.line_org,
                ref_prio + VDP_LINE_LEAD,VDP_PIX_WIDTH) != 0) pr = 0;
    }

  sprintf(msg,"%s: the sprite way draws the row the short way drew",name);
  check(memcmp(frame_a,sms.vdp.pixels[0],VDP_PIX_BUF_BYTES) == 0,msg);
  sprintf(msg,"%s: the sprite way's row is the reference picture",name);
  check(sc,msg);
  sprintf(msg,"%s: the priority mask holds the reference mask",name);
  check(pr,msg);
  sprintf(msg,"%s: nothing written past the picture in the scratch",name);
  check(tail,msg);
  sprintf(msg,"%s: nothing was written outside the picture buffer, scratch way",name);
  check(guard_intact(),msg);
  sprintf(msg,"%s: nothing was drawn and nothing collided",name);
  check((sms.vdp.spr_collision == 0UL) && (sms.vdp.spr_overflow == 0UL),msg);
#if VDP_COUNTERS
  sprintf(msg,"%s: every line went through the scratch (fast=%lu scratch=%lu)",
          name,(unsigned long)sms.vdp.cnt_line_fast,
          (unsigned long)sms.vdp.cnt_line_scratch);
  check((sms.vdp.cnt_line_scratch == VDP_ACTIVE_LINES)
        && (sms.vdp.cnt_line_fast == 0UL),msg);
#endif
}

/* ------------------------------------------------------------------ */
/*
 * The two byte orders, both of them, held against the byte run.
 *
 * The render moves pixel indexes inside words, and where a lane sits
 * inside a word is the byte order of the machine. vdp.h therefore carries
 * two forms of the recut and picks one. Only the picked one is on the
 * render's path -- but if only the picked one were ever COMPILED, the
 * form that ships to the console would be built by nothing and tested by
 * nothing on the machine this bench runs on, and a mistake in it would
 * ride all the way out with every check green. That was the case and it
 * was demonstrated, on the packing form the format once had: a single
 * digit changed in the big endian packing, which would scramble every
 * group of four pixels of every picture on the console, left this bench
 * entirely green.
 *
 * So both forms are spelled unconditionally (VDP_LANE_JOIN_MSB / _LSB)
 * and both are driven here. Each is fed words laid out the way ITS OWN
 * order would lay a run of index bytes, and each must produce the words
 * its own order lays for that same run begun r bytes further on -- a
 * reference that is built from bytes and owes the recut nothing. The
 * recut offset is swept over its four values, so the shift of thirty two
 * that r == 0 would ask for is walked too.
 *
 * The emitter is driven with them: what it stores is what it was handed,
 * two words to a stroke, in the row's order. That it is right in both
 * orders is not a property of the emitter but of where the words come
 * from (vdp.h, VDP_EMIT8), and the scenes of this bench exercise it for
 * real in this machine's order: a store that put the lanes down backwards
 * would fail every row of every scene against the byte reference.
 */
#define LANE_WORDS ((VDP_PIX_WIDTH / 4UL) + 1UL)
/* One word more than the recut reads, because the reference is laid from
   the run begun r bytes on, r up to three, and reads LANE_WORDS words from
   there. */
#define LANE_BYTES ((LANE_WORDS + 1UL) * 4UL)

static void lane_words_msb(const unsigned char *ind, uint32 *w)
{ uint32 k;
  for(k = 0; k < LANE_WORDS; k++)
    w[k] = ((uint32)ind[k * 4UL] << 24) | ((uint32)ind[(k * 4UL) + 1UL] << 16)
         | ((uint32)ind[(k * 4UL) + 2UL] << 8) | (uint32)ind[(k * 4UL) + 3UL]; }

static void lane_words_lsb(const unsigned char *ind, uint32 *w)
{ uint32 k;
  for(k = 0; k < LANE_WORDS; k++)
    w[k] = (uint32)ind[k * 4UL] | ((uint32)ind[(k * 4UL) + 1UL] << 8)
         | ((uint32)ind[(k * 4UL) + 2UL] << 16)
         | ((uint32)ind[(k * 4UL) + 3UL] << 24); }

static void lane_emit_msb(const uint32 *w, uint32 sl, uint32 sr, uint32 *out)
{ uint32 n, g0, g1;
  for(n = 0; n < (VDP_PIX_WIDTH / 8UL); n++)
    { g0 = VDP_LANE_JOIN_MSB(w[0],w[1],sl,sr);
      g1 = VDP_LANE_JOIN_MSB(w[1],w[2],sl,sr);
      VDP_EMIT8(g0,g1,out);
      w += 2; out += 2; } }

static void lane_emit_lsb(const uint32 *w, uint32 sl, uint32 sr, uint32 *out)
{ uint32 n, g0, g1;
  for(n = 0; n < (VDP_PIX_WIDTH / 8UL); n++)
    { g0 = VDP_LANE_JOIN_LSB(w[0],w[1],sl,sr);
      g1 = VDP_LANE_JOIN_LSB(w[1],w[2],sl,sr);
      VDP_EMIT8(g0,g1,out);
      w += 2; out += 2; } }

static void lane_order_checks(void)
{
  static unsigned char ind[LANE_BYTES];
  static uint32 w[LANE_WORDS];
  static uint32 got[VDP_PIX_ROW_BYTES / 4];
  static uint32 want[LANE_WORDS];
  uint32 pat, r, i, sl, sr, seed;
  char msg[160];

  for(pat = 0; pat < 5UL; pat++)
    {
      /*
       * Five runs of indexes, each catching a different mistake: a rising
       * one catches lanes taken in the wrong order, a walking bit catches
       * a lane shifted by one, a scattered one catches what neither does.
       *
       * The last two walk the whole byte. The render puts nothing above
       * 31 in a row, but a recut moves bytes and not indexes, and it is
       * held to the byte: the domain is walked to 255, and the top value
       * is laid down flat as well.
       */
      seed = 0x2545F491UL;
      for(i = 0; i < LANE_BYTES; i++)
        {
          if(pat == 0UL)      ind[i] = (unsigned char)(i & 31UL);
          else if(pat == 1UL) ind[i] = (unsigned char)(1UL << (i % 5UL));
          else if(pat == 2UL)
            { seed = (seed * 1103515245UL) + 12345UL;
              ind[i] = (unsigned char)((seed >> 17) & 31UL); }
          else if(pat == 3UL) ind[i] = (unsigned char)((i * 13UL) & 255UL);
          else                ind[i] = 255;
        }

      for(r = 0; r < 4UL; r++)
        {
          sl = r * 8UL;
          sr = 31UL - sl;

          lane_words_msb(ind,w);
          lane_emit_msb(w,sl,sr,got);
          lane_words_msb(ind + r,want);
          sprintf(msg,"pattern %lu offset %lu: the big endian recut lays the byte run begun %lu bytes on",
                  (unsigned long)pat,(unsigned long)r,(unsigned long)r);
          check(memcmp(got,want,VDP_PIX_ROW_BYTES) == 0,msg);

          lane_words_lsb(ind,w);
          lane_emit_lsb(w,sl,sr,got);
          lane_words_lsb(ind + r,want);
          sprintf(msg,"pattern %lu offset %lu: the little endian recut lays the byte run begun %lu bytes on",
                  (unsigned long)pat,(unsigned long)r,(unsigned long)r);
          check(memcmp(got,want,VDP_PIX_ROW_BYTES) == 0,msg);
        }
    }

  /* And the form this build actually calls is one of the two, not a third
     one that drifted: the switch is a choice, never a copy. */
#if VDP_LANE_MSB_FIRST
  check(VDP_LANE_JOIN(0x01020304UL,0x05060708UL,8UL,23UL)
           == VDP_LANE_JOIN_MSB(0x01020304UL,0x05060708UL,8UL,23UL),
        "this build calls the big endian recut and nothing else");
#else
  check(VDP_LANE_JOIN(0x01020304UL,0x05060708UL,8UL,23UL)
           == VDP_LANE_JOIN_LSB(0x01020304UL,0x05060708UL,8UL,23UL),
        "this build calls the little endian recut and nothing else");
#endif

  /*
   * The emitter on its own, in this machine's order: two words handed to
   * it must come back as the eight bytes they were built from, in memory
   * order. This is the one place the store is held to the byte by name
   * rather than through a scene.
   */
  {
    static const unsigned char eight[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };
    uint32 two[2];
    uint32 row[2];
    uint32 k;

    for(k = 0; k < 2UL; k++)
      {
#if VDP_LANE_MSB_FIRST
        two[k] = ((uint32)eight[k * 4UL] << 24) | ((uint32)eight[(k * 4UL) + 1UL] << 16)
               | ((uint32)eight[(k * 4UL) + 2UL] << 8) | (uint32)eight[(k * 4UL) + 3UL];
#else
        two[k] = (uint32)eight[k * 4UL] | ((uint32)eight[(k * 4UL) + 1UL] << 8)
               | ((uint32)eight[(k * 4UL) + 2UL] << 16) | ((uint32)eight[(k * 4UL) + 3UL] << 24);
#endif
      }
    VDP_EMIT8(two[0],two[1],row);
    check(memcmp(row,eight,8) == 0,
          "the emitter lays the two words it is handed as eight bytes in picture order");
  }
}

static void fastpath_checks(void)
{
  uint32 f;
  char nm[64];

  printf("the line that skips the scratch\n");

  lane_order_checks();

  for(f = 0; f < 8UL; f++)
    {
      sprintf(nm,"fine-%lu",(unsigned long)f);
      fastpath_scene(nm,f,7UL,0x06UL,0x62UL);
      sprintf(nm,"fine-%lu-masked",(unsigned long)f);
      fastpath_scene(nm,f,7UL,0x26UL,0x62UL);
    }

  /* Both ways at every fine scroll, and the left column masked on the odd
     ones: the sprite way recuts on the same eight values the short way
     does, and a defect in one of them would otherwise wait for a game
     that scrolls by that many pixels. */
  for(f = 0; f < 8UL; f++)
    {
      sprintf(nm,"both-ways-fine-%lu",(unsigned long)f);
      two_ways(nm,f,7UL,((f & 1UL) != 0UL) ? 0x26UL : 0x06UL,0x62UL);
    }

  /*
   * The shapes of scene the row cache pass used to hold the priority mask
   * on, and cannot any more: it renders no line through the scratch. Only
   * this pass does, so the vertical scroll inhibit and the tall and
   * magnified sprite modes are brought here, or the mask the sprite pass
   * reads would be compared against the reference on one shape of scene
   * only.
   */
  two_ways("both-ways-vsi",4UL,120UL,0xC6UL,0x62UL);
  two_ways("both-ways-vsi-masked",4UL,120UL,0xE6UL,0x62UL);
  two_ways("both-ways-tall-magnified",3UL,200UL,0x2EUL,0x63UL);
}

/* FNV-1a, so the two builds can be compared across processes. */
static unsigned long digest(const unsigned char *p, unsigned long n)
{ unsigned long h = 2166136261UL; unsigned long i;
  for(i = 0; i < n; i++) { h ^= (unsigned long)p[i]; h *= 16777619UL; h &= 0xFFFFFFFFUL; }
  return h; }

static int32 only = -1;
static int   digest_mode = 0;

static void one(const char *name, uint32 hs, uint32 vs, uint32 r0, uint32 r1, int sprites)
{
  uint32 v;

  if(!digest_mode) printf("%s\n",name);
  scene(hs,vs,r0,r1,sprites);
  snap = sms.vdp;

  if(only >= 0)
    { run_variant((uint32)only); return; }

  if(digest_mode)
    {
      /* The control alone: what is being compared across the two builds is
         the frame the delivered form renders against the frame the
         instrumented control renders, and the other four variants have no
         counterpart in a build that has none. */
      run_variant(0);
      printf("%s pix=%08lx prio=%08lx bits=%08lx\n",name,
             digest(pix[0],VDP_PIX_BUF_BYTES),
             digest(prio[0],VDP_LINE_SCRATCH),
             digest(bits[0],VDP_ACTIVE_LINES));
      return;
    }

  for(v = 0; v < VARIANTS; v++)
    run_variant(v);
#if VDP_PROFILE
  compare(name);
#endif
}

static void scenes(void)
{
  /* fine scroll on: 33 strokes, the first landing in the lead */
  one("fine-scroll-sprites-masked",5UL,7UL,0x26UL,0x62UL,1);
  /* fine scroll off: 32 strokes */
  one("no-fine-scroll-sprites",0UL,0UL,0x06UL,0x62UL,1);
  /* magnified and tall sprites */
  one("tall-magnified-sprites",3UL,200UL,0x2EUL,0x63UL,1);
  /* exactly eight sprites on a line: the boundary of the kept list */
  one("eight-sprites-on-a-line",2UL,9UL,0x06UL,0x62UL,2);
  /* no sprites at all: the composition never runs */
  one("no-sprites",1UL,17UL,0x06UL,0x60UL,0);
  /* vertical scroll inhibited on the right columns */
  one("vsi-on-hsi-on",4UL,120UL,0xC6UL,0x62UL,1);
  /* display off: the blank branch, which carries no post */
  one("display-off",4UL,120UL,0x06UL,0x22UL,1);
}

int
main(int argc, char **argv)
{
  if(argc > 1)
    {
      if(strcmp(argv[1],"variants") == 0)
        {
          /* The script drives its variant list and its expectations off
             this, so that a sixth variant is exercised the day it exists
             instead of silently skipped. */
          printf("%lu\n",(unsigned long)VARIANTS);
          return 0;
        }
      if(strcmp(argv[1],"digest") == 0)
        { digest_mode = 1; quiet = 1; }
      else
        {
          only = (int32)atoi(argv[1]);
          /* Anything outside the rotation would index past four arrays.
             Refused, and loudly: a coverage run driven by a stale list
             must not corrupt memory and report success. */
          if((only < 0) || (only >= (int32)VARIANTS))
            { printf("variant %ld is not one of the %lu\n",
                     (long)only,(unsigned long)VARIANTS);
              return 2; }
        }
    }

  /*
   * The refusal path first, because it can only be seen once: a block that
   * was taken is kept in a static and a second init never asks again. What
   * is proved is that the row cache answers with its OWN code -- a caller
   * that painted the video memory screen for it would name the wrong block
   * to the player.
   */
  tc_refuse = 1;
  check(vdp_init() == VDP_ERR_NO_TILECACHE,
        "a refused row cache stops the boot with a code of its own");
  tc_refuse = 0;

  if(vdp_init() < 0) { printf("vdp_init refused\n"); return 1; }

#if VDP_PROFILE
  if(!digest_mode && (only < 0))
    {
      uint32 v;
      char msg[160];
      printf("repetition counts\n");
      for(v = 0; v < VARIANTS; v++)
        {
          vdp_profile_select(v);
          sprintf(msg,"variant %lu: bg=%lu spr=%lu",(unsigned long)v,
                  (unsigned long)vdp_profile_reps(VDP_POST_BG),
                  (unsigned long)vdp_profile_reps(VDP_POST_SPRITES));
          check(vdp_profile_reps(VDP_POST_BG) ==
                  ((v == VDP_PROFILE_BG || v == VDP_PROFILE_ALL) ? 2UL : 1UL) &&
                vdp_profile_reps(VDP_POST_SPRITES) ==
                  ((v == VDP_PROFILE_SPRITES || v == VDP_PROFILE_ALL) ? 2UL : 1UL),msg);
        }
      vdp_profile_select(99UL);
      check(vdp_profile_reps(VDP_POST_BG) == 1UL,
            "a variant past the last arms the control");
    }
#endif

  if(!digest_mode && (only < 0))
    { printf("\n"); tilecache_checks(); printf("\n");
      fastpath_checks(); printf("\n"); }

  /*
   * The scenes below draw their video memory out of the generator above,
   * and the pass that may or may not have run just drew from it too. Reset
   * so that the seven scenes are the same seven whatever ran before them:
   * otherwise the frames the identity pass compares and the frames the two
   * builds digest against each other would be different frames.
   */
  rng = 0x1234567UL;

  scenes();

  /* The verdict is rendered in EVERY mode. A failure during the coverage
     pass or the digest pass has to leave with it, or the pass that found
     it reports success. */
  if(!digest_mode) printf("\nfailed=%d\n",failed);
  return failed ? 1 : 0;
}
