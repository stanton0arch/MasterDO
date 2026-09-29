#ifndef SMSMEM_H
#define SMSMEM_H

/*
 * Master System / Game Gear cartridge memory for the Z80 translator: ROM
 * seen through the Sega mapper (three 16 KiB slots, the first 1 KiB fixed,
 * paging registers at $FFFC-$FFFF), optional cartridge RAM in slot 2, and
 * 8 KiB of system RAM at $C000 mirrored at $E000.
 *
 * Ports and scanline events are not emulated here: IN returns $FF, OUT is
 * ignored. A machine emulator puts its own callbacks in m.
 */

#include "types.h"
#include "z80jit.h"

typedef struct {
    z80j_machine m;         /* callbacks; m.user points to this structure */
    z80j_ctx    *ctx;
    const uint8 *rom;
    uint32       rom_size;
    uint32       banks;     /* 16 KiB banks in the ROM */
    uint8       *cart_ram;  /* 32 KiB, or 0 */
    uint32       reg[4];    /* $FFFC-$FFFF */
    uint32       slot_ram[3]; /* slot currently showing cartridge RAM */
    uint32       remaps;    /* paging changes (statistics) */
} sms_mem;

/* Sets up the pages of ctx (system RAM in ctx->mram) for a ROM image and
 * the machine callbacks. */
void   sms_mem_init(sms_mem *s, z80j_ctx *ctx, const uint8 *rom, uint32 rom_size,
                    uint8 *cart_ram);

/* Applies the paging registers to the page tables. */
void   sms_mem_map(sms_mem *s);

/* Paging registers to their power-on values (banks 0, 1, 2). */
void   sms_mem_reset(sms_mem *s);

/* Machine write callback, for machines that wrap this one. */
uint32 sms_mem_write(z80j_machine *m, uint32 addr, uint32 value);


#endif /* SMSMEM_H */
