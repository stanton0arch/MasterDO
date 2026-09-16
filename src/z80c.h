#ifndef SMS3DO_Z80C_H
#define SMS3DO_Z80C_H

#include "common.h"

/*
 * Translated cartridge code: the C a host tool wrote out of the ROM, and
 * the small module that lets the processor core run it.
 *
 * The tool (tests/z80c/translate.c) reads the image once, on the PC, and
 * writes src/rom_code.c: one static function per block of instructions,
 * and a table from the block's POSITION IN THE CARTRIDGE to that function.
 * A position, never a Z80 address: the page tables of the core hold raw
 * pointers into the resident image (z80.h), the mapper moves them and the
 * core reads them, so at run time the page a PC falls on says where in the
 * cartridge the byte comes from, whatever bank the program has turned in.
 * Nothing is invalidated when a bank turns, a paged image is translated
 * exactly like a flat one, and the same block can run in whichever slot
 * the mapper put its bank. A block never crosses the edge of a 16k bank,
 * nor the edge of the first kilobyte of bank 0: the Sega mapper keeps that
 * kilobyte in place while it turns the rest of slot 0 (cart.c,
 * cart_mapper_project), so the bytes past it belong to whichever bank the
 * slot shows, and the tool stops there.
 *
 * THE TRANSLATED CODE KEEPS NO ACCOUNT OF TIME. A block holds no T-state,
 * no refresh register and no cap: it runs its instructions and hands back
 * its successor. What paces the program instead is WAITING. Once a table
 * is armed, a line of the picture is no longer a quota of T-states: the
 * core runs the program until it waits (z80_run_events, z80.h), the video
 * part counts the line, and the interrupt that line raised is taken at
 * the head of the next one. A program waits in two ways, and the tool
 * knows both when it writes the table: a HALT, which the interpreter
 * keeps, and a short loop that reads a fixed byte of work RAM, the status
 * port or the line counter of the video part, and writes nothing to
 * memory nor to a port. The entry of the block that starts such a loop
 * carries the wait flag, and arriving on it ends the line. A delay loop
 * that reads nothing is not a wait: it runs through at once.
 *
 * A program that never waits on a line never ends it, and on the console
 * nothing guards against that. A line the interpreter runs ends once the
 * largest quota a counter holds is spent -- minutes of Z80 time, on a
 * line; a line that runs entirely in translated blocks whose successors
 * are rendered never ends at all, the blocks spending nothing. That is
 * why the host runners refuse such a program on the PC, before the
 * console ever sees it (the guard below), and why the code that only a
 * pad reaches -- menus, later levels -- is not proved by the check: a
 * wait the tool missed there is a line that never ends, on the console.
 *
 * The block's contract. It is entered with the structure sms.z80 exact --
 * the core has flushed its resident window (z80.c) -- and PC on its first
 * byte. It loads the registers it reads into locals, executes its
 * instructions through the very macros of z80_ops.h the interpreter uses
 * -- the register names retargeted onto those locals for the length of the
 * generated file, the way z80_run retargets its five hot names -- stores
 * the registers it wrote, and leaves PC on the next instruction to run. It
 * ends on a transfer, on the instruction before one it does not translate,
 * on the edge of its bank, or in front of the start of another block; a
 * conditional branch may end it with two exits.
 *
 * WHAT A BLOCK RETURNS IS ITS SUCCESSOR: the table entry of the block
 * that starts where it left PC, when the tool knew it at emission -- the
 * linear continuation, the target of a jump, the callee of a call or a
 * restart, either exit of a conditional -- and null when it did not (a
 * return, a jump through a pair, a target no block starts at). An absolute
 * successor is rendered only while the block runs in the same 16k window
 * of the address space as its target, which the block tests at run time
 * against its own entry address: the same window is the same slot of the
 * mapper, so the target's bytes are the ones the tool translated. Every
 * other case goes through z80c_find, which reads the live page tables.
 * z80c_run chains the successors and stops in front of a wait, at a null
 * that finds no block, and asks the tables again after the mapper has
 * moved a page (z80c_map_epoch), when the static successor no longer
 * describes what the address holds.
 *
 * What a block does NOT translate, and leaves to the interpreter: HALT,
 * which ends the line; IM n, executed once at boot; LD A,R and LD R,A,
 * which read and write the refresh register the core holds; and any byte
 * the interpreter itself refuses. The block closes in front of such an
 * instruction, PC on it, and the instruction after it starts a block of
 * its own. Code in work RAM has no position: a program seen executing
 * there on the PC is refused by the runners (tests/z80c/sidebyside.c).
 *
 * The file the repository carries is the EMPTY table (tests/z80c/
 * rom_code_none.c): no block, size and digest zero. Nothing derived from a
 * cartridge is published; the human runs the tool locally, and the
 * generated file is ignored by git. With the empty table this module
 * never arms, the core runs the interpreter alone on its quotas of
 * T-states, saying so once at boot, and nothing below is reached.
 */

