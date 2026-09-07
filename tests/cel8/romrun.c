/* Host runner for the picture check: the real core on the real ROM.
 *
 * Boots src/cart.c, src/z80.c, src/sms.c and src/vdp.c as they stand, on
 * the ROM the console runs, and plays it frame by frame the way src/main.c
 * does -- 262 lines a frame, the processor's quota then the video part.
 * Every so many frames it takes the picture the cel would draw, one
 * palette index per pixel, 192 rows of 256, read as the row lies: byte 0
 * is pixel 0 on both machines, which is what the cel reads.
 *
 * With the picture drawn by the cel engine (the delivered form,
 * src/common.h SMS_DECOR_CEL 1) there is no such buffer: the picture is
 * what a LIST of cels makes the engine draw -- windows of the background,
 * sprites, priority tiles, backdrop -- and this runner reconstitutes it
 * from the blocks the render hands out, field by field, in their order
 * -- see "the picture as the list draws it" below. More lines come out
 * of that, and the script demands them: every window sound, no pixel
 * left uncovered, no more pixels read than the picture plus the
 * alignment columns allow, every small cel sound, and the two sprite
 * bits raised on the same lines as the older path raised them.
 *
 * Two modes. "compare" plays the frames and holds every row of every
 * picture against a REFERENCE: a text file of one digest per row, taken
 * once from the build that drew the picture before the format moved to a
 * byte a pixel, and kept beside this file. A row that differs is a
 * failure. "write" takes the same digests from the build it was compiled
 * against and writes them out; that is the only way a reference is ever
 * remade, after a change of picture that was meant, named in the header
 * it writes.
 *
 * The reference kept beside this file was NOT written by this runner: it
 * holds the picture of the build before the format moved, which this
 * runner refuses to compile against. That build's own runner -- this file
 * as it stood at the commit the header names -- played the same frames
 * and wrote its pictures raw, one index per byte once unpacked from six
 * bits; each row of 256 was then digested as below, and the derivation
 * was replayed from the archived tree, row for row, before the reference
 * was kept.
 *
 * The reference is digests and not pixels, so that it carries nothing of
 * the ROM's imagery; and it names the ROM it was taken from, by size and
 * by digest, so that another ROM is refused rather than compared.
 *
 * An optional directory takes one PPM per picture, the screen's colour
 * table applied and the border filled as the console shows it: the file
 * the eye reads when a figure disagrees with a screen. Two more figures
 * come out with the digests, and the script demands them: the cel's
 * palette is the identity, and the screen table is the colour memory
 * converted, both on every picture taken.
 *
 *   romrun <rom> <frames> <every> write|compare <reference> [ppm dir] [taken]
 *
 * "taken" names the build the reference is written from, one word of at
 * most 63 characters; it is carried in the header and never compared.
 *
 * Exit status: 0 every row the same, 1 a row differs, 2 the run could not
 * prove anything, 3 the reference is not this ROM's.
 *
 * Stubs follow tests/vdp-profile/bench_profile.c and the cartridge bench
 * that preceded it: the disc is the host file, the allocator is the host's,
 * the log goes to stderr, the cel is a block with the library's two words.
 */
#include "sms.h"
#include "cart.h"
#include "vdp.h"
#include "z80.h"
#include "log.h"
#include "blockfile.h"
#include "operror.h"
#include "filesystem.h"
#include "celutils.h"
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

#define LINES_PER_FRAME  262
#define TSTATES_PER_LINE 228

/* The picture is the render's, not a size of this file's own; and the row
   is read as one unpadded line of it, which vdp.h computes but this file
   pins, so that a padded row would refuse here rather than compare bytes
   of padding against pixels. */
#define PIC_W ((int)VDP_PIX_WIDTH)
#define PIC_H ((int)VDP_ACTIVE_LINES)
#define PIC_BYTES (PIC_W * PIC_H)
#if VDP_PIX_ROW_BYTES != VDP_PIX_WIDTH
#error "the row is read as an unpadded line of the picture: it no longer is one"
#endif
#if VDP_PIX_BPP != 8UL
#error "the row is read one index per byte: the depth is no longer eight"
#endif

/* ---- the disc: one file, the ROM named on the command line ---- */

static const char *rom_path = NULL;
static long rom_size = 0;

Err OpenBlockFile(char *name, BlockFilePtr bf)
{
  FILE *f;
  (void)bf;
  /* The boot tries the SMS name, then the GG name: only the first exists. */
  if(strstr(name,".gg") != NULL)
    return MAKEFERR(ER_SEVERE,ER_C_NSTND,ER_Fs_NoFile);
  f = fopen(rom_path,"rb");
  if(f == NULL)
    return MAKEFERR(ER_SEVERE,ER_C_NSTND,ER_Fs_NoFile);
  fseek(f,0,SEEK_END);
  rom_size = ftell(f);
  fclose(f);
  return 0;
}
void CloseBlockFile(BlockFilePtr bf) { (void)bf; }
int32 GetBlockFileSize(BlockFilePtr bf) { (void)bf; return (int32)rom_size; }
void *LoadFileHere(const char *fname, int32 *pfsize, void *buffer, int32 bufsize)
{
  FILE *f = fopen(rom_path,"rb");
  size_t n;
  (void)fname;
  if(f == NULL) { *pfsize = -1; return NULL; }
  n = fread(buffer,1,(size_t)bufsize,f);
  fclose(f);
  *pfsize = (int32)n;
  return buffer;
}

/* ---- the console ---- */

void *sys_alloc(const char *name, int32 size, uint32 memtype)
{
  void *p = malloc((size_t)size);
  (void)name; (void)memtype;
  if(p != NULL) memset(p,0,(size_t)size);
  return p;
}
void sys_mem_report(void) {}
void sys_mem_seal(void) {}
int32 sys_width(void) { return 320; }
int32 sys_height(void) { return 240; }
Item sys_bitmap(void) { return 0; }

/* One block per call. The two preamble words are the library's for a
   coded cel of eight bits, as vdp.c's own arbiter expects them: the row
   offset in the ten bit field. */
CCB *CreateCel(int32 width, int32 height, int32 bpp, int32 options, void *dataBuf)
{
  CCB *c = (CCB *)calloc(1,sizeof(CCB));
  uint16 *plut = (uint16 *)calloc(32,sizeof(uint16));
  (void)options;
  if(c == NULL || plut == NULL)
    {
      free(c);
      free(plut);
      return NULL;
    }
  c->ccb_Flags = CCB_SPABS | CCB_PPABS | CCB_LDSIZE | CCB_LDPRS | CCB_YOXY
               | CCB_ACW | CCB_ACCW | CCB_ACE | CCB_LAST;
  c->ccb_SourcePtr = (CelData *)dataBuf;
  c->ccb_PLUTPtr = plut;
  c->ccb_Width = width;
  c->ccb_Height = height;
  c->ccb_PRE0 = ((uint32)(height - 1) << PRE0_VCNT_SHIFT)
              | ((bpp == 8) ? PRE0_BPP_8 : PRE0_BPP_6);
  c->ccb_PRE1 = (bpp == 8)
              ? ((62UL << PRE1_WOFFSET10_SHIFT) | PRE1_TLLSB_PDC0 | 255UL)
              : ((46UL << PRE1_WOFFSET8_SHIFT) | PRE1_TLLSB_PDC0 | 255UL);
  return c;
}

/* ---- the log: boot in full, then warnings and errors only ---- */

static const char *cat_name[] =
  {"BOOT","SYS","CART","BUS","Z80","VDP","PSG","PAD","SAVE","PERF","GG"};
static const char *lvl_name[] = {"ERR","WARN","INFO","DBG","TRACE"};
static int booted = 0;
static int muted = 0;

void log_begin(int32 cat, int32 lvl)
{
  muted = (booted && lvl > LOG_LVL_WARN);
  if(muted) return;
  fprintf(stderr,"[%s][%s] ",cat_name[cat],lvl_name[lvl]);
}
void log_printf(const char *fmt, ...)
{
  va_list a;
  if(muted) return;
  va_start(a,fmt);
  vfprintf(stderr,fmt,a);
  va_end(a);
  fputc('\n',stderr);
}
void log_bind_screen(Item b, Item s) { (void)b; (void)s; }
void log_set_frame(const uint32 *f) { (void)f; }
void log_fatal(int32 cat, int32 code, const char *l1, const char *l2)
{
  fprintf(stderr,"FATAL cat=%ld code=%ld: %s / %s\n",(long)cat,(long)code,l1,l2);
  exit(2);
}

/* ---- the picture, one index per pixel, as the cel reads it ---- */

static unsigned char pic[PIC_BYTES];

#if !SMS_DECOR_CEL
/* The older path: the one buffer is the whole picture, background and
   sprites, read off the cel's source pointer at the end of the frame. */
static void take(void)
{
  CCB *c = (CCB *)vdp_cel();
  memcpy(pic,c->ccb_SourcePtr,(size_t)PIC_BYTES);
}
#define line_step() vdp_line()
#else

/* ---- the picture as the LIST draws it ----
 *
 * With the background drawn by the cel engine (src/common.h,
 * SMS_DECOR_CEL), the render composes no pixel at all: it keeps a picture
 * of the whole name table and a sheet of the sprite patterns up to date
 * and hands the frame loop, band by band, a chain of cel control blocks
 * -- windows of the picture, then the sprites as windows of the sheet,
 * then the priority tiles as windows of the picture under a second
 * palette, then the backdrop column over the lines switched off and the
 * masked left column. What the console shows is what the engine reads
 * off those blocks, so that is what is reconstituted here: every field of
 * every block is held against what a sound block of its kind is, the
 * chain against the order the kinds must come in, the sprite blocks
 * against a list this file derives on its own from the attribute table
 * and the documented admission rule, the priority blocks against the
 * priority bits of the tiles the windows show, the backdrop blocks
 * against the lines this file saw the display off on; and each block is
 * then painted the way the engine paints it -- source pixel through the
 * block's palette, a 000 entry transparent unless the background flag
 * stands (docs/3do/3do_portfolio_2.5.md:3783-3784), the horizontal and
 * vertical steps doubling pixels, cut to the picture's area of the screen
 * (the clip rectangle src/main.c sets on every screen; the folio takes
 * (0,0) as its corner, docs/3do/3do_portfolio_2.5.md:10984).
 *
 * Presented when line 191 has been counted, as src/main.c presents: the
 * writes of the blanking lines belong to the next picture, and the
 * reference encodes exactly that (line y shows the memory as it stood
 * after the processor's quota of line y).
 *
 * Counted over every frame played, not only the pictures taken: windows
 * seen and windows sound, pixels read by the windows against what a
 * frame is allowed to read -- the picture's area plus at most three
 * alignment columns per window, since a source pointer is a word address
 * -- pixels no window covered, journal entries and bands, small cels seen
 * and sound, the most small cels one presentation took. */

/* The kinds of a block, in the order a band's chain must hold them. */
#define K_WINDOW 0
#define K_SPRITE 1
#define K_PRIO   2
#define K_BACK   3

/* How many windows each pixel of the picture's area received this frame. */
static unsigned char cover[PIC_BYTES];
/* Per band: whether the window's pixel belongs to a tile with the priority
   bit, how many priority blocks and how many backdrop blocks painted it. */
static unsigned char prio_want[PIC_BYTES];
static unsigned char prio_got[PIC_BYTES];
static unsigned char back_got[PIC_BYTES];
static unsigned char band_cover[PIC_BYTES];
/* The lines this file saw the display off on when they were counted. */
static unsigned char off_line[PIC_H];

static unsigned long win_seen = 0, win_ok = 0;
static unsigned long decor_read = 0, decor_limit = 0, gaps = 0;
static unsigned long journal_total = 0, bands_total = 0;
static unsigned long cel_seen = 0, cel_ok = 0, cel_max = 0;

/* The first block found unsound, named once so that a reader knows which
   field to look at; the count is what the script judges. */
static void window_fault(const char *what, long frame, long band, long n)
{
  static int said = 0;
  if(said) return;
  said = 1;
  fprintf(stderr,"list cel unsound: %s (frame %ld band %ld cel %ld)\n",
          what,frame,band,n);
}

/* One line of the picture counted, the display bit read first: what the
   backdrop blocks of the band are held against. */
static void line_step(void)
{
  uint32 y = sms.vdp.vcount;
  if(y < (uint32)PIC_H)
    off_line[y] = (unsigned char)(((sms.vdp.reg[1] & 0x40U) == 0U) ? 1 : 0);
  vdp_line();
}

/* ---- the sprite blocks a band must hold, derived from the attribute table
 * as the video memory stands at the band, by the documented rules alone:
 * the walk stops on $D0, the screen line is the byte plus one and turns
 * negative past 224, the first eight entries touching a line are admitted
 * on it (docs/sms_gg/SMSOfficialDocs.md:393-397, 443-446), the higher
 * numbered entry is drawn first so that the lower one shows on top, the
 * horizontal position takes the shift of register 0 bit 3, the pattern
 * takes the base of register 6 bit 2 and is forced even when the sprites
 * are tall (:846-852). A sprite is drawn on the maximal runs of its
 * admitted lines inside the band, each run one block of the sheet, from
 * the pattern's place (src/vdp.h, VDP_SHEET_X and VDP_SHEET_Y); magnified,
 * a run whose first line is the second line of its doubled row, or whose
 * last line is the first line of its row, gives that line a block of one
 * row and one line, VDY at one. ---- */

