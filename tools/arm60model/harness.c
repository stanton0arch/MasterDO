/*
 * Driver of the ARM60 cycle model: runs the code of the ISO (bench_cpu.c,
 * game.c and everything they call, as compiled by armcc) with the host
 * services of hstub.s instead of the 3DO OS.
 *
 * Arguments from the command line of sim (sim_arg): 0 frames of the
 * cartridge run, 1 run the benchmarks of steps 0-1 first, 2 dump the
 * pictures every n frames, 3 frame from which the profile counts, 17 write
 * only the pictures at multiples of n and the first 17 of the others that
 * differ, 18 log the writes made during the active display of every
 * frame. The pad
 * is scripted by the host (sim_pad). At the end, the code buffer is dumped
 * for anacode.py and hotblocks.py.
 *
 * Pictures: the debug view (CPU reference, view_NNNNNN.ppm) and the cel
 * picture (cels_NNNNNN.ppm) composed by a small software model of the CEL
 * engine from the cel list built by the renderer, and the two are
 * compared pixel by pixel (differences expected where sprites pass behind
 * priority tiles, which the cels do not handle yet).
 */
#include "types.h"
#include "kernel.h"
#include "task.h"
#include "mem.h"
#include "game.h"
#include "z80jit_int.h"
#include "dbgview.h"
#include "bench_cpu.h"
#include "platform.h"
#include "stdio.h"
#include "string.h"

extern const uint8 *sim_rom(void);
extern uint32 sim_rom_size(void);
extern void sim_dump(const uint32 *fb, uint32 width, uint32 height, uint32 tag);
extern uint32 sim_arg(uint32 i);
extern uint32 sim_pad(uint32 frame);
extern void sim_mark(uint32 id);
extern void sim_codedump(uint32 code, uint32 cur, uint32 blocks, uint32 nblocks);
extern void dec_test(void);

static struct KernelBase kb;
static Task task;
struct KernelBase *KernelBase;

static game g;
static bench_cpu_result bres;
static uint32 fb[256 * (VDP_ACTIVE_MAX / 2)];
static uint32 fb2[256 * (VDP_ACTIVE_MAX / 2)];
static uint32 pic_h;            /* lines of the pictures compared */

/* LRFORM pixel access. */
static void put_pixel(uint32 *f, int32 x, int32 y, uint32 c)
{
    uint32 *w = f + (y >> 1) * 256 + x;

    if (y & 1)
        *w = (*w & 0xFFFF0000u) | c;
    else
        *w = (*w & 0xFFFFu) | (c << 16);
}

static uint32 get_pixel(const uint32 *f, int32 x, int32 y)
{
    uint32 w = f[(y >> 1) * 256 + x];

    return (y & 1) ? (w & 0xFFFF) : (w >> 16);
}

/* Software model of the CEL engine for the cels the renderer builds:
 * 8 bpp coded and 16 bpp uncoded sources, unit steps (VDY may stretch
 * rows), absolute pointers, transparency of 000 without CCB_BGND, clip to
 * the 256 x pic_h window. */
