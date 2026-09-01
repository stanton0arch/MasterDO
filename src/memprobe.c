#include "memprobe.h"

#if SMS_MEM_PROBE

#include "sys.h"
#include "log.h"
#include "sms.h"

/*
 * The guard common.h could not carry. Every figure this probe takes leaves
 * through one LOG_INFO, which is also the only reader of the sum the timed
 * loops feed: below LOG_LVL_INFO that line is preprocessed away, nothing reads
 * the sum, and the compiler may delete the very accesses being timed. The
 * level names belong to log.h, so the test belongs here -- written in
 * common.h, which does not include log.h, it would compare against a name the
 * preprocessor has never seen and pass everything.
 */
#if (LOG_LVL_INFO) > (LOG_LEVEL)
#error "SMS_MEM_PROBE needs LOG_LEVEL at LOG_LVL_INFO or above: its figures are INFO lines"
#endif

/*
 * ---------------------------------------------------------------------------
 * The buffers the timed loops walk over.
 *
 * Sixty-four kilobytes is not a round number picked for looks. The scattered
 * shape has to leave the open DRAM page on every single access or it measures
 * page mode and calls it a scattered access; a stride of 1031 words is 4124
 * bytes, past any page this memory system is likely to have, and 1031 being
 * odd it is coprime with 16384 words, so the walk visits every word of the
 * buffer before it comes back to one -- no accidental short orbit, no
 * accidental page reuse. That claim is not left as a claim: the report holds
 * the stride against the page size the system admits to, and says so when it
 * does not clear it.
 *
 * The VRAM buffer is the same size and exists to answer one question the
 * allocation sites of the render raise: two of the four video blocks are
 * asked for as MEMTYPE_ANY, which is zero (include/3do/mem.h:62), so nothing
 * says they are not in VRAM. If this block cannot be had, its shapes are not
 * measured and say so; the DRAM figures do not depend on it.
 * ---------------------------------------------------------------------------
 */
#define MEMPROBE_WORDS      16384UL
#define MEMPROBE_BYTES      (MEMPROBE_WORDS * 4UL)
#define MEMPROBE_WORD_MASK  (MEMPROBE_WORDS - 1UL)
#define MEMPROBE_BYTE_MASK  (MEMPROBE_BYTES - 1UL)

/*
 * How many times a shape runs inside one timed window.
 *
 * The window has to be long enough that the clock is not what is being
 * measured. sys_usec costs some tens of microseconds and is read twice, so the
 * clock's share of a window is about eighty microseconds; the shortest window
 * here is the reference loop, six instructions a turn, which at two hundred
 * thousand turns lands near a tenth of a second. The clock is therefore around
 * a thousandth of the shortest window and far less of the others -- small
 * enough to ignore, and worth stating as the number it is rather than as a
 * flattering one.
 */
#define MEMPROBE_ITERATIONS 200000UL

/*
 * Every shape is timed twice and both readings are published.
 *
 * Not for an average -- the readings are not averaged, and the smaller one is
 * what the net figure uses. It is because this clock returns a sample wrong by
 * whole seconds about once in seven windows. Two readings side by side make
 * that visible instead of letting it become the result, and the report says so
 * itself rather than leaving the reader to notice: a pair that disagrees by
 * more than an eighth is called out on its own line.
 */
#define MEMPROBE_PASSES 2UL

/*
 * The report line prints usec[0] and usec[1] by hand, because a figure the
 * reader has to trust is a figure they should be able to see both halves of.
 * That spelling and this constant have to move together, and this is what says
 * so: at one pass the line would read past the array, at three it would stop
 * publishing a reading the outlier argument rests on.
 */
#if MEMPROBE_PASSES != 2
#error "the report line prints two readings by hand: update it with MEMPROBE_PASSES"
#endif

/* The ARM60 runs at 12.5 MHz, so a microsecond is twelve and a half cycles. */
#define MEMPROBE_CYCLES_PER_MS 12500UL

