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
 * Display height: mode 4 shows 192 lines, or 224 / 240 lines when
 * register 0 bit 1 (M2) is set with register 1 bit 4 (M1) / bit 3 (M3)
 * (the extended modes of the later VDP revisions). The extended modes
 * take a name table of 32 rows (at (register 2 & $0C) << 10 | $0700, the
 * vertical scroll wrapping at 256 instead of 224) and have no sprite list
 * terminator.
 *
 * Line model: the line counter is decremented at the end of lines 0 to
 * the last active line + 1 and reloaded from register 10 at the end of
 * the other lines; an underflow reloads it and sets the line interrupt
 * flag. The VBlank flag is set at the end of the line after the active
 * display (192, 224 or 240), when the V counter leaves $C0 ($E0, $F0):
 * the same moment as the last decrement of the line counter, so that the
 * status read of a VBlank handler also clears a line interrupt raised by
 * that decrement.
 *
 * Colour RAM writes during the active display are recorded with the line
 * they affect, like the register writes, so that the picture can change
 * its palette from that line on (a horizon drawn in another colour).
 *
 * Game Gear mode (gg set by the machine): the colour RAM holds 32
 * colours of 12 bits (64 bytes, gg_cram: an even byte GGGGRRRR, an odd
 * byte ----BBBB). A data port write to an even colour RAM address is
 * latched; a write to an odd address stores the latched byte and its own
 * low nibble, so that a colour changes in one go. The rest of the chip is
 * the Master System's; the LCD shows a 160 x 144 window of the picture
 * (VDP_GG_*), which the renderer cuts out.
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
#define VDP_ACTIVE_STD  192         /* active lines of the usual mode */
#define VDP_ACTIVE_MAX  240         /* active lines of the tallest mode */
#define VDP_LINE_T      228         /* T-states per line */
#define VDP_LINES_NTSC  262
#define VDP_LINES_PAL   313
#define VDP_HS_LOG      256         /* register 8 writes recorded per frame */
#define VDP_NT_LOG      16          /* register 2 writes recorded per frame */
#define VDP_DE_LOG      8           /* display enable changes recorded per frame */
#define VDP_CR_LOG      64          /* colour RAM writes recorded per frame */
#define VDP_PAL_BANDS   8           /* palettes of a picture (vdp_palette_bands) */
#define VDP_PAL_WORDS   16          /* words of a band's colours: 8, or 16 in Game
                                       Gear mode (vdp_pal_words) */
#define VDP_GG_CRAM     64          /* bytes of the Game Gear colour RAM */
/* The LCD window of the Game Gear: 160 x 144 pixels, 6 cells from the
 * left and right of the 256-pixel picture and centred vertically. */
#define VDP_GG_W        160
#define VDP_GG_H        144
#define VDP_GG_X        48
#define VDP_SPRITES     64

/* Status flags. */
#define VDP_ST_VBLANK   0x80
#define VDP_ST_OVERFLOW 0x40
#define VDP_ST_COLLIDE  0x20

/* vdp_state.acc, the access word of the ports: the 14-bit address
 * register in bits 18-31 (so that its increment wraps at the end of
 * video RAM by itself), the access code of the last control word in bits
 * 14-15 (0 VRAM read, 1 VRAM write, 2 register write, 3 CRAM write),
 * VDP_PENDING while the second byte of a control word is expected,
 * VDP_ACC_SLOW whenever a data port write cannot take the plain video RAM
 * path (code 3, or a control word pending), and in bits 0-13 the data port
 * writes since the caller last cleared them (vdp_clear_data_writes, once
 * per frame: the field holds 16 383, more than a frame of Z80 time can
 * write). */
#define VDP_ACC_ADDR_SHIFT  18
#define VDP_ACC_CODE_SHIFT  14
#define VDP_PENDING         0x10000
#define VDP_ACC_SLOW        0x20000
#define VDP_ACC_DW_MASK     0x3FFF
#define vdp_addr(v)             ((v)->acc >> VDP_ACC_ADDR_SHIFT)
#define vdp_code(v)             (((v)->acc >> VDP_ACC_CODE_SHIFT) & 3)
#define vdp_data_writes(v)      ((v)->acc & VDP_ACC_DW_MASK)
#define vdp_clear_data_writes(v) ((v)->acc &= ~(uint32)VDP_ACC_DW_MASK)

/*
 * The port state comes first: the data and control ports also have
 * handlers in assembly (sms_io.s), which address it. CONTRACT with the
 * equates of sms_io.s - vdp.c checks the offsets at compile time.
 */