typedef struct { long sx, sy, w, h, x, y, hdx, vdy; } spr_cel_t;
static spr_cel_t want[1024];
static long want_n = 0, want_i = 0;

static void want_cel(long sx, long sy, long h, long x, long y, long hdx, long vdy)
{
  if(want_n >= (long)(sizeof want / sizeof want[0]))
    {
      window_fault("the runner's sprite oracle is full, its list cut short",-3,0,0);
      return;
    }
  want[want_n].sx = sx; want[want_n].sy = sy; want[want_n].w = 8; want[want_n].h = h;
  want[want_n].x = x; want[want_n].y = y; want[want_n].hdx = hdx; want[want_n].vdy = vdy;
  want_n++;
}

static void want_run(long sx, long sy, long x, long top, long la, long lb, int zoom)
{
  long d0 = la - top, d1 = lb - top;
  long hdx = (zoom ? 2L : 1L) << 20;
  int tail = 0;
  if(!zoom)
    {
      want_cel(sx,sy + d0,lb - la,x,la,hdx,1L << 16);
      return;
    }
  if(d0 & 1) { want_cel(sx,sy + (d0 >> 1),1,x,la,hdx,1L << 16); la++; d0++; }
  if((d1 & 1) && la < lb) { tail = 1; lb--; d1--; }
  if(la < lb) want_cel(sx,sy + (d0 >> 1),(d1 - d0) >> 1,x,la,hdx,2L << 16);
  if(tail) want_cel(sx,sy + (d1 >> 1),1,x,lb,hdx,1L << 16);
}

static void oracle(long a, long b)
{
  static unsigned char adm[PIC_H][64];
  static long top[64];
  const uint8 *sat = sms.vdp.vram + (((uint32)sms.vdp.reg[5] & 0x7EUL) << 7);
  int zoom = (sms.vdp.reg[1] & 1) != 0, tall = (sms.vdp.reg[1] & 2) != 0;
  long height = (tall ? 16L : 8L) << zoom, width = 8L << zoom;
  long base = (sms.vdp.reg[6] & 4) ? 256L : 0L, shift = (sms.vdp.reg[0] & 8) ? 8L : 0L;
  long alive, i, y, n, x, p, sx, sy, ya, yb, la;

  memset(adm,0,sizeof adm);
  for(alive = 0; alive < 64 && sat[alive] != 0xD0; alive++)
    {
      top[alive] = (long)sat[alive] + 1;
      if(top[alive] > 224) top[alive] -= 256;
    }
  for(y = 0; y < PIC_H; y++)
    {
      n = 0;
      for(i = 0; i < alive; i++)
        if(y >= top[i] && y < top[i] + height)
          {
            if(n < 8) adm[y][i] = 1;
            n++;
          }
    }

  want_n = 0;
  want_i = 0;
  for(i = alive - 1; i >= 0; i--)
    {
      x = (long)sat[128 + 2 * i] - shift;
      if(x + width <= 0) continue;
      p = (long)sat[129 + 2 * i] + base;
      if(tall) p &= ~1L;
      sx = ((p >> 1) & 31) * 8;
      sy = (p >> 6) * 16 + (p & 1) * 8;
      ya = (a > top[i]) ? a : top[i];
      if(ya < 0) ya = 0;
      yb = (b < top[i] + height) ? b : top[i] + height;
      y = ya;
      while(y < yb)
        {
          while(y < yb && !adm[y][i]) y++;
          if(y >= yb) break;
          la = y;
          while(y < yb && adm[y][i]) y++;
          want_run(sx,sy,x,top[i],la,y,zoom);
        }
    }
}

/* The number a palette entry paints, or -1 for 000: entry n of the
   identity holds n on each of its three five bit components, and so does
   every entry of the priority palette but the sixteenth. */
static long plut_number(uint16 e)
{
  long v = e & 31;
  if(e == 0) return -1;
  if(((e >> 5) & 31) != v || ((e >> 10) & 31) != v) return -2;
  return v;
}

/* One block of a band's chain: held against what the engine will read off
   it, then painted as the engine paints it. Returns the kind found, or -1
   when the block could not be placed at all; *ok is cleared on any fault.
   The palette the engine holds is tracked through *loaded: a block loads
   its own with CCB_LDPLUT, or must point at the one already loaded. */
static int cel(const CCB *c, long frame, long band, long n,
               const void **loaded, int *ok)
{
  const unsigned char *src = (const unsigned char *)c->ccb_SourcePtr;
  const unsigned char *buf;
  const uint16 *plut = (const uint16 *)c->ccb_PLUTPtr;
  const uint8 *nt = sms.vdp.vram + (((uint32)sms.vdp.reg[2] & 0x0EUL) << 10);
  long w = (long)c->ccb_Width, h = (long)c->ccb_Height;
  long pitch, bytes, off, x, y, hs, vs, r, i, dx, dy;
  int kind, bgnd = (c->ccb_Flags & CCB_BGND) != 0;
  const char *why = NULL;

  /* The column lies in the tail of the picture's page: tested first. */
  if(src >= sms.vdp.column && src < sms.vdp.column + VDP_COLUMN_BYTES)
    { kind = K_BACK; buf = sms.vdp.column; pitch = (long)VDP_COLUMN_W; bytes = VDP_COLUMN_BYTES; }
  else if(src >= sms.vdp.decor && src < sms.vdp.decor + VDP_DECOR_BYTES)
    { kind = bgnd ? K_WINDOW : K_PRIO; buf = sms.vdp.decor; pitch = PIC_W; bytes = VDP_DECOR_BYTES; }
  else if(src >= sms.vdp.sheet && src < sms.vdp.sheet + VDP_SHEET_BYTES)
    { kind = K_SPRITE; buf = sms.vdp.sheet; pitch = (long)VDP_SHEET_W; bytes = VDP_SHEET_BYTES; }
  else
    {
      window_fault("source in no buffer of the list",frame,band,n);
      *ok = 0;
      return -1;
    }

  if(c->ccb_Flags & CCB_LDPLUT) *loaded = c->ccb_PLUTPtr;
  off = (long)(src - buf);
  x = (long)(c->ccb_XPos / 65536L);
  y = (long)(c->ccb_YPos / 65536L);
  hs = (long)(c->ccb_HDX >> 20);
  vs = (long)(c->ccb_VDY >> 16);

  if((c->ccb_Flags & CCB_CCBPRE) == 0) why = "no CCB_CCBPRE, the preamble is not read";
  else if(c->ccb_PLUTPtr != *loaded) why = "the block's palette is not the one loaded";
  else if((c->ccb_XPos % 65536L) != 0 || (c->ccb_YPos % 65536L) != 0) why = "position not on a pixel";
  else if(c->ccb_HDY != 0 || c->ccb_VDX != 0 || c->ccb_HDDX != 0 || c->ccb_HDDY != 0) why = "a skew or perspective step is not zero";
  else if(c->ccb_PIXC != 0x1F001F00UL) why = "PIXC";
  else if(w < 1 || h < 1) why = "empty";
  else if(c->ccb_PRE0 != (((uint32)(h - 1) << PRE0_VCNT_SHIFT) | PRE0_BPP_8)) why = "PRE0";
  else if(c->ccb_PRE1 != (((uint32)(pitch / 4 - 2) << PRE1_WOFFSET10_SHIFT) | PRE1_TLLSB_PDC0 | (uint32)(w - 1))) why = "PRE1";
  else if((off % 4) != 0) why = "source not a word address";
  else if((off % pitch) + w > pitch || off / pitch + h > bytes / pitch) why = "source rectangle runs past its buffer";
  else if(kind == K_WINDOW)
    {
      if(plut != sms.vdp.plut) why = "window not under the identity palette";
      else if(n == 0 && (c->ccb_Flags & CCB_LDPLUT) == 0) why = "first block of the band without CCB_LDPLUT";
      else if(w > PIC_W + 3 || h > (long)VDP_DECOR_LINES) why = "window size (at most the width plus three alignment columns)";
      else if(hs != 1 || vs != 1) why = "window not a 1:1 mapping";
      else if(x < -3 || x + w > PIC_W || y < 0 || y + h > PIC_H) why = "window outside the picture's area";
    }
  else if(kind == K_SPRITE)
    {
      int zoom = (sms.vdp.reg[1] & 1) != 0;
      if(bgnd) why = "CCB_BGND on a sprite";
      else if(plut != sms.vdp.plut) why = "sprite not under the identity palette";
      else if(w != 8) why = "sprite not eight pixels wide";
      else if(hs != (zoom ? 2 : 1)) why = zoom ? "magnified sprite without HDX at two" : "sprite with HDX not one";
      else if(vs != 1 && !(zoom && vs == 2)) why = "sprite VDY";
      else if(want_i >= want_n) why = "more sprite blocks than the attribute table admits";
      else
        {
          const spr_cel_t *e = &want[want_i++];
          if(off % pitch != e->sx) why = "sprite source column (pattern place, or the order of the sprites)";
          else if(off / pitch != e->sy) why = "sprite source row (rows cut to the admitted lines)";
          else if(h != e->h) why = "sprite height (rows cut to the admitted lines)";
          else if(x != e->x) why = "sprite x";
          else if(y != e->y) why = "sprite y";
          else if((c->ccb_HDX != e->hdx) || (c->ccb_VDY != e->vdy)) why = "sprite steps (magnification)";
        }
    }
  else if(kind == K_PRIO)
    {
      if(plut != sms.vdp.plut_prio) why = "priority run not under the priority palette";
      else if(hs != 1 || vs != 1) why = "priority run not a 1:1 mapping";
      else if(x < -3 || x + w > PIC_W || y < 0 || y + h > PIC_H) why = "priority run outside the picture's area";
    }
  else
    {
      if(!bgnd) why = "backdrop block without CCB_BGND";
      else if(plut != sms.vdp.plut) why = "backdrop block not under the identity palette";
      else if(w != (long)VDP_COLUMN_W || x != 0) why = "backdrop block not the column at x 0";
      else if(vs != 1 || (hs != 1 && hs != (long)(PIC_W / VDP_COLUMN_W))) why = "backdrop steps";
      else if(y < 0 || y + h > PIC_H) why = "backdrop block outside the picture's area";
      else if(off / pitch != y) why = "backdrop block not read from its own lines of the column";
    }
  if(why != NULL)
    {
      window_fault(why,frame,band,n);
      *ok = 0;
    }
  if((off % 4) != 0 || (off % pitch) + w > pitch || off / pitch + h > bytes / pitch)
    return kind;

  /* The paint, cut to the area: what lands at a column below zero is the
     alignment read, and the clip rectangle erases it on the console. */
  for(r = 0; r < h; r++)
    for(i = 0; i < w; i++)
      {
        unsigned s = src[r * pitch + i];
        long v = (s < 32) ? plut_number(plut[s]) : -2;
        int paint = 1;
        if(v == -2)
          {
            window_fault("pixel outside the palette, or an entry that is not a number",frame,band,n);
            *ok = 0;
            paint = 0;
          }
        else if(v == -1)
          {
            if(bgnd) v = 0;
            else paint = 0;
          }
        for(dy = 0; dy < vs; dy++)
          for(dx = 0; dx < hs; dx++)
            {
              long sx = x + i * hs + dx, sy = y + r * vs + dy, k;
              if(sx < 0 || sx >= PIC_W || sy < 0 || sy >= PIC_H) continue;
              k = sy * PIC_W + sx;
              if(paint) pic[k] = (unsigned char)v;
              if(kind == K_WINDOW)
                {
                  long srow = off / pitch + r, scol = off % pitch + i;
                  long t = (srow / 8) * 32 + scol / 8;
                  cover[k]++;
                  band_cover[k]++;
                  prio_want[k] = (unsigned char)((read16_le(nt + t * 2) & 0x1000U) ? 1 : 0);
                }
              else if(kind == K_PRIO) prio_got[k]++;
              else if(kind == K_BACK) back_got[k]++;
            }
      }
  if(kind == K_WINDOW)
    {
      decor_read += (unsigned long)(w * h);
      decor_limit += (unsigned long)(3 * h);
    }
  return kind;
}

/* The coverage of one band held once its chain is painted: every pixel a
   window showed from a priority tile is painted by exactly one priority
   run and no other pixel by any; every line the display was off on is
   painted whole by the backdrop and no other line is, except its first
   eight columns when register 0 bit 5 masks them. */
static void band_check(long frame, long band, long a, long b)
{
  long y, x, k;
  int masked = (sms.vdp.reg[0] & 0x20) != 0;
  for(y = a; y < b; y++)
    for(x = 0; x < PIC_W; x++)
      {
        k = y * PIC_W + x;
        if(band_cover[k] == 0) continue;
        if(prio_want[k] != (prio_got[k] != 0) || prio_got[k] > 1)
          {
            window_fault("priority coverage: a priority tile's pixel not painted once by a priority run, or another pixel painted by one",frame,band,x + y * 1000L);
            return;
          }
        if(off_line[y])
          {
            if(back_got[k] == 0 || (x >= 8 && back_got[k] > 1))
              {
                window_fault("a line with the display off not painted once by the backdrop",frame,band,y);
                return;
              }
          }
        else if(back_got[k] != ((x < 8 && masked) ? 1 : 0))
          {
            window_fault(masked ? "the masked left column not painted once by the backdrop, or a pixel beyond it painted"
                                : "a backdrop block on a line with the display on",frame,band,x + y * 1000L);
            return;
          }
      }
}

