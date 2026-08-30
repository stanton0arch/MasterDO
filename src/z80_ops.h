#ifndef SMS3DO_Z80_OPS_H
#define SMS3DO_Z80_OPS_H

#include "sms.h"

/*
 * Access path of the Z80 core: emulated memory and processor registers, as
 * macros.
 *
 * This is a private header of the core, still, though not for the reason it
 * first was. The two tables it names, z80_rmap and z80_wmap, are published
 * objects now, declared in z80.h with the rule that goes with them: one
 * writer only, the cartridge module -- its boot installation, then its
 * mapper -- and the processor reading them ever after. What keeps this header out of every
 * other module is the rest of it: it names sms.z80, the processor's field
 * of the world structure (sms.h) -- a global, readable from any module,
 * z80.c being its only writer by declared rule of sms.h rather than by
 * constraint of the compiler, the price of letting other modules read the
 * processor's state without a pointer in between -- and two static
 * functions of z80.c, the ones that carry a byte to and from a port: they
 * are what the input and output instructions expand into, and they are not
 * state.
 *
 * Nothing here is a function, and that is a budget decision. The processor
 * budget is 3.49 ARM cycles per emulated T-state; an uncached call costs 29 to
 * 39 of them, which is 8 to 11 T-states spent before the call has done
 * anything, on a path a LD A,(HL) walks in 7. So an access expands to an
 * indexation and a dereference, with no test and no call anywhere on it.
 *
 * Two opposite contracts live here, and they are worth telling apart before
 * either is used.
 *
 * The access macros -- the reads and writes of emulated memory, the register
 * names, the composition of a pair -- evaluate their arguments more than once.
 * Pass them a plain address and a plain value, never an expression with a side
 * effect and never an increment.
 *
 * Every Z80_OP_ macro is the opposite: it evaluates each argument exactly
 * once, into a local, and the dispatch relies on it. Seventeen of its cases
 * hand an operand fetched straight out of emulated memory, so a second use of
 * an argument added to one of those bodies would quietly turn seventeen
 * instructions into two reads apiece, on a path where the second read need not
 * even return what the first one did once a mapper exists.
 *
 * Every Z80_OP_ macro is a statement and never an expression, so that it can be
 * written under an unbraced branch without changing what that branch covers.
 *
 * There is no dispatch in this file. Which opcode reaches which operation is
 * decided by the switch in z80.c; what lives here is the body an operation runs
 * once it has been reached, and the price its taken case adds.
 */

/*
 * ---------------------------------------------------------------------------
 * Emulated memory, eight bits.
 *
 * Semantics: TotalSMS/src/core/sms_bus.c:827-838. One shift to pick the page,
 * one mask to pick the offset in it, one dereference. The two tables hold raw
 * host pointers, so nothing here knows or cares whether the page it lands on
 * is cartridge ROM, work RAM or one of its mirrors, or a fixed page -- the
 * table entry is the whole of what an address means.
 *
 * Z80_WR8 CARRIES THE MAPPER TRIGGER, and its callers are written knowing
 * it. A write to a mapper register does two things at once on this
 * machine: the byte lands through the table -- in work RAM for the Sega
 * mapper's $FFFC-$FFFF, readable back at $DFFC-$DFFF -- AND a mapper bank
 * turns (docs/sms_gg/sms_gg_hardware_notes.md:72-78). Both effects
 * ride this one macro, in that order: the store through the table first,
 * then one compare on the address,
 * CART_MAPPER_TRIGGER (cart.h), and on a register address only, one cold
 * call into cart_mapper_write (cart.h), which repoints the tables. Which
 * addresses are registers is the bus's to say, through that macro, and
 * this header knows nothing else of the mapper: the build's mapper
 * constant of common.h chooses the compare's form in cart.h, and a build
 * serving no mapper defines no trigger at all, in which case the write
 * below is the plain store through the table. The compare is the whole
 * price the hot path pays; a program pays the call a few times per frame
 * at most. Anything that stored into emulated memory behind this macro's
 * back would turn banks silently on some writes and not on others: there
 * is no second write path, and Z80_WR16 below is two of these, so a word
 * written at $FFFE turns two banks under the Sega mapper.
 *
 * The trigger works from the macro's arguments and the mapper's own state,
 * never through sms.z80: it can be expanded inside z80_run's window, where
 * the five hot fields -- pc, tstates, r, a, f -- are stale.
 *
 * THE TRIGGER HAS ONE HOME, this macro, and THE FORMS BELOW ARE THE ONLY
 * HOME OF THE TWO ACCESSES, inside z80_run's resident window as
 * everywhere else -- unlike the five hot register names, which that window
 * retargets. The entries point at ROM beside a write page that absorbs, at
 * a fixed page, and at 8k of work RAM seen twice ($C000-$DFFF reflected at
 * $E000-$FFFF, docs/sms_gg/sms_gg_hardware_notes.md:60, :72-78 -- each
 * mirror exactly a repointed entry, installed by cart.c at boot), so a
 * flat local window would have to carry every one of those aliases into a
 * copy, and an absorbed ROM write has nowhere to be carried at all. The
 * table walk stays in force inside z80_run too, cost assumed, and z80.c
 * says so in place.
 * ---------------------------------------------------------------------------
 */
#define Z80_RD8(addr)                                       \
  (z80_rmap[(uint16)(addr) >> Z80_PAGE_BITS]                \
           [(uint16)(addr) & Z80_PAGE_MASK])

#ifndef SMS3DO_CART_H
#error "cart.h must precede z80_ops.h: it decides the mapper trigger"
#endif

#ifndef CART_MAPPER_TRIGGER
#define Z80_WR8(addr, value)                                        \
  do                                                                \
    {                                                               \
      uint16 z80_wr8_addr = (uint16)(addr);                         \
                                                                    \
      z80_wmap[z80_wr8_addr >> Z80_PAGE_BITS]                       \
              [z80_wr8_addr & Z80_PAGE_MASK] = (uint8)(value);      \
    }                                                               \
  while(0)
#else
#define Z80_WR8(addr, value)                                        \
  do                                                                \
    {                                                               \
      uint16 z80_wr8_addr = (uint16)(addr);                         \
      uint8  z80_wr8_val  = (uint8)(value);                         \
                                                                    \
      z80_wmap[z80_wr8_addr >> Z80_PAGE_BITS]                       \
              [z80_wr8_addr & Z80_PAGE_MASK] = z80_wr8_val;         \
      if(CART_MAPPER_TRIGGER(z80_wr8_addr))                         \
        cart_mapper_write(z80_wr8_addr,z80_wr8_val);                \
    }                                                               \
  while(0)
#endif

/*
 * ---------------------------------------------------------------------------
 * Emulated memory, sixteen bits. Low byte at the lower address, which is what
 * the emulated processor stores whatever the host does.
 *
 * Built out of two eight bit accesses through the tables, and not out of
 * read16_le / write16_le applied to a page pointer. That second form is one
 * indexation cheaper and it is wrong here, for three reasons that all outlive
 * this stage:
 *
 *   The address wraps at 0xFFFF. The emulated processor reads the second byte
 *   of a word at 0xFFFF from 0x0000; a host pointer walked one byte further
 *   reads whatever follows the block instead. Composing the address before
 *   each access makes the wrap free and exact.
 *
 *   Two adjacent pages need not be adjacent in host memory: a mapper points
 *   them at different banks -- at which point a word straddling
 *   the boundary read through one pointer would take its second byte from the
 *   wrong bank, on data that looks plausible.
 *
 *   A sixteen bit write must be two writes, or the mapper trigger described
 *   above fires once instead of twice. TotalSMS reaches the same conclusion on
 *   a big endian host, where its own write16 is two calls to its byte write
 *   (sms_bus.c:852-855) rather than the memcpy it uses elsewhere.
 *
 * read16_le and write16_le of common.h keep the job they were written for:
 * sixteen bit fields of data that genuinely is contiguous in host memory --
 * ROM images, colour entries, structures read out of a file. The page tables
 * are the one place where contiguity is not a given.
 * ---------------------------------------------------------------------------
 */
#define Z80_RD16(addr)                                              \
  ((uint16)((uint16)Z80_RD8(addr) |                                 \
            ((uint16)Z80_RD8((uint16)((addr) + 1)) << 8)))

#define Z80_WR16(addr, value)                                       \
  do                                                                \
    {                                                               \
      uint16 z80_wr16_addr = (uint16)(addr);                        \
      uint16 z80_wr16_val = (uint16)(value);                        \
                                                                    \
      Z80_WR8(z80_wr16_addr,z80_wr16_val & 0xFFU);                  \
      Z80_WR8((uint16)(z80_wr16_addr + 1),z80_wr16_val >> 8);       \
    }                                                               \
  while(0)

/*
 * ---------------------------------------------------------------------------
 * Registers.
 *
 * Names, not accessors: each of these expands to the field itself, so it reads
 * on the right of an assignment and is written on the left. The gain is that
 * an instruction body says Z80_A rather than sms.z80.main.a, which is the
 * difference between code that can be compared to a Z80 listing and code that
 * cannot.
 *
 * Naming and split follow TotalSMS/src/core/sms_z80.c:106-131.
 *
 * FIVE OF THESE NAMES HAVE TWO HOMES, and the second home is the point. PC,
 * the T-state counter, R, A and F are read or written on every instruction,
 * or on nearly every one -- and a field of the global structure can never
 * stay in a machine register across an emulated memory write: a store through
 * z80_wmap could, as far as the compiler can prove, land on sms.z80 itself,
 * so every such store forces the fields back to memory and reloads them
 * after. A local variable whose address is never taken cannot be aliased by
 * any store, and so can be held in a register across the whole body of the
 * loop.
 *
 * Hence the pair of names. Z80_x_STATE is the field of the structure, always.
 * Z80_x defaults to it, and z80.c retargets the five hot names onto locals of
 * z80_run for the extent of that function's body, restoring them afterwards;
 * the _STATE names are what the synchronisation at z80_run's boundaries reads
 * and writes. Everything else -- every operation, every cold function -- keeps
 * saying Z80_x and never needs to know which home is live where it stands.
 *
 * The restore block after z80_run re-states these five default definitions
 * verbatim: a change to any of them is a change in two places, here and
 * there, and the two must be kept in agreement by hand.
 * ---------------------------------------------------------------------------
 */
#define Z80_A_STATE sms.z80.main.a
#define Z80_F_STATE sms.z80.main.f

#define Z80_B sms.z80.main.b
#define Z80_C sms.z80.main.c
#define Z80_D sms.z80.main.d
#define Z80_E sms.z80.main.e
#define Z80_H sms.z80.main.h
#define Z80_L sms.z80.main.l
#define Z80_A Z80_A_STATE
#define Z80_F Z80_F_STATE

#define Z80_B_ALT sms.z80.alt.b
#define Z80_C_ALT sms.z80.alt.c
#define Z80_D_ALT sms.z80.alt.d
#define Z80_E_ALT sms.z80.alt.e
#define Z80_H_ALT sms.z80.alt.h
#define Z80_L_ALT sms.z80.alt.l
#define Z80_A_ALT sms.z80.alt.a
#define Z80_F_ALT sms.z80.alt.f

#define Z80_PC_STATE sms.z80.pc

#define Z80_PC Z80_PC_STATE
#define Z80_SP sms.z80.sp

#define Z80_IXH sms.z80.ixh
#define Z80_IXL sms.z80.ixl
#define Z80_IYH sms.z80.iyh
#define Z80_IYL sms.z80.iyl

#define Z80_R_STATE       sms.z80.r
#define Z80_TSTATES_STATE sms.z80.tstates

