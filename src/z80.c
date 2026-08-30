#include "z80.h"
#include "sms.h"
#include "vdp.h"
#include "z80_ops.h"
#include "sys.h"
#include "log.h"

/*
 * The emulated address space, and the whole of what this port keeps of it.
 * The processor state itself lives in the world structure as sms.z80
 * (sms.h): a global with a link-time constant address, which is exactly what
 * the file static it replaced was to the compiler, and which other modules
 * can now read. That this file is its only writer is a declared rule of
 * sms.h rather than something the language enforces; the hot names of
 * z80_ops.h expand to its fields directly, with no pointer in between.
 *
 * The two tables are published objects, declared in z80.h, and the
 * discipline that used to be a file static's is now a stated rule with one
 * writer: the cartridge module, which installs ROM, work RAM, its mirrors
 * and two fixed pages at boot (cart.c) -- projecting a bank there is
 * changing a pointer, never copying (TotalSMS/src/core/sms_bus.c:50-70).
 * Past boot the tables are read-only for everyone, this file included:
 * the mapper turns banks through the cartridge module's own code, and any
 * other write of an entry is a defect.
 *
 * Table shape: TotalSMS/src/core/sms_types.h:399-408, where the read side is
 * declared const and the write side is not. The const is not decoration: it is
 * a compiler-checked statement that nothing writes emulated memory through a
 * read mapping, and it costs nothing to hold.
 *
 * This file allocates nothing: the address space is the ROM already
 * resident plus the 8k the cartridge module allocates.
 */
const uint8 *z80_rmap[Z80_PAGE_COUNT];
uint8 *z80_wmap[Z80_PAGE_COUNT];

/*
 * Raised the first time the core meets an opcode it cannot execute, and never
 * lowered except by a reset.
 *
 * It exists because an incomplete instruction set is the expected state of this
 * stage rather than a failure. Without it the frame loop would spend every
 * quota of every scanline re-reading the same byte and re-deciding it cannot
 * run it, for as long as the console is on -- which costs the whole of the
 * measurement the loop exists to produce, and buries the one line that says
 * what is missing under the frames that follow it.
 */
static int32 z80_stopped = 0;

/*
 * ---------------------------------------------------------------------------
 * Reset values, and where each of them comes from -- because they do not all
 * come from the same kind of place.
 *
 * PC at zero is the processor's own behaviour. The stack pointer is not: the
 * value below is what TotalSMS sets, with the comment that it fixes two
 * particular games (sms_z80.c:2168), so it is a compatibility choice made by
 * an emulator author and not a documented property of the part. It is kept
 * because it is the only value any source in this repository gives, and it is
 * written down as what it is rather than as something Zilog said.
 *
 * A at 0xFF with all eight flags set -- AF = 0xFFFF -- comes from the same
 * place (sms_z80.c:2169-2177). No document under docs/sms_gg/ states the reset
 * value of the general registers, so TotalSMS is the source here, and the
 * whole of it.
 *
 * None of this matters much to the test program that exercises this core: it
 * loads a stack pointer of its own at its third instruction. It matters to
 * whoever reads the first trace and wonders where 0xDFF0 came from.
 * ---------------------------------------------------------------------------
 */
#define Z80_RESET_SP 0xDFF0
#define Z80_RESET_A  0xFF
#define Z80_RESET_F  (Z80_FLAG_C | Z80_FLAG_N | Z80_FLAG_PV | Z80_FLAG_B3 |  \
                      Z80_FLAG_H | Z80_FLAG_B5 | Z80_FLAG_Z | Z80_FLAG_S)

/*
 * ---------------------------------------------------------------------------
 * What each unprefixed opcode costs, in T-states, indexed by the opcode.
 * Table: TotalSMS/src/core/sms_z80.c:13-31.
 *
 * Two entries in three read differently from what they look like, and a
 * dispatch that took them at face value would keep perfect behaviour and lose
 * the clock -- which is the half of this core that the frame rate is made of.
 *
 *   The four prefix bytes cost zero here. Their real price is carried by the
 *   table of the prefix they open, so the cost of a prefixed instruction is the
 *   sum of two lookups and never of one.
 *
 *   Every conditional instruction holds the price of the case that is NOT
 *   taken. The taken case adds its surcharge in the operation itself -- seven
 *   for a call, six for a return, five for a relative jump and for the counted
 *   loop (sms_z80.c:802-810, :822-828, :845-875).
 *
 * The values are transcribed by counting values and never by counting lines.
 * The source's own CB table is correct in its 256 values and irregular in its
 * layout, seventeen of them sitting on its first line: a transcription that
 * took a line for a row would place the first sixteen entries correctly and
 * displace the two hundred and forty that follow, one place each, and nothing
 * about the result would look wrong.
 *
 * THE DISPATCH DOES NOT READ THIS TABLE PER INSTRUCTION. Every case of the
 * switch spends its entry as a literal of its own, transcribed from here
 * value by value under the rule above, because a lookup cost two loads on
 * the path every instruction walks -- see the comment at the switch. What
 * still reads it is the dispatch's default alone: the stop path spends the
 * entry of a byte it has no case for, so that the counter it flushes says
 * what execution would have said. The table stays whole as the reference
 * the literals transcribe -- a literal that drifts from its entry is a
 * defect of the case that carries it, never of this table.
 * ---------------------------------------------------------------------------
 */
static const uint8 z80_cycles[256] =
{
  /* 0x00 */ 0x04, 0x0A, 0x07, 0x06, 0x04, 0x04, 0x07, 0x04, 0x04, 0x0B, 0x07, 0x06, 0x04, 0x04, 0x07, 0x04,
  /* 0x10 */ 0x08, 0x0A, 0x07, 0x06, 0x04, 0x04, 0x07, 0x04, 0x0C, 0x0B, 0x07, 0x06, 0x04, 0x04, 0x07, 0x04,
  /* 0x20 */ 0x07, 0x0A, 0x10, 0x06, 0x04, 0x04, 0x07, 0x04, 0x07, 0x0B, 0x10, 0x06, 0x04, 0x04, 0x07, 0x04,
  /* 0x30 */ 0x07, 0x0A, 0x0D, 0x06, 0x0B, 0x0B, 0x0A, 0x04, 0x07, 0x0B, 0x0D, 0x06, 0x04, 0x04, 0x07, 0x04,
  /* 0x40 */ 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x07, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x07, 0x04,
  /* 0x50 */ 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x07, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x07, 0x04,
  /* 0x60 */ 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x07, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x07, 0x04,
  /* 0x70 */ 0x07, 0x07, 0x07, 0x07, 0x07, 0x07, 0x04, 0x07, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x07, 0x04,
  /* 0x80 */ 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x07, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x07, 0x04,
  /* 0x90 */ 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x07, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x07, 0x04,
  /* 0xA0 */ 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x07, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x07, 0x04,
  /* 0xB0 */ 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x07, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x07, 0x04,
  /* 0xC0 */ 0x05, 0x0A, 0x0A, 0x0A, 0x0A, 0x0B, 0x07, 0x0B, 0x05, 0x0A, 0x0A, 0x00, 0x0A, 0x11, 0x07, 0x0B,
  /* 0xD0 */ 0x05, 0x0A, 0x0A, 0x0B, 0x0A, 0x0B, 0x07, 0x0B, 0x05, 0x04, 0x0A, 0x0B, 0x0A, 0x00, 0x07, 0x0B,
  /* 0xE0 */ 0x05, 0x0A, 0x0A, 0x13, 0x0A, 0x0B, 0x07, 0x0B, 0x05, 0x04, 0x0A, 0x04, 0x0A, 0x00, 0x07, 0x0B,
  /* 0xF0 */ 0x05, 0x0A, 0x0A, 0x04, 0x0A, 0x0B, 0x07, 0x0B, 0x05, 0x06, 0x0A, 0x04, 0x0A, 0x00, 0x07, 0x0B,
};

/*
 * ---------------------------------------------------------------------------
 * What each opcode of the ED prefix costs, in T-states, indexed by the byte
 * behind the prefix.
 * Table: TotalSMS/src/core/sms_z80.c:33-51.
 *
 * The four T-states of reading the prefix itself are already inside these
 * values -- the unprefixed table holds zero for the prefix byte, so the whole
 * price of a prefixed instruction comes from here and nowhere else. LDI at
 * sixteen and LD (nn),BC at twenty are the check: both are the complete cost
 * of the two-byte instruction.
 *
 * The repeated block instructions hold the price of their LAST iteration, the
 * one that does not repeat. Every earlier iteration adds five, in the operation
 * itself, the same way a taken branch adds its surcharge.
 *
 * Transcribed by counting values and never by counting lines. This table is
 * regularly laid out, sixteen to a line, where the CB table of the same source
 * is not; the habit is kept anyway, because the day it is dropped is the day it
 * is needed.
 *
 * NOT READ PER INSTRUCTION: every implemented case of the prefix's dispatch
 * spends its entry as a literal, transcribed from here value by value, for
 * the reason the unprefixed table states. Its one remaining reader is that
 * dispatch's default, which spends the entry of a byte it has no case for
 * before stopping. The table stays whole as the reference the literals
 * transcribe.
 * ---------------------------------------------------------------------------
 */
static const uint8 z80_cycles_ed[256] =
{
  /* 0x00 */ 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08,
  /* 0x10 */ 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08,
  /* 0x20 */ 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08,
  /* 0x30 */ 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08,
  /* 0x40 */ 0x0C, 0x0C, 0x0F, 0x14, 0x08, 0x0E, 0x08, 0x09, 0x0C, 0x0C, 0x0F, 0x14, 0x08, 0x0E, 0x08, 0x09,
  /* 0x50 */ 0x0C, 0x0C, 0x0F, 0x14, 0x08, 0x0E, 0x08, 0x09, 0x0C, 0x0C, 0x0F, 0x14, 0x08, 0x0E, 0x08, 0x09,
  /* 0x60 */ 0x0C, 0x0C, 0x0F, 0x14, 0x08, 0x0E, 0x08, 0x12, 0x0C, 0x0C, 0x0F, 0x14, 0x08, 0x0E, 0x08, 0x12,
  /* 0x70 */ 0x0C, 0x0C, 0x0F, 0x14, 0x08, 0x0E, 0x08, 0x08, 0x0C, 0x0C, 0x0F, 0x14, 0x08, 0x0E, 0x08, 0x08,
  /* 0x80 */ 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08,
  /* 0x90 */ 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08,
  /* 0xA0 */ 0x10, 0x10, 0x10, 0x10, 0x08, 0x08, 0x08, 0x08, 0x10, 0x10, 0x10, 0x10, 0x08, 0x08, 0x08, 0x08,
  /* 0xB0 */ 0x10, 0x10, 0x10, 0x10, 0x08, 0x08, 0x08, 0x08, 0x10, 0x10, 0x10, 0x10, 0x08, 0x08, 0x08, 0x08,
  /* 0xC0 */ 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08,
  /* 0xD0 */ 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08,
  /* 0xE0 */ 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08,
  /* 0xF0 */ 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08,
};

/*
 * ---------------------------------------------------------------------------
 * What each opcode of the CB prefix costs, in T-states, indexed by the byte
 * behind the prefix.
 * Table: TotalSMS/src/core/sms_z80.c:73-91.
 *
 * As with the block prefix, the whole price of the two byte instruction is
 * here: the unprefixed table holds zero for the prefix byte itself. Every form
 * that names a register costs eight, every form that reaches the byte HL points
 * at costs twelve to test a bit and fifteen to change one -- the difference
 * between a read and a read-modify-write, priced.
 *
 * TRANSCRIBED BY COUNTING VALUES, AND THIS IS THE TABLE THAT MAKES THE RULE
 * WORTH HAVING. The source lays this one out irregularly: seventeen values on
 * its first line, fifteen on its last, sixteen on the fourteen in between. A
 * transcription that took a line for a row would get the first sixteen entries
 * right and displace the two hundred and forty that follow, one place each.
 * Only sixty of them would end up holding a wrong number, because eight
 * dominates this table and the rest fall back onto their own value by
 * coincidence -- which is what makes the mistake so quiet. The compiler would
 * say nothing, and sixty wrong timings scattered through the prefix is also
 * what the next paragraph would produce, so the two would be impossible to
 * tell apart afterwards. Four checks separate them: [0x06] is 15, [0x46] is
 * 12, [0x86] is 15, [0xFF] is 8. All four hold below.
 *
 * THE SOURCE MARKS THIS TABLE AS APPROXIMATE, in its own words, where it
 * charges it (sms_z80.c:1526). No document under docs/sms_gg/ gives instruction
 * timings, so there is nothing of higher standing to check it against and it
 * is taken as it stands. What follows from that is a rule about later: should a
 * timing test fail while results and flags are right, this table is the first
 * suspect and correcting it is a question to put rather than a number to
 * adjust. A table tuned until the tests stop complaining is a wrong table that
 * no longer says so.
 *
 * ITS ONE READER IS THE MEMORY-OPERAND CASE of the prefix's decode, the one
 * place a cost cannot ride as a literal: the price of the byte HL points at
 * is irregular -- twelve to test a bit, fifteen to change one -- and not
 * known before the operation field is decoded. The seven register forms all
 * cost eight and each carries that literal itself, for the reason the
 * unprefixed table states; the table stays whole as the reference both
 * kinds transcribe.
 * ---------------------------------------------------------------------------
 */
static const uint8 z80_cycles_cb[256] =
{
  /* 0x00 */ 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x0F, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x0F, 0x08,
  /* 0x10 */ 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x0F, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x0F, 0x08,
  /* 0x20 */ 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x0F, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x0F, 0x08,
  /* 0x30 */ 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x0F, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x0F, 0x08,
  /* 0x40 */ 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x0C, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x0C, 0x08,
  /* 0x50 */ 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x0C, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x0C, 0x08,
  /* 0x60 */ 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x0C, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x0C, 0x08,
  /* 0x70 */ 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x0C, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x0C, 0x08,
  /* 0x80 */ 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x0F, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x0F, 0x08,
  /* 0x90 */ 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x0F, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x0F, 0x08,
  /* 0xA0 */ 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x0F, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x0F, 0x08,
  /* 0xB0 */ 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x0F, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x0F, 0x08,
  /* 0xC0 */ 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x0F, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x0F, 0x08,
  /* 0xD0 */ 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x0F, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x0F, 0x08,
  /* 0xE0 */ 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x0F, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x0F, 0x08,
  /* 0xF0 */ 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x0F, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x0F, 0x08,
};

