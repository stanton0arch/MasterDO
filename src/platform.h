#ifndef PLATFORM_H
#define PLATFORM_H

/*
 * 3DO platform layer: display (three screens), microsecond clock, text
 * output and control pad.
 *
 * Presentation: plat_present() shows the back screen and waits for the
 * vertical blank (the text screens, the pause). The run instead queues
 * its finished screens (plat_queue) for a presenter thread that shows one
 * per vertical blank: the main task can then work a frame ahead, and a
 * frame that runs past its vertical blank is absorbed by the screen still
 * queued instead of being shown late. plat_next_screen() gives the main
 * task a screen to draw into, waiting for one when both others are
 * displayed or queued.
 */

#include "types.h"
#include "graphics.h"
#include "displayutils.h"

#define PLAT_NUM_SCREENS 3
#define PLAT_QUEUE_LEN   4     /* power of two, more than the screens */
#define PLAT_LONG_US     25000 /* a presentation two VBLs or more after the previous one */

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
    uint32 frame_us;    /* period of the display */
    int32  num_screens; /* screens created (3, or 2 without memory for the third) */
    int32  back;        /* index of the screen being drawn */
    /* Presenter thread: the screens queued by the main task, shown one
     * per vertical blank. The queue is read by the thread and written by
     * the main task; the other fields are the thread's. */
    Item   thread;      /* item of the thread, or -1 */
    Item   thread_task; /* its task item, for signals */
    Item   main_task;
    int32  thread_sig;  /* signal the thread waits for */
    int32  free_sig;    /* sent to the main task after each presentation */
    int32  init_sig;    /* sent by the thread once ready */
    volatile uint32 q_head;
    volatile uint32 q_tail;
    volatile int32  queue[PLAT_QUEUE_LEN];
    volatile int32  displayed;      /* screen on display */
    volatile uint32 shown_us;       /* clock after the last presentation */
    volatile uint32 presented;      /* presentations by the thread */
    volatile uint32 long_presents;  /* presentations PLAT_LONG_US or more after the previous */
    volatile uint32 long_us;        /* period of the last of them */
    volatile uint32 errors;         /* DisplayScreen errors in the thread */
    volatile uint32 quit;
} platform;

Err    plat_init(platform *p);
void   plat_shutdown(platform *p);

/* Microseconds since power-on, wraps after about 71 minutes. */
uint32 plat_usec_now(void);

Item   plat_bitmap(const platform *p, int32 screen);
void   plat_clear(const platform *p, int32 screen);
void   plat_text(const platform *p, int32 screen, int32 x, int32 y,
                 const char *text, Color color);

/* Shows the back screen, waits for the next vertical blank, then takes
 * another screen as the back screen (the queue must be empty). */
void   plat_present(platform *p);
void   plat_wait_vbl(const platform *p);

/* Queued presentation (the presenter thread; plat_has_presenter() tells
 * whether it runs). plat_queue() hands the back screen to the thread;
 * plat_next_screen() then takes a free screen as the back screen, waiting
 * for one when none is free; plat_free_screen() tells whether one is free
 * now; plat_next_vbl_us() estimates the clock value of the next vertical
 * blank from the last presentation; plat_drain() waits until every queued
 * screen has been shown (before plat_present() is used again). */
int32  plat_has_presenter(const platform *p);
void   plat_queue(platform *p);
void   plat_next_screen(platform *p);
int32  plat_free_screen(const platform *p);
uint32 plat_next_vbl_us(const platform *p);
void   plat_drain(platform *p);

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
