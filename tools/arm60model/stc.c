/*
 * stc.c - savestate coders, study prototypes (integer only, C89, so that
 * the same file builds with gcc on the host, stc_host.c, and with armcc
 * for the ARM60 model, sttest.c).
 *
 *   stc_lz_*: LZ77 over the state with matches into the cartridge image
 *             (an external dictionary: the image is in memory when a state
 *             is loaded), tokens coded with adaptive binary probabilities
 *             and a carry-less binary arithmetic coder.
 *   stc_cm_*: bitwise context mixing (order 0-2, the tile row above, the
 *             same row of the previous tile, a match model over the image
 *             and the state), same coder: the size bound, far too slow for
 *             the console (about 40 s per 24 KiB state on the ARM60).
 *
 * Input: the state as -statedump writes it (harness.c), with its region
 * boundaries (head, Z80 RAM, VRAM, colour RAM, cartridge RAM), and the
 * image (rom, rom_n bytes).
 *
 * Options (-D): LZ_DEPTH chain depth of the state's matches (32), LIT_BITS
 * bits of a literal's context (3), ROMMAP n: image matches found by one
 * probe per state position of the image index (depth n) instead of one
 * probe per phase of the stride, PROB_T the probability type (u16; u32 on
 * the ARM60, which has no halfword loads), CM_LIMIT, CM_LR (CM rates).
 */

typedef unsigned char  u8;
typedef unsigned short u16;
typedef unsigned int   u32;
typedef int            i32;

/*------------------------------------------------------------ the coder */

typedef struct {
    u32 x1, x2, x;
    u8 *p;          /* output (encoder) or input (decoder) cursor */
    u8 *end;
} rc_t;

static void rc_enc_init(rc_t *c, u8 *out, u8 *end)
{
    c->x1 = 0; c->x2 = 0xFFFFFFFFu; c->p = out; c->end = end; c->x = 0;
}

/* p: probability that the bit is 1, 12 bits (1..4095). */
static void rc_enc(rc_t *c, u32 bit, u32 p)
{
    u32 xmid = c->x1 + ((c->x2 - c->x1) >> 12) * p;

    if (bit) c->x2 = xmid; else c->x1 = xmid + 1;
    while (((c->x1 ^ c->x2) & 0xFF000000u) == 0) {
        if (c->p < c->end) *c->p = (u8)(c->x2 >> 24);
        c->p++;
        c->x1 <<= 8;
        c->x2 = (c->x2 << 8) | 255;
    }
}

static u32 rc_enc_flush(rc_t *c, u8 *out)
{
    /* the first byte of x1 suffices (x1 < x2 differ in their top byte) */
    if (c->p < c->end) *c->p = (u8)(c->x1 >> 24);
    c->p++;
    return (u32)(c->p - out);
}

static void rc_dec_init(rc_t *c, u8 *in, u8 *end)
{
    int k;
    c->x1 = 0; c->x2 = 0xFFFFFFFFu; c->x = 0; c->p = in; c->end = end;
    for (k = 0; k < 4; k++)
        c->x = (c->x << 8) | (c->p < c->end ? *c->p++ : 255);
}

static u32 rc_dec(rc_t *c, u32 p)
{
    u32 xmid = c->x1 + ((c->x2 - c->x1) >> 12) * p;
    u32 bit = c->x <= xmid;

    if (bit) c->x2 = xmid; else c->x1 = xmid + 1;
    while (((c->x1 ^ c->x2) & 0xFF000000u) == 0) {
        c->x1 <<= 8;
        c->x2 = (c->x2 << 8) | 255;
        c->x = (c->x << 8) | (c->p < c->end ? *c->p++ : 255);
    }
    return bit;
}

/*------------------------------------------------- shared: the input */

typedef struct {
    const u8 *s;        /* the state */
    u32 n;
    u32 rb[6];          /* region starts: head, mram, vram, cram, cart, end */
    const u8 *rom;
    u32 rom_n;
    u32 rom_stride;     /* image positions indexed: every rom_stride bytes */
} stc_in;

static u32 region_of(const stc_in *in, u32 i)
{
    u32 r = 0;
    while (r < 4 && i >= in->rb[r + 1]) r++;
    return r;
}

