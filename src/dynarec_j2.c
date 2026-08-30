#include "dynarec_j2.h"

#if SMS_DYNAREC_J2

#include "sys.h"
#include "log.h"
#include "z80.h"

/*
 * The region, unchanged from J1: the head of the hot loop of the SCF/CCF
 * pre-test, and keep_yxf which it calls four times. Five blocks, eight links
 * per pass -- four of them a CALL, whose target the instruction carries, and
 * four a RET, whose target is popped and therefore has to be looked up.
 */
static const uint8 j2_head_bytes[] =
{
  0x21, 0xAA, 0xC0, 0xAF, 0x3F, 0xCD, 0x04, 0x2B,
  0xAF, 0x3D, 0x3E, 0x00, 0x3F, 0xCD, 0x04, 0x2B,
  0xAF, 0x5F, 0x1D, 0x3F, 0xCD, 0x04, 0x2B,
  0xAF, 0x3E, 0xFF, 0x3F, 0xCD, 0x04, 0x2B
};

static const uint8 j2_keep_bytes[] =
{
  0xF5, 0xD1, 0x7B, 0xE6, 0x20, 0xCD, 0x0F, 0x2B, 0x7B, 0xE6, 0x08,
  0x28, 0x07, 0x34, 0x23, 0x20, 0x04, 0x34, 0x18, 0x01, 0x23, 0x23, 0xC9
};

#define J2_HEAD_ADDR   0x2A86UL
#define J2_KEEP_ADDR   0x2B04UL
#define J2_HEAD_LEN    ((uint32)sizeof(j2_head_bytes))
#define J2_KEEP_LEN    ((uint32)sizeof(j2_keep_bytes))
#define J2_STOP_ADDR   (J2_HEAD_ADDR + J2_HEAD_LEN)     /* 0x2AA4 */

#define J2_COUNTERS    0xC0AAUL
#define J2_RESULTS     0xC100UL
#define J2_STACK_TOP   0xDFF0UL
#define J2_IMAGE_SIZE  (J2_KEEP_ADDR + J2_KEEP_LEN + 32UL)

/*
 * The reference program, unchanged in shape from J1: it clears the counters
 * itself because only the emulated program can write emulated RAM, and the
 * epilogue makes the processor store its own registers because the core keeps
 * them to itself.
 */
static const uint8 j2_prologue[] =
{
  0x21, 0xAA, 0xC0,       /* LD HL,$C0AA                             */
  0x11, 0xAB, 0xC0,       /* LD DE,$C0AB                             */
  0x36, 0x00,             /* LD (HL),$00                             */
  0x01, 0x20, 0x00,       /* LD BC,$0020                             */
  0xED, 0xB0,             /* LDIR                                    */
  0x31, 0xF0, 0xDF,       /* LD SP,$DFF0                             */
  0x01, 0x00, 0x00,       /* LD BC,nn  -- C=F, B=A, indices 17, 18   */
  0xC5,                   /* PUSH BC                                 */
  0xF1,                   /* POP AF                                  */
  0xC3, 0x86, 0x2A        /* JP $2A86                                */
};

#define J2_PATCH_F  17
#define J2_PATCH_A  18
#define J2_ENTRY_D  0xC0UL
#define J2_ENTRY_E  0xCBUL

static const uint8 j2_epilogue[] =
{
  0x32, 0x00, 0xC1, 0xF5, 0xC1, 0x79, 0x32, 0x01, 0xC1,
  0x7A, 0x32, 0x02, 0xC1, 0x7B, 0x32, 0x03, 0xC1,
  0x22, 0x04, 0xC1, 0xED, 0x73, 0x06, 0xC1, 0xED, 0x00
};

/*
 * ---------------------------------------------------------------------------
 * The three little programs that ask the core what a form writes.
 *
 * A table of flags is a transposition of a rule, and a transposition nobody
 * checked is a second implementation that believes itself a copy. Rather than
 * write the rules out again here and hope, each of the 256 entries is taken
 * from the core itself: set the accumulator, run the one instruction, push the
 * flags and store them. What the table holds is then what the core does, by
 * construction -- and the version computed from the written rules below is
 * compared against it, entry by entry, so that a mistake in either shows.
 * ---------------------------------------------------------------------------
 */
