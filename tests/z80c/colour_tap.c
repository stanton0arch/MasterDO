/* The colours and the memory of every frame, taken inside the picture
 * runner (tests/cel8/romrun.c) without a line of it changed about them:
 * this file is linked with the runner and -Wl,--wrap=vdp_line, so that
 * every call the runner makes to vdp_line comes here first
 * (tests/z80c/play.sh, z80c_build).
 *
 * The colours. The runner digests every row of a picture as palette
 * NUMBERS; the colour those numbers show is the colour memory. So, for
 * each of the 192 lines of the picture, the colour memory is folded as it
 * stands when the line is about to be counted -- the state the program
 * left during the line, which is the state the runner copies for that
 * line (romrun.c, line_step) -- and one digest per frame is written once
 * line 191 has been folded:
 *
 *   <frame> <digest, 8 hex digits>
 *
 * to the file named by Z80C_COLOURS. Every byte of the colour memory is
 * folded as written, the bits the display ignores included: the program
 * cannot read them back, but a translation that writes them otherwise is
 * not writing what the interpreter wrote.
 *
 * The memory. At the end of every frame -- the last line of the frame
 * about to be counted, the program's part of the frame done -- one digest
 * of what the program keeps is written, in the same form, to the file
 * named by Z80C_MEMORY: the 8k of work RAM read through the address space,
 * the cartridge RAM when there is one, every register of the processor
 * but the T-state counter (the halt, the flip-flops, the interrupt mode
 * and the refresh register with them), the memory control register and
 * the four registers of the mapper, and of the video part its registers
 * and the state the program writes or reads back: the address, the code,
 * the control word and its latch, the read buffer, the line interrupt
 * counter, the two interrupt requests, the two sprite bits of the status
 * register and the two scroll latches. Not the line count, which the
 * frame loop drives the same on both sides, nor the video and colour
 * memories, which the pictures judge.
 *
 * Without either variable, nothing of it is written and the runner
 * behaves as it always did.
 *
 * A core that stops (an opcode it does not implement) spends every line
 * without moving, and its picture stands still: once line 191 of a frame
 * is folded with the core stopped, the line
 *
 *   stopped <frame>
 *
 * is written once to the colours, and the scripts refuse the take.
 *
 * The runner calls vdp_line again in its synthetic scenes after the
 * frames of the ROM; the records they add come after the frames' and are
 * not read.
 */
#include "sms.h"
#include "cart.h"
#include "vdp.h"
#include "z80.h"
#include <stdio.h>
#include <stdlib.h>

void __real_vdp_line(void);
void __wrap_vdp_line(void);

static FILE *colours = NULL;
static FILE *memory = NULL;
static int opened = 0;
static int stopped_said = 0;
static long frame = 0;
static long mframe = 0;
static unsigned long h = 2166136261UL;

static unsigned long fold(unsigned long d, const unsigned char *p, unsigned long n)
{
  unsigned long i;

  for(i = 0; i < n; i++)
    {
      d ^= (unsigned long)p[i];
      d *= 16777619UL;
      d &= 0xFFFFFFFFUL;
    }
  return d;
}

static unsigned long fold_u32(unsigned long d, unsigned long v)
{
  unsigned char b[4];

  b[0] = (unsigned char)(v & 0xFFUL);
  b[1] = (unsigned char)((v >> 8) & 0xFFUL);
  b[2] = (unsigned char)((v >> 16) & 0xFFUL);
  b[3] = (unsigned char)((v >> 24) & 0xFFUL);
  return fold(d,b,4);
}

