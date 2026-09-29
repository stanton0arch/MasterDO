#ifndef BENCH_VIDEO_H
#define BENCH_VIDEO_H

/*
 * Video micro-benchmarks: cost of drawing an SMS picture with the CEL
 * engine as one cel per 8x8 tile (the full-frame variants of step 0 are
 * settled and no longer run).
 */

#include "types.h"
#include "graphics.h"
#include "platform.h"

#define BV_COLS     33      /* 32 visible columns + 1 for fine scrolling */
#define BV_ROWS     25      /* 24 visible rows + 1 for fine scrolling */
#define BV_BG_CELS  (BV_COLS * BV_ROWS)
#define BV_SPRITES  64
#define BV_TILES    448     /* tiles that fit in the SMS video RAM */
#define BV_MAP_COLS 32
#define BV_MAP_ROWS 28
#define BV_PIC_W    256
#define BV_PIC_H    192

typedef struct {
    CCB    *cels;       /* BV_BG_CELS background cels, then BV_SPRITES sprites */
    uint8  *tiles;      /* BV_TILES tiles, 4 bpp, 8 bytes per row */
    uint16 *pluts;      /* 16-entry PLUTs: background 0, background 1, sprites */
    uint16 *map;        /* BV_MAP_COLS x BV_MAP_ROWS: tile index, bit 11 = palette */
    int32   origin_x;   /* top-left corner of the SMS picture on screen */
    int32   origin_y;
    uint32  frame;
} bench_video;

typedef struct {
    int32  status;      /* first DrawCels error, 0 when none */
    uint32 bg_us;       /* BV_BG_CELS background tile cels */
    uint32 spr_us;      /* BV_SPRITES sprite cels */
    uint32 all_us;      /* both in one DrawCels call */
    uint32 update_us;   /* CPU: rebuild the cel list from the tile map */
    uint32 clear_us;    /* SPORT clear of a whole screen */
} bench_video_result;

Err  bench_video_init(bench_video *bv, const platform *p);
void bench_video_free(bench_video *bv);
Err  bench_video_run(bench_video *bv, const platform *p, bench_video_result *res);
void bench_video_log(const bench_video_result *res);

/* Live scene: scrolls the tile map and moves the sprites by one frame. */
void bench_video_animate(bench_video *bv);
Err  bench_video_draw_scene(const bench_video *bv, const platform *p, int32 screen);

#endif /* BENCH_VIDEO_H */