static const uint8 j2_probe_dec[] =
{
  0x01, 0x00, 0x00,       /* LD BC,$0000  -- C becomes F, so carry clear */
  0xC5, 0xF1,             /* PUSH BC / POP AF                            */
  0x3E, 0x00,             /* LD A,v       -- patched, index 6            */
  0x3D,                   /* DEC A                                       */
  0xF5, 0xC1, 0x79,       /* PUSH AF / POP BC / LD A,C                   */
  0x32, 0x00, 0xC1,       /* LD ($C100),A                                */
  0xED, 0x00              /* stop                                        */
};

static const uint8 j2_probe_inc[] =
{
  0x01, 0x00, 0x00,
  0xC5, 0xF1,
  0x3E, 0x00,             /* LD A,v-1     -- patched, index 6            */
  0x3C,                   /* INC A                                       */
  0xF5, 0xC1, 0x79,
  0x32, 0x00, 0xC1,
  0xED, 0x00
};

static const uint8 j2_probe_logic[] =
{
  0x01, 0x00, 0x00,
  0xC5, 0xF1,
  0x3E, 0x00,             /* LD A,r       -- patched, index 6            */
  0xE6, 0xFF,             /* AND $FF      -- result is r, flags are the  */
  0xF5, 0xC1, 0x79,       /*                 logical group's             */
  0x32, 0x00, 0xC1,
  0xED, 0x00
};

#define J2_PROBE_PATCH 6
#define J2_PROBE_SIZE  32UL

typedef struct
{
  uint32 a;
  uint32 f;
} j2_case_t;

static const j2_case_t j2_cases[] =
{
  { 0x00, 0x00 }, { 0xFF, 0xFF }, { 0x55, 0x20 }, { 0xAA, 0x08 },
  { 0x12, 0x28 }, { 0x80, 0x01 }, { 0x7F, 0xC5 }
};

#define J2_CASE_COUNT ((uint32)(sizeof(j2_cases) / sizeof(j2_cases[0])))

static const uint32 j2_stops[] =
{
  J2_KEEP_ADDR, 0x2A8EUL, 0x2A96UL, 0x2A9DUL, J2_STOP_ADDR
};

#define J2_STOP_COUNT ((uint32)(sizeof(j2_stops) / sizeof(j2_stops[0])))

#define J2_ITERATIONS    100000UL
#define J2_CYCLES_PER_MS 12500UL
#define J2_QUOTA_WIDE    100000UL
#define J2_VARIANTS      4UL

extern const uint8 dynarec_j2_chain[];
extern const uint8 dynarec_j2_chain_end[];
extern const uint8 dynarec_j2_exit[];
extern const uint8 dynarec_j2_v0_2a86[];
extern const uint8 dynarec_j2_v0_2a8e[];
extern const uint8 dynarec_j2_v0_2a96[];
extern const uint8 dynarec_j2_v0_2a9d[];
extern const uint8 dynarec_j2_v0_2b04[];
extern const uint8 dynarec_j2_v1_2a86[];
extern const uint8 dynarec_j2_v1_2a8e[];
extern const uint8 dynarec_j2_v1_2a96[];
extern const uint8 dynarec_j2_v1_2a9d[];
extern const uint8 dynarec_j2_v1_2b04[];
extern const uint8 dynarec_j2_v2_2a86[];
extern const uint8 dynarec_j2_v2_2a8e[];
extern const uint8 dynarec_j2_v2_2a96[];
extern const uint8 dynarec_j2_v2_2a9d[];
extern const uint8 dynarec_j2_v2_2b04[];
extern const uint8 dynarec_j2_v3_2a86[];
extern const uint8 dynarec_j2_v3_2a8e[];
extern const uint8 dynarec_j2_v3_2a96[];
extern const uint8 dynarec_j2_v3_2a9d[];
extern const uint8 dynarec_j2_v3_2b04[];

/* Five block entry points per variant, in the order of the five addresses. */
static const uint8 *const j2_entries[J2_VARIANTS][5] =
{
  { dynarec_j2_v0_2a86, dynarec_j2_v0_2a8e, dynarec_j2_v0_2a96,
    dynarec_j2_v0_2a9d, dynarec_j2_v0_2b04 },
  { dynarec_j2_v1_2a86, dynarec_j2_v1_2a8e, dynarec_j2_v1_2a96,
    dynarec_j2_v1_2a9d, dynarec_j2_v1_2b04 },
  { dynarec_j2_v2_2a86, dynarec_j2_v2_2a8e, dynarec_j2_v2_2a96,
    dynarec_j2_v2_2a9d, dynarec_j2_v2_2b04 },
  { dynarec_j2_v3_2a86, dynarec_j2_v3_2a8e, dynarec_j2_v3_2a96,
    dynarec_j2_v3_2a9d, dynarec_j2_v3_2b04 }
};

