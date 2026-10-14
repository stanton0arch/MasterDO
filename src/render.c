/*
 * Picture of the SMS VDP with the CEL engine. See render.h.
 *
 * Memory layout notes (big-endian ARM): a name table entry is two bytes,
 * low byte first; four consecutive bytes are compared as one word with the
 * shadow copy. A converted tile row is two words, pixel 0 in the top byte
 * of the first word, which is what an 8 bpp coded cel reads.
 */

#include "render.h"

#include "stdio.h"
#include "string.h"
#include "stddef.h"
#include "mem.h"

/* Offsets of the layer fields read by render_a.s (its R_* equates). */
#define CHECK_OFF(name, cond) typedef char check_##name[(cond) ? 1 : -1]
CHECK_OFF(bitmap, offsetof(render_layer, bitmap) == 0);
CHECK_OFF(prio_bm, offsetof(render_layer, prio_bm) == 4);
CHECK_OFF(tiles, offsetof(render_layer, tiles) == 8);
CHECK_OFF(tiles_f, offsetof(render_layer, tiles_f) == 12);
CHECK_OFF(shadow, offsetof(render_layer, shadow) == 16);
CHECK_OFF(t_ok, offsetof(render_layer, t_ok) == 20);
CHECK_OFF(head, offsetof(render_layer, head) == 24);
CHECK_OFF(next, offsetof(render_layer, next) == 28);
CHECK_OFF(prev, offsetof(render_layer, prev) == 32);
CHECK_OFF(wlist, offsetof(render_layer, wlist) == 36);
CHECK_OFF(stamp, offsetof(render_layer, stamp) == 44);
CHECK_OFF(stamp_now, offsetof(render_layer, stamp_now) == 48);
CHECK_OFF(nt_base, offsetof(render_layer, nt_base) == 56);
CHECK_OFF(prio_rows, offsetof(render_layer, prio_rows) == 68);
CHECK_OFF(prio_dirty, offsetof(render_layer, prio_dirty) == 164);
CHECK_OFF(st_cells, offsetof(render_layer, cells) == 196);
CHECK_OFF(st_tile_cells, offsetof(render_layer, tile_cells) == 200);
CHECK_OFF(ccb_plut, offsetof(CCB, ccb_PLUTPtr) == 12);
CHECK_OFF(ccb_pre0, offsetof(CCB, ccb_PRE0) == 52);
CHECK_OFF(ccb_pre1, offsetof(CCB, ccb_PRE1) == 56);
CHECK_OFF(st_tiles_conv, offsetof(render_layer, tiles_conv) == 204);
CHECK_OFF(xtab, offsetof(render_layer, xtab) == 52);
CHECK_OFF(stale, offsetof(render_layer, stale) == 340);
CHECK_OFF(stale_rows, offsetof(render_layer, stale_rows) == 344);
CHECK_OFF(shown_rows, offsetof(render_layer, shown_rows) == 348);
CHECK_OFF(ncells, offsetof(render_layer, ncells) == 356);
CHECK_OFF(pg_n, offsetof(render_layer, pg_n) == 208);
CHECK_OFF(pg_row0, offsetof(render_layer, pg_row0) == 212);
CHECK_OFF(pg_nrows, offsetof(render_layer, pg_nrows) == 244);
CHECK_OFF(pg_col0, offsetof(render_layer, pg_col0) == 276);
CHECK_OFF(pg_col1, offsetof(render_layer, pg_col1) == 308);
CHECK_OFF(sa_end, offsetof(render_sprite_args, end) == 32);
CHECK_OFF(sa_needed, offsetof(render_sprite_args, needed) == 40);
CHECK_OFF(pa_p, offsetof(render_patch_args, p) == 28);
CHECK_OFF(pa_height, offsetof(render_patch_args, height) == 32);
CHECK_OFF(pc_bm_h, offsetof(piece_ctx, bm_h) == 8);
CHECK_OFF(pc_plut, offsetof(piece_ctx, plut) == 12);
CHECK_OFF(two_layers, RENDER_LAYERS == 2);      /* render_update handles both by name */

#define BM_PITCH        RENDER_W            /* bytes per bitmap row */
#define BM_WORDS        (RENDER_W / 4)      /* words per bitmap row */
#define POOL_BYTES      (2 * RENDER_BM_BYTES)   /* a bitmap block: two usual bitmaps */
#define TILE_WORDS      16                  /* 8 rows x 2 words */
#define WLIST_WORDS     VDP_TILES           /* the largest work list */
#define NONE            RENDER_NT_CELLS_MAX /* end of a cell list */

/* Index of the lowest set bit of a nonzero word. */
static uint32 lowest_bit(uint32 w)
{
    uint32 b = 0;

    if ((w & 0xFFFF) == 0) { b += 16; w >>= 16; }
    if ((w & 0xFF) == 0) { b += 8; w >>= 8; }
    if ((w & 0xF) == 0) { b += 4; w >>= 4; }
    if ((w & 0x3) == 0) { b += 2; w >>= 2; }
    if ((w & 0x1) == 0) { b += 1; }
    return b;
}

#define CEL_PRIO0       RENDER_MAX_PIECES
#define CEL_DBLANK0     (CEL_PRIO0 + RENDER_MAX_PRIO)
#define CEL_BLANK       (CEL_DBLANK0 + RENDER_MAX_DBLANK)
#define CEL_TERM        (CEL_BLANK + 1)
#define CEL_SPRITE0     (CEL_BLANK + 2)
#define N_CELS          (CEL_SPRITE0 + RENDER_SPRITE_CELS)

#define CEL_MEM         (MEMTYPE_DRAM | MEMTYPE_CEL | MEMTYPE_FILL)
#define VCEL_MEM        (MEMTYPE_VRAM | MEMTYPE_CEL | MEMTYPE_FILL)
#define PLAIN_MEM       (MEMTYPE_DRAM | MEMTYPE_FILL)

/* Flags of every cel: absolute pointers, everything loaded from the CCB,
 * as CreateCel() sets them. */
#define BASE_FLAGS (CCB_NPABS | CCB_SPABS | CCB_PPABS | CCB_LDSIZE | CCB_LDPRS | \
                    CCB_LDPPMP | CCB_CCBPRE | CCB_YOXY | CCB_USEAV | CCB_NOBLK | \
                    CCB_ACE | CCB_ACW | CCB_ACCW)

#define PIXC_OPAQUE_WORD 0x1F001F00u

/* Expansion tables: the word holding pixels 0-3 (hi) or 4-7 (lo) of a
 * tile row, one byte per pixel, from one plane byte (bit 7 = pixel 0);
 * the flipped tables give pixels 7-4 and 3-0. */
#define XT_HI   0
#define XT_LO   256
#define XT_FHI  512
#define XT_FLO  768

static void init_tables(uint32 *t)
{
    uint32 b;

    for (b = 0; b < 256; b++) {
        uint32 hi = 0;
        uint32 lo = 0;
        uint32 fhi = 0;
        uint32 flo = 0;
        uint32 k;

        for (k = 0; k < 4; k++) {
            uint32 shift = 24 - 8 * k;          /* byte k of the word */

            hi |= ((b >> (7 - k)) & 1) << shift;        /* pixel k */
            lo |= ((b >> (3 - k)) & 1) << shift;        /* pixel 4 + k */
            fhi |= ((b >> k) & 1) << shift;             /* pixel 7 - k */
            flo |= ((b >> (4 + k)) & 1) << shift;       /* pixel 3 - k */
        }
        t[XT_HI + b] = hi;
        t[XT_LO + b] = lo;
        t[XT_FHI + b] = fhi;
        t[XT_FLO + b] = flo;
    }
}

/* SMS colour (--BBGGRR) to 3DO RGB 5:5:5. */
static uint32 sms_rgb(uint32 c)
{
    static const uint8 level[4] = { 0, 10, 21, 31 };

    return ((uint32)level[c & 3] << 10) | ((uint32)level[(c >> 2) & 3] << 5) |
           (uint32)level[(c >> 4) & 3];
}

/* The same for the 64 colours, built once (words: the ARM60 has no
 * halfword loads). */
static uint32 rgb_tab[64];