/*
 * A block: a function that takes nothing, works on the structure, and
 * returns the table entry of its successor, or null. The entry type and
 * the function type name each other, hence the forward declaration.
 */
typedef struct z80c_entry z80c_entry_t;
typedef const z80c_entry_t *(*z80c_fn)(void);

struct z80c_entry
{
  uint32  pos;  /* position of the block's first byte in the cartridge */
  z80c_fn fn;
  uint32  wait; /* 1 when the block starts a loop the program waits in */
};

/*
 * What the generated file publishes, and the whole of it: the size and
 * the FNV-1a digest of the image it was written from, how many bytes its
 * blocks cover, how many blocks, and the table sorted by position. The
 * empty table has one null entry so that the array exists.
 */
extern const uint32       z80c_rom_size;
extern const uint32       z80c_rom_fnv;
extern const uint32       z80c_code_bytes;
extern const uint32       z80c_block_count;
extern const z80c_entry_t z80c_table[];

/*
 * Raised by z80c_init when the table pairs with the loaded cartridge. The
 * frame loop reads it to choose how a line is run: armed, the core runs
 * the program until it waits (z80_run_events); down, the core spends a
 * quota of T-states on the interpreter (z80_run). The table is the switch
 * -- there is no build option to take this path out, so the benches link
 * what the console runs.
 */
extern uint8 z80c_armed;

/*
 * Stepped by the cartridge module each time the mapper moves a page
 * (cart.c, cart_mapper_write): a static successor rendered by a block is
 * trusted only while the count has not moved since the block was entered.
 */
extern uint32 z80c_map_epoch;

/*
 * The address z80c_run last looked up and found no block at, or
 * Z80C_NO_PC. The core's loop does not look that address up again while
 * PC stays on it: the instruction the interpreter is running there is
 * not a block's start, and a repeated block instruction keeps PC on one
 * address for many turns. The first other address is looked up as any
 * other, and the next miss moves the mark. The mark is not compared
 * against the epoch: a bank turned while PC stays put leaves it stale,
 * which costs a chain and never an answer -- a wait is always a block's
 * start, so a stale mark never hides one.
 */
#define Z80C_NO_PC 0xFFFFFFFFUL
extern uint32 z80c_miss_pc;

/*
 * The per-block counters of the host runner: two tables indexed like the
 * block table -- how often each block was entered, and the instructions
 * its exits ran -- filled by the generated code when the runner builds it
 * with Z80C_HITS=1 (tests/z80c/play.sh) and nothing on the console. The
 * generated file defines both tables, sized to its block count; the tool
 * chooses a table under a budget from what the runner writes of them.
 */
#ifndef Z80C_HITS
#define Z80C_HITS 0
#endif
#if Z80C_HITS
extern uint32 z80c_hits[];
extern uint32 z80c_ran[];
#define Z80C_HIT(k)    (z80c_hits[(k)]++)
#define Z80C_RAN(k, n) (z80c_ran[(k)] += (uint32)(n))
#else
#define Z80C_HIT(k)    ((void)0)
#define Z80C_RAN(k, n) ((void)0)
#endif

/*
 * The counters, all under the telemetry: blocks run (z80c_run), emitted
 * instructions run (each exit of a block adds the count it ran),
 * instructions the interpreter executed while a line was run by events,
 * and among those the ones whose page was not the image -- the page test
 * is the one z80c_find makes, written out so that the core's loop pays no
 * call for it. Compiles to nothing without the telemetry, and so do the
 * four counters.
 */
