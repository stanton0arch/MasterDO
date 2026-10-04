/*
 * Cartridge run with frame timing and statistics. See game.h.
 */

#include "game.h"
#include "platform.h"

#include "stdio.h"
#include "string.h"
#include "mem.h"

#define JIT_ZONE_BYTES  (32 * 1024)
#define JIT_ZONES       14
#define JIT_HOT_ZONES   5       /* code translated again after an eviction goes there */
#define JIT_CODE_BYTES  (JIT_ZONE_BYTES * JIT_ZONES)
#define JIT_BLOCKS      2048
#define JIT_LINKS       (2 * JIT_BLOCKS)
#define ROM_QUEUE       1024
#define ROM_MAX_BLOCKS  4000

/* Interpreter: an address is interpreted the first time it is entered
 * and translated at once from its second entry, within a budget per
 * frame of HOT_SYNC_US, raised by HOT_INT_COST (1/16 us, the cost of an
 * interpreted instruction in the ARM60 model) per instruction the frame
 * has interpreted; past the budget it is queued for the spare time of
 * the frame. */
#define HOT_QUEUE_AT    2
#define HOT_SYNC_AT     2
#define HOT_FORCE_AT    8       /* translated whatever the budget from this entry */
#define HOT_SYNC_US     3000
#define HOT_INT_COST    100

/* Time of a frame outside the emulation, the picture update and the
 * drawing: the pad read, the presentation and the system. A translation
 * in spare time, or at once with the floor budget, must leave the frame
 * this margin on top of what the update and the drawing took in the
 * previous frame. */
#define FRAME_MARGIN_US 800

#define FRAME_T         (VDP_LINES_NTSC * VDP_LINE_T)   /* T-states per frame */

static uint32 percent(uint32 part, uint32 whole);

uint32 game_hot_zones = JIT_HOT_ZONES;

/* Records of the run's log: a record of n words of kind k is allocated
 * in the arena (NULL, counted as dropped, when it is full) and formatted
 * only by game_log_flush. */
static uint32 *rec_alloc(game *g, uint32 kind, uint32 bytes)
{
    uint32 words = bytes / 4;
    uint32 *r;

    if (g->log_len + 1 + words > GAME_LOG_WORDS) {
        g->log_dropped++;
        return NULL;
    }
    r = g->log + g->log_len;
    r[0] = (kind << 24) | words;
    g->log_len += 1 + words;
    return r + 1;
}

