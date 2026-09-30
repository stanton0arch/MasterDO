/*
 * 3DO platform layer: display, clock, text output and control pad.
 */

#include "platform.h"

#include "stdio.h"
#include "string.h"
#include "mem.h"
#include "time.h"
#include "event.h"
#include "controlpad.h"
#include "kernel.h"
#include "task.h"
#include "item.h"

#define PRESENTER_STACK 4096

/* The presenter thread finds its platform here (a thread takes no
 * argument). */
static platform *presenter_plat;

/* Presenter thread: shows the queued screens one per vertical blank.
 * Woken by the main task when it queues a screen; after each
 * presentation it tells the main task that a screen is free. The
 * screens belong to the main task: the thread opens them. */
static void presenter_main(void)
{
    platform *p = presenter_plat;
    Item ior;
    int32 sig;
    int32 i;

    OpenGraphicsFolio();
    ior = GetVBLIOReq();
    sig = AllocSignal(0);
    for (i = 0; i < p->num_screens; i++)
        OpenItem(p->sc.sc_ScreenItems[i], NULL);
    p->thread_task = CURRENTTASK->t.n_Item;
    p->thread_sig = (ior >= 0 && sig > 0) ? sig : 0;
    SendSignal(p->main_task, (uint32)p->init_sig);
    if (p->thread_sig == 0) {
        printf("WARNING: presenter thread without VBL request (0x%lx) or signal (%ld)\n",
               (unsigned long)ior, (long)sig);
        return;
    }
    for (;;) {
        WaitSignal((uint32)sig);
        if (p->quit)
            break;
        while (p->q_head != p->q_tail) {
            int32 screen = p->queue[p->q_head & (PLAT_QUEUE_LEN - 1)];
            Err err = DisplayScreen(p->sc.sc_ScreenItems[screen], 0);
            uint32 now;

            if (err < 0 && p->errors++ == 0)
                printf("WARNING: DisplayScreen failed in the presenter thread (0x%lx)\n",
                       (unsigned long)err);
            WaitVBL(ior, 1);
            now = plat_usec_now();
            if (p->presented != 0 && now - p->shown_us >= PLAT_LONG_US) {
                p->long_presents++;
                p->long_us = now - p->shown_us;
            }
            p->shown_us = now;
            p->presented++;
            p->displayed = screen;
            p->q_head++;
            SendSignal(p->main_task, (uint32)p->free_sig);
        }
    }
}

static void presenter_start(platform *p)
{
    uint8 pri;

    p->thread = -1;
    p->thread_sig = 0;
    p->main_task = CURRENTTASK->t.n_Item;
    p->free_sig = AllocSignal(0);
    p->init_sig = AllocSignal(0);
    if (p->free_sig <= 0 || p->init_sig <= 0) {
        printf("WARNING: no signals for the presenter thread\n");
        return;
    }
    pri = CURRENTTASK->t.n_Priority;
    if (pri < 199)
        pri++;
    presenter_plat = p;
    p->thread = CreateThread("presenter", pri, presenter_main, PRESENTER_STACK);
    if (p->thread < 0) {
        printf("WARNING: CreateThread failed (0x%lx)\n", (unsigned long)p->thread);
        return;
    }
    WaitSignal((uint32)p->init_sig);
    if (p->thread_sig == 0) {
        DeleteThread(p->thread);
        p->thread = -1;
    }
}

static void presenter_stop(platform *p)
{
    if (p->thread < 0)
        return;
    plat_drain(p);
    p->quit = 1;
    SendSignal(p->thread_task, (uint32)p->thread_sig);
    DeleteThread(p->thread);
    p->thread = -1;
}

#define PLAT_ALL_BUTTONS (ControlDown | ControlUp | ControlRight | ControlLeft | ControlA | \
                          ControlB | ControlC | ControlStart | ControlX | ControlRightShift | \
                          ControlLeftShift)