#ifndef Z80C_BENCH
#define Z80C_BENCH 0
#endif
#if LOG_ENABLE && SMS_TELEMETRY
extern uint32 z80c_insns_total;
extern uint32 z80c_fallback_total;
extern uint32 z80c_ram_total;
#define Z80C_INSNS(n) (z80c_insns_total += (n))
#define Z80C_INTERPRETED(pc)                                            \
  do                                                                    \
    {                                                                   \
      long z80c_off = (long)(z80_rmap[(uint16)(pc) >> Z80_PAGE_BITS]    \
                             - sms.cart.rom);                           \
                                                                        \
      z80c_fallback_total++;                                            \
      if(z80c_off < 0L || (uint32)z80c_off >= sms.cart.size)            \
        {                                                               \
          z80c_ram_total++;                                             \
          Z80C_RAM_SEEN(pc);                                            \
        }                                                               \
      else                                                              \
        Z80C_CART_SEEN((uint32)z80c_off + ((pc) & Z80_PAGE_MASK));     \
    }                                                                   \
  while(0)
void z80c_counts(uint32 *exec, uint32 *fallback, uint32 *insns,
                 uint32 *ram_exec);
uint32 z80c_chains(void);
#else
#define Z80C_INSNS(n) ((void)0)
#define Z80C_INTERPRETED(pc) ((void)0)
#endif

/*
 * Two switches of the host runners, and of them alone: Z80C_BENCH=1 on
 * the PC (tests/z80c/play.sh), never on the console.
 *
 *   z80c_no_exec   raised by a runner before the first line: the table
 *                  still says where the program waits, and a line still
 *                  ends there, but no block runs -- the interpreter
 *                  executes every instruction. That is the reference the
 *                  translated code is held to: the same clock, the same
 *                  waits, one semantics.
 *
 *   the guard      a line that runs more than Z80C_LINE_INSNS_MAX
 *                  instructions, translated and interpreted together,
 *                  never waits: the core ends the line there and raises
 *                  z80c_no_wait with the address it stopped on, and the
 *                  runner refuses the program -- never a pass. On the
 *                  console such a line simply goes on.
 *
 *   z80c_ram_pc    the first address the interpreter executed from a
 *                  page that is not the image, and z80c_last_pos the
 *                  position of the last cartridge code run before it, a
 *                  block's start or an interpreted instruction: a
 *                  program that runs code from RAM is refused by the
 *                  runner, which names both.
 *
 *   z80c_seen      one byte per position of the image, given by a
 *                  runner, or null: every position the interpreter
 *                  ENTERED -- ran an instruction from that did not
 *                  follow the last one -- is marked. Written out, the
 *                  marks are the seeds of the tool's next walk: the code
 *                  a program reaches through a table the walk cannot
 *                  read (tests/z80c/translate.c, --seeds).
 *
 *   z80c_ring      the last Z80C_RING entries, interpreted or a block's
 *                  start, in order. When the guard fires, the runner
 *                  reads the cycle the program was turning in off it
 *                  and names the cycle's lowest entry as the wait the
 *                  tool did not see -- a loop that waits through a call
 *                  it dispatches, which no shape names (sidebyside.c).
 *
 * All of them lean on the counters above, hence the telemetry.
 */
#if Z80C_BENCH
#if !(LOG_ENABLE && SMS_TELEMETRY)
#error "Z80C_BENCH needs the telemetry: the guard counts instructions with its counters"
#endif
#define Z80C_LINE_INSNS_MAX 16000000UL
extern uint8  z80c_no_exec;
extern uint8  z80c_no_wait;
extern uint32 z80c_no_wait_pc;
extern uint32 z80c_line_mark;
extern uint32 z80c_ram_pc;
extern uint32 z80c_last_pos;
extern uint8 *z80c_seen;
#define Z80C_RING 1024UL
extern uint32 z80c_ring[Z80C_RING];
extern uint32 z80c_ring_n;
#define Z80C_RING_PUSH(pos) \
  ((void)(z80c_ring[z80c_ring_n++ & (Z80C_RING - 1UL)] = (pos)))
#define Z80C_LINE_BEGIN() \
  (z80c_line_mark = z80c_insns_total + z80c_fallback_total)
#define Z80C_LINE_OVER() \
  ((uint32)(z80c_insns_total + z80c_fallback_total - z80c_line_mark) \
   > Z80C_LINE_INSNS_MAX)
/* The first address the interpreter ran outside the image, for the
   runner that refuses a program executing from RAM (Z80C_NO_PC: none). */
#define Z80C_RAM_SEEN(pc) \
  ((void)(z80c_ram_pc == Z80C_NO_PC ? (z80c_ram_pc = (uint32)(pc)) : 0UL))