static void init_cel(CCB *c, uint32 flags)
{
    c->ccb_Flags = flags;
    c->ccb_NextPtr = NULL;
    c->ccb_SourcePtr = NULL;
    c->ccb_PLUTPtr = NULL;
    c->ccb_XPos = 0;
    c->ccb_YPos = 0;
    c->ccb_HDX = 1 << 20;
    c->ccb_HDY = 0;
    c->ccb_VDX = 0;
    c->ccb_VDY = 1 << 16;
    c->ccb_HDDX = 0;
    c->ccb_HDDY = 0;
    c->ccb_PIXC = PIXC_OPAQUE_WORD;
    c->ccb_PRE0 = 0;
    c->ccb_PRE1 = 0;
    c->ccb_Width = 0;
    c->ccb_Height = 0;
}

static void layer_free(render_layer *l)
{
    uint32 n = l->cap_cells;

    if (l->stale != NULL) FreeMem(l->stale, (int32)n);
    if (l->stamp != NULL) FreeMem(l->stamp, (int32)n);
    if (l->prev != NULL) FreeMem(l->prev, (int32)(n * 4));
    if (l->next != NULL) FreeMem(l->next, (int32)(n * 4));
    if (l->head != NULL) FreeMem(l->head, VDP_TILES * 4);
    if (l->shadow != NULL) FreeMem(l->shadow, (int32)(n * 2));
    memset(l, 0, sizeof(*l));
}

void render_free(renderer *r)
{
    uint32 k;

    for (k = 0; k < RENDER_LAYERS; k++)
        layer_free(&r->layer[k]);
    for (k = 0; k < 2; k++) {
        if (r->bm_pool[k] != NULL)
            FreeMem(r->bm_pool[k], POOL_BYTES);
    }
    if (r->xtab != NULL) FreeMem(r->xtab, 1024 * 4);
    if (r->blank != NULL) FreeMem(r->blank, 16);
    if (r->pluts != NULL) FreeMem(r->pluts, VDP_PAL_BANDS * RENDER_PLUT_SET * 2);
    if (r->cels != NULL) FreeMem(r->cels, N_CELS * sizeof(CCB));
    if (r->wlist != NULL) FreeMem(r->wlist, WLIST_WORDS * 4);
    if (r->dlist != NULL) FreeMem(r->dlist, VDP_TILES * 4);
    if (r->t_ok != NULL) FreeMem(r->t_ok, VDP_TILES);
    if (r->tiles_f != NULL) FreeMem(r->tiles_f, VDP_TILES * TILE_WORDS * 4);
    if (r->tiles != NULL) FreeMem(r->tiles, VDP_TILES * TILE_WORDS * 4);
    memset(r, 0, sizeof(*r));
}

/* Allocates the lists of a layer for up to cap cells; returns nonzero on
 * failure. The bitmaps come from the pool (set_mode). */
static int32 layer_init(renderer *r, render_layer *l, uint32 cap)
{
    l->cap_cells = cap;
    l->shadow = (uint8 *)AllocMem((int32)(cap * 2), PLAIN_MEM);
    l->head = (uint32 *)AllocMem(VDP_TILES * 4, PLAIN_MEM);
    l->next = (uint32 *)AllocMem((int32)(cap * 4), PLAIN_MEM);
    l->prev = (uint32 *)AllocMem((int32)(cap * 4), PLAIN_MEM);
    l->stamp = (uint8 *)AllocMem((int32)cap, PLAIN_MEM);
    l->stale = (uint8 *)AllocMem((int32)cap, PLAIN_MEM);
    l->tiles = r->tiles;
    l->tiles_f = r->tiles_f;
    l->t_ok = r->t_ok;
    l->wlist = r->wlist;
    l->dlist = r->dlist;
    l->xtab = r->xtab;
    return (l->shadow == NULL || l->head == NULL || l->next == NULL || l->prev == NULL ||
            l->stamp == NULL || l->stale == NULL) ? -1 : 0;
}

/*
 * Display height of the picture: 192 lines with a name table of 28 rows,
 * or 224 / 240 lines with 32 rows. The bitmaps of the two layers share
 * the two pool blocks (bitmaps, priority bitmaps): with 28 rows each
 * layer takes half of each block, with 32 rows the first layer takes
 * 64 KiB of each and the second one is not used. Changing the number of
 * rows invalidates the layers, which are rebuilt when next used.
 */
static void set_mode(renderer *r, uint32 height, uint32 force)
{
    uint32 rows = (height > RENDER_H) ? RENDER_NT_ROWS_MAX : RENDER_NT_ROWS;
    uint32 k;

    if (rows != r->nrows || force) {
        r->nrows = rows;
        r->bm_h = rows * 8;
        r->nlayers = (rows == RENDER_NT_ROWS) ? RENDER_LAYERS : 1;
        for (k = 0; k < RENDER_LAYERS; k++) {
            render_layer *l = &r->layer[k];

            l->valid = 0;
            l->nt_base = 0xFFFFFFFFu;
            l->last_used = 0;
            l->pend_tile = RENDER_NO_TILE;
            l->rb_next = RENDER_NO_TILE;
            l->nt_pend[0] = l->nt_pend[1] = 0;
            memset(l->tile_pend, 0, sizeof(l->tile_pend));
            l->stale_rows = 0;
            l->shown_rows = 0;
            l->pg_n = 0;
            memset(l->prio_rows, 0, sizeof(l->prio_rows));
            l->bitmap = r->bm_pool[0] + k * RENDER_BM_BYTES;
            l->prio_bm = r->bm_pool[1] + k * RENDER_BM_BYTES;
            l->nrows = (k < r->nlayers) ? rows : 0;
            l->ncells = l->nrows * RENDER_NT_COLS;
        }
    }
    if (height != r->height) {
        r->height = height;
        r->cels[CEL_BLANK].ccb_VDY = (int32)(height << 16);
    }
}

Err render_init(renderer *r)
{
    CCB *c;
    uint32 k;
    int32 bad = 0;

    memset(r, 0, sizeof(*r));
    r->tiles = (uint32 *)AllocMem(VDP_TILES * TILE_WORDS * 4, CEL_MEM);
    /* The flipped store lives in VRAM, which the CPU and the cel engine
     * read like DRAM, to leave DRAM to the layers. */
    r->tiles_f = (uint32 *)AllocMem(VDP_TILES * TILE_WORDS * 4, VCEL_MEM);
    if (r->tiles_f == NULL)
        r->tiles_f = (uint32 *)AllocMem(VDP_TILES * TILE_WORDS * 4, CEL_MEM);
    r->t_ok = (uint8 *)AllocMem(VDP_TILES, PLAIN_MEM);
    r->wlist = (uint32 *)AllocMem(WLIST_WORDS * 4, PLAIN_MEM);
    r->dlist = (uint32 *)AllocMem(VDP_TILES * 4, PLAIN_MEM);
    r->cels = (CCB *)AllocMem(N_CELS * sizeof(CCB), CEL_MEM);
    r->pluts = (uint16 *)AllocMem(VDP_PAL_BANDS * RENDER_PLUT_SET * 2, CEL_MEM);
    r->blank = (uint32 *)AllocMem(16, CEL_MEM);
    r->xtab = (uint32 *)AllocMem(1024 * 4, PLAIN_MEM);
    r->bm_pool[0] = (uint8 *)AllocMem(POOL_BYTES, CEL_MEM);
    r->bm_pool[1] = (uint8 *)AllocMem(POOL_BYTES, CEL_MEM);
    if (r->tiles == NULL || r->tiles_f == NULL || r->t_ok == NULL || r->wlist == NULL ||
        r->dlist == NULL || r->cels == NULL || r->pluts == NULL || r->blank == NULL ||
        r->xtab == NULL || r->bm_pool[0] == NULL || r->bm_pool[1] == NULL)
        bad = 1;
    /* The first layer holds a table of 32 rows in the taller modes. */
    for (k = 0; k < RENDER_LAYERS && !bad; k++)
        bad = layer_init(r, &r->layer[k], k == 0 ? RENDER_NT_CELLS_MAX : RENDER_NT_CELLS) != 0;
    if (bad) {
        printf("ERROR: renderer out of memory\n");
        render_free(r);
        return -1;
    }
    init_tables(r->xtab);
    for (k = 0; k < 64; k++)
        rgb_tab[k] = (uint16)sms_rgb(k);

    /* Background pieces: opaque, the background PLUT of their band. */
    for (k = 0; k < RENDER_MAX_PIECES; k++) {
        c = &r->cels[k];
        init_cel(c, BASE_FLAGS | CCB_LDPLUT | CCB_BGND);
        c->ccb_PLUTPtr = r->pluts;
    }
    /* Priority strips: the same, with the PLUT whose colour 0 is
     * transparent. */
    for (k = 0; k < RENDER_MAX_PRIO; k++) {
        c = &r->cels[CEL_PRIO0 + k];
        init_cel(c, BASE_FLAGS | CCB_LDPLUT);
        c->ccb_PLUTPtr = r->pluts + 64;
    }
    /* Backdrop rectangles: an 8 x 1 uncoded 16 bpp row of the backdrop
     * colour stretched over a range of lines (the blanked left column) or
     * over the whole width of a range of lines (display off), drawn after
     * the sprites. */
    for (k = 0; k <= RENDER_MAX_DBLANK; k++) {
        c = &r->cels[CEL_DBLANK0 + k];       /* the last one is CEL_BLANK */
        init_cel(c, BASE_FLAGS | CCB_BGND);
        c->ccb_SourcePtr = (CelData *)r->blank;
        c->ccb_PRE0 = ((1 - PRE0_VCNT_PREFETCH) << PRE0_VCNT_SHIFT) | PRE0_LINEAR |
                      PRE0_BPP_16;
        c->ccb_PRE1 = ((4 - PRE1_WOFFSET_PREFETCH) << PRE1_WOFFSET10_SHIFT) |
                      ((8 - PRE1_TLHPCNT_PREFETCH) << PRE1_TLHPCNT_SHIFT) | PRE1_TLLSB_PDC0;
    }
    for (k = 0; k < RENDER_MAX_DBLANK; k++)
        r->cels[CEL_DBLANK0 + k].ccb_HDX = (RENDER_W / 8) << 20;
    /* Terminator: the list before the first update (skipped, last). The
     * chain built by an update ends with CCB_LAST on its real last cel. */
    c = &r->cels[CEL_TERM];
    init_cel(c, CCB_SKIP | CCB_LAST | CCB_NPABS | CCB_SPABS | CCB_PPABS);
    /* Sprites: colour 0 transparent, drawn from the last to the first. */
    for (k = 0; k < RENDER_SPRITE_CELS; k++) {
        c = &r->cels[CEL_SPRITE0 + k];
        init_cel(c, BASE_FLAGS | CCB_LDPLUT);
        c->ccb_PLUTPtr = r->pluts + 32;
        c->ccb_NextPtr = (k == 0) ? NULL : &r->cels[CEL_SPRITE0 + k - 1];
        c->ccb_PRE0 = ((8 - PRE0_VCNT_PREFETCH) << PRE0_VCNT_SHIFT) | PRE0_BPP_8;
        c->ccb_PRE1 = ((2 - PRE1_WOFFSET_PREFETCH) << PRE1_WOFFSET10_SHIFT) |
                      ((8 - PRE1_TLHPCNT_PREFETCH) << PRE1_TLHPCNT_SHIFT) | PRE1_TLLSB_PDC0;
    }
    render_reset(r);
    return 0;
}

