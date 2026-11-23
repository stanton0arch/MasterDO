/*
 * The cartridge runs on the Z80 translator with the VDP core, and its
 * picture is drawn every frame with the CEL engine (background bitmap
 * pieces and sprite cels, render.c). The spare time at the end of a frame
 * goes to the translation of code the game is about to reach.
 *
 * Boot: display, a measure of the system clock calls (logged), the ROM
 * menu (a browser of the roms directory's folders), the cartridge image,
 * then the code reachable in the cartridge is translated
 * and the game runs at the pace of the display: one emulated frame, the
 * picture update, DrawCels into a free screen, which is queued for the
 * presenter thread (platform.c) to show at a VBL; the emulation may thus
 * run a frame ahead of the display. game.c records the emulation, update
 * and drawing times, and the frames where the pad changed, written out
 * at the debug pause (C) or when the emulation stops (the pad changes as
 * a pad script that the cycle model replays). Game Gear images (the .gg
 * files, or a Game Gear region in the header) run in the Game Gear mode:
 * the LCD window of 160 x 144 pixels in a black case, 12-bit colours, P
 * held as the START button. Pad during
 * the run: D-pad, A (button 1) and B (button 2) for the SMS pad, P for
 * PAUSE (START on the Game Gear); the second 3DO pad of the chain is the
 * second SMS pad (P PAUSE too), read once the game has read port $DD,
 * where most of it is (none on the Game Gear); L saves the machine's
 * state in the NVRAM and R puts it back (state.c: one state for the console, kept
 * across a power-off, refused for another cartridge image), the screens
 * not on display lending their memory to the packing; C pauses the run
 * and shows the reference picture drawn by the CPU (debug view) to
 * compare with the cels (A: reference, B: cels, R: presentation between
 * the queue, where a presenter thread shows the finished screens one per
 * VBL with the emulation working a frame ahead, and the direct mode,
 * present and wait for the VBL after each frame; C: resume), and writes
 * the run's records and summary to the debug log; STOP (X) pauses the run on a black
 * screen naming the image and the display, where STOP or P resumes it
 * and C goes back to the menu. The PSG
 * registers of each frame drive voices on the DSP (psg.c), silenced
 * whenever the run pauses or ends. The cartridge's battery RAM is kept
 * in a file of the NVRAM (save.c): read when the RAM is given, written
 * when a look between frames finds a save the game has finished, and
 * when the run pauses or ends.
 *
 * On a 50 Hz (PAL) display the machine still runs 60 frames a second, as
 * on an NTSC console: the frames follow the clock, one per NTSC period
 * (the main task sleeps on the timer device between them), while the
 * presenter thread shows one picture per VBL; the picture of a frame that
 * starts while the presentation queue is full is not drawn, one in six.
 * The games and their music keep their speed, the pictures shown are
 * spaced unevenly in game time (a regular judder on scrolls).
 *
 * The release build (MASTERDO_DEBUG 0, options.h) has no debug pause: C
 * does nothing, no record of the run is written to the log, the clock is
 * not measured at start-up, and the banner names the version.
 */

#include "options.h"
#include "platform.h"
#include "rom.h"
#include "game.h"
#include "dbgview.h"
#include "save.h"
#include "state.h"

#include "stdio.h"      /* SDK header: plain sprintf, not the compiler built-in */
#include "mem.h"
#include "string.h"
#include "event.h"

#define MENU_LINES  20          /* entries of a folder shown at once */
#define MENU_COLS   39          /* characters of a menu line */
#define MENU_REPEAT 16          /* a direction held repeats after these loop turns */

#define TEXT_X 8
#define TEXT_Y 4

#define COLOR_TITLE MakeRGB15(31, 31, 8)
#define COLOR_HEAD  MakeRGB15(12, 24, 31)
#define COLOR_TEXT  MakeRGB15(28, 28, 28)
#define COLOR_GOOD  MakeRGB15(8, 31, 8)
#define COLOR_BAD   MakeRGB15(31, 8, 8)

#define NO_COLOR    0xFFFFFFFFu

#define DRAW_ERR_LOGS   20      /* DrawCels errors logged one by one */
#define NOTE_FRAMES     150     /* a note over the picture: about 2.5 s */
#define STATE_VRAM_KEEP (32 * 1024) /* VRAM a savestate leaves to the system */

typedef struct {
    rom_place          place;       /* the menu's folder and entry */
    rom_image          rom;
    int32              have_game;
    uint32             dram_free;   /* before the cartridge */
    uint32             vram_free;
    uint32             run_free;    /* DRAM free during the run */
    uint32             run_vram;    /* VRAM free during the run, or after a failed load */
    int32              pic_x;       /* picture position on screen (y even) */
    int32              pic_y;
    uint32             pic_w;       /* picture shown: 256 pixels, or 160 (Game Gear) */
    uint32             pic_h;       /* its height: 192, 224 or 240 lines, or 144 */
    uint32             gg;          /* the cartridge runs in the Game Gear mode */
    uint32             border[PLAT_NUM_SCREENS];  /* colour each screen is filled with */
    uint32             direct;      /* presentation without the queue */
    uint32             pal60;       /* 50 Hz display: 60 frames a second on the clock */
    uint32             dropped;     /* the run's pictures not shown (pal60) */
    uint32             late;        /* its frames started over a period late (pal60) */
    uint32             draw_errors;
    const char        *note;        /* told over the next pictures, or NULL */
    uint32             note_left;   /* frames it stays */
    Color              note_color;
} app_state;

/* The run holds the translator state, the application its lists and
 * results: both kept off the small task stack. */
static game g;
static app_state st;

/* The PSG voices on the DSP, opened once for the whole session. */
static psg_dsp snd;

/* The NVRAM file of the cartridge's battery RAM. */
static save_file sv;

static void text_line(const platform *p, int32 screen, int32 line, Color color,
                      const char *text)
{
    plat_text(p, screen, TEXT_X, TEXT_Y + line * PLAT_LINE_H, text, color);
}

static void show_message(platform *p, const char *text)
{
    plat_clear(p, p->back);
    text_line(p, p->back, 0, COLOR_TITLE, "MASTERDO");
    text_line(p, p->back, 2, COLOR_TEXT, text);
    plat_present(p);
}

