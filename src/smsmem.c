/*
 * Master System / Game Gear cartridge memory for the Z80 translator.
 */

#include "smsmem.h"

#include "string.h"
#include "stddef.h"

#define SLOT_PAGES 64u          /* 16 KiB of 256-byte pages */

/* Offsets used by smsmem_a.s. */
#define CHECK_OFF(name, cond) typedef char check_##name[(cond) ? 1 : -1]
CHECK_OFF(rom, offsetof(sms_mem, rom) == 24);
CHECK_OFF(cart_ram, offsetof(sms_mem, cart_ram) == 36);
CHECK_OFF(reg, offsetof(sms_mem, reg) == 40);
CHECK_OFF(remaps, offsetof(sms_mem, remaps) == 68);
CHECK_OFF(bank_mask, offsetof(sms_mem, bank_mask) == 72);

static uint32 bank_of(const sms_mem *s, uint32 reg)
{
    if ((s->banks & (s->banks - 1)) == 0)
        return reg & (s->banks - 1);
    return reg % s->banks;
}

/* Maps the bank selected for a slot, or the cartridge RAM in slot 2. */
static void map_slot(sms_mem *s, uint32 slot)
{
    z80j_ctx *ctx = s->ctx;
    uint32 bank = bank_of(s, s->reg[slot + 1]);
    uint32 base = slot * 0x4000u;
    uint32 ram = (slot == 2) && (s->reg[0] & 0x08) && s->cart_ram != 0;
    uint32 first = slot * SLOT_PAGES;
    uint32 *r = &ctx->rtab[255 - (first + SLOT_PAGES - 1)];
    uint32 e;
    uint32 p;

    ctx->slot_bank[slot] = bank;
    if (slot == 0)
        first = 4;                      /* the first 1 KiB always shows bank 0 */
    if (ram) {
        e = JIT_ADDR(s->cart_ram) +
            (((s->reg[0] & 0x04) && s->cart_ram_size >= 0x8000u) ? 0x4000u : 0u) - base;
    } else {
        e = JIT_ADDR(s->rom) + bank * 0x4000u - base;
    }
    if (ram != s->slot_ram[slot]) {
        for (p = first; p < (slot + 1) * SLOT_PAGES; p++) {
            ctx->wtab[p] = ram ? e : 0;
            ctx->page_kind[p] = ram ? Z80J_PAGE_RAM : (uint8)Z80J_PAGE_SLOT(slot);
        }
        s->slot_ram[slot] = ram;
    }
    /* Every page of the slot has the same entry (host address - Z80
     * address): the read table is filled with a single value. */
    z80j_fill_words(r, (slot + 1) * SLOT_PAGES - first, e);
    s->remaps++;
}

void sms_mem_map(sms_mem *s)
{
    uint32 p;

    for (p = 0; p < 4; p++) {
        s->ctx->rtab[255 - p] = JIT_ADDR(s->rom);
        s->ctx->wtab[p] = 0;
        s->ctx->page_kind[p] = Z80J_PAGE_FIXED | Z80J_PAGE_RFIXED;
    }
    map_slot(s, 0);
    map_slot(s, 1);
    map_slot(s, 2);
}

void sms_mem_reset(sms_mem *s)
{
    s->reg[0] = 0;
    s->reg[1] = 0;
    s->reg[2] = 1;
    s->reg[3] = 2;
    sms_mem_map(s);
}

uint32 sms_mem_write(z80j_machine *m, uint32 addr, uint32 value)
{
    sms_mem *s = (sms_mem *)m->user;

    if (addr >= 0xC000u) {
        s->ctx->mram[addr & 0x1FFFu] = (uint8)value;
        if (addr >= 0xFFFCu) {
            uint32 r = addr - 0xFFFCu;
            s->reg[r] = value;
            map_slot(s, r == 0 ? 2 : r - 1);
            /* $FFFC maps cartridge RAM (page kinds change), the others
             * only select banks. */
            return r == 0 ? Z80J_WRITE_LEAVE : Z80J_WRITE_PAGING;
        }
    }
    return Z80J_WRITE_STAY;     /* ROM area: ignored */
}

static uint32 sms_mem_in(z80j_machine *m, uint32 port, uint32 left)
{
    (void)m;
    (void)port;
    (void)left;
    return 0xFF;
}

static uint32 sms_mem_out(z80j_machine *m, uint32 port, uint32 value, uint32 left)
{
    (void)m;
    (void)port;
    (void)value;
    (void)left;
    return 0;
}

static void sms_mem_event(z80j_machine *m)
{
    sms_mem *s = (sms_mem *)m->user;

    s->ctx->event_line = 0xFFFFFFFFu;
}

void sms_mem_init(sms_mem *s, z80j_ctx *ctx, const uint8 *rom, uint32 rom_size,
                  uint8 *cart_ram, uint32 cart_ram_size)
{
    uint32 p;
    uint32 ram = JIT_ADDR(ctx->mram);

    memset(s, 0, sizeof(*s));
    s->ctx = ctx;
    s->rom = rom;
    s->rom_size = rom_size;
    s->banks = (rom_size + 0x3FFFu) / 0x4000u;
    if (s->banks == 0)
        s->banks = 1;
    s->bank_mask = ((s->banks & (s->banks - 1)) == 0) ? s->banks - 1 : 0xFFFFFFFFu;
    s->cart_ram = cart_ram;
    s->cart_ram_size = cart_ram_size;

    s->m.in = sms_mem_in;
    s->m.out = sms_mem_out;
    s->m.write = sms_mem_write;
    s->m.event = sms_mem_event;
    s->m.user = s;
    ctx->machine = JIT_ADDR(&s->m);
    ctx->write_a = JIT_ADDR(sms_mem_write_a);
    ctx->wdata = JIT_ADDR(s);

    /* System RAM, $C000-$DFFF mirrored at $E000-$FFFF; the last page
     * holds the paging registers, so its writes go through the machine. */
    for (p = 0xC0; p < 0x100; p++) {
        uint32 e = ram - ((p < 0xE0) ? 0xC000u : 0xE000u);
        ctx->rtab[255 - p] = e;
        ctx->wtab[p] = (p == 0xFF) ? 0 : e;
        ctx->page_kind[p] = (uint8)(Z80J_PAGE_RAM | Z80J_PAGE_RFIXED |
                                    ((p == 0xFF) ? 0 : Z80J_PAGE_WFIXED));
    }
    s->slot_ram[0] = s->slot_ram[1] = s->slot_ram[2] = 0xFF;   /* not mapped yet */
    sms_mem_reset(s);
    s->remaps = 0;
}
