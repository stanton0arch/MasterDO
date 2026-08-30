#ifndef SMS3DO_CART_H
#define SMS3DO_CART_H

#include "common.h"
#include "vdp.h"

/*
 * Cartridge: the resident ROM buffer, what was loaded into it, and the
 * system profile fixed from it.
 *
 * Owner of every field of cart_t: this module, and this module alone. Other
 * modules read sms.cart and never write it. The profile -- system and region
 * -- is written once, when a load succeeds, and is read-only after boot: any
 * later write is a defect, not a feature. The subsystems initialised after
 * the load read sms.cart.system at their own init and name it through
 * cart_system_name below.
 *
 * The bus lives here too: this module is
 * the one writer of the Z80 page tables (z80.h), and it writes them exactly
 * once, at boot, after the profile is fixed -- ROM projected over the three
 * wired slots, work RAM and its mirrors above, two fixed pages for what
 * neither covers (cart_bus_install below). After that the entries move for
 * one reason only: the mapper the build serves (SMS_MAPPER, common.h),
 * whose registers this module keeps and whose effect this module applies,
 * in cart_mapper_write, when the Z80's one write path reaches a register
 * address -- which addresses, and what the write does, is decided here
 * and nowhere else: CART_MAPPER_TRIGGER below is the address test the
 * Z80 expands, and it is the only thing the Z80 knows of the mapper. A
 * build serving no mapper has no trigger at all. The Z80 walks the tables
 * on every access and never writes an entry. The rule is
 * stated here rather than enforced by a static because the tables, like
 * the structure below, have to be nameable from the module that fills them
 * and the module that walks them.
 *
 * What this module does not do, by design: of the ROM's bytes it reads the
 * header alone -- the magic at one of its three offsets and the high nibble
 * of the last header byte, from which system and region are fixed. The size
 * code in the header's low nibble and the checksum are neither read nor
 * validated: the bounds on the size are the five below, applied to the file
 * system's figure. It never projects the ROM into the emulated address space
 * (that is the bus), and it never enumerates a directory (a fixed pair of
 * names is tried at boot, and a later selector will hand names to the same
 * two entry points below).
 */

/*
 * The buffer is one megabyte, taken once at boot and never resized. The
 * figure is a budget decision and not a measurement of any ROM: it is the
 * largest image the port commits to running, on either system, and it is
 * allocated whole so that no later allocation depends on what was found on
 * the disc. It is a multiple of the CD block size (2048 bytes,
 * docs/3do/3do_portfolio_2.5.md:2311-2313) by construction, which the loader
 * requires of any buffer it fills.
 */
#define CART_ROM_CAPACITY 1048576UL

/*
 * The bounds a ROM size has to clear before a byte of it is read, in the
 * order they are checked: not empty, at least the smallest cartridge, at
 * most the buffer, a whole number of mapper banks, and a power of two so
 * that a bank number wraps by a mask rather than a division
 * (docs/pseudocodes/30_mappers.md:189-198; sizes retained:
 * TotalSMS/src/core/sms.c:14-23).
 */
#define CART_ROM_MIN   32768UL
#define CART_BANK_SIZE 16384UL

/*
 * The system of a loaded image is SYS_NONE / SYS_SMS / SYS_GG of common.h,
 * fixed at load time with the precedence: SMS_FORCE_SYSTEM when the build
 * sets it, else the ROM header, else the file name's extension. The header
 * speaks for the cartridge itself, so the extension only decides when the
 * header is absent or mute, and a disagreement between the two is a warning,
 * never a refusal. The trace names the criterion that decided, not just the
 * verdict.
 */

/* Longest file name the module keeps, terminator excluded. */
#define CART_NAME_MAX 63

