/*
 * translate: reads a cartridge image on the PC and writes the C of its
 * code, for the core to run in place of interpreting it.
 *
 *   translate <rom> <out.c> [--seeds <file>] [--counts <file> --budget <bytes>]
 *
 * What it knows: the Z80 instruction set -- the length, the registers
 * read and written and the semantics of every instruction of the four
 * families, unprefixed, CB, ED, DD/FD and their double prefix -- the
 * shape of a cartridge image and the Sega mapper's slots. What it does
 * not know: any title, any per-game figure, and any price in T-states.
 * Every image is treated alike.
 *
 * The output (src/rom_code.c) holds one function per block, a table from
 * the block's position in the cartridge to the function, the size and the
 * digest of the image, and the bytes the blocks cover -- and nothing
 * else: no byte of the image is copied out, no data, no name. A block is
 * an exact translation of the bytes at its position, written with the
 * macros the interpreter itself expands (src/z80_ops.h) or the very form
 * the interpreter's switch writes in line (src/z80.c), every immediate
 * and every target folded at emission, so that there is one semantics
 * and not two. Each emitted form names, in a comment beside it here, the
 * macro or the case it transcribes.
 *
 * Registers live in locals for the length of a block. The register names
 * of z80_ops.h are retargeted at the head of the generated file onto
 * thirteen locals -- A, F, B, C, D, E, H, L, the four index halves, SP --
 * the way z80_run retargets its five hot names (src/z80.c); a block
 * declares the ones it touches, loads at its entry those it reads before
 * writing them, stores at each of its exits those it has written by then,
 * and nothing else. PC stays on the structure. The compiler keeps the
 * accounts: a register used and not declared does not compile, one
 * declared and not used is a warning the build refuses.
 *
 * NO ACCOUNT OF TIME. A block spends no T-state, ticks no refresh
 * register and is cut by no quota: it runs its instructions and hands
 * back its successor, and what paces the program is WAITING
 * (src/z80c.h). The tool marks, in the table, the blocks that start a
 * wait: a short loop -- at most WAIT_INSNS instructions, closed by a
 * branch back to its first -- that reads a fixed byte of the work RAM
 * (an absolute address at or above 0xC000, or a byte through a pair the
 * loop does not write), the status port or the line counter of the
 * video part, and writes nothing to memory nor to a port. Arriving on
 * such a block ends the line of the picture. A delay loop that reads
 * nothing (djnz to itself, a pair counted down) is not a wait and runs
 * through in one line; a halt is a wait the interpreter answers, and
 * needs no mark. The reads of the emitted C are written Z80C_RD8 and
 * Z80C_RD16 (src/z80c.h), so that the text of the generated file names
 * nothing of the interpreter's clock nor of its refresh register:
 * `grep -E 'Z80_SPEND|Z80_R|TSTATES' src/rom_code.c` finds nothing.
 *
 * A block returns its successor. When the tool knows where a block
 * leaves PC -- the linear continuation, a relative or absolute jump, a
 * call, a restart, either exit of a conditional -- it returns the table
 * entry of the block that starts there, and the core chains to it with
 * no search. An absolute successor is rendered only when its position is
 * in the same 16k bank as the block, and only, at run time, while its
 * address is in the same 16k window of the address space as the block's
 * entry address: the same window is the same slot of the mapper, so the
 * target's bytes are the bank the tool read. A block in the fixed first
 * kilobyte of bank 0 never renders a successor past that kilobyte: the
 * rest of its window is whatever bank the mapper has turned in. A return,
 * a jump through a pair, a repeated block instruction and a target no
 * block starts at return null, and the core looks the address up.
 *
 * Positions and addresses. The core finds a block by the position of the
 * byte PC falls on, read off its live page table (src/z80c.c); nothing is
 * decided here about which bank the program will have turned in. What IS
 * decided here is where a walk goes when it meets a jump: a target is an
 * address, and an address is turned into a position by the flat plan of
 * the slots -- slot 0 is bank 0, slot 1 is bank 1, slot 2 is the bank of
 * the block doing the jump when that block is itself in slot 2, and bank
 * 2 otherwise, the value the mapper resets to. A target the plan guesses
 * wrong yields a block that nothing executes, never a wrong block: a block
 * only runs where the live table says its bytes are.
 *
 * Discovery walks the reachable code from the three vectors (reset, the
 * maskable interrupt, the non-maskable one) and from every target it
 * meets -- jumps, calls and their returns, restarts, the conditional
 * forms, the instruction after one the interpreter keeps -- marking the
 * instructions it walks over and the starts it learns, stopping at
 * unconditional transfers and at the edge of a 16k bank, which a block
 * never crosses -- nor the edge of the first kilobyte of bank 0, which
 * the Sega mapper keeps in place while it turns the rest of slot 0
 * (src/cart.c, cart_mapper_project): past that edge the bytes at the
 * address are another bank's. Emission then writes one block per start:
 * the instructions in order until the next start, an unconditional
 * transfer, a repeated block instruction, an instruction left to the
 * interpreter (the block leaves PC on it), the instruction table's size
 * (MAX_INSNS), or the bank's edge. A block cut at the table's size makes
 * its continuation a start; the walk is repeated until no start is added.
 *
 * What is left to the interpreter, and nothing else: HALT, which ends
 * the line; IM n, run once at boot; LD A,R and LD R,A, which read and
 * write the refresh register the core holds; and every byte the
 * interpreter itself refuses (the defaults of its ED and DD/FD
 * dispatches).
 *
 * A jump or a call into the work RAM (an address at or above 0xC000)
 * met by the walk is counted and the first is named in the report: such
 * a target is code without a position, which no block can hold. The
 * walk over-approximates -- it follows both arms of every conditional
 * and the code no run without a pad reaches -- so the tool does not
 * refuse on it; the runners refuse the program that EXECUTES code from
 * RAM, and the one that never waits, when they see it run
 * (tests/z80c/sidebyside.c).
 *
 * THE DIRECT PATH. Every read and write the interpreter makes goes
 * through the page tables (src/z80_ops.h): the table's address, its
 * entry, then the byte -- three loads where one would do. The tables
 * exist so that the mapper can move banks; the work RAM never moves. So
 * the tool PROVES, here, that an access lands in the work RAM, and
 * emits it without the tables: the block receives the RAM's base as its
 * argument, and the access is one load or one store into it, the
 * address masked to the 8k (the mirror at 0xE000 comes out of the
 * mask). Three families are proved:
 *
 *   the absolute address (nn) at or above 0xC000;
 *
 *   a pair or an index whose value the tool knows at that point of the
 *   block -- set by LD rr,nn, moved by INC rr and DEC rr, a half set
 *   by LD r,n, and lost on anything else that writes it; a block is
 *   entered from anywhere, so nothing is known at its head;
 *
 *   the stack, when every instruction the walk reached writes SP only
 *   by LD SP,nn with nn between 0xC000 and 0xFFFC: the stack then
 *   leaves the work RAM only by running out of it, which the runners
 *   refuse on the PC (src/z80c.h, the stack check); one LD SP,HL or
 *   INC SP anywhere sends the stack back to the full path, and the
 *   report says which and where.
 *
 * What is not proved keeps the full path. A write proved to land on
 * 0xFFFC-0xFFFF -- the mapper's registers -- keeps the full path, which
 * carries the mapper trigger, and CLOSES THE BLOCK: the instructions
 * after it may belong to the bank that has just left, and the core
 * asks the tables again (src/z80c.c, the epoch). The entry of such a
 * block carries a second flag, and a block that sees the mapper move
 * its own bytes without carrying it is refused on the PC -- the case
 * left is a write through a pointer the tool could not prove, turning
 * the block's own window, and it is named; a pointer write that turns
 * another window, the way a program sets the registers one by one
 * from the fixed kilobyte, is let through. A word
 * is direct only when both its bytes are proved, on the same side of
 * the mirror's seam (0xDFFF/0xE000) and below the top of the address
 * space; the stack, whose pointer is known only at run time, reads and
 * writes its two bytes masked one by one. An absolute read of the
 * fixed first kilobyte of the image is counted and not made direct:
 * the image is a pointer too, and the direct form would load as much
 * as the tables. The report says how much was proved:
 *
 *   z80c: direct rd=<proved>/<all> wr=<proved>/<all> stack=proven|full (<form> at <pos>) rom_fixed=<n>
 *
 * in instructions of the written table; `grep -c 'Z80C_RAM_\|Z80C_STK_'
 * src/rom_code.c` counts the same instructions.
 *
 * SEEDS. The walk reads no table: a program that dispatches through one
 * (a restart followed by an index, a jump through a pair) hides its
 * code from it, and what is hidden is interpreted and never marked as a
 * wait. So a run of the translated table on the PC records every
 * position the interpreter ran an instruction from, and a file of them
 * (--seeds, written by tests/z80c/sidebyside.c) adds those positions to
 * the starts before the walk. The same file may name a WAIT the shape
 * above does not see ("wait <pos>"): the head of the cycle a run was
 * found turning in without the line ending -- a loop that waits through
 * a call it dispatches -- and the block there is marked as a wait. The
 * scripts run the table and translate again until no run adds a
 * position (tests/z80c/translate.sh).
 *
 * With a file of counts -- the hits and the instructions per block a
 * recorded run of the side-by-side check wrote (tests/z80c/sidebyside.c)
 * -- and a budget in bytes of Z80 code, the table is chosen: the blocks
 * that start a wait are always in, blocks never run are left out, the
 * others are taken by the instructions they ran, most first, while the
 * bytes they cover fit the budget. Blocks left out are interpreted, and
 * the successors that pointed at them are null.
 *
 * Lengths are those of the core's own dispatch (src/z80.c, the switch of
 * z80_run and the three prefixed ones), which is the reference the
 * emitted code must agree with. The one trap is written there at length:
 * an index prefix in front of an instruction it has nothing to
 * substitute in consumes no displacement, so the instruction is two
 * bytes and not three (z80.c, Z80_DDFD_INERT).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The bounds a size must clear, the loader's (src/cart.c, cart_size_check):
   at least the smallest cartridge, at most the buffer, whole banks, a
   power of two. */
#define ROM_CAPACITY 1048576UL
#define ROM_MIN      32768UL
#define BANK_SIZE    16384UL

/* The edge of the fixed first kilobyte of bank 0, in slot 0: a walk or a
   block that starts below it stops there (src/cart.c, cart_mapper_project:
   page 0 of the address space is never repointed, pages 1 to 15 are). */
#define SLOT0_FIXED 1024UL

/* The most instructions a block holds: a block that reaches this many is
   cut there and its continuation is a start. A size, never a time: it
   bounds the function the compiler is handed, nothing else. */
#define MAX_INSNS 256

/* The most instructions a wait loop holds, its closing branch included:
   the short loops a program spins in while it waits are two to six
   instructions long (tests/z80c/probe_code.c counts them so). */
#define WAIT_INSNS 6

/* The address space the work RAM occupies: a target there is code the
   tool cannot translate and refuses. */
#define RAM_BASE 0xC000UL

/* The mapper's registers, at the top of the work RAM's address space
   (src/cart.h): a write proved to land there keeps the full path, which
   carries the mapper trigger, and closes the block. */
#define MAPPER_FIRST_REG 0xFFFCUL

/* The first byte of the mirror: a word whose two bytes stand on either
   side of it is two pages of the tables, never one direct read. */
#define MIRROR_BASE 0xE000UL

/* Where the stack may point when it is proved: LD SP,nn with nn in this
   range, and nothing else writing SP. */
#define STACK_LOW  0xC000UL
#define STACK_HIGH 0xFFFCUL


static unsigned char rom[ROM_CAPACITY];
static unsigned long rom_size;
static unsigned long bank_mask;

/* One byte per position: whether an instruction was walked from here,
   whether a block starts here, whether a block is emitted here in the
   table being written, whether a block could be emitted here at all, and
   whether an emitted block covers this byte -- counted once when two
   blocks overlap. */
#define M_INSN  1U
#define M_START 2U
#define M_BLOCK 4U
#define M_COVER 8U
#define M_ANY   16U
#define M_WAIT  32U /* a wait the seeds name: forced on the block there */
static unsigned char mark[ROM_CAPACITY];

/* The starts still to walk. A position enters once. */
static unsigned long queue[ROM_CAPACITY];
static unsigned long queue_n;
static unsigned long queue_at;
static int starts_added;

/* The figures of the report. */
static unsigned long n_starts;
static unsigned long n_insns;
static unsigned long n_reached;
static unsigned long n_blocks;
static unsigned long n_emitted;
static unsigned long n_code_bytes;
static unsigned long n_end_next, n_end_jump, n_end_fallback, n_end_cut, n_end_bank, n_end_repeat, n_end_bankend;
static unsigned long n_fb_halt, n_fb_im, n_fb_ld_a_r, n_fb_ld_r_a, n_fb_refused;
static unsigned long n_waits;
static unsigned long n_ram_targets, ram_target_addr, ram_target_from;
static unsigned long n_seeds;

/* The direct path: the instructions of the written table that read
   memory and that write it, and among each how many were proved; the
   absolute reads of the fixed kilobyte, counted only; and the stack --
   proved until the walk meets an instruction that writes SP otherwise
   than LD SP,nn into the work RAM, named with its position. */
static unsigned long n_rd_all, n_rd_direct, n_wr_all, n_wr_direct, n_rom_fixed;
static int stack_proven = 1;
static unsigned long stack_full_pos;
static char stack_full_form[24];

/* ---- the registers ---------------------------------------------------- */

/* One bit per register a block may hold in a local. The order is the
   order of the declarations in the generated block. */
#define R_A   (1U << 0)
#define R_F   (1U << 1)
#define R_B   (1U << 2)
#define R_C   (1U << 3)
#define R_D   (1U << 4)
#define R_E   (1U << 5)
#define R_H   (1U << 6)
#define R_L   (1U << 7)
#define R_IXH (1U << 8)
#define R_IXL (1U << 9)
#define R_IYH (1U << 10)
#define R_IYL (1U << 11)
#define R_SP  (1U << 12)
#define R_COUNT 13

#define R_BC (R_B | R_C)
#define R_DE (R_D | R_E)
#define R_HL (R_H | R_L)
#define R_IX (R_IXH | R_IXL)
#define R_IY (R_IYH | R_IYL)

static const char *reg_local[R_COUNT] =
{
  "z80c_a", "z80c_f", "z80c_b", "z80c_c", "z80c_d", "z80c_e", "z80c_h",
  "z80c_l", "z80c_ixh", "z80c_ixl", "z80c_iyh", "z80c_iyl", "z80c_sp"
};

static const char *reg_state[R_COUNT] =
{
  "Z80_A_STATE", "Z80_F_STATE", "Z80_B_STATE", "Z80_C_STATE",
  "Z80_D_STATE", "Z80_E_STATE", "Z80_H_STATE", "Z80_L_STATE",
  "Z80_IXH_STATE", "Z80_IXL_STATE", "Z80_IYH_STATE", "Z80_IYL_STATE",
  "Z80_SP_STATE"
};

static const char *reg_macro[R_COUNT] =
{
  "Z80_A", "Z80_F", "Z80_B", "Z80_C", "Z80_D", "Z80_E", "Z80_H", "Z80_L",
  "Z80_IXH", "Z80_IXL", "Z80_IYH", "Z80_IYL", "Z80_SP"
};

/* The eight bit register field of an opcode, in the order of the field
   (z80.c, the register-to-register cases): the sixth names the byte HL
   points at. */
static const char *r8[8] =
{
  "Z80_B", "Z80_C", "Z80_D", "Z80_E", "Z80_H", "Z80_L", NULL, "Z80_A"
};
static const unsigned r8bit[8] = { R_B, R_C, R_D, R_E, R_H, R_L, 0U, R_A };
static const char *r8name[8] = { "b", "c", "d", "e", "h", "l", "(hl)", "a" };

/* The pair field: BC, DE, HL, SP. */
static const char *rp[4] = { "Z80_BC", "Z80_DE", "Z80_HL", "Z80_SP" };
static const char *rpname[4] = { "bc", "de", "hl", "sp" };
static const unsigned rpbit[4] = { R_BC, R_DE, R_HL, R_SP };

/* The eight conditions, in the order of the field (z80.c, the
   conditional branches): each as the test the interpreter makes. */