/*
 * ---------------------------------------------------------------------------
 * What each opcode behind an index prefix costs, in T-states, indexed by the
 * byte behind the prefix. The two prefixes share it: they select which pair an
 * instruction works on and change nothing else, timing included.
 * Table: TotalSMS/src/core/sms_z80.c:53-71.
 *
 * The whole price of the two byte instruction is here, the unprefixed table
 * holding zero for both prefix bytes. Three checks on that: an eight bit load
 * from a half of the pair costs 8 at [0x44], the same load from the displaced
 * byte costs 19 at [0x46], and the increment of the whole pair costs 10 at
 * [0x23] -- all three being complete instructions and not surcharges.
 *
 * TWO KINDS OF FOUR LIVE IN THIS TABLE AND THEY ARE NOT THE SAME THING. Where
 * the byte behind the prefix names an instruction that works on the pair or on
 * one of its halves, four is that instruction's whole price. Where it names an
 * instruction that mentions neither H nor L nor the byte HL points at, the
 * prefix has nothing to substitute, four is the price of the prefix alone, and
 * the instruction behind it then charges its own out of the unprefixed table --
 * so such a pair of bytes costs eight, four and four, and not four.
 *
 * The double prefix is the third case: [0xCB] holds zero, and what it opens has
 * no table at all. See the two constants below.
 *
 * Transcribed by counting values and never by counting lines, the habit the CB
 * table above earned. Four checks against the source: [0x09] is 15, [0x34] is
 * 23, [0x70] is 19, [0xE3] is 23.
 *
 * NOT READ PER INSTRUCTION: every case of the prefix's dispatch spends its
 * entry as a literal, transcribed from here value by value, and the inert
 * path spends the literal four -- every inert entry holds exactly that, the
 * price of the prefix alone, so the literal and the table cannot disagree
 * without the table having changed. Its one remaining reader is the
 * dispatch's default, which spends the entry of a byte it has no case for
 * before stopping. The table stays whole as the reference the literals
 * transcribe.
 * ---------------------------------------------------------------------------
 */
static const uint8 z80_cycles_ddfd[256] =
{
  /* 0x00 */ 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x0F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04,
  /* 0x10 */ 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x0F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04,
  /* 0x20 */ 0x04, 0x0E, 0x14, 0x0A, 0x08, 0x08, 0x0B, 0x04, 0x04, 0x0F, 0x14, 0x0A, 0x08, 0x08, 0x0B, 0x04,
  /* 0x30 */ 0x04, 0x04, 0x04, 0x04, 0x17, 0x17, 0x13, 0x04, 0x04, 0x0F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04,
  /* 0x40 */ 0x04, 0x04, 0x04, 0x04, 0x08, 0x08, 0x13, 0x04, 0x04, 0x04, 0x04, 0x04, 0x08, 0x08, 0x13, 0x04,
  /* 0x50 */ 0x04, 0x04, 0x04, 0x04, 0x08, 0x08, 0x13, 0x04, 0x04, 0x04, 0x04, 0x04, 0x08, 0x08, 0x13, 0x04,
  /* 0x60 */ 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x13, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x13, 0x08,
  /* 0x70 */ 0x13, 0x13, 0x13, 0x13, 0x13, 0x13, 0x04, 0x13, 0x04, 0x04, 0x04, 0x04, 0x08, 0x08, 0x13, 0x04,
  /* 0x80 */ 0x04, 0x04, 0x04, 0x04, 0x08, 0x08, 0x13, 0x04, 0x04, 0x04, 0x04, 0x04, 0x08, 0x08, 0x13, 0x04,
  /* 0x90 */ 0x04, 0x04, 0x04, 0x04, 0x08, 0x08, 0x13, 0x04, 0x04, 0x04, 0x04, 0x04, 0x08, 0x08, 0x13, 0x04,
  /* 0xA0 */ 0x04, 0x04, 0x04, 0x04, 0x08, 0x08, 0x13, 0x04, 0x04, 0x04, 0x04, 0x04, 0x08, 0x08, 0x13, 0x04,
  /* 0xB0 */ 0x04, 0x04, 0x04, 0x04, 0x08, 0x08, 0x13, 0x04, 0x04, 0x04, 0x04, 0x04, 0x08, 0x08, 0x13, 0x04,
  /* 0xC0 */ 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x00, 0x04, 0x04, 0x04, 0x04,
  /* 0xD0 */ 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04,
  /* 0xE0 */ 0x04, 0x0E, 0x04, 0x17, 0x04, 0x0F, 0x04, 0x04, 0x04, 0x08, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04,
  /* 0xF0 */ 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x0A, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04,
};

/*
 * What an instruction behind the double prefix costs, and THESE TWO NUMBERS
 * COME FROM NO TABLE (sms_z80.c:1552, :1561). Nobody should go looking for one:
 * the unprefixed table holds zero for the index prefixes, the indexed table
 * holds zero for the byte that opens this path, and the price stops there.
 *
 * Two values and not one, because this path holds the only instructions of the
 * prefix that differ in what they do with their result: ten of the eleven write
 * one back and pay for the write, the bit test writes none and does not.
 */
#define Z80_DDFD_CB_RMW_CYCLES 23
#define Z80_DDFD_CB_BIT_CYCLES 20

/*
 * ---------------------------------------------------------------------------
 * The input and output space.
 *
 * The decoding is the bus's (CART_IO_READ / CART_IO_WRITE, cart.h): the
 * Master System's port map on two bits, each destination a named hook that
 * the video, sound and input parts take over when they exist, and that
 * until then answers 0xFF, drops the byte, and is counted -- never traced
 * per access, the serial output being what it is. The bus module owns that
 * text and these two functions are its only expansion sites; nothing here
 * decodes a port on its own.
 *
 * ONE OBLIGATION. Both functions are called from inside z80_run's resident
 * window, where the five hot fields of sms.z80 -- pc, tstates, r, a, f --
 * are stale: neither function, nor anything the two macros expand to, may
 * read or write processor state through the structure. The accumulator
 * crosses by value, in the argument and in the return, and that is the
 * whole of the traffic. The bus's own fields (sms.cart) are not the
 * processor's and are free to be touched here. A hook that one day needs
 * more either keeps this property or the call sites in z80_run grow a
 * flush/reload boundary of their own.
 * ---------------------------------------------------------------------------
 */
static void
z80_io_write(uint8 port,
             uint8 value)
{
  CART_IO_WRITE(port,value);
}

static uint8
z80_io_read(uint8 port)
{
  uint8 value;

  CART_IO_READ(port,value);

  return value;
}

/*
 * Where the instruction currently being decoded begins.
 *
 * The counter has already stepped past the byte that was fetched, so the
 * address the trace has to name is one behind it, wrapped inside the emulated
 * address space rather than borrowed from the host's wider arithmetic. The
 * whole expression is evaluated at the width it is read at, which is what keeps
 * the compiler from remarking that a sixteen bit subtraction is being widened.
 *
 * Z80_PC has two homes, so this macro does too. Every use of it today sits
 * inside z80_run, where it reads the resident local and is exact; anything
 * moved across a residency boundary must re-check which home is live where
 * it lands.
 */
#define Z80_OP_ADDR ((((unsigned long)Z80_PC) - 1UL) & 0xFFFFUL)

Err
z80_init(void)
{
  /*
   * Nothing to allocate and nothing to fill: the tables stay empty until
   * the cartridge module installs them at boot (cart.c), the one moment
   * the ROM they must point at is known. An identity fill of a block that
   * does not exist would only manufacture entries the installation must
   * then be trusted to overwrite.
   *
   * The three quantities that define the access path are traced rather than
   * assumed. They are fixed at compile time and can be read off this file, but
   * a run that says which shape it was built with is what lets a later trace
   * be believed.
   */
  LOG_INFO(LOG_CAT_Z80,("init ok pagetable gran=%lu entries=%lu",
                        (unsigned long)Z80_PAGE_SIZE,
                        (unsigned long)Z80_PAGE_COUNT));

  /*
   * No reset here. The one reset of a boot happens after a program is at
   * the bottom of the address space -- the cartridge boot sequence holds
   * it (z80.h) -- and a second one before it would trace a state nothing
   * was about to run.
   */
  return 0;
}

void
z80_reset(void)
{
  /*
   * Every field, including the ones nothing reads yet. A field left alone
   * until the day it acquires a meaning is a defect that surfaces far from
   * where it was made, and reset is the one place that can be checked at a
   * glance for completeness.
   * Values: TotalSMS/src/core/sms_z80.c:2164-2182.
   */
  Z80_A = Z80_RESET_A;
  Z80_F = Z80_RESET_F;
  Z80_B = 0;
  Z80_C = 0;
  Z80_D = 0;
  Z80_E = 0;
  Z80_H = 0;
  Z80_L = 0;

  /*
   * The alternate set is not given reset values by any source here. It is
   * cleared rather than left as it lies, so that a run starts from a state
   * that can be described in one sentence.
   */
  Z80_A_ALT = 0;
  Z80_F_ALT = 0;
  Z80_B_ALT = 0;
  Z80_C_ALT = 0;
  Z80_D_ALT = 0;
  Z80_E_ALT = 0;
  Z80_H_ALT = 0;
  Z80_L_ALT = 0;

  Z80_PC = 0x0000;
  Z80_SP = Z80_RESET_SP;

  Z80_IXH = 0;
  Z80_IXL = 0;
  Z80_IYH = 0;
  Z80_IYL = 0;

  Z80_I = 0;
  Z80_R = 0;

  Z80_IFF1 = 0;
  Z80_IFF2 = 0;
  Z80_EI_DELAY = 0;
  Z80_HALTED = 0;
  Z80_NMI_SEEN = 0;
  Z80_IM = 0;

  /*
   * The clock starts at zero, and so does the refusal to execute: a reset that
   * left the core declining every instruction would not be a reset. What the
   * flag records is a gap in this port rather than a state of the processor,
   * which is why it is cleared here and mentioned nowhere in the trace below.
   */
  Z80_TSTATES = 0;
  z80_stopped = 0;

#if LOG_ENABLE && SMS_TELEMETRY
  sms.z80.irq_accepted = 0;
#endif

  /*
   * PC and SP are what the reader of a boot needs to see -- the entry point
   * the run starts from, and where the stack lives until the program moves
   * it. At INFO because there is exactly one reset per boot and it is a
   * boot-sequence event, not detail: the line the cartridge boot is read by
   * sits between the map installation and the first frame. AF is not on the
   * line any more; whoever needs the register file has the debug trace.
   */
  LOG_INFO(LOG_CAT_Z80,("reset pc=0x%04lx sp=0x%04lx",
                        (unsigned long)Z80_PC,
                        (unsigned long)Z80_SP));
}

/*
 * Records which interrupt mode a program has selected, and says so once per
 * mode.
 *
 * Nothing is emulated here: what a mode does is the business of the
 * acceptance stage at the head of z80_run, which branches on mode 1 and
 * refuses modes 0 and 2 loudly -- no authorised source describes those two,
 * the selector of TotalSMS being an empty function carrying an assertion
 * that the mode is 1 (sms_z80.c:1057-1061), which is a statement about the
 * machine rather than about the processor. So all three modes are recorded,
 * because the processor has three and a program is free to pick any of them,
 * and what an unimplemented one costs is decided where it would be used.
 *
 * The trace is bounded to one line per mode for the whole run, and it is a
 * mask of what has already been said rather than a comparison against the
 * mode in force. Two things follow, and neither is free.
 *
 *   A comparison would emit a line every time a program alternated between
 *   two modes, which it is entitled to do in a loop. The serial trace blocks:
 *   a line per execution would not slow that program down, it would replace
 *   it. The mask bounds the whole run to three lines whatever the program
 *   does.
 *
 *   A comparison would also say nothing at all when a program's first act is
 *   to select mode 0, since that is the mode reset leaves behind -- and
 *   "chose mode 0" would read exactly like "never chose a mode". The mask
 *   tells them apart, which is the only reason the first line is worth
 *   emitting.
 *
 * That the machine itself only ever uses mode 1 is written in its own
 * documentation (docs/sms_gg/SMSOfficialDocs.md:182,
 * docs/sms_gg/GGOfficialDocs.md:155 and :1019), and is a fact about what the
 * console does, not about what the processor can be told to do.
 */
static void
z80_set_im(uint8 mode)
{
  /*
   * One bit per mode already announced. It is never cleared, not even by a
   * reset: what it bounds is the amount of trace a single run can produce,
   * and a program that resets the processor has not made the modes it already
   * selected any more worth repeating.
   */
  static uint8 z80_im_said = 0;

  uint8 bit = (uint8)(1U << (mode & 3U));

  Z80_IM = mode;

  if((z80_im_said & bit) != 0)
    return;

  z80_im_said = (uint8)(z80_im_said | bit);

  LOG_INFO(LOG_CAT_Z80,("im mode=%lu",(unsigned long)mode));
}

#if SMS_IRQ_TEST_SOURCE
/*
 * ---------------------------------------------------------------------------
 * The test source of the non-maskable line, and it is scaffolding: a stub
 * of the read-only line interface of z80.h, kept to its own state and its
 * one accessor so that substituting the real owner -- the Pause button --
 * means retargeting the Z80_NMI_LINE macro below and deleting this block,
 * without touching the sampling in z80_run. It lives in this file because
 * no module owns that line yet, and a file created for a stand-in would
 * outlive it.
 *
 * It used to hold the maskable line as well, on a cadence of its own; that
 * line now has its owner, the video part, and the sampling reads the
 * device's macro (vdp.h) -- the substitution this block was written to
 * make easy, made once. What is left is one line and one counter.
 *
 * Its one notion of time is being interrogated, which the frame loop does
 * once per scanline; the counter below therefore counts scanlines, and
 * the accessor is the one place it advances -- called exactly once per
 * sampling, which is how a source with no clock of its own keeps time.
 *
 * The cadence is a documented choice, not a property of anything real:
 * the line rises once per 120 frames, two emulated seconds. A Pause
 * handler of the kind the instruction exerciser has spins until the NEXT
 * non-maskable interrupt lets it out
 * (docs/sms_gg/ZEXALL-SMS-0.21/source/zexall.sms.asm:210-224), so every
 * pulse both enters and leaves such a handler, and two seconds keeps the
 * accepted-line trace readable where a per-frame pulse would flood a
 * blocking serial port.
 * ---------------------------------------------------------------------------
 */
#define Z80_TEST_NMI_PERIOD (262UL * 120UL)

static uint32 z80_test_line_count = 0;

/*
 * Header: z80.h, where the shape of this call -- a read of a line the source
 * owns, never a write into the processor -- is written out.
 */
uint8
z80_nmi_line(void)
{
  z80_test_line_count++;

  return (uint8)((z80_test_line_count % Z80_TEST_NMI_PERIOD) == 0UL);
}
#endif /* SMS_IRQ_TEST_SOURCE */

/*
 * What the sampling stage of z80_run reads. The maskable line is the video
 * part's, in every build: a macro over its two pending requests and two
 * enable bits (vdp.h), no call behind it. The non-maskable line is the test
 * source's accessor while that scaffold is armed, and a line held low
 * otherwise -- a constant the compiler folds, taking the acceptance branch
 * and its trace text with it, which is the proof the size of the binary
 * gives. The module that brings the real owner retargets the macro for its
 * line and leaves the sampling alone.
 */
#define Z80_IRQ_LINE() VDP_IRQ_LINE()

#if SMS_IRQ_TEST_SOURCE
#define Z80_NMI_LINE() z80_nmi_line()
#else
#define Z80_NMI_LINE() 0
#endif