/* What the program keeps, as the head of this file lists it. */
static unsigned long memory_digest(void)
{
  unsigned long d = 2166136261UL;
  unsigned long a;
  const z80_t *z = &sms.z80;
  unsigned char r[16];

  for(a = 0xC000UL; a < 0xE000UL; a++)
    {
      const unsigned char *page = z80_rmap[a >> Z80_PAGE_BITS];

      d = fold(d,page + (a & Z80_PAGE_MASK),1);
    }
  if(sms.cart.cart_ram != NULL)
    d = fold(d,sms.cart.cart_ram,(unsigned long)CART_RAM_SIZE);

  d = fold(d,(const unsigned char *)&z->main,sizeof z->main);
  d = fold(d,(const unsigned char *)&z->alt,sizeof z->alt);
  r[0] = (unsigned char)(z->pc & 0xFF);
  r[1] = (unsigned char)(z->pc >> 8);
  r[2] = (unsigned char)(z->sp & 0xFF);
  r[3] = (unsigned char)(z->sp >> 8);
  r[4] = z->ixh;
  r[5] = z->ixl;
  r[6] = z->iyh;
  r[7] = z->iyl;
  r[8] = z->i;
  r[9] = z->r;
  r[10] = z->iff1;
  r[11] = z->iff2;
  r[12] = z->ei_delay;
  r[13] = z->im;
  r[14] = z->halted;
  r[15] = z->nmi_seen;
  d = fold(d,r,sizeof r);

  d = fold_u32(d,(unsigned long)sms.cart.memctl);
  d = fold_u32(d,(unsigned long)sms.cart.mapper_fffc);
  d = fold_u32(d,(unsigned long)sms.cart.mapper_fffd);
  d = fold_u32(d,(unsigned long)sms.cart.mapper_fffe);
  d = fold_u32(d,(unsigned long)sms.cart.mapper_ffff);

  d = fold(d,sms.vdp.reg,(unsigned long)VDP_REG_COUNT);
  d = fold_u32(d,(unsigned long)sms.vdp.addr);
  d = fold_u32(d,(unsigned long)sms.vdp.code);
  d = fold_u32(d,(unsigned long)sms.vdp.ctrl_word);
  d = fold_u32(d,(unsigned long)sms.vdp.latch);
  d = fold_u32(d,(unsigned long)sms.vdp.read_buf);
  d = fold_u32(d,(unsigned long)sms.vdp.line_ctr);
  d = fold_u32(d,(unsigned long)sms.vdp.frame_pending);
  d = fold_u32(d,(unsigned long)sms.vdp.line_pending);
  d = fold_u32(d,(unsigned long)sms.vdp.spr_overflow);
  d = fold_u32(d,(unsigned long)sms.vdp.spr_collision);
  d = fold_u32(d,(unsigned long)sms.vdp.vscroll);
  d = fold_u32(d,(unsigned long)sms.vdp.hscroll);
  return d;
}

static void files_close(void)
{
  if(colours != NULL && (ferror(colours) || fclose(colours) != 0))
    fprintf(stderr,"cannot finish the colours of the frames\n");
  if(memory != NULL && (ferror(memory) || fclose(memory) != 0))
    fprintf(stderr,"cannot finish the memory of the frames\n");
}

static FILE *open_named(const char *var)
{
  const char *path = getenv(var);
  FILE *f;

  if(path == NULL || *path == '\0')
    return NULL;
  f = fopen(path,"w");
  if(f == NULL)
    {
      fprintf(stderr,"cannot write %s %s\n",var,path);
      exit(2);
    }
  return f;
}

void __wrap_vdp_line(void)
{
  if(!opened)
    {
      opened = 1;
      colours = open_named("Z80C_COLOURS");
      memory = open_named("Z80C_MEMORY");
      if(colours != NULL || memory != NULL)
        atexit(files_close);
    }
  if(colours != NULL && sms.vdp.vcount < VDP_ACTIVE_LINES)
    {
      h = fold(h,sms.vdp.cram,(unsigned long)VDP_CRAM_SIZE);
      if(sms.vdp.vcount == VDP_ACTIVE_LINES - 1UL)
        {
          fprintf(colours,"%ld %08lx\n",frame,h);
          if(!stopped_said && z80_is_stopped())
            {
              fprintf(colours,"stopped %ld\n",frame);
              stopped_said = 1;
            }
          frame++;
          h = 2166136261UL;
        }
    }
  if(memory != NULL && sms.vdp.vcount == VDP_LINES_PER_FRAME - 1UL)
    {
      fprintf(memory,"%ld %08lx\n",mframe,memory_digest());
      mframe++;
    }
  __real_vdp_line();
}
