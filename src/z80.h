#ifndef SMS3DO_Z80_H
#define SMS3DO_Z80_H

#include "common.h"

/*
 * Z80 core: processor state, shape of the page tables, and the calls that
 * bring both up.
 *
 * This module owns the processor state (sms.z80 of the world structure;
 * that this file is its only writer is a declared rule of sms.h). The page
 * tables below are published objects, defined in z80.c, and they have
 * exactly one writer -- stated here and at their definition, and nowhere
 * contradicted: z80_init leaves them empty; the cartridge module installs
 * them at boot, once, after a successful load (cart.h) -- ROM low, work
 * RAM and its mirrors high, two fixed pages for what neither covers --
 * and turns banks through its own mapper code afterwards.
 *
 * The processor only ever reads them past that point: after installation
 * every entry is non-null and any write of an entry outside the cartridge
 * module is a defect.
 *
 * The processor is the same part on both emulated systems, so nothing here is
 * specific to either profile (docs/pseudocodes/10_gg_delta.md, section 0.1).
 */

/*
 * ---------------------------------------------------------------------------
 * Shape of the page tables.
 *
 * Fixed here, once, and deliberately ahead of the first opcode: every read and
 * every write of emulated memory is built on this shape, so reopening it later
 * would invalidate whatever had already been tested against the previous one.
 *
 * Three quantities, and they are the whole of it:
 *
 *   granularity          1024 bytes
 *   number of entries    64, one per kilobyte of the 64k address space
 *   content of an entry  a raw pointer into host memory
 *
 * Two tables and not one. Reads go through a table of pointers to const,
 * writes through a table of writable pointers. The const on the read side
 * costs nothing and gets the compiler to enforce, for free, that nothing
 * writes through a read mapping. Shape read off
 * TotalSMS/src/core/sms_bus.c:125-129 and sms_types.h:399-408.
 *
 * Why a kilobyte, when a 16k page -- the size of one mapper bank -- would give
 * four entries and an even cheaper index. Because the Sega mapper keeps the
 * first kilobyte of slot 0 fixed whatever bank is selected
 * (docs/sms_gg/mappers.md:62, corroborated by
 * TotalSMS/src/core/sms_bus.c:79-80). At 16k that fixed kilobyte would need a
 * test on every single access, and the cycle budget does not pay for tests on
 * this path. At 1k the special case disappears into the table: it is one entry
 * among 64 that happens to point elsewhere. The choice is made for a reason
 * that belongs to the mapper, not to the processor, which is why it is written
 * down here rather than left to be rediscovered.
 *
 * The shift and the mask below are derived from one constant instead of being
 * written out. A hardwired 10, 0x3FF or 64 anywhere on this path would turn a
 * change of granularity from impossible into silently wrong.
 * ---------------------------------------------------------------------------
 */
#define Z80_PAGE_BITS  10
#define Z80_PAGE_SIZE  (1UL << Z80_PAGE_BITS)
#define Z80_PAGE_MASK  (Z80_PAGE_SIZE - 1UL)
#define Z80_ADDR_SPACE 0x10000UL
#define Z80_PAGE_COUNT (Z80_ADDR_SPACE / Z80_PAGE_SIZE)

/*
 * The two tables, published. Reads go through pointers to const, writes
 * through writable pointers (shape: TotalSMS/src/core/sms_types.h:399-408);
 * the access macros of z80_ops.h expand onto these names. Who writes them,
 * and when, is the contract at the head of this file: the cartridge
 * module's installation at boot, and its mapper afterwards.
 */
extern const uint8 *z80_rmap[Z80_PAGE_COUNT];
extern uint8 *z80_wmap[Z80_PAGE_COUNT];

