#ifndef RENDER_H_INCLUDED
#define RENDER_H_INCLUDED

/*
 * Picture of the SMS VDP with the CEL engine.
 *
 * Background: a whole name table (32 x 28 cells, 32 x 32 in the 224 and
 * 240-line modes) is kept as a 256 x 224 (256 x 256) bitmap in the 8 bpp
 * coded cel format (pixel = colour index 0-31, the two halves of the
 * colour RAM as one 32-entry PLUT). A cell is redrawn into it only when
 * its name table entry changes or when its tile is written, flips being
 * applied by the copy. The visible window (256 x 192, 224 or 240) is then
 * a few rectangular pieces of that bitmap, one cel each: the horizontal
 * and vertical scroll wraps, the scroll inhibit bits of register 0 and the
 * horizontal scroll changes recorded during the active display (bands)
 * only add pieces. Pieces are positioned in the clip window of the
 * bitmap they are drawn into, which the caller sets to the picture
 * (render_height() lines).
 *
 * Palettes: colour RAM writes recorded during the active display split
 * the picture into palette bands (vdp_palette_bands); every cel takes the
 * PLUTs of the band it is drawn on, and a sprite those of the band of its
 * first line. The backdrop (border, blanked column, lines with the
 * display off) keeps the colour of the first band.
 *
 * Layers: a game may switch the name table base (register 2) during the
 * active display, so that different parts of the picture come from
 * different tables (a static sky above a raster-scrolled road, a status
 * panel). Each table in use has its own layer (bitmap, priority bitmap,
 * shadow and cell lists), kept up to date every frame; the bands of the
 * picture are drawn from the layer of their table. A table unused for a
 * while loses its layer; a table met with no free layer takes the least
 * recently used one, which is rebuilt. The bitmaps of the two layers
 * share two memory blocks, which hold the taller bitmaps of a single
 * layer in the 224 and 240-line modes.
 *
 * Sprites: one 8 bpp coded cel per active sprite (8x8 or 8x16, doubled
 * by the cel engine when zoomed), colour 0 transparent, drawn after the
 * background in reverse order so that sprite 0 ends on top. The VDP's
 * sprite evaluation (vdp_sprites) says which lines of each sprite the
 * hardware shows: a sprite hidden on some lines by the eight per line
 * limit is drawn as one cel per run of shown lines (a zoomed sprite's
 * runs are rounded to whole doubled rows).
 *
 * Display enable: changes of register 1 bit 6 recorded during the active
 * display split the picture like the scroll bands: the background and
 * the strips are only drawn on the lines where the display is on, and
 * the other lines are covered, after the sprites, by rectangles of the
 * backdrop colour. A picture with no line on is not drawn at all
 * (render_display_on() is 0): the caller fills it with the backdrop.
 *
 * Priority tiles: a second bitmap per layer holds the pixels of the
 * cells whose entry has the priority bit (zero elsewhere). After the
 * sprites, the rows of cells that hold priority cells are drawn from it
 * as strips with a PLUT whose colour 0 of both palettes is 000,
 * transparent: their other pixels cover the sprites as on the hardware.
 * Rows without priority cells cost nothing. When the parts of the strips
 * under the sprites cover fewer pixels than the strips themselves, those
 * parts are drawn instead (patches): a picture made of priority tiles
 * costs the sprites' area instead of a full-screen cel, and without a
 * sprite nothing is drawn at all.
 *
 * Tiles are converted from the VDP's planar format into 8 bpp rows of 8
 * bytes (two words) the first time a cell or a sprite needs them after
 * they were written (a cell needing the conversion is drawn from its rows
 * on the way); the horizontally flipped copy has its own store. The tile
 * stores are shared by the layers.
 *
 * The caller: render_update() after each emulated frame, then, if the
 * display is on, DrawCels(bitmap, render_chain()) into a bitmap whose clip
 * window is the 256 x render_height() picture; the border around the
 * picture and the whole picture while the display is off take the
 * backdrop colour (render_backdrop()).
 *
 * Catch-up in spare time: while the display is off the update draws
 * nothing and the work waits (a game rebuilding a screen writes most of
 * its name table and many tiles then), so the first frame with the
 * display on used to pay the whole catch-up at once. render_catch_up(),
 * called by the caller in the spare time of a frame with the display off,
 * does that work ahead up to a deadline: the written tiles are converted
 * and the changed cells and the cells of the written tiles redrawn, chunk
 * by chunk and cell by cell, and a table with no layer yet gets one
 * rebuilt in steps. What is not done keeps its dirty byte or its cursor
 * and is finished by the next call or by the update of the first frame
 * with the display on. The sprites, the bands, the pieces and the
 * palettes belong to the frame that shows the picture and are not
 * touched.
 */