typedef struct
{
  /*
   * The resident buffer and its capacity. The pointer is set once by
   * cart_init and never moves; the capacity is the constant above, kept as a
   * field so that a reader has the bound next to the pointer it bounds.
   */
  uint8 *rom;
  uint32 capacity;
  /*
   * What the last successful load left: the number of bytes now valid at the
   * start of the buffer, the name they came from, the system fixed for the
   * image (SYS_*, precedence above), and the region code the header carried
   * -- the high nibble of the last header byte, 0 when no header spoke. The
   * nibble's meaning (0x3/0x4 SMS, 0x5/0x6/0x7 GG) is emulated semantics
   * sourced from TotalSMS (TotalSMS/src/core/sms.c:34-41), not from any Sega
   * document; the region is extracted and traced, and no timing adaptation
   * follows from it.
   *
   * All four are zero and empty until a load has succeeded, and a failure
   * that may have touched the buffer leaves them zeroed again -- never
   * stale. cart_load alone writes them, once per successful load; after boot
   * the profile is read-only, and any later write is a defect.
   */
  uint32 size;
  char   name[CART_NAME_MAX + 1];
  int32  system;
  uint32 region;
  /*
   * The I/O side of the bus. memctl is the memory control register, port
   * $3E, as the program last wrote it -- CART_IO_MEMCTL_RESET at boot, the
   * value a Master System without a BIOS runs a cartridge under
   * (docs/sms_gg/sms_gg_hardware_notes.md:216-217). The three counters are
   * the aggregate the decoding below keeps of every access that reached an
   * empty hook: how many reads, how many writes, and the last port touched.
   * cart_io_report reads and clears them, once per emulated second at most,
   * from the frame loop.
   *
   * Written by this module's own code alone -- the CART_IO_* macros expand
   * inside the Z80's port functions, but they are this module's text and
   * touch this module's fields; the Z80 itself never writes a byte here.
   *
   * The register is eight bits wide and held in a word, so that these four
   * fields add no padding to the structure; the compare on the write path
   * is a word compare either way. The three counters exist for the trace
   * alone and are kept only when it is (LOG_ENABLE), the rule of log.h for
   * a figure that feeds one line: a silent build counts nothing.
   */
  uint32 memctl;
  uint32 io_unrouted_reads;
  uint32 io_unrouted_writes;
  uint32 io_last_port;
  /*
   * The mapper. banks is the image in 16k banks, size / CART_BANK_SIZE,
   * and bank_mask is banks - 1: a bank number written by the program wraps
   * by that mask and never by a division, which is exact because the five
   * bounds of cart_load admit powers of two only (30_mappers.md section 4).
   * Both are fixed by cart_load with the profile, in every configuration
   * and whatever the mapper.
   *
   * The four registers as the program last wrote them, named after the
   * Sega mapper's addresses because that is the mapper the default build
   * serves: $FFFC verbatim and the three bank registers already masked.
   * They start at the reset values of the 315-5235 (docs/sms_gg/
   * mappers.md:54, community reverse engineering; TotalSMS/src/core/
   * sms_bus.c:150-158 concurs): fffc=00 fffd=00 fffe=01 ffff=02. ffff is
   * kept even while the cartridge RAM holds slot 2, so that the ROM comes
   * back on the bank the program meant (30_mappers.md section 9, note C).
   * All four are held in words for the reason memctl is.
   *
   * A Codemasters build (SMS_MAPPER, common.h) reuses the same four as its
   * own state: mapper_fffd, mapper_fffe and mapper_ffff are the bank
   * registers of slots 0, 1 and 2 -- same reset values, same masking --
   * and mapper_fffc holds bit 7 of the last write to the slot 1 register,
   * the on-cart RAM request this port declares and does not honour, so
   * that its warning fires on the rising edge alone. A build serving no
   * mapper leaves all four at zero and reads none of them.
   *
   * cart_ram is the cartridge's own RAM, two banks of 16k allocated once at
   * the first bus installation and never freed: it exists so that a program
   * setting bit 3 of $FFFC finds memory on slot 2, and it survives nothing
   * -- persisting it is the save work's, not this module's. For that work:
   * the size exposed is not the size used -- most cartridges carry 8k,
   * mirrored over both banks (docs/sms_gg/mappers.md:80), so what is worth
   * persisting is not CART_RAM_SIZE by default. The Sega build alone
   * allocates and projects it; the other two leave the pointer null.
   */
  uint32 banks;
  uint32 bank_mask;
  uint32 mapper_fffc;
  uint32 mapper_fffd;
  uint32 mapper_fffe;
  uint32 mapper_ffff;
  uint8 *cart_ram;
} cart_t;

/* The cartridge RAM: two 16k banks, the second one selected by bit 2 of
   $FFFC (docs/sms_gg/mappers.md:78, community source -- the contract of
   cart_mapper_write below says on what authority). */