static void print_window(const game_window_rec *p)
{
    const game_window *w = &p->w;
    const game_counters *d = &p->d;
    uint32 frames = w->frames ? w->frames : 1;
    uint32 idle_pct = percent(d->idle >> 8, w->frames * FRAME_T);

    printf("Game: frames %lu-%lu: emulation %lu us average, %lu us worst (frame %lu), "
           "update %lu us average, %lu us worst, draw %lu us average, %lu us worst, "
           "total %lu us average, %lu us worst (frame %lu), real %lu us per frame, "
           "%lu over budget, "
           "%lu draw errors, %lu long presents, Z80 idle %lu%%, "
           "%lu IRQ, %lu NMI, VDP %lu data writes, %lu data reads, %lu control writes, "
           "%lu status reads, %lu counter reads, PSG %lu, pad %lu, other %lu, %lu leaves, "
           "%lu bank switches, %lu blocks (%lu in spare time, %lu at once in %lu us, "
           "%lu refused, %lu promoted, %lu us of spare time), %lu interpreted "
           "instructions in %lu stretches, %lu evictions (%lu blocks, %lu live chunks), "
           "%lu returns resumed, pad read %lu us\n",
           (unsigned long)p->first, (unsigned long)p->last,
           (unsigned long)(w->us / frames), (unsigned long)w->max_us,
           (unsigned long)w->max_frame,
           (unsigned long)(w->upd_us / frames), (unsigned long)w->max_upd_us,
           (unsigned long)(w->draw_us / frames), (unsigned long)w->max_draw_us,
           (unsigned long)(w->total_us / frames), (unsigned long)w->max_total_us,
           (unsigned long)w->max_total_frame, (unsigned long)(w->real_us / frames),
           (unsigned long)w->over,
           (unsigned long)w->draw_err, (unsigned long)w->long_vbl,
           (unsigned long)idle_pct,
           (unsigned long)d->irqs, (unsigned long)d->nmis,
           (unsigned long)d->vdp_data_w, (unsigned long)d->vdp_data_r,
           (unsigned long)d->vdp_ctrl_w, (unsigned long)d->io.vdp_stat_r,
           (unsigned long)d->io.counter_r, (unsigned long)d->io.psg_w,
           (unsigned long)d->io.pad_r, (unsigned long)d->io.other,
           (unsigned long)d->io.leaves, (unsigned long)d->banks, (unsigned long)d->blocks,
           (unsigned long)d->prefetched, (unsigned long)d->sync, (unsigned long)d->sync_us,
           (unsigned long)d->sync_refused, (unsigned long)d->promoted,
           (unsigned long)w->spare_us, (unsigned long)d->interp_insns, (unsigned long)d->interp,
           (unsigned long)d->evictions, (unsigned long)d->evicted,
           (unsigned long)d->evicted_live, (unsigned long)d->resumed,
           (unsigned long)(w->pad_us / frames));
    printf("Picture: frames %lu-%lu: %lu tiles converted, %lu cells drawn, %lu rebuilds, "
           "%lu cells for written tiles, %lu palettes, %lu backdrop changes, %lu sprite cels, "
           "%lu pieces, %lu priority strips, %lu priority patches, %lu frames in scroll bands, "
           "%lu frames partly blanked, %lu frames in name table bands, %lu bands dropped, "
           "%lu frames short of layers, %lu frames with sprites past the line limit, "
           "%lu sprite runs dropped, %lu frames with sprite overflow, %lu with sprite collision, "
           "%lu frames in palette bands, %lu colour writes merged, %lu frames of more than "
           "192 lines\n",
           (unsigned long)p->first, (unsigned long)p->last,
           (unsigned long)d->rd.tiles, (unsigned long)d->rd.cells, (unsigned long)d->rd.rebuilds,
           (unsigned long)d->rd.tile_cells, (unsigned long)d->rd.palettes,
           (unsigned long)d->rd.backdrops,
           (unsigned long)d->rd.sprites, (unsigned long)d->rd.pieces,
           (unsigned long)d->rd.prio_strips, (unsigned long)d->rd.prio_patches,
           (unsigned long)d->rd.bands, (unsigned long)d->rd.dbands,
           (unsigned long)d->rd.ntbands, (unsigned long)d->rd.dropped, (unsigned long)d->rd.layers,
           (unsigned long)d->rd.spr_partial, (unsigned long)d->rd.spr_dropped,
           (unsigned long)d->rd.overflows, (unsigned long)d->rd.collisions,
           (unsigned long)d->rd.pbands, (unsigned long)d->rd.pmerged, (unsigned long)d->rd.tall);
}

static void print_slow(const game_slow_rec *p)
{
    printf("Game: frame %lu took %lu us (emulation %lu, update %lu, draw %lu): "
           "Z80 idle %lu%%, %lu blocks translated%s (%lu us at once), "
           "%lu interpreted instructions in %lu stretches, "
           "%lu VDP data writes, %lu tiles converted, %lu cells drawn\n",
           (unsigned long)p->frame, (unsigned long)p->total_us, (unsigned long)p->us,
           (unsigned long)p->upd_us, (unsigned long)p->draw_us,
           (unsigned long)p->idle_pct, (unsigned long)p->blocks,
           p->evicted ? " after an eviction" : "", (unsigned long)p->sync_us,
           (unsigned long)p->insns, (unsigned long)p->stretches,
           (unsigned long)p->data_w, (unsigned long)p->tiles, (unsigned long)p->cells);
}

static void print_long(const game_long_rec *p)
{
    printf("Game: frame %lu shown after %lu us though under budget: emulation %lu, "
           "update %lu, draw %lu, spare time %lu, pad %lu us, %lu us of translations "
           "at once\n",
           (unsigned long)p->frame, (unsigned long)p->period_us,
           (unsigned long)p->us, (unsigned long)p->upd_us, (unsigned long)p->draw_us,
           (unsigned long)p->spare_us, (unsigned long)p->pad_us, (unsigned long)p->sync_us);
}

/* The pad script of the run so far, as a line of the panel file of the
 * analysis harness: the cartridge name, the frames run, then the buttons
 * held from each frame where they changed (hexadecimal, PAUSE as bit 8).
 * The whole script is written each time, so that the last one in the
 * log is complete; nothing is written when no change was recorded since
 * the previous time. */
