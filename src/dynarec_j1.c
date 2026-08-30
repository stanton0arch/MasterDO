#include "dynarec_j1.h"

#if SMS_DYNAREC_J1

#include "sys.h"
#include "log.h"
#include "z80.h"

/*
 * ---------------------------------------------------------------------------
 * The region under measurement: the head of the hot loop of the SCF/CCF
 * pre-test, twenty-three bytes at 0x2A86, plus keep_yxf at 0x2B04 which it
 * calls three times.
 *
 * Four blocks and six links per pass. Three of those links are a RET, whose
 * target is popped rather than written into the instruction -- the case a
 * translator can never resolve ahead of time, and the reason the table exists.
 * ---------------------------------------------------------------------------
 */
static const uint8 j1_head_bytes[] =
{
  0x21, 0xAA, 0xC0, /* 2A86  LD HL,$C0AA  */
  0xAF,             /* 2A89  XOR A        */
  0x3F,             /* 2A8A  CCF          */
  0xCD, 0x04, 0x2B, /* 2A8B  CALL $2B04   */
  0xAF,             /* 2A8E  XOR A        */
  0x3D,             /* 2A8F  DEC A        */
  0x3E, 0x00,       /* 2A90  LD A,$00     */
  0x3F,             /* 2A92  CCF          */
  0xCD, 0x04, 0x2B, /* 2A93  CALL $2B04   */
  0xAF,             /* 2A96  XOR A        */
  0x5F,             /* 2A97  LD E,A       */
  0x1D,             /* 2A98  DEC E        */
  0x3F,             /* 2A99  CCF          */
  0xCD, 0x04, 0x2B, /* 2A9A  CALL $2B04   */
  0xAF,             /* 2A9D  XOR A        */
  0x3E, 0xFF,       /* 2A9E  LD A,$FF     */
  0x3F,             /* 2AA0  CCF          */
  0xCD, 0x04, 0x2B  /* 2AA1  CALL $2B04   */
};

static const uint8 j1_keep_bytes[] =
{
  0xF5, 0xD1, 0x7B, 0xE6, 0x20, 0xCD, 0x0F, 0x2B, 0x7B, 0xE6, 0x08,
  0x28, 0x07, 0x34, 0x23, 0x20, 0x04, 0x34, 0x18, 0x01, 0x23, 0x23, 0xC9
};

#define J1_HEAD_ADDR   0x2A86UL
#define J1_KEEP_ADDR   0x2B04UL
#define J1_HEAD_LEN    ((uint32)sizeof(j1_head_bytes))
#define J1_KEEP_LEN    ((uint32)sizeof(j1_keep_bytes))

/* Where the chain runs out of translated code, and therefore where it stops. */
#define J1_STOP_ADDR   (J1_HEAD_ADDR + J1_HEAD_LEN)   /* 0x2A9D */

#define J1_COUNTERS    0xC0AAUL
#define J1_RESULTS     0xC100UL
#define J1_STACK_TOP   0xDFF0UL
/*
 * Room past the last block for the epilogue, which is parked at whichever
 * boundary a given check stops the chain at -- 0x2B04 among them, where it
 * runs past what keep_yxf occupies.
 */
#define J1_IMAGE_SIZE  (J1_KEEP_ADDR + J1_KEEP_LEN + 32UL)

/*
 * The four addresses that have a translation. Everything else in the table
 * stays zero, which is what stops the chain.
 */
#define J1_BLOCK_COUNT 5UL

/*
 * ---------------------------------------------------------------------------
 * The reference program. Same shape as J0's and for the same reason: the core
 * keeps its state to itself, so the way to see a register is to make the
 * emulated processor store it.
 *
 * It clears the counter area with a block copy before anything else. Installing
 * a cartridge writes the bottom of the address space and leaves the RAM above
 * it alone, so what one case leaves there is still there when the next starts.
 * Only the emulated program can clear emulated RAM.
 * ---------------------------------------------------------------------------
 */
