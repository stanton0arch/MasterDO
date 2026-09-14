/* Host runner of the check of the translated code: the pictures the
 * translated code draws held against the pictures the interpreter draws,
 * the two machines running free.
 *
 * Three modes.
 *
 *   translated   boots src/cart.c, src/z80.c, src/z80c.c, src/sms.c and
 *                src/vdp.c as they stand, with a generated table of
 *                translated code linked, on the ROM named, and plays it
 *                frame by frame the way src/main.c does -- 262 lines a
 *                frame, a quota of 228 T-states less the residue of the
 *                line before, then the video part, the presentation once
 *                line 191 is counted -- without drawing anything. It
 *                compares nothing: it refuses a table that did not pair
 *                with the ROM, a run in which no block ran and a run in
 *                which no chain ran a second block; it prints the counters
 *                of the translated code; and, when a last argument names a
 *                file, it writes the hits of every block there (one line
 *                per block: position, how often it was entered, the
 *                T-states its exits charged; after a header naming the
 *                frames and the T-states the run spent) -- what the
 *                translator chooses a table under a budget from.
 *
 *   romid        boots the ROM and prints "rom_bytes=<n> rom_fnv=<8 hex>",
 *                the two fields the picture runner writes in a take's
 *                header: how a script checks that a take it kept is this
 *                ROM's.
 *
 *   pictures     two takes of the picture runner (tests/cel8/romrun.c, in
 *                write mode, one picture every frame), the translated run's
 *                first and the interpreter's second, each a digest per row
 *                of the palette numbers of every row of every frame, and
 *                beside each the colours of every frame (tests/z80c/
 *                colour_tap.c: the colour memory as each line of the
 *                picture was counted). A translated frame MATCHES an
 *                interpreter's frame when every row and the colours are
 *                the same. Every frame n of the translated take must match
 *                the interpreter's frame n, n-1 or n+1 -- tried in that
 *                order -- and the interpreter's frames matched must never
 *                go backwards from one translated frame to the next: the
 *                same frame twice, or a frame skipped, is a shift; an
 *                earlier frame than the last one matched is not. Prints one
 *                line per frame that matches none of the allowed
 *                neighbours:
 *
 *                  unmatched frame <n> from=<first allowed> rows=<rows> [colours=differ]
 *
 *                with the rows that differ from the frame of the same rank,
 *                then the counts. Such a frame does not move the last
 *                frame matched. The frames named are not a verdict: a row
 *                digest holds palette NUMBERS, and two numbers may carry
 *                the same colour. tests/z80c/play.sh redraws each of them
 *                in colour on both sides and compares the screens byte for
 *                byte, and the colours; only a frame that differs there is
 *                a mismatch.
 *
 * Why one frame of shift and no more. Without an account of time shared by
 * the two runs, the frame interrupt may land just before or just after the
 * end of a frame's work, on one side and not the other: the program then
 * shows the same screen one frame earlier or later. With the translated
 * code judged here, which still counts T-states, more than one frame would
 * mean the program has fallen behind, which shows on the screen.
 *
 * What this check proves: on every frame played, the screen drawn with the
 * translated code -- rows and colours -- is the screen the interpreter drew
 * on that frame or on a neighbouring one, in the interpreter's order,
 * pixel for pixel. What it does not prove:
 *
 *   - the internal memory of the program: two runs going free end a frame
 *     on different instructions, and the interpreter against itself, one
 *     T-state apart, already leaves different memory behind;
 *   - the code that never runs without a pad: menus, later levels, a game
 *     really played;
 *   - that a translation without an account of time will draw the same
 *     pictures. Such a translation runs the program as fast as it waits
 *     and takes its interrupts as events: a frame the interpreter's
 *     program did not finish in time (a lag frame) may be finished by it,
 *     and the two may then legitimately drift apart by more than one frame.
 *     This rule of one frame will have to be revisited then, not relaxed
 *     in silence;
 *   - the T-states the emitted C charges. They are no longer held exactly:
 *     a sum off by one moves the interrupts, which may or may not move a
 *     picture by more than a frame, and the mutations of the time alone
 *     (tests/z80c/run_z80c.sh) are printed for information, never demanded.
 *
 *   sidebyside <rom> <frames> translated [counts]
 *   sidebyside <rom> romid
 *   sidebyside pictures <translated take> <its colours> <interpreter take> <its colours>
 *
 * Exit status: 0 the run went through, or every frame matched a
 * neighbour; 1 some frame matched none (the frames are named), or the
 * core stopped with translated code armed; 2 nothing could be proved
 * (arguments, boot, overrun of the quota, a chain never followed, no
 * block run, takes unreadable or not of one run, the interpreter's core
 * stopped); 3 the table is not this ROM's.
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

/* The counters this runner writes exist in the telemetry build alone;
   the script pins the switches, and a build that lost one must fail here
   rather than write zeroes. */