#define Z80_I        sms.z80.i
#define Z80_R        Z80_R_STATE
#define Z80_IFF1     sms.z80.iff1
#define Z80_IFF2     sms.z80.iff2
#define Z80_EI_DELAY sms.z80.ei_delay
#define Z80_HALTED   sms.z80.halted
#define Z80_NMI_SEEN sms.z80.nmi_seen
#define Z80_IM       sms.z80.im
#define Z80_TSTATES  Z80_TSTATES_STATE

/*
 * Sixteen bit views of the eight bit pairs. The pair is a composition and the
 * halves are the storage, which is the way round the instruction set asks for:
 * the prefixed instructions address IX and IY by half as readily as by pair,
 * and holding pairs would turn every half into a masked read and a masked
 * write.
 * Composition: TotalSMS/src/core/sms_z80.c:184-185.
 *
 * These have nothing to do with host endianness: they compose a value out of
 * two bytes the code already holds separately, they never reinterpret memory.
 */
#define Z80_PAIR(hi, lo) ((uint16)(((uint16)(hi) << 8) | (uint16)(lo)))

#define Z80_SET_PAIR(hi, lo, value)                     \
  do                                                    \
    {                                                   \
      uint16 z80_pair_val = (uint16)(value);            \
                                                        \
      (hi) = (uint8)(z80_pair_val >> 8);                \
      (lo) = (uint8)(z80_pair_val & 0xFFU);             \
    }                                                   \
  while(0)

#define Z80_AF Z80_PAIR(Z80_A,Z80_F)
#define Z80_BC Z80_PAIR(Z80_B,Z80_C)
#define Z80_DE Z80_PAIR(Z80_D,Z80_E)
#define Z80_HL Z80_PAIR(Z80_H,Z80_L)
#define Z80_IX Z80_PAIR(Z80_IXH,Z80_IXL)
#define Z80_IY Z80_PAIR(Z80_IYH,Z80_IYL)

#define Z80_SET_AF(v) Z80_SET_PAIR(Z80_A,Z80_F,(v))
#define Z80_SET_BC(v) Z80_SET_PAIR(Z80_B,Z80_C,(v))
#define Z80_SET_DE(v) Z80_SET_PAIR(Z80_D,Z80_E,(v))
#define Z80_SET_HL(v) Z80_SET_PAIR(Z80_H,Z80_L,(v))
#define Z80_SET_IX(v) Z80_SET_PAIR(Z80_IXH,Z80_IXL,(v))
#define Z80_SET_IY(v) Z80_SET_PAIR(Z80_IYH,Z80_IYL,(v))

/*
 * Flags, read and written in the byte that holds them.
 *
 * A flag is a bit of F rather than a variable of its own, which is what makes
 * PUSH AF and POP AF a move of one byte instead of a composition out of eight
 * places. The instruction test program compares expected flags to obtained
 * ones by pushing and popping AF, so that is the path walked most, and it is
 * the one kept free here.
 *
 * Z80_FLAG_SET takes any non zero value as true, so a condition can be handed
 * to it directly.
 */
#define Z80_FLAG_GET(mask) ((Z80_F & (uint8)(mask)) != 0)

#define Z80_FLAG_SET(mask, cond)                                \
  do                                                            \
    {                                                           \
      if(cond)                                                  \
        Z80_F |= (uint8)(mask);                                 \
      else                                                      \
        Z80_F &= (uint8)~(uint8)(mask);                         \
    }                                                           \
  while(0)

/*
 * ---------------------------------------------------------------------------
 * Composing F in one go.
 *
 * An operation that set its flags one at a time through Z80_FLAG_SET would pay
 * a test and a read-modify-write per flag, six to eight times per arithmetic
 * instruction, on the path that carries the whole emulation. So the operations
 * below build the new F in a local and store it once.
 *
 * Two of the masks fall out of the bit layout rather than being arranged, and
 * they are worth naming because they turn three flags into one AND. The sign is
 * bit 7 of F and bit 7 of the result; the two undocumented flags are bits 3 and
 * 5 of F and bits 3 and 5 of the result. So S, B5 and B3 together are the
 * result masked by 0xA8, with nothing to shift.
 * Bit assignment: TotalSMS/src/core/sms_z80.c:93-104.
 * ---------------------------------------------------------------------------
 */
#define Z80_SZ53_MASK (Z80_FLAG_S | Z80_FLAG_B5 | Z80_FLAG_B3)
#define Z80_53_MASK   (Z80_FLAG_B5 | Z80_FLAG_B3)

/*
 * Even parity of a byte, as the PV bit or zero.
 *
 * Four register operations and no memory at all, which is why it is this and
 * not a table of 256 bytes. The ARM60 has no cache
 * (docs/3do/3DO_Development_Notes.md:26): a table lookup here would be a full
 * price memory access on a path walked by every logical operation, where the
 * fold below is a shift, an exclusive or, a mask and a shift.
 * Fold: TotalSMS/src/core/sms.c:700-705, the branch taken when the compiler
 * offers no parity builtin -- which the C89 compiler of this target does not.
 *
 * The magic word is a truth table read by the nibble: bit n of 0x6996 is the
 * odd parity of n, so a clear bit means even parity, which is the case that
 * sets the flag.
 */
#define Z80_PARITY_PV(v)                                                \
  ((((0x6996UL >> (((uint8)(v) ^ ((uint8)(v) >> 4)) & 0x0FU)) & 1UL)    \
    != 0UL) ? 0U : (uint8)Z80_FLAG_PV)

/*
 * ---------------------------------------------------------------------------
 * Spending time, and reading an operand out of the instruction stream.
 *
 * The counter runs down rather than up: it is loaded with the quota of the
 * scanline and each instruction subtracts what it costs, so the loop condition
 * is a comparison against zero and the overrun of the last instruction is
 * already sitting in the counter, negative, when the loop ends.
 *
 * The base price of a form reaches this macro as an immediate: each dispatch
 * case in z80.c spends its own cost as a literal transcribed from the cost
 * tables there. The tables keep exactly two kinds of reader: the stop paths
 * of the dispatch defaults, cold, and the memory-operand case of the CB
 * prefix, warm -- the one place a cost cannot travel as a single literal,
 * its price depending on an operation field not yet decoded. The surcharges
 * of taken branches and repeating blocks stay in the operations below, on
 * top of that base.
 * ---------------------------------------------------------------------------
 */
#define Z80_SPEND(n) (Z80_TSTATES -= (int32)(n))

#define Z80_FETCH8(dst)                                 \
  do                                                    \
    {                                                   \
      (dst) = Z80_RD8(Z80_PC);                          \
      Z80_PC = (uint16)(Z80_PC + 1);                    \
    }                                                   \
  while(0)

#define Z80_FETCH16(dst)                                \
  do                                                    \
    {                                                   \
      (dst) = Z80_RD16(Z80_PC);                         \
      Z80_PC = (uint16)(Z80_PC + 2);                    \
    }                                                   \
  while(0)

/*
 * ---------------------------------------------------------------------------
 * Eight bit arithmetic.
 * Semantics: TotalSMS/src/core/sms_z80.c:352-445 for the six operations,
 * :342-345 for the overflow rule.
 *
 * Addition and subtraction take the carry in as an argument rather than having
 * a variant of their own, and that is a correction and not a simplification.
 * The source composes ADC as an ADD of value plus carry
 * (sms_z80.c:447-450), which loses two flags whenever that sum reaches 0x100 --
 * ADC A,0xFF with the carry set produces a half carry and the composed form
 * reports none, because 0x100 has an empty low nibble. Carrying the bit
 * separately all the way into the nibble arithmetic is what keeps the flag
 * right, and it costs one addend.
 * ---------------------------------------------------------------------------
 */
#define Z80_OP_ADD(value, cin)                                          \
  do                                                                    \
    {                                                                   \
      uint8 z80_a = Z80_A;                                              \
      uint8 z80_v = (uint8)(value);                                     \
      uint32 z80_k = (uint32)(cin);                                     \
      uint32 z80_s = (uint32)z80_a + (uint32)z80_v + z80_k;             \
      uint8 z80_r = (uint8)z80_s;                                       \
      uint8 z80_f = (uint8)(z80_r & Z80_SZ53_MASK);                     \
                                                                        \
      if(z80_r == 0)                                                    \
        z80_f |= (uint8)Z80_FLAG_Z;                                     \
      if(z80_s > 0xFFUL)                                                \
        z80_f |= (uint8)Z80_FLAG_C;                                     \
      if(((((uint32)z80_a & 0x0FUL) + ((uint32)z80_v & 0x0FUL) + z80_k) \
          & 0x10UL) != 0UL)                                             \
        z80_f |= (uint8)Z80_FLAG_H;                                     \
      if(((((uint32)z80_a ^ (uint32)z80_r) &                            \
           ((uint32)z80_v ^ (uint32)z80_r)) & 0x80UL) != 0UL)           \
        z80_f |= (uint8)Z80_FLAG_PV;                                    \
                                                                        \
      Z80_F = z80_f;                                                    \
      Z80_A = z80_r;                                                    \
    }                                                                   \
  while(0)

/*
 * Subtraction, and the comparison that is the same subtraction without its
 * result. The last argument says where the two undocumented flags are copied
 * from, and it is the whole difference between the two: SUB takes them off the
 * result, CP off the operand it was handed (sms_z80.c:376-377 against
 * :440-441). Everything else, the sign included, is read off the result in both.
 *
 * The overflow rule is stated directly on the subtraction rather than through
 * the source's trick of adding the two's complement (sms_z80.c:374): operands
 * of unlike sign, result unlike the accumulator.
 */
#define Z80_SUB_FLAGS(a, v, k, r, u)                                    \
  do                                                                    \
    {                                                                   \
      uint8 z80_sf = (uint8)(((r) & (uint8)Z80_FLAG_S) |                \
                             ((u) & (uint8)Z80_53_MASK) |               \
                             (uint8)Z80_FLAG_N);                        \
                                                                        \
      if((r) == 0)                                                      \
        z80_sf |= (uint8)Z80_FLAG_Z;                                    \
      if((uint32)(a) < ((uint32)(v) + (uint32)(k)))                     \
        z80_sf |= (uint8)Z80_FLAG_C;                                    \
      if(((uint32)(a) & 0x0FUL) <                                       \
         (((uint32)(v) & 0x0FUL) + (uint32)(k)))                        \
        z80_sf |= (uint8)Z80_FLAG_H;                                    \
      if(((((uint32)(a) ^ (uint32)(v)) &                                \
           ((uint32)(a) ^ (uint32)(r))) & 0x80UL) != 0UL)               \
        z80_sf |= (uint8)Z80_FLAG_PV;                                   \
                                                                        \
      Z80_F = z80_sf;                                                   \
    }                                                                   \
  while(0)

#define Z80_OP_SUB(value, bin)                                          \
  do                                                                    \
    {                                                                   \
      uint8 z80_a = Z80_A;                                              \
      uint8 z80_v = (uint8)(value);                                     \
      uint8 z80_k = (uint8)(bin);                                       \
      uint8 z80_r = (uint8)(z80_a - z80_v - z80_k);                     \
                                                                        \
      Z80_SUB_FLAGS(z80_a,z80_v,z80_k,z80_r,z80_r);                     \
      Z80_A = z80_r;                                                    \
    }                                                                   \
  while(0)

#define Z80_OP_CP(value)                                                \
  do                                                                    \
    {                                                                   \
      uint8 z80_a = Z80_A;                                              \
      uint8 z80_v = (uint8)(value);                                     \
      uint8 z80_r = (uint8)(z80_a - z80_v);                             \
                                                                        \
      Z80_SUB_FLAGS(z80_a,z80_v,0U,z80_r,z80_v);                        \
    }                                                                   \
  while(0)