/*
 * ---------------------------------------------------------------------------
 * Flags.
 *
 * The eight masks are those of TotalSMS/src/core/sms_z80.c:93-104, the two
 * undocumented bits included: bits 3 and 5 carry no meaning of their own but
 * are copied out of results and are readable, so an emulator that dropped them
 * would differ from the part on any code that looks.
 *
 * Bit 2 is two flags under one bit -- parity after a logical operation,
 * overflow after an arithmetic one -- which is a property of the processor and
 * not an economy taken here.
 *
 * F is kept as one byte, masked, rather than as eight separate booleans. Both
 * forms work and the choice is not neutral: eight bytes make setting a flag a
 * single store but make PUSH AF recompose F out of seven shifts
 * (the recomposition TotalSMS does by hand in sms_z80.c:153-183), while
 * one masked byte makes PUSH AF free and asks each arithmetic operation to
 * compose its result. The instruction test program that will exercise this
 * core compares expected flags to obtained ones by pushing and popping AF, so
 * that path is walked constantly, and it is the one made free here.
 * ---------------------------------------------------------------------------
 */
#define Z80_FLAG_C  0x01 /* carry */
#define Z80_FLAG_N  0x02 /* last operation was a subtraction, read by DAA */
#define Z80_FLAG_PV 0x04 /* parity after logic, overflow after arithmetic */
#define Z80_FLAG_B3 0x08 /* undocumented, copied from bit 3 of a result */
#define Z80_FLAG_H  0x10 /* half carry, out of bit 3 into bit 4 */
#define Z80_FLAG_B5 0x20 /* undocumented, copied from bit 5 of a result */
#define Z80_FLAG_Z  0x40 /* result was zero */
#define Z80_FLAG_S  0x80 /* bit 7 of the result, the sign */

/*
 * One of the two general purpose register sets. There are two, main and
 * alternate, and two instructions swap between them: one exchanges the six
 * general registers, the other exchanges A and F on their own. Keeping them as
 * two instances of the same type is what makes those exchanges a swap of
 * structures rather than a walk over fields.
 * Register set: TotalSMS/src/core/sms_types.h:108-127.
 */
typedef struct
{
  uint8 b;
  uint8 c;
  uint8 d;
  uint8 e;
  uint8 h;
  uint8 l;
  uint8 a;
  uint8 f;
} z80_regset_t;

/*
 * The processor state, and nothing beyond it.
 * Fields: TotalSMS/src/core/sms_types.h:108-170.
 *
 * Every field is written by z80_reset, including the ones nothing reads yet.
 * A field left uninitialised until the story that gives it meaning is a defect
 * that shows up far from where it was made.
 */