static const char *cc[8] =
{
  "!Z80_FLAG_GET(Z80_FLAG_Z)", "Z80_FLAG_GET(Z80_FLAG_Z)",
  "!Z80_FLAG_GET(Z80_FLAG_C)", "Z80_FLAG_GET(Z80_FLAG_C)",
  "!Z80_FLAG_GET(Z80_FLAG_PV)", "Z80_FLAG_GET(Z80_FLAG_PV)",
  "!Z80_FLAG_GET(Z80_FLAG_S)", "Z80_FLAG_GET(Z80_FLAG_S)"
};
static const char *ccname[8] = { "nz", "z", "nc", "c", "po", "pe", "p", "m" };

/* The eight arithmetic and logical operations, in the order of the
   field: the operation as the interpreter writes it, and what it reads
   and writes beyond its operand. */
static const char *alu_fmt[8] =
{
  "Z80_OP_ADD(%s,0)", "Z80_OP_ADD(%s,Z80_FLAG_GET(Z80_FLAG_C))",
  "Z80_OP_SUB(%s,0)", "Z80_OP_SUB(%s,Z80_FLAG_GET(Z80_FLAG_C))",
  "Z80_OP_AND(%s)", "Z80_OP_XOR(%s)", "Z80_OP_OR(%s)", "Z80_OP_CP(%s)"
};
static const char *alu_name[8] = { "add", "adc", "sub", "sbc", "and", "xor", "or", "cp" };
static const unsigned alu_rd[8] = { R_A, R_A | R_F, R_A, R_A | R_F, R_A, R_A, R_A, R_A };
static const unsigned alu_wr[8] = { R_A | R_F, R_A | R_F, R_A | R_F, R_A | R_F,
                                    R_A | R_F, R_A | R_F, R_A | R_F, R_F };

/* The eight rotations and shifts of the CB prefix, in the order of the
   field (z80_ops.h, Z80_CB_APPLY): two of them read the carry in. */
static const char *cb_rot[8] = { "RLC", "RRC", "RL", "RR", "SLA", "SRA", "SLL", "SRL" };
static const char *cb_rotname[8] = { "rlc", "rrc", "rl", "rr", "sla", "sra", "sll", "srl" };
static const unsigned cb_rot_rd[8] = { 0U, 0U, R_F, R_F, 0U, 0U, 0U, 0U };

/* ---- the instruction set: lengths ------------------------------------ */

/* An unprefixed opcode's length (src/z80.c, the switch of z80_run: three
   bytes where Z80_FETCH16 or an absolute target is read, two where
   Z80_FETCH8 or a displacement is, one otherwise). */
static int unprefixed_len(unsigned op)
{
  switch(op)
    {
    case 0x01: case 0x11: case 0x21: case 0x31:
    case 0x22: case 0x2A: case 0x32: case 0x3A:
    case 0xC3: case 0xCD:
    case 0xC2: case 0xCA: case 0xD2: case 0xDA:
    case 0xE2: case 0xEA: case 0xF2: case 0xFA:
    case 0xC4: case 0xCC: case 0xD4: case 0xDC:
    case 0xE4: case 0xEC: case 0xF4: case 0xFC:
      return 3;
    case 0x06: case 0x0E: case 0x16: case 0x1E:
    case 0x26: case 0x2E: case 0x36: case 0x3E:
    case 0xC6: case 0xCE: case 0xD6: case 0xDE:
    case 0xE6: case 0xEE: case 0xF6: case 0xFE:
    case 0x10: case 0x18: case 0x20: case 0x28: case 0x30: case 0x38:
    case 0xD3: case 0xDB:
      return 2;
    default:
      return 1;
    }
}

/* Whether a field of an eight bit load names H, L or (HL): the index
   prefix has something to substitute there (z80.c, Z80_DDFD_FIELD_INDEXED). */
static int ddfd_field_indexed(unsigned f)
{
  return f >= 4U && f <= 6U;
}

/* An eight bit load the index prefix has nothing to bite on: two bytes,
   no displacement (z80.c, Z80_DDFD_INERT). */
static int ddfd_inert(unsigned op)
{
  return (op & 0xC0U) == 0x40U &&
         !ddfd_field_indexed((op >> 3) & 7U) &&
         !ddfd_field_indexed(op & 7U);
}

/* Whether the instruction behind an index prefix reaches the byte the
   pair points at, and so carries a displacement: the two memory
   increments and the memory store of an immediate, the loads through
   (HL) either way, the eight arithmetic forms on (HL). HALT reads as
   (HL) both ways and is left out: the indexed dispatch has no case for
   it and stops (z80.c). */
static int ddfd_displaced(unsigned op)
{
  if(op == 0x34U || op == 0x35U || op == 0x36U)
    return 1;
  if((op & 0xC0U) == 0x40U && op != 0x76U)
    return ((op >> 3) & 7U) == 6U || (op & 7U) == 6U;
  if((op & 0xC0U) == 0x80U)
    return (op & 7U) == 6U;
  return 0;
}

/* The length of the instruction at p, with at most avail bytes readable. */
static int insn_len(const unsigned char *p, unsigned long avail)
{
  unsigned op = p[0];
  unsigned sub;

  if(op == 0xCBU)
    return 2;

  if(op == 0xEDU)
    {
      if(avail < 2UL)
        return 2;
      sub = p[1];
      switch(sub)
        {
        case 0x43: case 0x53: case 0x63: case 0x73:
        case 0x4B: case 0x5B: case 0x6B: case 0x7B:
          return 4;
        default:
          return 2;
        }
    }

  if(op == 0xDDU || op == 0xFDU)
    {
      if(avail < 2UL)
        return 2;
      sub = p[1];
      if(sub == 0xCBU)
        return 4;
      if(ddfd_inert(sub))
        return 2;
      return 1 + unprefixed_len(sub) + (ddfd_displaced(sub) ? 1 : 0);
    }

  return unprefixed_len(op);
}

/* ---- the flat plan: positions and addresses -------------------------- */

/* The address a position is walked at: banks 0 and 1 in their own slots,
   every other bank in slot 2. */
static unsigned long addr_of(unsigned long pos)
{
  if(pos < 2UL * BANK_SIZE)
    return pos;
  return 0x8000UL + (pos & (BANK_SIZE - 1UL));
}

/* The position a target address is taken to name, from a block in the
   given bank; ROM_CAPACITY when the address is not in the cartridge (the
   work RAM and above). */
static unsigned long pos_of(unsigned long addr, unsigned long bank)
{
  unsigned long b;

  addr &= 0xFFFFUL;
  if(addr >= 0xC000UL)
    return ROM_CAPACITY;
  if(addr < 0x8000UL)
    return addr & (rom_size - 1UL);
  b = (bank >= 2UL) ? bank : 2UL;
  b &= bank_mask;
  return b * BANK_SIZE + (addr & (BANK_SIZE - 1UL));
}

static void add_start(unsigned long pos)
{
  if(pos >= rom_size)
    return;
  if(mark[pos] & M_START)
    return;
  mark[pos] |= M_START;
  queue[queue_n++] = pos;
  n_starts++;
  starts_added = 1;
}

/* A target of a jump or a call walked at position from: a start in the
   cartridge, or, in the work RAM, code without a position -- counted,
   the first one kept for the report, and walked no further. */
static void add_target(unsigned long addr, unsigned long bank, unsigned long from)
{
  addr &= 0xFFFFUL;
  if(addr >= RAM_BASE)
    {
      if(n_ram_targets == 0UL)
        {
          ram_target_addr = addr;
          ram_target_from = from;
        }
      n_ram_targets++;
      return;
    }
  add_start(pos_of(addr,bank));
}

static long disp8(unsigned char d)
{
  return (d < 0x80U) ? (long)d : ((long)d - 256L);
}

static unsigned long imm16(const unsigned char *p)
{
  return (unsigned long)p[0] | ((unsigned long)p[1] << 8);
}

/* ---- what is left to the interpreter --------------------------------- */

enum fb_reason { FB_NONE, FB_HALT, FB_IM, FB_LD_A_R, FB_LD_R_A, FB_REFUSED };

/* Whether the byte behind the ED prefix is one the interpreter executes
   (z80.c, the cases of its ED dispatch); everything else is its default,
   which stops the core. */
static int ed_known(unsigned sub)
{
  if((sub & 0xC7U) == 0x40U || (sub & 0xC7U) == 0x41U) return 1; /* in, out */
  if((sub & 0xC7U) == 0x42U || (sub & 0xC7U) == 0x43U) return 1; /* adc/sbc hl, ld (nn)/ld rr */
  if((sub & 0xC7U) == 0x44U) return 1;                           /* neg */
  if((sub & 0xC7U) == 0x45U) return 1;                           /* retn, reti */
  if(sub == 0x46U || sub == 0x66U || sub == 0x56U || sub == 0x76U ||
     sub == 0x5EU || sub == 0x7EU) return 1;                      /* im */
  switch(sub)
    {
    case 0x47: case 0x57: case 0x4F: case 0x5F: case 0x67: case 0x6F:
    case 0xA0: case 0xA8: case 0xB0: case 0xB8:
    case 0xA1: case 0xA9: case 0xB1: case 0xB9:
    case 0xA2: case 0xAA: case 0xB2: case 0xBA:
    case 0xA3: case 0xAB: case 0xB3: case 0xBB:
      return 1;
    default:
      return 0;
    }
}

/* Whether the byte behind an index prefix is one the indexed dispatch
   executes (z80.c, the cases of its DD/FD dispatch); the inert loads are
   executed as the unprefixed instruction they are. */
static int ddfd_known(unsigned sub)
{
  if(ddfd_inert(sub))
    return 1;
  switch(sub)
    {
    case 0x09: case 0x19: case 0x29: case 0x39:
    case 0x21: case 0x22: case 0x2A: case 0x23: case 0x2B:
    case 0x26: case 0x2E: case 0x24: case 0x2C: case 0x25: case 0x2D:
    case 0x34: case 0x35: case 0x36:
    case 0xE1: case 0xE3: case 0xE5: case 0xE9: case 0xF9: case 0xCB:
      return 1;
    default:
      break;
    }
  if((sub & 0xC0U) == 0x40U)
    return sub != 0x76U;
  if((sub & 0xC0U) == 0x80U)
    return ddfd_field_indexed(sub & 7U);
  return 0;
}

/* The reason the instruction at p is left to the interpreter, or
   FB_NONE. avail is what can be read. */
static enum fb_reason fallback_reason(const unsigned char *p, unsigned long avail)
{
  unsigned op = p[0];

  if(op == 0x76U)
    return FB_HALT;
  if(op == 0xEDU)
    {
      unsigned sub;

      if(avail < 2UL)
        return FB_REFUSED;
      sub = p[1];
      /* z80.c, ED 0x46/0x66, 0x56/0x76, 0x5E/0x7E select a mode; 0x4E
         and 0x6E have no case and are refused. */
      if(sub == 0x46U || sub == 0x66U || sub == 0x56U || sub == 0x76U ||
         sub == 0x5EU || sub == 0x7EU)
        return FB_IM;
      if(sub == 0x5FU)
        return FB_LD_A_R;
      if(sub == 0x4FU)
        return FB_LD_R_A;
      return ed_known(sub) ? FB_NONE : FB_REFUSED;
    }
  if(op == 0xDDU || op == 0xFDU)
    {
      if(avail < 2UL)
        return FB_REFUSED;
      return ddfd_known(p[1]) ? FB_NONE : FB_REFUSED;
    }
  return FB_NONE;
}

static void count_fallback(enum fb_reason fb)
{
  switch(fb)
    {
    case FB_HALT:    n_fb_halt++;    break;
    case FB_IM:      n_fb_im++;      break;
    case FB_LD_A_R:  n_fb_ld_a_r++;  break;
    case FB_LD_R_A:  n_fb_ld_r_a++;  break;
    case FB_REFUSED: n_fb_refused++; break;
    default: break;
    }
}

/* ---- discovery ------------------------------------------------------- */

/* Whether the two byte instruction behind the block prefix is a return
   from interrupt: RETN, RETI and their six undocumented mirrors. */
static int ed_return(unsigned sub)
{
  return (sub & 0xC7U) == 0x45U;
}

/* The stack's proof, held over every instruction the walk reaches: SP
   written by LD SP,nn into the work RAM keeps it; any other write of SP
   -- LD SP,HL, LD SP,IX, INC SP, DEC SP, LD SP,(nn), or an LD SP,nn
   outside the RAM -- loses it, and the first such instruction is kept
   for the report. */
static void stack_seen(const unsigned char *p, unsigned long pos, unsigned long avail)
{
  const char *form = NULL;
  unsigned op = p[0];

  if(op == 0x31U)
    {
      /* An operand cut by the edge of the bank is an operand unknown:
         the proof falls, it does not stand by default. */
      if(avail < 3UL)
        form = "ld sp,nn (cut)";
      else
        {
          unsigned long nn = imm16(p + 1);

          if(nn < STACK_LOW || nn > STACK_HIGH)
            form = "ld sp,nn";
        }
    }
  else if(op == 0xF9U)
    form = "ld sp,hl";
  else if(op == 0x33U)
    form = "inc sp";
  else if(op == 0x3BU)
    form = "dec sp";
  else if(op == 0xEDU && avail >= 2UL && p[1] == 0x7BU)
    form = "ld sp,(nn)";
  else if((op == 0xDDU || op == 0xFDU) && avail >= 2UL && p[1] == 0xF9U)
    form = (op == 0xDDU) ? "ld sp,ix" : "ld sp,iy";
  else if((op == 0xDDU || op == 0xFDU) && avail >= 2UL &&
          (p[1] == 0x31U || p[1] == 0x33U || p[1] == 0x3BU ||
           (p[1] == 0xEDU && avail >= 3UL && p[2] == 0x7BU)))
    /* A stray prefix in front of a write of SP: the interpreter runs
       the instruction under it all the same, and the tool does not
       translate it. */
    form = "prefixed sp write";

  if(form != NULL && stack_proven)
    {
      stack_proven = 0;
      stack_full_pos = pos;
      strncpy(stack_full_form,form,sizeof stack_full_form - 1);
      stack_full_form[sizeof stack_full_form - 1] = '\0';
    }
}

/* Walks the reachable code from one start to the first unconditional
   transfer, the edge of the bank, or an instruction already walked. */
static void discover(unsigned long start)
{
  unsigned long bank = start / BANK_SIZE;
  unsigned long end  = (bank + 1UL) * BANK_SIZE;
  unsigned long pos  = start;

  if(start < SLOT0_FIXED)
    end = SLOT0_FIXED;

  while(pos < end)
    {
      const unsigned char *p = rom + pos;
      unsigned op = p[0];
      int len;
      enum fb_reason fb;

      if(mark[pos] & M_INSN)
        return;
      len = insn_len(p,end - pos);
      /* Seen by the stack's proof before the cut is tested: an
         instruction cut by the edge still runs in the interpreter. */
      stack_seen(p,pos,end - pos);
      if(pos + (unsigned long)len > end)
        return;

      mark[pos] |= M_INSN;
      n_insns++;
      n_reached += (unsigned long)len;

      /* The instruction after one the interpreter keeps is a start: the
         block before it closes on it, the block after it opens there. */
      fb = fallback_reason(p,end - pos);
      if(fb != FB_NONE)
        {
          count_fallback(fb);
          add_start(pos + (unsigned long)len);
        }

      switch(op)
        {
        case 0x18: /* jr */
          add_target(addr_of(pos) + 2UL + (unsigned long)disp8(p[1]),bank,pos);
          return;
        case 0x10: case 0x20: case 0x28: case 0x30: case 0x38: /* djnz, jr cc */
          add_target(addr_of(pos) + 2UL + (unsigned long)disp8(p[1]),bank,pos);
          break;
        case 0xC3: /* jp nn */
          add_target(imm16(p + 1),bank,pos);
          return;
        case 0xC2: case 0xCA: case 0xD2: case 0xDA:
        case 0xE2: case 0xEA: case 0xF2: case 0xFA: /* jp cc,nn */
          add_target(imm16(p + 1),bank,pos);
          break;
        case 0xCD: /* call nn: the callee, and the return */
          add_target(imm16(p + 1),bank,pos);
          add_start(pos + 3UL);
          return;
        case 0xC4: case 0xCC: case 0xD4: case 0xDC:
        case 0xE4: case 0xEC: case 0xF4: case 0xFC: /* call cc,nn */
          add_target(imm16(p + 1),bank,pos);
          break;
        case 0xC7: case 0xCF: case 0xD7: case 0xDF:
        case 0xE7: case 0xEF: case 0xF7: case 0xFF: /* rst */
          add_start(op & 0x38U);
          add_start(pos + 1UL);
          return;
        case 0xC9: /* ret */
        case 0xE9: /* jp (hl) */
          return;
        case 0xED:
          if(ed_return(p[1]))
            return;
          break;
        case 0xDD: case 0xFD:
          if(p[1] == 0xE9U) /* jp (ix), jp (iy) */
            return;
          break;
        default:
          break;
        }

      pos += (unsigned long)len;
    }
}