static const uint8 j1_prologue[] =
{
  0x21, 0xAA, 0xC0,       /* LD HL,$C0AA                              */
  0x11, 0xAB, 0xC0,       /* LD DE,$C0AB                              */
  0x36, 0x00,             /* LD (HL),$00                              */
  0x01, 0x20, 0x00,       /* LD BC,$0020                              */
  0xED, 0xB0,             /* LDIR        -- clears $C0AA..$C0CA       */
  0x31, 0xF0, 0xDF,       /* LD SP,$DFF0                              */
  0x01, 0x00, 0x00,       /* LD BC,nn    -- C=F, B=A, indices 17, 18  */
  0xC5,                   /* PUSH BC                                  */
  0xF1,                   /* POP AF                                   */
  0xC3, 0x86, 0x2A        /* JP $2A86                                 */
};

#define J1_PATCH_F  17
#define J1_PATCH_A  18

/*
 * What DE holds when the block copy above has finished, which is what the
 * chain has to be handed so that both start from the same registers. Neither
 * half is read before keep_yxf overwrites both, but the checker compares them
 * at the end and a difference at the start would be a difference it blames on
 * the translation.
 */
#define J1_ENTRY_D  0xC0UL
#define J1_ENTRY_E  0xCBUL

/* Stores A, F, D, E, HL and SP, then stops. Sits at the address the chain
 * stops at, so both executors are read at the same point of the program. */
static const uint8 j1_epilogue[] =
{
  0x32, 0x00, 0xC1,       /* LD ($C100),A                 */
  0xF5,                   /* PUSH AF                      */
  0xC1,                   /* POP BC        -- B=A, C=F    */
  0x79,                   /* LD A,C                       */
  0x32, 0x01, 0xC1,       /* LD ($C101),A  -- F           */
  0x7A,                   /* LD A,D                       */
  0x32, 0x02, 0xC1,       /* LD ($C102),A  -- D           */
  0x7B,                   /* LD A,E                       */
  0x32, 0x03, 0xC1,       /* LD ($C103),A  -- E           */
  0x22, 0x04, 0xC1,       /* LD ($C104),HL                */
  0xED, 0x73, 0x06, 0xC1, /* LD ($C106),SP                */
  0xED, 0x00              /* stop                         */
};

/*
 * ---------------------------------------------------------------------------
 * Starting states -- and what the bench found about them, which is worth
 * keeping written down.
 *
 * They change nothing. The region opens with LD HL and XOR A, which overwrite
 * the incoming accumulator, flags and pointer before anything reads them, so
 * every entry state below produces the same run. They are kept because a
 * translation that leaked an entry register into the result WOULD be caught by
 * them, and that is a real defect to guard against -- but the paths through the
 * region are decided by the region itself, not by them.
 *
 * What decides the paths is the block at 0x2A9D, which sets A to 0xFF: the two
 * undocumented bits CCF copies out of A are what keep_yxf branches on. The
 * three blocks before it leave A at zero and take the short path twice,
 * touching no memory. Both halves are therefore covered, and neither is
 * covered by an entry state.
 * ---------------------------------------------------------------------------
 */
typedef struct
{
  uint32 a;
  uint32 f;
} j1_case_t;

static const j1_case_t j1_cases[] =
{
  { 0x00, 0x00 },
  { 0xFF, 0xFF },
  { 0x55, 0x20 },
  { 0xAA, 0x08 },
  { 0x12, 0x28 },
  { 0x80, 0x01 },   /* carry set on entry: CCF takes the other branch */
  { 0x7F, 0xC5 }    /* S, Z, PV and C all set, to be kept or dropped  */
};

#define J1_CASE_COUNT ((uint32)(sizeof(j1_cases) / sizeof(j1_cases[0])))

#define J1_ITERATIONS    100000UL
#define J1_CYCLES_PER_MS 12500UL

/*
 * A quota far above what one pass costs, so that what stops the chain is
 * running out of translated code and not running out of time. The quota path
 * is exercised on purpose further down, by a deliberately small one.
 */
#define J1_QUOTA_WIDE    100000UL

extern const uint8 dynarec_j1_chain[];
extern const uint8 dynarec_j1_chain_end[];
extern const uint8 dynarec_j1_at_2a86[];
extern const uint8 dynarec_j1_at_2a8e[];
extern const uint8 dynarec_j1_at_2a96[];
extern const uint8 dynarec_j1_at_2a9d[];
extern const uint8 dynarec_j1_at_2b04[];