typedef struct
{
  z80_regset_t main;
  z80_regset_t alt;

  uint16 pc;
  uint16 sp;

  /*
   * The index registers are held as halves rather than as pairs because the
   * prefixed instructions address them both ways, by half and by pair. Holding
   * halves makes the pair a composition; holding pairs would make the half a
   * masked read and a masked write.
   */
  uint8 ixh;
  uint8 ixl;
  uint8 iyh;
  uint8 iyl;

  uint8 i; /* interrupt vector, high byte of the vector in mode 2 */

  /*
   * Memory refresh, and it is kept for real: incremented once per opcode read,
   * bit 7 left alone. The question of whether to hold it was left to the
   * instruction that reads it, and that instruction now exists.
   *
   * TotalSMS does not hold it. It answers a read with a value composed out of
   * three registers and its scheduler's tick count, saying in place that it
   * declines "this slight overhead" while describing, in the same comment,
   * what the part really does (TotalSMS/src/core/sms_z80.c:1465-1478). The
   * description is usable and the substitute is not, being built on a
   * scheduler this port does not have -- so the description is what was
   * followed, and the overhead is one addition per opcode read.
   *
   * Like the other hot fields, this one is exact only at z80_run's
   * boundaries: inside that function the increments land on a resident
   * free-running refresh counter, and the exact value is composed at each
   * boundary -- bit 7 from this field, which only structure-side code
   * writes (LD R,A stores the whole byte, reset clears it), low seven bits
   * from the refresh counter. Every reader outside z80_run runs outside
   * those windows, so what it sees is always the composed value.
   */
  uint8 r;

  /*
   * Interrupt enable flip-flops, and the one instruction delay after the
   * instruction that enables them. The disable and the enable instructions
   * write all three; the acceptance stage at the head of z80_run is what
   * reads the flip-flops and resolves the delay -- an interrupt is not taken
   * at the first sampling after an enable, so a return sitting behind one
   * still executes first (TotalSMS/src/core/sms_z80.c:2138-2144). RETN
   * restores the first flip-flop from the second, which is what makes the
   * pair a saved state rather than two flags.
   */
  uint8 iff1;
  uint8 iff2;
  uint8 ei_delay;

  /*
   * Which of the three interrupt modes is in force. Written by the three
   * instructions that select one, read at the moment a maskable interrupt is
   * accepted: mode 1 branches to 0x38, modes 0 and 2 refuse loudly -- no
   * authorised source describes their behaviour, the machine emulated here
   * only ever runs mode 1 (docs/sms_gg/SMSOfficialDocs.md:182), and an
   * invented branch would be worse than a traced refusal.
   *
   * It is held here and not derived later because the instruction that sets it
   * carries the whole of the information -- a program can select a mode
   * thousands of instructions before the first interrupt, and there is no
   * second chance to learn which one it picked.
   *
   * No reset value comes from TotalSMS: it keeps no such field, its own
   * selector being empty (sms_z80.c:1057-1061). Cleared at reset like every
   * other field, which is also the mode the part comes up in.
   */
  uint8 im;

  /*
   * Whether a halt is in force. Written by the halt instruction and by the
   * acceptance of an interrupt, which is what ends one; read at the head of
   * z80_run, where a halted core consumes its whole quota without advancing
   * PC and hands the line back, so the loop keeps sampling at every boundary
   * (TotalSMS/src/core/sms_z80.c:2108-2115, form set aside, semantics kept).
   * The emulated processor may stay halted indefinitely; the emulator's
   * scanline loop never does.
   */
  uint8 halted;

  /*
   * The level the non-maskable line held at the previous sampling. The NMI is
   * taken on the edge of its line, not on the level
   * (docs/sms_gg/SMSOfficialDocs.md:141-175, falling edge of the part's pin,
   * which is this line going high), so the processor has to remember what it
   * saw last -- this is the CPU's own latch, not a copy of any source's
   * state. It occupies the byte the compiler would otherwise have inserted
   * here as padding to align the counter below.
   */
  uint8 nmi_seen;

  /*
   * The T-state counter, and it belongs to this module: this is the only place
   * that writes it, and no other subsystem needs to read the clock. At the
   * scanline grain the video part is told to advance a line, it does not ask
   * what time it is.
   *
   * Signed, because an instruction that straddles the end of a quota overruns
   * it and the overrun is carried into the next one. Reset to zero here. The
   * live counter is a local of z80_run, seeded from the quota; this field
   * receives a snapshot at that function's boundaries, and is exact whenever
   * that function is not running.
   */
  int32 tstates;

#if LOG_ENABLE && SMS_TELEMETRY
  /*
   * How many maskable interrupts the core has accepted since reset. A
   * running count, kept for one reader outside this module -- the video
   * part's periodic line, which reports the difference between two of its
   * calls -- and written here alone: the acceptance stage steps it, reset
   * clears it. It replaced a trace line per acceptance: a game accepts one
   * interrupt per frame, and a blocking serial line per frame is a frame
   * destroyed, not slowed. Kept only with the line it feeds, the rule of
   * log.h; the field sits last so that a build without it changes no
   * offset above.
   */
  uint32 irq_accepted;
#endif
} z80_t;

/*
 * Brings the core up: traces the shape of the access path it was built
 * with, so that a later trace can be believed.
 *
 * It allocates nothing and fills nothing: the tables stay empty until the
 * cartridge module installs them at boot, the one moment the ROM they must
 * point at is known.
 *
 * It does not reset the processor. A reset belongs to whoever has just put
 * a program at the bottom of the address space: the cartridge boot resets
 * from the boot sequence right after the installation -- one reset, after
 * the map is in place, so that the traced state describes the program
 * about to run.
 *
 * Returns 0, or a negative value after tracing why. The return stays in
 * the contract for the day a core has something of its own to allocate:
 * a core with no memory cannot run anything, so a caller stops on it.
 */