/* Copies a name as shown: underscores as spaces. */
static char *shown(char *to, const char *name)
{
    char *t = to;

    do {
        *t = (*name == '_') ? ' ' : *name;
        t++;
    } while (*name++ != 0);
    return to;
}

/* SMS pad bits for the 3DO buttons held. */
static uint32 sms_buttons(uint32 held)
{
    uint32 b = 0;

    if (held & ControlUp)    b |= SMS_PAD_UP;
    if (held & ControlDown)  b |= SMS_PAD_DOWN;
    if (held & ControlLeft)  b |= SMS_PAD_LEFT;
    if (held & ControlRight) b |= SMS_PAD_RIGHT;
    if (held & ControlA)     b |= SMS_PAD_1;
    if (held & ControlB)     b |= SMS_PAD_2;
    return b;
}

/*--------------------------------------------------------------------------
 * Run
 *------------------------------------------------------------------------*/

#if MASTERDO_DEBUG
static void log_vdp(const char *what)
{
    const vdp_state *v = &g.sms->vdp;
    uint32 k;

    printf("View: %s at frame %lu, VDP registers", what, (unsigned long)g.frame);
    for (k = 0; k < 11; k++)
        printf(" %02x", v->reg[k]);
    printf(", address $%04lx code %lu, status $%02lx, CRAM", (unsigned long)vdp_addr(v),
           (unsigned long)vdp_code(v), (unsigned long)v->status);
    if (v->gg) {
        for (k = 0; k < VDP_GG_CRAM; k += 2)
            printf(" %03lx", (unsigned long)(v->gg_cram[k] | ((uint32)v->gg_cram[k + 1] << 8)));
        printf(", ports $00-$06");
        for (k = 0; k < 7; k++)
            printf(" %02lx", (unsigned long)g.sms->gg_port[k]);
    } else {
        for (k = 0; k < 32; k++)
            printf(" %02x", v->cram[k]);
    }
    printf(", %lu sprites, backdrop $%04lx, %lu lines\n", (unsigned long)g.rd->n_sprites,
           (unsigned long)render_backdrop(g.rd), (unsigned long)v->active);
    /* The writes made during the active display of the frame (the bands
     * of the picture): a raster effect's scroll per line, a name table
     * switch, the display turned off or on, each with the first line it
     * affects. */
    printf("View: %lu scroll writes", (unsigned long)v->hs_n);
    for (k = 0; k < v->hs_n; k++)
        printf(" %lu:%lu", (unsigned long)(v->hs_log[k] >> 8), (unsigned long)(v->hs_log[k] & 0xFF));
    printf(", %lu name table writes", (unsigned long)v->nt_n);
    for (k = 0; k < v->nt_n; k++)
        printf(" %lu:%02lx", (unsigned long)(v->nt_log[k] >> 8), (unsigned long)(v->nt_log[k] & 0xFF));
    printf(", display %s at start, %lu changes", v->de_start ? "on" : "off",
           (unsigned long)v->de_n);
    for (k = 0; k < v->de_n; k++)
        printf(" %lu:%s", (unsigned long)(v->de_log[k] >> 8), (v->de_log[k] & 1) ? "on" : "off");
    printf(", %lu colour writes", (unsigned long)v->cr_n);
    for (k = 0; k < v->cr_n; k++) {
        uint32 e = v->cr_log[k];

        if (v->gg)
            printf(" %lu:%02lx=%03lx", (unsigned long)(e >> 20), (unsigned long)((e >> 12) & 31),
                   (unsigned long)(e & 0xFFF));
        else
            printf(" %lu:%02lx=%02lx", (unsigned long)(e >> 16), (unsigned long)((e >> 8) & 31),
                   (unsigned long)(e & 0x3F));
    }
    printf("\n");
}
#endif

/* The picture on the screens: centred, with the size of the part shown
 * (the 256-pixel picture of the display mode, 192, 224 or 240 lines, or
 * the 160 x 144 LCD window of the Game Gear), the clip window of every
 * screen set to it (the cels are positioned in it). */
static void set_picture(platform *p, app_state *st, uint32 w, uint32 h)
{
    st->pic_w = w;
    st->pic_h = h;
    st->pic_x = (p->width - (int32)w) / 2;
    st->pic_y = ((p->height - (int32)h) / 2) & ~1;
    if (st->pic_y < 0)
        st->pic_y = 0;
    plat_reset_clip(p);
    plat_set_clip(p, st->pic_x, st->pic_y, (int32)w, (int32)h);
}

/* The colour around the picture: the backdrop on the Master System, the
 * black case of the LCD on the Game Gear. */
static uint32 border_colour(const app_state *st)
{
    return st->gg ? 0 : render_backdrop(g.rd);
}

/* Fills the back screen with the border colour when it does not hold it
 * already; force redraws it anyway (display off: the picture goes). */
static void fill_border(platform *p, app_state *st, uint32 force)
{
    uint32 bd = border_colour(st);

    if (force || st->border[p->back] != bd) {
        plat_fill(p, p->back, bd);
        st->border[p->back] = bd;
    }
}

/* Draws the cels of the current picture into the back screen; returns
 * the drawing time, from the end of the picture update (the last look at
 * the clock: the border fill, when its colour changes, is counted with
 * the drawing). */
static uint32 draw_picture(platform *p, app_state *st)
{
    uint32 t0 = g.upd_end_us;
    uint32 dt;
    Err err;

    if (!render_display_on(g.rd) && !st->gg) {
        fill_border(p, st, 1);
        return 0;
    }
    fill_border(p, st, 0);
    /* Game Gear with no line displayed: the LCD window in the backdrop
     * colour, the case left black. */
    err = DrawCels(plat_bitmap(p, p->back), render_display_on(g.rd) ? render_chain(g.rd)
                                                                     : render_blank_window(g.rd));
    dt = plat_usec_now() - t0;
    if (err < 0) {
        /* The screen may hold part of a picture: it is not shown again. */
        render_forget_picture(g.rd);
        game_note_draw_error(&g);
        if (st->draw_errors++ < DRAW_ERR_LOGS)
            printf("Picture: frame %lu: DrawCels returned an error (0x%lx) after %lu us, "
                   "%lu sprites\n", (unsigned long)g.frame, (unsigned long)err,
                   (unsigned long)dt, (unsigned long)g.rd->n_sprites);
    }
    return dt;
}