/*
 * ---------------------------------------------------------------------------
 * Whether an index prefix has anything to bite on.
 *
 * The prefix says one thing and one thing only: where this instruction would
 * have said HL, let it say IX -- or IY. Applied to the eight bit loads, three
 * cases follow. The prefix can land on the H or the L of the instruction, and it
 * names a half of the index register. It can land on the byte HL points at, and
 * the instruction gains a displacement byte and reaches memory. Or the
 * instruction names none of the three, and there is nothing to substitute: the
 * prefix is inert and the instruction runs exactly as it would have alone.
 *
 * Which is what this answers, by the two three-bit fields of the byte: a load
 * whose source and whose destination are both outside {H, L, (HL)}. HALT falls
 * outside on purpose -- both of its fields read as the byte HL points at, so the
 * prefix does have something to land on there and no source here says what it
 * makes of it.
 *
 * TWO CONSEQUENCES, AND THE SECOND IS A TRAP OF LENGTH. The prefix is read
 * and paid for, at its own price of four T-states, so it is not free. And NO
 * DISPLACEMENT BYTE IS CONSUMED: the instruction is two bytes long and not
 * three. A core that swallowed a displacement that was never emitted would
 * skip the byte behind it and read the one after that as an opcode, and the
 * symptom would appear a long way from the cause. The deposited exerciser
 * sweeps this range deliberately, says so in its own comment, and places a
 * nought behind each case that it describes as executed rather than absorbed
 * (docs/sms_gg/ZEXALL-SMS-0.21/source/zexall.sms.asm:1426-1439). Reasoning and
 * reservations: docs/pseudocodes/40_z80_undocumented.md, section 2.
 *
 * The rule is stated for the range of the eight bit loads, which is the range
 * the exerciser covers. Outside it, a byte the indexed dispatch has no case for
 * stops the core and names itself, which is what the source of the semantics
 * does with the same bytes.
 *
 * These two follow the convention of the access macros and not that of the
 * operations: THEY EVALUATE THEIR ARGUMENT MORE THAN ONCE -- the field twice,
 * the opcode three times. Pass a plain value, never an expression with a side
 * effect and never a read that moves PC, or the byte would be fetched three
 * times and the instruction stream would be three bytes further on than anyone
 * writing the call had in mind.
 * ---------------------------------------------------------------------------
 */
#define Z80_DDFD_FIELD_INDEXED(f) (((f) >= 0x4U) && ((f) <= 0x6U))

#define Z80_DDFD_INERT(op)                              \
  ((((op) & 0xC0U) == 0x40U) &&                         \
   !Z80_DDFD_FIELD_INDEXED(((op) >> 3) & 0x07U) &&      \
   !Z80_DDFD_FIELD_INDEXED((op) & 0x07U))

/*
 * ---------------------------------------------------------------------------
 * The hot half of the processor state, register-resident inside z80_run.
 *
 * PC, the T-state counter, R, A and F live in memory as fields of sms.z80,
 * and a field of a global structure has one property no keyword can remove:
 * any store through a uint8 pointer into host memory -- z80_wmap outside
 * this window, the resident base z80_run_mem inside it -- could alias it,
 * as far as the compiler can prove, so every emulated memory write forces
 * all five back to memory and reloads them afterwards -- on a path that
 * fetches an opcode, moves PC, ticks R and spends T-states for every
 * single instruction. A local whose
 * address is never taken cannot be aliased by any store, so the compiler may
 * hold it in a machine register across the whole body of the loop, emulated
 * writes included. That is the entire mechanism: no address of any of the
 * five locals below is ever taken, anywhere.
 *
 * From here to the matching block after z80_run, the five hot names of
 * z80_ops.h expand to those locals; the _STATE names keep meaning the fields.
 * The truth moves between the two homes at named boundaries only, each with
 * ONE direction of copy, never both, and each commented where it stands:
 *
 *   entry of z80_run          structure -> locals, EXCEPT the t-state
 *                             counter, which the quota seeds instead of the
 *                             field; that exception exists at entry and
 *                             nowhere else
 *   stopped early return      no copy in either direction: nothing was
 *                             loaded, the structure is the truth and stays it
 *   every other return        locals -> structure, without exception: every
 *                             stop is decided in the window, on the locals,
 *                             so the flush is what makes the structure exact
 *                             before the flag is raised
 *
 * There is no boundary in the middle of the loop any more. The two prefix
 * handlers that used to be called against the fields -- and to cost a
 * flush/reload pair around every prefixed instruction -- are expanded under
 * their case labels below, on the locals like everything else.
 *
 * R crosses these boundaries with one twist. Inside the window its local is
 * the refresh counter run free -- a whole word that a bare increment ticks;
 * Z80_TICK_R is retargeted below, beside the five names -- so the flush is
 * what composes the architectural byte back together, and its R line is the
 * one boundary copy that is more than a move.
 *
 * Between boundaries the structure's five hot fields are stale and nothing
 * may read them, with one scoped exception: bit 7 of the R field never goes
 * stale -- nothing in the window moves it except LD R,A, which writes the
 * field directly -- and the two refresh instructions behind the ED prefix
 * lean on exactly that, composing or writing both homes of R where they
 * stand. Every reader outside z80_run runs outside those windows, because
 * z80_run has synchronised on every path that leaves it. Code added inside
 * the loop lives on the locals, or pays a boundary of its own.
 * ---------------------------------------------------------------------------
 */
#undef Z80_PC
#undef Z80_TSTATES
#undef Z80_R
#undef Z80_A
#undef Z80_F
#define Z80_PC      z80_run_pc
#define Z80_TSTATES z80_run_tstates
#define Z80_R       z80_run_r
#define Z80_A       z80_run_a
#define Z80_F       z80_run_f

/*
 * The resident form of the refresh tick: a bare increment, one ARM
 * instruction where the masked form costs five. It is correct because the
 * low seven bits of a word counter incremented by one stay right modulo
 * 128 under any overflow -- 2^32 is a multiple of 128 -- and because the
 * two instructions that read or write R inside the window, LD A,R and
 * LD R,A behind the ED prefix, never touch the bare counter blindly: each
 * works the two homes explicitly where it is decoded. LD A,R reads the
 * composition -- bit 7 from the field, low seven bits from this counter,
 * the very line the flush uses -- and LD R,A writes both homes, the whole
 * byte into the field and the counter restarted from the accumulator. Bit
 * 7 changes through the field alone: LD R,A writes it in place, and
 * z80_reset clears it while the core is not running -- a carry into the
 * raw counter's bit 7 is exactly what every composition masks off. So the
 * flush below is where the architectural byte is composed for a boundary:
 * bit 7 from the field, low seven bits from this refresh counter. The
 * masked default stays in z80_ops.h as the out-of-window truth, though no
 * code ticks it any more: every opcode read happens inside this window.
 */
#undef Z80_TICK_R
#define Z80_TICK_R() (Z80_R = Z80_R + 1U)

/*
 * The two memory accesses keep their canonical form of z80_ops.h inside
 * the window: the table walk on every access. The tables point at ROM
 * served read-only beside a write page that absorbs, at a fixed page, and
 * at 8k of work RAM seen twice, so a flat local window would have to
 * carry every one of those aliases into a copy, and an absorbed ROM write
 * has nowhere to be carried at all -- the cost of the walk is assumed.
 * The mapper trigger of Z80_WR8 -- a write to a mapper register turns a
 * bank AND the byte still reaches memory -- has its one home there too,
 * and this file never reads which addresses are registers.
 */

/*
 * The one boundary copy, locals -> structure. A plain set of assignments,
 * not a call. Four of its lines are bare moves; the R line is the one
 * composition, because inside the window R is the refresh counter run free
 * (see Z80_TICK_R above): bit 7 comes from the field, which only LD R,A
 * and reset write, the low seven bits from the refresh counter. It is a
 * macro because it stands on several exits; the copy in the other
 * direction has exactly one site, the entry of z80_run, and is written in
 * plain there -- the prefix handlers whose call boundaries once reloaded
 * mid-loop are expanded in place now, and nothing else ever copies the
 * structure into the locals.
 *
 * The (uint32) cast on the field is for armcc, not for the arithmetic:
 * integer promotion alone would compute the same value, but without the
 * cast every flush site raises "lower precision in wider context", and a
 * build from this directory is required to raise nothing new.
 */
#define Z80_RESIDENT_FLUSH()                            \
  do                                                    \
    {                                                   \
      Z80_PC_STATE      = Z80_PC;                       \
      Z80_TSTATES_STATE = Z80_TSTATES;                  \
      Z80_R_STATE       = (uint8)(((uint32)Z80_R_STATE  \
                                    & 0x80U)            \
                                  | (Z80_R & 0x7FU));   \
      Z80_A_STATE       = Z80_A;                        \
      Z80_F_STATE       = Z80_F;                        \
    }                                                   \
  while(0)

