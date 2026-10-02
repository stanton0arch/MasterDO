#ifndef VDP_H
#define VDP_H

/*
 * Sega Master System video display processor (VDP), mode 4: registers,
 * video RAM, colour RAM, control and data ports, status flags, VBlank and
 * line interrupts, V and H counters. Nothing is drawn here: tiles written
 * to video RAM are marked dirty and colour RAM writes set a flag, for the
 * code that converts them.
 *
 * Timing follows the Z80 translator (z80jit.h). The machine runs one frame
 * per z80j_run() call, after vdp_frame_begin(). VBlank and line interrupts
 * are machine events at line boundaries; port accesses derive the beam
 * position from the time given by the translator. The line counter is
 * kept lazily: it is brought up to date at events and at the port
 * accesses that depend on it, and line interrupt events are only
 * scheduled while line interrupts are enabled. Writes to the horizontal
 * scroll register, to the name table base register and to the display
 * enable bit during the active display are recorded with the first line
 * they affect, so that the picture can be drawn in bands (games using
 * line interrupts for raster effects write the scroll on every line).
 *
 * Line model (192-line mode): the line counter is decremented at the end
 * of lines 0 to 192 and reloaded from register 10 at the end of the other
 * lines; an underflow reloads it and sets the line interrupt flag. The
 * VBlank flag is set at the start of line 192.
 *
 * Sprites: the sprite attribute table is evaluated once per frame by
 * vdp_sprites(), as the hardware does line by line: the first eight
 * sprites of the table covering a line are displayed, the ninth sets the
 * overflow flag and is hidden on that line, and two displayed sprites
 * with opaque pixels at the same position set the collision flag. Both
 * flags stay set until the status register is read. Zoomed sprites
 * (register 1 bit 0) are 16 x 16 or 16 x 32 pixels; every sprite of a
 * line is zoomed, as on the later VDP revisions.
 */

#include "types.h"
#include "z80jit.h"

#define VDP_VRAM_SIZE   0x4000
#define VDP_TILES       512         /* 32-byte tiles in video RAM */
#define VDP_ACTIVE      192         /* active lines (mode 4, 192-line mode) */
#define VDP_LINE_T      228         /* T-states per line */
#define VDP_LINES_NTSC  262
#define VDP_LINES_PAL   313
#define VDP_HS_LOG      256         /* register 8 writes recorded per frame */
#define VDP_NT_LOG      16          /* register 2 writes recorded per frame */
#define VDP_DE_LOG      8           /* display enable changes recorded per frame */
#define VDP_SPRITES     64

/* Status flags. */
#define VDP_ST_VBLANK   0x80
#define VDP_ST_OVERFLOW 0x40
#define VDP_ST_COLLIDE  0x20

/* vdp_state.ctl: access code of the last control word (0 VRAM read,
 * 1 VRAM write, 2 register write, 3 CRAM write), and VDP_PENDING while the
 * second byte of a control word is expected. */
#define VDP_PENDING     0x100

/*
 * The port state comes first: the data and control ports also have
 * handlers in assembly (sms_io.s), which address it. CONTRACT with the
 * equates of sms_io.s - vdp.c checks the offsets at compile time.
 */
