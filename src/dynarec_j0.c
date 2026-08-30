#include "dynarec_j0.h"

#if SMS_DYNAREC_J0

#include "sys.h"
#include "log.h"
#include "z80.h"

/*
 * ---------------------------------------------------------------------------
 * The block under measurement, as bytes.
 *
 * These twenty-three bytes are keep_yxf of the deposited test image, read out
 * of it at 0x2B04 and written here so that this file states what it measures
 * rather than pointing at an offset in an array of sixty-five thousand. They
 * are the same bytes: the reference run below installs them at the same
 * address and the core executes them, so a copy that had drifted would show up
 * as a run that goes somewhere else.
 *
 * The block has to sit at 0x2B04 and nowhere else. Its CALL at 0x2B09 names an
 * absolute target inside itself, so moving the block moves the target with it
 * and the call would land in whatever else is there.
 * ---------------------------------------------------------------------------
 */
static const uint8 j0_block_bytes[] =
{
  0xF5,             /* 2B04  PUSH AF     */
  0xD1,             /* 2B05  POP DE      */
  0x7B,             /* 2B06  LD A,E      */
  0xE6, 0x20,       /* 2B07  AND $20     */
  0xCD, 0x0F, 0x2B, /* 2B09  CALL $2B0F  */
  0x7B,             /* 2B0C  LD A,E      */
  0xE6, 0x08,       /* 2B0D  AND $08     */
  0x28, 0x07,       /* 2B0F  JR Z,$2B18  */
  0x34,             /* 2B11  INC (HL)    */
  0x23,             /* 2B12  INC HL      */
  0x20, 0x04,       /* 2B13  JR NZ,$2B19 */
  0x34,             /* 2B15  INC (HL)    */
  0x18, 0x01,       /* 2B16  JR $2B19    */
  0x23,             /* 2B18  INC HL      */
  0x23,             /* 2B19  INC HL      */
  0xC9              /* 2B1A  RET         */
};

#define J0_BLOCK_ADDR  0x2B04UL
#define J0_BLOCK_LEN   ((uint32)sizeof(j0_block_bytes))
#define J0_BLOCK_LAST  (J0_BLOCK_ADDR + J0_BLOCK_LEN - 1UL)
#define J0_IMAGE_SIZE  (J0_BLOCK_ADDR + J0_BLOCK_LEN)

/*
 * Where the emulated program keeps things. The counters are what the block
 * increments, the results are where the epilogue below parks the registers so
 * that they can be read back, and the stack sits at the top of the RAM the
 * cartridge window leaves alone.
 */
#define J0_COUNTERS    0xC000UL
#define J0_RESULTS     0xC100UL
#define J0_STACK_TOP   0xDFF0UL

/*
 * The state of the stack at the moment the block is entered, which the
 * translated run has to be handed because it does not execute the prologue
 * that produces it: the call into the block has pushed a return address, so
 * the stack pointer is two below its top and those two bytes hold that
 * address.
 */
#define J0_ENTRY_SP    (J0_STACK_TOP - 2UL)

/*
 * ---------------------------------------------------------------------------
 * The reference program: a prologue that builds a known state, the block, and
 * an epilogue that puts the registers somewhere they can be read.
 *
 * It exists because the core keeps its state to itself. There is no way in
 * from outside, and there should not be: the way to see a register is to make
 * the emulated processor store it, which is what any real program would do.
 *
 * It opens by clearing the counters, and that is not tidiness. Installing a
 * cartridge writes the bottom of the address space and deliberately leaves the
 * RAM above it alone, so what one case leaves in those bytes is still there
 * when the next one starts -- the second case would then be compared against a
 * counter the first one had already moved. Only the emulated program can clear
 * emulated RAM, so it does it itself.
 * ---------------------------------------------------------------------------
 */
static const uint8 j0_prologue[] =
{
  0xAF,                   /* XOR A          -- clear what is not seeded      */
  0x32, 0x01, 0xC0,       /* LD ($C001),A                                     */
  0x32, 0x03, 0xC0,       /* LD ($C003),A                                     */
  0x3E, 0x00,             /* LD A,seed      -- patched, index 8               */
  0x32, 0x00, 0xC0,       /* LD ($C000),A   -- first counter                  */
  0x3E, 0x00,             /* LD A,seed2     -- patched, index 13              */
  0x32, 0x02, 0xC0,       /* LD ($C002),A   -- second counter                 */
  0x31, 0xF0, 0xDF,       /* LD SP,$DFF0                                      */
  0x21, 0x00, 0xC0,       /* LD HL,$C000                                      */
  0x01, 0x00, 0x00,       /* LD BC,nn       -- C=F, B=A, indices 24 and 25    */
  0xC5,                   /* PUSH BC                                          */
  0xF1,                   /* POP AF         -- A and F now set                */
  0xCD, 0x04, 0x2B        /* CALL $2B04                                       */
};

