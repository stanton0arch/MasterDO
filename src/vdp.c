/*
 * Sega Master System video display processor: registers, memories, ports,
 * interrupts and counters. See vdp.h.
 */

#include "vdp.h"

#include "string.h"

/* Offsets used by sms_io.s. */
#define CHECK_OFF(name, cond) typedef char check_##name[(cond) ? 1 : -1]
CHECK_OFF(addr, offsetof(vdp_state, addr) == 0x00);
CHECK_OFF(ctl, offsetof(vdp_state, ctl) == 0x04);
CHECK_OFF(latch, offsetof(vdp_state, latch) == 0x08);
CHECK_OFF(buffer, offsetof(vdp_state, buffer) == 0x0C);
CHECK_OFF(cram_dirty, offsetof(vdp_state, cram_dirty) == 0x10);
CHECK_OFF(n_data_w, offsetof(vdp_state, n_data_w) == 0x14);
CHECK_OFF(n_data_r, offsetof(vdp_state, n_data_r) == 0x18);
CHECK_OFF(n_ctrl_w, offsetof(vdp_state, n_ctrl_w) == 0x1C);
CHECK_OFF(cram, offsetof(vdp_state, cram) == 0x20);
CHECK_OFF(reg, offsetof(vdp_state, reg) == 0x40);
CHECK_OFF(status, offsetof(vdp_state, status) == 0x50);
CHECK_OFF(line_flag, offsetof(vdp_state, line_flag) == 0x54);
CHECK_OFF(frame_base, offsetof(vdp_state, frame_base) == 0x68);
CHECK_OFF(lc_k, offsetof(vdp_state, lc_k) == 0x70);
CHECK_OFF(lc_val, offsetof(vdp_state, lc_val) == 0x74);
CHECK_OFF(nt_n, offsetof(vdp_state, nt_n) == 0x4804);
CHECK_OFF(n_stat_r, offsetof(vdp_state, n_stat_r) == 0x4870);
CHECK_OFF(spr_n, offsetof(vdp_state, spr_n) == 0x4874);
CHECK_OFF(spr_y, offsetof(vdp_state, spr_y) == 0x4884);
CHECK_OFF(spr_vis, offsetof(vdp_state, spr_vis) == 0x4984);
CHECK_OFF(spr_rows, offsetof(vdp_state, spr_rows) == 0x4A84);
CHECK_OFF(spr_tile_rows, offsetof(vdp_state, spr_tile_rows) == 0x4B84);
CHECK_OFF(spr_tile_ok, offsetof(vdp_state, spr_tile_ok) == 0x4D84);
CHECK_OFF(hc, offsetof(vdp_state, hc) == 0x7C);
CHECK_OFF(dirty, offsetof(vdp_state, dirty) == 0x200);
CHECK_OFF(vram, offsetof(vdp_state, vram) == 0x400);
CHECK_OFF(hs_n, offsetof(vdp_state, hs_n) == 0x4400);
CHECK_OFF(hs_log, offsetof(vdp_state, hs_log) == 0x4404);

/* Register values left by the BIOS (SMS official documentation). */
static const uint8 vdp_power_on[11] = {
    0x36, 0xA0, 0xFF, 0xFF, 0xFF, 0xFF, 0xFB, 0x00, 0x00, 0x00, 0xFF
};

/* Line of a frame T-state t, for 0 <= t < 171196: (t / 4) / 57 with a
 * multiplication (the ARM60 has no divide instruction). */
#define T_LINE(t) (((uint32)(t) >> 2) * 36793u >> 21)

/* T-state of an access, counted from the start of the current frame; it
 * is negative for an access that ends the previous frame. */
static int32 vdp_time(const vdp_state *v, uint32 left)
{
    return (int32)((v->ctx->line - v->frame_base) * VDP_LINE_T) - ((int32)left >> 8);
}

/* Lines of the current frame completed at the time of an access. */
static uint32 vdp_lines_done(const vdp_state *v, uint32 left)
{
    int32 t = vdp_time(v, left);

    return (t <= 0) ? 0 : T_LINE(t);
}

/* Line being displayed at the time of an access, and the T-state in it. */
static uint32 vdp_beam(const vdp_state *v, uint32 left, uint32 *hpos)
{
    int32 t = vdp_time(v, left);
    uint32 line;

    if (t < 0)
        t += (int32)(v->lines * VDP_LINE_T);
    line = T_LINE(t);
    *hpos = (uint32)t - line * VDP_LINE_T;
    if (line >= v->lines)
        line -= v->lines;
    return line;
}

/*
 * Brings the line counter to the end of line k - 1 of the current frame.
 * Lines 0 to VDP_ACTIVE decrement it, the others reload it; an underflow
 * reloads it and sets the line interrupt flag.
 */