#define CART_RAM_BANK_SIZE CART_BANK_SIZE
#define CART_RAM_SIZE      (2UL * CART_RAM_BANK_SIZE)

/* Reset values of the four mapper registers, docs/sms_gg/mappers.md:54. */
#define CART_MAPPER_RESET_FFFC (0x00UL)
#define CART_MAPPER_RESET_FFFD (0x00UL)
#define CART_MAPPER_RESET_FFFE (0x01UL)
#define CART_MAPPER_RESET_FFFF (0x02UL)

/* The first address of the Sega mapper's four registers. */
#define CART_MAPPER_FIRST_REG (0xFFFCU)

/* The Codemasters mapper's registers cover the three ROM slots whole:
   every address below this one is a register (docs/sms_gg/mappers.md:
   103-111, community source). */
#define CART_MAPPER_CM_REG_LIMIT (0xC000U)

/*
 * CART_MAPPER_TRIGGER(addr) -- the address test of the mapper trigger,
 * the one thing Z80_WR8 (z80_ops.h) knows of the mapper. It is expanded
 * on the hot path, on every emulated write, after the byte has landed
 * through the table: one compare, and on a hit one cold call into
 * cart_mapper_write below. The build's mapper (SMS_MAPPER, common.h)
 * decides the form, here and nowhere else:
 *
 *   Sega          addr >= $FFFC  the four registers at the top of memory
 *   Codemasters   addr <  $C000  the three ROM slots, each a register
 *   none          no macro at all -- Z80_WR8 is then a plain store
 *                 through the table, and nothing in the Z80 names the
 *                 mapper
 *
 * Defined under the guard of the function it calls (z80_ops.h says why).
 *
 * Evaluates its argument once; Z80_WR8 passes it a plain local.
 */
#if SMS_MAPPER == MAPPER_SEGA
#define CART_MAPPER_TRIGGER(addr) ((addr) >= CART_MAPPER_FIRST_REG)
#elif SMS_MAPPER == MAPPER_CODEMASTERS
#define CART_MAPPER_TRIGGER(addr) ((addr) < CART_MAPPER_CM_REG_LIMIT)
#endif

/*
 * What cart_identify learns about a file without reading its bytes.
 */
typedef struct
{
  uint32 size;   /* as reported by the file system */
  int32  system; /* SYS_*, from the extension alone: no byte was read */
} cart_info_t;

/*
 * Failure codes of the loading and installing entry points, each distinct
 * because the caller that stops the console has to say which bound or which
 * step failed.
 */
#define CART_ERR_NOT_FOUND (-1) /* the file could not be opened */
#define CART_ERR_SIZE      (-2) /* the size breaks one of the five bounds */
#define CART_ERR_READ      (-3) /* the bytes could not be read whole */
#define CART_ERR_NO_BUFFER (-4) /* cart_init has not succeeded */
#define CART_ERR_NO_ROM    (-5) /* bus installation asked with no image */
#define CART_ERR_WORK_RAM  (-6) /* the work or cartridge RAM was refused */

/*
 * Allocates the resident ROM buffer, once, through sys_alloc.
 *
 * Cold: called from the boot sequence before the memory report and before
 * the seal, never again. Returns 0, or a negative code after the allocator
 * has traced its own refusal and this module has said what the block was
 * for. The caller decides that this is a stop; it cannot be anything else,
 * since without the buffer no cartridge can ever be loaded.
 */
Err cart_init(void);

/*
 * Identifies a file by name: whether it exists, how large it is, and which
 * system its extension announces. Reads no byte of it.
 *
 * The size comes from the file system's own record of the file
 * (include/3do/blockfile.h:49-51), which is what lets the bounds be checked
 * before any byte is read. On success returns 0 and fills the record; when
 * the file is not there returns CART_ERR_NOT_FOUND, having traced the name
 * and the error at debug level -- a missing file is a normal outcome of a
 * lookup, and whether it is an error is the caller's to decide. Any other
 * failure of the open, or a size the file system cannot report, is traced
 * as an error and returns CART_ERR_READ. A null name or record is refused
 * with CART_ERR_NOT_FOUND after an error line.
 *
 * Assumes nothing about when it is called or which name it is given: a
 * selector that browses a directory later will call it on each candidate.
 */
Err cart_identify(const char *name, cart_info_t *info);

