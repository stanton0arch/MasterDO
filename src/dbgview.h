#ifndef DBGVIEW_H
#define DBGVIEW_H

/*
 * Debug view: the picture held by the VDP (background with its scroll
 * registers, then sprites), drawn by the CPU into a 16-bit 3DO frame
 * buffer. It takes tens of milliseconds: it shows what a game displays,
 * it is not meant to be drawn every frame.
 *
 * Frame buffers are in LRFORM: lines 2k and 2k+1 share each 32-bit word,
 * the even line in the high half. Pictures are drawn two lines at a time,
 * so y0 must be even.
 */

#include "types.h"
#include "vdp.h"

#define DBGVIEW_WIDTH  256
#define DBGVIEW_HEIGHT_MAX VDP_ACTIVE_MAX   /* the picture has v->active lines */

/* The part of the picture shown, vdp_win_w(v) x vdp_win_h(v) (the
 * 256 x v->active picture, or the LCD window of the Game Gear), at
 * (x0, y0) of a frame buffer width pixels wide. The tables of the drawing
 * (about 10 KiB) are allocated for its duration, in DRAM or else in VRAM;
 * -1 when neither has room, and nothing is drawn. */
Err dbgview_draw(const vdp_state *v, uint32 *fb, int32 width, int32 x0, int32 y0);

/* Lines first to first + count - 1 of that part only (first even). */
Err dbgview_lines(const vdp_state *v, uint32 *fb, int32 width, int32 x0, int32 y0,
                  uint32 first, uint32 count);

/* The 32 colours of the colour RAM: two rows of sixteen 8x8 squares
 * (background palette, then sprite palette). */
void dbgview_palette(const vdp_state *v, uint32 *fb, int32 width, int32 x0, int32 y0);

#endif /* DBGVIEW_H */