typedef void (*j1_fn_t)(const j1_state_t *in, j1_state_t *out);

typedef union
{
  uint8   *bytes;
  j1_fn_t  fn;
} j1_cast_t;

static uint8  *j1_code  = NULL;
static uint8  *j1_guest = NULL;
static uint8  *j1_image = NULL;
static uint32 *j1_table = NULL;
static j1_fn_t j1_call  = NULL;

static void j1_fill_table(uint32 stop);

Err
dynarec_j1_install(void)
{
  uint32    span;
  uint32    i;
  j1_cast_t cast;

  span = (uint32)(dynarec_j1_chain_end - dynarec_j1_chain);

  if((span == 0UL) || (span > 8192UL))
    {
      LOG_ERR(LOG_CAT_Z80,("j1 install failed: bad span=%lu",(unsigned long)span));
      return -1;
    }

  j1_code  = (uint8 *)sys_alloc("j1_code",(int32)span,
                                MEMTYPE_DRAM | MEMTYPE_FILL);
  j1_guest = (uint8 *)sys_alloc("j1_guest",(int32)65536UL,
                                MEMTYPE_DRAM | MEMTYPE_FILL);
  j1_image = (uint8 *)sys_alloc("j1_image",(int32)J1_IMAGE_SIZE,
                                MEMTYPE_DRAM | MEMTYPE_FILL);
  j1_table = (uint32 *)sys_alloc("j1_table",
                                 (int32)(J1_TABLE_ENTRIES * 4UL),
                                 MEMTYPE_DRAM | MEMTYPE_FILL);

  if((j1_code == NULL) || (j1_guest == NULL) ||
     (j1_image == NULL) || (j1_table == NULL))
    {
      LOG_ERR(LOG_CAT_Z80,("j1 install failed: no memory, milestone absent"));
      return -1;
    }

  /*
   * Copied and then entered, with nothing asked of the system in between --
   * the property J0 verified on this machine and that everything here rests
   * on.
   */
  for(i = 0UL; i < span; i++)
    j1_code[i] = dynarec_j1_chain[i];

  /*
   * The table holds addresses inside the buffer, so each entry is where that
   * block sits in the original plus where the buffer is. Filling it here and
   * not in the assembly is the whole reason the assembly can be position
   * independent: it never names an address of its own.
   *
   * Filled for the whole pass to begin with; the checker refills it for each
   * boundary it wants the chain to stop at.
   */
  j1_fill_table(J1_STOP_ADDR);

  cast.bytes = j1_code;
  j1_call    = cast.fn;

  LOG_INFO(LOG_CAT_Z80,("j1 installed span=%lu blocks=%lu table=%lu bytes at 0x%08lx",
                        (unsigned long)span,
                        (unsigned long)J1_BLOCK_COUNT,
                        (unsigned long)(J1_TABLE_ENTRIES * 4UL),
                        (unsigned long)j1_code));
  return 0;
}

static void
j1_build_image(const j1_case_t *c, uint32 stop)
{
  uint32 i;

  for(i = 0UL; i < J1_IMAGE_SIZE; i++)
    j1_image[i] = 0;

  for(i = 0UL; i < (uint32)sizeof(j1_prologue); i++)
    j1_image[i] = j1_prologue[i];

  j1_image[J1_PATCH_F] = (uint8)c->f;
  j1_image[J1_PATCH_A] = (uint8)c->a;

  for(i = 0UL; i < J1_HEAD_LEN; i++)
    j1_image[J1_HEAD_ADDR + i] = j1_head_bytes[i];

  for(i = 0UL; i < J1_KEEP_LEN; i++)
    j1_image[J1_KEEP_ADDR + i] = j1_keep_bytes[i];

  /* The reading point, parked at whichever boundary this check stops at. */
  for(i = 0UL; i < (uint32)sizeof(j1_epilogue); i++)
    j1_image[stop + i] = j1_epilogue[i];
}

/*
 * ---------------------------------------------------------------------------
 * Every boundary the chain reaches, in order.
 *
 * The checker stops it at each of them in turn rather than only at the end,
 * and that is not thoroughness for its own sake -- it is the answer to a
 * defect the bench found. Comparing only the final state hides any difference
 * that does not survive to it, and almost none do: the last block rebuilds the
 * flags from nothing and the last call overwrites the stack the earlier ones
 * pushed on. A wrong flag three blocks back was invisible.
 *
 * Stopping is done by leaving blocks out of the table, not by starving the
 * quota: which block the chain stops before is then decided rather than
 * predicted.
 * ---------------------------------------------------------------------------
 */
