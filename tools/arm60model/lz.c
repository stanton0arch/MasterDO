/*
 * lz - compression of cartridge images bank by bank (16 KiB, each on its
 * own), for the bank store measured in step 16.0.
 *
 *   lz [-min n] [-blob file] image...
 *
 * For each image: the LZ4-format sizes with the greedy parse of
 * lzcomp.c (the one the console would run at load, matches of at least
 * -min bytes, 4 by default) and with an optimal parse of the same format
 * (the bound of that format), an LZSS with flag bytes (literal 1 byte,
 * match 2 bytes: 12-bit offset, 4-bit length 3 to 18) with an optimal
 * parse, and the shape of the greedy LZ4 stream (sequences per bank,
 * literal and match bytes, matches by offset class): the decoder's work.
 *
 * -blob file: the greedy LZ4 store of the (first) image, for the model's
 * -lztest (harness.c): "LZB1", the bank count and -min, the offset of
 * each bank (count + 1 words, from the start of the data), the banks
 * (each starting on a word), then the image itself to check against.
 * Words big-endian, as the console reads them.
 *
 * Build: gcc -O2 -o lz lz.c
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lzcomp.c"

#define BANK LZ_BANK

/* Optimal parse of the LZ4 format (approximate costs: a literal 1 byte, a
 * sequence 3 bytes plus its extra length bytes), matches from hash chains
 * of 4-byte strings (depth 512). Returns the compressed size. */
static unsigned lz4_optimal(const unsigned char *in)
{
    static int head[1 << 16], prev[BANK];
    static unsigned cost[BANK + 1];
    unsigned i;

    for (i = 0; i < (1u << 16); i++) head[i] = -1;
    cost[BANK] = 0;
    /* Chains built forward first (prev[i]: the last earlier position with
     * the same 4 bytes hashed). */
    for (i = 0; i + 4 <= BANK; i++) {
        unsigned v = ((unsigned)in[i] << 24) | ((unsigned)in[i + 1] << 16) | ((unsigned)in[i + 2] << 8) | in[i + 3];
        unsigned h = (v * 2654435761u) >> 16;
        prev[i] = head[h];
        head[h] = (int)i;
    }
    for (i = BANK; i-- > 0;) {
        unsigned best = cost[i + 1] + 1;
        int c = (i + 4 <= BANK) ? prev[i] : -1;
        int depth = 0;
        unsigned maxl = 0;

        while (c >= 0 && depth++ < 512) {
            unsigned l = 0;
            while (i + l < BANK && in[c + l] == in[i + l]) l++;
            if (l >= 4 && l > maxl) {
                unsigned L;
                /* Every length from the previous best to this one. */
                for (L = (maxl < 4 ? 4 : maxl + 1); L <= l; L++) {
                    unsigned e = 3 + (L - 4 >= 15 ? 1 + (L - 4 - 15) / 255 : 0);
                    if (cost[i + L] + e < best) best = cost[i + L] + e;
                }
                maxl = l;
            }
            c = prev[c];
        }
        cost[i] = best;
    }
    return cost[0] + 1;
}

/* LZSS with flag bytes, optimal parse: an item is a literal (1 byte) or a
 * match of 3 to 18 bytes at a distance of 1 to 4 096 (2 bytes); a flag
 * byte per 8 items. Returns the compressed size. */
static unsigned lzss_optimal(const unsigned char *in)
{
    static unsigned cost[BANK + 1];
    static unsigned items[BANK + 1];
    static int head[1 << 16], prev[BANK];
    unsigned i;

    for (i = 0; i < (1u << 16); i++) head[i] = -1;
    for (i = 0; i + 3 <= BANK; i++) {
        unsigned h = ((unsigned)in[i] << 8 ^ (unsigned)in[i + 1] << 4 ^ in[i + 2]) & 0xFFFF;
        h = (h * 40503u) & 0xFFFF;
        prev[i] = head[h];
        head[h] = (int)i;
    }
    cost[BANK] = 0;
    items[BANK] = 0;
    for (i = BANK; i-- > 0;) {
        unsigned best = cost[i + 1] + 1, bi = items[i + 1] + 1;
        int c = (i + 3 <= BANK) ? prev[i] : -1;
        int depth = 0;
        unsigned maxl = 2;

        for (; c >= 0 && i - (unsigned)c <= 4096 && depth++ < 1024; c = prev[c]) {
            unsigned l = 0;
            while (l < 18 && i + l < BANK && in[c + l] == in[i + l]) l++;
            if (l > maxl) {
                unsigned L;
                for (L = maxl + 1; L <= l; L++)
                    if (cost[i + L] + 2 < best || (cost[i + L] + 2 == best && items[i + L] + 1 < bi)) {
                        best = cost[i + L] + 2;
                        bi = items[i + L] + 1;
                    }
                maxl = l;
                if (l == 18) break;
            }
        }
        cost[i] = best;
        items[i] = bi;
    }
    return cost[0] + (items[0] + 7) / 8;
}

