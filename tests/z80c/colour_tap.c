/* The colours of every frame, taken inside the picture runner
 * (tests/cel8/romrun.c) without a line of it changed: this file is linked
 * with the runner and -Wl,--wrap=vdp_line, so that every call the runner
 * makes to vdp_line comes here first (tests/z80c/play.sh, z80c_build).
 *
 * The runner digests every row of a picture as palette NUMBERS; the
 * colour those numbers show is the colour memory. So, for each of the 192
 * lines of the picture, the colour memory is folded as it stands when the
 * line is about to be counted -- the state the program left during the
 * line's quota, which is the state the runner copies for that line
 * (romrun.c, line_step) -- and one digest per frame is written once line
 * 191 has been folded:
 *
 *   <frame> <digest, 8 hex digits>
 *
 * to the file named by Z80C_COLOURS. Without the variable nothing is
 * written and the runner behaves as it always did. Every byte of the
 * colour memory is folded as written, the bits the display ignores
 * included: the program cannot read them back, but a translation that
 * writes them otherwise is not writing what the interpreter wrote.
 *
 * A core that stops (an opcode it does not implement) spends every quota
 * without moving, and its picture stands still: once line 191 of a frame
 * is folded with the core stopped, the line
 *
 *   stopped <frame>
 *
 * is written once, and the scripts refuse the take.
 *
 * The runner calls vdp_line again in its synthetic scenes after the
 * frames of the ROM; the records they add come after the frames' and are
 * not read.
 */
#include "sms.h"
#include "vdp.h"
#include "z80.h"
#include <stdio.h>
#include <stdlib.h>

void __real_vdp_line(void);
void __wrap_vdp_line(void);

static FILE *colours = NULL;
static int opened = 0;
static int stopped_said = 0;
static long frame = 0;
static unsigned long h = 2166136261UL;

static void colours_close(void)
{
  if(colours != NULL && (ferror(colours) || fclose(colours) != 0))
    fprintf(stderr,"cannot finish the colours of the frames\n");
}

void __wrap_vdp_line(void)
{
  if(!opened)
    {
      const char *path = getenv("Z80C_COLOURS");

      opened = 1;
      if(path != NULL && *path != '\0')
        {
          colours = fopen(path,"w");
          if(colours == NULL)
            {
              fprintf(stderr,"cannot write the colours %s\n",path);
              exit(2);
            }
          atexit(colours_close);
        }
    }
  if(colours != NULL && sms.vdp.vcount < VDP_ACTIVE_LINES)
    {
      unsigned long i;

      for(i = 0; i < (unsigned long)VDP_CRAM_SIZE; i++)
        {
          h ^= (unsigned long)sms.vdp.cram[i];
          h *= 16777619UL;
          h &= 0xFFFFFFFFUL;
        }
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
  __real_vdp_line();
}