/*
 * Loads a file whole into the resident buffer, after checking its size
 * against the five bounds in their fixed order.
 *
 * Never loads partially: the size is known and checked before the first byte
 * is read, and a read that returns fewer bytes than announced is a failure,
 * not a shorter ROM. Every failure traces a [CART][ERR] line naming the
 * cause -- the size read and the bound it broke, or the name and the read
 * error -- and returns the matching CART_ERR_* code. A failure met before the
 * buffer could be touched leaves the structure as it was; one met after
 * leaves it describing no ROM (size zero, no name, no system, no region).
 *
 * On success the structure describes the loaded ROM and its profile, fixed
 * here and nowhere else: the header is searched at its three offsets, and
 * the system is decided with the precedence stated above (forced constant,
 * then header, then extension). The trace carries the name, the size, the
 * header if one was found, and a system= line naming the criterion that
 * decided. A header that contradicts the extension wins and the
 * disagreement is a warning; a header whose nibble names no system is mute
 * and the extension decides; when nothing decides the image is still
 * loaded, with SYS_NONE and a warning -- none of these is a refusal.
 */
Err cart_load(const char *name);

/*
 * Installs the bus: writes all 64 entries of both Z80 page tables from the
 * loaded ROM, and allocates the 8k of work RAM the top quarter of the
 * address space is made of, once, through sys_alloc and before the seal.
 *
 * The map it leaves, and it leaves no null entry:
 *
 *   $0000-$BFFF reads   the ROM, linearly -- the three slots hold banks
 *                       0/1/2, which is what the mapper registers select at
 *                       reset; pages past the image's end read a fixed page
 *                       of 0x00.
 *   $0000-$BFFF writes  absorbed by a scrap page: the machine has no RAM
 *                       there, and the ROM re-reads intact.
 *   $C000-$FFFF         the 8k of work RAM, seen twice -- $C000-$DFFF
 *                       reflected at $E000-$FFFF, each mirror a repointed
 *                       entry and never a copy.
 *
 * Called from cart_boot after the profile is fixed and before the profile
 * line; callable again only by a later ROM selector, over the same work
 * RAM and, in the Sega build -- the only one that has it -- the same
 * cartridge RAM. Between installations the tables move
 * through cart_mapper_write alone, never through anyone else, Z80
 * included (z80.h).
 *
 * The map above is the reset state of the mapper: the registers are set
 * to their reset values here, after the tables, and traced on one mapper
 * reset line -- without re-projecting anything, since for any image of
 * 48k or more the linear map IS banks 0, 1 and 2, and for a 32k image
 * the fixed page above the image is the documented choice, which no
 * program of that size ever reads (docs/sms_gg/mappers.md:19: such
 * cartridges have no mapper to write). Installing is the only reset the
 * mapper has: a console reset that does not re-install keeps the
 * program's banks, and the day one exists it re-installs.
 *
 * What follows the map ok line depends on the mapper the build serves
 * (SMS_MAPPER, common.h). Sega: the cartridge RAM is allocated, once,
 * after the work RAM, and the four registers are reset. Codemasters: one
 * [CART][INFO] line saying the mapper is served best effort, then the
 * three slot registers are reset -- no cartridge RAM. None: one
 * [CART][WARN] line saying the mapper is unknown and the bus falls back
 * to the fixed banks 0/1/2, in place of the reset line -- there is no
 * register to reset, no cartridge RAM, and nothing will ever move an
 * entry again (the fallback is a documented map and a warning, never a
 * stop: docs/pseudocodes/30_mappers.md section 9). The load line's mapper=none and this warning's mapper
 * unknown name one and the same state, the fallback: there is no third
 * mapper behind the second word.
 *
 * Returns 0, or a distinct negative code after tracing why: CART_ERR_NO_ROM
 * when the guard above fires, CART_ERR_WORK_RAM when the work RAM or the
 * cartridge RAM is refused -- and a bus with no RAM cannot run a program,
 * so the caller stops on that one.
 */
Err cart_bus_install(void);