static const uint32 j1_stops[] =
{
  J1_KEEP_ADDR,   /* after the first block, before keep_yxf is ever entered */
  0x2A8EUL,
  0x2A96UL,
  0x2A9DUL,
  J1_STOP_ADDR    /* the whole pass */
};

#define J1_STOP_COUNT ((uint32)(sizeof(j1_stops) / sizeof(j1_stops[0])))

/*
 * Fills the table with the blocks that come before the stop point, so that the
 * chain runs out of translated code exactly there.
 */
static void
j1_fill_table(uint32 stop)
{
  uint32 i;

  for(i = 0UL; i < J1_TABLE_ENTRIES; i++)
    j1_table[i] = 0UL;

  if(J1_HEAD_ADDR < stop)
    j1_table[J1_HEAD_ADDR] =
      (uint32)(j1_code + (dynarec_j1_at_2a86 - dynarec_j1_chain));
  if(0x2A8EUL < stop)
    j1_table[0x2A8EUL] =
      (uint32)(j1_code + (dynarec_j1_at_2a8e - dynarec_j1_chain));
  if(0x2A96UL < stop)
    j1_table[0x2A96UL] =
      (uint32)(j1_code + (dynarec_j1_at_2a96 - dynarec_j1_chain));
  if(0x2A9DUL < stop)
    j1_table[0x2A9DUL] =
      (uint32)(j1_code + (dynarec_j1_at_2a9d - dynarec_j1_chain));
  if(stop != J1_KEEP_ADDR)
    j1_table[J1_KEEP_ADDR] =
      (uint32)(j1_code + (dynarec_j1_at_2b04 - dynarec_j1_chain));
}

/*
 * Runs the region under the core and answers with the state it left and what
 * it cost. A quota of one T-state makes the core run exactly one instruction
 * and hand back what it overran by, so its cost is that plus one; only the
 * instructions inside the region are added up.
 */
static Err
j1_reference(const j1_case_t *c, uint32 stop, j1_state_t *out, uint32 *tstates)
{
  uint32 steps;
  uint16 pc;
  int32  over;
  int32  inside;

  j1_build_image(c,stop);
  z80_load_cartridge(j1_image,J1_IMAGE_SIZE);
  z80_reset();

  *tstates = 0UL;
  steps    = 0UL;

  for(;;)
    {
      if(z80_is_stopped())
        break;

      pc = z80_pc();
      inside = (((uint32)pc >= J1_HEAD_ADDR) && ((uint32)pc < stop)) ||
               (((uint32)pc >= J1_KEEP_ADDR) &&
                ((uint32)pc < (J1_KEEP_ADDR + J1_KEEP_LEN)) &&
                (stop != J1_KEEP_ADDR));

      over = z80_run(1);

      if(inside)
        *tstates += (uint32)over + 1UL;

      steps++;
      if(steps > 8192UL)
        {
          LOG_ERR(LOG_CAT_Z80,("j1 reference did not stop, pc=0x%04lx",
                               (unsigned long)z80_pc()));
          return -1;
        }
    }

  out->a  = (uint32)z80_peek((uint16)(J1_RESULTS + 0UL));
  out->f  = (uint32)z80_peek((uint16)(J1_RESULTS + 1UL));
  out->d  = (uint32)z80_peek((uint16)(J1_RESULTS + 2UL));
  out->e  = (uint32)z80_peek((uint16)(J1_RESULTS + 3UL));
  out->hl = (uint32)z80_peek((uint16)(J1_RESULTS + 4UL)) |
            ((uint32)z80_peek((uint16)(J1_RESULTS + 5UL)) << 8);
  out->sp = (uint32)z80_peek((uint16)(J1_RESULTS + 6UL)) |
            ((uint32)z80_peek((uint16)(J1_RESULTS + 7UL)) << 8);
  out->pc = stop;

  return 0;
}

/*
 * Puts the chain's address space into the state the reference program had
 * reached at the head of the region, and runs it.
 */