/*
 * The three logical operations. They differ by their operator and by the half
 * carry alone, which AND leaves set and the other two clear
 * (sms_z80.c:384-431); all three clear the carry and the subtraction flag and
 * report parity in the shared bit.
 */
#define Z80_OP_LOGIC(result, hflag)                             \
  do                                                            \
    {                                                           \
      uint8 z80_r = (uint8)(result);                            \
      uint8 z80_f = (uint8)((z80_r & Z80_SZ53_MASK) |           \
                            (uint8)(hflag) |                    \
                            Z80_PARITY_PV(z80_r));              \
                                                                \
      if(z80_r == 0)                                            \
        z80_f |= (uint8)Z80_FLAG_Z;                             \
                                                                \
      Z80_F = z80_f;                                            \
      Z80_A = z80_r;                                            \
    }                                                           \
  while(0)

#define Z80_OP_AND(value) Z80_OP_LOGIC(Z80_A & (uint8)(value),Z80_FLAG_H)
#define Z80_OP_XOR(value) Z80_OP_LOGIC(Z80_A ^ (uint8)(value),0U)
#define Z80_OP_OR(value)  Z80_OP_LOGIC(Z80_A | (uint8)(value),0U)

/*
 * ---------------------------------------------------------------------------
 * Increment and decrement, eight bits.
 * Semantics: TotalSMS/src/core/sms_z80.c:670-699.
 *
 * The carry is not touched, and that is as much a part of the instruction as
 * the flags it does write: a loop that increments a counter between an addition
 * and the branch that reads its carry depends on it. So F is rebuilt from the
 * carry it already holds rather than from nothing.
 *
 * Overflow is a single equality because there is only one value that overflows:
 * 0x7F going up, 0x80 going down. Half carry likewise -- the low nibble has to
 * be full going up, empty going down.
 * ---------------------------------------------------------------------------
 */
#define Z80_INC_FLAGS(v, r)                                             \
  do                                                                    \
    {                                                                   \
      uint8 z80_if = (uint8)((Z80_F & (uint8)Z80_FLAG_C) |              \
                             ((r) & Z80_SZ53_MASK));                    \
                                                                        \
      if((r) == 0)                                                      \
        z80_if |= (uint8)Z80_FLAG_Z;                                    \
      if((v) == 0x7FU)                                                  \
        z80_if |= (uint8)Z80_FLAG_PV;                                   \
      if(((v) & 0x0FU) == 0x0FU)                                        \
        z80_if |= (uint8)Z80_FLAG_H;                                    \
                                                                        \
      Z80_F = z80_if;                                                   \
    }                                                                   \
  while(0)

#define Z80_DEC_FLAGS(v, r)                                             \
  do                                                                    \
    {                                                                   \
      uint8 z80_df = (uint8)((Z80_F & (uint8)Z80_FLAG_C) |              \
                             ((r) & Z80_SZ53_MASK) |                    \
                             (uint8)Z80_FLAG_N);                        \
                                                                        \
      if((r) == 0)                                                      \
        z80_df |= (uint8)Z80_FLAG_Z;                                    \
      if((v) == 0x80U)                                                  \
        z80_df |= (uint8)Z80_FLAG_PV;                                   \
      if(((v) & 0x0FU) == 0x00U)                                        \
        z80_df |= (uint8)Z80_FLAG_H;                                    \
                                                                        \
      Z80_F = z80_df;                                                   \
    }                                                                   \
  while(0)

#define Z80_OP_INC_R(reg)                               \
  do                                                    \
    {                                                   \
      uint8 z80_v = (uint8)(reg);                       \
      uint8 z80_r = (uint8)(z80_v + 1U);                \
                                                        \
      (reg) = z80_r;                                    \
      Z80_INC_FLAGS(z80_v,z80_r);                       \
    }                                                   \
  while(0)

#define Z80_OP_DEC_R(reg)                               \
  do                                                    \
    {                                                   \
      uint8 z80_v = (uint8)(reg);                       \
      uint8 z80_r = (uint8)(z80_v - 1U);                \
                                                        \
      (reg) = z80_r;                                    \
      Z80_DEC_FLAGS(z80_v,z80_r);                       \
    }                                                   \
  while(0)

/*
 * The same two on a byte of memory. The address is composed once by the caller
 * and held here, because the read and the write must land on the same place even
 * once a mapper can move a page under them.
 *
 * The address is an argument rather than HL by name: the indexed forms reach the
 * same operation at a displaced address, and one body serving both is one body
 * to get right. Pass a plain local, never an expression that moves PC -- the
 * address is read twice below.
 */
#define Z80_OP_INC_MEM_AT(addr)                         \
  do                                                    \
    {                                                   \
      uint16 z80_ad = (uint16)(addr);                   \
      uint8 z80_v = Z80_RD8(z80_ad);                    \
      uint8 z80_r = (uint8)(z80_v + 1U);                \
                                                        \
      Z80_WR8(z80_ad,z80_r);                            \
      Z80_INC_FLAGS(z80_v,z80_r);                       \
    }                                                   \
  while(0)

#define Z80_OP_DEC_MEM_AT(addr)                         \
  do                                                    \
    {                                                   \
      uint16 z80_ad = (uint16)(addr);                   \
      uint8 z80_v = Z80_RD8(z80_ad);                    \
      uint8 z80_r = (uint8)(z80_v - 1U);                \
                                                        \
      Z80_WR8(z80_ad,z80_r);                            \
      Z80_DEC_FLAGS(z80_v,z80_r);                       \
    }                                                   \
  while(0)

#define Z80_OP_INC_MEM() Z80_OP_INC_MEM_AT(Z80_HL)
#define Z80_OP_DEC_MEM() Z80_OP_DEC_MEM_AT(Z80_HL)

/*
 * ---------------------------------------------------------------------------
 * Sixteen bit arithmetic on the pairs.
 *
 * The increment and the decrement of a pair touch no flag at all, which is what
 * makes them usable between an operation and the branch that reads its result
 * (sms_z80.c:701-712).
 *
 * ADD HL,rr is the opposite trap: it writes the carry, the half carry, the
 * subtraction flag and the two undocumented bits, and it leaves sign, zero and
 * overflow exactly as it found them (sms_z80.c:604-617). A flag written here
 * that should not have been shows up several instructions later, in a branch,
 * which is the most expensive kind of defect this core can have.
 *
 * The half carry of a sixteen bit addition comes out of bit 11, not bit 3: it
 * is the carry into the high byte's low nibble. The undocumented bits are read
 * off the high byte of the result for the same reason.
 *
 * The destination is a pair of halves rather than HL by name, because the index
 * registers take the very same addition and there is no second version of it to
 * be had: one body, and the pair it lands in is an argument. ADD HL,rr is that
 * body applied to H and L, and it keeps its own name so that the dispatch of the
 * unprefixed set still reads like a listing.
 *
 * The two halves follow the convention of the register names rather than that of
 * the operations: EACH IS READ MORE THAN ONCE, to compose the pair and again to
 * store the result. Pass a plain lvalue -- a register name, or a half reached
 * through a pointer -- never an expression with a side effect.
 * ---------------------------------------------------------------------------
 */
#define Z80_OP_ADD16(hi, lo, value)                                     \
  do                                                                    \
    {                                                                   \
      uint32 z80_p = (uint32)Z80_PAIR(hi,lo);                           \
      uint32 z80_v = (uint32)(value);                                   \
      uint32 z80_s = z80_p + z80_v;                                     \
      uint8 z80_f = (uint8)(Z80_F & (uint8)(Z80_FLAG_S | Z80_FLAG_Z |   \
                                            Z80_FLAG_PV));              \
                                                                        \
      if(z80_s > 0xFFFFUL)                                              \
        z80_f |= (uint8)Z80_FLAG_C;                                     \
      if((((z80_p & 0x0FFFUL) + (z80_v & 0x0FFFUL)) & 0x1000UL) !=      \
         0UL)                                                           \
        z80_f |= (uint8)Z80_FLAG_H;                                     \
      z80_f |= (uint8)(((z80_s >> 8) & 0xFFUL) & Z80_53_MASK);          \
                                                                        \
      Z80_F = z80_f;                                                    \
      Z80_SET_PAIR(hi,lo,(uint16)z80_s);                                \
    }                                                                   \
  while(0)

#define Z80_OP_ADD_HL(value) Z80_OP_ADD16(Z80_H,Z80_L,(value))

/*
 * Incrementing and decrementing a pair, written as the low half and a carry into
 * the high one (sms_z80.c:701-712, :1607-1608). Not a flourish: the index
 * registers are stored as halves, and recomposing a sixteen bit value only to
 * take it apart again would give back exactly what that storage was chosen to
 * avoid. Not a flag is touched by either.
 *
 * The carry condition is read off the half AFTER the step, which is what makes
 * it one comparison: going up, only 0xFF wraps and it wraps to zero; going down,
 * only zero wraps and it wraps to 0xFF.
 *
 * Each half is read more than once here too, the low one three times. Plain
 * lvalues only, for the reason given above.
 */
#define Z80_OP_INC_PAIR(hi, lo)                         \
  do                                                    \
    {                                                   \
      (lo) = (uint8)((lo) + 1U);                        \
      if((lo) == 0x00U)                                 \
        (hi) = (uint8)((hi) + 1U);                      \
    }                                                   \
  while(0)

#define Z80_OP_DEC_PAIR(hi, lo)                         \
  do                                                    \
    {                                                   \
      (lo) = (uint8)((lo) - 1U);                        \
      if((lo) == 0xFFU)                                 \
        (hi) = (uint8)((hi) - 1U);                      \
    }                                                   \
  while(0)

/*
 * ---------------------------------------------------------------------------
 * The four accumulator rotations.
 * Semantics: TotalSMS/src/core/sms_z80.c:925-967.
 *
 * They are not the rotations of the CB prefix applied to A: they leave sign,
 * zero and parity untouched, which the prefixed ones set. The source says so by
 * saving those three around a call to the shared body; here the three are
 * simply carried over from the F that already holds them.
 *
 * carry_in is the bit shifted into the vacated end, cout the bit shifted out.
 * ---------------------------------------------------------------------------
 */
#define Z80_ROT_A(result, cout)                                         \
  do                                                                    \
    {                                                                   \
      uint8 z80_r = (uint8)(result);                                    \
      uint8 z80_f = (uint8)(Z80_F & (uint8)(Z80_FLAG_S | Z80_FLAG_Z |   \
                                            Z80_FLAG_PV));              \
                                                                        \
      z80_f |= (uint8)(z80_r & Z80_53_MASK);                            \
      if((cout) != 0U)                                                  \
        z80_f |= (uint8)Z80_FLAG_C;                                     \
                                                                        \
      Z80_F = z80_f;                                                    \
      Z80_A = z80_r;                                                    \
    }                                                                   \
  while(0)

#define Z80_OP_RLCA()                                                   \
  Z80_ROT_A((uint8)((Z80_A << 1) | (Z80_A >> 7)),(uint8)(Z80_A & 0x80U))

#define Z80_OP_RRCA()                                                   \
  Z80_ROT_A((uint8)((Z80_A >> 1) | (Z80_A << 7)),(uint8)(Z80_A & 0x01U))

#define Z80_OP_RLA()                                                    \
  Z80_ROT_A((uint8)((Z80_A << 1) |                                      \
                    (uint8)Z80_FLAG_GET(Z80_FLAG_C)),                   \
            (uint8)(Z80_A & 0x80U))

