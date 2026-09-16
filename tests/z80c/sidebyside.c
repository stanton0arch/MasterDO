/* Host runner of the check of the translated code: the translated code
 * held against the interpreter led by the same clock.
 *
 * The clock. Once a table is armed a line of the picture is no longer a
 * quota of T-states: the program runs until it waits -- a halt, or the
 * start of a loop the translator marked as a wait -- the video part
 * counts the line, and the interrupt it raised is taken at the head of
 * the next line (src/z80.h, z80_run_events). The REFERENCE is that same
 * clock with the blocks not executed: the table still says where the
 * program waits and a line still ends there, but the interpreter runs
 * every instruction (src/z80c.h, z80c_no_exec). The two runs then end
 * every line on the same instruction, and nothing is allowed to differ.
 *
 * Four modes.
 *
 *   translated   boots src/cart.c, src/z80.c, src/z80c.c, src/sms.c and
 *                src/vdp.c as they stand, with a generated table of
 *                translated code linked, on the ROM named, and plays it
 *                frame by frame the way src/main.c does -- 262 lines a
 *                frame, each run by events, then the video part, the
 *                presentation once line 191 is counted -- without drawing
 *                anything. It compares nothing: it refuses a table that
 *                did not pair with the ROM, a run in which no block ran
 *                and a run in which no chain ran a second block; it
 *                refuses the PROGRAM when a line never waits (the guard of
 *                src/z80c.h), naming where; it prints the counters of the
 *                translated code and the instructions a frame ran; and,
 *                when a last argument names a file, it writes the hits of
 *                every block there (one line per block: position, how
 *                often it was entered, the instructions its exits ran;
 *                after a header naming the frames and the instructions of
 *                the run) -- what the translator chooses a table under a
 *                budget from -- and, when a fifth argument names a
 *                file, the SEEDS: every position of the image the
 *                interpreter ran an instruction from, one per line after
 *                a header naming the ROM, which the translator takes as
 *                the starts of its next walk (translate.c, --seeds): the
 *                code a program reaches through a table the walk cannot
 *                read. The seeds are written even when the run is
 *                refused, or the core stopped, so that the next table
 *                reaches further. When
 *                the run is refused for a line that never waits, the
 *                cycle the program was turning in is read off the last
 *                entries it made, and its lowest entry is written to the
 *                seeds as "wait <pos>": the head of a loop that waits
 *                through a call it dispatches -- the main loop of a game
 *                whose state handler returns at once until an interrupt
 *                moves the state -- which no shape the translator knows
 *                names. The translator marks that block as a wait. A
 *                loop of sixteen million instructions inside one line is
 *                a program waiting for the line to end: nothing else a
 *                frame holds runs that long.
 *
 *                Two refusals, exit status 4, the program's and not the
 *                translation's: a line that never waits (the guard), and
 *                an instruction executed from a page that is not the
 *                image -- code in RAM, which no table can hold -- named
 *                with the position of the last cartridge code run before
 *                it.
 *
 *   romid        boots the ROM and prints "rom_bytes=<n> rom_fnv=<8 hex>",
 *                the two fields the picture runner writes in a take's
 *                header: how a script checks that a take it kept is this
 *                ROM's.
 *
 *   events       THE VERDICT. Two takes of the picture runner
 *                (tests/cel8/romrun.c, write mode, one picture every
 *                frame), the translated run's first and the reference's
 *                second, each a digest per row of the palette numbers of
 *                every frame, and beside each, in <take>.colours and
 *                <take>.memory, the colours of every frame line by line
 *                and the memory the program keeps at the end of every
 *                frame (tests/z80c/colour_tap.c). Frame n of the one must
 *                be frame n of the other, every row, the colours and the
 *                memory: no shift, no tolerance. The first frame that
 *                differs is named, with the rows, and whether the colours
 *                and the memory differ:
 *
 *                  z80c: events MISMATCH frame <n> rows=<rows> colours=same|differ memory=same|differ
 *
 *   pictures     FOR INFORMATION: the translated take held to the take of
 *                the classic interpreter, the one that spends quotas of
 *                T-states with no table armed. Every frame n must match
 *                that interpreter's frame n, n-1 or n+1 -- tried in that
 *                order, never going backwards -- rows and colours. Prints
 *                one line per frame that matches none of them, then the
 *                counts. The two clocks are not the same clock: a program
 *                run by events finishes a frame the classic interpreter
 *                did not finish in time, a start-up delay loop runs
 *                through at once, and the two may legitimately drift
 *                apart. The scripts print the counts and judge nothing on
 *                them.
 *
 * What this check proves: on every frame played, the translated code left
 * the screen -- rows and colours line by line -- and the memory the
 * program keeps exactly as the interpreter left them when led by the same
 * clock and stopping at the same waits. What it does not prove:
 *
 *   - that the translated program behaves as the console did: start-up
 *     delays are shortened, fewer frames come late, and a sample of voice
 *     played by a delay loop is played in one go -- the clock by events
 *     is not the part's clock, and the pictures against the classic
 *     interpreter are information, not a verdict;
 *   - that the waits the translator marked are the program's waits: a
 *     loop marked by mistake, or a wait left unmarked, changes the clock
 *     of both runs alike; a wait left unmarked shows as a line that never
 *     ends (refused), a loop marked by mistake shows only against a
 *     reference taken with the waits of another table (tests/z80c/
 *     run_z80c.sh does that for its mutations);
 *   - the code that never runs without a pad: menus, later levels, a game
 *     really played;
 *   - what the memory digest leaves out: the video memory is judged by
 *     the pictures only, the sound part not at all.
 *
 *   sidebyside <rom> <frames> translated [counts [seeds]]
 *   sidebyside <rom> romid
 *   sidebyside events <translated take> <reference take>
 *   sidebyside pictures <translated take> <its colours> <interpreter take> <its colours>
 *
 * Exit status: 0 the run went through, or every frame agreed; 1 a frame
 * differs (named), or the core stopped with translated code armed against
 * a reference that did not; 2 nothing could be proved (arguments, boot, a
 * chain never followed, no block run, takes unreadable or not of one run,
 * the reference's core stopped); 3 the table is not this ROM's; 4 the
 * program is refused: a line never waited, or code ran from RAM.
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
#if !Z80C_BENCH
#error "sidebyside needs Z80C_BENCH=1: the guard of a line that never waits lives under it"
#endif

#define LINES_PER_FRAME  262

/* The line the console presents the picture on: once line 191 has been
   counted, src/main.c builds the list. */
