/*
 * lzcomp.c - compressor of 16 KiB cartridge banks for the bank store
 * measured in step 16.0 (bank cache over a compressed store).
 *
 * Format (LZ4 block format, each bank on its own): a sequence is a token
 * byte (literal count in the high nibble, match length - MINMATCH in the
 * low nibble, 15 meaning that bytes follow, each added, until one is not
 * 255), the literals, then the match offset (two bytes, low first, 1 to
 * 16 383) and the extra match length bytes. The last sequence has
 * literals only, one at least, and ends the bank (the decoder stops when
 * 16 384 bytes are out after literals). MINMATCH is 4; a compressor may refuse matches shorter than
 * `min_match` (fewer, longer sequences: faster to decode on the ARM60,
 * whose every load and store costs 5 to 6 cycles).
 *
 * Built twice: by gcc into the host tool lz (lz.c), and by armcc into the
 * model's image (harness.c, -lztest), where it is timed as the console
 * would run it when the image is loaded. Greedy parse, one hash table of
 * 4 096 entries (the last position of a 4-byte string with each hash and
 * those 4 bytes), the match extended forward; no search for a longer
 * match, no position inside a match hashed.
 */

#define LZ_BANK      0x4000
#define LZ_MINMATCH  4
#define LZ_HBITS     12
#define LZ_NONE      0xFFFFFFFFu

/* Hash of the 4 bytes at a position, held big-endian in v: shifts and
 * exclusive ors (a multiply costs up to 17 cycles on the ARM60). */
#define LZ_HASH(v)   (((v) ^ ((v) >> 9) ^ ((v) >> 20)) & ((1u << LZ_HBITS) - 1))

/* Compresses one bank (LZ_BANK bytes at in) to out (room for
 * LZ_BANK + LZ_BANK / 255 + 16 bytes); returns the compressed size.
 * htab: 2 << LZ_HBITS words of work space (each entry the last position
 * with that hash and its 4 bytes, compared without reading the bank). */
static unsigned lz_compress_bank(const unsigned char *in, unsigned char *out,
                                 unsigned *htab, unsigned min_match)
{
    unsigned i = 0;
    unsigned anchor = 0;
    unsigned char *o = out;
    unsigned k;
    unsigned v;

    for (k = 0; k < (2u << LZ_HBITS); k += 2) {
        htab[k] = LZ_NONE;
        htab[k + 1] = 0;
    }
    v = ((unsigned)in[0] << 24) | ((unsigned)in[1] << 16) | ((unsigned)in[2] << 8) | in[3];
    /* A match never reaches the end: the last sequence has a literal at
     * least, after which the decoder stops. */
    while (i + LZ_MINMATCH < LZ_BANK) {
        unsigned *e = htab + 2 * LZ_HASH(v);
        unsigned c = e[0];
        unsigned len;

        e[0] = i;
        if (e[1] != v || c == LZ_NONE) {
            e[1] = v;
            i++;
            v = (v << 8) | in[i + 3];
            continue;
        }
        len = 4;
        while (i + len < LZ_BANK - 1 && in[c + len] == in[i + len])
            len++;
        if (len < min_match) {
            i++;
            v = (v << 8) | in[i + 3];
            continue;
        }
        {
            unsigned lit = i - anchor;
            unsigned ml = len - LZ_MINMATCH;
            unsigned off = i - c;
            unsigned char *tok = o++;

            *tok = (unsigned char)(((lit < 15 ? lit : 15) << 4) | (ml < 15 ? ml : 15));
            if (lit >= 15) {
                unsigned r = lit - 15;
                while (r >= 255) { *o++ = 255; r -= 255; }
                *o++ = (unsigned char)r;
            }
            for (k = 0; k < lit; k++)
                *o++ = in[anchor + k];
            *o++ = (unsigned char)off;
            *o++ = (unsigned char)(off >> 8);
            if (ml >= 15) {
                unsigned r = ml - 15;
                while (r >= 255) { *o++ = 255; r -= 255; }
                *o++ = (unsigned char)r;
            }
            i += len;
            anchor = i;
            v = ((unsigned)in[i] << 24) | ((unsigned)in[i + 1] << 16) |
                ((unsigned)in[i + 2] << 8) | in[i + 3];
        }
    }
    {
        unsigned lit = LZ_BANK - anchor;

        *o++ = (unsigned char)((lit < 15 ? lit : 15) << 4);
        if (lit >= 15) {
            unsigned r = lit - 15;
            while (r >= 255) { *o++ = 255; r -= 255; }
            *o++ = (unsigned char)r;
        }
        for (k = 0; k < lit; k++)
            *o++ = in[anchor + k];
    }
    return (unsigned)(o - out);
}
