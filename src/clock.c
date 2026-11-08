/*
 * Microsecond clock: the kernel's system time (SampleSystemTimeTV, a
 * kernel call of tens of microseconds on the console), kept apart from
 * the platform layer so that the cycle model runs this same code with
 * its own SampleSystemTimeTV, charging the cost of the call.
 */

#include "platform.h"
#include "time.h"

/* Looks at the clock (plat_usec_now) and kernel calls they made, for the
 * logs: the calls per frame are part of every frame's cost. */
static uint32 clock_looks;
static uint32 clock_calls;

static uint32 usec_sample(void)
{
    TimeVal tv;

    SampleSystemTimeTV(&tv);
    clock_calls++;
    return (uint32)tv.tv_sec * 1000000u + (uint32)tv.tv_usec;
}

uint32 plat_usec_now(void)
{
    uint32 a = usec_sample();
    uint32 b;

    clock_looks++;
    /* A sample is occasionally off by about a second (seconds and
     * microseconds not read together): two samples taken in a row must
     * agree, else sample again. */
    for (;;) {
        b = usec_sample();
        if (b - a < 1000u)
            return b;
        a = b;
    }
}

void plat_clock_counts(uint32 *looks, uint32 *calls)
{
    *looks = clock_looks;
    *calls = clock_calls;
}