static const uint32 j2_addrs[5] =
{
  J2_HEAD_ADDR, 0x2A8EUL, 0x2A96UL, 0x2A9DUL, J2_KEEP_ADDR
};

static const char *const j2_names[J2_VARIANTS] =
{
  "neither", "link", "flags", "both"
};

typedef void (*j2_fn_t)(const j2_state_t *in, j2_state_t *out);

typedef union
{
  uint8   *bytes;
  j2_fn_t  fn;
} j2_cast_t;

static uint8  *j2_code   = NULL;
static uint8  *j2_guest  = NULL;
static uint8  *j2_image  = NULL;
static uint32 *j2_table  = NULL;
static uint8  *j2_flags  = NULL;
static uint32  j2_exit_at = 0UL;
static j2_fn_t j2_call   = NULL;

/*
 * Runs one of the three little programs for one value and answers with the F
 * the core left. A quota per instruction rather than a free run, so that a
 * program that failed to stop is caught rather than looping.
 */
static Err
j2_probe(const uint8 *prog, uint32 len, uint8 value, uint32 *flags)
{
  uint32 i;
  uint32 steps;

  for(i = 0UL; i < J2_PROBE_SIZE; i++)
    j2_image[i] = 0;
  for(i = 0UL; i < len; i++)
    j2_image[i] = prog[i];
  j2_image[J2_PROBE_PATCH] = value;

  z80_load_cartridge(j2_image,J2_PROBE_SIZE);
  z80_reset();

  steps = 0UL;
  while(!z80_is_stopped())
    {
      (void)z80_run(1);
      steps++;
      if(steps > 64UL)
        {
          LOG_ERR(LOG_CAT_Z80,("j2 probe did not stop"));
          return -1;
        }
    }

  *flags = (uint32)z80_peek((uint16)J2_RESULTS);
  return 0;
}

/*
 * The written rules, from z80_ops.h, so that the tables taken from the core
 * have something independent to be wrong against.
 */
static uint32
j2_parity_pv(uint32 v)
{
  uint32 x = (v ^ (v >> 4)) & 0x0FUL;
  return (((0x6996UL >> x) & 1UL) != 0UL) ? 0UL : 0x04UL;
}

static uint32
j2_rule_dec(uint32 v)
{
  uint32 r = (v - 1UL) & 0xFFUL;
  uint32 f = (r & 0xA8UL) | 0x02UL;

  if(r == 0UL)             f |= 0x40UL;
  if(v == 0x80UL)          f |= 0x04UL;
  if((v & 0x0FUL) == 0UL)  f |= 0x10UL;
  return f;
}

static uint32
j2_rule_inc(uint32 r)
{
  uint32 f = (r & 0xA8UL);

  if(r == 0UL)             f |= 0x40UL;
  if(r == 0x80UL)          f |= 0x04UL;
  if((r & 0x0FUL) == 0UL)  f |= 0x10UL;
  return f;
}

static uint32
j2_rule_sz53p(uint32 r)
{
  uint32 f = (r & 0xA8UL) | j2_parity_pv(r);

  if(r == 0UL) f |= 0x40UL;
  return f;
}

/*
 * Builds the three tables from the core and confronts each entry with the
 * written rule. Both have to agree on all 256 values, or nothing is measured:
 * every figure this file produces rests on these bytes.
 */
