/*
 * Video micro-benchmarks.
 *
 * The SMS picture is 256x192 pixels, built from 8x8 tiles in 4 bpp with two
 * 16-colour palettes, plus up to 64 sprites. It is drawn with one coded
 * 4 bpp cel per tile (33x25 background tiles, the extra row and column
 * covering fine scrolling, then 64 sprite cels), the PLUT standing for the
 * SMS palette. The CPU side is timed as well: rebuilding the cel list from
 * the tile map. The CEL engine cannot run in parallel with the CPU, so
 * every figure here adds up with the Z80 emulation time.
 */

#include "bench_video.h"

#include "stdio.h"
#include "string.h"
#include "mem.h"
#include "celutils.h"

#define TILE_ROW_BYTES 8    /* 8 pixels x 4 bpp = 4 bytes, padded to the 2-word minimum */
#define TILE_BYTES     (8 * TILE_ROW_BYTES)
#define PLUT_ENTRIES   16
#define REPEATS        30

#define CEL_MEM (MEMTYPE_DRAM | MEMTYPE_CEL | MEMTYPE_FILL)

/* Same flags as CreateCel(), plus a PLUT load for every cel. */
#define CEL_FLAGS (CCB_NPABS | CCB_SPABS | CCB_PPABS | CCB_LDSIZE | CCB_LDPRS | \
                   CCB_LDPPMP | CCB_LDPLUT | CCB_CCBPRE | CCB_YOXY | CCB_USEAV | \
                   CCB_NOBLK | CCB_ACE | CCB_ACW | CCB_ACCW)

static void bv_init_tile_cel(CCB *c, CCB *next, int32 opaque)
{
    c->ccb_Flags = (uint32)CEL_FLAGS;
    if (opaque)
        c->ccb_Flags |= CCB_BGND;
    c->ccb_NextPtr = next;
    c->ccb_SourcePtr = NULL;
    c->ccb_PLUTPtr = NULL;
    c->ccb_XPos = 0;
    c->ccb_YPos = 0;
    c->ccb_HDX = 1 << 20;           /* 12.20 fixed point */
    c->ccb_HDY = 0;
    c->ccb_VDX = 0;
    c->ccb_VDY = 1 << 16;           /* 16.16 fixed point */
    c->ccb_HDDX = 0;
    c->ccb_HDDY = 0;
    c->ccb_PIXC = PIXC_OPAQUE;
    c->ccb_PRE0 = ((8 - PRE0_VCNT_PREFETCH) << PRE0_VCNT_SHIFT) | PRE0_BPP_4;
    c->ccb_PRE1 = ((8 - PRE1_TLHPCNT_PREFETCH) << PRE1_TLHPCNT_SHIFT) |
                  ((TILE_ROW_BYTES / 4 - PRE1_WOFFSET_PREFETCH) << PRE1_WOFFSET8_SHIFT) |
                  PRE1_TLLSB_PDC0;
    c->ccb_Width = 8;
    c->ccb_Height = 8;
}

/* Tile pattern: a 1-pixel frame and a per-tile texture. */
static uint32 bv_pixel(int32 tile, int32 row, int32 x)
{
    if (row == 0 || x == 0)
        return 1;
    return (uint32)(2 + (tile + x * row) % 14);
}

static void bv_free_buffers(bench_video *bv)
{
    if (bv->map != NULL) FreeMem(bv->map, BV_MAP_COLS * BV_MAP_ROWS * sizeof(uint16));
    if (bv->pluts != NULL) FreeMem(bv->pluts, 3 * PLUT_ENTRIES * sizeof(uint16));
    if (bv->tiles != NULL) FreeMem(bv->tiles, BV_TILES * TILE_BYTES);
    if (bv->cels != NULL) FreeMem(bv->cels, sizeof(CCB) * (BV_BG_CELS + BV_SPRITES));
    memset(bv, 0, sizeof(*bv));
}