static void compose(const CCB *c, uint32 backdrop)
{
    int32 x;
    int32 y;

    for (y = 0; y < (int32)pic_h; y++)
        for (x = 0; x < 256; x++)
            put_pixel(fb2, x, y, backdrop);

    for (; c != NULL; c = c->ccb_NextPtr) {
        uint32 flags = c->ccb_Flags;
        uint32 bpp;
        uint32 rows;
        uint32 cols;
        uint32 pitch;
        int32 x0;
        int32 y0;
        int32 stretch;
        int32 xstretch;
        const uint16 *plut = (const uint16 *)c->ccb_PLUTPtr;
        uint32 r;

        if (flags & CCB_SKIP) {
            if (flags & CCB_LAST)
                break;
            continue;
        }
        bpp = c->ccb_PRE0 & 7;
        rows = ((c->ccb_PRE0 >> PRE0_VCNT_SHIFT) & 0x3FF) + 1;
        cols = (c->ccb_PRE1 & 0x7FF) + 1;
        pitch = (bpp >= PRE0_BPP_8) ? ((c->ccb_PRE1 >> PRE1_WOFFSET10_SHIFT) & 0x3FF) + 2
                                    : ((c->ccb_PRE1 >> PRE1_WOFFSET8_SHIFT) & 0xFF) + 2;
        x0 = (int32)c->ccb_XPos >> 16;
        y0 = (int32)c->ccb_YPos >> 16;
        stretch = c->ccb_VDY >> 16;
        xstretch = c->ccb_HDX >> 20;
        if (xstretch < 1 || stretch < 1)
            printf("compose: unexpected cel geometry\n");
        for (r = 0; r < rows; r++) {
            const uint8 *src = (const uint8 *)c->ccb_SourcePtr + r * pitch * 4;
            uint32 k;
            int32 s;

            for (k = 0; k < cols; k++) {
                uint32 v;

                if (bpp == PRE0_BPP_8)
                    v = plut[src[k] & 31];
                else if (bpp == PRE0_BPP_16)
                    v = ((uint32)src[2 * k] << 8) | src[2 * k + 1];
                else {
                    printf("compose: unexpected bpp %lu\n", (unsigned long)bpp);
                    return;
                }
                v &= 0x7FFF;
                if (v == 0 && !(flags & CCB_BGND))
                    continue;
                for (s = 0; s < stretch; s++) {
                    int32 py = y0 + (int32)r * stretch + s;
                    int32 t;

                    for (t = 0; t < xstretch; t++) {
                        int32 px = x0 + (int32)k * xstretch + t;

                        if (px >= 0 && px < 256 && py >= 0 && py < (int32)pic_h)
                            put_pixel(fb2, px, py, v);
                    }
                }
            }
        }
        if (flags & CCB_LAST)
            break;
    }
}

/* Pixels covered by a nonzero pixel of a priority tile, where the
 * reference hides sprites (the differences found there are counted
 * apart). */
static uint8 prio[VDP_ACTIVE_MAX][256];

static void prio_mask(const vdp_state *v)
{
    const uint8 *vram = v->vram;
    const uint8 *nt = vram + vdp_nt_base(v, v->reg[2]);
    uint32 bh = v->nt_rows * 8;
    uint32 y;

    memset(prio, 0, sizeof(prio));
    for (y = 0; y < pic_h; y++) {
        uint32 hs = (y < 16 && (v->reg[0] & 0x40)) ? 0 : v->reg[8];
        uint32 col;

        for (col = 0; col < 32; col++) {
            uint32 sx = (col * 8 + hs) & 255;
            uint32 ya = y + (((v->reg[0] & 0x80) && sx >= 192) ? 0 : v->reg[9]);
            const uint8 *e;
            uint32 entry;
            uint32 row;
            const uint8 *p;
            uint32 k;

            while (ya >= bh)
                ya -= bh;
            e = nt + ((ya >> 3) * 32 + col) * 2;
            entry = e[0] | ((uint32)e[1] << 8);
            if (!(entry & 0x1000))
                continue;
            row = ya & 7;
            if (entry & 0x400)
                row = 7 - row;
            p = vram + (entry & 0x1FF) * 32 + row * 4;
            for (k = 0; k < 8; k++) {
                uint32 bit = (entry & 0x200) ? k : 7 - k;
                uint32 c = ((p[0] >> bit) & 1) | (((p[1] >> bit) & 1) << 1) |
                           (((p[2] >> bit) & 1) << 2) | (((p[3] >> bit) & 1) << 3);

                if (c != 0)
                    prio[y][(sx + k) & 255] = 1;
            }
        }
    }
}

/* Compares the cel picture with the reference; 000 and 001 are the same
 * black. Returns the number of differing pixels, and in *outside those
 * that are not under a priority tile pixel. */
static uint32 compare(uint32 *outside)
{
    uint32 n = 0;
    int32 x;
    int32 y;

    *outside = 0;
    for (y = 0; y < (int32)pic_h; y++) {
        for (x = 0; x < 256; x++) {
            uint32 a = get_pixel(fb, x, y);
            uint32 b = get_pixel(fb2, x, y);

            if (a != b && (a | b) != 1) {
                n++;
                if (!prio[y][x])
                    (*outside)++;
            }
        }
    }
    return n;
}



/* Reference of the VDP's sprite evaluation (vdp_sprites, in assembly): the
 * same decisions written plainly in C, compared with it every frame. */
