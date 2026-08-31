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
  memset(PIXELS + VDP_PIX_BUF_BYTES,GUARD_FILL,GUARD_BYTES); }

static int guard_intact(void)
{ int i;
  for(i = 0; i < GUARD_BYTES; i++)
    { if(pixels_pool[i] != GUARD_FILL) return 0;
      if(PIXELS[VDP_PIX_BUF_BYTES + i] != GUARD_FILL) return 0; }
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
  memcpy(scratch[v],sms.vdp.line,VDP_LINE_SCRATCH);
  memcpy(prio[v],sms.vdp.prio,VDP_LINE_SCRATCH);
}

static void scene(uint32 hs, uint32 vs, uint32 r0, uint32 r1, int sprites)
{
  uint32 i;

  for(i = 0; i < VDP_VRAM_SIZE; i++)
    sms.vdp.vram[i] = (uint8)rnd();
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

  scenes();

  /* The verdict is rendered in EVERY mode. A failure during the coverage
     pass or the digest pass has to leave with it, or the pass that found
     it reports success. */
  if(!digest_mode) printf("\nfailed=%d\n",failed);
  return failed ? 1 : 0;
}
