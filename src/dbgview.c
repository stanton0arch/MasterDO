/*
 * Debug view of the VDP picture, drawn by the CPU. See dbgview.h.
 *
 * Lines are drawn in pairs, which is what an LRFORM word holds: when both
 * lines of a pair use the same scroll values, each of the 32 background
 * columns gives eight words, one per pixel column (a pair split by a band
 * boundary is drawn line by line and merged). Sprites are then drawn
 * over the pair, from the last to the first so that sprite 0 ends on top,
 * except over background pixels of tiles that have the priority bit. On
 * each line only the first eight sprites of the table covering it are
 * drawn, as on the hardware; zoomed sprites have their pixels doubled.
 * The 224 and 240-line modes read a name table of 32 rows and have no
 * sprite list terminator; each line takes the colours of its palette
 * band (colour RAM writes during the active display).
 *
 * The tables of a drawing are allocated for its duration only: the view
 * is drawn at a pause, while the run holds its memory, and nothing of it
 * is kept in the program's static data.
 */

#include "dbgview.h"

#include "string.h"
#include "mem.h"

#define MAX_SPRITES 64

typedef struct {
    int32  x;
    uint32 y;               /* first line */
    uint32 tile;            /* first tile */
} sprite;

typedef struct {
    /* Pixel k (from the left) of a tile row plane byte, as bit 0 of
     * nibble 7 - k; mirrored for horizontally flipped tiles. */
    uint32 expand[256];
    uint32 expand_flip[256];
    /* Background pixels of priority tiles that hide sprites, for the two
     * lines of the pair being drawn. */
    uint8  prio_mask[2][DBGVIEW_WIDTH];
    /* A pair split by a band boundary: each line drawn on its own. */
    uint32 tmp[2][DBGVIEW_WIDTH];
    uint8  mask0[DBGVIEW_WIDTH];
    /* Bands of the frame. */
    uint32 tops[VDP_HS_LOG + 1];
    uint32 hss[VDP_HS_LOG + 1];
    uint32 ntops[VDP_NT_LOG + 1];
    uint32 nvals[VDP_NT_LOG + 1];
    uint32 dtops[VDP_DE_LOG + 1];
    uint32 dons[VDP_DE_LOG + 1];
    uint32 ptops[VDP_PAL_BANDS];
    uint32 pcram[VDP_PAL_BANDS * VDP_PAL_WORDS];
    uint32 row[DBGVIEW_WIDTH];
    uint32 pals[VDP_PAL_BANDS][32];
    uint32 borders[VDP_PAL_BANDS];
} work;

static work *work_alloc(void)
{
    work *w = (work *)AllocMem(sizeof(work), MEMTYPE_DRAM);
    uint32 v;
    uint32 k;

    if (w == NULL)
        w = (work *)AllocMem(sizeof(work), MEMTYPE_VRAM);
    if (w == NULL)
        return NULL;
    for (v = 0; v < 256; v++) {
        uint32 e = 0;
        uint32 f = 0;

        for (k = 0; k < 8; k++) {
            if (v & ((uint32)0x80 >> k)) {
                e |= (uint32)1 << (4 * (7 - k));
                f |= (uint32)1 << (4 * k);
            }
        }
        w->expand[v] = e;
        w->expand_flip[v] = f;
    }
    return w;
}

/* SMS colour (--BBGGRR) to 3DO RGB 5:5:5. */
static uint32 sms_rgb(uint32 c)
{
    static const uint8 level[4] = { 0, 10, 21, 31 };

    return ((uint32)level[c & 3] << 10) | ((uint32)level[(c >> 2) & 3] << 5) |
           (uint32)level[(c >> 4) & 3];
}

/* Game Gear colour (GGGGRRRR, then ----BBBB) to 3DO RGB 5:5:5, each
 * component widened from 4 to 5 bits. */
static uint32 gg_rgb(const uint8 *c)
{
    uint32 r = c[0] & 15;
    uint32 g = c[0] >> 4;
    uint32 b = c[1] & 15;

    return (((r << 1) | (r >> 3)) << 10) | (((g << 1) | (g >> 3)) << 5) | ((b << 1) | (b >> 3));
}

/* Row r of the tile of name table entry e, pixel k in nibble 7 - k. */
static uint32 tile_row(const work *w, const uint8 *vram, uint32 e, uint32 r)
{
    const uint8 *p;
    const uint32 *ex = (e & 0x200) ? w->expand_flip : w->expand;

    if (e & 0x400)
        r = 7 - r;
    p = vram + (e & 0x1FF) * 32 + r * 4;
    return ex[p[0]] | (ex[p[1]] << 1) | (ex[p[2]] << 2) | (ex[p[3]] << 3);
}