#include "types.h"
#include "graphics.h"
#include "vdp.h"

#define RENDER_W        256
#define RENDER_H        192         /* picture height of the usual mode */
#define RENDER_H_MAX    VDP_ACTIVE_MAX
#define RENDER_NT_COLS  32
#define RENDER_NT_ROWS  28          /* name table rows of the usual mode */
#define RENDER_NT_ROWS_MAX 32       /* of the 224 and 240-line modes */
#define RENDER_NT_CELLS (RENDER_NT_COLS * RENDER_NT_ROWS)
#define RENDER_NT_CELLS_MAX (RENDER_NT_COLS * RENDER_NT_ROWS_MAX)
#define RENDER_BM_BYTES (RENDER_W * RENDER_NT_ROWS * 8)  /* a bitmap, usual mode */
#define RENDER_LAYERS   2           /* name tables kept as bitmaps */
#define RENDER_MAX_PIECES 256       /* background pieces (a band per line, wraps) */
#define RENDER_MAX_PRIO   128       /* priority strips */
#define RENDER_MAX_DBLANK (VDP_DE_LOG + 1)  /* blanked line ranges */
#define RENDER_SPRITES  64
#define RENDER_SPRITE_CELS 96       /* cels for the sprites, runs included */
#define RENDER_LAYER_KEEP 16        /* frames a layer outlives its last use */
#define RENDER_PLUT_SET 96          /* PLUT entries of a palette band: background,
                                       sprites, priority (32 each) */

/* Cumulative counters, logged as differences by the caller. */
typedef struct {
    uint32 tiles;           /* tile conversions (either store) */
    uint32 cells;           /* cells redrawn into the bitmaps */
    uint32 rebuilds;        /* whole bitmap rebuilt (a layer took a new table) */
    uint32 tile_cells;      /* cells redrawn because their tile was written */
    uint32 palettes;        /* PLUT rebuilds */
    uint32 backdrops;       /* backdrop colour changes */
    uint32 sprites;         /* sprite cels set up */
    uint32 pieces;          /* background pieces */
    uint32 prio_strips;     /* priority strips */
    uint32 prio_patches;    /* priority patches under sprites */
    uint32 spr_partial;     /* frames with sprites hidden on some lines */
    uint32 spr_dropped;     /* sprite runs beyond the cel capacity */
    uint32 overflows;       /* frames setting the sprite overflow flag */
    uint32 collisions;      /* frames setting the sprite collision flag */
    uint32 bands;           /* frames with more than one scroll band */
    uint32 dbands;          /* frames with the display off on some lines */
    uint32 ntbands;         /* frames with more than one name table band */
    uint32 dropped;         /* bands beyond the piece capacity */
    uint32 layers;          /* frames needing more layers than there are */
    uint32 pbands;          /* frames with more than one palette band */
    uint32 pmerged;         /* colour writes beyond the palette bands */
    uint32 tall;            /* frames of more than 192 lines */
    uint32 ahead_cells;     /* cells drawn by the catch-up in spare time */
    uint32 ahead_tiles;     /* tiles it converted */
    uint32 ahead_calls;     /* catch-up calls that found work */
    uint32 ahead_cut;       /* of which stopped by their deadline */
    uint32 ahead_us;        /* time they took */
} render_stats;

