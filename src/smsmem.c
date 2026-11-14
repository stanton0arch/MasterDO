/*
 * Master System / Game Gear cartridge memory for the Z80 translator.
 */

#include "smsmem.h"

#include "string.h"
#include "stddef.h"

#define SLOT_PAGES 64u          /* 16 KiB of 256-byte pages (write table, page kinds) */
#define RPAGE      4u           /* 256-byte pages per 1 KiB page of the read table */

/* Offsets used by smsmem_a.s. */
#define CHECK_OFF(name, cond) typedef char check_##name[(cond) ? 1 : -1]
CHECK_OFF(rom, offsetof(sms_mem, rom) == 24);
CHECK_OFF(cart_ram, offsetof(sms_mem, cart_ram) == 36);
CHECK_OFF(reg, offsetof(sms_mem, reg) == 40);
CHECK_OFF(remaps, offsetof(sms_mem, remaps) == 68);
CHECK_OFF(bank_mask, offsetof(sms_mem, bank_mask) == 72);
CHECK_OFF(mapper, offsetof(sms_mem, mapper) == 80);

/* The cartridge RAM, asked for when the paging registers first show it
 * (none when it is not given). Kept out of map_slot, which every bank
 * switch of the Codemasters mapper runs: a call there costs it registers. */
static void take_ram(sms_mem *s)
{
    if (s->ram_get != 0)
        s->cart_ram = s->ram_get(s->ram_arg);
    if (s->cart_ram == 0)
        s->cart_ram_size = 0;
}

static void map_slot(sms_mem *s, uint32 slot);

/* A write to the Codemasters mapper's slot 1 register while its cartridge
 * RAM has not been given (sms_mem_ram_source): slot 2 has shown ROM until
 * this write shows the RAM. Out of sms_mem_write, which every bank switch
 * of that mapper runs. */
static uint32 first_ram(sms_mem *s)
{
    if ((s->reg[2] & 0x80) == 0)
        return Z80J_WRITE_PAGING;
    take_ram(s);
    map_slot(s, 2);
    return Z80J_WRITE_LEAVE;
}

static uint32 bank_of(const sms_mem *s, uint32 reg)
{
    if ((s->banks & (s->banks - 1)) == 0)
        return reg & (s->banks - 1);
    return reg % s->banks;
}

/* Maps the bank selected for a slot, and the cartridge RAM where the
 * mapper shows it: the whole of slot 2 for the Sega mapper (register
 * $FFFC bit 3), its upper half for the Codemasters mapper (bit 7 of the
 * slot 1 register). */
static void map_slot(sms_mem *s, uint32 slot)
{
    z80j_ctx *ctx = s->ctx;
    uint32 reg = s->reg[slot + 1];
    uint32 base = slot * 0x4000u;
    uint32 first = slot * SLOT_PAGES;
    uint32 end = first + SLOT_PAGES;
    uint32 ram_first = end;             /* first page showing cartridge RAM */
    uint32 ram_e = 0;
    uint32 rom_e;
    uint32 bank;
    uint32 p;

    if (s->mapper == SMS_MAPPER_CODEMASTERS) {
        reg &= 0x7F;
        if (slot == 2 && (s->reg[2] & 0x80) && s->cart_ram != 0) {
            ram_first = first + SLOT_PAGES / 2;
            ram_e = JIT_ADDR(s->cart_ram) - 0xA000u;
        }
    } else {
        if (slot == 0)
            first = 4;                  /* the first 1 KiB always shows bank 0 */
        if (slot == 2 && (s->reg[0] & 0x08) && s->cart_ram != 0) {
            ram_first = first;
            ram_e = JIT_ADDR(s->cart_ram) +
                    (((s->reg[0] & 0x04) && s->cart_ram_size >= 0x8000u) ? 0x4000u : 0u) - base;
        }
    }
    bank = bank_of(s, reg);
    ctx->slot_bank[slot] = bank;
    rom_e = JIT_ADDR(s->rom) + bank * 0x4000u - base;
    if (ram_first != s->slot_ram[slot] || ram_e != s->slot_ram_e[slot]) {
        for (p = first; p < end; p++) {
            ctx->wtab[p] = (p >= ram_first) ? ram_e : 0;
            ctx->page_kind[p] = (p >= ram_first) ? Z80J_PAGE_RAM : (uint8)Z80J_PAGE_SLOT(slot);
        }
        s->slot_ram[slot] = ram_first;
        s->slot_ram_e[slot] = ram_e;
    }
    /* Every page of a range has the same entry (host address - Z80
     * address): the read table, in 1 KiB pages (page q at rtab[63 - q]),
     * gets a single value per range; the ranges start on 1 KiB boundaries
     * (the fixed first 1 KiB of slot 0, the Codemasters RAM at $A000). */
    for (p = first / RPAGE; p < end / RPAGE; p++)
        ctx->rtab[63 - p] = (p * RPAGE >= ram_first) ? ram_e : rom_e;
    s->remaps++;
}