Err bench_video_init(bench_video *bv, const platform *p)
{
    int32 i;
    int32 x;
    uint32 seed;

    memset(bv, 0, sizeof(*bv));
    bv->origin_x = (p->width - BV_PIC_W) / 2;
    bv->origin_y = (p->height - BV_PIC_H) / 2;

    bv->cels = (CCB *)AllocMem(sizeof(CCB) * (BV_BG_CELS + BV_SPRITES), CEL_MEM);
    bv->tiles = (uint8 *)AllocMem(BV_TILES * TILE_BYTES, CEL_MEM);
    bv->pluts = (uint16 *)AllocMem(3 * PLUT_ENTRIES * sizeof(uint16), CEL_MEM);
    bv->map = (uint16 *)AllocMem(BV_MAP_COLS * BV_MAP_ROWS * sizeof(uint16),
                                 MEMTYPE_DRAM | MEMTYPE_FILL);
    if (bv->cels == NULL || bv->tiles == NULL || bv->pluts == NULL || bv->map == NULL) {
        printf("ERROR: bench_video out of memory\n");
        bv_free_buffers(bv);
        return -1;
    }

    /* Tiles, 4 bpp, first pixel in the high nibble. */
    for (i = 0; i < BV_TILES; i++) {
        uint8 *t = bv->tiles + i * TILE_BYTES;
        int32 r;

        for (r = 0; r < 8; r++) {
            for (x = 0; x < 8; x += 2) {
                t[r * TILE_ROW_BYTES + (x >> 1)] =
                    (uint8)((bv_pixel(i, r, x) << 4) | bv_pixel(i, r, x + 1));
            }
        }
    }

    /* Two dim background palettes and a bright sprite palette whose
     * colour 0 stays transparent (000 without CCB_BGND). */
    for (i = 0; i < PLUT_ENTRIES; i++) {
        bv->pluts[i] = (uint16)MakeRGB15(i, i, 4 + i);
        bv->pluts[PLUT_ENTRIES + i] = (uint16)MakeRGB15(4 + i, 2 + i / 2, i);
        bv->pluts[2 * PLUT_ENTRIES + i] =
            (uint16)(i == 0 ? 0 : MakeRGB15(31, 8 + i, 31 - 2 * i));
    }

    /* Pseudo-random tile map: tile index in bits 0-8, palette in bit 11. */
    seed = 12345;
    for (i = 0; i < BV_MAP_COLS * BV_MAP_ROWS; i++) {
        seed = seed * 1103515245u + 12345u;
        bv->map[i] = (uint16)(((seed >> 16) % BV_TILES) | (((seed >> 8) & 1) << 11));
    }

    /* One chain: background cels (opaque), then sprite cels. */
    for (i = 0; i < BV_BG_CELS + BV_SPRITES; i++) {
        CCB *next = (i + 1 < BV_BG_CELS + BV_SPRITES) ? &bv->cels[i + 1] : NULL;
        bv_init_tile_cel(&bv->cels[i], next, i < BV_BG_CELS);
    }
    bv->cels[BV_BG_CELS + BV_SPRITES - 1].ccb_Flags |= CCB_LAST;
    for (i = 0; i < BV_SPRITES; i++) {
        CCB *c = &bv->cels[BV_BG_CELS + i];
        c->ccb_SourcePtr = (CelData *)(bv->tiles + ((i * 7) % BV_TILES) * TILE_BYTES);
        c->ccb_PLUTPtr = bv->pluts + 2 * PLUT_ENTRIES;
    }
    bench_video_animate(bv);

    printf("Video: SMS picture at (%ld,%ld), %d background + %d sprite cels\n",
           (long)bv->origin_x, (long)bv->origin_y, BV_BG_CELS, BV_SPRITES);
    return 0;
}

void bench_video_free(bench_video *bv)
{
    bv_free_buffers(bv);
}