static uint32 ref_tile_mask(const uint8 *vram, uint32 t, uint32 r)
{
    const uint8 *p = vram + (t & 0x1FF) * 32 + r * 4;

    return (uint32)p[0] | p[1] | p[2] | p[3];
}

static uint32 ref_zoom_mask(uint32 m)
{
    m = ((m & 0xF0) << 4) | (m & 0x0F);
    m = ((m & 0x0C0C) << 2) | (m & 0x0303);
    m = ((m & 0x2222) << 1) | (m & 0x1111);
    return m | (m << 1);
}

static uint32 ref_sprites(const vdp_state *v, int32 *spr_y, uint32 *spr_vis, uint32 *pn,
                          uint32 *partial)
{
    const uint8 *vram = v->vram;
    const uint8 *sat = vram + ((v->reg[5] & 0x7E) << 7);
    uint32 zoom = (uint32)v->reg[1] & 1;
    uint32 h = ((v->reg[1] & 0x02) ? 16 : 8) << zoom;
    int32 w = 8 << zoom;
    uint32 tbase = (v->reg[6] & 0x04) ? 256 : 0;
    uint32 tmask = (v->reg[1] & 0x02) ? 0xFE : 0xFF;
    int32 xoff = (v->reg[0] & 0x08) ? 8 : 0;
    int32 act = (int32)v->active;
    int8 diff[VDP_ACTIVE_MAX + 1];
    uint8 ya[VDP_SPRITES];
    uint8 yb[VDP_SPRITES];
    uint32 over[(VDP_ACTIVE_MAX + 31) / 32];
    uint32 flags = 0;
    uint32 n;
    uint32 k;
    uint32 maxrun = 0;
    int32 run = 0;

    for (n = 0; n < VDP_SPRITES; n++) {
        if (sat[n] == 0xD0 && act == VDP_ACTIVE_STD)
            break;
    }
    *pn = n;
    *partial = 0;
    memset(diff, 0, sizeof(diff));
    for (k = 0; k < n; k++) {
        int32 y = (int32)sat[k] + 1;
        int32 a;
        int32 b;

        if (y + (int32)h > 256)
            y -= 256;
        spr_y[k] = y;
        a = (y < 0) ? 0 : y;
        b = y + (int32)h;
        if (b > act)
            b = act;
        if (a >= b) {
            ya[k] = 0;
            yb[k] = 0;
            spr_vis[k] = 0;
            continue;
        }
        ya[k] = (uint8)a;
        yb[k] = (uint8)b;
        spr_vis[k] = (((uint32)1 << (b - a)) - 1) << (a - y);
        diff[a]++;
        diff[b]--;
    }
    if (n < 2)
        return 0;
    memset(over, 0, sizeof(over));
    for (k = 0; k < (uint32)act; k++) {
        run += diff[k];
        if ((uint32)run > maxrun)
            maxrun = (uint32)run;
        if (run > 8)
            over[k >> 5] |= (uint32)1 << (k & 31);
    }
    if (maxrun > 8) {
        uint8 cnt[VDP_ACTIVE_MAX];

        flags |= VDP_ST_OVERFLOW;
        memset(cnt, 0, sizeof(cnt));
        for (k = 0; k < n; k++) {
            uint32 line;

            for (line = ya[k]; line < yb[k]; line++) {
                if (!(over[line >> 5] & ((uint32)1 << (line & 31))))
                    continue;
                if (++cnt[line] > 8) {
                    spr_vis[k] &= ~((uint32)1 << (line - (uint32)spr_y[k]));
                    *partial = 1;
                }
            }
        }
    }
    if (maxrun >= 2) {
        uint8 head[VDP_ACTIVE_MAX];
        uint8 next[VDP_SPRITES];
        uint8 order[VDP_SPRITES];
        uint32 m = 0;
        uint32 i;

        memset(head, 0xFF, sizeof(head));
        for (k = n; k-- > 0;) {
            if (ya[k] < yb[k]) {
                next[k] = head[ya[k]];
                head[ya[k]] = (uint8)k;
            }
        }
        for (k = 0; k < (uint32)act; k++) {
            uint32 s;

            for (s = head[k]; s != 0xFF; s = next[s])
                order[m++] = (uint8)s;
        }
        for (i = 0; i < m && !(flags & VDP_ST_COLLIDE); i++) {
            uint32 a = order[i];
            int32 xa = (int32)sat[0x80 + 2 * a] - xoff;
            uint32 ta = tbase + ((uint32)sat[0x81 + 2 * a] & tmask);
            uint32 j;

            for (j = i + 1; j < m; j++) {
                uint32 b = order[j];
                int32 dx;
                uint32 line;
                uint32 end;

                if (ya[b] >= yb[a])
                    break;
                dx = (int32)sat[0x80 + 2 * b] - xoff - xa;
                if (dx >= w || dx <= -w)
                    continue;
                end = (yb[a] < yb[b]) ? yb[a] : yb[b];
                for (line = ya[b]; line < end; line++) {
                    uint32 ra = (line - (uint32)spr_y[a]);
                    uint32 rb = (line - (uint32)spr_y[b]);
                    uint32 tb;
                    uint32 ma;
                    uint32 mb;

                    if (!((spr_vis[a] >> ra) & 1) || !((spr_vis[b] >> rb) & 1))
                        continue;
                    ra >>= zoom;
                    rb >>= zoom;
                    tb = tbase + ((uint32)sat[0x81 + 2 * b] & tmask);
                    ma = ref_tile_mask(vram, ta + (ra >> 3), ra & 7);
                    mb = ref_tile_mask(vram, tb + (rb >> 3), rb & 7);
                    if (zoom) {
                        ma = ref_zoom_mask(ma);
                        mb = ref_zoom_mask(mb);
                    }
                    if (ma & ((dx >= 0) ? (mb >> dx) : (mb << -dx))) {
                        flags |= VDP_ST_COLLIDE;
                        break;
                    }
                }
                if (flags & VDP_ST_COLLIDE)
                    break;
            }
        }
    }
    return flags;
}

