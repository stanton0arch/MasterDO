/*
 * Cartridge run with frame timing and statistics. See game.h.
 */

#include "game.h"
#include "platform.h"
#include "z80jit_int.h"

#include "stdio.h"
#include "string.h"
#include "mem.h"

#define JIT_ZONE_BYTES  (32 * 1024)
#define JIT_ZONES       13      /* zones of the code buffer, at least */
#define GAME_MAX_ZONES  16      /* at most: 256 run marks of 2 KiB chunks */
#define JIT_HOT_ZONES   5       /* code translated again after an eviction goes there */
#define JIT_CODE_BYTES  (JIT_ZONE_BYTES * g->zones)
/* Zones beyond JIT_ZONES go to the code buffer only while the DRAM left
 * once the run is set up stays above this reserve (images small enough
 * to leave the entry counters and the cartridge RAM to VRAM). */
#define DRAM_RESERVE    (24 * 1024)
/* Block descriptors (24 bytes each, about 40 KiB): the recorded plays of
 * the panel use 1 226 at most; when they run out a zone is evicted. */
#define JIT_BLOCKS      1700
#define JIT_LINKS       4096
#define ROM_QUEUE       1024
#define ROM_MAX_BLOCKS  4000
/* The code translated before the run stops at half of the smallest code
 * buffer: the rest is left to the code met while running (a large
 * cartridge has more reachable code than the buffer holds, whose
 * translation would evict itself), and so are the zones a larger buffer
 * adds. */
#define ROM_MAX_CODE    (JIT_ZONE_BYTES * JIT_ZONES / 2)

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
/* A frame translates the addresses reaching HOT_FORCE_AT whatever the
 * budget until it has run for this many display periods: a working set
 * entered as a whole (a level, a race or a match starting) is spread over
 * a few frames instead of stopping one of them. */
#define FORCE_PERIODS   3

/* Time of a frame outside the emulation, the picture update and the
 * drawing: the pad read, the presentation and the system. A translation
 * in spare time, or at once with the floor budget, must leave the frame
 * this margin on top of what the update and the drawing took in the
 * previous frame. */
#define FRAME_MARGIN_US 800

#define FRAME_T         (VDP_LINES_NTSC * VDP_LINE_T)   /* T-states per frame */

static uint32 percent(uint32 part, uint32 whole);

uint32 game_hot_zones = JIT_HOT_ZONES;
uint32 game_zones = 0;

/* Records of the run's log: a record of n words of kind k is allocated
 * in the arena (NULL, counted as dropped, when it is full) and formatted
 * only by game_log_flush. */
static uint32 *rec_alloc(game *g, uint32 kind, uint32 bytes)
{
    uint32 words = bytes / 4;
    uint32 *r;

    if (g->log == NULL || g->log_len + 1 + words > GAME_LOG_WORDS) {
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
           "%lu bank switches, %lu blocks (%lu in spare time, %lu at once in %lu us "
           "(%lu forced), %lu refused, %lu promoted, %lu us of spare time), %lu interpreted "
           "instructions in %lu stretches, %lu evictions (%lu blocks, %lu live chunks, "
           "%lu ahead in spare time, %lu us), "
           "%lu returns resumed, pad read %lu us, sound %lu us average, %lu us worst, "
           "%lu knob tweaks, end PC $%04lx SP $%04lx IFF %lu, %lu links (%lu refused), "
           "%lu descriptors\n",
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
           (unsigned long)d->forced, (unsigned long)d->sync_refused, (unsigned long)d->promoted,
           (unsigned long)w->spare_us, (unsigned long)d->interp_insns, (unsigned long)d->interp,
           (unsigned long)d->evictions, (unsigned long)d->evicted,
           (unsigned long)d->evicted_live, (unsigned long)d->evict_ahead,
           (unsigned long)d->evict_us, (unsigned long)d->resumed,
           (unsigned long)(w->pad_us / frames), (unsigned long)(w->snd_us / frames),
           (unsigned long)w->max_snd_us, (unsigned long)d->tweaks,
           (unsigned long)p->end_pc, (unsigned long)p->end_sp, (unsigned long)p->end_iff,
           (unsigned long)p->nlinks, (unsigned long)p->link_full, (unsigned long)p->nblocks);
    printf("Picture: frames %lu-%lu: %lu tiles converted, %lu cells drawn, %lu rebuilds, "
           "%lu cells for written tiles, %lu palettes, %lu backdrop changes, %lu sprite cels, "
           "%lu pieces, %lu priority strips, %lu priority patches, %lu frames in scroll bands, "
           "%lu frames partly blanked, %lu frames in name table bands, %lu bands dropped, "
           "%lu frames short of layers, %lu frames with sprites past the line limit, "
           "%lu sprite runs dropped, %lu frames with sprite overflow, %lu with sprite collision, "
           "%lu frames in palette bands, %lu colour writes merged, %lu frames of more than "
           "192 lines, %lu cells and %lu tiles ahead in %lu frames with the display off "
           "(%lu us, %lu cut short), %lu pictures shown again\n",
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
           (unsigned long)d->rd.pbands, (unsigned long)d->rd.pmerged, (unsigned long)d->rd.tall,
           (unsigned long)d->rd.ahead_cells, (unsigned long)d->rd.ahead_tiles,
           (unsigned long)d->rd.ahead_calls, (unsigned long)d->rd.ahead_us,
           (unsigned long)d->rd.ahead_cut, (unsigned long)d->rd.repeats);
}