/* The shape of a greedy LZ4 stream. */
struct shape {
    unsigned seqs, lits, matches, mbytes, lruns_long, m_long;
    unsigned off_lt8, off_lt16, off_1, off_2, off_4;
};

static void lz4_shape(const unsigned char *p, unsigned n, struct shape *s)
{
    const unsigned char *e = p + n;
    unsigned out = 0;

    while (p < e) {
        unsigned t = *p++;
        unsigned lit = t >> 4, ml = t & 15, off;
        if (lit == 15) { unsigned b; do { b = *p++; lit += b; } while (b == 255); }
        p += lit;
        out += lit;
        s->lits += lit;
        if (lit >= 16) s->lruns_long++;
        s->seqs++;
        if (out >= BANK) break;
        off = p[0] | (p[1] << 8);
        p += 2;
        if (ml == 15) { unsigned b; do { b = *p++; ml += b; } while (b == 255); }
        ml += 4;
        out += ml;
        s->matches++;
        s->mbytes += ml;
        if (ml >= 16) s->m_long++;
        if (off < 8) s->off_lt8++;
        else if (off < 16) s->off_lt16++;
        if (off == 1) s->off_1++;
        if (off == 2) s->off_2++;
        if (off == 4) s->off_4++;
    }
}

static void put32(FILE *f, unsigned v)
{
    fputc(v >> 24, f); fputc(v >> 16, f); fputc(v >> 8, f); fputc(v, f);
}

int main(int argc, char **argv)
{
    unsigned min_match = 4;
    const char *blob = NULL;
    int i, first = 1;
    static unsigned htab[2 << LZ_HBITS];

    printf("%-28s %8s %7s %7s %7s %7s  %6s %6s %6s %6s %6s %6s\n", "image", "size", "greedy", "opt", "lzss",
           "", "seq/bk", "lit/sq", "mat/sq", "off<8", "o<16", "long");
    for (i = 1; i < argc; i++) {
        FILE *f;
        unsigned char *img;
        long n;
        unsigned nb, b, g = 0, o = 0, z = 0;
        unsigned char *store, *sp;
        unsigned *offs;
        struct shape s;
        const char *name;

        if (!strcmp(argv[i], "-min") && i + 1 < argc) { min_match = (unsigned)atoi(argv[++i]); continue; }
        if (!strcmp(argv[i], "-blob") && i + 1 < argc) { blob = argv[++i]; continue; }
        f = fopen(argv[i], "rb");
        if (!f) { perror(argv[i]); return 1; }
        fseek(f, 0, SEEK_END);
        n = ftell(f);
        fseek(f, 0, SEEK_SET);
        img = malloc(n + BANK);
        n = (long)fread(img, 1, n, f);
        fclose(f);
        if (n > 512 && (n & 0x3FFF) == 512) { memmove(img, img + 512, n - 512); n -= 512; }
        nb = (unsigned)((n + BANK - 1) / BANK);
        memset(img + n, 0xFF, nb * BANK - n);
        store = malloc(nb * (BANK + BANK / 255 + 32));
        offs = malloc((nb + 1) * sizeof(unsigned));
        sp = store;
        memset(&s, 0, sizeof(s));
        for (b = 0; b < nb; b++) {
            unsigned c = lz_compress_bank(img + b * BANK, sp, htab, min_match);
            offs[b] = (unsigned)(sp - store);
            lz4_shape(sp, c, &s);
            g += c;
            sp += (c + 3) & ~3u;
            o += lz4_optimal(img + b * BANK);
            z += lzss_optimal(img + b * BANK);
        }
        offs[nb] = (unsigned)(sp - store);
        name = strrchr(argv[i], '/') ? strrchr(argv[i], '/') + 1 : argv[i];
        printf("%-28s %8ld %6.1f%% %6.1f%% %6.1f%% %7s  %6u %6.2f %6.2f %5.1f%% %5.1f%% %5.1f%%\n", name, n,
               100.0 * g / (nb * BANK), 100.0 * o / (nb * BANK), 100.0 * z / (nb * BANK), "",
               s.seqs / nb, (double)s.lits / s.seqs, s.matches ? (double)s.mbytes / s.matches : 0.0,
               s.matches ? 100.0 * s.off_lt8 / s.matches : 0.0, s.matches ? 100.0 * s.off_lt16 / s.matches : 0.0,
               s.matches ? 100.0 * s.m_long / s.matches : 0.0);
        if (blob && first) {
            FILE *bf = fopen(blob, "wb");
            if (!bf) { perror(blob); return 1; }
            fwrite("LZB1", 1, 4, bf);
            put32(bf, nb);
            put32(bf, min_match);
            for (b = 0; b <= nb; b++) put32(bf, offs[b]);
            fwrite(store, 1, offs[nb], bf);
            fwrite(img, 1, nb * BANK, bf);
            fclose(bf);
        }
        first = 0;
        free(img); free(store); free(offs);
    }
    return 0;
}
