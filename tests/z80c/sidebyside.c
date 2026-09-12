/* Host runner for the side-by-side check: the translated code held
 * against the interpreter, line for line, frame for frame.
 *
 * Boots src/cart.c, src/z80.c, src/z80c.c, src/sms.c and src/vdp.c as they
 * stand, with a generated table of translated code linked, on the ROM the
 * console runs, and plays it frame by frame the way src/main.c does --
 * 262 lines a frame, the processor's quota then the video part -- without
 * drawing anything. Three modes, on the same binary:
 *
 *   record   translated code armed. After every line the registers are
 *            written to the trace, with the T-states the line spent, the
 *            block that started at the line's entry PC and how many blocks
 *            the line ran; after every frame, a digest of the state (see
 *            "the frame record" below) and the counters. Prints two lines
 *            at the end -- the four counters, then how many blocks of the
 *            table ran at all -- and, when a sixth argument names a file,
 *            writes the hits of every block there (one line per block:
 *            position, how often it was entered, the T-states its exits
 *            charged; after a header naming the frames and the T-states
 *            the run spent): what the translator chooses a table under a
 *            budget from. <every> does not apply.
 *   replay   the table disarmed after z80c_init: the interpreter alone,
 *            reading the trace. Every line is given as its quota exactly
 *            what the translated run spent on it, so that it stops on the
 *            same instruction boundary -- the interpreter overruns by at
 *            most one instruction, the translated code by at most one
 *            block, and with 228 - residue on both sides the two would
 *            legitimately stop at different instructions and sample the
 *            interrupt at different points. Then the registers are held
 *            against the recorded ones, and at the end of the frame the
 *            digest and the counters. Prints a line every <every> frames
 *            and on the last. The first difference is named: the frame,
 *            the line, the block that started at the line's entry PC when
 *            one does ("none" otherwise), how many blocks the line ran,
 *            and both states.
 *   replay-poke  replay, with one byte of the work RAM flipped on frame 0
 *            just before the frame's digest is taken. This is the check
 *            of the frame path itself, on its own terms: the mutations
 *            the script plays bite on the line path first, so the digest
 *            is seen to bite here, without them. Expected: a MISMATCH on
 *            frame 0, line 261, exit 1.
 *
 * What the comparison measures is the C the tool emitted and nothing
 * else: same compiler, same core, same table linked, only the switch
 * differs. Two processes rather than two boots in one: the work RAM is
 * allocated once and never cleared (src/cart.c).
 *
 * A wrong static T-state sum shows as well: the interpreter given the
 * recorded quota then stops one instruction early or late, its residue is
 * not zero, and the spent figure of the line differs.
 *
 * The state is read field by field, never as the bytes of a structure:
 * padding, and fields that exist in one configuration only, would compare
 * noise. The trace holds positions, digests, counters and registers --
 * no byte of the ROM. It weighs 10516 bytes per frame (262 line records
 * of 40 and one frame record of 36), about 31.5 megabytes at 3000 frames.
 *
 * A core that stops -- an opcode it does not implement -- would freeze
 * identically in both runs; it is asked once per frame and a stopped
 * core is a failure, never a pass.
 *
 *   sidebyside <rom> <frames> <every> record|replay|replay-poke <trace> [counts]
 *
 * Exit status: 0 every line and every frame the same, 1 a difference,
 * 2 the run could not prove anything (arguments, boot, overrun of the
 * quota, a stopped core, trace unreadable), 3 the table is not this
 * ROM's.
 *
 * Stubs on the same terms as the picture check (tests/cel8/romrun.c):
 * the disc is the host file, the allocator is the host's, the log goes
 * to stderr.
 */
#include "sms.h"
#include "cart.h"
#include "vdp.h"
#include "z80.h"
#include "z80c.h"
#include "log.h"
#include "blockfile.h"
#include "operror.h"
#include "filesystem.h"
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

/* The counters this runner records exist in the telemetry build alone;
   the script pins the switches, and a build that lost one must fail here
   rather than record zeroes. */