#define PRESENT_LINE ((int)VDP_ACTIVE_LINES - 1)

/* The rows of a picture, and their width, as the picture runner digests
   them (tests/cel8/romrun.c, PIC_H and PIC_W). */
#define ROWS  ((int)VDP_ACTIVE_LINES)
#define WIDTH 256L

/* ---- takes ---- */

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
  unsigned long *memory;  /* frames digests, when read */
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

/* One digest per frame, in order, the frames of the take and no fewer, out
   of a file of "<frame> <digest>" records (tests/z80c/colour_tap.c); a
   "stopped" line among them, or right after the last, is noted. Records
   past the frames (the runner's synthetic scenes) are not read. */
static int digests_read(const char *path, const char *what, take_t *t,
                        unsigned long **out)
{
  FILE *f;
  char line[128];
  long fr = 0, got;
  unsigned long d;

  *out = (unsigned long *)malloc((size_t)t->frames * sizeof(unsigned long));
  if(*out == NULL)
    {
      printf("FAIL: no memory for the %s %s\n",what,path);
      return -1;
    }
  f = fopen(path,"r");
  if(f == NULL)
    {
      printf("FAIL: cannot open the %s %s\n",what,path);
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
      if(sscanf(line,"%ld %lx",&got,&d) != 2 || got != fr)
        {
          printf("FAIL: %s does not hold the %s of frame %ld where it should\n",path,what,fr);
          fclose(f);
          return -1;
        }
      (*out)[fr++] = d;
    }
  fclose(f);
  if(fr != t->frames)
    {
      printf("FAIL: %s holds the %s of %ld frames, not %ld\n",path,what,fr,t->frames);
      return -1;
    }
  return 0;
}

