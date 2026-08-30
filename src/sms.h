#ifndef SMS3DO_SMS_H
#define SMS3DO_SMS_H

#include "z80.h"
#include "cart.h"
#include "vdp.h"

/*
 * The world: every emulated state the port keeps, gathered in one structure
 * with one instance.
 *
 * Ownership, field by field. Each member is written by exactly one module and
 * read by any other:
 *
 *   z80   the processor core, z80.c; nothing else writes a register, a
 *         flip-flop or the T-state counter.
 *   cart  the cartridge module, cart.c; nothing else writes the buffer
 *         pointer, the loaded size, the name or the deduced system.
 *   vdp   the video part, vdp.c; nothing else writes an address, a
 *         register, a memory byte or an interrupt request. The processor
 *         reads two requests and two registers through the line macro of
 *         vdp.h, and that is the whole of the traffic the other way.
 *
 * The rule used to be a static in each file, which the compiler enforced by
 * making the object unnameable elsewhere. It is now a declared rule, and it
 * is the price of aggregation: a module that needs to read another's state
 * has to be able to name it.
 *
 * Why one global instance and never a pointer. A global has a link-time
 * constant address, so sms.z80.main.a compiles to a load off an immediate
 * base -- exactly what the file static it replaces cost. A pointer to this
 * structure passed as an argument would add one load of that pointer per
 * access, on the hottest path the program has. So no function takes or
 * stores an sms_t pointer: every module names sms directly, and the five
 * register-resident locals of z80_run keep their mechanism untouched.
 */
typedef struct
{
  z80_t  z80;
  cart_t cart;
  vdp_t  vdp;
} sms_t;

extern sms_t sms;

#endif /* SMS3DO_SMS_H */
