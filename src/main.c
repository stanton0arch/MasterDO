/*
 * The cartridge runs on the Z80 translator with the VDP core, and its
 * picture is drawn every frame with the CEL engine (background bitmap
 * pieces and sprite cels, render.c). The spare time at the end of a frame
 * goes to the translation of code the game is about to reach.
 *
 * Boot: display, cartridge image, the CPU and CEL benchmarks of steps 0
 * and 1 (logged), then the code reachable in the cartridge is translated
 * and the game runs at the pace of the display: one emulated frame, the
 * picture update, DrawCels into a free screen, which is queued for the
 * presenter thread (platform.c) to show at a VBL; the emulation may thus
 * run a frame ahead of the display. game.c records the emulation, update
 * and drawing times, and the frames where the pad changed, written out
 * when the run pauses or ends (the pad changes as a pad script that the
 * cycle model replays). Pad during
 * the run: D-pad, A (button 1) and B (button 2) for the SMS pad, P for
 * PAUSE; L pauses the run and shows the reference picture drawn by the CPU
 * (debug view) to compare with the cels (A: reference, B: cels, L: resume);
 * R toggles the presentation between the queue (a presenter thread shows
 * the finished screens one per VBL, the emulation working a frame ahead)
 * and the direct mode (present and wait for the VBL after each frame);
 * X ends the run. Results go to the debug log and to the screen.
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

#define MENU_LINES 20           /* ROM names shown at once */

#define TEXT_X 8
#define TEXT_Y 4

#define COLOR_TITLE MakeRGB15(31, 31, 8)
#define COLOR_HEAD  MakeRGB15(12, 24, 31)
#define COLOR_TEXT  MakeRGB15(28, 28, 28)
#define COLOR_GOOD  MakeRGB15(8, 31, 8)
#define COLOR_BAD   MakeRGB15(31, 8, 8)

#define NO_COLOR    0xFFFFFFFFu

#define DRAW_ERR_LOGS   20      /* DrawCels errors logged one by one */

typedef struct {
    rom_list           roms;
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
    uint32             pic_h;       /* picture height: 192, 224 or 240 lines */
    uint32             border[PLAT_NUM_SCREENS];  /* colour each screen is filled with */
    uint32             direct;      /* presentation without the queue */
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
    text_line(p, p->back, 0, COLOR_TITLE, "MASTERDO - STEP 3D");
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
    for (k = 0; k < v->cr_n; k++)
        printf(" %lu:%02lx=%02lx", (unsigned long)(v->cr_log[k] >> 16),
               (unsigned long)((v->cr_log[k] >> 8) & 31), (unsigned long)(v->cr_log[k] & 0x3F));
    printf("\n");
}

/* The picture on the screens: centred, with the height of the display
 * mode (192, 224 or 240 lines), the clip window of every screen set to
 * it (the cels are positioned in it). */
static void set_picture(platform *p, app_state *st, uint32 h)
{
    st->pic_h = h;
    st->pic_y = ((p->height - (int32)h) / 2) & ~1;
    if (st->pic_y < 0)
        st->pic_y = 0;
    plat_reset_clip(p);
    plat_set_clip(p, st->pic_x, st->pic_y, RENDER_W, (int32)h);
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
                set_picture(p, st, st->pic_h);
                if (render_display_on(g.rd))
                    DrawCels(plat_bitmap(p, p->back), render_chain(g.rd));
                dt = plat_usec_now() - t0;
                plat_reset_clip(p);
            }
            /* The colour RAM under the picture, when there is room. */
            if (st->pic_y + (int32)st->pic_h + 4 + 16 <= p->height)
                dbgview_palette(v, plat_pixels(p, p->back), p->width, st->pic_x,
                                st->pic_y + (int32)st->pic_h + 4);
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

static void reset_borders(app_state *st)
{
    int32 i;

    for (i = 0; i < PLAT_NUM_SCREENS; i++)
        st->border[i] = NO_COLOR;
}