/* ---- one instruction, decoded for emission --------------------------- */

/* How an instruction ends, or does not end, a block. */
enum kind
{
  K_PLAIN,      /* runs and falls through */
  K_JUMP,       /* unconditional transfer, target known: the block ends */
  K_JUMP_LOST,  /* unconditional transfer, target unknown: the block ends */
  K_COND,       /* a taken exit, then falls through */
  K_REPEAT,     /* a repeated block instruction: the block ends */
  K_FALLBACK    /* left to the interpreter: not emitted */
};

typedef struct
{
  int len;
  unsigned rd, wr;        /* the registers read and written */
  enum kind kind;
  enum fb_reason fb;
  int uses_pc0;           /* whether the text names z80_pc0 */
  char text[512];         /* the instruction, or the part before the test */
  char cond[64];          /* K_COND: the test */
  char taken[256];        /* K_COND: what the taken exit runs before leaving */
  int target_known;       /* K_JUMP, K_COND: where PC goes when taken */
  int target_rel;         /* the target is relative to the entry address */
  unsigned long target_pos;
  unsigned long target_addr;
  /* What a wait is made of (classify): whether the instruction reads a
     fixed byte -- an absolute address in the work RAM, the status port
     or the line counter -- or a byte through a pair, named by wait_pair,
     which the loop must then not write; and whether it writes memory or
     a port. */
  int wait_read;
  unsigned wait_pair;
  int writes;
  /* The direct path: whether the instruction reads memory (1) or
     writes it (2, an exchange with the stack included); whether the
     form emitted is the direct one; whether it is an absolute read of
     the fixed kilobyte; whether it is a proved write to the mapper's
     registers, after which the block closes; and the bytes each path
     moves, in the text and in the taken exit. */
  int mem;
  int direct;
  int rom_fixed;
  int bankend;
  int acc_direct, acc_full;
  int taken_direct, taken_full;
} insn_t;

static void set_text(insn_t *in, const char *s)
{
  strncpy(in->text,s,sizeof in->text - 1);
  in->text[sizeof in->text - 1] = '\0';
}

/* ---- the proof of the work RAM --------------------------------------- */

/* What the tool knows of the registers at one point of a block: the
   value of each half when it is known. Indexed like the register bits;
   only the ten halves of the five pairs are ever known. */
typedef struct
{
  int known[R_COUNT];
  unsigned value[R_COUNT];
} regs_t;

/* The state the instruction being decoded is emitted under: set by
   decode, read by the forms below. Nothing is known at a block's head. */
static const regs_t *proof;

static void regs_reset(regs_t *r)
{
  memset(r,0,sizeof *r);
}

/* The value of a pair, when both halves are known. The bits name the
   halves (R_H | R_L, R_IXH | R_IXL...). */
static int regs_pair(const regs_t *r, unsigned bits, unsigned long *value)
{
  unsigned i, hi = 0U, lo = 0U;
  int n = 0;

  for(i = 0; i < R_COUNT; i++)
    if(bits & (1U << i))
      {
        if(!r->known[i])
          return 0;
        if(n == 0)
          hi = r->value[i];
        else
          lo = r->value[i];
        n++;
      }
  if(n != 2)
    return 0;
  *value = ((unsigned long)hi << 8) | (unsigned long)lo;
  return 1;
}

static void regs_set_half(regs_t *r, unsigned bit, unsigned v)
{
  unsigned i;

  for(i = 0; i < R_COUNT; i++)
    if(bit == (1U << i))
      {
        r->known[i] = 1;
        r->value[i] = v & 0xFFU;
      }
}

static void regs_set_pair(regs_t *r, unsigned bits, unsigned long v)
{
  unsigned i;
  int n = 0;

  for(i = 0; i < R_COUNT; i++)
    if(bits & (1U << i))
      {
        r->known[i] = 1;
        r->value[i] = (n == 0) ? (unsigned)((v >> 8) & 0xFFUL) : (unsigned)(v & 0xFFUL);
        n++;
      }
}

/* What is known after the instruction at p, decoded as in: every
   register it writes is lost, then the three forms that set a value
   -- LD rr,nn, LD r,n on a half, INC rr and DEC rr on a pair whose
   value was known -- put one back. SP is never held here: the stack is
   proved over the whole program. */
static void regs_after(regs_t *r, const insn_t *in, const unsigned char *p)
{
  unsigned op = p[0];
  unsigned i;

  for(i = 0; i < R_COUNT; i++)
    if(in->wr & (1U << i))
      r->known[i] = 0;
  if(in->kind == K_FALLBACK)
    return;

  if(op == 0xDDU || op == 0xFDU)
    {
      unsigned bits = (op == 0xDDU) ? R_IX : R_IY;
      unsigned bhi  = (op == 0xDDU) ? R_IXH : R_IYH;
      unsigned blo  = (op == 0xDDU) ? R_IXL : R_IYL;

      switch(p[1])
        {
        case 0x21: regs_set_pair(r,bits,imm16(p + 2)); break;
        case 0x26: regs_set_half(r,bhi,p[2]); break;
        case 0x2E: regs_set_half(r,blo,p[2]); break;
        case 0x23: case 0x2B:
          /* The halves were lost above; the pair is known again only
             when it was known before, which the caller's copy says. */
          break;
        default: break;
        }
      return;
    }
  if(op == 0xCBU || op == 0xEDU)
    return;

  if((op & 0xCFU) == 0x01U && ((op >> 4) & 3U) != 3U)
    {
      regs_set_pair(r,rpbit[(op >> 4) & 3U],imm16(p + 1));
      return;
    }
  if((op & 0xC7U) == 0x06U && ((op >> 3) & 7U) != 6U)
    regs_set_half(r,r8bit[(op >> 3) & 7U],p[1]);
}

/* The pair moved by INC rr or DEC rr, plain or indexed, when its value
   was known before: known after, one further. Applied by the caller
   on the state before the instruction, since regs_after has lost it. */
static void regs_step(regs_t *r, const regs_t *before, const unsigned char *p)
{
  unsigned op = p[0];
  unsigned bits;
  int dec;
  unsigned long v;

  if(op == 0xDDU || op == 0xFDU)
    {
      if(p[1] != 0x23U && p[1] != 0x2BU)
        return;
      bits = (op == 0xDDU) ? R_IX : R_IY;
      dec = (p[1] == 0x2BU);
    }
  else if((op & 0xC7U) == 0x03U && ((op >> 4) & 3U) != 3U)
    {
      bits = rpbit[(op >> 4) & 3U];
      dec = (op & 8U) != 0U;
    }
  else
    return;
  if(!regs_pair(before,bits,&v))
    return;
  v = (dec ? (v - 1UL) : (v + 1UL)) & 0xFFFFUL;
  regs_set_pair(r,bits,v);
}

/* Whether an address is the work RAM's, and whether a byte written
   there is one of the mapper's registers. */
static int in_ram(unsigned long addr)
{
  return (addr & 0xFFFFUL) >= RAM_BASE;
}

static int in_mapper(unsigned long addr)
{
  return (addr & 0xFFFFUL) >= MAPPER_FIRST_REG;
}

/* The two bytes of a word at addr: both in the work RAM, on the same
   side of the mirror's seam, below the top of the address space. */
static int word_in_ram(unsigned long addr)
{
  addr &= 0xFFFFUL;
  if(addr + 1UL > 0xFFFFUL)
    return 0;
  if(!in_ram(addr))
    return 0;
  return (addr < MIRROR_BASE) == (addr + 1UL < MIRROR_BASE);
}

/* The forms of one access at a known address, and the counts that go
   with them. An eight bit read is direct in the work RAM; a write is
   direct below the mapper's registers, and on them it keeps the full
   path and ends the block. A word is direct when both its bytes are.
   The absolute read of the fixed kilobyte is counted. */
static const char *form_rd8(insn_t *in, unsigned long addr, int absolute)
{
  in->mem = 1;
  if(absolute && (addr & 0xFFFFUL) < SLOT0_FIXED)
    in->rom_fixed = 1;
  if(in_ram(addr))
    {
      in->direct = 1;
      in->acc_direct += 1;
      return "Z80C_RAM_RD8";
    }
  in->acc_full += 1;
  return "Z80C_RD8";
}

static const char *form_wr8(insn_t *in, unsigned long addr)
{
  in->mem = 2;
  if(in_ram(addr) && !in_mapper(addr))
    {
      in->direct = 1;
      in->acc_direct += 1;
      return "Z80C_RAM_WR8";
    }
  if(in_mapper(addr))
    in->bankend = 1;
  in->acc_full += 1;
  return "Z80_WR8";
}

static const char *form_rd16(insn_t *in, unsigned long addr)
{
  in->mem = 1;
  if(word_in_ram(addr))
    {
      in->direct = 1;
      in->acc_direct += 2;
      return "Z80C_RAM_RD16";
    }
  in->acc_full += 2;
  return "Z80C_RD16";
}

static const char *form_wr16(insn_t *in, unsigned long addr)
{
  in->mem = 2;
  if(word_in_ram(addr) && !in_mapper(addr + 1UL))
    {
      in->direct = 1;
      in->acc_direct += 2;
      return "Z80C_RAM_WR16";
    }
  /* A word at 0xFFFF puts its second byte at 0x0000, absorbed; its
     first is a register all the same. */
  if(in_mapper(addr) || in_mapper(addr + 1UL))
    in->bankend = 1;
  in->acc_full += 2;
  return "Z80_WR16";
}

/* The same forms for an access through a pair, or a displaced index:
   known, the address decides as above; unknown, the full path. */
static const char *pair_rd8(insn_t *in, unsigned bits, long d)
{
  unsigned long v;

  if(proof != NULL && regs_pair(proof,bits,&v))
    return form_rd8(in,(unsigned long)((long)v + d) & 0xFFFFUL,0);
  in->mem = 1;
  in->acc_full += 1;
  return "Z80C_RD8";
}

static const char *pair_wr8(insn_t *in, unsigned bits, long d)
{
  unsigned long v;

  if(proof != NULL && regs_pair(proof,bits,&v))
    return form_wr8(in,(unsigned long)((long)v + d) & 0xFFFFUL);
  in->mem = 2;
  in->acc_full += 1;
  return "Z80_WR8";
}

/* The stack's forms: the direct ones when the stack is proved, the
   interpreter's otherwise. The bytes moved: two for a push, a pop, a
   call, a return, a restart; four for the exchange. */
static const char *stk_form(insn_t *in, const char *direct, const char *full,
                            int mem, int bytes, int taken)
{
  in->mem = mem;
  if(stack_proven)
    {
      in->direct = 1;
      if(taken) in->taken_direct += bytes; else in->acc_direct += bytes;
      return direct;
    }
  if(taken) in->taken_full += bytes; else in->acc_full += bytes;
  return full;
}

#define STK_PUSH(in, taken) stk_form((in),"Z80C_STK_PUSH","Z80_OP_PUSH",2,2,(taken))
#define STK_POP(in)         stk_form((in),"Z80C_STK_POP","Z80_OP_POP",1,2,0)
#define STK_RET(in, taken)  stk_form((in),"Z80C_STK_RET","Z80_OP_RET",1,2,(taken))
#define STK_RETN(in)        stk_form((in),"Z80C_STK_RETN","Z80_OP_RETN",1,2,0)
#define STK_RETI(in)        stk_form((in),"Z80C_STK_RETI","Z80_OP_RETI",1,2,0)
#define STK_EXSP(in)        stk_form((in),"Z80C_STK_EXSP","Z80_OP_EX_SP_PAIR",2,4,0)

/* A read-modify-write through a pair or a displaced index (the CB
   forms on (hl) and (ix+d)): direct when the byte is known to be in
   the work RAM below the mapper's registers, the full path for both
   accesses otherwise -- on a register, the write closes the block. */
static void pair_rmw(insn_t *in, unsigned bits, long d, const char **rd, const char **wr)
{
  unsigned long v;

  in->mem = 2;
  if(proof != NULL && regs_pair(proof,bits,&v))
    {
      unsigned long addr = (unsigned long)((long)v + d) & 0xFFFFUL;

      if(in_ram(addr) && !in_mapper(addr))
        {
          in->direct = 1;
          in->acc_direct += 2;
          *rd = "Z80C_RAM_RD8";
          *wr = "Z80C_RAM_WR8";
          return;
        }
      if(in_mapper(addr))
        in->bankend = 1;
    }
  in->acc_full += 2;
  *rd = "Z80C_RD8";
  *wr = "Z80_WR8";
}

/* A read-modify-write the interpreter's macro makes on its own (INC
   (HL), RRD, the block moves): the full path, counted -- and, when the
   pair it writes through is known to point at a register of the
   mapper, the block closes on it like on any proved mapper write. */
static void full_rmw(insn_t *in, unsigned bits, long d, int bytes)
{
  unsigned long v;

  in->mem = 2;
  in->acc_full += bytes;
  if(proof != NULL && regs_pair(proof,bits,&v) &&
     in_mapper((unsigned long)((long)v + d) & 0xFFFFUL))
    in->bankend = 1;
}

/* The address the byte the index pair points at, displaced, as the
   interpreter composes it (z80_ops.h, Z80_FETCH_IXY_ADDR) with the
   displacement folded. */
static void ixaddr_text(char *buf, size_t cap, const char *pair, long d)
{
  snprintf(buf,cap,"uint16 ixaddr = (uint16)((int32)(uint32)%s + (%ld))",pair,d);
}

/* The relative target of a branch at offset off, of length 2, in the
   block at start: as a position, and as the fold from the entry
   address. Relative targets need no window test: a target position in
   the block's bank is at the same offset from the block's address
   whatever slot the bank sits in. */
static void set_rel_target(insn_t *in, unsigned long start, unsigned long off, long d)
{
  long t = (long)(start + off) + 2L + d;

  in->target_known = 1;
  in->target_rel = 1;
  in->target_pos = (t < 0L) ? ROM_CAPACITY : (unsigned long)t;
  in->target_addr = (unsigned long)((long)off + 2L + d) & 0xFFFFUL;
  in->uses_pc0 = 1;
}

static void set_abs_target(insn_t *in, unsigned long start, unsigned long addr)
{
  in->target_known = 1;
  in->target_rel = 0;
  in->target_addr = addr & 0xFFFFUL;
  in->target_pos = pos_of(addr,start / BANK_SIZE);
}

/* Decodes the CB prefixed instruction, plain or behind an index prefix.
   For the plain form, opnd names the register or is NULL for (HL); for
   the indexed one, ixpair names the pair and d is the displacement, the
   operand being the displaced byte and the result going to the register
   the low field names, or to that byte when it names it (z80.c, the
   double prefix case). */