/* Sprites before the $D0 terminator (192-line mode), in drawing order
 * (last first). */
static uint32 sprite_list(const vdp_state *v, sprite *list)
{
    const uint8 *sat = v->vram + ((v->reg[5] & 0x7E) << 7);
    uint32 tbase = (v->reg[6] & 0x04) ? 256 : 0;
    uint32 tmask = (v->reg[1] & 0x02) ? 0xFE : 0xFF;
    int32 xoff = (v->reg[0] & 0x08) ? 8 : 0;
    uint32 n;
    uint32 i;

    for (n = 0; n < MAX_SPRITES; n++) {
        if (sat[n] == 0xD0 && v->active == VDP_ACTIVE_STD)
            break;
    }
    for (i = 0; i < n; i++) {
        uint32 s = n - 1 - i;

        list[i].y = (uint32)sat[s] + 1;
        list[i].x = (int32)sat[0x80 + 2 * s] - xoff;
        list[i].tile = tbase + ((uint32)sat[0x81 + 2 * s] & tmask);
    }
    return n;
}

/* Name table entry at row ty, column col. */
static uint32 nt_entry(const uint8 *nt, uint32 ty, uint32 col)
{
    const uint8 *e = nt + (ty * 32 + col) * 2;

    return e[0] | ((uint32)e[1] << 8);
}

/* Background of lines y and y + 1 into the words of their pair; returns
 * nonzero when a priority tile filled prio_mask. */
static uint32 draw_background(work *w, const vdp_state *v, const uint32 *pal, uint32 y,
                              uint32 hs_y, uint32 nt_reg, uint32 *d)
{
    const uint8 *vram = v->vram;
    const uint8 *nt = vram + vdp_nt_base(v, nt_reg);
    uint32 bh = v->nt_rows * 8;
    uint32 hs = (y < 16 && (v->reg[0] & 0x40)) ? 0 : hs_y;
    uint32 vsi = (uint32)v->reg[0] & 0x80;
    uint32 prio = 0;
    uint32 col;

    for (col = 0; col < 32; col++) {
        uint32 sx = (col * 8 + hs) & 255;
        uint32 ya = y + ((vsi && sx >= 192) ? 0 : v->reg[9]);
        uint32 yb;
        uint32 ea;
        uint32 eb;
        uint32 pa;
        uint32 pb;
        const uint32 *pala;
        const uint32 *palb;
        uint32 k;

        while (ya >= bh)
            ya -= bh;
        yb = (ya == bh - 1) ? 0 : ya + 1;
        ea = nt_entry(nt, ya >> 3, col);
        eb = ((yb >> 3) == (ya >> 3)) ? ea : nt_entry(nt, yb >> 3, col);
        pa = tile_row(w, vram, ea, ya & 7);
        pb = tile_row(w, vram, eb, yb & 7);
        pala = pal + ((ea >> 7) & 16);
        palb = pal + ((eb >> 7) & 16);

        if ((ea | eb) & 0x1000) {
            if (!prio) {
                memset(w->prio_mask, 0, sizeof(w->prio_mask));
                prio = 1;
            }
            for (k = 0; k < 8; k++) {
                uint32 x = (sx + k) & 255;

                w->prio_mask[0][x] = (uint8)((ea & 0x1000) && ((pa << (4 * k)) >> 28) != 0);
                w->prio_mask[1][x] = (uint8)((eb & 0x1000) && ((pb << (4 * k)) >> 28) != 0);
            }
        }
        if (sx <= 248) {
            uint32 *o = d + sx;

            o[0] = (pala[pa >> 28] << 16) | palb[pb >> 28];
            o[1] = (pala[(pa >> 24) & 15] << 16) | palb[(pb >> 24) & 15];
            o[2] = (pala[(pa >> 20) & 15] << 16) | palb[(pb >> 20) & 15];
            o[3] = (pala[(pa >> 16) & 15] << 16) | palb[(pb >> 16) & 15];
            o[4] = (pala[(pa >> 12) & 15] << 16) | palb[(pb >> 12) & 15];
            o[5] = (pala[(pa >> 8) & 15] << 16) | palb[(pb >> 8) & 15];
            o[6] = (pala[(pa >> 4) & 15] << 16) | palb[(pb >> 4) & 15];
            o[7] = (pala[pa & 15] << 16) | palb[pb & 15];
        } else {
            for (k = 0; k < 8; k++) {
                d[(sx + k) & 255] = (pala[pa >> 28] << 16) | palb[pb >> 28];
                pa <<= 4;
                pb <<= 4;
            }
        }
    }
    return prio;
}

