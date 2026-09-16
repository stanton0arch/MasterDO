#include "z80c.h"
#include "sms.h"
#include "z80.h"
#include "log.h"

uint8  z80c_armed     = 0;
uint32 z80c_map_epoch = 0;
uint32 z80c_miss_pc   = Z80C_NO_PC;

#if Z80C_BENCH
uint8  z80c_no_exec    = 0;
uint8  z80c_no_wait    = 0;
uint32 z80c_no_wait_pc = 0;
uint32 z80c_line_mark  = 0;
uint32 z80c_ram_pc     = Z80C_NO_PC;
uint32 z80c_last_pos   = 0;
uint8 *z80c_seen       = NULL;
uint32 z80c_ring[Z80C_RING];
uint32 z80c_ring_n     = 0;
#endif

#if LOG_ENABLE && SMS_TELEMETRY
/*
 * The four running totals since reset, and what each of them was when the
 * periodic line last said it. Blocks run and instructions run translated
 * are stepped here and in the generated code; the two interpreter-side
 * counts are stepped by the core's loop (z80c.h, Z80C_INTERPRETED). Kept
 * only with the lines they feed (log.h).
 */
static uint32 z80c_exec_total     = 0;
static uint32 z80c_chains_total   = 0;
uint32        z80c_insns_total    = 0;
uint32        z80c_fallback_total = 0;
uint32        z80c_ram_total      = 0;
static uint32 z80c_exec_said      = 0;
static uint32 z80c_insns_said     = 0;
static uint32 z80c_fallback_said  = 0;
static uint32 z80c_ram_said       = 0;
#define Z80C_COUNT(counter) ((counter)++)
#else
#define Z80C_COUNT(counter) ((void)0)
#endif

void
z80c_init(void)
{
  z80c_armed     = 0;
  z80c_map_epoch = 0;
  z80c_miss_pc   = Z80C_NO_PC;

#if Z80C_BENCH
  z80c_no_wait    = 0;
  z80c_no_wait_pc = 0;
  z80c_line_mark  = 0;
  z80c_ram_pc     = Z80C_NO_PC;
  z80c_last_pos   = 0;
  z80c_ring_n     = 0;
#endif

#if LOG_ENABLE && SMS_TELEMETRY
  z80c_exec_total     = 0;
  z80c_chains_total   = 0;
  z80c_insns_total    = 0;
  z80c_fallback_total = 0;
  z80c_ram_total      = 0;
  z80c_exec_said      = 0;
  z80c_insns_said     = 0;
  z80c_fallback_said  = 0;
  z80c_ram_said       = 0;
#endif

  if(z80c_block_count == 0UL)
    {
      LOG_WARN(LOG_CAT_Z80,("translated code: none, interpreter only"));
      return;
    }

  /*
   * The size alone decides here; the digest is journaled so that a reader
   * of the trace can hold it against the tool's report. Checking it at
   * boot -- a walk of the whole image -- is the loader's third operation
   * and is not written yet.
   */
  if(z80c_rom_size != sms.cart.size)
    {
      LOG_WARN(LOG_CAT_Z80,
               ("translated code: rom size %lu vs %lu, interpreter only",
                (unsigned long)z80c_rom_size,
                (unsigned long)sms.cart.size));
      return;
    }

  LOG_INFO(LOG_CAT_Z80,
           ("translated code: blocks=%lu bytes=%lu rom=%lu/%08lx paired",
            (unsigned long)z80c_block_count,
            (unsigned long)z80c_code_bytes,
            (unsigned long)z80c_rom_size,
            (unsigned long)z80c_rom_fnv));

  z80c_armed = 1;
}

const z80c_entry_t *
z80c_find(uint16 pc)
{
  const uint8 *page = z80_rmap[pc >> Z80_PAGE_BITS];
  const uint8 *rom  = sms.cart.rom;
  long   off;
  uint32 pos;
  uint32 lo;
  uint32 hi;

  /*
   * The page decides: an entry that points into the image is a position,
   * anything else -- the work RAM and its mirrors, the cartridge RAM, the
   * two fixed pages -- is interpreted. The difference is taken as a
   * signed offset and bounded by the loaded size, so that a page below
   * the buffer or beyond the image answers the same "none".
   */
  off = (long)(page - rom);
  if(off < 0L || (uint32)off >= sms.cart.size)
    return NULL;

  pos = (uint32)off + (uint32)(pc & Z80_PAGE_MASK);

  lo = 0;
  hi = z80c_block_count;
  while(lo < hi)
    {
      uint32 mid = lo + ((hi - lo) >> 1);

      if(z80c_table[mid].pos < pos)
        lo = mid + 1;
      else
        hi = mid;
    }

  if(lo < z80c_block_count && z80c_table[lo].pos == pos)
    return &z80c_table[lo];

  return NULL;
}