#if !LOG_ENABLE
#error "sidebyside needs LOG_ENABLE: the counters of the translated code live under it"
#endif
#if !SMS_TELEMETRY
#error "sidebyside needs SMS_TELEMETRY=1: z80c_counts lives under it"
#endif
#if !Z80C_HITS
#error "sidebyside needs Z80C_HITS=1: the hits per block live under it"
#endif

#define LINES_PER_FRAME  262
#define TSTATES_PER_LINE 228

/* The line the console presents the picture on: once line 191 has been
   counted, src/main.c builds the list. */
#define PRESENT_LINE ((int)VDP_ACTIVE_LINES - 1)

/* The rows of a picture, and their width, as the picture runner digests
   them (tests/cel8/romrun.c, PIC_H and PIC_W). */
#define ROWS  ((int)VDP_ACTIVE_LINES)
#define WIDTH 256L

/* ---- pictures ---- */

/* The header line of a take (tests/cel8/romrun.c, ref_header_write). */
#define TAKE_TAG "cel8-picture-reference"

typedef struct
{
  long frames;
  unsigned long rom_bytes;
  unsigned long rom_fnv;
  char taken[64];
  unsigned long *rows;    /* frames * ROWS digests */
  unsigned long *colours; /* frames digests */
  long stopped;           /* the frame the core was seen stopped on, or -1 */
} take_t;

/* A take read whole: its header held -- one picture every frame, as many
   pictures as frames, rows of the picture's height and width -- then
   every frame in order, each followed by its line of sprite flags, which
   is skipped. */
static int take_read(const char *path, take_t *t)
{
  FILE *f;
  char line[512];
  char tag[64];
  long every, width, lines, fr, got;
  unsigned long pictures;
  int y, c;

  f = fopen(path,"r");
  if(f == NULL)
    {
      printf("FAIL: cannot open the take %s\n",path);
      return -1;
    }
  if(fgets(line,sizeof line,f) == NULL ||
     sscanf(line,"%63s frames=%ld every=%ld width=%ld lines=%ld pictures=%lu "
            "rom_bytes=%lu rom_fnv=%lx taken=%63s",
            tag,&t->frames,&every,&width,&lines,&pictures,
            &t->rom_bytes,&t->rom_fnv,t->taken) != 9 ||
     strcmp(tag,TAKE_TAG) != 0)
    {
      printf("FAIL: %s is not a take of the picture runner\n",path);
      fclose(f);
      return -1;
    }
  if(every != 1L || width != WIDTH || lines != (long)ROWS || t->frames <= 0L ||
     pictures != (unsigned long)t->frames)
    {
      printf("FAIL: %s is not one picture of %ldx%d every frame "
             "(frames=%ld every=%ld width=%ld lines=%ld pictures=%lu)\n",
             path,WIDTH,ROWS,t->frames,every,width,lines,pictures);
      fclose(f);
      return -1;
    }
  t->rows = (unsigned long *)malloc((size_t)t->frames * ROWS * sizeof(unsigned long));
  if(t->rows == NULL)
    {
      printf("FAIL: no memory for the take %s\n",path);
      fclose(f);
      return -1;
    }
  for(fr = 0; fr < t->frames; fr++)
    {
      if(fscanf(f," frame=%ld",&got) != 1 || got != fr)
        {
          printf("FAIL: %s does not hold frame %ld where it should\n",path,fr);
          fclose(f);
          return -1;
        }
      for(y = 0; y < ROWS; y++)
        if(fscanf(f," %lx",&t->rows[fr * ROWS + y]) != 1)
          {
            printf("FAIL: %s ends inside frame %ld\n",path,fr);
            fclose(f);
            return -1;
          }
      /* The rest of the digest line, then the flags line. */
      while((c = fgetc(f)) != EOF && c != '\n')
        ;
      if(fgets(line,sizeof line,f) == NULL || strncmp(line,"flags ",6) != 0)
        {
          printf("FAIL: %s has no flags line after frame %ld\n",path,fr);
          fclose(f);
          return -1;
        }
    }
  if(fscanf(f," frame=%ld",&got) == 1)
    {
      printf("FAIL: %s holds more frames than its header says\n",path);
      fclose(f);
      return -1;
    }
  fclose(f);
  return 0;
}