#define Z80_OP_RRA()                                                    \
  Z80_ROT_A((uint8)((Z80_A >> 1) |                                      \
                    (uint8)(Z80_FLAG_GET(Z80_FLAG_C) ? 0x80U : 0U)),    \
            (uint8)(Z80_A & 0x01U))

/*
 * ---------------------------------------------------------------------------
 * The four operations on the accumulator and the flag byte alone.
 * Semantics: TotalSMS/src/core/sms_z80.c:1381-1405.
 *
 * All four copy the two undocumented bits off the accumulator rather than off
 * any result, which is the one thing they have in common.
 *
 * The decimal adjustment departs from the source twice, and both departures are
 * forced by the exhaustive instruction test rather than chosen: the source
 * fails that test, and says so itself in a note left at sms_z80.c:1373.
 *
 * The adjustment is ONE correction, computed the same way whatever the
 * direction, and then added or subtracted according to the subtraction flag:
 *
 *   0x06 enters the correction when the half carry is set or the low nibble
 *        exceeds 9
 *   0x60 enters the correction when the carry is set or the accumulator
 *        exceeds 0x99, and the carry comes out set
 *
 * The manufacturer's table, transcribed at docs/sms_gg/daa.md:24-37, is
 * reproduced row for row by that rule -- nine rows of addition and four of
 * subtraction, value and carry alike. What the table cannot give is the rest:
 * it lists the states a real addition or subtraction can leave behind, and a
 * flag byte loaded off the stack reaches states no arithmetic ever produces.
 * A half carry set with a low nibble under 6, an accumulator above 0x99 with
 * the carry clear after a subtraction -- the table has no row for either, and
 * the test exercises both. The rule above answers them; the table's four
 * subtraction rows, read as an enumeration rather than as a projection of the
 * rule, do not. The source reads them as an enumeration (sms_z80.c:1350-1358):
 * it lets the flags alone decide the subtraction correction, and leaves the
 * carry untouched. That is right on every reachable state and wrong on 2832 of
 * the 16384 the test covers.
 *
 * The half carry is the second departure. The source leaves it exactly as it
 * found it and says, in place, that it does not know how the flag is meant to
 * be set (sms_z80.c:1373). The manufacturer's own instruction sheet, at
 * docs/sms_gg/daa.md:53-67, lists the half carry among the bits this
 * instruction affects -- it writes "not affected" two lines below, for the
 * subtraction flag, so the omission is not a shorthand -- and then points at a
 * table that has a column for the half carry going in and none for it coming
 * out. The sheet promises the value and does not give it, which is very likely
 * the dead end the source walked into.
 *
 * What the sheet does give is the rule, in its flag chapter: the half carry is
 * the carry out of bit 3 in an addition, the borrow into bit 4 in a
 * subtraction (docs/sms_gg/daa.md:89-102). Applying it to the correction
 * settles the flag, because the two large corrections have an empty low nibble
 * and can carry nothing:
 *
 *   after an addition     set when the low nibble held A to F beforehand
 *   after a subtraction   set when the half carry was already set and the low
 *                         nibble held less than 6
 *
 * Three ways of writing that rule -- the two lines above, a comparison of bit 4
 * before and after, and the carry computed out of the nibble arithmetic itself
 * -- agree on all 16384 cases, which is what one expects of a flag that is
 * simply the half carry of the adjustment.
 * ---------------------------------------------------------------------------
 */
#define Z80_OP_DAA()                                                    \
  do                                                                    \
    {                                                                   \
      uint8 z80_f = Z80_F;                                              \
      uint8 z80_a = Z80_A;                                              \
      uint8 z80_lo = (uint8)(z80_a & 0x0FU);                            \
      uint8 z80_hf = 0;                                                 \
      uint8 z80_adj = 0;                                                \
                                                                        \
      if(((z80_f & (uint8)Z80_FLAG_H) != 0U) || (z80_lo > 0x09U))       \
        z80_adj = (uint8)(z80_adj | 0x06U);                             \
      if(((z80_f & (uint8)Z80_FLAG_C) != 0U) || (z80_a > 0x99U))        \
        {                                                               \
          z80_adj = (uint8)(z80_adj | 0x60U);                           \
          z80_f |= (uint8)Z80_FLAG_C;                                   \
        }                                                               \
                                                                        \
      if((z80_f & (uint8)Z80_FLAG_N) != 0U)                             \
        {                                                               \
          z80_a = (uint8)(z80_a - z80_adj);                             \
          if(((z80_f & (uint8)Z80_FLAG_H) != 0U) && (z80_lo < 0x06U))   \
            z80_hf = (uint8)Z80_FLAG_H;                                 \
        }                                                               \
      else                                                              \
        {                                                               \
          z80_a = (uint8)(z80_a + z80_adj);                             \
          if(z80_lo > 0x09U)                                            \
            z80_hf = (uint8)Z80_FLAG_H;                                 \
        }                                                               \
                                                                        \
      z80_f &= (uint8)(Z80_FLAG_C | Z80_FLAG_N);                        \
      z80_f |= (uint8)(z80_hf |                                         \
                       (z80_a & Z80_SZ53_MASK) |                        \
                       Z80_PARITY_PV(z80_a));                           \
      if(z80_a == 0)                                                    \
        z80_f |= (uint8)Z80_FLAG_Z;                                     \
                                                                        \
      Z80_F = z80_f;                                                    \
      Z80_A = z80_a;                                                    \
    }                                                                   \
  while(0)

#define Z80_OP_CPL()                                                    \
  do                                                                    \
    {                                                                   \
      uint8 z80_a = (uint8)~Z80_A;                                      \
                                                                        \
      Z80_A = z80_a;                                                    \
      Z80_F = (uint8)((Z80_F & (uint8)(Z80_FLAG_S | Z80_FLAG_Z |        \
                                       Z80_FLAG_PV | Z80_FLAG_C)) |     \
                      (uint8)(Z80_FLAG_H | Z80_FLAG_N) |                \
                      (uint8)(z80_a & Z80_53_MASK));                    \
    }                                                                   \
  while(0)

#define Z80_OP_SCF()                                                    \
  do                                                                    \
    {                                                                   \
      Z80_F = (uint8)((Z80_F & (uint8)(Z80_FLAG_S | Z80_FLAG_Z |        \
                                       Z80_FLAG_PV)) |                  \
                      (uint8)Z80_FLAG_C |                               \
                      (uint8)(Z80_A & Z80_53_MASK));                    \
    }                                                                   \
  while(0)

/*
 * The complement of the carry moves the old carry into the half carry, which is
 * how a Z80 records what it just undid.
 */
#define Z80_OP_CCF()                                                    \
  do                                                                    \
    {                                                                   \
      uint8 z80_c = (uint8)(Z80_F & (uint8)Z80_FLAG_C);                 \
      uint8 z80_f = (uint8)((Z80_F & (uint8)(Z80_FLAG_S | Z80_FLAG_Z |  \
                                             Z80_FLAG_PV)) |            \
                            (uint8)(Z80_A & Z80_53_MASK));              \
                                                                        \
      if(z80_c != 0U)                                                   \
        z80_f |= (uint8)Z80_FLAG_H;                                     \
      else                                                              \
        z80_f |= (uint8)Z80_FLAG_C;                                     \
                                                                        \
      Z80_F = z80_f;                                                    \
    }                                                                   \
  while(0)

/*
 * ---------------------------------------------------------------------------
 * The stack.
 * Semantics: TotalSMS/src/core/sms_z80.c:593-604.
 *
 * The pointer moves before each byte and the high half goes first, so a pair
 * pushed at SP ends up with its low half at the lower address, like everything
 * else the emulated processor stores. Written as two byte accesses rather than
 * one sixteen bit one for the reason Z80_WR16 above gives at length: the wrap
 * at the top of the address space, and the page boundary a mapper can put in
 * the middle of a word.
 * ---------------------------------------------------------------------------
 */
#define Z80_OP_PUSH(value)                                              \
  do                                                                    \
    {                                                                   \
      uint16 z80_pv = (uint16)(value);                                  \
                                                                        \
      Z80_SP = (uint16)(Z80_SP - 1);                                    \
      Z80_WR8(Z80_SP,(uint8)(z80_pv >> 8));                             \
      Z80_SP = (uint16)(Z80_SP - 1);                                    \
      Z80_WR8(Z80_SP,(uint8)(z80_pv & 0xFFU));                          \
    }                                                                   \
  while(0)

#define Z80_OP_POP(hi, lo)                              \
  do                                                    \
    {                                                   \
      (lo) = Z80_RD8(Z80_SP);                           \
      Z80_SP = (uint16)(Z80_SP + 1);                    \
      (hi) = Z80_RD8(Z80_SP);                           \
      Z80_SP = (uint16)(Z80_SP + 1);                    \
    }                                                   \
  while(0)

/*
 * ---------------------------------------------------------------------------
 * The four exchanges.
 * Semantics: TotalSMS/src/core/sms_z80.c:1075-1110.
 *
 * The one on the stack swaps HL with the word the pointer addresses, and leaves
 * the pointer itself alone -- which the source thought worth a comment of its
 * own, and which is the mistake to make here.
 * ---------------------------------------------------------------------------
 */
#define Z80_SWAP8(a, b)                                 \
  do                                                    \
    {                                                   \
      uint8 z80_t = (a);                                \
                                                        \
      (a) = (b);                                        \
      (b) = z80_t;                                      \
    }                                                   \
  while(0)

#define Z80_OP_EX_AF()                                  \
  do                                                    \
    {                                                   \
      Z80_SWAP8(Z80_A,Z80_A_ALT);                       \
      Z80_SWAP8(Z80_F,Z80_F_ALT);                       \
    }                                                   \
  while(0)

#define Z80_OP_EXX()                                    \
  do                                                    \
    {                                                   \
      Z80_SWAP8(Z80_B,Z80_B_ALT);                       \
      Z80_SWAP8(Z80_C,Z80_C_ALT);                       \
      Z80_SWAP8(Z80_D,Z80_D_ALT);                       \
      Z80_SWAP8(Z80_E,Z80_E_ALT);                       \
      Z80_SWAP8(Z80_H,Z80_H_ALT);                       \
      Z80_SWAP8(Z80_L,Z80_L_ALT);                       \
    }                                                   \
  while(0)

#define Z80_OP_EX_DE_HL()                               \
  do                                                    \
    {                                                   \
      Z80_SWAP8(Z80_D,Z80_H);                           \
      Z80_SWAP8(Z80_E,Z80_L);                           \
    }                                                   \
  while(0)

/*
 * The pair is an argument for the reason the sixteen bit addition above gives:
 * the index registers take this exchange too, and two copies of it would be two
 * chances to get the pointer left alone wrong in only one of them.
 */
#define Z80_OP_EX_SP_PAIR(hi, lo)                       \
  do                                                    \
    {                                                   \
      uint16 z80_sv = Z80_RD16(Z80_SP);                 \
                                                        \
      Z80_WR16(Z80_SP,Z80_PAIR(hi,lo));                 \
      Z80_SET_PAIR(hi,lo,z80_sv);                       \
    }                                                   \
  while(0)

#define Z80_OP_EX_SP_HL() Z80_OP_EX_SP_PAIR(Z80_H,Z80_L)

/*
 * ---------------------------------------------------------------------------
 * Branches, and what the taken case costs on top.
 * Semantics: TotalSMS/src/core/sms_z80.c:789-875.
 *
 * The cost table holds, for every conditional instruction, the price of the
 * case that is not taken; the taken case adds its surcharge here. A dispatch
 * that charged the table alone would run every taken call, return and relative
 * jump too cheaply, and the error would be invisible in behaviour and plain in
 * the frame rate.
 *
 * The relative displacement is signed and is widened by hand rather than by a
 * cast to a signed character. Converting a value above 127 to a signed type is
 * left to the implementation by the language, and what it would decide here is
 * the target of a branch: a rule the compiler is free to change is not one to
 * hang a jump on.
 * ---------------------------------------------------------------------------
 */