#define J0_PATCH_SEED  8
#define J0_PATCH_SEED2 13
#define J0_PATCH_F     24
#define J0_PATCH_A     25

/* Where the CALL above returns to, and therefore what the block's RET pops. */
#define J0_RET_ADDR    ((uint32)sizeof(j0_prologue))

/*
 * Stores A, F, D, E, HL and SP, then stops on an opcode the core does not
 * implement -- the way this project ends an emulated run on purpose.
 *
 * Order matters at the top: the store of A carries no flag, so F is still the
 * one the block left when it is pushed on the next line.
 */
static const uint8 j0_epilogue[] =
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
 * The starting states the check runs through.
 *
 * Chosen for the paths they take rather than for variety. The block branches
 * three times, twice on the zero left by a mask of one bit of F and once on
 * the carry out of the low byte of the counter, so the set below has to reach
 * both sides of all three. The last two seed the counter at 0xFF, which is
 * what makes the increment wrap and the third branch fall the other way -- the
 * case that only shows up once in two hundred and fifty six and that a
 * translation gets wrong quietly.
 * ---------------------------------------------------------------------------
 */
typedef struct
{
  uint32 a;
  uint32 f;
  uint32 seed;   /* the first counter, at 0xC000  */
  uint32 seed2;  /* the second one, at 0xC002     */
} j0_case_t;

static const j0_case_t j0_cases[] =
{
  { 0x00, 0x00, 0x00, 0x00 },  /* neither bit set: both masks give zero      */
  { 0xFF, 0xFF, 0x00, 0x00 },  /* both bits set: both masks give non zero    */
  { 0x55, 0x20, 0x00, 0x00 },  /* bit 5 only                                 */
  { 0xAA, 0x08, 0x00, 0x00 },  /* bit 3 only                                 */
  { 0x12, 0x28, 0x00, 0x00 },  /* both, other bits of F set as well          */
  { 0xFF, 0xFF, 0xFF, 0x00 },  /* first counter wraps: the carry path        */
  { 0x00, 0x28, 0xFF, 0x00 },  /* the carry path from the other side         */
  { 0x12, 0x28, 0x00, 0x0F },  /* last increment crosses a nibble: H is set  */
  { 0x12, 0x28, 0x00, 0xFF },  /* last increment wraps: H and Z both set     */
  { 0x12, 0x28, 0x0F, 0x0F }   /* both counters cross a nibble               */
};

#define J0_CASE_COUNT ((uint32)(sizeof(j0_cases) / sizeof(j0_cases[0])))

/*
 * Which of them the timed loop runs, and it is not a free choice.
 *
 * The path through this block depends on the state it is entered with, and the
 * two shortest paths skip the read-modify-write of memory entirely -- the one
 * post the study calls tight, and the one a figure taken without it would be
 * silent about. The case below is the one that takes both increments, so the
 * measurement covers the dear path rather than the cheap one. A Go decided on
 * the cheap path would be a Go decided on the wrong block.
 */
#define J0_TIMED_CASE  4UL

/*
 * How many times the timed loops call the block.
 *
 * Large enough that the microsecond clock is not what is being measured: at
 * the cost this block is expected to have, a hundred thousand calls take
 * somewhere over a second, so one tick of the clock is a millionth of the
 * window rather than a fraction of it.
 */
#define J0_ITERATIONS  100000UL

/*
 * The ARM60 runs at 12.5 MHz, so a microsecond is twelve and a half cycles.
 * Stated the way the rest of this project states it -- cycles per millisecond
 * -- so that a figure from here and a figure from the frame loop are in the
 * same unit and can be put side by side.
 */
#define J0_CYCLES_PER_MS 12500UL

/*
 * The translated code, and the boundary sequence with nothing between it.
 * Declared as bytes because what this file wants from them is where they are
 * and how long they are, not to call them where they sit -- they are called
 * from the buffer they get copied into.
 */