void render_reset(renderer *r)
{
    uint32 k;

    set_mode(r, RENDER_H, 1);
    for (k = 0; k < RENDER_LAYERS; k++)
        r->layer[k].stamp_now = 0;
    r->update = 0;
    r->spr_mode = 0xFFFFFFFFu;
    r->spr_runs = 0;
    r->spr_bands = 0;
    r->n_sprites = 0;
    r->npb = 1;
    r->backdrop = 0xFFFFFFFFu;
    r->display_on = 0;
    r->chain = &r->cels[CEL_TERM];
    r->last_cel = &r->cels[CEL_TERM];
    memset(r->t_ok, 0, VDP_TILES);
    memset(&r->st, 0, sizeof(r->st));
}

/*--------------------------------------------------------------------------
 * Tiles and cells
 *------------------------------------------------------------------------*/

/* Takes the name table of the layer as its shadow and empties the cell
 * lists and the priority bookkeeping, before the cells are drawn. */
static void rebuild_start(render_layer *l, const uint8 *vram)
{
    const uint8 *nt = vram + l->nt_base;
    uint32 i;

    memcpy(l->shadow, nt, (size_t)(l->ncells * 2));
    memset(l->prio_bm, 0, (size_t)(l->nrows * 8 * BM_PITCH));
    memset(l->prio_rows, 0, sizeof(l->prio_rows));
    memset(l->prio_dirty, 1, sizeof(l->prio_dirty));
    memset(l->stale, 0, (size_t)l->ncells);
    l->stale_rows = 0;
    l->pend_tile = RENDER_NO_TILE;
    l->nt_pend[0] = l->nt_pend[1] = 0;
    memset(l->tile_pend, 0, sizeof(l->tile_pend));
    for (i = 0; i < VDP_TILES; i++)
        l->head[i] = NONE;
}

/* A rebuild begun ahead (render_catch_up) goes on from the table as it
 * stands: the shadow of the cells not drawn yet is taken again, and the
 * pending chunks entirely beyond the next cell are dropped, their cells
 * being drawn with the current entries. */
static void rebuild_refresh(render_layer *l, const uint8 *vram)
{
    uint32 from = l->rb_next;
    uint32 c;

    memcpy(l->shadow + 2 * from, vram + l->nt_base + 2 * from, (size_t)(2 * (l->ncells - from)));
    for (c = (from + 15) >> 4; c < l->nrows * 2; c++)
        l->nt_pend[c >> 5] &= ~((uint32)1 << (c & 31));
}

/* Draws the whole name table of the layer into its bitmap. */
static void rebuild(renderer *r, render_layer *l, const uint8 *vram)
{
    rebuild_start(l, vram);
    render_rebuild_cells(l, vram);
    l->valid = 1;
    l->rb_next = RENDER_NO_TILE;
    r->st.rebuilds++;
}

/* Name table words of the dirty chunks that differ from the shadow:
 * their entries are re-linked, drawn and copied to the shadow. */
static void update_entries(render_layer *l, const uint32 *nt_mask, const uint8 *vram)
{
    const uint32 *a = (const uint32 *)(vram + l->nt_base);
    uint32 n = render_nt_scan(a, (const uint32 *)l->shadow, nt_mask, l->wlist, l->nrows * 2);

    if (n != 0)
        render_entries(l, vram, n);
}

/*--------------------------------------------------------------------------
 * Layers
 *------------------------------------------------------------------------*/

/* The layer holding the name table at base: valid, or being rebuilt for
 * it in spare time. */
#define layer_holds(l, b) (((l)->valid || (l)->rb_next != RENDER_NO_TILE) && (l)->nt_base == (b))

static render_layer *layer_of(renderer *r, uint32 base)
{
    if (layer_holds(&r->layer[0], base))
        return &r->layer[0];
    if (r->nlayers > 1 && layer_holds(&r->layer[1], base))
        return &r->layer[1];
    return NULL;
}

/* Assigns a layer to each name table band of the frame (r->nvals, nnb
 * bands): the layer already holding the table, else a free or stale one
 * (an invalid layer, then the least recently used among those the frame
 * does not need), which is marked for a rebuild. Layers unused for a
 * while are dropped first. */
static void assign_layers(renderer *r, const vdp_state *v, uint32 nnb)
{
    uint32 k;
    uint32 m;

    for (k = 0; k < r->nlayers; k++) {
        render_layer *l = &r->layer[k];

        if (l->valid && r->update - l->last_used > RENDER_LAYER_KEEP)
            l->valid = 0;
    }
    for (m = 0; m < nnb; m++) {
        uint32 base = vdp_nt_base(v, r->nvals[m]);
        render_layer *l = layer_of(r, base);
        uint32 j;

        /* The table of an earlier band of this frame (its layer may be
         * waiting for its rebuild, hence not valid yet). */
        for (j = 0; j < m && l == NULL; j++) {
            if (r->nlayer[j]->nt_base == base)
                l = r->nlayer[j];
        }
        if (l == NULL) {
            /* A layer taken by an earlier band of this frame is not
             * free; with none left the first band's layer is reused,
             * which draws the wrong table there. */
            render_layer *best = NULL;

            for (k = 0; k < r->nlayers; k++) {
                render_layer *c = &r->layer[k];
                uint32 taken = 0;

                for (j = 0; j < m; j++) {
                    if (r->nlayer[j] == c)
                        taken = 1;
                }
                if (taken)
                    continue;
                if (best == NULL || (!c->valid && best->valid) ||
                    (c->valid == best->valid && c->last_used < best->last_used))
                    best = c;
            }
            if (best == NULL) {
                r->st.layers++;
                r->nlayer[m] = r->nlayer[0];
                continue;
            }
            l = best;
            l->nt_base = base;
            l->valid = 0;               /* rebuilt by the update */
            l->rb_next = RENDER_NO_TILE;    /* a rebuild begun ahead for another table is dropped */
        }
        l->last_used = r->update;
        r->nlayer[m] = l;
    }
}