static void decode_cb(insn_t *in, unsigned cbop, const char *ixpair, unsigned ixbits, long d)
{
  unsigned r = cbop & 7U;
  unsigned fam = cbop & 0xC0U;
  unsigned op = (cbop >> 3) & 7U;
  unsigned mask = 1U << op;
  char buf[512];
  char ax[96];

  if(ixpair == NULL)
    {
      /* z80.c, the CB case: the operand read by the low field, the
         operation applied (z80_ops.h, Z80_CB_APPLY), the result stored
         back by the same field. */
      if(r != 6U)
        {
          if(fam == 0x00U)
            {
              snprintf(buf,sizeof buf,
                       "  { uint8 cbval = %s, cbres; Z80_CB_%s(cbval,cbres); %s = cbres; } /* %s %s */",
                       r8[r],cb_rot[op],r8[r],cb_rotname[op],r8name[r]);
              in->rd = r8bit[r] | cb_rot_rd[op];
              in->wr = r8bit[r] | R_F;
            }
          else if(fam == 0x40U)
            {
              snprintf(buf,sizeof buf,"  Z80_CB_BIT(%s,0x%02XU,%s); /* bit %u,%s */",
                       r8[r],mask,r8[r],op,r8name[r]);
              in->rd = r8bit[r] | R_F;
              in->wr = R_F;
            }
          else
            {
              snprintf(buf,sizeof buf,
                       "  { uint8 cbval = %s, cbres; Z80_CB_%s(cbval,cbres,0x%02XU); %s = cbres; } /* %s %u,%s */",
                       r8[r],(fam == 0x80U) ? "RES" : "SET",mask,r8[r],
                       (fam == 0x80U) ? "res" : "set",op,r8name[r]);
              in->rd = r8bit[r];
              in->wr = r8bit[r];
            }
        }
      else
        {
          const char *rdf, *wrf;

          if(fam == 0x00U)
            {
              pair_rmw(in,R_HL,0L,&rdf,&wrf);
              snprintf(buf,sizeof buf,
                       "  { uint16 cbaddr = Z80_HL; uint8 cbval = %s(cbaddr), cbres; Z80_CB_%s(cbval,cbres); %s(cbaddr,cbres); } /* %s (hl) */",
                       rdf,cb_rot[op],wrf,cb_rotname[op]);
              in->rd = R_HL | cb_rot_rd[op];
              in->wr = R_F;
            }
          else if(fam == 0x40U)
            {
              rdf = pair_rd8(in,R_HL,0L);
              snprintf(buf,sizeof buf,
                       "  { uint16 cbaddr = Z80_HL; uint8 cbval = %s(cbaddr); Z80_CB_BIT(cbval,0x%02XU,(uint8)(cbaddr >> 8)); } /* bit %u,(hl) */",
                       rdf,mask,op);
              in->rd = R_HL | R_F;
              in->wr = R_F;
            }
          else
            {
              pair_rmw(in,R_HL,0L,&rdf,&wrf);
              snprintf(buf,sizeof buf,
                       "  { uint16 cbaddr = Z80_HL; uint8 cbval = %s(cbaddr), cbres; Z80_CB_%s(cbval,cbres,0x%02XU); %s(cbaddr,cbres); } /* %s %u,(hl) */",
                       rdf,(fam == 0x80U) ? "RES" : "SET",mask,wrf,
                       (fam == 0x80U) ? "res" : "set",op);
              in->rd = R_HL;
              in->wr = 0U;
            }
        }
      set_text(in,buf);
      return;
    }

  /* z80.c, the double prefix: the displaced byte is the operand, the
     result goes to the register the low field names or to the byte
     itself, and a bit test takes its two undocumented flags off the high
     byte of the address. */
  ixaddr_text(ax,sizeof ax,ixpair,d);
  if(fam == 0x40U)
    {
      const char *rdf = pair_rd8(in,ixbits,d);

      snprintf(buf,sizeof buf,
               "  { %s; uint8 cbval = %s(ixaddr); Z80_CB_BIT(cbval,0x%02XU,(uint8)(ixaddr >> 8)); } /* bit %u,(%s+d) */",
               ax,rdf,mask,op,(ixbits == R_IX) ? "ix" : "iy");
      in->rd = ixbits | R_F;
      in->wr = R_F;
    }
  else
    {
      char store[64];
      const char *rdf, *wrf;

      /* The result goes back to the byte, a read-modify-write, or to a
         register, a read alone. */
      if(r == 6U)
        {
          pair_rmw(in,ixbits,d,&rdf,&wrf);
          snprintf(store,sizeof store,"%s(ixaddr,cbres)",wrf);
          in->wr = 0U;
        }
      else
        {
          rdf = pair_rd8(in,ixbits,d);
          snprintf(store,sizeof store,"%s = cbres",r8[r]);
          in->wr = r8bit[r];
        }
      if(fam == 0x00U)
        {
          snprintf(buf,sizeof buf,
                   "  { %s; uint8 cbval = %s(ixaddr), cbres; Z80_CB_%s(cbval,cbres); %s; } /* %s (%s+d)%s%s */",
                   ax,rdf,cb_rot[op],store,cb_rotname[op],(ixbits == R_IX) ? "ix" : "iy",
                   (r == 6U) ? "" : ",",(r == 6U) ? "" : r8name[r]);
          in->rd = ixbits | cb_rot_rd[op];
          in->wr |= R_F;
        }
      else
        {
          snprintf(buf,sizeof buf,
                   "  { %s; uint8 cbval = %s(ixaddr), cbres; Z80_CB_%s(cbval,cbres,0x%02XU); %s; } /* %s %u,(%s+d)%s%s */",
                   ax,rdf,(fam == 0x80U) ? "RES" : "SET",mask,store,
                   (fam == 0x80U) ? "res" : "set",op,(ixbits == R_IX) ? "ix" : "iy",
                   (r == 6U) ? "" : ",",(r == 6U) ? "" : r8name[r]);
          in->rd = ixbits;
        }
    }
  set_text(in,buf);
}

/* Decodes the ED prefixed instruction (z80.c, the ED case). */
static void decode_ed(insn_t *in, const unsigned char *p, unsigned long start, unsigned long off)
{
  unsigned sub = p[1];
  unsigned r = (sub >> 3) & 7U;
  unsigned q = (sub >> 4) & 3U;
  char buf[512];

  (void)start;

  if((sub & 0xC7U) == 0x40U)
    {
      /* z80.c, ED 0x40..0x78: Z80_OP_IN_C, Z80_OP_IN_C_DROP. */
      if(r == 6U)
        {
          set_text(in,"  Z80_OP_IN_C_DROP(); /* in (c) */");
          in->rd = R_C | R_F;
          in->wr = R_F;
        }
      else
        {
          snprintf(buf,sizeof buf,"  Z80_OP_IN_C(%s); /* in %s,(c) */",r8[r],r8name[r]);
          set_text(in,buf);
          in->rd = R_C | R_F;
          in->wr = r8bit[r] | R_F;
        }
      return;
    }
  if((sub & 0xC7U) == 0x41U)
    {
      /* z80.c, ED 0x41..0x79: Z80_OP_OUT_C. */
      if(r == 6U)
        {
          set_text(in,"  Z80_OP_OUT_C(0); /* out (c),0 */");
          in->rd = R_C;
        }
      else
        {
          snprintf(buf,sizeof buf,"  Z80_OP_OUT_C(%s); /* out (c),%s */",r8[r],r8name[r]);
          set_text(in,buf);
          in->rd = R_C | r8bit[r];
        }
      return;
    }
  if((sub & 0xC7U) == 0x42U)
    {
      /* z80.c, ED 0x4A/0x42 and the three others: Z80_OP_ADC_HL, Z80_OP_SBC_HL. */
      snprintf(buf,sizeof buf,"  Z80_OP_%s_HL(%s); /* %s hl,%s */",
               (sub & 8U) ? "ADC" : "SBC",rp[q],(sub & 8U) ? "adc" : "sbc",rpname[q]);
      set_text(in,buf);
      in->rd = R_HL | R_F | rpbit[q];
      in->wr = R_HL | R_F;
      return;
    }
  if((sub & 0xC7U) == 0x43U)
    {
      /* z80.c, ED 0x43/0x4B and the others: Z80_FETCH16 then Z80_WR16 of
         the pair, or the pair set from Z80_RD16; SP in plain. */
      unsigned long nn = imm16(p + 2);

      if(sub & 8U)
        {
          const char *rdf = form_rd16(in,nn);

          if(q == 3U)
            snprintf(buf,sizeof buf,"  Z80_SP = %s(0x%04lXU); /* ld sp,(nn) */",rdf,nn);
          else
            snprintf(buf,sizeof buf,"  Z80_SET_%s(%s(0x%04lXU)); /* ld %s,(nn) */",
                     rp[q] + 4,rdf,nn,rpname[q]);
          in->wr = rpbit[q];
        }
      else
        {
          snprintf(buf,sizeof buf,"  %s(0x%04lXU,%s); /* ld (nn),%s */",form_wr16(in,nn),nn,rp[q],rpname[q]);
          in->rd = rpbit[q];
        }
      set_text(in,buf);
      return;
    }
  if((sub & 0xC7U) == 0x44U)
    {
      /* z80.c, ED 0x44 and its seven mirrors: Z80_OP_NEG. */
      set_text(in,"  Z80_OP_NEG(); /* neg */");
      in->rd = R_A;
      in->wr = R_A | R_F;
      return;
    }
  if((sub & 0xC7U) == 0x45U)
    {
      /* z80.c, ED 0x45 and its mirrors: Z80_OP_RETN; 0x4D: Z80_OP_RETI;
         the stack's direct forms when the stack is proved. */
      if(sub == 0x4DU)
        snprintf(buf,sizeof buf,"  %s(); /* reti */",STK_RETI(in));
      else
        snprintf(buf,sizeof buf,"  %s(); /* retn */",STK_RETN(in));
      set_text(in,buf);
      in->rd = R_SP;
      in->wr = R_SP;
      in->kind = K_JUMP_LOST;
      return;
    }

  switch(sub)
    {
    case 0x47: /* z80.c, ED 0x47: Z80_I = Z80_A */
      set_text(in,"  Z80_I = Z80_A; /* ld i,a */");
      in->rd = R_A;
      return;
    case 0x57: /* z80.c, ED 0x57: Z80_OP_LD_A_I */
      set_text(in,"  Z80_OP_LD_A_I(); /* ld a,i */");
      in->rd = R_F;
      in->wr = R_A | R_F;
      return;
    case 0x67: /* z80.c, ED 0x67: Z80_OP_RRD */
      set_text(in,"  Z80_OP_RRD(); /* rrd */");
      in->rd = R_HL | R_A | R_F;
      in->wr = R_A | R_F;
      full_rmw(in,R_HL,0L,2);
      return;
    case 0x6F: /* z80.c, ED 0x6F: Z80_OP_RLD */
      set_text(in,"  Z80_OP_RLD(); /* rld */");
      in->rd = R_HL | R_A | R_F;
      in->wr = R_A | R_F;
      full_rmw(in,R_HL,0L,2);
      return;

    /* z80.c, ED 0xA0..0xBB: the eight block instructions and their
       repeats (z80_ops.h, Z80_BLOCK_LD, Z80_BLOCK_CP, Z80_BLOCK_IN,
       Z80_BLOCK_OUT, Z80_BLOCK_REPEAT). A repeat backs PC up two bytes
       when it goes on, so PC is set past the instruction first and the
       block closes: the next block is found by the core. */
    case 0xA0: case 0xA8: case 0xB0: case 0xB8:
      in->rd = R_A | R_F | R_BC | R_DE | R_HL;
      in->wr = R_F | R_BC | R_DE | R_HL;
      full_rmw(in,R_DE,0L,2);
      if(sub & 0x10U)
        {
          in->kind = K_REPEAT;
          in->uses_pc0 = 1;
          snprintf(buf,sizeof buf,
                   "  Z80_PC = (uint16)(z80_pc0 + 0x%04lXU); Z80_OP_%s(); /* %s: PC past it first, the macro backs PC up when it repeats */",
                   (off + 2UL) & 0xFFFFUL,(sub & 8U) ? "LDDR" : "LDIR",(sub & 8U) ? "lddr" : "ldir");
        }
      else
        snprintf(buf,sizeof buf,"  Z80_OP_%s(); /* %s */",(sub & 8U) ? "LDD" : "LDI",(sub & 8U) ? "ldd" : "ldi");
      set_text(in,buf);
      return;
    case 0xA1: case 0xA9: case 0xB1: case 0xB9:
      in->rd = R_A | R_F | R_BC | R_HL;
      in->wr = R_F | R_BC | R_HL;
      in->mem = 1;
      in->acc_full += 1;
      if(sub & 0x10U)
        {
          in->kind = K_REPEAT;
          in->uses_pc0 = 1;
          snprintf(buf,sizeof buf,
                   "  Z80_PC = (uint16)(z80_pc0 + 0x%04lXU); Z80_OP_%s(); /* %s: PC past it first, the macro backs PC up when it repeats */",
                   (off + 2UL) & 0xFFFFUL,(sub & 8U) ? "CPDR" : "CPIR",(sub & 8U) ? "cpdr" : "cpir");
        }
      else
        snprintf(buf,sizeof buf,"  Z80_OP_%s(); /* %s */",(sub & 8U) ? "CPD" : "CPI",(sub & 8U) ? "cpd" : "cpi");
      set_text(in,buf);
      return;
    case 0xA2: case 0xAA: case 0xB2: case 0xBA:
    case 0xA3: case 0xAB: case 0xB3: case 0xBB:
      in->rd = R_F | R_BC | R_HL;
      in->wr = R_F | R_B | R_HL;
      /* The port moves write the byte (IN) or read it (OUT). */
      if(sub & 1U)
        {
          in->mem = 1;
          in->acc_full += 1;
        }
      else
        full_rmw(in,R_HL,0L,1);
      {
        const char *name = (sub & 1U) ? ((sub & 8U) ? "OUTD" : "OUTI")
                                      : ((sub & 8U) ? "IND" : "INI");
        const char *rname = (sub & 1U) ? ((sub & 8U) ? "OTDR" : "OTIR")
                                       : ((sub & 8U) ? "INDR" : "INIR");

        if(sub & 0x10U)
          {
            in->kind = K_REPEAT;
            in->uses_pc0 = 1;
            snprintf(buf,sizeof buf,
                     "  Z80_PC = (uint16)(z80_pc0 + 0x%04lXU); Z80_OP_%s(); /* PC past it first, the macro backs PC up when it repeats */",
                     (off + 2UL) & 0xFFFFUL,rname);
          }
        else
          snprintf(buf,sizeof buf,"  Z80_OP_%s();",name);
      }
      set_text(in,buf);
      return;
    default:
      break;
    }

  /* Not reached: fallback_reason has already sorted the rest out. */
  in->kind = K_FALLBACK;
  in->fb = FB_REFUSED;
}

/* Decodes the instruction behind an index prefix (z80.c, the DD/FD
   case: where the instruction said HL, it says the pair; the halves are
   the eight bit registers they are; the displaced byte replaces (HL)). */