#ifndef PROB_T
#define PROB_T u16
#endif
typedef PROB_T prob;

/*============================================================ LZ coder */

#define LZ_MIN      3           /* shortest match in the state */
#define LZ_RMIN     6           /* shortest match into the image */
#define LZ_HBITS    14
#define LZ_RHBITS   15
#ifndef LZ_DEPTH
#define LZ_DEPTH    32
#endif
#ifndef LIT_BITS
#define LIT_BITS 3
#endif

static void upd(prob *p, u32 bit)
{
    if (bit) *p += (65536 - *p) >> 4; else *p -= *p >> 4;
}

typedef struct {
    prob ismatch[2][5][4];       /* [last was match][region][pos & 3] */
    prob lit[5][1 << LIT_BITS][256];   /* [region][ctx][tree node] */
    prob kind[3][4];             /* rep / state / image */
    prob len[3][2][24];          /* gamma: [kind][unary / bits][index] */
    prob slot[2][32];            /* offset slot tree (5 bits): state, image */
    prob obits[2][32][4];        /* the top bits under the slot */
    prob rcont[2];               /* image match continuing the last one */
} lz_model;

static void lz_model_init(lz_model *m)
{
    prob *p = (prob *)m;
    u32 k;
    for (k = 0; k < sizeof(*m) / sizeof(prob); k++) p[k] = 32768;
}

/* bit coding in one place for both directions */
typedef struct {
    rc_t rc;
    int enc;
    lz_model m;
} lz_io;

static u32 bit_io(lz_io *z, prob *p, u32 bit)
{
    if (z->enc) rc_enc(&z->rc, bit, (*p >> 4) | 1); else bit = rc_dec(&z->rc, (*p >> 4) | 1);
    upd(p, bit);
    return bit;
}

static u32 raw_io(lz_io *z, u32 bit)
{
    if (z->enc) rc_enc(&z->rc, bit, 2048); else bit = rc_dec(&z->rc, 2048);
    return bit;
}

/* Elias gamma of v >= 1 with adaptive bits. */
static u32 gamma_io(lz_io *z, prob *u, prob *b, u32 v)
{
    u32 nb = 0, k, r;
    if (z->enc) {
        while ((v >> (nb + 1)) != 0) nb++;
        for (k = 0; k < nb; k++) bit_io(z, &u[k < 23 ? k : 23], 1);
        bit_io(z, &u[nb < 23 ? nb : 23], 0);
        for (k = nb; k-- > 0;) bit_io(z, &b[k < 23 ? k : 23], (v >> k) & 1);
        return v;
    }
    while (bit_io(z, &u[nb < 23 ? nb : 23], 0)) nb++;
    r = 1;
    for (k = nb; k-- > 0;) r = (r << 1) | bit_io(z, &b[k < 23 ? k : 23], 0);
    return r;
}

/* offset or address v (0 .. 2^24): 5-bit slot = bit length, then the
 * two bits under the top one adaptive, the rest raw. */
static u32 off_io(lz_io *z, u32 t, u32 v)
{
    u32 nb = 0, k, node = 1, r = 1, bit;
    if (z->enc) while ((v >> nb) != 0) nb++;
    for (k = 5; k-- > 0;) node = (node << 1) | bit_io(z, &z->m.slot[t][node], (nb >> k) & 1);
    nb = node & 31;
    if (nb <= 1) return nb;
    for (k = nb - 1; k-- > 0;) {
        u32 d = nb - 2 - k;
        bit = (v >> k) & 1;
        if (d < 2) bit = bit_io(z, &z->m.obits[t][nb][d == 0 ? 0 : 1 + (r & 1)], bit);
        else bit = raw_io(z, bit);
        r = (r << 1) | bit;
    }
    return r;
}

static u32 lit_ctx(const u8 *s, u32 i, u32 rg)
{
    /* VRAM: the high bits of the byte of the row above (same plane);
     * elsewhere the high bits of the previous byte. */
    if (rg == 2) return i >= 4 ? s[i - 4] >> (8 - LIT_BITS) : 0;
    return i >= 1 ? s[i - 1] >> (8 - LIT_BITS) : 0;
}