Err z80_init(void);

/*
 * Puts the processor back in the state it has out of reset, and traces it.
 * Values: TotalSMS/src/core/sms_z80.c:2164-2182.
 *
 * Leaves the memory and the tables alone: this resets the processor, not the
 * machine.
 */
void z80_reset(void);

/*
 * ---------------------------------------------------------------------------
 * Executing.
 *
 * Two entry points, and the difference between them is who cuts up the time.
 * z80_run is handed a budget and spends it; z80_step advances by exactly one
 * instruction and is what a caller uses when it wants to watch the processor
 * rather than let it run.
 *
 * Both are built in every configuration: the frame loop consumes a quota
 * on every scanline, so the core is reached in every build. Nothing calls
 * z80_step today; it stays as the observation entry of the core, for a
 * bench or a debugger to take.
 * ---------------------------------------------------------------------------
 */

/*
 * Executes instructions until the quota of T-states is spent, and answers with
 * the amount by which the last one overran it.
 *
 * The quota is the caller's to choose, and the caller is the frame loop: the
 * counter belongs to this module but the cutting up of time does not, because
 * what a scanline is worth is a property of the video part and of the region,
 * neither of which the processor has any business knowing.
 *
 * An instruction is never cut in half and never charged twice. The one that
 * straddles the end of the quota runs to completion, and what it spent past
 * the boundary comes back as the return value, for the caller to subtract from
 * the quota it hands over next. The value is never negative, and never above
 * one less than the dearest instruction: a quota has to have at least one
 * T-state left in it for that instruction to have been started at all. The
 * bound therefore rises as prefixes are opened. It was 18 while only the
 * unprefixed set existed, whose dearest instruction costs 19; then 20, a
 * repeated block transfer at 21 being the dearest of the two sets; it is 22
 * now that an index prefix can reach a read-modify-write costing 23.
 *
 * Returns 0 once the core has stopped, and stops for good the first time it
 * meets an opcode it cannot execute: an incomplete instruction set is the
 * expected state of this stage, and a core that kept trying would spend every
 * quota re-reading the same byte.
 *
 * The entry is also where the interrupt lines are sampled -- once per call,
 * which the frame loop makes once per scanline, the grain interrupt lines
 * are sampled at throughout this port; the
 * step entry below enters once per instruction and pays the read knowingly,
 * being a watching path and not the game path. An acceptance is charged to
 * the head of the quota -- 13 T-states for the maskable line, 11 for the
 * non-maskable one -- and stays inside the overrun bound above. A core in
 * HALT consumes the whole quota without advancing PC and hands the line
 * back, so the caller's loop keeps sampling at every boundary.
 */
int32 z80_run(int32 quota);

/*
 * Executes one instruction.
 *
 * Returns Z80_STEP_OK, or Z80_STEP_UNIMPL having traced the opcode and the
 * address it sits at, once. That trace is the normal way a run ends for as
 * long as the instruction set is incomplete, and it is read as the description
 * of the next piece of work -- so it has to be exact. An opcode reached
 * through a prefix names the prefix as well as the byte behind it: the second
 * byte alone does not identify an instruction.
 */
#define Z80_STEP_OK     0
#define Z80_STEP_UNIMPL (-1)

int32 z80_step(void);

