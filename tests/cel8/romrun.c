/* Host runner for the cel depth probe: the real core on the real ROM.
 *
 * Boots src/cart.c, src/z80.c, src/sms.c and src/vdp.c as they stand, on
 * the ROM the console runs, and plays it frame by frame the way src/main.c
 * does -- 262 lines a frame, the processor's quota then the video part.
 * Every so many frames it takes the picture the cel would draw, as one
 * palette index per pixel, 192 rows of 256:
 *
 *   built without the switch, the six bit row is decoded the way
 *   vdp_pack_row defines it, sixteen indexes in three words, MSB first;
 *   built with -DSMS_CEL_BPP8=1, the eight bit row is read as it lies --
 *   byte 0 is pixel 0 on both machines, which is what the cel reads.
 *
 * Two modes. "write" stores the pictures in a file; "compare" plays the same
 * frames in the other build and holds each row against that file. The two
 * compositions of one line must be the same 256 bytes: that is the whole
 * claim the probe rests on, and it is held here on a real screen rather
 * than on a scene the bench made up. A row that differs is a failure. A
 * row that is flat where the reference is not is counted apart, because
 * that is what an earlier form of the probe did to a line with sprites on
 * it, and it must now be zero.
 *
 * An optional directory takes one PPM per picture, the palette applied and
 * the border filled as the console shows it: the file the eye reads when a
 * figure disagrees with a screen.
 *
 *   romrun <rom> <frames> <every> write|compare <raw file> [ppm dir]
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
#include "mem.h"
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

#define LINES_PER_FRAME  262
#define TSTATES_PER_LINE 228

/* The picture is the render's, not a size of this file's own; and the eight
   bit row is read as one unpadded line of it, which vdp.h computes but this
   file pins, so that a padded row would refuse here rather than compare
   bytes of padding against pixels. */
#define PIC_W ((int)VDP_PIX_WIDTH)
#define PIC_H ((int)VDP_ACTIVE_LINES)
#define PIC_BYTES (PIC_W * PIC_H)
#if VDP_CEL8_ROW_BYTES != VDP_PIX_WIDTH
#error "the eight bit row is read as an unpadded line: it no longer is one"
#endif

/* The raw file: four words of header, then PIC_BYTES per picture. A
   reference written with other parameters, or by the other depth, is refused
   by the header rather than compared on a common prefix. */
#define RAW_MAGIC 0x43454C38UL   /* "CEL8" */

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
void AvailMem(MemInfo *mi, uint32 memtype)
{ (void)memtype; memset(mi,0,sizeof *mi); }

/* One block per call: the probe builds two cels and keeps both. The two
   preamble words are the library's, as vdp.c's own arbiter expects them. */
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

/* ---- the picture, one index per pixel ---- */

static unsigned char pic[PIC_BYTES];

#if !SMS_CEL_BPP8
/* Sixteen indexes in three words, MSB first: the format vdp_pack_row
   defines (src/vdp.c). Read as a 96 bit number to keep one rule. */
static void take_six(void)
{
  unsigned y, g, i;
  for(y = 0; y < PIC_H; y++)
    {
      const uint32 *w = (const uint32 *)(sms.vdp.pixels[0] + (y * VDP_PIX_ROW_BYTES));
      unsigned char *row = pic + (y * PIC_W);
      for(g = 0; g < PIC_W / 16; g++)
        {
          unsigned long long hi = ((unsigned long long)w[0] << 32) | w[1];
          unsigned long long lo = w[2];
          for(i = 0; i < 16; i++)
            {
              int bit = 90 - 6 * (int)i;   /* lowest bit of the field */
              unsigned idx;
              if(bit >= 32)
                idx = (unsigned)((hi >> (bit - 32)) & 63);
              else if(bit + 6 <= 32)
                idx = (unsigned)((lo >> bit) & 63);
              else
                idx = (unsigned)((((hi << 32) | lo) >> bit) & 63);
              row[g * 16 + i] = (unsigned char)idx;
            }
          w += 3;
        }
    }
}
#else
/* The eight bit buffer as the cel reads it: byte 0 is pixel 0. */
static void take_eight(void)
{
  CCB *c = (CCB *)vdp_cel();
  memcpy(pic,c->ccb_SourcePtr,(size_t)PIC_BYTES);
}
#endif