static void print_slow(const game_slow_rec *p)
{
    printf("Game: frame %lu took %lu us (emulation %lu, update %lu, draw %lu): "
           "Z80 idle %lu%%, %lu blocks translated%s (%lu us at once), "
           "%lu interpreted instructions in %lu stretches, "
           "%lu VDP data writes, %lu tiles converted, %lu cells drawn, sound %lu us\n",
           (unsigned long)p->frame, (unsigned long)p->total_us, (unsigned long)p->us,
           (unsigned long)p->upd_us, (unsigned long)p->draw_us,
           (unsigned long)p->idle_pct, (unsigned long)p->blocks,
           p->evicted ? " after an eviction" : "", (unsigned long)p->sync_us,
           (unsigned long)p->insns, (unsigned long)p->stretches,
           (unsigned long)p->data_w, (unsigned long)p->tiles, (unsigned long)p->cells,
           (unsigned long)p->snd_us);
}

static void print_hash(const game_hash_rec *p)
{
    uint32 k;

    for (k = 0; k < GAME_HASH_PER_REC; k++)
        printf("Hash: frame %lu: %08lx\n", (unsigned long)(p->first + k * GAME_HASH_EVERY),
               (unsigned long)p->h[k]);
}

/* The ring of the last frames: PC and SP at the end of each, and what
 * the frame did (interrupts, data port writes, status reads, blocks
 * translated, instructions interpreted), oldest first. */
static void print_trace(const game *g)
{
    uint32 n = g->trace_n < GAME_TRACE ? g->trace_n : GAME_TRACE;
    uint32 k;

    if (n < 2)
        return;
    printf("Game: last frames (PC SP +IRQ +data +status +blocks +interpreted):");
    for (k = 1; k < n; k++) {
        const uint32 *t = &g->trace[(g->trace_n - n + k) % GAME_TRACE][0];
        const uint32 *u = &g->trace[(g->trace_n - n + k - 1) % GAME_TRACE][0];

        printf(" %lu: $%04lx $%04lx %lu %lu %lu %lu %lu;", (unsigned long)t[0],
               (unsigned long)t[1], (unsigned long)t[2], (unsigned long)(t[3] - u[3]),
               (unsigned long)(t[4] - u[4]), (unsigned long)(t[5] - u[5]),
               (unsigned long)(t[6] - u[6]), (unsigned long)(t[7] - u[7]));
    }
    printf("\n");
}