static uint32 sprite_checks;
static uint32 sprite_errors;
static uint32 sprite_flag_frames[2];

/* Compares the assembly evaluation of the current frame with the
 * reference (both pure functions of the VDP state apart from the status
 * bits, which the assembly ORs in again here). */
static void check_sprites(vdp_state *v, uint32 f)
{
    int32 ry[VDP_SPRITES];
    uint32 rvis[VDP_SPRITES];
    uint32 rn;
    uint32 rpartial;
    uint32 rflags;
    uint32 aflags;
    uint32 k;
    uint32 bad = 0;

    rflags = ref_sprites(v, ry, rvis, &rn, &rpartial);
    aflags = vdp_sprites(v);
    sprite_checks++;
    if (aflags & VDP_ST_OVERFLOW)
        sprite_flag_frames[0]++;
    if (aflags & VDP_ST_COLLIDE)
        sprite_flag_frames[1]++;
    if (aflags != rflags || rn != v->spr_n || rpartial != v->spr_partial)
        bad = 1;
    /* The lines shown are only written when some line overflows. */
    for (k = 0; k < rn && !bad; k++) {
        if (ry[k] != v->spr_y[k] || ((rflags & VDP_ST_OVERFLOW) && rvis[k] != v->spr_vis[k]))
            bad = 1;
    }
    if (bad && sprite_errors++ < 20) {
        const uint8 *sat = v->vram + ((v->reg[5] & 0x7E) << 7);

        printf("Sprites: frame %lu: assembly flags %lx n %lu partial %lu, reference flags %lx n %lu partial %lu\n",
               (unsigned long)f, (unsigned long)aflags, (unsigned long)v->spr_n,
               (unsigned long)v->spr_partial, (unsigned long)rflags, (unsigned long)rn,
               (unsigned long)rpartial);
        if (sprite_errors <= 3) {
            for (k = 0; k < rn; k++)
                printf("Sprites:   sprite %lu: y %ld x %lu tile %lu rows %08lx\n", (unsigned long)k,
                       (long)v->spr_y[k], (unsigned long)sat[0x80 + 2 * k],
                       (unsigned long)sat[0x81 + 2 * k], (unsigned long)v->spr_rows[k]);
        }
        for (k = 0; k < rn; k++) {
            if (ry[k] != v->spr_y[k] || rvis[k] != v->spr_vis[k])
                printf("Sprites:   sprite %lu: y %ld vis %08lx, reference y %ld vis %08lx\n",
                       (unsigned long)k, (long)v->spr_y[k], (unsigned long)v->spr_vis[k],
                       (long)ry[k], (unsigned long)rvis[k]);
        }
    }
}