static void print_pad_script(game *g)
{
    uint32 k;

    if (g->pad_log_n == g->pad_log_printed)
        return;
    printf("Pad: %lu changes recorded, %lu dropped; the next line is the panel entry\n",
           (unsigned long)g->pad_log_n, (unsigned long)g->pad_log_dropped);
    printf("Pad script: %s %lu ", g->name != NULL ? g->name : "rom.sms",
           (unsigned long)g->frame);
    for (k = 0; k < g->pad_log_n; k++)
        printf("%s%lu:%lx", k ? "," : "", (unsigned long)(g->pad_log[k] >> 9),
               (unsigned long)(g->pad_log[k] & 0x1FF));
    printf("\n");
    g->pad_log_printed = g->pad_log_n;
}

void game_log_flush(game *g)
{
    uint32 k = 0;

    print_pad_script(g);
    if (g->log_len == 0 && g->log_dropped == 0)
        return;
    printf("Log: %lu words of records from frame %lu, written at frame %lu, %lu records dropped\n",
           (unsigned long)g->log_len, (unsigned long)g->log_from, (unsigned long)g->frame,
           (unsigned long)g->log_dropped);
    while (k < g->log_len) {
        uint32 head = g->log[k];
        const uint32 *body = g->log + k + 1;

        switch (head >> 24) {
        case GAME_REC_WINDOW: print_window((const game_window_rec *)body); break;
        case GAME_REC_SLOW:   print_slow((const game_slow_rec *)body); break;
        case GAME_REC_LONG:   print_long((const game_long_rec *)body); break;
        default: break;
        }
        k += 1 + (head & 0xFFFFFFu);
    }
    g->log_len = 0;
    g->log_dropped = 0;
    g->log_from = g->frame;
}

static void read_counters(const game *g, game_counters *c)
{
    c->idle = g->ctx->idle;
    c->irqs = g->jit.stats.interrupts + g->ctx->irq_count;
    c->nmis = g->jit.stats.nmis;
    c->banks = g->sms->mem.remaps;
    c->blocks = g->jit.stats.translations;
    c->flushes = g->jit.stats.flushes;
    c->evictions = g->jit.stats.evictions;
    c->evicted = g->jit.stats.evicted;
    c->evicted_live = g->jit.stats.evicted_live;
    c->prefetched = g->jit.stats.prefetched;
    c->promoted = g->jit.stats.promoted;
    c->interp = g->ctx->int_runs;
    c->interp_insns = g->ctx->int_insns;
    c->sync = g->jit.stats.sync;
    c->sync_us = g->jit.stats.sync_us;
    c->sync_refused = g->jit.stats.sync_refused;
    c->resumed = g->jit.stats.resumed + g->ctx->resumed_count;
    c->vdp_data_w = g->sms->vdp.n_data_w;
    c->vdp_data_r = g->sms->vdp.n_data_r;
    c->vdp_ctrl_w = g->sms->vdp.n_ctrl_w;
    memcpy(&c->io, &g->sms->io, sizeof(c->io));
    c->io.vdp_stat_r += g->sms->vdp.n_stat_r;   /* the assembly handler counts apart */
    memcpy(&c->rd, &g->rd->st, sizeof(c->rd));
}

/* d = a - b, field by field. */
static void diff_counters(game_counters *d, const game_counters *a, const game_counters *b)
{
    const uint32 *pa = (const uint32 *)a;
    const uint32 *pb = (const uint32 *)b;
    uint32 *pd = (uint32 *)d;
    uint32 k;

    for (k = 0; k < sizeof(game_counters) / sizeof(uint32); k++)
        pd[k] = pa[k] - pb[k];
}

/* part * 100 / whole without overflow, for part <= whole. */
static uint32 percent(uint32 part, uint32 whole)
{
    if (whole == 0)
        return 0;
    if (whole > 0x01000000u)
        return part / (whole / 100u);
    return (part * 100u) / whole;
}

static void window_start(game *g)
{
    memset(&g->win, 0, sizeof(g->win));
    read_counters(g, &g->win_start);
    g->win.start_us = plat_usec_now();
}