#if MASTERDO_DEBUG
/* Paused view: the reference picture drawn by the CPU (A) or the cels
 * (B), with the palette under it; R switches the presentation mode;
 * returns the buttons held when it ends, with ControlX set when the run
 * must end. */
static uint32 show_snapshot(platform *p, app_state *st, uint32 prev)
{
    const vdp_state *v = &g.sms->vdp;
    uint32 which = 0;       /* 0 reference, 1 cels */
    uint32 redraw = 1;
    uint32 no_mem = 0;      /* no memory for the reference picture */
    char buf[80];

    log_vdp("paused");
    game_log_state(&g);
    for (;;) {
        uint32 held;
        uint32 pressed;

        if (redraw) {
            uint32 t0 = plat_usec_now();
            uint32 dt;

            redraw = 0;
            plat_reset_clip(p);
            plat_fill(p, p->back, border_colour(st));
            if (which == 0) {
                no_mem = dbgview_draw(v, plat_pixels(p, p->back), p->width, st->pic_x,
                                      st->pic_y) != 0;
                dt = plat_usec_now() - t0;
            } else {
                set_picture(p, st, st->pic_w, st->pic_h);
                if (render_display_on(g.rd))
                    DrawCels(plat_bitmap(p, p->back), render_chain(g.rd));
                else if (st->gg)
                    DrawCels(plat_bitmap(p, p->back), render_blank_window(g.rd));
                dt = plat_usec_now() - t0;
                plat_reset_clip(p);
            }
            /* The colour RAM under the picture, when there is room. */
            if (st->pic_y + (int32)st->pic_h + 4 + 16 <= p->height)
                dbgview_palette(v, plat_pixels(p, p->back), p->width, st->pic_x,
                                st->pic_y + (int32)st->pic_h + 4);
            sprintf(buf, "PAUSED FRAME %lu: %s   A/B: VIEW  C: RESUME  STOP: MENU",
                    (unsigned long)g.frame,
                    which == 1 ? "CELS" : no_mem ? "NO MEMORY" : "CPU REFERENCE");
            text_line(p, p->back, 0, COLOR_TITLE, buf);
            plat_present(p);
            if (which == 0 && no_mem)
                printf("View: no memory for the reference picture\n");
            else
                printf("View: %s drawn in %lu us\n", which == 0 ? "reference" : "cels",
                       (unsigned long)dt);
        }
        held = plat_pad_state();
        pressed = held & ~prev;
        prev = held;
        if (pressed & ControlX)
            return held | ControlX;
        if (pressed & ControlC)
            return held;
        if (pressed & ControlRightShift) {
            st->direct = !st->direct;
            printf("Present: frame %lu: %s\n", (unsigned long)g.frame,
                   (plat_has_presenter(p) && !st->direct) ? "queued (presenter thread)" :
                   "direct (present, then wait for the VBL)");
        }
        if ((pressed & ControlA) && which != 0) {
            which = 0;
            redraw = 1;
        }
        if ((pressed & ControlB) && which != 1) {
            which = 1;
            redraw = 1;
        }
        plat_wait_vbl(p);
    }
}
#endif

static void reset_borders(app_state *st)
{
    int32 i;

    for (i = 0; i < PLAT_NUM_SCREENS; i++)
        st->border[i] = NO_COLOR;
}

/* The screen of STOP: black, the image's name and size and the display;
 * STOP or P resumes the run (returns 0, once both are released: STOP
 * would open this screen again, P is also the machine's PAUSE, or START
 * held on the Game Gear), C leaves it (returns 1), the other buttons do
 * nothing. With a problem (the image not loaded, the run stopped), the
 * problem instead of the resume line, and only C. */
static uint32 stop_screen(platform *p, const app_state *st, const char *problem)
{
    int32 s = p->back;
    uint32 prev = plat_pad_state();
    char name[ROM_NAME_LEN];
    char buf[64];

    plat_reset_clip(p);
    plat_clear(p, s);
    text_line(p, s, 0, COLOR_TITLE, "MASTERDO");
    sprintf(buf, "%s : %luK", shown(name, st->place.name),
            (unsigned long)((st->place.size + 1023) / 1024));
    text_line(p, s, 1, COLOR_TEXT, buf);
    sprintf(buf, "%s %ldX%ld", p->is_pal ? "PAL" : "NTSC", (long)p->width, (long)p->height);
    text_line(p, s, 2, COLOR_TEXT, buf);
    text_line(p, s, 4, problem != NULL ? COLOR_BAD : COLOR_HEAD,
              problem != NULL ? problem : "STOP OR P: RESUME THE GAME");
    text_line(p, s, 5, COLOR_HEAD, "C: BACK TO THE MASTERDO MAIN MENU");
    plat_present(p);
    for (;;) {
        uint32 held = plat_pad_state();
        uint32 pressed = held & ~prev;

        prev = held;
        if (pressed & ControlC)
            return 1;
        if ((pressed & (ControlX | ControlStart)) && problem == NULL) {
            while (plat_pad_state() & (ControlX | ControlStart))
                plat_wait_vbl(p);
            return 0;
        }
        plat_wait_vbl(p);
    }
}

/* Between frames: the machine's state saved to the NVRAM, or loaded
 * from it, with a word on the screen on display while it works (a few
 * seconds to save) and the result told over the next pictures. The
 * screens not on display lend their memory, and to a save the VRAM left
 * free beyond STATE_VRAM_KEEP; they are filled again before being drawn
 * into. */