#if SMS_MAPPER != MAPPER_NONE
/*
 * The mapper's effect of a write at a register address, applied after the
 * byte has landed through the table: a write there does two things on this
 * machine, and the Z80's one write path, Z80_WR8 in z80_ops.h, carries both
 * -- the store through the table first, then this call when
 * CART_MAPPER_TRIGGER says the address is a register -- one write path
 * carrying both effects, never two (docs/sms_gg/mappers.md:66;
 * 30_mappers.md section 9, note B). Which addresses those
 * are is the build's mapper's, above; the two forms are described in turn
 * below. A build serving no mapper compiles neither this function nor the
 * call.
 *
 * Cold, and reached from that one site alone: the test on the address is
 * the whole cost the hot path pays, and a bank turn is a handful of
 * pointer writes a program makes a few times per frame at most. Works from
 * its two arguments and sms.cart alone, never from sms.z80 -- the resident
 * window of z80_run keeps its hot fields stale in the structure.
 *
 * SEGA (SMS_MAPPER == MAPPER_SEGA). The registers are $FFFC-$FFFF; the
 * byte reaches work RAM through the mirror first, readable back at
 * $DFFC-$DFFF. The mirror of the same RAM cells at $DFFC-$DFFF never
 * reaches here: the effect is attached to the high address, not to the
 * cell. What it does, per register (30_mappers.md section 9):
 *
 *   $FFFD  bank = value & bank_mask; pages 1..15 of slot 0 point at the
 *          LAST 15k of that bank (docs/sms_gg/mappers.md:62); page 0 is
 *          never touched, it keeps the first kilobyte of the image and
 *          the interrupt vectors in it.
 *   $FFFE  bank = value & bank_mask; the 16 pages of slot 1.
 *   $FFFF  bank = value & bank_mask, remembered always; the 16 pages of
 *          slot 2 are re-pointed only while the cartridge RAM is not on
 *          the slot (bit 3 of $FFFC clear).
 *   $FFFC  bit 3 rising: slot 2, reads and writes, becomes the cartridge
 *          RAM bank bit 2 selects (docs/sms_gg/mappers.md:74, :78 -- a
 *          community reading which GGOfficialDocs.md:435 contradicts on
 *          bit 2; the project decision of 2026-08-15 retains the
 *          community one and cites it as such). Bit 3 falling: the ROM
 *          bank $FFFF holds comes back, its writes absorbed again. Bit 2
 *          changing while bit 3 is set moves slot 2 to the other RAM bank.
 *          Bits 7, 4, 1 and 0 -- ROM write enable, RAM at $C000, bank
 *          shift -- are ignored and warned about on the rising edge of
 *          each, a project decision of 2026-08-15 (30_mappers.md section
 *          7) that no story reopens alone.
 *
 * Traces, all cold: one [BUS][DBG] bank line per change of a bank
 * register's value (a rewrite of the same value is silent: the tables
 * would not move), one [BUS][INFO] line per edge of the cartridge RAM on
 * slot 2, one [BUS][WARN] per ignored bit on its rising edge. A write that
 * changes nothing costs the compare and the return.
 *
 * CODEMASTERS (SMS_MAPPER == MAPPER_CODEMASTERS), best effort and not a
 * release commitment (human decision of 2026-08-28). Every address of the
 * three ROM slots is a register, and the register is the slot the address
 * falls in (docs/sms_gg/mappers.md:99-121, community source; emulated
 * semantics TotalSMS/src/core/sms_bus.c:174-190, :242-248 -- where it
 * takes a modulo, this masks). The byte lands on the scrap page first,
 * as any ROM write does: these registers are not readable back anywhere
 * (mappers.md:115). What it does, per slot:
 *
 *   $0000-$3FFF  bank = value & 0x7F & bank_mask; ALL 16 pages of slot 0,
 *                page 0 included -- this mapper protects no first
 *                kilobyte (mappers.md:103), so a program repointing slot
 *                0 takes its interrupt vectors with it.
 *   $4000-$7FFF  bank as above; the 16 pages of slot 1. Bit 7 of the
 *                value is the on-cart RAM request of one game
 *                (mappers.md:121, Ernie Els Golf): declared and not
 *                emulated -- the bit is stripped, the bank turns as if it
 *                were clear, and one [BUS][WARN] codemasters on-cart ram
 *                ignored bit=7 fires on its rising edge, never fatal.
 *   $8000-$BFFF  bank as above; the 16 pages of slot 2.
 *
 * $FFFC-$FFFF are plain work RAM under this mapper: a write there lands
 * in RAM and turns nothing. The reset values are 0/1/2, TotalSMS's
 * (:242-248), which is the linear map the install wrote -- the community
 * page says 0/1/0 (mappers.md:113). OPEN QUESTION, not arbitrated: which
 * of the two the hardware does is not established here, and whether a
 * program ever reads slot 2 before writing its register is not tested
 * (mappers.md:117 says most use slot 2, no more); a program that did
 * would see bank 2 here and bank 0 on the community reading. The
 * cartridge RAM of the Sega form is neither
 * allocated nor projected. Codemasters cartridges aim at PAL machines
 * (docs/sms_gg/sms_gg_hardware_notes.md:82); the 50 Hz gap is accepted
 * and not corrected (SSOT section 1.6).
 *
 * Traces, all cold: one [BUS][DBG] bank line per change of a slot
 * register's value, same spelling as the Sega form; the one warning
 * above. A rewrite of the same value costs the compare and the return.
 */