Err plat_init(platform *p)
{
    Err err;
    int32 display_type;
    int32 page_size;
    int32 i;

    memset(p, 0, sizeof(*p));
    p->vbl_ior = -1;
    p->vram_ior = -1;

    err = OpenGraphicsFolio();
    if (err < 0) {
        printf("ERROR: OpenGraphicsFolio failed (0x%lx)\n", (unsigned long)err);
        return err;
    }

    /* Ask the console for its display type. A PAL console refuses an NTSC
     * display, so every PAL variant is mapped to the 50 Hz PAL1 type. */
    display_type = DI_TYPE_DEFAULT;
    QueryGraphics(QUERYGRAF_TAG_DEFAULTDISPLAYTYPE, &display_type);
    if (display_type == DI_TYPE_PAL1 || display_type == DI_TYPE_PAL2) {
        display_type = DI_TYPE_PAL1;
        p->is_pal = 1;
    } else {
        display_type = DI_TYPE_NTSC;
        p->is_pal = 0;
    }

    p->frame_us = p->is_pal ? PLAT_PAL_FRAME_US : PLAT_NTSC_FRAME_US;

    /* Three screens: one displayed, one queued, one being drawn. Without
     * the memory for the third, two (no queued presentation). */
    p->num_screens = PLAT_NUM_SCREENS;
    err = CreateBasicDisplay(&p->sc, (uint32)display_type, PLAT_NUM_SCREENS);
    if (err < 0) {
        printf("WARNING: CreateBasicDisplay failed for %d screens (0x%lx), trying 2\n",
               PLAT_NUM_SCREENS, (unsigned long)err);
        p->num_screens = 2;
        err = CreateBasicDisplay(&p->sc, (uint32)display_type, 2);
        if (err < 0) {
            printf("ERROR: CreateBasicDisplay failed (0x%lx)\n", (unsigned long)err);
            return err;
        }
    }

    /* The cel engine draws into bitmap items: make sure they are set. */
    for (i = 0; i < p->num_screens; i++)
        p->sc.sc_BitmapItems[i] = p->sc.sc_Bitmaps[i]->bm.n_Item;

    /* Trust the bitmap the display really built (a PAL raster can be
     * taller than 240 lines). */
    p->width = p->sc.sc_Bitmaps[0]->bm_Width;
    p->height = p->sc.sc_Bitmaps[0]->bm_Height;
    if (p->width <= 0 || p->height <= 0) {
        printf("WARNING: bitmap %ldx%ld invalid, using 320x240\n",
               (long)p->width, (long)p->height);
        p->width = 320;
        p->height = 240;
    }

    page_size = GetPageSize(MEMTYPE_VRAM);
    if (page_size <= 0)
        page_size = 2048;
    p->sc.sc_NumBitmapPages = (uint32)((p->width * p->height * 2 + page_size - 1) / page_size);

    err = InitControlPad(1);
    if (err < 0)
        printf("WARNING: InitControlPad failed (0x%lx)\n", (unsigned long)err);

    p->vbl_ior = GetVBLIOReq();
    if (p->vbl_ior < 0)
        printf("WARNING: GetVBLIOReq failed (0x%lx)\n", (unsigned long)p->vbl_ior);
    p->vram_ior = GetVRAMIOReq();
    if (p->vram_ior < 0)
        printf("WARNING: GetVRAMIOReq failed (0x%lx)\n", (unsigned long)p->vram_ior);

    p->back = 0;
    p->displayed = -1;
    for (i = 0; i < p->num_screens; i++)
        plat_clear(p, i);
    if (p->num_screens >= 3)
        presenter_start(p);
    else
        p->thread = -1;

    printf("Display: %s, bitmap %ldx%ld, %lu VRAM pages per screen, %ld screens, "
           "presenter thread %s\n",
           p->is_pal ? "PAL 50 Hz" : "NTSC 60 Hz",
           (long)p->width, (long)p->height,
           (unsigned long)p->sc.sc_NumBitmapPages, (long)p->num_screens,
           p->thread >= 0 ? "running" : "absent");
    return 0;
}

void plat_shutdown(platform *p)
{
    presenter_stop(p);
    KillControlPad();
    DeleteBasicDisplay(&p->sc);
}

static uint32 usec_sample(void)
{
    TimeVal tv;

    SampleSystemTimeTV(&tv);
    return (uint32)tv.tv_sec * 1000000u + (uint32)tv.tv_usec;
}

uint32 plat_usec_now(void)
{
    uint32 a = usec_sample();
    uint32 b;

    /* A sample is occasionally off by about a second (seconds and
     * microseconds not read together): two samples taken in a row must
     * agree, else sample again. */
    for (;;) {
        b = usec_sample();
        if (b - a < 1000u)
            return b;
        a = b;
    }
}

Item plat_bitmap(const platform *p, int32 screen)
{
    return p->sc.sc_BitmapItems[screen];
}

