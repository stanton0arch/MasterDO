/*
 * Debug view of the VDP picture, drawn by the CPU. See dbgview.h.
 *
 * Lines are drawn in pairs, which is what an LRFORM word holds: both lines
 * of a pair use the same scroll values, so each of the 32 background
 * columns gives eight words, one per pixel column. Sprites are then drawn
 * over the pair, from the last to the first so that sprite 0 ends on top,
 * except over background pixels of tiles that have the priority bit.
 */

#include "dbgview.h"

#include "string.h"

#define MAX_SPRITES 64

typedef struct {
    int32  x;
    uint32 y;               /* first line */
    uint32 tile;            /* first tile */
} sprite;

/* Pixel k (from the left) of a tile row plane byte, as bit 0 of nibble
 * 7 - k; mirrored for horizontally flipped tiles. */
static uint32 expand[256];
static uint32 expand_flip[256];
static uint32 tables_ready;

/* Background pixels of priority tiles that hide sprites, for the two
 * lines of the pair being drawn. */
static uint8 prio_mask[2][DBGVIEW_WIDTH];

static void init_tables(void)
{
    uint32 v;
    uint32 k;

    for (v = 0; v < 256; v++) {
        uint32 e = 0;
        uint32 f = 0;

        for (k = 0; k < 8; k++) {
            if (v & ((uint32)0x80 >> k)) {
                e |= (uint32)1 << (4 * (7 - k));
                f |= (uint32)1 << (4 * k);
            }
        }
        expand[v] = e;
        expand_flip[v] = f;
    }
    tables_ready = 1;
}

/* SMS colour (--BBGGRR) to 3DO RGB 5:5:5. */
static uint32 sms_rgb(uint32 c)
{
    static const uint8 level[4] = { 0, 10, 21, 31 };

    return ((uint32)level[c & 3] << 10) | ((uint32)level[(c >> 2) & 3] << 5) |
           (uint32)level[(c >> 4) & 3];
}

/* Row r of the tile of name table entry e, pixel k in nibble 7 - k. */
static uint32 tile_row(const uint8 *vram, uint32 e, uint32 r)
{
    const uint8 *p;
    const uint32 *ex = (e & 0x200) ? expand_flip : expand;

    if (e & 0x400)
        r = 7 - r;
    p = vram + (e & 0x1FF) * 32 + r * 4;
    return ex[p[0]] | (ex[p[1]] << 1) | (ex[p[2]] << 2) | (ex[p[3]] << 3);
}