/*--------------------------------------------------------------------------
 * Palettes
 *------------------------------------------------------------------------*/

/* The PLUTs of a palette band from its 32 colours: background, sprites
 * (colour 0 transparent (000), black elsewhere made opaque (001)) and
 * priority strips (colour 0 of both palettes transparent, black elsewhere
 * opaque: the cel engine tests the colour the PLUT gives, not the index). */
static void band_pluts(uint16 *set, const uint8 *cram)
{
    /* Two entries per word store, the first in the high half (big
     * endian): the ARM60 has no halfword stores. */
    uint32 *bg = (uint32 *)set;
    uint32 *sp = (uint32 *)(set + 32);
    uint32 *pr = (uint32 *)(set + 64);
    uint32 k;

    for (k = 0; k < 32; k += 2) {
        uint32 c0 = rgb_tab[cram[k] & 63];
        uint32 c1 = rgb_tab[cram[k + 1] & 63];
        uint32 o0 = c0 ? c0 : 1;        /* black made opaque */
        uint32 o1 = c1 ? c1 : 1;

        bg[k >> 1] = (c0 << 16) | c1;
        if ((k & 15) == 0)
            o0 = 0;                     /* colour 0 of each palette: transparent */
        pr[k >> 1] = (o0 << 16) | o1;
        if (k >= 16)
            sp[(k - 16) >> 1] = sp[k >> 1] = (o0 << 16) | o1;
    }
}

/* The palette bands of the frame and their PLUTs: the first band's when
 * the colour RAM changed, the others' (colour writes during the active
 * display) every frame that has them. */
static void update_palettes(renderer *r, vdp_state *v)
{
    uint32 merged;
    uint32 k;

    r->npb = vdp_palette_bands(v, r->ptops, r->pcram, &merged);
    if (v->cram_dirty) {
        v->cram_dirty = 0;
        band_pluts(r->pluts, (const uint8 *)r->pcram);
        r->st.palettes++;
    }
    for (k = 1; k < r->npb; k++)
        band_pluts(r->pluts + k * RENDER_PLUT_SET, (const uint8 *)(r->pcram + 8 * k));
    if (r->npb > 1)
        r->st.pbands++;
    r->st.pmerged += merged;
    k = rgb_tab[((const uint8 *)r->pcram)[16 + (v->reg[7] & 15)] & 63];
    if (k != r->backdrop) {
        r->backdrop = k;
        r->st.backdrops++;
        k |= k << 16;
        r->blank[0] = r->blank[1] = r->blank[2] = r->blank[3] = k;
    }
}

/*--------------------------------------------------------------------------
 * Background pieces and priority strips
 *------------------------------------------------------------------------*/

/* One piece: screen columns sa to sb - 1 of h lines from screen line sy,
 * showing rows from src_y of bitmap bm with horizontal scroll hs, with
 * the PLUT of the piece context. The source starts on a word boundary:
 * the piece may begin up to 3 pixels left of sa, where it is either
 * clipped or covered by the piece drawn after (or shows the right pixels
 * of the transparent priority layer). Beyond the capacity the piece is
 * dropped and counted. */
#define EMIT_PIECE(r, p, bm, sy, h, src_y, hs, sa, sb) do { \
        uint32 c0_ = ((sa) - (hs)) & 255; \
        uint32 ca_ = c0_ & ~3u; \
        CCB *cc_ = (p)->c; \
        if (cc_ == (p)->end) { \
            (r)->st.dropped++; \
        } else { \
            (p)->c = cc_ + 1; \
            cc_->ccb_SourcePtr = (CelData *)((bm) + (src_y) * BM_PITCH + ca_); \
            cc_->ccb_PLUTPtr = (p)->plut; \
            cc_->ccb_XPos = (Coord)((int32)((sa) - (c0_ - ca_)) << 16); \
            cc_->ccb_YPos = (Coord)((sy) << 16); \
            cc_->ccb_PRE0 = (((h) - PRE0_VCNT_PREFETCH) << PRE0_VCNT_SHIFT) | PRE0_BPP_8; \
            cc_->ccb_PRE1 = ((BM_WORDS - PRE1_WOFFSET_PREFETCH) << PRE1_WOFFSET10_SHIFT) | \
                            (((sb) - (sa) + (c0_ - ca_) - PRE1_TLHPCNT_PREFETCH) \
                             << PRE1_TLHPCNT_SHIFT) | PRE1_TLLSB_PDC0; \
        } \
    } while (0)

/*
 * The rectangle of bitmap columns bx0 to bx1 - 1 and rows by0 to by1 - 1
 * on the lines of line band lb (ya | yb << 8 | hs << 16), as pieces:
 * bitmap column bx shows at screen column (bx + hs) & 255 and row by at
 * screen line (by - vs) mod p->bm_h, a wrap of the rows or of the
 * columns splits the rectangle, the right part going first so that the
 * left part covers its overdraw. Register 0 bit 6 keeps rows 0-1 (the
 * lines before 16) unscrolled horizontally, bit 7 keeps the right 64
 * columns unscrolled vertically: each makes a window of its own, the
 * right columns first. Without those bits the callers use
 * render_rect_pieces (render_a.s), which does the same with one window.
 */
static void rect_pieces(renderer *r, piece_ctx *p, const uint8 *bm, uint32 bx0, uint32 bx1,
                        uint32 by0, uint32 by1, uint32 lb, uint32 reg0, uint32 vs)
{
    uint32 bh = p->bm_h;
    uint32 lya;
    uint32 lyb;
    uint32 lhs;
    uint32 h;
    uint32 wd;
    uint32 lp_ya[2];
    uint32 lp_yb[2];
    uint32 lp_hs[2];
    uint32 cp_xa[2];
    uint32 cp_xb[2];
    uint32 cp_vs[2];
    uint32 nlp = 1;
    uint32 ncp = 1;
    uint32 lpi;
    uint32 cpi;

    lya = lb & 0xFF;
    lyb = (lb >> 8) & 0xFF;
    lhs = (lb >> 16) & 0xFF;
    h = by1 - by0;
    wd = bx1 - bx0;
    lp_ya[0] = lya;
    lp_yb[0] = lyb;
    lp_hs[0] = lhs;
    if ((reg0 & 0x40) && lya < 16) {
        lp_hs[0] = 0;
        if (lyb > 16) {
            lp_yb[0] = 16;
            lp_ya[1] = 16;
            lp_yb[1] = lyb;
            lp_hs[1] = lhs;
            nlp = 2;
        }
    }
    cp_xa[0] = 0;
    cp_xb[0] = RENDER_W;
    cp_vs[0] = vs;
    if (reg0 & 0x80) {
        cp_xa[0] = 192;
        cp_vs[0] = 0;
        cp_xa[1] = 0;
        cp_xb[1] = 192;
        cp_vs[1] = vs;
        ncp = 2;
    }
    for (lpi = 0; lpi < nlp; lpi++)
    for (cpi = 0; cpi < ncp; cpi++) {
        uint32 ya = lp_ya[lpi];
        uint32 yb = lp_yb[lpi];
        uint32 hs = lp_hs[lpi];
        uint32 xa = cp_xa[cpi];
        uint32 xb = cp_xb[cpi];
        uint32 wvs = cp_vs[cpi];
        uint32 s0;
        uint32 t0;
        uint32 vp;

        s0 = by0 + 2 * bh - wvs;            /* screen line of row by0 */
        while (s0 >= bh)
            s0 -= bh;
        t0 = (bx0 + hs) & 255;              /* screen column of column bx0 */
        for (vp = 0; vp < 2; vp++) {
            uint32 iy0;
            uint32 iy1;
            uint32 src_y;
            uint32 sy;
            uint32 ey;
            uint32 sa;
            uint32 sb;

            if (vp == 0) {
                iy0 = s0;
                iy1 = (s0 + h <= bh) ? s0 + h : bh;
                src_y = by0;
            } else {
                if (s0 + h <= bh)
                    break;
                iy0 = 0;
                iy1 = s0 + h - bh;
                src_y = by0 + (bh - s0);
            }
            sy = (iy0 > ya) ? iy0 : ya;
            ey = (iy1 < yb) ? iy1 : yb;
            if (sy >= ey)
                continue;
            src_y += sy - iy0;
            /* The part from screen column t0, then the part wrapped to
             * the left edge. */
            sa = (t0 > xa) ? t0 : xa;
            sb = (t0 + wd < 256) ? t0 + wd : 256;
            if (sb > xb)
                sb = xb;
            if (sa < sb)
                EMIT_PIECE(r, p, bm, sy, ey - sy, src_y, hs, sa, sb);
            if (t0 + wd > 256) {
                sa = xa;
                sb = (t0 + wd - 256 < xb) ? t0 + wd - 256 : xb;
                if (sa < sb)
                    EMIT_PIECE(r, p, bm, sy, ey - sy, src_y, hs, sa, sb);
            }
        }
    }
}