static Err
j2_build_flag_tables(void)
{
  uint32 v;
  uint32 got;
  uint32 want;

  for(v = 0UL; v < 256UL; v++)
    {
      /* DEC, indexed by the value before, carry left out. */
      if(j2_probe(j2_probe_dec,(uint32)sizeof(j2_probe_dec),(uint8)v,&got) != 0)
        return -1;
      got &= ~0x01UL;
      want = j2_rule_dec(v);
      if(got != want)
        {
          LOG_ERR(LOG_CAT_Z80,("j2 dec flags v=0x%02lx core=0x%02lx rule=0x%02lx",
                               (unsigned long)v,(unsigned long)got,(unsigned long)want));
          return -1;
        }
      j2_flags[J2_TAB_DEC + v] = (uint8)got;

      /*
       * INC, indexed by the value AFTER, so the probe is handed the one
       * before -- which for a result of zero is 0xFF, the wrap being the
       * whole point of an eight bit increment.
       */
      if(j2_probe(j2_probe_inc,(uint32)sizeof(j2_probe_inc),
                  (uint8)((v - 1UL) & 0xFFUL),&got) != 0)
        return -1;
      got &= ~0x01UL;
      want = j2_rule_inc(v);
      if(got != want)
        {
          LOG_ERR(LOG_CAT_Z80,("j2 inc flags r=0x%02lx core=0x%02lx rule=0x%02lx",
                               (unsigned long)v,(unsigned long)got,(unsigned long)want));
          return -1;
        }
      j2_flags[J2_TAB_INC + v] = (uint8)got;

      /* The logical group, with the half carry the operation adds taken off. */
      if(j2_probe(j2_probe_logic,(uint32)sizeof(j2_probe_logic),(uint8)v,&got) != 0)
        return -1;
      got &= ~0x10UL;
      want = j2_rule_sz53p(v);
      if(got != want)
        {
          LOG_ERR(LOG_CAT_Z80,("j2 sz53p r=0x%02lx core=0x%02lx rule=0x%02lx",
                               (unsigned long)v,(unsigned long)got,(unsigned long)want));
          return -1;
        }
      j2_flags[J2_TAB_SZ53P + v] = (uint8)got;
    }

  LOG_INFO(LOG_CAT_Z80,("j2 flag tables built from the core, 3x256 entries, rules agree"));
  return 0;
}

/*
 * Fills the block table for one variant, whole.
 *
 * It is no longer a way of stopping the chain, and the run of 2026-08-25 is
 * what said so: **a statically linked chain does not consult the table**. A
 * CALL whose target the instruction carries becomes a direct branch, so
 * removing that target's entry stops nothing -- the chain walked straight past
 * a boundary the checker believed it had closed, and the gate caught it.
 *
 * That is not a quirk of the harness. It is a property of direct chaining that
 * every later milestone inherits: once two blocks are branched together, the
 * table no longer governs the path between them, and unlinking them means
 * rewriting the branch.
 *
 * Stopping is done by the quota instead, which every link honours -- static or
 * not -- because every block ends by spending it.
 *
 * Every entry with no translation holds the address of the exit, which is what
 * lets the reduced link be a load straight into the program counter with
 * nothing to test afterwards.
 */
static void
j2_fill_table(uint32 variant)
{
  uint32 i;

  for(i = 0UL; i < J2_TABLE_ENTRIES; i++)
    j2_table[i] = j2_exit_at;

  for(i = 0UL; i < 5UL; i++)
    j2_table[j2_addrs[i]] =
      (uint32)(j2_code + (j2_entries[variant][i] - dynarec_j2_chain));
}

Err
dynarec_j2_install(void)
{
  uint32    span;
  uint32    i;
  j2_cast_t cast;

  span = (uint32)(dynarec_j2_chain_end - dynarec_j2_chain);

  if((span == 0UL) || (span > 16384UL))
    {
      LOG_ERR(LOG_CAT_Z80,("j2 install failed: bad span=%lu",(unsigned long)span));
      return -1;
    }

  j2_code  = (uint8 *)sys_alloc("j2_code",(int32)span,
                                MEMTYPE_DRAM | MEMTYPE_FILL);
  j2_guest = (uint8 *)sys_alloc("j2_guest",(int32)65536UL,
                                MEMTYPE_DRAM | MEMTYPE_FILL);
  j2_image = (uint8 *)sys_alloc("j2_image",(int32)J2_IMAGE_SIZE,
                                MEMTYPE_DRAM | MEMTYPE_FILL);
  j2_table = (uint32 *)sys_alloc("j2_table",(int32)(J2_TABLE_ENTRIES * 4UL),
                                 MEMTYPE_DRAM | MEMTYPE_FILL);
  j2_flags = (uint8 *)sys_alloc("j2_flags",(int32)J2_TAB_SIZE,
                                MEMTYPE_DRAM | MEMTYPE_FILL);

  if((j2_code == NULL) || (j2_guest == NULL) || (j2_image == NULL) ||
     (j2_table == NULL) || (j2_flags == NULL))
    {
      LOG_ERR(LOG_CAT_Z80,("j2 install failed: no memory, milestone absent"));
      return -1;
    }

  for(i = 0UL; i < span; i++)
    j2_code[i] = dynarec_j2_chain[i];

  j2_exit_at = (uint32)(j2_code + (dynarec_j2_exit - dynarec_j2_chain));

  cast.bytes = j2_code;
  j2_call    = cast.fn;

  LOG_INFO(LOG_CAT_Z80,("j2 installed span=%lu variants=%lu at 0x%08lx exit=0x%08lx",
                        (unsigned long)span,(unsigned long)J2_VARIANTS,
                        (unsigned long)j2_code,(unsigned long)j2_exit_at));
  return 0;
}