extern const uint8 dynarec_j0_block[];
extern const uint8 dynarec_j0_block_end[];
extern const uint8 dynarec_j0_stub[];
extern const uint8 dynarec_j0_stub_end[];

typedef void (*j0_fn_t)(const j0_state_t *in, j0_state_t *out);

/*
 * A pointer to code and a pointer to bytes are the same address here and the
 * union says so once, rather than a cast saying it at every use. There is no
 * machine underneath on which they differ: this is one target, with no memory
 * management unit and no separate code space.
 */
typedef union
{
  uint8   *bytes;
  j0_fn_t  fn;
} j0_cast_t;

static uint8    *j0_code     = NULL;   /* the buffer both routines live in   */
static uint8    *j0_guest    = NULL;   /* the translated run's address space */
static uint8    *j0_image    = NULL;   /* the reference program, as an image */
static j0_fn_t   j0_call_block = NULL;
static j0_fn_t   j0_call_stub  = NULL;

Err
dynarec_j0_install(void)
{
  uint32    block_len;
  uint32    stub_len;
  uint32    stub_off;
  uint32    i;
  j0_cast_t cast;

  block_len = (uint32)(dynarec_j0_block_end - dynarec_j0_block);
  stub_len  = (uint32)(dynarec_j0_stub_end  - dynarec_j0_stub);

  /*
   * A length that makes no sense means the two markers no longer bracket what
   * they are named after -- a reordering by the assembler or the linker. It is
   * caught here because the failure it would otherwise produce is a jump into
   * the middle of something, which says nothing about its cause.
   */
  if((block_len == 0UL) || (block_len > 4096UL) ||
     (stub_len  == 0UL) || (stub_len  > 4096UL))
    {
      LOG_ERR(LOG_CAT_Z80,("j0 install failed: bad span block=%lu stub=%lu",
                           (unsigned long)block_len,
                           (unsigned long)stub_len));
      return -1;
    }

  /* The stub starts on a word, because instructions are fetched by word. */
  stub_off = (block_len + 3UL) & ~3UL;

  j0_code = (uint8 *)sys_alloc("j0_code",
                               (int32)(stub_off + stub_len),
                               MEMTYPE_DRAM | MEMTYPE_FILL);
  j0_guest = (uint8 *)sys_alloc("j0_guest",
                                (int32)65536UL,
                                MEMTYPE_DRAM | MEMTYPE_FILL);
  j0_image = (uint8 *)sys_alloc("j0_image",
                                (int32)J0_IMAGE_SIZE,
                                MEMTYPE_DRAM | MEMTYPE_FILL);

  if((j0_code == NULL) || (j0_guest == NULL) || (j0_image == NULL))
    {
      /* sys_alloc has already said which one and why. */
      LOG_ERR(LOG_CAT_Z80,("j0 install failed: no memory, mock-up absent"));
      return -1;
    }

  /*
   * The copy is the whole point of the exercise, not a detail of it: this
   * machine has no instruction cache, so bytes written here are executable on
   * the next line with nothing asked of the system in between. Every other
   * machine would need a synchronisation call right here.
   */
  for(i = 0UL; i < block_len; i++)
    j0_code[i] = dynarec_j0_block[i];
  for(i = 0UL; i < stub_len; i++)
    j0_code[stub_off + i] = dynarec_j0_stub[i];

  cast.bytes    = j0_code;
  j0_call_block = cast.fn;
  cast.bytes    = j0_code + stub_off;
  j0_call_stub  = cast.fn;

  LOG_INFO(LOG_CAT_Z80,("j0 installed block=%lu stub=%lu bytes at 0x%08lx",
                        (unsigned long)block_len,
                        (unsigned long)stub_len,
                        (unsigned long)j0_code));
  return 0;
}

/*
 * Builds the reference program for one starting state.
 */
static void
j0_build_image(const j0_case_t *c)
{
  uint32 i;

  for(i = 0UL; i < J0_IMAGE_SIZE; i++)
    j0_image[i] = 0;

  for(i = 0UL; i < (uint32)sizeof(j0_prologue); i++)
    j0_image[i] = j0_prologue[i];

  j0_image[J0_PATCH_SEED]  = (uint8)c->seed;
  j0_image[J0_PATCH_SEED2] = (uint8)c->seed2;
  j0_image[J0_PATCH_F]    = (uint8)c->f;
  j0_image[J0_PATCH_A]    = (uint8)c->a;

  for(i = 0UL; i < (uint32)sizeof(j0_epilogue); i++)
    j0_image[J0_RET_ADDR + i] = j0_epilogue[i];

  for(i = 0UL; i < J0_BLOCK_LEN; i++)
    j0_image[J0_BLOCK_ADDR + i] = j0_block_bytes[i];
}

