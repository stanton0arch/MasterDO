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
#define GAME_SLOW_LOGS  120     /* frames over the budget logged alone */
#define GAME_LONG_LOGS  60      /* frames shown late while under budget */
#define GAME_LOG_WORDS  (24 * 1024 / 4) /* record arena of the run's log */
#define GAME_PAD_LOGS   1024    /* pad changes kept by the pad recorder */

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
    uint32 evicted_live;    /* live chunks of the evicted zones */
    uint32 prefetched;      /* blocks translated in spare time */
    uint32 promoted;        /* blocks translated again into the hot area */
    uint32 interp;          /* stretches of interpretation */
    uint32 interp_insns;    /* instructions interpreted */
    uint32 sync;            /* hot blocks translated at once */
    uint32 sync_us;         /* time they took */
    uint32 sync_refused;    /* translations at once refused, budget spent */
    uint32 resumed;         /* interrupt returns resumed inside their block */
    uint32 vdp_data_w;
    uint32 vdp_data_r;
    uint32 vdp_ctrl_w;
    sms_io_stats io;
    render_stats rd;
} game_counters;

typedef struct {
    uint32 frames;
    uint32 start_us;        /* clock when the window began */
    uint32 real_us;         /* real time the window took, once closed */
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
} game_window;

/* The run's log is kept as records in a word arena and only formatted
 * when it is written out (formatting the two lines of a window costs
 * about 9 ms on the console and printing them 45 ms, so neither happens
 * inside a frame): a closed window, a frame over budget, a frame shown
 * late while under budget. A record starts with a word holding its kind
 * (top byte) and its length in words; a full arena drops records.
 *
 * The pad recorder keeps, apart from the arena, the frames where the
 * buttons held changed (PAUSE as bit 8) and writes them out with the log
 * as a pad script in the format of the analysis harness ("frame:bits,..."
 * preceded by the cartridge name and the frame count), so that a run on
 * the console can be replayed in the cycle model. */
#define GAME_REC_WINDOW 1
#define GAME_REC_SLOW   2
#define GAME_REC_LONG   3

typedef struct {
    uint32        first;    /* frames first-last */
    uint32        last;
    game_window   w;
    game_counters d;        /* counters over the window */
} game_window_rec;

typedef struct {
    uint32 frame;
    uint32 total_us;
    uint32 us;
    uint32 upd_us;
    uint32 draw_us;
    uint32 idle_pct;
    uint32 blocks;
    uint32 evicted;         /* an eviction happened in the frame */
    uint32 sync_us;
    uint32 insns;           /* interpreted */
    uint32 stretches;
    uint32 data_w;
    uint32 tiles;
    uint32 cells;
} game_slow_rec;

typedef struct {
    uint32 frame;
    uint32 period_us;       /* between its presentation and the previous one */
    uint32 us;
    uint32 upd_us;
    uint32 draw_us;
    uint32 spare_us;
    uint32 pad_us;
    uint32 sync_us;
} game_long_rec;

/* Zones of the code buffer given to the hot area; set before game_init
 * (a tuning knob of the analysis harness). */
extern uint32 game_hot_zones;

typedef struct {
    z80j_ctx     *ctx;
    uint32       *code;
    void         *blocks;
    uint32       *links;
    uint8        *hot;      /* entry counts of the interpreter */
    uint32       *log;      /* records of the run, written out later */
    uint32        log_len;  /* words used */
    uint32        log_from; /* first frame the records cover */
    uint32        log_dropped;
    uint32       *pad_log;  /* pad changes: frame << 9 | buttons (PAUSE bit 8) */
    uint32        pad_log_n;
    uint32        pad_log_printed; /* entries already written out */
    uint32        pad_log_dropped;
    uint32        pad_last; /* buttons of the previous frame */
    const char   *name;     /* cartridge file name, for the pad script line */
    uint32        frame_us; /* period of the display: the frame budget */
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
    game_counters win_start;    /* counters when the window began */
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
    uint8  *cart_ram;       /* cartridge RAM (battery RAM of the mapper), or NULL */
    uint32  cart_ram_size;  /* 32 KiB in VRAM, else 16 KiB in DRAM */
    uint32  cart_ram_vram;  /* allocated in VRAM */
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

/* Period of the display, in microseconds: the budget of a frame (NTSC
 * by default). */
void  game_set_frame_us(game *g, uint32 us);

/* Names the cartridge in the pad script line of the log (the string is
 * kept, not copied). */
void  game_set_name(game *g, const char *name);

/* Spare time at the end of a frame that began at frame_start
 * (microsecond clock): translates queued code ahead while a translation
 * still fits before deadline_us (a clock value: the end of the frame, or
 * later when the presentation queue gives more time); returns the time
 * it used. */
uint32 game_spare_time(game *g, uint32 frame_start, uint32 deadline_us);

/* Writes out the records buffered during the run (printing costs about
 * 35 us per character on the debug link, so the run only buffers them):
 * called when the run pauses or ends, never by the run itself. */
void  game_log_flush(game *g);

void  game_log_summary(game *g);

#endif /* GAME_H */
