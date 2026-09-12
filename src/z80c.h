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
 * The block's contract. It is entered with the structure sms.z80 exact --
 * the core has flushed its resident window (z80.c) -- and PC on its first
 * byte. It loads the registers it reads into locals, executes its
 * instructions through the very macros of z80_ops.h the interpreter uses
 * -- the register names retargeted onto those locals for the length of the
 * generated file, the way z80_run retargets its five hot names -- stores
 * the registers it wrote, ticks R once per opcode read, leaves PC on the
 * next instruction to run and the T-state counter decremented by what it
 * spent. It ends on a transfer, on the instruction before one it does not
 * translate, on the edge of its bank, or on the cap below; a conditional
 * branch may end it with two exits, each charging its own price. The
 * price of every exit is static: the sum of the block plus the surcharge
 * of a taken branch, known when the block is written.
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
 * z80c_run chains the successors and stops at the quota, at a null, and
 * after the mapper has moved a page (z80c_map_epoch), when the static
 * successor no longer describes what the address holds.
 *
 * What a block does NOT translate, and leaves to the interpreter: HALT,
 * which consumes the quota without reading anything; IM n, executed once
 * at boot; LD A,R and LD R,A, which read and write the refresh register
 * that only the interpreter's flush composes; and any byte the interpreter
 * itself refuses. The block closes in front of such an instruction, PC on
 * it, and the instruction after it starts a block of its own. Code in
 * work RAM is interpreted by construction: it has no position.
 *
 * The file the repository carries is the EMPTY table (tests/z80c/
 * rom_code_none.c): no block, size and digest zero. Nothing derived from a
 * cartridge is published; the human runs the tool locally, and the
 * generated file is ignored by git. With the empty table this module
 * never arms and the core runs the interpreter alone, saying so once at
 * boot.
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
  uint32  pos; /* position of the block's first byte in the cartridge */
  z80c_fn fn;
};

/*
 * The two figures of the cap. A block closes as soon as the sum of the
 * DEAREST price of each of its instructions -- the taken branch, the
 * repeating iteration -- reaches the first, and never takes an instruction
 * that would carry that sum past the second: so a block spends at most
 * 80 T-states whatever path it takes, and z80_run's overrun -- the
 * T-states the last thing it ran spent past the quota, that thing having
 * been started with at least one T-state left -- is at most 79, well
 * inside a scanline of 228. z80.h states the bound in the contract of
 * z80_run; the tool applies both figures at emission, reading them off
 * these two lines (tests/z80c/translate.sh).
 */
#define Z80C_BLOCK_TSTATES     64
#define Z80C_BLOCK_TSTATES_MAX 80

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
 * Raised by z80c_init when the table pairs with the loaded cartridge, and
 * read by the core at the head of its loop: while it is down, the core
 * never looks for a block. The table is the switch -- there is no build
 * option to take this path out, so the benches link what the console
 * runs.
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
 * address for many turns (a halt does not reach that test; z80.c). The
 * first other address is looked up as any other, and the next miss moves
 * the mark. The mark is not compared against the epoch: a bank turned
 * while PC stays put leaves it stale, which costs a chain and never an
 * answer.
 */
#define Z80C_NO_PC 0xFFFFFFFFUL
extern uint32 z80c_miss_pc;

/*
 * The per-block counters of the host runner: two tables indexed like the
 * block table -- how often each block was entered, and the T-states its
 * exits charged -- filled by the generated code when the runner builds
 * it with Z80C_HITS=1 (tests/z80c/play.sh) and nothing on the console.
 * The generated file defines both tables, sized to its block count; the
 * tool chooses a table under a budget from what the runner writes of
 * them.
 */
#ifndef Z80C_HITS
#define Z80C_HITS 0
#endif
#if Z80C_HITS
extern uint32 z80c_hits[];
extern uint32 z80c_tstates[];
#define Z80C_HIT(k)      (z80c_hits[(k)]++)
#define Z80C_SPENT(k, n) (z80c_tstates[(k)] += (uint32)(n))
#else
#define Z80C_HIT(k)      ((void)0)
#define Z80C_SPENT(k, n) ((void)0)
#endif

/*
 * The counters, all under the telemetry: blocks run (z80c_run), emitted
 * instructions run (each exit of a block adds the count it ran),
 * instructions the interpreter executed while the table was armed, and
 * among those the ones whose page was not the image -- the page test is
 * the one z80c_find makes, written out so that the core's loop pays no
 * call for it. Compiles to nothing without the telemetry, and so do the
 * four counters.
 */
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
        z80c_ram_total++;                                               \
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
 * for as long as the quota holds and a block starts at PC. Called by the
 * core with sms.z80 exact; returns with it exact, PC on the next
 * instruction to interpret or on the next block that the next quota will
 * find, and z80c_miss_pc set when it returned on a miss.
 */
void z80c_run(const z80c_entry_t *e);

/*
 * The periodic lines, at the pace of the [PERF] line, given the processor
 * share of a frame in tenths of a millisecond: the ARM cycles per T-state
 * that share amounts to, the part of the instructions that ran translated
 * over the window, and, while the table is armed, the four counters as
 * differences since the last call. Compiles to nothing without the
 * telemetry.
 */
void z80c_report(uint32 z80_tenths_ms);

#endif /* SMS3DO_Z80C_H */