static void state_op(platform *p, app_state *st, uint32 load)
{
    stp_mem m;
    uint32 dram = 0;
    uint32 vram = 0;
    uint32 more = 0;
    uint8 *extra = NULL;
    int32 i;
    int32 r;

    plat_drain(p);
    psg_mute(&snd);
    plat_reset_clip(p);
    if (p->displayed >= 0)
        plat_text(p, p->displayed, st->pic_x + 4, st->pic_y + 14,
                  load ? "LOADING STATE..." : "SAVING STATE...", COLOR_TITLE);
    memset(&m, 0, sizeof(m));
    for (i = 0; i < p->num_screens; i++) {
        if (i != p->displayed)
            stp_lend(&m, plat_pixels(p, i), (uint32)(p->width * p->height * 2));
    }
    if (!load) {
        plat_mem_free(&dram, &vram);
        if (vram > STATE_VRAM_KEEP + 16 * 1024) {
            more = vram - STATE_VRAM_KEEP;
            extra = (uint8 *)AllocMem((int32)more, MEMTYPE_VRAM);
            if (extra != NULL)
                stp_lend(&m, extra, more);
        }
    }
    r = load ? state_load(&g, &m) : state_save(&g, &m);
    if (extra != NULL)
        FreeMem(extra, (int32)more);
    st->note_left = NOTE_FRAMES;
    st->note_color = (r < 0) ? COLOR_BAD : COLOR_GOOD;
    st->note = (r >= 0) ? (load ? "STATE LOADED" : "STATE SAVED") :
               (r == STATE_NONE || r == STATE_OTHER) ? "NO STATE SAVED FOR THIS GAME" :
               r == STATE_FULL ? "STATE: NVRAM FULL" :
               r == STATE_NOMEM ? "STATE: NO MEMORY" :
               r == STATE_LARGE ? "STATE TOO LARGE" :
               r == STATE_BAD ? "STATE DAMAGED" : "STATE: NVRAM ERROR";
    /* The screens lent hold garbage: filled before being drawn into, and
     * no picture shown again from them. */
    reset_borders(st);
    set_picture(p, st, render_win_w(g.rd), render_win_h(g.rd));
    render_forget_picture(g.rd);
}

#if MASTERDO_DEBUG
/* 50 Hz display: the frames emulated since the reset, the pictures not
 * shown and the frames that started over a period late (the clock then
 * followed from there: game time lost), for the log. */
static void log_pace(const app_state *st)
{
    if (st->pal60)
        printf("Pace: %lu frames emulated on the clock, %lu pictures not shown, "
               "%lu frames started over a period late\n", (unsigned long)g.frame,
               (unsigned long)st->dropped, (unsigned long)st->late);
}

/* The run's records and summary for the log (debug pause), the window
 * of measures in progress left open: the summary closes the current
 * window unless it holds no frame, and a window closed early would put
 * the next ones off the 100-frame grid that the cycle model's follow. */
static void log_run(const app_state *st)
{
    uint32 frames = g.win.frames;

    g.win.frames = 0;
    game_log_summary(&g);
    g.win.frames = frames;
    log_pace(st);
}
#endif

/* The frame loop looks at the clock (a kernel call of tens of
 * microseconds on the console) at the end of the pad read, which starts
 * the emulation, at the ends of the emulation, of the picture update
 * (the start of the drawing) and of the drawing, and in the spare time.
 * A frame starts at the loop's last look: the end of the previous
 * frame's spare time, or the presentation that freed the screen the loop
 * waited for (the presenter thread looks at the clock then). On a 50 Hz
 * display a frame starts at its time on the clock (the end of the sleep
 * before it), or at the last look when it starts late. */