#define Z80_OP_JR()                                             \
  do                                                            \
    {                                                           \
      uint8 z80_d = Z80_RD8(Z80_PC);                            \
      int32 z80_off = (z80_d < 0x80U) ?                         \
                      (int32)z80_d : ((int32)z80_d - 256);      \
                                                                \
      Z80_PC = (uint16)((int32)Z80_PC + 1 + z80_off);           \
    }                                                           \
  while(0)

#define Z80_OP_JR_CC(cond)                              \
  do                                                    \
    {                                                   \
      if(cond)                                          \
        {                                               \
          Z80_OP_JR();                                  \
          Z80_SPEND(5);                                 \
        }                                               \
      else                                              \
        {                                               \
          Z80_PC = (uint16)(Z80_PC + 1);                \
        }                                               \
    }                                                   \
  while(0)

/*
 * The counter register is decremented whether or not the branch is taken: the
 * loop that ends on zero depends on the pass that ends it having counted.
 */
#define Z80_OP_DJNZ()                                   \
  do                                                    \
    {                                                   \
      Z80_B = (uint8)(Z80_B - 1U);                      \
      Z80_OP_JR_CC(Z80_B != 0U);                        \
    }                                                   \
  while(0)

#define Z80_OP_JP()                                     \
  do                                                    \
    {                                                   \
      Z80_PC = Z80_RD16(Z80_PC);                        \
    }                                                   \
  while(0)

#define Z80_OP_JP_CC(cond)                              \
  do                                                    \
    {                                                   \
      if(cond)                                          \
        Z80_OP_JP();                                    \
      else                                              \
        Z80_PC = (uint16)(Z80_PC + 2);                  \
    }                                                   \
  while(0)

/*
 * The return address pushed is the one past the two byte operand, not the one
 * the operand starts at.
 */
#define Z80_OP_CALL()                                   \
  do                                                    \
    {                                                   \
      uint16 z80_ta = Z80_RD16(Z80_PC);                 \
                                                        \
      Z80_OP_PUSH((uint16)(Z80_PC + 2));                \
      Z80_PC = z80_ta;                                  \
    }                                                   \
  while(0)

#define Z80_OP_CALL_CC(cond)                            \
  do                                                    \
    {                                                   \
      if(cond)                                          \
        {                                               \
          Z80_OP_CALL();                                \
          Z80_SPEND(7);                                 \
        }                                               \
      else                                              \
        {                                               \
          Z80_PC = (uint16)(Z80_PC + 2);                \
        }                                               \
    }                                                   \
  while(0)

#define Z80_OP_RET()                                    \
  do                                                    \
    {                                                   \
      uint8 z80_rl;                                     \
      uint8 z80_rh;                                     \
                                                        \
      Z80_OP_POP(z80_rh,z80_rl);                        \
      Z80_PC = Z80_PAIR(z80_rh,z80_rl);                 \
    }                                                   \
  while(0)

#define Z80_OP_RET_CC(cond)                             \
  do                                                    \
    {                                                   \
      if(cond)                                          \
        {                                               \
          Z80_OP_RET();                                 \
          Z80_SPEND(6);                                 \
        }                                               \
    }                                                   \
  while(0)

#define Z80_OP_RST(target)                              \
  do                                                    \
    {                                                   \
      Z80_OP_PUSH(Z80_PC);                              \
      Z80_PC = (uint16)(target);                        \
    }                                                   \
  while(0)

/*
 * ---------------------------------------------------------------------------
 * Interrupt enable and disable.
 * Semantics: TotalSMS/src/core/sms_z80.c:891-905.
 *
 * Disabling takes effect at once and clears both flip-flops. Enabling does not:
 * it raises the delay and nothing else, because the flip-flops only come up
 * after the instruction that follows, so that a return sitting behind an enable
 * still executes before any interrupt can be taken. The source resolves the
 * delay a whole instruction later (sms_z80.c:2139-2144), and resolving it here
 * would defeat the very purpose of holding it.
 *
 * What consumes the delay, and what reads the flip-flops afterwards, is the
 * acceptance stage at the head of z80_run (z80.c): a pending delay is
 * resolved at the sampling boundary -- both flip-flops come up, the delay
 * falls -- and no interrupt is taken at that same boundary, which is the
 * transposition of the source's one-cycle retry (sms_z80.c:2138-2144) onto
 * the scanline grain. The source's own EI also queues an immediate scheduler
 * check; that is the scheduler form this port sets aside for the scanline
 * grain, and nothing of it is kept here.
 * ---------------------------------------------------------------------------
 */
#define Z80_OP_DI()                                     \
  do                                                    \
    {                                                   \
      Z80_EI_DELAY = 0;                                 \
      Z80_IFF1 = 0;                                     \
      Z80_IFF2 = 0;                                     \
    }                                                   \
  while(0)

#define Z80_OP_EI()                                     \
  do                                                    \
    {                                                   \
      Z80_EI_DELAY = 1;                                 \
    }                                                   \
  while(0)

/*
 * ---------------------------------------------------------------------------
 * The memory refresh counter.
 *
 * The part increments it once per opcode read and never touches bit 7, so an
 * instruction reached through a prefix ticks it twice: once for the prefix,
 * once for the byte behind it.
 *
 * TotalSMS does not keep it. It answers a read with a value composed out of
 * three registers and its scheduler's tick count, and says so in place -- "to
 * avoid this slight overhead" -- while describing, in the same comment, what
 * the part really does (sms_z80.c:1465-1478). The description is usable; the
 * substitute is not, because it is built on a scheduler this port does not
 * have. So the counter is kept for what it costs: one addition per opcode
 * read.
 *
 * LIKE THE FIVE HOT REGISTER NAMES, THIS MACRO HAS TWO HOMES. The form below
 * is the default: R is the eight bit field, so every tick masks. Inside
 * z80_run the name is retargeted onto a bare increment of a free-running
 * word-sized local, and the mask is paid at the boundaries, where the flush
 * composes the architectural byte, instead of once per instruction. The two
 * instructions that read or write R -- LD A,R and LD R,A, behind the ED
 * prefix -- are decoded inside that window and work the two homes of R
 * explicitly where they stand, so the bare increment never has a blind
 * reader. NO CODE TICKS THE FORM BELOW ANY MORE: every opcode read happens
 * inside the window. It stays here as the statement of what a tick means,
 * for whatever is ever added outside. The restore block after z80_run
 * re-states the definition below verbatim, under the same
 * kept-in-agreement-by-hand rule as the five names.
 * ---------------------------------------------------------------------------
 */
#define Z80_TICK_R()                                            \
  (Z80_R = (uint8)((Z80_R & 0x80U) | ((Z80_R + 1U) & 0x7FU)))

/*
 * ---------------------------------------------------------------------------
 * Sixteen bit addition and subtraction with carry.
 * Semantics: TotalSMS/src/core/sms_z80.c:555-592.
 *
 * These two write every flag, where ADD HL,rr writes four and preserves three.
 * The half carry comes out of bit 11 and the two undocumented bits off the high
 * byte of the result, both for the reason ADD HL,rr gives above: this is a
 * sixteen bit result and the flags read the half of it that matters.
 *
 * Two departures from the source, written down here because they are exactly
 * the kind of thing that gets quietly put back.
 *
 *   The carry in is carried into the twelve bit arithmetic instead of being
 *   folded into the operand beforehand. The source folds (:557, :576), and a
 *   folded operand of 0xFFFF with the carry set becomes 0x10000, whose bottom
 *   twelve bits are empty: the half carry is then read off nothing and comes
 *   out clear where the operation really produced one. The same defect the
 *   eight bit addition above corrects, for the same reason and at the same
 *   price of one addend. The sign of a folded operand is empty as well, which
 *   is the second reason the overflow below is not built on it.
 *
 *   The overflow is composed here instead of being taken from calc_vflag_16
 *   (:347-350), which cannot work: its three parameters are declared eight bits
 *   wide and masked with 0x8000, so every mask yields zero, the first
 *   comparison is always true, the second always false, and the function
 *   returns false whatever it is handed. What is written below is the rule of
 *   its eight bit twin (:342-345) at sixteen bits -- overflow when the result
 *   differs in sign from both operands, which is the same statement as operands
 *   alike in sign and result unlike them -- in the form the eight bit addition
 *   above already uses.
 * ---------------------------------------------------------------------------
 */
#define Z80_OP_ADC_HL(value)                                            \
  do                                                                    \
    {                                                                   \
      uint32 z80_hl = (uint32)Z80_HL;                                   \
      uint32 z80_v = (uint32)(value);                                   \
      uint32 z80_k = (uint32)Z80_FLAG_GET(Z80_FLAG_C);                  \
      uint32 z80_s = z80_hl + z80_v + z80_k;                            \
      uint16 z80_r = (uint16)z80_s;                                     \
      uint8 z80_f = (uint8)(((uint32)z80_r >> 8) & Z80_SZ53_MASK);      \
                                                                        \
      if(z80_r == 0)                                                    \
        z80_f |= (uint8)Z80_FLAG_Z;                                     \
      if(z80_s > 0xFFFFUL)                                              \
        z80_f |= (uint8)Z80_FLAG_C;                                     \
      if((((z80_hl & 0x0FFFUL) + (z80_v & 0x0FFFUL) + z80_k) &          \
          0x1000UL) != 0UL)                                             \
        z80_f |= (uint8)Z80_FLAG_H;                                     \
      if((((z80_hl ^ z80_s) & (z80_v ^ z80_s)) & 0x8000UL) != 0UL)      \
        z80_f |= (uint8)Z80_FLAG_PV;                                    \
                                                                        \
      Z80_F = z80_f;                                                    \
      Z80_SET_HL(z80_r);                                                \
    }                                                                   \
  while(0)

#define Z80_OP_SBC_HL(value)                                            \
  do                                                                    \
    {                                                                   \
      uint32 z80_hl = (uint32)Z80_HL;                                   \
      uint32 z80_v = (uint32)(value);                                   \
      uint32 z80_k = (uint32)Z80_FLAG_GET(Z80_FLAG_C);                  \
      uint16 z80_r = (uint16)(z80_hl - z80_v - z80_k);                  \
      uint8 z80_f = (uint8)((((uint32)z80_r >> 8) & Z80_SZ53_MASK) |    \
                            (uint8)Z80_FLAG_N);                         \
                                                                        \
      if(z80_r == 0)                                                    \
        z80_f |= (uint8)Z80_FLAG_Z;                                     \
      if(z80_hl < (z80_v + z80_k))                                      \
        z80_f |= (uint8)Z80_FLAG_C;                                     \
      if((z80_hl & 0x0FFFUL) < ((z80_v & 0x0FFFUL) + z80_k))            \
        z80_f |= (uint8)Z80_FLAG_H;                                     \
      if((((z80_hl ^ z80_v) & (z80_hl ^ (uint32)z80_r)) & 0x8000UL) !=  \
         0UL)                                                           \
        z80_f |= (uint8)Z80_FLAG_PV;                                    \
                                                                        \
      Z80_F = z80_f;                                                    \
      Z80_SET_HL(z80_r);                                                \
    }                                                                   \
  while(0)