void sms_mem_map(sms_mem *s)
{
    uint32 p;

    if (s->mapper != SMS_MAPPER_CODEMASTERS) {
        s->ctx->rtab[63] = JIT_ADDR(s->rom);   /* the first 1 KiB */
        for (p = 0; p < RPAGE; p++) {
            s->ctx->wtab[p] = 0;
            s->ctx->page_kind[p] = Z80J_PAGE_FIXED | Z80J_PAGE_RFIXED;
        }
    }
    map_slot(s, 0);
    map_slot(s, 1);
    map_slot(s, 2);
}

#if Z80J_CHURN
static uint32 show_bank;                /* the slot's bank before sms_mem_show */
static uint32 show_rtab[SLOT_PAGES / RPAGE];

uint32 sms_mem_show(void *arg, uint32 key, uint32 on)
{
    sms_mem *s = (sms_mem *)arg;
    z80j_ctx *ctx = s->ctx;
    uint32 kind = key >> 16;
    uint32 slot = kind - Z80J_PAGE_SLOT(0);
    uint32 q0 = slot * (SLOT_PAGES / RPAGE);
    uint32 bank = key & 0xFFFFu;
    uint32 rom_e;
    uint32 q;

    if (kind < Z80J_PAGE_SLOT(0) || kind > Z80J_PAGE_SLOT(2))
        return 0;
    if (!on) {
        ctx->slot_bank[slot] = show_bank;
        for (q = 0; q < SLOT_PAGES / RPAGE; q++)
            ctx->rtab[63 - (q0 + q)] = show_rtab[q];
        return 1;
    }
    if (bank >= s->banks)
        return 0;
    show_bank = ctx->slot_bank[slot];
    rom_e = JIT_ADDR(s->rom) + bank * 0x4000u - slot * 0x4000u;
    for (q = 0; q < SLOT_PAGES / RPAGE; q++) {
        show_rtab[q] = ctx->rtab[63 - (q0 + q)];
        if ((ctx->page_kind[(q0 + q) * RPAGE] & Z80J_PAGE_KIND) == kind)
            ctx->rtab[63 - (q0 + q)] = rom_e;
    }
    ctx->slot_bank[slot] = bank;
    return 1;
}
#endif

void sms_mem_reset(sms_mem *s)
{
    s->reg[0] = 0;
    s->reg[1] = 0;
    s->reg[2] = 1;
    s->reg[3] = (s->mapper == SMS_MAPPER_CODEMASTERS) ? 0 : 2;
    sms_mem_map(s);
}