static void take(void)
{
#if SMS_CEL_BPP8
  take_eight();
#else
  take_six();
#endif
}

/* Whether the probe is on, asked once after init rather than after 1200
   frames: a probe that fell back draws the six bit cel, and comparing that
   would compare the delivered build with itself. */
static int probe_is_on(void)
{
  CCB *c = (CCB *)vdp_cel();
  return c->ccb_SourcePtr != (CelData *)sms.vdp.pixels[0];
}

/* ---- PPM, the screen as the console shows it ---- */

static unsigned char screen[240][320][3];

static void rgb555(uint16 c, unsigned char *p)
{
  p[0] = (unsigned char)(((c >> 10) & 31) * 255 / 31);
  p[1] = (unsigned char)(((c >> 5) & 31) * 255 / 31);
  p[2] = (unsigned char)((c & 31) * 255 / 31);
}

static void ppm(const char *dir, long frame)
{
  char path[512];
  unsigned char b[3];
  int x, y;
  FILE *f;
  int n;

  rgb555(vdp_backdrop(),b);
  for(y = 0; y < 240; y++)
    for(x = 0; x < 320; x++)
      memcpy(screen[y][x],b,3);
  for(y = 0; y < PIC_H; y++)
    for(x = 0; x < PIC_W; x++)
      {
        unsigned idx = pic[y * PIC_W + x];
        if(idx < VDP_PLUT_ENTRIES)
          rgb555(sms.vdp.plut[idx],screen[24 + y][32 + x]);
        else
          { screen[24 + y][32 + x][0] = 255; screen[24 + y][32 + x][1] = 0;
            screen[24 + y][32 + x][2] = 255; }
      }
  n = snprintf(path,sizeof path,"%s/f%05ld_%d.ppm",dir,frame,SMS_CEL_BPP8 ? 8 : 6);
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

/* ---- the raw file header ---- */

static void put_word(FILE *f, unsigned long w)
{
  unsigned char b[4];
  b[0] = (unsigned char)(w >> 24); b[1] = (unsigned char)(w >> 16);
  b[2] = (unsigned char)(w >> 8);  b[3] = (unsigned char)w;
  fwrite(b,1,4,f);
}

static int get_word(FILE *f, unsigned long *w)
{
  unsigned char b[4];
  if(fread(b,1,4,f) != 4) return 0;
  *w = ((unsigned long)b[0] << 24) | ((unsigned long)b[1] << 16)
     | ((unsigned long)b[2] << 8) | b[3];
  return 1;
}

/* ---- main ---- */

static int row_flat(const unsigned char *r)
{
  int x;
  for(x = 1; x < PIC_W; x++)
    if(r[x] != r[0]) return 0;
  return 1;
}

int main(int argc, char **argv)
{
  long frames, every, fr;
  int writing;
  FILE *raw;
  const char *ppmdir;
  int line;
  int32 residue = 0;
  unsigned long pictures = 0, lines = 0, identical = 0, different = 0, flat = 0;
  /* What the run exercised, per frame, so the figures say what they cover:
     frames with the picture off, with the left column masked, and with each
     of the eight fine scrolls (video registers 1, 0 and 8). */
  unsigned long off_frames = 0, masked_frames = 0, fine_frames[8];
  unsigned long w;
  static unsigned char ref[PIC_BYTES];

  memset(fine_frames,0,sizeof fine_frames);

  if(argc < 6)
    {
      fprintf(stderr,"usage: romrun <rom> <frames> <every> write|compare <raw> [ppm dir]\n");
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
  raw = fopen(argv[5],writing ? "wb" : "rb");
  ppmdir = (argc > 6) ? argv[6] : NULL;
  if(raw == NULL)
    {
      fprintf(stderr,"cannot open %s\n",argv[5]);
      return 2;
    }

  z80_init();
  if(cart_init() < 0) return 2;
  if(cart_boot() < 0) return 2;
  if(vdp_init() < 0) return 2;
  z80_reset();
  booted = 1;

#if SMS_CEL_BPP8
  if(!probe_is_on())
    {
      fprintf(stderr,"the probe is off: nothing to compare\n");
      return 2;
    }
#endif

  if(writing)
    {
      put_word(raw,RAW_MAGIC);
      put_word(raw,(unsigned long)frames);
      put_word(raw,(unsigned long)every);
      put_word(raw,(unsigned long)PIC_BYTES);
    }
  else
    {
      unsigned long h[4];
      if(!get_word(raw,&h[0]) || !get_word(raw,&h[1])
         || !get_word(raw,&h[2]) || !get_word(raw,&h[3])
         || h[0] != RAW_MAGIC || h[1] != (unsigned long)frames
         || h[2] != (unsigned long)every || h[3] != (unsigned long)PIC_BYTES)
        {
          fprintf(stderr,"the reference was not written for %ld frames every %ld\n",
                  frames,every);
          fclose(raw);
          return 2;
        }
    }

  for(fr = 0; fr < frames; fr++)
    {
      for(line = 0; line < LINES_PER_FRAME; line++)
        {
          residue = z80_run(TSTATES_PER_LINE - residue);
          vdp_line();
        }
      if((sms.vdp.reg[1] & 0x40U) == 0U) off_frames++;
      if((sms.vdp.reg[0] & 0x20U) != 0U) masked_frames++;
      fine_frames[sms.vdp.reg[8] & 7U]++;

      if((fr % every) != 0 && fr != frames - 1)
        continue;

      take();
      pictures++;
      if(ppmdir != NULL) ppm(ppmdir,fr);

      if(writing)
        {
          if(fwrite(pic,1,(size_t)PIC_BYTES,raw) != (size_t)PIC_BYTES)
            {
              fprintf(stderr,"short write on the reference at frame %ld\n",fr);
              fclose(raw);
              return 2;
            }
        }
      else
        {
          int y;
          if(fread(ref,1,(size_t)PIC_BYTES,raw) != (size_t)PIC_BYTES)
            {
              fprintf(stderr,"the reference ends before frame %ld\n",fr);
              fclose(raw);
              return 2;
            }
          for(y = 0; y < PIC_H; y++)
            {
              const unsigned char *a = pic + y * PIC_W;
              const unsigned char *b = ref + y * PIC_W;
              lines++;
              if(memcmp(a,b,(size_t)PIC_W) == 0)
                identical++;
              else
                {
                  different++;
                  if(row_flat(a) && !row_flat(b)) flat++;
                  if(different <= 8)
                    fprintf(stderr,"  frame %ld line %d differs\n",fr,y);
                }
            }
        }
    }

  if(!writing && fgetc(raw) != EOF)
    {
      fprintf(stderr,"the reference holds more pictures than this run took\n");
      fclose(raw);
      return 2;
    }
  if(fclose(raw) != 0)
    {
      fprintf(stderr,"cannot close %s\n",argv[5]);
      return 2;
    }

  printf("covered frames=%lu off=%lu masked=%lu fine=",
         (unsigned long)frames,off_frames,masked_frames);
  for(w = 0; w < 8; w++)
    printf("%s%lu",(w != 0) ? "/" : "",fine_frames[w]);
  printf("\n");

  if(writing)
    printf("pictures=%lu written\n",pictures);
  else
    printf("pictures=%lu lines=%lu identical=%lu different=%lu flat=%lu\n",
           pictures,lines,identical,different,flat);
  if(pictures == 0 || (!writing && lines == 0))
    {
      fprintf(stderr,"nothing taken: the run proves nothing\n");
      return 2;
    }
  return (different != 0) ? 1 : 0;
}