static void lit_io(lz_io *z, u8 *s, u32 i, u32 rg)
{
    prob *t = z->m.lit[rg][lit_ctx(s, i, rg)];
    u32 node = 1, k, c = s[i];
    for (k = 8; k-- > 0;) node = (node << 1) | bit_io(z, &t[node], (c >> k) & 1);
    s[i] = (u8)node;
}

/* match finder: hash chains over the state, a hash table over the image */
typedef struct {
    i32 head[1 << LZ_HBITS];
    i32 *prev;          /* n entries */
    i32 *rhead;         /* 1 << LZ_RHBITS entries: image positions (stride) */
    i32 *rnext;         /* chain over the indexed image positions */
} lz_mf;

static u32 h3(const u8 *p) { return ((p[0] << 16 | p[1] << 8 | p[2]) * 2654435761u) >> (32 - LZ_HBITS); }
static u32 h6(const u8 *p)
{
    u32 a = (u32)p[0] | (u32)p[1] << 8 | (u32)p[2] << 16 | (u32)p[3] << 24;
    u32 b = (u32)p[4] | (u32)p[5] << 8;
    return ((a * 2654435761u) ^ (b * 0x9E3779B1u + (a >> 15))) * 2246822519u >> (32 - LZ_RHBITS);
}

static int memcmp_(const u8 *a, const u8 *b, u32 n) { while (n--) if (*a++ != *b++) return 1; return 0; }

static void rom_index(const stc_in *in, i32 *rhead, i32 *rnext)
{
    u32 k, q;
    for (k = 0; k < (1u << LZ_RHBITS); k++) rhead[k] = -1;
    for (q = 0, k = 0; q + LZ_RMIN <= in->rom_n; q += in->rom_stride, k++) {
        u32 h = h6(in->rom + q);
        rnext[k] = rhead[h];
        rhead[h] = (i32)k;
    }
}

#ifdef ROMMAP
/* Pre-pass: every state position probes the image index (8-byte grams at
 * the stride); a hit is extended both ways and recorded for each position
 * it covers (rlen: bytes left from there, raddr: image address). */
static u32 *rlen, *raddr;
static void rom_map(const stc_in *in, lz_mf *mf)
{
    u32 i = 0, n = in->n, k;
    for (k = 0; k < n; k++) rlen[k] = 0;
    while (i + 8 <= n) {
        i32 c = mf->rhead[h6(in->s + i)];
        int depth = ROMMAP;
        u32 bestl = 0, bj = 0, ba = 0;
        for (; c >= 0 && depth-- > 0; c = mf->rnext[c]) {
            u32 q = (u32)c * in->rom_stride, j = i, a = q, l;
            if (memcmp_(in->s + i, in->rom + q, 6)) continue;
            while (j > 0 && a > 0 && rlen[j - 1] == 0 && in->s[j - 1] == in->rom[a - 1]) { j--; a--; }
            for (l = i - j; j + l < n && a + l < in->rom_n && in->s[j + l] == in->rom[a + l]; l++) ;
            if (l > bestl) { bestl = l; bj = j; ba = a; }
        }
        if (bestl >= LZ_RMIN) {
            for (k = 0; k < bestl; k++) if (rlen[bj + k] < bestl - k) { rlen[bj + k] = bestl - k; raddr[bj + k] = ba + k; }
            i = bj + bestl > i + 1 ? bj + bestl : i + 1;
        } else i++;
    }
}
#endif