typedef struct {
    uint32  acc;            /* +0x00: the access word (see VDP_ACC_*) */
    uint8   gg_latch;       /* +0x04: Game Gear: the even byte of a colour */
    uint8   pad1[3];
    uint32  buffer;         /* +0x08: read buffer */
    uint32  cram_dirty;     /* +0x0C: colour RAM written since last cleared */
    uint32  n_data_r;       /* +0x10: data port reads */
    uint32  n_ctrl_w;       /* +0x14: control port writes */
    uint32  n_stat_r;       /* +0x18: status reads handled in assembly */
    uint32  gg;             /* +0x1C: Game Gear mode (set by the machine) */
    uint8   cram[32];       /* +0x20 */
    uint8   reg[16];        /* +0x40 */
    uint32  status;         /* +0x50: VDP_ST_* */
    uint32  line_flag;      /* +0x54: line interrupt pending */
    /* Timing (the fields the event handler of sms_io.s loads together). */
    uint32  active;         /* +0x58: active lines: 192, 224 or 240 */
    uint32  lines;          /* lines per frame */
    uint32  vc_jump;        /* last V counter value before the jump back */
    uint32  vc_back;        /* amount the V counter jumps back by */
    uint32  frame_base;     /* ctx->line at the start of the current frame */
    uint32  vbl_done;       /* the VBlank event of this frame has run */
    uint32  lc_k;           /* line counter valid at the end of line lc_k - 1 */
    uint32  lc_val;
    uint32  frames;
    uint8   hc[VDP_LINE_T]; /* +0x7C: H counter for each T-state of a line */
    z80j_ctx *ctx;          /* +0x160 */
    uint32  nt_rows;        /* +0x164: name table rows: 28, or 32 in the extended modes */
    uint32  n_count_r;      /* +0x168: V and H counter reads (handled in assembly) */
    uint8   gg_cram[VDP_GG_CRAM];   /* +0x16C: Game Gear colour RAM */
    uint8   pad[0x200 - 0x7C - VDP_LINE_T - 12 - VDP_GG_CRAM];
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
    uint32  pad3;
    /* Sprite evaluation of the frame (vdp_sprites). */
    uint32  spr_n;          /* sprites before the terminator */
    uint32  spr_h;          /* lines per sprite: 8, 16 or 32 */
    uint32  spr_zoom;       /* register 1 bit 0 */
    uint32  spr_partial;    /* some sprite is hidden on some of its lines */
    int32   spr_y[VDP_SPRITES];     /* screen line of the first row */
    uint32  spr_vis[VDP_SPRITES];   /* bit k: line spr_y + k displayed, lines
                                       off the screen clear (only written
                                       when some line overflows) */
    uint32  spr_rows[VDP_SPRITES];  /* bit r: tile row r holds an opaque pixel */
    uint8   spr_tile_rows[VDP_TILES];   /* the same per tile, cached */
    uint8   spr_tile_ok[VDP_TILES];     /* the cache entry is valid */
    /* Colour RAM writes during the active display of the current frame:
     * (first line affected << 16) | (index << 8) | value; in Game Gear
     * mode (first line affected << 20) | (colour << 12) | 12-bit value. */
    uint32  cr_n;                   /* +0x4F84 */
    uint32  cr_log[VDP_CR_LOG];     /* +0x4F88 */
    /* The sprites on the screen with an opaque pixel, by first row, for
     * the code drawing the priority layer under them: sprite | x << 8 |
     * first screen line << 16 | last screen line + 1 << 24. */
    uint32  spr_order[VDP_SPRITES]; /* +0x5088 */
    uint32  spr_nord;               /* +0x5188 */
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
void   vdp_data_write(vdp_state *v, uint32 value, uint32 left);
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

/* Video RAM address of the name table selected by a register 2 value in
 * the current display mode. */
#define vdp_nt_base(v, reg2) \
    (((v)->nt_rows > 28) ? ((((uint32)(reg2) & 0x0C) << 10) | 0x0700) \
                         : (((uint32)(reg2) & 0x0E) << 10))

/* Palettes of the picture: band 0 starts at line 0 with the colour RAM
 * held at the end of the frame (written for the picture that goes with
 * the video RAM as it stands, as for the scroll), and each later band
 * applies the colour RAM writes recorded during the active display from
 * the line they affect. Bands whose colours do not change merge; writes
 * beyond VDP_PAL_BANDS bands go to the last one. tops[k] is the first
 * line of band k and the bytes of cram + vdp_pal_words(v) * k its 32
 * colours (cram holds VDP_PAL_BANDS * VDP_PAL_WORDS words): the colour
 * RAM's 32 bytes, or in Game Gear mode its 64. Returns the band count;
 * *merged counts the writes moved to an earlier line. */
#define vdp_pal_words(v)    ((v)->gg ? 16u : 8u)

/* The part of the picture that is shown: the whole 256 x active picture,
 * or in Game Gear mode the LCD window, centred vertically (lines 24 to
 * 167 of the 192-line mode). */
#define vdp_win_x(v)        ((v)->gg ? (uint32)VDP_GG_X : 0u)
#define vdp_win_y(v)        ((v)->gg ? ((v)->active - VDP_GG_H) / 2 : 0u)
#define vdp_win_w(v)        ((v)->gg ? (uint32)VDP_GG_W : 256u)
#define vdp_win_h(v)        ((v)->gg ? (uint32)VDP_GG_H : (v)->active)
uint32 vdp_palette_bands(const vdp_state *v, uint32 *tops, uint32 *cram, uint32 *merged);

/* Evaluates the sprite attribute table for the picture of the frame
 * (spr_* fields): the per-line limit, the overflow and collision flags,
 * which are set in the status and returned, and the ordered list of the
 * sprites on the screen with an opaque pixel (spr_order). The opaque rows of the
 * tiles are cached; a written tile is seen through the dirty bytes, and
 * the caller clears spr_tile_ok for the tiles it clears them for. */
uint32 vdp_sprites(vdp_state *v);

#endif /* VDP_H */
