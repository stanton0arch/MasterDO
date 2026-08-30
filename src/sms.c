#include "sms.h"

/*
 * The one instance of the world. Defined here and nowhere else, and defined
 * as a plain object rather than through any accessor, so that its address is
 * a link-time constant for every module that names it (see sms.h).
 *
 * Zero at load like every other object of static storage. That is not the
 * initial state of any module: z80_reset, cart_init and vdp_init set it,
 * each for its own field. No initialiser is written, so that the file says exactly
 * what it does.
 */
sms_t sms;