/*
 * Negation, written as the subtraction it is.
 *
 * The source composes it as A - 2A (sms_z80.c:537-540), which produces the
 * right result -- A - 2A is -A -- and the wrong flags, because the flags are
 * computed from the operand handed to the subtraction, and that operand is 2A.
 * The half carry is the shortest demonstration: it is (A & 0xF) < (value & 0xF)
 * (:374), so for A = 0x08 the composed form asks whether 8 < 0 and answers no,
 * while the real operation asks whether 0 < 8 and answers yes. The overflow,
 * computed from the same operand, is exposed to the same gap.
 *
 * So the operation is written out: nought minus the accumulator, through the
 * subtraction flag rule already established above (:368-381) applied to a null
 * minuend.
 */
#define Z80_OP_NEG()                                    \
  do                                                    \
    {                                                   \
      uint8 z80_a = Z80_A;                              \
      uint8 z80_r = (uint8)(0U - (uint32)z80_a);        \
                                                        \
      Z80_SUB_FLAGS(0U,z80_a,0U,z80_r,z80_r);           \
      Z80_A = z80_r;                                    \
    }                                                   \
  while(0)

/*
 * ---------------------------------------------------------------------------
 * The eight block instructions: four transfers, four comparisons.
 * Semantics: TotalSMS/src/core/sms_z80.c:1113-1225.
 *
 * Three things about them are easy to smooth over and expensive to find again.
 *
 *   The second undocumented flag is copied from BIT 1 of the reference value,
 *   not from bit 5, and that holds for all eight. It is not a slip of the
 *   source (:1133, :1188): it is what these instructions do. Reaching for the
 *   shared 0xA8 mask used everywhere else in this file would put bit 5 back
 *   without a word, and the failure would show up on a flag test that names
 *   neither bit.
 *
 *   The reference value differs between the two families. A transfer reads its
 *   two undocumented bits off the accumulator plus the byte moved (:1187); a
 *   comparison reads them off the accumulator minus the byte read minus the
 *   half carry it has just computed (:1130). Two neighbouring families, two
 *   distinct quantities.
 *
 *   A comparison does not touch the carry, and the shared bit carries "the
 *   counter is not empty" rather than a parity, in both families.
 * ---------------------------------------------------------------------------
 */
#define Z80_BLOCK_LD(step)                                              \
  do                                                                    \
    {                                                                   \
      uint16 z80_hl = Z80_HL;                                           \
      uint16 z80_de = Z80_DE;                                           \
      uint16 z80_bc = (uint16)(Z80_BC - 1U);                            \
      uint8 z80_v = Z80_RD8(z80_hl);                                    \
      uint8 z80_n;                                                      \
      uint8 z80_f;                                                      \
                                                                        \
      Z80_WR8(z80_de,z80_v);                                            \
                                                                        \
      z80_n = (uint8)((uint32)Z80_A + (uint32)z80_v);                   \
      z80_f = (uint8)(Z80_F & (uint8)(Z80_FLAG_S | Z80_FLAG_Z |         \
                                      Z80_FLAG_C));                     \
      z80_f |= (uint8)(z80_n & (uint8)Z80_FLAG_B3);                     \
      if((z80_n & 0x02U) != 0U)                                         \
        z80_f |= (uint8)Z80_FLAG_B5;                                    \
      if(z80_bc != 0)                                                   \
        z80_f |= (uint8)Z80_FLAG_PV;                                    \
                                                                        \
      Z80_F = z80_f;                                                    \
      Z80_SET_BC(z80_bc);                                               \
      Z80_SET_DE((uint16)(z80_de + (step)));                            \
      Z80_SET_HL((uint16)(z80_hl + (step)));                            \
    }                                                                   \
  while(0)

#define Z80_BLOCK_CP(step)                                              \
  do                                                                    \
    {                                                                   \
      uint16 z80_hl = Z80_HL;                                           \
      uint16 z80_bc = (uint16)(Z80_BC - 1U);                            \
      uint8 z80_a = Z80_A;                                              \
      uint8 z80_v = Z80_RD8(z80_hl);                                    \
      uint8 z80_r = (uint8)(z80_a - z80_v);                             \
      uint8 z80_h = (uint8)((((uint32)z80_a & 0x0FUL) <                 \
                             ((uint32)z80_v & 0x0FUL)) ? 1U : 0U);      \
      uint8 z80_n = (uint8)(z80_r - z80_h);                             \
      uint8 z80_f = (uint8)((Z80_F & (uint8)Z80_FLAG_C) |               \
                            (z80_r & (uint8)Z80_FLAG_S) |               \
                            (uint8)Z80_FLAG_N);                         \
                                                                        \
      if(z80_r == 0)                                                    \
        z80_f |= (uint8)Z80_FLAG_Z;                                     \
      if(z80_h != 0U)                                                   \
        z80_f |= (uint8)Z80_FLAG_H;                                     \
      if(z80_bc != 0)                                                   \
        z80_f |= (uint8)Z80_FLAG_PV;                                    \
      z80_f |= (uint8)(z80_n & (uint8)Z80_FLAG_B3);                     \
      if((z80_n & 0x02U) != 0U)                                         \
        z80_f |= (uint8)Z80_FLAG_B5;                                    \
                                                                        \
      Z80_F = z80_f;                                                    \
      Z80_SET_BC(z80_bc);                                               \
      Z80_SET_HL((uint16)(z80_hl + (step)));                            \
    }                                                                   \
  while(0)

/*
 * How a repeated block instruction repeats, and it is not a loop.
 *
 * One iteration runs, and if the condition still holds the counter is backed up
 * over the two bytes of the instruction so that the instruction is fetched and
 * executed again, at the price of five more T-states (sms_z80.c:1204-1208).
 *
 * Three consequences, all of them wanted. The quota of the scanline is honoured
 * one iteration at a time, where an internal loop of 65535 iterations would
 * overrun it by two orders of magnitude. Anything sampled between instructions
 * is sampled between iterations. And the refresh counter advances once per
 * opcode read of every iteration -- twice, this being a prefixed instruction --
 * as it does on the part.
 *
 * An internal loop would be shorter to write and would break all three.
 */
#define Z80_BLOCK_REPEAT(cond)                          \
  do                                                    \
    {                                                   \
      if((cond))                                        \
        {                                               \
          Z80_PC = (uint16)(Z80_PC - 2);                \
          Z80_SPEND(5);                                 \
        }                                               \
    }                                                   \
  while(0)

#define Z80_OP_LDI() Z80_BLOCK_LD(+1)
#define Z80_OP_LDD() Z80_BLOCK_LD(-1)
#define Z80_OP_CPI() Z80_BLOCK_CP(+1)
#define Z80_OP_CPD() Z80_BLOCK_CP(-1)

#define Z80_OP_LDIR()                                   \
  do                                                    \
    {                                                   \
      Z80_BLOCK_LD(+1);                                 \
      Z80_BLOCK_REPEAT(Z80_BC != 0);                    \
    }                                                   \
  while(0)

#define Z80_OP_LDDR()                                   \
  do                                                    \
    {                                                   \
      Z80_BLOCK_LD(-1);                                 \
      Z80_BLOCK_REPEAT(Z80_BC != 0);                    \
    }                                                   \
  while(0)

/*
 * A repeated comparison stops on either of two things: the counter emptying or
 * the byte being found (sms_z80.c:1148).
 */
#define Z80_OP_CPIR()                                                   \
  do                                                                    \
    {                                                                   \
      Z80_BLOCK_CP(+1);                                                 \
      Z80_BLOCK_REPEAT((Z80_BC != 0) && !Z80_FLAG_GET(Z80_FLAG_Z));     \
    }                                                                   \
  while(0)

#define Z80_OP_CPDR()                                                   \
  do                                                                    \
    {                                                                   \
      Z80_BLOCK_CP(-1);                                                 \
      Z80_BLOCK_REPEAT((Z80_BC != 0) && !Z80_FLAG_GET(Z80_FLAG_Z));     \
    }                                                                   \
  while(0)

/*
 * ---------------------------------------------------------------------------
 * The input and output instructions of this prefix, and their block forms.
 * Semantics: TotalSMS/src/core/sms_z80.c:1227-1337.
 *
 * The mechanics are complete here: the counter is decremented, the pointer
 * advances, the flags are written, the repetition repeats. The other end is
 * the bus's: z80_io_write and z80_io_read (z80.c) decode the port on the
 * Master System's map and hand it to a named hook (cart.h). Until a part
 * exists behind a hook, a read answers 0xFF, a write drops the byte, and
 * the access is counted into a once-a-second aggregate line -- so a write
 * that goes nowhere is no longer silent, it is a figure that moves.
 *
 * THE FLAGS ARE A DECLARED DEVIATION, NOT A SIMPLIFICATION LEFT UNSAID. The
 * source is sparing with them, and this file transcribes the source: a
 * register form (Z80_IN_FLAGS) writes parity, clears the subtraction and
 * half carry flags, and leaves sign, zero, the two undocumented bits and
 * the carry as it found them; a block form writes the zero flag and the
 * subtraction flag and leaves the other six alone. On the machine, IN
 * r,(C) is commonly held to take sign, zero and the two undocumented bits
 * from the byte read, and the block forms to derive more -- but no source
 * of the first or second rank in this repository states that rule, and a
 * rule this port cannot cite it does not implement (SSOT 11.3). The gap
 * is invisible while every hook answers 0xFF, as they all still do; it
 * becomes observable the day a hook answers a value of its own, and it is
 * assumed here as such ahead of that day, on record
 * (deferred-work.md, "Les drapeaux de IN r,(C)"). It is reopened by a
 * cited source or by a program shown to depend on it, not by taste.
 * ---------------------------------------------------------------------------
 */
#define Z80_IN_FLAGS(v)                                                 \
  do                                                                    \
    {                                                                   \
      Z80_F = (uint8)((Z80_F & (uint8)(Z80_FLAG_S | Z80_FLAG_Z |        \
                                       Z80_53_MASK | Z80_FLAG_C)) |     \
                      Z80_PARITY_PV(v));                                \
    }                                                                   \
  while(0)

#define Z80_OP_IN_C(dst)                                \
  do                                                    \
    {                                                   \
      uint8 z80_iv = z80_io_read(Z80_C);                \
                                                        \
      Z80_IN_FLAGS(z80_iv);                             \
      (dst) = z80_iv;                                   \
    }                                                   \
  while(0)

/*
 * The one form that reads a port for its flags alone and drops the byte.
 */
#define Z80_OP_IN_C_DROP()                              \
  do                                                    \
    {                                                   \
      uint8 z80_iv = z80_io_read(Z80_C);                \
                                                        \
      Z80_IN_FLAGS(z80_iv);                             \
    }                                                   \
  while(0)

#define Z80_OP_OUT_C(value)                             \
  do                                                    \
    {                                                   \
      z80_io_write(Z80_C,(uint8)(value));               \
    }                                                   \
  while(0)

#define Z80_BLOCK_IN(step)                                              \
  do                                                                    \
    {                                                                   \
      uint16 z80_hl = Z80_HL;                                           \
      uint8 z80_v = z80_io_read(Z80_C);                                 \
      uint8 z80_b = (uint8)(Z80_B - 1U);                                \
      uint8 z80_f = (uint8)((Z80_F & (uint8)~(uint8)Z80_FLAG_Z) |       \
                            (uint8)Z80_FLAG_N);                         \
                                                                        \
      Z80_WR8(z80_hl,z80_v);                                            \
      if(z80_b == 0U)                                                   \
        z80_f |= (uint8)Z80_FLAG_Z;                                     \
                                                                        \
      Z80_F = z80_f;                                                    \
      Z80_B = z80_b;                                                    \
      Z80_SET_HL((uint16)(z80_hl + (step)));                            \
    }                                                                   \
  while(0)

