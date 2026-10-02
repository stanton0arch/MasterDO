#ifndef RENDER_H_INCLUDED
#define RENDER_H_INCLUDED

/*
 * Picture of the SMS VDP with the CEL engine.
 *
 * Background: a whole name table (32 x 28 cells) is kept as a 256 x 224
 * bitmap in the 8 bpp coded cel format (pixel = colour index 0-31, the two
 * halves of the colour RAM as one 32-entry PLUT). A cell is redrawn into
 * it only when its name table entry changes or when its tile is written,
 * flips being applied by the copy. The visible 256 x 192 window is then a
 * few rectangular pieces of that bitmap, one cel each: the horizontal and
 * vertical scroll wraps, the scroll inhibit bits of register 0 and the
 * horizontal scroll changes recorded during the active display (bands)
 * only add pieces. Pieces are positioned in the clip window of the
 * bitmap they are drawn into, which the caller sets to the picture.
 *
 * Layers: a game may switch the name table base (register 2) during the
 * active display, so that different parts of the picture come from
 * different tables (a static sky above a raster-scrolled road, a status
 * panel). Each table in use has its own layer (bitmap, priority bitmap,
 * shadow and cell lists), kept up to date every frame; the bands of the
 * picture are drawn from the layer of their table. A table unused for a
 * while loses its layer; a table met with no free layer takes the least
 * recently used one, which is rebuilt.
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
 * they were written; the horizontally flipped copy has its own store.
 * The tile stores are shared by the layers.
 *
 * The caller: render_update() after each emulated frame, then, if the
 * display is on, DrawCels(bitmap, render_chain()) into a bitmap whose clip
 * window is the 256 x 192 picture; the border around the picture and the
 * whole picture while the display is off take the backdrop colour
 * (render_backdrop()).
 */

#include "types.h"
#include "graphics.h"
#include "vdp.h"

#define RENDER_W        256
#define RENDER_H        192
#define RENDER_NT_COLS  32
#define RENDER_NT_ROWS  28
#define RENDER_NT_CELLS (RENDER_NT_COLS * RENDER_NT_ROWS)
#define RENDER_NT_BYTES 0x700
#define RENDER_BM_H     (RENDER_NT_ROWS * 8)        /* 224 */
#define RENDER_LAYERS   2           /* name tables kept as bitmaps */
#define RENDER_MAX_PIECES 256       /* background pieces (a band per line, wraps) */
#define RENDER_MAX_PRIO   128       /* priority strips */
#define RENDER_MAX_DBLANK (VDP_DE_LOG + 1)  /* blanked line ranges */
#define RENDER_SPRITES  64
#define RENDER_SPRITE_CELS 96       /* cels for the sprites, runs included */
#define RENDER_LAYER_KEEP 16        /* frames a layer outlives its last use */

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
} render_stats;

/*
 * One name table as a bitmap. CONTRACT: the R_* equates of render_a.s
 * are the offsets of these fields (checked in render.c). The tile
 * stores, the work lists and the expansion tables are shared by the
 * layers: each layer holds the same pointers.
 */
typedef struct render_layer_s {
    uint8  *bitmap;         /* RENDER_W x RENDER_BM_H, 8 bpp */
    uint8  *prio_bm;        /* same, priority cells only */
    uint32 *tiles;          /* VDP_TILES x 16 words (shared) */
    uint32 *tiles_f;        /* horizontally flipped copies (shared) */
    uint8  *shadow;         /* name table bytes as drawn (RENDER_NT_BYTES) */
    uint8  *t_ok;           /* tile converted (bit 0: tiles, bit 1: tiles_f) (shared) */
    /* Cells using each tile, as doubly linked lists (RENDER_NT_CELLS
     * means none), so that a written tile only redraws its cells. */
    uint32 *head;           /* VDP_TILES */
    uint32 *next;           /* RENDER_NT_CELLS */
    uint32 *prev;
    uint32 *wlist;          /* work list: changed words, tiles to convert (shared) */
    uint32 *dlist;          /* dirty tiles of the update (shared) */
    uint8  *stamp;          /* update in which each cell was last drawn */
    uint32  stamp_now;
    uint32 *xtab;           /* expansion tables (4 x 256 words) (shared) */
    uint32  nt_base;        /* VRAM address of the name table drawn */
    uint32  valid;          /* shadow and lists describe the bitmap */
    uint32  last_used;      /* update in which the layer was last drawn */
    uint8   prio_rows[RENDER_NT_ROWS];  /* priority cells per row */
    uint8   prio_min[RENDER_NT_ROWS];   /* their column range */
    uint8   prio_max[RENDER_NT_ROWS];
    uint8   prio_dirty[RENDER_NT_ROWS]; /* range to recompute */
    uint32  cells;          /* counters of the update, added to the stats */
    uint32  tile_cells;
    uint32  tiles_conv;
    /* Priority strips of the table, built once per update: runs of
     * consecutive rows holding priority cells in the same columns. */
    uint32  pg_n;
    uint8   pg_row0[RENDER_NT_ROWS];
    uint8   pg_nrows[RENDER_NT_ROWS];
    uint8   pg_col0[RENDER_NT_ROWS];    /* first column */
    uint8   pg_col1[RENDER_NT_ROWS];    /* last column + 1 */
    /* Cells of a written tile are only redrawn in the rows the picture
     * shows (shown_rows, bit per name table row, from the bands of the
     * update); the others are left stale and redrawn when their row is
     * shown again. */
    uint8  *stale;          /* RENDER_NT_CELLS: cell waiting for a redraw */
    uint32  stale_rows;     /* rows holding stale cells */
    uint32  shown_rows;     /* rows shown by the update */
} render_layer;