static void
j1_run_chain(const j1_case_t *c, uint32 quota, uint32 stop, j1_state_t *out)
{
  j1_state_t in;
  uint32     i;

  j1_fill_table(stop);

  for(i = 0UL; i < 65536UL; i++)
    j1_guest[i] = 0;

  in.a     = c->a;
  in.f     = c->f;
  in.d     = J1_ENTRY_D;
  in.e     = J1_ENTRY_E;
  in.hl    = 0UL;   /* the first instruction of the region loads it */
  in.sp    = J1_STACK_TOP;
  in.pc    = J1_HEAD_ADDR;
  in.quota = quota;
  in.links = 0UL;
  in.mem   = j1_guest;
  in.table = j1_table;

  j1_call(&in,out);
}

static Err
j1_compare(uint32 index, const j1_state_t *ref, const j1_state_t *got)
{
  uint32 addr;
  uint32 a;
  uint32 b;

#define J1_CHK(field,name,width)                                         \
  if(ref->field != got->field)                                           \
    {                                                                    \
      LOG_ERR(LOG_CAT_Z80,("j1 case=%lu " name " ref=0x%0" width "lx got=0x%0" width "lx", \
                           (unsigned long)index,                         \
                           (unsigned long)ref->field,                    \
                           (unsigned long)got->field));                  \
      return -1;                                                         \
    }

  J1_CHK(a,"A","2")
  J1_CHK(f,"F","2")
  J1_CHK(d,"D","2")
  J1_CHK(e,"E","2")
  J1_CHK(hl,"HL","4")
  J1_CHK(sp,"SP","4")
  J1_CHK(pc,"PC","4")
#undef J1_CHK

  /* The counters the three calls walk through, and the stack they pushed on. */
  for(addr = J1_COUNTERS; addr < (J1_COUNTERS + 16UL); addr++)
    {
      a = (uint32)z80_peek((uint16)addr);
      b = (uint32)j1_guest[addr];
      if(a != b)
        {
          LOG_ERR(LOG_CAT_Z80,("j1 case=%lu mem 0x%04lx ref=0x%02lx got=0x%02lx",
                               (unsigned long)index,(unsigned long)addr,
                               (unsigned long)a,(unsigned long)b));
          return -1;
        }
    }

  /*
   * The two bytes just under the reference's stack pointer are skipped, and
   * which two that is depends on where it stopped: the epilogue pushes AF to
   * read F back, and that push lands exactly there. J0 could name them once
   * because its stack pointer was always at the top; here it is not.
   */
  for(addr = (J1_STACK_TOP - 12UL); addr < J1_STACK_TOP; addr++)
    {
      if((addr == (ref->sp - 1UL)) || (addr == (ref->sp - 2UL)))
        continue;

      a = (uint32)z80_peek((uint16)addr);
      b = (uint32)j1_guest[addr];
      if(a != b)
        {
          LOG_ERR(LOG_CAT_Z80,("j1 case=%lu stack 0x%04lx ref=0x%02lx got=0x%02lx",
                               (unsigned long)index,(unsigned long)addr,
                               (unsigned long)a,(unsigned long)b));
          return -1;
        }
    }

  return 0;
}