static void
j2_build_image(const j2_case_t *c, uint32 stop)
{
  uint32 i;

  for(i = 0UL; i < J2_IMAGE_SIZE; i++)
    j2_image[i] = 0;
  for(i = 0UL; i < (uint32)sizeof(j2_prologue); i++)
    j2_image[i] = j2_prologue[i];

  j2_image[J2_PATCH_F] = (uint8)c->f;
  j2_image[J2_PATCH_A] = (uint8)c->a;

  for(i = 0UL; i < J2_HEAD_LEN; i++)
    j2_image[J2_HEAD_ADDR + i] = j2_head_bytes[i];
  for(i = 0UL; i < J2_KEEP_LEN; i++)
    j2_image[J2_KEEP_ADDR + i] = j2_keep_bytes[i];
  for(i = 0UL; i < (uint32)sizeof(j2_epilogue); i++)
    j2_image[stop + i] = j2_epilogue[i];
}

static Err
j2_reference(const j2_case_t *c, uint32 stop, j2_state_t *out, uint32 *tstates)
{
  uint32 steps;
  uint16 pc;
  int32  over;
  int32  inside;

  j2_build_image(c,stop);
  z80_load_cartridge(j2_image,J2_IMAGE_SIZE);
  z80_reset();

  *tstates = 0UL;
  steps    = 0UL;

  for(;;)
    {
      if(z80_is_stopped())
        break;

      pc = z80_pc();
      inside = (((uint32)pc >= J2_HEAD_ADDR) && ((uint32)pc < stop)) ||
               (((uint32)pc >= J2_KEEP_ADDR) &&
                ((uint32)pc < (J2_KEEP_ADDR + J2_KEEP_LEN)) &&
                (stop != J2_KEEP_ADDR));

      over = z80_run(1);
      if(inside)
        *tstates += (uint32)over + 1UL;

      steps++;
      if(steps > 8192UL)
        {
          LOG_ERR(LOG_CAT_Z80,("j2 reference did not stop, pc=0x%04lx",
                               (unsigned long)z80_pc()));
          return -1;
        }
    }

  out->a  = (uint32)z80_peek((uint16)(J2_RESULTS + 0UL));
  out->f  = (uint32)z80_peek((uint16)(J2_RESULTS + 1UL));
  out->d  = (uint32)z80_peek((uint16)(J2_RESULTS + 2UL));
  out->e  = (uint32)z80_peek((uint16)(J2_RESULTS + 3UL));
  out->hl = (uint32)z80_peek((uint16)(J2_RESULTS + 4UL)) |
            ((uint32)z80_peek((uint16)(J2_RESULTS + 5UL)) << 8);
  out->sp = (uint32)z80_peek((uint16)(J2_RESULTS + 6UL)) |
            ((uint32)z80_peek((uint16)(J2_RESULTS + 7UL)) << 8);
  out->pc = stop;
  return 0;
}

static void
j2_prime(j2_state_t *in, const j2_case_t *c, uint32 quota)
{
  uint32 i;

  for(i = 0UL; i < 65536UL; i++)
    j2_guest[i] = 0;

  in->a     = c->a;
  in->f     = c->f;
  in->d     = J2_ENTRY_D;
  in->e     = J2_ENTRY_E;
  in->hl    = 0UL;
  in->sp    = J2_STACK_TOP;
  in->pc    = J2_HEAD_ADDR;
  in->quota = quota;
  in->mem   = j2_guest;
  in->table = j2_table;
  in->flags = j2_flags;
}

