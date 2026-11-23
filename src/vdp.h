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
 * flag. The VBlank flag is set one line after the last decrement of the
 * line counter, at the end of the second line after the active display
 * (193, 225 or 241): a line interrupt raised by that decrement (register
 * 10 at $C0 in the 192-line mode, or a line interrupt on every line) is
 * taken with the VBlank flag still clear, and a VBlank handler's status
 * read leaves no line interrupt pending after it, since the counter no
 * longer counts down.
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
 *
 * The hardware reads the sprite attribute table again for every line, so
 * a game may rewrite it during the active display, ahead of the beam, to
 * show more sprites than the table holds (a slot reused lower down the
 * screen, typically by a line interrupt handler). Those writes are
 * recorded with their time (sat_log): a control word setting an address
 * inside the table during the active display marks the access word slow
 * (one compare with the key kept in the low byte of the count of control
 * writes, which the control port's paths load and store anyway: the
 * table's address bits 8-13, followed by every write of register 5, with
 * VDP_NCTRL_LOG with record memory, VDP_NCTRL_OFF set outside the
 * active display), and the data port writes that follow are recorded
 * until the address leaves the table or the writes no longer reach a line
 * shown (the last line of the picture, or of the LCD window in Game Gear
 * mode); OUTI and buffered runs inside the table are recorded in one
 * pass, and a run outside the table or past those lines ends the watch at
 * once. vdp_sat_view() then gives the renderer the table the lines see:
 * the table in video RAM when every recorded write either holds the value
 * the frame leaves in its byte (a table uploaded during the display, the
 * usual case) or leaves its byte as it was (a table written again with
 * the values it holds), else every content a slot held during the frame,
 * as long as it reached the lines that saw it, in slot order.
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
#define VDP_SAT_LOG     512         /* sprite table writes recorded per frame */
/* First line a recorded sprite table write affects (the one after the line
 * of its time, t / 4 in bits 16-29 of the entry). */
#define VDP_SAT_LINE(e) ((((((e) >> 16) & 0x3FFF) * 36793u) >> 21) + 1)
/* In an entry: the write left its byte as it was. */
#define VDP_SAT_SAME    0x80000000u
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
 * path (code 3, a control word pending, or an address in the sprite
 * attribute table during the active display), and in bits 0-13 the data port
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

/* vdp_state.n_ctrl_w: the latest control writes in bits 8-31 (the earlier
 * ones in ctrl_base, moved there by vdp_fold_ctrl_writes at least every
 * million writes), the sprite table watch's key in bits 0-7: the table's
 * address bits 8-13 (VDP_NCTRL_TABLE), VDP_NCTRL_LOG when the machine gave
 * record memory, VDP_NCTRL_OFF outside the active display. An address is
 * watched when VDP_NCTRL_LOG | its bits 8-13 equal the whole key: then the
 * key is the second byte of a control word setting a video RAM write
 * address in the table. */
#define VDP_NCTRL_ONE       0x100
#define VDP_NCTRL_KEY       0xFF
#define VDP_NCTRL_TABLE     0x3F
#define VDP_NCTRL_LOG       0x40
#define VDP_NCTRL_OFF       0x80
#define vdp_ctrl_writes(v)  ((v)->ctrl_base + ((v)->n_ctrl_w >> 8))
#define vdp_fold_ctrl_writes(v)                                             \
    do {                                                                    \
        (v)->ctrl_base += (v)->n_ctrl_w >> 8;                               \
        (v)->n_ctrl_w &= VDP_NCTRL_KEY;                                     \
    } while (0)

/*
 * The port state comes first: the data and control ports also have
 * handlers in assembly (sms_io.s), which address it. CONTRACT with the
 * equates of sms_io.s and vdp_a.s - vdp.c checks the offsets at compile
 * time.
 *
 * The counts kept by the assembly handlers for the logs each sit next to
 * a word the handler's path loads or stores anyway, so that one LDM or
 * STM carries both and a count costs an addition: the control writes
 * after the access word, the data reads after the read buffer, the
 * status reads after the status, the counter reads before the line count.
 */