Err
dynarec_j1_measure(void)
{
  j1_state_t    ref;
  j1_state_t    got;
  j1_state_t    in;
  j1_state_t    sink;
  uint32        i;
  uint32        k;
  uint32        tstates;
  uint32        pass_tstates;
  uint32        pass_links;
  uint32        t0;
  uint32        usec;
  uint32        cyc_x100;
  uint32        per_tstate_x100;
  uint32        per_link_x100;
  unsigned long vi, vf, ci, cf, li, lf;

  if(j1_call == NULL)
    {
      LOG_WARN(LOG_CAT_Z80,("j1 not installed, nothing measured"));
      return -1;
    }

  pass_tstates = 0UL;
  pass_links   = 0UL;

  for(k = 0UL; k < J1_STOP_COUNT; k++)
    {
      for(i = 0UL; i < J1_CASE_COUNT; i++)
        {
          if(j1_reference(&j1_cases[i],j1_stops[k],&ref,&tstates) != 0)
            return -1;

          j1_run_chain(&j1_cases[i],J1_QUOTA_WIDE,j1_stops[k],&got);

          if(j1_compare(i,&ref,&got) != 0)
            {
              LOG_ERR(LOG_CAT_Z80,("j1 chain disagrees with the core at stop 0x%04lx, no figure published",
                                   (unsigned long)j1_stops[k]));
              return -1;
            }

          if((i == 0UL) && (j1_stops[k] == J1_STOP_ADDR))
            {
              pass_tstates = tstates;
              pass_links   = got.links;
            }
        }
    }

  LOG_INFO(LOG_CAT_Z80,("j1 checked cases=%lu stops=%lu all agree, pass tstates=%lu links=%lu",
                        (unsigned long)J1_CASE_COUNT,
                        (unsigned long)J1_STOP_COUNT,
                        (unsigned long)pass_tstates,
                        (unsigned long)pass_links));

  if((pass_tstates == 0UL) || (pass_links == 0UL))
    {
      LOG_ERR(LOG_CAT_Z80,("j1 pass cost or link count is zero, no figure published"));
      return -1;
    }

  /*
   * The quota path, proved rather than assumed: with a budget under the cost
   * of one pass the chain has to stop early, and it has to stop on a block
   * boundary -- never inside one.
   */
  j1_run_chain(&j1_cases[0],1UL,J1_STOP_ADDR,&got);
  if((got.links >= pass_links) ||
     ((got.pc != J1_HEAD_ADDR) && (got.pc != J1_KEEP_ADDR) &&
      (got.pc != 0x2A8EUL) && (got.pc != 0x2A96UL) && (got.pc != J1_STOP_ADDR)))
    {
      LOG_ERR(LOG_CAT_Z80,("j1 short quota did not stop on a boundary: pc=0x%04lx links=%lu",
                           (unsigned long)got.pc,(unsigned long)got.links));
      return -1;
    }
  LOG_INFO(LOG_CAT_Z80,("j1 short quota stops on a boundary pc=0x%04lx links=%lu",
                        (unsigned long)got.pc,(unsigned long)got.links));

  /* The timed loop, on the whole pass. Every call starts from the same state. */
  j1_fill_table(J1_STOP_ADDR);

  for(i = 0UL; i < 65536UL; i++)
    j1_guest[i] = 0;

  in.a     = j1_cases[0].a;
  in.f     = j1_cases[0].f;
  in.d     = J1_ENTRY_D;
  in.e     = J1_ENTRY_E;
  in.hl    = 0UL;
  in.sp    = J1_STACK_TOP;
  in.pc    = J1_HEAD_ADDR;
  in.quota = J1_QUOTA_WIDE;
  in.links = 0UL;
  in.mem   = j1_guest;
  in.table = j1_table;

  t0 = sys_usec();
  for(i = 0UL; i < J1_ITERATIONS; i++)
    j1_call(&in,&sink);
  usec = sys_usec() - t0;

  cyc_x100        = (usec * (J1_CYCLES_PER_MS / 100UL)) / (J1_ITERATIONS / 10UL);
  per_tstate_x100 = cyc_x100 / pass_tstates;
  per_link_x100   = cyc_x100 / pass_links;

  vi = (unsigned long)(per_tstate_x100 / 100UL);
  vf = (unsigned long)(per_tstate_x100 % 100UL);
  ci = (unsigned long)(cyc_x100 / 100UL);
  cf = (unsigned long)(cyc_x100 % 100UL);
  li = (unsigned long)(per_link_x100 / 100UL);
  lf = (unsigned long)(per_link_x100 % 100UL);

  /*
   * The verdict, against J0's 1.91 for one block carrying one whole boundary.
   * Here one boundary is carried by the whole chain, and what is left between
   * two blocks is the link.
   */
  LOG_INFO(LOG_CAT_Z80,
           ("j1 verdict c/tstate=%lu.%02lu cycles=%lu.%02lu perlink=%lu.%02lu "
            "links=%lu tstates=%lu iters=%lu",
            vi,vf,ci,cf,li,lf,
            (unsigned long)pass_links,(unsigned long)pass_tstates,
            (unsigned long)J1_ITERATIONS));

  return 0;
}

#endif /* SMS_DYNAREC_J1 */