void
z80c_run(const z80c_entry_t *e)
{
  uint32 epoch = z80c_map_epoch;

  Z80C_COUNT(z80c_chains_total);

  for(;;)
    {
      const z80c_entry_t *next;

      Z80C_BLOCK_SEEN(e->pos);
      next = e->fn();

      Z80C_COUNT(z80c_exec_total);

#if Z80C_BENCH
      /* The guard, on the PC: the core's loop names the address. */
      if(Z80C_LINE_OVER())
        return;
#endif

      /*
       * The successor the block rendered is trusted while the mapper has
       * not moved a page since this chain started: the block tested the
       * window, the epoch tests the pages behind it. Otherwise -- a
       * null, or a moved page -- the live tables say what starts at PC,
       * and a miss ends the chain with the address left for the core's
       * loop, which does not search it a second time.
       */
      if(next == NULL || z80c_map_epoch != epoch)
        {
          epoch = z80c_map_epoch;
          next = z80c_find(sms.z80.pc);
          if(next == NULL)
            {
              z80c_miss_pc = (uint32)sms.z80.pc;
              return;
            }
        }

      /*
       * A wait is never entered from a chain: the program has arrived
       * where it waits, and the core's loop, which finds the same entry
       * at PC, ends the line there.
       */
      if(next->wait != 0UL)
        return;

      e = next;
    }
}

/*
 * The ARM cycles one T-state cost, in tenths, out of the processor share
 * of a frame in tenths of a millisecond: the ARM60 runs 12500 cycles per
 * millisecond and a frame is 262 lines of 228 T-states, 59736. The
 * product stays inside 32 bits for any share a frame can hold.
 */
#define Z80C_CYCLES_PER_MS   12500UL
#define Z80C_TSTATES_A_FRAME 59736UL

void
z80c_report(uint32 z80_tenths_ms, uint32 frames)
{
#if LOG_ENABLE && SMS_TELEMETRY
  uint32 exec     = z80c_exec_total - z80c_exec_said;
  uint32 insns    = z80c_insns_total - z80c_insns_said;
  uint32 fallback = z80c_fallback_total - z80c_fallback_said;
  uint32 ram      = z80c_ram_total - z80c_ram_said;
  uint32 pct      = 0;
  uint32 per_frame;
  uint32 cyc10;

  z80c_exec_said     = z80c_exec_total;
  z80c_insns_said    = z80c_insns_total;
  z80c_fallback_said = z80c_fallback_total;
  z80c_ram_said      = z80c_ram_total;

  /*
   * With the interpreter alone the program is still paced by T-states, and
   * the ratio a reader holds against earlier runs is the cycles a T-state
   * cost.
   */
  if(!z80c_armed)
    {
      cyc10 = (z80_tenths_ms * Z80C_CYCLES_PER_MS) / Z80C_TSTATES_A_FRAME;
      LOG_HOT(LOG_CAT_Z80,LOG_LVL_INFO,
              ("z80c cyc/tstate=%lu.%lu translated=0%%",
               (unsigned long)(cyc10 / 10UL),(unsigned long)(cyc10 % 10UL)));
      return;
    }

  /*
   * With a table armed there is no T-state left to divide by: the work of
   * a frame is the instructions it ran, translated and interpreted, and
   * the share is divided by those. The tenths of a millisecond times the
   * cycles of a millisecond are tenths of a cycle -- a frame of a hundred
   * milliseconds is 12.5 million, well inside 32 bits -- and divided by
   * the instructions of a frame, tenths of a cycle per instruction.
   */
  if(insns + fallback != 0UL)
    pct = (uint32)(((unsigned long)insns * 100UL) / (insns + fallback));
  per_frame = (frames != 0UL) ? (insns + fallback) / frames : 0UL;
  cyc10 = (per_frame != 0UL)
            ? (z80_tenths_ms * Z80C_CYCLES_PER_MS) / per_frame
            : 0UL;
  LOG_HOT(LOG_CAT_Z80,LOG_LVL_INFO,
          ("z80c insns/frame=%lu cyc/insn=%lu.%lu translated=%lu%%",
           (unsigned long)per_frame,
           (unsigned long)(cyc10 / 10UL),(unsigned long)(cyc10 % 10UL),
           (unsigned long)pct));

  LOG_HOT(LOG_CAT_Z80,LOG_LVL_DBG,
          ("exec=%lu insns=%lu fallback=%lu ram_exec=%lu",
           (unsigned long)exec,(unsigned long)insns,
           (unsigned long)fallback,(unsigned long)ram));
#else
  (void)z80_tenths_ms;
  (void)frames;
#endif
}

#if LOG_ENABLE && SMS_TELEMETRY
void
z80c_counts(uint32 *exec, uint32 *fallback, uint32 *insns, uint32 *ram_exec)
{
  *exec     = z80c_exec_total;
  *fallback = z80c_fallback_total;
  *insns    = z80c_insns_total;
  *ram_exec = z80c_ram_total;
}

/*
 * How many times a chain was entered. Held apart from the four counts
 * above because it answers one question and one only: whether the chain
 * is followed at all. Blocks run divided by chains entered is the length
 * of the average chain, and a run where the two are equal is a run in
 * which every block was found again by the core's loop -- which is what
 * this whole path exists to avoid.
 */
uint32
z80c_chains(void)
{
  return z80c_chains_total;
}
#endif
