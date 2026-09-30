/*
 * Step 3b: the cartridge runs on the Z80 translator with the VDP core, and
 * its picture is drawn every frame with the CEL engine (background bitmap
 * pieces and sprite cels, render.c). The spare time at the end of a frame
 * goes to the translation of code the game is about to reach.
 *
 * Boot: display, cartridge image, the CPU and CEL benchmarks of steps 0
 * and 1 (logged), then the code reachable in the cartridge is translated
 * and the game runs at the pace of the display: one emulated frame, the
 * picture update, DrawCels into the back buffer, then the buffer swap at
 * the VBL. game.c logs the emulation, update and drawing times. Pad during
 * the run: D-pad, A (button 1) and B (button 2) for the SMS pad, P for
 * PAUSE; L pauses the run and shows the reference picture drawn by the CPU
 * (debug view) to compare with the cels (A: reference, B: cels, L: resume);
 * R toggles the presentation mode (every VBL, or locked to every second
 * VBL); X ends the run. Results go to the debug log and to the screen.
 */

#include "platform.h"
#include "rom.h"
#include "bench_cpu.h"
#include "bench_video.h"
#include "game.h"
#include "dbgview.h"

#include "stdio.h"      /* SDK header: plain sprintf, not the compiler built-in */
#include "string.h"
#include "event.h"

#define ROM_PATH "roms/rom.sms"

#define TEXT_X 8
#define TEXT_Y 4

#define COLOR_TITLE MakeRGB15(31, 31, 8)
#define COLOR_HEAD  MakeRGB15(12, 24, 31)
#define COLOR_TEXT  MakeRGB15(28, 28, 28)
#define COLOR_GOOD  MakeRGB15(8, 31, 8)
#define COLOR_BAD   MakeRGB15(31, 8, 8)

#define NO_COLOR    0xFFFFFFFFu

#define DRAW_ERR_LOGS   20      /* DrawCels errors logged one by one */
#define LONG_VBL_US     25000   /* a presentation of two VBLs or more */

typedef struct {
    rom_image          rom;
    bench_cpu_result   cpu;
    bench_video_result video;
    int32              have_video;
    int32              have_game;
    uint32             dram_free;   /* before the benchmarks */
    uint32             vram_free;
    uint32             run_free;    /* DRAM free during the run */
    int32              pic_x;       /* picture position on screen (y even) */
    int32              pic_y;
    uint32             border[PLAT_NUM_SCREENS];  /* colour each screen is filled with */
    uint32             locked;      /* presentation locked to every second VBL */
    uint32             draw_errors;
} app_state;

/* The run holds the translator state: kept off the small task stack. */
static game g;

static void text_line(const platform *p, int32 screen, int32 line, Color color,
                      const char *text)
{
    plat_text(p, screen, TEXT_X, TEXT_Y + line * PLAT_LINE_H, text, color);
}

static void show_message(platform *p, const char *text)
{
    plat_clear(p, p->back);
    text_line(p, p->back, 0, COLOR_TITLE, "MASTERDO - STEP 3B");
    text_line(p, p->back, 2, COLOR_TEXT, text);
    plat_present(p);
}