/* -check: consistency of the translator's block table after each frame
 * (hashed blocks with their code, RAM entry headers, zone counts, lookup
 * entries), to find a corruption at the frame it happens. */
static uint32 check_printed;

static void check_blocks(uint32 f)
{
    z80j_state *j = &g.jit;
    uint32 cnt[JIT_MAX_ZONES];
    uint32 k;

    memset(cnt, 0, sizeof(cnt));
    for (k = 0; k < 1024; k++) {
        z80j_block *b;
        uint32 n = 0;

        for (b = j->hash[k]; b != 0 && check_printed < 40; b = b->next) {
            const uint32 *h = b->entry;
            uint32 z;
            uint32 look;
            z80j_block *b2;

            if (++n > 64) {
                printf("Check: frame %lu: hash chain %lu loops\n", (unsigned long)f, (unsigned long)k);
                check_printed++;
                break;
            }
            if (h == 0) {
                printf("Check: frame %lu: block $%04lx key %06lx hashed without code\n",
                       (unsigned long)f, (unsigned long)b->pc, (unsigned long)b->key);
                check_printed++;
                continue;
            }
            z = (uint32)(h - j->code) / j->zone_words;
            if (z < JIT_MAX_ZONES)
                cnt[z]++;
            if (b->key == KEY_RAM &&
                ((h[0] & 0x0F000000u) != 0x0B000000u || h[1] != b->pc || h[2] > 72)) {
                printf("Check: frame %lu: RAM block $%04lx zone %lu entry %08lx: header %08lx %08lx %08lx\n",
                       (unsigned long)f, (unsigned long)b->pc, (unsigned long)z,
                       (unsigned long)JIT_ADDR(h), (unsigned long)h[0], (unsigned long)h[1],
                       (unsigned long)h[2]);
                check_printed++;
            }
            look = j->ctx->lookup[b->pc];
            if (look != j->glue.miss && look != j->glue.interp && look != JIT_ADDR(h) &&
                !KEY_IS_SLOT(b->key)) {
                printf("Check: frame %lu: block $%04lx key %06lx entry %08lx but lookup %08lx\n",
                       (unsigned long)f, (unsigned long)b->pc, (unsigned long)b->key,
                       (unsigned long)JIT_ADDR(h), (unsigned long)look);
                check_printed++;
            }
            for (b2 = b->next; b2 != 0; b2 = b2->next) {
                if (b2->pc == b->pc && b2->key == b->key) {
                    printf("Check: frame %lu: block $%04lx key %06lx hashed twice (%08lx, %08lx)\n",
                           (unsigned long)f, (unsigned long)b->pc, (unsigned long)b->key,
                           (unsigned long)JIT_ADDR(h), (unsigned long)JIT_ADDR(b2->entry));
                    check_printed++;
                    break;
                }
            }
        }
    }
    for (k = 0; k < j->nzones && check_printed < 40; k++) {
        if (cnt[k] != j->zone_blocks[k]) {
            printf("Check: frame %lu: zone %lu holds %lu hashed blocks, counted %lu\n",
                   (unsigned long)f, (unsigned long)k, (unsigned long)cnt[k],
                   (unsigned long)j->zone_blocks[k]);
            check_printed++;
        }
    }
}