/* best match at i: *off (state offset) or *addr (image) */
static u32 find(const stc_in *in, lz_mf *mf, u32 i, u32 *kind, u32 *val, u32 rep, u32 rnext_addr)
{
    const u8 *s = in->s;
    u32 n = in->n, best = 0, d, l, max = n - i;
    i32 c;
    if (max > 4096) max = 4096;
    /* rep */
    if (rep != 0 && rep <= i) {
        for (l = 0; l < max && s[i + l] == s[i - rep + l]; l++) ;
        if (l >= 2) { best = l; *kind = 0; *val = rep; }
    }
    /* image continuing the last image match */
    if (rnext_addr != 0 && rnext_addr < in->rom_n) {
        u32 lm = in->rom_n - rnext_addr;
        lm = lm < max ? lm : max;
        for (l = 0; l < lm && s[i + l] == in->rom[rnext_addr + l]; l++) ;
        if (l >= 4 && l > best + 1) { best = l; *kind = 3; *val = rnext_addr; }
    }
    if (i + LZ_MIN <= n) {
        int depth = LZ_DEPTH;
        for (c = mf->head[h3(s + i)]; c >= 0 && depth-- > 0; c = mf->prev[c]) {
            d = i - (u32)c;
            for (l = 0; l < max && s[i + l] == s[c + l]; l++) ;
            if (l >= LZ_MIN && l > best + (d > 4096 ? 1 : 0) + (*kind == 0 && best ? 1 : 0)) {
                best = l; *kind = 1; *val = d;
            }
        }
    }
#ifdef ROMMAP
    if (in->rom_n != 0) {
        l = rlen[i] < max ? rlen[i] : max;
        if (l >= LZ_RMIN && l > best + 2) { best = l; *kind = 2; *val = raddr[i]; }
    }
    if (0) {
#else
    if (in->rom_n != 0) {
#endif
        /* image positions indexed every stride bytes: try the phases */
        u32 ph;
        for (ph = 0; ph < in->rom_stride && i + ph + LZ_RMIN <= n; ph++) {
            int depth = 8;
            for (c = mf->rhead[h6(s + i + ph)]; c >= 0 && depth-- > 0; c = mf->rnext[c]) {
                u32 q = (u32)c * in->rom_stride;
                u32 a, lm;
                if (q < ph) continue;
                a = q - ph;
                lm = in->rom_n - a;
                lm = lm < max ? lm : max;
                for (l = 0; l < lm && s[i + l] == in->rom[a + l]; l++) ;
                if (l >= LZ_RMIN && l > best + 2) { best = l; *kind = 2; *val = a; }
            }
            if (ph == 0 && best >= 32) break;
        }
    }
    return best;
}

static void mf_insert(const stc_in *in, lz_mf *mf, u32 i)
{
    if (i + LZ_MIN <= in->n) {
        u32 h = h3(in->s + i);
        mf->prev[i] = mf->head[h];
        mf->head[h] = (i32)i;
    }
}

/* Codes the state; returns the size. work: scratch for the match finder. */
u32 stc_lz_enc(const stc_in *in, u8 *out, u32 cap, lz_io *z, lz_mf *mf, u32 *nmatch)
{
    u32 i = 0, k, rep = 0, last = 0, rnext_addr = 0;
    u8 *s = (u8 *)in->s;
    z->enc = 1;
    lz_model_init(&z->m);
    rc_enc_init(&z->rc, out, out + cap);
    for (k = 0; k < (1u << LZ_HBITS); k++) mf->head[k] = -1;
    if (in->rom_n) rom_index(in, mf->rhead, mf->rnext);
#ifdef ROMMAP
    if (in->rom_n) rom_map(in, mf);
#endif
    *nmatch = 0;
    while (i < in->n) {
        u32 kind = 0, val = 0, l, rg = region_of(in, i);
        l = find(in, mf, i, &kind, &val, rep, rnext_addr);
        /* lazy: a longer match one byte on */
        if (l >= 2 && l < 64 && i + 1 < in->n) {
            u32 k2 = 0, v2 = 0, l2 = find(in, mf, i + 1, &k2, &v2, rep, rnext_addr);
            if (l2 > l + 1) l = 0;
        }
        if (l < 2 || (kind == 1 && l < LZ_MIN)) {
            bit_io(z, &z->m.ismatch[last][rg][i & 3], 0);
            lit_io(z, s, i, rg);
            mf_insert(in, mf, i);
            i++; last = 0;
            continue;
        }
        (*nmatch)++;
        bit_io(z, &z->m.ismatch[last][rg][i & 3], 1);
        /* kind: 0 rep, 1 state, 2 image, 3 image continued */
        if (kind == 0) bit_io(z, &z->m.kind[0][0], 0);
        else {
            bit_io(z, &z->m.kind[0][0], 1);
            if (kind == 1) bit_io(z, &z->m.kind[1][0], 0);
            else {
                bit_io(z, &z->m.kind[1][0], 1);
                bit_io(z, &z->m.rcont[0], kind == 3);
            }
        }
        {
            u32 t = kind == 0 ? 0 : kind == 1 ? 1 : 2;
            gamma_io(z, z->m.len[t][0], z->m.len[t][1], l - 1);
            if (kind == 1) { off_io(z, 0, val); rep = val; }
            else if (kind == 2) off_io(z, 1, val);
            if (kind >= 2) rnext_addr = val + l;
        }
        for (k = 0; k < l; k++) mf_insert(in, mf, i + k);
        i += l; last = 1;
    }
    return rc_enc_flush(&z->rc, out);
}

/* Decodes n bytes into s (region bounds in in, s written). */
void stc_lz_dec(const stc_in *in, u8 *s, const u8 *src, u32 len, lz_io *z)
{
    u32 i = 0, k, rep = 0, last = 0, rnext_addr = 0;
    z->enc = 0;
    lz_model_init(&z->m);
    rc_dec_init(&z->rc, (u8 *)src, (u8 *)src + len);
    while (i < in->n) {
        u32 rg = region_of(in, i), kind, l, val = 0;
        if (!bit_io(z, &z->m.ismatch[last][rg][i & 3], 0)) {
            lit_io(z, s, i, rg);
            i++; last = 0;
            continue;
        }
        if (!bit_io(z, &z->m.kind[0][0], 0)) kind = 0;
        else if (!bit_io(z, &z->m.kind[1][0], 0)) kind = 1;
        else kind = bit_io(z, &z->m.rcont[0], 0) ? 3 : 2;
        {
            u32 t = kind == 0 ? 0 : kind == 1 ? 1 : 2;
            l = gamma_io(z, z->m.len[t][0], z->m.len[t][1], 0) + 1;
        }
        if (kind == 0) val = rep;
        else if (kind == 1) { val = off_io(z, 0, 0); rep = val; }
        else if (kind == 2) val = off_io(z, 1, 0);
        else val = rnext_addr;
        if (kind <= 1) for (k = 0; k < l; k++) s[i + k] = s[i + k - val];
        else { for (k = 0; k < l; k++) s[i + k] = in->rom[val + k]; rnext_addr = val + l; }
        i += l; last = 1;
    }
}

/*============================================================ CM coder */

/* stretch(p) = ln(p / (1 - p)), squash its inverse, 12-bit fixed point */
static short st_tab[4096];
static i32 dt_tab[1024];
#ifndef CM_LIMIT
#define CM_LIMIT 127
#endif

static i32 squash(i32 d)
{
    static const i32 t[33] = {
        1, 2, 3, 6, 10, 16, 27, 45, 73, 120, 194, 310, 488, 747, 1101,
        1546, 2047, 2549, 2994, 3348, 3607, 3785, 3901, 3975, 4024,
        4050, 4068, 4079, 4085, 4089, 4092, 4093, 4094 };
    i32 w;
    if (d > 2047) return 4095;
    if (d < -2047) return 1;
    w = d & 127;
    d = (d >> 7) + 16;
    return (t[d] * (128 - w) + t[d + 1] * w + 64) >> 7;
}

static void st_init(void)
{
    i32 pi = 0, x, i, v;
    for (x = -2047; x <= 2047; x++) {
        v = squash(x);
        for (i = pi; i <= v; i++) st_tab[i] = (short)x;
        pi = v + 1;
    }
    for (i = pi; i < 4096; i++) st_tab[i] = 2047;
    for (i = 0; i < 1024; i++) dt_tab[i] = 16384 / (i + i + 3);
}

#ifndef CM_LR
#define CM_LR 14
#endif
#define CM_N        8           /* inputs of the mixer (7 models + bias) */
#define CM_T1       16          /* order-1 table bits */
#define CM_T2       16          /* hashed contexts: table bits each */

typedef struct {
    u32 *t[7];                  /* probability tables: p (22 bits) << 10 | count, per model */
    u32 tmask[7];
    u32 base[7];                /* context hash of the byte, per model */
    i32 w[64][CM_N];            /* mixer weights by selector */
    i32 st[CM_N];
    u32 idx[7];
    u16 apm[1024 * 33];         /* final SSE: [order-1 low bits ctx][interp] */
    u32 apm_i;
    i32 pr_mix;
    /* match model */
    i32 *mhead;                 /* hash of the last MM bytes -> position */
    u32 mptr;                   /* predicted position in hist (0 none) */
    u32 mlen;
    u16 mp[64][2];              /* [len bucket][expected bit] */
    u32 mbit;                   /* expected bit */
    u32 sel;
} cm_t;

#define MM  5
#define MHB 18

static u32 hmm(const u8 *p) { return ((u32)p[0] << 24 | (u32)p[1] << 16 | (u32)p[2] << 8 | p[3]) * 2654435761u ^ (u32)p[4] * 0x27D4EB2Du; }

/* One byte through the CM coder. hist: image then the state (the state
 * starts at hist + base). */
static u32 cm_byte(cm_t *m, rc_t *rc, int enc, const u8 *hist, u32 pos, u32 c, u32 rg, u32 rom_n)
{
    u32 c0 = 1, k, j;
    u32 c1 = hist[pos - 1], c2 = hist[pos - 2], c4 = hist[pos - 4], c32 = hist[pos - 32];
    u32 h[7];
    u32 pexp = 0;
    /* contexts of this byte */
    h[0] = rg << 8;
    h[1] = (rg << 16 | c1 << 8) ;
    h[2] = ((c2 << 8 | c1) * 0x9E3779B1u + rg) ;
    h[3] = (rg == 2 ? c4 : c2) << 8 | rg << 16;
    h[4] = ((rg == 2 ? c4 << 8 | c1 : c2 << 8 | c1) + (rg << 16)) * 0x85EBCA77u;
    h[5] = (rg == 2 ? c32 : hist[pos - 3]) << 8 | rg << 16 | 0x40000;
    h[6] = (((c4 << 8 | c32) << 8 | c1) * 0xC2B2AE3Du) ^ rg;
    if (m->mlen > 0) pexp = hist[m->mptr];
    for (k = 8; k-- > 0;) {
        i32 dot = 0, p, err;
        u32 bit, sel;
        for (j = 0; j < 7; j++) {
            u32 ix;
            if (j == 0) ix = (h[0] | c0) & m->tmask[0];
            else if (j == 1) ix = (h[1] | c0) & m->tmask[1];
            else if (j == 3 || j == 5) ix = (h[j] | c0) & m->tmask[j];
            else ix = ((h[j] + c0 * 0x2F0F3D3u) ^ (h[j] >> 15)) & m->tmask[j];
            m->idx[j] = ix;
            m->st[j] = st_tab[m->t[j][ix] >> 20];
        }
        /* match model input */
        if (m->mlen > 0 && ((pexp + 256) >> (k + 1)) == c0) {
            u32 eb = (pexp >> k) & 1;
            u32 lb = m->mlen > 31 ? 31 : m->mlen;
            i32 sp = st_tab[m->mp[lb][0] >> 4];
            m->st[7] = eb ? sp : -sp;
            m->mbit = eb;
            sel = 1 + lb / 4;
        } else {
            m->st[7] = 0;
            m->mbit = 2;
            sel = 0;
        }
        sel = sel * 5 + rg;
        m->sel = sel;
        for (j = 0; j < CM_N; j++) dot += (m->st[j] * m->w[sel][j]) >> 16;
        p = squash(dot);
        /* SSE on order 1 */
        {
            i32 s = st_tab[p] + 2048, lo = s >> 7, wt = s & 127;
            u32 a = ((c1 & 0x1F) << 8 | c0) & 1023;
            u32 base = a * 33 + lo;
            i32 pa = (m->apm[base] * (128 - wt) + m->apm[base + 1] * wt) >> 11;
            m->apm_i = base + (wt >> 6);
            p = (p + 3 * pa) >> 2;
            if (p < 1) p = 1;
            if (p > 4095) p = 4095;
        }
        if (enc) { bit = (c >> k) & 1; rc_enc(rc, bit, (u32)p); }
        else bit = rc_dec(rc, (u32)p);
        /* updates */
        err = ((i32)(bit << 12) - squash(dot)) * 10;
        for (j = 0; j < CM_N; j++) m->w[sel][j] += (m->st[j] * err) >> CM_LR;
        for (j = 0; j < 7; j++) {
            u32 *t = &m->t[j][m->idx[j]];
            u32 n = *t & 1023, pp = *t >> 10;
            if (n < CM_LIMIT) ++*t; else *t = (*t & 0xFFFFFC00u) | CM_LIMIT;
            *t += (u32)((((i32)(bit << 22) - (i32)pp) >> 3) * dt_tab[n]) & 0xFFFFFC00u;
        }
        {
            u16 *a = &m->apm[m->apm_i];
            if (bit) *a += (65535 - *a) >> 6; else *a -= *a >> 6;
        }
        if (m->mbit != 2) {
            u32 lb = m->mlen > 31 ? 31 : m->mlen;
            u16 *q = &m->mp[lb][0];
            if (bit == m->mbit) *q += (65536 - *q) >> 6; else *q -= *q >> 6;
            if (bit != m->mbit) m->mlen = 0;
        }
        c0 = (c0 << 1) | bit;
    }
    return c0 & 255;
}

/* after byte at pos: match model update (hist[pos] is known) */
static void cm_after(cm_t *m, const u8 *hist, u32 pos, u32 rom_n, u32 stride, u32 hist0)
{
    u32 h;
    if (m->mlen > 0) {
        if (hist[m->mptr] == hist[pos]) { m->mlen++; m->mptr++; }
        else m->mlen = 0;
    }
    if (pos + 1 >= hist0 + MM) {
        h = hmm(hist + pos + 1 - MM) >> (32 - MHB);
        if (m->mlen == 0) {
            i32 c = m->mhead[h];
            if (c > 0) {
                /* verify and measure the match backwards */
                u32 q = (u32)c, l = 0;
                while (l < 32 && q > l && hist[q - 1 - l] == hist[pos - l]) l++;
                if (l >= MM) { m->mlen = l; m->mptr = q; }
            }
        }
        m->mhead[h] = (i32)(pos + 1);
    }
    (void)rom_n; (void)stride;
}

static void cm_init(const stc_in *in, u8 *hist, cm_t *m, const u32 *hb)
{
    u32 j, i, k;
    u32 base = 32 + in->rom_n;
    st_init();
    for (j = 0; j < 7; j++) {
        m->tmask[j] = (1u << hb[j]) - 1;
        for (k = 0; k <= m->tmask[j]; k++) m->t[j][k] = 1u << 31;
    }
    for (i = 0; i < 64; i++) for (j = 0; j < CM_N; j++) m->w[i][j] = j == 7 ? 30000 : 12000;
    for (i = 0; i < 1024; i++) for (j = 0; j < 33; j++) m->apm[i * 33 + j] = (u16)(squash(((i32)j - 16) * 128) * 16);
    for (i = 0; i < 64; i++) { m->mp[i][0] = 49152; m->mp[i][1] = 49152; }
    for (i = 0; i < (1u << MHB); i++) m->mhead[i] = 0;
    m->mlen = 0; m->mptr = 0;
    /* the image indexed into the match model at the stride */
    if (in->rom_n) {
        for (i = 32 + MM; i + 1 < base; i += in->rom_stride) {
            u32 h = hmm(hist + i - MM) >> (32 - MHB);
            m->mhead[h] = (i32)i;
        }
    }
}

u32 stc_cm_run(const stc_in *in, u8 *hist, u8 *out, u32 cap, int enc, u32 len, cm_t *m, const u32 *hb)
{
    rc_t rc;
    u32 j, i, k;
    u32 base = 32 + in->rom_n;  /* hist: 32 zeros, the image, the state */
    cm_init(in, hist, m, hb);
    if (enc) rc_enc_init(&rc, out, out + cap); else rc_dec_init(&rc, out, out + len);
    /* 32 bytes of zeros before the state in hist when there is no image */
    for (i = 0; i < in->n; i++) {
        u32 c = enc ? hist[base + i] : 0;
        u32 rg = region_of(in, i);
        c = cm_byte(m, &rc, enc, hist, base + i, c, rg, in->rom_n);
        hist[base + i] = (u8)c;
        cm_after(m, hist, base + i, in->rom_n, in->rom_stride, base);
    }
    return enc ? rc_enc_flush(&rc, out) : in->n;
}
