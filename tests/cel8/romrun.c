/* Host runner for the picture check: the real core on the real ROM.
 *
 * Boots src/cart.c, src/z80.c, src/sms.c and src/vdp.c as they stand, on
 * the ROM the console runs, and plays it frame by frame the way src/main.c
 * does -- 262 lines a frame, the processor's quota then the video part.
 * Every so many frames it takes the picture the cel would draw, one
 * palette index per pixel, 192 rows of 256, read as the row lies: byte 0
 * is pixel 0 on both machines, which is what the cel reads.
 *
 * With the background drawn by the cel engine (the delivered form,
 * src/common.h SMS_DECOR_CEL 1) there is no such buffer: the picture is
 * what a LIST of windows makes the engine draw, and this runner
 * reconstitutes it from the blocks the render hands out, field by field,
 * then lays the sprite layer over it -- see "the picture as the list
 * draws it" below. Three more lines come out of that, and the script
 * demands them: every window sound, no pixel left uncovered, no more
 * pixels read than the picture plus the alignment columns allow.
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
#else

/* ---- the picture as the LIST draws it: the windows, then the sprites ----
 *
 * With the background drawn by the cel engine (src/common.h,
 * SMS_DECOR_CEL), the render composes no background pixel: it keeps a
 * picture of the whole name table up to date and hands the frame loop,
 * band by band, a chain of cel control blocks that each draw a window of
 * that picture. What the console shows is what the engine reads off those
 * blocks, so that is what is reconstituted here: every field of every
 * block is held against what a sound window of the picture is, and the
 * window is then copied the way the engine copies it -- from the source
 * pointer, at the block's position, cut to the picture's area of the
 * screen (the clip rectangle src/main.c sets on every screen; the folio
 * takes (0,0) as its corner, docs/3do/3do_portfolio_2.5.md:10984), every
 * pixel painted, zero included (CCB_BGND). The sprite layer -- the index
 * buffer, zero where the background shows -- goes over it, every non-zero
 * pixel, as the sprite cel draws it without the background flag.
 *
 * Presented when line 191 has been counted, as src/main.c presents: the
 * writes of the blanking lines belong to the next picture, and the
 * reference encodes exactly that (line y shows the memory as it stood
 * after the processor's quota of line y).
 *
 * Counted over every frame played, not only the pictures taken: windows
 * seen and windows sound, pixels read against what a frame is allowed to
 * read -- the picture's area plus at most three alignment columns per
 * window, since a source pointer is a word address -- pixels no window
 * covered, journal entries and bands. */

/* How many windows each pixel of the picture's area received this frame. */
static unsigned char cover[PIC_BYTES];

static unsigned long win_seen = 0, win_ok = 0;
static unsigned long decor_read = 0, decor_limit = 0, gaps = 0;
static unsigned long journal_total = 0, bands_total = 0;

/* The first window found unsound, named once so that a reader knows which
   field to look at; the count is what the script judges. */
static void window_fault(const char *what, long frame, long band, long n)
{
  static int said = 0;
  if(said) return;
  said = 1;
  fprintf(stderr,"decor window unsound: %s (frame %ld band %ld window %ld)\n",
          what,frame,band,n);
}

/* One block of a band's chain: held against what the engine will read off
   it -- flags, preamble, source, size, position, 1:1 mapping, the
   identity palette -- then copied as the engine copies it. Returns 1 when
   every field is sound; a block whose source lies outside the picture is
   not copied at all, the engine would read what it should not. */
static int window(const CCB *c, long frame, long band, long n)
{
  const unsigned char *src = (const unsigned char *)c->ccb_SourcePtr;
  long off, w, h, x, y, px, py, r, i;
  int ok = 1;
  const char *why = NULL;

  w = (long)c->ccb_Width;
  h = (long)c->ccb_Height;
  off = (long)(src - sms.vdp.decor);

  if((c->ccb_Flags & CCB_BGND) == 0) why = "no CCB_BGND, zero would be transparent";
  else if((c->ccb_Flags & CCB_CCBPRE) == 0) why = "no CCB_CCBPRE, the preamble is not read";
  else if(n == 0 && (c->ccb_Flags & CCB_LDPLUT) == 0) why = "first window of the band without CCB_LDPLUT";
  else if(w < 1 || w > PIC_W + 3 || h < 1 || h > (long)VDP_DECOR_LINES) why = "size (a window is at most the width plus three alignment columns)";
  else if(c->ccb_PRE0 != (((uint32)(h - 1) << PRE0_VCNT_SHIFT) | PRE0_BPP_8)) why = "PRE0";
  else if(c->ccb_PRE1 != ((62UL << PRE1_WOFFSET10_SHIFT) | PRE1_TLLSB_PDC0 | (uint32)(w - 1))) why = "PRE1";
  else if(c->ccb_HDX != (1L << 20) || c->ccb_HDY != 0 || c->ccb_VDX != 0 || c->ccb_VDY != (1L << 16)) why = "not a 1:1 mapping";
  else if(c->ccb_PLUTPtr != (void *)sms.vdp.plut) why = "not the identity palette";
  else if((c->ccb_XPos % 65536L) != 0 || (c->ccb_YPos % 65536L) != 0) why = "position not on a pixel";
  if(why != NULL)
    {
      window_fault(why,frame,band,n);
      ok = 0;
    }

  if(src < sms.vdp.decor || off >= (long)VDP_DECOR_BYTES || (off % 4) != 0)
    {
      window_fault("source outside the picture or not a word address",frame,band,n);
      return 0;
    }
  px = off % PIC_W;
  py = off / PIC_W;
  if(px + w > PIC_W || py + h > (long)VDP_DECOR_LINES)
    {
      window_fault("source rectangle runs past the picture",frame,band,n);
      return 0;
    }

  x = (long)(c->ccb_XPos / 65536L);
  y = (long)(c->ccb_YPos / 65536L);
  if(x < -3 || x + w > PIC_W || y < 0 || y + h > PIC_H)
    {
      window_fault("position outside the picture's area",frame,band,n);
      ok = 0;
    }

  /* The copy, cut to the area: what lands at a column below zero is the
     alignment read, and the clip rectangle erases it on the console. */
  for(r = 0; r < h; r++)
    {
      long sy = y + r;
      if(sy < 0 || sy >= PIC_H) continue;
      for(i = 0; i < w; i++)
        {
          long sx = x + i;
          if(sx < 0 || sx >= PIC_W) continue;
          pic[sy * PIC_W + sx] = src[r * PIC_W + i];
          cover[sy * PIC_W + sx]++;
        }
    }
  decor_read += (unsigned long)(w * h);
  decor_limit += (unsigned long)(3 * h);
  return ok;
}

