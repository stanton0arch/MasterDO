/*
 * Driver of the ARM60 cycle model: runs the code of the ISO (bench_cpu.c,
 * game.c and everything they call, as compiled by armcc) with the host
 * services of hstub.s instead of the 3DO OS.
 *
 * Arguments from the command line of sim (sim_arg): 0 frames of the
 * cartridge run, 1 run the benchmarks of steps 0-1 first, 2 dump the
 * pictures every n frames, 3 frame from which the profile counts. The pad
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

static struct KernelBase kb;
static Task task;
struct KernelBase *KernelBase;

static game g;
static bench_cpu_result bres;
static uint32 fb[256 * 96];
static uint32 fb2[256 * 96];

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
 * the 256 x 192 window. */
static void compose(const CCB *c, uint32 backdrop)
{
    int32 x;
    int32 y;

    for (y = 0; y < 192; y++)
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

                        if (px >= 0 && px < 256 && py >= 0 && py < 192)
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
 * reference hides sprites and the cels do not yet. */
static uint8 prio[192][256];

static void prio_mask(const vdp_state *v)
{
    const uint8 *vram = v->vram;
    const uint8 *nt = vram + ((v->reg[2] & 0x0E) << 10);
    uint32 y;

    memset(prio, 0, sizeof(prio));
    for (y = 0; y < 192; y++) {
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

            if (ya >= 224)
                ya -= 224;
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
    for (y = 0; y < 192; y++) {
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
    uint32 f;
    uint32 ndiff;
    uint32 outside;

    kb.kb_CurrentTask = &task;
    KernelBase = &kb;
    if (bench) {
        bench_cpu_run(&bres);
        bench_cpu_log(&bres);
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

            if (game_frame(&g, pad & 0x3F, (pad >> 8) & 1) != 0)
                break;
            /* A DrawCels of 3.5 ms is added to the frame time, as on
             * the console, before the spare time is used. */
            game_frame_done(&g, 3500);
            game_spare_time(&g, t0 - 3500, t0 - 3500 + PLAT_NTSC_FRAME_US);
        }
        if (sim_arg(13))
            check_blocks(f);
        if (sim_arg(14) && (f + 1) % 100 == 0) {
            /* -hash: a hash of the machine state every 100 frames, to
             * compare runs made under different host timings. */
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
        if (dump_every && ((f + 1) % dump_every == 0 || g.sms->vdp.de_n != 0 ||
                           g.sms->vdp.hs_n != 0 || !g.sms->vdp.de_start)) {
            const vdp_state *v = &g.sms->vdp;

            sim_mark(1);
            dbgview_draw(v, fb, 256, 0, 0);
            sim_mark(2);
            sim_dump(fb, 256, 192, f + 1);
            if (render_display_on(g.rd))
                compose(render_chain(g.rd), render_backdrop(g.rd));
            else
                compose(NULL, render_backdrop(g.rd));
            sim_dump(fb2, 256, 192, 500000 + f + 1);
            prio_mask(v);
            ndiff = compare(&outside);
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
    sim_codedump((uint32)g.jit.code, (uint32)g.jit.code_end, (uint32)g.jit.blocks, g.jit.nblocks);
}