/* A path with a suffix appended, or NULL when it does not fit. */
static const char *suffixed(char *buf, size_t cap, const char *path, const char *suffix)
{
  int n = snprintf(buf,cap,"%s%s",path,suffix);

  if(n < 0 || (size_t)n >= cap)
    {
      printf("FAIL: the path %s is too long\n",path);
      return NULL;
    }
  return buf;
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

static void take_free(take_t *t)
{
  free(t->rows);
  free(t->colours);
  free(t->memory);
}

/* Two takes that must be of one ROM and one length, named by the sides
   that took them. */
static int takes_agree(const take_t *t, const char *tname, const take_t *i,
                       const char *iname)
{
  if(strcmp(t->taken,tname) != 0 || strcmp(i->taken,iname) != 0)
    {
      printf("FAIL: the takes must be taken=%s and taken=%s (they are %s and %s)\n",
             tname,iname,t->taken,i->taken);
      return 0;
    }
  if(t->frames != i->frames || t->rom_bytes != i->rom_bytes || t->rom_fnv != i->rom_fnv)
    {
      printf("FAIL: the two takes are not of one rom and one length "
             "(%ld frames of %lu/%08lx, %ld frames of %lu/%08lx)\n",
             t->frames,t->rom_bytes,t->rom_fnv,i->frames,i->rom_bytes,i->rom_fnv);
      return 0;
    }
  return 1;
}

/* ---- events: the verdict ---- */

static int events(const char *tpath, const char *rpath)
{
  take_t t, r;
  char buf[1024];
  const char *p;
  long fr;
  int rc = 0;

  memset(&t,0,sizeof t);
  memset(&r,0,sizeof r);
  t.stopped = -1;
  r.stopped = -1;
  if(take_read(tpath,&t) != 0 || take_read(rpath,&r) != 0)
    {
      rc = 2;
      goto done;
    }
  if(!takes_agree(&t,"translated",&r,"reference"))
    {
      rc = 2;
      goto done;
    }
  if((p = suffixed(buf,sizeof buf,tpath,".colours")) == NULL ||
     digests_read(p,"colours",&t,&t.colours) != 0 ||
     (p = suffixed(buf,sizeof buf,tpath,".memory")) == NULL ||
     digests_read(p,"memory",&t,&t.memory) != 0 ||
     (p = suffixed(buf,sizeof buf,rpath,".colours")) == NULL ||
     digests_read(p,"colours",&r,&r.colours) != 0 ||
     (p = suffixed(buf,sizeof buf,rpath,".memory")) == NULL ||
     digests_read(p,"memory",&r,&r.memory) != 0)
    {
      rc = 2;
      goto done;
    }
  /* A stopped reference proves nothing, and the translation is never
     blamed for it. A stopped core with translated code armed, against a
     reference that went to its end, is. */
  if(r.stopped >= 0)
    {
      printf("FAIL: the reference's core stopped at frame %ld: nothing to hold the translated code to\n",
             r.stopped);
      rc = 2;
      goto done;
    }

  for(fr = 0; fr < t.frames; fr++)
    {
      if(same_frame(&t,fr,&r,fr) && t.memory[fr] == r.memory[fr])
        continue;
      /* A frame that differs once the translated core has stopped is
         the stop's doing, and the stop is what is named. */
      if(t.stopped >= 0 && t.stopped <= fr)
        {
          printf("z80c: MISMATCH frame %ld: the core stopped with translated code armed\n",
                 t.stopped);
          rc = 1;
          goto done;
        }
      printf("z80c: events MISMATCH frame %ld rows=",fr);
      print_rows(&t,&r,fr);
      printf(" colours=%s memory=%s\n",
             (t.colours[fr] == r.colours[fr]) ? "same" : "differ",
             (t.memory[fr] == r.memory[fr]) ? "same" : "differ");
      rc = 1;
      goto done;
    }
  if(t.stopped >= 0)
    {
      printf("z80c: MISMATCH frame %ld: the core stopped with translated code armed\n",
             t.stopped);
      rc = 1;
      goto done;
    }
  printf("z80c: events frames=%ld same=%ld\n",t.frames,t.frames);

done:
  take_free(&t);
  take_free(&r);
  return rc;
}

/* ---- pictures: the classic interpreter, for information ---- */

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
  if(take_read(tpath,&t) != 0 || digests_read(tcol,"colours",&t,&t.colours) != 0 ||
     take_read(ipath,&i) != 0 || digests_read(icol,"colours",&i,&i.colours) != 0)
    {
      rc = 2;
      goto done;
    }
  if(!takes_agree(&t,"translated",&i,"interpreter"))
    {
      rc = 2;
      goto done;
    }
  if(i.stopped >= 0 || t.stopped >= 0)
    {
      printf("FAIL: a core stopped (translated at %ld, interpreter at %ld): nothing compared\n",
             t.stopped,i.stopped);
      rc = 2;
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
          rc = 1;
        }
    }
  printf("z80c: pictures frames=%ld same=%lu shifted=%lu unmatched=%lu\n",
         t.frames,same,shifted,unmatched);
  /* Where the unmatched frames are, for the reader: a program run by
     events runs its delay loops through at once and gets AHEAD of the
     classic interpreter, so its frame n is often that interpreter's
     frame n + k. Every unmatched frame is looked for anywhere in the
     interpreter's take, and the farthest it was found ahead and behind
     is said, with how many were found nowhere. */
  if(unmatched != 0UL)
    {
      unsigned long found = 0, nowhere = 0;
      long ahead = 0, behind = 0;

      for(fr = 0; fr < t.frames; fr++)
        {
          long k, best = -1;

          if(same_frame(&t,fr,&i,fr) ||
             (fr > 0 && same_frame(&t,fr,&i,fr - 1)) ||
             (fr + 1 < i.frames && same_frame(&t,fr,&i,fr + 1)))
            continue;
          for(k = 0; k < i.frames; k++)
            if(same_frame(&t,fr,&i,k) && (best < 0 || labs(k - fr) < labs(best - fr)))
              best = k;
          if(best < 0)
            nowhere++;
          else
            {
              found++;
              if(best - fr > ahead) ahead = best - fr;
              if(fr - best > behind) behind = fr - best;
            }
        }
      printf("z80c: pictures elsewhere=%lu ahead_max=%ld behind_max=%ld nowhere=%lu\n",
             found,ahead,behind,nowhere);
    }