/*
 * One name table as a bitmap. CONTRACT: the R_* equates of render_a.s
 * are the offsets of these fields (checked in render.c). The tile
 * stores, the work lists and the expansion tables are shared by the
 * layers: each layer holds the same pointers.
 */
typedef struct render_layer_s {
    uint32 *tiles;          /* VDP_TILES x 16 words (shared) */
    uint32 *tiles_f;        /* horizontally flipped copies (shared) */
    uint8  *t_ok;           /* tile converted (bit 0: tiles, bit 1: tiles_f) (shared) */
    uint8  *bitmap;         /* RENDER_W x 8 * nrows, 8 bpp */
    uint8  *prio_bm;        /* same, priority cells only */
    uint32 *xtab;           /* expansion tables (2 x 256 words) (shared) */
    uint8  *shadow;         /* name table bytes as drawn (64 per row) */
    /* Per cell: the update in which it was last drawn (stamps are even,
     * 0 to RENDER_STAMP_LAST), RENDER_STALE when it waits for a redraw
     * (see stale_rows), or RENDER_NEUTRAL, which no stamp equals. */
    uint8  *stamp;
    uint32  stamp_now;
    /* Cells using each tile, as lists (RENDER_NT_CELLS_MAX ends a list),
     * so that a written tile only redraws its cells. Each cell has a node
     * of two words: the next cell of its list, and the address of the
     * word holding the cell (the head of its tile, or the node of the
     * cell before it), so that a cell leaves its list without looking at
     * its tile. The VDP_TILES heads lie right before the nodes. */
    uint32 *node;           /* cap_cells x 2 */
    uint32 *head;           /* VDP_TILES */
    uint32 *wlist;          /* work list: changed words, tiles to convert (shared) */
    uint32 *dlist;          /* dirty tiles of the update (shared) */
    uint32  nt_base;        /* VRAM address of the name table drawn */
    uint32  valid;          /* shadow and lists describe the bitmap */
    uint32  last_used;      /* update in which the layer was last drawn */
    uint8   prio_rows[RENDER_NT_ROWS_MAX];  /* priority cells per row */
    uint8   prio_min[RENDER_NT_ROWS_MAX];   /* their column range */
    uint8   prio_max[RENDER_NT_ROWS_MAX];
    uint8   prio_dirty[RENDER_NT_ROWS_MAX]; /* range to recompute */
    uint32  cells;          /* counters of the update, added to the stats */
    uint32  tile_cells;
    uint32  tiles_conv;
    /* Priority strips of the table, built once per update: runs of
     * consecutive rows holding priority cells in the same columns. */
    uint32  pg_n;
    uint8   pg_row0[RENDER_NT_ROWS_MAX];
    uint8   pg_nrows[RENDER_NT_ROWS_MAX];
    uint8   pg_col0[RENDER_NT_ROWS_MAX];    /* first column */
    uint8   pg_col1[RENDER_NT_ROWS_MAX];    /* last column + 1 */
    /* Cells of a written tile are only redrawn in the rows the picture
     * shows (shown_rows, bit per name table row, from the bands of the
     * update); the others are left stale (their stamp RENDER_STALE) and
     * redrawn when their row is shown again. */
    uint32  stale_rows;     /* rows holding stale cells */
    uint32  shown_rows;     /* rows shown by the update */
    /* Size of the table drawn: 28 or 32 rows. */
    uint32  nrows;
    uint32  ncells;         /* nrows * 32 */
    uint32  cap_cells;      /* cells the lists and the shadow can hold */
    /* Catch-up in spare time (render_catch_up): the work it took from
     * the VDP's dirty bytes and has not finished: the name table chunks
     * whose entries are to be compared (bit per chunk), the tiles whose
     * cells are to be redrawn (bit per tile), the tile whose cell list
     * is being walked with the cell to continue from (RENDER_NO_TILE
     * when none), and the next cell of a rebuild begun ahead
     * (RENDER_NO_TILE when no rebuild is in progress; a layer with one
     * holds its table although it is not valid yet). */
    uint32  nt_pend[2];
    uint32  tile_pend[VDP_TILES / 32];
    uint32  pend_tile;
    uint32  pend_cell;
    uint32  rb_next;
} render_layer;

