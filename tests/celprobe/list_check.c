/* Host check of the cel probe's lists and pattern.
 *
 * Every figure the probe publishes is the time of a list the cel engine
 * draws, and the figure means what the probe says it means only if the
 * list is what the probe says it is: the two decor windows of a band read
 * the band's rows once each and nothing else; the bands together read the
 * visible picture once; the 896 tiles read the whole picture once; every
 * source is a word address; every preamble word is the pair the render's
 * arbiter proved on the console; every chain ends on exactly one last
 * cel. A window four pixels too wide would read a strip twice, time a
 * little more, and publish that as the cost of a cel -- and no run would
 * say so. The pattern is checked too, since the colour proof is read off
 * it, and so is the rule the decision line applies.
 *
 * The probe's builders are static, so this file includes the probe's
 * source and calls them directly; what the probe would call into the
 * console is stubbed below and never reached by a check.
 *
 *   list_check            -> [OK]/[FAIL] lines, then failed=N
 *
 * Exit status: 0 when every check passed, 1 otherwise.
 */
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

/* The host's cel structure is wider than the console's (pointers of eight
   bytes), so 896 of them need more than a console page here. Twice the
   page is room enough, and the probe's own arithmetic is what is checked,
   not the page it fits on the console -- that is a console fact. */
#define CELPROBE_PAGE_BYTES 131072UL

#include CELPROBE_SRC

/* ---- the console, as far as the builders reach ---- */

static uint32 clock_now = 0;
/* Knobs of the stubs: which allocation refuses, which clock reading jumps
   by two seconds, how many draw calls refuse, whether the list is refused. */
static int alloc_calls = 0;
static int alloc_fail_at = 0;
static int clock_calls_to_spike = 0;
static int draw_fail_left = 0;
static int vdl_refuse = 0;
static int setvdl_refuse = 0;

void *sys_alloc(const char *name, int32 size, uint32 memtype)
{
  void *p;
  (void)name; (void)memtype;
  alloc_calls++;
  if(alloc_calls == alloc_fail_at)
    return NULL;
  p = malloc((size_t)size);
  if(p != NULL) memset(p,0,(size_t)size);
  return p;
}
uint32 sys_usec(void)
{
  clock_now += 100UL;
  if(clock_calls_to_spike > 0 && --clock_calls_to_spike == 0)
    clock_now += 2000000UL;
  return clock_now;
}
int32 sys_width(void) { return 320; }
int32 sys_height(void) { return 240; }
Item sys_bitmap(void) { return 1; }
Item sys_screen_at(int32 i) { return (Item)(i + 1); }
Item sys_bitmap_at(int32 i) { return (Item)(i + 1); }
int32 sys_screen_count(void) { return 2; }
int32 sys_screen_index(void) { return 0; }
Err sys_display_show(void) { return 0; }
Err sys_vbl_wait(uint32 fields) { (void)fields; return 0; }

Err DrawCels(Item b, CCB *c)
{
  (void)b; (void)c;
  if(draw_fail_left > 0) { draw_fail_left--; return -1; }
  return 0;
}
Err SetScreenColors(Item s, uint32 *e, int32 n) { (void)s; (void)e; (void)n; return 0; }
Err DisableHAVG(Item s) { (void)s; return 0; }
Err DisableVAVG(Item s) { (void)s; return 0; }
Err EnableHAVG(Item s) { (void)s; return 0; }
Err EnableVAVG(Item s) { (void)s; return 0; }
Item SubmitVDL(VDLEntry *p, int32 n, int32 t)
{
  (void)p; (void)n; (void)t;
  return vdl_refuse ? -1 : 1;
}
Err SetVDL(Item s, Item v) { (void)s; (void)v; return setvdl_refuse ? -1 : 0; }
/* A line address that is never read, distinct per line and non-zero in
   its low 32 bits, which is what the probe keeps of it. */
#define FAKE_LINE(y) ((unsigned long)0x00100000UL + ((unsigned long)(y) * 1280UL))
void *GetPixelAddress(Item s, Coord x, Coord y) { (void)s; (void)x; return (void *)FAKE_LINE(y); }

static struct GrafFolio graf_base = { NULL };
struct GrafFolio *GrafBase = &graf_base;