/*
 * Above this the conversion below would overflow a 32 bit multiply and hand
 * back a small, entirely plausible-looking number. Thirty seconds is far past
 * any window this probe can produce honestly and far short of the wrap, so a
 * window over it is the clock having lied -- including the case where the
 * first sample was the wrong one and the unsigned subtraction came back near
 * the top of its range.
 */
#define MEMPROBE_USEC_MAX 30000000UL

/*
 * Word strides in words, byte strides in bytes.
 *
 * The byte stride is 4127 and not 1031 * 4 = 4124, and the difference matters.
 * A walk has to be coprime with its buffer to cover it instead of orbiting a
 * quarter of it, which forces an odd number, which in turn means consecutive
 * byte accesses land at different offsets within their word. That is a second
 * property changed, against this file's own rule of one at a time, and it is
 * accepted here for a stated reason: the ARM60 addresses bytes directly
 * (docs/3do/arm60.md:1003-1007), so where a byte sits inside its word costs
 * nothing. An even stride would have cost a quarter of the buffer, which is
 * not free at all.
 */
#define MEMPROBE_W_SCAT 1031UL
#define MEMPROBE_B_SCAT 4127UL

#define MEMPROBE_KIND_NONE 0UL
#define MEMPROBE_KIND_RDW  1UL
#define MEMPROBE_KIND_WRW  2UL
#define MEMPROBE_KIND_RDB  3UL
#define MEMPROBE_KIND_WRB  4UL

typedef struct
{
  const char *name;
  uint32      kind;
  uint32      stride;
  uint32      vram;
} memprobe_shape_t;

/*
 * The shapes, and the whole design of this probe is in this table.
 *
 * Three strides run through the same walk and the same compiled loop: zero
 * reads one word for ever, which is what a spilled register is; one walks
 * forward, which is what page mode was built for; the scattered one leaves the
 * page every time. Nothing differs between those three runs but a number that
 * arrives as a function argument, so whatever separates their timings is the
 * memory system and cannot be anything else.
 *
 * That is also why the strides live in a table rather than at the call sites.
 * Written as literals the compiler would see stride zero for what it is, hoist
 * the load out of the loop, and hand back the cost of an empty loop under the
 * name of a memory access -- a green figure that means nothing, which is the
 * failure this project has already met twice. The table is the defence; the
 * zero-net warning below is what catches the defence failing.
 *
 * The byte write is here because the render does one on its hot path: the row
 * decode writes its eight index bytes one at a time (src/vdp.c:829-831).
 */
static const memprobe_shape_t memprobe_shapes[] =
{
  { "loop",      MEMPROBE_KIND_NONE, 1UL,             0UL },
  { "rdw.hot",   MEMPROBE_KIND_RDW,  0UL,             0UL },
  { "rdw.seq",   MEMPROBE_KIND_RDW,  1UL,             0UL },
  { "rdw.scat",  MEMPROBE_KIND_RDW,  MEMPROBE_W_SCAT, 0UL },
  { "wrw.hot",   MEMPROBE_KIND_WRW,  0UL,             0UL },
  { "wrw.seq",   MEMPROBE_KIND_WRW,  1UL,             0UL },
  { "wrw.scat",  MEMPROBE_KIND_WRW,  MEMPROBE_W_SCAT, 0UL },
  { "rdb.hot",   MEMPROBE_KIND_RDB,  0UL,             0UL },
  { "rdb.seq",   MEMPROBE_KIND_RDB,  1UL,             0UL },
  { "rdb.scat",  MEMPROBE_KIND_RDB,  MEMPROBE_B_SCAT, 0UL },
  { "wrb.seq",   MEMPROBE_KIND_WRB,  1UL,             0UL },
  { "wrb.scat",  MEMPROBE_KIND_WRB,  MEMPROBE_B_SCAT, 0UL },
  { "vram.seq",  MEMPROBE_KIND_RDW,  1UL,             1UL },
  { "vram.scat", MEMPROBE_KIND_RDW,  MEMPROBE_W_SCAT, 1UL }
};