/*
 * ---------------------------------------------------------------------------
 * The interrupt lines, and the shape a source of one must offer.
 *
 * Reads the interrupt line of whichever device owns it. The CPU never writes
 * to that device's state: the source owns the line, the CPU samples it.
 * Sampling happens on scanline boundaries only -- the line does not change
 * more often than that, and a per-instruction read would be a pure cost.
 * Semantics: TotalSMS/src/core/sms_z80.c:2103-2106.
 *
 * What this interface forbids, deliberately, is the other resolution: a
 * "raise an interrupt" call by which a source would push a flag into the
 * processor state. A source that wrote z80_t would be writing another
 * module's state, and the two resolutions are incompatible in a way no
 * later review would catch -- the game boots and the score screen never
 * comes apart. This form, and it alone, is the one the real owners of the
 * two lines -- the VDP, when it arrives, and the Pause button after it --
 * can fill without touching the Z80.
 *
 * Two lines, because the part has two pins and they do not behave alike:
 *
 *   maskable      level 0 or 1, accepted when IFF1 is up and no EI delay is
 *                 pending, at the cost and vector the acceptance stage
 *                 states (sms_z80.c:2134-2155). Its owner is the video
 *                 part: the sampling reads VDP_IRQ_LINE of vdp.h, a
 *                 conjunction of the device's two pending requests with
 *                 its two enable bits, and the device drops it when its
 *                 status register is read. No accessor of this module is
 *                 involved any more.
 *   z80_nmi_line  level of the non-maskable line, 0 or 1. The CPU accepts on
 *                 the edge -- the falling edge of the part's pin, which is
 *                 this line going high (docs/sms_gg/SMSOfficialDocs.md:
 *                 141-175) -- whatever IFF1 says. Its owner, the Pause
 *                 button, has no module yet; until it does the level comes
 *                 from the test source in z80.c, a scaffold behind
 *                 SMS_IRQ_TEST_SOURCE (common.h) that pulses the line on a
 *                 documented cadence so the acceptance path can be seen.
 *                 With the switch off the CPU samples a line held low,
 *                 folded away at compile time.
 *
 * The maskable line shows what substituting an owner looks like: the
 * macro in z80.c was retargeted to the device's macro, the stub accessor
 * and its cadence were deleted, and the sampling in z80_run did not move.
 * The non-maskable line will go the same way.
 * ---------------------------------------------------------------------------
 */
#if SMS_IRQ_TEST_SOURCE
uint8 z80_nmi_line(void);
#endif

/*
 * The program counter, as it stands between two instructions.
 *
 * By value, and that word carries the design. The processor state is a file
 * static of z80.c so that no other translation unit can name it, and therefore
 * none can write it (z80.c:26-29). Returning a copy keeps that invariant
 * enforced by the language rather than by convention, which handing back an
 * address would not: a pointer to the counter is a licence to move the
 * processor from outside, and there is no caller that should have one.
 *
 * What it holds depends on where it is read, and the difference is not a
 * detail. Called between two instructions -- which is where a diagnostic that
 * watches progress reads it -- it holds the address of the next one. Called
 * from inside a stop, it holds whatever the fetch left behind, one or more
 * bytes past the opcode that failed, and it is then not the address of
 * anything. The core names that address itself, through Z80_OP_ADDR, on the
 * line that reports the opcode (z80.c:347, z80.c:688): nothing else should try
 * to compute it a second time from this value.
 *
 * Built in every configuration: it reads state the core holds in every
 * one of them, and a diagnostic that exists only in some builds is one
 * more thing to reason about at the moment it is needed. Nothing on the
 * execution path calls it.
 */
uint16 z80_pc(void);

/*
 * Whether the core has met an opcode it cannot execute and stopped, by value.
 *
 * By value for the reason z80_pc is: the flag is a file static of z80.c, and
 * a returned copy keeps the language enforcing that nothing outside can write
 * it -- least of all clear it, which only a reset may do.
 *
 * It exists for the caller that runs the processor by quotas rather than by
 * steps. z80_step answers a stop in its return value; z80_run answers it by
 * consuming nothing, silently, because saying so on every quota would repeat
 * one fact several million times. A caller that hands out quotas needs the
 * fact once, on its own schedule -- once per frame, outside anything being
 * measured -- and this is the reading it takes. The opcode and its address
 * are not repeated here: the core traced both, once, when it stopped.
 *
 * Built in every configuration, for the reason z80_pc is: it reads state
 * the core holds in every one of them. Nothing on the execution path calls
 * it.
 */
int32 z80_is_stopped(void);

#endif /* SMS3DO_Z80_H */