done:
  take_free(&t);
  take_free(&i);
  return rc;
}

/* ---- the translated run ---- */

/* The end of a translated run: the counters, how many blocks ran, the
   chains, the instructions a frame ran, and the hits per block when a
   file is named. */
static int report_translated(long frames, const char *counts_path)
{
  uint32 z80c_exec, z80c_fallback, z80c_insns, z80c_ram;
  unsigned long i, hit = 0, all;
  uint32 chains;

  z80c_counts(&z80c_exec,&z80c_fallback,&z80c_insns,&z80c_ram);
  all = (unsigned long)z80c_insns + (unsigned long)z80c_fallback;
  printf("z80c: translated %lu frames exec=%lu fallback=%lu insns=%lu ram_exec=%lu\n",
         (unsigned long)frames,(unsigned long)z80c_exec,
         (unsigned long)z80c_fallback,(unsigned long)z80c_insns,
         (unsigned long)z80c_ram);
  printf("z80c: insns/frame=%lu\n",all / (unsigned long)frames);
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
   * matches, a chain that returns after its first block -- nothing the
   * program does changes: the core's loop finds every block again
   * through z80c_find. Only the time changes, and the time is what this
   * path exists for. So a run in which no chain ever ran a second block
   * is refused, the way a run in which no block ran at all is refused.
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
      fprintf(cf,"z80c-counts frames=%lu insns=%lu\n",(unsigned long)frames,all);
      for(i = 0; i < z80c_block_count; i++)
        fprintf(cf,"%06lx %lu %lu\n",(unsigned long)z80c_table[i].pos,
                (unsigned long)z80c_hits[i],(unsigned long)z80c_ran[i]);
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
  z80c_no_exec = 0;
  host_booted = 1;
  *rom_fnv = host_fnv_add(host_fnv_begin(),sms.cart.rom,(unsigned long)sms.cart.size);
  return 0;
}