typedef struct {
    uint32  addr;           /* +0x00: 14-bit address register */
    uint32  ctl;            /* +0x04: code | VDP_PENDING */
    uint32  latch;          /* +0x08: first byte of a control word */
    uint32  buffer;         /* +0x0C: read buffer */
    uint32  cram_dirty;     /* +0x10: colour RAM written since last cleared */
    uint32  n_data_w;       /* +0x14: data port writes */
    uint32  n_data_r;       /* +0x18: data port reads */
    uint32  n_ctrl_w;       /* +0x1C: control port writes */
    uint8   cram[32];       /* +0x20 */
    uint8   reg[16];        /* +0x40 */
    uint32  status;         /* VDP_ST_* */
    uint32  line_flag;      /* line interrupt pending */
    /* Timing. */
    z80j_ctx *ctx;
    uint32  lines;          /* lines per frame */
    uint32  vc_jump;        /* last V counter value before the jump back */
    uint32  vc_back;        /* amount the V counter jumps back by */
    uint32  frame_base;     /* ctx->line at the start of the current frame */
    uint32  vbl_done;       /* the VBlank event of this frame has run */
    uint32  lc_k;           /* line counter valid at the end of line lc_k - 1 */
    uint32  lc_val;
    uint32  frames;
    uint8   hc[VDP_LINE_T]; /* +0x7C: H counter for each T-state of a line */
    uint8   pad[0x200 - 0x7C - VDP_LINE_T];
    uint8   dirty[VDP_TILES];       /* +0x200: nonzero: tile written */
    uint8   vram[VDP_VRAM_SIZE];    /* +0x400 */
    /* Writes to register 8 during the active display of the current
     * frame, for the renderer: (first line affected << 8) | value. */
    uint32  hs_n;                   /* +0x4400 */
    uint32  hs_log[VDP_HS_LOG];     /* +0x4404 */
    /* Writes to register 2 (name table base), the same way. */
    uint32  nt_n;
    uint32  nt_log[VDP_NT_LOG];
    /* Changes of the display enable bit (register 1 bit 6) during the
     * active display: (first line affected << 8) | new state (0 or 1). */
    uint32  de_n;
    uint32  de_log[VDP_DE_LOG];
    uint32  de_start;       /* display enabled at the start of the frame */
    uint32  n_stat_r;       /* status reads handled in assembly */
    /* Sprite evaluation of the frame (vdp_sprites). */
    uint32  spr_n;          /* sprites before the terminator */
    uint32  spr_h;          /* lines per sprite: 8, 16 or 32 */
    uint32  spr_zoom;       /* register 1 bit 0 */
    uint32  spr_partial;    /* some sprite is hidden on some of its lines */
    int32   spr_y[VDP_SPRITES];     /* screen line of the first row */
    uint32  spr_vis[VDP_SPRITES];   /* bit k: line spr_y + k displayed */
} vdp_state;

/* Power-on state; lines is VDP_LINES_NTSC or VDP_LINES_PAL. */
void   vdp_init(vdp_state *v, z80j_ctx *ctx, uint32 lines);
void   vdp_reset(vdp_state *v);

/* Starts a frame at ctx->line and schedules its first event. */
void   vdp_frame_begin(vdp_state *v);

/* Machine event at line boundary ctx->line. */
void   vdp_event(vdp_state *v);

/* Ports. left is the translator's time argument (see Z80J_TIME). The
 * control port write returns nonzero when the translated block must be
 * left (interrupt line raised or event moved). */
uint32 vdp_data_read(vdp_state *v);
void   vdp_data_write(vdp_state *v, uint32 value);
uint32 vdp_status_read(vdp_state *v, uint32 left);
uint32 vdp_control_write(vdp_state *v, uint32 value, uint32 left);
uint32 vdp_vcounter(const vdp_state *v, uint32 left);

/* Bands of the picture for a value written during the active display
 * (log = hs_log, nt_log or de_log with its count): tops[k] is the first line of
 * band k and vals[k] its value; band 0 starts at line 0 with top_value.
 * For the scroll that is the value held at the end of the frame, written
 * for the picture that goes with the video RAM as it stands; for the
 * display enable it is the state at the start of the frame (de_start): a
 * display switched off in the vertical blank for an upload blanks the
 * top lines of the next frame, where the write that turns it back on is
 * recorded, not the whole picture of this one. Consecutive equal values
 * merge. Returns the band count, at most n + 1. */
uint32 vdp_bands(const uint32 *log, uint32 n, uint32 top_value, uint32 *tops, uint32 *vals);
uint32 vdp_hcounter(const vdp_state *v, uint32 left);

/* Evaluates the sprite attribute table for the picture of the frame
 * (spr_* fields): the per-line limit, the overflow and collision flags,
 * which are set in the status and returned. */
uint32 vdp_sprites(vdp_state *v);

#endif /* VDP_H */