#define Z80_BLOCK_OUT(step)                                             \
  do                                                                    \
    {                                                                   \
      uint16 z80_hl = Z80_HL;                                           \
      uint8 z80_v = Z80_RD8(z80_hl);                                    \
      uint8 z80_b = (uint8)(Z80_B - 1U);                                \
      uint8 z80_f = (uint8)((Z80_F & (uint8)~(uint8)Z80_FLAG_Z) |       \
                            (uint8)Z80_FLAG_N);                         \
                                                                        \
      z80_io_write(Z80_C,z80_v);                                        \
      if(z80_b == 0U)                                                   \
        z80_f |= (uint8)Z80_FLAG_Z;                                     \
                                                                        \
      Z80_F = z80_f;                                                    \
      Z80_B = z80_b;                                                    \
      Z80_SET_HL((uint16)(z80_hl + (step)));                            \
    }                                                                   \
  while(0)

#define Z80_OP_INI() Z80_BLOCK_IN(+1)
#define Z80_OP_IND() Z80_BLOCK_IN(-1)
#define Z80_OP_OUTI() Z80_BLOCK_OUT(+1)
#define Z80_OP_OUTD() Z80_BLOCK_OUT(-1)

/*
 * The counter of these four is B and not BC, so the repetition ends on B alone
 * (sms_z80.c:1252, :1300).
 */
#define Z80_OP_INIR()                                   \
  do                                                    \
    {                                                   \
      Z80_BLOCK_IN(+1);                                 \
      Z80_BLOCK_REPEAT(Z80_B != 0U);                    \
    }                                                   \
  while(0)

#define Z80_OP_INDR()                                   \
  do                                                    \
    {                                                   \
      Z80_BLOCK_IN(-1);                                 \
      Z80_BLOCK_REPEAT(Z80_B != 0U);                    \
    }                                                   \
  while(0)

#define Z80_OP_OTIR()                                   \
  do                                                    \
    {                                                   \
      Z80_BLOCK_OUT(+1);                                \
      Z80_BLOCK_REPEAT(Z80_B != 0U);                    \
    }                                                   \
  while(0)

#define Z80_OP_OTDR()                                   \
  do                                                    \
    {                                                   \
      Z80_BLOCK_OUT(-1);                                \
      Z80_BLOCK_REPEAT(Z80_B != 0U);                    \
    }                                                   \
  while(0)

/*
 * ---------------------------------------------------------------------------
 * The two nibble rotations through the accumulator and the byte HL addresses.
 * Semantics: TotalSMS/src/core/sms_z80.c:1408-1457.
 *
 * The low nibble of the accumulator and the two nibbles of the byte make a
 * three-digit ring, rotated one digit either way. The flags are those of a
 * logical operation on the accumulator, the carry alone surviving untouched.
 * ---------------------------------------------------------------------------
 */
#define Z80_RXD_FLAGS(a)                                        \
  do                                                            \
    {                                                           \
      uint8 z80_xf = (uint8)((Z80_F & (uint8)Z80_FLAG_C) |      \
                             ((a) & Z80_SZ53_MASK) |            \
                             Z80_PARITY_PV(a));                 \
                                                                \
      if((a) == 0)                                              \
        z80_xf |= (uint8)Z80_FLAG_Z;                            \
                                                                \
      Z80_F = z80_xf;                                           \
    }                                                           \
  while(0)

#define Z80_OP_RRD()                                                    \
  do                                                                    \
    {                                                                   \
      uint16 z80_ad = Z80_HL;                                           \
      uint8 z80_a = Z80_A;                                              \
      uint8 z80_v = Z80_RD8(z80_ad);                                    \
                                                                        \
      Z80_WR8(z80_ad,(uint8)(((uint32)z80_a << 4) | ((uint32)z80_v >> 4))); \
      z80_a = (uint8)((z80_a & 0xF0U) | (z80_v & 0x0FU));               \
                                                                        \
      Z80_A = z80_a;                                                    \
      Z80_RXD_FLAGS(z80_a);                                             \
    }                                                                   \
  while(0)

#define Z80_OP_RLD()                                                    \
  do                                                                    \
    {                                                                   \
      uint16 z80_ad = Z80_HL;                                           \
      uint8 z80_a = Z80_A;                                              \
      uint8 z80_v = Z80_RD8(z80_ad);                                    \
                                                                        \
      Z80_WR8(z80_ad,(uint8)(((uint32)z80_a & 0x0FUL) |                 \
                             ((uint32)z80_v << 4)));                    \
      z80_a = (uint8)((z80_a & 0xF0U) | (z80_v >> 4));                  \
                                                                        \
      Z80_A = z80_a;                                                    \
      Z80_RXD_FLAGS(z80_a);                                             \
    }                                                                   \
  while(0)

/*
 * ---------------------------------------------------------------------------
 * The interrupt vector and the refresh counter, read and written through the
 * accumulator.
 * Semantics: TotalSMS/src/core/sms_z80.c:1444-1478.
 *
 * Writing either one writes no flag at all. Reading either one writes five, and
 * the one that matters is the shared bit: it receives the second interrupt
 * flip-flop rather than a parity, which is the only way a program has of
 * finding out whether interrupts are enabled. The acceptance stage at the
 * head of z80_run is what gives that bit its meaning: a maskable acceptance
 * clears both flip-flops, a non-maskable one parks IFF1 in IFF2, and this
 * read is how a program sees either happen.
 *
 * The source writes five flags and leaves the carry and the two undocumented
 * bits as it found them, so that is what is written here.
 *
 * LD A,R HAS NO MACRO HERE, AND LD R,A NEVER HAD ONE. Both are decoded
 * inside z80_run's resident window, where R lives in two homes -- bit 7 in
 * the field, the running low seven bits in a resident counter -- so the read
 * is a composition of both homes and the write lands in both, each written
 * out at its dispatch in z80.c beside the comment that argues it. The flag
 * body below serves the read there, as it serves LD A,I here.
 * ---------------------------------------------------------------------------
 */
#define Z80_LD_A_IR_FLAGS(v)                                            \
  do                                                                    \
    {                                                                   \
      uint8 z80_lf = (uint8)((Z80_F & (uint8)(Z80_FLAG_C |              \
                                              Z80_53_MASK)) |           \
                             ((v) & (uint8)Z80_FLAG_S));                \
                                                                        \
      if((v) == 0)                                                      \
        z80_lf |= (uint8)Z80_FLAG_Z;                                    \
      if(Z80_IFF2 != 0)                                                 \
        z80_lf |= (uint8)Z80_FLAG_PV;                                   \
                                                                        \
      Z80_F = z80_lf;                                                   \
    }                                                                   \
  while(0)

#define Z80_OP_LD_A_I()                                 \
  do                                                    \
    {                                                   \
      uint8 z80_lv = Z80_I;                             \
                                                        \
      Z80_A = z80_lv;                                   \
      Z80_LD_A_IR_FLAGS(z80_lv);                        \
    }                                                   \
  while(0)

/*
 * ---------------------------------------------------------------------------
 * The two returns of this prefix.
 * Semantics: TotalSMS/src/core/sms_z80.c:832-842.
 *
 * The return from a non-maskable interrupt restores the first interrupt
 * flip-flop from the second, which is what makes the pair of them a saved
 * state rather than two flags: the acceptance of an NMI parks IFF1 in IFF2
 * (z80.c, the sampling stage), and this instruction is what brings it back
 * -- the NMI path is the first thing that ever exercises it, so a failure
 * there is checked against this line before the acceptance is suspected.
 *
 * The return from a maskable interrupt is a plain return, and the reason is
 * worth keeping rather than the behaviour alone: what distinguishes it on the
 * part is a signal to the device that raised the interrupt, saying it has been
 * serviced -- and this machine has no such device. Written down because a
 * reader who finds two names doing one thing will otherwise reopen the
 * question.
 * ---------------------------------------------------------------------------
 */
#define Z80_OP_RETN()                                   \
  do                                                    \
    {                                                   \
      Z80_OP_RET();                                     \
      Z80_IFF1 = Z80_IFF2;                              \
    }                                                   \
  while(0)

#define Z80_OP_RETI() Z80_OP_RET()

/*
 * ---------------------------------------------------------------------------
 * The rotations, the shifts and the three bit operations of the CB prefix.
 * Semantics: TotalSMS/src/core/sms_z80.c:631-653 for the flags the eight
 * rotations and shifts share, :907-1055 for the eleven operations themselves.
 *
 * These carry a Z80_CB_ name rather than a Z80_OP_ one, and the difference is
 * the contract and not the family. They take the operand and the result as two
 * plain locals of the caller and read each of them more than once. That is safe
 * here and nowhere else: the dispatch of this prefix reads its operand into a
 * local before it knows which of the eleven it has, because the operand field
 * is decoded first, so the single evaluation the Z80_OP_ macros promise has
 * already happened by the time one of these is reached. Naming them Z80_OP_
 * would promise it a second time, in the one place it is not being made.
 *
 * The eight rotations and shifts differ by their result and by which end of the
 * value leaves it. Their flags are one body: the subtraction flag and the half
 * carry are cleared, parity is the even parity of the result, the two
 * undocumented bits and the sign come off the result, and zero is zero.
 *
 * THE BIT THAT BECOMES THE CARRY IS TAKEN FROM THE INPUT, NEVER FROM THE
 * RESULT. On a shift it has already left the result, so reading it there reads
 * a different bit entirely; nothing says so at the time, and the first sign of
 * it is a branch taken the wrong way some instructions later.
 *
 * ONE RULE THAT BINDS EVERY CALLER OF THESE, AND IT FALLS OUT OF THAT ONE. The
 * shared flag body reads its result argument three times, and the operand and
 * the result must never be the same lvalue. A caller that passed one variable
 * for both would have overwritten the operand before the outgoing bit is taken
 * off it, and would then be taking that bit off the result -- the exact mistake
 * the paragraph above warns against, arrived at from the other end. Two
 * separate locals, always; it costs one register and it makes the mistake
 * unwritable.
 *
 * These are not the four accumulator rotations above. Those preserve sign, zero
 * and parity; these three write all three, and that is the whole of what
 * separates RLCA from RLC A.
 * ---------------------------------------------------------------------------
 */
#define Z80_CB_SHIFT_FLAGS(r, cout)                             \
  do                                                            \
    {                                                           \
      uint8 z80_sf = (uint8)(((r) & Z80_SZ53_MASK) |            \
                             Z80_PARITY_PV(r));                 \
                                                                \
      if((r) == 0)                                              \
        z80_sf |= (uint8)Z80_FLAG_Z;                            \
      if((cout) != 0U)                                          \
        z80_sf |= (uint8)Z80_FLAG_C;                            \
                                                                \
      Z80_F = z80_sf;                                           \
    }                                                           \
  while(0)

/*
 * The two rotations that carry their own bit round: the bit that leaves one end
 * enters the other, and the carry receives a copy of it rather than taking part
 * in the move.
 */
#define Z80_CB_RLC(v, r)                          \
  do                                              \
    {                                             \
      (r) = (uint8)(((v) << 1) | ((v) >> 7));     \
      Z80_CB_SHIFT_FLAGS(r,(uint8)((v) & 0x80U)); \
    }                                             \
  while(0)

#define Z80_CB_RRC(v, r)                          \
  do                                              \
    {                                             \
      (r) = (uint8)(((v) >> 1) | ((v) << 7));     \
      Z80_CB_SHIFT_FLAGS(r,(uint8)((v) & 0x01U)); \
    }                                             \
  while(0)

