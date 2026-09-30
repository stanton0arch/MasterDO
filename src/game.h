#ifndef GAME_H
#define GAME_H

/*
 * Cartridge run: the SMS machine (sms.c) on the Z80 translator and the
 * cel renderer (render.c), driven one frame at a time by the caller. A
 * frame is the emulation, then the update of the picture (bitmap and
 * cels); the caller draws the cels and reports the drawing time. The
 * times of each part and the machine and renderer counters are logged
 * every GAME_WINDOW frames, with a summary at the end.
 */

#include "types.h"
#include "z80jit.h"
#include "sms.h"
#include "render.h"

#define GAME_WINDOW     100     /* frames per log line */
#define GAME_HIST       32      /* frame time histogram, 1 ms per bucket */
#define GAME_SLOW_LOGS  120     /* frames over the NTSC budget logged alone */
#define GAME_LONG_LOGS  60      /* frames shown for two VBLs while under budget */

/* Cumulative counters of the machine and the translator. */
typedef struct {
    uint32 idle;            /* ctx->idle */
    uint32 irqs;
    uint32 nmis;
    uint32 banks;           /* paging register writes */
    uint32 blocks;          /* translations */
    uint32 flushes;
    uint32 evictions;       /* code buffer zones emptied */
    uint32 evicted;         /* blocks lost to them */
    uint32 prefetched;      /* blocks translated in spare time */
    uint32 promoted;        /* blocks translated again into the hot area */
    uint32 interp;          /* stretches of interpretation */
    uint32 interp_insns;    /* instructions interpreted */
    uint32 sync;            /* hot blocks translated at once */
    uint32 sync_us;         /* time they took */
    uint32 sync_refused;    /* translations at once refused, budget spent */
    uint32 vdp_data_w;
    uint32 vdp_data_r;
    uint32 vdp_ctrl_w;
    sms_io_stats io;
    render_stats rd;
} game_counters;

typedef struct {
    uint32 frames;
    uint32 us;              /* emulation */
    uint32 max_us;
    uint32 max_frame;
    uint32 upd_us;          /* picture update */
    uint32 max_upd_us;
    uint32 draw_us;         /* DrawCels, timed by the caller */
    uint32 max_draw_us;
    uint32 total_us;        /* the three together */
    uint32 max_total_us;
    uint32 max_total_frame;
    uint32 over;            /* frames whose total is over the budget */
    uint32 draw_err;        /* DrawCels errors */
    uint32 long_vbl;        /* frames shown for two VBLs or more */
    uint32 pad_us;          /* pad reads, timed by the caller */
    uint32 spare_us;        /* spare-time translation */
    game_counters start;    /* counters when the window began */
} game_window;

/* A window closed but not logged yet: the log is written in the spare
 * time of a later frame, so that it does not push a frame past its VBL. */
typedef struct {
    uint32        valid;
    uint32        first;    /* frames first-last */
    uint32        last;
    game_window   w;
    game_counters now;      /* counters when the window closed */
} game_pending_log;

/* Zones of the code buffer given to the hot area; set before game_init
 * (a tuning knob of the analysis harness). */
extern uint32 game_hot_zones;

typedef struct {
    z80j_ctx     *ctx;
    uint32       *code;
    void         *blocks;
    uint32       *links;
    uint8        *hot;      /* entry counts of the interpreter */
    sms_machine  *sms;
    renderer     *rd;
    z80j_state    jit;
    const uint8  *rom;
    uint32        rom_size;
    uint32        seed_hot; /* translate the reachable code into the hot area */
    int32         status;   /* 0, or the translator exit reason */
    uint32        stop_pc;
    /* Translation of the reachable code before the run. */
    uint32        rom_blocks;
    uint32        rom_insns;
    uint32        rom_bytes;
    uint32        rom_us;
    uint32        rom_busy;
    /* Run since the last reset. */
    uint32        frame;
    uint32        last_us;  /* last frame: emulation */
    uint32        last_upd_us;
    uint32        last_draw_us;
    uint32        last_spare_us;
    uint32        last_pad_us;
    uint32        long_logged;
    game_pending_log pend;
    uint32        total_us; /* emulation */
    uint32        max_us;
    uint32        max_frame;
    uint32        upd_total_us;
    uint32        draw_total_us;
    uint32        all_total_us;
    uint32        max_all_us;
    uint32        max_all_frame;
    uint32        over;     /* frames whose total is over the NTSC budget */
    uint32        idle_t16; /* idle T-states / 16 */
    uint32        slow_logged;
    uint32        hist[GAME_HIST];  /* total time per frame */
    game_counters run_start;
    game_window   win;
    uint32        win_avg_us;   /* last complete window: emulation */
    uint32        win_max_us;
    uint32        win_total_us; /* average total */
    uint32        win_idle_pct;
    /* Frame in progress (between game_frame and game_frame_done). */
    uint32        f_idle;
    uint32        f_blocks;
    uint32        f_flush;
    uint32        f_data;
    uint32        f_tiles;
    uint32        f_cells;
    uint32        f_interp;
    uint32        f_interp_insns;
    uint32        f_sync_us;
} game;

/* Allocates the translator and the machine for a cartridge image. */
Err   game_init(game *g, const uint8 *rom, uint32 rom_size);
void  game_free(game *g);

/* Translates the code reachable from the entry points ($0000, $0038,
 * $0066) by static jumps and calls, and logs the time it took. */
void  game_translate_rom(game *g);

/* Resets the machine (translations are kept) and the statistics. */
void  game_reset(game *g);

/* Runs one frame with the buttons held (SMS_PAD_*) and the PAUSE button,
 * then updates the picture; returns 0, or the translator exit reason once
 * the run has stopped. The caller then draws render_chain(g->rd) when
 * render_display_on(g->rd), and calls game_frame_done() with the
 * drawing time. */
int32 game_frame(game *g, uint32 pad, uint32 pause);
void  game_frame_done(game *g, uint32 draw_us);

/* Notes of the caller about the frame: a DrawCels error, a frame whose
 * presentation took two VBLs or more (period_us from the previous
 * presentation; logged when the frame's own work was under budget). */
void  game_note_draw_error(game *g);
void  game_note_long_vbl(game *g, uint32 period_us);

/* Adds the time of a pad read to the current window. */
void  game_pad_time(game *g, uint32 us);

/* Spare time at the end of a frame that began at frame_start (microsecond
 * clock): translates queued code ahead while a translation still fits in
 * the frame, then writes a pending window log if there is room for it;
 * returns the time it used. */
uint32 game_spare_time(game *g, uint32 frame_start);

void  game_log_summary(game *g);

#endif /* GAME_H */