/* The bands of one presentation, between vdp_list_begin and vdp_list_end:
   each band's chain asked for, held and copied into pic. */
static void compose_bands(long frame, int32 bands)
{
  int32 k;
  const CCB *c;
  long n;

  /* What one presentation may read: the picture's area, plus the
     alignment columns each window adds below. */
  decor_limit += (unsigned long)PIC_BYTES;

  for(k = 0; k < bands; k++)
    {
      c = (const CCB *)vdp_list_band(k);
      if(c == NULL)
        {
          window_fault("band with no window",frame,(long)k,0);
          win_seen++;
          continue;
        }
      for(n = 0;; n++)
        {
          win_seen++;
          if(n >= (long)VDP_LIST_WINDOWS)
            {
              window_fault("chain longer than the arena",frame,(long)k,n);
              break;
            }
          if(window(c,frame,(long)k,n)) win_ok++;
          if(c->ccb_Flags & CCB_LAST) break;
          c = c->ccb_NextPtr;
          if(c == NULL)
            {
              window_fault("chain ends without CCB_LAST",frame,(long)k,n);
              win_seen++;
              break;
            }
        }
    }
}

static void present(long frame)
{
  int32 bands;
  long i;
  /* The sprite layer is read where the sprite cel reads it, off the
     block's source pointer: that the block points at the buffer the
     render writes is one of the things held here. */
  const unsigned char *spr = (const unsigned char *)((CCB *)vdp_cel())->ccb_SourcePtr;

  /* A pixel no window paints keeps this value, which no index can be:
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

  /* The sprite layer over the windows: every pixel that is not zero. */
  for(i = 0; i < PIC_BYTES; i++)
    if(spr[i] != 0) pic[i] = spr[i];
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

static void scenes(void)
{
  uint32 nt, addr, old, p, q, t, i, before, reg2, pa, pb;
  int32 bands;
  const CCB *c;
  long n;

  /* A known geometry: no lock, no scroll, the table where register 2
     left it. The control port's latch dropped first: the ROM may have
     stopped between the two bytes of a sequence. In the blanking, so
     that the register writes count as nothing. */
  sms.vdp.latch = 0;
  sms.vdp.vcount = 200;
  reg_write(0,(uint32)sms.vdp.reg[0] & ~0xC0UL);
  reg_write(8,0);
  reg_write(9,0);
  sms.vdp.hscroll = 0;
  sms.vdp.vscroll = 0;
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

  /* A sprite pattern written on line 70: nothing, no band. */
  q = free_pattern(0);
  expect(q != 0xFFFFFFFFUL,"a second pattern the picture does not show exists");
  if(q != 0xFFFFFFFFUL)
    {
      sms.vdp.vcount = 70;
      addr = q * 32;
      vram_write(addr,sms.vdp.vram[addr] ^ 0xFF);
      expect(sms.vdp.journal_count == 0,"sprite pattern written on line 70: not journaled");
      bands = vdp_list_begin();
      expect(bands == 1,"sprite pattern written on line 70: one band");
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
          vdp_line();
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

      if(writing)
        {
          fprintf(ref,"frame=%ld",fr);
          for(y = 0; y < PIC_H; y++)
            fprintf(ref," %08lx",row_digest[y]);
          fputc('\n',ref);
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
  scenes();
  printf("decor windows %lu/%lu\n",win_ok,win_seen);
  printf("decor read=%lu limit=%lu gaps=%lu\n",decor_read,decor_limit,gaps);
  printf("decor journal=%lu bands=%lu\n",journal_total,bands_total);
  printf("decor scenes %lu/%lu\n",scene_ok,scene_want);
#endif

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