void cart_mapper_write(uint16 addr, uint8 value);
#endif /* SMS_MAPPER != MAPPER_NONE */

/*
 * ---------------------------------------------------------------------------
 * The I/O space: how a port number reaches a subsystem.
 *
 * The bus owns the decoding, the Z80 expands it. Both entry points below are
 * macros rather than functions for one reason, the cycle budget of the
 * port path: a
 * function here would be one call per IN or OUT on top of the one the core
 * already makes into its own port function, and a table of function
 * pointers would be an indirection. The text is this module's, the cost is
 * the Z80's, and each expansion site is exactly one: z80_io_read and
 * z80_io_write in z80.c. They name sms.cart, so the file that expands them
 * includes sms.h, as z80.c does -- the host bench expands them too, on
 * purpose, to exercise the map without the core.
 *
 * The map is the Master System's, decoded on two bits: the top two of the
 * port select a quarter, the lowest one picks between the two functions
 * each quarter carries. The quarters and their functions
 * (docs/sms_gg/sms_gg_hardware_notes.md:112-125; emulated semantics
 * TotalSMS/src/core/sms_bus.c:858-953 for reads, :955-1042 for writes --
 * the semantics, never the form):
 *
 *   $00-$3F  write even  memory control register ($3E), kept below
 *            write odd   I/O port control ($3F)            -> hook
 *            read        the last byte of the instruction  -> hook
 *   $40-$7F  write       PSG                               -> hook
 *            read even   V counter ($7E)                   -> hook
 *            read odd    H counter ($7F)                   -> hook
 *   $80-$BF  write even  VDP data ($BE)                    -> hook
 *            write odd   VDP control ($BF)                 -> hook
 *            read even   VDP data                          -> hook
 *            read odd    VDP status                        -> hook
 *   $C0-$FF  write       no effect, on the machine itself: neither counted
 *                        nor traced, since it is not a gap
 *            read even   joypad port A/B ($DC)             -> hook
 *            read odd    joypad port B/misc ($DD)          -> hook
 *
 * The Game Gear's own ports, $00-$06, are not decoded here: they belong to
 * the Game Gear work and until then those ports fall in the first quarter
 * like any other.
 *
 * EVERY HOOK IS NAMED. A hook is where a subsystem plugs in when it exists
 * -- the video part on the four VDP hooks and the two counters, the sound
 * part on the PSG hook, the input part on the two pad hooks. A hook still
 * empty has one provisional behaviour, written here and nowhere else: a
 * read answers 0xFF, the level an unconnected bus line settles at (and,
 * for the pads, the reading of no button pressed); a write drops the byte.
 * Each access to an empty hook is counted, never traced: the serial output
 * blocks, and a program waiting for a VBlank reads the V counter thousands
 * of times a second. The aggregate goes out through cart_io_report.
 *
 * A subsystem takes a hook over by redefining the macro's body, not by
 * touching the decoding above it. The video part has taken its six: the
 * data write expands to the macro of vdp.h in place, being the one access
 * a program makes thousands of times a frame; the five others are calls
 * into vdp.c, made a handful of times per frame or by a program waiting
 * for a line, and their accesses leave the unrouted count for the video
 * part's own aggregate (vdp_report).
 * ---------------------------------------------------------------------------
 */