static void run_game(platform *p, app_state *st)
{
    uint32 prev = plat_pad_state();
    uint32 prev2 = 0;                   /* second pad */
    uint32 shown = plat_usec_now();     /* direct mode: return of the last presentation */
    uint32 last = shown;                /* the loop's last look at the clock */
    uint32 next = shown;                /* 50 Hz display: the frame's time on the clock */
    uint32 longs = p->long_presents;    /* queued mode: long presents seen */
    uint32 queued = plat_has_presenter(p) && !st->direct;
    char buf[48];

    reset_borders(st);
    st->draw_errors = 0;
    st->note_left = 0;
    st->dropped = 0;
    st->late = 0;
    set_picture(p, st, render_win_w(g.rd), render_win_h(g.rd));
    render_forget_picture(g.rd);
    for (;;) {
        uint32 t0 = last;
        uint32 held = plat_pad_state();
        uint32 held2 = (g.sms->pad2_read && !st->gg) ? plat_pad2_state() : 0;
        uint32 pressed = held & ~prev;
        uint32 pause;
        uint32 draw_us;
        uint32 again;
        uint32 drop = 0;                /* the frame's picture is not shown */
        uint32 fresh;                   /* it is drawn */
        uint32 now = plat_usec_now();

        game_pad_time(&g, now - t0);
        prev = held;
        if (pressed & ControlX) {
            plat_drain(p);
            psg_mute(&snd);
            game_save(&g);
            if (stop_screen(p, st, NULL))
                break;
            prev = plat_pad_state();
            reset_borders(st);
            set_picture(p, st, st->pic_w, st->pic_h);
            render_forget_picture(g.rd);
            shown = plat_usec_now();
            last = shown;
            next = shown;
            longs = p->long_presents;
            continue;
        }
        if (pressed & (ControlLeftShift | ControlRightShift)) {
            state_op(p, st, (pressed & ControlLeftShift) == 0);
            shown = plat_usec_now();
            last = shown;
            next = shown;
            longs = p->long_presents;
            continue;
        }
#if MASTERDO_DEBUG
        if (pressed & ControlC) {
            plat_drain(p);
            psg_mute(&snd);
            game_save(&g);
            log_run(st);
            prev = show_snapshot(p, st, prev);
            if (prev & ControlX)
                break;
            queued = plat_has_presenter(p) && !st->direct;
            reset_borders(st);
            set_picture(p, st, st->pic_w, st->pic_h);
            render_forget_picture(g.rd);
            shown = plat_usec_now();
            last = shown;
            next = shown;
            longs = p->long_presents;
            continue;
        }
#endif
        /* 50 Hz display: the queue takes one picture per VBL, 50 of the 60
         * frames a second; a frame that starts while it is full is not
         * shown (one in six, wherever the VBLs fall), and the screens never
         * hold the main task. In the direct mode, the sixth of every six. */
        if (st->pal60) {
            if (!queued)
                drop = (g.frame % 6) == 5;
            else if (plat_free_screen(p))
                plat_next_screen(p);
            else
                drop = 1;
        }

        /* P: the PAUSE button when pressed on either pad, or START while
         * held on the Game Gear. */
        pause = (((st->gg ? held : pressed) | (held2 & ~prev2)) & ControlStart) != 0;
        prev2 = held2;
        if (game_frame(&g, sms_buttons(held) | SMS_PAD2(sms_buttons(held2)), pause, now) != 0) {
            /* The emulation stopped: its records and summary go to the
             * log (a fault to report). */
            plat_drain(p);
            psg_mute(&snd);
            game_save(&g);
#if MASTERDO_DEBUG
            game_log_summary(&g);
            log_pace(st);
#endif
            sprintf(buf, "RUN STOPPED AT PC %04lX", (unsigned long)g.stop_pc);
            stop_screen(p, st, buf);
            break;
        }
        if (render_win_h(g.rd) != st->pic_h || render_win_w(g.rd) != st->pic_w) {
            /* The display mode changed height: the picture moves, and the
             * screens get their border again. */
            set_picture(p, st, render_win_w(g.rd), render_win_h(g.rd));
            reset_borders(st);
        }
        /* A picture identical to the previous one is not drawn: the screen
         * holding it is shown once more. */
        again = render_repeated(g.rd);
        if (drop) {
            /* The next picture repeats only a picture the screens hold. */
            if (!again)
                render_forget_picture(g.rd);
            st->dropped++;
        }
        fresh = !again && !drop;
        draw_us = fresh ? draw_picture(p, st) : 0;
        /* A save the NVRAM had no room for, or the result of a
         * savestate, told over the picture for a few seconds (a picture
         * shown again already holds it); when it ends, the screens get
         * their border again and the next picture is drawn. */
        if (g.save_note != 0) {
            g.save_note--;
            if (fresh)
                plat_text(p, p->back, 4, 4, "SAVE: NVRAM FULL", COLOR_BAD);
            if (g.save_note == 0) {
                reset_borders(st);
                render_forget_picture(g.rd);
            }
        }
        if (st->note_left != 0) {
            st->note_left--;
            if (fresh)
                plat_text(p, p->back, 4, 14, st->note, st->note_color);
            if (st->note_left == 0) {
                reset_borders(st);
                render_forget_picture(g.rd);
            }
        }
        game_frame_done(&g, draw_us);
        if (queued && p->errors != 0) {
            /* The thread cannot show the screens: back to the direct mode. */
            plat_drain(p);
            render_forget_picture(g.rd);
            queued = 0;
            st->direct = 1;
            printf("Present: frame %lu: the presenter thread fails, direct mode\n",
                   (unsigned long)g.frame);
        }
        if (queued && st->pal60) {
            /* 50 Hz display: the next frame's time is one NTSC period
             * later on the clock, wherever the VBLs fall. A frame that ends
             * past it is followed at once (later frames catch up), but a
             * frame that would start over a period late starts the clock
             * again from there. */
            if (!drop) {
                if (again)
                    plat_queue_again(p);
                else
                    plat_queue(p);
            }
            next += PLAT_NTSC_FRAME_US;
            last = game_spare_time(&g, next);
            if ((int32)(next - last) > 0) {
                plat_sleep(p, next - last);
                last = next;
            } else if ((int32)(last - next) > PLAT_NTSC_FRAME_US) {
                next = last;
                st->late++;
            }
            if (p->long_presents != longs) {
                longs = p->long_presents;
                game_note_long_vbl(&g, p->long_us);
            }
        } else if (queued) {
            uint32 deadline = t0 + p->frame_us;

            /* The finished screen goes to the presenter at once. When
             * no screen is free the main task would wait for the next
             * VBL anyway: that time goes to translation. */
            if (again)
                plat_queue_again(p);
            else
                plat_queue(p);
            if (!plat_free_screen(p)) {
                uint32 vbl = plat_next_vbl_us(p);

                if ((int32)(vbl - deadline) > 0)
                    deadline = vbl;
            }
            last = game_spare_time(&g, deadline);
            plat_next_screen(p);
            /* A presentation after the last look freed the screen waited
             * for: the next frame starts with it. */
            if ((int32)(p->shown_us - last) > 0)
                last = p->shown_us;
            if (p->long_presents != longs) {
                longs = p->long_presents;
                game_note_long_vbl(&g, p->long_us);
            }
        } else if (drop) {
            /* Direct mode, 50 Hz display: a frame not shown ends with its
             * update, and the next one follows. */
            last = g.upd_end_us;
        } else {
            game_spare_time(&g, t0 + p->frame_us);
            if (again)
                plat_present_again(p);
            else
                plat_present(p);
            now = plat_usec_now();
            if (now - shown >= PLAT_LONG_US)
                game_note_long_vbl(&g, now - shown);
            shown = now;
            last = now;
        }
    }
    plat_drain(p);
    psg_mute(&snd);
    plat_reset_clip(p);
    game_save(&g);
}

/* The display and the memory left, for the screens: the console has no
 * log, so this is where its figures are read. */
static void memory_line(platform *p, int32 screen, int32 line, uint32 dram, uint32 vram)
{
    char buf[80];

    sprintf(buf, "%s %ldx%ld %ld SCREENS, DRAM %luK VRAM %luK FREE",
            p->is_pal ? "PAL" : "NTSC", (long)p->width, (long)p->height,
            (long)p->num_screens, (unsigned long)(dram / 1024),
            (unsigned long)(vram / 1024));
    text_line(p, screen, line, COLOR_HEAD, buf);
}

/*--------------------------------------------------------------------------
 * ROM menu
 *
 * A browser of the roms directory: a folder's folders, then its images,
 * 20 lines a page. Up / Down one line (held: a repeat that speeds up),
 * Left / Right one page, L / R the previous / next initial letter, A or
 * START opens the folder or runs the image, B goes up, STOP (X) quits (at
 * the root). The folder's list is read when the folder is shown, into a block
 * freed before the image loads. Underscores are shown as spaces: the names
 * under the CD's root directory cannot hold spaces (the build's dependency
 * list would split them).
 *
 * At the root, when it holds only a few folders, each folder is shown as
 * a box, and a panel under them explains the region and translation
 * codes of the image names.
 *------------------------------------------------------------------------*/

#define HOME_MAX     3          /* root folders shown as boxes over the legend */
#define HOME_FILL    MakeRGB15(2, 5, 10)
#define HOME_FILL_CUR MakeRGB15(2, 9, 3)
#define BOX_X        8          /* boxes span the text's width */
#define BOX_W        304
#define BOX_Y        36         /* first folder box, under the folder line */
#define BOX_H        14
#define BOX_PITCH    18
#define LEGEND_H     140        /* under three folder boxes: ends at line 236 */
#define LEGEND_BAR   12         /* title bar */
#define LEGEND_PITCH 10         /* pixels between two lines of the panel */
#define LEGEND_FILL  MakeRGB15(1, 2, 6)
#define LEGEND_EDGE  MakeRGB15(6, 9, 15)
#define LEGEND_TITLE "ROM NOMENCLATURE"