/* The bands of one presentation, between vdp_list_begin and vdp_list_end:
   each band's chain asked for, held, painted into pic and checked. */
static void compose_bands(long frame, int32 bands)
{
  int32 k;
  const CCB *c;
  const void *loaded;
  long n, a, b, small = 0;
  int kind, prev, ok;

  /* What one presentation may read through its windows: the picture's
     area, plus the alignment columns each window adds below. */
  decor_limit += (unsigned long)PIC_BYTES;

  for(k = 0; k < bands; k++)
    {
      a = (long)sms.vdp.band_line[k];
      b = (k + 1 < bands) ? (long)sms.vdp.band_line[k + 1] : PIC_H;
      c = (const CCB *)vdp_list_band(k);
      memset(prio_want,0,sizeof prio_want);
      memset(prio_got,0,sizeof prio_got);
      memset(back_got,0,sizeof back_got);
      memset(band_cover,0,sizeof band_cover);
      oracle(a,b);
      if(c == NULL)
        {
          window_fault("band with no block",frame,(long)k,0);
          win_seen++;
          continue;
        }
      loaded = NULL;
      prev = K_WINDOW;
      for(n = 0;; n++)
        {
          if(n >= (long)(VDP_LIST_WINDOWS + VDP_LIST_CELS))
            {
              window_fault("chain longer than the two arenas",frame,(long)k,n);
              break;
            }
          ok = 1;
          kind = cel(c,frame,(long)k,n,&loaded,&ok);
          if(kind >= 0 && kind < prev)
            {
              window_fault("chain order: windows, then sprites, then priority runs, then backdrop",frame,(long)k,n);
              ok = 0;
            }
          if(kind > prev) prev = kind;
          if(kind == K_WINDOW || kind < 0) { win_seen++; if(ok) win_ok++; }
          else { cel_seen++; small++; if(ok) cel_ok++; }
          if(c->ccb_Flags & CCB_LAST) break;
          c = c->ccb_NextPtr;
          if(c == NULL)
            {
              window_fault("chain ends without CCB_LAST",frame,(long)k,n);
              win_seen++;
              break;
            }
        }
      if(want_i != want_n)
        window_fault("fewer sprite blocks than the attribute table admits",frame,(long)k,n);
      band_check(frame,(long)k,a,b);
    }
  if((unsigned long)small > cel_max) cel_max = (unsigned long)small;
}

static void present(long frame)
{
  int32 bands;
  long i;

  /* A pixel no block paints keeps this value, which no index can be:
     a gap shows in the digests as well as in its own count. */
  memset(pic,0xFF,sizeof pic);
  memset(cover,0,sizeof cover);

  journal_total += sms.vdp.journal_count;
  bands = vdp_list_begin();
  bands_total += (unsigned long)bands;
  compose_bands(frame,bands);
  vdp_list_end();

  for(i = 0; i < PIC_BYTES; i++)
    if(cover[i] == 0) gaps++;
}

/* ---- synthetic scenes: the cases of the journal and the bands the ROM
 * does not reach, driven straight into the video part once the ROM has
 * played (its state no longer matters). Each scene sets the line counter
 * and writes the video memory through the same port macro the processor
 * uses, then holds what the journal, the bands, the counters or the
 * windows must show; each ends with everything applied and the journal
 * empty. Expectations held are counted against expectations made, and the
 * first that failed is named. ---- */

static unsigned long scene_want = 0, scene_ok = 0;

static void expect(int held, const char *what)
{
  scene_want++;
  if(held) scene_ok++;
  else fprintf(stderr,"decor scene failed: %s\n",what);
}

/* One byte into the video memory through the port, at the address given:
   any code but the colour memory's writes the video memory. */
static void vram_write(uint32 addr, uint32 v)
{
  sms.vdp.code = 0;
  sms.vdp.addr = addr & VDP_VRAM_MASK;
  VDP_IO_DATA_WRITE(v);
}

/* One register, through the control port as the processor sets it: the
   value, then the register number under code 2 (docs/sms_gg/SMSOfficialDocs.md
   register write; vdp_io_ctrl_write). */
static void reg_write(uint32 n, uint32 v)
{
  vdp_io_ctrl_write((uint8)v);
  vdp_io_ctrl_write((uint8)(0x80 | (n & 0xF)));
}

static uint32 nt_base(void) { return ((uint32)sms.vdp.reg[2] & 0x0EUL) << 10; }
static uint32 nt_word(uint32 t) { return read16_le(sms.vdp.vram + nt_base() + t * 2); }
static void nt_write(uint32 t, uint32 word)
{
  vram_write(nt_base() + t * 2,word & 0xFF);
  vram_write(nt_base() + t * 2 + 1,word >> 8);
}

/* One pixel of one tile as a name table word draws it, from the planes
   alone (docs/sms_gg/SMSOfficialDocs.md:321-334, 474-481): the render's
   own picture owes this nothing. */
static unsigned tile_pixel(uint32 word, int r, int c)
{
  const uint8 *row;
  unsigned idx = 0;
  int k;
  if(word & 0x400) r = 7 - r;
  if(word & 0x200) c = 7 - c;
  row = sms.vdp.vram + (word & 0x1FF) * 32 + r * 4;
  for(k = 0; k < 4; k++) idx |= ((row[k] >> (7 - c)) & 1) << k;
  if(word & 0x800) idx += 16;
  return idx;
}

/* A pattern the picture neither shows nor watches, from the one given on:
   a sprite pattern, as far as the background is concerned. 0xFFFFFFFF
   when there is none. */
static uint32 free_pattern(uint32 from)
{
  uint32 p;
  for(p = from; p < 512; p++)
    if(sms.vdp.refs[p] == 0 && sms.vdp.watch[p] == 0) return p;
  return 0xFFFFFFFFUL;
}

/* Everything applied, the journal emptied, the counter in the blanking. */
static void flush(void)
{
  int32 bands, k;
  sms.vdp.vcount = 200;
  bands = vdp_list_begin();
  for(k = 0; k < bands; k++) vdp_list_band(k);
  vdp_list_end();
}

/* The offset of a window's source in the picture, its x on the screen. */
static long win_off(const CCB *c) { return (long)((const unsigned char *)c->ccb_SourcePtr - sms.vdp.decor); }
static long win_x(const CCB *c) { return (long)(c->ccb_XPos / 65536L); }

/* ---- the sprite scenes ---- */

/* The n-th block of one kind in a band's chain, or NULL. The kind is read
   off the source as cel() reads it. */
static const CCB *nth_kind(const CCB *c, int kind, long n)
{
  for(; c != NULL; c = (c->ccb_Flags & CCB_LAST) ? NULL : c->ccb_NextPtr)
    {
      const unsigned char *src = (const unsigned char *)c->ccb_SourcePtr;
      int k;
      if(src >= sms.vdp.sheet && src < sms.vdp.sheet + VDP_SHEET_BYTES) k = K_SPRITE;
      else if(src >= sms.vdp.column && src < sms.vdp.column + VDP_COLUMN_BYTES) k = K_BACK;
      else if(c->ccb_Flags & CCB_BGND) k = K_WINDOW;
      else k = K_PRIO;
      if(k != kind) continue;
      if(n == 0) return c;
      n--;
    }
  return NULL;
}

static long cel_off(const CCB *c, const unsigned char *buf) { return (long)((const unsigned char *)c->ccb_SourcePtr - buf); }
static long cel_x(const CCB *c) { return (long)(c->ccb_XPos / 65536L); }
static long cel_y(const CCB *c) { return (long)(c->ccb_YPos / 65536L); }

/* One entry of the attribute table at $3F00, through the port. */
static void sat_write(uint32 i, uint32 y, uint32 x, uint32 pattern)
{
  vram_write(0x3F00 + i,y);
  vram_write(0x3F80 + 2 * i,x);
  vram_write(0x3F81 + 2 * i,pattern);
}

/* The picture played from line 0 to line 191 as the frame loop plays it,
   one write landing in the quota of the line given (none when the line
   is past the picture), then presented into pic. Returns the band count. */
static uint32 play_addr = 0, play_value = 0;
static long play_line = 999, play_reg = -1;

static int32 play_and_present(void)
{
  int32 bands;
  long y;
  sms.vdp.vcount = 0;
  for(y = 0; y < PIC_H; y++)
    {
      if(y == play_line)
        {
          if(play_reg >= 0) reg_write((uint32)play_reg,play_value);
          else vram_write(play_addr,play_value);
        }
      line_step();
    }
  bands = vdp_list_begin();
  memset(pic,0xFF,sizeof pic);
  compose_bands(-2,bands);
  play_line = 999;
  play_reg = -1;
  return bands;
}

/* The n-th priority run of a chain standing at (x, y), w by h, or NULL. */
static const CCB *find_prio(const CCB *c, long x, long y, long w, long h)
{
  long n;
  const CCB *p;
  for(n = 0; (p = nth_kind(c,K_PRIO,n)) != NULL; n++)
    if(cel_x(p) == x && cel_y(p) == y && p->ccb_Width == w && p->ccb_Height == h)
      return p;
  return NULL;
}