/*
 * The two accounting forms behind every empty hook. The port is recorded so
 * that the aggregate line can name the last one touched; the count is what
 * the line reports. The last port is not cleared by the report: it is a
 * fact about the run, not a figure of the window, and it stays until the
 * next access overwrites it. In a silent build the count is not kept and
 * only the provisional value remains.
 *
 * Every macro of this block evaluates its port argument more than once and
 * CART_IO_MEMCTL_WRITE its value twice: the arguments must be free of side
 * effects. The two expansion sites pass plain parameters.
 */
#if LOG_ENABLE
#define CART_IO_UNROUTED_READ(port, dst)                \
  do                                                    \
    {                                                   \
      sms.cart.io_unrouted_reads++;                     \
      sms.cart.io_last_port = (uint32)(port);           \
      (dst) = (uint8)0xFF;                              \
    }                                                   \
  while(0)

#define CART_IO_UNROUTED_WRITE(port)                    \
  do                                                    \
    {                                                   \
      sms.cart.io_unrouted_writes++;                    \
      sms.cart.io_last_port = (uint32)(port);           \
    }                                                   \
  while(0)
#else
#define CART_IO_UNROUTED_READ(port, dst)  ((dst) = (uint8)0xFF)
#define CART_IO_UNROUTED_WRITE(port)      ((void)0)
#endif

/*
 * The hooks. The empty ones keep the provisional behaviour of the block
 * above; the name of each is the machine's function, not what the
 * placeholder does -- the last-byte read answers 0xFF today, not the last
 * byte of anything. The six of the video part route to vdp.h: the port
 * argument is dropped, the value or the destination goes through.
 */
#define CART_IO_LASTBYTE_READ(port, dst)   CART_IO_UNROUTED_READ((port),(dst))
#define CART_IO_CTRL_WRITE(port, v)        CART_IO_UNROUTED_WRITE(port)
#define CART_IO_PSG_WRITE(port, v)         CART_IO_UNROUTED_WRITE(port)
#define CART_IO_VCOUNTER_READ(port, dst)   ((dst) = vdp_io_vcounter_read())
#define CART_IO_HCOUNTER_READ(port, dst)   ((dst) = vdp_io_hcounter_read())
#define CART_IO_VDP_DATA_WRITE(port, v)    VDP_IO_DATA_WRITE(v)
#define CART_IO_VDP_CTRL_WRITE(port, v)    vdp_io_ctrl_write((uint8)(v))
#define CART_IO_VDP_DATA_READ(port, dst)   ((dst) = vdp_io_data_read())
#define CART_IO_VDP_STATUS_READ(port, dst) ((dst) = vdp_io_status_read())
#define CART_IO_PAD_A_READ(port, dst)      CART_IO_UNROUTED_READ((port),(dst))
#define CART_IO_PAD_B_READ(port, dst)      CART_IO_UNROUTED_READ((port),(dst))

/*
 * The memory control register, $3E, the one port this module keeps rather
 * than hands on. Its bits (docs/sms_gg/sms_gg_hardware_notes.md:206-217):
 * 7 expansion slot, 6 cartridge slot, 5 card slot, 4 work RAM, 3 BIOS ROM,
 * 2 I/O chip, each disabled when set; 1 and 0 unused. A machine with no
 * BIOS runs a cartridge under 0xAB: expansion, card and BIOS off,
 * cartridge and work RAM on.
 *
 * The register is kept and traced; its effect on the bus is not applied,
 * and the reason is said rather than left to be inferred: this port has no
 * BIOS, no card and no expansion, so there is nothing to switch to. A
 * program setting bit 6 would be cutting the only source of code it has,
 * and one setting bit 4 would be losing the only RAM. Either is traced as
 * a warning the moment it happens, so a program that then runs off the
 * rails leaves a clue. What is not decided here is what to do if a real
 * program does it: that is a question for the human, on the trace. Bit 2,
 * the I/O chip, is not honoured either and is not warned about: with it
 * off the ports read 0xFF (hardware_notes:231-233), which is what every
 * empty hook answers today, so the difference has nothing to show yet.
 *
 * The write is a compare on the hot path; the store of a changed value and
 * its trace are one cold call, made only when the value differs from the
 * last one written -- a program rewriting the same byte every frame costs
 * the compare alone.
 */
#define CART_IO_MEMCTL_RESET (0xABUL)

