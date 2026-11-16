/*
 * sttest.c - ARM60 model timing of the savestate coders of stc.c: one
 * state built into the binary (st_data.h, written by stmk.sh), the
 * cartridge image from -rom; prints each coder's size and its encode and
 * decode times on the model's clock. Built by stmk.sh, run with
 *   sim -img sttest.bin -sym sttest.sym -rom image -frames 1
 */
#include "stc.c"

extern int printf(const char *, ...);
extern const u8 *sim_rom(void);
extern u32 sim_rom_size(void);
extern u32 sim_time(void);

#include "st_data.h"

void lz_main(void) {}

static u8 *heap = (u8 *)0x200000;
static void *take(u32 n) { void *p = heap; heap += (n + 15) & ~15u; return p; }

static u32 be(const u8 *p) { return (u32)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }

static void bounds(stc_in *in, const u8 *s, u32 sn)
{
    u32 cram = be(s + 8), cart = be(s + 12);
    u32 hl = sn - 0x2000 - 0x4000 - cram - cart;
    in->rb[0] = 0; in->rb[1] = hl; in->rb[2] = hl + 0x2000; in->rb[3] = hl + 0x6000;
    in->rb[4] = hl + 0x6000 + cram; in->rb[5] = sn;
}

static void run_lz(u32 stride)
{
    stc_in in;
    u8 *s = take(ST_N), *d = take(ST_N), *out = take(65536);
    lz_io *z = take(sizeof(lz_io));
    lz_mf *mf = take(sizeof(lz_mf));
    u32 t0, t1, t2, len, nm, k, bad = 0;
    for (k = 0; k < ST_N; k++) s[k] = st_data[k];
    bounds(&in, s, ST_N);
    in.s = s; in.n = ST_N; in.rom = sim_rom(); in.rom_n = stride ? sim_rom_size() : 0;
    in.rom_stride = stride ? stride : 1;
    mf->prev = take(4 * ST_N);
    mf->rhead = take(4 << LZ_RHBITS);
    mf->rnext = take(4 * (in.rom_n / in.rom_stride + 1));
#ifdef ROMMAP
    rlen = take(4 * ST_N); raddr = take(4 * ST_N);
#endif
    t0 = sim_time();
    len = stc_lz_enc(&in, out, 65536, z, mf, &nm);
    t1 = sim_time();
    stc_lz_dec(&in, d, out, len, z);
    t2 = sim_time();
    for (k = 0; k < ST_N; k++) bad |= d[k] ^ st_data[k];
    printf("lz stride %u: %u bytes, %u matches, encode %u us, decode %u us%s\n", stride, len, nm,
           t1 - t0, t2 - t1, bad ? " BAD" : "");
}

static void run_cm(u32 stride)
{
    stc_in in;
    cm_t *m = take(sizeof(cm_t));
    u32 hb[7] = { 12, 19, 16, 19, 16, 19, 16 };
    u32 t0, t1, t2, t3, len, k, bad = 0, j;
    u8 *out = take(65536), *hist;
    in.rom = sim_rom(); in.rom_n = stride ? sim_rom_size() : 0;
    in.rom_stride = stride ? stride : 1;
    hist = take(32 + in.rom_n + ST_N + 64);
    for (k = 0; k < 32; k++) hist[k] = 0;
    for (k = 0; k < in.rom_n; k++) hist[32 + k] = in.rom[k];
    for (k = 0; k < ST_N; k++) hist[32 + in.rom_n + k] = st_data[k];
    bounds(&in, st_data, ST_N);
    in.s = hist + 32 + in.rom_n; in.n = ST_N;
    for (j = 0; j < 7; j++) m->t[j] = take(4 << hb[j]);
    m->mhead = take(4 << MHB);
    t0 = sim_time();
    cm_init(&in, hist, m, hb);
    t1 = sim_time();
    len = stc_cm_run(&in, hist, out, 65536, 1, 0, m, hb);
    t2 = sim_time();
    for (k = 0; k < ST_N; k++) hist[32 + in.rom_n + k] = 0;
    stc_cm_run(&in, hist, out, 65536, 0, len, m, hb);
    t3 = sim_time();
    for (k = 0; k < ST_N; k++) bad |= hist[32 + in.rom_n + k] ^ st_data[k];
    printf("cm stride %u: %u bytes, set-up %u us, encode %u us, decode %u us (each with a set-up)%s\n",
           stride, len, t1 - t0, t2 - t1, t3 - t2, bad ? " BAD" : "");
}

void hmain(void)
{
    u8 *mark = heap;
    printf("state of %u bytes, image of %u bytes\n", ST_N, sim_rom_size());
    run_lz(0); heap = mark;
    run_lz(8); heap = mark;
    run_lz(16); heap = mark;
#ifdef STTEST_CM
    run_cm(0); heap = mark;
    run_cm(8); heap = mark;
#endif
}
