#ifndef SMSMEM_H
#define SMSMEM_H

/*
 * Master System / Game Gear cartridge memory for the Z80 translator: ROM
 * seen through the Sega mapper (three 16 KiB slots, the first 1 KiB fixed,
 * paging registers at $FFFC-$FFFF) or through the Codemasters mapper
 * (recognised from its header at $7FE0: the three slots entirely paged,
 * each by a write anywhere inside it, no fixed area, no RAM mirror of
 * the registers), optional cartridge RAM in slot 2 (the Sega mapper's
 * battery RAM, or the 8 KiB of the Codemasters mapper at $A000, shown by
 * bit 7 of the slot 1 register), and 8 KiB of system RAM at $C000
 * mirrored at $E000.
 *
 * Ports and scanline events are not emulated here: IN returns $FF, OUT is
 * ignored. A machine emulator puts its own callbacks in m.
 */

#include "types.h"
#include "z80jit.h"

#define SMS_MAPPER_SEGA        0
#define SMS_MAPPER_CODEMASTERS 1

typedef struct {
    z80j_machine m;         /* callbacks; m.user points to this structure */
    z80j_ctx    *ctx;
    const uint8 *rom;
    uint32       rom_size;
    uint32       banks;     /* 16 KiB banks in the ROM */
    uint8       *cart_ram;  /* battery RAM shown in slot 2 by $FFFC bit 3, or 0 */
    uint32       reg[4];    /* $FFFC-$FFFF */
    uint32       slot_ram[3]; /* first page of each slot showing cartridge RAM (its end: none) */
    uint32       remaps;    /* paging changes (statistics) */
    uint32       bank_mask; /* banks - 1, or 0xFFFFFFFF when banks is not a power of two */
    uint32       cart_ram_size; /* 16 KiB (one page) or 32 KiB (bit 2 selects the page) */
    uint32       mapper;    /* SMS_MAPPER_* */
    uint32       slot_ram_e[3]; /* read table entry of that RAM, as mapped */
} sms_mem;

/* The mapper an image calls for, from its header (SMS_MAPPER_*). */
uint32 sms_mem_detect(const uint8 *rom, uint32 rom_size);

/* Special write handler in assembly (smsmem_a.s), installed by
 * sms_mem_init. CONTRACT: the field offsets above match its equates. */
void   sms_mem_write_a(void);

/* Sets up the pages of ctx (system RAM in ctx->mram) for a ROM image and
 * the machine callbacks; cart_ram (16 or 32 KiB, or 0 for none) is the
 * cartridge RAM that register $FFFC can map into slot 2. */
void   sms_mem_init(sms_mem *s, z80j_ctx *ctx, const uint8 *rom, uint32 rom_size,
                    uint8 *cart_ram, uint32 cart_ram_size);

/* Applies the paging registers to the page tables. */
void   sms_mem_map(sms_mem *s);

/* Paging registers to their power-on values (banks 0, 1, 2; the
 * Codemasters mapper starts with banks 0, 1, 0). */
void   sms_mem_reset(sms_mem *s);

/* Machine write callback, for machines that wrap this one. */
uint32 sms_mem_write(z80j_machine *m, uint32 addr, uint32 value);


#endif /* SMSMEM_H */