static void decode_ddfd(insn_t *in, const unsigned char *p, unsigned long start, unsigned long off)
{
  unsigned pfx = p[0];
  unsigned sub = p[1];
  const char *pair = (pfx == 0xDDU) ? "Z80_IX" : "Z80_IY";
  const char *hi   = (pfx == 0xDDU) ? "Z80_IXH" : "Z80_IYH";
  const char *lo   = (pfx == 0xDDU) ? "Z80_IXL" : "Z80_IYL";
  const char *pn   = (pfx == 0xDDU) ? "ix" : "iy";
  unsigned bits    = (pfx == 0xDDU) ? R_IX : R_IY;
  unsigned bhi     = (pfx == 0xDDU) ? R_IXH : R_IYH;
  unsigned blo     = (pfx == 0xDDU) ? R_IXL : R_IYL;
  char buf[512];
  char ax[96];

  (void)start;
  (void)off;

  if(sub == 0xCBU)
    {
      decode_cb(in,p[3],pair,bits,disp8(p[2]));
      return;
    }

  if(ddfd_inert(sub))
    {
      /* z80.c, the inert path: the prefix left alone, the byte behind it
         executed on the next turn as the unprefixed load it is. */
      unsigned d = (sub >> 3) & 7U;
      unsigned s = sub & 7U;

      if(d == s)
        snprintf(buf,sizeof buf,"  /* %s prefix with nothing to substitute, then ld %s,%s -- no operation */",pn,r8name[d],r8name[s]);
      else
        {
          snprintf(buf,sizeof buf,"  %s = %s; /* %s prefix with nothing to substitute, then ld %s,%s */",r8[d],r8[s],pn,r8name[d],r8name[s]);
          in->rd = r8bit[s];
          in->wr = r8bit[d];
        }
      set_text(in,buf);
      return;
    }

  switch(sub)
    {
    case 0x09: case 0x19: case 0x29: case 0x39:
      /* z80.c, DD 0x09..0x39: Z80_OP_ADD16 on the halves. */
      {
        unsigned q = (sub >> 4) & 3U;

        snprintf(buf,sizeof buf,"  Z80_OP_ADD16(%s,%s,%s); /* add %s,%s */",
                 hi,lo,(q == 2U) ? pair : rp[q],pn,(q == 2U) ? pn : rpname[q]);
        in->rd = bits | R_F | ((q == 2U) ? 0U : rpbit[q]);
        in->wr = bits | R_F;
      }
      set_text(in,buf);
      return;
    case 0x21: /* z80.c, DD 0x21: the low half fetched, then the high */
      snprintf(buf,sizeof buf,"  %s = 0x%02XU; %s = 0x%02XU; /* ld %s,nn */",lo,(unsigned)p[2],hi,(unsigned)p[3],pn);
      set_text(in,buf);
      in->wr = bits;
      return;
    case 0x22: /* z80.c, DD 0x22: Z80_WR16 of the pair */
      snprintf(buf,sizeof buf,"  %s(0x%04lXU,%s); /* ld (nn),%s */",form_wr16(in,imm16(p + 2)),imm16(p + 2),pair,pn);
      set_text(in,buf);
      in->rd = bits;
      return;
    case 0x2A: /* z80.c, DD 0x2A: the word read, the halves set */
      snprintf(buf,sizeof buf,"  Z80_SET_%s(%s(0x%04lXU)); /* ld %s,(nn) */",pair + 4,form_rd16(in,imm16(p + 2)),imm16(p + 2),pn);
      set_text(in,buf);
      in->wr = bits;
      return;
    case 0x23: case 0x2B: /* z80.c, DD 0x23/0x2B: Z80_OP_INC_PAIR, Z80_OP_DEC_PAIR */
      snprintf(buf,sizeof buf,"  Z80_OP_%s_PAIR(%s,%s); /* %s %s */",
               (sub == 0x23U) ? "INC" : "DEC",hi,lo,(sub == 0x23U) ? "inc" : "dec",pn);
      set_text(in,buf);
      in->rd = bits;
      in->wr = bits;
      return;
    case 0x26: case 0x2E: /* z80.c, DD 0x26/0x2E: a half fetched */
      snprintf(buf,sizeof buf,"  %s = 0x%02XU; /* ld %s%c,n */",(sub == 0x26U) ? hi : lo,(unsigned)p[2],pn,(sub == 0x26U) ? 'h' : 'l');
      set_text(in,buf);
      in->wr = (sub == 0x26U) ? bhi : blo;
      return;
    case 0x24: case 0x2C: case 0x25: case 0x2D:
      /* z80.c, DD 0x24..0x2D: Z80_OP_INC_R, Z80_OP_DEC_R on a half */
      snprintf(buf,sizeof buf,"  Z80_OP_%s_R(%s); /* %s %s%c */",
               (sub & 1U) ? "DEC" : "INC",(sub & 8U) ? lo : hi,
               (sub & 1U) ? "dec" : "inc",pn,(sub & 8U) ? 'l' : 'h');
      set_text(in,buf);
      in->rd = ((sub & 8U) ? blo : bhi) | R_F;
      in->wr = ((sub & 8U) ? blo : bhi) | R_F;
      return;
    case 0x34: case 0x35:
      /* z80.c, DD 0x34/0x35: Z80_FETCH_IXY_ADDR then Z80_OP_INC_MEM_AT / DEC */
      ixaddr_text(ax,sizeof ax,pair,disp8(p[2]));
      snprintf(buf,sizeof buf,"  { %s; Z80_OP_%s_MEM_AT(ixaddr); } /* %s (%s+d) */",
               ax,(sub == 0x34U) ? "INC" : "DEC",(sub == 0x34U) ? "inc" : "dec",pn);
      set_text(in,buf);
      in->rd = bits | R_F;
      in->wr = R_F;
      full_rmw(in,bits,disp8(p[2]),2);
      return;
    case 0x36: /* z80.c, DD 0x36: the address, then the immediate, then Z80_WR8 */
      ixaddr_text(ax,sizeof ax,pair,disp8(p[2]));
      snprintf(buf,sizeof buf,"  { %s; %s(ixaddr,0x%02XU); } /* ld (%s+d),n */",ax,pair_wr8(in,bits,disp8(p[2])),(unsigned)p[3],pn);
      set_text(in,buf);
      in->rd = bits;
      return;
    case 0xE1: /* z80.c, DD 0xE1: Z80_OP_POP on the halves */
      snprintf(buf,sizeof buf,"  %s(%s,%s); /* pop %s */",STK_POP(in),hi,lo,pn);
      set_text(in,buf);
      in->rd = R_SP;
      in->wr = R_SP | bits;
      return;
    case 0xE3: /* z80.c, DD 0xE3: Z80_OP_EX_SP_PAIR on the halves */
      snprintf(buf,sizeof buf,"  %s(%s,%s); /* ex (sp),%s */",STK_EXSP(in),hi,lo,pn);
      set_text(in,buf);
      in->rd = R_SP | bits;
      in->wr = bits;
      return;
    case 0xE5: /* z80.c, DD 0xE5: Z80_OP_PUSH of the pair */
      snprintf(buf,sizeof buf,"  %s(%s); /* push %s */",STK_PUSH(in,0),pair,pn);
      set_text(in,buf);
      in->rd = R_SP | bits;
      in->wr = R_SP;
      return;
    case 0xE9: /* z80.c, DD 0xE9: Z80_PC = pair */
      snprintf(buf,sizeof buf,"  Z80_PC = %s; /* jp (%s) */",pair,pn);
      set_text(in,buf);
      in->rd = bits;
      in->kind = K_JUMP_LOST;
      return;
    case 0xF9: /* z80.c, DD 0xF9: Z80_SP = pair */
      snprintf(buf,sizeof buf,"  Z80_SP = %s; /* ld sp,%s */",pair,pn);
      set_text(in,buf);
      in->rd = bits;
      in->wr = R_SP;
      return;
    default:
      break;
    }

  if((sub & 0xC0U) == 0x40U)
    {
      /* z80.c, DD 0x44..0x7D and 0x46..0x77: a half into a register or
         back, the displaced byte into a register or back. H and L are
         the real H and L on the displaced forms. */
      unsigned d = (sub >> 3) & 7U;
      unsigned s = sub & 7U;

      if(s == 6U)
        {
          ixaddr_text(ax,sizeof ax,pair,disp8(p[2]));
          snprintf(buf,sizeof buf,"  { %s; %s = %s(ixaddr); } /* ld %s,(%s+d) */",ax,r8[d],pair_rd8(in,bits,disp8(p[2])),r8name[d],pn);
          set_text(in,buf);
          in->rd = bits;
          in->wr = r8bit[d];
          return;
        }
      if(d == 6U)
        {
          ixaddr_text(ax,sizeof ax,pair,disp8(p[2]));
          snprintf(buf,sizeof buf,"  { %s; %s(ixaddr,%s); } /* ld (%s+d),%s */",ax,pair_wr8(in,bits,disp8(p[2])),r8[s],pn,r8name[s]);
          set_text(in,buf);
          in->rd = bits | r8bit[s];
          return;
        }
      {
        const char *dn = (d == 4U) ? hi : (d == 5U) ? lo : r8[d];
        const char *sn = (s == 4U) ? hi : (s == 5U) ? lo : r8[s];
        unsigned db = (d == 4U) ? bhi : (d == 5U) ? blo : r8bit[d];
        unsigned sb = (s == 4U) ? bhi : (s == 5U) ? blo : r8bit[s];

        if(d == s)
          snprintf(buf,sizeof buf,"  /* ld %s%c,%s%c -- no operation */",pn,(d == 4U) ? 'h' : 'l',pn,(d == 4U) ? 'h' : 'l');
        else
          {
            snprintf(buf,sizeof buf,"  %s = %s; /* ld half */",dn,sn);
            in->rd = sb;
            in->wr = db;
          }
        set_text(in,buf);
      }
      return;
    }

  if((sub & 0xC0U) == 0x80U)
    {
      /* z80.c, DD 0x84..0xBD: the eight operations against a half, and
         0x86..0xBE against the displaced byte. */
      unsigned a = (sub >> 3) & 7U;
      unsigned s = sub & 7U;
      char opnd[64];

      if(s == 6U)
        {
          char rdx[32];

          ixaddr_text(ax,sizeof ax,pair,disp8(p[2]));
          snprintf(rdx,sizeof rdx,"%s(ixaddr)",pair_rd8(in,bits,disp8(p[2])));
          snprintf(opnd,sizeof opnd,alu_fmt[a],rdx);
          snprintf(buf,sizeof buf,"  { %s; %s; } /* %s (%s+d) */",ax,opnd,alu_name[a],pn);
          in->rd = bits | alu_rd[a];
        }
      else
        {
          snprintf(opnd,sizeof opnd,alu_fmt[a],(s == 4U) ? hi : lo);
          snprintf(buf,sizeof buf,"  %s; /* %s %s%c */",opnd,alu_name[a],pn,(s == 4U) ? 'h' : 'l');
          in->rd = ((s == 4U) ? bhi : blo) | alu_rd[a];
        }
      in->wr = alu_wr[a];
      set_text(in,buf);
      return;
    }

  in->kind = K_FALLBACK;
  in->fb = FB_REFUSED;
}

static void decode_form(const unsigned char *p, unsigned long start, unsigned long off,
                        unsigned long avail, insn_t *in);
static void classify(insn_t *in);

/* Decodes the instruction at p, at offset off in the block that starts
   at start, with avail bytes readable, under what is known of the
   registers there (regs, or NULL for nothing), and classifies it. */
static void decode(const unsigned char *p, unsigned long start, unsigned long off,
                   unsigned long avail, const regs_t *regs, insn_t *in)
{
  proof = regs;
  decode_form(p,start,off,avail,in);
  proof = NULL;
  classify(in);
}