/*
 * Runs the reference program to its stop, and answers with the state it left
 * and what the block alone cost in T-states.
 *
 * The cost is taken instruction by instruction: a quota of one T-state makes
 * the core run exactly one instruction and hand back what that instruction
 * spent past the quota, so its cost is that plus one. Only the instructions
 * executed inside the block are added up, which is what makes the figure the
 * block's and not the whole program's.
 */
static Err
j0_reference(const j0_case_t *c, j0_state_t *out, uint32 *tstates)
{
  uint32 steps;
  uint16 pc;
  int32  over;

  j0_build_image(c);
  z80_load_cartridge(j0_image, J0_IMAGE_SIZE);
  z80_reset();

  *tstates = 0UL;
  steps    = 0UL;

  for(;;)
    {
      if(z80_is_stopped())
        break;

      pc = z80_pc();
      over = z80_run(1);

      if(((uint32)pc >= J0_BLOCK_ADDR) && ((uint32)pc <= J0_BLOCK_LAST))
        *tstates += (uint32)over + 1UL;

      steps++;
      if(steps > 4096UL)
        {
          LOG_ERR(LOG_CAT_Z80,("j0 reference did not stop, pc=0x%04lx",
                               (unsigned long)z80_pc()));
          return -1;
        }
    }

  out->a  = (uint32)z80_peek((uint16)(J0_RESULTS + 0UL));
  out->f  = (uint32)z80_peek((uint16)(J0_RESULTS + 1UL));
  out->d  = (uint32)z80_peek((uint16)(J0_RESULTS + 2UL));
  out->e  = (uint32)z80_peek((uint16)(J0_RESULTS + 3UL));
  out->hl = (uint32)z80_peek((uint16)(J0_RESULTS + 4UL)) |
            ((uint32)z80_peek((uint16)(J0_RESULTS + 5UL)) << 8);
  out->sp = (uint32)z80_peek((uint16)(J0_RESULTS + 6UL)) |
            ((uint32)z80_peek((uint16)(J0_RESULTS + 7UL)) << 8);

  /*
   * The block returned where the call came from -- the epilogue having run at
   * all is the proof, since that is the only address it could have reached.
   */
  out->pc  = J0_RET_ADDR;
  out->mem = NULL;

  return 0;
}

/*
 * Puts the translated run's address space into the state the reference program
 * had reached when it entered the block, and runs the block once.
 */
static void
j0_translated(const j0_case_t *c, j0_state_t *out)
{
  j0_state_t in;
  uint32     i;

  for(i = 0UL; i < 65536UL; i++)
    j0_guest[i] = 0;

  j0_guest[J0_COUNTERS]      = (uint8)c->seed;
  j0_guest[J0_COUNTERS + 2UL]= (uint8)c->seed2;
  j0_guest[J0_ENTRY_SP]      = (uint8)(J0_RET_ADDR & 0xFFUL);
  j0_guest[J0_ENTRY_SP + 1UL]= (uint8)((J0_RET_ADDR >> 8) & 0xFFUL);

  in.a   = c->a;
  in.f   = c->f;
  in.d   = 0UL;   /* overwritten by the POP DE the block opens with */
  in.e   = 0UL;
  in.hl  = J0_COUNTERS;
  in.sp  = J0_ENTRY_SP;
  in.pc  = 0UL;
  in.mem = j0_guest;

  j0_call_block(&in, out);
}

/*
 * Compares what the two executors left, and names the first thing they
 * disagree about.
 */