static void run_game(platform *p, app_state *st)
{
    uint32 prev = plat_pad_state();
    uint32 shown = plat_usec_now();     /* direct mode: return of the last presentation */
    uint32 longs = p->long_presents;    /* queued mode: long presents seen */
    uint32 queued = plat_has_presenter(p) && !st->direct;
    char buf[48];

    reset_borders(st);
    st->draw_errors = 0;
    set_picture(p, st, render_height(g.rd));
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
            plat_drain(p);
            game_log_flush(&g);
            prev = show_snapshot(p, st, prev);
            if (prev & ControlX)
                break;
            reset_borders(st);
            set_picture(p, st, st->pic_h);
            shown = plat_usec_now();
            longs = p->long_presents;
            continue;
        }
        if (pressed & ControlRightShift) {
            plat_drain(p);
            st->direct = !st->direct;
            queued = plat_has_presenter(p) && !st->direct;
            printf("Present: frame %lu: %s\n", (unsigned long)g.frame,
                   queued ? "queued (presenter thread)" : "direct (present, then wait for the VBL)");
            shown = plat_usec_now();
            longs = p->long_presents;
        }

        if (game_frame(&g, sms_buttons(held), (pressed & ControlStart) != 0) != 0) {
            plat_drain(p);
            plat_reset_clip(p);
            sprintf(buf, "RUN STOPPED AT PC %04lX", (unsigned long)g.stop_pc);
            show_message(p, buf);
            break;
        }
        if (render_height(g.rd) != st->pic_h) {
            /* The display mode changed height: the picture moves, and the
             * screens get their border again. */
            set_picture(p, st, render_height(g.rd));
            reset_borders(st);
        }
        draw_us = draw_picture(p, st);
        game_frame_done(&g, draw_us);
        if (queued && p->errors != 0) {
            /* The thread cannot show the screens: back to the direct mode. */
            plat_drain(p);
            queued = 0;
            st->direct = 1;
            printf("Present: frame %lu: the presenter thread fails, direct mode\n",
                   (unsigned long)g.frame);
        }
        if (queued) {
            uint32 deadline = t0 + p->frame_us;

            /* The finished screen goes to the presenter at once. When
             * no screen is free the main task would wait for the next
             * VBL anyway: that time goes to translation. */
            plat_queue(p);
            if (!plat_free_screen(p)) {
                uint32 vbl = plat_next_vbl_us(p);

                if ((int32)(vbl - deadline) > 0)
                    deadline = vbl;
            }
            game_spare_time(&g, t0, deadline);
            plat_next_screen(p);
            if (p->long_presents != longs) {
                longs = p->long_presents;
                game_note_long_vbl(&g, p->long_us);
            }
        } else {
            game_spare_time(&g, t0, t0 + p->frame_us);
            plat_present(p);
            now = plat_usec_now();
            if (now - shown >= PLAT_LONG_US)
                game_note_long_vbl(&g, now - shown);
            shown = now;
        }
    }
    plat_drain(p);
    plat_reset_clip(p);
    game_log_summary(&g);
}

/*--------------------------------------------------------------------------
 * ROM menu
 *------------------------------------------------------------------------*/

static void draw_menu(platform *p, const app_state *st, int32 cur)
{
    int32 s = p->back;
    int32 first = (cur / MENU_LINES) * MENU_LINES;
    int32 k;
    char buf[64];

    plat_clear(p, s);
    sprintf(buf, "MASTERDO - CHOOSE A CARTRIDGE (%ld FILES)   A: RUN  X: QUIT",
            (long)st->roms.count);
    text_line(p, s, 0, COLOR_TITLE, buf);
    for (k = first; k < st->roms.count && k < first + MENU_LINES; k++) {
        sprintf(buf, "%c %-32s %4luK", k == cur ? '>' : ' ', st->roms.name[k],
                (unsigned long)((st->roms.size[k] + 1023) / 1024));
        text_line(p, s, 2 + k - first, k == cur ? COLOR_GOOD : COLOR_TEXT, buf);
    }
    plat_present(p);
}