/* The cycle the program is turning in, read off the ring of its last
   entries: the shortest period on which the ring repeats over its
   second half. Returns the cycle's lowest entry, or -1 when the ring
   holds no cycle (or too few entries). */
static long cycle_head(void)
{
  unsigned long n = (z80c_ring_n < Z80C_RING) ? z80c_ring_n : Z80C_RING;
  unsigned long p, i;

  for(p = 1; p <= n / 4UL; p++)
    {
      int repeats = 1;

      for(i = n / 2UL; i < n; i++)
        if(z80c_ring[(z80c_ring_n - n + i) & (Z80C_RING - 1UL)] !=
           z80c_ring[(z80c_ring_n - n + i - p) & (Z80C_RING - 1UL)])
          {
            repeats = 0;
            break;
          }
      if(repeats)
        {
          unsigned long head = 0xFFFFFFFFUL;

          for(i = n - p; i < n; i++)
            {
              unsigned long e = z80c_ring[(z80c_ring_n - n + i) & (Z80C_RING - 1UL)];

              if(e < head)
                head = e;
            }
          printf("z80c: the program cycles through %lu entries, lowest %06lx\n",p,head);
          return (long)head;
        }
    }
  printf("z80c: no cycle read off the last %lu entries: no wait named\n",n);
  return -1L;
}

/* The seeds of the translator's next walk: the positions the interpreter
   ran from, after a header naming the ROM, and the wait the cycle names
   when there is one. */
static int write_seeds(const char *path, unsigned long rom_fnv, long wait_head)
{
  FILE *f = fopen(path,"w");
  unsigned long i, n = 0;

  if(f == NULL)
    {
      fprintf(stderr,"cannot write the seeds %s\n",path);
      return 2;
    }
  fprintf(f,"z80c-seeds rom_bytes=%lu rom_fnv=%08lx\n",(unsigned long)sms.cart.size,rom_fnv);
  for(i = 0; i < (unsigned long)sms.cart.size; i++)
    if(z80c_seen[i])
      {
        fprintf(f,"%06lx\n",i);
        n++;
      }
  if(wait_head >= 0L)
    fprintf(f,"wait %06lx\n",(unsigned long)wait_head);
  if(fclose(f) != 0)
    {
      fprintf(stderr,"cannot finish the seeds %s\n",path);
      return 2;
    }
  printf("z80c: seeds=%lu positions interpreted from the image\n",n);
  return 0;
}

/* The position in the image of an address, or -1 when its page is not
   the image, as src/z80c.c (z80c_find) decides it. */
static long cart_pos(uint32 addr)
{
  long off = (long)(z80_rmap[(uint16)addr >> Z80_PAGE_BITS] - sms.cart.rom);

  if(off < 0L || (unsigned long)off >= (unsigned long)sms.cart.size)
    return -1L;
  return off + (long)(addr & Z80_PAGE_MASK);
}