typedef struct {
    render_layer layer[RENDER_LAYERS];
    uint32 *tiles;          /* the shared stores and tables (see render_layer) */
    uint32 *tiles_f;
    uint8  *t_ok;
    uint32 *wlist;
    uint32 *dlist;
    uint32 *xtab;
    CCB    *cels;           /* pieces, strips, blank rectangles, sprites, terminator */
    uint16 *pluts;          /* background (32), sprites (32), priority (32) */
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
    /* Line bands of the update: the scroll bands crossed with the name
     * table bands and the lines with the display on, as
     * ya | yb << 8 | hs << 16 | layer << 24. */
    uint32  lbands[VDP_HS_LOG + VDP_NT_LOG + VDP_DE_LOG + 4];
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
} render_sprite_args;

/* Indices of the words of the dirty name table chunks that differ from
 * the shadow; returns their count. */
uint32 render_nt_scan(const uint32 *nt, const uint32 *shadow, const uint32 *mask,
                      uint32 *out);
/* Indices of the dirty tiles, which are cleared; returns their count. */
uint32 render_dirty_scan(uint8 *dirty, uint32 *out);
/* Source and position of the cels of sprites 0 to n - 1; returns the
 * number of tiles appended to a->need. */
uint32 render_sprites(const uint8 *sat, uint32 n, CCB *cel, const render_sprite_args *a);
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
} render_patch_args;
uint32 render_prio_patches(render_patch_args *a);
/* Cells of a layer (render_a.s). Each draws cells into the bitmap (and
 * the priority bitmap for entries with the priority bit), converting the
 * tiles found unconverted through render_tile_conv, stamping the cells
 * drawn and counting them.
 * render_entries: the n name table words listed in l->wlist, copied to
 * the shadow; each entry that differs is re-linked in the cell list of
 * its tile, its priority bookkeeping done, and drawn. render_tile_cells:
 * the cells of tile t not stamped in this update, those of the rows
 * not shown left stale. render_stale_cells: the stale cells of the rows
 * shown. render_rebuild_cells: every cell from the shadow, linked into
 * lists that must be empty. */
void   render_entries(render_layer *l, const uint8 *vram, uint32 n);
void   render_tile_cells(render_layer *l, const uint8 *vram, uint32 t);
void   render_stale_cells(render_layer *l, const uint8 *vram);
void   render_rebuild_cells(render_layer *l, const uint8 *vram);
/* Pieces of a bitmap rectangle on a line band in the usual case (no
 * scroll inhibit); see render.c, rect_pieces. */
typedef struct piece_ctx_s {
    CCB    *c;              /* next cel */
    CCB    *end;            /* capacity */
} piece_ctx;
uint32 render_rect_pieces(piece_ctx *p, const uint8 *bm, uint32 bx0, uint32 bx1,
                          uint32 by0, uint32 by1, uint32 lb, uint32 vs);
/* Converts tile t (flip: mirrored horizontally) from the VDP's four bit
 * planes to 8 bpp rows into the layer's stores (render_a.s). */
void   render_tile_conv(render_layer *l, uint32 t, uint32 flip, const uint8 *vram);

#define render_chain(r)     ((r)->chain)
#define render_backdrop(r)  ((r)->backdrop)
#define render_display_on(r) ((r)->display_on)

#endif /* RENDER_H_INCLUDED */
