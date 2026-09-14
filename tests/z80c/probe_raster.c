/* Host probe of the writes a ROM makes to the video part while the
 * picture is being counted: how many frames change what is shown in the
 * middle of the picture.
 *
 * Boots the core with the empty table (the interpreter alone) and plays
 * the ROM the way src/main.c does. The core is a COPY of src/, with two
 * hooks laid by tests/z80c/sweep.sh and nothing else changed: one as the
 * first statement of VDP_IO_DATA_WRITE (src/vdp.h), which calls
 * probe_note(0, address) for a colour write and probe_note(1, address)
 * for a video memory write, and one just before a register is stored in
 * vdp_reg_write (src/vdp.c), which calls probe_note(2, number | value << 8).
 * The line a write lands on is the line being counted, sms.vdp.vcount.
 *
 * A write counts as made during the picture when the line is one of the
 * 192 and the display is on (register 1, bit 6) -- or when the write is
 * the one that turns it on: a write of register 1 with bit 6 set, which
 * shows from that line on. It is VISIBLE when it
 * can change a row still to be shown: a register that draws (0, 1, 2, 5,
 * 6, 7, 8, 9), the name table, the sprite attribute table, or a pattern
 * that a name of the table or an entry of the sprite table uses at the
 * moment of the write. Colour writes are counted apart.
 *
 * Figures, one line on stdout, key=value:
 *
 *   frames              frames played
 *   picture_writes      writes made during the picture, of any kind
 *   midframe_visible    frames with at least one visible write during the
 *                       picture
 *   midframe_lines_max  most lines carrying a visible write in one frame
 *   midframe_cram       frames with a colour write during the picture
 *   hidden_patterns     pattern writes during the picture that no name
 *                       and no sprite used
 *   reg10_writes        writes of the line counter's register during
 *                       the picture
 *
 *   probe_raster <rom> <frames>
 *
 * Exit status: 0 the line printed, 2 the ROM did not boot or the core
 * stopped (named on stdout).
 */
#include "sms.h"
#include "cart.h"
#include "vdp.h"
#include "z80.h"
#include "z80c.h"
#include "log.h"
#include "host.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define LINES_PER_FRAME  262
#define TSTATES_PER_LINE 228
#define PRESENT_LINE     ((int)VDP_ACTIVE_LINES - 1)

void probe_note(int kind, unsigned long a);

static unsigned long picture_writes = 0, hidden_patterns = 0, reg10_writes = 0;
static unsigned char line_visible[VDP_ACTIVE_LINES];
static int frame_cram = 0;

/* Whether a pattern is used by a name of the table or by an entry of the
   sprite table as the video memory stands. */
static int pattern_used(unsigned long pattern)
{
  unsigned long nt = ((unsigned long)sms.vdp.reg[2] & 0x0EUL) << 10;
  unsigned long sat = ((unsigned long)sms.vdp.reg[5] & 0x7EUL) << 7;
  unsigned long base = (sms.vdp.reg[6] & 4U) ? 256UL : 0UL;
  int tall = (sms.vdp.reg[1] & 2U) != 0;
  unsigned long i;

  for(i = 0; i < 896UL; i++)
    {
      unsigned long w = (unsigned long)sms.vdp.vram[(nt + i * 2UL) & 0x3FFFUL] |
                        ((unsigned long)sms.vdp.vram[(nt + i * 2UL + 1UL) & 0x3FFFUL] << 8);
      if((w & 0x1FFUL) == pattern) return 1;
    }
  for(i = 0; i < 64UL; i++)
    {
      unsigned long p;
      if(sms.vdp.vram[(sat + i) & 0x3FFFUL] == 0xD0U) break;
      p = base + (unsigned long)sms.vdp.vram[(sat + 0x81UL + i * 2UL) & 0x3FFFUL];
      if(tall) p &= ~1UL;
      if(p == pattern || (tall && p + 1UL == pattern)) return 1;
    }
  return 0;
}

void probe_note(int kind, unsigned long a)
{
  unsigned long v = (unsigned long)sms.vdp.vcount;
  int visible = 0;
  int display_on = (sms.vdp.reg[1] & 0x40U) != 0U;

  /* A register write carries its number low and its value above. */
  if(kind == 2 && (a & 0xFFUL) == 1UL && ((a >> 8) & 0x40UL) != 0UL)
    display_on = 1;
  if(kind == 2)
    a &= 0xFFUL;
  if(v >= VDP_ACTIVE_LINES || !display_on)
    return;
  picture_writes++;
  if(kind == 0)
    {
      frame_cram = 1;
      return;
    }
  if(kind == 1)
    {
      unsigned long nt = ((unsigned long)sms.vdp.reg[2] & 0x0EUL) << 10;
      unsigned long sat = ((unsigned long)sms.vdp.reg[5] & 0x7EUL) << 7;
      a &= 0x3FFFUL;
      if((a >= nt && a < nt + 0x700UL) || (a >= sat && a < sat + 0x100UL))
        visible = 1;
      else if(pattern_used(a >> 5))
        visible = 1;
      else
        hidden_patterns++;
    }
  else
    {
      if(a == 10UL)
        reg10_writes++;
      else if(a <= 2UL || (a >= 5UL && a <= 9UL))
        visible = 1;
    }
  if(visible)
    line_visible[v] = 1;
}

int main(int argc, char **argv)
{
  long frames, fr;
  int line, y;
  int32 residue = 0;
  unsigned long vis_frames = 0, vis_max = 0, cram_frames = 0;

  if(argc != 3)
    {
      fprintf(stderr,"usage: probe_raster <rom> <frames>\n");
      return 2;
    }
  frames = host_count_arg(argv[2]);
  if(frames < 0)
    {
      fprintf(stderr,"frames must be a positive integer\n");
      return 2;
    }
  host_rom_path = argv[1];
  z80_init();
  if(cart_init() < 0 || cart_boot() < 0 || vdp_init() < 0)
    {
      printf("FAIL: %s did not boot\n",argv[1]);
      return 2;
    }
  z80_reset();
  z80c_init();
  host_booted = 1;

  for(fr = 0; fr < frames; fr++)
    {
      unsigned long lines = 0;

      memset(line_visible,0,sizeof line_visible);
      frame_cram = 0;
      for(line = 0; line < LINES_PER_FRAME; line++)
        {
          residue = z80_run((int32)TSTATES_PER_LINE - residue);
          vdp_line();
          if(line == PRESENT_LINE) host_present();
        }
      if(z80_is_stopped())
        {
          printf("FAIL: the core stopped at frame %lu\n",(unsigned long)fr);
          return 2;
        }
      for(y = 0; y < (int)VDP_ACTIVE_LINES; y++)
        if(line_visible[y]) lines++;
      if(lines != 0UL) vis_frames++;
      if(lines > vis_max) vis_max = lines;
      if(frame_cram) cram_frames++;
    }

  printf("frames=%ld picture_writes=%lu midframe_visible=%lu midframe_lines_max=%lu"
         " midframe_cram=%lu hidden_patterns=%lu reg10_writes=%lu\n",
         frames,picture_writes,vis_frames,vis_max,cram_frames,hidden_patterns,
         reg10_writes);
  return 0;
}