static const char *cat_name[] =
  {"BOOT","SYS","CART","BUS","Z80","VDP","PSG","PAD","SAVE","PERF","GG"};
static const char *lvl_name[] = {"ERR","WARN","INFO","DBG","TRACE"};

void log_begin(int32 cat, int32 lvl)
{
  fprintf(stderr,"[%s][%s] ",cat_name[cat],lvl_name[lvl]);
}
void log_printf(const char *fmt, ...)
{
  va_list a;
  va_start(a,fmt);
  vfprintf(stderr,fmt,a);
  va_end(a);
  fputc('\n',stderr);
}

/* ---- the checks ---- */

static int failed = 0;

static void
check(int ok, const char *what)
{
  printf("  [%s] %s\n",ok ? "OK" : "FAIL",what);
  if(!ok)
    failed = 1;
}

/*
 * Walks one chain: counts its cels, and for each one marks every source
 * pixel it reads in the coverage map and every destination pixel it lands
 * on in the second map. Returns the count, or -1 when a cel breaks a rule
 * of the list -- a source off the picture or off a word, a preamble word
 * that is not the render's pair, a last flag anywhere but on the last cel.
 */
static long
walk(CCB *head, uint8 *src_cov, uint8 *dst_cov, int dst_x0, int dst_y0,
     int dst_w, int dst_h, int allow_bgnd_only_first)
{
  CCB *c;
  long n = 0;
  int first = 1;

  for(c = head; c != NULL; c = c->ccb_NextPtr)
    {
      const uint8 *src = (const uint8 *)c->ccb_SourcePtr;
      long off = (long)(src - celprobe_pic);
      uint32 w = (uint32)c->ccb_Width;
      uint32 h = (uint32)c->ccb_Height;
      uint32 r, x;
      int lx = (int)(c->ccb_XPos >> 16) - dst_x0;
      int ly = (int)(c->ccb_YPos >> 16) - dst_y0;

      n++;

      if((off < 0) || ((off & 3L) != 0L))
        return -1;
      /* The render's pair, written out here on its own so that a wrong
         formula in the probe is not its own witness: rows less one in
         PRE0 with the eight bit code, the picture's 64 words a row less
         two in the ten bit field, PDC0, pixels less one. */
      if(c->ccb_PRE0 != (((h - 1UL) << 6) | 5UL)
         || c->ccb_PRE1 != ((62UL << 16) | 0x1000UL | (w - 1UL)))
        return -2;
      if((c->ccb_Flags & CCB_NPABS) == 0UL || (c->ccb_Flags & CCB_CCBPRE) == 0UL)
        return -3;
      if(((c->ccb_Flags & CCB_LAST) != 0UL) != (c->ccb_NextPtr == NULL))
        return -4;
      if(allow_bgnd_only_first && !first && (c->ccb_Flags & CCB_LDPLUT))
        return -5;
      if(c->ccb_PLUTPtr != celprobe_plut)
        return -6;

      for(r = 0; r < h; r++)
        for(x = 0; x < w; x++)
          {
            long p = off + (long)(r * CELPROBE_PIC_W) + (long)x;
            int dx = lx + (int)x;
            int dy = ly + (int)r;

            if(p < 0 || p >= (long)CELPROBE_PIC_BYTES)
              return -7;
            src_cov[p]++;
            if(dst_cov != NULL)
              {
                if(dx < 0 || dx >= dst_w || dy < 0 || dy >= dst_h)
                  return -8;
                dst_cov[dy * dst_w + dx]++;
              }
          }
      first = 0;
    }

  return n;
}

/* Every byte of [lo, hi) covered exactly once, none of [hi, total). */
static int
once(const uint8 *cov, long lo, long hi, long total)
{
  long i;
  for(i = 0; i < total; i++)
    {
      uint8 want = (i >= lo && i < hi) ? 1 : 0;
      if(cov[i] != want)
        return 0;
    }
  return 1;
}

