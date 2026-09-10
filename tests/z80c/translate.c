/*
 * translate: reads a cartridge image on the PC and writes the C of the
 * code it can translate, for the core to run in place of interpreting it.
 *
 *   translate <rom> <out.c>
 *
 * What it knows: the Z80 instruction set -- the length of every
 * instruction, prefixes included, so as to walk the code; the semantics
 * of thirteen of them, the ones it emits -- the shape of a cartridge
 * image and the Sega mapper's slots. What it does not know: any title,
 * any per-game figure. Every image is treated alike.
 *
 * The output (src/rom_code.c) holds one function per block, a table from
 * the block's position in the cartridge to the function, the size and the
 * digest of the image, and the bytes the blocks cover -- and nothing
 * else: no byte of the image is copied out, no data, no name. A block is
 * an exact translation of the bytes at its position, written with the
 * macros the interpreter itself expands (src/z80_ops.h), so that there is
 * one semantics and not two.
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
 * Two passes. Discovery walks the reachable code from the three vectors
 * (reset, the maskable interrupt, the non-maskable one) and from every
 * target it meets -- jumps, calls and their returns, restarts, the
 * conditional forms -- marking the instructions it walks over and the
 * starts it learns, stopping at unconditional transfers and at the edge
 * of a 16k bank, which a block never crosses -- nor the edge of the first
 * kilobyte of bank 0, which the Sega mapper keeps in place while it turns
 * the rest of slot 0 (src/cart.c, cart_mapper_project): past that edge
 * the bytes at the address are another bank's. Emission then writes one
 * block per start: the thirteen instructions, in order, until the next
 * start, a transfer, an instruction it does not emit (the fallback: the
 * block leaves PC on it for the interpreter), the T-state cap, or the
 * bank's edge. A block cut by the cap makes its continuation a start.
 *
 * Lengths and costs are those of the core's own dispatch (src/z80.c, the
 * switch of z80_run and the three prefixed ones), which is the reference
 * the emitted code must agree with. The one trap is written there at
 * length: an index prefix in front of an instruction it has nothing to
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

/* The sum a block closes at: src/z80c.h, Z80C_BLOCK_TSTATES, which
   translate.sh reads off the header and passes here, so that the two
   cannot drift apart. The figure below is the fallback for a bare build
   of the tool. */
#ifndef BLOCK_TSTATES
#define BLOCK_TSTATES 64UL
#endif

/* The edge of the fixed first kilobyte of bank 0, in slot 0: a walk or a
   block that starts below it stops there (src/cart.c, cart_mapper_project:
   page 0 of the address space is never repointed, pages 1 to 15 are). */
#define SLOT0_FIXED 1024UL

static unsigned char rom[ROM_CAPACITY];
static unsigned long rom_size;
static unsigned long bank_mask;

/* One byte per position: whether an instruction was walked from here,
   whether a block starts here, whether one was emitted here, and whether
   an emitted block covers this byte -- counted once when two blocks
   overlap. */
#define M_INSN  1U
#define M_START 2U
#define M_BLOCK 4U
#define M_COVER 8U
static unsigned char mark[ROM_CAPACITY];

/* The starts still to walk. A position enters once. */
static unsigned long queue[ROM_CAPACITY];
static unsigned long queue_n;
static unsigned long queue_at;

/* The figures of the report. */
static unsigned long n_starts;
static unsigned long n_insns;
static unsigned long n_reached;
static unsigned long n_blocks;
static unsigned long n_emitted;
static unsigned long n_code_bytes;
static unsigned long n_end_next, n_end_jump, n_end_fallback, n_end_cap, n_end_bank;

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
}