void bench_video_animate(bench_video *bv)
{
    uint32 f = bv->frame++;
    int32 sx = (int32)(f & 255);
    int32 sy = (int32)((f >> 1) % (BV_MAP_ROWS * 8));
    int32 map_col0 = sx >> 3;
    int32 map_row = sy >> 3;
    int32 row;
    int32 col;
    int32 i;
    CCB *c = bv->cels;

    for (row = 0; row < BV_ROWS; row++) {
        const uint16 *m = bv->map + map_row * BV_MAP_COLS;
        Coord x = (Coord)((bv->origin_x - (sx & 7)) << 16);
        Coord y = (Coord)((bv->origin_y + row * 8 - (sy & 7)) << 16);

        for (col = 0; col < BV_COLS; col++, c++) {
            uint32 e = m[(map_col0 + col) & (BV_MAP_COLS - 1)];

            c->ccb_SourcePtr = (CelData *)(bv->tiles + (e & 0x1FF) * TILE_BYTES);
            c->ccb_PLUTPtr = bv->pluts + ((e >> 11) & 1) * PLUT_ENTRIES;
            c->ccb_XPos = x;
            c->ccb_YPos = y;
            x += 8 << 16;
        }
        if (++map_row == BV_MAP_ROWS)
            map_row = 0;
    }

    for (i = 0; i < BV_SPRITES; i++, c++) {
        uint32 px = ((uint32)i * 37u + f * (uint32)(1 + (i & 3))) % (BV_PIC_W - 8);
        uint32 py = ((uint32)i * 23u + f * (uint32)(1 + ((i >> 2) & 3))) % (BV_PIC_H - 8);

        c->ccb_XPos = (Coord)((bv->origin_x + (int32)px) << 16);
        c->ccb_YPos = (Coord)((bv->origin_y + (int32)py) << 16);
    }
}

Err bench_video_draw_scene(const bench_video *bv, const platform *p, int32 screen)
{
    return DrawCels(plat_bitmap(p, screen), bv->cels);
}

static uint32 bv_time_draw(Item bitmap, CCB *first, int32 *status)
{
    uint32 t0;
    uint32 t1;
    int32 i;
    Err err;

    t0 = plat_usec_now();
    for (i = 0; i < REPEATS; i++) {
        err = DrawCels(bitmap, first);
        if (err < 0 && *status == 0)
            *status = err;
    }
    t1 = plat_usec_now();
    return (t1 - t0) / REPEATS;
}

Err bench_video_run(bench_video *bv, const platform *p, bench_video_result *res)
{
    Item bitmap = plat_bitmap(p, p->back);
    CCB *last_bg = &bv->cels[BV_BG_CELS - 1];
    uint32 t0;
    int32 i;

    memset(res, 0, sizeof(*res));

    t0 = plat_usec_now();
    for (i = 0; i < REPEATS; i++)
        bench_video_animate(bv);
    res->update_us = (plat_usec_now() - t0) / REPEATS;

    last_bg->ccb_Flags |= CCB_LAST;
    res->bg_us = bv_time_draw(bitmap, bv->cels, &res->status);
    last_bg->ccb_Flags &= ~CCB_LAST;
    res->spr_us = bv_time_draw(bitmap, &bv->cels[BV_BG_CELS], &res->status);
    res->all_us = bv_time_draw(bitmap, bv->cels, &res->status);

    t0 = plat_usec_now();
    for (i = 0; i < REPEATS; i++)
        plat_clear(p, p->back);
    res->clear_us = (plat_usec_now() - t0) / REPEATS;

    return res->status;
}

void bench_video_log(const bench_video_result *res)
{
    if (res->status != 0)
        printf("Video: DrawCels returned an error (0x%lx)\n", (unsigned long)res->status);
    printf("Video: %d tile cels %lu us, %d sprite cels %lu us, both %lu us, "
           "cel list update (CPU) %lu us\n",
           BV_BG_CELS, (unsigned long)res->bg_us, BV_SPRITES, (unsigned long)res->spr_us,
           (unsigned long)res->all_us, (unsigned long)res->update_us);
    printf("Video: SPORT clear %lu us\n", (unsigned long)res->clear_us);
}