#define CART_IO_MEMCTL_WRITE(port, v)                   \
  do                                                    \
    {                                                   \
      if((uint32)(uint8)(v) != sms.cart.memctl)         \
        cart_io_memctl_write((uint8)(v));               \
    }                                                   \
  while(0)

/*
 * The two decoders. Two tests each, no table: the top two bits of the port
 * choose the quarter, the lowest bit the function.
 */
#define CART_IO_READ(port, dst)                                         \
  do                                                                    \
    {                                                                   \
      switch(((port) >> 6) & 3U)                                        \
        {                                                               \
        case 0:                                                         \
          CART_IO_LASTBYTE_READ((port),(dst));                          \
          break;                                                        \
        case 1:                                                         \
          if((port) & 1U)                                               \
            CART_IO_HCOUNTER_READ((port),(dst));                        \
          else                                                          \
            CART_IO_VCOUNTER_READ((port),(dst));                        \
          break;                                                        \
        case 2:                                                         \
          if((port) & 1U)                                               \
            CART_IO_VDP_STATUS_READ((port),(dst));                      \
          else                                                          \
            CART_IO_VDP_DATA_READ((port),(dst));                        \
          break;                                                        \
        default:                                                        \
          if((port) & 1U)                                               \
            CART_IO_PAD_B_READ((port),(dst));                           \
          else                                                          \
            CART_IO_PAD_A_READ((port),(dst));                           \
          break;                                                        \
        }                                                               \
    }                                                                   \
  while(0)

#define CART_IO_WRITE(port, v)                                          \
  do                                                                    \
    {                                                                   \
      switch(((port) >> 6) & 3U)                                        \
        {                                                               \
        case 0:                                                         \
          if((port) & 1U)                                               \
            CART_IO_CTRL_WRITE((port),(v));                             \
          else                                                          \
            CART_IO_MEMCTL_WRITE((port),(v));                           \
          break;                                                        \
        case 1:                                                         \
          CART_IO_PSG_WRITE((port),(v));                                \
          break;                                                        \
        case 2:                                                         \
          if((port) & 1U)                                               \
            CART_IO_VDP_CTRL_WRITE((port),(v));                         \
          else                                                          \
            CART_IO_VDP_DATA_WRITE((port),(v));                         \
          break;                                                        \
        default:                                                        \
          /* $C0-$FF: a write has no effect on the machine. */          \
          break;                                                        \
        }                                                               \
    }                                                                   \
  while(0)

/*
 * The cold half of a memory control write: stores the new value and traces
 * it, with a warning for each of the two bits this port does not honour.
 * Reached through CART_IO_MEMCTL_WRITE alone, on a change of value.
 */
void cart_io_memctl_write(uint8 value);

/*
 * Emits the aggregate of the accesses that reached an empty hook since the
 * last call -- one [BUS][WARN] line naming the counts and the last port --
 * and clears the counters. Emits nothing when both counts are zero.
 *
 * Cold: the frame loop calls it once per emulated second, never more often
 * (the serial output blocks, and a line per access would not slow a
 * frame down but destroy it). The line is a warning because every access
 * it counts is a program talking to a part that is not there yet.
 */
void cart_io_report(void);

#if LOG_ENABLE
/*
 * Name of a SYS_* value for the trace: "SMS", "GG", or "none". Published so
 * that every subsystem initialised after the load can print the profile it
 * picked up from sms.cart.system in its own init line, through one function
 * and one spelling. Under the LOG guard because it serves traces alone: a
 * silent build keeps neither the strings nor their callers.
 */
const char *cart_system_name(int32 system);
#endif

/*
 * The boot policy: tries roms/rom.sms, then roms/rom.gg, and loads the first
 * that exists. Any impossibility -- no file at all, a refused size, a failed
 * read -- stops the console on the error screen with a code of the
 * cartridge range, so on failure this function does not return. Returns 0
 * on success, after the I/O side of the bus has been reset -- the memory
 * control register at its boot value, traced, then one io map ok line --
 * and after one [SYS] profile= line naming the system the whole program will
 * now run as, emitted here because this is the moment the profile becomes
 * final.
 *
 * The paths are relative to the directory the program was started from,
 * which is the root of the disc (precedent: src_exemple/aaplayer/main.c:208).
 */
Err cart_boot(void);

#endif /* SMS3DO_CART_H */