#define RENDER_NO_TILE  0xFFFFFFFFu
#define RENDER_STALE    0xFF        /* stamp of a cell waiting for a redraw */
#define RENDER_NEUTRAL  0xFE        /* stamp of a cell drawn in no recent update */
#define RENDER_STAMP_LAST 0xFC      /* the last stamp before the wrap */

typedef struct {
    render_layer layer[RENDER_LAYERS];
    uint8  *bm_pool[2];     /* bitmaps, priority bitmaps: two RENDER_BM_BYTES each */
    uint32  nrows;          /* name table rows of the current mode: 28 or 32 */
    uint32  bm_h;           /* bitmap lines: nrows * 8 */
    uint32  height;         /* picture lines: 192, 224 or 240 */
    uint32  nlayers;        /* layers usable in the current mode: 2, or 1 */
    uint32 *tiles;          /* the shared stores and tables (see render_layer) */
    uint32 *tiles_f;
    uint8  *t_ok;
    uint32 *wlist;
    uint32 *dlist;
    uint32 *xtab;
    CCB    *cels;           /* pieces, strips, blank rectangles, sprites, terminator */
    uint16 *pluts;          /* per palette band: background (32), sprites (32),
                               priority (32) */
    uint32 *blank;          /* 8 x 1 uncoded 16 bpp source of the blank column */
    uint32  update;         /* updates so far (layer ages) */
    uint32  backdrop;       /* RGB 5:5:5 */
    uint32  display_on;
    uint32  n_sprites;      /* sprite cels in the chain */
    uint32  spr_mode;       /* the sprite cels are set up for: bit 0 tall, bit 1 zoom */
    uint32  spr_runs;       /* the last update made cels per run of lines */
    CCB    *chain;          /* first cel to draw */
    CCB    *last_cel;       /* the cel carrying CCB_LAST */
    /* Band tables of the update, kept off the task stack. */
    uint32  tops[VDP_HS_LOG + 1];
    uint32  hss[VDP_HS_LOG + 1];
    uint32  ntops[VDP_NT_LOG + 1];
    uint32  nvals[VDP_NT_LOG + 1];
    render_layer *nlayer[VDP_NT_LOG + 1];   /* layer of each name table band */
    uint32  dtops[VDP_DE_LOG + 1];
    uint32  dons[VDP_DE_LOG + 1];
    /* Palette bands of the update (vdp_palette_bands). */
    uint32  ptops[VDP_PAL_BANDS];
    uint32  pcram[VDP_PAL_BANDS * 8];   /* 32 colour bytes per band */
    uint32  npb;
    uint32  spr_bands;      /* the sprite cels take the PLUTs of their band */
    /* Line bands of the update: the scroll bands crossed with the name
     * table bands, the palette bands and the lines with the display on,
     * as ya | yb << 8 | hs << 16 | layer << 24 | palette band << 25, with
     * room for the end mark render_band_pieces reads them up to. */
    uint32  lbands[VDP_HS_LOG + VDP_NT_LOG + VDP_DE_LOG + VDP_PAL_BANDS + 4];
    uint32  nlb;
    render_stats st;
} renderer;

Err   render_init(renderer *r);
void  render_free(renderer *r);

/* Forgets the picture: everything is rebuilt at the next update. */
void  render_reset(renderer *r);