/* Name table rows shown by the lines ya to yb - 1 with vertical scroll
 * vs, as a bit mask, for a table of rows rows (bitmap of rows * 8 lines). */
static uint32 rows_of(uint32 ya, uint32 yb, uint32 vs, uint32 rows)
{
    uint32 bh = rows * 8;
    uint32 r0 = ya + vs;
    uint32 r1 = yb - 1 + vs;
    uint32 m;

    while (r0 >= bh)
        r0 -= bh;
    while (r1 >= bh)
        r1 -= bh;
    r0 >>= 3;
    r1 >>= 3;
    m = (r1 >= r0) ? (((uint32)2 << r1) - ((uint32)1 << r0))
                   : ((((uint32)2 << r1) - 1) | ~(((uint32)1 << r0) - 1));
    return (rows >= 32) ? m : (m & (((uint32)1 << rows) - 1));
}

/* Palette band holding line y. */
static uint32 pband_of(const renderer *r, uint32 y)
{
    uint32 q = 0;

    while (q + 1 < r->npb && r->ptops[q + 1] <= y)
        q++;
    return q;
}

/*
 * Line bands of the update: the scroll bands (register 8 writes recorded
 * during the active display) crossed with the name table bands
 * (register 2), the palette bands (colour writes) and the display bands
 * (register 1 bit 6). The lines with the display on make the line bands
 * (r->lbands, with the scroll, the layer of their table and their
 * palette band); the other lines get a backdrop rectangle. Also notes the
 * rows each layer shows. Returns the number of backdrop rectangles and,
 * in *any_on, whether any line is displayed. The band tables were filled
 * by the caller (r->tops... with nb, nnb and ndb bands, r->ptops with
 * r->npb).
 */
static uint32 line_bands(renderer *r, const vdp_state *v, uint32 nb, uint32 nnb, uint32 ndb,
                         uint32 *any_on)
{
    uint32 vs = v->reg[9];
    uint32 vsi = (uint32)v->reg[0] & 0x80;
    uint32 h = r->height;
    uint32 nbl = 0;
    uint32 nl = 0;
    uint32 on = 0;
    uint32 j;
    uint32 k;
    uint32 m;
    uint32 q;

    if (nb > 1)
        r->st.bands++;
    if (ndb > 1)
        r->st.dbands++;
    if (nnb > 1)
        r->st.ntbands++;
    for (m = 0; m < RENDER_LAYERS; m++)
        r->layer[m].shown_rows = 0;
    for (j = 0; j < ndb; j++) {
        uint32 da = r->dtops[j];
        uint32 db = (j + 1 < ndb) ? r->dtops[j + 1] : h;

        if (da >= h)
            break;
        if (db > h)
            db = h;
        if (!r->dons[j]) {
            CCB *c = &r->cels[CEL_DBLANK0 + nbl++];

            c->ccb_YPos = (Coord)(da << 16);
            c->ccb_VDY = (int32)((db - da) << 16);
            continue;
        }
        on = 1;
        for (m = 0; m < nnb; m++) {
            uint32 na = r->ntops[m];
            uint32 nb2 = (m + 1 < nnb) ? r->ntops[m + 1] : h;
            render_layer *l = r->nlayer[m];
            uint32 li = (l == &r->layer[0]) ? 0 : 1;    /* no pointer division */

            if (na < da)
                na = da;
            if (nb2 > db)
                nb2 = db;
            if (na >= nb2)
                continue;
            for (k = 0; k < nb; k++) {
                uint32 ya = r->tops[k];
                uint32 yb = (k + 1 < nb) ? r->tops[k + 1] : h;

                if (ya < na)
                    ya = na;
                if (yb > nb2)
                    yb = nb2;
                if (ya >= yb)
                    continue;
                if (r->npb == 1) {
                    r->lbands[nl++] = ya | (yb << 8) | ((r->hss[k] & 0xFF) << 16) | (li << 24);
                } else {
                    for (q = pband_of(r, ya); q < r->npb && r->ptops[q] < yb; q++) {
                        uint32 pa = r->ptops[q];
                        uint32 pz = (q + 1 < r->npb) ? r->ptops[q + 1] : h;

                        if (pa < ya)
                            pa = ya;
                        if (pz > yb)
                            pz = yb;
                        r->lbands[nl++] = pa | (pz << 8) | ((r->hss[k] & 0xFF) << 16) |
                                          (li << 24) | (q << 25);
                    }
                }
                l->shown_rows |= rows_of(ya, yb, vs, l->nrows);
                if (vsi)
                    l->shown_rows |= rows_of(ya, yb, 0, l->nrows);
            }
        }
    }
    r->nlb = nl;
    for (k = 1; k < nbl; k++)
        r->cels[CEL_DBLANK0 + k - 1].ccb_NextPtr = &r->cels[CEL_DBLANK0 + k];
    *any_on = on;
    return nbl;
}

/* Priority strips of the layer for this update: consecutive rows holding
 * priority cells make one strip, over the columns any of them uses (a
 * few transparent pixels cost less than a cel per row). */
static void prio_groups(render_layer *l)
{
    uint32 rows = l->nrows;
    uint32 row = 0;
    uint32 n = 0;

    while (row < rows) {
        uint32 row0;
        uint32 c0;
        uint32 c1;

        if (l->prio_rows[row] == 0) {
            row++;
            continue;
        }
        if (l->prio_dirty[row]) {
            const uint8 *sh = l->shadow + row * 64;
            uint32 lo = 32;
            uint32 hi = 0;
            uint32 col;

            for (col = 0; col < 32; col++) {
                if (sh[2 * col + 1] & 0x10) {
                    if (col < lo)
                        lo = col;
                    hi = col;
                }
            }
            l->prio_min[row] = (uint8)lo;
            l->prio_max[row] = (uint8)hi;
            l->prio_dirty[row] = 0;
        }
        row0 = row;
        c0 = l->prio_min[row];
        c1 = l->prio_max[row] + 1;
        for (row++; row < rows; row++) {
            if (l->prio_rows[row] == 0 || l->prio_dirty[row])
                break;
            if (l->prio_min[row] < c0)
                c0 = l->prio_min[row];
            if (l->prio_max[row] + 1u > c1)
                c1 = l->prio_max[row] + 1u;
        }
        l->pg_row0[n] = (uint8)row0;
        l->pg_nrows[n] = (uint8)(row - row0);
        l->pg_col0[n] = (uint8)c0;
        l->pg_col1[n] = (uint8)c1;
        n++;
    }
    l->pg_n = n;
}

/* First line band (sorted by their first line, which is also the order
 * of their last lines) ending after line y. */
static uint32 lband_from(const renderer *r, uint32 y)
{
    uint32 lo = 0;
    uint32 hi = r->nlb;

    while (lo < hi) {
        uint32 mid = (lo + hi) >> 1;

        if (((r->lbands[mid] >> 8) & 0xFF) > y)
            hi = mid;
        else
            lo = mid + 1;
    }
    return lo;
}

/* The strips of a group of priority rows of layer m, whose screen lines
 * are y0 to y1 - 1 (y1 may run past the bitmap height: the rest wraps to
 * the top), on the line bands of that layer which they reach, each band
 * cut to the lines of the pass so that a group wrapping to the top is
 * not drawn twice on a band holding both of its parts. Each strip takes
 * the priority PLUT of its band's palette. */
static void group_strips(renderer *r, piece_ctx *p, uint32 m, const uint8 *bm, uint32 bx0,
                         uint32 bx1, uint32 by0, uint32 by1, uint32 y0, uint32 y1,
                         uint32 reg0, uint32 vs)
{
    uint32 pass;

    for (pass = 0; pass < 2; pass++) {
        uint32 ga;
        uint32 gb;
        uint32 k;

        if (pass == 0) {
            ga = y0;
            gb = (y1 < r->height) ? y1 : r->height;
        } else {
            if (y1 <= r->bm_h)
                break;
            ga = 0;
            gb = y1 - r->bm_h;
        }
        if (ga >= gb)
            continue;
        for (k = lband_from(r, ga); k < r->nlb; k++) {
            uint32 lb = r->lbands[k];
            uint32 la = lb & 0xFF;
            uint32 lz = (lb >> 8) & 0xFF;

            if (la >= gb)
                break;
            if (((lb >> 24) & 1) != m)
                continue;
            if (la < ga)
                la = ga;
            if (lz > gb)
                lz = gb;
            lb = (lb & 0xFFFF0000u) | la | (lz << 8);
            p->plut = r->pluts + (lb >> 25) * RENDER_PLUT_SET + 64;
            if (reg0 & 0xC0)
                rect_pieces(r, p, bm, bx0, bx1, by0, by1, lb, reg0, vs);
            else
                r->st.dropped += render_rect_pieces(p, bm, bx0, bx1, by0, by1, lb, vs);
        }
    }
}

