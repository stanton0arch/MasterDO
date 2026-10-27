/*
 * Sega Master System machine: ports, pad, PSG writes and frames. See sms.h.
 */

#include "sms.h"

#include "stdio.h"
#include "string.h"
#include "stddef.h"

/* The machine from the callback block of its mapper (sms_mem.m). */
#define SMS_OF(m) ((sms_machine *)((uint8 *)(m) - offsetof(sms_machine, mem)))

/* Layout relied on by the assembly port handlers (sms_io.s): the VDP
 * state at SMS_VDP_OFFSET from the context, which game.c places the
 * machine right after. */
#define CHECK_OFF(name, cond) typedef char check_##name[(cond) ? 1 : -1]
CHECK_OFF(ctx_size, sizeof(z80j_ctx) == SMS_VDP_OFFSET);
CHECK_OFF(vdp_first, offsetof(sms_machine, vdp) == 0);
CHECK_OFF(psg_reg, offsetof(sms_machine, psg_reg) == SMS_PSG_REG);
CHECK_OFF(psg_latch, offsetof(sms_machine, psg_latch) == SMS_PSG_REG - 4);
CHECK_OFF(psg_w, offsetof(sms_machine, io.psg_w) == SMS_PSG_W);

/* Port index: address bits 7 and 6, then bit 0. */
#define PORT_KIND(p) ((((p) >> 5) & 6) | ((p) & 1))

/* SN76489 register write: a latch byte (bit 7 set) selects a register and
 * gives its low 4 bits; a data byte gives the high 6 bits of a tone
 * register, or the whole value of the other registers. */
static void psg_write(sms_machine *s, uint32 v)
{
    uint32 r;

    if (v & 0x80) {
        r = (v >> 4) & 7;
        s->psg_latch = r;
        if ((r & 1) == 0 && r != 6)
            s->psg_reg[r] = (s->psg_reg[r] & 0x3F0) | (v & 0x0F);
        else
            s->psg_reg[r] = v & 0x0F;
    } else {
        r = s->psg_latch;
        if ((r & 1) == 0 && r != 6)
            s->psg_reg[r] = (s->psg_reg[r] & 0x00F) | ((v & 0x3F) << 4);
        else
            s->psg_reg[r] = v & 0x0F;
    }
}

/* Port $DD: port B pins (nothing connected), RESET button released, and
 * the TH pins, which read back their output level when set as outputs
 * (export console). */
static uint32 port_dd(const sms_machine *s)
{
    uint32 v = 0x3F;
    uint32 c = s->io_ctrl;

    if (!(c & 0x02) ? (c & 0x20) : 1)
        v |= 0x40;
    if (!(c & 0x08) ? (c & 0x80) : 1)
        v |= 0x80;
    return v;
}

/* SDSC debug console: printable characters are collected and each line
 * goes to the debug log. */
static void sdsc_write(sms_machine *s, uint32 c)
{
    if (c == '\n' || c == '\r' || s->sdsc_len == sizeof(s->sdsc) - 1) {
        if (s->sdsc_len != 0) {
            s->sdsc[s->sdsc_len] = 0;
            printf("SDSC: %s\n", s->sdsc);
            s->sdsc_len = 0;
        }
        if (c == '\n' || c == '\r')
            return;
    }
    if (c >= 0x20 && c < 0x7F)
        s->sdsc[s->sdsc_len++] = (char)c;
}

/* Game Gear registers, ports $00-$06. */
static uint32 gg_in(sms_machine *s, uint32 port)
{
    s->io.other++;
    if (port == 0)
        return (s->gg_start ? 0 : 0x80) | (s->gg_japan ? 0 : 0x40);
    return s->gg_port[port];
}

static void gg_out(sms_machine *s, uint32 port, uint32 value)
{
    s->io.other++;
    if (port != 0 && port != 4)         /* $00 and $04 are read only */
        s->gg_port[port] = value;
}

static uint32 sms_in(z80j_machine *m, uint32 port, uint32 left)
{
    sms_machine *s = SMS_OF(m);

    if (s->gg && (port & 0xFF) < 7)
        return gg_in(s, port & 0xFF);
    switch (PORT_KIND(port)) {
    case 2:
        s->io.counter_r++;
        return vdp_vcounter(&s->vdp, left);
    case 3:
        s->io.counter_r++;
        return vdp_hcounter(&s->vdp, left);
    case 4:
        return vdp_data_read(&s->vdp);
    case 5:
        s->io.vdp_stat_r++;
        return vdp_status_read(&s->vdp, left);
    case 6:
        s->io.pad_r++;
        return (s->mem_ctrl & 0x04) ? 0xFF : (~s->pad & 0xFF);
    case 7:
        s->io.pad_r++;
        return (s->mem_ctrl & 0x04) ? 0xFF : port_dd(s);
    default:
        s->io.other++;
        return 0xFF;
    }
}

static uint32 sms_out(z80j_machine *m, uint32 port, uint32 value, uint32 left)
{
    sms_machine *s = SMS_OF(m);
    uint32 r;

    value &= 0xFF;
    if (s->gg && (port & 0xFF) < 7) {
        gg_out(s, port & 0xFF, value);
        return 0;
    }
    switch (PORT_KIND(port)) {
    case 0:
        s->io.other++;
        s->mem_ctrl = value;
        return 0;
    case 1:
        s->io.other++;
        s->io_ctrl = value;
        return 0;
    case 2:
    case 3:
        s->io.psg_w++;
        psg_write(s, value);
        return 0;
    case 4:
        vdp_data_write(&s->vdp, value, left);
        return 0;
    case 5:
        r = vdp_control_write(&s->vdp, value, left);
        s->io.leaves += r;
        return r;
    default:
        s->io.other++;
        if ((port & 0xFF) == 0xFD)
            sdsc_write(s, value);
        return 0;
    }
}