void game_free(game *g)
{
    if (g->rd != NULL) {
        render_free(g->rd);
        FreeMem(g->rd, sizeof(renderer));
    }
    if (g->sms != NULL) FreeMem(g->sms, sizeof(sms_machine));
    if (g->blocks != NULL) FreeMem(g->blocks, (int32)z80j_block_bytes(JIT_BLOCKS));
    if (g->links != NULL) FreeMem(g->links, (int32)z80j_link_bytes(JIT_LINKS));
    if (g->hot != NULL) FreeMem(g->hot, 0x10000);
    if (g->log != NULL) FreeMem(g->log, GAME_LOG_WORDS * 4);
    if (g->pad_log != NULL) FreeMem(g->pad_log, GAME_PAD_LOGS * 4);
    if (g->code != NULL) FreeMem(g->code, JIT_CODE_BYTES);
    if (g->ctx != NULL) FreeMem(g->ctx, sizeof(z80j_ctx));
    if (g->cart_ram != NULL) FreeMem(g->cart_ram, (int32)g->cart_ram_size);
    g->rd = NULL;
    g->sms = NULL;
    g->blocks = NULL;
    g->links = NULL;
    g->hot = NULL;
    g->log = NULL;
    g->pad_log = NULL;
    g->code = NULL;
    g->ctx = NULL;
    g->cart_ram = NULL;
}

Err game_init(game *g, const uint8 *rom, uint32 rom_size)
{
    z80j_glue glue;

    memset(g, 0, sizeof(*g));
    g->rom = rom;
    g->rom_size = rom_size;
    g->ctx = (z80j_ctx *)AllocMem(sizeof(z80j_ctx), MEMTYPE_DRAM | MEMTYPE_FILL);
    g->code = (uint32 *)AllocMem(JIT_CODE_BYTES, MEMTYPE_DRAM);
    g->blocks = AllocMem((int32)z80j_block_bytes(JIT_BLOCKS), MEMTYPE_DRAM);
    g->links = (uint32 *)AllocMem((int32)z80j_link_bytes(JIT_LINKS), MEMTYPE_DRAM);
    g->hot = (uint8 *)AllocMem(0x10000, MEMTYPE_DRAM);
    g->log = (uint32 *)AllocMem(GAME_LOG_WORDS * 4, MEMTYPE_DRAM);
    g->sms = (sms_machine *)AllocMem(sizeof(sms_machine), MEMTYPE_DRAM);
    g->rd = (renderer *)AllocMem(sizeof(renderer), MEMTYPE_DRAM | MEMTYPE_FILL);
    /* The pad recorder is optional: without memory nothing is recorded. */
    g->pad_log = (uint32 *)AllocMem(GAME_PAD_LOGS * 4, MEMTYPE_DRAM);
    if (g->ctx == NULL || g->code == NULL || g->blocks == NULL || g->links == NULL ||
        g->hot == NULL || g->log == NULL || g->sms == NULL || g->rd == NULL) {
        printf("ERROR: out of memory for the machine (%lu + %lu + %lu + %lu + %lu bytes)\n",
               (unsigned long)sizeof(z80j_ctx), (unsigned long)JIT_CODE_BYTES,
               (unsigned long)z80j_block_bytes(JIT_BLOCKS), (unsigned long)z80j_link_bytes(JIT_LINKS),
               (unsigned long)sizeof(sms_machine));
        game_free(g);
        return -1;
    }
    if (render_init(g->rd) < 0) {
        game_free(g);
        return -1;
    }
    z80j_default_glue(&glue);
    z80j_init(&g->jit, g->ctx, g->code, JIT_CODE_BYTES / 4, JIT_ZONES, game_hot_zones,
              g->blocks, JIT_BLOCKS, g->links, JIT_LINKS, &glue);
    z80j_set_interp(&g->jit, g->hot, HOT_QUEUE_AT, HOT_SYNC_AT);
    z80j_set_force(&g->jit, HOT_FORCE_AT);
    game_set_frame_us(g, PLAT_NTSC_FRAME_US);
    /* Cartridge RAM (the battery RAM of the Sega mapper, shown in slot 2
     * by register $FFFC): 32 KiB in VRAM, which the CPU reads and writes
     * like DRAM, when the renderer has left some, else the 16 KiB page
     * most cartridges have, in DRAM; zero-filled, as a cartridge with no
     * save would be. */
    g->cart_ram_size = 0x8000;
    g->cart_ram_vram = 1;
    g->cart_ram = (uint8 *)AllocMem(0x8000, MEMTYPE_VRAM | MEMTYPE_FILL);
    if (g->cart_ram == NULL) {
        g->cart_ram_size = 0x4000;
        g->cart_ram_vram = 0;
        g->cart_ram = (uint8 *)AllocMem(0x4000, MEMTYPE_DRAM | MEMTYPE_FILL);
    }
    if (g->cart_ram == NULL)
        g->cart_ram_size = 0;
    sms_init(g->sms, g->ctx, rom, rom_size, VDP_LINES_NTSC, g->cart_ram, g->cart_ram_size);
    printf("Cartridge RAM: %lu bytes in %s\n", (unsigned long)g->cart_ram_size,
           g->cart_ram == NULL ? "no memory" : g->cart_ram_vram ? "VRAM" : "DRAM");
    printf("Mapper: %s\n", g->sms->mem.mapper == SMS_MAPPER_CODEMASTERS ?
           "Codemasters (header at $7FE0; slots paged by writes to $0000, $4000, $8000)" :
           "Sega (paging registers at $FFFC-$FFFF)");
    game_reset(g);
    return 0;
}