int
main(void)
{
  uint8 *src_cov = malloc(CELPROBE_PIC_BYTES);
  uint8 *dst_cov = malloc(CELPROBE_PIC_W * CELPROBE_PIC_H);
  CCB  *arena;
  CCB  *heads[CELPROBE_BANDS_MAX];
  uint32 bands[5] = { 1, 2, 4, 8, 16 };
  uint32 b, k, x, y;
  long n;
  char what[160];
  int ok;
  int seen[32];

  /* A page refused: the probe is absent, says so, and measure RETURNS --
     the one case in which it does -- so that the boot goes on. */
  alloc_fail_at = 1;
  ok = (celprobe_install() < 0) && (celprobe_pic == NULL) && (celprobe_list == NULL);
  alloc_calls = 0;
  alloc_fail_at = 2;
  ok = ok && (celprobe_install() < 0) && (celprobe_pic == NULL) && (celprobe_list == NULL);
  alloc_fail_at = 0;
  if(!ok)
    {
      /* Never call measure over an install that went through: it would
         hold the pattern for ever, and so would this check. */
      printf("  [FAIL] page refused: install should have failed\nfailed=1\n");
      return 1;
    }
  celprobe_measure();
  check(ok,"page refused (first or second): install fails, measure returns without measuring");

  if(src_cov == NULL || dst_cov == NULL || celprobe_install() < 0)
    {
      printf("  [FAIL] install\nfailed=1\n");
      return 1;
    }
  arena = (CCB *)celprobe_list;

  /* The timed window: a clock reading that jumps by two seconds drops its
     frame and counts it; a refused draw does the same and is said once. */
  {
    celprobe_fig_t fig;
    heads[0] = celprobe_build_list(arena,0UL);
    clock_calls_to_spike = 2;
    celprobe_time(&fig,"check",heads,1UL);
    check(fig.kept == 29 && fig.dropped == 1 && fig.min == 100 && fig.max == 100
          && fig.avg == 100,
          "clock: a window over a second is dropped and counted, the mean is of the kept frames");
    draw_fail_left = 3;
    celprobe_time(&fig,"check",heads,1UL);
    check(fig.kept == 27 && fig.dropped == 3 && fig.avg == 100,
          "draw refused: its frames are dropped and counted, the mean is of the kept frames");
    draw_fail_left = 30;
    celprobe_time(&fig,"check",heads,1UL);
    check(fig.kept == 0 && fig.dropped == 30 && fig.avg == 0 && fig.min == 0 && fig.max == 0,
          "every frame dropped: no mean, every figure zero");
  }

  /* The display list: refused by the folio, the probe says so and returns
     the code; accepted, it returns 0. */
  vdl_refuse = 1;
  ok = (celprobe_vdl() < 0);
  vdl_refuse = 0;
  setvdl_refuse = 1;
  ok = ok && (celprobe_vdl() < 0);
  setvdl_refuse = 0;
  ok = ok && (celprobe_vdl() == 0);
  check(ok,"display list: a refusal of submit or of set is returned, an acceptance is 0");

  /* The words of the list as built: one entry a line, the DMA word, the
     two line addresses, the link, the control word on the first entry
     only, the colours rotated from the split line, the background, the
     padding. Read back from the page, not from the probe's constants. */
  {
    const uint32 *w = (const uint32 *)celprobe_list;
    ok = 1;
    for(y = 0; y < 240; y++)
      {
        const uint32 *e = w + y * 40;
        uint32 dma = 0x00010000UL | 0x00008000UL | (34UL << 9) | 1UL;
        uint32 rot = (y < 96) ? 0 : 16;
        uint32 i;
        if(y < 239) dma |= 0x00200000UL | 0x00040000UL;
        if(e[0] != dma) ok = 0;
        if(e[1] != (uint32)FAKE_LINE(y)) ok = 0;
        if(e[2] != (uint32)FAKE_LINE(y > 0 ? y - 1 : 0)) ok = 0;
        if(y < 239 ? (e[3] != 160UL) : (e[3] != 0UL)) ok = 0;
        if(y == 0 ? (e[4] != (DEFAULT_DISPCTRL & ~(VDL_HINTEN | VDL_VINTEN)))
                  : (e[4] != VDL_NOP)) ok = 0;
        for(i = 0; i < 32; i++)
          {
            uint32 rgb = celprobe_colour[(i + rot) % 32];
            if(e[5 + i] != (((uint32)i << 24) | rgb)) ok = 0;
          }
        if(e[37] != MakeCLUTBackgroundEntry(0,0,0)) ok = 0;
        if(e[38] != VDL_NOP || e[39] != VDL_NOP) ok = 0;
      }
    check(ok,"display list: 240 entries of 40 words, DMA word, addresses, links, control on the first only, colours rotated from line 96");
  }

  /* The derived figures, from means chosen so that each formula shows. */
  {
    celprobe_derived_t d;
    celprobe_derive(&d,3000,3640,4280,3000,4500,9000,200,300);
    ok = d.cel_us == 10 && d.sprite_us == 10 && d.tile_us == 10 && d.call_us == 100
      && d.a_us == 3640 && d.b_us == 9000 && d.image1 == 4780 && d.image7 == 5380;
    celprobe_derive(&d,3000,2000,2000,3000,2000,0,0,0);
    ok = ok && d.cel_us == 0 && d.sprite_us == 0 && d.tile_us == 0 && d.call_us == 0
      && d.a_us == 3000 && d.image1 == 2000 && d.image7 == 2000;
    check(ok,"derive: cel, sprite, tile and call costs, A, B, image1 and image7 by the written rule, clamped at zero");
  }

  /* The pattern: each cell its number, the checker rows, the tail. */
  ok = 1;
  memset(seen,0,sizeof(seen));
  for(y = 0; y < CELPROBE_PIC_H; y++)
    for(x = 0; x < CELPROBE_PIC_W; x++)
      {
        uint8 v = celprobe_pic[y * CELPROBE_PIC_W + x];
        uint8 want;
        if(y >= CELPROBE_VIS_H)
          want = ((x ^ y) & 1) ? 31 : 0;
        else
          {
            uint8 cell = (uint8)((y / 48) * 8 + (x / 32));
            want = ((y % 48) >= 40 && ((x ^ y) & 1)) ? (uint8)(cell ^ 16) : cell;
          }
        if(v != want) ok = 0;
        if(v < 32) seen[v] = 1;
      }
  for(k = 0; k < 32; k++)
    if(!seen[k]) ok = 0;
  check(ok,"pattern: cells numbered 0-31 row-major, checker rows, tail of 0/31");
  ok = 1;
  for(k = 0; k < 32; k++)
    if(celprobe_plut[k] != (uint16)MakeRGB15(k,k,k)) ok = 0;
  check(ok,"palette: identity, bit 15 clear");

  /* The band cases: 2 cels a band, the visible rows read once in all. */
  for(b = 0; b < 5; b++)
    {
      memset(src_cov,0,CELPROBE_PIC_BYTES);
      memset(dst_cov,0,CELPROBE_PIC_W * CELPROBE_PIC_H);
      celprobe_build_bands(arena,bands[b],heads);
      n = 0;
      for(k = 0; k < bands[b] && n >= 0; k++)
        {
          long m = walk(heads[k],src_cov,dst_cov,celprobe_px,celprobe_py,
                        (int)CELPROBE_PIC_W,(int)CELPROBE_VIS_H,0);
          n = (m < 0) ? m : n + m;
        }
      ok = (n == 2L * (long)bands[b])
        && once(src_cov,0,(long)(CELPROBE_PIC_W * CELPROBE_VIS_H),(long)CELPROBE_PIC_BYTES)
        && once(dst_cov,0,(long)(CELPROBE_PIC_W * CELPROBE_VIS_H),(long)(CELPROBE_PIC_W * CELPROBE_PIC_H));
      sprintf(what,"bands=%lu: %ld cels, every visible pixel read once and landed once",
              (unsigned long)bands[b],n);
      check(ok,what);
    }

  /* The list cases: the one-band decor then the small cels asked for. */
  {
    uint32 small[3] = { 0, 64, 128 };
    for(b = 0; b < 3; b++)
      {
        CCB *c;
        long cnt = 0, small_seen = 0;
        int flags_ok = 1;
        ok = 1;
        memset(src_cov,0,CELPROBE_PIC_BYTES);
        memset(dst_cov,0,CELPROBE_PIC_W * CELPROBE_PIC_H);
        heads[0] = celprobe_build_list(arena,small[b]);
        n = walk(heads[0],src_cov,dst_cov,celprobe_px,celprobe_py,
                 (int)CELPROBE_PIC_W,(int)CELPROBE_VIS_H,1);
        /* The decor lands once everywhere; a small cel adds one; two small
           cels on one pixel would be three, which no cost figure wants. */
        for(k = 0; k < CELPROBE_PIC_W * CELPROBE_VIS_H; k++)
          if(dst_cov[k] < 1 || dst_cov[k] > 2) ok = 0;
        /* The decor is read once; the small cels read inside the visible
           lines, on top of it, and never paint their colour 0. */
        ok = ok && (n == 2L + (long)small[b]);
        for(c = heads[0]; c != NULL; c = c->ccb_NextPtr)
          {
            cnt++;
            if(cnt > 2)
              {
                small_seen++;
                if(c->ccb_Flags & (CCB_BGND | CCB_LDPLUT)) flags_ok = 0;
                if((c->ccb_Width != 8) || (c->ccb_Height != 8 && c->ccb_Height != 16)) flags_ok = 0;
                /* Every source pixel of a small cel is a non-zero number:
                   a transparent cel would cost less than a cel. */
                {
                  const uint8 *sp = (const uint8 *)c->ccb_SourcePtr;
                  int r2, x2;
                  for(r2 = 0; r2 < c->ccb_Height; r2++)
                    for(x2 = 0; x2 < c->ccb_Width; x2++)
                      if(sp[r2 * (int)CELPROBE_PIC_W + x2] == 0) flags_ok = 0;
                }
              }
            else if(cnt == 1 && !(c->ccb_Flags & CCB_LDPLUT)) flags_ok = 0;
            else if(!(c->ccb_Flags & CCB_BGND)) flags_ok = 0;
          }
        for(k = 0; k < CELPROBE_PIC_W * CELPROBE_VIS_H; k++)
          if(src_cov[k] == 0) ok = 0;
        for(k = CELPROBE_PIC_W * CELPROBE_VIS_H; k < CELPROBE_PIC_BYTES; k++)
          if(src_cov[k] != 0) ok = 0;
        ok = ok && flags_ok && (small_seen == (long)small[b]);
        sprintf(what,"list small=%lu: %ld cels, decor flags on the two windows only, small cels 8 wide inside the visible lines",
                (unsigned long)small[b],n);
        check(ok,what);
      }
  }

  /* The second variant: one cel a tile, the picture read once. */
  {
    uint32 rows[2] = { 28, 24 };
    for(b = 0; b < 2; b++)
      {
        memset(src_cov,0,CELPROBE_PIC_BYTES);
        memset(dst_cov,0,CELPROBE_PIC_W * CELPROBE_PIC_H);
        heads[0] = celprobe_build_tiles(arena,rows[b]);
        n = walk(heads[0],src_cov,dst_cov,celprobe_px,
                 (240 - (int)(rows[b] * 8)) / 2,(int)CELPROBE_PIC_W,(int)(rows[b] * 8),1);
        ok = (n == 32L * (long)rows[b])
          && once(src_cov,0,(long)(CELPROBE_PIC_W * rows[b] * 8),(long)CELPROBE_PIC_BYTES)
          && once(dst_cov,0,(long)(CELPROBE_PIC_W * rows[b] * 8),(long)(CELPROBE_PIC_W * CELPROBE_PIC_H));
        sprintf(what,"tiles rows=%lu: %ld cels of 8x8, every pixel read once and landed once",
                (unsigned long)rows[b],n);
        check(ok,what);
      }
  }

  /* The rule, on figures chosen either side of it. */
  check(strcmp(celprobe_pick(1000,900),"B") == 0
        && strcmp(celprobe_pick(1000,1000),"B") == 0
        && strcmp(celprobe_pick(1000,1001),"A") == 0,
        "decision: B when B <= A, else A");
  check(strcmp(celprobe_verdict(3000,600,100,300),"fits") == 0
        && strcmp(celprobe_verdict(7000,1200,300,600),"limit") == 0
        && strcmp(celprobe_verdict(9000,100,100,100),"exceeds:draw") == 0
        && strcmp(celprobe_verdict(4000,5000,3000,2000),"exceeds:bands") == 0
        && strcmp(celprobe_verdict(3000,100,5000,2000),"exceeds:clut") == 0
        && strcmp(celprobe_verdict(3000,100,2000,5000),"exceeds:build") == 0,
        "verdict: fits / limit / exceeds naming the heaviest post");

  printf("failed=%d\n",failed);
  return failed;
}
