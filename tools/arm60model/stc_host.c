/*
 * stc_host - host driver of the savestate coders of stc.c: the coded size
 * of one state and its round trip.
 *
 *   stc_host lz|cm stride state.bin [image|-] [table bits (cm)]
 *
 * stride: the image is indexed every stride bytes (0: no image). Prints
 * the size and "ok" when the state decodes back identical.
 *
 * Build: gcc -O2 [-DROMMAP=8 -DLZ_DEPTH=8 ...] -o stc_host stc_host.c
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "stc.c"

static u8 *load(const char *p, u32 *n)
{
    FILE *f = fopen(p, "rb"); u8 *b; long l;
    if (!f) { perror(p); exit(1); }
    fseek(f, 0, SEEK_END); l = ftell(f); fseek(f, 0, SEEK_SET);
    b = malloc(l + 64); fread(b, 1, l, f); fclose(f); *n = (u32)l; return b;
}

static u32 be(const u8 *p) { return (u32)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }

int main(int argc, char **argv)
{
    u32 sn, rn = 0, k;
    u8 *s, *rom = 0;
    stc_in in;
    static u8 out[1 << 20], dec[1 << 17];
    static lz_io z; static lz_mf mf;
    u32 nm;
    const char *mode = argv[1];
    u32 stride = atoi(argv[2]);
    s = load(argv[3], &sn);
    if (argc > 4 && strcmp(argv[4], "-") != 0) rom = load(argv[4], &rn);
    {
        u32 cram = be(s + 8), cart = be(s + 12);
        u32 hl = sn - 0x2000 - 0x4000 - cram - cart;
        in.rb[0] = 0; in.rb[1] = hl; in.rb[2] = hl + 0x2000; in.rb[3] = hl + 0x6000;
        in.rb[4] = hl + 0x6000 + cram; in.rb[5] = sn;
    }
    in.s = s; in.n = sn; in.rom = rom; in.rom_n = rom ? rn : 0; in.rom_stride = stride ? stride : 1;
    if (stride == 0) in.rom_n = 0;
    if (!strcmp(mode, "lz")) {
        u32 len;
        u8 *copy = malloc(sn);
        memcpy(copy, s, sn);
        mf.prev = malloc(4 * sn);
        mf.rhead = malloc(4 << LZ_RHBITS);
        mf.rnext = malloc(4 * (in.rom_n / in.rom_stride + 1));
#ifdef ROMMAP
        rlen = malloc(4 * sn); raddr = malloc(4 * sn);
#endif
        len = stc_lz_enc(&in, out, sizeof(out), &z, &mf, &nm);
        memset(dec, 0, sizeof(dec));
        stc_lz_dec(&in, dec, out, len, &z);
        printf("%u %s\n", len, memcmp(dec, copy, sn) ? "BAD" : "ok");
    } else {
        static cm_t m;
        u32 hb[7];
        u32 big = argc > 5 ? atoi(argv[5]) : 18;
        u8 *hist = calloc(32 + in.rom_n + sn + 64, 1), *hist2 = calloc(32 + in.rom_n + sn + 64, 1);
        u32 len;
        hb[0] = 12; hb[1] = big + 1 > 19 ? 19 : big + 1; hb[2] = big; hb[3] = 19; hb[4] = big; hb[5] = 19; hb[6] = big;
        if (big < 16) { hb[1] = 19; hb[3] = 19; hb[5] = 19; }
        for (k = 0; k < 7; k++) m.t[k] = malloc(4 << hb[k]);
        m.mhead = malloc(4 << MHB);
        if (in.rom_n) { memcpy(hist + 32, rom, in.rom_n); memcpy(hist2 + 32, rom, in.rom_n); }
        memcpy(hist + 32 + in.rom_n, s, sn);
        len = stc_cm_run(&in, hist, out, sizeof(out), 1, 0, &m, hb);
        stc_cm_run(&in, hist2, out, sizeof(out), 0, len, &m, hb);
        printf("%u %s\n", len, memcmp(hist2 + 32 + in.rom_n, s, sn) ? "BAD" : "ok");
    }
    return 0;
}
