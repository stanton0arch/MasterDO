/* The instruction-length freeze.
 *
 * READ THIS BEFORE TRUSTING IT: THIS IS NOT AN ORACLE.  Nobody published
 * these numbers.  Each is a CRC-32 over (uint8)(PC - ZEX_IUT) for every case
 * of one descriptor -- how far this core moved the program counter -- taken
 * from THIS core, on the day the file was written.  It does not say the
 * instruction lengths are right.  It says they have not moved.
 *
 * Why it exists: the ZEXALL fingerprint cannot see a length defect at all.
 * run_case reloads PC for every case, so the final position is never
 * observed, and two reviewers independently proved the hole by adding a
 * stray PC increment to an opcode and watching the bench stay green.  On a
 * console that is the loudest failure there is; here it was the quietest.
 *
 * It is deliberately kept in its own file and its own CRC, never mixed into
 * the ZEXALL fingerprint, which must stay comparable to the published values.
 *
 * Taken in the documented pass only.
 *
 * To re-freeze -- and only ever after deciding, with reasons, that a length
 * change is correct:
 *
 *     sh tests/z80/run_z80.sh            # build it
 *     tests/z80/z80_bench --emit-pclen   # paste the table below
 */
#ifndef SMS3DO_ZEX_PCLEN_BASELINE_H
#define SMS3DO_ZEX_PCLEN_BASELINE_H

#include "zexall_tests.h"

typedef struct
{
  const char   *label;   /* checked against the descriptor table's own */
  unsigned long crc;
} zex_pclen_t;

static const zex_pclen_t zex_pclen[ZEX_TEST_COUNT] =
{
  { "ld162", 0x7CD6F7A1UL },
  { "ld163", 0xD0E6ACC6UL },
  { "ld166", 0x7CD6F7A1UL },
  { "ld167", 0xD0E6ACC6UL },
  { "ld8imx", 0x75BA4530UL },
  { "ld161", 0xC0D3BB04UL },
  { "ld164", 0xC0D3BB04UL },
  { "ld16ix", 0xC0D3BB04UL },
  { "ld8bd", 0xDBDA25F7UL },
  { "lda", 0x956F8031UL },
  { "ldd1", 0xB23552D2UL },
  { "ldd2", 0xB23552D2UL },
  { "ldi1", 0xB23552D2UL },
  { "ldi2", 0xB23552D2UL },
  { "ld165", 0x531DEA59UL },
  { "ld168", 0x531DEA59UL },
  { "ld16im", 0x0E680F7CUL },
  { "ld8im", 0x6590C40EUL },
  { "sccf", 0xCAF10BC4UL },
  { "st8ix3", 0xFA81F4E5UL },
  { "cplop", 0x9ECD7839UL },
  { "ld8ix3", 0x5E79C1ADUL },
  { "stabd", 0x1FA7CEA0UL },
  { "rotxy", 0x9A560A44UL },
  { "srzx", 0xBB971E5BUL },
  { "ld8ix2", 0xD20BE7F0UL },
  { "st8ix2", 0xD20BE7F0UL },
  { "ld8ixy", 0x2F12CD84UL },
  { "ld8ix1", 0xEABE73F9UL },
  { "incbc", 0xF81BDABBUL },
  { "incde", 0xF81BDABBUL },
  { "inchl", 0xF81BDABBUL },
  { "incix", 0x727A166FUL },
  { "inciy", 0x727A166FUL },
  { "incsp", 0xF81BDABBUL },
  { "st8ix1", 0x06506D05UL },
  { "bitx", 0x79EA0B80UL },
  { "ld8rr", 0x982F1D7CUL },
  { "inca", 0x7BA297F1UL },
  { "incb", 0x7BA297F1UL },
  { "incc", 0x7BA297F1UL },
  { "incd", 0x7BA297F1UL },
  { "ince", 0x7BA297F1UL },
  { "inch", 0x7BA297F1UL },
  { "incl", 0x7BA297F1UL },
  { "incm", 0x7BA297F1UL },
  { "incxh", 0x60B3B126UL },
  { "incxl", 0x60B3B126UL },
  { "incyh", 0x60B3B126UL },
  { "incyl", 0x60B3B126UL },
  { "rotz80", 0xCE5F454CUL },
  { "ld8rrx", 0xD4B6A2BEUL },
  { "srz80", 0xD89869ABUL },
  { "incx", 0x5446E834UL },
  { "rot8080", 0x0F59D5E1UL },
  { "rldop", 0x8DF4A1A2UL },
  { "alu8r_a", 0x9C1A0B4FUL },
  { "cpd1", 0xF2EC2D0AUL },
  { "cpi1", 0xF2EC2D0AUL },
  { "negop", 0x03741430UL },
  { "daaop", 0x92FC32FDUL },
  { "alu8i", 0x02914F31UL },
  { "alu8r_b", 0x933DBD9FUL },
  { "alu8r_c", 0x933DBD9FUL },
  { "alu8r_d", 0x933DBD9FUL },
  { "alu8r_e", 0x933DBD9FUL },
  { "alu8r_h", 0x933DBD9FUL },
  { "alu8r_l", 0x933DBD9FUL },
  { "alu8r_hl", 0x933DBD9FUL },
  { "alu8rx_ixh", 0x02914F31UL },
  { "alu8rx_ixl", 0x02914F31UL },
  { "alu8rx_iyh", 0x02914F31UL },
  { "alu8rx_iyl", 0x02914F31UL },
  { "add16", 0xBD5B71C1UL },
  { "add16x", 0x8E04C994UL },
  { "add16y", 0x8E04C994UL },
  { "bitz80", 0xF8D6850BUL },
  { "adc16", 0x2878C0A2UL },
  { "alu8x", 0x2F9CB3CEUL }
};

#endif /* SMS3DO_ZEX_PCLEN_BASELINE_H */