void game_translate_rom(game *g)
{
    z80j_state *j = &g->jit;
    uint32 *visited;
    uint32 *queue;
    uint32 head = 0;
    uint32 tail = 0;
    uint32 targets[8];
    uint32 busy0 = j->stats.busy_loops;
    uint32 t0;
    uint32 k;

    /* The visited bitmap and the queue borrow the log arena, which holds
     * nothing before the run. */
    visited = g->log;
    queue = g->log + 0x10000 / 32;
    memset(visited, 0, 0x10000 / 8);
    queue[tail++] = 0x0000;
    queue[tail++] = 0x0038;
    queue[tail++] = 0x0066;

    t0 = plat_usec_now();
    j->seed_hot = g->seed_hot;
    while (head != tail && g->rom_blocks < ROM_MAX_BLOCKS) {
        uint32 pc = queue[head];
        uint32 bytes = j->stats.code_bytes;
        uint32 n;
        uint32 nt;

        head = (head + 1) % ROM_QUEUE;
        if (visited[pc >> 5] & ((uint32)1 << (pc & 31)))
            continue;
        visited[pc >> 5] |= (uint32)1 << (pc & 31);
        n = z80j_prepare(j, pc, targets, 8, &nt);
        if (n == 0)
            continue;
        g->rom_blocks++;
        g->rom_insns += n;
        g->rom_bytes += j->stats.code_bytes - bytes;
        for (k = 0; k < nt; k++) {
            uint32 tg = targets[k] & 0xFFFFu;

            if (tg < 0xC000u && !(visited[tg >> 5] & ((uint32)1 << (tg & 31))) &&
                (tail + 1) % ROM_QUEUE != head) {
                queue[tail] = tg;
                tail = (tail + 1) % ROM_QUEUE;
            }
        }
    }
    g->rom_us = plat_usec_now() - t0;
    g->rom_busy = j->stats.busy_loops - busy0;

    g->log_len = 0;

    printf("ROM translation: %lu blocks, %lu instructions, %lu bytes of ARM code, %lu us "
           "(%lu us per instruction), %lu busy-wait loops, %lu flag scans from the cache, "
           "%lu evictions; code buffer "
           "%d zones of %d bytes shared by the cold and hot areas, run marks per "
           "%lu bytes, %d block descriptors, %d link entries; "
           "interpreter: translation queued at %d entries, at once from %d within "
           "%d us per frame when the frame has room, plus %d/16 us per interpreted "
           "instruction, whatever the budget from %d entries\n",
           (unsigned long)g->rom_blocks, (unsigned long)g->rom_insns,
           (unsigned long)g->rom_bytes, (unsigned long)g->rom_us,
           (unsigned long)(g->rom_insns ? g->rom_us / g->rom_insns : 0),
           (unsigned long)g->rom_busy, (unsigned long)j->stats.scan_hits,
           (unsigned long)j->stats.evictions,
           JIT_ZONES, JIT_ZONE_BYTES, (unsigned long)(4u << j->chunk_shift), JIT_BLOCKS, JIT_LINKS,
           HOT_QUEUE_AT, HOT_SYNC_AT, HOT_SYNC_US, HOT_INT_COST, HOT_FORCE_AT);
}