static void sprite_scenes(uint32 pa)
{
  uint32 ps, pf, pe, q, i, reg1, reg7, before_ovf, before_col, before, t, word;
  uint32 saved[4], saved_w[8];
  int32 bands;
  const CCB *c;
  const CCB *s;
  const CCB *s2;
  long sx, sy, n;

  /* The table, the pattern base, the size, the display: known. */
  sms.vdp.latch = 0;
  sms.vdp.vcount = 200;
  reg_write(5,0xFF);
  reg_write(6,0xFB);
  reg1 = (uint32)sms.vdp.reg[1];
  reg_write(1,(reg1 | 0x40UL) & ~0x03UL);
  reg_write(0,(uint32)sms.vdp.reg[0] & ~0x28UL);
  for(i = 0; i < 64; i++) sat_write(i,0xD0,0,0);
  /* No priority tile anywhere: a sprite under one would be covered,
     rightly, and the scenes read the sprite. The one priority scene
     below raises the bit on its own tile. */
  for(t = 0; t < VDP_NT_TILES; t++)
    if(nt_word(t) & 0x1000UL) nt_write(t,nt_word(t) & ~0x1000UL);
  flush();

  /* A sprite pattern of this file's own: index 2 down column 0 and
     nothing else, so that a sprite shows 18 at its first column. */
  ps = free_pattern(0);
  pf = (ps == 0xFFFFFFFFUL) ? ps : free_pattern(ps + 1);
  expect(ps != 0xFFFFFFFFUL && pf != 0xFFFFFFFFUL && pf < 256,"two free patterns in the first eight kilobytes exist for the sprites");
  if(pf == 0xFFFFFFFFUL || pf >= 256) return;
  sms.vdp.vcount = 200;
  for(i = 0; i < 32; i++)
    vram_write(ps * 32 + i,((i & 3) == 1) ? 0x80 : 0);
  sx = ((ps >> 1) & 31) * 8;
  sy = (ps >> 6) * 16 + (ps & 1) * 8;
  flush();

  /* Y past 224: the byte 0xF8 puts the top at -7, so line 0 shows row 7
     and nothing else of the sprite is drawn -- one block of one row from
     the pattern's eighth row, at (100, 0). */
  sat_write(0,0xF8,100,ps);
  sat_write(1,0xD0,0,0);
  bands = play_and_present();
  c = (const CCB *)vdp_list_band(0);
  s = nth_kind(c,K_SPRITE,0);
  expect(bands == 1 && s != NULL,"sprite with y past 224: a sprite block in the band");
  if(s != NULL)
    expect(cel_off(s,sms.vdp.sheet) == (sy + 7) * PIC_W + sx && s->ccb_Height == 1
           && cel_x(s) == 100 && cel_y(s) == 0 && nth_kind(c,K_SPRITE,1) == NULL,
           "sprite with y past 224: one block of row 7 of the pattern, one line, at (100, 0)");
  expect(pic[0 * PIC_W + 100] == 18,"sprite with y past 224: line 0 shows its colour at column 100");
  vdp_list_end();
  flush();

  /* A ninth sprite on a range: entries 0 to 7 on lines 100 to 107, entry
     8 on lines 96 to 103 -- admitted on 96 to 99 alone, refused on 100
     to 103 where it is the ninth, the overflow raised on those four
     lines. Drawn first, being the highest numbered: one block of rows 0
     to 3 at line 96. No two sprites share a column: no collision. */
  for(i = 0; i < 8; i++) sat_write(i,99,8 + 16 * i,ps);
  sat_write(8,95,200,ps);
  sat_write(9,0xD0,0,0);
  before_ovf = sms.vdp.cnt_spr_ovf;
  before_col = sms.vdp.cnt_spr_col;
  sms.vdp.spr_overflow = 0;
  bands = play_and_present();
  expect(sms.vdp.cnt_spr_ovf == before_ovf + 4 && sms.vdp.spr_overflow == 1,
         "ninth sprite: the overflow is raised on the four lines it is the ninth on");
  expect(sms.vdp.cnt_spr_col == before_col,"ninth sprite: no column shared, no collision");
  c = (const CCB *)vdp_list_band(0);
  s = nth_kind(c,K_SPRITE,0);
  expect(s != NULL && cel_off(s,sms.vdp.sheet) == sy * PIC_W + sx && s->ccb_Height == 4
         && cel_x(s) == 200 && cel_y(s) == 96,
         "ninth sprite: drawn first, rows 0 to 3 at line 96, its refused lines cut off");
  expect(nth_kind(c,K_SPRITE,8) != NULL && nth_kind(c,K_SPRITE,9) == NULL,
         "ninth sprite: nine sprite blocks in the band");
  expect(pic[96 * PIC_W + 200] == 18 && pic[99 * PIC_W + 200] == 18 && pic[100 * PIC_W + 200] != 18
         && pic[100 * PIC_W + 8] == 18,
         "ninth sprite: shown on lines 96 to 99, not on line 100, where the first sprite shows");
  vdp_list_end();
  flush();

  /* Two sprites sharing a column: entries 0 and 1 both at column 50 on
     lines 100 to 107, index 2 at their first column -- eight lines of
     collision, and the first of the table wins the pixel. */
  sat_write(0,99,50,ps);
  sat_write(1,99,50,ps);
  sat_write(2,0xD0,0,0);
  before_col = sms.vdp.cnt_spr_col;
  sms.vdp.spr_collision = 0;
  bands = play_and_present();
  expect(sms.vdp.cnt_spr_col == before_col + 8 && sms.vdp.spr_collision == 1,
         "two sprites on one column: the collision is raised on their eight lines");
  vdp_list_end();
  flush();

  /* Magnified (register 1 bit 0), with a cut on an odd line: entries 0
     to 7 from -3 cover lines 0 to 12; entry 8 from line 10 covers 10 to
     25 and is admitted from 13 on. Line 13 is the second line of its
     doubled row 1: one block of one row and one line, VDY at one; then
     rows 2 to 7 doubled from line 14, six rows on twelve lines. Both
     with HDX at two. */
  reg_write(1,(uint32)sms.vdp.reg[1] | 0x01UL);
  for(i = 0; i < 8; i++) sat_write(i,0xFC,8 + 16 * i,ps);
  sat_write(8,9,200,ps);
  sat_write(9,0xD0,0,0);
  before_ovf = sms.vdp.cnt_spr_ovf;
  bands = play_and_present();
  expect(sms.vdp.cnt_spr_ovf == before_ovf + 3,"magnified ninth sprite: the overflow is raised on lines 10 to 12");
  c = (const CCB *)vdp_list_band(0);
  s = nth_kind(c,K_SPRITE,0);
  s2 = nth_kind(c,K_SPRITE,1);
  expect(s != NULL && cel_off(s,sms.vdp.sheet) == (sy + 1) * PIC_W + sx && s->ccb_Height == 1
         && cel_x(s) == 200 && cel_y(s) == 13 && s->ccb_HDX == (2L << 20) && s->ccb_VDY == (1L << 16),
         "magnified sprite cut on an odd line: one block of row 1 on line 13, VDY at one, HDX at two");
  expect(s2 != NULL && cel_off(s2,sms.vdp.sheet) == (sy + 2) * PIC_W + sx && s2->ccb_Height == 6
         && cel_x(s2) == 200 && cel_y(s2) == 14 && s2->ccb_HDX == (2L << 20) && s2->ccb_VDY == (2L << 16),
         "magnified sprite: then rows 2 to 7 doubled from line 14");
  expect(pic[13 * PIC_W + 200] == 18 && pic[13 * PIC_W + 201] == 18 && pic[25 * PIC_W + 201] == 18
         && pic[12 * PIC_W + 200] != 18 && pic[13 * PIC_W + 202] != 18,
         "magnified sprite: two pixels wide from line 13 to line 25, nothing on line 12");
  vdp_list_end();

  /* A magnified sprite cut by the bottom of the picture on the first
     line of a doubled row: entry 0 from line 177 covers 177 to 192, line
     191 being the first line of row 7. Rows 0 to 6 doubled from line
     177, then one block of row 7 on line 191 alone, VDY at one. */
  sat_write(0,176,100,ps);
  sat_write(1,0xD0,0,0);
  bands = play_and_present();
  c = (const CCB *)vdp_list_band(0);
  s = nth_kind(c,K_SPRITE,0);
  s2 = nth_kind(c,K_SPRITE,1);
  expect(s != NULL && cel_off(s,sms.vdp.sheet) == sy * PIC_W + sx && s->ccb_Height == 7
         && cel_y(s) == 177 && s->ccb_VDY == (2L << 16),
         "magnified sprite at the bottom: rows 0 to 6 doubled from line 177");
  expect(s2 != NULL && cel_off(s2,sms.vdp.sheet) == (sy + 7) * PIC_W + sx && s2->ccb_Height == 1
         && cel_y(s2) == 191 && s2->ccb_VDY == (1L << 16) && nth_kind(c,K_SPRITE,2) == NULL,
         "magnified sprite at the bottom: one block of row 7 on line 191, VDY at one");
  expect(pic[191 * PIC_W + 100] == 18 && pic[191 * PIC_W + 101] == 18 && pic[190 * PIC_W + 100] == 18,
         "magnified sprite at the bottom: shown on lines 190 and 191");
  vdp_list_end();
  reg_write(1,(uint32)sms.vdp.reg[1] & ~0x01UL);
  flush();

  /* A priority tile of the second bank over a sprite: tile (10, 10) --
     screen (80, 80) -- shows probe A with bit 12 and bit 11, so column
     0 draws 17 and the rest 16; the sprite at (81, 80) puts its 18 at
     tile column 1. The run of priority follows the sprites under the
     priority palette; 16 passes, the sprite shows through it, 17
     covers. */
  t = 10 * 32 + 10;
  word = nt_word(t);
  sms.vdp.vcount = 200;
  nt_write(t,pa | 0x1000UL | 0x800UL);
  sat_write(0,79,81,ps);
  sat_write(1,0xD0,0,0);
  bands = play_and_present();
  c = (const CCB *)vdp_list_band(0);
  s = nth_kind(c,K_PRIO,0);
  expect(s != NULL && s->ccb_PLUTPtr == (void *)sms.vdp.plut_prio && (s->ccb_Flags & CCB_BGND) == 0
         && nth_kind(c,K_SPRITE,0) != NULL,
         "priority tile: a run under the priority palette, without the background flag, beside a sprite block");
  expect(pic[80 * PIC_W + 80] == 17 && pic[80 * PIC_W + 81] == 18 && pic[80 * PIC_W + 82] == 16,
         "priority tile of bank 1: colour 1 covers, colour 0 (entry 16) lets the sprite through");
  vdp_list_end();
  sms.vdp.vcount = 200;
  nt_write(t,word);
  flush();

  /* The sprite's pattern rewritten on line 70 while it is shown on lines
     66 to 73: the table names it, so the write is journaled and cuts a
     band; the sprite is two blocks -- rows 0 to 3 from the sheet as it
     was, rows 4 to 7 from the sheet as it is, the rewritten row 4 showing
     index 3 where row 0 shows 2. */
  sat_write(0,65,50,ps);
  sat_write(1,0xD0,0,0);
  flush();
  play_line = 70;
  play_addr = ps * 32 + 4 * 4;
  play_value = 0x80;
  bands = play_and_present();
  expect(bands == 2 && sms.vdp.band_line[1] == 70,"sprite pattern rewritten on line 70: journaled, a band from line 70");
  /* The bands asked for again from the head: the journal undone, then
     replayed band by band, so that band 0 shows the memory of line 0. */
  vdp_list_begin();
  c = (const CCB *)vdp_list_band(0);
  s = nth_kind(c,K_SPRITE,0);
  expect(s != NULL && cel_off(s,sms.vdp.sheet) == sy * PIC_W + sx && s->ccb_Height == 4 && cel_y(s) == 66,
         "sprite pattern rewritten on line 70: band 0 draws rows 0 to 3");
  c = (const CCB *)vdp_list_band(1);
  s = nth_kind(c,K_SPRITE,0);
  expect(s != NULL && cel_off(s,sms.vdp.sheet) == (sy + 4) * PIC_W + sx && s->ccb_Height == 4 && cel_y(s) == 70,
         "sprite pattern rewritten on line 70: band 1 draws rows 4 to 7");
  expect(pic[66 * PIC_W + 50] == 18 && pic[70 * PIC_W + 50] == 19,
         "sprite pattern rewritten on line 70: the old row above the line, the new row on it");
  vdp_list_end();
  sms.vdp.vcount = 200;
  vram_write(ps * 32 + 4 * 4,0);
  flush();

  /* The attribute table written on line 50: the sprite's y byte moved
     from 39 (lines 40 to 47) to 54 (lines 55 to 62). Journaled, two
     bands; the per-line table is rebuilt at line 50 for the lines after
     it and keeps what lines 40 to 47 admitted: band 0 draws the sprite
     at line 40, band 1 at line 55. */
  sat_write(0,39,60,ps);
  sat_write(1,0xD0,0,0);
  flush();
  play_line = 50;
  play_addr = 0x3F00;
  play_value = 54;
  bands = play_and_present();
  expect(bands == 2 && sms.vdp.band_line[1] == 50,"sprite table written on line 50: journaled, a band from line 50");
  expect(sms.vdp.spr_dirty == 0 && (sms.vdp.spr_adm[55][0] & 1UL) != 0UL && (sms.vdp.spr_adm[40][0] & 1UL) != 0UL
         && (sms.vdp.spr_adm[50][0] & 1UL) == 0UL,
         "sprite table written on line 50: the per-line table rebuilt from line 50, lines before it kept");
  vdp_list_begin();
  c = (const CCB *)vdp_list_band(0);
  s = nth_kind(c,K_SPRITE,0);
  expect(s != NULL && cel_y(s) == 40 && s->ccb_Height == 8,"sprite table written on line 50: band 0 draws the sprite at line 40");
  c = (const CCB *)vdp_list_band(1);
  s = nth_kind(c,K_SPRITE,0);
  expect(s != NULL && cel_y(s) == 55 && s->ccb_Height == 8,"sprite table written on line 50: band 1 draws the sprite at line 55");
  expect(pic[40 * PIC_W + 60] == 18 && pic[47 * PIC_W + 60] == 18 && pic[55 * PIC_W + 60] == 18
         && pic[50 * PIC_W + 60] != 18,
         "sprite table written on line 50: shown on lines 40 to 47 and 55 to 62");
  vdp_list_end();
  flush();

  /* The display off from line 100 to 149 with the left column masked:
     the backdrop over those lines whole, the column over every other
     line's first eight pixels, the sprite at column 4 covered; and nine
     sprites on lines 100 to 107, two of them on one column, raise no
     flag at all -- a line with the display off reads no sprite. */
  sat_write(0,19,4,ps);
  for(i = 1; i < 10; i++) sat_write(i,99,(i < 9) ? 8 + 16 * i : 24,ps);
  sat_write(10,0xD0,0,0);
  reg_write(0,(uint32)sms.vdp.reg[0] | 0x20UL);
  flush();
  before_ovf = sms.vdp.cnt_spr_ovf;
  before_col = sms.vdp.cnt_spr_col;
  sms.vdp.vcount = 0;
  for(i = 0; i < (uint32)PIC_H; i++)
    {
      if(i == 100) reg_write(1,(uint32)sms.vdp.reg[1] & ~0x40UL);
      if(i == 150) reg_write(1,(uint32)sms.vdp.reg[1] | 0x40UL);
      line_step();
    }
  bands = vdp_list_begin();
  memset(pic,0xFF,sizeof pic);
  compose_bands(-2,bands);
  c = (const CCB *)vdp_list_band(0);
  s = nth_kind(c,K_BACK,0);
  s2 = nth_kind(c,K_BACK,1);
  expect(s != NULL && cel_y(s) == 100 && s->ccb_Height == 50 && s->ccb_HDX == (32L << 20) && (s->ccb_Flags & CCB_BGND) != 0,
         "display off on lines 100 to 149: one backdrop block of fifty lines, stretched to the width");
  expect(s2 != NULL && cel_y(s2) == 0 && s2->ccb_Height == PIC_H && s2->ccb_HDX == (1L << 20),
         "masked left column: one backdrop block of the column over the band, 1:1");
  expect(pic[120 * PIC_W + 128] == (unsigned char)(16 + (sms.vdp.reg[7] & 15)) && pic[100 * PIC_W + 4] != 18
         && pic[20 * PIC_W + 4] == (unsigned char)(16 + (sms.vdp.reg[7] & 15)),
         "display off and masked column: the backdrop index on both, the sprite covered");
  expect(sms.vdp.cnt_spr_ovf == before_ovf && sms.vdp.cnt_spr_col == before_col,
         "display off: nine sprites on the off lines, two on one column, raise neither flag");
  vdp_list_end();
  reg_write(0,(uint32)sms.vdp.reg[0] & ~0x20UL);
  flush();

  /* The reserve of small cels spent: one block left for a band that
     wants a sprite and the masked column. The sprite takes it, the
     column is refused and counted (the warning is said once, to the
     log), and the chain still closes on CCB_LAST. */
  sat_write(0,59,100,ps);
  sat_write(1,0xD0,0,0);
  reg_write(0,(uint32)sms.vdp.reg[0] | 0x20UL);
  flush();
  /* One picture played whole first, so that the per-line table and the
     display lines are this scene's; then the presentation asked again
     with one block left. */
  bands = play_and_present();
  vdp_list_end();
  sms.vdp.vcount = 200;
  bands = vdp_list_begin();
  sms.vdp.cels_used = VDP_LIST_CELS - 1;
  before = sms.vdp.cnt_cels_refused;
  c = (const CCB *)vdp_list_band(0);
  expect(sms.vdp.cnt_cels_refused == before + 1,"reserve spent: the refused cel is counted");
  expect(nth_kind(c,K_SPRITE,0) != NULL && nth_kind(c,K_BACK,0) == NULL,
         "reserve spent: the sprite took the last block, the column was refused");
  for(n = 0, s = c; s != NULL && n < (long)(VDP_LIST_WINDOWS + VDP_LIST_CELS); n++)
    {
      if(s->ccb_Flags & CCB_LAST) break;
      s = s->ccb_NextPtr;
    }
  expect(s != NULL && (s->ccb_Flags & CCB_LAST) != 0,"reserve spent: the chain is closed on CCB_LAST");
  vdp_list_end();
  reg_write(0,(uint32)sms.vdp.reg[0] & ~0x20UL);
  flush();

  /* Register 5 written on line 100 to another table, at $3E00 -- the
     last rows of the name table, unseen at this scroll: two sprites of
     the first table share a column on lines 40 to 47, the second table
     holds one sprite on lines 100 to 107. Counted as written mid-frame;
     the lines before 100 keep their flags and their admission, the list
     takes the last table: one sprite block, at line 100. */
  for(i = 0; i < 4; i++) saved[i] = sms.vdp.vram[0x3E00 + ((i < 2) ? i : 0x7E + i)];
  sms.vdp.vcount = 200;
  vram_write(0x3E00,99);
  vram_write(0x3E01,0xD0);
  vram_write(0x3E80,120);
  vram_write(0x3E81,ps);
  sat_write(0,39,60,ps);
  sat_write(1,39,60,ps);
  sat_write(2,0xD0,0,0);
  flush();
  before = sms.vdp.cnt_reg_mid;
  before_col = sms.vdp.cnt_spr_col;
  play_line = 100;
  play_reg = 5;
  play_value = 0xFD;
  bands = play_and_present();
  expect(sms.vdp.cnt_reg_mid == before + 1,"register 5 written on line 100: counted as written mid-frame");
  expect(sms.vdp.cnt_spr_col == before_col + 8 && (sms.vdp.spr_adm[40][0] & 3UL) == 3UL
         && (sms.vdp.spr_adm[100][0] & 1UL) != 0UL && (sms.vdp.spr_adm[100][0] & 2UL) == 0UL,
         "register 5 written on line 100: the lines before it keep their collision and their admission");
  c = (const CCB *)vdp_list_band(0);
  s = nth_kind(c,K_SPRITE,0);
  expect(bands == 1 && s != NULL && cel_y(s) == 100 && cel_x(s) == 120 && nth_kind(c,K_SPRITE,1) == NULL,
         "register 5 written on line 100: the list takes the last table, one sprite at line 100");
  expect(pic[100 * PIC_W + 120] == 18 && pic[40 * PIC_W + 60] != 18,
         "register 5 written on line 100: the last table's sprite shows, the first table's does not");
  vdp_list_end();
  sms.vdp.vcount = 200;
  reg_write(5,0xFF);
  for(i = 0; i < 4; i++) vram_write(0x3E00 + ((i < 2) ? i : 0x7E + i),saved[i]);
  flush();

  /* The right edge and the two bits of register 0: a pattern opaque on
     its eight columns at x 250 shows columns 250 to 255 and nothing
     past them -- the block stands at 250 and the clip cuts it; with the
     shift of bit 3 the same block stands at 242. Then two sprites at x
     0 under the masked column of bit 5: no pixel shown, no collision. */
  sms.vdp.vcount = 200;
  for(i = 0; i < 32; i++)
    vram_write(pf * 32 + i,((i & 3) == 1) ? 0xFF : 0);
  sat_write(0,59,250,pf);
  sat_write(1,0xD0,0,0);
  flush();
  bands = play_and_present();
  c = (const CCB *)vdp_list_band(0);
  s = nth_kind(c,K_SPRITE,0);
  expect(s != NULL && cel_x(s) == 250 && cel_y(s) == 60 && s->ccb_Height == 8,"sprite at x 250: the block stands at 250");
  expect(pic[60 * PIC_W + 250] == 18 && pic[60 * PIC_W + 255] == 18 && pic[60 * PIC_W + 249] != 18
         && pic[61 * PIC_W + 0] != 18 && pic[61 * PIC_W + 1] != 18,
         "sprite at x 250: columns 250 to 255 shown, nothing before, nothing wrapped onto the next line");
  vdp_list_end();
  reg_write(0,(uint32)sms.vdp.reg[0] | 0x08UL);
  bands = play_and_present();
  c = (const CCB *)vdp_list_band(0);
  s = nth_kind(c,K_SPRITE,0);
  expect(s != NULL && cel_x(s) == 242,"register 0 bit 3: the same sprite stands eight to the left, at 242");
  expect(pic[60 * PIC_W + 242] == 18 && pic[60 * PIC_W + 249] == 18 && pic[60 * PIC_W + 250] != 18,
         "register 0 bit 3: shown on columns 242 to 249");
  vdp_list_end();
  reg_write(0,((uint32)sms.vdp.reg[0] & ~0x08UL) | 0x20UL);
  sat_write(0,59,0,pf);
  sat_write(1,59,0,pf);
  sat_write(2,0xD0,0,0);
  before_col = sms.vdp.cnt_spr_col;
  bands = play_and_present();
  expect(sms.vdp.cnt_spr_col == before_col,"two sprites at x 0 under the masked column: no collision");
  for(i = 0, n = 0; i < 8; i++)
    if(pic[60 * PIC_W + i] == (unsigned char)(16 + (sms.vdp.reg[7] & 15))) n++;
  expect(n == 8 && pic[60 * PIC_W + 8] != 18,"two sprites at x 0 under the masked column: no pixel of them shown");
  vdp_list_end();
  reg_write(0,(uint32)sms.vdp.reg[0] & ~0x20UL);
  sat_write(0,0xD0,0,0);
  flush();

  /* A run of four priority tiles, columns 28 to 31 of tile row 5, under
     a horizontal scroll of 16: the run lands at screen columns 240 to
     271, so it folds -- two runs of the one source row, tiles 30 and 31
     at x 0, tiles 28 and 29 at x 240. */
  for(i = 0; i < 4; i++) { saved_w[i] = nt_word(5 * 32 + 28 + i); saved_w[4 + i] = nt_word(2 * 32 + 28 + i); }
  sms.vdp.vcount = 200;
  for(i = 0; i < 4; i++) nt_write(5 * 32 + 28 + i,pa | 0x1000UL);
  reg_write(8,16);
  flush();
  bands = play_and_present();
  c = (const CCB *)vdp_list_band(0);
  s = find_prio(c,0,40,16,8);
  s2 = find_prio(c,240,40,16,8);
  expect(s != NULL && cel_off(s,sms.vdp.decor) == 40 * PIC_W + 240,
         "priority run at the fold: tiles 30 and 31 at x 0, from picture column 240, row 40");
  expect(s2 != NULL && cel_off(s2,sms.vdp.decor) == 40 * PIC_W + 224 && nth_kind(c,K_PRIO,2) == NULL,
         "priority run at the fold: tiles 28 and 29 at x 240, from picture column 224, and no third run");
  expect(pic[40 * PIC_W + 240] == 1 && pic[40 * PIC_W + 0] == 1,"priority run at the fold: colour 1 at x 240 and at x 0");
  vdp_list_end();

  /* The same run on tile row 2 under the top lock (register 0 bit 6)
     with a vertical scroll of 4: picture rows 16 to 23 land on screen
     lines 12 to 19, across line 16 where the lock ends. Lines 12 to 15
     take no horizontal scroll -- one run at x 224, rows 16 to 19 of the
     picture -- and lines 16 to 19 take the scroll of 16 and fold as
     above, rows 20 to 23. */
  sms.vdp.vcount = 200;
  for(i = 0; i < 4; i++) nt_write(5 * 32 + 28 + i,saved_w[i]);
  for(i = 0; i < 4; i++) nt_write(2 * 32 + 28 + i,pa | 0x1000UL);
  reg_write(0,(uint32)sms.vdp.reg[0] | 0x40UL);
  reg_write(9,4);
  sms.vdp.vscroll = 4;
  flush();
  bands = play_and_present();
  c = (const CCB *)vdp_list_band(0);
  s = find_prio(c,224,12,32,4);
  expect(s != NULL && cel_off(s,sms.vdp.decor) == 16 * PIC_W + 224,
         "priority run under the top lock: lines 12 to 15 unscrolled, one run at x 224 from picture row 16");
  s = find_prio(c,0,16,16,4);
  s2 = find_prio(c,240,16,16,4);
  expect(s != NULL && cel_off(s,sms.vdp.decor) == 20 * PIC_W + 240 && s2 != NULL
         && cel_off(s2,sms.vdp.decor) == 20 * PIC_W + 224 && nth_kind(c,K_PRIO,3) == NULL,
         "priority run under the top lock: lines 16 to 19 scrolled and folded, two runs from picture row 20");
  expect(pic[12 * PIC_W + 224] == 1 && pic[16 * PIC_W + 240] == 1 && pic[16 * PIC_W + 0] == 1,
         "priority run under the top lock: colour 1 at (224, 12), (240, 16) and (0, 16)");
  vdp_list_end();
  sms.vdp.vcount = 200;
  for(i = 0; i < 4; i++) nt_write(2 * 32 + 28 + i,saved_w[4 + i]);
  reg_write(0,(uint32)sms.vdp.reg[0] & ~0x40UL);
  reg_write(8,0);
  reg_write(9,0);
  sms.vdp.vscroll = 0;
  flush();

  /* The table rewritten on line 100 to name pf instead of ps, then ps
     rewritten on line 150: the per-line table rebuilt at line 100 no
     longer names ps, but lines 40 to 47 drew it and their band replays
     the table that named it, so the write of line 150 is journaled and
     undone for that band -- three bands, and line 40 shows the row as
     it was. */
  sat_write(0,39,60,ps);
  sat_write(1,0xD0,0,0);
  flush();
  sms.vdp.vcount = 0;
  for(i = 0; i < (uint32)PIC_H; i++)
    {
      if(i == 100) vram_write(0x3F81,pf);
      if(i == 150) vram_write(ps * 32 + 1,0);
      line_step();
    }
  bands = vdp_list_begin();
  memset(pic,0xFF,sizeof pic);
  compose_bands(-2,bands);
  expect(bands == 3 && sms.vdp.band_line[1] == 100 && sms.vdp.band_line[2] == 150,
         "pattern of the previous table rewritten on line 150: journaled, three bands");
  expect(pic[40 * PIC_W + 60] == 18,
         "pattern of the previous table rewritten on line 150: line 40 shows the row as it was");
  vdp_list_end();
  sms.vdp.vcount = 200;
  vram_write(ps * 32 + 1,0x80);
  sat_write(0,0xD0,0,0);
  flush();

  /* Register 7 moved in the blanking: the backdrop column is refilled,
     so the masked left column and a line with the display off show the
     new index. Then put back. */
  reg7 = (uint32)sms.vdp.reg[7];
  sms.vdp.vcount = 200;
  reg_write(7,(reg7 & 0xF0UL) | ((reg7 + 5UL) & 0x0FUL));
  reg_write(0,(uint32)sms.vdp.reg[0] | 0x20UL);
  flush();
  play_line = 180;
  play_reg = 1;
  play_value = (uint32)sms.vdp.reg[1] & ~0x40UL;
  bands = play_and_present();
  expect(pic[20 * PIC_W + 3] == (unsigned char)(16 + ((reg7 + 5) & 15))
         && pic[185 * PIC_W + 128] == (unsigned char)(16 + ((reg7 + 5) & 15)),
         "register 7 moved: the masked column and a line with the display off show the new backdrop");
  vdp_list_end();
  sms.vdp.vcount = 200;
  reg_write(1,(uint32)sms.vdp.reg[1] | 0x40UL);
  reg_write(7,reg7);
  reg_write(0,(uint32)sms.vdp.reg[0] & ~0x20UL);
  flush();

  /* Register 5 moved in the blanking to a table on eight free chunks
     outside the name table, never watched before: they are watched from
     the move, the per-line table owed; put back, they are watched no
     more. Then register 6 moved to the second bank with the table
     untouched: the per-line table owed again, since its patterns must
     be named anew. Nothing is written to the free chunks. */
  sms.vdp.vcount = 0;
  for(i = 0; i < (uint32)PIC_H; i++) line_step();
  flush();
  expect(sms.vdp.spr_dirty == 0,"a picture played: the per-line table stands");
  for(q = 0; q < 512; q += 8)
    {
      for(i = 0; i < 8; i++)
        if(sms.vdp.refs[q + i] != 0 || sms.vdp.watch[q + i] != 0) break;
      if(i == 8) break;
    }
  expect(q < 512,"eight free chunks on a table boundary exist for a moved sprite table");
  if(q < 512)
    {
      sms.vdp.vcount = 200;
      reg_write(5,(q >> 2) | 0x81UL);
      expect(sms.vdp.spr_dirty != 0 && sms.vdp.sat_base == q * 32 && sms.vdp.watch[q] == 1 && sms.vdp.watch[q + 7] == 1,
             "register 5 moved to a free table: its eight chunks are watched, the per-line table owed");
      reg_write(5,0xFF);
      expect(sms.vdp.watch[q] == 0 && sms.vdp.watch[q + 7] == 0,"register 5 put back: the free chunks are watched no more");
      sms.vdp.vcount = 0;
      for(i = 0; i < (uint32)PIC_H; i++) line_step();
      flush();
      expect(sms.vdp.spr_dirty == 0,"a picture played after the move: the per-line table stands");
      sms.vdp.vcount = 200;
      reg_write(6,0xFF);
      expect(sms.vdp.spr_dirty != 0,"register 6 moved to the second bank: the per-line table owed");
      reg_write(6,0xFB);
      flush();
    }

  /* Tall sprites (register 1 bit 1): an odd pattern number draws the
     pair (p - 1, p), sixteen rows from the even pattern's place, and two
     tall sprites sharing a column collide on the rows of the second
     pattern as on the first. Pattern pe: index 2 down column 0; pe + 1:
     index 3 down column 0. */
  for(pe = 0; pe < 256; pe += 2)
    if(sms.vdp.refs[pe] == 0 && sms.vdp.watch[pe] == 0
       && sms.vdp.refs[pe + 1] == 0 && sms.vdp.watch[pe + 1] == 0) break;
  expect(pe < 256,"an even pair of free patterns exists in the first eight kilobytes for the tall sprites");
  if(pe < 256)
    {
      sms.vdp.vcount = 200;
      for(i = 0; i < 32; i++) vram_write(pe * 32 + i,((i & 3) == 1) ? 0x80 : 0);
      for(i = 0; i < 32; i++) vram_write((pe + 1) * 32 + i,((i & 3) <= 1) ? 0x80 : 0);
      reg_write(1,(uint32)sms.vdp.reg[1] | 0x02UL);
      sat_write(0,59,100,pe + 1);
      sat_write(1,0xD0,0,0);
      flush();
      bands = play_and_present();
      c = (const CCB *)vdp_list_band(0);
      s = nth_kind(c,K_SPRITE,0);
      expect(s != NULL && cel_off(s,sms.vdp.sheet) == ((pe >> 6) * 16) * PIC_W + ((pe >> 1) & 31) * 8
             && s->ccb_Height == 16 && cel_y(s) == 60 && nth_kind(c,K_SPRITE,1) == NULL,
             "tall sprite named by its odd pattern: one block of sixteen rows from the even pattern's place");
      expect(pic[60 * PIC_W + 100] == 18 && pic[67 * PIC_W + 100] == 18 && pic[68 * PIC_W + 100] == 19
             && pic[75 * PIC_W + 100] == 19 && pic[76 * PIC_W + 100] != 19,
             "tall sprite: the even pattern on lines 60 to 67, the odd one on 68 to 75");
      vdp_list_end();
      /* Two tall sprites on one column, opaque on their second pattern
         alone: the collision on lines 68 to 75 alone. */
      sms.vdp.vcount = 200;
      for(i = 0; i < 32; i++) vram_write(pe * 32 + i,0);
      sat_write(1,59,100,pe + 1);
      sat_write(2,0xD0,0,0);
      flush();
      before_col = sms.vdp.cnt_spr_col;
      bands = play_and_present();
      expect(sms.vdp.cnt_spr_col == before_col + 8,
             "two tall sprites on one column, opaque on their second pattern alone: the collision on eight lines");
      vdp_list_end();
      sms.vdp.vcount = 200;
      reg_write(1,(uint32)sms.vdp.reg[1] & ~0x02UL);
      sat_write(0,0xD0,0,0);
      flush();
    }

  /* The table put back to nothing, the registers as they were. */
  sms.vdp.vcount = 200;
  for(i = 0; i < 3; i++) sat_write(i,0xD0,0,0);
  reg_write(1,reg1);
  flush();
}