typedef struct {
    uint32  acc;            /* +0x00: the access word (see VDP_ACC_*) */
    uint32  n_ctrl_w;       /* +0x04: control port writes of the frame in bits 8-31
                               (VDP_NCTRL_ONE each), and in bits 0-7 the sprite
                               table watch's key (VDP_NCTRL_KEY, see below) */
    uint32  buffer;         /* +0x08: read buffer */
    uint32  n_data_r;       /* +0x0C: data port reads */
    uint32  cram_dirty;     /* +0x10: colour RAM written since last cleared */
    uint32  line_flag;      /* +0x14: line interrupt pending */
    uint32  active;         /* +0x18: active lines: 192, 224 or 240 */
    uint32  gg;             /* +0x1C: Game Gear mode (set by the machine) */
    uint8   cram[32];       /* +0x20 */
    uint8   reg[16];        /* +0x40 */
    uint32  status;         /* +0x50: VDP_ST_* */
    uint32  n_stat_r;       /* +0x54: status reads handled in assembly */
    uint32  n_count_r;      /* +0x58: V and H counter reads (handled in assembly) */
    /* Timing. */
    uint32  lines;          /* +0x5C: lines per frame */
    uint32  vc_jump;        /* last V counter value before the jump back */
    uint32  vc_back;        /* amount the V counter jumps back by */
    /* The event handler of sms_io.s loads the next four together. */
    uint32  frame_base;     /* +0x68: ctx->line at the start of the current frame */
    uint32  vbl_done;       /* the VBlank event of this frame has run */
    uint32  lc_k;           /* line counter valid at the end of line lc_k - 1 */
    uint32  lc_val;
    uint32  frames;
    uint8   hc[VDP_LINE_T]; /* +0x7C: H counter for each T-state of a line */
    z80j_ctx *ctx;          /* +0x160 */
    uint32  nt_rows;        /* +0x164: name table rows: 28, or 32 in the extended modes */
    uint8   gg_latch;       /* +0x168: Game Gear: the even byte of a colour */
    uint8   pad1[3];
    uint8   gg_cram[VDP_GG_CRAM];   /* +0x16C: Game Gear colour RAM */
    /* Writes into the sprite attribute table during the active display of
     * the current frame: (t / 4) << 16 | (offset in the table << 8) |
     * value, t the T-state of the write in the frame (VDP_SAT_LINE gives
     * the first line it affects), VDP_SAT_SAME when the write left its
     * byte as it was. sat_log is NULL when the machine gave it
     * no memory: no write is watched then. */
    uint32 *sat_log;                /* +0x1AC: VDP_SAT_LOG entries */
    uint32  sat_n;                  /* +0x1B0: entries of the frame */
    uint32  sat_lost;               /* +0x1B4: writes not recorded (log full), in all */
    uint8  *sat_view;               /* +0x1B8: room for the table the lines see
                                       (vdp_sat_view: 256 bytes, then its work) */
    const uint8 *spr_sat;           /* +0x1BC: the table vdp_sprites evaluates; NULL:
                                       the one register 5 points at */
    uint32  sat_tmax;               /* +0x1C0: t / 4 of the first write that reaches
                                       no line shown: 57 times the last line shown */
    uint32  ctrl_base;              /* +0x1C4: control writes before those of n_ctrl_w */
    uint8   pad[0x200 - 0x7C - VDP_LINE_T - 12 - VDP_GG_CRAM - 28];
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
    int32   spr_y[VDP_SPRITES];     /* screen line of the first row (only
                                       written for the sprites on the screen) */
    uint32  spr_vis[VDP_SPRITES];   /* bit k: line spr_y + k displayed, lines
                                       off the screen clear (only written
                                       when some line overflows) */
    uint8   spr_tile_rows[VDP_TILES];   /* bit r: row r of the tile holds an
                                           opaque pixel, cached */
    uint8   spr_tile_ok[VDP_TILES];     /* the cache entry is valid */
    /* Colour RAM writes during the active display of the current frame:
     * (first line affected << 16) | (index << 8) | value; in Game Gear
     * mode (first line affected << 20) | (colour << 12) | 12-bit value. */
    uint32  cr_n;                   /* +0x4E84 */
    uint32  cr_log[VDP_CR_LOG];     /* +0x4E88 */
    /* The sprites on the screen with an opaque pixel, by first row, for
     * the code drawing the priority layer under them: sprite | x << 8 |
     * first screen line << 16 | last screen line + 1 << 24. */
    uint32  spr_order[VDP_SPRITES]; /* +0x4F88 */
    uint32  spr_nord;               /* +0x5088 */
} vdp_state;