int32
z80_run(int32 quota)
{
  uint8 op;
  uint8 imm8;
  uint16 imm16;

  /*
   * The five resident fields, in declaration the reason they exist: locals
   * whose address is never taken, which is what lets them live in registers
   * across the emulated memory writes that would evict any field of
   * sms.z80. The hot names of z80_ops.h expand to these throughout this
   * function; see the block above.
   *
   * One of the five is wider than its field: r is a whole word, the
   * free-running refresh counter behind the resident Z80_TICK_R, and only
   * its low seven bits mean anything. The flush composes the eight bit
   * register out of it; the width is what makes the bare increment safe
   * under overflow.
   */
  uint16 z80_run_pc;
  int32  z80_run_tstates;
  uint32 z80_run_r;
  uint8  z80_run_a;
  uint8  z80_run_f;

  /*
   * The halves of the index pair an index prefix has selected. At function
   * scope and not in the prefix's block, because the two case labels of
   * that prefix each write them before running the one shared body -- the
   * label itself is what says IX or IY, so the opcode byte does not have
   * to (see the case for why that matters). Written on the two paths into
   * that body and read nowhere else; they carry nothing between
   * instructions.
   */
  uint8 *ixy_hi;
  uint8 *ixy_lo;

  /*
   * Boundary, and an empty one: nothing has been loaded yet, the structure
   * is the truth and stays it -- a stopped core returns without copying
   * anything in either direction.
   */
  if(z80_stopped)
    return 0;

  /*
   * Entry boundary: structure -> locals, the one direction, and the only
   * copy that ever runs this way -- written in plain because it has this
   * one site. The t-state counter is the exception, seeded from the quota
   * instead: what was left of the previous one has already been handed
   * back to the caller, and the caller has already taken it off the figure
   * it passes here. Adding it in both places would pay the overrun twice.
   *
   * The R line is a bare copy on purpose: the field's bit 7 rides into the
   * refresh counter and is harmless there -- every composition masks it
   * off, so only the low seven bits of the counter ever mean anything.
   */
  Z80_PC = Z80_PC_STATE;
  Z80_R  = Z80_R_STATE;
  Z80_A  = Z80_A_STATE;
  Z80_F  = Z80_F_STATE;
  Z80_TSTATES = quota;

  /*
   * ---------------------------------------------------------------------
   * The interrupt lines, sampled once per entry -- which the frame loop
   * makes once per scanline, before the quota is consumed. The source
   * does not change its line more often than that, so a read inside the
   * dispatch loop would be a pure cost on the path that has 3.49 ARM
   * cycles per T-state to spend; the stepping entry samples once per
   * instruction and pays it knowingly, being a watching path.
   *
   * Semantics transposed from the acceptance of the source, its
   * scheduler set aside: when an interrupt is acceptable, what changes,
   * and what it costs (TotalSMS/src/core/sms_z80.c:2103-2155).
   *
   * The calls behind the two line macros are tolerated inside the
   * resident window for the reason the port functions are: they read and
   * write nothing but the source's own state. Everything modified below
   * is either a plain field of the structure -- the flip-flops, the halt,
   * the edge latch -- or one of the residents, live since the boundary
   * above.
   * ---------------------------------------------------------------------
   */
  {
    uint8 z80_irq_up;
    uint8 z80_nmi_up;

    z80_irq_up = Z80_IRQ_LINE();
    z80_nmi_up = Z80_NMI_LINE();

    if(z80_nmi_up && !Z80_NMI_SEEN)
      {
        /*
         * Non-maskable, so nothing is consulted: taken on the edge of the
         * line whatever IFF1 says. IFF1 is parked in IFF2 -- with a
         * pending EI delay counting as an enable already granted, the
         * detail of sms_z80.c:2120 that cannot be rediscovered later --
         * for RETN to bring back, and the delay itself is spent
         * (:2117-2125). Eleven T-states, vector 0x66.
         */
        if(Z80_HALTED)
          {
            Z80_HALTED = 0;
            LOG_HOT(LOG_CAT_Z80,LOG_LVL_DBG,("halt left on nmi"));
          }

        LOG_HOT(LOG_CAT_Z80,LOG_LVL_INFO,
                ("nmi accepted pc=0x%04lx",(unsigned long)Z80_PC));

        Z80_IFF2 = (uint8)(Z80_IFF1 | Z80_EI_DELAY);
        Z80_IFF1 = 0;
        Z80_EI_DELAY = 0;
        Z80_SPEND(11);
        Z80_OP_RST(0x0066);
      }
    else if(Z80_EI_DELAY)
      {
        /*
         * The delay an enable leaves behind: both flip-flops come up, and
         * no interrupt is taken at this same boundary -- the instruction
         * behind the enable has to run first, and the retry the source
         * schedules one cycle later (:2138-2144) becomes the next
         * sampling here. A line still up then is accepted then.
         */
        Z80_EI_DELAY = 0;
        Z80_IFF1 = 1;
        Z80_IFF2 = 1;
      }
    else if(Z80_IFF1 && z80_irq_up)
      {
        if(Z80_IM == 1)
          {
            /*
             * Accepted: both flip-flops fall, the halt ends, thirteen
             * T-states, vector 0x38 (:2145-2153).
             *
             * Counted, not traced -- neither the halt leaving nor the
             * acceptance itself. A game takes one of these per frame and
             * halts once per frame waiting for it: a serial line at each
             * would be a blocking write per frame, which does not slow
             * the frame down, it destroys it. The count goes out on the
             * video part's periodic line (vdp_report), which is where a
             * reader looks for the interrupt anyway. The non-maskable
             * acceptance above keeps its line: it is rare, and it is the
             * scaffold's own trace.
             */
            if(Z80_HALTED)
              Z80_HALTED = 0;

#if LOG_ENABLE && SMS_TELEMETRY
            sms.z80.irq_accepted++;
#endif

            Z80_IFF1 = 0;
            Z80_IFF2 = 0;
            Z80_SPEND(13);
            Z80_OP_RST(0x0038);
          }
        else
          {
            /*
             * Modes 0 and 2 are recorded but refuse loudly: no authorised
             * source describes their behaviour -- the source of the
             * semantics branches unconditionally on the mode 1 vector and
             * asserts the mode is 1 (:1057-1061, :2151) -- and a branch
             * invented here would work on most programs and hand a false
             * verdict on the rest. Nothing stops: the refusal is traced,
             * the state is left alone, and the question goes to the
             * human. One line per mode for the whole run, the bound the
             * mode selector's own trace uses -- the line would otherwise
             * repeat at every sampling that finds the line still up.
             */
            static uint8 z80_irq_refused_said = 0;

            uint8 z80_irq_mode_bit = (uint8)(1U << (Z80_IM & 3U));

            if((z80_irq_refused_said & z80_irq_mode_bit) == 0)
              {
                z80_irq_refused_said =
                  (uint8)(z80_irq_refused_said | z80_irq_mode_bit);

                LOG_ERR(LOG_CAT_Z80,
                        ("irq mode=%lu not implemented, ignored",
                         (unsigned long)Z80_IM));
              }
          }
      }

    Z80_NMI_SEEN = z80_nmi_up;
  }

  /*
   * A core in HALT executes nothing and stays where it is: the quota is
   * consumed as so many no-operations, PC does not move, and the line is
   * handed back so the loop above keeps sampling at every boundary -- the
   * emulated processor may stay halted for as long as no line rises, the
   * emulator's loop never blocks on it. The source loops its scheduler
   * instead (sms_z80.c:2108-2115): form set aside, semantics kept. An
   * acceptance above has already ended the halt before this test reads it.
   */
  if(Z80_HALTED)
    {
      while(Z80_TSTATES > 0)
        Z80_SPEND(4);

      Z80_RESIDENT_FLUSH();
      return -Z80_TSTATES;
    }

  while(Z80_TSTATES > 0)
    {
      op = Z80_RD8(Z80_PC);
      Z80_PC = (uint16)(Z80_PC + 1);
      Z80_TICK_R();

      /*
       * One switch, expanded in place, and no table of function pointers
       * anywhere near it. An uncached call on this processor costs 29 to 39
       * cycles where the whole budget is 3.49 ARM cycles per emulated T-state:
       * a call per opcode would spend eight to eleven T-states arriving at an
       * instruction that a register move finishes in four.
       *
       * EVERY CASE OPENS BY SPENDING ITS OWN PRICE AS AN IMMEDIATE, each
       * literal a transcription of the cost table's entry for that opcode --
       * carried over by counting values and never lines, the rule the tables
       * themselves state. The header above therefore reads nothing but the
       * opcode. Charging out of the table here cost two loads on the path
       * every instruction walks -- the table base, reloaded each iteration
       * because every register that survives the loop body is already held
       * by the residents, then the entry -- and the compiler of this target
       * does not fold a constant index into a static const array down to an
       * immediate, so the literal is the only form without a read. The four
       * prefix opcodes spend nothing, exactly as their table entries say:
       * their price is carried behind the prefix.
       */
      switch(op)
        {
        case 0x00: Z80_SPEND(4); break; /* nop */

        /* Sixteen bit immediate loads. */
        case 0x01: Z80_SPEND(10); Z80_FETCH16(imm16); Z80_SET_BC(imm16); break;
        case 0x11: Z80_SPEND(10); Z80_FETCH16(imm16); Z80_SET_DE(imm16); break;
        case 0x21: Z80_SPEND(10); Z80_FETCH16(imm16); Z80_SET_HL(imm16); break;
        case 0x31: Z80_SPEND(10); Z80_FETCH16(imm16); Z80_SP = imm16; break;

        /* The accumulator through a pair, both ways. */
        case 0x02: Z80_SPEND(7); Z80_WR8(Z80_BC,Z80_A); break;
        case 0x12: Z80_SPEND(7); Z80_WR8(Z80_DE,Z80_A); break;
        case 0x0A: Z80_SPEND(7); Z80_A = Z80_RD8(Z80_BC); break;
        case 0x1A: Z80_SPEND(7); Z80_A = Z80_RD8(Z80_DE); break;

        /* The accumulator and HL through a direct address. */
        case 0x22: Z80_SPEND(16); Z80_FETCH16(imm16); Z80_WR16(imm16,Z80_HL); break;
        case 0x2A: Z80_SPEND(16); Z80_FETCH16(imm16); Z80_SET_HL(Z80_RD16(imm16)); break;
        case 0x32: Z80_SPEND(13); Z80_FETCH16(imm16); Z80_WR8(imm16,Z80_A); break;
        case 0x3A: Z80_SPEND(13); Z80_FETCH16(imm16); Z80_A = Z80_RD8(imm16); break;

        /* Eight bit immediate loads. */
        case 0x06: Z80_SPEND(7); Z80_FETCH8(Z80_B); break;
        case 0x0E: Z80_SPEND(7); Z80_FETCH8(Z80_C); break;
        case 0x16: Z80_SPEND(7); Z80_FETCH8(Z80_D); break;
        case 0x1E: Z80_SPEND(7); Z80_FETCH8(Z80_E); break;
        case 0x26: Z80_SPEND(7); Z80_FETCH8(Z80_H); break;
        case 0x2E: Z80_SPEND(7); Z80_FETCH8(Z80_L); break;
        case 0x36: Z80_SPEND(10); Z80_FETCH8(imm8); Z80_WR8(Z80_HL,imm8); break;
        case 0x3E: Z80_SPEND(7); Z80_FETCH8(Z80_A); break;

        case 0xF9: Z80_SPEND(6); Z80_SP = Z80_HL; break;

        /* Increment and decrement, eight bits: the carry survives all sixteen. */
        case 0x04: Z80_SPEND(4); Z80_OP_INC_R(Z80_B); break;
        case 0x0C: Z80_SPEND(4); Z80_OP_INC_R(Z80_C); break;
        case 0x14: Z80_SPEND(4); Z80_OP_INC_R(Z80_D); break;
        case 0x1C: Z80_SPEND(4); Z80_OP_INC_R(Z80_E); break;
        case 0x24: Z80_SPEND(4); Z80_OP_INC_R(Z80_H); break;
        case 0x2C: Z80_SPEND(4); Z80_OP_INC_R(Z80_L); break;
        case 0x34: Z80_SPEND(11); Z80_OP_INC_MEM(); break;
        case 0x3C: Z80_SPEND(4); Z80_OP_INC_R(Z80_A); break;

        case 0x05: Z80_SPEND(4); Z80_OP_DEC_R(Z80_B); break;
        case 0x0D: Z80_SPEND(4); Z80_OP_DEC_R(Z80_C); break;
        case 0x15: Z80_SPEND(4); Z80_OP_DEC_R(Z80_D); break;
        case 0x1D: Z80_SPEND(4); Z80_OP_DEC_R(Z80_E); break;
        case 0x25: Z80_SPEND(4); Z80_OP_DEC_R(Z80_H); break;
        case 0x2D: Z80_SPEND(4); Z80_OP_DEC_R(Z80_L); break;
        case 0x35: Z80_SPEND(11); Z80_OP_DEC_MEM(); break;
        case 0x3D: Z80_SPEND(4); Z80_OP_DEC_R(Z80_A); break;

        /* Increment and decrement, sixteen bits: not one flag is touched. */
        case 0x03: Z80_SPEND(6); Z80_SET_BC((uint16)(Z80_BC + 1)); break;
        case 0x13: Z80_SPEND(6); Z80_SET_DE((uint16)(Z80_DE + 1)); break;
        case 0x23: Z80_SPEND(6); Z80_SET_HL((uint16)(Z80_HL + 1)); break;
        case 0x33: Z80_SPEND(6); Z80_SP = (uint16)(Z80_SP + 1); break;

        case 0x0B: Z80_SPEND(6); Z80_SET_BC((uint16)(Z80_BC - 1)); break;
        case 0x1B: Z80_SPEND(6); Z80_SET_DE((uint16)(Z80_DE - 1)); break;
        case 0x2B: Z80_SPEND(6); Z80_SET_HL((uint16)(Z80_HL - 1)); break;
        case 0x3B: Z80_SPEND(6); Z80_SP = (uint16)(Z80_SP - 1); break;

        case 0x09: Z80_SPEND(11); Z80_OP_ADD_HL(Z80_BC); break;
        case 0x19: Z80_SPEND(11); Z80_OP_ADD_HL(Z80_DE); break;
        case 0x29: Z80_SPEND(11); Z80_OP_ADD_HL(Z80_HL); break;
        case 0x39: Z80_SPEND(11); Z80_OP_ADD_HL(Z80_SP); break;

        /* The accumulator rotations, which leave sign, zero and parity alone. */
        case 0x07: Z80_SPEND(4); Z80_OP_RLCA(); break;
        case 0x0F: Z80_SPEND(4); Z80_OP_RRCA(); break;
        case 0x17: Z80_SPEND(4); Z80_OP_RLA(); break;
        case 0x1F: Z80_SPEND(4); Z80_OP_RRA(); break;

        case 0x27: Z80_SPEND(4); Z80_OP_DAA(); break;
        case 0x2F: Z80_SPEND(4); Z80_OP_CPL(); break;
        case 0x37: Z80_SPEND(4); Z80_OP_SCF(); break;
        case 0x3F: Z80_SPEND(4); Z80_OP_CCF(); break;

        /*
         * Register to register, and the byte HL points at as the seventh
         * source and the seventh destination. Written out one opcode at a
         * time rather than decoded from the two three-bit fields: decoding
         * them would put a second switch inside this one, on the path this
         * whole file exists to keep short.
         * Register order of the field: TotalSMS/src/core/sms_z80.c:286-301.
         */
        case 0x40: Z80_SPEND(4); break; /* ld b,b -- no operation */
        case 0x41: Z80_SPEND(4); Z80_B = Z80_C; break;
        case 0x42: Z80_SPEND(4); Z80_B = Z80_D; break;
        case 0x43: Z80_SPEND(4); Z80_B = Z80_E; break;
        case 0x44: Z80_SPEND(4); Z80_B = Z80_H; break;
        case 0x45: Z80_SPEND(4); Z80_B = Z80_L; break;
        case 0x46: Z80_SPEND(7); Z80_B = Z80_RD8(Z80_HL); break;
        case 0x47: Z80_SPEND(4); Z80_B = Z80_A; break;
        case 0x48: Z80_SPEND(4); Z80_C = Z80_B; break;
        case 0x49: Z80_SPEND(4); break; /* ld c,c -- no operation */
        case 0x4A: Z80_SPEND(4); Z80_C = Z80_D; break;
        case 0x4B: Z80_SPEND(4); Z80_C = Z80_E; break;
        case 0x4C: Z80_SPEND(4); Z80_C = Z80_H; break;
        case 0x4D: Z80_SPEND(4); Z80_C = Z80_L; break;
        case 0x4E: Z80_SPEND(7); Z80_C = Z80_RD8(Z80_HL); break;
        case 0x4F: Z80_SPEND(4); Z80_C = Z80_A; break;
        case 0x50: Z80_SPEND(4); Z80_D = Z80_B; break;
        case 0x51: Z80_SPEND(4); Z80_D = Z80_C; break;
        case 0x52: Z80_SPEND(4); break; /* ld d,d -- no operation */
        case 0x53: Z80_SPEND(4); Z80_D = Z80_E; break;
        case 0x54: Z80_SPEND(4); Z80_D = Z80_H; break;
        case 0x55: Z80_SPEND(4); Z80_D = Z80_L; break;
        case 0x56: Z80_SPEND(7); Z80_D = Z80_RD8(Z80_HL); break;
        case 0x57: Z80_SPEND(4); Z80_D = Z80_A; break;
        case 0x58: Z80_SPEND(4); Z80_E = Z80_B; break;
        case 0x59: Z80_SPEND(4); Z80_E = Z80_C; break;
        case 0x5A: Z80_SPEND(4); Z80_E = Z80_D; break;
        case 0x5B: Z80_SPEND(4); break; /* ld e,e -- no operation */
        case 0x5C: Z80_SPEND(4); Z80_E = Z80_H; break;
        case 0x5D: Z80_SPEND(4); Z80_E = Z80_L; break;
        case 0x5E: Z80_SPEND(7); Z80_E = Z80_RD8(Z80_HL); break;
        case 0x5F: Z80_SPEND(4); Z80_E = Z80_A; break;
        case 0x60: Z80_SPEND(4); Z80_H = Z80_B; break;
        case 0x61: Z80_SPEND(4); Z80_H = Z80_C; break;
        case 0x62: Z80_SPEND(4); Z80_H = Z80_D; break;
        case 0x63: Z80_SPEND(4); Z80_H = Z80_E; break;
        case 0x64: Z80_SPEND(4); break; /* ld h,h -- no operation */
        case 0x65: Z80_SPEND(4); Z80_H = Z80_L; break;
        case 0x66: Z80_SPEND(7); Z80_H = Z80_RD8(Z80_HL); break;
        case 0x67: Z80_SPEND(4); Z80_H = Z80_A; break;
        case 0x68: Z80_SPEND(4); Z80_L = Z80_B; break;
        case 0x69: Z80_SPEND(4); Z80_L = Z80_C; break;
        case 0x6A: Z80_SPEND(4); Z80_L = Z80_D; break;
        case 0x6B: Z80_SPEND(4); Z80_L = Z80_E; break;
        case 0x6C: Z80_SPEND(4); Z80_L = Z80_H; break;
        case 0x6D: Z80_SPEND(4); break; /* ld l,l -- no operation */
        case 0x6E: Z80_SPEND(7); Z80_L = Z80_RD8(Z80_HL); break;
        case 0x6F: Z80_SPEND(4); Z80_L = Z80_A; break;
        case 0x70: Z80_SPEND(7); Z80_WR8(Z80_HL,Z80_B); break;
        case 0x71: Z80_SPEND(7); Z80_WR8(Z80_HL,Z80_C); break;
        case 0x72: Z80_SPEND(7); Z80_WR8(Z80_HL,Z80_D); break;
        case 0x73: Z80_SPEND(7); Z80_WR8(Z80_HL,Z80_E); break;
        case 0x74: Z80_SPEND(7); Z80_WR8(Z80_HL,Z80_H); break;
        case 0x75: Z80_SPEND(7); Z80_WR8(Z80_HL,Z80_L); break;
        case 0x77: Z80_SPEND(7); Z80_WR8(Z80_HL,Z80_A); break;
        case 0x78: Z80_SPEND(4); Z80_A = Z80_B; break;
        case 0x79: Z80_SPEND(4); Z80_A = Z80_C; break;
        case 0x7A: Z80_SPEND(4); Z80_A = Z80_D; break;
        case 0x7B: Z80_SPEND(4); Z80_A = Z80_E; break;
        case 0x7C: Z80_SPEND(4); Z80_A = Z80_H; break;
        case 0x7D: Z80_SPEND(4); Z80_A = Z80_L; break;
        case 0x7E: Z80_SPEND(7); Z80_A = Z80_RD8(Z80_HL); break;
        case 0x7F: Z80_SPEND(4); break; /* ld a,a -- no operation */

        /*
         * Stopping until an interrupt lifts it is the whole of this
         * instruction. The price is paid, the halt is recorded, the rest of
         * the line's quota is consumed as so many no-operations without PC
         * moving -- it already sits on the instruction behind the halt,
         * which is where an acceptance resumes -- and the line is handed
         * back: the sampling at the next entry is what can end the halt,
         * and until it does every entry consumes its quota the same way.
         * Semantics: TotalSMS/src/core/sms_z80.c:1339-1344, its scheduler
         * loop (:2108-2115) set aside, this port cutting time at the
         * scanline rather than scheduling events.
         *
         * The source asserts that a halt with interrupts disabled is a
         * program fault (:1341). On this target an assertion does not exist
         * and a definitive block would be a dark screen: the state is said
         * once -- only an NMI can end it, which remains possible -- and the
         * scanline loop keeps turning either way. The emulated processor
         * may stay halted; the emulator may not.
         *
         * The oracle did not exercise this: its author avoids emitting
         * halts on purpose
         * (docs/sms_gg/ZEXALL-SMS-0.21/source/zexall.sms.asm:1796-1806).
         */
        case 0x76:
          Z80_SPEND(4);
          Z80_HALTED = 1;

          LOG_HOT(LOG_CAT_Z80,LOG_LVL_DBG,
                  ("halt entered pc=0x%04lx",Z80_OP_ADDR));

          if(!Z80_IFF1 && !Z80_EI_DELAY)
            LOG_ONCE(LOG_CAT_Z80,LOG_LVL_WARN,
                     ("halt with interrupts disabled at pc=0x%04lx, only an nmi can end it",
                      Z80_OP_ADDR));

          while(Z80_TSTATES > 0)
            Z80_SPEND(4);

          /*
           * Exit boundary: locals -> structure, the rule of every return
           * decided in the window. Not a stop: the core is halted, not
           * stopped, and the next entry samples its way out.
           */
          Z80_RESIDENT_FLUSH();
          return -Z80_TSTATES;

        /*
         * The eight arithmetic and logical operations against each of the
         * eight sources, in the order the opcode's middle three bits give:
         * add, add with carry, subtract, subtract with borrow, and, xor,
         * or, compare.
         */
        case 0x80: Z80_SPEND(4); Z80_OP_ADD(Z80_B,0); break;
        case 0x81: Z80_SPEND(4); Z80_OP_ADD(Z80_C,0); break;
        case 0x82: Z80_SPEND(4); Z80_OP_ADD(Z80_D,0); break;
        case 0x83: Z80_SPEND(4); Z80_OP_ADD(Z80_E,0); break;
        case 0x84: Z80_SPEND(4); Z80_OP_ADD(Z80_H,0); break;
        case 0x85: Z80_SPEND(4); Z80_OP_ADD(Z80_L,0); break;
        case 0x86: Z80_SPEND(7); Z80_OP_ADD(Z80_RD8(Z80_HL),0); break;
        case 0x87: Z80_SPEND(4); Z80_OP_ADD(Z80_A,0); break;
        case 0x88: Z80_SPEND(4); Z80_OP_ADD(Z80_B,Z80_FLAG_GET(Z80_FLAG_C)); break;
        case 0x89: Z80_SPEND(4); Z80_OP_ADD(Z80_C,Z80_FLAG_GET(Z80_FLAG_C)); break;
        case 0x8A: Z80_SPEND(4); Z80_OP_ADD(Z80_D,Z80_FLAG_GET(Z80_FLAG_C)); break;
        case 0x8B: Z80_SPEND(4); Z80_OP_ADD(Z80_E,Z80_FLAG_GET(Z80_FLAG_C)); break;
        case 0x8C: Z80_SPEND(4); Z80_OP_ADD(Z80_H,Z80_FLAG_GET(Z80_FLAG_C)); break;
        case 0x8D: Z80_SPEND(4); Z80_OP_ADD(Z80_L,Z80_FLAG_GET(Z80_FLAG_C)); break;
        case 0x8E: Z80_SPEND(7); Z80_OP_ADD(Z80_RD8(Z80_HL),Z80_FLAG_GET(Z80_FLAG_C)); break;
        case 0x8F: Z80_SPEND(4); Z80_OP_ADD(Z80_A,Z80_FLAG_GET(Z80_FLAG_C)); break;
        case 0x90: Z80_SPEND(4); Z80_OP_SUB(Z80_B,0); break;
        case 0x91: Z80_SPEND(4); Z80_OP_SUB(Z80_C,0); break;
        case 0x92: Z80_SPEND(4); Z80_OP_SUB(Z80_D,0); break;
        case 0x93: Z80_SPEND(4); Z80_OP_SUB(Z80_E,0); break;
        case 0x94: Z80_SPEND(4); Z80_OP_SUB(Z80_H,0); break;
        case 0x95: Z80_SPEND(4); Z80_OP_SUB(Z80_L,0); break;
        case 0x96: Z80_SPEND(7); Z80_OP_SUB(Z80_RD8(Z80_HL),0); break;
        case 0x97: Z80_SPEND(4); Z80_OP_SUB(Z80_A,0); break;
        case 0x98: Z80_SPEND(4); Z80_OP_SUB(Z80_B,Z80_FLAG_GET(Z80_FLAG_C)); break;
        case 0x99: Z80_SPEND(4); Z80_OP_SUB(Z80_C,Z80_FLAG_GET(Z80_FLAG_C)); break;
        case 0x9A: Z80_SPEND(4); Z80_OP_SUB(Z80_D,Z80_FLAG_GET(Z80_FLAG_C)); break;
        case 0x9B: Z80_SPEND(4); Z80_OP_SUB(Z80_E,Z80_FLAG_GET(Z80_FLAG_C)); break;
        case 0x9C: Z80_SPEND(4); Z80_OP_SUB(Z80_H,Z80_FLAG_GET(Z80_FLAG_C)); break;
        case 0x9D: Z80_SPEND(4); Z80_OP_SUB(Z80_L,Z80_FLAG_GET(Z80_FLAG_C)); break;
        case 0x9E: Z80_SPEND(7); Z80_OP_SUB(Z80_RD8(Z80_HL),Z80_FLAG_GET(Z80_FLAG_C)); break;
        case 0x9F: Z80_SPEND(4); Z80_OP_SUB(Z80_A,Z80_FLAG_GET(Z80_FLAG_C)); break;
        case 0xA0: Z80_SPEND(4); Z80_OP_AND(Z80_B); break;
        case 0xA1: Z80_SPEND(4); Z80_OP_AND(Z80_C); break;
        case 0xA2: Z80_SPEND(4); Z80_OP_AND(Z80_D); break;
        case 0xA3: Z80_SPEND(4); Z80_OP_AND(Z80_E); break;
        case 0xA4: Z80_SPEND(4); Z80_OP_AND(Z80_H); break;
        case 0xA5: Z80_SPEND(4); Z80_OP_AND(Z80_L); break;
        case 0xA6: Z80_SPEND(7); Z80_OP_AND(Z80_RD8(Z80_HL)); break;
        case 0xA7: Z80_SPEND(4); Z80_OP_AND(Z80_A); break;
        case 0xA8: Z80_SPEND(4); Z80_OP_XOR(Z80_B); break;
        case 0xA9: Z80_SPEND(4); Z80_OP_XOR(Z80_C); break;
        case 0xAA: Z80_SPEND(4); Z80_OP_XOR(Z80_D); break;
        case 0xAB: Z80_SPEND(4); Z80_OP_XOR(Z80_E); break;
        case 0xAC: Z80_SPEND(4); Z80_OP_XOR(Z80_H); break;
        case 0xAD: Z80_SPEND(4); Z80_OP_XOR(Z80_L); break;
        case 0xAE: Z80_SPEND(7); Z80_OP_XOR(Z80_RD8(Z80_HL)); break;
        case 0xAF: Z80_SPEND(4); Z80_OP_XOR(Z80_A); break;
        case 0xB0: Z80_SPEND(4); Z80_OP_OR(Z80_B); break;
        case 0xB1: Z80_SPEND(4); Z80_OP_OR(Z80_C); break;
        case 0xB2: Z80_SPEND(4); Z80_OP_OR(Z80_D); break;
        case 0xB3: Z80_SPEND(4); Z80_OP_OR(Z80_E); break;
        case 0xB4: Z80_SPEND(4); Z80_OP_OR(Z80_H); break;
        case 0xB5: Z80_SPEND(4); Z80_OP_OR(Z80_L); break;
        case 0xB6: Z80_SPEND(7); Z80_OP_OR(Z80_RD8(Z80_HL)); break;
        case 0xB7: Z80_SPEND(4); Z80_OP_OR(Z80_A); break;
        case 0xB8: Z80_SPEND(4); Z80_OP_CP(Z80_B); break;
        case 0xB9: Z80_SPEND(4); Z80_OP_CP(Z80_C); break;
        case 0xBA: Z80_SPEND(4); Z80_OP_CP(Z80_D); break;
        case 0xBB: Z80_SPEND(4); Z80_OP_CP(Z80_E); break;
        case 0xBC: Z80_SPEND(4); Z80_OP_CP(Z80_H); break;
        case 0xBD: Z80_SPEND(4); Z80_OP_CP(Z80_L); break;
        case 0xBE: Z80_SPEND(7); Z80_OP_CP(Z80_RD8(Z80_HL)); break;
        case 0xBF: Z80_SPEND(4); Z80_OP_CP(Z80_A); break;

        /* The same eight operations against an immediate byte. */
        case 0xC6: Z80_SPEND(7); Z80_FETCH8(imm8); Z80_OP_ADD(imm8,0); break;
        case 0xCE: Z80_SPEND(7); Z80_FETCH8(imm8);
                   Z80_OP_ADD(imm8,Z80_FLAG_GET(Z80_FLAG_C)); break;
        case 0xD6: Z80_SPEND(7); Z80_FETCH8(imm8); Z80_OP_SUB(imm8,0); break;
        case 0xDE: Z80_SPEND(7); Z80_FETCH8(imm8);
                   Z80_OP_SUB(imm8,Z80_FLAG_GET(Z80_FLAG_C)); break;
        case 0xE6: Z80_SPEND(7); Z80_FETCH8(imm8); Z80_OP_AND(imm8); break;
        case 0xEE: Z80_SPEND(7); Z80_FETCH8(imm8); Z80_OP_XOR(imm8); break;
        case 0xF6: Z80_SPEND(7); Z80_FETCH8(imm8); Z80_OP_OR(imm8); break;
        case 0xFE: Z80_SPEND(7); Z80_FETCH8(imm8); Z80_OP_CP(imm8); break;

        /* Unconditional branches. */
        case 0x10: Z80_SPEND(8); Z80_OP_DJNZ(); break;
        case 0x18: Z80_SPEND(12); Z80_OP_JR(); break;
        case 0xC3: Z80_SPEND(10); Z80_OP_JP(); break;
        case 0xE9: Z80_SPEND(4); Z80_PC = Z80_HL; break;
        case 0xC9: Z80_SPEND(10); Z80_OP_RET(); break;
        case 0xCD: Z80_SPEND(17); Z80_OP_CALL(); break;

        /*
         * Conditional branches. The eight conditions read in this order --
         * not zero, zero, no carry, carry, odd parity, even parity, positive,
         * negative -- and the relative form has only the first four.
         * Order: TotalSMS/src/core/sms_z80.c:2026-2060.
         */
        case 0x20: Z80_SPEND(7); Z80_OP_JR_CC(!Z80_FLAG_GET(Z80_FLAG_Z)); break;
        case 0x28: Z80_SPEND(7); Z80_OP_JR_CC(Z80_FLAG_GET(Z80_FLAG_Z)); break;
        case 0x30: Z80_SPEND(7); Z80_OP_JR_CC(!Z80_FLAG_GET(Z80_FLAG_C)); break;
        case 0x38: Z80_SPEND(7); Z80_OP_JR_CC(Z80_FLAG_GET(Z80_FLAG_C)); break;

        case 0xC2: Z80_SPEND(10); Z80_OP_JP_CC(!Z80_FLAG_GET(Z80_FLAG_Z)); break;
        case 0xCA: Z80_SPEND(10); Z80_OP_JP_CC(Z80_FLAG_GET(Z80_FLAG_Z)); break;
        case 0xD2: Z80_SPEND(10); Z80_OP_JP_CC(!Z80_FLAG_GET(Z80_FLAG_C)); break;
        case 0xDA: Z80_SPEND(10); Z80_OP_JP_CC(Z80_FLAG_GET(Z80_FLAG_C)); break;
        case 0xE2: Z80_SPEND(10); Z80_OP_JP_CC(!Z80_FLAG_GET(Z80_FLAG_PV)); break;
        case 0xEA: Z80_SPEND(10); Z80_OP_JP_CC(Z80_FLAG_GET(Z80_FLAG_PV)); break;
        case 0xF2: Z80_SPEND(10); Z80_OP_JP_CC(!Z80_FLAG_GET(Z80_FLAG_S)); break;
        case 0xFA: Z80_SPEND(10); Z80_OP_JP_CC(Z80_FLAG_GET(Z80_FLAG_S)); break;

        case 0xC4: Z80_SPEND(10); Z80_OP_CALL_CC(!Z80_FLAG_GET(Z80_FLAG_Z)); break;
        case 0xCC: Z80_SPEND(10); Z80_OP_CALL_CC(Z80_FLAG_GET(Z80_FLAG_Z)); break;
        case 0xD4: Z80_SPEND(10); Z80_OP_CALL_CC(!Z80_FLAG_GET(Z80_FLAG_C)); break;
        case 0xDC: Z80_SPEND(10); Z80_OP_CALL_CC(Z80_FLAG_GET(Z80_FLAG_C)); break;
        case 0xE4: Z80_SPEND(10); Z80_OP_CALL_CC(!Z80_FLAG_GET(Z80_FLAG_PV)); break;
        case 0xEC: Z80_SPEND(10); Z80_OP_CALL_CC(Z80_FLAG_GET(Z80_FLAG_PV)); break;
        case 0xF4: Z80_SPEND(10); Z80_OP_CALL_CC(!Z80_FLAG_GET(Z80_FLAG_S)); break;
        case 0xFC: Z80_SPEND(10); Z80_OP_CALL_CC(Z80_FLAG_GET(Z80_FLAG_S)); break;

        case 0xC0: Z80_SPEND(5); Z80_OP_RET_CC(!Z80_FLAG_GET(Z80_FLAG_Z)); break;
        case 0xC8: Z80_SPEND(5); Z80_OP_RET_CC(Z80_FLAG_GET(Z80_FLAG_Z)); break;
        case 0xD0: Z80_SPEND(5); Z80_OP_RET_CC(!Z80_FLAG_GET(Z80_FLAG_C)); break;
        case 0xD8: Z80_SPEND(5); Z80_OP_RET_CC(Z80_FLAG_GET(Z80_FLAG_C)); break;
        case 0xE0: Z80_SPEND(5); Z80_OP_RET_CC(!Z80_FLAG_GET(Z80_FLAG_PV)); break;
        case 0xE8: Z80_SPEND(5); Z80_OP_RET_CC(Z80_FLAG_GET(Z80_FLAG_PV)); break;
        case 0xF0: Z80_SPEND(5); Z80_OP_RET_CC(!Z80_FLAG_GET(Z80_FLAG_S)); break;
        case 0xF8: Z80_SPEND(5); Z80_OP_RET_CC(Z80_FLAG_GET(Z80_FLAG_S)); break;

        /*
         * The eight fixed call targets are the opcode's own middle three
         * bits, written out as the constants they are rather than computed
         * as op & 0x38. Not a flourish: computing it is the one hot read of
         * the opcode after the dispatch has jumped, and that single use is
         * what decides whether armcc keeps the opcode in a register or
         * round-trips it through the stack on every instruction of the run
         * -- the header pays for what this case reads.
         */
        case 0xC7: Z80_SPEND(11); Z80_OP_RST(0x00); break;
        case 0xCF: Z80_SPEND(11); Z80_OP_RST(0x08); break;
        case 0xD7: Z80_SPEND(11); Z80_OP_RST(0x10); break;
        case 0xDF: Z80_SPEND(11); Z80_OP_RST(0x18); break;
        case 0xE7: Z80_SPEND(11); Z80_OP_RST(0x20); break;
        case 0xEF: Z80_SPEND(11); Z80_OP_RST(0x28); break;
        case 0xF7: Z80_SPEND(11); Z80_OP_RST(0x30); break;
        case 0xFF: Z80_SPEND(11); Z80_OP_RST(0x38); break;

        /* The stack. */
        case 0xC1: Z80_SPEND(10); Z80_OP_POP(Z80_B,Z80_C); break;
        case 0xD1: Z80_SPEND(10); Z80_OP_POP(Z80_D,Z80_E); break;
        case 0xE1: Z80_SPEND(10); Z80_OP_POP(Z80_H,Z80_L); break;
        case 0xF1: Z80_SPEND(10); Z80_OP_POP(Z80_A,Z80_F); break;

        case 0xC5: Z80_SPEND(11); Z80_OP_PUSH(Z80_BC); break;
        case 0xD5: Z80_SPEND(11); Z80_OP_PUSH(Z80_DE); break;
        case 0xE5: Z80_SPEND(11); Z80_OP_PUSH(Z80_HL); break;
        case 0xF5: Z80_SPEND(11); Z80_OP_PUSH(Z80_AF); break;

        /* The exchanges. */
        case 0x08: Z80_SPEND(4); Z80_OP_EX_AF(); break;
        case 0xD9: Z80_SPEND(4); Z80_OP_EXX(); break;
        case 0xE3: Z80_SPEND(19); Z80_OP_EX_SP_HL(); break;
        case 0xEB: Z80_SPEND(4); Z80_OP_EX_DE_HL(); break;

        /*
         * The input and output space. These two calls need no resident
         * boundary: neither function reads or writes the processor state
         * -- the accumulator crosses by value, in the argument and in the
         * return (the contract stands at the two functions).
         */
        case 0xD3: Z80_SPEND(11); Z80_FETCH8(imm8); z80_io_write(imm8,Z80_A); break;
        case 0xDB: Z80_SPEND(11); Z80_FETCH8(imm8); Z80_A = z80_io_read(imm8); break;

        case 0xF3: Z80_SPEND(4); Z80_OP_DI(); break;
        case 0xFB: Z80_SPEND(4); Z80_OP_EI(); break;

        /*
         * The prefix that opens the block instructions, the sixteen bit
         * arithmetic and the interrupt registers. Its whole price rides on
         * its own cases, the unprefixed table holding zero for the prefix
         * byte: each case spends its entry of the prefix's cost table as a
         * literal, the way the outer switch does, and it stops the core the
         * same way this switch does when it meets a byte it has no
         * instruction for -- spending that byte's table entry first.
         *
         * DECODED WHERE IT STANDS AND NOT BEHIND A CALL, like the CB prefix
         * below -- and it used to be a call, argued by the arithmetic that
         * these instructions cost eight to twenty T-states against the 29 to
         * 39 ARM cycles of an uncached call. Measurement inverted that
         * argument: the call did not travel alone. It carried a flush/reload
         * boundary -- ten copies and a composition of R around every prefixed
         * instruction -- and the body it reached ran against the structure
         * and the page tables, outside everything the resident window buys,
         * so the prefixed stretches of the instruction exerciser were the
         * slowest regime of the whole run. Expanded here, the body inherits
         * the residents and the bare refresh tick, and the boundary
         * disappears with the call.
         *
         * The calls that remain inside this block -- z80_set_im and the two
         * port functions -- are the tolerated ones: none of them reads or
         * writes the five hot fields (see the input/output block above).
         */
        case 0xED:
          {
            uint8 edop;
            uint16 edaddr;

            edop = Z80_RD8(Z80_PC);
            Z80_PC = (uint16)(Z80_PC + 1);
            Z80_TICK_R();

            switch(edop)
              {
              /*
               * A port into a register, and one form that reads a port for
               * its flags alone. The port is the one C names, and where it
               * leads is provisional.
               */
              case 0x40: Z80_SPEND(12); Z80_OP_IN_C(Z80_B); break;
              case 0x48: Z80_SPEND(12); Z80_OP_IN_C(Z80_C); break;
              case 0x50: Z80_SPEND(12); Z80_OP_IN_C(Z80_D); break;
              case 0x58: Z80_SPEND(12); Z80_OP_IN_C(Z80_E); break;
              case 0x60: Z80_SPEND(12); Z80_OP_IN_C(Z80_H); break;
              case 0x68: Z80_SPEND(12); Z80_OP_IN_C(Z80_L); break;
              case 0x70: Z80_SPEND(12); Z80_OP_IN_C_DROP(); break;
              case 0x78: Z80_SPEND(12); Z80_OP_IN_C(Z80_A); break;

              /*
               * A register out to that same port, and one form that writes
               * nought.
               */
              case 0x41: Z80_SPEND(12); Z80_OP_OUT_C(Z80_B); break;
              case 0x49: Z80_SPEND(12); Z80_OP_OUT_C(Z80_C); break;
              case 0x51: Z80_SPEND(12); Z80_OP_OUT_C(Z80_D); break;
              case 0x59: Z80_SPEND(12); Z80_OP_OUT_C(Z80_E); break;
              case 0x61: Z80_SPEND(12); Z80_OP_OUT_C(Z80_H); break;
              case 0x69: Z80_SPEND(12); Z80_OP_OUT_C(Z80_L); break;
              case 0x71: Z80_SPEND(12); Z80_OP_OUT_C(0); break;
              case 0x79: Z80_SPEND(12); Z80_OP_OUT_C(Z80_A); break;

              /* Sixteen bit arithmetic through the carry, both directions. */
              case 0x4A: Z80_SPEND(15); Z80_OP_ADC_HL(Z80_BC); break;
              case 0x5A: Z80_SPEND(15); Z80_OP_ADC_HL(Z80_DE); break;
              case 0x6A: Z80_SPEND(15); Z80_OP_ADC_HL(Z80_HL); break;
              case 0x7A: Z80_SPEND(15); Z80_OP_ADC_HL(Z80_SP); break;

              case 0x42: Z80_SPEND(15); Z80_OP_SBC_HL(Z80_BC); break;
              case 0x52: Z80_SPEND(15); Z80_OP_SBC_HL(Z80_DE); break;
              case 0x62: Z80_SPEND(15); Z80_OP_SBC_HL(Z80_HL); break;
              case 0x72: Z80_SPEND(15); Z80_OP_SBC_HL(Z80_SP); break;

              /*
               * A pair through a direct address, both ways, the stack pointer
               * included.
               */
              case 0x43: Z80_SPEND(20); Z80_FETCH16(edaddr); Z80_WR16(edaddr,Z80_BC); break;
              case 0x53: Z80_SPEND(20); Z80_FETCH16(edaddr); Z80_WR16(edaddr,Z80_DE); break;
              case 0x63: Z80_SPEND(20); Z80_FETCH16(edaddr); Z80_WR16(edaddr,Z80_HL); break;
              case 0x73: Z80_SPEND(20); Z80_FETCH16(edaddr); Z80_WR16(edaddr,Z80_SP); break;

              case 0x4B: Z80_SPEND(20); Z80_FETCH16(edaddr); Z80_SET_BC(Z80_RD16(edaddr)); break;
              case 0x5B: Z80_SPEND(20); Z80_FETCH16(edaddr); Z80_SET_DE(Z80_RD16(edaddr)); break;
              case 0x6B: Z80_SPEND(20); Z80_FETCH16(edaddr); Z80_SET_HL(Z80_RD16(edaddr)); break;
              case 0x7B: Z80_SPEND(20); Z80_FETCH16(edaddr); Z80_SP = Z80_RD16(edaddr); break;

              /*
               * Negation. Eight opcodes reach it, one documented and seven
               * not (sms_z80.c:1828-1831).
               */
              case 0x44: case 0x4C: case 0x54: case 0x5C:
              case 0x64: case 0x6C: case 0x74: case 0x7C:
                Z80_SPEND(8);
                Z80_OP_NEG();
                break;

              /*
               * The two returns. Seven opcodes reach the first and one the
               * second, which is the whole of what separates them in the
               * encoding.
               */
              case 0x45: case 0x55: case 0x5D: case 0x65:
              case 0x6D: case 0x75: case 0x7D:
                Z80_SPEND(14);
                Z80_OP_RETN();
                break;

              case 0x4D: Z80_SPEND(14); Z80_OP_RETI(); break;

              /* Selecting an interrupt mode: recorded, traced, nothing else. */
              case 0x46: case 0x66: Z80_SPEND(8); z80_set_im(0); break;
              case 0x56: case 0x76: Z80_SPEND(8); z80_set_im(1); break;
              case 0x5E: case 0x7E: Z80_SPEND(8); z80_set_im(2); break;

              /* The interrupt vector, through the accumulator. */
              case 0x47: Z80_SPEND(9); Z80_I = Z80_A; break;
              case 0x57: Z80_SPEND(9); Z80_OP_LD_A_I(); break;

              /*
               * LD R,A writes BOTH HOMES of the refresh register. Inside the
               * window Z80_R is the free-running counter, current in its low
               * seven bits alone; bit 7 lives in the field, and this store is
               * the one instruction that changes it. So the field takes the
               * whole byte -- bit 7 with it -- and the counter restarts from
               * the accumulator, so the ticks that follow count on from its
               * low seven bits. The bit 7 the counter carries away is inert:
               * every composition masks it off, the same argument the entry
               * copy's bare R line rests on (see the entry boundary).
               */
              case 0x4F:
                Z80_SPEND(9);
                Z80_R_STATE = Z80_A;
                Z80_R = Z80_A;
                break;

              /*
               * LD A,R reads THE COMPOSITION, never the bare counter: bit 7
               * from the field, its only home, and the low seven bits from
               * the counter, the only place they are current -- the very
               * line the flush uses to rebuild the architectural byte,
               * applied in place. The (uint32) cast is for armcc, as at the
               * flush.
               */
              case 0x5F:
                Z80_SPEND(9);
                {
                  uint8 edval;

                  edval = (uint8)(((uint32)Z80_R_STATE & 0x80U) |
                                  (Z80_R & 0x7FU));
                  Z80_A = edval;
                  Z80_LD_A_IR_FLAGS(edval);
                }
                break;

              case 0x67: Z80_SPEND(18); Z80_OP_RRD(); break;
              case 0x6F: Z80_SPEND(18); Z80_OP_RLD(); break;

              /* The four transfers and the four comparisons. */
              case 0xA0: Z80_SPEND(16); Z80_OP_LDI(); break;
              case 0xA8: Z80_SPEND(16); Z80_OP_LDD(); break;
              case 0xB0: Z80_SPEND(16); Z80_OP_LDIR(); break;
              case 0xB8: Z80_SPEND(16); Z80_OP_LDDR(); break;

              case 0xA1: Z80_SPEND(16); Z80_OP_CPI(); break;
              case 0xA9: Z80_SPEND(16); Z80_OP_CPD(); break;
              case 0xB1: Z80_SPEND(16); Z80_OP_CPIR(); break;
              case 0xB9: Z80_SPEND(16); Z80_OP_CPDR(); break;

              /*
               * The eight block forms of input and output, mechanics
               * complete, port not.
               */
              case 0xA2: Z80_SPEND(16); Z80_OP_INI(); break;
              case 0xAA: Z80_SPEND(16); Z80_OP_IND(); break;
              case 0xB2: Z80_SPEND(16); Z80_OP_INIR(); break;
              case 0xBA: Z80_SPEND(16); Z80_OP_INDR(); break;

              case 0xA3: Z80_SPEND(16); Z80_OP_OUTI(); break;
              case 0xAB: Z80_SPEND(16); Z80_OP_OUTD(); break;
              case 0xB3: Z80_SPEND(16); Z80_OP_OTIR(); break;
              case 0xBB: Z80_SPEND(16); Z80_OP_OTDR(); break;

              /*
               * Every byte this prefix leaves undefined lands here, and it
               * stops rather than carrying on quietly. The part is said to
               * treat them as two byte no operations; no source in this
               * repository says so, and the source that is here treats them
               * as a fault (sms_z80.c:1869-1872). A stop that names the byte
               * can be turned into a no operation the day something needs
               * it, where a silent no operation cannot be turned back into a
               * diagnosis.
               */
              default:
                /*
                 * The byte's price is spent before the stop, out of the
                 * table: this path has no case to carry a literal for it,
                 * and the counter the flush records has to hold the same
                 * value it would have held had the byte been executable.
                 */
                Z80_SPEND(z80_cycles_ed[edop]);
                LOG_ONCE(LOG_CAT_Z80,LOG_LVL_ERR,
                         ("unimplemented opcode=0xed%02lx pc=0x%04lx",
                          (unsigned long)edop,
                          (((unsigned long)Z80_PC) - 2UL) & 0xFFFFUL));
                /*
                 * Exit boundary: locals -> structure. The stop was decided
                 * here, on the locals, so they are the truth -- pc among
                 * them, which z80_pc() reads once this returns.
                 */
                Z80_RESIDENT_FLUSH();
                z80_stopped = 1;
                return 0;
              }
          }
          break;

        /*
         * The prefix that opens the rotations, the shifts and the three
         * operations on a single bit. Its whole price is charged inside this
         * case -- a literal eight on every register operand, its table's
         * entry on the memory operand, where the price is irregular -- and
         * every one of its 256 bytes is an instruction: this case has no way
         * of stopping the core, which is why it is written inline and not as
         * something that could report a failure.
         *
         * DECODED WHERE IT STANDS AND NOT BEHIND A CALL, unlike the block
         * prefix above, and the reason is arithmetic. Every form of this prefix
         * that names a register costs eight T-states, which is 27.9 ARM cycles
         * at the budget this port holds; an uncached call costs 29 to 39 before
         * the instruction it reaches has done anything. The block prefix can
         * afford one because most of its instructions cost twelve to twenty and
         * its rarest cost is its cheapest; here the cheapest cost is also the
         * commonest, and a call would be the largest single item in it.
         *
         * Three fields, the same three the unprefixed set uses: the two top
         * bits pick the family, the middle three the operation or the bit
         * number, the low three the operand -- six meaning the byte HL points
         * at (sms_z80.c:286-320, :1482-1517). So the operand is decoded once,
         * before it is known which of the eleven operations will receive it.
         */
        case 0xCB:
          {
            uint8 cbop;
            uint8 cbval;
            uint8 cbres;
            uint8 cbundoc;
            uint8 cbwrote;
            uint16 cbaddr;

            cbop = Z80_RD8(Z80_PC);
            Z80_PC = (uint16)(Z80_PC + 1);
            Z80_TICK_R();

            /*
             * The operand, read by value and written back by the same
             * three-bit field, WITH NO POINTER ANYWHERE. This block used to
             * hold a pointer to the selected register, and one of the eight
             * is the accumulator -- a register-resident local of this
             * function whose address must never be taken, or every store
             * through the pointer could alias it and the residency above is
             * lost. So the operand is decoded twice, by two dense switches
             * the compiler turns into table jumps, instead of once into a
             * pointer whose every use is a store the compiler cannot place.
             *
             * The address is composed once and held rather than recomposed
             * for the write, because the two halves of a read-modify-write
             * have to land on the same place even once a mapper can move a
             * page between them.
             *
             * Neither nought below is a value anything reads: they are what
             * keeps a data flow analysis quiet about the path that touches
             * neither -- the address on a register operand, the result on the
             * one operation that produces none.
             *
             * THE PRICE IS SPENT WHERE THE OPERAND IS DECODED, because that
             * is the first point that knows it. Every register form costs
             * eight whatever the operation, so the seven register cases each
             * carry the literal; the memory operand is the one form whose
             * price depends on the operation still to be decoded -- twelve
             * to test a bit, fifteen to change one -- so that case, and it
             * alone, reads the prefix's cost table.
             */
            cbaddr = 0;
            cbres = 0;

            switch(cbop & 0x07U)
              {
              case 0x0: Z80_SPEND(8); cbval = Z80_B; break;
              case 0x1: Z80_SPEND(8); cbval = Z80_C; break;
              case 0x2: Z80_SPEND(8); cbval = Z80_D; break;
              case 0x3: Z80_SPEND(8); cbval = Z80_E; break;
              case 0x4: Z80_SPEND(8); cbval = Z80_H; break;
              case 0x5: Z80_SPEND(8); cbval = Z80_L; break;
              case 0x6: Z80_SPEND(z80_cycles_cb[cbop]); cbaddr = Z80_HL; cbval = Z80_RD8(cbaddr); break;
              default:  Z80_SPEND(8); cbval = Z80_A; break;
              }

            /*
             * Which byte the two undocumented flags of a bit test are taken
             * from, and only that instruction reads it: the operand when the
             * operand is a register, and the high byte of the address when it
             * is memory -- which for this prefix is H itself.
             */
            cbundoc = ((cbop & 0x07U) == 0x6U) ? (uint8)(cbaddr >> 8) : cbval;

            Z80_CB_APPLY(cbop,cbval,cbundoc,cbres,cbwrote);

            /*
             * Ten of the eleven operations leave a result; the bit test leaves
             * flags alone and nothing else, which on an operand of HL is the
             * whole difference between a read and a read-modify-write.
             */
            if(cbwrote)
              {
                switch(cbop & 0x07U)
                  {
                  case 0x0: Z80_B = cbres; break;
                  case 0x1: Z80_C = cbres; break;
                  case 0x2: Z80_D = cbres; break;
                  case 0x3: Z80_E = cbres; break;
                  case 0x4: Z80_H = cbres; break;
                  case 0x5: Z80_L = cbres; break;
                  case 0x6: Z80_WR8(cbaddr,cbres); break;
                  default:  Z80_A = cbres; break;
                  }
              }
          }
          break;

        /*
         * The two prefixes that select an index register. One says IX and the
         * other says IY, and that is the whole of what separates them: ONE
         * BODY SERVES BOTH, through two local pointers at the halves of the
         * chosen pair. Two copies of the switch below would be two opcode
         * tables to keep in step, and the day they drifted apart the
         * difference would show up as one index register behaving unlike the
         * other on a single opcode -- the kind of defect that survives a long
         * time. Taking the halves' addresses is safe here: they are fields of
         * the structure, never one of the resident locals, whose addresses
         * must stay untaken.
         *
         * THE BYTE BEHIND THE PREFIX IS READ ONCE, and that one read serves
         * both decisions made on it -- the inert test and the dispatch. An
         * instruction that names neither H nor L nor the byte HL points at
         * gives the prefix nothing to substitute. That case is not executed
         * here: the prefix is charged the literal four -- its own price,
         * which every inert entry of the indexed table holds -- and the byte
         * is left where it lies, so the loop fetches it on its next turn as
         * the unprefixed opcode it is -- charging its own price, ticking the
         * refresh counter once more the way a second opcode read does, and,
         * above all, consuming no displacement byte that was never emitted.
         *
         * The peeked byte is kept in the immediate operand rather than in a
         * local of its own. This function is entered on every scanline
         * whether or not it executes anything, and what it costs to enter
         * grows with the frame it has to set up. The cases below may
         * overwrite that operand once the dispatch has jumped: the switch
         * has read it by then.
         *
         * DECODED WHERE IT STANDS AND NOT BEHIND A CALL, for the reason the
         * ED prefix above gives: the call carried a flush/reload boundary
         * and a body outside the resident window, and the indexed stretches
         * of the instruction exerciser were the slowest regime of the run for it.
         *
         * TWO CASE LABELS, ONE SHARED TAIL, AND THE LABEL IS WHAT SAYS IX OR
         * IY -- the opcode byte is never read again once the dispatch has
         * jumped. That is the one goto of this file and it is paid for: any
         * read of the opcode after the dispatch -- an IX-or-IY test here was
         * the last one -- kept the opcode alive across the whole switch, and
         * with every callee-saved register already held by the residents,
         * armcc answered by giving the opcode a stack home: a store and two
         * reloads IN THE LOOP HEADER, paid by every instruction of the run
         * to serve the rare ones. Two entry stubs that each set the half
         * pointers and meet at the label keep the body single and let the
         * opcode die where it dispatched. The inert path sets the pointers
         * too and never uses them -- two register moves, cheaper than the
         * alternative everywhere it matters.
         */
        case 0xDD:
          ixy_hi = &Z80_IXH;
          ixy_lo = &Z80_IXL;
          goto ddfd_execute;

        case 0xFD:
          ixy_hi = &Z80_IYH;
          ixy_lo = &Z80_IYL;
          /* falls through to the shared tail */

        ddfd_execute:
          imm8 = Z80_RD8(Z80_PC);

          if(Z80_DDFD_INERT(imm8))
            {
              Z80_SPEND(4);
              break;
            }

          {
            uint16 pair;
            uint16 ixaddr;

            Z80_PC = (uint16)(Z80_PC + 1);
            Z80_TICK_R();

            /*
             * The pair, composed once on entry. Safe here, and it is worth
             * saying why rather than leaving it to be rediscovered, because
             * plenty of cases below do write a half: the sixteen bit
             * addition, the immediate load of the pair and the one through a
             * direct address, the increment and the decrement of the pair,
             * the loads into a half, the increments and decrements of a
             * half, the pop, and the exchange on the stack.
             *
             * WHAT NONE OF THEM DOES IS READ THIS VALUE AFTER WRITING A
             * HALF, and that is the only thing that would make it stale. Two
             * of them both read and write it -- the pair added to itself,
             * and the exchange on the stack -- and both read before they
             * write, inside a single operation. A case added below that
             * wrote a half and then wanted the pair would have to compose it
             * again; this comment is here so that whoever writes it knows
             * to.
             */
            pair = Z80_PAIR(*ixy_hi,*ixy_lo);

            switch(imm8)
              {
              /*
               * Sixteen bit addition into the pair. The pair added to itself
               * is one.
               */
              case 0x09: Z80_SPEND(15); Z80_OP_ADD16(*ixy_hi,*ixy_lo,Z80_BC); break;
              case 0x19: Z80_SPEND(15); Z80_OP_ADD16(*ixy_hi,*ixy_lo,Z80_DE); break;
              case 0x29: Z80_SPEND(15); Z80_OP_ADD16(*ixy_hi,*ixy_lo,pair); break;
              case 0x39: Z80_SPEND(15); Z80_OP_ADD16(*ixy_hi,*ixy_lo,Z80_SP); break;

              /*
               * The pair through an immediate and through a direct address,
               * both ways.
               */
              case 0x21: Z80_SPEND(14); Z80_FETCH8(*ixy_lo); Z80_FETCH8(*ixy_hi); break;
              case 0x22: Z80_SPEND(20); Z80_FETCH16(ixaddr); Z80_WR16(ixaddr,pair); break;
              case 0x2A:
                Z80_SPEND(20);
                Z80_FETCH16(ixaddr);
                imm16 = Z80_RD16(ixaddr);
                *ixy_hi = (uint8)(imm16 >> 8);
                *ixy_lo = (uint8)(imm16 & 0xFFU);
                break;

              /*
               * Increment and decrement of the pair: by the halves, and no
               * flag moves.
               */
              case 0x23: Z80_SPEND(10); Z80_OP_INC_PAIR(*ixy_hi,*ixy_lo); break;
              case 0x2B: Z80_SPEND(10); Z80_OP_DEC_PAIR(*ixy_hi,*ixy_lo); break;

              /*
               * The halves, addressed as the eight bit registers they are.
               * This is what holding the pair as two bytes buys: an
               * immediate load, an increment and a decrement of a half are
               * the unprefixed operations, unchanged, pointed at a different
               * byte.
               */
              case 0x26: Z80_SPEND(11); Z80_FETCH8(*ixy_hi); break;
              case 0x2E: Z80_SPEND(11); Z80_FETCH8(*ixy_lo); break;
              case 0x24: Z80_SPEND(8); Z80_OP_INC_R(*ixy_hi); break;
              case 0x2C: Z80_SPEND(8); Z80_OP_INC_R(*ixy_lo); break;
              case 0x25: Z80_SPEND(8); Z80_OP_DEC_R(*ixy_hi); break;
              case 0x2D: Z80_SPEND(8); Z80_OP_DEC_R(*ixy_lo); break;

              /*
               * The displaced byte. The displacement is read into the
               * address before anything else happens, and the immediate of
               * the third case is read after it -- that order is the
               * instruction's own (sms_z80.c:1633-1638).
               */
              case 0x34:
                Z80_SPEND(23);
                Z80_FETCH_IXY_ADDR(pair,ixaddr);
                Z80_OP_INC_MEM_AT(ixaddr);
                break;

              case 0x35:
                Z80_SPEND(23);
                Z80_FETCH_IXY_ADDR(pair,ixaddr);
                Z80_OP_DEC_MEM_AT(ixaddr);
                break;

              case 0x36:
                Z80_SPEND(19);
                Z80_FETCH_IXY_ADDR(pair,ixaddr);
                Z80_FETCH8(imm8);
                Z80_WR8(ixaddr,imm8);
                break;

              /* A half into a register. */
              case 0x44: Z80_SPEND(8); Z80_B = *ixy_hi; break;
              case 0x45: Z80_SPEND(8); Z80_B = *ixy_lo; break;
              case 0x4C: Z80_SPEND(8); Z80_C = *ixy_hi; break;
              case 0x4D: Z80_SPEND(8); Z80_C = *ixy_lo; break;
              case 0x54: Z80_SPEND(8); Z80_D = *ixy_hi; break;
              case 0x55: Z80_SPEND(8); Z80_D = *ixy_lo; break;
              case 0x5C: Z80_SPEND(8); Z80_E = *ixy_hi; break;
              case 0x5D: Z80_SPEND(8); Z80_E = *ixy_lo; break;
              case 0x7C: Z80_SPEND(8); Z80_A = *ixy_hi; break;
              case 0x7D: Z80_SPEND(8); Z80_A = *ixy_lo; break;

              /*
               * A register into a half, the two halves into each other
               * included.
               */
              case 0x60: Z80_SPEND(8); *ixy_hi = Z80_B; break;
              case 0x61: Z80_SPEND(8); *ixy_hi = Z80_C; break;
              case 0x62: Z80_SPEND(8); *ixy_hi = Z80_D; break;
              case 0x63: Z80_SPEND(8); *ixy_hi = Z80_E; break;
              case 0x64: Z80_SPEND(8); break; /* the high half into itself -- no operation */
              case 0x65: Z80_SPEND(8); *ixy_hi = *ixy_lo; break;
              case 0x67: Z80_SPEND(8); *ixy_hi = Z80_A; break;
              case 0x68: Z80_SPEND(8); *ixy_lo = Z80_B; break;
              case 0x69: Z80_SPEND(8); *ixy_lo = Z80_C; break;
              case 0x6A: Z80_SPEND(8); *ixy_lo = Z80_D; break;
              case 0x6B: Z80_SPEND(8); *ixy_lo = Z80_E; break;
              case 0x6C: Z80_SPEND(8); *ixy_lo = *ixy_hi; break;
              case 0x6D: Z80_SPEND(8); break; /* the low half into itself -- no operation */
              case 0x6F: Z80_SPEND(8); *ixy_lo = Z80_A; break;

              /*
               * The displaced byte into a register, and back. H and L are
               * the real H and L in these two families: the prefix has
               * already been spent on the byte the instruction reaches, so
               * it has nothing left to spend on the register at the other
               * end.
               */
              case 0x46: Z80_SPEND(19); Z80_FETCH_IXY_ADDR(pair,ixaddr); Z80_B = Z80_RD8(ixaddr); break;
              case 0x4E: Z80_SPEND(19); Z80_FETCH_IXY_ADDR(pair,ixaddr); Z80_C = Z80_RD8(ixaddr); break;
              case 0x56: Z80_SPEND(19); Z80_FETCH_IXY_ADDR(pair,ixaddr); Z80_D = Z80_RD8(ixaddr); break;
              case 0x5E: Z80_SPEND(19); Z80_FETCH_IXY_ADDR(pair,ixaddr); Z80_E = Z80_RD8(ixaddr); break;
              case 0x66: Z80_SPEND(19); Z80_FETCH_IXY_ADDR(pair,ixaddr); Z80_H = Z80_RD8(ixaddr); break;
              case 0x6E: Z80_SPEND(19); Z80_FETCH_IXY_ADDR(pair,ixaddr); Z80_L = Z80_RD8(ixaddr); break;
              case 0x7E: Z80_SPEND(19); Z80_FETCH_IXY_ADDR(pair,ixaddr); Z80_A = Z80_RD8(ixaddr); break;

              case 0x70: Z80_SPEND(19); Z80_FETCH_IXY_ADDR(pair,ixaddr); Z80_WR8(ixaddr,Z80_B); break;
              case 0x71: Z80_SPEND(19); Z80_FETCH_IXY_ADDR(pair,ixaddr); Z80_WR8(ixaddr,Z80_C); break;
              case 0x72: Z80_SPEND(19); Z80_FETCH_IXY_ADDR(pair,ixaddr); Z80_WR8(ixaddr,Z80_D); break;
              case 0x73: Z80_SPEND(19); Z80_FETCH_IXY_ADDR(pair,ixaddr); Z80_WR8(ixaddr,Z80_E); break;
              case 0x74: Z80_SPEND(19); Z80_FETCH_IXY_ADDR(pair,ixaddr); Z80_WR8(ixaddr,Z80_H); break;
              case 0x75: Z80_SPEND(19); Z80_FETCH_IXY_ADDR(pair,ixaddr); Z80_WR8(ixaddr,Z80_L); break;
              case 0x77: Z80_SPEND(19); Z80_FETCH_IXY_ADDR(pair,ixaddr); Z80_WR8(ixaddr,Z80_A); break;

              /*
               * The eight arithmetic and logical operations against a half
               * of the pair, in the order the opcode's middle three bits
               * give.
               *
               * THE SOURCE OF THE SEMANTICS LISTS TWELVE OF THESE SIXTEEN,
               * and the four it leaves out -- the two with the carry in and
               * the two with the borrow in -- are not a case it declines to
               * describe. Its own cost table prices all sixteen alike, at
               * eight T-states (sms_z80.c:53-71, entries 0x8C, 0x8D, 0x9C
               * and 0x9D), and the exerciser sweeps the operation field
               * across all eight operations for both halves and expects one
               * fingerprint for the sixteen (docs/sms_gg/ZEXALL-SMS-0.21/source/zexall.sms.asm:764-789). The
               * four are written here on the same rule as the twelve: where
               * the instruction said H, it says the high half.
               */
              case 0x84: Z80_SPEND(8); Z80_OP_ADD(*ixy_hi,0); break;
              case 0x85: Z80_SPEND(8); Z80_OP_ADD(*ixy_lo,0); break;
              case 0x8C: Z80_SPEND(8); Z80_OP_ADD(*ixy_hi,Z80_FLAG_GET(Z80_FLAG_C)); break;
              case 0x8D: Z80_SPEND(8); Z80_OP_ADD(*ixy_lo,Z80_FLAG_GET(Z80_FLAG_C)); break;
              case 0x94: Z80_SPEND(8); Z80_OP_SUB(*ixy_hi,0); break;
              case 0x95: Z80_SPEND(8); Z80_OP_SUB(*ixy_lo,0); break;
              case 0x9C: Z80_SPEND(8); Z80_OP_SUB(*ixy_hi,Z80_FLAG_GET(Z80_FLAG_C)); break;
              case 0x9D: Z80_SPEND(8); Z80_OP_SUB(*ixy_lo,Z80_FLAG_GET(Z80_FLAG_C)); break;
              case 0xA4: Z80_SPEND(8); Z80_OP_AND(*ixy_hi); break;
              case 0xA5: Z80_SPEND(8); Z80_OP_AND(*ixy_lo); break;
              case 0xAC: Z80_SPEND(8); Z80_OP_XOR(*ixy_hi); break;
              case 0xAD: Z80_SPEND(8); Z80_OP_XOR(*ixy_lo); break;
              case 0xB4: Z80_SPEND(8); Z80_OP_OR(*ixy_hi); break;
              case 0xB5: Z80_SPEND(8); Z80_OP_OR(*ixy_lo); break;
              case 0xBC: Z80_SPEND(8); Z80_OP_CP(*ixy_hi); break;
              case 0xBD: Z80_SPEND(8); Z80_OP_CP(*ixy_lo); break;

              /* The same eight against the displaced byte. */
              case 0x86:
                Z80_SPEND(19);
                Z80_FETCH_IXY_ADDR(pair,ixaddr);
                Z80_OP_ADD(Z80_RD8(ixaddr),0);
                break;
              case 0x8E:
                Z80_SPEND(19);
                Z80_FETCH_IXY_ADDR(pair,ixaddr);
                Z80_OP_ADD(Z80_RD8(ixaddr),Z80_FLAG_GET(Z80_FLAG_C));
                break;
              case 0x96:
                Z80_SPEND(19);
                Z80_FETCH_IXY_ADDR(pair,ixaddr);
                Z80_OP_SUB(Z80_RD8(ixaddr),0);
                break;
              case 0x9E:
                Z80_SPEND(19);
                Z80_FETCH_IXY_ADDR(pair,ixaddr);
                Z80_OP_SUB(Z80_RD8(ixaddr),Z80_FLAG_GET(Z80_FLAG_C));
                break;
              case 0xA6: Z80_SPEND(19); Z80_FETCH_IXY_ADDR(pair,ixaddr); Z80_OP_AND(Z80_RD8(ixaddr)); break;
              case 0xAE: Z80_SPEND(19); Z80_FETCH_IXY_ADDR(pair,ixaddr); Z80_OP_XOR(Z80_RD8(ixaddr)); break;
              case 0xB6: Z80_SPEND(19); Z80_FETCH_IXY_ADDR(pair,ixaddr); Z80_OP_OR(Z80_RD8(ixaddr)); break;
              case 0xBE: Z80_SPEND(19); Z80_FETCH_IXY_ADDR(pair,ixaddr); Z80_OP_CP(Z80_RD8(ixaddr)); break;

              /*
               * The stack, the exchange on it, and the two jumps through the
               * pair.
               */
              case 0xE1: Z80_SPEND(14); Z80_OP_POP(*ixy_hi,*ixy_lo); break;
              case 0xE3: Z80_SPEND(23); Z80_OP_EX_SP_PAIR(*ixy_hi,*ixy_lo); break;
              case 0xE5: Z80_SPEND(15); Z80_OP_PUSH(pair); break;
              case 0xE9: Z80_SPEND(8); Z80_PC = pair; break;
              case 0xF9: Z80_SPEND(10); Z80_SP = pair; break;

              /*
               * The double prefix, and its four bytes come in an order of
               * their own: prefix, prefix, DISPLACEMENT, opcode. The
               * displacement arrives before the opcode here and after it
               * everywhere else in this switch. That inversion is what makes
               * this path different to read, and it is the thing about it
               * that gets forgotten.
               *
               * The full order of the accesses below, since three of the
               * four reads are not fetches of the instruction and the
               * summary above hides one of them:
               *
               *   the displacement byte, out of the instruction stream
               *   THE OPERAND, read at the address the displacement composes
               *   the opcode byte, out of the instruction stream
               *
               * The operand is read BETWEEN the displacement and the opcode,
               * so the byte at the address is fetched before it is known
               * which operation will receive it. That is the order of the
               * source of the semantics, read off sms_z80.c:1532-1534, and
               * it is kept rather than tidied: the day a read of emulated
               * memory carries an effect of its own, when it happens
               * relative to the rest will have stopped being a matter of
               * taste.
               *
               * Everything else is the operation table of the single prefix,
               * unchanged and not copied. What this path adds is where the
               * operand comes from and where the result goes, and those two
               * are not the same place: the operand is always the displaced
               * byte, while the result goes to the register the low three
               * bits name, or to that same byte when they name it.
               */
              case 0xCB:
                {
                  uint8 cbop;
                  uint8 cbval;
                  uint8 cbres;
                  uint8 cbwrote;

                  /*
                   * The nought is not a value anything reads: it keeps a
                   * data flow analysis quiet about the one operation that
                   * produces no result.
                   */
                  cbres = 0;

                  Z80_FETCH_IXY_ADDR(pair,ixaddr);
                  cbval = Z80_RD8(ixaddr);
                  Z80_FETCH8(cbop);

                  /*
                   * The refresh counter is NOT ticked for this byte. The
                   * part increments it once per opcode read and this
                   * instruction reads two, both of them prefixes; the byte
                   * that names the operation is fetched like an operand and
                   * not like an opcode.
                   *
                   * The two undocumented flags of a bit test come off the
                   * high byte of the address, which is the case the source
                   * of the semantics singles out with a comment of its own
                   * (sms_z80.c:1557-1561) -- and the only place in the whole
                   * instruction set where those two bits come from neither a
                   * result nor an operand.
                   */
                  Z80_CB_APPLY(cbop,cbval,(uint8)(ixaddr >> 8),cbres,cbwrote);

                  if(cbwrote)
                    {
                      /*
                       * ONE DESTINATION, AND THE OTHER ONE IS KNOWINGLY LEFT
                       * OUT. The seven forms whose low three bits name a
                       * register are the ones no data sheet describes, and
                       * on the part they are held to write the result TWICE:
                       * into memory at the displaced address, and into the
                       * named register. Only the register is written here.
                       *
                       * That is a decision and not an oversight. No source
                       * in this repository establishes the double write --
                       * the hardware notes are silent on these forms, and
                       * the source of the semantics writes to one
                       * destination without remarking on it
                       * (sms_z80.c:1540-1550) -- so the choice is to follow
                       * what can be cited rather than what cannot. The
                       * exerciser accepts the result as it stands.
                       *
                       * IF THAT EVER CHANGES, THE SECOND WRITE IS A QUESTION
                       * TO PUT AND NEVER AN ADDITION MADE HERE. A memory
                       * write added on the strength of a failing test would
                       * be a guess wearing the clothes of a fix, and it
                       * would go through the one write path at that -- so it
                       * would turn a mapper bank on this machine, on
                       * instructions nothing has shown to do so.
                       */
                      switch(cbop & 0x07U)
                        {
                        case 0x0: Z80_B = cbres; break;
                        case 0x1: Z80_C = cbres; break;
                        case 0x2: Z80_D = cbres; break;
                        case 0x3: Z80_E = cbres; break;
                        case 0x4: Z80_H = cbres; break;
                        case 0x5: Z80_L = cbres; break;
                        case 0x6: Z80_WR8(ixaddr,cbres); break;
                        default:  Z80_A = cbres; break;
                        }

                      Z80_SPEND(Z80_DDFD_CB_RMW_CYCLES);
                    }
                  else
                    {
                      Z80_SPEND(Z80_DDFD_CB_BIT_CYCLES);
                    }
                }
                break;

              /*
               * A byte this switch has no case for, and it is not
               * necessarily a hole. The prefix has something to substitute
               * in only part of the instruction set; where it has nothing,
               * the inert test above has already recognised it and never
               * reaches here. What is left is the bytes no source in this
               * repository describes behind an index prefix -- the prefixed
               * halt among them -- and they stop the core rather than being
               * guessed at. The line names both bytes and the address of the
               * first, because that is where the instruction begins and the
               * second byte alone identifies nothing.
               */
              default:
                /*
                 * The byte's price is spent before the stop, out of the
                 * table: no case carries a literal for it, and the counter
                 * the flush records has to hold the same value it would
                 * have held had the byte been executable.
                 */
                Z80_SPEND(z80_cycles_ddfd[imm8]);
                /*
                 * The prefix byte is read back out of emulated memory for
                 * the line, rather than kept from the fetch: keeping it
                 * would be the last read of the opcode local after the
                 * dispatch, and that is the read whose price lands in the
                 * loop header (see the case comment). Exact by
                 * construction: PC has moved two bytes past the prefix and
                 * nothing has written emulated memory since it was fetched,
                 * so the byte at PC-2 is the prefix. Cold path -- it runs
                 * once, on the way to a stop.
                 */
                LOG_ONCE(LOG_CAT_Z80,LOG_LVL_ERR,
                         ("unimplemented opcode=0x%02lx%02lx pc=0x%04lx",
                          (unsigned long)Z80_RD8((uint16)(Z80_PC - 2)),
                          (unsigned long)imm8,
                          (((unsigned long)Z80_PC) - 2UL) & 0xFFFFUL));
                /*
                 * Exit boundary: locals -> structure, the rule of every stop
                 * decided in the window -- pc among the truth, which
                 * z80_pc() reads once this returns.
                 */
                Z80_RESIDENT_FLUSH();
                z80_stopped = 1;
                return 0;
              }
          }
          break;

        /*
         * An unprefixed opcode with no case of its own is a hole in this
         * switch and not a property of the processor: all 256 are meant to be
         * covered. The line says which one, because that is the only way the
         * hole gets found.
         */
        default:
          /*
           * The opcode's price is spent before the stop, out of the table:
           * a hole has no case to carry a literal, and the counter the
           * flush records has to hold the same value it would have held
           * had the opcode been executable.
           *
           * One read serves the spend and the line, back out of emulated
           * memory rather than from the opcode local: a read of that local
           * after the dispatch is what turns the loop header into a spill,
           * and this path runs once. Exact by construction: PC has moved
           * one byte past the opcode and nothing has written since. The
           * byte lands in the immediate operand -- a local this function
           * already carries, because a fresh one, even block-scoped, would
           * reshuffle the register allocation of the whole function; the
           * stop consumes it before anything else could want it.
           */
          imm8 = Z80_RD8((uint16)(Z80_PC - 1));
          Z80_SPEND(z80_cycles[imm8]);
          LOG_ONCE(LOG_CAT_Z80,LOG_LVL_ERR,
                   ("unimplemented opcode=0x%02lx pc=0x%04lx",
                    (unsigned long)imm8,
                    Z80_OP_ADDR));
          /*
           * Exit boundary: locals -> structure. The stop was decided here,
           * on the locals, so they are the truth -- pc among them, which
           * z80_pc() reads once this returns.
           */
          Z80_RESIDENT_FLUSH();
          z80_stopped = 1;
          return 0;
        }
    }

  /*
   * Exit boundary of the nominal return: locals -> structure, so that
   * everything outside this function -- z80_pc(), the next entry here --
   * reads exact state.
   *
   * The counter has gone to zero or past it, and how far past is what the last
   * instruction overran the quota by. Handing it back positive is what lets the
   * caller subtract it from the next quota without having to know that the
   * counter runs downwards.
   */
  Z80_RESIDENT_FLUSH();
  return -Z80_TSTATES;
}