static void scenes(void)
{
  uint32 nt, addr, old, p, q, t, i, before, reg2, pa, pb;
  int32 bands;
  const CCB *c;
  long n;

  /* A known geometry: no lock, no scroll, no masked column, the table
     where register 2 left it, no sprite -- the ROM's attribute table
     ended on its first entry -- and one picture played blank so that
     the per-line table is the ended table's, as a frame would leave it.
     The control port's latch dropped first: the ROM may have stopped
     between the two bytes of a sequence. In the blanking, so that the
     register writes count as nothing. */
  sms.vdp.latch = 0;
  sms.vdp.vcount = 200;
  reg_write(0,(uint32)sms.vdp.reg[0] & ~0xE0UL);
  reg_write(8,0);
  reg_write(9,0);
  sms.vdp.hscroll = 0;
  sms.vdp.vscroll = 0;
  vram_write((((uint32)sms.vdp.reg[5] & 0x7EUL) << 7),0xD0);
  sms.vdp.vcount = 0;
  for(i = 0; i < (uint32)PIC_H; i++) line_step();
  flush();
  nt = nt_base();

  /* A write outside the display: dirty, not journaled. */
  addr = nt + 2 * 17;
  old = sms.vdp.vram[addr];
  sms.vdp.vcount = 200;
  before = sms.vdp.journal_count;
  vram_write(addr,old ^ 1);
  expect(sms.vdp.journal_count == before,"blanking write: the journal is unchanged");
  expect(VDP_DECOR_DIRTY[addr >> 5] == 1,"blanking write: the chunk is marked dirty");
  flush();

  /* A name table entry written on line 50: one entry, one boundary. */
  t = 5 * 32 + 5;
  addr = nt + 2 * t;
  old = sms.vdp.vram[addr];
  sms.vdp.vcount = 50;
  vram_write(addr,old ^ 1);
  expect(sms.vdp.journal_count == 1
         && sms.vdp.journal[0].line == 50 && sms.vdp.journal[0].addr == addr
         && sms.vdp.journal[0].old == old && sms.vdp.journal[0].val == (old ^ 1),
         "name table entry written on line 50: journaled as (50, addr, old, new)");
  bands = vdp_list_begin();
  expect(bands == 2 && sms.vdp.band_line[1] == 50,
         "name table entry written on line 50: two bands, the second from line 50");
  compose_bands(-1,bands);
  vdp_list_end();
  flush();

  /* A pattern the picture shows, written on line 60: journaled. */
  p = nt_word(0) & 0x1FF;
  expect(sms.vdp.refs[p] > 0,"the pattern of tile 0 is counted as referenced");
  addr = p * 32 + 3;
  old = sms.vdp.vram[addr];
  sms.vdp.vcount = 60;
  vram_write(addr,old ^ 0x5A);
  expect(sms.vdp.journal_count == 1,"referenced pattern written on line 60: journaled");
  flush();

  /* A pattern a name table entry names earlier in the same picture, hot
     before the conversion counted it: written after, journaled. */
  q = free_pattern(0);
  expect(q != 0xFFFFFFFFUL,"a pattern the picture does not show exists");
  if(q != 0xFFFFFFFFUL)
    {
      t = 6 * 32 + 6;
      sms.vdp.vcount = 40;
      vram_write(nt + 2 * t,q & 0xFF);
      vram_write(nt + 2 * t + 1,q >> 8);
      expect(sms.vdp.hot[q] == 1 && sms.vdp.watch[q] == 1,
             "pattern named by a name table write on line 40: marked hot and watched");
      before = sms.vdp.journal_count;
      sms.vdp.vcount = 41;
      addr = q * 32;
      vram_write(addr,sms.vdp.vram[addr] ^ 0xFF);
      expect(sms.vdp.journal_count == before + 1,"hot pattern written on line 41: journaled");
      flush();
      expect(sms.vdp.hot[q] == 0 && sms.vdp.refs[q] > 0,
             "after the presentation the hot mark fell and the pattern is referenced");
    }

  /* A pattern neither the picture shows nor the sprite table names,
     written on line 70: nothing, no band. (A pattern the table names is
     the sprite scenes' business, below.) */
  q = free_pattern(0);
  expect(q != 0xFFFFFFFFUL,"a second pattern the picture does not show exists");
  if(q != 0xFFFFFFFFUL)
    {
      sms.vdp.vcount = 70;
      addr = q * 32;
      vram_write(addr,sms.vdp.vram[addr] ^ 0xFF);
      expect(sms.vdp.journal_count == 0,"unused pattern written on line 70: not journaled");
      bands = vdp_list_begin();
      expect(bands == 1,"unused pattern written on line 70: one band");
      compose_bands(-1,bands);
      vdp_list_end();
      flush();
    }

  /* Two probe patterns of this file's own, in the block the picture does
     not use: A draws index 1 down its column 0, B index 2, so that a tile
     shown as A or as B is told apart by one pixel, and the two words
     differ in their low byte alone, one write each. */
  pa = free_pattern(0);
  pb = free_pattern(pa + 1);
  if((pa >> 8) != (pb >> 8))
    {
      pa = pb;
      pb = free_pattern(pa + 1);
    }
  expect(pa != 0xFFFFFFFFUL && pb != 0xFFFFFFFFUL && (pa >> 8) == (pb >> 8),
         "two free patterns in one block of 256 exist for the probes");
  sms.vdp.vcount = 200;
  for(i = 0; i < 32; i++)
    {
      vram_write(pa * 32 + i,((i & 3) == 0) ? 0x80 : 0);
      vram_write(pb * 32 + i,((i & 3) == 1) ? 0x80 : 0);
    }
  flush();
  expect(tile_pixel(pa,0,0) == 1 && tile_pixel(pb,0,0) == 2 && tile_pixel(pb,4,0) == 2,
         "the probe patterns draw index 1 and index 2 at column 0");

  /* A pattern the picture shows, its bytes rewritten. Two tiles show A,
     in rows 1 and 4. Rewritten in the blanking so that its column 0
     draws index 3, the next presentation shows 3 in both tiles: the
     tiles were converted again for the pattern's sake, not the table's.
     Rewritten back to index 1 on line 30, band 0 (lines 0 to 29) shows
     row 1 as it was, band 1 (from line 30) shows row 4 as it is. */
  nt_write(1 * 32 + 3,pa);
  nt_write(4 * 32 + 3,pa);
  flush();
  sms.vdp.vcount = 200;
  for(i = 0; i < 8; i++)
    vram_write(pa * 32 + i * 4 + 1,0x80);
  bands = vdp_list_begin();
  memset(pic,0xFF,sizeof pic);
  compose_bands(-1,bands);
  expect(pic[8 * PIC_W + 24] == 3 && pic[32 * PIC_W + 24] == 3,
         "referenced pattern rewritten in the blanking: both tiles show the new index");
  vdp_list_end();
  sms.vdp.vcount = 30;
  for(i = 0; i < 8; i++)
    vram_write(pa * 32 + i * 4 + 1,0);
  expect(sms.vdp.journal_count == 8,"referenced pattern rewritten on line 30: eight entries");
  bands = vdp_list_begin();
  expect(bands == 2 && sms.vdp.band_line[1] == 30,"referenced pattern rewritten on line 30: a band from line 30");
  memset(pic,0xFF,sizeof pic);
  compose_bands(-1,bands);
  expect(pic[8 * PIC_W + 24] == 3,"band 0 shows the tile of row 1 with the pattern as it was");
  expect(pic[32 * PIC_W + 24] == 1,"band 1, from line 30, shows the tile of row 4 with the pattern as it is");
  vdp_list_end();
  flush();

  /* Nine distinct lines, 10 to 90, each moving the tile of column 3 in
     row 2i from A to B: eight bands, the ninth line folded into the last,
     counted. Band 0 shows tile row 0 as it was before its write on line
     10; the last band, from line 70, shows tile row 16 after its write on
     line 90, which is past the cap; tile row 2, written on line 20, is
     old at line 16 in band 1 and new at line 20 in band 2 -- the write of
     a band's first line belongs to that band. */
  for(i = 0; i < 9; i++)
    {
      t = (i * 2) * 32 + 3;
      nt_write(t,pa);
    }
  flush();
  before = sms.vdp.cnt_bands_capped;
  for(i = 0; i < 9; i++)
    {
      t = (i * 2) * 32 + 3;
      sms.vdp.vcount = 10 * (i + 1);
      vram_write(nt + 2 * t,pb & 0xFF);
    }
  expect(sms.vdp.journal_count == 9,"nine writes on nine lines: nine entries");
  bands = vdp_list_begin();
  expect(bands == 8 && sms.vdp.band_line[7] == 70,"nine distinct lines: eight bands, the last from line 70");
  expect(sms.vdp.cnt_bands_capped == before + 1,"nine distinct lines: the cap counted once");
  memset(pic,0xFF,sizeof pic);
  compose_bands(-1,bands);
  expect(pic[0 * PIC_W + 24] == 1,"band 0 shows tile row 0 as it stood before its write on line 10");
  expect(pic[128 * PIC_W + 24] == 2,"the last band shows tile row 16 after its write on line 90, folded past the cap");
  expect(pic[16 * PIC_W + 24] == 1,"band 1 shows tile row 2 as it stood before its write on line 20");
  expect(pic[20 * PIC_W + 24] == 2,"band 2, from line 20, shows tile row 2 after its write on line 20");
  vdp_list_end();
  flush();

  /* The journal full: 257 visible writes on line 100 -- tile 0 from A
     to B, 254 toggles, tile row 13 from A to B, then tile 0 back to A,
     the 257th, on an address the journal already holds. The 257th is
     lost to the journal and counted, and the presentation must then
     touch the memory not at all: one band, every tile as it finally is
     -- tile 0 as A, the 257th value, tile row 13 as B -- and after the
     presentation the memory still holds the 257th value: an undo and a
     replay of the journal would have left B there for good. */
  nt_write(0,pa);
  nt_write(13 * 32,pa);
  flush();
  before = sms.vdp.cnt_journal_full;
  sms.vdp.vcount = 100;
  vram_write(nt,pb & 0xFF);
  for(i = 1; i < 255; i++)
    vram_write(nt + 2 * i,sms.vdp.vram[nt + 2 * i] ^ 1);
  vram_write(nt + 2 * (13 * 32),pb & 0xFF);
  vram_write(nt,pa & 0xFF);
  expect(sms.vdp.journal_count == 256,"257 visible writes: the journal holds 256");
  expect(sms.vdp.cnt_journal_full == before + 1 && sms.vdp.journal_overflow == 1,
         "257 visible writes: one counted lost, the overflow flag up");
  bands = vdp_list_begin();
  expect(bands == 1 && sms.vdp.journal_count == 0,"journal overflowed: one band, the journal dropped");
  expect(sms.vdp.vram[nt] == (pa & 0xFF),"journal overflowed: nothing is undone at the head of the presentation");
  memset(pic,0xFF,sizeof pic);
  compose_bands(-1,bands);
  expect(pic[0] == 1,"band 0 shows tile 0 with the 257th value, the final one");
  expect(pic[104 * PIC_W] == 2,"band 0 shows tile row 13 as it finally is");
  vdp_list_end();
  expect(sms.vdp.journal_overflow == 0 && sms.vdp.vram[nt] == (pa & 0xFF),
         "after the presentation the flag fell and the memory holds the 257th value");
  flush();

  /* Register 2 moved: every tile never converted, then all 896 converted
     at the next presentation. Then put back. */
  reg2 = sms.vdp.reg[2];
  reg_write(2,(uint32)reg2 ^ 0x02UL);
  for(i = 0, n = 0; i < VDP_NT_TILES; i++)
    if(sms.vdp.decor_word[i] == 0xFFFFU) n++;
  expect(n == (long)VDP_NT_TILES,"register 2 moved: 896 tiles marked never converted");
  before = sms.vdp.cnt_decor_tiles;
  sms.vdp.vcount = 200;
  bands = vdp_list_begin();
  expect(sms.vdp.cnt_decor_tiles == before + VDP_NT_TILES,"register 2 moved: 896 tiles converted at the presentation");
  compose_bands(-1,bands);
  vdp_list_end();
  reg_write(2,reg2);
  flush();
  nt = nt_base();

  /* Fine scroll 5: the scrolled region's first window shows columns 251
     to 255 at x 0 to 4, so it reads from column 248, stands at -3 and is
     8 wide; the second starts at column 0, x 5. */
  sms.vdp.hscroll = 5;
  sms.vdp.vcount = 200;
  bands = vdp_list_begin();
  c = (const CCB *)vdp_list_band(0);
  expect(c != NULL && bands == 1,"fine scroll 5: one band with windows");
  if(c != NULL)
    {
      expect((win_off(c) % PIC_W) == 248 && win_x(c) == -3 && c->ccb_Width == 8,
             "fine scroll 5: the first window reads from column 248, stands at -3, is 8 wide");
      expect(c->ccb_NextPtr != NULL && (win_off(c->ccb_NextPtr) % PIC_W) == 0
             && win_x(c->ccb_NextPtr) == 5 && c->ccb_NextPtr->ccb_Width == 251,
             "fine scroll 5: the second window reads from column 0, stands at 5, is 251 wide");
    }
  vdp_list_end();

  /* With the right columns locked (register 0 bit 7) and a vertical
     scroll of 40: their window first in the chain -- columns 197 to 255
     from picture column 192, ROW 0 since they take no vertical scroll,
     all 192 lines -- then the scrolled region's, from row 40. */
  sms.vdp.vscroll = 40;
  reg_write(0,(uint32)sms.vdp.reg[0] | 0x80UL);
  bands = vdp_list_begin();
  c = (const CCB *)vdp_list_band(0);
  expect(c != NULL,"right lock: a band with windows");
  if(c != NULL)
    {
      expect(win_x(c) == 197 && c->ccb_Width == 59 && win_off(c) == 192 && c->ccb_Height == 192,
             "right lock: the locked columns' window comes first, from picture column 192, row 0, 192 lines");
      expect(c->ccb_NextPtr != NULL && win_x(c->ccb_NextPtr) == -3
             && win_off(c->ccb_NextPtr) == 40 * PIC_W + 248,
             "right lock: the scrolled region's window follows it, from row 40");
    }
  vdp_list_end();
  reg_write(0,(uint32)sms.vdp.reg[0] & ~0x80UL);

  /* With the top rows locked (register 0 bit 6), scroll 5 and 40: rows
     0 to 15 take no horizontal scroll -- one window, column 0 at x 0,
     the whole width, no join -- but they do take the vertical one, as
     the line render did (row 40 of the picture); from row 16 the
     horizontal scroll applies, the join at x 5, row 56 of the picture. */
  reg_write(0,(uint32)sms.vdp.reg[0] | 0x40UL);
  bands = vdp_list_begin();
  c = (const CCB *)vdp_list_band(0);
  expect(c != NULL,"top lock: a band with windows");
  if(c != NULL)
    {
      expect(win_x(c) == 0 && c->ccb_YPos == 0 && c->ccb_Width == 256 && c->ccb_Height == 16
             && win_off(c) == 40 * PIC_W,
             "top lock: rows 0 to 15 are one window, column 0 at x 0, 256 wide, from picture row 40");
      expect(c->ccb_NextPtr != NULL && win_x(c->ccb_NextPtr) == -3
             && (c->ccb_NextPtr->ccb_YPos / 65536L) == 16 && c->ccb_NextPtr->ccb_Height == 168
             && win_off(c->ccb_NextPtr) == 56 * PIC_W + 248,
             "top lock: from row 16 the scroll applies, a window at -3 from picture row 56, column 248");
      expect(c->ccb_NextPtr != NULL && c->ccb_NextPtr->ccb_NextPtr != NULL
             && win_x(c->ccb_NextPtr->ccb_NextPtr) == 5 && c->ccb_NextPtr->ccb_NextPtr->ccb_Width == 251,
             "top lock: then the window at x 5, 251 wide");
    }
  vdp_list_end();
  reg_write(0,(uint32)sms.vdp.reg[0] & ~0x40UL);

  /* The arena nearly full: with scroll 5 and no vertical scroll the
     band wants two windows and finds one block; the refusal is counted
     and the chain still closes. */
  sms.vdp.vscroll = 0;
  bands = vdp_list_begin();
  sms.vdp.list_used = VDP_LIST_WINDOWS - 1;
  before = sms.vdp.cnt_list_refused;
  c = (const CCB *)vdp_list_band(0);
  expect(sms.vdp.cnt_list_refused == before + 1,"arena full: the refused window is counted");
  for(n = 0; c != NULL && n < (long)VDP_LIST_WINDOWS; n++)
    {
      if(c->ccb_Flags & CCB_LAST) break;
      c = c->ccb_NextPtr;
    }
  expect(c != NULL && (c->ccb_Flags & CCB_LAST) != 0,"arena full: the chain is closed on CCB_LAST");
  vdp_list_end();
  sms.vdp.hscroll = 0;
  sms.vdp.vscroll = 0;
  flush();

  /* ---- the sprites and the priority tiles: the cases the ROM does not
     reach, on a table of this file's own at $3F00, patterns in the first
     eight kilobytes, no shift, no mask, the display on. The picture is
     played line by line as the frame loop plays it, so that the per-line
     table, the flags and the journal are exercised as on the console. ---- */
  sprite_scenes(pa);
}
#endif /* SMS_DECOR_CEL */