/* Brings the bitmaps, the pieces and the sprite cels up to date with the
 * VDP; clears the VDP's dirty flags. */
void  render_update(renderer *r, vdp_state *v);

/* Spare time of a frame with the display off: converts the written tiles
 * and redraws the changed cells and the cells of the written tiles into
 * the bitmaps, and rebuilds in steps a layer for the name table the VDP
 * points at when none holds it, until the microsecond clock now() reaches
 * deadline_us (looked at every few cells); what is left waits for the
 * next call or the next update. Returns nonzero when work is left. */
uint32 render_catch_up(renderer *r, vdp_state *v, uint32 (*now)(void), uint32 deadline_us);

/* Assembly helpers (render_a.s). CONTRACT: render_sprite_args matches
 * the SA_* equates of render_a.s. */
typedef struct {
    uint32 *tiles;
    uint8  *t_ok;
    uint32  tbase;          /* 0 or 256 */
    uint32  tmask;          /* 0xFF, or 0xFE for tall sprites */
    uint32  xoff;           /* 0 or 8 */
    uint32 *need;           /* tiles whose conversion is missing */
    uint32  tall;           /* 0 or 1 */
    uint32  ywrap;          /* 256 - lines per sprite: a Y past it wraps to the top */
    CCB    *end;            /* render_sprite_runs: cel capacity */
    uint32  dropped;        /* runs beyond it */
    uint32  needed;         /* tiles appended to need */
    uint32  height;         /* picture lines: sprites off them get no cel */
} render_sprite_args;

/* Indices of the words of the dirty name table chunks (32 bytes each,
 * chunks of them, 64 at most) that differ from the shadow; returns their
 * count. */
uint32 render_nt_scan(const uint32 *nt, const uint32 *shadow, const uint32 *mask,
                      uint32 *out, uint32 chunks);
/* Indices of the dirty tiles, which are cleared; returns their count. */
uint32 render_dirty_scan(uint8 *dirty, uint32 *out);
/* Source and position of one cel per sprite among sprites 0 to n - 1
 * that reaches the screen, in table order; returns the cel after the last
 * one, the tiles to convert in a->need (a->needed of them). */
CCB   *render_sprites(const uint8 *sat, uint32 n, CCB *cel, render_sprite_args *a);
/* The same from the VDP's evaluation when sprites are hidden on some
 * lines: one cel per run of shown lines; returns the cel after the last
 * one, the tiles to convert in a->need (a->needed of them). */
CCB   *render_sprite_runs(const uint8 *sat, const vdp_state *v, CCB *cel,
                          render_sprite_args *a);
/* The parts of the priority strips under the sprites, as cels of their
 * own (render_a.s). CONTRACT: matches the PA_* equates. Returns 0 when
 * the pixels exceed limit or the cels run out. */
typedef struct {
    const CCB *strips;      /* the strips of the frame: screen rectangles */
    uint32  nstrips;
    uint32  xoff;
    const vdp_state *v;
    const uint8 *sat;
    uint32  limit;          /* pixels beyond which the patches are given up */
    uint32  area;           /* out: pixels of the patches */
    struct piece_ctx_s *p;
    uint32  height;         /* picture lines */
} render_patch_args;
uint32 render_prio_patches(render_patch_args *a);

/* Priority strip groups of a layer (pg_* fields): runs of consecutive
 * rows holding priority cells, with the union of their column ranges;
 * the ranges of the rows marked dirty are found again from the shadow. */
void   render_prio_groups(render_layer *l);
/* Cells of a layer (render_a.s). Each draws cells into the bitmap (and
 * the priority bitmap for entries with the priority bit), converting the
 * tiles found unconverted through render_tile_conv, stamping the cells
 * drawn and counting them.
 * render_entries: the n name table words listed in l->wlist (which has
 * room for an end mark after them), copied to the shadow; each entry
 * that differs is re-linked in the cell list of its tile, its priority
 * bookkeeping done, and drawn. render_tile_cells:
 * the cells of tile t not stamped in this update, those of the rows
 * not shown left stale. render_stale_cells: the stale cells of the rows
 * shown. render_rebuild_cells: every cell from the shadow, linked into
 * lists that must be empty. */