static int run(const char *rom, long frames, const char *counts_path,
               const char *seeds_path)
{
  int line;
  long fr;
  unsigned long rom_fnv;
  int rc;

  if(boot(rom,&rom_fnv) != 0) return 2;
  if(seeds_path != NULL)
    {
      z80c_seen = (uint8 *)calloc((size_t)sms.cart.size,1);
      if(z80c_seen == NULL)
        {
          printf("FAIL: no memory for the seeds\n");
          return 2;
        }
    }
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
          z80_run_events();
          /* The two refusals of the program, said with the position and
             the frame; the seeds are written first, so that the next
             walk reaches what this run reached. */
          if(z80c_ram_pc != Z80C_NO_PC)
            {
              printf("z80c: REFUSED ram code at %04lX reached from %06lx frame %ld\n",
                     (unsigned long)z80c_ram_pc,(unsigned long)z80c_last_pos,fr);
              printf("z80c: the program executes code from a page that is not the image\n");
              if(seeds_path != NULL) (void)write_seeds(seeds_path,rom_fnv,-1L);
              return 4;
            }
          if(z80c_no_wait)
            {
              long off = cart_pos(z80c_no_wait_pc);

              if(off >= 0L)
                printf("z80c: REFUSED no wait at %06lx frame %ld\n",(unsigned long)off,fr);
              else
                printf("z80c: REFUSED no wait at ram %04lx frame %ld\n",
                       (unsigned long)z80c_no_wait_pc,fr);
              printf("z80c: a line ran more than %lu instructions without waiting\n",
                     (unsigned long)Z80C_LINE_INSNS_MAX);
              if(seeds_path != NULL) (void)write_seeds(seeds_path,rom_fnv,cycle_head());
              return 4;
            }

          vdp_line();
          if(line == PRESENT_LINE) host_present();
        }

      /* A stopped core runs nothing, and its picture stands still: never
         a pass. This run does not know whether the reference stops on the
         same ROM, so a stop here says nothing about the translation yet:
         it is refused as proving nothing. The picture takes say which side
         stopped (tests/z80c/colour_tap.c), and only a stop with
         translated code armed against a reference that went to its end is
         a mismatch (the events mode). */
      if(z80_is_stopped())
        {
          printf("FAIL: the core stopped at frame %lu with translated code armed\n",
                 (unsigned long)fr);
          if(seeds_path != NULL) (void)write_seeds(seeds_path,rom_fnv,-1L);
          return 2;
        }
    }

  rc = report_translated(frames,counts_path);
  if(seeds_path != NULL && write_seeds(seeds_path,rom_fnv,-1L) != 0)
    return 2;
  return rc;
}

int main(int argc, char **argv)
{
  long frames;
  unsigned long rom_fnv;

  if(argc == 6 && strcmp(argv[1],"pictures") == 0)
    return pictures(argv[2],argv[3],argv[4],argv[5]);
  if(argc == 4 && strcmp(argv[1],"events") == 0)
    return events(argv[2],argv[3]);
  if(argc == 3 && strcmp(argv[2],"romid") == 0)
    {
      if(boot(argv[1],&rom_fnv) != 0) return 2;
      printf("rom_bytes=%lu rom_fnv=%08lx\n",(unsigned long)sms.cart.size,rom_fnv);
      return 0;
    }
  if((argc < 4 || argc > 6) || strcmp(argv[3],"translated") != 0)
    {
      fprintf(stderr,"usage: sidebyside <rom> <frames> translated [counts [seeds]]\n"
              "       sidebyside <rom> romid\n"
              "       sidebyside events <translated take> <reference take>\n"
              "       sidebyside pictures <translated take> <its colours> "
              "<interpreter take> <its colours>\n"
              "  counts: where the run writes the hits per block\n"
              "  seeds: where the run writes the positions interpreted from the image\n");
      return 2;
    }
  frames = host_count_arg(argv[2]);
  if(frames < 0)
    {
      fprintf(stderr,"frames must be a positive integer\n");
      return 2;
    }
  return run(argv[1],frames,argc >= 5 ? argv[4] : NULL,argc == 6 ? argv[5] : NULL);
}