/* Sprites over lines y and y + 1 (halves: bit 0 line y, bit 1 line
 * y + 1), each with the colours of its line: on each line the first
 * eight of the table covering it (the list is in reverse table order). */
static void draw_sprites(const work *w, const vdp_state *v, const uint32 *const *pals,
                         const sprite *list, uint32 n, uint32 y, uint32 prio, uint32 halves,
                         uint32 *d)
{
    uint32 zoom = (uint32)v->reg[1] & 1;
    uint32 h = ((v->reg[1] & 0x02) ? 16 : 8) << zoom;
    uint32 half;

    for (half = 0; half < 2; half++) {
        const uint32 *pal = pals[half];
        uint8 shown[MAX_SPRITES];
        uint32 count = 0;
        uint32 i;

        if (!(halves & (1u << half)))
            continue;
        for (i = n; i-- > 0;) {
            uint32 dy = (y + half - list[i].y) & 255;

            shown[i] = 0;
            if (dy < h && count < 8) {
                shown[i] = 1;
                count++;
            }
        }
        for (i = 0; i < n; i++) {
            const sprite *s = &list[i];
            uint32 dy = (y + half - s->y) & 255;
            uint32 pix;
            int32 x;
            uint32 k;

            if (!shown[i])
                continue;
            dy >>= zoom;
            pix = tile_row(w, v->vram, s->tile + (dy >> 3), dy & 7);
            for (k = 0, x = s->x; k < 8; k++, pix <<= 4) {
                uint32 c = pix >> 28;
                uint32 t;

                for (t = 0; t <= zoom; t++, x++) {
                    if (c == 0 || x < 0 || x >= DBGVIEW_WIDTH || (prio && w->prio_mask[half][x]))
                        continue;
                    if (half == 0)
                        d[x] = (d[x] & 0xFFFF) | (pal[16 + c] << 16);
                    else
                        d[x] = (d[x] & 0xFFFF0000u) | pal[16 + c];
                }
            }
        }
    }
}

/* A pair of lines whose two lines take different band values (a band
 * starting on an odd line): each line is drawn with its own values into a
 * scratch pair and the halves are merged, with the priority mask of each
 * line kept from its own drawing. */
static void draw_split_pair(work *w, const vdp_state *v, const uint32 *const *pals,
                            const sprite *list, uint32 nspr, uint32 y, const uint32 *hs,
                            const uint32 *nt, const uint32 *on, const uint32 *border, uint32 *d)
{
    uint32 prio[2];
    uint32 half;
    uint32 k;

    for (half = 0; half < 2; half++) {
        prio[half] = 0;
        if (on[half])
            prio[half] = draw_background(w, v, pals[half], y, hs[half], nt[half], w->tmp[half]);
        if (half == 0 && prio[0])
            memcpy(w->mask0, w->prio_mask[0], sizeof(w->mask0));
    }
    if (prio[0])
        memcpy(w->prio_mask[0], w->mask0, sizeof(w->mask0));
    else
        memset(w->prio_mask[0], 0, sizeof(w->prio_mask[0]));
    if (!prio[1])
        memset(w->prio_mask[1], 0, sizeof(w->prio_mask[1]));
    for (k = 0; k < DBGVIEW_WIDTH; k++)
        d[k] = ((on[0] ? w->tmp[0][k] : border[0]) & 0xFFFF0000u) |
               ((on[1] ? w->tmp[1][k] : border[1]) & 0xFFFFu);
    draw_sprites(w, v, pals, list, nspr, y, prio[0] | prio[1],
                 (on[0] ? 1u : 0) | (on[1] ? 2u : 0), d);
}