static Err
j2_compare(uint32 variant, uint32 stop, uint32 index,
           const j2_state_t *ref, const j2_state_t *got)
{
  uint32 addr;
  uint32 a;
  uint32 b;

#define J2_CHK(field,name)                                               \
  if(ref->field != got->field)                                           \
    {                                                                    \
      LOG_ERR(LOG_CAT_Z80,("j2 %s stop=0x%04lx case=%lu " name           \
                           " ref=0x%04lx got=0x%04lx",                   \
                           j2_names[variant],(unsigned long)stop,        \
                           (unsigned long)index,                         \
                           (unsigned long)ref->field,                    \
                           (unsigned long)got->field));                  \
      return -1;                                                         \
    }

  J2_CHK(a,"A") J2_CHK(f,"F") J2_CHK(d,"D") J2_CHK(e,"E")
  J2_CHK(hl,"HL") J2_CHK(sp,"SP") J2_CHK(pc,"PC")
#undef J2_CHK

  for(addr = J2_COUNTERS; addr < (J2_COUNTERS + 16UL); addr++)
    {
      a = (uint32)z80_peek((uint16)addr);
      b = (uint32)j2_guest[addr];
      if(a != b)
        {
          LOG_ERR(LOG_CAT_Z80,("j2 %s stop=0x%04lx mem 0x%04lx ref=0x%02lx got=0x%02lx",
                               j2_names[variant],(unsigned long)stop,
                               (unsigned long)addr,(unsigned long)a,(unsigned long)b));
          return -1;
        }
    }

  /*
   * The two bytes under the reference's stack pointer are the epilogue's own
   * PUSH AF, and where they land moves with the stop point.
   */
  for(addr = (J2_STACK_TOP - 12UL); addr < J2_STACK_TOP; addr++)
    {
      if((addr == (ref->sp - 1UL)) || (addr == (ref->sp - 2UL)))
        continue;

      a = (uint32)z80_peek((uint16)addr);
      b = (uint32)j2_guest[addr];
      if(a != b)
        {
          LOG_ERR(LOG_CAT_Z80,("j2 %s stop=0x%04lx stack 0x%04lx ref=0x%02lx got=0x%02lx",
                               j2_names[variant],(unsigned long)stop,
                               (unsigned long)addr,(unsigned long)a,(unsigned long)b));
          return -1;
        }
    }

  return 0;
}