#define MEMPROBE_SHAPES \
  (sizeof(memprobe_shapes) / sizeof(memprobe_shapes[0]))

static uint32 *memprobe_dram = NULL;
static uint32 *memprobe_vram = NULL;

/*
 * The loop with no memory access in it, and the reference every other figure
 * is taken against. It carries the same counter, the same accumulate, the same
 * add and the same mask as the walks below, so subtracting it leaves one
 * access and nothing else. Each walk below is THIS BODY PLUS ONE INSTRUCTION,
 * and keeping it that way is the whole validity of the net figure -- the first
 * draft of the write walk dropped the accumulate, which cost it a register
 * operation and was caught by reading the emitted code, not by running it.
 *
 * The buffer is taken and ignored so that the walks share one signature and
 * the dispatch cannot pick a wrong one.
 */
static uint32
memprobe_walk_none(uint32 *buf,
                   uint32  mask,
                   uint32  stride,
                   uint32  iters)
{
  uint32 i;
  uint32 p;
  uint32 sink;

  (void)buf;

  p = 0UL;
  sink = 0UL;

  for(i = 0UL; i < iters; i++)
    {
      sink += p;
      p = (p + stride) & mask;
    }

  return sink;
}

/* The reference body with one word read added. */
static uint32
memprobe_walk_rdw(uint32 *buf,
                  uint32  mask,
                  uint32  stride,
                  uint32  iters)
{
  uint32 i;
  uint32 p;
  uint32 sink;

  p = 0UL;
  sink = 0UL;

  for(i = 0UL; i < iters; i++)
    {
      sink += buf[p];
      p = (p + stride) & mask;
    }

  return sink;
}

/*
 * The reference body with one word write added -- the accumulate stays, which
 * is what makes the subtraction exact. The value written is the counter rather
 * than a constant: a run of stores of one value to one address is something a
 * compiler may keep only the last of, and the hot stride is exactly that run.
 */
static uint32
memprobe_walk_wrw(uint32 *buf,
                  uint32  mask,
                  uint32  stride,
                  uint32  iters)
{
  uint32 i;
  uint32 p;
  uint32 sink;

  p = 0UL;
  sink = 0UL;

  for(i = 0UL; i < iters; i++)
    {
      buf[p] = i;
      sink += p;
      p = (p + stride) & mask;
    }

  return sink;
}

/* The reference body with one byte read added, mask and stride in bytes. */
static uint32
memprobe_walk_rdb(uint32 *buf,
                  uint32  mask,
                  uint32  stride,
                  uint32  iters)
{
  const uint8 *b;
  uint32 i;
  uint32 p;
  uint32 sink;

  b = (const uint8 *)buf;
  p = 0UL;
  sink = 0UL;

  for(i = 0UL; i < iters; i++)
    {
      sink += (uint32)b[p];
      p = (p + stride) & mask;
    }

  return sink;
}

/* The reference body with one byte write added, mask and stride in bytes. */
static uint32
memprobe_walk_wrb(uint32 *buf,
                  uint32  mask,
                  uint32  stride,
                  uint32  iters)
{
  uint8 *b;
  uint32 i;
  uint32 p;
  uint32 sink;

  b = (uint8 *)buf;
  p = 0UL;
  sink = 0UL;

  for(i = 0UL; i < iters; i++)
    {
      b[p] = (uint8)i;
      sink += p;
      p = (p + stride) & mask;
    }

  return sink;
}

/*
 * One timed window over one shape, in microseconds, or MEMPROBE_USEC_MAX + 1
 * for a window the caller must throw away.
 *
 * The dispatch sits inside the clock rather than before it. A switch taken
 * once cannot register against two hundred thousand turns, whereas a clock
 * read placed after the dispatch would time a different amount of preamble for
 * each shape -- so the constant that is inside for all of them is preferred to
 * the variable that would be outside. The accumulate into *sink is inside for
 * the same reason and is one add.
 *
 * An unknown kind is an error and not a fourth case: routed to the reference
 * loop it would publish the cost of an empty loop under the name of a memory
 * access, which is the one wrong answer this file exists to avoid.
 */