/* FNV-1a over a run of bytes, the digest the render bench uses too
   (tests/vdp-profile/bench_profile.c). The reference holds one per row
   and one for the ROM. */
static unsigned long digest(const unsigned char *p, unsigned long n)
{
  unsigned long h = 2166136261UL;
  unsigned long i;
  for(i = 0; i < n; i++)
    {
      h ^= (unsigned long)p[i];
      h *= 16777619UL;
      h &= 0xFFFFFFFFUL;
    }
  return h;
}

/* ---- the two tables the console turns an index into a colour with ---- */

/* The cel's palette must be the identity -- entry n holds n on each of its
   three five bit components -- or the index a pixel carries is not the
   index the screen table is asked for. Returns how many of the 32 entries
   are. */
static unsigned long plut_identity(void)
{
  unsigned long ok = 0, n;
  for(n = 0; n < VDP_PLUT_ENTRIES; n++)
    if(sms.vdp.plut[n] == (uint16)((n << 10) | (n << 5) | n)) ok++;
  return ok;
}

/* The screen table must be the colour memory converted: entry i carries i
   in its high byte and, per component, the level of the two bits of
   cram[i] -- R1R0 in bits 0-1, G1G0 in 2-3, B1B0 in 4-5 -- at 0, 85, 170
   or 255 (docs/sms_gg/SMSOfficialDocs.md:483-495); and the 33rd entry, the
   display's background colour, carries the control bits 0xE0000000
   (include/3do/hardware.h:71,73) over the three components of colour 0,
   since a pixel of index 0 is the zero word the display paints in that
   colour. Computed here from the colour memory alone, owing the render's
   own table nothing. Returns how many of the 33 entries are. */
