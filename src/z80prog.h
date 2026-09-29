#ifndef Z80PROG_H
#define Z80PROG_H

/*
 * Z80 benchmark program and the helpers shared by the interpreter and the
 * translator benchmarks: memory map, flag table, RAM set-up and the check
 * of the results.
 */

#include "types.h"

#define Z80PROG_CART_SIZE 0x4000
#define Z80PROG_RAM_SIZE  0x2000
#define Z80PROG_SP        0xDFF0

/* Below this PC the object update routine is not running, so the RAM is
 * consistent and can be checked. */
#define Z80PROG_SAFE_PC   0x003A

extern const uint8  z80prog_code[];
extern const uint32 z80prog_size;

/* Memory map in the 1 KiB page format of both cores (page p at
 * memmap[63 - p]): the cartridge mirrored over $0000-$BFFF, the RAM over
 * $C000-$DFFF and $E000-$FFFF. Addresses as seen by the generated code. */
void  z80prog_fill_memmap(uint32 *memmap, uint32 cart_addr, uint32 ram_addr);

/* Sign / zero / parity flags of every byte, in the cores' flag format. */
void  z80prog_fill_pzst(uint8 *pzst);

void  z80prog_init_ram(uint8 *ram);

/* 1 when the RAM matches the semantics of the program. */
int32 z80prog_check(const uint8 *ram);

#endif /* Z80PROG_H */
