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
static unsigned char scratch[VARIANTS][VDP_LINE_SCRATCH];
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
 * The sixteen bytes past the picture in the composition scratch. The
 * strokes stop at byte 263 and the picture ends there at the latest, so
 * nothing may write from VDP_LINE_LEAD + VDP_PIX_WIDTH upward. Filled with
 * the guard pattern rather than tested for zero: a stroke written one
 * stroke too far would lay indexes there, and some of them are zero.
 */
static void tail_arm(void)
{ memset(VDP_LINE_BYTES + VDP_LINE_LEAD + VDP_PIX_WIDTH,GUARD_FILL,VDP_LINE_TAIL);
  memset(VDP_PRIO_BYTES + VDP_LINE_LEAD + VDP_PIX_WIDTH,GUARD_FILL,VDP_LINE_TAIL); }

static int tail_intact(void)
{ uint32 i;
  for(i = 0; i < VDP_LINE_TAIL; i++)
    { if(VDP_LINE_BYTES[VDP_LINE_LEAD + VDP_PIX_WIDTH + i] != GUARD_FILL) return 0;
      if(VDP_PRIO_BYTES[VDP_LINE_LEAD + VDP_PIX_WIDTH + i] != GUARD_FILL) return 0; }
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
  memcpy(scratch[v],VDP_LINE_BYTES,VDP_LINE_SCRATCH);
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
      sprintf(msg,"%s: variant %lu composition scratch identical",name,(unsigned long)v);
      check(memcmp(scratch[v],scratch[0],VDP_LINE_SCRATCH) == 0,msg);
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
      for(x = 0; x < 8UL; x++)
        {
          uint32 k = ((word & 0x200UL) != 0UL) ? (7UL - x) : x;
          idx = (uint32)t0[k] | (uint32)t1[k] | (uint32)t2[k] | (uint32)t3[k];
          dst[x] = (uint8)(idx | bank);
          pd[x] = (uint8)(pr & (idx != 0UL));
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

static int picture_matches(void)
{
  return memcmp(VDP_LINE_BYTES + sms.vdp.line_org,
                ref_line + VDP_LINE_LEAD,VDP_PIX_WIDTH) == 0
      && memcmp(VDP_PRIO_BYTES + sms.vdp.line_org,
                ref_prio + VDP_LINE_LEAD,VDP_PIX_WIDTH) == 0;
}

static void tilecache_scene(const char *name, uint32 hs, uint32 vs,
                            uint32 r0, uint32 r1, int drawn)
{
  uint32 y;
  int same = 1;
  int tail = 1;
  char msg[160];
#if VDP_COUNTERS
  uint32 miss_first, miss_second;
#endif

  scene(hs,vs,r0,r1,0);          /* no sprite: the scratch is background */
  ref_mirrored = ref_flipped = ref_banked = ref_prioritised = 0;
#if VDP_COUNTERS
  sms.vdp.cnt_tc_hit = 0;
  sms.vdp.cnt_tc_miss = 0;
#endif

  for(y = 0; y < VDP_ACTIVE_LINES; y++)
    {
      tail_arm();
      render_pinned(y,hs,vs);
      if(!tail_intact()) tail = 0;
      reference_bg(y,hs,vs);
      if(!picture_matches()) same = 0;
    }
  sprintf(msg,"%s: every line matches a composition that owes the cache nothing",name);
  check(same,msg);
  sprintf(msg,"%s: nothing written past the picture in either scratch",name);
  check(tail,msg);

  /* The blank branch fills the row and returns before the background post:
     it has no row to decode, and the three counts below would be zero for
     a right reason. Its picture is checked above like every other. */
  if(!drawn) return;

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
  static unsigned char before[VDP_PIX_WIDTH];
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
  memcpy(before,VDP_LINE_BYTES + sms.vdp.line_org,VDP_PIX_WIDTH);

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
  check(memcmp(before,VDP_LINE_BYTES + sms.vdp.line_org,VDP_PIX_WIDTH) == 0,
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
  check(memcmp(before,VDP_LINE_BYTES + sms.vdp.line_org,VDP_PIX_WIDTH) != 0,
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
  check(picture_matches(),
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
   * fixed place. The composed row and the packed row must come out
   * identical under a fine scroll of zero and of five. A sprite pass, a
   * masked left column or a packer left on the old origin moves them by
   * the fine scroll.
   */
  {
    static unsigned char row_a[VDP_PIX_ROW_BYTES];
    static unsigned char scratch_a[VDP_PIX_WIDTH];
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
    memcpy(scratch_a,VDP_LINE_BYTES + sms.vdp.line_org,VDP_PIX_WIDTH);
    memcpy(row_a,sms.vdp.pixels[0] + (sy * VDP_PIX_ROW_BYTES),VDP_PIX_ROW_BYTES);

    render_pinned(sy,5UL,0UL);
    check(memcmp(scratch_a,VDP_LINE_BYTES + sms.vdp.line_org,VDP_PIX_WIDTH) == 0,
          "a fine scroll moves the origin and not what stands in picture coordinates");
    check(memcmp(row_a,sms.vdp.pixels[0] + (sy * VDP_PIX_ROW_BYTES),
                 VDP_PIX_ROW_BYTES) == 0,
          "the packer reads the picture from the origin the background left");
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
          sprintf(msg,"variant %lu: bg=%lu spr=%lu pack=%lu",(unsigned long)v,
                  (unsigned long)vdp_profile_reps(VDP_POST_BG),
                  (unsigned long)vdp_profile_reps(VDP_POST_SPRITES),
                  (unsigned long)vdp_profile_reps(VDP_POST_PACK));
          check(vdp_profile_reps(VDP_POST_BG) ==
                  ((v == VDP_PROFILE_BG || v == VDP_PROFILE_ALL) ? 2UL : 1UL) &&
                vdp_profile_reps(VDP_POST_SPRITES) ==
                  ((v == VDP_PROFILE_SPRITES || v == VDP_PROFILE_ALL) ? 2UL : 1UL) &&
                vdp_profile_reps(VDP_POST_PACK) ==
                  ((v == VDP_PROFILE_PACK || v == VDP_PROFILE_ALL) ? 2UL : 1UL),msg);
        }
      vdp_profile_select(99UL);
      check(vdp_profile_reps(VDP_POST_BG) == 1UL,
            "a variant past the last arms the control");
    }
#endif

  if(!digest_mode && (only < 0))
    { printf("\n"); tilecache_checks(); printf("\n"); }

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
