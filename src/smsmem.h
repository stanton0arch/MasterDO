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

/* Images larger than this are not loaded as one block: their banks are
 * read into a bank cache (sms_banks_new) when the mapper shows them. */
#define SMS_BLOCK_MAX   (512 * 1024)
/* Entries of a bank table: every value of an 8-bit bank register. */
#define SMS_BANK_TAB    256
/* Buffers of a bank cache: at most, and at least (bank 0, the banks the
 * three slots show, and one to read into). */
#define SMS_BANK_BUFS   64
#define SMS_BANK_MIN    5
/* VRAM a bank cache leaves to the run: what a 512 KiB image leaves (the
 * renderer's state and tile copy, the pad log) and a margin. */
#define SMS_BANK_KEEP   (42 * 1024)
/* Buffers a bank cache takes in DRAM after those of VRAM: the DRAM of
 * four code zones, which the code buffer of such an image goes without
 * (game_init). The banks a game maps over a scene or a fight outnumber
 * the VRAM buffers: each bank read while running holds the picture for
 * about four display periods, an eviction of code costs much less. */
#define SMS_BANK_DRAM   8

/* Reads bank b of the image into to (16 KiB); returns 0, or not 0 on an
 * error. */
typedef uint32 (*sms_bank_read)(void *arg, uint32 b, uint8 *to);

/* The banks of an image not loaded as one block. The table gives the host
 * address of each bank, 0 while the bank is absent (always past the bank
 * count); a cache of nbuf buffers of 16 KiB holds the banks present, and
 * a bank the mapper shows while absent is read into the buffer of the
 * bank shown least lately (bank 0, the fixed first 1 KiB, and the banks
 * the slots show stay; the banks not shown since they were read go
 * first, the highest first). Without a reader every bank is present.
 * CONTRACT: tab and last at offsets 0 and 1024 (smsmem_a.s). */
typedef struct {
    const uint8  *tab[SMS_BANK_TAB];
    uint32        last[SMS_BANK_TAB];   /* paging count when the bank last entered or
                                           left a slot; 0: not since it was read */
    uint8        *buf[SMS_BANK_BUFS];   /* VRAM ones in one block from buf[0], this
                                           structure after; then the DRAM ones in one
                                           block from buf[nbuf - ndram] */
    uint8         held[SMS_BANK_BUFS];  /* the bank in each buffer */
    uint32        nbuf;
    uint32        ndram;                /* buffers in DRAM, among the nbuf */
    sms_bank_read read;
    void         *arg;
    /* Called after each bank read while running (the cache's misses),
     * with its time in microseconds. */
    void        (*note)(void *arg, uint32 b, uint32 us);
    void         *note_arg;
    uint32        reads;                /* banks read while running, their time, */
    uint32        read_us;              /* the reads that failed */
    uint32        errors;
} sms_banks;

/* A bank cache for an image of `banks` banks read by read(arg, b, to):
 * the buffers (at most max in all, 0 for no limit) in one block of VRAM
 * leaving SMS_BANK_KEEP of it, then up to dram more in one block of DRAM
 * (none when it cannot be had), filled with banks 0 to nbuf - 1. Returns
 * 0 when the memory or a read fails. */
sms_banks *sms_banks_new(uint32 banks, uint32 max, uint32 dram, sms_bank_read read, void *arg);
void       sms_banks_free(sms_banks *t);

typedef struct {
    z80j_machine m;         /* callbacks; m.user points to this structure */
    z80j_ctx    *ctx;
    const uint8 *rom;       /* the image, or its first bank when its banks are apart */
    sms_banks   *bank_tab;  /* the banks when they are apart, or 0 */
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
    uint32       ram_hides; /* writes that hid the cartridge RAM again (Sega mapper) */
} sms_mem;

/* Does slot 2 show cartridge RAM now? (Its first 256-byte page showing
 * RAM is before the end of the slot, page $C0.) */
#define SMS_RAM_SHOWN(s) ((s)->slot_ram[2] < 0xC0u)

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
 * blocks of 16 KiB anywhere: banks->tab[b] is the address of bank b (rom
 * then the first one's), kept by the caller for the run. cart_ram
 * (cart_ram_size bytes, 16 or 32 KiB; a size of 0 for none) is the
 * cartridge RAM that register $FFFC can map into slot 2. With cart_ram 0
 * and a size, the memory is asked for the first time the mapper shows it
 * (sms_mem_ram_source). */
void   sms_mem_init(sms_mem *s, z80j_ctx *ctx, const uint8 *rom, sms_banks *banks,
                    uint32 rom_size, uint8 *cart_ram, uint32 cart_ram_size);

/* Host address of bank b (below s->banks; present: banks 0 and 1 are at
 * the set-up). */
const uint8 *sms_mem_bank(const sms_mem *s, uint32 b);

/* The cartridge RAM given at its first showing: get(arg), called from the
 * mapper's write handler (inside the run), returns cart_ram_size bytes
 * filled with zeros, or 0 (the cartridge then has no RAM).
 *
 * The Sega mapper counts in ram_hides the writes to $FFFC that hide the
 * RAM again (sms_mem_write, which every change of $FFFC reaches): a game
 * shows its battery RAM to write its save and hides it when the save is
 * done. The Codemasters mapper counts none (its RAM is seen shown or not
 * between frames, SMS_RAM_SHOWN). */
void   sms_mem_ram_source(sms_mem *s, uint8 *(*get)(void *arg), void *arg);

/* The cartridge RAM moved to ram, its bytes already copied there by the
 * caller: the pages showing it read and write the new place from now on.
 * Called between frames, never from translated code. */
void   sms_mem_move_ram(sms_mem *s, uint8 *ram);

/* Applies the paging registers to the page tables (cartridge RAM not
 * given yet is asked for by the writes that show it, never here: the
 * power-on values do not). */
void   sms_mem_map(sms_mem *s);
/* For the translator (z80j_set_show): with on, the ROM pages of the slot
 * of a block key read as the key's bank shows them, the previous mapping
 * kept; without, that mapping put back. Returns 0 when the key is not a
 * slot's or (with on) the bank is not in memory. arg: the sms_mem. */
uint32 sms_mem_show(void *arg, uint32 key, uint32 on);

/* Paging registers to their power-on values (banks 0, 1, 2; the
 * Codemasters mapper starts with banks 0, 1, 0). */
void   sms_mem_reset(sms_mem *s);

/* Machine write callback, for machines that wrap this one. */
uint32 sms_mem_write(z80j_machine *m, uint32 addr, uint32 value);


#endif /* SMSMEM_H */