/* The colours of a take: one record per frame in order, the frames of
   the take and no fewer; a "stopped" line among them, or right after
   the last, is noted. Records past the frames (the runner's synthetic
   scenes) are not read. */
static int colours_read(const char *path, take_t *t)
{
  FILE *f;
  char line[128];
  long fr = 0, got;
  unsigned long h;

  t->colours = (unsigned long *)malloc((size_t)t->frames * sizeof(unsigned long));
  if(t->colours == NULL)
    {
      printf("FAIL: no memory for the colours %s\n",path);
      return -1;
    }
  f = fopen(path,"r");
  if(f == NULL)
    {
      printf("FAIL: cannot open the colours %s\n",path);
      return -1;
    }
  while(fgets(line,sizeof line,f) != NULL)
    {
      if(sscanf(line,"stopped %ld",&got) == 1)
        {
          if(t->stopped < 0 && got < t->frames)
            t->stopped = got;
          continue;
        }
      if(fr == t->frames)
        break;
      if(sscanf(line,"%ld %lx",&got,&h) != 2 || got != fr)
        {
          printf("FAIL: %s does not hold the colours of frame %ld where it should\n",path,fr);
          fclose(f);
          return -1;
        }
      t->colours[fr++] = h;
    }
  fclose(f);
  if(fr != t->frames)
    {
      printf("FAIL: %s holds the colours of %ld frames, not %ld\n",path,fr,t->frames);
      return -1;
    }
  return 0;
}

/* Whether frame a of one take matches frame b of the other: every row and
   the colours. */
static int same_frame(const take_t *t, long a, const take_t *i, long b)
{
  return t->colours[a] == i->colours[b] &&
         memcmp(t->rows + a * ROWS,i->rows + b * ROWS,
                (size_t)ROWS * sizeof(unsigned long)) == 0;
}

/* The rows of frame fr that differ between the two takes, as ranges, the
   first few only. */
static void print_rows(const take_t *t, const take_t *i, long fr)
{
  int y = 0, first = 1, ranges = 0;

  while(y < ROWS)
    {
      int a;

      if(t->rows[fr * ROWS + y] == i->rows[fr * ROWS + y])
        {
          y++;
          continue;
        }
      a = y;
      while(y < ROWS && t->rows[fr * ROWS + y] != i->rows[fr * ROWS + y])
        y++;
      if(ranges == 6)
        {
          printf(",...");
          return;
        }
      if(y - 1 == a)
        printf("%s%d",first ? "" : ",",a);
      else
        printf("%s%d-%d",first ? "" : ",",a,y - 1);
      first = 0;
      ranges++;
    }
  if(first)
    printf("none");
}