/* Background pieces of the line bands, from the bitmap of their layer,
 * then the priority layer: each strip group of a layer on the line bands
 * of that layer which its lines reach, replaced by the parts of those
 * strips under the sprites when they cover fewer pixels (nothing at all
 * without a sprite over a strip). Returns the number of pieces and, in
 * *nprio, the number of priority cels, which start at *prio0. */
static uint32 update_pieces(renderer *r, const vdp_state *v, uint32 *nprio, uint32 *prio0)
{
    piece_ctx pb;
    piece_ctx pp;
    uint32 reg0 = v->reg[0];
    uint32 vs = v->reg[9];
    uint32 np;
    uint32 npr;
    uint32 ns;
    uint32 first = CEL_PRIO0;
    uint32 k;
    uint32 m;

    pb.c = &r->cels[0];
    pb.end = &r->cels[RENDER_MAX_PIECES];
    pb.bm_h = r->bm_h;
    for (k = 0; k < r->nlb; k++) {
        uint32 lb = r->lbands[k];
        const uint8 *bm = r->layer[(lb >> 24) & 1].bitmap;

        pb.plut = r->pluts + (lb >> 25) * RENDER_PLUT_SET;
        if (reg0 & 0xC0)
            rect_pieces(r, &pb, bm, 0, RENDER_W, 0, r->bm_h, lb, reg0, vs);
        else
            r->st.dropped += render_rect_pieces(&pb, bm, 0, RENDER_W, 0, r->bm_h, lb, vs);
    }
    np = (uint32)(pb.c - &r->cels[0]);
    for (k = 1; k < np; k++)
        r->cels[k - 1].ccb_NextPtr = &r->cels[k];

    pp.c = &r->cels[CEL_PRIO0];
    pp.end = &r->cels[CEL_DBLANK0];
    pp.bm_h = r->bm_h;
    pp.plut = r->pluts + 64;
    for (m = 0; m < RENDER_LAYERS; m++) {
        render_layer *l = &r->layer[m];
        uint32 g;

        for (g = 0; g < l->pg_n; g++) {
            uint32 by0 = (uint32)l->pg_row0[g] * 8;
            uint32 by1 = by0 + (uint32)l->pg_nrows[g] * 8;
            uint32 bx0 = (uint32)l->pg_col0[g] * 8;
            uint32 bx1 = (uint32)l->pg_col1[g] * 8;
            uint32 y0 = by0 + 2 * r->bm_h - vs;     /* screen lines of the group */

            while (y0 >= r->bm_h)
                y0 -= r->bm_h;
            if (reg0 & 0x80) {
                /* With the vertical scroll inhibit the right columns show
                 * the rows unscrolled as well: every band may be reached. */
                group_strips(r, &pp, m, l->prio_bm, bx0, bx1, by0, by1, 0, r->height, reg0,
                             vs);
            } else {
                group_strips(r, &pp, m, l->prio_bm, bx0, bx1, by0, by1, y0, y0 + (by1 - by0),
                             reg0, vs);
            }
        }
    }
    ns = (uint32)(pp.c - &r->cels[CEL_PRIO0]);
    npr = ns;
    if (ns != 0 && v->spr_n != 0) {
        render_patch_args a;
        uint32 pixels = 0;

        for (k = 0; k < ns; k++) {
            const CCB *c = &r->cels[CEL_PRIO0 + k];

            pixels += (((c->ccb_PRE0 >> PRE0_VCNT_SHIFT) & 0x3FF) + 1) *
                      ((c->ccb_PRE1 & 0x7FF) + 1);
        }
        a.strips = &r->cels[CEL_PRIO0];
        a.nstrips = ns;
        a.xoff = (reg0 & 0x08) ? 8 : 0;
        a.v = v;
        a.sat = v->vram + ((v->reg[5] & 0x7E) << 7);
        a.limit = pixels;
        a.area = 0;
        a.p = &pp;
        a.height = r->height;
        if (render_prio_patches(&a)) {
            first = CEL_PRIO0 + ns;
            npr = (uint32)(pp.c - &r->cels[first]);
            r->st.prio_patches += npr;
            ns = 0;
        }
    }
    r->st.prio_strips += ns;
    for (k = 1; k < npr; k++)
        r->cels[first + k - 1].ccb_NextPtr = &r->cels[first + k];
    r->st.pieces += np;
    *nprio = npr;
    *prio0 = first;
    return np;
}

/*--------------------------------------------------------------------------
 * Sprites
 *------------------------------------------------------------------------*/

static void sprite_tiles(renderer *r, uint32 t, uint32 tall, const uint8 *vram)
{
    if (!(r->t_ok[t] & 1))
        render_tile_conv(&r->layer[0], t, 0, vram);
    if (tall && !(r->t_ok[t + 1] & 1))
        render_tile_conv(&r->layer[0], t + 1, 0, vram);
}

/* Sets up the cels of the active sprites (before the $D0 terminator in
 * the 192-line mode) and converts the tiles they miss; returns the count.
 * The usual case, every sprite shown whole, is one cel per sprite set up
 * in assembly. With several palette bands each cel takes the sprite PLUT
 * of the band of its first line. */
static uint32 update_sprites(renderer *r, const vdp_state *v)
{
    const uint8 *vram = v->vram;
    const uint8 *sat = vram + ((v->reg[5] & 0x7E) << 7);
    uint32 tall = (v->reg[1] & 0x02) ? 1 : 0;
    uint32 mode = tall | (v->spr_zoom << 1);
    render_sprite_args a;
    uint32 n = v->spr_n;
    uint32 nn;
    uint32 k;

    if (mode != r->spr_mode || r->spr_runs) {
        uint32 scale = 1 << v->spr_zoom;

        r->spr_mode = mode;
        r->spr_runs = 0;
        for (k = 0; k < RENDER_SPRITE_CELS; k++) {
            CCB *c = &r->cels[CEL_SPRITE0 + k];

            c->ccb_PRE0 = (((tall ? 16 : 8) - PRE0_VCNT_PREFETCH) << PRE0_VCNT_SHIFT) |
                          PRE0_BPP_8;
            c->ccb_HDX = (int32)(scale << 20);
            c->ccb_VDY = (int32)(scale << 16);
        }
    }
    a.tiles = r->tiles;
    a.t_ok = r->t_ok;
    a.tbase = (v->reg[6] & 0x04) ? 256 : 0;
    a.tmask = tall ? 0xFE : 0xFF;
    a.xoff = (v->reg[0] & 0x08) ? 8 : 0;
    a.need = r->wlist;
    a.tall = tall;
    a.ywrap = 256 - v->spr_h;
    if (v->spr_partial) {
        CCB *c;

        r->spr_runs = 1;
        r->st.spr_partial++;
        a.end = &r->cels[CEL_SPRITE0 + RENDER_SPRITE_CELS];
        a.dropped = 0;
        c = render_sprite_runs(sat, v, &r->cels[CEL_SPRITE0], &a);
        r->st.spr_dropped += a.dropped;
        n = (uint32)(c - &r->cels[CEL_SPRITE0]);
        nn = a.needed;
    } else {
        nn = render_sprites(sat, n, &r->cels[CEL_SPRITE0], &a);
    }
    for (k = 0; k < nn; k++)
        sprite_tiles(r, r->wlist[k], tall, vram);
    if (r->npb > 1 || r->spr_bands) {
        uint16 *spl[VDP_PAL_BANDS];
        int32 top[VDP_PAL_BANDS + 1];
        uint32 npb = r->npb;
        CCB *c = &r->cels[CEL_SPRITE0];
        uint32 q;

        r->spr_bands = (npb > 1);
        for (q = 0; q < npb; q++) {
            spl[q] = r->pluts + q * RENDER_PLUT_SET + 32;
            top[q] = (int32)r->ptops[q];
        }
        top[npb] = 0x7FFFFFFF;          /* the last band runs to the bottom */
        for (k = 0; k < n; k++, c++) {
            int32 y = (int32)c->ccb_YPos >> 16;

            for (q = 0; top[q + 1] <= y; q++)
                ;
            c->ccb_PLUTPtr = spl[q];
        }
    }
    r->st.sprites += n;
    return n;
}