static void add_target(unsigned long addr, unsigned long bank)
{
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

/* ---- discovery ------------------------------------------------------- */

/* Whether the two byte instruction behind the block prefix is a return
   from interrupt: RETN, RETI and their six undocumented mirrors. */
static int ed_return(unsigned sub)
{
  return (sub & 0xC7U) == 0x45U;
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

      if(mark[pos] & M_INSN)
        return;
      len = insn_len(p,end - pos);
      if(pos + (unsigned long)len > end)
        return;

      mark[pos] |= M_INSN;
      n_insns++;
      n_reached += (unsigned long)len;

      switch(op)
        {
        case 0x18: /* jr */
          add_target(addr_of(pos) + 2UL + (unsigned long)disp8(p[1]),bank);
          return;
        case 0x10: case 0x20: case 0x28: case 0x30: case 0x38: /* djnz, jr cc */
          add_target(addr_of(pos) + 2UL + (unsigned long)disp8(p[1]),bank);
          break;
        case 0xC3: /* jp nn */
          add_target(imm16(p + 1),bank);
          return;
        case 0xC2: case 0xCA: case 0xD2: case 0xDA:
        case 0xE2: case 0xEA: case 0xF2: case 0xFA: /* jp cc,nn */
          add_target(imm16(p + 1),bank);
          break;
        case 0xCD: /* call nn: the callee, and the return */
          add_target(imm16(p + 1),bank);
          add_start(pos + 3UL);
          return;
        case 0xC4: case 0xCC: case 0xD4: case 0xDC:
        case 0xE4: case 0xEC: case 0xF4: case 0xFC: /* call cc,nn */
          add_target(imm16(p + 1),bank);
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

/* ---- emission -------------------------------------------------------- */

static const char *reg_name[8] =
{
  "Z80_B", "Z80_C", "Z80_D", "Z80_E", "Z80_H", "Z80_L", NULL, "Z80_A"
};

/* One block's C, accumulated before its head is written: whether the
   entry address is used decides whether the local is declared at all. */
static char body[8192];
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

/* Why a block closed. */
enum end_kind { END_NEXT, END_JUMP, END_FALLBACK, END_CAP, END_BANK };

/* Emits the instruction at p when it is one of the thirteen: returns its
   cost, or 0 when it is not emitted. off is its offset from the block's
   entry; *transfer is raised when it ends the block. The pieces are
   written as the interpreter executes them (src/z80.c, src/z80_ops.h):
   a plain move for the loads, a macro for everything with a flag, and
   the targets folded at emission -- relative ones from the entry address
   read at run time, absolute ones as the immediate. */
static unsigned long emit_insn(const unsigned char *p, unsigned long off,
                               int *transfer)
{
  unsigned op = p[0];
  char line[160];

  *transfer = 0;

  if(op == 0x00U)
    {
      emit("  /* nop */\n");
      return 4UL;
    }

  /* ld r,n */
  if((op & 0xC7U) == 0x06U && ((op >> 3) & 7U) != 6U)
    {
      sprintf(line,"  %s = 0x%02XU; /* ld r,n */\n",
              reg_name[(op >> 3) & 7U],(unsigned)p[1]);
      emit(line);
      return 7UL;
    }

  /* ld r,r -- the byte HL points at is neither source nor destination */
  if((op & 0xC0U) == 0x40U && ((op >> 3) & 7U) != 6U && (op & 7U) != 6U)
    {
      if(((op >> 3) & 7U) != (op & 7U))
        {
          sprintf(line,"  %s = %s; /* ld r,r */\n",
                  reg_name[(op >> 3) & 7U],reg_name[op & 7U]);
          emit(line);
        }
      else
        emit("  /* ld r,r: the same register, no operation */\n");
      return 4UL;
    }

  /* inc r, dec r */
  if((op & 0xC6U) == 0x04U && ((op >> 3) & 7U) != 6U)
    {
      sprintf(line,"  %s(%s); /* %s r */\n",
              (op & 1U) ? "Z80_OP_DEC_R" : "Z80_OP_INC_R",
              reg_name[(op >> 3) & 7U],
              (op & 1U) ? "dec" : "inc");
      emit(line);
      return 4UL;
    }

  switch(op)
    {
    case 0xE6:
      sprintf(line,"  Z80_OP_AND(0x%02XU); /* and n */\n",(unsigned)p[1]);
      emit(line);
      return 7UL;
    case 0xEE:
      sprintf(line,"  Z80_OP_XOR(0x%02XU); /* xor n */\n",(unsigned)p[1]);
      emit(line);
      return 7UL;
    case 0xF6:
      sprintf(line,"  Z80_OP_OR(0x%02XU); /* or n */\n",(unsigned)p[1]);
      emit(line);
      return 7UL;
    case 0xFE:
      sprintf(line,"  Z80_OP_CP(0x%02XU); /* cp n */\n",(unsigned)p[1]);
      emit(line);
      return 7UL;

    case 0x18: /* jr d: the target from the entry address, folded */
      {
        long d = disp8(p[1]);
        unsigned long t = (unsigned long)((long)off + 2L + d) & 0xFFFFUL;

        sprintf(line,"  Z80_PC = (uint16)(z80_pc0 + 0x%04lXU); /* jr %ld */\n",
                t,d);
        emit(line);
        uses_pc0 = 1;
        *transfer = 1;
        return 12UL;
      }

    case 0xC3: /* jp nn */
      sprintf(line,"  Z80_PC = 0x%04lXU; /* jp nn */\n",imm16(p + 1));
      emit(line);
      *transfer = 1;
      return 10UL;

    case 0xC9: /* ret */
      emit("  Z80_OP_RET(); /* ret */\n");
      *transfer = 1;
      return 10UL;

    case 0xCD: /* call nn: the return address from the entry address */
      sprintf(line,"  Z80_OP_PUSH((uint16)(z80_pc0 + 0x%04lXU)); /* call nn */\n",
              (off + 3UL) & 0xFFFFUL);
      emit(line);
      sprintf(line,"  Z80_PC = 0x%04lXU;\n",imm16(p + 1));
      emit(line);
      uses_pc0 = 1;
      *transfer = 1;
      return 17UL;

    default:
      return 0UL;
    }
}

/* Writes the block that starts at a position, if it holds at least one
   emitted instruction; records its entry for the table. */
static void emit_block(FILE *out, unsigned long start)
{
  unsigned long bank = start / BANK_SIZE;
  unsigned long end  = (bank + 1UL) * BANK_SIZE;
  unsigned long pos  = start;
  unsigned long sum  = 0;
  unsigned long n    = 0;
  enum end_kind kind = END_BANK;
  char line[160];

  if(start < SLOT0_FIXED)
    end = SLOT0_FIXED;

  body_n = 0;
  uses_pc0 = 0;

  for(;;)
    {
      int len;
      int transfer;
      unsigned long cost;

      if(pos >= end)
        {
          kind = END_BANK;
          break;
        }
      if(pos != start && (mark[pos] & M_START))
        {
          kind = END_NEXT;
          break;
        }
      len = insn_len(rom + pos,end - pos);
      if(pos + (unsigned long)len > end)
        {
          kind = END_BANK;
          break;
        }

      cost = emit_insn(rom + pos,pos - start,&transfer);
      if(cost == 0UL)
        {
          kind = END_FALLBACK;
          break;
        }

      sum += cost;
      n++;
      pos += (unsigned long)len;

      if(transfer)
        {
          kind = END_JUMP;
          break;
        }
      if(sum >= BLOCK_TSTATES)
        {
          kind = END_CAP;
          if(pos < end)
            {
              if(!(mark[pos] & M_START))
                n_starts++;
              mark[pos] |= M_START;
            }
          break;
        }
    }

  if(n == 0UL)
    return;

  if(kind != END_JUMP)
    {
      sprintf(line,"  Z80_PC = (uint16)(z80_pc0 + 0x%04lXU); /* %s */\n",
              (pos - start) & 0xFFFFUL,
              (kind == END_NEXT) ? "next block" :
              (kind == END_FALLBACK) ? "fallback: interpreted from here" :
              (kind == END_CAP) ? "cap" : "bank edge");
      emit(line);
      uses_pc0 = 1;
    }

  fprintf(out,"static void\nb_%06lx(void)\n{\n",start);
  if(uses_pc0)
    fprintf(out,"  uint16 z80_pc0 = Z80_PC;\n\n");
  fwrite(body,1,body_n,out);
  fprintf(out,"  Z80_R = (uint8)((Z80_R & 0x80U) | ((Z80_R + %luU) & 0x7FU));\n",n);
  fprintf(out,"  Z80_SPEND(%lu);\n}\n\n",sum);

  /* The table entry is written after every block, from this mark, in
     position order; the bytes the block covers are summed here. */
  mark[start] |= M_BLOCK;
  n_blocks++;
  n_emitted += n;
  {
    unsigned long i;

    for(i = start; i < pos; i++)
      if(!(mark[i] & M_COVER))
        {
          mark[i] |= M_COVER;
          n_code_bytes++;
        }
  }
  switch(kind)
    {
    case END_NEXT:     n_end_next++;     break;
    case END_JUMP:     n_end_jump++;     break;
    case END_FALLBACK: n_end_fallback++; break;
    case END_CAP:      n_end_cap++;      break;
    case END_BANK:     n_end_bank++;     break;
    }
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

  if(argc != 3)
    {
      fprintf(stderr,"usage: translate <rom> <out.c>\n");
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

  /* Discovery from the three vectors. */
  add_start(0x0000UL);
  add_start(0x0038UL);
  add_start(0x0066UL);
  while(queue_at < queue_n)
    discover(queue[queue_at++]);

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
          " * functions; src/z80c.h says how the core runs them.\n"
          " */\n"
          "#include \"z80c.h\"\n"
          "#include \"z80_ops.h\"\n\n");

  /* Emission in position order, which is the table's order. */
  for(pos = 0; pos < rom_size; pos++)
    if(mark[pos] & M_START)
      emit_block(out,pos);

  fprintf(out,"const uint32 z80c_rom_size    = %luUL;\n",rom_size);
  fprintf(out,"const uint32 z80c_rom_fnv     = 0x%08lXUL;\n",fnv);
  fprintf(out,"const uint32 z80c_code_bytes  = %luUL;\n",n_code_bytes);
  fprintf(out,"const uint32 z80c_block_count = %luUL;\n\n",n_blocks);

  if(n_blocks == 0UL)
    fprintf(out,"const z80c_entry_t z80c_table[1] = { { 0UL, 0 } };\n");
  else
    {
      fprintf(out,"const z80c_entry_t z80c_table[%lu] =\n{\n",n_blocks);
      for(pos = 0; pos < rom_size; pos++)
        if(mark[pos] & M_BLOCK)
          fprintf(out,"  { 0x%06lXUL, b_%06lx },\n",pos,pos);
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
  fprintf(stderr,
          "z80c: starts=%lu reached=%lu ends: next=%lu jump=%lu fallback=%lu "
          "cap=%lu bank=%lu\n",
          n_starts,n_reached,n_end_next,n_end_jump,n_end_fallback,n_end_cap,
          n_end_bank);
  return 0;
}