static void decode_form(const unsigned char *p, unsigned long start, unsigned long off,
                        unsigned long avail, insn_t *in)
{
  unsigned op = p[0];
  char buf[512];

  memset(in,0,sizeof *in);
  in->len = insn_len(p,avail);
  in->kind = K_PLAIN;

  in->fb = fallback_reason(p,avail);
  if(in->fb != FB_NONE)
    {
      in->kind = K_FALLBACK;
      return;
    }

  if(op == 0xCBU)
    {
      decode_cb(in,p[1],NULL,0U,0L);
      return;
    }
  if(op == 0xEDU)
    {
      decode_ed(in,p,start,off);
      return;
    }
  if(op == 0xDDU || op == 0xFDU)
    {
      decode_ddfd(in,p,start,off);
      return;
    }

  /* The register-to-register loads and the byte HL points at (z80.c,
     cases 0x40..0x7F). */
  if((op & 0xC0U) == 0x40U)
    {
      unsigned d = (op >> 3) & 7U;
      unsigned s = op & 7U;

      if(d == 6U)
        {
          snprintf(buf,sizeof buf,"  %s(Z80_HL,%s); /* ld (hl),%s */",pair_wr8(in,R_HL,0L),r8[s],r8name[s]);
          in->rd = R_HL | r8bit[s];
        }
      else if(s == 6U)
        {
          snprintf(buf,sizeof buf,"  %s = %s(Z80_HL); /* ld %s,(hl) */",r8[d],pair_rd8(in,R_HL,0L),r8name[d]);
          in->rd = R_HL;
          in->wr = r8bit[d];
        }
      else
        {
          if(d == s)
            snprintf(buf,sizeof buf,"  /* ld %s,%s -- no operation */",r8name[d],r8name[s]);
          else
            {
              snprintf(buf,sizeof buf,"  %s = %s; /* ld %s,%s */",r8[d],r8[s],r8name[d],r8name[s]);
              in->rd = r8bit[s];
              in->wr = r8bit[d];
            }
        }
      set_text(in,buf);
      return;
    }

  /* The eight operations against the eight sources (z80.c, cases
     0x80..0xBF). */
  if((op & 0xC0U) == 0x80U)
    {
      unsigned a = (op >> 3) & 7U;
      unsigned s = op & 7U;
      char opnd[64];

      if(s == 6U)
        {
          char rdx[32];

          snprintf(rdx,sizeof rdx,"%s(Z80_HL)",pair_rd8(in,R_HL,0L));
          snprintf(opnd,sizeof opnd,alu_fmt[a],rdx);
          in->rd = R_HL | alu_rd[a];
        }
      else
        {
          snprintf(opnd,sizeof opnd,alu_fmt[a],r8[s]);
          in->rd = r8bit[s] | alu_rd[a];
        }
      in->wr = alu_wr[a];
      snprintf(buf,sizeof buf,"  %s; /* %s %s */",opnd,alu_name[a],r8name[s]);
      set_text(in,buf);
      return;
    }

  /* The same eight against an immediate (z80.c, cases 0xC6..0xFE). */
  if((op & 0xC7U) == 0xC6U)
    {
      unsigned a = (op >> 3) & 7U;
      char imm[16];
      char opnd[64];

      snprintf(imm,sizeof imm,"0x%02XU",(unsigned)p[1]);
      snprintf(opnd,sizeof opnd,alu_fmt[a],imm);
      snprintf(buf,sizeof buf,"  %s; /* %s n */",opnd,alu_name[a]);
      in->rd = alu_rd[a];
      in->wr = alu_wr[a];
      set_text(in,buf);
      return;
    }

  /* The eight bit immediate loads (z80.c, cases 0x06..0x3E). */
  if((op & 0xC7U) == 0x06U)
    {
      unsigned d = (op >> 3) & 7U;

      if(d == 6U)
        {
          snprintf(buf,sizeof buf,"  %s(Z80_HL,0x%02XU); /* ld (hl),n */",pair_wr8(in,R_HL,0L),(unsigned)p[1]);
          in->rd = R_HL;
        }
      else
        {
          snprintf(buf,sizeof buf,"  %s = 0x%02XU; /* ld %s,n */",r8[d],(unsigned)p[1],r8name[d]);
          in->wr = r8bit[d];
        }
      set_text(in,buf);
      return;
    }

  /* Increment and decrement, eight bits (z80.c, cases 0x04..0x3D). */
  if((op & 0xC6U) == 0x04U)
    {
      unsigned d = (op >> 3) & 7U;

      if(d == 6U)
        {
          snprintf(buf,sizeof buf,"  Z80_OP_%s_MEM(); /* %s (hl) */",(op & 1U) ? "DEC" : "INC",(op & 1U) ? "dec" : "inc");
          in->rd = R_HL | R_F;
          in->wr = R_F;
          full_rmw(in,R_HL,0L,2);
        }
      else
        {
          snprintf(buf,sizeof buf,"  Z80_OP_%s_R(%s); /* %s %s */",(op & 1U) ? "DEC" : "INC",r8[d],(op & 1U) ? "dec" : "inc",r8name[d]);
          in->rd = r8bit[d] | R_F;
          in->wr = r8bit[d] | R_F;
        }
      set_text(in,buf);
      return;
    }

  /* The pair forms of the first quarter (z80.c, cases 0x01..0x3B). */
  if((op & 0xCFU) == 0x01U)
    {
      unsigned q = (op >> 4) & 3U;

      if(q == 3U)
        snprintf(buf,sizeof buf,"  Z80_SP = 0x%04lXU; /* ld sp,nn */",imm16(p + 1));
      else
        snprintf(buf,sizeof buf,"  Z80_SET_%s(0x%04lXU); /* ld %s,nn */",rp[q] + 4,imm16(p + 1),rpname[q]);
      in->wr = rpbit[q];
      set_text(in,buf);
      return;
    }
  if((op & 0xC7U) == 0x03U)
    {
      unsigned q = (op >> 4) & 3U;

      if(q == 3U)
        snprintf(buf,sizeof buf,"  Z80_SP = (uint16)(Z80_SP %c 1); /* %s sp */",(op & 8U) ? '-' : '+',(op & 8U) ? "dec" : "inc");
      else
        snprintf(buf,sizeof buf,"  Z80_SET_%s((uint16)(%s %c 1)); /* %s %s */",rp[q] + 4,rp[q],(op & 8U) ? '-' : '+',(op & 8U) ? "dec" : "inc",rpname[q]);
      in->rd = rpbit[q];
      in->wr = rpbit[q];
      set_text(in,buf);
      return;
    }
  if((op & 0xCFU) == 0x09U)
    {
      unsigned q = (op >> 4) & 3U;

      snprintf(buf,sizeof buf,"  Z80_OP_ADD_HL(%s); /* add hl,%s */",rp[q],rpname[q]);
      in->rd = R_HL | R_F | rpbit[q];
      in->wr = R_HL | R_F;
      set_text(in,buf);
      return;
    }

  /* The conditional forms (z80.c, the conditional branches: Z80_OP_JR_CC,
     Z80_OP_JP_CC, Z80_OP_CALL_CC, Z80_OP_RET_CC), each as a test and a
     taken exit; the untaken path falls through. */
  if((op & 0xE7U) == 0x20U)
    {
      unsigned c = (op >> 3) & 3U;

      in->kind = K_COND;
      in->rd = R_F;
      set_text(in,"");
      snprintf(in->cond,sizeof in->cond,"%s",cc[c]);
      set_rel_target(in,start,off,disp8(p[1]));
      snprintf(in->taken,sizeof in->taken,"Z80_PC = (uint16)(z80_pc0 + 0x%04lXU); /* jr %s,%ld */",
               in->target_addr,ccname[c],disp8(p[1]));
      return;
    }
  if((op & 0xC7U) == 0xC2U)
    {
      unsigned c = (op >> 3) & 7U;

      in->kind = K_COND;
      in->rd = R_F;
      set_text(in,"");
      snprintf(in->cond,sizeof in->cond,"%s",cc[c]);
      set_abs_target(in,start,imm16(p + 1));
      snprintf(in->taken,sizeof in->taken,"Z80_PC = 0x%04lXU; /* jp %s,nn */",in->target_addr,ccname[c]);
      return;
    }
  if((op & 0xC7U) == 0xC4U)
    {
      unsigned c = (op >> 3) & 7U;

      in->kind = K_COND;
      in->rd = R_F | R_SP;
      in->wr = R_SP;
      in->uses_pc0 = 1;
      set_text(in,"");
      snprintf(in->cond,sizeof in->cond,"%s",cc[c]);
      set_abs_target(in,start,imm16(p + 1));
      snprintf(in->taken,sizeof in->taken,
               "%s((uint16)(z80_pc0 + 0x%04lXU)); Z80_PC = 0x%04lXU; /* call %s,nn */",
               STK_PUSH(in,1),(off + 3UL) & 0xFFFFUL,in->target_addr,ccname[c]);
      return;
    }
  if((op & 0xC7U) == 0xC0U)
    {
      unsigned c = (op >> 3) & 7U;

      in->kind = K_COND;
      in->rd = R_F | R_SP;
      in->wr = R_SP;
      set_text(in,"");
      snprintf(in->cond,sizeof in->cond,"%s",cc[c]);
      snprintf(in->taken,sizeof in->taken,"%s(); /* ret %s */",STK_RET(in,1),ccname[c]);
      return;
    }
  if((op & 0xC7U) == 0xC7U)
    {
      /* z80.c, cases 0xC7..0xFF: Z80_OP_RST, the return address being the
         byte after the opcode, folded from the entry address. */
      in->kind = K_JUMP;
      in->rd = R_SP;
      in->wr = R_SP;
      in->uses_pc0 = 1;
      set_abs_target(in,start,(unsigned long)(op & 0x38U));
      snprintf(buf,sizeof buf,"  %s((uint16)(z80_pc0 + 0x%04lXU)); Z80_PC = 0x%04lXU; /* rst */",
               STK_PUSH(in,0),(off + 1UL) & 0xFFFFUL,in->target_addr);
      set_text(in,buf);
      return;
    }
  if((op & 0xCFU) == 0xC1U)
    {
      /* z80.c, cases 0xC1..0xF1: Z80_OP_POP into the halves, AF last. */
      unsigned q = (op >> 4) & 3U;

      if(q == 3U)
        {
          snprintf(buf,sizeof buf,"  %s(Z80_A,Z80_F); /* pop af */",STK_POP(in));
          set_text(in,buf);
          in->wr = R_A | R_F | R_SP;
        }
      else
        {
          snprintf(buf,sizeof buf,"  %s(%s,%s); /* pop %s */",STK_POP(in),r8[q * 2U],r8[q * 2U + 1U],rpname[q]);
          set_text(in,buf);
          in->wr = rpbit[q] | R_SP;
        }
      in->rd = R_SP;
      return;
    }
  if((op & 0xCFU) == 0xC5U)
    {
      /* z80.c, cases 0xC5..0xF5: Z80_OP_PUSH of the pair, AF last. */
      unsigned q = (op >> 4) & 3U;

      if(q == 3U)
        {
          snprintf(buf,sizeof buf,"  %s(Z80_AF); /* push af */",STK_PUSH(in,0));
          set_text(in,buf);
          in->rd = R_A | R_F | R_SP;
        }
      else
        {
          snprintf(buf,sizeof buf,"  %s(%s); /* push %s */",STK_PUSH(in,0),rp[q],rpname[q]);
          set_text(in,buf);
          in->rd = rpbit[q] | R_SP;
        }
      in->wr = R_SP;
      return;
    }

  switch(op)
    {
    case 0x00: /* z80.c, case 0x00 */
      set_text(in,"  /* nop */");
      return;
    case 0x02: case 0x12: /* z80.c, cases 0x02/0x12: Z80_WR8 through the pair */
      snprintf(buf,sizeof buf,"  %s(%s,Z80_A); /* ld (%s),a */",pair_wr8(in,rpbit[op >> 4],0L),rp[op >> 4],rpname[op >> 4]);
      set_text(in,buf);
      in->rd = rpbit[op >> 4] | R_A;
      return;
    case 0x0A: case 0x1A: /* z80.c, cases 0x0A/0x1A: Z80_RD8 through the pair */
      snprintf(buf,sizeof buf,"  Z80_A = %s(%s); /* ld a,(%s) */",pair_rd8(in,rpbit[op >> 4],0L),rp[op >> 4],rpname[op >> 4]);
      set_text(in,buf);
      in->rd = rpbit[op >> 4];
      in->wr = R_A;
      return;
    case 0x22: /* z80.c, case 0x22: Z80_WR16 of HL at nn */
      snprintf(buf,sizeof buf,"  %s(0x%04lXU,Z80_HL); /* ld (nn),hl */",form_wr16(in,imm16(p + 1)),imm16(p + 1));
      set_text(in,buf);
      in->rd = R_HL;
      return;
    case 0x2A: /* z80.c, case 0x2A: HL set from Z80_RD16 at nn */
      snprintf(buf,sizeof buf,"  Z80_SET_HL(%s(0x%04lXU)); /* ld hl,(nn) */",form_rd16(in,imm16(p + 1)),imm16(p + 1));
      set_text(in,buf);
      in->wr = R_HL;
      return;
    case 0x32: /* z80.c, case 0x32: Z80_WR8 of A at nn */
      snprintf(buf,sizeof buf,"  %s(0x%04lXU,Z80_A); /* ld (nn),a */",form_wr8(in,imm16(p + 1)),imm16(p + 1));
      set_text(in,buf);
      in->rd = R_A;
      return;
    case 0x3A: /* z80.c, case 0x3A: A from Z80_RD8 at nn */
      snprintf(buf,sizeof buf,"  Z80_A = %s(0x%04lXU); /* ld a,(nn) */",form_rd8(in,imm16(p + 1),1),imm16(p + 1));
      set_text(in,buf);
      in->wr = R_A;
      return;
    case 0xF9: /* z80.c, case 0xF9 */
      set_text(in,"  Z80_SP = Z80_HL; /* ld sp,hl */");
      in->rd = R_HL;
      in->wr = R_SP;
      return;
    case 0x07: case 0x0F: case 0x17: case 0x1F:
      /* z80.c, cases 0x07..0x1F: Z80_OP_RLCA, RRCA, RLA, RRA */
      {
        static const char *rot[4] = { "RLCA", "RRCA", "RLA", "RRA" };

        snprintf(buf,sizeof buf,"  Z80_OP_%s();",rot[op >> 3]);
      }
      set_text(in,buf);
      in->rd = R_A | R_F;
      in->wr = R_A | R_F;
      return;
    case 0x27: /* z80.c, case 0x27: Z80_OP_DAA */
      set_text(in,"  Z80_OP_DAA();");
      in->rd = R_A | R_F;
      in->wr = R_A | R_F;
      return;
    case 0x2F: /* z80.c, case 0x2F: Z80_OP_CPL */
      set_text(in,"  Z80_OP_CPL();");
      in->rd = R_A | R_F;
      in->wr = R_A | R_F;
      return;
    case 0x37: /* z80.c, case 0x37: Z80_OP_SCF */
      set_text(in,"  Z80_OP_SCF();");
      in->rd = R_A | R_F;
      in->wr = R_F;
      return;
    case 0x3F: /* z80.c, case 0x3F: Z80_OP_CCF */
      set_text(in,"  Z80_OP_CCF();");
      in->rd = R_A | R_F;
      in->wr = R_F;
      return;
    case 0x10: /* z80.c, case 0x10: Z80_OP_DJNZ, the counter stepped then tested */
      in->kind = K_COND;
      in->rd = R_B;
      in->wr = R_B;
      set_text(in,"  Z80_B = (uint8)(Z80_B - 1U); /* djnz: the counter, then the test */");
      snprintf(in->cond,sizeof in->cond,"Z80_B != 0U");
      set_rel_target(in,start,off,disp8(p[1]));
      snprintf(in->taken,sizeof in->taken,"Z80_PC = (uint16)(z80_pc0 + 0x%04lXU); /* djnz %ld */",
               in->target_addr,disp8(p[1]));
      return;
    case 0x18: /* z80.c, case 0x18: Z80_OP_JR, the target folded */
      in->kind = K_JUMP;
      set_rel_target(in,start,off,disp8(p[1]));
      snprintf(buf,sizeof buf,"  Z80_PC = (uint16)(z80_pc0 + 0x%04lXU); /* jr %ld */",in->target_addr,disp8(p[1]));
      set_text(in,buf);
      return;
    case 0xC3: /* z80.c, case 0xC3: Z80_OP_JP, the target folded */
      in->kind = K_JUMP;
      set_abs_target(in,start,imm16(p + 1));
      snprintf(buf,sizeof buf,"  Z80_PC = 0x%04lXU; /* jp nn */",in->target_addr);
      set_text(in,buf);
      return;
    case 0xE9: /* z80.c, case 0xE9 */
      in->kind = K_JUMP_LOST;
      set_text(in,"  Z80_PC = Z80_HL; /* jp (hl) */");
      in->rd = R_HL;
      return;
    case 0xC9: /* z80.c, case 0xC9: Z80_OP_RET */
      in->kind = K_JUMP_LOST;
      snprintf(buf,sizeof buf,"  %s(); /* ret */",STK_RET(in,0));
      set_text(in,buf);
      in->rd = R_SP;
      in->wr = R_SP;
      return;
    case 0xCD: /* z80.c, case 0xCD: Z80_OP_CALL, the return address folded */
      in->kind = K_JUMP;
      in->rd = R_SP;
      in->wr = R_SP;
      in->uses_pc0 = 1;
      set_abs_target(in,start,imm16(p + 1));
      snprintf(buf,sizeof buf,"  %s((uint16)(z80_pc0 + 0x%04lXU)); Z80_PC = 0x%04lXU; /* call nn */",
               STK_PUSH(in,0),(off + 3UL) & 0xFFFFUL,in->target_addr);
      set_text(in,buf);
      return;
    case 0x08: /* z80.c, case 0x08: Z80_OP_EX_AF */
      set_text(in,"  Z80_OP_EX_AF();");
      in->rd = R_A | R_F;
      in->wr = R_A | R_F;
      return;
    case 0xD9: /* z80.c, case 0xD9: Z80_OP_EXX */
      set_text(in,"  Z80_OP_EXX();");
      in->rd = R_BC | R_DE | R_HL;
      in->wr = R_BC | R_DE | R_HL;
      return;
    case 0xE3: /* z80.c, case 0xE3: Z80_OP_EX_SP_HL, the pair form of it */
      snprintf(buf,sizeof buf,"  %s(Z80_H,Z80_L); /* ex (sp),hl */",STK_EXSP(in));
      set_text(in,buf);
      in->rd = R_SP | R_HL;
      in->wr = R_HL;
      return;
    case 0xEB: /* z80.c, case 0xEB: Z80_OP_EX_DE_HL */
      set_text(in,"  Z80_OP_EX_DE_HL();");
      in->rd = R_DE | R_HL;
      in->wr = R_DE | R_HL;
      return;
    case 0xD3: /* z80.c, case 0xD3: z80_io_write of A at the port */
      snprintf(buf,sizeof buf,"  z80_io_write(0x%02XU,Z80_A); /* out (n),a */",(unsigned)p[1]);
      set_text(in,buf);
      in->rd = R_A;
      return;
    case 0xDB: /* z80.c, case 0xDB: A from z80_io_read of the port */
      snprintf(buf,sizeof buf,"  Z80_A = z80_io_read(0x%02XU); /* in a,(n) */",(unsigned)p[1]);
      set_text(in,buf);
      in->wr = R_A;
      return;
    case 0xF3: /* z80.c, case 0xF3: Z80_OP_DI */
      set_text(in,"  Z80_OP_DI();");
      return;
    case 0xFB: /* z80.c, case 0xFB: Z80_OP_EI */
      set_text(in,"  Z80_OP_EI();");
      return;
    default:
      break;
    }

  /* Every unprefixed byte has a case in the interpreter; a byte reaching
     here is a hole in this tool, and the run must say so. */
  fprintf(stderr,"translate: no form for opcode 0x%02X at position %06lx\n",op,start + off);
  exit(2);
}

/* Whether the port an IN A,(n) reads is one a program waits on: the V
   counter (the even ports of the second quarter) or the status of the
   video part (the odd ports of the third), as src/cart.h decodes them.
   The data port, read in the same quarter, moves the address of the
   video part and is no wait. */
static int wait_port(unsigned port)
{
  unsigned quarter = (port >> 6) & 3U;

  if(quarter == 1U)
    return (port & 1U) == 0U;
  if(quarter == 2U)
    return (port & 1U) != 0U;
  return 0;
}

/* What the emitted text of a decoded instruction tells of a wait, read
   off the forms this file writes: the reads (Z80C_RD8, Z80C_RD16, and
   z80_io_read of an immediate port; a read through cbaddr is a read
   through HL, the bit test on (hl)) and the writes (Z80_WR8, Z80_WR16,
   the push, the exchange with the stack, the read-modify-write forms,
   the block moves and the port writes). The forms are this file's own,
   so a new form that reads or writes is added here as it is added
   above. A K_COND instruction's taken exit is part of it: a jr cc or jp
   cc back to the start closes the loop, and a call cc taken LEAVES the
   loop -- its push is on the taken exit, which is the way out, not a
   write of the loop -- so a conditional call inside a wait loop is
   judged by its untaken arm, and the loop is a wait if the rest is. */
static void classify(insn_t *in)
{
  static const char *const write_forms[] =
  {
    "Z80_WR8(", "Z80_WR16(", "Z80_OP_PUSH(", "Z80_OP_EX_SP", "_MEM_AT(",
    "_MEM(", "Z80_OP_LDI", "Z80_OP_LDD", "Z80_OP_INI", "Z80_OP_IND",
    "Z80_OP_RRD", "Z80_OP_RLD", "z80_io_write(", "Z80_OP_OUT",
    "Z80C_RAM_WR8(", "Z80C_RAM_WR16(", "Z80C_STK_PUSH(", "Z80C_STK_EXSP",
    NULL
  };
  /* The reads, the full path's and the direct path's alike: the form
     says nothing of the wait, the address does. */
  static const char *const read_forms[] =
  {
    "Z80C_RD8(", "Z80C_RD16(", "Z80C_RAM_RD8(", "Z80C_RAM_RD16(", NULL
  };
  const char *texts[2];
  const char *const *w;
  int t;

  in->wait_read = 0;
  in->wait_pair = 0U;
  in->writes = 0;
  if(in->kind == K_FALLBACK)
    return;
  texts[0] = in->text;
  texts[1] = in->taken;
  for(t = 0; t < 2; t++)
    {
      const char *x = texts[t];
      const char *r;

      /* The taken exit of a conditional call leaves the loop: what it
         pushes is not written inside it. */
      if(t == 1 && in->kind == K_COND && strstr(x,"Z80_OP_CALL") != NULL)
        ;
      else
        for(w = write_forms; *w != NULL; w++)
          if(strstr(x,*w) != NULL)
            in->writes = 1;

      r = NULL;
      for(w = read_forms; *w != NULL && r == NULL; w++)
        r = strstr(x,*w);
      if(r != NULL)
        {
          r = strchr(r,'(') + 1;
          if(strncmp(r,"0x",2) == 0)
            {
              if(strtoul(r,NULL,16) >= RAM_BASE)
                in->wait_read = 1;
            }
          else if(strncmp(r,"Z80_HL",6) == 0 || strncmp(r,"cbaddr",6) == 0)
            in->wait_pair = R_HL;
          else if(strncmp(r,"Z80_BC",6) == 0)
            in->wait_pair = R_BC;
          else if(strncmp(r,"Z80_DE",6) == 0)
            in->wait_pair = R_DE;
          else if(strncmp(r,"ixaddr",6) == 0)
            in->wait_pair = (strstr(x,"(uint32)Z80_IX") != NULL) ? R_IX : R_IY;
        }
      r = strstr(x,"z80_io_read(0x");
      if(r != NULL && wait_port((unsigned)strtoul(r + 12,NULL,16)))
        in->wait_read = 1;
    }
}