void game_reset(game *g)
{
    sms_reset(g->sms);
    render_reset(g->rd);
    g->status = 0;
    g->stop_pc = 0;
    g->frame = 0;
    g->last_us = 0;
    g->last_upd_us = 2000;      /* usual values, for the first frame's budget */
    g->last_draw_us = 3500;
    g->last_spare_us = 0;
    g->last_pad_us = 0;
    g->long_logged = 0;
    g->log_len = 0;
    g->log_from = 0;
    g->log_dropped = 0;
    g->pad_log_n = 0;
    g->pad_log_printed = 0;
    g->pad_log_dropped = 0;
    g->pad_last = 0;
    g->total_us = 0;
    g->max_us = 0;
    g->max_frame = 0;
    g->upd_total_us = 0;
    g->draw_total_us = 0;
    g->all_total_us = 0;
    g->max_all_us = 0;
    g->max_all_frame = 0;
    g->over = 0;
    g->idle_t16 = 0;
    g->slow_logged = 0;
    memset(g->hist, 0, sizeof(g->hist));
    g->win_avg_us = 0;
    g->win_max_us = 0;
    g->win_total_us = 0;
    g->win_idle_pct = 0;
    read_counters(g, &g->run_start);
    window_start(g);
}

/* Closes the window into a log record and starts a new one. */
static void window_close(game *g)
{
    game_window *w = &g->win;
    game_counters now;
    game_window_rec *p;

    if (w->frames == 0)
        return;
    read_counters(g, &now);
    g->win_avg_us = w->us / w->frames;
    g->win_max_us = w->max_us;
    g->win_total_us = w->total_us / w->frames;
    w->real_us = plat_usec_now() - w->start_us;
    p = (game_window_rec *)rec_alloc(g, GAME_REC_WINDOW, sizeof(*p));
    if (p != NULL) {
        p->first = g->frame - w->frames;
        p->last = g->frame - 1;
        memcpy(&p->w, w, sizeof(p->w));
        diff_counters(&p->d, &now, &g->win_start);
        g->win_idle_pct = percent(p->d.idle >> 8, w->frames * FRAME_T);
    }
    window_start(g);
}

int32 game_frame(game *g, uint32 pad, uint32 pause)
{
    z80j_ctx *ctx = g->ctx;
    uint32 idle0;
    uint32 t0;
    uint32 t1;

    if (g->status != 0)
        return g->status;
    idle0 = ctx->idle;
    g->f_blocks = g->jit.stats.translations;
    g->f_flush = g->jit.stats.evictions;
    g->f_data = g->sms->vdp.n_data_w;
    g->f_tiles = g->rd->st.tiles;
    g->f_cells = g->rd->st.cells;
    g->f_interp = g->ctx->int_runs;
    g->f_interp_insns = g->ctx->int_insns;
    g->f_sync_us = g->jit.stats.sync_us;

    /* Pad recorder: a change of the buttons held is kept with its frame. */
    {
        uint32 bits = (pad & 0x3F) | (pause ? 0x100 : 0);

        if (bits != g->pad_last) {
            g->pad_last = bits;
            if (g->pad_log != NULL && g->pad_log_n < GAME_PAD_LOGS)
                g->pad_log[g->pad_log_n++] = (g->frame << 9) | bits;
            else
                g->pad_log_dropped++;
        }
    }

    t0 = plat_usec_now();
    z80j_frame_start(&g->jit, t0, g->last_upd_us + g->last_draw_us + FRAME_MARGIN_US);
    sms_frame(g->sms, pad, pause);
    t1 = plat_usec_now();
    g->last_us = t1 - t0;

    if (ctx->exit_reason != Z80J_EXIT_LINES) {
        g->status = (int32)ctx->exit_reason;
        g->stop_pc = ctx->exit_arg;
        game_log_flush(g);
        printf("Game: stopped at frame %lu, exit reason %ld at PC $%04lx\n",
               (unsigned long)g->frame, (long)g->status, (unsigned long)g->stop_pc);
        return g->status;
    }
    g->f_idle = (ctx->idle - idle0) >> 8;

    render_update(g->rd, &g->sms->vdp);
    g->last_upd_us = plat_usec_now() - t1;
    return 0;
}