static void lc_sync(vdp_state *v, uint32 k)
{
    uint32 end;
    uint32 n;

    if (k <= v->lc_k)
        return;
    if (v->lc_k <= VDP_ACTIVE) {
        end = (k <= VDP_ACTIVE) ? k : VDP_ACTIVE + 1;
        n = end - v->lc_k;
        if (n > v->lc_val) {
            /* Underflow at line lc_k + lc_val; later lines count down from
             * register 10, with an underflow every reg[10] + 1 lines. */
            n -= v->lc_val + 1;
            v->lc_val = v->reg[10] - n % ((uint32)v->reg[10] + 1);
            v->line_flag = 1;
        } else {
            v->lc_val -= n;
        }
    }
    if (k > VDP_ACTIVE + 1)
        v->lc_val = v->reg[10];
    v->lc_k = k;
}

/* Sets the interrupt line; returns nonzero when it has just been raised. */
static uint32 vdp_irq(vdp_state *v)
{
    uint32 old = v->ctx->irq_line;
    uint32 irq = ((v->status & VDP_ST_VBLANK) && (v->reg[1] & 0x20)) ||
                 (v->line_flag && (v->reg[0] & 0x10));

    v->ctx->irq_line = irq;
    return irq && !old;
}

/* Next event: the VBlank line, or the next line counter underflow while
 * line interrupts are enabled; the end of the frame otherwise. */
static void vdp_schedule(vdp_state *v)
{
    uint32 next = v->lines;
    uint32 u;

    if (!v->vbl_done)
        next = VDP_ACTIVE;
    if ((v->reg[0] & 0x10) && v->lc_k <= VDP_ACTIVE) {
        u = v->lc_k + v->lc_val + 1;
        if (u <= VDP_ACTIVE + 1 && u < next)
            next = u;
    }
    v->ctx->event_line = v->frame_base + next;
}

void vdp_reset(vdp_state *v)
{
    memset(v->vram, 0, sizeof(v->vram));
    memset(v->cram, 0, sizeof(v->cram));
    memset(v->spr_tile_ok, 0, sizeof(v->spr_tile_ok));
    memset(v->reg, 0, sizeof(v->reg));
    memcpy(v->reg, vdp_power_on, sizeof(vdp_power_on));
    v->addr = 0;
    v->ctl = 0;
    v->latch = 0;
    v->buffer = 0;
    v->status = 0;
    v->line_flag = 0;
    v->frame_base = v->ctx->line;
    v->vbl_done = 0;
    v->hs_n = 0;
    v->nt_n = 0;
    v->de_n = 0;
    v->de_start = ((uint32)v->reg[1] >> 6) & 1;
    v->lc_k = 0;
    v->lc_val = v->reg[10];
    v->frames = 0;
    memset(v->dirty, 1, sizeof(v->dirty));
    v->cram_dirty = 1;
    v->n_data_w = 0;
    v->n_data_r = 0;
    v->n_ctrl_w = 0;
    v->n_stat_r = 0;
}

void vdp_init(vdp_state *v, z80j_ctx *ctx, uint32 lines)
{
    uint32 t;

    v->ctx = ctx;
    v->spr_n = 0;
    v->spr_partial = 0;
    memset(v->spr_tile_ok, 0, sizeof(v->spr_tile_ok));
    v->lines = lines;
    if (lines == VDP_LINES_PAL) {
        v->vc_jump = 0xF2;
        v->vc_back = 0x39;
    } else {
        v->vc_jump = 0xDA;
        v->vc_back = 0x06;
    }
    /* A line is 342 pixels long: the 9-bit H counter runs from $000 to
     * $127, then from $1D2 to $1FF, and the port returns its bits 8-1.
     * Line boundaries are taken at H = $F4, when line interrupts occur. */
    for (t = 0; t < VDP_LINE_T; t++) {
        uint32 px = (318 + t * 3 / 2) % 342;
        uint32 h9 = (px < 296) ? px : px - 296 + 0x1D2;

        v->hc[t] = (uint8)(h9 >> 1);
    }
    vdp_reset(v);
}

void vdp_frame_begin(vdp_state *v)
{
    if (v->frames != 0)
        lc_sync(v, v->lines);
    v->frame_base = v->ctx->line;
    v->lc_k = 0;
    v->lc_val = v->reg[10];
    v->vbl_done = 0;
    v->hs_n = 0;
    v->nt_n = 0;
    v->de_n = 0;
    v->de_start = ((uint32)v->reg[1] >> 6) & 1;
    v->frames++;
    vdp_schedule(v);
}

void vdp_event(vdp_state *v)
{
    uint32 k = v->ctx->line - v->frame_base;

    lc_sync(v, k);
    if (k >= VDP_ACTIVE && !v->vbl_done) {
        v->vbl_done = 1;
        v->status |= VDP_ST_VBLANK;
    }
    vdp_irq(v);
    vdp_schedule(v);
}