/* Returns the index of the file chosen, or -1 to quit. */
static int32 rom_menu(platform *p, const app_state *st)
{
    int32 cur = 0;
    uint32 prev = plat_pad_state();
    uint32 hold = 0;

    if (st->roms.count == 1)
        return 0;
    draw_menu(p, st, cur);
    for (;;) {
        uint32 held = plat_pad_state();
        uint32 pressed = held & ~prev;
        int32 was = cur;

        /* Auto-repeat of the direction keys after a while. */
        hold = (held & (ControlUp | ControlDown)) ? hold + 1 : 0;
        if (hold > 20 && (hold & 3) == 0)
            pressed |= held & (ControlUp | ControlDown);
        prev = held;
        if (pressed & ControlX)
            return -1;
        if (pressed & (ControlA | ControlStart))
            return cur;
        if ((pressed & ControlUp) && cur > 0)
            cur--;
        if ((pressed & ControlDown) && cur + 1 < st->roms.count)
            cur++;
        if (cur != was)
            draw_menu(p, st, cur);
        plat_wait_vbl(p);
    }
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
                (unsigned long)percent_of(r->frame_us, p->frame_us),
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
    text_line(p, s, n++, COLOR_TITLE, "MASTERDO - RESULTS");
    if (st->rom.size <= 0)
        sprintf(buf, "ROM not loaded (0x%lx)", (unsigned long)st->rom.size);
    else
        sprintf(buf, "%s: %ld bytes, %s, DRAM %luK free", st->rom.name, (long)st->rom.size,
                p->is_pal ? "PAL" : "NTSC", (unsigned long)(st->run_free / 1024));
    text_line(p, s, n++, st->rom.size > 0 ? COLOR_TEXT : COLOR_BAD, buf);

    text_line(p, s, n++, COLOR_HEAD, p->is_pal ? "BENCHMARKS (Z80 PER PAL FRAME)"
                                               : "BENCHMARKS (Z80 PER NTSC FRAME)");
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
                (unsigned long)percent_of(all, p->frame_us));
        text_line(p, s, n++, all <= p->frame_us ? COLOR_GOOD : COLOR_BAD, buf);
        sprintf(buf, " worst %lu us, %lu over %lu us", (unsigned long)g.max_all_us,
                (unsigned long)g.over, (unsigned long)p->frame_us);
        text_line(p, s, n++, g.over == 0 ? COLOR_GOOD : COLOR_TEXT, buf);
    }
    text_line(p, s, n + 1, COLOR_HEAD, st->have_game ? "B: RUN AGAIN   X: BACK TO THE MENU"
                                                     : "X: BACK TO THE MENU");
    plat_present(p);
}

int main(int argc, char **argv)
{
    platform plat;
    app_state st;
    uint32 prev;

    (void)argc;
    (void)argv;

    printf("masterdo: hot address queue with bounded probes, immediate operands encoded inline, refused translations not asked again, forced translations counted\n");
    if (plat_init(&plat) < 0)
        return 1;
    memset(&st, 0, sizeof(st));
    st.pic_x = (plat.width - RENDER_W) / 2;
    st.pic_y = ((plat.height - RENDER_H) / 2) & ~1;
    st.pic_h = RENDER_H;

    show_message(&plat, "Benchmarks of steps 0 and 1...");
    run_benchmarks(&plat, &st);
    plat_mem_free(&st.dram_free, &st.vram_free);
    printf("Memory: %lu bytes of DRAM and %lu bytes of VRAM free before the cartridge\n",
           (unsigned long)st.dram_free, (unsigned long)st.vram_free);

    if (rom_scan(&st.roms) <= 0) {
        show_message(&plat, "No cartridge image in the " ROM_DIR " directory");
        printf("ROM: no file in " ROM_DIR "\n");
        plat_wait_vbl(&plat);
        for (;;) {
            if (plat_pad_pressed() & ControlX)
                goto quit;
            plat_wait_vbl(&plat);
        }
    }
    for (;;) {
        int32 pick = rom_menu(&plat, &st);
        char buf[64];

        if (pick < 0)
            goto quit;
        sprintf(buf, "Loading %s...", st.roms.name[pick]);
        show_message(&plat, buf);
        st.have_game = 0;
        if (rom_load(&st.rom, st.roms.name[pick]) == 0)
            rom_log(&st.rom);
        if (st.rom.data != NULL && st.rom.size >= 0x4000 &&
            game_init(&g, st.rom.data, (uint32)st.rom.size) == 0) {
            uint32 vram;

            st.have_game = 1;
            game_set_name(&g, st.rom.name);
            game_set_frame_us(&g, plat.frame_us);
            show_message(&plat, "Translating the cartridge code...");
            game_translate_rom(&g);
            plat_mem_free(&st.run_free, &vram);
            printf("Memory: %lu bytes of DRAM and %lu bytes of VRAM free during the run; "
                   "picture at (%ld,%ld)\n",
                   (unsigned long)st.run_free, (unsigned long)vram, (long)st.pic_x,
                   (long)st.pic_y);
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
                    goto leave;
                if ((pressed & ControlB) && st.have_game)
                    break;
                plat_wait_vbl(&plat);
            }
        }
leave:
        if (st.have_game)
            game_free(&g);
        rom_unload(&st.rom);
        if (st.roms.count == 1)
            goto quit;
    }

quit:
    printf("masterdo: quit\n");
    plat_shutdown(&plat);
    return 0;
}