static uint32
memprobe_window(const memprobe_shape_t *s,
                uint32                 *sink)
{
  uint32 *buf;
  uint32  mask;
  uint32  t0;
  uint32  usec;

  buf = (s->vram != 0UL) ? memprobe_vram : memprobe_dram;
  mask = ((s->kind == MEMPROBE_KIND_RDB) || (s->kind == MEMPROBE_KIND_WRB))
         ? MEMPROBE_BYTE_MASK : MEMPROBE_WORD_MASK;

  t0 = sys_usec();
  switch(s->kind)
    {
    case MEMPROBE_KIND_NONE:
      *sink += memprobe_walk_none(buf,mask,s->stride,MEMPROBE_ITERATIONS);
      break;
    case MEMPROBE_KIND_RDW:
      *sink += memprobe_walk_rdw(buf,mask,s->stride,MEMPROBE_ITERATIONS);
      break;
    case MEMPROBE_KIND_WRW:
      *sink += memprobe_walk_wrw(buf,mask,s->stride,MEMPROBE_ITERATIONS);
      break;
    case MEMPROBE_KIND_RDB:
      *sink += memprobe_walk_rdb(buf,mask,s->stride,MEMPROBE_ITERATIONS);
      break;
    case MEMPROBE_KIND_WRB:
      *sink += memprobe_walk_wrb(buf,mask,s->stride,MEMPROBE_ITERATIONS);
      break;
    default:
      LOG_ERR(LOG_CAT_PERF,("memprobe %s has no walk: kind=%lu",
                            s->name,(unsigned long)s->kind));
      return MEMPROBE_USEC_MAX + 1UL;
    }
  usec = sys_usec() - t0;

  if(usec > MEMPROBE_USEC_MAX)
    {
      LOG_WARN(LOG_CAT_PERF,("memprobe %s window discarded: the clock read %lu us",
                             s->name,(unsigned long)usec));
      return MEMPROBE_USEC_MAX + 1UL;
    }

  return usec;
}

/*
 * Microseconds over the window turned into hundredths of a cycle per turn.
 *
 * The division is arranged the way the mock-ups arranged theirs, so that
 * nothing overflows on the way: the microseconds are multiplied by a hundredth
 * of the cycles per millisecond and divided by a tenth of the turn count,
 * which is the same ratio in numbers a thousand times smaller. The window has
 * already been bounded by the caller, so the multiply cannot wrap.
 */
static uint32
memprobe_cycles_x100(uint32 usec)
{
  return (usec * (MEMPROBE_CYCLES_PER_MS / 100UL))
         / (MEMPROBE_ITERATIONS / 10UL);
}

/*
 * Says where a block actually lives, and at what address.
 *
 * The question is not academic: two of the four video blocks are asked for as
 * MEMTYPE_ANY, and MEMTYPE_ANY is zero (include/3do/mem.h:62), so the
 * allocator is free to serve them out of VRAM. GetMemType
 * (include/3do/mem.h:214) is the only thing that knows, and it costs one call
 * at boot. The pointer is printed beside it because alignment and bank are
 * things the reader may want and nothing else here would tell them.
 *
 * A type of zero is not read as DRAM. Zero is the value of MEMTYPE_ANY itself
 * and carries no VRAM bit, so calling it DRAM would be an answer where there
 * is none.
 */
static void
memprobe_where(const char *name,
               void       *p)
{
  uint32 t;

  if(p == NULL)
    {
      LOG_INFO(LOG_CAT_PERF,("memprobe where %s absent",name));
      return;
    }

  t = GetMemType(p);

  LOG_INFO(LOG_CAT_PERF,("memprobe where %s %s at=0x%lx type=0x%lx",
                         name,
                         (t == 0UL) ? "unknown"
                           : (((t & MEMTYPE_VRAM) != 0UL) ? "vram" : "dram"),
                         (unsigned long)p,
                         (unsigned long)t));
}

