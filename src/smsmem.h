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
    const uint8 *rom;       /* the image, or its first bank when its banks are apart */
    const uint8 *const *bank_tab; /* host address of each bank when they are apart, or 0 */
    uint32       banks;     /* 16 KiB banks in the ROM */
    uint8       *cart_ram;  /* battery RAM shown in slot 2 by $FFFC bit 3, or 0 */
                            /* (with cart_ram_size set: given at its first showing) */
    uint32       reg[4];    /* $FFFC-$FFFF */
    uint32       slot_ram[3]; /* first page of each slot showing cartridge RAM (its end: none) */
    uint32       remaps;    /* paging changes (statistics) */
    uint32       bank_mask; /* banks - 1, or 0xFFFFFFFF when banks is not a power of two */
    uint32       cart_ram_size; /* 16 KiB (one page) or 32 KiB (bit 2 selects the page) */
    uint32       mapper;    /* SMS_MAPPER_* */
    uint32       slot_ram_e[3]; /* read table entry of that RAM, as mapped */
    uint8     *(*ram_get)(void *arg); /* gives the cartridge RAM (sms_mem_ram_source) */
    void        *ram_arg;
} sms_mem;

/* The mapper an image calls for, from its header (SMS_MAPPER_*); bank1
 * is the address of its second bank. */
uint32 sms_mem_detect(const uint8 *bank1, uint32 rom_size);

/* Special write handlers in assembly (smsmem_a.s), installed by
 * sms_mem_init: _a for an image in one block, _t for an image whose
 * banks are apart (bank_tab). CONTRACT: the field offsets above match
 * their equates. */
void   sms_mem_write_a(void);
void   sms_mem_write_t(void);

/* Sets up the pages of ctx (system RAM in ctx->mram) for a ROM image and
 * the machine callbacks. The image is one block at rom, or, with banks,
 * blocks of 16 KiB anywhere: banks[b] is the address of bank b (rom then
 * the first one's), the table kept by the caller for the run. cart_ram
 * (cart_ram_size bytes, 16 or 32 KiB; a size of 0 for none) is the
 * cartridge RAM that register $FFFC can map into slot 2. With cart_ram 0
 * and a size, the memory is asked for the first time the mapper shows it
 * (sms_mem_ram_source). */
void   sms_mem_init(sms_mem *s, z80j_ctx *ctx, const uint8 *rom, const uint8 *const *banks,
                    uint32 rom_size, uint8 *cart_ram, uint32 cart_ram_size);

/* Host address of bank b (below s->banks). */
const uint8 *sms_mem_bank(const sms_mem *s, uint32 b);

/* The cartridge RAM given at its first showing: get(arg), called from the
 * mapper's write handler (inside the run), returns cart_ram_size bytes
 * filled with zeros, or 0 (the cartridge then has no RAM). */
void   sms_mem_ram_source(sms_mem *s, uint8 *(*get)(void *arg), void *arg);

/* The cartridge RAM moved to ram, its bytes already copied there by the
 * caller: the pages showing it read and write the new place from now on.
 * Called between frames, never from translated code. */
void   sms_mem_move_ram(sms_mem *s, uint8 *ram);

/* Applies the paging registers to the page tables (cartridge RAM not
 * given yet is asked for by the writes that show it, never here: the
 * power-on values do not). */
void   sms_mem_map(sms_mem *s);
#if Z80J_CHURN
/* Diagnostic build (z80j_churn): with on, the ROM pages of the slot of a
 * block key read as the key's bank shows them, the previous mapping kept;
 * without, that mapping put back. Returns 0 when the key is not a slot's
 * or (with on) the bank is not in the image. arg: the sms_mem. */
uint32 sms_mem_show(void *arg, uint32 key, uint32 on);
#endif

/* Paging registers to their power-on values (banks 0, 1, 2; the
 * Codemasters mapper starts with banks 0, 1, 0). */
void   sms_mem_reset(sms_mem *s);

/* Machine write callback, for machines that wrap this one. */
uint32 sms_mem_write(z80j_machine *m, uint32 addr, uint32 value);


#endif /* SMSMEM_H */