/* Sprites before the $D0 terminator, in drawing order (last first). */
static uint32 sprite_list(const vdp_state *v, sprite *list)
{
    const uint8 *sat = v->vram + ((v->reg[5] & 0x7E) << 7);
    uint32 tbase = (v->reg[6] & 0x04) ? 256 : 0;
    uint32 tmask = (v->reg[1] & 0x02) ? 0xFE : 0xFF;
    int32 xoff = (v->reg[0] & 0x08) ? 8 : 0;
    uint32 n;
    uint32 i;

    for (n = 0; n < MAX_SPRITES; n++) {
        if (sat[n] == 0xD0)
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
static uint32 draw_background(const vdp_state *v, const uint32 *pal, uint32 y, uint32 hs_y,
                              uint32 *d)
{
    const uint8 *vram = v->vram;
    const uint8 *nt = vram + ((v->reg[2] & 0x0E) << 10);
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

        if (ya >= 224)
            ya -= 224;
        yb = (ya == 223) ? 0 : ya + 1;
        ea = nt_entry(nt, ya >> 3, col);
        eb = ((yb >> 3) == (ya >> 3)) ? ea : nt_entry(nt, yb >> 3, col);
        pa = tile_row(vram, ea, ya & 7);
        pb = tile_row(vram, eb, yb & 7);
        pala = pal + ((ea >> 7) & 16);
        palb = pal + ((eb >> 7) & 16);

        if ((ea | eb) & 0x1000) {
            if (!prio) {
                memset(prio_mask, 0, sizeof(prio_mask));
                prio = 1;
            }
            for (k = 0; k < 8; k++) {
                uint32 x = (sx + k) & 255;

                prio_mask[0][x] = (uint8)((ea & 0x1000) && ((pa << (4 * k)) >> 28) != 0);
                prio_mask[1][x] = (uint8)((eb & 0x1000) && ((pb << (4 * k)) >> 28) != 0);
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

/* Sprites over lines y and y + 1. */
static void draw_sprites(const vdp_state *v, const uint32 *pal, const sprite *list,
                         uint32 n, uint32 y, uint32 prio, uint32 *d)
{
    uint32 h = (v->reg[1] & 0x02) ? 16 : 8;
    uint32 i;
    uint32 half;

    for (i = 0; i < n; i++) {
        const sprite *s = &list[i];

        for (half = 0; half < 2; half++) {
            uint32 dy = (y + half - s->y) & 255;
            uint32 pix;
            int32 x;
            uint32 k;

            if (dy >= h)
                continue;
            pix = tile_row(v->vram, s->tile + (dy >> 3), dy & 7);
            for (k = 0, x = s->x; k < 8; k++, x++, pix <<= 4) {
                uint32 c = pix >> 28;

                if (c == 0 || x < 0 || x >= DBGVIEW_WIDTH || (prio && prio_mask[half][x]))
                    continue;
                if (half == 0)
                    d[x] = (d[x] & 0xFFFF) | (pal[16 + c] << 16);
                else
                    d[x] = (d[x] & 0xFFFF0000u) | pal[16 + c];
            }
        }
    }
}

void dbgview_lines(const vdp_state *v, uint32 *fb, int32 width, int32 x0, int32 y0,
                   uint32 first, uint32 count)
{
    uint32 pal[32];
    sprite list[MAX_SPRITES];
    uint32 tops[VDP_HS_LOG + 1];
    uint32 hss[VDP_HS_LOG + 1];
    uint32 dtops[VDP_DE_LOG + 1];
    uint32 dons[VDP_DE_LOG + 1];
    uint32 nb;
    uint32 ndb;
    uint32 nspr;
    uint32 border;
    uint32 y;
    uint32 k;

    if (!tables_ready)
        init_tables();
    for (k = 0; k < 32; k++)
        pal[k] = sms_rgb(v->cram[k]);
    border = pal[16 + (v->reg[7] & 15)];
    border |= border << 16;
    nspr = sprite_list(v, list);
    /* Same band model as the renderer (a pair of lines takes the values
     * of its even line). */
    nb = vdp_bands(v->hs_log, v->hs_n, v->reg[8], tops, hss);
    ndb = vdp_bands(v->de_log, v->de_n, v->de_start, dtops, dons);

    for (y = first & ~1u; y < first + count && y < DBGVIEW_HEIGHT; y += 2) {
        uint32 *d = fb + ((uint32)(y0 + (int32)y) >> 1) * (uint32)width + (uint32)x0;
        uint32 hs = hss[0];
        uint32 on = dons[0];

        for (k = 1; k < nb && tops[k] <= y; k++)
            hs = hss[k];
        for (k = 1; k < ndb && dtops[k] <= y; k++)
            on = dons[k];
        if (!on) {                              /* display off */
            for (k = 0; k < DBGVIEW_WIDTH; k++)
                d[k] = border;
            continue;
        }
        draw_sprites(v, pal, list, nspr, y, draw_background(v, pal, y, hs, d), d);
        if (v->reg[0] & 0x20) {                 /* left column blanked */
            for (k = 0; k < 8; k++)
                d[k] = border;
        }
    }
}

void dbgview_draw(const vdp_state *v, uint32 *fb, int32 width, int32 x0, int32 y0)
{
    dbgview_lines(v, fb, width, x0, y0, 0, DBGVIEW_HEIGHT);
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
                uint32 c = sms_rgb(v->cram[row * 16 + (x >> 3)]);

                d[x] = (c << 16) | c;
            }
        }
    }
}