static int pictures(const char *tpath, const char *tcol,
                    const char *ipath, const char *icol)
{
  take_t t, i;
  long fr, last = 0;
  unsigned long same = 0, shifted = 0, unmatched = 0;
  int rc = 0;

  memset(&t,0,sizeof t);
  memset(&i,0,sizeof i);
  t.stopped = -1;
  i.stopped = -1;
  if(take_read(tpath,&t) != 0 || colours_read(tcol,&t) != 0 ||
     take_read(ipath,&i) != 0 || colours_read(icol,&i) != 0)
    {
      rc = 2;
      goto done;
    }
  /* The translated take first, the interpreter's second: the picture
     scripts name them so, and two takes swapped would judge nothing. */
  if(strcmp(t.taken,"translated") != 0 || strcmp(i.taken,"interpreter") != 0)
    {
      printf("FAIL: %s must be taken=translated and %s taken=interpreter "
             "(they are %s and %s)\n",tpath,ipath,t.taken,i.taken);
      rc = 2;
      goto done;
    }
  if(t.frames != i.frames || t.rom_bytes != i.rom_bytes || t.rom_fnv != i.rom_fnv)
    {
      printf("FAIL: the two takes are not of one rom and one length "
             "(%ld frames of %lu/%08lx, %ld frames of %lu/%08lx)\n",
             t.frames,t.rom_bytes,t.rom_fnv,i.frames,i.rom_bytes,i.rom_fnv);
      rc = 2;
      goto done;
    }
  /* A stopped interpreter is no reference: nothing is proved, and the
     translation is never blamed for it. A stopped core with translated
     code armed, against an interpreter that went to its end, is. */
  if(i.stopped >= 0)
    {
      printf("FAIL: the interpreter's core stopped at frame %ld: nothing to hold the pictures to\n",
             i.stopped);
      rc = 2;
      goto done;
    }
  if(t.stopped >= 0)
    {
      printf("z80c: MISMATCH frame %ld: the core stopped with translated code armed\n",
             t.stopped);
      rc = 1;
      goto done;
    }

  for(fr = 0; fr < t.frames; fr++)
    {
      /* The interpreter's frames allowed: n, n-1, n+1 in that order, none
         before the last one matched. The last one matched is at most n,
         so n is always allowed. */
      if(same_frame(&t,fr,&i,fr))
        {
          same++;
          last = fr;
        }
      else if(fr > 0 && fr - 1 >= last && same_frame(&t,fr,&i,fr - 1))
        {
          shifted++;
          last = fr - 1;
        }
      else if(fr + 1 < i.frames && same_frame(&t,fr,&i,fr + 1))
        {
          shifted++;
          last = fr + 1;
        }
      else
        {
          unmatched++;
          printf("unmatched frame %ld from=%ld rows=",fr,last);
          print_rows(&t,&i,fr);
          if(t.colours[fr] != i.colours[fr])
            printf(" colours=differ");
          printf("\n");
          rc = 1;
        }
    }
  printf("z80c: pictures frames=%ld same=%lu shifted=%lu unmatched=%lu\n",
         t.frames,same,shifted,unmatched);

done:
  free(t.rows);
  free(t.colours);
  free(i.rows);
  free(i.colours);
  return rc;
}

/* ---- the translated run ---- */

/* The end of a translated run: the counters, how many blocks ran, the
   chains, and the hits per block when a file is named. */
static int report_translated(long frames, unsigned long spent_total,
                             const char *counts_path)
{
  uint32 z80c_exec, z80c_fallback, z80c_insns, z80c_ram;
  unsigned long i, hit = 0;
  uint32 chains;

  z80c_counts(&z80c_exec,&z80c_fallback,&z80c_insns,&z80c_ram);
  printf("z80c: translated %lu frames exec=%lu fallback=%lu insns=%lu ram_exec=%lu\n",
         (unsigned long)frames,(unsigned long)z80c_exec,
         (unsigned long)z80c_fallback,(unsigned long)z80c_insns,
         (unsigned long)z80c_ram);
  /* A translated run in which no block ran is the interpreter judged
     against itself. */
  if(z80c_exec == 0UL)
    {
      printf("FAIL: no translated block ran\n");
      return 2;
    }
  for(i = 0; i < z80c_block_count; i++)
    if(z80c_hits[i] != 0UL)
      hit++;
  printf("z80c: hits %lu/%lu blocks ran\n",hit,(unsigned long)z80c_block_count);
  /*
   * The chain seen to be followed. A block hands the core the entry of
   * its successor so that the next one runs with no lookup; when that
   * hand-off is lost -- a successor rendered as 0, an epoch that never
   * matches, a chain that returns after its first block -- the pictures
   * do not change: the core's loop finds every block again through
   * z80c_find. Only the time changes, and the time is what this path
   * exists for. So a run in which no chain ever ran a second block is
   * refused, the way a run in which no block ran at all is refused.
   */
  chains = z80c_chains();
  printf("z80c: chains %lu entered, %lu blocks, %lu.%02lu blocks a chain\n",
         (unsigned long)chains,(unsigned long)z80c_exec,
         chains != 0UL ? (unsigned long)(z80c_exec / chains) : 0UL,
         chains != 0UL
           ? (unsigned long)(((z80c_exec % chains) * 100UL) / chains)
           : 0UL);
  if(z80c_exec <= chains)
    {
      printf("FAIL: no chain ran a second block (%lu blocks for %lu chains):"
             " the successors are not being followed\n",
             (unsigned long)z80c_exec,(unsigned long)chains);
      return 2;
    }
  if(counts_path != NULL)
    {
      FILE *cf = fopen(counts_path,"w");

      if(cf == NULL)
        {
          fprintf(stderr,"cannot write the counts %s\n",counts_path);
          return 2;
        }
      fprintf(cf,"z80c-counts frames=%lu tstates=%lu\n",(unsigned long)frames,spent_total);
      for(i = 0; i < z80c_block_count; i++)
        fprintf(cf,"%06lx %lu %lu\n",(unsigned long)z80c_table[i].pos,
                (unsigned long)z80c_hits[i],(unsigned long)z80c_tstates[i]);
      if(fclose(cf) != 0)
        {
          fprintf(stderr,"cannot finish the counts %s\n",counts_path);
          return 2;
        }
    }
  return 0;
}