void   render_entries(render_layer *l, const uint8 *vram, uint32 n);
/* ORs RENDER_NEUTRAL into every byte of nwords words of stamps (a multiple
 * of 8): the wrap of the update stamps (render_a.s). */
void   render_stamp_wrap(uint8 *stamp, uint32 nwords);
void   render_tile_cells(render_layer *l, const uint8 *vram, uint32 t);
void   render_stale_cells(render_layer *l, const uint8 *vram);
void   render_rebuild_cells(render_layer *l, const uint8 *vram);
/* Resumable forms for the catch-up: render_tile_cells for the list of
 * tile t from cell from (RENDER_NT_CELLS_MAX: the head of the list), at
 * most budget cells drawn, returning the cell to continue from
 * (RENDER_NT_CELLS_MAX once the list is done); and the rebuild of cells
 * from to from + budget - 1, returning the next cell (ncells at the end). */
uint32 render_tile_cells_part(render_layer *l, const uint8 *vram, uint32 t, uint32 from,
                              uint32 budget);
uint32 render_rebuild_cells_part(render_layer *l, const uint8 *vram, uint32 from,
                                 uint32 budget);
/* Pieces of a bitmap rectangle on a line band that the scroll inhibit
 * bits do not split; see render.c, rect_pieces. CONTRACT: the PC_*
 * equates of render_a.s. */
typedef struct piece_ctx_s {
    CCB    *c;              /* next cel */
    CCB    *end;            /* capacity */
    uint32  bm_h;           /* bitmap lines: the vertical wrap */
    uint16 *plut;           /* PLUT of the cels made */
} piece_ctx;
uint32 render_rect_pieces(piece_ctx *p, const uint8 *bm, uint32 bx0, uint32 bx1,
                          uint32 by0, uint32 by1, uint32 lb, uint32 vs);
/* The background pieces of line bands that no scroll inhibit window
 * splits, read from lb up to a word of all ones, in one pass: per band
 * what render_rect_pieces makes of the whole bitmap of its layer, with
 * the PLUT of its palette band; each cel is chained to the cel after it
 * (render_a.s). CONTRACT: matches the BA_* equates. Returns the number of
 * pieces dropped for lack of cels. */
typedef struct {
    CCB    *c;              /* next cel (updated) */
    CCB    *end;            /* capacity */
    uint32  bm_h;           /* bitmap lines: the vertical wrap */
    uint32  vs;             /* vertical scroll */
    const uint8 *bm[RENDER_LAYERS];     /* bitmap of each layer */
    uint16 *pluts;          /* PLUT sets of the palette bands */
} render_band_args;
uint32 render_band_pieces(render_band_args *a, const uint32 *lb);
/* The line bands of the usual frame (the display on, one name table, one
 * palette band) straight from the log of the horizontal scroll writes, as
 * vdp_bands and line_bands make them; hl = picture height | layer << 24
 * (render_a.s). Returns the words written to out | the bands << 16. */
uint32 render_scroll_lbands(const uint32 *log, uint32 n, uint32 top, uint32 hl, uint32 *out);
/* Converts tile t (flip: mirrored horizontally) from the VDP's four bit
 * planes to 8 bpp rows into the layer's stores (render_a.s). */
void   render_tile_conv(render_layer *l, uint32 t, uint32 flip, const uint8 *vram);

#define render_chain(r)     ((r)->chain)
#define render_backdrop(r)  ((r)->backdrop)
#define render_display_on(r) ((r)->display_on)
#define render_height(r)    ((r)->height)

#endif /* RENDER_H_INCLUDED */