void hmain(void)
{
    const uint8 *rom = sim_rom();
    uint32 size = sim_rom_size();
    uint32 frames = sim_arg(0);
    uint32 bench = sim_arg(1);
    uint32 dump_every = sim_arg(2);
    uint32 dump_diff = sim_arg(17);
    uint32 diff_written = 0;
    uint32 f;
    uint32 ndiff;
    uint32 outside;

    kb.kb_CurrentTask = &task;
    KernelBase = &kb;
    if (bench) {
        bench_cpu_run(&bres);
        bench_cpu_log(&bres);
    }
    if (sim_arg(21)) {
        dec_test();             /* -dectest: the decoder in assembly against C */
        return;
    }
    if (frames == 0)
        return;
    if (sim_arg(12))
        game_hot_zones = sim_arg(12);   /* -hotzones n */
    if (game_init(&g, rom, size) != 0)
        return;
    if (sim_arg(4)) {
        /* -interp: nothing is translated, everything is interpreted. */
        z80j_set_interp(&g.jit, g.hot, 0, 0);
        z80j_set_force(&g.jit, 0);
    } else {
        g.seed_hot = sim_arg(11);   /* -seed: reachable code into the hot area */
        game_translate_rom(&g);
    }
    if (sim_arg(10)) {
        /* -hot q:s: the entry counts at which an address is queued and
         * translated at once. */
        z80j_set_interp(&g.jit, g.hot, sim_arg(8), sim_arg(9));
    }
    if (sim_arg(20))
        z80j_set_force(&g.jit, sim_arg(20) - 1);     /* -force n */
    if (sim_arg(7)) {
        /* -budget floor:cost: the budget of translations at once (us per
         * frame, 1/16 us per interpreted instruction). */
        z80j_set_budget(&g.jit, plat_usec_now, sim_arg(5), sim_arg(6), PLAT_NTSC_FRAME_US);
    }
    game_reset(&g);
    for (f = 0; f < frames; f++) {
        uint32 pad = sim_pad(f);

        if (f == sim_arg(3))
            sim_mark(15);           /* profile from here on */
        {
            uint32 t0 = plat_usec_now();
            z80j_stats s0 = g.jit.stats;
            uint32 i0 = g.ctx->int_insns;

            if (game_frame(&g, pad & 0x3F, (pad >> 8) & 1) != 0)
                break;
            /* A DrawCels of 3.5 ms is added to the frame time, as on
             * the console, before the spare time is used. */
            game_frame_done(&g, 3500);
            game_spare_time(&g, t0 - 3500, t0 - 3500 + PLAT_NTSC_FRAME_US);
            if (sim_arg(19)) {
                /* -framelog: one line per frame, for the presentation
                 * model (present.py) and the translation policy. */
                const z80j_stats *s = &g.jit.stats;

                printf("Frame: %lu emu %lu upd %lu draw %lu spare %lu sync %lu sync_us %lu "
                       "forced %lu refused %lu spare_blocks %lu insns %lu shown %lu\n",
                       (unsigned long)f, (unsigned long)g.last_us, (unsigned long)g.last_upd_us,
                       (unsigned long)(render_display_on(g.rd) ? 3500 : 0),
                       (unsigned long)g.last_spare_us,
                       (unsigned long)(s->sync - s0.sync), (unsigned long)(s->sync_us - s0.sync_us),
                       (unsigned long)(s->forced - s0.forced),
                       (unsigned long)(s->sync_refused - s0.sync_refused),
                       (unsigned long)(s->prefetched - s0.prefetched),
                       (unsigned long)(g.ctx->int_insns - i0),
                       (unsigned long)render_display_on(g.rd));
            }
        }
        if (sim_arg(13))
            check_blocks(f);
        if (render_display_on(g.rd))
            check_sprites(&g.sms->vdp, f);
        if (sim_arg(14) && (f + 1) % sim_arg(14) == 0) {
            /* -hash: a hash of the machine state (Z80 RAM and registers,
             * VDP registers, video and colour RAM) every 100 frames
             * (-hashevery n: every n frames), to compare runs made under
             * different host timings or by two builds. */
            const z80j_ctx *c = g.ctx;
            uint32 h = 0;
            uint32 k;

            for (k = 0; k < 0x2000; k++)
                h = h * 31 + c->mram[k];
            for (k = 0; k < 8; k++)
                h = h * 31 + c->regs[k];
            h = h * 31 + c->ix + c->iy * 7 + c->bc2 * 11 + c->de2 * 13 + c->hl2 * 17 +
                c->iff1 * 19 + c->im * 23 + c->a2 * 29 + c->f2 * 31;
            for (k = 0; k < 11; k++)
                h = h * 31 + g.sms->vdp.reg[k];
            for (k = 0; k < VDP_VRAM_SIZE; k++)
                h = h * 31 + g.sms->vdp.vram[k];
            for (k = 0; k < 32; k++)
                h = h * 31 + g.sms->vdp.cram[k];
            printf("Hash: frame %lu: %08lx\n", (unsigned long)(f + 1), (unsigned long)h);
        }
        {
            const vdp_state *v = &g.sms->vdp;
            uint32 k;

            if (v->de_n != 0 || !(v->reg[1] & 0x40) || !v->de_start) {
                printf("Display: frame %lu: start %s, end %s, drawn %s,", (unsigned long)f,
                       v->de_start ? "on" : "off", (v->reg[1] & 0x40) ? "on" : "off",
                       render_display_on(g.rd) ? "yes" : "no");
                for (k = 0; k < v->de_n; k++)
                    printf(" line %lu %s", (unsigned long)(v->de_log[k] >> 8),
                           (v->de_log[k] & 1) ? "on" : "off");
                printf("\n");
            }
        }
        if (sim_arg(18)) {
            /* -rasterlog: the writes made during the active display of
             * every frame that has some, with the first line they affect. */
            const vdp_state *v = &g.sms->vdp;
            uint32 k;

            if (v->hs_n != 0 || v->nt_n != 0 || v->de_n != 0 || v->cr_n != 0) {
                printf("Raster: frame %lu: scroll end %lu:", (unsigned long)(f + 1),
                       (unsigned long)v->reg[8]);
                for (k = 0; k < v->hs_n; k++)
                    printf(" %lu@%lu", (unsigned long)(v->hs_log[k] & 0xFF),
                           (unsigned long)(v->hs_log[k] >> 8));
                printf(", name table end %02lx:", (unsigned long)v->reg[2]);
                for (k = 0; k < v->nt_n; k++)
                    printf(" %02lx@%lu", (unsigned long)(v->nt_log[k] & 0xFF),
                           (unsigned long)(v->nt_log[k] >> 8));
                printf(", display start %lu:", (unsigned long)v->de_start);
                for (k = 0; k < v->de_n; k++)
                    printf(" %lu@%lu", (unsigned long)(v->de_log[k] & 1),
                           (unsigned long)(v->de_log[k] >> 8));
                printf(", colours:");
                for (k = 0; k < v->cr_n; k++)
                    printf(" %02lx=%02lx@%lu", (unsigned long)((v->cr_log[k] >> 8) & 31),
                           (unsigned long)(v->cr_log[k] & 0x3F), (unsigned long)(v->cr_log[k] >> 16));
                printf("\n");
            }
        }
        if (sim_arg(16) == f + 1 && render_display_on(g.rd)) {
            const CCB *c;
            const vdp_state *v = &g.sms->vdp;
            const uint8 *sat = v->vram + ((v->reg[5] & 0x7E) << 7);
            uint32 k;

            for (k = 0; k < v->spr_n; k++)
                printf("Sprite: frame %lu: %lu: y %ld x %lu tile %lu rows %08lx vis %08lx\n",
                       (unsigned long)(f + 1), (unsigned long)k, (long)v->spr_y[k],
                       (unsigned long)sat[0x80 + 2 * k], (unsigned long)sat[0x81 + 2 * k],
                       (unsigned long)v->spr_rows[k], (unsigned long)v->spr_vis[k]);

            printf("Scroll: frame %lu: end %lu", (unsigned long)(f + 1),
                   (unsigned long)v->reg[8]);
            for (k = 0; k < v->hs_n; k++)
                printf(" line %lu %lu", (unsigned long)(v->hs_log[k] >> 8),
                       (unsigned long)(v->hs_log[k] & 0xFF));
            printf("\n");
            printf("Palette: frame %lu: %lu lines, %lu colour writes", (unsigned long)(f + 1),
                   (unsigned long)v->active, (unsigned long)v->cr_n);
            for (k = 0; k < v->cr_n; k++)
                printf(" line %lu %02lx=%02lx", (unsigned long)(v->cr_log[k] >> 16),
                       (unsigned long)((v->cr_log[k] >> 8) & 31), (unsigned long)(v->cr_log[k] & 0x3F));
            printf("\n");
            for (c = render_chain(g.rd); c != NULL; c = c->ccb_NextPtr) {
                printf("Cel: frame %lu: at %ld,%ld size %lu x %lu bpp %lu%s%s\n",
                       (unsigned long)(f + 1), (long)(c->ccb_XPos >> 16),
                       (long)(c->ccb_YPos >> 16), (unsigned long)((c->ccb_PRE1 & 0x7FF) + 1),
                       (unsigned long)(((c->ccb_PRE0 >> PRE0_VCNT_SHIFT) & 0x3FF) + 1),
                       (unsigned long)(c->ccb_PRE0 & 7), (c->ccb_Flags & CCB_BGND) ? " opaque" : "",
                       (c->ccb_Flags & CCB_LAST) ? " last" : "");
                if (c->ccb_Flags & CCB_LAST)
                    break;
            }
            /* The strips the patches replaced, if any: the priority cels
             * before the first one in the chain. */
            for (c = render_chain(g.rd); c != NULL; c = c->ccb_NextPtr) {
                if (c >= &g.rd->cels[RENDER_MAX_PIECES] &&
                    c < &g.rd->cels[RENDER_MAX_PIECES + RENDER_MAX_PRIO])
                    break;
                if (c->ccb_Flags & CCB_LAST) {
                    c = NULL;
                    break;
                }
            }
            if (c != NULL) {
                const CCB *s;

                for (s = &g.rd->cels[RENDER_MAX_PIECES]; s < c; s++)
                    printf("Strip: frame %lu: at %ld,%ld size %lu x %lu\n",
                           (unsigned long)(f + 1), (long)(s->ccb_XPos >> 16),
                           (long)(s->ccb_YPos >> 16), (unsigned long)((s->ccb_PRE1 & 0x7FF) + 1),
                           (unsigned long)(((s->ccb_PRE0 >> PRE0_VCNT_SHIFT) & 0x3FF) + 1));
            }
        }
        /* Compared: the frames at multiples of the dump period and those
         * with a scroll or display band (a frame whose display was off
         * from its start with no band is blank in both pictures). */
        if (dump_every && ((f + 1) % dump_every == 0 || g.sms->vdp.de_n != 0 ||
                           g.sms->vdp.hs_n != 0 || g.sms->vdp.cr_n != 0)) {
            const vdp_state *v = &g.sms->vdp;

            pic_h = v->active;
            sim_mark(1);
            dbgview_draw(v, fb, 256, 0, 0);
            sim_mark(2);
            if (render_display_on(g.rd))
                compose(render_chain(g.rd), render_backdrop(g.rd));
            else
                compose(NULL, render_backdrop(g.rd));
            prio_mask(v);
            ndiff = compare(&outside);
            /* -dumpdiff n: the other frames compared (bands, display
             * changes) are only written when they differ, the first n of
             * them; the pictures at multiples of the dump period are
             * always written. */
            if (!dump_diff || (f + 1) % dump_every == 0 ||
                (ndiff != 0 && diff_written < dump_diff)) {
                if (dump_diff && (f + 1) % dump_every != 0)
                    diff_written++;
                sim_dump(fb, 256, pic_h, f + 1);
                sim_dump(fb2, 256, pic_h, 500000 + f + 1);
            }
            printf("Compare: frame %lu: %lu pixels differ, %lu outside priority tiles, "
                   "display %s, %lu sprites, "
                   "registers 0 %02x 1 %02x 2 %02x 5 %02x 6 %02x 8 %02x 9 %02x, %lu bands\n",
                   (unsigned long)(f + 1), (unsigned long)ndiff, (unsigned long)outside,
                   render_display_on(g.rd) ? "on" : "off", (unsigned long)g.rd->n_sprites,
                   v->reg[0], v->reg[1], v->reg[2], v->reg[5], v->reg[6], v->reg[8], v->reg[9],
                   (unsigned long)v->hs_n);
        }
    }
    game_log_summary(&g);
    printf("Sprites: %lu frames checked, %lu with differences, %lu with overflow, %lu with collision\n",
           (unsigned long)sprite_checks, (unsigned long)sprite_errors,
           (unsigned long)sprite_flag_frames[0], (unsigned long)sprite_flag_frames[1]);
    sim_codedump((uint32)g.jit.code, (uint32)g.jit.code_end, (uint32)g.jit.blocks, g.jit.nblocks);
}