static Err
j0_compare(uint32 index, const j0_state_t *ref, const j0_state_t *got)
{
  uint32 addr;
  uint32 a;
  uint32 b;

  if(ref->a != got->a)
    { LOG_ERR(LOG_CAT_Z80,("j0 case=%lu A ref=0x%02lx got=0x%02lx",
        (unsigned long)index,(unsigned long)ref->a,(unsigned long)got->a)); return -1; }
  if(ref->f != got->f)
    { LOG_ERR(LOG_CAT_Z80,("j0 case=%lu F ref=0x%02lx got=0x%02lx",
        (unsigned long)index,(unsigned long)ref->f,(unsigned long)got->f)); return -1; }
  if(ref->d != got->d)
    { LOG_ERR(LOG_CAT_Z80,("j0 case=%lu D ref=0x%02lx got=0x%02lx",
        (unsigned long)index,(unsigned long)ref->d,(unsigned long)got->d)); return -1; }
  if(ref->e != got->e)
    { LOG_ERR(LOG_CAT_Z80,("j0 case=%lu E ref=0x%02lx got=0x%02lx",
        (unsigned long)index,(unsigned long)ref->e,(unsigned long)got->e)); return -1; }
  if(ref->hl != got->hl)
    { LOG_ERR(LOG_CAT_Z80,("j0 case=%lu HL ref=0x%04lx got=0x%04lx",
        (unsigned long)index,(unsigned long)ref->hl,(unsigned long)got->hl)); return -1; }
  if(ref->sp != got->sp)
    { LOG_ERR(LOG_CAT_Z80,("j0 case=%lu SP ref=0x%04lx got=0x%04lx",
        (unsigned long)index,(unsigned long)ref->sp,(unsigned long)got->sp)); return -1; }
  if(ref->pc != got->pc)
    { LOG_ERR(LOG_CAT_Z80,("j0 case=%lu PC ref=0x%04lx got=0x%04lx",
        (unsigned long)index,(unsigned long)ref->pc,(unsigned long)got->pc)); return -1; }

  /*
   * The counters the block increments, and the stack it pushed on. Registers
   * agreeing while memory does not is exactly the shape a wrong translation
   * takes when it keeps a value in a register that the original wrote out.
   */
  for(addr = J0_COUNTERS; addr < (J0_COUNTERS + 8UL); addr++)
    {
      a = (uint32)z80_peek((uint16)addr);
      b = (uint32)j0_guest[addr];
      if(a != b)
        {
          LOG_ERR(LOG_CAT_Z80,("j0 case=%lu mem 0x%04lx ref=0x%02lx got=0x%02lx",
                               (unsigned long)index,(unsigned long)addr,
                               (unsigned long)a,(unsigned long)b));
          return -1;
        }
    }

  /*
   * Stopping two bytes below the top, and the two left out are not an
   * oversight: the epilogue pushes AF to get at F, which lands exactly there
   * and overwrites what the block had left. Everything the block pushed sits
   * below that line and is compared.
   */
  for(addr = (J0_STACK_TOP - 8UL); addr < (J0_STACK_TOP - 2UL); addr++)
    {
      a = (uint32)z80_peek((uint16)addr);
      b = (uint32)j0_guest[addr];
      if(a != b)
        {
          LOG_ERR(LOG_CAT_Z80,("j0 case=%lu stack 0x%04lx ref=0x%02lx got=0x%02lx",
                               (unsigned long)index,(unsigned long)addr,
                               (unsigned long)a,(unsigned long)b));
          return -1;
        }
    }

  return 0;
}