/* ---- blocks ---------------------------------------------------------- */

/* Why a block closed: END_BANKEND after a write proved to land on the
   mapper's registers, the bank behind the next instruction being
   possibly gone. */
enum end_kind { END_NEXT, END_JUMP, END_FALLBACK, END_CUT, END_BANK, END_REPEAT, END_BANKEND };

typedef struct
{
  unsigned long start;
  unsigned long end;      /* the position after the last emitted instruction */
  int n;
  insn_t ins[MAX_INSNS];
  enum end_kind kind;
  unsigned loaded;        /* registers read before being written */
  unsigned written;       /* registers written anywhere in the block */
  int wait;               /* the block starts a loop the program waits in */
} block_t;

/* The two flags of a table entry (src/z80c.h). */
#define FLAG_WAIT    1UL
#define FLAG_BANKEND 2UL

/* Whether the block starts a wait: among its first WAIT_INSNS
   instructions, a branch back to the block's own start closes a loop
   that reads a fixed byte -- an absolute one, or one through a pair no
   instruction of the loop writes -- and writes nothing to memory nor to
   a port. The loop is the instructions from the start to that branch,
   the branch included; an unconditional transfer elsewhere, a lost
   target or a repeat met first means no loop closes on the start. */
static int wait_of(const block_t *b)
{
  int i, j;
  int n = (b->n < WAIT_INSNS) ? b->n : WAIT_INSNS;
  unsigned written = 0;

  for(i = 0; i < n; i++)
    {
      const insn_t *in = &b->ins[i];
      int closes = (in->kind == K_COND || in->kind == K_JUMP) &&
                   in->target_known && in->target_pos == b->start;

      written |= in->wr;
      if(closes)
        {
          for(j = 0; j <= i; j++)
            if(b->ins[j].writes)
              return 0;
          for(j = 0; j <= i; j++)
            {
              const insn_t *r = &b->ins[j];

              if(r->wait_read)
                return 1;
              if(r->wait_pair != 0U && (r->wait_pair & written) == 0U)
                return 1;
            }
          return 0;
        }
      if(in->kind == K_JUMP || in->kind == K_JUMP_LOST || in->kind == K_REPEAT)
        return 0;
    }
  return 0;
}

/* Scans the block that starts at a position: the instructions it holds,
   why it closes and whether it starts a wait. Adds the starts a cut or a
   fallback creates. */
static void scan_block(unsigned long start, block_t *b)
{
  unsigned long bank = start / BANK_SIZE;
  unsigned long end  = (bank + 1UL) * BANK_SIZE;
  unsigned long pos  = start;
  regs_t regs, before;

  if(start < SLOT0_FIXED)
    end = SLOT0_FIXED;

  b->start = start;
  b->n = 0;
  b->loaded = 0;
  b->written = 0;
  b->wait = 0;
  b->kind = END_BANK;
  /* Nothing is known of the registers at a block's head: a block is
     entered from anywhere. */
  regs_reset(&regs);

  for(;;)
    {
      insn_t *in;
      int len;

      if(pos >= end)
        {
          b->kind = END_BANK;
          break;
        }
      if(pos != start && (mark[pos] & M_START))
        {
          b->kind = END_NEXT;
          break;
        }
      len = insn_len(rom + pos,end - pos);
      if(pos + (unsigned long)len > end)
        {
          b->kind = END_BANK;
          break;
        }

      /* The instruction table is full: the block is cut here, and the
         instruction it could not take starts the next one. */
      if(b->n >= MAX_INSNS)
        {
          b->kind = END_CUT;
          add_start(pos);
          break;
        }
      in = &b->ins[b->n];
      decode(rom + pos,start,pos - start,end - pos,&regs,in);

      if(in->kind == K_FALLBACK)
        {
          b->kind = END_FALLBACK;
          add_start(pos + (unsigned long)len);
          break;
        }

      b->n++;
      b->loaded |= in->rd & ~b->written;
      b->written |= in->wr;
      /* What the instruction leaves known: the registers it writes are
         lost, then the forms that set a value put one back. */
      before = regs;
      regs_after(&regs,in,rom + pos);
      regs_step(&regs,&before,rom + pos);
      pos += (unsigned long)len;

      if(in->kind == K_JUMP || in->kind == K_JUMP_LOST)
        {
          b->kind = END_JUMP;
          break;
        }
      if(in->kind == K_REPEAT)
        {
          b->kind = END_REPEAT;
          break;
        }
      /* A write proved to land on the mapper's registers: the bank
         behind the next instruction may be gone, the block closes and
         the core asks the tables again (src/z80c.c). The next
         instruction is a start of its own. */
      if(in->bankend)
        {
          /* On the last byte of the bank the edge closes the block as
             it would anyway: no start queued in the next bank, no
             linear successor rendered into it. */
          if(pos < end)
            {
              b->kind = END_BANKEND;
              add_start(pos);
            }
          else
            b->kind = END_BANK;
          break;
        }
    }

  b->end = pos;
  b->wait = (b->n > 0) && (wait_of(b) || (mark[start] & M_WAIT) != 0U);
  /* A wait the seeds name on a block of no instruction -- the position
     holds something the interpreter keeps -- carries no flag: the line
     there ends only if the interpreter's own wait ends it. */
  if(b->n == 0 && (mark[start] & M_WAIT) != 0U)
    fprintf(stderr,"translate: wait at %06lx is a fallback, not marked\n",start);
}

/* The blocks of the table being written, in position order, and the
   index each has in it. */
typedef struct
{
  unsigned long pos;
  unsigned long bytes;    /* the bytes it covers */
  unsigned long hits;     /* from the counts file, when there is one */
  unsigned long weight;   /* the instructions it ran over the recorded run */
  int wait;               /* it starts a wait: always in the table */
  int bankend;            /* it closes on a write to the mapper's registers */
  int selected;
  long index;             /* its index in the written table, when selected */
} blockinfo_t;

static blockinfo_t *blocks;
static unsigned long nblocks;

/* The index in the written table of the block at a position, or -1. */
static long table_index(unsigned long pos)
{
  unsigned long lo = 0, hi = nblocks;

  if(pos >= rom_size || !(mark[pos] & M_BLOCK))
    return -1L;
  while(lo < hi)
    {
      unsigned long mid = lo + ((hi - lo) >> 1);

      if(blocks[mid].pos < pos)
        lo = mid + 1;
      else
        hi = mid;
    }
  if(lo < nblocks && blocks[lo].pos == pos)
    return blocks[lo].index;
  return -1L;
}

/* The flags of the block at a position, as the block list holds them:
   whether it starts a wait, whether it closes on a mapper write. */
static unsigned long blocks_flags_at(unsigned long pos)
{
  unsigned long lo = 0, hi = nblocks;

  while(lo < hi)
    {
      unsigned long mid = lo + ((hi - lo) >> 1);

      if(blocks[mid].pos < pos)
        lo = mid + 1;
      else
        hi = mid;
    }
  if(lo < nblocks && blocks[lo].pos == pos)
    return (blocks[lo].wait ? FLAG_WAIT : 0UL) | (blocks[lo].bankend ? FLAG_BANKEND : 0UL);
  return 0UL;
}

/* One block's C, accumulated before its head is written: whether the
   entry address is used decides whether the local is declared at all.
   Sized for MAX_INSNS instructions of the longest form, each with an
   exit of its own. */
static char body[MAX_INSNS * 1024];
static size_t body_n;
static int uses_pc0;

static void emit(const char *line)
{
  size_t n = strlen(line);

  if(body_n + n + 1 >= sizeof body)
    {
      fprintf(stderr,"translate: a block outgrew its buffer\n");
      exit(2);
    }
  memcpy(body + body_n,line,n);
  body_n += n;
}

/* The successor a transfer renders: the table entry of the block at the
   target, when the target is in the block's bank, on the same side of
   the fixed kilobyte, and a block is written there; behind the window
   test for an absolute target. "0" otherwise. */
static void succ_expr(char *buf, size_t cap, unsigned long start, const insn_t *in)
{
  long k;

  if(!in->target_known || in->target_pos >= rom_size)
    {
      snprintf(buf,cap,"0");
      return;
    }
  if(in->target_pos / BANK_SIZE != start / BANK_SIZE)
    {
      snprintf(buf,cap,"0");
      return;
    }
  if(start < SLOT0_FIXED && in->target_pos >= SLOT0_FIXED)
    {
      snprintf(buf,cap,"0");
      return;
    }
  k = table_index(in->target_pos);
  if(k < 0L)
    {
      snprintf(buf,cap,"0");
      return;
    }
  if(in->target_rel)
    {
      /* A relative target under the first kilobyte of its bank, from a
         bank that is not bank 0: in window 0 the mapper keeps bank 0's
         kilobyte in place there, so the bytes at the address are not
         the ones translated; rendered only while the block runs in
         another window. */
      if(start / BANK_SIZE != 0UL && (in->target_pos % BANK_SIZE) < SLOT0_FIXED)
        {
          snprintf(buf,cap,"(((z80_pc0 & 0xC000U) != 0U) ? &z80c_table[%ld] : 0)",k);
          uses_pc0 = 1;
        }
      else
        snprintf(buf,cap,"&z80c_table[%ld]",k);
    }
  else
    {
      snprintf(buf,cap,"((((z80_pc0 ^ 0x%04lXU) & 0xC000U) == 0U) ? &z80c_table[%ld] : 0)",
               in->target_addr,k);
      uses_pc0 = 1;
    }
}

/* The stores of an exit: every register written so far, back to its
   field. */
static void emit_stores(unsigned written, const char *indent)
{
  unsigned i;
  char line[128];

  for(i = 0; i < R_COUNT; i++)
    if(written & (1U << i))
      {
        snprintf(line,sizeof line,"%s%s = %s;\n",indent,reg_state[i],reg_local[i]);
        emit(line);
      }
}

/* The tail of an exit: the instructions run to it, counted for the
   telemetry and for the block's own count, and the bytes each memory
   path moved, counted by the host runner (src/z80c.h). */
static void emit_tail(long k, int insns, int direct, int full, const char *indent)
{
  char line[192];

  snprintf(line,sizeof line,"%sZ80C_INSNS(%d);\n",indent,insns);
  emit(line);
  snprintf(line,sizeof line,"%sZ80C_RAN(%ld,%d);\n",indent,k,insns);
  emit(line);
  snprintf(line,sizeof line,"%sZ80C_ACCESS(%d,%d);\n",indent,direct,full);
  emit(line);
}

/* Writes the block at index k of the table. */
static void emit_block(FILE *out, const block_t *b, long k)
{
  unsigned written = 0;
  int i;
  int direct = 0, full = 0;
  char line[1024];
  char succ[128];
  const insn_t *last = (b->n > 0) ? &b->ins[b->n - 1] : NULL;

  body_n = 0;
  uses_pc0 = 0;

  snprintf(line,sizeof line,"  Z80C_HIT(%ld);\n",k);
  emit(line);

  for(i = 0; i < b->n; i++)
    {
      const insn_t *in = &b->ins[i];

      if(in->uses_pc0)
        uses_pc0 = 1;
      if(in->text[0] != '\0')
        {
          emit(in->text);
          emit("\n");
        }
      written |= in->wr;
      direct += in->acc_direct;
      full += in->acc_full;

      if(in->kind == K_COND)
        {
          succ_expr(succ,sizeof succ,b->start,in);
          snprintf(line,sizeof line,"  if(%s)\n    {\n      %s\n",in->cond,in->taken);
          emit(line);
          emit_stores(written,"      ");
          emit_tail(k,i + 1,direct + in->taken_direct,full + in->taken_full,"      ");
          snprintf(line,sizeof line,"      return %s;\n    }\n",succ);
          emit(line);
        }
    }

  /* The last exit: PC left where the block stops when no transfer set
     it, the stores, the tail, the successor. */
  if(b->kind != END_JUMP && b->kind != END_REPEAT)
    {
      snprintf(line,sizeof line,"  Z80_PC = (uint16)(z80_pc0 + 0x%04lXU); /* %s */\n",
               (b->end - b->start) & 0xFFFFUL,
               (b->kind == END_NEXT) ? "next block" :
               (b->kind == END_FALLBACK) ? "fallback: interpreted from here" :
               (b->kind == END_CUT) ? "cut" :
               (b->kind == END_BANKEND) ? "mapper written: the bank behind the next byte may have turned" :
               "bank edge");
      emit(line);
      uses_pc0 = 1;
    }
  emit_stores(written,"  ");
  emit_tail(k,b->n,direct,full,"  ");

  /* The successor after a mapper write is the linear one, as after any
     other instruction: the core drops it when the epoch has moved, and
     keeps it when the write turned nothing (the same bank written
     again), in which case the bytes are the ones translated. */
  switch(b->kind)
    {
    case END_NEXT:
    case END_CUT:
    case END_BANKEND:
      {
        long kn = table_index(b->end);

        if(kn >= 0L && b->end / BANK_SIZE == b->start / BANK_SIZE &&
           !(b->start < SLOT0_FIXED && b->end >= SLOT0_FIXED))
          snprintf(succ,sizeof succ,"&z80c_table[%ld]",kn);
        else
          snprintf(succ,sizeof succ,"0");
      }
      break;
    case END_JUMP:
      succ_expr(succ,sizeof succ,b->start,last);
      break;
    default:
      snprintf(succ,sizeof succ,"0");
      break;
    }
  snprintf(line,sizeof line,"  return %s;\n",succ);
  emit(line);

  /* The head, now that it is known what the body names: the base of
     the work RAM, which the direct forms index and every block takes;
     the entry address; and the locals -- loaded where read first, bare
     where written first. */
  fprintf(out,"static const z80c_entry_t *\nb_%06lx(uint8 *ram)\n{\n",b->start);
  if(uses_pc0)
    fprintf(out,"  uint16 z80_pc0 = Z80_PC;\n");
  {
    unsigned i2;

    for(i2 = 0; i2 < R_COUNT; i2++)
      {
        unsigned bit = 1U << i2;

        if(!((b->loaded | b->written) & bit))
          continue;
        if(b->loaded & bit)
          fprintf(out,"  %s %s = %s;\n",(bit == R_SP) ? "uint16" : "uint8",reg_local[i2],reg_state[i2]);
        else
          fprintf(out,"  %s %s;\n",(bit == R_SP) ? "uint16" : "uint8",reg_local[i2]);
      }
  }
  fprintf(out,"\n  (void)ram;\n");
  fwrite(body,1,body_n,out);
  fprintf(out,"}\n\n");
}

/* ---- the counts and the choice --------------------------------------- */

static unsigned long counts_frames;
static unsigned long counts_insns;

/* Reads the counts a recorded run wrote: a header line, then one line
   per block -- its position, how often it was entered, the instructions
   its exits ran. Positions with no block are ignored; a malformed file
   is refused. */
static void read_counts(const char *path)
{
  FILE *f = fopen(path,"r");
  char line[128];

  if(f == NULL)
    {
      fprintf(stderr,"translate: cannot open the counts %s\n",path);
      exit(2);
    }
  if(fgets(line,sizeof line,f) == NULL ||
     sscanf(line,"z80c-counts frames=%lu insns=%lu",&counts_frames,&counts_insns) != 2)
    {
      fprintf(stderr,"translate: %s is not a counts file\n",path);
      exit(2);
    }
  while(fgets(line,sizeof line,f) != NULL)
    {
      unsigned long pos, hits, ran;
      unsigned long lo = 0, hi = nblocks;

      if(sscanf(line,"%lx %lu %lu",&pos,&hits,&ran) != 3)
        {
          fprintf(stderr,"translate: bad line in %s: %s",path,line);
          exit(2);
        }
      while(lo < hi)
        {
          unsigned long mid = lo + ((hi - lo) >> 1);

          if(blocks[mid].pos < pos)
            lo = mid + 1;
          else
            hi = mid;
        }
      if(lo < nblocks && blocks[lo].pos == pos)
        {
          blocks[lo].hits = hits;
          blocks[lo].weight = ran;
        }
    }
  fclose(f);
}