Err
memprobe_install(void)
{
  memprobe_dram = (uint32 *)sys_alloc("memprobe_dram",
                                      (int32)MEMPROBE_BYTES,
                                      MEMTYPE_DRAM | MEMTYPE_FILL);
  if(memprobe_dram == NULL)
    {
      LOG_ERR(LOG_CAT_PERF,("memprobe absent: no dram buffer"));
      return -1;
    }

  /*
   * The VRAM buffer is allowed to fail on its own. The display owns most of
   * that memory and has taken its share before this runs, so a refusal here is
   * an ordinary outcome and not a defect: the shapes that needed it are
   * dropped by name in the report, which is the difference between a figure
   * that is missing and a figure that is silently absent.
   */
  memprobe_vram = (uint32 *)sys_alloc("memprobe_vram",
                                      (int32)MEMPROBE_BYTES,
                                      MEMTYPE_VRAM | MEMTYPE_FILL);
  if(memprobe_vram == NULL)
    LOG_WARN(LOG_CAT_PERF,("memprobe vram shapes dropped: no vram buffer"));

  LOG_INFO(LOG_CAT_PERF,("memprobe install dram=%lu vram=%lu bytes",
                         (unsigned long)MEMPROBE_BYTES,
                         (unsigned long)((memprobe_vram != NULL)
                                         ? MEMPROBE_BYTES : 0UL)));

  return 0;
}

