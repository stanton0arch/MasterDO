/* Host probe of the code a ROM runs: how much of the cartridge, how much
 * from RAM, how much of the time waiting, and whether the program ever
 * falls behind its frame or looks at the line it is on.
 *
 * Boots the core as it stands with the empty table (the interpreter
 * alone) and plays the ROM the way src/main.c does -- 262 lines, 228
 * T-states a line less the overrun of the line before, the video part,
 * the presentation once line 191 is counted -- but one instruction at a
 * time, z80_run(1), so that every instruction is seen where it starts.
 *
 * Figures, one line on stdout, key=value:
 *
 *   frames            frames played
 *   insns_per_frame   instructions executed per frame, mean
 *   cart_starts       distinct positions of the image an instruction
 *                     started at
 *   cart_pct          cart_starts times two over the image's size: the
 *                     share of the cartridge that ran, at two bytes an
 *                     instruction (an estimate: the probe decodes no
 *                     length)
 *   ram_insns         instructions that started outside the image (work
 *                     RAM, cartridge RAM), and ram_insns_pct their share
 *   ram_time_pct      the share of the T-states they spent
 *   loop_time_pct     the share of the T-states spent in short loops: an
 *                     instruction whose position is the position of one
 *                     of the 2 to 6 instructions before it
 *   halt_time_pct     the share of the T-states spent halted
 *   nowait_frames     frames in which no T-state was spent halted nor in
 *                     a short loop: the program was at work from the
 *                     first line to the last
 *   irq_frame         frame interrupts accepted
 *   irq_busy          of those, accepted while the program was neither
 *                     halted nor in a short loop: it had not finished its
 *                     frame when the next one came
 *   irq_line          interrupts accepted while the line request was up
 *                     and enabled (register 0, bit 4)
 *   vcount_reads      reads of the V counter port made while one of the
 *                     192 lines of the picture was being counted, and
 *                     vcount_frames the frames that carry one
 *
 * A short loop is a shape, not an intent: a copy loop of two to six
 * instructions counts as one, and a wait written longer than six does
 * not. The figures are for reading a ROM's profile, not for judging it.
 *
 *   probe_code <rom> <frames>
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

#if !(LOG_ENABLE && SMS_TELEMETRY)
#error "probe_code needs the telemetry: the accepted interrupts are counted under it"
#endif

#define LINES_PER_FRAME  262
#define TSTATES_PER_LINE 228
#define PRESENT_LINE     ((int)VDP_ACTIVE_LINES - 1)
#define HISTORY          8

/* The share of a whole, in tenths of a percent, printed as nn.n. */
static void pct(const char *key, unsigned long part, unsigned long whole)
{
  unsigned long t = (whole != 0UL) ? (unsigned long)((1000.0 * part) / whole) : 0UL;
  printf(" %s=%lu.%lu",key,t / 10UL,t % 10UL);
}