/* The machine booted as src/main.c boots it, the table paired; the digest
   of the ROM as the picture runner computes it (romrun.c, digest over
   sms.cart.rom). */
static int boot(const char *rom, unsigned long *rom_fnv)
{
  host_rom_path = rom;
  z80_init();
  if(cart_init() < 0) return 2;
  if(cart_boot() < 0) return 2;
  if(vdp_init() < 0) return 2;
  z80_reset();
  z80c_init();
  host_booted = 1;
  *rom_fnv = host_fnv_add(host_fnv_begin(),sms.cart.rom,(unsigned long)sms.cart.size);
  return 0;
}

static int run(const char *rom, long frames, const char *counts_path)
{
  int line;
  long fr;
  unsigned long rom_fnv, spent_total = 0;
  int32 residue = 0;

  if(boot(rom,&rom_fnv) != 0) return 2;
  /* The console pairs the table by size alone; this runner has the digest
     in hand and refuses a table written from another image of the same
     size, rather than judging two programs that never were one. */
  if(z80c_armed && (unsigned long)z80c_rom_fnv != rom_fnv)
    {
      printf("FAIL: table digest %08lx is not the rom's %08lx\n",
             (unsigned long)z80c_rom_fnv,rom_fnv);
      return 3;
    }
  if(!z80c_armed)
    {
      printf("FAIL: no translated block ran: the table did not pair with the rom\n");
      return 2;
    }

  for(fr = 0; fr < frames; fr++)
    {
      for(line = 0; line < LINES_PER_FRAME; line++)
        {
          int32 quota = (int32)TSTATES_PER_LINE - residue;

          residue = z80_run(quota);
          /* The overrun z80_run hands back is bounded by the contract in
             z80.h: below one block with translated code armed. Held on
             every line, since the quota arithmetic of the loop rests on
             it. */
          if(residue >= (int32)Z80C_BLOCK_TSTATES_MAX)
            {
              printf("FAIL: z80_run overran the quota by %ld T-states "
                     "(frame %lu line %d)\n",
                     (long)residue,(unsigned long)fr,line);
              return 2;
            }
          spent_total += (unsigned long)((long)quota + (long)residue);

          vdp_line();
          if(line == PRESENT_LINE) host_present();
        }

      /* A stopped core spends every quota without moving, and its picture
         stands still: never a pass. This run does not know whether the
         interpreter stops on the same ROM, so a stop here says nothing
         about the translation yet: it is refused as proving nothing. The
         picture takes say which side stopped (tests/z80c/colour_tap.c),
         and only a stop with translated code armed against an interpreter
         that went to its end is a mismatch (the pictures mode). */
      if(z80_is_stopped())
        {
          printf("FAIL: the core stopped at frame %lu with translated code armed\n",
                 (unsigned long)fr);
          return 2;
        }
    }

  return report_translated(frames,spent_total,counts_path);
}

int main(int argc, char **argv)
{
  long frames;
  unsigned long rom_fnv;

  if(argc == 6 && strcmp(argv[1],"pictures") == 0)
    return pictures(argv[2],argv[3],argv[4],argv[5]);
  if(argc == 3 && strcmp(argv[2],"romid") == 0)
    {
      if(boot(argv[1],&rom_fnv) != 0) return 2;
      printf("rom_bytes=%lu rom_fnv=%08lx\n",(unsigned long)sms.cart.size,rom_fnv);
      return 0;
    }
  if((argc != 4 && argc != 5) || strcmp(argv[3],"translated") != 0)
    {
      fprintf(stderr,"usage: sidebyside <rom> <frames> translated [counts]\n"
              "       sidebyside <rom> romid\n"
              "       sidebyside pictures <translated take> <its colours> "
              "<interpreter take> <its colours>\n"
              "  counts: where the run writes the hits per block\n");
      return 2;
    }
  frames = host_count_arg(argv[2]);
  if(frames < 0)
    {
      fprintf(stderr,"frames must be a positive integer\n");
      return 2;
    }
  return run(argv[1],frames,argc == 5 ? argv[4] : NULL);
}