/*--------------------------------------------------------------------------
 * Frame update
 *------------------------------------------------------------------------*/

void render_update(renderer *r, vdp_state *v)
{
    const uint8 *vram = v->vram;
    uint32 nt_mask[RENDER_LAYERS][2];
    uint32 c0;
    uint32 n0;
    uint32 c1;
    uint32 n1;
    uint32 nb;
    uint32 nnb;
    uint32 ndb;
    uint32 nd;
    uint32 np;
    uint32 npr;
    uint32 npr0;
    uint32 nbl;
    uint32 any_on;
    uint32 ns;
    uint32 k;
    uint32 m;
    CCB *last;

    /* The display height decides the rows of the name table bitmaps. */
    set_mode(r, v->active, 0);
    if (r->height > RENDER_H)
        r->st.tall++;
    update_palettes(r, v);

    /* While the display is off nothing is drawn: the work waits, and the
     * dirty flags accumulate, until it is on for some lines. */
    r->display_on = v->de_start || v->de_n != 0;
    if (!r->display_on)
        return;
    r->update++;

    /* The name tables of the frame and their layers, then the line
     * bands, which also say which rows each layer shows. */
    nnb = vdp_bands(v->nt_log, v->nt_n, v->reg[2], r->ntops, r->nvals);
    assign_layers(r, v, nnb);
    nb = vdp_bands(v->hs_log, v->hs_n, v->reg[8], r->tops, r->hss);
    ndb = vdp_bands(v->de_log, v->de_n, v->de_start, r->dtops, r->dons);
    nbl = line_bands(r, v, nb, nnb, ndb, &any_on);
    if (!any_on) {
        r->display_on = 0;
        return;
    }
    /* The sprites the hardware shows on each line, and the flags it sets
     * while drawing them. */
    k = vdp_sprites(v);
    if (k & VDP_ST_OVERFLOW)
        r->st.overflows++;
    if (k & VDP_ST_COLLIDE)
        r->st.collisions++;

    /* Each cell is drawn at most once per update: the stamp of the
     * update marks the cells drawn (a wrap makes a cell drawn 256
     * updates ago look drawn now, which only costs a redraw). */
    for (m = 0; m < RENDER_LAYERS; m++) {
        render_layer *l = &r->layer[m];

        l->stamp_now = (l->stamp_now + 1) & 0xFF;
        if (l->stamp_now == 0)
            memset(l->stamp, 0xFF, (size_t)l->cap_cells);
    }

    /* Written tiles: forget their conversions and note the name table
     * chunks among them, for each layer; with them, the work the
     * catch-up took in spare time and left unfinished (a tile whose cell
     * list it was walking is drawn again whole). */
    nd = render_dirty_scan(v->dirty, r->dlist);
    for (m = 0; m < RENDER_LAYERS; m++) {
        render_layer *l = &r->layer[m];

        if (l->pend_tile != RENDER_NO_TILE) {
            l->tile_pend[l->pend_tile >> 5] |= (uint32)1 << (l->pend_tile & 31);
            l->pend_tile = RENDER_NO_TILE;
        }
        nt_mask[m][0] = nt_mask[m][1] = 0;
    }
    c0 = r->layer[0].nt_base >> 5;      /* first chunk and chunks of each table */
    n0 = r->layer[0].nrows * 2;
    c1 = r->layer[1].nt_base >> 5;
    n1 = r->layer[1].nrows * 2;
    for (k = 0; k < nd; k++) {
        uint32 j = r->dlist[k];
        uint32 d;

        r->t_ok[j] = 0;
        v->spr_tile_ok[j] = 0;
        d = j - c0;
        if (d < n0)
            nt_mask[0][d >> 5] |= (uint32)1 << (d & 31);
        d = j - c1;
        if (d < n1)
            nt_mask[1][d >> 5] |= (uint32)1 << (d & 31);
    }

    /* Every layer in use follows the VDP: a layer that took a new table
     * is drawn whole, the others draw their changed entries first (with
     * the written tiles converted again), then the stale cells of the
     * rows shown, then the other cells of the written tiles in the rows
     * shown (the rest is left stale). A layer with no table is left
     * alone. */
    for (m = 0; m < RENDER_LAYERS; m++) {
        render_layer *l = &r->layer[m];
        uint32 used = 0;

        for (k = 0; k < nnb; k++) {
            if (r->nlayer[k] == l)
                used = 1;
        }
        if (!used && !l->valid)
            continue;
        if (!l->valid) {
            if (l->rb_next == RENDER_NO_TILE) {
                rebuild(r, l, vram);
                continue;
            }
            /* A rebuild begun ahead: the rest of its cells from the
             * table as it stands, then the entries written since the
             * drawn cells took their shadow, and the tiles written
             * since, which the catch-up left pending. */
            rebuild_refresh(l, vram);
            render_rebuild_cells_part(l, vram, l->rb_next, l->ncells);
            l->valid = 1;
            l->rb_next = RENDER_NO_TILE;
            r->st.rebuilds++;
        }
        nt_mask[m][0] |= l->nt_pend[0];
        nt_mask[m][1] |= l->nt_pend[1];
        l->nt_pend[0] = l->nt_pend[1] = 0;
        if ((nt_mask[m][0] | nt_mask[m][1]) != 0)
            update_entries(l, nt_mask[m], vram);
        if ((l->stale_rows & l->shown_rows) != 0)
            render_stale_cells(l, vram);
        for (k = 0; k < nd; k++) {
            uint32 j = r->dlist[k];

            if (l->head[j] != NONE)
                render_tile_cells(l, vram, j);
        }
        for (k = 0; k < VDP_TILES / 32; k++) {
            uint32 w = l->tile_pend[k];

            l->tile_pend[k] = 0;
            for (; w != 0; w &= w - 1) {
                uint32 j = k * 32 + lowest_bit(w);

                if (l->head[j] != NONE)
                    render_tile_cells(l, vram, j);
            }
        }
    }
    for (m = 0; m < RENDER_LAYERS; m++) {
        render_layer *l = &r->layer[m];

        if (l->valid)
            prio_groups(l);
        else
            l->pg_n = 0;
        r->st.cells += l->cells;
        r->st.tile_cells += l->tile_cells;
        r->st.tiles += l->tiles_conv;
        l->cells = 0;
        l->tile_cells = 0;
        l->tiles_conv = 0;
    }

    np = update_pieces(r, v, &npr, &npr0);
    ns = update_sprites(r, v);
    r->n_sprites = ns;
    r->st.tiles += r->layer[0].tiles_conv;
    r->layer[0].tiles_conv = 0;

    /* Chain: pieces, sprites, priority strips, backdrop rectangles of
     * the lines with the display off, the blanked column (which hides
     * sprites too), end. */
    last = &r->cels[np - 1];
    if (ns) {
        last->ccb_NextPtr = &r->cels[CEL_SPRITE0 + ns - 1];
        last = &r->cels[CEL_SPRITE0];
    }
    if (npr) {
        last->ccb_NextPtr = &r->cels[npr0];
        last = &r->cels[npr0 + npr - 1];
    }
    if (nbl) {
        last->ccb_NextPtr = &r->cels[CEL_DBLANK0];
        last = &r->cels[CEL_DBLANK0 + nbl - 1];
    }
    if (v->reg[0] & 0x20) {
        last->ccb_NextPtr = &r->cels[CEL_BLANK];
        last = &r->cels[CEL_BLANK];
    }
    last->ccb_NextPtr = NULL;
    r->last_cel->ccb_Flags &= ~CCB_LAST;
    last->ccb_Flags |= CCB_LAST;
    r->last_cel = last;
    r->chain = &r->cels[0];
}

/*--------------------------------------------------------------------------
 * Catch-up in spare time
 *------------------------------------------------------------------------*/

#define CATCH_BUDGET    16      /* cells drawn between two looks at the clock (a
                                   look costs two system time samples on the console) */

#define past(now, deadline) ((int32)((now)() - (deadline)) >= 0)

/* When no layer holds the name table the VDP points at, a layer gets it
 * and its rebuild starts (the cells are drawn by the catch-up steps): a
 * layer with no table, else the least recently used of those the last
 * picture did not use; a layer being rebuilt for another table is left
 * to finish. */