/* The panel's lines, '\n' apart; bytes 1 to 3 choose the colour of what
 * follows: headings, codes, plain text. */
static const char legend[] =
    "\1REGION\n"
    "  \2j  \3Japan        \2b  \3Brazil\n"
    "  \2u  \3USA          \2k  \3Korea\n"
    "  \2e  \3Europe       \2w  \3World\n"
    "\n"
    "\3  Combinations are kept as-is:\n"
    "\2  ue, ju, ueb...\n"
    "\3  (USA, Europe) -> \2ue\n"
    "\n"
    "\1TRANSLATION\n"
    "\2  T_ \3+ 3-letter language code:\n"
    "\2  eng, fre, spa, ger, ita, bra, cat";

/* A box across the text's width: a 1-pixel edge around a fill, which
 * becomes the background of the text drawn next with gc. */
static void draw_box(Item bm, GrafCon *gc, int32 y, int32 h, Color edge, Color fill)
{
    Rect r;

    gc->gc_FGPen = edge;
    r.rect_XLeft = BOX_X;
    r.rect_YTop = y;
    r.rect_XRight = BOX_X + BOX_W;
    r.rect_YBottom = y + h;
    FillRect(bm, gc, &r);
    gc->gc_FGPen = fill;
    r.rect_XLeft = BOX_X + 1;
    r.rect_YTop = y + 1;
    r.rect_XRight = BOX_X + BOX_W - 1;
    r.rect_YBottom = y + h - 1;
    FillRect(bm, gc, &r);
    gc->gc_BGPen = fill;
}

static void draw_legend(Item bm, GrafCon *gc, int32 y)
{
    static const Color ink[3] = {
        MakeRGB15(10, 17, 25), MakeRGB15(25, 23, 10), MakeRGB15(20, 20, 20)
    };
    const char *c = legend;
    Rect r;

    /* The panel, the rule between its two sections (in the blank line
     * before TRANSLATION), the title bar and its title. */
    draw_box(bm, gc, y, LEGEND_H, LEGEND_EDGE, LEGEND_FILL);
    gc->gc_FGPen = LEGEND_EDGE;
    r.rect_XLeft = BOX_X + 8;
    r.rect_XRight = BOX_X + BOX_W - 8;
    r.rect_YTop = y + LEGEND_BAR + 5 + 8 * LEGEND_PITCH + 4;
    r.rect_YBottom = r.rect_YTop + 1;
    FillRect(bm, gc, &r);
    draw_box(bm, gc, y, LEGEND_BAR, LEGEND_EDGE, LEGEND_EDGE);
    gc->gc_FGPen = MakeRGB15(20, 23, 28);
    gc->gc_PenX = BOX_X + (BOX_W - (sizeof(LEGEND_TITLE) - 1) * PLAT_GLYPH_W) / 2;
    gc->gc_PenY = y + 2;
    DrawText8(gc, bm, (const uint8 *)LEGEND_TITLE);

    /* The lines, their background in the fill's colour; each character
     * drawn moves the pen past it. */
    gc->gc_BGPen = LEGEND_FILL;
    gc->gc_PenX = BOX_X + 8;
    gc->gc_PenY = y + LEGEND_BAR + 5;
    for (;;) {
        uint32 ch = (uint8)*c++;

        if (ch >= ' ') {
            DrawChar(gc, bm, ch);
        } else if (ch == '\n') {
            gc->gc_PenX = BOX_X + 8;
            gc->gc_PenY += LEGEND_PITCH;
        } else if (ch != 0) {
            gc->gc_FGPen = ink[ch - 1];
        } else {
            break;
        }
    }
}

static void draw_menu(platform *p, const app_state *st, const rom_dir *d, int32 cur)
{
    int32 s = p->back;
    int32 first = (cur / MENU_LINES) * MENU_LINES;
    int32 root = st->place.dir[0] == 0;
    int32 home = root && d->count > 0 && d->count <= HOME_MAX && d->folders == d->count;
    Item bm = plat_bitmap(p, s);
    GrafCon gc;
    int32 k;
    uint32 n;
    uint32 room;
    char pos[24];
    char buf[64];

    plat_clear(p, s);
    memset(&gc, 0, sizeof(gc));
#if Z80J_DIAG || Z80J_CHURN
    sprintf(buf, "MASTERDO DIAGNOSTIC %d A:OPEN %s", (int)Z80J_DIAG,
            root ? "STOP:QUIT" : "B:BACK");
#else
    sprintf(buf, "MASTERDO  A:OPEN  %s  L/R:LETTER", root ? "STOP:QUIT" : "B:BACK");
#endif
    text_line(p, s, 0, COLOR_TITLE, buf);
    memory_line(p, s, 1, st->dram_free, st->vram_free);

    /* The folder (its end when too long) and the position in it. */
    sprintf(pos, " %ld/%ld%s", (long)(d->count ? cur + 1 : 0), (long)d->count,
            d->left_out ? "+" : "");
    room = MENU_COLS - strlen(pos);
    n = 1 + strlen(st->place.dir);
    if (n <= room) {
        buf[0] = '/';
        shown(buf + 1, st->place.dir);
    } else {
        strcpy(buf, "...");
        shown(buf + 3, st->place.dir + (n - 1) - (room - 3));
        n = room;
    }
    while (n < room)
        buf[n++] = ' ';
    strcpy(buf + n, pos);
    text_line(p, s, 2, COLOR_HEAD, buf);

    for (k = first; k < d->count && k < first + MENU_LINES; k++) {
        uint32 size = rom_dir_size(d, k);
        Color c = COLOR_TEXT;
        char name[ROM_NAME_LEN];

        shown(name, rom_dir_name(d, k));
        if (size == ROM_FOLDER) {
            sprintf(buf, "%c %s/", k == cur ? '>' : ' ', name);
            c = COLOR_HEAD;
        } else {
            sprintf(buf, "%c %-31s %4luK", k == cur ? '>' : ' ', name,
                    (unsigned long)((size + 1023) / 1024));
        }
        if (k == cur)
            c = COLOR_GOOD;
        if (home) {
            int32 y = BOX_Y + k * BOX_PITCH;

            draw_box(bm, &gc, y, BOX_H, c, k == cur ? HOME_FILL_CUR : HOME_FILL);
            gc.gc_FGPen = c;
            gc.gc_PenX = BOX_X + 8;
            gc.gc_PenY = y + 3;
            DrawText8(&gc, bm, (const uint8 *)buf);
        } else {
            text_line(p, s, 3 + k - first, c, buf);
        }
    }
    if (d->count == 0)
        text_line(p, s, 3, COLOR_BAD, "  no folder or image here");
    if (home)
        draw_legend(bm, &gc, BOX_Y + d->count * BOX_PITCH + 6);
    plat_present(p);
}