static void print_long(const game_long_rec *p)
{
    printf("Game: frame %lu shown after %lu us though under budget: emulation %lu, "
           "update %lu, draw %lu, spare time %lu, pad %lu us, %lu us of translations "
           "at once, sound %lu\n",
           (unsigned long)p->frame, (unsigned long)p->period_us,
           (unsigned long)p->us, (unsigned long)p->upd_us, (unsigned long)p->draw_us,
           (unsigned long)p->spare_us, (unsigned long)p->pad_us, (unsigned long)p->sync_us,
           (unsigned long)p->snd_us);
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
    print_trace(g);
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
        case GAME_REC_HASH:   print_hash((const game_hash_rec *)body); break;
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
    c->evict_ahead = g->jit.stats.evict_ahead;
    c->evict_us = g->jit.stats.evict_us;
    c->prefetched = g->jit.stats.prefetched;
    c->promoted = g->jit.stats.promoted;
    c->interp = g->ctx->int_runs;
    c->interp_insns = g->ctx->int_insns;
    c->sync = g->jit.stats.sync;
    c->sync_us = g->jit.stats.sync_us;
    c->sync_refused = g->jit.stats.sync_refused;
    c->forced = g->jit.stats.forced;
    c->resumed = g->jit.stats.resumed + g->ctx->resumed_count;
    c->tweaks = g->snd != NULL ? g->snd->tweaks : 0;
    c->vdp_data_w = g->data_w_total;
    c->vdp_data_r = g->sms->vdp.n_data_r;
    c->vdp_ctrl_w = g->sms->vdp.n_ctrl_w;
    memcpy(&c->io, &g->sms->io, sizeof(c->io));
    c->io.vdp_stat_r += g->sms->vdp.n_stat_r;   /* the assembly handlers count apart */
    c->io.counter_r += g->sms->vdp.n_count_r;
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
    if (g->blocks != NULL) FreeMem(g->blocks, (int32)z80j_block_bytes(JIT_BLOCKS));
    if (g->links != NULL) FreeMem(g->links, (int32)z80j_link_bytes(JIT_LINKS));
    if (g->hot != NULL) FreeMem(g->hot, 0x10000);
    if (g->log != NULL) FreeMem(g->log, GAME_LOG_WORDS * 4);
    if (g->pad_log != NULL) FreeMem(g->pad_log, GAME_PAD_LOGS * 4);
    if (g->code != NULL) FreeMem(g->code, JIT_CODE_BYTES);
    if (g->ctx != NULL) FreeMem(g->ctx, sizeof(z80j_ctx) + sizeof(sms_machine));
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

/* The code buffer: JIT_ZONES zones, and up to GAME_MAX_ZONES when the
 * DRAM measured now still leaves DRAM_RESERVE free once the buffer and
 * the arenas of the logs are allocated. Code that outgrows the buffer
 * is evicted and translated again where it runs again, so every zone the
 * memory allows saves translations. A buffer found nowhere in one piece
 * falls back to fewer zones. */
static uint32 *alloc_code(game *g)
{
    uint32 dram = 0;
    uint32 vram = 0;
    uint32 need = JIT_ZONE_BYTES * JIT_ZONES + GAME_LOG_WORDS * 4 + GAME_PAD_LOGS * 4 + DRAM_RESERVE;
    uint32 *code;

    g->zones = JIT_ZONES;
    if (game_zones != 0) {
        g->zones = game_zones;
    } else {
        plat_mem_free(&dram, &vram);
        if (dram > need)
            g->zones += (dram - need) / JIT_ZONE_BYTES;
        if (g->zones > GAME_MAX_ZONES)
            g->zones = GAME_MAX_ZONES;
    }
    for (;;) {
        code = (uint32 *)AllocMem(JIT_CODE_BYTES, MEMTYPE_DRAM);
        if (code != NULL || g->zones <= JIT_ZONES || game_zones != 0)
            return code;
        g->zones--;
    }
}

Err game_init(game *g, const uint8 *rom, uint32 rom_size, uint32 gg)
{
    z80j_glue glue;
    uint32 k;

    memset(g, 0, sizeof(*g));
    g->rom = rom;
    g->rom_size = rom_size;
    /* The machine follows the context in one block: the assembly port
     * handlers address its VDP state at a fixed offset from the context
     * (SMS_VDP_OFFSET, sms.h). */
    g->ctx = (z80j_ctx *)AllocMem(sizeof(z80j_ctx) + sizeof(sms_machine), MEMTYPE_DRAM | MEMTYPE_FILL);
    g->sms = (g->ctx != NULL) ? (sms_machine *)((uint8 *)g->ctx + sizeof(z80j_ctx)) : NULL;
    g->blocks = AllocMem((int32)z80j_block_bytes(JIT_BLOCKS), MEMTYPE_DRAM);
    g->links = (uint32 *)AllocMem((int32)z80j_link_bytes(JIT_LINKS), MEMTYPE_DRAM);
    /* The entry counters of the interpreter go to VRAM, which the CPU
     * reads and writes like DRAM, when the cartridge image leaves room
     * there (images up to 256 KiB): their 64 KiB stay free in DRAM. */
    g->hot_vram = 1;
    g->hot = (uint8 *)AllocMem(0x10000, MEMTYPE_VRAM);
    if (g->hot == NULL) {
        g->hot_vram = 0;
        g->hot = (uint8 *)AllocMem(0x10000, MEMTYPE_DRAM);
    }
    g->rd = (renderer *)AllocMem(sizeof(renderer), MEMTYPE_DRAM | MEMTYPE_FILL);
    if (g->ctx == NULL || g->blocks == NULL || g->links == NULL ||
        g->hot == NULL || g->sms == NULL || g->rd == NULL) {
        printf("ERROR: out of memory for the machine (%lu + %lu + %lu + %lu bytes)\n",
               (unsigned long)sizeof(z80j_ctx),
               (unsigned long)z80j_block_bytes(JIT_BLOCKS), (unsigned long)z80j_link_bytes(JIT_LINKS),
               (unsigned long)sizeof(sms_machine));
        game_free(g);
        return -1;
    }
    if (render_init(g->rd) < 0) {
        game_free(g);
        return -1;
    }
    /* Cartridge RAM (the battery RAM of the Sega mapper, shown in slot 2
     * by register $FFFC): 32 KiB in VRAM, which the CPU reads and writes
     * like DRAM, when the renderer has left some, else the 16 KiB page
     * most cartridges have, in DRAM; zero-filled, as a cartridge with no
     * save would be. It is allocated before the arenas of the logs, which
     * the game can do without: a shortage of memory then hits the logs
     * rather than the game, whose cartridge RAM holds its saves. */
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
    g->code = alloc_code(g);
    if (g->code == NULL) {
        printf("ERROR: out of memory for the code buffer (%lu bytes)\n",
               (unsigned long)(JIT_ZONE_BYTES * JIT_ZONES));
        game_free(g);
        return -1;
    }
    z80j_default_glue(&glue);
    z80j_init(&g->jit, g->ctx, g->code, JIT_CODE_BYTES / 4, g->zones, game_hot_zones,
              g->blocks, JIT_BLOCKS, g->links, JIT_LINKS, &glue);
    z80j_set_interp(&g->jit, g->hot, HOT_QUEUE_AT, HOT_SYNC_AT);
    z80j_set_force(&g->jit, HOT_FORCE_AT);
    game_set_frame_us(g, PLAT_NTSC_FRAME_US);
    /* The record arena of the run's log (also lent to the translation of
     * the reachable code before the run) and the pad recorder are
     * optional: without memory the records are dropped and counted, and
     * nothing is recorded. */
    g->log = (uint32 *)AllocMem(GAME_LOG_WORDS * 4, MEMTYPE_DRAM);
    g->pad_log = (uint32 *)AllocMem(GAME_PAD_LOGS * 4, MEMTYPE_DRAM);
    sms_init(g->sms, g->ctx, rom, rom_size, VDP_LINES_NTSC, g->cart_ram, g->cart_ram_size, gg);
    for (k = 0; k < Z80J_PORTS; k++) {
        if (SMS_PORT_VCOUNTER(k))
            z80j_set_line_port(&g->jit, k);
        if (SMS_PORT_DATA(k))
            z80j_set_buffer_port(&g->jit, k);
    }
    printf("Cartridge RAM: %lu bytes in %s; entry counters in %s; log arena %s, pad log %s\n",
           (unsigned long)g->cart_ram_size,
           g->cart_ram == NULL ? "no memory" : g->cart_ram_vram ? "VRAM" : "DRAM",
           g->hot_vram ? "VRAM" : "DRAM",
           g->log != NULL ? "allocated" : "out of memory",
           g->pad_log != NULL ? "allocated" : "out of memory");
    printf("Mapper: %s\n", g->sms->mem.mapper == SMS_MAPPER_CODEMASTERS ?
           "Codemasters (header at $7FE0; slots paged by writes to $0000, $4000, $8000)" :
           "Sega (paging registers at $FFFC-$FFFF)");
    if (gg)
        printf("Machine: Game Gear (12-bit colours, %ux%u LCD window, ports $00-$06, "
               "%s region, START on port $00, PSG stereo)\n", VDP_GG_W, VDP_GG_H,
               g->sms->gg_japan ? "Japan" : "overseas");
    else
        printf("Machine: Master System\n");
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
    if (g->log == NULL) {
        printf("ROM translation: out of memory (no log arena for the visited bitmap and the queue)\n");
        return;
    }
    visited = g->log;
    queue = g->log + 0x10000 / 32;
    memset(visited, 0, 0x10000 / 8);
    queue[tail++] = 0x0000;
    queue[tail++] = 0x0038;
    queue[tail++] = 0x0066;

    t0 = plat_usec_now();
    j->seed_hot = g->seed_hot;
    g->rom_capped = 0;
    while (head != tail && g->rom_blocks < ROM_MAX_BLOCKS) {
        uint32 pc = queue[head];
        uint32 bytes = j->stats.code_bytes;
        uint32 n;
        uint32 nt;

        head = (head + 1) % ROM_QUEUE;
        if (visited[pc >> 5] & ((uint32)1 << (pc & 31)))
            continue;
        visited[pc >> 5] |= (uint32)1 << (pc & 31);
        if (g->rom_bytes >= ROM_MAX_CODE) {
            g->rom_capped = 1;
            break;
        }
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
    if (j->nblocks > g->blocks_peak)
        g->blocks_peak = j->nblocks;
    g->rom_busy = j->stats.busy_loops - busy0;

    g->log_len = 0;

    printf("ROM translation: %lu blocks, %lu instructions, %lu bytes of ARM code, %lu us "
           "(%lu us per instruction), %lu busy-wait loops, %lu flag scans from the cache, "
           "%lu evictions%s; code buffer "
           "%lu zones of %d bytes shared by the cold and hot areas, run marks per "
           "%lu bytes, %d block descriptors, %d link entries; "
           "interpreter: translation queued at %d entries, at once from %d within "
           "%d us per frame when the frame has room, plus %d/16 us per interpreted "
           "instruction, whatever the budget from %d entries while the frame is under %lu us\n",
           (unsigned long)g->rom_blocks, (unsigned long)g->rom_insns,
           (unsigned long)g->rom_bytes, (unsigned long)g->rom_us,
           (unsigned long)(g->rom_insns ? g->rom_us / g->rom_insns : 0),
           (unsigned long)g->rom_busy, (unsigned long)j->stats.scan_hits,
           (unsigned long)j->stats.evictions,
           g->rom_capped ? ", stopped at half of the smallest code buffer" : "",
           (unsigned long)g->zones, JIT_ZONE_BYTES, (unsigned long)(4u << j->chunk_shift), JIT_BLOCKS, JIT_LINKS,
           HOT_QUEUE_AT, HOT_SYNC_AT, HOT_SYNC_US, HOT_INT_COST, HOT_FORCE_AT,
           (unsigned long)j->force_limit_us);
}

void game_reset(game *g)
{
    sms_reset(g->sms);
    render_reset(g->rd);
    render_set_window(g->rd, &g->sms->vdp);
    g->status = 0;
    g->stop_pc = 0;
    g->frame = 0;
    g->data_w_total = 0;
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
        p->end_pc = g->ctx->regs[6] & 0xFFFFu;
        p->end_sp = g->ctx->regs[7] >> 16;
        p->end_iff = g->ctx->iff1;
        p->nlinks = g->jit.nlinks;
        p->link_full = g->jit.stats.link_full;
        p->nblocks = g->jit.nblocks;
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
    vdp_clear_data_writes(&g->sms->vdp);
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
    z80j_frame_start(&g->jit, t0, g->last_snd_us + g->last_upd_us + g->last_draw_us +
                     FRAME_MARGIN_US);
    sms_frame(g->sms, pad, pause);
    t1 = plat_usec_now();
    g->last_us = t1 - t0;
    g->f_data = vdp_data_writes(&g->sms->vdp);
    g->data_w_total += g->f_data;

    if (ctx->exit_reason != Z80J_EXIT_LINES) {
        g->status = (int32)ctx->exit_reason;
        g->stop_pc = ctx->exit_arg;
        game_log_flush(g);
        printf("Game: stopped at frame %lu, exit reason %ld at PC $%04lx\n",
               (unsigned long)g->frame, (long)g->status, (unsigned long)g->stop_pc);
        return g->status;
    }
    g->f_idle = (ctx->idle - idle0) >> 8;

    /* The PSG registers as the frame leaves them go to the DSP voices
     * before the picture is built: the sound of the frame then plays
     * while its picture waits for its VBL. */
    g->last_snd_us = 0;
    if (g->snd != NULL) {
        uint32 t2;

        psg_update(g->snd, g->sms->psg_reg,
                   g->sms->vdp.lines == VDP_LINES_PAL ? PSG_CLOCK_PAL : PSG_CLOCK_NTSC,
                   g->sms->gg_port[6]);
        t2 = plat_usec_now();
        g->last_snd_us = t2 - t1;
        t1 = t2;
    }

    render_update(g->rd, &g->sms->vdp);
    g->last_upd_us = plat_usec_now() - t1;
    return 0;
}

void game_frame_done(game *g, uint32 draw_us)
{
    uint32 dt = g->last_us;
    uint32 total = dt + g->last_snd_us + g->last_upd_us + draw_us;
    uint32 b;

    g->last_draw_us = draw_us;
    g->idle_t16 += g->f_idle >> 4;
    g->total_us += dt;
    g->snd_total_us += g->last_snd_us;
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
                p->data_w = g->f_data;
                p->tiles = g->rd->st.tiles - g->f_tiles;
                p->cells = g->rd->st.cells - g->f_cells;
                p->snd_us = g->last_snd_us;
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
    g->win.snd_us += g->last_snd_us;
    if (g->last_snd_us > g->win.max_snd_us)
        g->win.max_snd_us = g->last_snd_us;
    g->win.total_us += total;
    if (total > g->win.max_total_us) {
        g->win.max_total_us = total;
        g->win.max_total_frame = g->frame;
    }
    if (total > g->frame_us)
        g->win.over++;
    g->frame++;
    if (g->jit.nblocks > g->blocks_peak)
        g->blocks_peak = g->jit.nblocks;
    {
        uint32 *t = &g->trace[g->trace_n % GAME_TRACE][0];

        t[0] = g->frame;
        t[1] = g->ctx->regs[6] & 0xFFFFu;
        t[2] = g->ctx->regs[7] >> 16;
        t[3] = g->jit.stats.interrupts + g->ctx->irq_count;
        t[4] = g->data_w_total;
        t[5] = g->sms->io.vdp_stat_r + g->sms->vdp.n_stat_r;
        t[6] = g->jit.stats.translations;
        t[7] = g->ctx->int_insns;
        g->trace_n++;
    }
    if ((g->frame % GAME_HASH_EVERY) == 0) {
        if (g->hash_n == 0)
            g->hash_cur.first = g->frame;
        g->hash_cur.h[g->hash_n++] = game_state_hash(g);
        if (g->hash_n == GAME_HASH_PER_REC) {
            game_hash_rec *p = (game_hash_rec *)rec_alloc(g, GAME_REC_HASH, sizeof(*p));

            if (p != NULL)
                memcpy(p, &g->hash_cur, sizeof(*p));
            g->hash_n = 0;
        }
    }
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

void game_set_sound(game *g, psg_dsp *snd)
{
    g->snd = snd;
}

void game_set_frame_us(game *g, uint32 us)
{
    g->frame_us = us;
    z80j_set_budget(&g->jit, plat_usec_now, HOT_SYNC_US, HOT_INT_COST, us);
    z80j_set_force_limit(&g->jit, FORCE_PERIODS * us);
}

uint32 game_spare_time(game *g, uint32 frame_start, uint32 deadline_us)
{
    uint32 t0 = plat_usec_now();
    uint32 now = t0;
    uint32 origin = deadline_us - g->frame_us;  /* the frame the deadline ends */

    (void)frame_start;
    /* With the display off the picture's catch-up comes first (the work
     * the first frame with the display on would pay), then the
     * translation of queued code in what is left. */
    if (!render_display_on(g->rd)) {
        render_catch_up(g->rd, &g->sms->vdp, plat_usec_now, deadline_us - FRAME_MARGIN_US);
        now = plat_usec_now();
    }
    /* The zone the next translations would have to empty is emptied now
     * when the frame has room for it, so that no frame pays an eviction
     * with a translation at once. */
    if (z80j_evict_ahead(&g->jit, now - origin, FRAME_MARGIN_US))
        now = plat_usec_now();
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
    uint32 total = g->last_us + g->last_snd_us + g->last_upd_us + g->last_draw_us;

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
            p->snd_us = g->last_snd_us;
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
           "(frame %lu), update %lu us average, draw %lu us average, sound %lu us average "
           "(%lu knob tweaks), total %lu us average "
           "(%lu%%), worst %lu us (frame %lu), %lu frames over %d us, Z80 idle %lu%%, %lu IRQ, "
           "%lu NMI, %lu bank switches, %lu blocks translated while running (%lu in spare "
           "time, %lu at once in %lu us (%lu forced), %lu refused, %lu promoted), %lu interpreted "
           "instructions in %lu stretches, %lu evictions (%lu blocks, %lu live chunks, "
           "%lu ahead in spare time, %lu us), "
           "%lu busy-wait loops found (%lu waiting for a line), %lu RAM blocks cut at a seam, "
           "%lu block descriptors in use at most, status %ld\n",
           (unsigned long)n, (unsigned long)avg,
           (unsigned long)percent(avg, g->frame_us), (int)g->frame_us,
           (unsigned long)g->max_us, (unsigned long)g->max_frame,
           (unsigned long)(n ? g->upd_total_us / n : 0),
           (unsigned long)(n ? g->draw_total_us / n : 0),
           (unsigned long)(n ? g->snd_total_us / n : 0), (unsigned long)d.tweaks,
           (unsigned long)all, (unsigned long)percent(all, g->frame_us),
           (unsigned long)g->max_all_us, (unsigned long)g->max_all_frame,
           (unsigned long)g->over, (int)g->frame_us,
           (unsigned long)percent(g->idle_t16, n * (FRAME_T / 16)),
           (unsigned long)d.irqs, (unsigned long)d.nmis, (unsigned long)d.banks,
           (unsigned long)d.blocks, (unsigned long)d.prefetched, (unsigned long)d.sync,
           (unsigned long)d.sync_us, (unsigned long)d.forced, (unsigned long)d.sync_refused,
           (unsigned long)d.promoted, (unsigned long)d.interp_insns, (unsigned long)d.interp,
           (unsigned long)d.evictions, (unsigned long)d.evicted, (unsigned long)d.evicted_live,
           (unsigned long)d.evict_ahead, (unsigned long)d.evict_us,
           (unsigned long)g->jit.stats.busy_loops, (unsigned long)g->jit.stats.line_loops,
           (unsigned long)g->jit.stats.seams, (unsigned long)g->blocks_peak, (long)g->status);
    printf("Game summary: picture: %lu tiles converted, %lu cells drawn, %lu rebuilds, "
           "%lu cells for written tiles, %lu palettes, %lu sprite cels, %lu pieces, %lu priority strips, "
           "%lu priority patches, %lu frames in bands, %lu frames partly blanked, "
           "%lu frames in name table bands, %lu bands dropped, %lu frames short of layers, "
           "%lu frames with sprites past the line limit, %lu sprite runs dropped, "
           "%lu frames with sprite overflow, %lu with sprite collision, "
           "%lu frames in palette bands, %lu colour writes merged, %lu frames of more than "
           "192 lines, %lu cells and %lu tiles ahead in %lu frames with the display off "
           "(%lu us, %lu cut short), %lu pictures shown again\n",
           (unsigned long)d.rd.tiles, (unsigned long)d.rd.cells, (unsigned long)d.rd.rebuilds,
           (unsigned long)d.rd.tile_cells, (unsigned long)d.rd.palettes,
           (unsigned long)d.rd.sprites, (unsigned long)d.rd.pieces,
           (unsigned long)d.rd.prio_strips, (unsigned long)d.rd.prio_patches,
           (unsigned long)d.rd.bands, (unsigned long)d.rd.dbands,
           (unsigned long)d.rd.ntbands, (unsigned long)d.rd.dropped, (unsigned long)d.rd.layers,
           (unsigned long)d.rd.spr_partial, (unsigned long)d.rd.spr_dropped,
           (unsigned long)d.rd.overflows, (unsigned long)d.rd.collisions,
           (unsigned long)d.rd.pbands, (unsigned long)d.rd.pmerged, (unsigned long)d.rd.tall,
           (unsigned long)d.rd.ahead_cells, (unsigned long)d.rd.ahead_tiles,
           (unsigned long)d.rd.ahead_calls, (unsigned long)d.rd.ahead_us,
           (unsigned long)d.rd.ahead_cut, (unsigned long)d.rd.repeats);
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

uint32 game_state_hash(const game *g)
{
    const z80j_ctx *c = g->ctx;
    const uint32 *w = (const uint32 *)c->mram;
    uint32 h;
    uint32 k;

    h = z80j_hash_words(0, w, 0x2000 / 4);
    for (k = 0; k < 8; k++)
        h = h * 31 + c->regs[k];
    h = h * 31 + c->ix + c->iy * 7 + c->bc2 * 11 + c->de2 * 13 + c->hl2 * 17 +
        c->iff1 * 19 + c->im * 23 + c->a2 * 29 + c->f2 * 31 + c->i * 37;
    for (k = 0; k < 11; k++)
        h = h * 31 + g->sms->vdp.reg[k];
    for (k = 0; k < 32; k++)
        h = h * 31 + g->sms->vdp.cram[k];
    for (k = 0; k < 4; k++)
        h = h * 31 + g->sms->mem.reg[k];
    if (g->sms->gg) {
        h = z80j_hash_words(h, (const uint32 *)g->sms->vdp.gg_cram, VDP_GG_CRAM / 4);
        for (k = 0; k < 7; k++)
            h = h * 31 + g->sms->gg_port[k];
    }
    return h;
}

void game_log_state(game *g)
{
    const z80j_ctx *c = g->ctx;
    const z80j_state *j = &g->jit;
    uint32 pc = c->regs[6] & 0xFFFFu;
    uint32 host = c->lookup[pc];
    const z80j_resume *e = &j->resume[Z80J_RESUME_SLOT(pc)];
    uint32 k;

    printf("View: Z80 PC $%04lx SP $%04lx AF $%02lx%02lx BC $%04lx DE $%04lx HL $%04lx "
           "IX $%04lx IY $%04lx I $%02lx IFF %lu %lu IM %lu%s, paging registers %02lx %02lx %02lx %02lx, "
           "bytes at PC",
           (unsigned long)pc, (unsigned long)(c->regs[7] >> 16),
           (unsigned long)(c->regs[1] >> 24), (unsigned long)(c->regs[0] & 0xFFu),
           (unsigned long)(c->regs[2] >> 16), (unsigned long)(c->regs[3] >> 16),
           (unsigned long)(c->regs[4] >> 16), (unsigned long)(c->ix >> 16),
           (unsigned long)(c->iy >> 16), (unsigned long)c->i, (unsigned long)c->iff1,
           (unsigned long)c->iff2, (unsigned long)c->im, c->halted ? " halted" : "",
           (unsigned long)g->sms->mem.reg[0], (unsigned long)g->sms->mem.reg[1],
           (unsigned long)g->sms->mem.reg[2], (unsigned long)g->sms->mem.reg[3]);
    for (k = 0; k < 12; k++)
        printf(" %02lx", (unsigned long)jit_byte(c, pc + k));
    printf("; lookup %s, entry count %lu, resume slot %s\n",
           host == j->glue.miss ? "miss" :
           host == j->glue.interp ? "interpreter" : "block entry",
           (unsigned long)(j->hot != 0 ? j->hot[pc] : 0),
           (e->host != 0 && e->pc == pc) ? "held for this address" :
           e->host != 0 ? "held for another address" : "empty");
    printf("View: translator: %lu blocks in use (%lu at most), %lu links of %lu (%lu refused), "
           "%lu evictions (%lu blocks, %lu unlinked, %lu ahead in spare time, %lu us), "
           "%lu RAM retranslations, %lu dynamic RAM "
           "blocks, %lu seams, %lu returns resumed, %lu hot addresses dropped, "
           "%lu interpreted instructions, generation %lu\n",
           (unsigned long)j->nblocks, (unsigned long)g->blocks_peak, (unsigned long)j->nlinks,
           (unsigned long)j->max_links, (unsigned long)j->stats.link_full,
           (unsigned long)j->stats.evictions, (unsigned long)j->stats.evicted,
           (unsigned long)j->stats.unlinked, (unsigned long)j->stats.evict_ahead,
           (unsigned long)j->stats.evict_us, (unsigned long)j->stats.retranslations,
           (unsigned long)j->stats.dyn_blocks, (unsigned long)j->stats.seams,
           (unsigned long)(j->stats.resumed + c->resumed_count),
           (unsigned long)j->stats.queue_full, (unsigned long)c->int_insns,
           (unsigned long)j->generation);
}