/* Power-on state; lines is VDP_LINES_NTSC or VDP_LINES_PAL. */
void   vdp_init(vdp_state *v, z80j_ctx *ctx, uint32 lines);
void   vdp_reset(vdp_state *v);

/* Between frames, after a saved state was written into the registers,
 * the access state, the line counter state and the video and colour
 * RAMs: the timing taken up from the frame boundary it was saved at
 * (line_rel: ctx->line less the start of the frame then; event_rel: the
 * next event line less that start, or 0xFFFFFFFF), the display mode
 * derived again, every tile and colour marked written for the renderer. */
void   vdp_restored(vdp_state *v, uint32 line_rel, uint32 event_rel);

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
 * (spr_sat when set, else the one register 5 points at; spr_* fields):
 * the per-line limit, the overflow and collision flags,
 * which are set in the status and returned, and the ordered list of the
 * sprites on the screen with an opaque pixel (spr_order). The opaque rows of the
 * tiles are cached; a written tile is seen through the dirty bytes, and
 * the caller clears spr_tile_ok for the tiles it clears them for. */
uint32 vdp_sprites(vdp_state *v);

/* Bytes of the room sat_view points at (the table, then the work of
 * vdp_sat_view), and the log's. */
#define VDP_SAT_VIEW_BYTES  (256 + 4 * 96 + 2 * 96)
#define VDP_SAT_LOG_BYTES   (VDP_SAT_LOG * 4)

/* Between frames, before vdp_sprites, when some write was recorded
 * (sat_n): sets spr_sat to the sprite attribute table the lines of the
 * frame saw (left NULL for the one in video RAM when every recorded write
 * holds the value the frame leaves in its byte, else the view built in
 * sat_view) and returns it. The view starts from the table as the frame
 * leaves it (as the picture does from the video RAM); the slots that a
 * recorded write not marked VDP_SAT_SAME sets to a value other than the
 * table's change content at the lines their writes (all of them) changed
 * it; every content is a
 * sprite of the view (in slot order, then in line order) when its lines
 * on the screen meet the lines that saw it, the others left out, at most
 * VDP_SPRITES. In the 192-line mode a $D0 terminator that the lines of
 * some sprite saw whole hides it. spr_sat is NULL until then (every
 * frame starts with it so). */
const uint8 *vdp_sat_view(vdp_state *v);

/* The sprite attribute table vdp_sprites evaluated. */
#define vdp_spr_table(v) \
    ((v)->spr_sat != NULL ? (v)->spr_sat : (v)->vram + (((uint32)(v)->reg[5] & 0x7E) << 7))

/* Gives the VDP the memory to record the sprite table writes of the active
 * display (VDP_SAT_LOG_BYTES at log, VDP_SAT_VIEW_BYTES at view), or none
 * (NULL): the table in video RAM is then the only one. */
void vdp_set_sat_log(vdp_state *v, uint32 *log, uint8 *view);

#endif /* VDP_H */
