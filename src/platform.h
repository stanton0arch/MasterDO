#ifndef PLATFORM_H
#define PLATFORM_H

/*
 * 3DO platform layer: display (double buffered), microsecond clock,
 * text output and control pad.
 */

#include "types.h"
#include "graphics.h"
#include "displayutils.h"

#define PLAT_NUM_SCREENS 2

/* Nominal frame periods, in microseconds. */
#define PLAT_NTSC_FRAME_US 16683
#define PLAT_PAL_FRAME_US  20000

/* Text grid of the 8x8 system font. */
#define PLAT_GLYPH_W 8
#define PLAT_LINE_H  10

typedef struct {
    ScreenContext sc;
    Item   vbl_ior;     /* VBL timer request, used to pace presentation */
    Item   vram_ior;    /* SPORT request, used to clear screens */
    int32  width;       /* bitmap size actually built by the display */
    int32  height;
    int32  is_pal;      /* 1 when the console runs a 50 Hz display */
    int32  back;        /* index of the screen being drawn */
} platform;

Err    plat_init(platform *p);
void   plat_shutdown(platform *p);

/* Microseconds since power-on, wraps after about 71 minutes. */
uint32 plat_usec_now(void);

Item   plat_bitmap(const platform *p, int32 screen);
void   plat_clear(const platform *p, int32 screen);
void   plat_text(const platform *p, int32 screen, int32 x, int32 y,
                 const char *text, Color color);

/* Shows the back screen, waits for the next vertical blank, then swaps. */
void   plat_present(platform *p);
void   plat_wait_vbl(const platform *p);

/* Free memory in bytes (system and task pools). */
void   plat_mem_free(uint32 *dram, uint32 *vram);

/* Buttons newly pressed since the previous call (ControlX, ControlA...). */
uint32 plat_pad_pressed(void);

/* Buttons held down (ControlX, ControlA...). */
uint32 plat_pad_state(void);

/* Pixels of a screen: 16-bit LRFORM, p->width pixels per line. */
uint32 *plat_pixels(const platform *p, int32 screen);

/* Clip window of both screens: drawing (cels, text) is limited to it and
 * cel coordinates become relative to its top-left corner (y even). */
void   plat_set_clip(const platform *p, int32 x, int32 y, int32 w, int32 h);
void   plat_reset_clip(const platform *p);

/* Fills a whole screen with an RGB 5:5:5 colour (SPORT). */
void   plat_fill(const platform *p, int32 screen, uint32 rgb15);

#endif /* PLATFORM_H */