#if !LOG_ENABLE
#error "sidebyside needs LOG_ENABLE: the port counters live under it"
#endif
#if !SMS_TELEMETRY
#error "sidebyside needs SMS_TELEMETRY=1: z80c_counts lives under it"
#endif
#if !Z80C_HITS
#error "sidebyside needs Z80C_HITS=1: the hits per block live under it"
#endif
#if !VDP_COUNTERS
#error "sidebyside needs the video counters (VDP_COUNTERS)"
#endif
/* The category names below are indexed by the log's own numbering. */
#if LOG_CAT_COUNT != 11
#error "the log categories changed: the names in cat_name[] must follow"
#endif

#define LINES_PER_FRAME  262
#define TSTATES_PER_LINE 228

/* The line the console presents the picture on: once line 191 has been
   counted, src/main.c builds the list, so the decor journal is replayed
   and emptied there and not left to fill. */
#define PRESENT_LINE ((int)VDP_ACTIVE_LINES - 1)

/* The work RAM: the console's 8 kilobytes behind the pages from 0xC000,
   one kilobyte each, read through the write map -- the one table that
   points at the RAM and at nothing the processor could not write. The
   size is the cartridge module's own (src/cart.c, CART_WORK_RAM_SIZE),
   not published; the eight pages are pinned here, and the mirror above
   them is the same bytes. */
#define RAM_FIRST_PAGE (0xC000UL >> Z80_PAGE_BITS)
#define RAM_PAGES      (8192UL / Z80_PAGE_SIZE)

/* The trace. A header, then per frame LINES_PER_FRAME line records and
   one frame record, every field little-endian and at a fixed offset. */
#define TRACE_MAGIC   "SBS2"
#define TRACE_HDR     16
#define LINE_REC      40
#define LINE_REGS     32 /* the part compared by memcmp, spent included */
#define FRAME_REC     36
#define FRAME_CMP     28 /* the part compared: digest and six counters */
#define NO_BLOCK      0xFFFFFFFFUL

/* ---- the disc: one file, the ROM named on the command line ---- */

static const char *rom_path = NULL;
static long rom_size = 0;

/* Whether a name ends in the extension, letter case set aside. */
static int has_ext(const char *name, const char *ext)
{
  size_t n = strlen(name), e = strlen(ext), i;
  if(n < e) return 0;
  for(i = 0; i < e; i++)
    {
      int c = name[n - e + i], d = ext[i];
      if(c >= 'A' && c <= 'Z') c += 'a' - 'A';
      if(c != d) return 0;
    }
  return 1;
}