int main(int argc, char **argv)
{
  long frames, fr;
  int line;
  unsigned char *seen;
  unsigned long starts = 0, insns = 0, ram_insns = 0;
  unsigned long ts_total = 0, ts_ram = 0, ts_loop = 0, ts_halt = 0;
  unsigned long nowait = 0, irq_frame = 0, irq_busy = 0, irq_line = 0;
  unsigned long vc_reads = 0, vc_frames = 0;
  unsigned long hist[HISTORY];
  unsigned hi = 0;
  int waiting = 0;
  long over = 0;

  if(argc != 3)
    {
      fprintf(stderr,"usage: probe_code <rom> <frames>\n");
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

  seen = (unsigned char *)calloc((size_t)sms.cart.size,1);
  if(seen == NULL)
    {
      fprintf(stderr,"no memory for the map of the image\n");
      return 2;
    }
  memset(hist,0xFF,sizeof hist);

  for(fr = 0; fr < frames; fr++)
    {
      unsigned long wait_f = 0, vc_f = 0;

      for(line = 0; line < LINES_PER_FRAME; line++)
        {
          long budget = (long)TSTATES_PER_LINE - over, t = 0;

          while(t < budget)
            {
              unsigned long pc = (unsigned long)z80_pc();
              int halted = sms.z80.halted != 0;
              int line_up = sms.vdp.line_pending && (sms.vdp.reg[0] & 0x10U);
              int frame_up = sms.vdp.frame_pending && (sms.vdp.reg[1] & 0x20U);
              unsigned long irq0 = (unsigned long)sms.z80.irq_accepted;
              unsigned long vc0 = (unsigned long)sms.vdp.cnt_vcnt_r;
              const uint8 *page = z80_rmap[pc >> Z80_PAGE_BITS];
              long spent;
              unsigned long cost;
              unsigned k;
              int in_loop = 0;
              long off;

              /* A quota of one hands back the overrun past it: the
                 instruction cost one more than that. A core that spent
                 nothing, or stopped, would never move this loop on. */
              spent = 1L + (long)z80_run(1);
              if(spent <= 0L || z80_is_stopped())
                {
                  printf("FAIL: the core stopped at frame %lu line %d\n",
                         (unsigned long)fr,line);
                  free(seen);
                  return 2;
                }
              cost = (unsigned long)spent;
              t += spent;
              ts_total += cost;

              if((unsigned long)sms.z80.irq_accepted != irq0)
                {
                  /* An acceptance, not an instruction: the vector is
                     taken, and whether the program was waiting is what
                     the instructions before said. */
                  if(frame_up)
                    {
                      irq_frame++;
                      if(!waiting) irq_busy++;
                    }
                  if(line_up)
                    irq_line++;
                  continue;
                }
              if(halted)
                {
                  ts_halt += cost;
                  wait_f += cost;
                  waiting = 1;
                  continue;
                }

              insns++;
              /* The page decides, as src/z80c.c (z80c_find) decides: the
                 difference from the image taken as a signed offset and
                 bounded by the loaded size. */
              off = (long)(page - sms.cart.rom);
              if(off >= 0L && (unsigned long)off < (unsigned long)sms.cart.size)
                {
                  unsigned long at = (unsigned long)off + (pc & (Z80_PAGE_SIZE - 1UL));
                  if(at < (unsigned long)sms.cart.size && !seen[at])
                    {
                      seen[at] = 1;
                      starts++;
                    }
                }
              else
                {
                  ram_insns++;
                  ts_ram += cost;
                }
              for(k = 2; k <= 6; k++)
                if(hist[(hi - k) % HISTORY] == pc)
                  {
                    in_loop = 1;
                    break;
                  }
              if(in_loop)
                {
                  ts_loop += cost;
                  wait_f += cost;
                }
              waiting = in_loop;
              hist[hi % HISTORY] = pc;
              hi++;

              if((unsigned long)sms.vdp.cnt_vcnt_r != vc0 &&
                 sms.vdp.vcount < VDP_ACTIVE_LINES)
                {
                  vc_reads++;
                  vc_f++;
                }
            }
          over = t - budget;

          vdp_line();
          if(line == PRESENT_LINE) host_present();
        }
      if(z80_is_stopped())
        {
          printf("FAIL: the core stopped at frame %lu\n",(unsigned long)fr);
          return 2;
        }
      if(wait_f == 0UL) nowait++;
      if(vc_f != 0UL) vc_frames++;
    }

  printf("frames=%ld insns_per_frame=%lu cart_starts=%lu",
         frames,insns / (unsigned long)frames,starts);
  pct("cart_pct",starts * 2UL,(unsigned long)sms.cart.size);
  printf(" ram_insns=%lu",ram_insns);
  pct("ram_insns_pct",ram_insns,insns);
  pct("ram_time_pct",ts_ram,ts_total);
  pct("loop_time_pct",ts_loop,ts_total);
  pct("halt_time_pct",ts_halt,ts_total);
  printf(" nowait_frames=%lu irq_frame=%lu irq_busy=%lu irq_line=%lu"
         " vcount_reads=%lu vcount_frames=%lu\n",
         nowait,irq_frame,irq_busy,irq_line,vc_reads,vc_frames);
  free(seen);
  return 0;
}