/*
 * End of the resident window: the five hot names go back to meaning the
 * fields of sms.z80, which is what every function below -- and any code
 * added below -- must read and write, and the refresh tick goes back to its
 * masked default, re-stated verbatim from z80_ops.h and kept in agreement by
 * hand like the five names -- with the field for R there is no flush to
 * defer the mask to. The two memory accesses never left the page tables,
 * so nothing is re-stated for them. The boundary macro goes with the
 * five names; it has no meaning outside z80_run.
 */
#undef Z80_RESIDENT_FLUSH
#undef Z80_PC
#undef Z80_TSTATES
#undef Z80_R
#undef Z80_A
#undef Z80_F
#undef Z80_TICK_R
#define Z80_PC      Z80_PC_STATE
#define Z80_TSTATES Z80_TSTATES_STATE
#define Z80_R       Z80_R_STATE
#define Z80_A       Z80_A_STATE
#define Z80_F       Z80_F_STATE
#define Z80_TICK_R()                                            \
  (Z80_R = (uint8)((Z80_R & 0x80U) | ((Z80_R + 1U) & 0x7FU)))
int32
z80_step(void)
{
  /*
   * One instruction, expressed as a quota no instruction can fit inside twice.
   * The alternative would be a second copy of the switch above, and a switch
   * over 256 opcodes is not something to hold two of in a two megabyte console
   * for the sake of a call the frame loop never makes.
   *
   * The overrun is discarded, and that is correct for what this is used for:
   * the caller that steps one instruction at a time is watching the processor,
   * not pacing it. Whoever needs the time kept calls z80_run.
   */
  (void)z80_run(1);

  return z80_stopped ? Z80_STEP_UNIMPL : Z80_STEP_OK;
}

/*
 * Header: z80.h, where what this value means at each call site is written out,
 * along with the reason it is returned rather than pointed at.
 */
uint16
z80_pc(void)
{
  return Z80_PC;
}

/*
 * Header: z80.h, where who reads this and on what schedule is written out,
 * along with the reason it is a copy rather than a pointer.
 */
int32
z80_is_stopped(void)
{
  return z80_stopped;
}
