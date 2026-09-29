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
#include "mem.h"

#define BM_PITCH        RENDER_W            /* bytes per bitmap row */
#define BM_WORDS        (RENDER_W / 4)      /* words per bitmap row */
#define TILE_WORDS      16                  /* 8 rows x 2 words */
#define NT_CHUNKS       (RENDER_NT_BYTES / 32)
#define NT_WORDS        (RENDER_NT_BYTES / 4)
#define WLIST_WORDS     VDP_TILES           /* the largest work list */
#define NONE            RENDER_NT_CELLS     /* end of a cell list */

#define CEL_PRIO0       RENDER_MAX_PIECES
#define CEL_DBLANK0     (CEL_PRIO0 + RENDER_MAX_PRIO)
#define CEL_BLANK       (CEL_DBLANK0 + RENDER_MAX_DBLANK)
#define CEL_TERM        (CEL_BLANK + 1)
#define CEL_SPRITE0     (CEL_BLANK + 2)
#define N_CELS          (CEL_SPRITE0 + RENDER_SPRITES)

#define CEL_MEM         (MEMTYPE_DRAM | MEMTYPE_CEL | MEMTYPE_FILL)
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

void render_free(renderer *r)
{
    if (r->xtab != NULL) FreeMem(r->xtab, 1024 * 4);
    if (r->blank != NULL) FreeMem(r->blank, 16);
    if (r->pluts != NULL) FreeMem(r->pluts, 96 * 2);
    if (r->cels != NULL) FreeMem(r->cels, N_CELS * sizeof(CCB));
    if (r->wlist != NULL) FreeMem(r->wlist, WLIST_WORDS * 4);
    if (r->prev != NULL) FreeMem(r->prev, RENDER_NT_CELLS * 2);
    if (r->next != NULL) FreeMem(r->next, RENDER_NT_CELLS * 2);
    if (r->head != NULL) FreeMem(r->head, VDP_TILES * 2);
    if (r->t_ok != NULL) FreeMem(r->t_ok, VDP_TILES);
    if (r->shadow != NULL) FreeMem(r->shadow, RENDER_NT_BYTES);
    if (r->tiles_f != NULL) FreeMem(r->tiles_f, VDP_TILES * TILE_WORDS * 4);
    if (r->tiles != NULL) FreeMem(r->tiles, VDP_TILES * TILE_WORDS * 4);
    if (r->prio_bm != NULL) FreeMem(r->prio_bm, RENDER_W * RENDER_BM_H);
    if (r->bitmap != NULL) FreeMem(r->bitmap, RENDER_W * RENDER_BM_H);
    memset(r, 0, sizeof(*r));
}