static uint32 initial(const rom_dir *d, int32 k)
{
    uint32 c = (uint8)rom_dir_name(d, k)[0];

    return (c >= 'a' && c <= 'z') ? c - 'a' + 'A' : c;
}

/* The first entry of the run of entries that share entry k's initial. */
static int32 letter_start(const rom_dir *d, int32 k)
{
    uint32 c = initial(d, k);

    while (k > 0 && initial(d, k - 1) == c)
        k--;
    return k;
}

/* Reads the place's folder, back to the root when it cannot be read;
 * returns the entry of the place's name (0 when absent), or -1 when
 * not even the root can be read. */
static int32 menu_read(app_state *st, rom_dir *d)
{
    for (;;) {
        if (rom_dir_read(d, st->place.dir) >= 0)
            break;
        printf("Menu: cannot read " ROM_DIR "/%s\n", st->place.dir);
        if (st->place.dir[0] == 0)
            return -1;
        st->place.dir[0] = 0;
        st->place.name[0] = 0;
    }
    printf("Menu: /%s: %ld entries (%ld folders) read in %lu us, sorted in %lu us",
           st->place.dir, (long)d->count, (long)d->folders, (unsigned long)d->read_us,
           (unsigned long)d->sort_us);
    if (d->left_out)
        printf(", %ld left out (the list is full)", (long)d->left_out);
    printf("\n");
    return rom_dir_find(d, st->place.name);
}

/* Returns 1 with the image chosen in st->place (its folder, name and
 * size), 0 to quit, -1 when the roms directory cannot be read. */
static int32 rom_menu(platform *p, app_state *st)
{
    rom_dir d;
    rom_place *pl = &st->place;
    int32 cur;
    int32 res = -1;
    uint32 prev = plat_pad_state();
    uint32 hold = 0;
    uint32 held_dir = 0;

    memset(&d, 0, sizeof(d));
    d.mem = (uint8 *)AllocMem(ROM_LIST_BYTES, MEMTYPE_DRAM);
    if (d.mem == NULL) {
        printf("Menu: no memory for the list\n");
        return -1;
    }
    cur = menu_read(st, &d);
    if (cur >= 0)
        draw_menu(p, st, &d, cur);
    while (cur >= 0) {
        uint32 held = plat_pad_state();
        uint32 pressed = held & ~prev;
        uint32 moves = held & (ControlUp | ControlDown | ControlLeft | ControlRight);
        uint32 fresh = pressed;
        int32 was = cur;
        int32 last = d.count - 1;

        /* A direction held repeats, faster and faster. */
        if (moves != 0 && moves == held_dir) {
            hold++;
            if (hold >= MENU_REPEAT) {
                uint32 every = (hold >= 4 * MENU_REPEAT) ? 1 : (hold >= 2 * MENU_REPEAT) ? 2 : 4;

                if ((hold & (every - 1)) == 0)
                    pressed |= moves;
            }
        } else {
            hold = 0;
        }
        held_dir = moves;
        prev = held;

        if (pressed & ControlX) {
            if (pl->dir[0] == 0) {
                res = 0;
                break;
            }
        } else if (pressed & (ControlA | ControlStart)) {
            if (d.count > 0 && rom_dir_size(&d, cur) != ROM_FOLDER) {
                strcpy(pl->name, rom_dir_name(&d, cur));
                pl->size = rom_dir_size(&d, cur);
                res = 1;
                break;
            }
            if (d.count > 0) {
                const char *name = rom_dir_name(&d, cur);
                uint32 n = strlen(pl->dir);

                if (n + 1 + strlen(name) < ROM_DIR_LEN) {
                    if (n != 0)
                        pl->dir[n++] = '/';
                    strcpy(pl->dir + n, name);
                    pl->name[0] = 0;
                    cur = menu_read(st, &d);
                    was = -1;
                } else {
                    printf("Menu: %s/%s: path too long\n", pl->dir, name);
                }
            }
        } else if ((pressed & ControlB) && pl->dir[0] != 0) {
            /* Up, the cursor on the folder left. */
            char *slash = NULL;
            char *leaf = pl->dir;
            char *c;

            for (c = pl->dir; *c != 0; c++) {
                if (*c == '/')
                    slash = c;
            }
            if (slash != NULL)
                leaf = slash + 1;

            strcpy(pl->name, leaf);
            if (slash != NULL)
                *slash = 0;
            else
                pl->dir[0] = 0;
            cur = menu_read(st, &d);
            was = -1;
        } else if (d.count > 0) {
            if (pressed & ControlRightShift) {
                uint32 c = initial(&d, cur);

                while (cur < last && initial(&d, cur) == c)
                    cur++;
                if (initial(&d, cur) == c)
                    cur = 0;
            } else if (pressed & ControlLeftShift) {
                cur = letter_start(&d, cur);
                cur = letter_start(&d, cur > 0 ? cur - 1 : last);
            } else if (pressed & ControlUp) {
                if (cur > 0)
                    cur--;
                else if (fresh & ControlUp)
                    cur = last;
            } else if (pressed & ControlDown) {
                if (cur < last)
                    cur++;
                else if (fresh & ControlDown)
                    cur = 0;
            } else if (pressed & ControlLeft) {
                cur = (cur > MENU_LINES) ? cur - MENU_LINES : 0;
            } else if (pressed & ControlRight) {
                cur = (cur + MENU_LINES < last) ? cur + MENU_LINES : last;
            }
        }
        if (cur < 0)
            break;
        if (cur != was)
            draw_menu(p, st, &d, cur);
        else
            plat_wait_vbl(p);
    }
    /* Nothing of the list stays for the run. */
    FreeMem(d.mem, ROM_LIST_BYTES);
    ScavengeMem();
    return res;
}