static void sms_event(z80j_machine *m)
{
    vdp_event(&SMS_OF(m)->vdp);
}

/* Ports $00-$06 of the Game Gear at power-on. */
static const uint8 gg_power_on[7] = { 0xC0, 0x7F, 0xFF, 0x00, 0xFF, 0x00, 0xFF };

void sms_reset(sms_machine *s)
{
    z80j_ctx *ctx = s->ctx;
    uint32 r;

    sms_mem_reset(&s->mem);
    memset(ctx->mram, 0, sizeof(ctx->mram));
    /* The BIOS keeps the last value written to port $3E at $C000. */
    s->mem_ctrl = 0xAB;
    ctx->mram[0] = (uint8)s->mem_ctrl;
    s->io_ctrl = 0xFF;
    s->pad = 0;
    s->psg_latch = 0;
    memset(s->psg_reg, 0, sizeof(s->psg_reg));
    s->psg_reg[1] = s->psg_reg[3] = s->psg_reg[5] = s->psg_reg[7] = 0x0F;
    memset(&s->io, 0, sizeof(s->io));
    s->sdsc_len = 0;
    for (r = 0; r < 7; r++)
        s->gg_port[r] = gg_power_on[r];
    s->gg_start = 0;
    z80j_reset(ctx);
    vdp_reset(&s->vdp);
}

/* Port handlers, written into the tables of the context: the C callbacks
 * above, except for the VDP data port, the status read, the control port,
 * the V and H counter reads and the PSG writes, handled in assembly (the
 * C callbacks remain their slow paths). */
static void sms_ports(sms_machine *s)
{
    z80j_ctx *ctx = s->ctx;
    uint32 p;

    for (p = 0; p < Z80J_PORTS; p++) {
        ctx->in_tab[p] = JIT_ADDR(z80j_port_in_c);
        ctx->out_tab[p] = JIT_ADDR(z80j_port_out_c);
        ctx->outn_tab[p] = JIT_ADDR(z80j_port_outn_loop);
        if (PORT_KIND(p) == 4) {
            ctx->in_tab[p] = JIT_ADDR(sms_vdp_data_r);
            ctx->out_tab[p] = JIT_ADDR(sms_vdp_data_w);
            ctx->outn_tab[p] = JIT_ADDR(sms_vdp_data_wn);
        } else if (PORT_KIND(p) == 5) {
            ctx->in_tab[p] = JIT_ADDR(sms_vdp_stat_r);
            ctx->out_tab[p] = JIT_ADDR(sms_vdp_ctrl_w);
        } else if (PORT_KIND(p) == 2) {
            ctx->in_tab[p] = JIT_ADDR(sms_vdp_vc_r);
            ctx->out_tab[p] = JIT_ADDR(sms_psg_w);
        } else if (PORT_KIND(p) == 3) {
            ctx->in_tab[p] = JIT_ADDR(sms_vdp_hc_r);
            ctx->out_tab[p] = JIT_ADDR(sms_psg_w);
        }
    }
    ctx->port_in = JIT_ADDR(ctx->in_tab);
    ctx->port_out = JIT_ADDR(ctx->out_tab);
    ctx->port_outn = JIT_ADDR(ctx->outn_tab);
    ctx->mdata = JIT_ADDR(&s->vdp);
    ctx->event_a = JIT_ADDR(sms_vdp_event_a);
}

uint32 sms_rom_region(const uint8 *rom, uint32 rom_size)
{
    static const uint32 at[3] = { 0x7FF0, 0x3FF0, 0x1FF0 };
    uint32 k;

    for (k = 0; k < 3; k++) {
        const uint8 *h = rom + at[k];

        if (at[k] + 16 <= rom_size && memcmp(h, "TMR SEGA", 8) == 0)
            return (uint32)h[0x0F] >> 4;
    }
    return 0;
}

void sms_init(sms_machine *s, z80j_ctx *ctx, const uint8 *rom, uint32 rom_size,
              uint32 lines, uint8 *cart_ram, uint32 cart_ram_size, uint32 gg)
{
    memset(s, 0, sizeof(*s));
    s->ctx = ctx;
    s->gg = gg != 0;
    s->gg_japan = sms_rom_region(rom, rom_size) == 5;
    s->vdp.gg = s->gg;
    sms_mem_init(&s->mem, ctx, rom, rom_size, cart_ram, cart_ram_size);
    s->mem.m.in = sms_in;
    s->mem.m.out = sms_out;
    s->mem.m.event = sms_event;
    vdp_init(&s->vdp, ctx, lines);
    sms_ports(s);
    sms_reset(s);
}

void sms_frame(sms_machine *s, uint32 pad, uint32 pause)
{
    s->pad = pad;
    if (s->gg)
        s->gg_start = pause;
    else if (pause)
        s->ctx->nmi = 1;
    vdp_frame_begin(&s->vdp);
    z80j_run(s->ctx, (int32)s->vdp.lines);
}