uint32 sms_mem_write(z80j_machine *m, uint32 addr, uint32 value)
{
    sms_mem *s = (sms_mem *)m->user;

    if (addr >= 0xC000u) {
        s->ctx->mram[addr & 0x1FFFu] = (uint8)value;
        if (addr >= 0xFFFCu && s->mapper != SMS_MAPPER_CODEMASTERS) {
            uint32 r = addr - 0xFFFCu;
            s->reg[r] = value;
            map_slot(s, r == 0 ? 2 : r - 1);
            /* $FFFC maps cartridge RAM (page kinds change), the others
             * only select banks. */
            return r == 0 ? Z80J_WRITE_LEAVE : Z80J_WRITE_PAGING;
        }
        return Z80J_WRITE_STAY;
    }
    if (s->mapper == SMS_MAPPER_CODEMASTERS) {
        /* A write anywhere in a slot selects its bank; the slot 1
         * register also maps the cartridge RAM into slot 2. */
        uint32 slot = addr >> 14;

        s->reg[slot + 1] = value;
        map_slot(s, slot);
        if (slot == 1 && s->cart_ram != 0) {
            map_slot(s, 2);
            return Z80J_WRITE_LEAVE;
        }
        if (slot == 1 && s->cart_ram_size != 0)
            return first_ram(s);
        return Z80J_WRITE_PAGING;
    }
    return Z80J_WRITE_STAY;     /* ROM area: ignored */
}

/* The write callback of the Sega mapper while its cartridge RAM has not
 * been given: the write to $FFFC that first shows it asks for it, then
 * sms_mem_write takes over for good. The test stays off sms_mem_write,
 * which every write to $FFFC goes through. */
static uint32 write_first(z80j_machine *m, uint32 addr, uint32 value)
{
    sms_mem *s = (sms_mem *)m->user;

    if (addr == 0xFFFCu && (value & 0x08)) {
        take_ram(s);
        m->write = sms_mem_write;
    }
    return sms_mem_write(m, addr, value);
}

void sms_mem_ram_source(sms_mem *s, uint8 *(*get)(void *arg), void *arg)
{
    s->ram_get = get;
    s->ram_arg = arg;
    if (s->cart_ram == 0 && s->cart_ram_size != 0 && s->mapper != SMS_MAPPER_CODEMASTERS)
        s->m.write = write_first;
}

void sms_mem_move_ram(sms_mem *s, uint8 *ram)
{
    s->cart_ram = ram;
    map_slot(s, 2);
}

/* The Codemasters header at $7FE0: the number of 16 KiB banks, a date,
 * then a checksum and its complement (the two words add up to $10000). */
uint32 sms_mem_detect(const uint8 *rom, uint32 rom_size)
{
    const uint8 *h = rom + 0x7FE0;
    uint32 sum;

    if (rom_size < 0x8000u)
        return SMS_MAPPER_SEGA;
    sum = ((uint32)h[6] | ((uint32)h[7] << 8)) + ((uint32)h[8] | ((uint32)h[9] << 8));
    if (sum == 0x10000u && (uint32)h[0] == (rom_size + 0x3FFFu) / 0x4000u)
        return SMS_MAPPER_CODEMASTERS;
    return SMS_MAPPER_SEGA;
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
    s->mapper = sms_mem_detect(rom, rom_size);

    s->m.in = sms_mem_in;
    s->m.out = sms_mem_out;
    s->m.write = sms_mem_write;
    s->m.event = sms_mem_event;
    s->m.user = s;
    ctx->machine = JIT_ADDR(&s->m);
    ctx->write_a = JIT_ADDR(sms_mem_write_a);
    ctx->wdata = JIT_ADDR(s);

    /* System RAM, $C000-$DFFF mirrored at $E000-$FFFF; the last page
     * holds the paging registers of the Sega mapper, so its writes go
     * through the machine (the assembly handler stores the byte and
     * leaves the page alone under the Codemasters mapper). */
    for (p = 0xC0; p < 0x100; p++) {
        uint32 e = ram - ((p < 0xE0) ? 0xC000u : 0xE000u);
        ctx->rtab[63 - p / RPAGE] = e;
        ctx->wtab[p] = (p == 0xFF) ? 0 : e;
        ctx->page_kind[p] = (uint8)(Z80J_PAGE_RAM | Z80J_PAGE_RFIXED |
                                    ((p == 0xFF) ? 0 : Z80J_PAGE_WFIXED));
    }
    s->slot_ram[0] = s->slot_ram[1] = s->slot_ram[2] = 0xFFFFFFFFu;   /* not mapped yet */
    sms_mem_reset(s);
    s->remaps = 0;
}