Err OpenBlockFile(char *name, BlockFilePtr bf)
{
  FILE *f;
  (void)bf;
  /* The boot tries the SMS name, then the GG name (src/cart.c): the one
     found is the one whose extension is the file's on disc, so that a
     Game Gear image boots with the Game Gear profile and nothing else. */
  if(!(has_ext(name,".sms") && has_ext(rom_path,".sms")) &&
     !(has_ext(name,".gg") && has_ext(rom_path,".gg")))
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

/* ---- FNV-1a, 32 bits, continued over several runs of bytes ---- */

static unsigned long fnv_begin(void)
{
  return 2166136261UL;
}

static unsigned long fnv_add(unsigned long h, const unsigned char *p,
                             unsigned long n)
{
  unsigned long i;
  for(i = 0; i < n; i++)
    {
      h ^= (unsigned long)p[i];
      h *= 16777619UL;
      h &= 0xFFFFFFFFUL;
    }
  return h;
}

/* A word folded in as four bytes, low first, whatever the host's order. */
static unsigned long fnv_add_u32(unsigned long h, unsigned long v)
{
  unsigned char b[4];
  b[0] = (unsigned char)(v & 0xFFUL);
  b[1] = (unsigned char)((v >> 8) & 0xFFUL);
  b[2] = (unsigned char)((v >> 16) & 0xFFUL);
  b[3] = (unsigned char)((v >> 24) & 0xFFUL);
  return fnv_add(h,b,4);
}

/* ---- little-endian fields of the trace ---- */

static void put16(unsigned char *p, unsigned long v)
{
  p[0] = (unsigned char)(v & 0xFFUL);
  p[1] = (unsigned char)((v >> 8) & 0xFFUL);
}
static void put32(unsigned char *p, unsigned long v)
{
  put16(p,v);
  put16(p + 2,v >> 16);
}
static unsigned long get16(const unsigned char *p)
{
  return (unsigned long)p[0] | ((unsigned long)p[1] << 8);
}
static unsigned long get32(const unsigned char *p)
{
  return get16(p) | (get16(p + 2) << 16);
}

/* ---- the line record ----
 *
 *   0..7   a f b c d e h l
 *   8..15  the alternate set, same order
 *   16..19 ixh ixl iyh iyl
 *   20..21 sp        22..23 pc        24 i   25 r
 *   26     iff1 | iff2 << 1 | im << 2 | halted << 4 | ei_delay << 5
 *   27     zero
 *   28..29 spent: the T-states the line consumed, quota plus residue
 *   30..31 zero
 *   32..33 pc at the entry of the line
 *   34..35 blocks the line ran (z80c_counts before and after)
 *   36..39 position of the block starting at the entry pc, NO_BLOCK when
 *          none starts there
 *
 * The first 32 bytes are what the replay compares; the last 8 name the
 * place and are read back, never recomputed, in replay. */

static void regs_take(unsigned char *r, long spent)
{
  const z80_t *z = &sms.z80;

  memset(r,0,LINE_REGS);
  r[0] = z->main.a; r[1] = z->main.f;
  r[2] = z->main.b; r[3] = z->main.c;
  r[4] = z->main.d; r[5] = z->main.e;
  r[6] = z->main.h; r[7] = z->main.l;
  r[8] = z->alt.a;  r[9] = z->alt.f;
  r[10] = z->alt.b; r[11] = z->alt.c;
  r[12] = z->alt.d; r[13] = z->alt.e;
  r[14] = z->alt.h; r[15] = z->alt.l;
  r[16] = z->ixh; r[17] = z->ixl; r[18] = z->iyh; r[19] = z->iyl;
  put16(r + 20,(unsigned long)z->sp);
  put16(r + 22,(unsigned long)z->pc);
  r[24] = z->i;
  r[25] = z->r;
  r[26] = (unsigned char)((z->iff1 & 1U) | ((z->iff2 & 1U) << 1) |
                          ((z->im & 3U) << 2) | ((z->halted & 1U) << 4) |
                          ((z->ei_delay & 1U) << 5));
  put16(r + 28,(unsigned long)spent);
}

/* The state as one line of text, from a record. */
static void regs_format(char *out, size_t cap, const unsigned char *r)
{
  snprintf(out,cap,
           "af=%02x%02x bc=%02x%02x de=%02x%02x hl=%02x%02x "
           "af'=%02x%02x bc'=%02x%02x de'=%02x%02x hl'=%02x%02x "
           "ix=%02x%02x iy=%02x%02x sp=%04lx pc=%04lx i=%02x r=%02x "
           "iff=%u%u im=%u halt=%u ei=%u t=%lu",
           r[0],r[1],r[2],r[3],r[4],r[5],r[6],r[7],
           r[8],r[9],r[10],r[11],r[12],r[13],r[14],r[15],
           r[16],r[17],r[18],r[19],get16(r + 20),get16(r + 22),r[24],r[25],
           (unsigned)(r[26] & 1U),(unsigned)((r[26] >> 1) & 1U),
           (unsigned)((r[26] >> 2) & 3U),(unsigned)((r[26] >> 4) & 1U),
           (unsigned)((r[26] >> 5) & 1U),get16(r + 28));
}

/* ---- the frame record ----
 *
 *   0..3   digest of the state at the end of the frame (below)
 *   4..7   unrouted port reads       8..11  unrouted port writes
 *   12..15 video register writes     16..19 video memory writes
 *   20..23 colour memory writes      24..27 status port reads
 *   28..31 blocks executed           32..35 hand-backs to the interpreter
 *
 * The first 28 bytes are what the replay compares. The last two fields
 * are the translated run's figures, carried for the report: the replay
 * runs none. Every counter is a running total since reset -- nothing in
 * this loop calls the periodic reports that clear them.
 *
 * The digest covers what the program can read back or what decides what
 * it reads next: the work RAM, the video memory, the colour memory, the
 * sixteen registers, the two scroll latches, the address, the code, the
 * control word and its latch, the read buffer, the line count and the
 * line counter, the two interrupt requests, the two sprite bits of the
 * status port (a program branches on them), the four mapper registers,
 * the memory control register, and the cartridge RAM. Left out on
 * purpose: everything the render keeps for itself -- the decor journal,
 * the cel lists and their arena, the sheets, the tile cache, the PLUT
 * and the CLUT -- which the processor never reads and which the
 * presentation rebuilds from the memory digested above; and the
 * telemetry fields of the processor (irq_accepted), which exist in one
 * configuration only. */

static unsigned long state_digest(void)
{
  unsigned long h = fnv_begin();
  unsigned long i;

  for(i = 0; i < RAM_PAGES; i++)
    h = fnv_add(h,z80_wmap[RAM_FIRST_PAGE + i],Z80_PAGE_SIZE);
  h = fnv_add(h,sms.vdp.vram,VDP_VRAM_SIZE);
  h = fnv_add(h,sms.vdp.cram,VDP_CRAM_SIZE);
  h = fnv_add(h,sms.vdp.reg,VDP_REG_COUNT);
  h = fnv_add_u32(h,(unsigned long)sms.vdp.vscroll);
  h = fnv_add_u32(h,(unsigned long)sms.vdp.hscroll);
  h = fnv_add_u32(h,(unsigned long)sms.vdp.addr);
  h = fnv_add_u32(h,(unsigned long)sms.vdp.code);
  h = fnv_add_u32(h,(unsigned long)sms.vdp.ctrl_word);
  h = fnv_add_u32(h,(unsigned long)sms.vdp.latch);
  h = fnv_add_u32(h,(unsigned long)sms.vdp.read_buf);
  h = fnv_add_u32(h,(unsigned long)sms.vdp.vcount);
  h = fnv_add_u32(h,(unsigned long)sms.vdp.line_ctr);
  h = fnv_add_u32(h,(unsigned long)sms.vdp.frame_pending);
  h = fnv_add_u32(h,(unsigned long)sms.vdp.line_pending);
  h = fnv_add_u32(h,(unsigned long)sms.vdp.spr_overflow);
  h = fnv_add_u32(h,(unsigned long)sms.vdp.spr_collision);
  h = fnv_add_u32(h,(unsigned long)sms.cart.mapper_fffc);
  h = fnv_add_u32(h,(unsigned long)sms.cart.mapper_fffd);
  h = fnv_add_u32(h,(unsigned long)sms.cart.mapper_fffe);
  h = fnv_add_u32(h,(unsigned long)sms.cart.mapper_ffff);
  h = fnv_add_u32(h,(unsigned long)sms.cart.memctl);
  /* The cartridge RAM exists in the Sega build alone; a build without
     it digests nothing here, on both sides alike. */
  if(sms.cart.cart_ram != NULL)
    h = fnv_add(h,sms.cart.cart_ram,CART_RAM_SIZE);
  return h;
}

static void frame_take(unsigned char *f)
{
  uint32 z80c_exec, z80c_fallback, z80c_insns, z80c_ram;

  z80c_counts(&z80c_exec,&z80c_fallback,&z80c_insns,&z80c_ram);
  put32(f,state_digest());
  put32(f + 4,(unsigned long)sms.cart.io_unrouted_reads);
  put32(f + 8,(unsigned long)sms.cart.io_unrouted_writes);
  put32(f + 12,(unsigned long)sms.vdp.cnt_reg_w);
  put32(f + 16,(unsigned long)sms.vdp.cnt_vram_w);
  put32(f + 20,(unsigned long)sms.vdp.cnt_cram_w);
  put32(f + 24,(unsigned long)sms.vdp.cnt_status_r);
  put32(f + 28,(unsigned long)z80c_exec);
  put32(f + 32,(unsigned long)z80c_fallback);
}

/* The digest, then the counters in the record's order: port reads and
   writes, register, video memory and colour writes, status reads. */
static void frame_format(char *out, size_t cap, const unsigned char *f)
{
  snprintf(out,cap,"digest=%08lx io=%lu/%lu vdp=%lu/%lu/%lu/%lu",
           get32(f),get32(f + 4),get32(f + 8),
           get32(f + 12),get32(f + 16),get32(f + 20),get32(f + 24));
}

/* The block's position for a table entry: the lookup answers the entry,
   and the position is what names it. */
static unsigned long block_pos(const z80c_entry_t *e)
{
  if(e == NULL) return NO_BLOCK;
  return (unsigned long)e->pos;
}

static void block_name(char *out, size_t cap, unsigned long pos)
{
  if(pos == NO_BLOCK)
    snprintf(out,cap,"none");
  else
    snprintf(out,cap,"%06lx",pos);
}

/* The presentation as the console makes it once line 191 is counted:
   the journal undone, every band replayed -- the last one puts the video
   memory back in its final state -- and the journal emptied. Without it
   the journal fills and the picture is degraded; the processor sees no
   difference, but the console never runs that way. */
static void present(void)
{
  int32 bands, k;
  bands = vdp_list_begin();
  for(k = 0; k < bands; k++) vdp_list_band(k);
  vdp_list_end();
}

/* A positive count from the command line, the whole word or nothing. */
static long count_arg(const char *s)
{
  char *end;
  long v;
  if(*s == '\0') return -1;
  v = strtol(s,&end,10);
  if(*end != '\0' || v <= 0) return -1;
  return v;
}

int main(int argc, char **argv)
{
  long frames, every, fr;
  int line;
  int recording, poke = 0;
  FILE *tr;
  unsigned char hdr[TRACE_HDR];
  unsigned char rec[LINE_REC], got[LINE_REC];
  unsigned char frec[FRAME_REC], fgot[FRAME_REC];
  unsigned long rom_fnv;
  int32 residue = 0;
  long total_exec = 0;
  unsigned long spent_total = 0;
  const char *counts_path = NULL;
  char a[256], b[256], name[16];

  if(argc != 6 && argc != 7)
    {
      fprintf(stderr,"usage: sidebyside <rom> <frames> <every> "
              "record|replay|replay-poke <trace> [counts]\n"
              "  every: the frames between two progress lines of a replay\n"
              "  counts: where a record writes the hits per block\n");
      return 2;
    }
  if(argc == 7)
    counts_path = argv[6];
  rom_path = argv[1];
  frames = count_arg(argv[2]);
  every = count_arg(argv[3]);
  if(frames < 0 || every < 0)
    {
      fprintf(stderr,"frames and every must be positive integers\n");
      return 2;
    }
  if(strcmp(argv[4],"record") == 0)
    recording = 1;
  else if(strcmp(argv[4],"replay") == 0)
    recording = 0;
  else if(strcmp(argv[4],"replay-poke") == 0)
    {
      recording = 0;
      poke = 1;
    }
  else
    {
      fprintf(stderr,"mode must be record, replay or replay-poke, not %s\n",
              argv[4]);
      return 2;
    }

  z80_init();
  if(cart_init() < 0) return 2;
  if(cart_boot() < 0) return 2;
  if(vdp_init() < 0) return 2;
  z80_reset();
  /* The table paired as src/main.c pairs it; the replay then takes the
     switch down, and the core never looks for a block again (src/z80.c
     reads it at the head of every turn). Same binary, same table linked,
     the interpreter alone. */
  z80c_init();
  booted = 1;

  rom_fnv = fnv_add(fnv_begin(),sms.cart.rom,(unsigned long)sms.cart.size);
  /* The console pairs the table by size alone; this runner has the digest
     in hand and refuses a table written from another image of the same
     size, rather than judging two programs that never were one. */
  if(z80c_armed && (unsigned long)z80c_rom_fnv != rom_fnv)
    {
      printf("FAIL: table digest %08lx is not the rom's %08lx\n",
             (unsigned long)z80c_rom_fnv,rom_fnv);
      return 3;
    }
  if(!recording)
    z80c_armed = 0;

  if(recording)
    {
      tr = fopen(argv[5],"wb");
      if(tr == NULL)
        {
          fprintf(stderr,"cannot write the trace %s\n",argv[5]);
          return 2;
        }
      memcpy(hdr,TRACE_MAGIC,4);
      put32(hdr + 4,(unsigned long)frames);
      put32(hdr + 8,(unsigned long)sms.cart.size);
      put32(hdr + 12,rom_fnv);
      fwrite(hdr,1,TRACE_HDR,tr);
    }
  else
    {
      tr = fopen(argv[5],"rb");
      if(tr == NULL)
        {
          fprintf(stderr,"cannot open the trace %s\n",argv[5]);
          return 2;
        }
      if(fread(hdr,1,TRACE_HDR,tr) != TRACE_HDR ||
         memcmp(hdr,TRACE_MAGIC,4) != 0)
        {
          printf("FAIL: %s is not a trace of this runner\n",argv[5]);
          return 2;
        }
      if(get32(hdr + 4) != (unsigned long)frames ||
         get32(hdr + 8) != (unsigned long)sms.cart.size ||
         get32(hdr + 12) != rom_fnv)
        {
          printf("FAIL: the trace is of %lu frames of rom %lu/%08lx, "
                 "not %lu of %lu/%08lx\n",
                 get32(hdr + 4),get32(hdr + 8),get32(hdr + 12),
                 (unsigned long)frames,(unsigned long)sms.cart.size,rom_fnv);
          return 2;
        }
    }

  for(fr = 0; fr < frames; fr++)
    {
      for(line = 0; line < LINES_PER_FRAME; line++)
        {
          int32 quota;
          uint32 exec0, exec1, fb, ins, ram;
          unsigned long pc_entry = (unsigned long)sms.z80.pc;

          if(recording)
            {
              quota = (int32)TSTATES_PER_LINE - residue;
              z80c_counts(&exec0,&fb,&ins,&ram);
              put16(rec + 32,pc_entry);
              put32(rec + 36,block_pos(z80c_find((uint16)pc_entry)));
            }
          else
            {
              if(fread(rec,1,LINE_REC,tr) != LINE_REC)
                {
                  printf("FAIL: the trace ends at frame %lu line %d\n",
                         (unsigned long)fr,line);
                  return 2;
                }
              /* The quota is what the translated run spent on this line:
                 the interpreter stops on the same instruction boundary,
                 or its residue says the static sum was wrong. */
              quota = (int32)get16(rec + 28);
              exec0 = 0;
            }

          residue = z80_run(quota);
          /* The overrun z80_run hands back is bounded by the contract in
             z80.h: below one instruction with the interpreter alone, and
             below one block with translated code armed -- a block spends
             at most Z80C_BLOCK_TSTATES_MAX, started with at least one
             T-state left. Held on every line, since the quota arithmetic
             of the scanline loop rests on it. */
          if(residue >= (int32)Z80C_BLOCK_TSTATES_MAX)
            {
              printf("FAIL: z80_run overran the quota by %ld T-states "
                     "(frame %lu line %d)\n",
                     (long)residue,(unsigned long)fr,line);
              return 2;
            }

          if(recording)
            {
              regs_take(rec,(long)quota + (long)residue);
              spent_total += (unsigned long)((long)quota + (long)residue);
              z80c_counts(&exec1,&fb,&ins,&ram);
              put16(rec + 34,(unsigned long)(exec1 - exec0));
              fwrite(rec,1,LINE_REC,tr);
            }
          else
            {
              regs_take(got,(long)quota + (long)residue);
              if(memcmp(got,rec,LINE_REGS) != 0)
                {
                  regs_format(a,sizeof a,got);
                  regs_format(b,sizeof b,rec);
                  block_name(name,sizeof name,get32(rec + 36));
                  printf("z80c: MISMATCH frame %lu line %d block %s blocks=%lu "
                         ": %s / %s\n",
                         (unsigned long)fr,line,name,get16(rec + 34),a,b);
                  return 1;
                }
            }

          vdp_line();
          if(line == PRESENT_LINE) present();
        }

      /* A stopped core spends every quota without moving: both runs
         would agree on a dead machine. */
      if(z80_is_stopped())
        {
          printf("FAIL: the core stopped at frame %lu\n",(unsigned long)fr);
          return 2;
        }

      if(recording)
        {
          frame_take(frec);
          fwrite(frec,1,FRAME_REC,tr);
        }
      else
        {
          if(fread(frec,1,FRAME_REC,tr) != FRAME_REC)
            {
              printf("FAIL: the trace ends at frame %lu\n",(unsigned long)fr);
              return 2;
            }
          /* The self-check of the frame path: one byte of the work RAM
             flipped before the digest is taken, once. */
          if(poke && fr == 0)
            z80_wmap[RAM_FIRST_PAGE][0] ^= 1U;
          frame_take(fgot);
          if(memcmp(fgot,frec,FRAME_CMP) != 0)
            {
              frame_format(a,sizeof a,fgot);
              frame_format(b,sizeof b,frec);
              block_name(name,sizeof name,get32(rec + 36));
              printf("z80c: MISMATCH frame %lu line %d block %s blocks=%lu "
                     ": %s / %s\n",
                     (unsigned long)fr,LINES_PER_FRAME - 1,name,
                     get16(rec + 34),a,b);
              return 1;
            }
          total_exec = (long)get32(frec + 28);
          if((fr % every) == 0 || fr == frames - 1)
            printf("z80c: frame %lu OK digest=%08lx exec=%lu fallback=%lu\n",
                   (unsigned long)fr,get32(frec),get32(frec + 28),
                   get32(frec + 32));
        }
    }

  if(recording)
    {
      uint32 z80c_exec, z80c_fallback, z80c_insns, z80c_ram;
      unsigned long i, hit = 0;

      if(fclose(tr) != 0)
        {
          fprintf(stderr,"cannot finish the trace %s\n",argv[5]);
          return 2;
        }
      z80c_counts(&z80c_exec,&z80c_fallback,&z80c_insns,&z80c_ram);
      printf("z80c: recorded %lu frames exec=%lu fallback=%lu insns=%lu ram_exec=%lu\n",
             (unsigned long)frames,(unsigned long)z80c_exec,
             (unsigned long)z80c_fallback,(unsigned long)z80c_insns,
             (unsigned long)z80c_ram);
      /* The hits per block: how many of the table ran at all, and the
         file the translator chooses from. */
      for(i = 0; i < z80c_block_count; i++)
        if(z80c_hits[i] != 0UL)
          hit++;
      printf("z80c: hits %lu/%lu blocks ran\n",hit,(unsigned long)z80c_block_count);
      /*
       * The chain seen to be followed. A block hands the core the entry
       * of its successor so that the next one runs with no lookup; when
       * that hand-off is lost -- a successor rendered as 0, an epoch
       * that never matches, a chain that returns after its first block
       * -- nothing else here changes: the core's loop finds every block
       * again through z80c_find and the two runs still agree, because
       * the interpreter is right either way. Only the time changes, and
       * the time is what this path exists for. So a run in which no
       * chain ever ran a second block is refused, the way a run in which
       * no block ran at all is refused: it proves the translation, not
       * the chain.
       */
      if(z80c_block_count != 0UL)
        {
          uint32 chains = z80c_chains();

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

  fclose(tr);
  printf("z80c: PASS %lu/%lu frames%s\n",(unsigned long)frames,
         (unsigned long)frames,
         total_exec == 0 ? " (no translated block ran)" : "");
  return 0;
}