Err
dynarec_j0_measure(void)
{
  j0_state_t ref;
  j0_state_t got;
  j0_state_t in;
  j0_state_t sink;
  uint32     i;
  uint32     tstates;
  uint32     block_tstates;
  uint32     t0;
  uint32     usec_block;
  uint32     usec_stub;
  uint32     cyc_block_x100;
  uint32     cyc_stub_x100;
  uint32     cyc_body_x100;
  uint32     per_tstate_x100;
  uint32     body_per_tstate_x100;
  unsigned long vi, vf, bi, bf, ci, cf, ei, ef;

  if(j0_call_block == NULL)
    {
      LOG_WARN(LOG_CAT_Z80,("j0 not installed, nothing measured"));
      return -1;
    }

  /*
   * Correctness first, and the figure only if it holds. A translated block
   * that is timed but not checked produces a number that reads exactly like a
   * real one, which makes it worse than no number at all.
   */
  block_tstates = 0UL;
  for(i = 0UL; i < J0_CASE_COUNT; i++)
    {
      if(j0_reference(&j0_cases[i],&ref,&tstates) != 0)
        return -1;

      j0_translated(&j0_cases[i],&got);

      if(j0_compare(i,&ref,&got) != 0)
        {
          LOG_ERR(LOG_CAT_Z80,("j0 translation disagrees with the core, no figure published"));
          return -1;
        }

      /*
       * The path through the block depends on the state, so its cost does too.
       * The T-states kept are the ones of the case the timed loop runs, or the
       * two figures would be about two different paths.
       */
      if(i == J0_TIMED_CASE)
        block_tstates = tstates;
    }

  LOG_INFO(LOG_CAT_Z80,("j0 checked cases=%lu all agree, block tstates=%lu",
                        (unsigned long)J0_CASE_COUNT,
                        (unsigned long)block_tstates));

  if(block_tstates == 0UL)
    {
      LOG_ERR(LOG_CAT_Z80,("j0 block cost zero T-states, no figure published"));
      return -1;
    }

  /*
   * The timed loops. Both read their input from the same struct and write
   * their output to another, so every call starts from the same registers and
   * nothing has to be re-primed between two of them.
   *
   * What the block leaves in memory does accumulate -- the counter it
   * increments climbs, and wraps once every two hundred and fifty six calls,
   * which takes the other branch that once. It is a part in two hundred and
   * fifty six of the window and it is the block's own behaviour, not an
   * artefact of measuring it.
   */
  for(i = 0UL; i < 65536UL; i++)
    j0_guest[i] = 0;
  j0_guest[J0_COUNTERS]       = (uint8)j0_cases[J0_TIMED_CASE].seed;
  j0_guest[J0_COUNTERS + 2UL] = (uint8)j0_cases[J0_TIMED_CASE].seed2;
  j0_guest[J0_ENTRY_SP]       = (uint8)(J0_RET_ADDR & 0xFFUL);
  j0_guest[J0_ENTRY_SP + 1UL] = (uint8)((J0_RET_ADDR >> 8) & 0xFFUL);

  in.a   = j0_cases[J0_TIMED_CASE].a;
  in.f   = j0_cases[J0_TIMED_CASE].f;
  in.d   = 0UL;
  in.e   = 0UL;
  in.hl  = J0_COUNTERS;
  in.sp  = J0_ENTRY_SP;
  in.pc  = 0UL;
  in.mem = j0_guest;

  t0 = sys_usec();
  for(i = 0UL; i < J0_ITERATIONS; i++)
    j0_call_block(&in,&sink);
  usec_block = sys_usec() - t0;

  t0 = sys_usec();
  for(i = 0UL; i < J0_ITERATIONS; i++)
    j0_call_stub(&in,&sink);
  usec_stub = sys_usec() - t0;

  /*
   * Cycles per call, in hundredths. The division is arranged so that nothing
   * overflows on the way: the microseconds are multiplied by an eighth of the
   * cycles per millisecond and divided by an eighth of the iteration count,
   * which is the same ratio in numbers a third of the size.
   */
  cyc_block_x100 = (usec_block * (J0_CYCLES_PER_MS / 100UL)) / (J0_ITERATIONS / 10UL);
  cyc_stub_x100  = (usec_stub  * (J0_CYCLES_PER_MS / 100UL)) / (J0_ITERATIONS / 10UL);

  cyc_body_x100 = 0UL;
  if(cyc_block_x100 > cyc_stub_x100)
    cyc_body_x100 = cyc_block_x100 - cyc_stub_x100;

  per_tstate_x100      = cyc_block_x100 / block_tstates;
  body_per_tstate_x100 = cyc_body_x100  / block_tstates;

  /*
   * The verdict, in one line, in the unit the decision is written in. The
   * first figure is the one that decides: it carries the block boundary and
   * the call, which a real dispatcher pays too.
   */
  vi = (unsigned long)(per_tstate_x100 / 100UL);
  vf = (unsigned long)(per_tstate_x100 % 100UL);
  bi = (unsigned long)(body_per_tstate_x100 / 100UL);
  bf = (unsigned long)(body_per_tstate_x100 % 100UL);
  ci = (unsigned long)(cyc_block_x100 / 100UL);
  cf = (unsigned long)(cyc_block_x100 % 100UL);
  ei = (unsigned long)(cyc_stub_x100 / 100UL);
  ef = (unsigned long)(cyc_stub_x100 % 100UL);

  LOG_INFO(LOG_CAT_Z80,
           ("j0 verdict c/tstate=%lu.%02lu body=%lu.%02lu cycles=%lu.%02lu "
            "boundary=%lu.%02lu tstates=%lu iters=%lu",
            vi,vf,bi,bf,ci,cf,ei,ef,
            (unsigned long)block_tstates,(unsigned long)J0_ITERATIONS));

  return 0;
}

#endif /* SMS_DYNAREC_J0 */
