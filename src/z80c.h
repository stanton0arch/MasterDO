#ifndef SMS3DO_Z80C_H
#define SMS3DO_Z80C_H

#include "common.h"

/*
 * Translated cartridge code: the C a host tool wrote out of the ROM, and
 * the small module that lets the processor core run it.
 *
 * The tool (tests/z80c/translate.c) reads the image once, on the PC, and
 * writes src/rom_code.c: one static function per block of instructions it
 * could translate, and a table from the block's POSITION IN THE CARTRIDGE
 * to that function. A position, never a Z80 address: the page tables of
 * the core hold raw pointers into the resident image (z80.h), the mapper
 * moves them and the core reads them, so at run time the page a PC falls
 * on says where in the cartridge the byte comes from, whatever bank the
 * program has turned in. Nothing is invalidated when a bank turns, a paged
 * image is translated exactly like a flat one, and the same block can run
 * in whichever slot the mapper put its bank. A block never crosses the
 * edge of a 16k bank, nor the edge of the first kilobyte of bank 0: the
 * Sega mapper keeps that kilobyte in place while it turns the rest of
 * slot 0 (cart.c, cart_mapper_project), so the bytes past it belong to
 * whichever bank the slot shows, and the tool stops there.
 *
 * The block's contract. It is entered with the structure sms.z80 exact --
 * the core has flushed its resident window (z80.c) -- and PC on its first
 * byte. It executes its instructions through the very macros of z80_ops.h
 * the interpreter uses, on the fields of sms.z80, and leaves PC on the
 * next instruction to run, the T-state counter decremented by the sum of
 * what it executed and R ticked once per opcode. That sum is static: a
 * block never contains a conditional branch, so its price is known when
 * it is written. It closes as soon as the sum reaches Z80C_BLOCK_TSTATES,
 * which is what keeps the core's overrun bound one block wide.
 *
 * A block that meets an instruction the tool did not translate stops in
 * front of it: PC on that instruction, the T-states of what ran before it
 * spent, and the interpreter executes it -- the fallback. That the C came
 * from the same bytes the interpreter reads is what makes the two agree.
 *
 * The file the repository carries is the EMPTY table (tests/z80c/
 * rom_code_none.c): no block, size and digest zero. Nothing derived from a
 * cartridge is published; the human runs the tool locally, and the
 * generated file is ignored by git. With the empty table this module
 * never arms and the core runs the interpreter alone, saying so once at
 * boot.
 */

/* A block: a function that takes and returns nothing, the state being
   the structure it works on. */
typedef void (*z80c_fn)(void);

typedef struct
{
  uint32  pos; /* position of the block's first byte in the cartridge */
  z80c_fn fn;
} z80c_entry_t;

/*
 * The T-state sum a block closes at. The dearest instruction a block may
 * end on costs 17 (CALL), so a block spends at most 63 + 17 = 80 T-states,
 * and z80_run's overrun -- the T-states the last thing it ran spent past
 * the quota, that thing having been started with at least one T-state
 * left -- is at most 79, well inside a scanline of 228. z80.h states the
 * bound in the contract of z80_run; the tool applies the cap at emission,
 * reading it off this line (tests/z80c/translate.sh).
 */
#define Z80C_BLOCK_TSTATES 64

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
 * Pairs the table with the cartridge just loaded, after z80_reset. The
 * size alone is compared here and the digest is journaled: a table for
 * another size, or the empty one, leaves the core interpreting with a
 * WARN line. A table written from another image of the same size is not
 * told apart until the loader checks the digest; the host runner
 * (tests/cel8/romrun.c), which has the digest in hand, refuses it.
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
z80c_fn z80c_find(uint16 pc);

/*
 * Runs a block, then the block that follows it, for as long as the
 * quota holds and a block starts at PC. Called by the core with sms.z80
 * exact; returns with it exact, PC on the next instruction to interpret
 * or on the next block that the next quota will find.
 */
void z80c_run(z80c_fn fn);

/*
 * The periodic line, at the pace of the [PERF] line: blocks executed and
 * hand-backs to the interpreter since the last call. Compiles to nothing
 * without the telemetry, and so do the two counters.
 */
void z80c_report(void);

#if LOG_ENABLE && SMS_TELEMETRY
/* The two running totals since reset, for a host runner to print. */
void z80c_counts(uint32 *exec, uint32 *fallback);
#endif

#endif /* SMS3DO_Z80C_H */