static unsigned long clut_entries(void)
{
  static const unsigned long level[4] = { 0, 85, 170, 255 };
  const uint32 *clut = vdp_clut();
  unsigned long ok = 0, i, want, c, rgb;
  for(i = 0; i < VDP_CRAM_SIZE; i++)
    {
      c = sms.vdp.cram[i] & 0x3FUL;
      want = (i << 24) | (level[c & 3] << 16) | (level[(c >> 2) & 3] << 8)
           | level[(c >> 4) & 3];
      if((unsigned long)clut[i] == want) ok++;
    }
  c = sms.vdp.cram[0] & 0x3FUL;
  rgb = (level[c & 3] << 16) | (level[(c >> 2) & 3] << 8) | level[(c >> 4) & 3];
  if((unsigned long)clut[VDP_CRAM_SIZE] == (0xE0000000UL | rgb)) ok++;
  return ok;
}

/* The border is filled with a NUMBER and not a colour: the backdrop entry,
   16 plus the low four bits of register 7 (docs/sms_gg/SMSOfficialDocs.md:
   861-864), repeated on the three components exactly as the identity
   palette emits a pixel of that index. Computed here from the register
   alone. Returns 1 when the render agrees. */
static int backdrop_number(void)
{
  unsigned long n = 16UL + (sms.vdp.reg[7] & 15UL);
  return vdp_backdrop() == (uint16)((n << 10) | (n << 5) | n);
}

/* ---- PPM, the screen as the console shows it ---- */

static unsigned char screen[240][320][3];

/* One entry of the screen table to its three bytes: red, green, blue. */
static void entry_rgb(uint32 e, unsigned char *p)
{
  p[0] = (unsigned char)((e >> 16) & 255);
  p[1] = (unsigned char)((e >> 8) & 255);
  p[2] = (unsigned char)(e & 255);
}

static void ppm(const char *dir, long frame)
{
  char path[512];
  unsigned char b[3];
  int x, y;
  FILE *f;
  int n;
  const uint32 *clut = vdp_clut();

  /* The border is filled with the backdrop NUMBER on every component; the
     screen table gives it its colour, as it does every pixel. */
  entry_rgb(clut[vdp_backdrop() & 31],b);
  for(y = 0; y < 240; y++)
    for(x = 0; x < 320; x++)
      memcpy(screen[y][x],b,3);
  for(y = 0; y < PIC_H; y++)
    for(x = 0; x < PIC_W; x++)
      {
        unsigned idx = pic[y * PIC_W + x];
        if(idx < VDP_CRAM_SIZE)
          entry_rgb(clut[idx],screen[24 + y][32 + x]);
        else
          { screen[24 + y][32 + x][0] = 255; screen[24 + y][32 + x][1] = 0;
            screen[24 + y][32 + x][2] = 255; }
      }
  n = snprintf(path,sizeof path,"%s/f%05ld.ppm",dir,frame);
  if(n < 0 || (size_t)n >= sizeof path)
    {
      fprintf(stderr,"ppm path too long, picture %ld not written\n",frame);
      return;
    }
  f = fopen(path,"wb");
  if(f == NULL)
    {
      fprintf(stderr,"cannot write %s\n",path);
      return;
    }
  fprintf(f,"P6\n320 240\n255\n");
  fwrite(screen,1,sizeof screen,f);
  fclose(f);
}

/* ---- the reference: one header line, then one line per picture ---- */

/* The header names everything a comparison depends on, and every field is
   held: a reference taken with other parameters, on another ROM, or of
   another picture shape is refused, never compared on a common prefix.
   The last field names the build the reference was taken from and is not
   compared -- it is there for the reader. */
#define REF_TAG "cel8-picture-reference"