/*
 * The two that rotate through the carry: nine bits round a ring, the carry
 * being the ninth. The old carry enters the end the value vacates, and the bit
 * that leaves becomes the new one -- so the operand of the flag write and the
 * bit read out of the flags are two different bits, and the result has to be
 * composed before the flags are touched.
 */
#define Z80_CB_RL(v, r)                               \
  do                                                  \
    {                                                 \
      (r) = (uint8)(((v) << 1) |                      \
                    (uint8)Z80_FLAG_GET(Z80_FLAG_C)); \
      Z80_CB_SHIFT_FLAGS(r,(uint8)((v) & 0x80U));     \
    }                                                 \
  while(0)

#define Z80_CB_RR(v, r)                                              \
  do                                                                 \
    {                                                                \
      (r) = (uint8)(((v) >> 1) |                                     \
                    (uint8)(Z80_FLAG_GET(Z80_FLAG_C) ? 0x80U : 0U)); \
      Z80_CB_SHIFT_FLAGS(r,(uint8)((v) & 0x01U));                    \
    }                                                                \
  while(0)

/*
 * The arithmetic and logical shifts. The vacated end takes a nought, except in
 * the arithmetic shift right, which keeps the sign where it is -- that is the
 * one thing that makes it arithmetic.
 */
#define Z80_CB_SLA(v, r)                          \
  do                                              \
    {                                             \
      (r) = (uint8)((v) << 1);                    \
      Z80_CB_SHIFT_FLAGS(r,(uint8)((v) & 0x80U)); \
    }                                             \
  while(0)

#define Z80_CB_SRA(v, r)                          \
  do                                              \
    {                                             \
      (r) = (uint8)(((v) >> 1) | ((v) & 0x80U));  \
      Z80_CB_SHIFT_FLAGS(r,(uint8)((v) & 0x01U)); \
    }                                             \
  while(0)

/*
 * The shift left that fills with a one, which no data sheet from the part's
 * maker describes. It is one of the eight all the same: the encoding leaves a
 * gap where it sits, the part does something definite in that gap, and the
 * source implements it beside the seven others without remark
 * (sms_z80.c:1489). Leaving it out would fail tests that name no instruction at
 * all, since nothing published names this one.
 */
#define Z80_CB_SLL(v, r)                          \
  do                                              \
    {                                             \
      (r) = (uint8)(((v) << 1) | 0x01U);          \
      Z80_CB_SHIFT_FLAGS(r,(uint8)((v) & 0x80U)); \
    }                                             \
  while(0)

#define Z80_CB_SRL(v, r)                          \
  do                                              \
    {                                             \
      (r) = (uint8)((v) >> 1);                    \
      Z80_CB_SHIFT_FLAGS(r,(uint8)((v) & 0x01U)); \
    }                                             \
  while(0)

/*
 * Testing one bit, and three of its five flags are traps.
 * Semantics: TotalSMS/src/core/sms_z80.c:1026-1037.
 *
 *   The shared bit is NOT parity here. It is a copy of the zero flag, which is
 *   what the source says and what makes this instruction unlike every logical
 *   operation that writes the same bit.
 *   The half carry is set unconditionally, not computed.
 *   The two undocumented bits do not come off the result. The result of this
 *   instruction is one bit of the operand and nothing else, so bits 3 and 5 of
 *   it are nought in seven cases out of eight, and taking them there would be
 *   taking them from a value that has been emptied on purpose.
 *
 * The sign is the exception that keeps the reader honest: it does come off the
 * result, which means it is nought for every bit tested except the seventh.
 * The carry is not touched at all, so it is carried over rather than rebuilt.
 *
 * WHERE THE TWO UNDOCUMENTED BITS COME FROM IS THE THIRD ARGUMENT, AND IT IS
 * NOT ALWAYS THE OPERAND. The rule that holds for the whole family:
 *
 *   operand in a register    they come off that register
 *   operand at (HL)          they come off the high byte of the address, H
 *   operand at (IX+d)        they come off the high byte of the address
 *
 * The first is the operand and the other two are not, which is why the byte they
 * come from is passed in rather than derived here. The three-line rule is one
 * rule and not two: for an operand held in a register there is no address to
 * take them from, and for an operand in memory there is no reason to prefer the
 * byte read over the address it was read at.
 *
 * The source of the semantics is asymmetric on this point and the asymmetry is
 * not followed. It takes these bits off the operand for the (HL) form and off
 * the high byte of the address for the indexed form, marking the second as a
 * special case (sms_z80.c:1557-1561); the deposited exerciser, run with its
 * undocumented-flag image, accepts the indexed form and rejects the one through
 * HL, and expects the fingerprint that the address form produces
 * (docs/sms_gg/ZEXALL-SMS-0.21/source/zexall.sms.asm:851). The exerciser is the
 * higher authority here, so the (HL) form was brought onto the indexed form's
 * behaviour rather than the reverse. Reasoning, measurements and the reservation
 * that goes with them: docs/pseudocodes/40_z80_undocumented.md, section 1.
 *
 * The result is not written back anywhere -- see the dispatch, where the whole
 * difference between this and a bit set on the byte HL points at is a read
 * against a read-modify-write.
 */
#define Z80_CB_BIT(v, mask, undoc)                              \
  do                                                            \
    {                                                           \
      uint8 z80_bv = (uint8)(v);                                \
      uint8 z80_br = (uint8)(z80_bv & (uint8)(mask));           \
      uint8 z80_bf = (uint8)((Z80_F & (uint8)Z80_FLAG_C) |      \
                             (uint8)Z80_FLAG_H |                \
                             ((uint8)(undoc) & Z80_53_MASK) |   \
                             (z80_br & (uint8)Z80_FLAG_S));     \
                                                                \
      if(z80_br == 0)                                           \
        z80_bf |= (uint8)(Z80_FLAG_Z | Z80_FLAG_PV);            \
                                                                \
      Z80_F = z80_bf;                                           \
    }                                                           \
  while(0)

/*
 * Clearing one bit and setting one bit, and NEITHER TOUCHES A FLAG
 * (sms_z80.c:1039-1055). A flag written where none should be is exactly as
 * wrong as a flag left where one should have been written, and it is the
 * cheaper mistake to make: these two sit between the eight above and the one
 * before them, all of which write flags.
 */
#define Z80_CB_RES(v, r, mask)                          \
  do                                                    \
    {                                                   \
      (r) = (uint8)((v) & (uint8)~(uint8)(mask));       \
    }                                                   \
  while(0)

#define Z80_CB_SET(v, r, mask)                          \
  do                                                    \
    {                                                   \
      (r) = (uint8)((v) | (uint8)(mask));               \
    }                                                   \
  while(0)

/*
 * ---------------------------------------------------------------------------
 * Which of the eleven operations above an opcode of this prefix names, and what
 * it leaves behind.
 * Grouping: TotalSMS/src/core/sms_z80.c:1482-1512.
 *
 * This exists because the same eleven operations are reached from two places --
 * the prefix on its own, and the prefix behind an index prefix -- and the table
 * of operations must be one table. What differs between the two callers is not
 * decided here at all: where the operand was read, where the result is written,
 * and which byte supplies the two undocumented bits of a bit test are all
 * arguments or the caller's business. Duplicating this selection so that each
 * caller could keep its own copy would be two switches to keep in step, and the
 * day they drifted apart nothing would say so.
 *
 * WROTE ANSWERS WHETHER THERE IS A RESULT TO STORE. Ten of the eleven produce
 * one; the bit test produces flags and nothing else, and that single difference
 * is what separates a read from a read-modify-write for the caller -- including
 * what the instruction costs, which the indexed caller charges out of that
 * answer.
 *
 * A Z80_CB_ name and a Z80_CB_ contract: the operand, the result and the byte of
 * the undocumented bits are plain locals of the caller, read more than once, and
 * the operand and the result must be two distinct lvalues. Passing one variable
 * for both would overwrite the operand before the bit that becomes the carry has
 * been taken off it.
 * ---------------------------------------------------------------------------
 */
#define Z80_CB_APPLY(op, v, undoc, r, wrote)                            \
  do                                                                    \
    {                                                                   \
      uint8 z80_cop = (uint8)(op);                                      \
      uint8 z80_cbit = (uint8)(1U << ((z80_cop >> 3) & 0x07U));         \
                                                                        \
      (wrote) = 1;                                                      \
                                                                        \
      switch(z80_cop & 0xC0U)                                           \
        {                                                               \
        case 0x00:                                                      \
          switch((z80_cop >> 3) & 0x07U)                                \
            {                                                           \
            case 0x0: Z80_CB_RLC(v,r); break;                           \
            case 0x1: Z80_CB_RRC(v,r); break;                           \
            case 0x2: Z80_CB_RL(v,r);  break;                           \
            case 0x3: Z80_CB_RR(v,r);  break;                           \
            case 0x4: Z80_CB_SLA(v,r); break;                           \
            case 0x5: Z80_CB_SRA(v,r); break;                           \
            case 0x6: Z80_CB_SLL(v,r); break;                           \
            default:  Z80_CB_SRL(v,r); break;                           \
            }                                                           \
          break;                                                        \
                                                                        \
        case 0x40:                                                      \
          Z80_CB_BIT(v,z80_cbit,undoc);                                 \
          (wrote) = 0;                                                  \
          break;                                                        \
                                                                        \
        case 0x80: Z80_CB_RES(v,r,z80_cbit); break;                     \
        default:   Z80_CB_SET(v,r,z80_cbit); break;                     \
        }                                                               \
    }                                                                   \
  while(0)

/*
 * ---------------------------------------------------------------------------
 * The index registers.
 *
 * Two prefixes select one of two pairs and otherwise change nothing: where the
 * instruction behind them would have said HL, it says IX or IY instead
 * (sms_z80.c:2080-2082). So the operations below are the ones that have no
 * unprefixed equivalent to borrow -- everything else is reached by handing the
 * halves of the selected pair to a macro that already exists.
 * ---------------------------------------------------------------------------
 */

/*
 * Reads the displacement byte and composes the address it names.
 *
 * THE DISPLACEMENT IS SIGNED, from -128 to +127 (sms_z80.c:1572). Read as an
 * unsigned byte it would reach an address above the register instead of below
 * it, on half of all values, and the symptom of that appears wherever the
 * program next reads what it thought it had written.
 *
 * The sign is recovered by comparison rather than by a cast to a signed eight
 * bit type: the value of such a conversion is left to the implementation when
 * the byte does not fit, which is precisely the half of the range that matters
 * here. A comparison and a subtraction are exact everywhere and cost the same.
 *
 * IT IS A STATEMENT AND IT ADVANCES PC, WHICH IS WHY IT IS ONE. The source
 * writes this read inside argument lists -- write8(pair + DISP(), get_r8(...))
 * at sms_z80.c:1710 -- where the order in which the arguments are evaluated is
 * not specified by the language. It happens to be harmless there because no
 * other argument touches PC; the shape is not, and a second reader of PC added
 * to such a list would compile clean and behave differently from one compiler to
 * the next. Composing the address into a local, in a statement of its own, makes
 * that whole class of defect unwritable.
 */
#define Z80_FETCH_IXY_ADDR(base, addr)                          \
  do                                                            \
    {                                                           \
      uint8 z80_db = Z80_RD8(Z80_PC);                           \
      int32 z80_ds = (z80_db < 0x80U) ? (int32)z80_db           \
                                      : ((int32)z80_db - 256);  \
                                                                \
      Z80_PC = (uint16)(Z80_PC + 1);                            \
      (addr) = (uint16)((int32)(uint32)(base) + z80_ds);        \
    }                                                           \
  while(0)

#endif /* SMS3DO_Z80_OPS_H */