uint32 vdp_data_read(vdp_state *v)
{
    uint32 r = v->buffer;

    v->n_data_r++;
    v->ctl &= ~VDP_PENDING;
    v->buffer = v->vram[v->addr];
    v->addr = (v->addr + 1) & (VDP_VRAM_SIZE - 1);
    return r;
}

void vdp_data_write(vdp_state *v, uint32 value)
{
    uint32 a = v->addr;

    v->n_data_w++;
    v->ctl &= ~VDP_PENDING;
    v->buffer = value;
    if (v->ctl == 3) {
        v->cram[a & 31] = (uint8)(value & 0x3F);
        v->cram_dirty = 1;
    } else {
        v->vram[a] = (uint8)value;
        v->dirty[a >> 5] = 1;
    }
    v->addr = (a + 1) & (VDP_VRAM_SIZE - 1);
}

uint32 vdp_status_read(vdp_state *v, uint32 left)
{
    uint32 s = v->status;

    /* The read also clears a line interrupt flag set since the last
     * event, which only exists in the lazy counter. */
    lc_sync(v, vdp_lines_done(v, left));
    v->status = 0;
    v->line_flag = 0;
    v->ctl &= ~VDP_PENDING;
    v->ctx->irq_line = 0;
    return s;
}

static uint32 vdp_reg_write(vdp_state *v, uint32 r, uint32 d, uint32 left)
{
    uint32 ev = v->ctx->event_line;
    uint32 raised;

    if (r > 10)
        return 0;
    /* The counter runs up to now with the old values. */
    if (r == 0 || r == 10)
        lc_sync(v, vdp_lines_done(v, left));
    if (r == 1 && ((v->reg[1] ^ d) & 0x40)) {
        /* Display enable: a change during the active display shows from
         * the next line on. */
        uint32 hpos;
        uint32 line = vdp_beam(v, left, &hpos) + 1;

        if (line < VDP_ACTIVE && v->de_n < VDP_DE_LOG)
            v->de_log[v->de_n++] = (line << 8) | ((d >> 6) & 1);
    }
    v->reg[r] = (uint8)d;
    if (r == 8 || r == 2) {
        /* The scroll and the name table of a line are taken at its
         * start: a write during the active display shows from the next
         * line on. */
        uint32 hpos;
        uint32 line = vdp_beam(v, left, &hpos) + 1;

        if (line < VDP_ACTIVE) {
            if (r == 8) {
                if (v->hs_n < VDP_HS_LOG)
                    v->hs_log[v->hs_n++] = (line << 8) | (d & 0xFF);
            } else if (v->nt_n < VDP_NT_LOG) {
                v->nt_log[v->nt_n++] = (line << 8) | (d & 0xFF);
            }
        }
    }
    if (r > 1)
        return 0;
    raised = vdp_irq(v);
    if (r == 0)
        vdp_schedule(v);
    return raised || v->ctx->event_line != ev;
}

uint32 vdp_control_write(vdp_state *v, uint32 value, uint32 left)
{
    v->n_ctrl_w++;
    if (!(v->ctl & VDP_PENDING)) {
        v->latch = value;
        v->addr = (v->addr & 0x3F00) | value;
        v->ctl |= VDP_PENDING;
        return 0;
    }
    v->ctl = value >> 6;
    v->addr = ((value & 0x3F) << 8) | v->latch;
    if (v->ctl == 0) {
        v->buffer = v->vram[v->addr];
        v->addr = (v->addr + 1) & (VDP_VRAM_SIZE - 1);
    } else if (v->ctl == 2) {
        return vdp_reg_write(v, value & 0x0F, v->latch, left);
    }
    return 0;
}

uint32 vdp_vcounter(const vdp_state *v, uint32 left)
{
    uint32 hpos;
    uint32 line = vdp_beam(v, left, &hpos);

    if (line > v->vc_jump)
        line -= v->vc_back;
    return line & 0xFF;
}

uint32 vdp_hcounter(const vdp_state *v, uint32 left)
{
    uint32 hpos;

    vdp_beam(v, left, &hpos);
    return v->hc[hpos];
}

uint32 vdp_bands(const uint32 *log, uint32 n, uint32 top_value, uint32 *tops, uint32 *vals)
{
    uint32 nb = 1;
    uint32 k;

    tops[0] = 0;
    vals[0] = top_value;
    for (k = 0; k < n; k++) {
        uint32 line = log[k] >> 8;
        uint32 val = log[k] & 0xFF;

        if (line == tops[nb - 1]) {
            vals[nb - 1] = val;
            if (nb > 1 && vals[nb - 1] == vals[nb - 2])
                nb--;
        } else if (line > tops[nb - 1] && val != vals[nb - 1]) {
            tops[nb] = line;
            vals[nb] = val;
            nb++;
        }
    }
    return nb;
}