static int ref_header_read(FILE *f, long frames, long every,
                           unsigned long rom_bytes, unsigned long rom_fnv,
                           unsigned long *pictures)
{
  char line[512];
  char tag[64];
  char taken[64];
  long h_frames, h_every, h_width, h_lines;
  unsigned long h_pictures, h_rom_bytes, h_rom_fnv;

  if(fgets(line,sizeof line,f) == NULL)
    {
      fprintf(stderr,"the reference is empty\n");
      return 2;
    }
  if(sscanf(line,"%63s frames=%ld every=%ld width=%ld lines=%ld pictures=%lu "
            "rom_bytes=%lu rom_fnv=%lx taken=%63s",
            tag,&h_frames,&h_every,&h_width,&h_lines,&h_pictures,
            &h_rom_bytes,&h_rom_fnv,taken) != 9
     || strcmp(tag,REF_TAG) != 0)
    {
      fprintf(stderr,"the reference header is not one this runner reads\n");
      return 2;
    }
  if(h_rom_bytes != rom_bytes || h_rom_fnv != rom_fnv)
    {
      fprintf(stderr,"skipped: the rom on disc (%lu bytes, fnv %08lx) is not the one the "
              "reference was taken from (%lu bytes, fnv %08lx)\n",
              rom_bytes,rom_fnv,h_rom_bytes,h_rom_fnv);
      return 3;
    }
  if(h_frames != frames || h_every != every
     || h_width != PIC_W || h_lines != PIC_H)
    {
      fprintf(stderr,"the reference was taken for %ld frames every %ld on a %ldx%ld "
              "picture, not %ld every %ld on %dx%d\n",
              h_frames,h_every,h_width,h_lines,frames,every,PIC_W,PIC_H);
      return 2;
    }
  *pictures = h_pictures;
  return 0;
}

static void ref_header_write(FILE *f, long frames, long every,
                             unsigned long pictures, unsigned long rom_bytes,
                             unsigned long rom_fnv, const char *taken)
{
  fprintf(f,"%s frames=%ld every=%ld width=%d lines=%d pictures=%lu "
          "rom_bytes=%lu rom_fnv=%08lx taken=%s\n",
          REF_TAG,frames,every,PIC_W,PIC_H,pictures,rom_bytes,rom_fnv,taken);
}

/* ---- main ---- */

int main(int argc, char **argv)
{
  long frames, every, fr;
  int writing;
  FILE *ref;
  const char *ppmdir;
  const char *taken = "unnamed";
  char tmp[512];
  int n;
  int line;
  int32 residue = 0;
  unsigned long pictures = 0, lines = 0, identical = 0, different = 0;
  unsigned long want_pictures = 0;
  unsigned long rom_fnv;
  /* What the run exercised, per frame, so the figures say what they cover:
     frames with the picture off, with the left column masked, and with each
     of the eight fine scrolls (video registers 1, 0 and 8). */
  unsigned long off_frames = 0, masked_frames = 0, fine_frames[8];
  unsigned long w;
  /* The fewest entries found right, over every picture taken: one bad
     entry on one picture is a failure, whatever the others showed. */
  unsigned long plut_ok = VDP_PLUT_ENTRIES, clut_ok = VDP_CLUT_ENTRIES, k;
  /* Frames on which the rebuild signal the frame loop consumes agreed with
     the colour write counter: raised when and only when a colour byte was
     written since the last frame. Counted on every frame, not only the
     pictures taken. */
  unsigned long take_ok = 0, take_frames = 0, cram_w_seen = 0;
  /* Pictures whose border carries the backdrop number register 7 names. */
  unsigned long backdrop_ok = 0;
  /* The two sprite bits, as lines raised per frame: the older path writes
     them after each picture of a reference it mints, the list path holds
     its own against them -- pictures where both agreed, pictures where the
     reference carried them at all. */
  unsigned long ovf_seen = 0, col_seen = 0, ovf_d, col_d;
  unsigned long flags_ok = 0, flags_seen = 0;
  static unsigned long row_digest[PIC_H];

  memset(fine_frames,0,sizeof fine_frames);

  if(argc < 6)
    {
      fprintf(stderr,"usage: romrun <rom> <frames> <every> write|compare <reference> [ppm dir] [taken]\n");
      return 2;
    }
  rom_path = argv[1];
  frames = atol(argv[2]);
  every = atol(argv[3]);
  if(frames <= 0 || every <= 0)
    {
      fprintf(stderr,"frames and every must be positive integers\n");
      return 2;
    }
  if(strcmp(argv[4],"write") == 0)
    writing = 1;
  else if(strcmp(argv[4],"compare") == 0)
    writing = 0;
  else
    {
      fprintf(stderr,"mode must be write or compare, not %s\n",argv[4]);
      return 2;
    }
  ppmdir = (argc > 6 && argv[6][0] != '\0') ? argv[6] : NULL;
  if(argc > 7 && argv[7][0] != '\0')
    {
      /* The header is read back as one word: a name that is not one would
         make the runner refuse the file it wrote itself. */
      if(strlen(argv[7]) > 63 || strpbrk(argv[7]," \t\r\n") != NULL)
        {
          fprintf(stderr,"taken must be one word of at most 63 characters, not '%s'\n",argv[7]);
          return 2;
        }
      taken = argv[7];
    }

  z80_init();
  if(cart_init() < 0) return 2;
  if(cart_boot() < 0) return 2;
  if(vdp_init() < 0) return 2;
  z80_reset();
  booted = 1;

  rom_fnv = digest(sms.cart.rom,(unsigned long)sms.cart.size);

  /* Written to a temporary name beside the final one and renamed at the
     end, once the run has proved it took what the header says: a run that
     stops half way leaves no half reference behind. */
  if(writing)
    {
      n = snprintf(tmp,sizeof tmp,"%s.tmp",argv[5]);
      if(n < 0 || (size_t)n >= sizeof tmp)
        {
          fprintf(stderr,"reference path too long\n");
          return 2;
        }
      ref = fopen(tmp,"w");
      if(ref == NULL)
        {
          fprintf(stderr,"cannot write %s\n",tmp);
          return 2;
        }
      /* The header needs the picture count, which the loop decides: it is
         written first with the count computed the same way. */
      for(fr = 0; fr < frames; fr++)
        if((fr % every) == 0 || fr == frames - 1) want_pictures++;
      ref_header_write(ref,frames,every,want_pictures,
                       (unsigned long)sms.cart.size,rom_fnv,taken);
    }
  else
    {
      int rc;
      ref = fopen(argv[5],"r");
      if(ref == NULL)
        {
          fprintf(stderr,"cannot open the reference %s\n",argv[5]);
          return 2;
        }
      rc = ref_header_read(ref,frames,every,(unsigned long)sms.cart.size,
                           rom_fnv,&want_pictures);
      if(rc != 0)
        {
          fclose(ref);
          return rc;
        }
    }

  for(fr = 0; fr < frames; fr++)
    {
      int y;

      for(line = 0; line < LINES_PER_FRAME; line++)
        {
          residue = z80_run(TSTATES_PER_LINE - residue);
          line_step();
#if SMS_DECOR_CEL
          /* The presentation, once line 191 is counted, as src/main.c
             makes it: the list built, held and copied, on every frame,
             so that the journal and the bands are exercised as they are
             on the console and not on the pictures taken alone. */
          if(line == PIC_H - 1) present(fr);
#endif
        }
      if((sms.vdp.reg[1] & 0x40U) == 0U) off_frames++;
      if((sms.vdp.reg[0] & 0x20U) != 0U) masked_frames++;
      fine_frames[sms.vdp.reg[8] & 7U]++;

      /* The end of the frame as src/main.c closes it: the screen table is
         rebuilt now if a colour moved, so what the picture below is read
         through is what the console would show. The signal itself is held
         against the colour write counter: the frame loop rearms its table
         countdown on it, so a rebuild that stayed silent, or one that
         fired without a write, would leave the console in the wrong
         colours while every table here reads right. */
      k = (sms.vdp.cnt_cram_w != cram_w_seen) ? 1UL : 0UL;
      cram_w_seen = sms.vdp.cnt_cram_w;
      if((vdp_clut_take() != 0) == (k != 0UL)) take_ok++;
      take_frames++;

      if((fr % every) != 0 && fr != frames - 1)
        continue;

#if !SMS_DECOR_CEL
      take();
#endif
      pictures++;
      k = plut_identity();
      if(k < plut_ok) plut_ok = k;
      k = clut_entries();
      if(k < clut_ok) clut_ok = k;
      if(backdrop_number()) backdrop_ok++;
      if(ppmdir != NULL) ppm(ppmdir,fr);
      for(y = 0; y < PIC_H; y++)
        row_digest[y] = digest(pic + (y * PIC_W),(unsigned long)PIC_W);
      ovf_d = sms.vdp.cnt_spr_ovf - ovf_seen;
      col_d = sms.vdp.cnt_spr_col - col_seen;
      ovf_seen = sms.vdp.cnt_spr_ovf;
      col_seen = sms.vdp.cnt_spr_col;

      if(writing)
        {
          fprintf(ref,"frame=%ld",fr);
          for(y = 0; y < PIC_H; y++)
            fprintf(ref," %08lx",row_digest[y]);
          fputc('\n',ref);
#if !SMS_DECOR_CEL
          /* The two sprite bits of the frame, from the path that raises
             them pixel by pixel: the line the list path is held to. Never
             written by the list path, so a frozen reference minted from it
             carries none and the older path is never held to itself. */
          fprintf(ref,"flags ovf=%lu col=%lu\n",ovf_d,col_d);
#endif
        }
      else
        {
          long ref_frame;
          if(fscanf(ref," frame=%ld",&ref_frame) != 1 || ref_frame != fr)
            {
              fprintf(stderr,"the reference does not hold frame %ld where this run took it\n",fr);
              fclose(ref);
              return 2;
            }
          for(y = 0; y < PIC_H; y++)
            {
              unsigned long h;
              if(fscanf(ref," %lx",&h) != 1)
                {
                  fprintf(stderr,"the reference ends inside frame %ld\n",fr);
                  fclose(ref);
                  return 2;
                }
              lines++;
              if(h == row_digest[y])
                identical++;
              else
                {
                  different++;
                  if(different <= 8)
                    fprintf(stderr,"  frame %ld line %d differs\n",fr,y);
                }
            }
          /* The flags line after the digests, when the reference carries
             one: the rest of the digest line is consumed, the next line
             read and put back if it is not one. */
          {
            char fl[128];
            long pos;
            unsigned long r_ovf, r_col;
            if(fgets(fl,sizeof fl,ref) != NULL)
              {
                pos = ftell(ref);
                if(fgets(fl,sizeof fl,ref) != NULL)
                  {
                    if(sscanf(fl,"flags ovf=%lu col=%lu",&r_ovf,&r_col) == 2)
                      {
                        flags_seen++;
                        if(r_ovf == ovf_d && r_col == col_d)
                          flags_ok++;
                        else if(flags_seen - flags_ok <= 8)
                          fprintf(stderr,"  frame %ld flags differ: ovf %lu/%lu col %lu/%lu\n",
                                  fr,ovf_d,r_ovf,col_d,r_col);
                      }
                    else
                      fseek(ref,pos,SEEK_SET);
                  }
              }
          }
        }
    }

  if(!writing)
    {
      long stray;
      if(fscanf(ref," frame=%ld",&stray) == 1)
        {
          fprintf(stderr,"the reference holds more pictures than this run took\n");
          fclose(ref);
          return 2;
        }
    }
  if(writing && ferror(ref))
    {
      fprintf(stderr,"write error on %s\n",tmp);
      fclose(ref);
      remove(tmp);
      return 2;
    }
  if(fclose(ref) != 0)
    {
      fprintf(stderr,"cannot close %s\n",writing ? tmp : argv[5]);
      if(writing) remove(tmp);
      return 2;
    }

  printf("covered frames=%lu off=%lu masked=%lu fine=",
         (unsigned long)frames,off_frames,masked_frames);
  for(w = 0; w < 8; w++)
    printf("%s%lu",(w != 0) ? "/" : "",fine_frames[w]);
  printf("\n");

  printf("plut identity %lu/%d\n",plut_ok,(int)VDP_PLUT_ENTRIES);
  printf("clut entries %lu/%d\n",clut_ok,(int)VDP_CLUT_ENTRIES);
  printf("backdrop number %lu/%lu\n",backdrop_ok,pictures);
  printf("clut take %lu/%lu\n",take_ok,take_frames);
#if SMS_DECOR_CEL
  /* The scenes after the ROM's frames and before the windows are
     counted: their windows are held like any other. */
  /* The reserve refusals of the ROM's frames, read before the scenes:
     one scene spends the reserve on purpose and holds its own count. */
  k = sms.vdp.cnt_cels_refused;
  scenes();
  printf("decor windows %lu/%lu\n",win_ok,win_seen);
  printf("decor read=%lu limit=%lu gaps=%lu\n",decor_read,decor_limit,gaps);
  printf("decor journal=%lu bands=%lu\n",journal_total,bands_total);
  printf("decor scenes %lu/%lu\n",scene_ok,scene_want);
  printf("list cels %lu/%lu\n",cel_ok,cel_seen);
  printf("list cels max=%lu refused=%lu\n",cel_max,k);
#endif
  if(!writing)
    printf("sprite flags %lu/%lu\n",flags_ok,flags_seen);

  if(writing)
    printf("pictures=%lu written\n",pictures);
  else
    printf("pictures=%lu lines=%lu identical=%lu different=%lu\n",
           pictures,lines,identical,different);
  if(pictures == 0 || pictures != want_pictures || (!writing && lines == 0))
    {
      fprintf(stderr,"nothing taken, or not what the header says: the run proves nothing\n");
      if(writing) remove(tmp);
      return 2;
    }
  if(writing && rename(tmp,argv[5]) != 0)
    {
      fprintf(stderr,"cannot rename %s to %s\n",tmp,argv[5]);
      remove(tmp);
      return 2;
    }
  return (different != 0) ? 1 : 0;
}