Err render_init(renderer *r)
{
    CCB *c;
    uint32 k;

    memset(r, 0, sizeof(*r));
    r->bitmap = (uint8 *)AllocMem(RENDER_W * RENDER_BM_H, CEL_MEM);
    r->prio_bm = (uint8 *)AllocMem(RENDER_W * RENDER_BM_H, CEL_MEM);
    r->tiles = (uint32 *)AllocMem(VDP_TILES * TILE_WORDS * 4, CEL_MEM);
    r->tiles_f = (uint32 *)AllocMem(VDP_TILES * TILE_WORDS * 4, CEL_MEM);
    r->shadow = (uint8 *)AllocMem(RENDER_NT_BYTES, PLAIN_MEM);
    r->t_ok = (uint8 *)AllocMem(VDP_TILES, PLAIN_MEM);
    r->head = (uint16 *)AllocMem(VDP_TILES * 2, PLAIN_MEM);
    r->next = (uint16 *)AllocMem(RENDER_NT_CELLS * 2, PLAIN_MEM);
    r->prev = (uint16 *)AllocMem(RENDER_NT_CELLS * 2, PLAIN_MEM);
    r->wlist = (uint32 *)AllocMem(WLIST_WORDS * 4, PLAIN_MEM);
    r->cels = (CCB *)AllocMem(N_CELS * sizeof(CCB), CEL_MEM);
    r->pluts = (uint16 *)AllocMem(96 * 2, CEL_MEM);
    r->blank = (uint32 *)AllocMem(16, CEL_MEM);
    r->xtab = (uint32 *)AllocMem(1024 * 4, PLAIN_MEM);
    if (r->bitmap == NULL || r->prio_bm == NULL || r->tiles == NULL || r->tiles_f == NULL || r->shadow == NULL ||
        r->t_ok == NULL || r->head == NULL || r->next == NULL || r->prev == NULL ||
        r->wlist == NULL || r->cels == NULL || r->pluts == NULL ||
        r->blank == NULL || r->xtab == NULL) {
        printf("ERROR: renderer out of memory\n");
        render_free(r);
        return -1;
    }
    init_tables(r->xtab);

    /* Background pieces: opaque, the background PLUT. */
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
    c = &r->cels[CEL_BLANK];
    c->ccb_VDY = RENDER_H << 16;
    for (k = 0; k < RENDER_MAX_DBLANK; k++)
        r->cels[CEL_DBLANK0 + k].ccb_HDX = (RENDER_W / 8) << 20;
    /* Terminator: the list before the first update (skipped, last). The
     * chain built by an update ends with CCB_LAST on its real last cel. */
    c = &r->cels[CEL_TERM];
    init_cel(c, CCB_SKIP | CCB_LAST | CCB_NPABS | CCB_SPABS | CCB_PPABS);
    /* Sprites: colour 0 transparent, drawn from the last to the first. */
    for (k = 0; k < RENDER_SPRITES; k++) {
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
    r->valid = 0;
    r->nt_base = 0xFFFFFFFFu;
    r->spr_tall = 0xFFFFFFFFu;
    r->n_sprites = 0;
    r->backdrop = 0xFFFFFFFFu;
    r->display_on = 0;
    r->chain = &r->cels[CEL_TERM];
    r->last_cel = &r->cels[CEL_TERM];
    memset(r->t_ok, 0, VDP_TILES);
    memset(r->prio_rows, 0, sizeof(r->prio_rows));
    memset(&r->st, 0, sizeof(r->st));
}

/*--------------------------------------------------------------------------
 * Tiles and cells
 *------------------------------------------------------------------------*/

/* Converts tile t (flip: mirrored horizontally) from the VDP's four bit
 * planes to 8 bpp rows. */
static void tile_conv(renderer *r, uint32 t, uint32 flip, const uint8 *vram)
{
    const uint8 *p = vram + t * 32;
    const uint32 *hi = r->xtab + (flip ? XT_FHI : XT_HI);
    const uint32 *lo = hi + 256;
    uint32 *d = (flip ? r->tiles_f : r->tiles) + t * TILE_WORDS;
    uint32 k;

    for (k = 0; k < 8; k++) {
        uint32 p0 = p[0];
        uint32 p1 = p[1];
        uint32 p2 = p[2];
        uint32 p3 = p[3];

        d[0] = hi[p0] | (hi[p1] << 1) | (hi[p2] << 2) | (hi[p3] << 3);
        d[1] = lo[p0] | (lo[p1] << 1) | (lo[p2] << 2) | (lo[p3] << 3);
        p += 4;
        d += 2;
    }
    r->t_ok[t] |= (uint8)(1 << flip);
    r->st.tiles++;
}

/* Draws name table entry e into cell i of the bitmap, and into the
 * priority bitmap when the entry has the priority bit. */
static void draw_cell(renderer *r, uint32 i, uint32 e, const uint8 *vram)
{
    uint32 t = e & 0x1FF;
    uint32 flip = (e >> 9) & 1;
    uint32 pal = (e & 0x800) ? 0x10101010u : 0;
    uint32 off = (i >> 5) * (8 * BM_PITCH) + (i & 31) * 8;
    const uint32 *s0;
    const uint32 *s;
    uint32 *d = (uint32 *)(r->bitmap + off);
    int32 step = 2;
    uint32 k;

    if (!(r->t_ok[t] & (1 << flip)))
        tile_conv(r, t, flip, vram);
    s0 = (flip ? r->tiles_f : r->tiles) + t * TILE_WORDS;
    if (e & 0x400) {
        s0 += 14;
        step = -2;
    }
    s = s0;
    for (k = 0; k < 8; k++) {
        d[0] = s[0] | pal;
        d[1] = s[1] | pal;
        d += BM_WORDS;
        s += step;
    }
    if (e & 0x1000) {
        d = (uint32 *)(r->prio_bm + off);
        s = s0;
        for (k = 0; k < 8; k++) {
            d[0] = s[0] | pal;
            d[1] = s[1] | pal;
            d += BM_WORDS;
            s += step;
        }
    }
    r->st.cells++;
}

/* Cell i no longer has the priority bit. */
static void clear_prio_cell(renderer *r, uint32 i)
{
    uint32 *d = (uint32 *)(r->prio_bm + (i >> 5) * (8 * BM_PITCH) + (i & 31) * 8);
    uint32 k;

    for (k = 0; k < 8; k++) {
        d[0] = 0;
        d[1] = 0;
        d += BM_WORDS;
    }
}

/* Cell lists per tile. */
static void cell_link(renderer *r, uint32 i, uint32 t)
{
    uint32 h = r->head[t];

    r->prev[i] = (uint16)NONE;
    r->next[i] = (uint16)h;
    if (h != NONE)
        r->prev[h] = (uint16)i;
    r->head[t] = (uint16)i;
}

static void cell_unlink(renderer *r, uint32 i, uint32 t)
{
    uint32 p = r->prev[i];
    uint32 n = r->next[i];

    if (p == NONE)
        r->head[t] = (uint16)n;
    else
        r->next[p] = (uint16)n;
    if (n != NONE)
        r->prev[n] = (uint16)p;
}

/* Name table entry k of a raw copy. */
#define NT_ENTRY(p, k) ((uint32)(p)[2 * (k)] | ((uint32)(p)[2 * (k) + 1] << 8))

static void rebuild(renderer *r, const uint8 *vram)
{
    const uint8 *nt = vram + r->nt_base;
    uint32 i;

    memcpy(r->shadow, nt, RENDER_NT_BYTES);
    memset(r->prio_bm, 0, RENDER_W * RENDER_BM_H);
    memset(r->prio_rows, 0, sizeof(r->prio_rows));
    memset(r->prio_dirty, 1, sizeof(r->prio_dirty));
    for (i = 0; i < VDP_TILES; i++)
        r->head[i] = (uint16)NONE;
    for (i = 0; i < RENDER_NT_CELLS; i++) {
        uint32 e = NT_ENTRY(nt, i);

        cell_link(r, i, e & 0x1FF);
        if (e & 0x1000)
            r->prio_rows[i >> 5]++;
        draw_cell(r, i, e, vram);
    }
    r->valid = 1;
    r->st.rebuilds++;
}

/* Redraws the cells of a tile that was written. */
static void redraw_tile_cells(renderer *r, uint32 t, const uint8 *vram)
{
    const uint8 *sh = r->shadow;
    uint32 i;

    for (i = r->head[t]; i != NONE; i = r->next[i]) {
        draw_cell(r, i, NT_ENTRY(sh, i), vram);
        r->st.tile_cells++;
    }
}

/* Entry change at cell i. */
static void cell_change(renderer *r, uint32 i, uint32 eo, uint32 en, const uint8 *vram)
{
    if ((eo ^ en) & 0x1FF) {
        cell_unlink(r, i, eo & 0x1FF);
        cell_link(r, i, en & 0x1FF);
    }
    if ((eo ^ en) & 0x1000) {
        uint32 row = i >> 5;

        if (en & 0x1000) {
            r->prio_rows[row]++;
        } else {
            r->prio_rows[row]--;
            clear_prio_cell(r, i);
        }
        r->prio_dirty[row] = 1;
    }
    draw_cell(r, i, en, vram);
}

/* Name table words of the dirty chunks that differ from the shadow. */
static void update_entries(renderer *r, const uint32 *nt_mask, const uint8 *vram)
{
    const uint32 *a = (const uint32 *)(vram + r->nt_base);
    uint32 *s = (uint32 *)r->shadow;
    uint32 n = render_nt_scan(a, s, nt_mask, r->wlist);
    uint32 k;

    for (k = 0; k < n; k++) {
        uint32 w = r->wlist[k];
        uint32 nv = a[w];
        uint32 ov = s[w];
        uint32 en;
        uint32 eo;

        s[w] = nv;
        en = (nv >> 24) | ((nv >> 8) & 0xFF00);
        eo = (ov >> 24) | ((ov >> 8) & 0xFF00);
        if (en != eo)
            cell_change(r, w * 2, eo, en, vram);
        en = ((nv >> 8) & 0xFF) | ((nv & 0xFF) << 8);
        eo = ((ov >> 8) & 0xFF) | ((ov & 0xFF) << 8);
        if (en != eo)
            cell_change(r, w * 2 + 1, eo, en, vram);
    }
}

/*--------------------------------------------------------------------------
 * Palettes
 *------------------------------------------------------------------------*/

static void update_palettes(renderer *r, vdp_state *v)
{
    uint16 *bg = r->pluts;
    uint16 *sp = r->pluts + 32;
    uint16 *pr = r->pluts + 64;
    uint32 k;

    if (v->cram_dirty) {
        v->cram_dirty = 0;
        for (k = 0; k < 32; k++)
            bg[k] = (uint16)sms_rgb(v->cram[k]);
        /* Sprites: colour 0 transparent (000), black elsewhere made
         * opaque (001). */
        sp[0] = 0;
        for (k = 1; k < 16; k++) {
            uint32 c = bg[16 + k];

            sp[k] = (uint16)(c ? c : 1);
        }
        for (k = 16; k < 32; k++)
            sp[k] = sp[k - 16];
        /* Priority strips: colour 0 of both palettes transparent, black
         * elsewhere opaque. The cel engine tests the colour the PLUT
         * gives, not the index. */
        for (k = 0; k < 32; k++) {
            uint32 c = bg[k];

            pr[k] = (uint16)((k & 15) == 0 ? 0 : (c ? c : 1));
        }
        r->st.palettes++;
    }
    k = bg[16 + (v->reg[7] & 15)];
    if (k != r->backdrop) {
        r->backdrop = k;
        r->st.backdrops++;
        k |= k << 16;
        r->blank[0] = r->blank[1] = r->blank[2] = r->blank[3] = k;
    }
}

/*--------------------------------------------------------------------------
 * Background pieces
 *------------------------------------------------------------------------*/

/* One piece: screen columns sa to sb - 1 of h lines from screen line sy,
 * showing rows from src_y of bitmap bm with horizontal scroll hs, in cel
 * c (unless c is end: the piece is dropped); returns the next cel. The
 * source starts on a word boundary: the piece may begin up to 3 pixels
 * left of sa, where it is either clipped or covered by the piece drawn
 * after (or shows the right pixels of the transparent priority layer). */
static CCB *piece(renderer *r, CCB *c, CCB *end, const uint8 *bm, uint32 sy, uint32 h,
                  uint32 src_y, uint32 hs, uint32 sa, uint32 sb)
{
    uint32 c0 = (sa - hs) & 255;
    uint32 ca = c0 & ~3u;
    uint32 w = sb - sa + (c0 - ca);

    if (c == end) {
        r->st.dropped++;
        return c;
    }
    c->ccb_SourcePtr = (CelData *)(bm + src_y * BM_PITCH + ca);
    c->ccb_XPos = (Coord)((int32)(sa - (c0 - ca)) << 16);
    c->ccb_YPos = (Coord)(sy << 16);
    c->ccb_PRE0 = ((h - PRE0_VCNT_PREFETCH) << PRE0_VCNT_SHIFT) | PRE0_BPP_8;
    c->ccb_PRE1 = ((BM_WORDS - PRE1_WOFFSET_PREFETCH) << PRE1_WOFFSET10_SHIFT) |
                  ((w - PRE1_TLHPCNT_PREFETCH) << PRE1_TLHPCNT_SHIFT) | PRE1_TLLSB_PDC0;
    return c + 1;
}

/* Background pieces: cels 0 to RENDER_MAX_PIECES - 1 of the main bitmap. */
#define BG_PIECE(r, np, sy, h, src_y, hs, sa, sb) \
    (*(np) = (uint32)(piece((r), &(r)->cels[*(np)], &(r)->cels[RENDER_MAX_PIECES], \
                            (r)->bitmap, (sy), (h), (src_y), (hs), (sa), (sb)) - (r)->cels))

/* Screen columns xa to xb - 1: split where the source wraps, the right
 * part first so that the left part covers its overdraw. */
static void hparts(renderer *r, uint32 *np, uint32 sy, uint32 h, uint32 src_y,
                   uint32 hs, uint32 xa, uint32 xb)
{
    uint32 xw = hs & 255;

    if (xw > xa && xw < xb) {
        BG_PIECE(r, np, sy, h, src_y, hs, xw, xb);
        BG_PIECE(r, np, sy, h, src_y, hs, xa, xw);
    } else {
        BG_PIECE(r, np, sy, h, src_y, hs, xa, xb);
    }
}

/* Screen lines ya to yb - 1, columns xa to xb - 1, with vertical scroll
 * vs: split where the source wraps. */
static void vparts(renderer *r, uint32 *np, uint32 ya, uint32 yb, uint32 hs,
                   uint32 xa, uint32 xb, uint32 vs)
{
    uint32 y0 = ya + vs;
    uint32 n = yb - ya;
    uint32 n1;

    while (y0 >= RENDER_BM_H)
        y0 -= RENDER_BM_H;
    n1 = RENDER_BM_H - y0;
    if (n1 > n)
        n1 = n;
    hparts(r, np, ya, n1, y0, hs, xa, xb);
    if (n1 < n)
        hparts(r, np, ya + n1, n - n1, 0, hs, xa, xb);
}

/* A band of lines with one horizontal scroll value. With the vertical
 * scroll inhibit, the right 64 columns come first (unscrolled) so that
 * the left part covers their overdraw. */
static void band(renderer *r, uint32 *np, const vdp_state *v, uint32 ya, uint32 yb, uint32 hs)
{
    uint32 vs = v->reg[9];

    if (v->reg[0] & 0x80) {
        vparts(r, np, ya, yb, hs, 192, RENDER_W, 0);
        vparts(r, np, ya, yb, hs, 0, 192, vs);
    } else {
        vparts(r, np, ya, yb, hs, 0, RENDER_W, vs);
    }
}

/*
 * Priority strips of nrows consecutive name table rows from row0 inside
 * a band: the bitmap columns of their priority cells, bx0 to bx1 - 1,
 * mapped to the screen with the scroll of the region (columns xa to
 * xb - 1, vertical scroll vs) and cut at the wraps; returns the next cel.
 */
static CCB *prio_region(renderer *r, CCB *c, uint32 row0, uint32 nrows, uint32 bx0, uint32 bx1,
                        uint32 ya, uint32 yb, uint32 hs, uint32 xa, uint32 xb, uint32 vs)
{
    CCB *end = &r->cels[CEL_BLANK];
    int32 y0 = (int32)(row0 * 8) - (int32)vs;      /* screen line of the first row */
    uint32 hgt = nrows * 8;
    uint32 sx0 = (bx0 + hs) & 255;
    uint32 w = bx1 - bx0;
    uint32 vp;

    while (y0 < 0)
        y0 += RENDER_BM_H;
    for (vp = 0; vp < 2; vp++) {
        uint32 iy0;
        uint32 iy1;
        uint32 src_y;
        uint32 sy;
        uint32 ey;
        uint32 hp;

        if (vp == 0) {
            iy0 = (uint32)y0;
            iy1 = ((uint32)y0 + hgt <= RENDER_BM_H) ? (uint32)y0 + hgt : RENDER_BM_H;
            src_y = row0 * 8;
        } else {
            if ((uint32)y0 + hgt <= RENDER_BM_H)
                break;
            iy0 = 0;
            iy1 = (uint32)y0 + hgt - RENDER_BM_H;
            src_y = row0 * 8 + (RENDER_BM_H - (uint32)y0);
        }
        sy = (iy0 > ya) ? iy0 : ya;
        ey = (iy1 < yb) ? iy1 : yb;
        if (sy >= ey)
            continue;
        src_y += sy - iy0;
        for (hp = 0; hp < 2; hp++) {
            uint32 ix0;
            uint32 ix1;
            uint32 sa;
            uint32 sb;

            if (hp == 0) {
                ix0 = sx0;
                ix1 = (sx0 + w <= 256) ? sx0 + w : 256;
            } else {
                if (sx0 + w <= 256)
                    break;
                ix0 = 0;
                ix1 = sx0 + w - 256;
            }
            sa = (ix0 > xa) ? ix0 : xa;
            sb = (ix1 < xb) ? ix1 : xb;
            if (sa < sb) {
                c = piece(r, c, end, r->prio_bm, sy, ey - sy, src_y, hs, sa, sb);
                if (c != end)
                    r->st.prio_strips++;
            }
        }
    }
    return c;
}

/* Priority strips for a band: consecutive rows holding priority cells in
 * the same columns make one strip. */
static CCB *prio_band(renderer *r, CCB *c, const vdp_state *v, uint32 ya, uint32 yb, uint32 hs)
{
    uint32 vs = v->reg[9];
    uint32 row = 0;

    while (row < RENDER_NT_ROWS) {
        uint32 row0;
        uint32 bx0;
        uint32 bx1;

        if (r->prio_rows[row] == 0) {
            row++;
            continue;
        }
        if (r->prio_dirty[row]) {
            const uint8 *sh = r->shadow + row * 64;
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
            r->prio_min[row] = (uint8)lo;
            r->prio_max[row] = (uint8)hi;
            r->prio_dirty[row] = 0;
        }
        row0 = row;
        bx0 = r->prio_min[row] * 8;
        bx1 = r->prio_max[row] * 8 + 8;
        for (row++; row < RENDER_NT_ROWS; row++) {
            if (r->prio_rows[row] == 0 || r->prio_dirty[row] ||
                r->prio_min[row] * 8 != bx0 || r->prio_max[row] * 8 + 8 != bx1)
                break;
        }
        if (v->reg[0] & 0x80) {
            c = prio_region(r, c, row0, row - row0, bx0, bx1, ya, yb, hs, 192, RENDER_W, 0);
            c = prio_region(r, c, row0, row - row0, bx0, bx1, ya, yb, hs, 0, 192, vs);
        } else {
            c = prio_region(r, c, row0, row - row0, bx0, bx1, ya, yb, hs, 0, RENDER_W, vs);
        }
    }
    return c;
}

/* Background pieces and priority strips of the lines ya to yb - 1 with
 * horizontal scroll hs. Rows 0-1 are not scrolled horizontally when
 * register 0 bit 6 is set. */
static CCB *lines_pieces(renderer *r, uint32 *np, CCB *pc, const vdp_state *v,
                         uint32 ya, uint32 yb, uint32 hs)
{
    if ((v->reg[0] & 0x40) && ya < 16) {
        band(r, np, v, ya, yb < 16 ? yb : 16, 0);
        pc = prio_band(r, pc, v, ya, yb < 16 ? yb : 16, 0);
        if (yb > 16) {
            band(r, np, v, 16, yb, hs);
            pc = prio_band(r, pc, v, 16, yb, hs);
        }
    } else {
        band(r, np, v, ya, yb, hs);
        pc = prio_band(r, pc, v, ya, yb, hs);
    }
    return pc;
}

/* Scroll bands (register 8 writes recorded during the active display)
 * crossed with the display bands (register 1 bit 6): the background is
 * drawn where the display is on, the other lines get a backdrop
 * rectangle. Returns the number of background pieces and, in *nprio and
 * *nblank, the numbers of priority strips and of backdrop rectangles;
 * *any_on tells whether any line is displayed. */
static uint32 update_pieces(renderer *r, const vdp_state *v, uint32 *nprio, uint32 *nblank,
                            uint32 *any_on)
{
    uint32 tops[VDP_HS_LOG + 1];
    uint32 hss[VDP_HS_LOG + 1];
    uint32 dtops[VDP_DE_LOG + 1];
    uint32 dons[VDP_DE_LOG + 1];
    CCB *pc = &r->cels[CEL_PRIO0];
    uint32 nb;
    uint32 ndb;
    uint32 np = 0;
    uint32 nbl = 0;
    uint32 on = 0;
    uint32 k;
    uint32 j;

    nb = vdp_bands(v->hs_log, v->hs_n, v->reg[8], tops, hss);
    ndb = vdp_bands(v->de_log, v->de_n, v->de_start, dtops, dons);
    if (nb > 1)
        r->st.bands++;
    if (ndb > 1)
        r->st.dbands++;
    for (j = 0; j < ndb; j++) {
        uint32 da = dtops[j];
        uint32 db = (j + 1 < ndb) ? dtops[j + 1] : RENDER_H;

        if (!dons[j]) {
            CCB *c = &r->cels[CEL_DBLANK0 + nbl++];

            c->ccb_YPos = (Coord)(da << 16);
            c->ccb_VDY = (int32)((db - da) << 16);
            continue;
        }
        on = 1;
        for (k = 0; k < nb; k++) {
            uint32 ya = tops[k];
            uint32 yb = (k + 1 < nb) ? tops[k + 1] : RENDER_H;

            if (ya < da)
                ya = da;
            if (yb > db)
                yb = db;
            if (ya < yb)
                pc = lines_pieces(r, &np, pc, v, ya, yb, hss[k]);
        }
    }
    for (k = 1; k < np; k++)
        r->cels[k - 1].ccb_NextPtr = &r->cels[k];
    *nprio = (uint32)(pc - &r->cels[CEL_PRIO0]);
    for (k = 1; k < *nprio; k++)
        r->cels[CEL_PRIO0 + k - 1].ccb_NextPtr = &r->cels[CEL_PRIO0 + k];
    for (k = 1; k < nbl; k++)
        r->cels[CEL_DBLANK0 + k - 1].ccb_NextPtr = &r->cels[CEL_DBLANK0 + k];
    *nblank = nbl;
    *any_on = on;
    r->st.pieces += np;
    return np;
}

/*--------------------------------------------------------------------------
 * Sprites
 *------------------------------------------------------------------------*/

static void sprite_tiles(renderer *r, uint32 t, uint32 tall, const uint8 *vram)
{
    if (!(r->t_ok[t] & 1))
        tile_conv(r, t, 0, vram);
    if (tall && !(r->t_ok[t + 1] & 1))
        tile_conv(r, t + 1, 0, vram);
}

/* Sets up the cels of the active sprites (before the $D0 terminator) and
 * converts the tiles they miss; returns the count. */
static uint32 update_sprites(renderer *r, const vdp_state *v)
{
    const uint8 *vram = v->vram;
    const uint8 *sat = vram + ((v->reg[5] & 0x7E) << 7);
    uint32 tall = (v->reg[1] & 0x02) ? 1 : 0;
    render_sprite_args a;
    uint32 n;
    uint32 nn;
    uint32 k;

    for (n = 0; n < RENDER_SPRITES; n++) {
        if (sat[n] == 0xD0)
            break;
    }
    if (tall != r->spr_tall) {
        r->spr_tall = tall;
        for (k = 0; k < RENDER_SPRITES; k++) {
            r->cels[CEL_SPRITE0 + k].ccb_PRE0 =
                (((tall ? 16 : 8) - PRE0_VCNT_PREFETCH) << PRE0_VCNT_SHIFT) | PRE0_BPP_8;
        }
    }
    a.tiles = r->tiles;
    a.t_ok = r->t_ok;
    a.tbase = (v->reg[6] & 0x04) ? 256 : 0;
    a.tmask = tall ? 0xFE : 0xFF;
    a.xoff = (v->reg[0] & 0x08) ? 8 : 0;
    a.need = r->wlist;
    a.tall = tall;
    nn = render_sprites(sat, n, &r->cels[CEL_SPRITE0], &a);
    for (k = 0; k < nn; k++)
        sprite_tiles(r, r->wlist[k], tall, vram);
    r->st.sprites += n;
    return n;
}

/*--------------------------------------------------------------------------
 * Frame update
 *------------------------------------------------------------------------*/

void render_update(renderer *r, vdp_state *v)
{
    const uint8 *vram = v->vram;
    uint32 nt_base = ((uint32)v->reg[2] & 0x0E) << 10;
    uint32 nt_first = nt_base >> 5;
    uint32 nt_mask[2];
    uint32 rebuilt = 0;
    uint32 nd;
    uint32 np;
    uint32 npr;
    uint32 nbl;
    uint32 any_on;
    uint32 ns;
    uint32 k;
    CCB *last;

    update_palettes(r, v);

    /* While the display is off nothing is drawn: the work waits, and the
     * dirty flags accumulate, until it is on for some lines. */
    r->display_on = v->de_start || v->de_n != 0;
    if (!r->display_on)
        return;

    if (nt_base != r->nt_base || !r->valid) {
        r->nt_base = nt_base;
        rebuild(r, vram);
        rebuilt = 1;
    }

    /* Written tiles: forget their conversions, redraw their cells, note
     * the name table chunks among them. */
    nt_mask[0] = nt_mask[1] = 0;
    nd = render_dirty_scan(v->dirty, r->wlist);
    for (k = 0; k < nd; k++) {
        uint32 j = r->wlist[k];
        uint32 d = j - nt_first;

        r->t_ok[j] = 0;
        if (r->head[j] != NONE && !rebuilt)
            redraw_tile_cells(r, j, vram);
        if (d < NT_CHUNKS)
            nt_mask[d >> 5] |= (uint32)1 << (d & 31);
    }
    if (!rebuilt && (nt_mask[0] | nt_mask[1]) != 0)
        update_entries(r, nt_mask, vram);

    np = update_pieces(r, v, &npr, &nbl, &any_on);
    if (!any_on) {
        r->display_on = 0;
        return;
    }
    ns = update_sprites(r, v);
    r->n_sprites = ns;

    /* Chain: pieces, sprites, priority strips, backdrop rectangles of
     * the lines with the display off, the blanked column (which hides
     * sprites too), end. */
    last = &r->cels[np - 1];
    if (ns) {
        last->ccb_NextPtr = &r->cels[CEL_SPRITE0 + ns - 1];
        last = &r->cels[CEL_SPRITE0];
    }
    if (npr) {
        last->ccb_NextPtr = &r->cels[CEL_PRIO0];
        last = &r->cels[CEL_PRIO0 + npr - 1];
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