static void assign_ahead(renderer *r, const vdp_state *v)
{
    uint32 base = vdp_nt_base(v, v->reg[2]);
    render_layer *best = NULL;
    uint32 k;

    if (layer_of(r, base) != NULL)
        return;
    for (k = 0; k < r->nlayers; k++) {
        render_layer *c = &r->layer[k];

        if (c->rb_next != RENDER_NO_TILE)
            continue;
        if (!c->valid) {
            best = c;
            break;
        }
        if (c->last_used < r->update && (best == NULL || c->last_used < best->last_used))
            best = c;
    }
    if (best == NULL)
        return;
    best->nt_base = base;
    best->valid = 0;
    best->last_used = r->update;
    rebuild_start(best, v->vram);
    best->rb_next = 0;
}

/* The layer's counters of the work done, into the statistics. */
static void count_ahead(renderer *r, render_layer *l)
{
    r->st.cells += l->cells;
    r->st.ahead_cells += l->cells;
    r->st.tile_cells += l->tile_cells;
    r->st.tiles += l->tiles_conv;
    r->st.ahead_tiles += l->tiles_conv;
    l->cells = 0;
    l->tile_cells = 0;
    l->tiles_conv = 0;
}

uint32 render_catch_up(renderer *r, vdp_state *v, uint32 (*now)(void), uint32 deadline_us)
{
    const uint8 *vram = v->vram;
    uint32 t0 = now();
    uint32 pending = 0;
    uint32 cut = 0;
    uint32 nd;
    uint32 k;
    uint32 m;

    if ((int32)(t0 - deadline_us) >= 0)
        return 1;
    set_mode(r, v->active, 0);

    /* The written tiles become pending work of every layer holding a
     * table: their conversions are forgotten, their cells are to be
     * redrawn, and the name table chunks among them are to be compared
     * (once the rebuild of a layer begun ahead is done, for that layer).
     * A tile written again while its list was being walked starts over.
     * A layer given a table below takes its shadow now, so the pending
     * work of a fresh rebuild is dropped with it. */
    nd = render_dirty_scan(v->dirty, r->dlist);
    for (k = 0; k < nd; k++) {
        uint32 j = r->dlist[k];

        r->t_ok[j] = 0;
        v->spr_tile_ok[j] = 0;
        for (m = 0; m < RENDER_LAYERS; m++) {
            render_layer *l = &r->layer[m];
            uint32 d;

            if (!layer_holds(l, l->nt_base))
                continue;
            l->tile_pend[j >> 5] |= (uint32)1 << (j & 31);
            if (j == l->pend_tile)
                l->pend_tile = RENDER_NO_TILE;
            d = j - (l->nt_base >> 5);
            if (d < l->nrows * 2)
                l->nt_pend[d >> 5] |= (uint32)1 << (d & 31);
        }
    }
    assign_ahead(r, v);
    for (m = 0; m < RENDER_LAYERS; m++) {
        render_layer *l = &r->layer[m];

        pending |= l->nt_pend[0] | l->nt_pend[1] | (l->pend_tile != RENDER_NO_TILE) |
                   (l->rb_next != RENDER_NO_TILE);
        for (k = 0; k < VDP_TILES / 32; k++)
            pending |= l->tile_pend[k];
    }
    if (!pending)
        return 0;
    r->st.ahead_calls++;
    for (m = 0; m < RENDER_LAYERS; m++) {
        render_layer *l = &r->layer[m];

        l->stamp_now = (l->stamp_now + 1) & 0xFF;
        if (l->stamp_now == 0)
            memset(l->stamp, 0xFF, (size_t)l->cap_cells);
        /* The rows of the last picture decide what is drawn first and
         * which cells of the written tiles are left stale; before the
         * first picture every row counts (the next update sets them
         * again from its bands). */
        if (l->shown_rows == 0 && l->nrows != 0)
            l->shown_rows = (l->nrows >= 32) ? 0xFFFFFFFFu : (((uint32)1 << l->nrows) - 1);
    }

    /* The changed entries of the pending chunks first (an entry decides
     * which tile a cell shows: a cell drawn for a written tile before
     * its entry changes is drawn for nothing), a chunk at a time, the
     * chunks of the rows the last picture showed first. */
    for (m = 0; m < RENDER_LAYERS; m++) {
        render_layer *l = &r->layer[m];
        uint32 nchunks = l->nrows * 2;
        uint32 pass;

        if (!l->valid || (l->nt_pend[0] | l->nt_pend[1]) == 0)
            continue;
        for (pass = 0; pass < 2; pass++) {
            uint32 c;

            for (c = 0; c < nchunks; c++) {
                uint32 one[2];
                uint32 n;

                if (!((l->nt_pend[c >> 5] >> (c & 31)) & 1))
                    continue;
                if ((((l->shown_rows >> (c >> 1)) & 1) != 0) != (pass == 0))
                    continue;
                one[0] = one[1] = 0;
                one[c >> 5] = (uint32)1 << (c & 31);
                n = render_nt_scan((const uint32 *)(vram + l->nt_base),
                                   (const uint32 *)l->shadow, one, l->wlist, nchunks);
                if (n != 0)
                    render_entries(l, vram, n);
                l->nt_pend[c >> 5] &= ~((uint32)1 << (c & 31));
                if (past(now, deadline_us)) {
                    cut = 1;
                    goto out;
                }
            }
        }
    }

    /* The list walk left unfinished by the previous call, when its cell
     * still belongs to the tile (an entry change since then moved the
     * cell to another list, which cannot be followed: the tile is then
     * drawn again whole; the cells the entries moved in the meantime
     * were drawn with their new entry). */
    for (m = 0; m < RENDER_LAYERS; m++) {
        render_layer *l = &r->layer[m];
        uint32 t = l->pend_tile;

        if (t == RENDER_NO_TILE)
            continue;
        if ((((uint32)l->shadow[2 * l->pend_cell + 1] & 1) << 8 |
             l->shadow[2 * l->pend_cell]) != t) {
            l->tile_pend[t >> 5] |= (uint32)1 << (t & 31);
            l->pend_tile = RENDER_NO_TILE;
            continue;
        }
        for (;;) {
            uint32 next = render_tile_cells_part(l, vram, t, l->pend_cell, CATCH_BUDGET);

            if (next == NONE) {
                l->pend_tile = RENDER_NO_TILE;
                break;
            }
            l->pend_cell = next;
            if (past(now, deadline_us)) {
                cut = 1;
                goto out;
            }
        }
    }

    /* The cells of a rebuild in progress, from the table as it stands. */
    for (m = 0; m < RENDER_LAYERS; m++) {
        render_layer *l = &r->layer[m];

        if (l->rb_next != RENDER_NO_TILE)
            rebuild_refresh(l, vram);
        while (l->rb_next != RENDER_NO_TILE) {
            l->rb_next = render_rebuild_cells_part(l, vram, l->rb_next, CATCH_BUDGET);
            if (l->rb_next >= l->ncells) {
                l->valid = 1;
                l->rb_next = RENDER_NO_TILE;
                r->st.rebuilds++;
            } else if (past(now, deadline_us)) {
                cut = 1;
                goto out;
            }
        }
    }

    /* The cells of the pending tiles of each layer holding a table (a
     * layer being rebuilt has linked the cells drawn so far: the ones
     * to redraw), in the rows the last picture showed; the others are
     * left stale, as the update leaves them. */
    for (m = 0; m < RENDER_LAYERS; m++) {
        render_layer *l = &r->layer[m];

        if (!layer_holds(l, l->nt_base))
            continue;
        for (k = 0; k < VDP_TILES / 32; k++) {
            uint32 w;

            while ((w = l->tile_pend[k]) != 0) {
                uint32 j = k * 32 + lowest_bit(w);
                uint32 from = NONE;

                l->tile_pend[k] = w & (w - 1);
                if (l->head[j] == NONE)
                    continue;
                for (;;) {
                    uint32 next = render_tile_cells_part(l, vram, j, from, CATCH_BUDGET);

                    if (next == NONE)
                        break;
                    from = next;
                    if (past(now, deadline_us)) {
                        /* The rest of this list is the cursor's. */
                        l->pend_tile = j;
                        l->pend_cell = next;
                        cut = 1;
                        goto out;
                    }
                }
            }
        }
    }
out:
    for (m = 0; m < RENDER_LAYERS; m++)
        count_ahead(r, &r->layer[m]);
    r->st.ahead_cut += cut;
    r->st.ahead_us += now() - t0;
    return cut;
}
