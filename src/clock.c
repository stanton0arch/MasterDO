/*
 * Microsecond clock: the kernel's system time (SampleSystemTimeTV, a
 * kernel call of tens of microseconds on the console), kept apart from
 * the platform layer so that the cycle model runs this same code with
 * its own SampleSystemTimeTV, charging the cost of the call.
 *
 * A sample is occasionally off by about a second: the seconds and the
 * microseconds are not read together, so a sample taken at the turn of a
 * second can pair the seconds of one side with the microseconds of the
 * other. A look at the clock therefore trusts a single sample only when
 * its microseconds are away from the turn of a second and it is in line
 * with the last value returned (not behind it, less than half a second
 * ahead): a sample a second off fails one test or the other. Otherwise
 * (and on the first look after a long time) two samples taken in a row
 * must agree.
 */

#include "platform.h"
#include "time.h"

#define CLOCK_EDGE_US   10000u  /* microseconds this near the turn of a second: checked */
#define CLOCK_AHEAD_US  500000u /* a sample this far ahead of the last value: checked */

/* Looks at the clock (plat_usec_now) and kernel calls they made, for the
 * logs: the calls per frame are part of every frame's cost. */
static uint32 clock_looks;
static uint32 clock_calls;
/* The last value returned. The presenter thread looks at the clock too:
 * a look of the main task interrupted by one of the thread's finds its
 * sample behind this value and samples twice, which is only slower. */
static uint32 clock_last;

static uint32 usec_sample(void)
{
    TimeVal tv;

    SampleSystemTimeTV(&tv);
    clock_calls++;
    return (uint32)tv.tv_sec * 1000000u + (uint32)tv.tv_usec;
}

uint32 plat_usec_now(void)
{
    TimeVal tv;
    uint32 a;
    uint32 b;

    SampleSystemTimeTV(&tv);
    clock_calls++;
    clock_looks++;
    a = (uint32)tv.tv_sec * 1000000u + (uint32)tv.tv_usec;
    if ((uint32)tv.tv_usec - CLOCK_EDGE_US < 1000000u - 2 * CLOCK_EDGE_US &&
        a - clock_last < CLOCK_AHEAD_US) {
        clock_last = a;
        return a;
    }
    for (;;) {
        b = usec_sample();
        if (b - a < 1000u) {
            clock_last = b;
            return b;
        }
        a = b;
    }
}

void plat_clock_counts(uint32 *looks, uint32 *calls)
{
    *looks = clock_looks;
    *calls = clock_calls;
}