Err dbgview_lines(const vdp_state *v, uint32 *fb, int32 width, int32 x0, int32 y0,
                  uint32 first, uint32 count)
{
    work *w = work_alloc();
    uint32 *tops;
    uint32 *hss;
    uint32 *ntops;
    uint32 *nvals;
    uint32 *dtops;
    uint32 *dons;
    uint32 *ptops;
    uint32 *row;
    sprite list[MAX_SPRITES];
    uint32 nb;
    uint32 nnb;
    uint32 ndb;
    uint32 npb;
    uint32 merged;
    uint32 nspr;
    uint32 wx = vdp_win_x(v);
    uint32 wy = vdp_win_y(v);
    uint32 ww = vdp_win_w(v);
    uint32 wh = vdp_win_h(v);
    uint32 y;
    uint32 k;
    uint32 q;

    if (w == NULL)
        return -1;
    tops = w->tops;
    hss = w->hss;
    ntops = w->ntops;
    nvals = w->nvals;
    dtops = w->dtops;
    dons = w->dons;
    ptops = w->ptops;
    row = w->row;
    npb = vdp_palette_bands(v, ptops, w->pcram, &merged);
    for (q = 0; q < npb; q++) {
        const uint8 *c = (const uint8 *)(w->pcram + vdp_pal_words(v) * q);

        for (k = 0; k < 32; k++)
            w->pals[q][k] = v->gg ? gg_rgb(c + 2 * k) : sms_rgb(c[k]);
        w->borders[q] = w->pals[q][16 + (v->reg[7] & 15)];
        w->borders[q] |= w->borders[q] << 16;
    }
    nspr = sprite_list(v, list);
    /* Same band model as the renderer; each line of a pair takes the
     * values of its own band. */
    nb = vdp_bands(v->hs_log, v->hs_n, v->reg[8], tops, hss);
    nnb = vdp_bands(v->nt_log, v->nt_n, v->reg[2], ntops, nvals);
    ndb = vdp_bands(v->de_log, v->de_n, v->de_start, dtops, dons);

    /* The lines of the window; with a window narrower than the picture
     * (Game Gear) each pair is drawn whole into a row of its own, then
     * the window's columns are copied. */
    for (y = wy + (first & ~1u); y < wy + first + count && y < wy + wh; y += 2) {
        uint32 *dst = fb + ((uint32)(y0 + (int32)(y - wy)) >> 1) * (uint32)width + (uint32)x0;
        uint32 *d = (ww < DBGVIEW_WIDTH) ? row : dst;
        const uint32 *pal[2];
        uint32 border[2];
        uint32 hs[2];
        uint32 nt[2];
        uint32 on[2];
        uint32 half;

        for (half = 0; half < 2; half++) {
            hs[half] = hss[0];
            nt[half] = nvals[0];
            on[half] = dons[0];
            q = 0;
            for (k = 1; k < nb && tops[k] <= y + half; k++)
                hs[half] = hss[k];
            for (k = 1; k < nnb && ntops[k] <= y + half; k++)
                nt[half] = nvals[k];
            for (k = 1; k < ndb && dtops[k] <= y + half; k++)
                on[half] = dons[k];
            while (q + 1 < npb && ptops[q + 1] <= y + half)
                q++;
            pal[half] = w->pals[q];
            border[half] = w->borders[q];
        }
        if (hs[0] != hs[1] || nt[0] != nt[1] || on[0] != on[1] || pal[0] != pal[1]) {
            draw_split_pair(w, v, pal, list, nspr, y, hs, nt, on, border, d);
        } else if (!on[0]) {                    /* display off */
            for (k = 0; k < DBGVIEW_WIDTH; k++)
                d[k] = border[0];
        } else {
            draw_sprites(w, v, pal, list, nspr, y,
                         draw_background(w, v, pal[0], y, hs[0], nt[0], d), 3, d);
        }
        if (on[0] | on[1]) {
            if (v->reg[0] & 0x20) {             /* left column blanked */
                for (k = 0; k < 8; k++)
                    d[k] = (border[0] & 0xFFFF0000u) | (border[1] & 0xFFFFu);
            }
        }
        if (d != dst)
            memcpy(dst, d + wx, ww * 4);
    }
    FreeMem(w, sizeof(work));
    return 0;
}

Err dbgview_draw(const vdp_state *v, uint32 *fb, int32 width, int32 x0, int32 y0)
{
    return dbgview_lines(v, fb, width, x0, y0, 0, vdp_win_h(v));
}

void dbgview_palette(const vdp_state *v, uint32 *fb, int32 width, int32 x0, int32 y0)
{
    uint32 row;
    uint32 y;
    uint32 x;

    for (row = 0; row < 2; row++) {
        for (y = 0; y < 8; y += 2) {
            uint32 *d = fb + ((uint32)(y0 + (int32)(row * 8 + y)) >> 1) * (uint32)width +
                        (uint32)x0;

            for (x = 0; x < 16 * 8; x++) {
                uint32 i = row * 16 + (x >> 3);
                uint32 c = v->gg ? gg_rgb(v->gg_cram + 2 * i) : sms_rgb(v->cram[i]);

                d[x] = (c << 16) | c;
            }
        }
    }
}