void game_frame_done(game *g, uint32 draw_us)
{
    uint32 dt = g->last_us;
    uint32 total = dt + g->last_upd_us + draw_us;
    uint32 b;

    g->last_draw_us = draw_us;
    g->idle_t16 += g->f_idle >> 4;
    g->total_us += dt;
    if (dt > g->max_us) {
        g->max_us = dt;
        g->max_frame = g->frame;
    }
    g->upd_total_us += g->last_upd_us;
    g->draw_total_us += draw_us;
    g->all_total_us += total;
    if (total > g->max_all_us) {
        g->max_all_us = total;
        g->max_all_frame = g->frame;
    }
    b = total / 1000;
    g->hist[b < GAME_HIST ? b : GAME_HIST - 1]++;
    if (total > g->frame_us) {
        g->over++;
        if (g->slow_logged < GAME_SLOW_LOGS) {
            game_slow_rec *p = (game_slow_rec *)rec_alloc(g, GAME_REC_SLOW, sizeof(*p));

            g->slow_logged++;
            if (p != NULL) {
                p->frame = g->frame;
                p->total_us = total;
                p->us = dt;
                p->upd_us = g->last_upd_us;
                p->draw_us = draw_us;
                p->idle_pct = percent(g->f_idle, FRAME_T);
                p->blocks = g->jit.stats.translations - g->f_blocks;
                p->evicted = g->jit.stats.evictions != g->f_flush;
                p->sync_us = g->jit.stats.sync_us - g->f_sync_us;
                p->insns = g->ctx->int_insns - g->f_interp_insns;
                p->stretches = g->ctx->int_runs - g->f_interp;
                p->data_w = g->sms->vdp.n_data_w - g->f_data;
                p->tiles = g->rd->st.tiles - g->f_tiles;
                p->cells = g->rd->st.cells - g->f_cells;
            }
        }
    }

    g->win.frames++;
    g->win.us += dt;
    if (dt > g->win.max_us) {
        g->win.max_us = dt;
        g->win.max_frame = g->frame;
    }
    g->win.upd_us += g->last_upd_us;
    if (g->last_upd_us > g->win.max_upd_us)
        g->win.max_upd_us = g->last_upd_us;
    g->win.draw_us += draw_us;
    if (draw_us > g->win.max_draw_us)
        g->win.max_draw_us = draw_us;
    g->win.total_us += total;
    if (total > g->win.max_total_us) {
        g->win.max_total_us = total;
        g->win.max_total_frame = g->frame;
    }
    if (total > g->frame_us)
        g->win.over++;
    g->frame++;
    if (g->win.frames == GAME_WINDOW)
        window_close(g);
}

void game_pad_time(game *g, uint32 us)
{
    g->win.pad_us += us;
    g->last_pad_us = us;
}

void game_set_name(game *g, const char *name)
{
    g->name = name;
}

void game_set_frame_us(game *g, uint32 us)
{
    g->frame_us = us;
    z80j_set_budget(&g->jit, plat_usec_now, HOT_SYNC_US, HOT_INT_COST, us);
}

uint32 game_spare_time(game *g, uint32 frame_start, uint32 deadline_us)
{
    uint32 t0 = plat_usec_now();
    uint32 now = t0;
    uint32 origin = deadline_us - g->frame_us;  /* the frame the deadline ends */

    (void)frame_start;
    while (z80j_spare_fits(&g->jit, now - origin, FRAME_MARGIN_US)) {
        if (!z80j_prefetch(&g->jit))
            break;
        now = plat_usec_now();
    }
    g->win.spare_us += now - t0;
    g->last_spare_us = now - t0;
    return now - t0;
}

void game_note_draw_error(game *g)
{
    g->win.draw_err++;
}

void game_note_long_vbl(game *g, uint32 period_us)
{
    uint32 total = g->last_us + g->last_upd_us + g->last_draw_us;

    g->win.long_vbl++;
    if (total <= g->frame_us && g->long_logged < GAME_LONG_LOGS) {
        game_long_rec *p = (game_long_rec *)rec_alloc(g, GAME_REC_LONG, sizeof(*p));

        g->long_logged++;
        if (p != NULL) {
            p->frame = g->frame - 1;
            p->period_us = period_us;
            p->us = g->last_us;
            p->upd_us = g->last_upd_us;
            p->draw_us = g->last_draw_us;
            p->spare_us = g->last_spare_us;
            p->pad_us = g->last_pad_us;
            p->sync_us = g->jit.stats.sync_us - g->f_sync_us;
        }
    }
}