Err
memprobe_measure(void)
{
  uint32 i;
  uint32 pass;
  uint32 sink;
  uint32 usec[MEMPROBE_PASSES];
  uint32 best;
  uint32 worst;
  uint32 cyc;
  uint32 base;
  uint32 net;
  uint32 dropped;
  int32  page;

  if(memprobe_dram == NULL)
    {
      LOG_WARN(LOG_CAT_PERF,("memprobe not installed, nothing measured"));
      return -1;
    }

  /*
   * The reference has to be the first shape measured, since every net figure
   * is taken against it, and it has to be the loop with no access in it. Both
   * are properties of a table a later hand may reorder, so both are checked
   * rather than assumed: a reordered table would otherwise reprice every net
   * figure in the report against whatever happened to be first, in silence.
   */
  if(memprobe_shapes[0].kind != MEMPROBE_KIND_NONE)
    {
      LOG_ERR(LOG_CAT_PERF,("memprobe refuses: shape 0 is not the reference loop"));
      return -1;
    }

  /*
   * Said before the stall and not after. The probe holds the processor for
   * some twenty seconds with nothing on screen, and a trace that goes quiet
   * for twenty seconds without warning is a trace that reads as a hung
   * console.
   */
  LOG_INFO(LOG_CAT_PERF,("memprobe starting: %lu shapes x %lu passes, about 20 s",
                         (unsigned long)MEMPROBE_SHAPES,
                         (unsigned long)MEMPROBE_PASSES));

  /*
   * The page size, and what it is worth. This is the allocator's page, not the
   * DRAM row that page mode serves (include/3do/mem.h:88-92), so it is a lower
   * bound on what the scattered stride has to clear rather than the number
   * itself. It is still the only figure the system will give, and a stride
   * that fails to clear even this one is certainly measuring page mode.
   */
  page = GetPageSize(MEMTYPE_DRAM);
  LOG_INFO(LOG_CAT_PERF,("memprobe pagesize dram=%ld vram=%ld scat=%lu bytes",
                         (long)page,
                         (long)GetPageSize(MEMTYPE_VRAM),
                         (unsigned long)(MEMPROBE_W_SCAT * 4UL)));
  if((page <= 0) || ((uint32)page >= (MEMPROBE_W_SCAT * 4UL)))
    LOG_WARN(LOG_CAT_PERF,
             ("memprobe scattered stride may not clear the page: the scat "
              "figures are then page mode and the comparison is void"));

  memprobe_where("vdp.vram",sms.vdp.vram);
  memprobe_where("vdp.planes",sms.vdp.planes);
  memprobe_where("vdp.tilecache",sms.vdp.tc);
  for(i = 0UL; i < (uint32)SMS_VDP_BUFFERS; i++)
    memprobe_where("vdp.pixels",sms.vdp.pixels[i]);
  memprobe_where("probe.dram",memprobe_dram);
  memprobe_where("probe.vram",memprobe_vram);

  sink = 0UL;
  base = 0UL;
  dropped = 0UL;

  for(i = 0UL; i < MEMPROBE_SHAPES; i++)
    {
      if((memprobe_shapes[i].vram != 0UL) && (memprobe_vram == NULL))
        {
          LOG_INFO(LOG_CAT_PERF,("memprobe %s dropped: no vram buffer",
                                 memprobe_shapes[i].name));
          dropped++;
          continue;
        }

      for(pass = 0UL; pass < MEMPROBE_PASSES; pass++)
        usec[pass] = memprobe_window(&memprobe_shapes[i],&sink);

      best = usec[0];
      worst = usec[0];
      for(pass = 1UL; pass < MEMPROBE_PASSES; pass++)
        {
          if(usec[pass] < best)
            best = usec[pass];
          if(usec[pass] > worst)
            worst = usec[pass];
        }

      if(best > MEMPROBE_USEC_MAX)
        {
          LOG_WARN(LOG_CAT_PERF,("memprobe %s dropped: no usable window",
                                 memprobe_shapes[i].name));
          dropped++;
          continue;
        }

      /*
       * The two readings should agree closely; an eighth apart is already far
       * more than this loop can vary by. Said on its own line rather than left
       * for the reader to spot in a column of fourteen.
       */
      if((worst - best) > (best >> 3))
        LOG_WARN(LOG_CAT_PERF,("memprobe %s readings disagree: %lu vs %lu us",
                               memprobe_shapes[i].name,
                               (unsigned long)best,(unsigned long)worst));

      cyc = memprobe_cycles_x100(best);

      if(i == 0UL)
        base = cyc;

      /*
       * Net is the access alone, clamped rather than allowed to go negative: a
       * walk that timed under the reference has measured noise, and a zero
       * says so where a wrapped unsigned would read as an enormous cost.
       */
      net = (cyc > base) ? (cyc - base) : 0UL;

      /*
       * A net of zero on a shape that carries an access is the exact signature
       * of the failure this file is built to avoid -- the load hoisted out of
       * the loop, the empty loop timed, the figure published as a memory
       * access. The table defends against it; this is what says the defence
       * gave way.
       */
      if((memprobe_shapes[i].kind != MEMPROBE_KIND_NONE) && (net == 0UL))
        LOG_WARN(LOG_CAT_PERF,
                 ("memprobe %s net is zero: the access was probably hoisted "
                  "out of the loop, do not publish this figure",
                  memprobe_shapes[i].name));

      LOG_INFO(LOG_CAT_PERF,
               ("memprobe %s cyc=%lu.%02lu net=%lu.%02lu us=%lu/%lu",
                memprobe_shapes[i].name,
                (unsigned long)(cyc / 100UL),
                (unsigned long)(cyc % 100UL),
                (unsigned long)(net / 100UL),
                (unsigned long)(net % 100UL),
                (unsigned long)usec[0],
                (unsigned long)usec[1]));
    }

  /*
   * The accumulator is published, and that is its whole purpose: a sum no one
   * ever reads is a sum the compiler is entitled to stop computing, taking the
   * loads that fed it with it. The value itself says nothing and is not meant
   * to. The dropped count is beside it so that a partial report announces
   * itself as one.
   */
  LOG_INFO(LOG_CAT_PERF,("memprobe done shapes=%lu dropped=%lu iters=%lu sink=0x%lx",
                         (unsigned long)MEMPROBE_SHAPES,
                         (unsigned long)dropped,
                         (unsigned long)MEMPROBE_ITERATIONS,
                         (unsigned long)sink));

  return (dropped == 0UL) ? 0 : 1;
}

#endif /* SMS_MEM_PROBE */