/* Reads the seeds a run wrote (tests/z80c/sidebyside.c): a header naming
   the ROM, which must be this one, then one position per line, each
   made a start. A malformed file, or another ROM's, is refused. */
static void read_seeds(const char *path, unsigned long fnv)
{
  FILE *f = fopen(path,"r");
  char line[128];
  unsigned long bytes, seeds_fnv;

  if(f == NULL)
    {
      fprintf(stderr,"translate: cannot open the seeds %s\n",path);
      exit(2);
    }
  if(fgets(line,sizeof line,f) == NULL ||
     sscanf(line,"z80c-seeds rom_bytes=%lu rom_fnv=%lx",&bytes,&seeds_fnv) != 2)
    {
      fprintf(stderr,"translate: %s is not a seeds file\n",path);
      exit(2);
    }
  if(bytes != rom_size || seeds_fnv != fnv)
    {
      fprintf(stderr,"translate: the seeds %s are of another rom (%lu/%08lx, this one is %lu/%08lx)\n",
              path,bytes,seeds_fnv,rom_size,fnv);
      exit(2);
    }
  while(fgets(line,sizeof line,f) != NULL)
    {
      unsigned long pos;
      int wait = (strncmp(line,"wait ",5) == 0);

      if(sscanf(wait ? line + 5 : line,"%lx",&pos) != 1 || pos >= rom_size)
        {
          fprintf(stderr,"translate: bad line in %s: %s",path,line);
          exit(2);
        }
      add_start(pos);
      if(wait)
        mark[pos] |= M_WAIT;
      n_seeds++;
    }
  fclose(f);
}

static int by_weight(const void *a, const void *b)
{
  const blockinfo_t *x = a;
  const blockinfo_t *y = b;

  if(x->weight != y->weight)
    return (x->weight > y->weight) ? -1 : 1;
  return (x->pos < y->pos) ? -1 : (x->pos > y->pos) ? 1 : 0;
}

/* ---- main ------------------------------------------------------------ */

static unsigned long fnv1a(const unsigned char *p, unsigned long n)
{
  unsigned long h = 2166136261UL;
  unsigned long i;

  for(i = 0; i < n; i++)
    {
      h ^= (unsigned long)p[i];
      h *= 16777619UL;
      h &= 0xFFFFFFFFUL;
    }
  return h;
}

int main(int argc, char **argv)
{
  FILE *f;
  FILE *out;
  unsigned long fnv;
  unsigned long pos;
  unsigned long pct10;
  const char *counts_path = NULL;
  const char *seeds_path = NULL;
  unsigned long budget = 0;
  int have_budget = 0;
  int i;
  unsigned long n_all;
  unsigned long sel_bytes = 0, sel_weight = 0, all_weight = 0;
  block_t *blk;

  if(argc < 3)
    {
      fprintf(stderr,"usage: translate <rom> <out.c> [--seeds <file>] [--counts <file> --budget <bytes>]\n");
      return 2;
    }
  for(i = 3; i < argc; i++)
    {
      if(strcmp(argv[i],"--counts") == 0 && i + 1 < argc)
        counts_path = argv[++i];
      else if(strcmp(argv[i],"--seeds") == 0 && i + 1 < argc)
        seeds_path = argv[++i];
      else if(strcmp(argv[i],"--budget") == 0 && i + 1 < argc)
        {
          budget = strtoul(argv[++i],NULL,10);
          have_budget = 1;
        }
      else
        {
          fprintf(stderr,"translate: unknown argument %s\n",argv[i]);
          return 2;
        }
    }
  if((counts_path == NULL) != (!have_budget))
    {
      fprintf(stderr,"translate: --counts and --budget go together\n");
      return 2;
    }

  f = fopen(argv[1],"rb");
  if(f == NULL)
    {
      fprintf(stderr,"translate: cannot open the rom %s\n",argv[1]);
      return 2;
    }
  rom_size = (unsigned long)fread(rom,1,sizeof rom,f);
  if(ferror(f))
    {
      fclose(f);
      fprintf(stderr,"translate: cannot read the rom %s\n",argv[1]);
      return 2;
    }
  /* A read that fills the buffer exactly has not met the end of the file
     yet: one more byte says whether there is more. */
  if(rom_size == sizeof rom && fgetc(f) != EOF)
    {
      fclose(f);
      fprintf(stderr,"translate: the rom is larger than %lu bytes\n",ROM_CAPACITY);
      return 2;
    }
  fclose(f);

  if(rom_size < ROM_MIN ||
     (rom_size & (BANK_SIZE - 1UL)) != 0UL ||
     (rom_size & (rom_size - 1UL)) != 0UL)
    {
      fprintf(stderr,
              "translate: rom size %lu is not what the loader accepts "
              "(at least %lu, whole banks of %lu, a power of two)\n",
              rom_size,ROM_MIN,BANK_SIZE);
      return 2;
    }
  bank_mask = (rom_size / BANK_SIZE) - 1UL;

  fnv = fnv1a(rom,rom_size);

  blk = malloc(sizeof *blk);
  if(blk == NULL)
    {
      fprintf(stderr,"translate: out of memory\n");
      return 2;
    }

  /* Discovery from the three vectors and the seeds, then the blocks
     scanned until no scan adds a start: a cut or a fallback met while
     scanning opens a start that an earlier block may have walked over. */
  add_start(0x0000UL);
  add_start(0x0038UL);
  add_start(0x0066UL);
  if(seeds_path != NULL)
    read_seeds(seeds_path,fnv);
  do
    {
      starts_added = 0;
      while(queue_at < queue_n)
        discover(queue[queue_at++]);
      for(pos = 0; pos < rom_size; pos++)
        if(mark[pos] & M_START)
          scan_block(pos,blk);
    }
  while(starts_added);

  /* The blocks that exist: a start whose first instruction is emitted. */
  n_all = 0;
  for(pos = 0; pos < rom_size; pos++)
    if(mark[pos] & M_START)
      {
        scan_block(pos,blk);
        if(blk->n > 0)
          {
            mark[pos] |= M_ANY;
            n_all++;
          }
      }
  blocks = calloc((n_all == 0UL) ? 1UL : n_all,sizeof *blocks);
  if(blocks == NULL)
    {
      fprintf(stderr,"translate: out of memory\n");
      return 2;
    }
  nblocks = 0;
  for(pos = 0; pos < rom_size; pos++)
    if(mark[pos] & M_ANY)
      {
        scan_block(pos,blk);
        blocks[nblocks].pos = pos;
        blocks[nblocks].bytes = blk->end - pos;
        blocks[nblocks].wait = blk->wait;
        blocks[nblocks].bankend = (blk->kind == END_BANKEND);
        blocks[nblocks].selected = 1;
        nblocks++;
      }

  /* The choice under the budget: the blocks that start a wait are in
     first, whatever they ran -- a wait left out of the table is a line
     that never ends -- then blocks never run are out, and the others are
     taken by the instructions they ran while the bytes fit. */
  if(counts_path != NULL)
    {
      blockinfo_t *order;
      unsigned long j;

      read_counts(counts_path);
      order = malloc(((nblocks == 0UL) ? 1UL : nblocks) * sizeof *order);
      if(order == NULL)
        {
          fprintf(stderr,"translate: out of memory\n");
          return 2;
        }
      for(j = 0; j < nblocks; j++)
        {
          blocks[j].selected = 0;
          all_weight += blocks[j].weight;
          order[j] = blocks[j];
          if(blocks[j].wait)
            {
              blocks[j].selected = 1;
              sel_bytes += blocks[j].bytes;
              sel_weight += blocks[j].weight;
            }
        }
      /* The waits alone may cover more than the budget: they are in all
         the same, said so, and nothing else is taken. */
      if(sel_bytes > budget)
        fprintf(stderr,"translate: the waits alone cover %lu bytes, over the budget of %lu\n",
                sel_bytes,budget);
      qsort(order,nblocks,sizeof *order,by_weight);
      for(j = 0; j < nblocks; j++)
        {
          unsigned long lo = 0, hi = nblocks;

          if(order[j].wait || order[j].hits == 0UL)
            continue;
          if(sel_bytes + order[j].bytes > budget)
            continue;
          while(lo < hi)
            {
              unsigned long mid = lo + ((hi - lo) >> 1);

              if(blocks[mid].pos < order[j].pos)
                lo = mid + 1;
              else
                hi = mid;
            }
          /* order[] is a permutation of blocks[], so the search always
             lands on the entry it came from; said out loud because the
             write below has no other bound. */
          if(lo >= nblocks || blocks[lo].pos != order[j].pos)
            {
              fprintf(stderr,"translate: block %06lx lost while choosing\n",
                      order[j].pos);
              return 2;
            }
          blocks[lo].selected = 1;
          sel_bytes += order[j].bytes;
          sel_weight += order[j].weight;
        }
      free(order);
    }

  /* The written table: the selected blocks, in position order, each
     knowing its index. */
  {
    unsigned long j, k = 0;

    for(j = 0; j < nblocks; j++)
      {
        blocks[j].index = -1L;
        if(blocks[j].selected)
          {
            mark[blocks[j].pos] |= M_BLOCK;
            blocks[j].index = (long)k++;
          }
      }
    n_blocks = k;
  }

  out = fopen(argv[2],"w");
  if(out == NULL)
    {
      fprintf(stderr,"translate: cannot write %s\n",argv[2]);
      return 2;
    }

  fprintf(out,
          "/*\n"
          " * Translated cartridge code, written by tests/z80c/translate.c.\n"
          " * Generated: not tracked, not edited by hand. One function per\n"
          " * block, then the table from positions in the cartridge to the\n"
          " * functions, each entry carrying the block's flags (a wait, a\n"
          " * close on a mapper write); src/z80c.h says how the core runs\n"
          " * them, and that no block keeps an account of time. Every block\n"
          " * takes the base of the work RAM: the accesses the tool proved\n"
          " * to land there index it directly, the others go through the\n"
          " * page tables.\n"
          " *\n"
          " * The thirteen register names of z80_ops.h are retargeted below\n"
          " * onto locals of the block, the way z80.c retargets its five hot\n"
          " * names inside z80_run: a block declares the ones it touches,\n"
          " * loads at its entry those it reads first, stores at each exit\n"
          " * those it has written. PC stays on the structure.\n"
          " */\n"
          "#define Z80C_BLOCK_FILE 1\n"
          "#include \"z80_ops.h\"\n"
          "#include \"z80c.h\"\n\n");
  for(i = 0; i < R_COUNT; i++)
    fprintf(out,"#undef %s\n#define %s %s\n",reg_macro[i],reg_macro[i],reg_local[i]);
  fprintf(out,"\n#if Z80C_HITS\nuint32 z80c_hits[%lu];\nuint32 z80c_ran[%lu];\n#endif\n\n",
          (n_blocks == 0UL) ? 1UL : n_blocks,(n_blocks == 0UL) ? 1UL : n_blocks);

  /* Emission in position order, which is the table's order. */
  for(pos = 0; pos < rom_size; pos++)
    if(mark[pos] & M_BLOCK)
      {
        long k = table_index(pos);
        unsigned long i2;

        scan_block(pos,blk);
        emit_block(out,blk,k);

        n_emitted += (unsigned long)blk->n;
        if(blk->wait)
          n_waits++;
        /* The direct path, over the instructions written: how many
           read memory, how many write it, and how many of each were
           proved; the absolute reads of the fixed kilobyte. */
        for(i2 = 0; i2 < (unsigned long)blk->n; i2++)
          {
            const insn_t *in = &blk->ins[i2];

            if(in->mem == 1)
              {
                n_rd_all++;
                n_rd_direct += (in->direct != 0);
              }
            else if(in->mem == 2)
              {
                n_wr_all++;
                n_wr_direct += (in->direct != 0);
              }
            n_rom_fixed += (in->rom_fixed != 0);
          }
        for(i2 = pos; i2 < blk->end; i2++)
          if(!(mark[i2] & M_COVER))
            {
              mark[i2] |= M_COVER;
              n_code_bytes++;
            }
        switch(blk->kind)
          {
          case END_NEXT:     n_end_next++;     break;
          case END_JUMP:     n_end_jump++;     break;
          case END_FALLBACK: n_end_fallback++; break;
          case END_CUT:      n_end_cut++;      break;
          case END_BANK:     n_end_bank++;     break;
          case END_REPEAT:   n_end_repeat++;   break;
          case END_BANKEND:  n_end_bankend++;  break;
          }
      }

  fprintf(out,"const uint32 z80c_rom_size    = %luUL;\n",rom_size);
  fprintf(out,"const uint32 z80c_rom_fnv     = 0x%08lXUL;\n",fnv);
  fprintf(out,"const uint32 z80c_code_bytes  = %luUL;\n",n_code_bytes);
  fprintf(out,"const uint32 z80c_block_count = %luUL;\n\n",n_blocks);

  if(n_blocks == 0UL)
    fprintf(out,"const z80c_entry_t z80c_table[1] = { { 0UL, 0, 0UL } };\n");
  else
    {
      fprintf(out,"const z80c_entry_t z80c_table[%lu] =\n{\n",n_blocks);
      for(pos = 0; pos < rom_size; pos++)
        if(mark[pos] & M_BLOCK)
          {
            long k = table_index(pos);

            fprintf(out,"  { 0x%06lXUL, b_%06lx, %luUL },\n",pos,pos,
                    (k >= 0L) ? blocks_flags_at(pos) : 0UL);
          }
      fprintf(out,"};\n");
    }

  if(fclose(out) != 0)
    {
      fprintf(stderr,"translate: cannot close %s\n",argv[2]);
      return 2;
    }

  pct10 = (n_code_bytes * 1000UL) / rom_size;
  printf("z80c: rom %lu/%08lx blocks=%lu insns=%lu bytes=%lu covered=%lu.%lu%% "
         "emitted=%lu fallback_ends=%lu\n",
         rom_size,fnv,n_blocks,n_insns,n_code_bytes,pct10 / 10UL,pct10 % 10UL,
         n_emitted,n_end_fallback);
  printf("z80c: fallback ops: halt=%lu im=%lu ld_a_r=%lu ld_r_a=%lu refused=%lu waits=%lu seeds=%lu\n",
         n_fb_halt,n_fb_im,n_fb_ld_a_r,n_fb_ld_r_a,n_fb_refused,n_waits,n_seeds);
  if(n_ram_targets != 0UL)
    printf("z80c: ram targets=%lu walked, first %04lX reached from %06lx: code without a position, not translated\n",
           n_ram_targets,ram_target_addr,ram_target_from);
  /* The direct path: instructions of the written table, proved over
     all; the stack, proved or not, and where it was lost; the reads of
     the fixed kilobyte, counted only. */
  if(stack_proven)
    printf("z80c: direct rd=%lu/%lu wr=%lu/%lu stack=proven rom_fixed=%lu\n",
           n_rd_direct,n_rd_all,n_wr_direct,n_wr_all,n_rom_fixed);
  else
    printf("z80c: direct rd=%lu/%lu wr=%lu/%lu stack=full (%s at %06lx) rom_fixed=%lu\n",
           n_rd_direct,n_rd_all,n_wr_direct,n_wr_all,stack_full_form,stack_full_pos,n_rom_fixed);
  if(counts_path != NULL)
    {
      unsigned long run = counts_insns;
      unsigned long p10 = (run != 0UL) ? (unsigned long)(((double)sel_weight * 1000.0) / (double)run) : 0UL;
      unsigned long a10 = (run != 0UL) ? (unsigned long)(((double)all_weight * 1000.0) / (double)run) : 0UL;

      printf("z80c: selected blocks=%lu/%lu bytes=%lu insns=%lu.%lu%% of the recorded run "
             "(every block: %lu.%lu%%, %lu frames)\n",
             n_blocks,n_all,sel_bytes,p10 / 10UL,p10 % 10UL,a10 / 10UL,a10 % 10UL,counts_frames);
    }
  fprintf(stderr,
          "z80c: starts=%lu reached=%lu ends: next=%lu jump=%lu fallback=%lu "
          "cut=%lu bank=%lu repeat=%lu bankend=%lu\n",
          n_starts,n_reached,n_end_next,n_end_jump,n_end_fallback,n_end_cut,
          n_end_bank,n_end_repeat,n_end_bankend);
  free(blocks);
  free(blk);
  return 0;
}
