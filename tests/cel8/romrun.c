/* Host runner for the picture check: the real core on the real ROM.
 *
 * Boots src/cart.c, src/z80.c, src/sms.c and src/vdp.c as they stand, on
 * the ROM the console runs, and plays it frame by frame the way src/main.c
 * does -- 262 lines a frame, the processor's quota then the video part.
 * Every so many frames it takes the picture the cel would draw, one
 * palette index per pixel, 192 rows of 256, read as the row lies: byte 0
 * is pixel 0 on both machines, which is what the cel reads.
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
 * An optional directory takes one PPM per picture, the palette applied and
 * the border filled as the console shows it: the file the eye reads when a
 * figure disagrees with a screen.
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

static void take(void)
{
  CCB *c = (CCB *)vdp_cel();
  memcpy(pic,c->ccb_SourcePtr,(size_t)PIC_BYTES);
}

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
        }
      if((sms.vdp.reg[1] & 0x40U) == 0U) off_frames++;
      if((sms.vdp.reg[0] & 0x20U) != 0U) masked_frames++;
      fine_frames[sms.vdp.reg[8] & 7U]++;

      if((fr % every) != 0 && fr != frames - 1)
        continue;

      take();
      pictures++;
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