void game_log_summary(game *g)
{
    game_counters now;
    game_counters d;
    uint32 n = g->frame;
    uint32 avg = n ? g->total_us / n : 0;
    uint32 all = n ? g->all_total_us / n : 0;
    uint32 k;

    window_close(g);
    game_log_flush(g);
    read_counters(g, &now);
    diff_counters(&d, &now, &g->run_start);
    printf("Game summary: %lu frames, emulation %lu us average (%lu%% of %d us), worst %lu us "
           "(frame %lu), update %lu us average, draw %lu us average, total %lu us average "
           "(%lu%%), worst %lu us (frame %lu), %lu frames over %d us, Z80 idle %lu%%, %lu IRQ, "
           "%lu NMI, %lu bank switches, %lu blocks translated while running (%lu in spare "
           "time, %lu at once in %lu us, %lu refused, %lu promoted), %lu interpreted "
           "instructions in %lu stretches, %lu evictions (%lu blocks, %lu live chunks), "
           "%lu busy-wait loops found, %lu RAM blocks cut at a seam, status %ld\n",
           (unsigned long)n, (unsigned long)avg,
           (unsigned long)percent(avg, g->frame_us), (int)g->frame_us,
           (unsigned long)g->max_us, (unsigned long)g->max_frame,
           (unsigned long)(n ? g->upd_total_us / n : 0),
           (unsigned long)(n ? g->draw_total_us / n : 0),
           (unsigned long)all, (unsigned long)percent(all, g->frame_us),
           (unsigned long)g->max_all_us, (unsigned long)g->max_all_frame,
           (unsigned long)g->over, (int)g->frame_us,
           (unsigned long)percent(g->idle_t16, n * (FRAME_T / 16)),
           (unsigned long)d.irqs, (unsigned long)d.nmis, (unsigned long)d.banks,
           (unsigned long)d.blocks, (unsigned long)d.prefetched, (unsigned long)d.sync,
           (unsigned long)d.sync_us, (unsigned long)d.sync_refused,
           (unsigned long)d.promoted, (unsigned long)d.interp_insns, (unsigned long)d.interp,
           (unsigned long)d.evictions, (unsigned long)d.evicted, (unsigned long)d.evicted_live,
           (unsigned long)g->jit.stats.busy_loops, (unsigned long)g->jit.stats.seams,
           (long)g->status);
    printf("Game summary: picture: %lu tiles converted, %lu cells drawn, %lu rebuilds, "
           "%lu cells for written tiles, %lu palettes, %lu sprite cels, %lu pieces, %lu priority strips, "
           "%lu priority patches, %lu frames in bands, %lu frames partly blanked, "
           "%lu frames in name table bands, %lu bands dropped, %lu frames short of layers, "
           "%lu frames with sprites past the line limit, %lu sprite runs dropped, "
           "%lu frames with sprite overflow, %lu with sprite collision, "
           "%lu frames in palette bands, %lu colour writes merged, %lu frames of more than "
           "192 lines\n",
           (unsigned long)d.rd.tiles, (unsigned long)d.rd.cells, (unsigned long)d.rd.rebuilds,
           (unsigned long)d.rd.tile_cells, (unsigned long)d.rd.palettes,
           (unsigned long)d.rd.sprites, (unsigned long)d.rd.pieces,
           (unsigned long)d.rd.prio_strips, (unsigned long)d.rd.prio_patches,
           (unsigned long)d.rd.bands, (unsigned long)d.rd.dbands,
           (unsigned long)d.rd.ntbands, (unsigned long)d.rd.dropped, (unsigned long)d.rd.layers,
           (unsigned long)d.rd.spr_partial, (unsigned long)d.rd.spr_dropped,
           (unsigned long)d.rd.overflows, (unsigned long)d.rd.collisions,
           (unsigned long)d.rd.pbands, (unsigned long)d.rd.pmerged, (unsigned long)d.rd.tall);
    printf("Game summary: VDP %lu data writes, %lu data reads, %lu control writes, "
           "%lu status reads, %lu counter reads, PSG %lu writes, pad %lu reads\n",
           (unsigned long)d.vdp_data_w, (unsigned long)d.vdp_data_r,
           (unsigned long)d.vdp_ctrl_w, (unsigned long)d.io.vdp_stat_r,
           (unsigned long)d.io.counter_r, (unsigned long)d.io.psg_w,
           (unsigned long)d.io.pad_r);
    printf("Game summary: total frame time histogram (ms: frames)");
    for (k = 0; k < GAME_HIST; k++) {
        if (g->hist[k] != 0)
            printf(" %lu%s:%lu", (unsigned long)k, k == GAME_HIST - 1 ? "+" : "",
                   (unsigned long)g->hist[k]);
    }
    printf("\n");
}