int main(int argc, char **argv)
{
    platform plat;

    (void)argc;
    (void)argv;

#if Z80J_DIAG == 1
    printf("masterdo: DIAGNOSTIC BUILD 1: 1 KiB of code words stored per frame where the "
           "next block goes, never run\n");
#elif Z80J_DIAG == 2
    printf("masterdo: DIAGNOSTIC BUILD 2: a fresh 1 KiB routine written per frame in an "
           "empty code zone and run once\n");
#elif Z80J_DIAG == 3
    printf("masterdo: DIAGNOSTIC BUILD 3: 4 link branches of code run lately written again "
           "per frame\n");
#elif Z80J_DIAG == 4
    printf("masterdo: DIAGNOSTIC BUILD 4: 30 link branches of code run lately written again "
           "at once every 22 frames\n");
#elif Z80J_DIAG == 5
    printf("masterdo: DIAGNOSTIC BUILD 5: code run lately translated again in spare time, "
           "%d blocks per frame at most, each code zone emptied once its blocks have moved\n",
           (int)Z80J_CHURN);
#elif Z80J_DIAG == 6
    printf("masterdo: DIAGNOSTIC BUILD 6: no code written, about 2 ms of plain work per frame "
           "in spare time\n");
#elif Z80J_DIAG == 7
    printf("masterdo: DIAGNOSTIC BUILD 7: 4 link branches of code run lately undone per frame, "
           "30 at once every 22 frames, each linked again at its next run\n");
#elif Z80J_DIAG == 8
    printf("masterdo: DIAGNOSTIC BUILD 8: the translations of build 5, %d blocks per frame at "
           "most, their new code never run, no zone emptied by the move\n", (int)Z80J_CHURN);
#elif Z80J_DIAG
    printf("masterdo: DIAGNOSTIC BUILD 9: the move of build 5 at %d block per frame at most\n",
           (int)Z80J_CHURN);
#elif Z80J_CHURN
    printf("masterdo: DIAGNOSTIC BUILD: code run lately translated again in spare time, "
           "%d blocks per frame at most, each code zone emptied once its blocks have moved\n",
           (int)Z80J_CHURN);
#endif
#if MASTERDO_DEBUG
    /* The banner names the build by its latest changes (the oldest ones
     * dropped as new ones come: its bytes are DRAM the code buffer may
     * need). */
    printf("masterdo: line wait loop closed by a jump idled to the next line; "
           "interrupt pending at a HALT after EI taken after the HALT; code buffer "
           "cleared when a game starts\n");
#else
    printf("masterdo " MASTERDO_VERSION "\n");
#endif
    if (plat_init(&plat) < 0)
        return 1;
    psg_open(&snd);
    memset(&st, 0, sizeof(st));
    /* 50 Hz display: the 60 Hz machine paced by the clock (without a timer
     * request, one emulated frame per VBL, 17 % slow). */
    st.pal60 = plat.is_pal && plat.timer_ior >= 0;
    if (plat.is_pal)
        printf("Pace: %s\n", st.pal60 ? "50 Hz display, 60 emulated frames per second on the "
               "clock, the picture of one in six not shown" : "50 Hz display, one emulated "
               "frame per VBL");
    st.pic_x = (plat.width - RENDER_W) / 2;
    st.pic_y = ((plat.height - RENDER_H) / 2) & ~1;
    st.pic_w = RENDER_W;
    st.pic_h = RENDER_H;

#if MASTERDO_DEBUG
    plat_clock_bench();
#endif
    plat_mem_free(&st.dram_free, &st.vram_free);
    printf("Memory: %lu bytes of DRAM and %lu bytes of VRAM free before the cartridge\n",
           (unsigned long)st.dram_free, (unsigned long)st.vram_free);

    for (;;) {
        int32 pick = rom_menu(&plat, &st);
        char name[ROM_NAME_LEN];
        char buf[64];

        if (pick == 0)
            goto quit;
        if (pick < 0) {
            show_message(&plat, "No cartridge image in the " ROM_DIR " directory");
            plat_wait_vbl(&plat);
            for (;;) {
                if (plat_pad_pressed() & ControlX)
                    goto quit;
                plat_wait_vbl(&plat);
            }
        }
        sprintf(buf, "Loading %s...", shown(name, st.place.name));
        show_message(&plat, buf);
        st.have_game = 0;
        if (rom_load(&st.rom, st.place.dir, st.place.name, st.place.size) == 0)
            rom_log(&st.rom);
        /* The memory left after a failed load; replaced by the run's
         * figures once the game is set up. */
        plat_mem_free(&st.run_free, &st.run_vram);
        st.gg = st.rom.gg;
        if (st.rom.data != NULL && st.rom.size >= 0x4000 &&
            game_init(&g, st.rom.data, st.rom.banks, (uint32)st.rom.size, st.gg) == 0) {
            st.have_game = 1;
            game_set_name(&g, st.rom.name);
            game_set_sound(&g, snd.ok ? &snd : NULL);
            game_set_frame_us(&g, st.pal60 ? PLAT_NTSC_FRAME_US : plat.frame_us);
            save_init(&sv, st.rom.name);
            game_set_saves(&g, save_io, &sv);
            show_message(&plat, "Translating the cartridge code...");
            game_translate_rom(&g);
            plat_mem_free(&st.run_free, &st.run_vram);
            printf("Memory: %lu bytes of DRAM and %lu bytes of VRAM free during the run; "
                   "picture of %lux%lu at (%ld,%ld)\n",
                   (unsigned long)st.run_free, (unsigned long)st.run_vram,
                   (unsigned long)render_win_w(g.rd), (unsigned long)render_win_h(g.rd),
                   (long)((plat.width - (int32)render_win_w(g.rd)) / 2),
                   (long)(((plat.height - (int32)render_win_h(g.rd)) / 2) & ~1));
        }

        if (st.have_game) {
            game_reset(&g);
            run_game(&plat, &st);
            game_free(&g);
        } else {
            stop_screen(&plat, &st, st.rom.data == NULL ? "CANNOT LOAD THIS IMAGE"
                                                        : "CANNOT RUN THIS IMAGE");
        }
        rom_unload(&st.rom);
    }

quit:
    printf("masterdo: quit\n");
    psg_close(&snd);
    plat_shutdown(&plat);
    return 0;
}
