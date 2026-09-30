/*
 * Cartridge run with frame timing and statistics. See game.h.
 */

#include "game.h"
#include "platform.h"

#include "stdio.h"
#include "string.h"
#include "mem.h"

#define JIT_ZONE_BYTES  (32 * 1024)
#define JIT_ZONES       16
#define JIT_HOT_ZONES   10      /* code translated again after an eviction goes there */
#define JIT_CODE_BYTES  (JIT_ZONE_BYTES * JIT_ZONES)
#define JIT_BLOCKS      3072
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
#define HOT_SYNC_US     3000
#define HOT_INT_COST    100

/* Spare-time translation goes on while the frame is under SPARE_LIMIT_US,
 * and at least one queued block is translated while it is under
 * SPARE_ONE_US, so that the queue drains even in busy stretches. */
#define SPARE_LIMIT_US  (PLAT_NTSC_FRAME_US - 2500)
#define SPARE_ONE_US    (PLAT_NTSC_FRAME_US - 1800)

#define FRAME_T         (VDP_LINES_NTSC * VDP_LINE_T)   /* T-states per frame */

static void read_counters(const game *g, game_counters *c)
{
    c->idle = g->ctx->idle;
    c->irqs = g->jit.stats.interrupts;
    c->nmis = g->jit.stats.nmis;
    c->banks = g->sms->mem.remaps;
    c->blocks = g->jit.stats.translations;
    c->flushes = g->jit.stats.flushes;
    c->evictions = g->jit.stats.evictions;
    c->evicted = g->jit.stats.evicted;
    c->prefetched = g->jit.stats.prefetched;
    c->promoted = g->jit.stats.promoted;
    c->interp = g->ctx->int_runs;
    c->interp_insns = g->ctx->int_insns;
    c->sync = g->jit.stats.sync;
    c->sync_us = g->jit.stats.sync_us;
    c->sync_refused = g->jit.stats.sync_refused;
    c->vdp_data_w = g->sms->vdp.n_data_w;
    c->vdp_data_r = g->sms->vdp.n_data_r;
    c->vdp_ctrl_w = g->sms->vdp.n_ctrl_w;
    memcpy(&c->io, &g->sms->io, sizeof(c->io));
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
    read_counters(g, &g->win.start);
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
    if (g->code != NULL) FreeMem(g->code, JIT_CODE_BYTES);
    if (g->ctx != NULL) FreeMem(g->ctx, sizeof(z80j_ctx));
    g->rd = NULL;
    g->sms = NULL;
    g->blocks = NULL;
    g->links = NULL;
    g->hot = NULL;
    g->code = NULL;
    g->ctx = NULL;
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
    g->sms = (sms_machine *)AllocMem(sizeof(sms_machine), MEMTYPE_DRAM);
    g->rd = (renderer *)AllocMem(sizeof(renderer), MEMTYPE_DRAM | MEMTYPE_FILL);
    if (g->ctx == NULL || g->code == NULL || g->blocks == NULL || g->links == NULL ||
        g->hot == NULL || g->sms == NULL || g->rd == NULL) {
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
    z80j_init(&g->jit, g->ctx, g->code, JIT_CODE_BYTES / 4, JIT_ZONES, JIT_HOT_ZONES,
              g->blocks, JIT_BLOCKS, g->links, JIT_LINKS, &glue);
    z80j_set_interp(&g->jit, g->hot, HOT_QUEUE_AT, HOT_SYNC_AT);
    z80j_set_budget(&g->jit, plat_usec_now, HOT_SYNC_US, HOT_INT_COST);
    sms_init(g->sms, g->ctx, rom, rom_size, VDP_LINES_NTSC);
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

    visited = (uint32 *)AllocMem(0x10000 / 8, MEMTYPE_DRAM | MEMTYPE_FILL);
    queue = (uint32 *)AllocMem(ROM_QUEUE * 4, MEMTYPE_DRAM);
    if (visited == NULL || queue == NULL) {
        printf("ROM translation: out of memory\n");
        if (visited != NULL) FreeMem(visited, 0x10000 / 8);
        if (queue != NULL) FreeMem(queue, ROM_QUEUE * 4);
        return;
    }
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

    FreeMem(queue, ROM_QUEUE * 4);
    FreeMem(visited, 0x10000 / 8);

    printf("ROM translation: %lu blocks, %lu instructions, %lu bytes of ARM code, %lu us "
           "(%lu us per instruction), %lu busy-wait loops, %lu evictions; code buffer "
           "%d zones of %d bytes (%d hot), %d block descriptors, %d link entries; "
           "interpreter: translation queued at %d entries, at once from %d within "
           "%d us per frame plus %d/16 us per interpreted instruction\n",
           (unsigned long)g->rom_blocks, (unsigned long)g->rom_insns,
           (unsigned long)g->rom_bytes, (unsigned long)g->rom_us,
           (unsigned long)(g->rom_insns ? g->rom_us / g->rom_insns : 0),
           (unsigned long)g->rom_busy, (unsigned long)j->stats.evictions,
           JIT_ZONES, JIT_ZONE_BYTES, JIT_HOT_ZONES, JIT_BLOCKS, JIT_LINKS,
           HOT_QUEUE_AT, HOT_SYNC_AT, HOT_SYNC_US, HOT_INT_COST);
}

void game_reset(game *g)
{
    sms_reset(g->sms);
    render_reset(g->rd);
    g->status = 0;
    g->stop_pc = 0;
    g->frame = 0;
    g->last_us = 0;
    g->last_upd_us = 0;
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

static void log_window(game *g)
{
    game_window *w = &g->win;
    game_counters now;
    game_counters d;
    uint32 first = g->frame - w->frames;

    read_counters(g, &now);
    diff_counters(&d, &now, &w->start);
    g->win_avg_us = w->us / w->frames;
    g->win_max_us = w->max_us;
    g->win_total_us = w->total_us / w->frames;
    g->win_idle_pct = percent(d.idle >> 8, w->frames * FRAME_T);
    printf("Game: frames %lu-%lu: emulation %lu us average, %lu us worst (frame %lu), "
           "update %lu us average, %lu us worst, draw %lu us average, %lu us worst, "
           "total %lu us average, %lu us worst (frame %lu), %lu over budget, "
           "%lu draw errors, %lu long presents, Z80 idle %lu%%, "
           "%lu IRQ, %lu NMI, VDP %lu data writes, %lu data reads, %lu control writes, "
           "%lu status reads, %lu counter reads, PSG %lu, pad %lu, other %lu, %lu leaves, "
           "%lu bank switches, %lu blocks (%lu in spare time, %lu at once in %lu us, "
           "%lu refused, %lu promoted, %lu us of spare time), %lu interpreted "
           "instructions in %lu stretches, %lu evictions (%lu blocks), pad read %lu us\n",
           (unsigned long)first, (unsigned long)(g->frame - 1),
           (unsigned long)g->win_avg_us, (unsigned long)w->max_us,
           (unsigned long)w->max_frame,
           (unsigned long)(w->upd_us / w->frames), (unsigned long)w->max_upd_us,
           (unsigned long)(w->draw_us / w->frames), (unsigned long)w->max_draw_us,
           (unsigned long)g->win_total_us, (unsigned long)w->max_total_us,
           (unsigned long)w->max_total_frame, (unsigned long)w->over,
           (unsigned long)w->draw_err, (unsigned long)w->long_vbl,
           (unsigned long)g->win_idle_pct,
           (unsigned long)d.irqs, (unsigned long)d.nmis,
           (unsigned long)d.vdp_data_w, (unsigned long)d.vdp_data_r,
           (unsigned long)d.vdp_ctrl_w, (unsigned long)d.io.vdp_stat_r,
           (unsigned long)d.io.counter_r, (unsigned long)d.io.psg_w,
           (unsigned long)d.io.pad_r, (unsigned long)d.io.other,
           (unsigned long)d.io.leaves, (unsigned long)d.banks, (unsigned long)d.blocks,
           (unsigned long)d.prefetched, (unsigned long)d.sync, (unsigned long)d.sync_us,
           (unsigned long)d.sync_refused, (unsigned long)d.promoted,
           (unsigned long)w->spare_us, (unsigned long)d.interp_insns, (unsigned long)d.interp,
           (unsigned long)d.evictions, (unsigned long)d.evicted,
           (unsigned long)(w->pad_us / w->frames));
    printf("Picture: frames %lu-%lu: %lu tiles converted, %lu cells drawn, %lu rebuilds, "
           "%lu cells for written tiles, %lu palettes, %lu backdrop changes, %lu sprite cels, "
           "%lu pieces, %lu priority strips, %lu frames in scroll bands, %lu frames partly blanked, "
           "%lu bands dropped\n",
           (unsigned long)first, (unsigned long)(g->frame - 1),
           (unsigned long)d.rd.tiles, (unsigned long)d.rd.cells, (unsigned long)d.rd.rebuilds,
           (unsigned long)d.rd.tile_cells, (unsigned long)d.rd.palettes,
           (unsigned long)d.rd.backdrops,
           (unsigned long)d.rd.sprites, (unsigned long)d.rd.pieces,
           (unsigned long)d.rd.prio_strips, (unsigned long)d.rd.bands, (unsigned long)d.rd.dbands,
           (unsigned long)d.rd.dropped);
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

    z80j_frame_start(&g->jit);
    t0 = plat_usec_now();
    sms_frame(g->sms, pad, pause);
    t1 = plat_usec_now();
    g->last_us = t1 - t0;

    if (ctx->exit_reason != Z80J_EXIT_LINES) {
        g->status = (int32)ctx->exit_reason;
        g->stop_pc = ctx->exit_arg;
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
    if (total > PLAT_NTSC_FRAME_US) {
        g->over++;
        if (g->slow_logged < GAME_SLOW_LOGS) {
            g->slow_logged++;
            printf("Game: frame %lu took %lu us (emulation %lu, update %lu, draw %lu): "
                   "Z80 idle %lu%%, %lu blocks translated%s (%lu us at once), "
                   "%lu interpreted instructions in %lu stretches, "
                   "%lu VDP data writes, %lu tiles converted, %lu cells drawn\n",
                   (unsigned long)g->frame, (unsigned long)total, (unsigned long)dt,
                   (unsigned long)g->last_upd_us, (unsigned long)draw_us,
                   (unsigned long)percent(g->f_idle, FRAME_T),
                   (unsigned long)(g->jit.stats.translations - g->f_blocks),
                   g->jit.stats.evictions != g->f_flush ? " after an eviction" : "",
                   (unsigned long)(g->jit.stats.sync_us - g->f_sync_us),
                   (unsigned long)(g->ctx->int_insns - g->f_interp_insns),
                   (unsigned long)(g->ctx->int_runs - g->f_interp),
                   (unsigned long)(g->sms->vdp.n_data_w - g->f_data),
                   (unsigned long)(g->rd->st.tiles - g->f_tiles),
                   (unsigned long)(g->rd->st.cells - g->f_cells));
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
    if (total > PLAT_NTSC_FRAME_US)
        g->win.over++;
    g->frame++;
    if (g->win.frames == GAME_WINDOW) {
        log_window(g);
        window_start(g);
    }
}

void game_pad_time(game *g, uint32 us)
{
    g->win.pad_us += us;
}

uint32 game_spare_time(game *g, uint32 frame_start)
{
    uint32 t0 = plat_usec_now();
    uint32 now = t0;
    uint32 done = 0;

    for (;;) {
        uint32 elapsed = now - frame_start;

        if (elapsed >= SPARE_LIMIT_US && (done != 0 || elapsed >= SPARE_ONE_US))
            break;
        if (!z80j_prefetch(&g->jit))
            break;
        done++;
        now = plat_usec_now();
    }
    g->win.spare_us += now - t0;
    return now - t0;
}

void game_note_draw_error(game *g)
{
    g->win.draw_err++;
}

void game_note_long_vbl(game *g)
{
    g->win.long_vbl++;
}

void game_log_summary(const game *g)
{
    game_counters now;
    game_counters d;
    uint32 n = g->frame;
    uint32 avg = n ? g->total_us / n : 0;
    uint32 all = n ? g->all_total_us / n : 0;
    uint32 k;

    read_counters(g, &now);
    diff_counters(&d, &now, &g->run_start);
    printf("Game summary: %lu frames, emulation %lu us average (%lu%% of %d us), worst %lu us "
           "(frame %lu), update %lu us average, draw %lu us average, total %lu us average "
           "(%lu%%), worst %lu us (frame %lu), %lu frames over %d us, Z80 idle %lu%%, %lu IRQ, "
           "%lu NMI, %lu bank switches, %lu blocks translated while running (%lu in spare "
           "time, %lu at once in %lu us, %lu refused, %lu promoted), %lu interpreted "
           "instructions in %lu stretches, %lu evictions (%lu blocks), "
           "%lu busy-wait loops found, status %ld\n",
           (unsigned long)n, (unsigned long)avg,
           (unsigned long)percent(avg, PLAT_NTSC_FRAME_US), PLAT_NTSC_FRAME_US,
           (unsigned long)g->max_us, (unsigned long)g->max_frame,
           (unsigned long)(n ? g->upd_total_us / n : 0),
           (unsigned long)(n ? g->draw_total_us / n : 0),
           (unsigned long)all, (unsigned long)percent(all, PLAT_NTSC_FRAME_US),
           (unsigned long)g->max_all_us, (unsigned long)g->max_all_frame,
           (unsigned long)g->over, PLAT_NTSC_FRAME_US,
           (unsigned long)percent(g->idle_t16, n * (FRAME_T / 16)),
           (unsigned long)d.irqs, (unsigned long)d.nmis, (unsigned long)d.banks,
           (unsigned long)d.blocks, (unsigned long)d.prefetched, (unsigned long)d.sync,
           (unsigned long)d.sync_us, (unsigned long)d.sync_refused,
           (unsigned long)d.promoted, (unsigned long)d.interp_insns, (unsigned long)d.interp,
           (unsigned long)d.evictions, (unsigned long)d.evicted,
           (unsigned long)g->jit.stats.busy_loops, (long)g->status);
    printf("Game summary: picture: %lu tiles converted, %lu cells drawn, %lu rebuilds, "
           "%lu cells for written tiles, %lu palettes, %lu sprite cels, %lu pieces, %lu priority strips, "
           "%lu frames in bands, %lu bands dropped\n",
           (unsigned long)d.rd.tiles, (unsigned long)d.rd.cells, (unsigned long)d.rd.rebuilds,
           (unsigned long)d.rd.tile_cells, (unsigned long)d.rd.palettes,
           (unsigned long)d.rd.sprites, (unsigned long)d.rd.pieces,
           (unsigned long)d.rd.prio_strips, (unsigned long)d.rd.bands, (unsigned long)d.rd.dbands,
           (unsigned long)d.rd.dropped);
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
