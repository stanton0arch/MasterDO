#include "z80c.h"
#include "sms.h"
#include "z80.h"
#include "log.h"

uint8 z80c_armed = 0;

#if LOG_ENABLE && SMS_TELEMETRY
/*
 * Blocks run, and blocks after which the interpreter had to take over
 * inside the quota. Kept only with the line they feed (log.h). The
 * hand-back is counted where it is seen: after a block, when the quota
 * still holds and no block starts at PC. A block that ends the quota is
 * counted on neither side -- what follows it is the next quota's to find.
 */
static uint32 z80c_exec_total     = 0;
static uint32 z80c_fallback_total = 0;
static uint32 z80c_exec_said      = 0;
static uint32 z80c_fallback_said  = 0;
#define Z80C_COUNT(counter) ((counter)++)
#else
#define Z80C_COUNT(counter) ((void)0)
#endif

void
z80c_init(void)
{
  z80c_armed = 0;

#if LOG_ENABLE && SMS_TELEMETRY
  z80c_exec_total     = 0;
  z80c_fallback_total = 0;
  z80c_exec_said      = 0;
  z80c_fallback_said  = 0;
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

z80c_fn
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
    return z80c_table[lo].fn;

  return NULL;
}

void
z80c_run(z80c_fn fn)
{
  for(;;)
    {
      fn();
      Z80C_COUNT(z80c_exec_total);

      if(sms.z80.tstates <= 0)
        return;

      fn = z80c_find(sms.z80.pc);
      if(fn == NULL)
        {
          Z80C_COUNT(z80c_fallback_total);
          return;
        }
    }
}

void
z80c_report(void)
{
#if LOG_ENABLE && SMS_TELEMETRY
  if(!z80c_armed)
    return;

  LOG_HOT(LOG_CAT_Z80,LOG_LVL_DBG,
          ("exec=%lu fallback=%lu",
           (unsigned long)(z80c_exec_total - z80c_exec_said),
           (unsigned long)(z80c_fallback_total - z80c_fallback_said)));

  z80c_exec_said     = z80c_exec_total;
  z80c_fallback_said = z80c_fallback_total;
#endif
}

#if LOG_ENABLE && SMS_TELEMETRY
void
z80c_counts(uint32 *exec, uint32 *fallback)
{
  *exec     = z80c_exec_total;
  *fallback = z80c_fallback_total;
}
#endif
