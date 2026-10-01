/*
 * Sega Master System machine: ports, pad, PSG writes and frames. See sms.h.
 */

#include "sms.h"

#include "stdio.h"
#include "string.h"

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

static uint32 sms_in(z80j_machine *m, uint32 port, uint32 left)
{
    sms_machine *s = (sms_machine *)m;

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
    sms_machine *s = (sms_machine *)m;
    uint32 r;

    value &= 0xFF;
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
        vdp_data_write(&s->vdp, value);
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
    vdp_event(&((sms_machine *)m)->vdp);
}

void sms_reset(sms_machine *s)
{
    z80j_ctx *ctx = s->ctx;

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
    z80j_reset(ctx);
    vdp_reset(&s->vdp);
}

/* Port handlers: the C callbacks above, except for the VDP data port,
 * the status read and the control port, handled in assembly (the C
 * callbacks remain their slow paths). */
static void sms_ports(sms_machine *s)
{
    z80j_ctx *ctx = s->ctx;
    uint32 p;

    for (p = 0; p < Z80J_PORTS; p++) {
        s->port_in[p] = JIT_ADDR(z80j_port_in_c);
        s->port_out[p] = JIT_ADDR(z80j_port_out_c);
        s->port_outn[p] = JIT_ADDR(z80j_port_outn_loop);
        if (PORT_KIND(p) == 4) {
            s->port_in[p] = JIT_ADDR(sms_vdp_data_r);
            s->port_out[p] = JIT_ADDR(sms_vdp_data_w);
            s->port_outn[p] = JIT_ADDR(sms_vdp_data_wn);
        } else if (PORT_KIND(p) == 5) {
            s->port_in[p] = JIT_ADDR(sms_vdp_stat_r);
            s->port_out[p] = JIT_ADDR(sms_vdp_ctrl_w);
        }
    }
    ctx->port_in = JIT_ADDR(s->port_in);
    ctx->port_out = JIT_ADDR(s->port_out);
    ctx->port_outn = JIT_ADDR(s->port_outn);
    ctx->mdata = JIT_ADDR(&s->vdp);
    ctx->event_a = JIT_ADDR(sms_vdp_event_a);
}

void sms_init(sms_machine *s, z80j_ctx *ctx, const uint8 *rom, uint32 rom_size,
              uint32 lines, uint8 *cart_ram, uint32 cart_ram_size)
{
    memset(s, 0, sizeof(*s));
    s->ctx = ctx;
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
    if (pause)
        s->ctx->nmi = 1;
    vdp_frame_begin(&s->vdp);
    z80j_run(s->ctx, (int32)s->vdp.lines);
}