/* A position of the image the interpreter ran from: the last one, and
   the mark for the seeds -- an ENTRY, not every instruction: a position
   one to four bytes past the last one is the next instruction of the
   same stretch, which the walk from the entry reaches by itself; any
   other is where a transfer the walk could not see landed. Marking
   every instruction would make every one a block start and break a
   wait loop into blocks of one instruction, which no mark could name. */
#define Z80C_CART_SEEN(pos) \
  do \
    { \
      uint32 z80c_p = (pos); \
      \
      if((uint32)(z80c_p - z80c_last_pos - 1UL) >= 4UL) \
        { \
          if(z80c_seen != NULL) \
            z80c_seen[z80c_p] = 1; \
          Z80C_RING_PUSH(z80c_p); \
        } \
      z80c_last_pos = z80c_p; \
    } \
  while(0)
/* A block entered: its position is the last cartridge code run, and an
   entry of the ring. */
#define Z80C_BLOCK_SEEN(pos) \
  do \
    { \
      z80c_last_pos = (pos); \
      Z80C_RING_PUSH(pos); \
    } \
  while(0)
#else
#define Z80C_RAM_SEEN(pc)    ((void)0)
#define Z80C_CART_SEEN(pos)  ((void)0)
#define Z80C_BLOCK_SEEN(pos) ((void)0)
#endif

/*
 * Pairs the table with the cartridge just loaded, after z80_reset. The
 * size alone is compared here and the digest is journaled: a table for
 * another size, or the empty one, leaves the core interpreting with a
 * WARN line. A table written from another image of the same size is not
 * told apart until the loader checks the digest; the host runners
 * (tests/cel8/romrun.c, tests/z80c/sidebyside.c), which have the digest
 * in hand, refuse it.
 */
void z80c_init(void);

/*
 * The block that starts at a Z80 address, if there is one: the page the
 * address falls on gives the position in the cartridge -- or none, when
 * the page is not the image (work RAM, cartridge RAM, the fixed pages) --
 * and the table is searched by halving. Returns NULL when no block starts
 * there. Reads the page tables, the image's base and size, and the table:
 * nothing of the core's resident window (z80.c).
 */
const z80c_entry_t *z80c_find(uint16 pc);

/*
 * Runs a block, then its successor -- the one the block rendered when the
 * mapper has not moved since, the one the page tables give otherwise --
 * for as long as a block starts at PC and that block is not a wait.
 * Called by the core, inside a line run by events, with sms.z80 exact;
 * returns with it exact, PC on the wait it stopped in front of, on the
 * next instruction to interpret, or on the address of a miss, left in
 * z80c_miss_pc. On the PC it also returns once the line has run past the
 * guard. The wait in front of which it stops is the core's to answer
 * (z80.c): a chain never ends a line by itself.
 */
void z80c_run(const z80c_entry_t *e);

/*
 * The periodic lines, at the pace of the [PERF] line, given the processor
 * share of a frame in tenths of a millisecond and the frames of the
 * window. With the interpreter alone: the ARM cycles per T-state that
 * share amounts to. With a table armed: the instructions a frame ran,
 * translated and interpreted, the ARM cycles per instruction, the part
 * of the instructions that ran translated, and the four counters as
 * differences since the last call. Compiles to nothing without the
 * telemetry.
 */
void z80c_report(uint32 z80_tenths_ms, uint32 frames);

/*
 * ---------------------------------------------------------------------------
 * For the generated file alone, which defines Z80C_BLOCK_FILE and includes
 * z80_ops.h before this header. Two things of the interpreter's macros are
 * not the translated code's:
 *
 *   the price    a repeated block instruction backs PC up and spends its
 *                surcharge (z80_ops.h, Z80_BLOCK_REPEAT); a block has no
 *                clock to spend it on, so the spending is compiled away
 *                for the length of the generated file;
 *
 *   the reads    written under names of this module, so that the text of
 *                the generated file names nothing of the interpreter's
 *                clock nor of its refresh register -- which a reader
 *                checks with one search -- while expanding to the very
 *                reads the interpreter makes.
 * ---------------------------------------------------------------------------
 */
#ifdef Z80C_BLOCK_FILE
#ifndef SMS3DO_Z80_OPS_H
#error "the generated file must include z80_ops.h before z80c.h: the spending is compiled away on its macros"
#endif
#undef Z80_SPEND
#define Z80_SPEND(n) ((void)0)
#define Z80C_RD8(addr)  Z80_RD8(addr)
#define Z80C_RD16(addr) Z80_RD16(addr)
#endif

#endif /* SMS3DO_Z80C_H */