void plat_fill(const platform *p, int32 screen, uint32 rgb15)
{
    if (p->vram_ior < 0)
        return;
    SetVRAMPages(p->vram_ior, p->sc.sc_Bitmaps[screen]->bm_Buffer,
                 (int32)((rgb15 << 16) | rgb15), (int32)p->sc.sc_NumBitmapPages,
                 (int32)0xFFFFFFFF);
}

void plat_clear(const platform *p, int32 screen)
{
    plat_fill(p, screen, 0);
}

void plat_set_clip(const platform *p, int32 x, int32 y, int32 w, int32 h)
{
    int32 i;

    /* The origin must be set within the current window: shrink first. */
    for (i = 0; i < p->num_screens; i++) {
        Item bm = p->sc.sc_BitmapItems[i];

        SetClipWidth(bm, w);
        SetClipHeight(bm, h);
        SetClipOrigin(bm, x, y);
    }
}

void plat_reset_clip(const platform *p)
{
    int32 i;

    for (i = 0; i < p->num_screens; i++) {
        Item bm = p->sc.sc_BitmapItems[i];

        SetClipOrigin(bm, 0, 0);
        SetClipWidth(bm, p->width);
        SetClipHeight(bm, p->height);
    }
}

void plat_text(const platform *p, int32 screen, int32 x, int32 y,
               const char *text, Color color)
{
    GrafCon gc;

    memset(&gc, 0, sizeof(gc));
    SetFGPen(&gc, color);
    MoveTo(&gc, x, y);
    DrawText8(&gc, p->sc.sc_BitmapItems[screen], (const uint8 *)text);
}

/* Is screen s neither displayed nor queued? */
static int32 screen_free(const platform *p, int32 s)
{
    uint32 k;

    if (s == p->displayed)
        return 0;
    for (k = p->q_head; k != p->q_tail; k++) {
        if (p->queue[k & (PLAT_QUEUE_LEN - 1)] == s)
            return 0;
    }
    return 1;
}

void plat_present(platform *p)
{
    DisplayScreen(p->sc.sc_ScreenItems[p->back], 0);
    if (p->vbl_ior >= 0)
        WaitVBL(p->vbl_ior, 1);
    p->displayed = p->back;
    p->back = (p->back + 1) % p->num_screens;
}

int32 plat_has_presenter(const platform *p)
{
    return p->thread >= 0;
}

void plat_queue(platform *p)
{
    p->queue[p->q_tail & (PLAT_QUEUE_LEN - 1)] = p->back;
    p->q_tail++;
    SendSignal(p->thread_task, (uint32)p->thread_sig);
}

int32 plat_free_screen(const platform *p)
{
    int32 s;

    for (s = 0; s < p->num_screens; s++) {
        if (screen_free(p, s))
            return 1;
    }
    return 0;
}

void plat_next_screen(platform *p)
{
    for (;;) {
        int32 s;

        for (s = 0; s < p->num_screens; s++) {
            if (screen_free(p, s)) {
                p->back = s;
                return;
            }
        }
        WaitSignal((uint32)p->free_sig);
    }
}

uint32 plat_next_vbl_us(const platform *p)
{
    return p->shown_us + p->frame_us;
}

void plat_drain(platform *p)
{
    if (p->thread < 0)
        return;
    while (p->q_head != p->q_tail)
        WaitSignal((uint32)p->free_sig);
    p->back = (p->displayed + 1) % p->num_screens;
}

void plat_wait_vbl(const platform *p)
{
    if (p->vbl_ior >= 0)
        WaitVBL(p->vbl_ior, 1);
}

void plat_mem_free(uint32 *dram, uint32 *vram)
{
    MemInfo info;

    AvailMem(&info, MEMTYPE_DRAM);
    *dram = info.minfo_SysFree + info.minfo_TaskFree;
    AvailMem(&info, MEMTYPE_VRAM);
    *vram = info.minfo_SysFree + info.minfo_TaskFree;
}

uint32 plat_pad_pressed(void)
{
    uint32 buttons = 0;

    if (DoControlPad(1, &buttons, 0) < 0)
        buttons = 0;
    return buttons;
}

uint32 plat_pad_state(void)
{
    uint32 buttons = 0;

    /* Every button reported while held. */
    if (DoControlPad(1, &buttons, (int32)PLAT_ALL_BUTTONS) < 0)
        buttons = 0;
    return buttons;
}

uint32 *plat_pixels(const platform *p, int32 screen)
{
    return (uint32 *)p->sc.sc_Bitmaps[screen]->bm_Buffer;
}