static uint32 percent_of(uint32 us, uint32 frame_us)
{
    return us * 100u / frame_us;
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

static void log_vdp(const char *what)
{
    const vdp_state *v = &g.sms->vdp;
    uint32 k;

    printf("View: %s at frame %lu, VDP registers", what, (unsigned long)g.frame);
    for (k = 0; k < 11; k++)
        printf(" %02x", v->reg[k]);
    printf(", address $%04lx code %lu, status $%02lx, CRAM", (unsigned long)v->addr,
           (unsigned long)(v->ctl & 3), (unsigned long)v->status);
    for (k = 0; k < 32; k++)
        printf(" %02x", v->cram[k]);
    printf(", %lu sprites, backdrop $%04lx\n", (unsigned long)g.rd->n_sprites,
           (unsigned long)render_backdrop(g.rd));
}

/* Fills the back screen with the backdrop colour when it does not hold it
 * already; force redraws it anyway (display off: the picture goes). */
static void fill_border(platform *p, app_state *st, uint32 force)
{
    uint32 bd = render_backdrop(g.rd);

    if (force || st->border[p->back] != bd) {
        plat_fill(p, p->back, bd);
        st->border[p->back] = bd;
    }
}

/* Draws the cels of the current picture into the back screen; returns
 * the DrawCels time. */
static uint32 draw_picture(platform *p, app_state *st)
{
    uint32 t0;
    uint32 dt;
    Err err;

    if (!render_display_on(g.rd)) {
        fill_border(p, st, 1);
        return 0;
    }
    fill_border(p, st, 0);
    t0 = plat_usec_now();
    err = DrawCels(plat_bitmap(p, p->back), render_chain(g.rd));
    dt = plat_usec_now() - t0;
    if (err < 0) {
        game_note_draw_error(&g);
        if (st->draw_errors++ < DRAW_ERR_LOGS)
            printf("Picture: frame %lu: DrawCels returned an error (0x%lx) after %lu us, "
                   "%lu sprites\n", (unsigned long)g.frame, (unsigned long)err,
                   (unsigned long)dt, (unsigned long)g.rd->n_sprites);
    }
    return dt;
}

/* Paused view: the reference picture drawn by the CPU (A) or the cels
 * (B), with the palette under it; returns the buttons held when it ends,
 * with ControlX set when the run must end. */
static uint32 show_snapshot(platform *p, app_state *st, uint32 prev)
{
    const vdp_state *v = &g.sms->vdp;
    uint32 which = 0;       /* 0 reference, 1 cels */
    uint32 redraw = 1;
    char buf[64];

    log_vdp("paused");
    for (;;) {
        uint32 held;
        uint32 pressed;

        if (redraw) {
            uint32 t0 = plat_usec_now();
            uint32 dt;

            redraw = 0;
            plat_reset_clip(p);
            plat_fill(p, p->back, render_backdrop(g.rd));
            if (which == 0) {
                dbgview_draw(v, plat_pixels(p, p->back), p->width, st->pic_x, st->pic_y);
                dt = plat_usec_now() - t0;
            } else {
                plat_set_clip(p, st->pic_x, st->pic_y, RENDER_W, RENDER_H);
                if (render_display_on(g.rd))
                    DrawCels(plat_bitmap(p, p->back), render_chain(g.rd));
                dt = plat_usec_now() - t0;
                plat_reset_clip(p);
            }
            dbgview_palette(v, plat_pixels(p, p->back), p->width, st->pic_x,
                            st->pic_y + RENDER_H + 4);
            sprintf(buf, "PAUSED FRAME %lu: %s   A/B: VIEW  L: RESUME  X: END",
                    (unsigned long)g.frame, which == 0 ? "CPU REFERENCE" : "CELS");
            text_line(p, p->back, 0, COLOR_TITLE, buf);
            plat_present(p);
            printf("View: %s drawn in %lu us\n", which == 0 ? "reference" : "cels",
                   (unsigned long)dt);
        }
        held = plat_pad_state();
        pressed = held & ~prev;
        prev = held;
        if (pressed & ControlX)
            return held | ControlX;
        if (pressed & ControlLeftShift)
            return held;
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

static void run_game(platform *p, app_state *st)
{
    uint32 prev = plat_pad_state();
    uint32 shown = plat_usec_now();     /* return of the last presentation */
    char buf[48];

    st->border[0] = st->border[1] = NO_COLOR;
    st->draw_errors = 0;
    plat_set_clip(p, st->pic_x, st->pic_y, RENDER_W, RENDER_H);
    for (;;) {
        uint32 t0 = plat_usec_now();
        uint32 held = plat_pad_state();
        uint32 pressed = held & ~prev;
        uint32 draw_us;
        uint32 now;

        game_pad_time(&g, plat_usec_now() - t0);
        prev = held;
        if (pressed & ControlX)
            break;
        if (pressed & ControlLeftShift) {
            game_log_flush(&g);
            prev = show_snapshot(p, st, prev);
            if (prev & ControlX)
                break;
            st->border[0] = st->border[1] = NO_COLOR;
            plat_set_clip(p, st->pic_x, st->pic_y, RENDER_W, RENDER_H);
            shown = plat_usec_now();
            continue;
        }
        if (pressed & ControlRightShift) {
            st->locked = !st->locked;
            printf("Present: frame %lu: %s\n", (unsigned long)g.frame,
                   st->locked ? "locked to every second VBL" : "every VBL");
        }

        if (game_frame(&g, sms_buttons(held), (pressed & ControlStart) != 0) != 0) {
            plat_reset_clip(p);
            sprintf(buf, "RUN STOPPED AT PC %04lX", (unsigned long)g.stop_pc);
            show_message(p, buf);
            break;
        }
        draw_us = draw_picture(p, st);
        game_frame_done(&g, draw_us);
        game_spare_time(&g, t0);
        plat_present(p);
        if (st->locked && plat_usec_now() - shown < LONG_VBL_US)
            plat_wait_vbl(p);
        now = plat_usec_now();
        if (now - shown >= LONG_VBL_US)
            game_note_long_vbl(&g, now - shown);
        shown = now;
    }
    plat_reset_clip(p);
    game_log_summary(&g);
}

/*--------------------------------------------------------------------------
 * Benchmarks and results
 *------------------------------------------------------------------------*/

static void run_benchmarks(platform *p, app_state *st)
{
    bench_video bv;

    bench_cpu_run(&st->cpu);
    bench_cpu_log(&st->cpu);
    memset(&bv, 0, sizeof(bv));
    if (bench_video_init(&bv, p) == 0) {
        st->have_video = 1;
        bench_video_run(&bv, p, &st->video);
        bench_video_log(&st->video);
        bench_video_free(&bv);
    }
}

static void draw_core(const platform *p, int32 screen, int32 line,
                      const char *name, const bench_z80_result *r)
{
    char buf[64];

    if (r->status != 0) {
        sprintf(buf, " %s stopped: %lX PC %04lX", name,
                (unsigned long)r->stop_op, (unsigned long)r->stop_pc);
        text_line(p, screen, line, COLOR_BAD, buf);
    } else {
        sprintf(buf, " %-12s %6lu us %3lu pct %s", name, (unsigned long)r->frame_us,
                (unsigned long)percent_of(r->frame_us, PLAT_NTSC_FRAME_US),
                r->check ? "OK" : "BAD");
        text_line(p, screen, line, r->check ? COLOR_TEXT : COLOR_BAD, buf);
    }
}

static void draw_results(platform *p, const app_state *st)
{
    char buf[64];
    int32 s = p->back;
    int32 n = 0;
    uint32 frames = g.frame ? g.frame : 1;

    plat_clear(p, s);
    text_line(p, s, n++, COLOR_TITLE, "MASTERDO - STEP 3B RESULTS");
    if (st->rom.size <= 0)
        sprintf(buf, "ROM not loaded (0x%lx)", (unsigned long)st->rom.size);
    else
        sprintf(buf, "ROM %ld bytes, %s, DRAM %luK free", (long)st->rom.size,
                p->is_pal ? "PAL" : "NTSC", (unsigned long)(st->run_free / 1024));
    text_line(p, s, n++, st->rom.size > 0 ? COLOR_TEXT : COLOR_BAD, buf);

    text_line(p, s, n++, COLOR_HEAD, "BENCHMARKS (Z80 PER NTSC FRAME)");
    draw_core(p, s, n++, "interpreted", &st->cpu.interp);
    draw_core(p, s, n++, "translated", &st->cpu.jit);
    if (st->have_video) {
        sprintf(buf, " tiles + sprites %lu us", (unsigned long)st->video.all_us);
        text_line(p, s, n++, COLOR_TEXT, buf);
    }

    text_line(p, s, n++, COLOR_HEAD, "CARTRIDGE RUN WITH CELS (US PER FRAME)");
    if (!st->have_game) {
        text_line(p, s, n++, COLOR_BAD, " not run");
    } else {
        uint32 all = g.all_total_us / frames;

        sprintf(buf, " code: %lu blocks, %lu busy loops", (unsigned long)g.rom_blocks,
                (unsigned long)g.jit.stats.busy_loops);
        text_line(p, s, n++, COLOR_TEXT, buf);
        if (g.status != 0) {
            sprintf(buf, " stopped: exit %ld at %04lX", (long)g.status,
                    (unsigned long)g.stop_pc);
            text_line(p, s, n++, COLOR_BAD, buf);
        }
        sprintf(buf, " %lu frames: emulation %lu, update %lu", (unsigned long)g.frame,
                (unsigned long)(g.total_us / frames), (unsigned long)(g.upd_total_us / frames));
        text_line(p, s, n++, COLOR_TEXT, buf);
        sprintf(buf, " draw %lu, total %lu = %lu pct",
                (unsigned long)(g.draw_total_us / frames), (unsigned long)all,
                (unsigned long)percent_of(all, PLAT_NTSC_FRAME_US));
        text_line(p, s, n++, all <= PLAT_NTSC_FRAME_US ? COLOR_GOOD : COLOR_BAD, buf);
        sprintf(buf, " worst %lu us, %lu over %d us", (unsigned long)g.max_all_us,
                (unsigned long)g.over, PLAT_NTSC_FRAME_US);
        text_line(p, s, n++, g.over == 0 ? COLOR_GOOD : COLOR_TEXT, buf);
    }
    text_line(p, s, n + 1, COLOR_HEAD, st->have_game ? "B: RUN AGAIN   X: QUIT" : "X: QUIT");
    plat_present(p);
}

int main(int argc, char **argv)
{
    platform plat;
    app_state st;
    uint32 prev;

    (void)argc;
    (void)argv;

    printf("masterdo step 3b: cartridge run with the picture drawn by the CEL engine, optimisation pass\n");
    if (plat_init(&plat) < 0)
        return 1;
    memset(&st, 0, sizeof(st));
    st.pic_x = (plat.width - RENDER_W) / 2;
    st.pic_y = ((plat.height - RENDER_H) / 2) & ~1;

    show_message(&plat, "Loading " ROM_PATH "...");
    if (rom_load(&st.rom, ROM_PATH) == 0)
        rom_log(&st.rom);
    plat_mem_free(&st.dram_free, &st.vram_free);
    printf("Memory: %lu bytes of DRAM and %lu bytes of VRAM free before the benchmarks\n",
           (unsigned long)st.dram_free, (unsigned long)st.vram_free);

    show_message(&plat, "Benchmarks of steps 0 and 1...");
    run_benchmarks(&plat, &st);

    if (st.rom.data != NULL && st.rom.size >= 0x4000 &&
        game_init(&g, st.rom.data, (uint32)st.rom.size) == 0) {
        uint32 vram;

        st.have_game = 1;
        show_message(&plat, "Translating the cartridge code...");
        game_translate_rom(&g);
        plat_mem_free(&st.run_free, &vram);
        printf("Memory: %lu bytes of DRAM free during the run; picture at (%ld,%ld)\n",
               (unsigned long)st.run_free, (long)st.pic_x, (long)st.pic_y);
    }

    for (;;) {
        if (st.have_game) {
            game_reset(&g);
            run_game(&plat, &st);
        }
        draw_results(&plat, &st);
        prev = plat_pad_state();
        for (;;) {
            uint32 held = plat_pad_state();
            uint32 pressed = held & ~prev;

            prev = held;
            if (pressed & ControlX)
                goto quit;
            if ((pressed & ControlB) && st.have_game)
                break;
            plat_wait_vbl(&plat);
        }
    }

quit:
    printf("masterdo: quit\n");
    if (st.have_game)
        game_free(&g);
    rom_unload(&st.rom);
    plat_shutdown(&plat);
    return 0;
}