Err
dynarec_j2_measure(void)
{
  j2_state_t    ref;
  j2_state_t    got;
  j2_state_t    in;
  j2_state_t    sink;
  uint32        v;
  uint32        i;
  uint32        k;
  uint32        tstates;
  uint32        pass_tstates;
  uint32        t0;
  uint32        usec;
  uint32        cyc_x100[J2_VARIANTS];
  uint32        per_x100;
  unsigned long ci, cf, pi, pf;

  if(j2_call == NULL)
    {
      LOG_WARN(LOG_CAT_Z80,("j2 not installed, nothing measured"));
      return -1;
    }

  if(j2_build_flag_tables() != 0)
    {
      LOG_ERR(LOG_CAT_Z80,("j2 flag tables refused, no figure published"));
      return -1;
    }

  /*
   * Every variant, at every boundary. They are four writings of one region and
   * they have to agree with the core and therefore with each other; a lever
   * that changed a result would not be an optimisation.
   */
  pass_tstates = 0UL;

  for(v = 0UL; v < J2_VARIANTS; v++)
    {
      for(k = 0UL; k < J2_STOP_COUNT; k++)
        {
          for(i = 0UL; i < J2_CASE_COUNT; i++)
            {
              if(j2_reference(&j2_cases[i],j2_stops[k],&ref,&tstates) != 0)
                return -1;

              /*
               * The chain is stopped at the boundary by handing it exactly the
               * T-states the core spent reaching it. Every block ends by
               * spending the quota, so the count falls to zero at that
               * boundary and at no earlier one -- and unlike a hole in the
               * table, a quota is honoured by a link that branches direct.
               */
              j2_fill_table(v);
              j2_prime(&in,&j2_cases[i],tstates);
              j2_call(&in,&got);

              if(j2_compare(v,j2_stops[k],i,&ref,&got) != 0)
                {
                  LOG_ERR(LOG_CAT_Z80,("j2 a variant disagrees with the core, no figure published"));
                  return -1;
                }

              if((v == 0UL) && (i == 0UL) && (j2_stops[k] == J2_STOP_ADDR))
                pass_tstates = tstates;
            }
        }
    }

  LOG_INFO(LOG_CAT_Z80,("j2 checked variants=%lu stops=%lu cases=%lu all agree, pass tstates=%lu",
                        (unsigned long)J2_VARIANTS,(unsigned long)J2_STOP_COUNT,
                        (unsigned long)J2_CASE_COUNT,(unsigned long)pass_tstates));

  if(pass_tstates == 0UL)
    {
      LOG_ERR(LOG_CAT_Z80,("j2 pass cost is zero, no figure published"));
      return -1;
    }

  /*
   * One run with time to spare, per variant, which is the only thing that
   * exercises the table's empty entry: with a calibrated quota the chain
   * always stops on the clock before it ever reads one. It has to come to rest
   * where the region runs out of translation, and nowhere else.
   */
  for(v = 0UL; v < J2_VARIANTS; v++)
    {
      j2_fill_table(v);
      j2_prime(&in,&j2_cases[0],J2_QUOTA_WIDE);
      j2_call(&in,&got);
      if(got.pc != J2_STOP_ADDR)
        {
          LOG_ERR(LOG_CAT_Z80,("j2 %s with time to spare stopped at 0x%04lx, not 0x%04lx",
                               j2_names[v],(unsigned long)got.pc,
                               (unsigned long)J2_STOP_ADDR));
          return -1;
        }
    }
  LOG_INFO(LOG_CAT_Z80,("j2 all variants come to rest at 0x%04lx when given time",
                        (unsigned long)J2_STOP_ADDR));

  /*
   * The quota path, proved rather than assumed: a budget under one block's
   * cost has to stop the chain on a boundary and hand back that boundary.
   */
  j2_fill_table(J2_VARIANTS - 1UL);
  j2_prime(&in,&j2_cases[0],1UL);
  j2_call(&in,&got);
  if(got.pc != J2_KEEP_ADDR)
    {
      LOG_ERR(LOG_CAT_Z80,("j2 short quota stopped at 0x%04lx, not on the first boundary",
                           (unsigned long)got.pc));
      return -1;
    }
  LOG_INFO(LOG_CAT_Z80,("j2 short quota stops on a boundary pc=0x%04lx",
                        (unsigned long)got.pc));

  /* The four timed runs, same region, same tours, one lever apart. */
  for(v = 0UL; v < J2_VARIANTS; v++)
    {
      j2_fill_table(v);
      j2_prime(&in,&j2_cases[0],J2_QUOTA_WIDE);

      t0 = sys_usec();
      for(i = 0UL; i < J2_ITERATIONS; i++)
        j2_call(&in,&sink);
      usec = sys_usec() - t0;

      cyc_x100[v] = (usec * (J2_CYCLES_PER_MS / 100UL)) / (J2_ITERATIONS / 10UL);
      per_x100    = cyc_x100[v] / pass_tstates;

      ci = (unsigned long)(cyc_x100[v] / 100UL);
      cf = (unsigned long)(cyc_x100[v] % 100UL);
      pi = (unsigned long)(per_x100 / 100UL);
      pf = (unsigned long)(per_x100 % 100UL);

      LOG_INFO(LOG_CAT_Z80,("j2 verdict %s c/tstate=%lu.%02lu cycles=%lu.%02lu tstates=%lu iters=%lu",
                            j2_names[v],pi,pf,ci,cf,
                            (unsigned long)pass_tstates,
                            (unsigned long)J2_ITERATIONS));
    }

  /*
   * And what the four figures say about the split, which is the question J1
   * left open and the reason there are four of them.
   */
  LOG_INFO(LOG_CAT_Z80,("j2 split: link saves %lu, flags save %lu, both save %lu (hundredths of a cycle per pass)",
                        (unsigned long)(cyc_x100[0] - cyc_x100[1]),
                        (unsigned long)(cyc_x100[0] - cyc_x100[2]),
                        (unsigned long)(cyc_x100[0] - cyc_x100[3])));

  return 0;
}

#endif /* SMS_DYNAREC_J2 */
